# The native half of the Android companion.
#
# It builds the same sources as the desktop and Switch clients -- read from
# cmake/companion_sources.cmake, so a file cannot be added to the client and
# forgotten here -- plus SDL2, which Gradle needs as a library in the APK
# rather than as something already installed.
cmake_minimum_required(VERSION 3.22)
project(dualscreen_companion LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(DUALSCREEN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

if(DEFINED ENV{SDL2_ANDROID_SRC})
    set(SDL_SOURCE_DIR "$ENV{SDL2_ANDROID_SRC}")
else()
    set(SDL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/jni/SDL")
endif()
if(NOT EXISTS "${SDL_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Cannot find the SDL2 source at ${SDL_SOURCE_DIR}. "
        "See android/README.md -- it is one git clone.")
endif()
add_subdirectory("${SDL_SOURCE_DIR}" SDL EXCLUDE_FROM_ALL)

# The console artwork, turned into source so it travels inside the library.
# The same script the desktop and Switch builds run, so all three cannot end
# up with different pictures in them.
set(BEZEL_SOURCES "")
function(dualscreen_embed_asset image symbol out_variable)
    set(asset "${DUALSCREEN_ROOT}/assets/${image}")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated/${symbol}.cpp")
    add_custom_command(
        OUTPUT "${generated}"
        COMMAND ${CMAKE_COMMAND}
                -DINPUT=${asset}
                -DOUTPUT=${generated}
                -DSYMBOL=${symbol}
                -P ${DUALSCREEN_ROOT}/tools/embed_asset.cmake
        DEPENDS "${asset}" "${DUALSCREEN_ROOT}/tools/embed_asset.cmake"
        COMMENT "Embedding assets/${image}"
        VERBATIM)
    set(${out_variable} "${${out_variable}};${generated}" PARENT_SCOPE)
endfunction()

dualscreen_embed_asset(ds_bezel.png kDsBezelPng BEZEL_SOURCES)
dualscreen_embed_asset(three_ds_bezel.png kThreeDsBezelPng BEZEL_SOURCES)
dualscreen_embed_asset(inter.ttf kUiFont BEZEL_SOURCES)
dualscreen_embed_asset(stylus_hand.png kStylusHandPng BEZEL_SOURCES)
dualscreen_embed_asset(button_icons.png kButtonIconsPng BEZEL_SOURCES)

include(${DUALSCREEN_ROOT}/cmake/companion_sources.cmake)

# SDLActivity loads a library called "main" and calls SDL_main in it, so the
# name is not ours to pick.
add_library(main SHARED
    ${BEZEL_SOURCES}
    ${DUALSCREEN_COMPANION_SOURCES}
)

target_include_directories(main PRIVATE
    ${DUALSCREEN_ROOT}/src
    ${DUALSCREEN_ROOT}/third_party
    ${DUALSCREEN_ROOT}/companion/source
)

# No GL is named at link time: gl_loader.cpp reaches every entry point through
# SDL_GL_GetProcAddress, exactly as the Switch build does. zlib is the frame
# codec's, and the NDK ships it. `log` is what logcat output goes through.
target_link_libraries(main PRIVATE SDL2 SDL2main z log android)

# Optional, the same way the desktop build and the Switch one both treat it:
# the GamePad screen carries fine as changed tiles without it, just at
# several times the bandwidth. There is no one official FFmpeg-for-Android
# package the way switch-ffmpeg is for the Switch, so this expects a
# self-built one -- see android/README.md for the configure line that gets
# h264_mediacodec built in -- laid out as <dir>/<ANDROID_ABI>/{include,lib},
# which is what FFmpeg's own android build instructions produce.
# FFMPEG_ANDROID_DIR names it, the same way SDL2_ANDROID_SRC names the SDL2
# source above; ANDROID_ABI is set for us by the NDK's own CMake toolchain
# file, to whichever ABI this particular build is for.
if(DEFINED ENV{FFMPEG_ANDROID_DIR})
    set(FFMPEG_DIR "$ENV{FFMPEG_ANDROID_DIR}/${ANDROID_ABI}")
endif()
if(FFMPEG_DIR AND EXISTS "${FFMPEG_DIR}/lib/libavcodec.a")
    message(STATUS "video codec: libavcodec found at ${FFMPEG_DIR}, building with MediaCodec")
    target_compile_definitions(main PRIVATE DS_HAVE_VIDEO_CODEC=1)
    target_include_directories(main PRIVATE "${FFMPEG_DIR}/include")
    # --start-group/--end-group rather than a fixed order: what a from-scratch
    # FFmpeg build actually needs beyond these three depends on how it was
    # configured -- a build with libdav1d or libass enabled statically links
    # those into libavcodec.a's own symbol table the same way switch-ffmpeg
    # does, and there is no one list of extras that is right for every build
    # a reader might have made. Add more .a files to this list if the link
    # asks for symbols none of these three provide.
    target_link_libraries(main PRIVATE
        -Wl,--start-group
        "${FFMPEG_DIR}/lib/libavcodec.a"
        "${FFMPEG_DIR}/lib/libavutil.a"
        "${FFMPEG_DIR}/lib/libswscale.a"
        -Wl,--end-group)
else()
    message(STATUS "video codec: FFMPEG_ANDROID_DIR not set or incomplete, building without one")
    target_compile_definitions(main PRIVATE DS_HAVE_VIDEO_CODEC=0)
endif()

target_compile_options(main PRIVATE -Wall -Wextra)
