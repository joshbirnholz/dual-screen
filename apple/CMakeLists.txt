# The Dualscreen companion, as an Apple application.
#
# It builds the same sources as the desktop, Switch and Android clients -- read
# from cmake/companion_sources.cmake, so a file cannot be added to the client
# and forgotten here -- and wraps them in a bundle, which is the part that is
# Apple's rather than this app's.
#
# Separate from the root CMakeLists.txt rather than a branch inside it, for the
# same reason android/app has its own: that one builds the host as well, and
# the host is a program for running emulator cores beside a window manager. It
# has no meaning here. This builds the companion and nothing else.
#
# macOS today. iOS and tvOS are the reason the file is arranged the way it is
# -- one source list, one plist template, and the differences between the three
# named where they occur rather than assumed away -- but only macOS is built
# and only macOS has been run. Nothing below claims otherwise.
cmake_minimum_required(VERSION 3.20)
project(dualscreen_companion LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT APPLE)
    message(FATAL_ERROR
        "This directory builds the Apple companion. For Linux, configure the "
        "repository root instead -- it builds the same client as "
        "dualscreen-companion.")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

get_filename_component(DUALSCREEN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# Big Sur is the floor: the first release on Apple silicon, so it is the oldest
# macOS anybody is running this on a machine it was built for. Nothing here
# needs anything newer.
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

# The console artwork, turned into source so it travels inside the binary. The
# same script every other build runs, so no two can end up with different
# pictures in them.
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

include(${DUALSCREEN_ROOT}/cmake/build_stamp.cmake)

set(DUALSCREEN_APP_NAME "Dualscreen Companion")

add_executable(dualscreen_companion MACOSX_BUNDLE
    ${BEZEL_SOURCES}
    ${DUALSCREEN_COMPANION_SOURCES}
)

target_include_directories(dualscreen_companion PRIVATE
    ${DUALSCREEN_ROOT}/src
    ${DUALSCREEN_ROOT}/third_party
    ${DUALSCREEN_ROOT}/companion/source
)
target_compile_options(dualscreen_companion PRIVATE -Wall -Wextra)
dualscreen_stamp_build("${DUALSCREEN_ROOT}/src/util/build_stamp.cpp")

# --- SDL2 -------------------------------------------------------------------
#
# Homebrew's SDL2 ships CMake config files, so this finds it with nothing said.
# SDL2main is not optional here the way it is on Linux: on Apple platforms
# SDL.h renames main to SDL_main, and SDL2main is what provides the real one
# and sets up the Cocoa application object behind it. Without it the link fails
# on a missing _main, which reads as something far stranger than it is.
find_package(SDL2 REQUIRED)
target_link_libraries(dualscreen_companion PRIVATE SDL2::SDL2 SDL2::SDL2main)

find_package(ZLIB REQUIRED)
target_link_libraries(dualscreen_companion PRIVATE ZLIB::ZLIB)

find_package(Threads REQUIRED)
target_link_libraries(dualscreen_companion PRIVATE Threads::Threads)

# OpenGL, which macOS deprecated and still ships. gl_loader.cpp reaches every
# entry point through SDL_GL_GetProcAddress, so this is the framework and
# nothing more.
find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
target_link_libraries(dualscreen_companion PRIVATE ${OPENGL_FRAMEWORK})

# --- the video codec --------------------------------------------------------
#
# Optional, exactly as it is everywhere else: a DS or 3DS screen carries fine
# as changed tiles without it. A Wii U's does not, in practice -- see the
# bandwidth measurements in docs/research-notes.md -- so a Mac meant for Wii U
# sessions wants this.
#
# `brew install ffmpeg` is the whole of it, and Homebrew's build has
# VideoToolbox in it, which is the hardware decoder on every Apple machine.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG IMPORTED_TARGET libavcodec libavutil libswscale)
endif()
if(FFMPEG_FOUND)
    message(STATUS "video codec: libavcodec ${FFMPEG_libavcodec_VERSION}, decoding through VideoToolbox")
    target_link_libraries(dualscreen_companion PRIVATE PkgConfig::FFMPEG)
    target_compile_definitions(dualscreen_companion PRIVATE DS_HAVE_VIDEO_CODEC=1)
    # Homebrew's libavcodec is a dylib and brings its own framework
    # dependencies, so these are only needed when somebody points this at a
    # static FFmpeg they built themselves -- where the VideoToolbox hwaccel's
    # symbols are in the .a and its frameworks are not. Harmless either way.
    foreach(framework VideoToolbox CoreMedia CoreVideo CoreFoundation)
        find_library(${framework}_FRAMEWORK ${framework})
        if(${framework}_FRAMEWORK)
            target_link_libraries(dualscreen_companion PRIVATE ${${framework}_FRAMEWORK})
        endif()
    endforeach()
else()
    message(STATUS "video codec: not found, building without one (brew install ffmpeg)")
    target_compile_definitions(dualscreen_companion PRIVATE DS_HAVE_VIDEO_CODEC=0)
endif()

# --- the bundle -------------------------------------------------------------
#
# An icon, built from the artwork already in the repository rather than
# committed again as a second copy of the same picture. iconutil and sips are
# part of macOS, so this needs nothing installed -- but it is not worth failing
# a build over, so a missing tool means a bundle with the generic icon on it.
set(ICON_NAME "Dualscreen")
set(ICON_SOURCE "${DUALSCREEN_ROOT}/assets/ds_bezel.png")
set(ICONSET_DIR "${CMAKE_CURRENT_BINARY_DIR}/${ICON_NAME}.iconset")
set(ICNS_FILE "${CMAKE_CURRENT_BINARY_DIR}/${ICON_NAME}.icns")
find_program(SIPS_TOOL sips)
find_program(ICONUTIL_TOOL iconutil)
if(SIPS_TOOL AND ICONUTIL_TOOL AND EXISTS "${ICON_SOURCE}")
    add_custom_command(
        OUTPUT "${ICNS_FILE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ICONSET_DIR}"
        # The sizes iconutil insists on. A square crop of a picture that is not
        # square is what sips -z does here, which is the right shape even if it
        # is not yet the right picture -- see apple/README.md.
        COMMAND ${SIPS_TOOL} -z 16 16     "${ICON_SOURCE}" --out "${ICONSET_DIR}/icon_16x16.png" > /dev/null
        COMMAND ${SIPS_TOOL} -z 32 32     "${ICON_SOURCE}" --out "${ICONSET_DIR}/icon_16x16@2x.png" > /dev/null
        COMMAND ${SIPS_TOOL} -z 32 32     "${ICON_SOURCE}" --out "${ICONSET_DIR}/icon_32x32.png" > /dev/null
        COMMAND ${SIPS_TOOL} -z 64 64     "${ICON_SOURCE}" --out "${ICONSET_DIR}/icon_32x32@2x.png" > /dev/null
        COMMAND ${SIPS_TOOL} -z 128 128   "${ICON_SOURCE}" --out "${ICONSET_DIR}/icon_128x128.png" > /dev/null
        COMMAND ${SIPS_TOOL} -z 256 256   "${ICON_SOURCE}" --out "${ICONSET_DIR}/icon_128x128@2x.png" > /dev/null
        COMMAND ${SIPS_TOOL} -z 256 256   "${ICON_SOURCE}" --out "${ICONSET_DIR}/icon_256x256.png" > /dev/null
        COMMAND ${SIPS_TOOL} -z 512 512   "${ICON_SOURCE}" --out "${ICONSET_DIR}/icon_256x256@2x.png" > /dev/null
        COMMAND ${SIPS_TOOL} -z 512 512   "${ICON_SOURCE}" --out "${ICONSET_DIR}/icon_512x512.png" > /dev/null
        COMMAND ${SIPS_TOOL} -z 1024 1024 "${ICON_SOURCE}" --out "${ICONSET_DIR}/icon_512x512@2x.png" > /dev/null
        COMMAND ${ICONUTIL_TOOL} -c icns "${ICONSET_DIR}" -o "${ICNS_FILE}"
        DEPENDS "${ICON_SOURCE}"
        COMMENT "Building ${ICON_NAME}.icns"
        VERBATIM)
    target_sources(dualscreen_companion PRIVATE "${ICNS_FILE}")
    set_source_files_properties("${ICNS_FILE}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")
    set(DUALSCREEN_ICON_FILE "${ICON_NAME}")
else()
    message(STATUS "icon: sips/iconutil not found, building without one")
    set(DUALSCREEN_ICON_FILE "")
endif()

set_target_properties(dualscreen_companion PROPERTIES
    OUTPUT_NAME "${DUALSCREEN_APP_NAME}"
    MACOSX_BUNDLE_BUNDLE_NAME "${DUALSCREEN_APP_NAME}"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.joshbirnholz.dualscreen.companion"
    MACOSX_BUNDLE_BUNDLE_VERSION "0.1.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
    MACOSX_BUNDLE_ICON_FILE "${DUALSCREEN_ICON_FILE}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
)

# Signed, even if only ad-hoc.
#
# Not about trust: it is how macOS knows one build of this app from another.
# The permission to use the local network is remembered against a bundle's
# identity, and a bundle with no signature has none to remember -- so the grant
# has nothing to attach to, and a connection to a machine on the same network
# comes back EHOSTUNREACH, which prints as "No route to host" and sends you to
# look at your router. See explain_connect_error() in transport.cpp, which
# catches that sentence on the way out.
#
# An ad-hoc signature changes with every build, so macOS may ask again after a
# rebuild. That is worth it against not being asked at all.
find_program(CODESIGN_TOOL codesign)
if(CODESIGN_TOOL)
    add_custom_command(TARGET dualscreen_companion POST_BUILD
        COMMAND ${CODESIGN_TOOL} --force --sign -
                "$<TARGET_BUNDLE_DIR:dualscreen_companion>"
        COMMENT "Signing the bundle (ad-hoc)"
        VERBATIM)
else()
    message(STATUS "codesign not found; the bundle will be unsigned and macOS "
                   "may refuse it the local network")
endif()

# What was just written, and what is in it.
#
# A bundle is a directory with a fixed name, so a build that wrote a new one
# and a build that wrote nothing look identical afterwards -- the folder is
# there either way, with the same name and the same icon. Printing the stamp
# here means the build output itself says what it produced, without having to
# start the app to find out.
add_custom_command(TARGET dualscreen_companion POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo
            "Built $<TARGET_BUNDLE_DIR:dualscreen_companion> -- ${DUALSCREEN_BUILD_STAMP}"
    VERBATIM)
