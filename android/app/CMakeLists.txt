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
dualscreen_embed_asset(rodin.otf kRodinOtf BEZEL_SOURCES)
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

target_compile_options(main PRIVATE -Wall -Wextra)
