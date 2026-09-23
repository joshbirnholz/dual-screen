# The Dualscreen companion, as an Apple application: macOS, iOS, iPadOS, tvOS.
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
# Four products, three platforms, one source list. What actually differs
# between them is smaller than it looks, and is named where it occurs:
#
#   macOS   desktop OpenGL, Homebrew's SDL2 and FFmpeg, a window, an .icns
#   iOS     OpenGL ES, SDL2 built here, touch, a sandbox to write in
#   iPadOS  iOS exactly -- one binary, one bundle; iPad is not a target
#   tvOS    iOS without the touch screen, and with nowhere to keep a file
#
# See apple/README.md for what has been run on real hardware and what has not.
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

# --- which Apple platform ---------------------------------------------------
#
# CMAKE_SYSTEM_NAME is how CMake is told, and it must be set on the configure
# line before anything here runs:
#
#   cmake -S apple -B build-ios  -G Xcode -DCMAKE_SYSTEM_NAME=iOS
#   cmake -S apple -B build-tvos -G Xcode -DCMAKE_SYSTEM_NAME=tvOS
#
# Left unset it is "Darwin", which is a Mac. There is no autodetection to do
# here and guessing would be worse than asking: a build for the wrong platform
# fails late, in the linker, over a framework that does not exist.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(DUALSCREEN_PLATFORM "ios")
elseif(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    set(DUALSCREEN_PLATFORM "tvos")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(DUALSCREEN_PLATFORM "macos")
else()
    message(FATAL_ERROR
        "CMAKE_SYSTEM_NAME is '${CMAKE_SYSTEM_NAME}'. This builds for Darwin "
        "(macOS), iOS or tvOS.")
endif()

# iOS and tvOS together, which is the distinction that actually matters to
# nearly everything below: OpenGL ES rather than desktop GL, a bundle signed
# for a device rather than for a desk, and no package manager to find a
# library with.
if(DUALSCREEN_PLATFORM STREQUAL "macos")
    set(DUALSCREEN_EMBEDDED OFF)
else()
    set(DUALSCREEN_EMBEDDED ON)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

get_filename_component(DUALSCREEN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# --- how old a system this will run on --------------------------------------
#
# macOS 11 is the first release on Apple silicon.
#
# iOS and tvOS are 17, which is a choice rather than a floor: Xcode 27 will
# deploy as low as 15, and the one thing this app needs from the OS itself --
# UILaunchScreen, without which an app is handed a window the size of an
# iPhone 4 and letterboxed -- has been there since 14. 17 is chosen because
# picking up two more years of VideoToolbox is worth more to an app that
# decodes video than two more years of devices are, and because it is the
# oldest release still getting Apple's security fixes. It buys, among other
# things, being able to simply ask for the hardware decoder rather than work
# around not being able to (see videotoolbox_decoder.cpp).
#
# All three are one number rather than a rule, because raising them is a
# decision to be made, not one to drift into.
if(DUALSCREEN_PLATFORM STREQUAL "macos")
    set(DUALSCREEN_MIN_OS "11.0")
else()
    set(DUALSCREEN_MIN_OS "17.0")
endif()
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${DUALSCREEN_MIN_OS}" CACHE STRING "" FORCE)
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

# UIKit's scene life cycle, which SDL2 never adopted and apps built against
# the iOS 26 SDK are killed at launch for lacking. See the file itself; macOS
# has no UIScene and wants none of it.
set(DUALSCREEN_PLATFORM_SOURCES "")
if(NOT DUALSCREEN_PLATFORM STREQUAL "macos")
    list(APPEND DUALSCREEN_PLATFORM_SOURCES
         "${CMAKE_CURRENT_SOURCE_DIR}/ios_scene_lifecycle.m")
endif()

add_executable(dualscreen_companion MACOSX_BUNDLE
    ${BEZEL_SOURCES}
    ${DUALSCREEN_COMPANION_SOURCES}
    ${DUALSCREEN_PLATFORM_SOURCES}
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
# On a Mac, Homebrew's ships CMake config files and this finds it with nothing
# said. On iOS and tvOS there is no Homebrew and no system copy, so SDL is
# built here as a subproject -- from a checkout if one is pointed at, and
# otherwise fetched.
#
# SDL2main is not optional on any Apple platform the way it is on Linux: SDL.h
# renames main to SDL_main, and SDL2main provides the real one and stands up
# the Cocoa or UIKit application object behind it. Without it the link fails on
# a missing _main, which reads as something far stranger than it is.
# An SDL2 that is already built and installed, which is the way to get an
# Xcode project out of this.
#
# Every configure failure seen on iOS so far has been inside SDL's own feature
# checks, not this project's -- a hundred lines of "not found" for things like
# stdio.h, ending at the first one SDL treats as fatal. Those checks are
# try_compile calls, and CMake runs a try_compile under the Xcode generator by
# driving xcodebuild, which is the part that breaks with a new Xcode.
#
# Handing over an SDL that is already built removes all of them at a stroke.
# What is left in this file is find_library for frameworks and find_package for
# zlib, neither of which compiles anything, so the Xcode generator has almost
# nothing left to trip over.
#
# Build it once, with the generator that works, and point this at it:
#
#   git clone --depth 1 -b release-2.30.9 https://github.com/libsdl-org/SDL.git
#   cmake -S SDL -B sdl-ios -G Ninja \
#         -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
#         -DCMAKE_OSX_ARCHITECTURES=arm64 \
#         -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF \
#         -DCMAKE_INSTALL_PREFIX=$PWD/sdl-ios-install
#   cmake --build sdl-ios --target install
#
# SDL installs its own CMake package files, so the frameworks a static SDL
# needs come with it rather than having to be listed again here.
set(DUALSCREEN_SDL2_PREFIX "" CACHE PATH
    "An installed SDL2 to link, rather than building one (iOS/tvOS)")
set(DUALSCREEN_SDL2_SOURCE_DIR "" CACHE PATH
    "A local SDL2 checkout to build against, instead of fetching one (iOS/tvOS)")
set(DUALSCREEN_SDL2_TAG "release-2.30.9" CACHE STRING
    "The SDL2 tag to fetch when no checkout is given (iOS/tvOS)")

if(DUALSCREEN_EMBEDDED AND DUALSCREEN_SDL2_PREFIX)
    message(STATUS "SDL2: using the build at ${DUALSCREEN_SDL2_PREFIX}")
    list(APPEND CMAKE_PREFIX_PATH "${DUALSCREEN_SDL2_PREFIX}")
    # CMAKE_FIND_ROOT_PATH_BOTH, or this is never found.
    #
    # Setting CMAKE_SYSTEM_NAME puts CMake into cross-compiling mode, and in
    # that mode every search is re-rooted into the sysroot:
    # CMAKE_FIND_ROOT_PATH_MODE_PACKAGE defaults to ONLY for iOS and tvOS, so
    # find_package looks inside iPhoneSimulator.sdk and nowhere else --
    # CMAKE_PREFIX_PATH included. An SDL built and installed anywhere outside
    # that SDK is invisible, and the error says only that no SDL2Config.cmake
    # could be found, which reads as "you did not install it" rather than "I
    # did not look where you put it".
    #
    # BOTH rather than NO_CMAKE_FIND_ROOT_PATH: the sysroot is still searched
    # first, and only then the paths given here.
    find_package(SDL2 CONFIG REQUIRED CMAKE_FIND_ROOT_PATH_BOTH)
    if(TARGET SDL2::SDL2-static)
        target_link_libraries(dualscreen_companion PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2::SDL2)
        target_link_libraries(dualscreen_companion PRIVATE SDL2::SDL2)
    else()
        message(FATAL_ERROR
            "${DUALSCREEN_SDL2_PREFIX} has SDL2 package files but no library "
            "target this knows. Was it installed from an SDL2 build?")
    endif()
    if(TARGET SDL2::SDL2main)
        target_link_libraries(dualscreen_companion PRIVATE SDL2::SDL2main)
    endif()
elseif(DUALSCREEN_EMBEDDED)
    # Feature checks must not try to *link*, or every one of them fails.
    #
    # check_symbol_exists() and friends build a tiny program and, by default,
    # link it into an executable. Cross-compiling to iOS or tvOS that means
    # producing a real app binary, which Xcode will not do without a signing
    # identity and a device architecture it is happy with -- so the probe
    # fails, and a failed probe reads as "this platform does not have it".
    #
    # The failure is spectacular rather than subtle, and misleading in exactly
    # the way that costs an hour: SDL's configure reported no stdio.h, no
    # memcpy, no M_PI and no arm64, then stopped on the first check it treats
    # as fatal -- "Threads are needed by many SDL subsystems" -- which is the
    # one line anybody reads. Threads were never the problem. Nothing compiled
    # at all.
    #
    # STATIC_LIBRARY makes the probes compile without linking, which is what
    # CMake documents for cross-compiling and is enough to answer every
    # question SDL is actually asking. Not forced: somebody debugging this
    # wants to be able to put it back.
    if(NOT DEFINED CMAKE_TRY_COMPILE_TARGET_TYPE)
        set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    endif()

    # SDL 2.30 predates CMake 4, and CMake 4 is stricter about that than any
    # release before it: a project declaring an old cmake_minimum_required gets
    # policies it was never written against, and the first sign of it is
    # feature checks that all fail for no stated reason. This is the escape
    # hatch CMake documents for exactly that -- it says "treat this old project
    # as though it asked for 3.10" -- and it applies to the fetched subproject
    # rather than to anything here.
    #
    # Overridable, because the right long-term answer is a newer SDL rather
    # than a compatibility flag, and somebody trying one wants this out of the
    # way.
    if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
        set(CMAKE_POLICY_VERSION_MINIMUM "3.10")
    endif()

    # Static, because a bundle for a device cannot go looking for a dylib on a
    # machine it is not installed on.
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    if(DUALSCREEN_SDL2_SOURCE_DIR)
        message(STATUS "SDL2: building ${DUALSCREEN_SDL2_SOURCE_DIR}")
        add_subdirectory("${DUALSCREEN_SDL2_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/sdl2" EXCLUDE_FROM_ALL)
    else()
        message(STATUS "SDL2: fetching ${DUALSCREEN_SDL2_TAG} (set "
                       "DUALSCREEN_SDL2_SOURCE_DIR to build a checkout instead)")
        include(FetchContent)
        FetchContent_Declare(SDL2
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG "${DUALSCREEN_SDL2_TAG}"
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(SDL2)
    endif()
    # Which target name exists depends on how SDL was configured; asking is
    # cheaper than assuming, and assuming wrong is a link error about a target
    # that was never built.
    if(TARGET SDL2::SDL2-static)
        target_link_libraries(dualscreen_companion PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2-static)
        target_link_libraries(dualscreen_companion PRIVATE SDL2-static)
    elseif(TARGET SDL2::SDL2)
        target_link_libraries(dualscreen_companion PRIVATE SDL2::SDL2)
    else()
        message(FATAL_ERROR "SDL2 was built but exported no target this knows.")
    endif()
    if(TARGET SDL2::SDL2main)
        target_link_libraries(dualscreen_companion PRIVATE SDL2::SDL2main)
    elseif(TARGET SDL2main)
        target_link_libraries(dualscreen_companion PRIVATE SDL2main)
    endif()
else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(dualscreen_companion PRIVATE SDL2::SDL2 SDL2::SDL2main)
endif()

find_package(ZLIB REQUIRED)
target_link_libraries(dualscreen_companion PRIVATE ZLIB::ZLIB)

# Threads, and not REQUIRED on a device.
#
# find_package(Threads) answers by compiling and linking a test program, which
# is the same thing that fails when cross-compiling for iOS -- so requiring it
# turns a detection quirk into a configure error, on a platform where the
# answer is known anyway: pthreads are in libSystem on every Apple OS and there
# is nothing to link.
if(DUALSCREEN_EMBEDDED)
    find_package(Threads QUIET)
    if(TARGET Threads::Threads)
        target_link_libraries(dualscreen_companion PRIVATE Threads::Threads)
    else()
        message(STATUS "threads: in libSystem, nothing to link")
    endif()
else()
    find_package(Threads REQUIRED)
    target_link_libraries(dualscreen_companion PRIVATE Threads::Threads)
endif()

# --- OpenGL -----------------------------------------------------------------
#
# gl_loader.cpp reaches every entry point through SDL_GL_GetProcAddress, so
# this is the framework and nothing more. Which framework is the whole of the
# difference: macOS has desktop GL and no ES, iOS and tvOS have ES and no
# desktop GL. See GlFamily in render/gl_window.hpp, where the same split
# decides which context to ask for -- and test_gl_window, which checks it from
# a machine that is none of the three.
if(DUALSCREEN_EMBEDDED)
    find_library(OPENGLES_FRAMEWORK OpenGLES REQUIRED)
    target_link_libraries(dualscreen_companion PRIVATE ${OPENGLES_FRAMEWORK})
else()
    find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
    target_link_libraries(dualscreen_companion PRIVATE ${OPENGL_FRAMEWORK})
endif()

# --- the video codec --------------------------------------------------------
#
# Optional, exactly as it is everywhere else: a DS or 3DS screen carries fine
# as changed tiles without it. A Wii U's does not, in practice -- see the
# bandwidth measurements in docs/research-notes.md -- so a device meant for
# Wii U sessions wants this.
#
# On a Mac that is `brew install ffmpeg`, and Homebrew's build has VideoToolbox
# in it. On iOS and tvOS there is no such thing: FFmpeg has to be cross-built
# for the device and handed over, which is why this defaults to off there
# rather than failing a build over a library nobody has yet. Point
# DUALSCREEN_FFMPEG_DIR at a prefix holding include/ and lib/ for those.
set(DUALSCREEN_FFMPEG_DIR "" CACHE PATH
    "A prefix with FFmpeg's include/ and lib/, cross-built for the device (iOS/tvOS)")

# --- Apple's own decoder ----------------------------------------------------
#
# VideoToolbox, linked always and on every Apple platform, because it is no
# longer optional: videotoolbox_decoder.cpp decodes H.264 through it directly,
# with no FFmpeg anywhere in the path. That is what gives an iPhone or an Apple
# TV H.264 at all -- there is no Homebrew for a phone -- and it is what those
# platforms use even when FFmpeg is present.
#
# It was already doing the decoding before this: FFmpeg's h264 decoder with a
# VideoToolbox device attached is a wrapper around exactly these frameworks.
# What has gone is the wrapper.
foreach(framework VideoToolbox CoreMedia CoreVideo CoreFoundation)
    find_library(${framework}_FRAMEWORK ${framework} REQUIRED)
    target_link_libraries(dualscreen_companion PRIVATE ${${framework}_FRAMEWORK})
endforeach()

set(DUALSCREEN_HAVE_CODEC OFF)
if(DUALSCREEN_EMBEDDED)
    if(DUALSCREEN_FFMPEG_DIR)
        set(DUALSCREEN_HAVE_CODEC ON)
        target_include_directories(dualscreen_companion PRIVATE "${DUALSCREEN_FFMPEG_DIR}/include")
        foreach(library avcodec avutil swscale swresample)
            # CMAKE_FIND_ROOT_PATH_BOTH for the same reason SDL2 needs it
            # above: a cross-compiling find_library only looks inside the
            # sysroot, and NO_DEFAULT_PATH does not exempt an explicit PATHS
            # from being re-rooted. Without this a cross-built FFmpeg handed
            # over here would never be found either.
            find_library(FFMPEG_${library}_LIB ${library}
                         PATHS "${DUALSCREEN_FFMPEG_DIR}/lib" NO_DEFAULT_PATH
                         CMAKE_FIND_ROOT_PATH_BOTH)
            if(FFMPEG_${library}_LIB)
                target_link_libraries(dualscreen_companion PRIVATE ${FFMPEG_${library}_LIB})
            elseif(NOT library STREQUAL "swresample")
                message(FATAL_ERROR
                    "DUALSCREEN_FFMPEG_DIR is set but lib${library} is not in "
                    "${DUALSCREEN_FFMPEG_DIR}/lib")
            endif()
        endforeach()
        message(STATUS "video codec: ${DUALSCREEN_FFMPEG_DIR}, decoding through VideoToolbox")
    else()
        message(STATUS "video codec: VideoToolbox only, which is all this needs "
                       "-- H.264 decodes through Apple's own decoder")
    endif()
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        # Asked afresh on every configure -- see the same line in the
        # top-level CMakeLists.txt. `brew upgrade` deletes the old version's
        # Cellar directory, and a cached answer would go on naming it.
        unset(__pkg_config_checked_FFMPEG CACHE)
        pkg_check_modules(FFMPEG IMPORTED_TARGET libavcodec libavutil libswscale)
    endif()
    if(FFMPEG_FOUND)
        set(DUALSCREEN_HAVE_CODEC ON)
        message(STATUS "video codec: libavcodec ${FFMPEG_libavcodec_VERSION}, decoding through VideoToolbox")
        target_link_libraries(dualscreen_companion PRIVATE PkgConfig::FFMPEG)
    else()
        message(STATUS "video codec: not found, building without one (brew install ffmpeg)")
    endif()
endif()

# FFmpeg is now only what encodes, and what decodes anything that is not
# H.264. The frameworks its VideoToolbox hwaccel needs are already linked
# above, which is convenient: on a device every FFmpeg is a static one, and a
# static hwaccel's symbols are in the .a while its frameworks are not.
if(DUALSCREEN_HAVE_CODEC)
    target_compile_definitions(dualscreen_companion PRIVATE DS_HAVE_VIDEO_CODEC=1)
else()
    target_compile_definitions(dualscreen_companion PRIVATE DS_HAVE_VIDEO_CODEC=0)
endif()

# --- the bundle -------------------------------------------------------------
set(DUALSCREEN_BUNDLE_ID "com.joshbirnholz.dualscreen.companion")

if(DUALSCREEN_PLATFORM STREQUAL "macos")
    set(DUALSCREEN_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")
elseif(DUALSCREEN_PLATFORM STREQUAL "ios")
    set(DUALSCREEN_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info-iOS.plist.in")
else()
    set(DUALSCREEN_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info-tvOS.plist.in")
endif()

# An icon on the Mac, built from the artwork already in the repository rather
# than committed again as a second copy of the same picture. iconutil and sips
# are part of macOS, so this needs nothing installed -- but it is not worth
# failing a build over, so a missing tool means a bundle with the generic icon
# on it.
#
# iOS and tvOS want an asset catalog instead, which is a different shape and a
# different tool, and a malformed one fails the build rather than degrading to
# a blank square. See apple/README.md: adding one in Xcode is a two-minute job
# and is the point at which somebody is looking at the app on a device anyway.
set(DUALSCREEN_ICON_FILE "")
if(NOT DUALSCREEN_EMBEDDED)
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
            # The sizes iconutil insists on. A square crop of a picture that is
            # not square is what sips -z does here, which is the right shape
            # even if it is not yet the right picture -- see apple/README.md.
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
    endif()
endif()

set_target_properties(dualscreen_companion PROPERTIES
    OUTPUT_NAME "${DUALSCREEN_APP_NAME}"
    MACOSX_BUNDLE_BUNDLE_NAME "${DUALSCREEN_APP_NAME}"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${DUALSCREEN_BUNDLE_ID}"
    MACOSX_BUNDLE_BUNDLE_VERSION "0.1.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
    MACOSX_BUNDLE_ICON_FILE "${DUALSCREEN_ICON_FILE}"
    MACOSX_BUNDLE_INFO_PLIST "${DUALSCREEN_PLIST}"
)

if(DUALSCREEN_EMBEDDED)
    # Xcode signs a device build, and it needs the identifier on the target as
    # well as in the plist -- the plist is what ends up in the bundle, this is
    # what the signing step reads. A team is not set here: it belongs to
    # whoever is building, and is given on the configure line or picked in
    # Xcode's own settings.
    #
    #   -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=XXXXXXXXXX
    #
    # Without one, a simulator build still works; a build for a real device
    # will not sign.
    #
    # One binary for iPhone and iPad -- device family 1 and 2. There is no
    # separate iPad product and no separate layout: the app already sizes
    # itself from the panel it is given, which is the same thing it does when a
    # Switch is undocked. An Apple TV is family 3 and is its own build, because
    # it is its own platform and not a size of this one.
    #
    # Set plainly rather than through a generator expression: the commas in
    # "1,2" are argument separators to $<IF:>, so the obvious one-liner
    # silently produces a device family of "1".
    if(DUALSCREEN_PLATFORM STREQUAL "tvos")
        set(DUALSCREEN_DEVICE_FAMILY "3")
    else()
        set(DUALSCREEN_DEVICE_FAMILY "1,2")
    endif()
    set_target_properties(dualscreen_companion PROPERTIES
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${DUALSCREEN_BUNDLE_ID}"
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "${DUALSCREEN_DEVICE_FAMILY}")
else()
    # Signed, even if only ad-hoc.
    #
    # Not about trust: it is how macOS knows one build of this app from
    # another. The permission to use the local network is remembered against a
    # bundle's identity, and a bundle with no signature has none to remember --
    # so the grant has nothing to attach to, and a connection to a machine on
    # the same network comes back EHOSTUNREACH, which prints as "No route to
    # host" and sends you to look at your router. See explain_connect_error()
    # in transport.cpp, which catches that sentence on the way out.
    #
    # An ad-hoc signature changes with every build, so macOS may ask again
    # after a rebuild. That is worth it against not being asked at all.
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
            "Built $<TARGET_BUNDLE_DIR:dualscreen_companion> for ${DUALSCREEN_PLATFORM} -- ${DUALSCREEN_BUILD_STAMP}"
    VERBATIM)
