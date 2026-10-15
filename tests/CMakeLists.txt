find_program(XVFB_RUN_EXECUTABLE xvfb-run)

# The mock libretro core: a real .so with the libretro C ABI, so the frontend's
# dlopen path is exercised exactly as it would be with a real core. Built twice
# from one source -- shaped like a DS, and like a 3DS, whose screens are
# different sizes from each other.
function(dualscreen_add_mock_core target output)
    add_library(${target} MODULE mock_core/mock_core.cpp)
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "${output}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    if(ARGN)
        target_compile_definitions(${target} PRIVATE ${ARGN})
    endif()
endfunction()

dualscreen_add_mock_core(mock_core mock_ds_libretro)

# The whole app with the mock core linked into it instead of loaded from a
# file: the shape a Switch build has, with melonDS in the mock's place. The
# loader is compiled again with DS_STATIC_LIBRETRO_CORE, and that object stands
# in for the ordinary one in dualscreen_core -- the linker only takes a member
# of a static library for a symbol nothing earlier defined.
add_executable(dualscreen_builtin_core
    ${CMAKE_SOURCE_DIR}/src/main.cpp
    ${CMAKE_SOURCE_DIR}/src/core/libretro_core.cpp
    mock_core/mock_core.cpp)
target_compile_definitions(dualscreen_builtin_core PRIVATE DS_STATIC_LIBRETRO_CORE=1)
target_link_libraries(dualscreen_builtin_core PRIVATE dualscreen_core)
target_compile_options(dualscreen_builtin_core PRIVATE -Wall -Wextra)

# And the Switch's own program in the shape a Switch build with melonDS has:
# the companion, able to play a DS game itself through the desktop app, with
# the mock core linked in. Everything but libnx -- see local_play.hpp.
add_executable(dualscreen_companion_local_play
    ${CMAKE_SOURCE_DIR}/companion/source/main.cpp
    ${CMAKE_SOURCE_DIR}/companion/source/local_play.cpp
    ${CMAKE_SOURCE_DIR}/src/core/libretro_core.cpp
    mock_core/mock_core.cpp)
target_include_directories(dualscreen_companion_local_play PRIVATE
    ${CMAKE_SOURCE_DIR}/companion/source)
target_compile_definitions(dualscreen_companion_local_play PRIVATE
    DS_STATIC_LIBRETRO_CORE=1 DS_LOCAL_PLAY=1)
target_link_libraries(dualscreen_companion_local_play PRIVATE dualscreen_core)
target_compile_options(dualscreen_companion_local_play PRIVATE -Wall -Wextra)
dualscreen_add_mock_core(mock_core_3ds mock_3ds_libretro MOCK_CORE_3DS)

set(MOCK_CORE_PATH "${CMAKE_BINARY_DIR}/tests/mock_ds_libretro.so")
set(MOCK_CORE_3DS_PATH "${CMAKE_BINARY_DIR}/tests/mock_3ds_libretro.so")

function(dualscreen_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE dualscreen_core)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    # Most tests need no display, and pinning the dummy driver keeps them
    # independent of whatever SDL_VIDEODRIVER is set in the caller's
    # environment. NEEDS_GL marks the ones that do need a real context -- a
    # core rendering on the GPU cannot be tested without one -- and those run
    # under Xvfb where it is available.
    cmake_parse_arguments(ARG "NEEDS_GL" "" "" ${ARGN})
    if(ARG_NEEDS_GL AND XVFB_RUN_EXECUTABLE)
        add_test(NAME ${name} COMMAND ${XVFB_RUN_EXECUTABLE} -a -s "-screen 0 320x240x24" $<TARGET_FILE:${name}>)
        set(driver "x11")
    else()
        add_test(NAME ${name} COMMAND ${name})
        set(driver "dummy")
    endif()
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "DUALSCREEN_MOCK_CORE=${MOCK_CORE_PATH};DUALSCREEN_MOCK_CORE_3DS=${MOCK_CORE_3DS_PATH};SDL_VIDEODRIVER=${driver}")
endfunction()

# A helper binary, not a test itself: the smoke test uses it to inspect output.
# Half of the frame link ends up compiled into a fork of Cemu, and only three
# files go with it. See docs/cemu-integration.md.
add_test(NAME external_link_stands_alone
    COMMAND ${CMAKE_COMMAND}
        -DCOMPILER=${CMAKE_CXX_COMPILER}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}/src
        -DWORK_DIR=${CMAKE_BINARY_DIR}/external-link-alone
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_standalone_link_test.cmake)

# The Apple bundles' property lists. Nothing compiles a plist, so a broken one
# is an app that launches and misbehaves on hardware this repository is not
# developed on -- see the script for the three that have already gone wrong.
add_test(NAME apple_plists
    COMMAND ${CMAKE_COMMAND}
        -DPLIST_DIR=${CMAKE_SOURCE_DIR}/apple
        -DSCRATCH_DIR=${CMAKE_BINARY_DIR}/apple-plists
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_apple_plist_test.cmake)

# What apple/build-all.sh asks cmake for. Two Apple builds have been lost to
# this: one linked against nothing because xcodebuild with no destination
# builds every architecture and the SDL had one, and one offered no Apple TV
# to build for because a device project was configured over a simulator
# project's cache. Stubs stand in for uname, lipo and cmake -- see the script.
add_test(NAME apple_build_flags
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_apple_build_test.sh
        ${CMAKE_SOURCE_DIR}/apple/build-all.sh
        ${CMAKE_BINARY_DIR}/apple-build-flags)

add_executable(check_screenshot check_screenshot.cpp)
target_compile_options(check_screenshot PRIVATE -Wall -Wextra)

dualscreen_add_test(test_layout)
dualscreen_add_test(test_frame_delay)
dualscreen_add_test(test_video_codec)
dualscreen_add_test(test_h264_bitstream)
dualscreen_add_test(test_external_link)
dualscreen_add_test(test_menu_overlay NEEDS_GL)
dualscreen_add_test(test_text)
dualscreen_add_test(test_ui_sounds)
dualscreen_add_test(test_launcher)
dualscreen_add_test(test_game_dialog)
dualscreen_add_test(test_menu_scroll_sync)
dualscreen_add_test(test_menu_entrance)
dualscreen_add_test(test_shutdown_watchdog)
dualscreen_add_test(test_updater)
dualscreen_add_test(test_pad_routing)
dualscreen_add_test(test_image)
dualscreen_add_test(test_gl_window)
dualscreen_add_test(test_protocol)
dualscreen_add_test(test_audio)
dualscreen_add_test(test_dsu)

# The Switch build cannot run here, so at least hold its libnx calls to the
# right signatures -- see run_switch_typecheck.cmake for what that is and is
# not worth.
add_test(NAME switch_motion_typecheck
    COMMAND ${CMAKE_COMMAND}
        -DCOMPILER=${CMAKE_CXX_COMPILER}
        -DSOURCES=${CMAKE_SOURCE_DIR}/src/input/motion.cpp
        -DSTUB_DIR=${CMAKE_CURRENT_SOURCE_DIR}/switch_stub
        -DSRC_DIR=${CMAKE_SOURCE_DIR}/src
        "-DEXTRA_INCLUDES=${SDL2_INCLUDE_DIRS}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_switch_typecheck.cmake)
# The Switch's networking, against the headers a Switch has. transport.cpp is
# compiled for every client and grew an include libnx does not ship, which the
# check above could never have seen: it compiles against this machine's own
# headers, and this machine has them all. tests/switch_stub now also holds
# stand-ins for the headers a Switch lacks, which fail when included.
add_test(NAME switch_network_typecheck
    COMMAND ${CMAKE_COMMAND}
        -DCOMPILER=${CMAKE_CXX_COMPILER}
        -DSOURCES=${CMAKE_SOURCE_DIR}/src/net/transport.cpp
        -DSTUB_DIR=${CMAKE_CURRENT_SOURCE_DIR}/switch_stub
        -DSRC_DIR=${CMAKE_SOURCE_DIR}/src
        "-DEXTRA_INCLUDES=${SDL2_INCLUDE_DIRS}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_switch_typecheck.cmake)
# And the Switch that plays DS games itself: the desktop app with melonDS
# linked in, the paths and file browser on an SD card, and the hand-off
# between the companion's menu and a game. See companion/source/local_play.hpp.
get_target_property(DUALSCREEN_JSON_INCLUDES nlohmann_json::nlohmann_json
                    INTERFACE_INCLUDE_DIRECTORIES)
add_test(NAME switch_local_play_typecheck
    COMMAND ${CMAKE_COMMAND}
        -DCOMPILER=${CMAKE_CXX_COMPILER}
        "-DSOURCES=${CMAKE_SOURCE_DIR}/src/app.cpp ${CMAKE_SOURCE_DIR}/src/core/libretro_core.cpp ${CMAKE_SOURCE_DIR}/src/config/paths.cpp ${CMAKE_SOURCE_DIR}/src/config/settings.cpp ${CMAKE_SOURCE_DIR}/src/ui/game_browser.cpp ${CMAKE_SOURCE_DIR}/companion/source/local_play.cpp"
        -DSTUB_DIR=${CMAKE_CURRENT_SOURCE_DIR}/switch_stub
        -DSRC_DIR=${CMAKE_SOURCE_DIR}/src
        "-DEXTRA_INCLUDES=${SDL2_INCLUDE_DIRS};${CMAKE_SOURCE_DIR}/third_party;${CMAKE_SOURCE_DIR}/companion/source;${DUALSCREEN_JSON_INCLUDES}"
        "-DDEFINES=DS_STATIC_LIBRETRO_CORE=1;DS_LOCAL_PLAY=1;DS_HAVE_VIDEO_CODEC=0"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_switch_typecheck.cmake)

# And whether switch/Makefile still lists everything the companion is built
# from. See the script for the two times it did not.
add_test(NAME switch_makefile_sources
    COMMAND ${CMAKE_COMMAND}
        -DSOURCES_CMAKE=${CMAKE_SOURCE_DIR}/cmake/companion_sources.cmake
        -DSWITCH_MAKEFILE=${CMAKE_SOURCE_DIR}/switch/Makefile
        -DDESKTOP_CMAKE=${CMAKE_SOURCE_DIR}/CMakeLists.txt
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_switch_sources_test.cmake)
# And whether every page of menu is a MenuPage, so a tap on either device
# reaches it. See the script.
add_test(NAME lint_menu_pages
    COMMAND ${CMAKE_COMMAND} -DUI_DIR=${CMAKE_SOURCE_DIR}/src/ui
        -P ${CMAKE_CURRENT_SOURCE_DIR}/lint_menu_pages.cmake)
# And the configuration iOS and tvOS actually ship: no FFmpeg, VideoToolbox
# doing the decoding. Linux always finds FFmpeg, so that half of the fork is
# dead code to every build here and would rot unwatched.
add_test(NAME no_codec_typecheck
    COMMAND ${CMAKE_COMMAND}
        -DCOMPILER=${CMAKE_CXX_COMPILER}
        "-DSOURCES=${CMAKE_SOURCE_DIR}/src/video/video_decoder.cpp;${CMAKE_SOURCE_DIR}/src/video/videotoolbox_decoder.cpp;${CMAKE_SOURCE_DIR}/src/video/mediacodec_decoder.cpp;${CMAKE_SOURCE_DIR}/src/video/h264_bitstream.cpp;${CMAKE_SOURCE_DIR}/src/net/companion_session.cpp;${CMAKE_SOURCE_DIR}/companion/source/main.cpp"
        -DSRC_DIR=${CMAKE_SOURCE_DIR}/src
        "-DEXTRA_INCLUDES=${SDL2_INCLUDE_DIRS};${CMAKE_SOURCE_DIR}/third_party;${CMAKE_SOURCE_DIR}/companion/source;${NLOHMANN_JSON_INCLUDE_DIR};${nlohmann_json_SOURCE_DIR}/include"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_no_codec_typecheck.cmake)

# And the GL a device has, which is OpenGL ES and not the desktop GL every
# build here links against. Registered only where the ES headers exist, since
# nothing else in this repository needs them.
find_path(DUALSCREEN_GLES2_INCLUDE GLES2/gl2.h)
if(DUALSCREEN_GLES2_INCLUDE)
    set(DUALSCREEN_RENDER_SOURCES "")
    foreach(name IN ITEMS bezel_art button_glyphs compositor font gl_loader
                          gl_window menu_backdrop png_image quad_renderer
                          text_renderer texture)
        list(APPEND DUALSCREEN_RENDER_SOURCES
             "${CMAKE_SOURCE_DIR}/src/render/${name}.cpp")
    endforeach()
    add_test(NAME gles_typecheck
        COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=${CMAKE_CXX_COMPILER}
            "-DSOURCES=${DUALSCREEN_RENDER_SOURCES}"
            -DSTUB_DIR=${CMAKE_CURRENT_SOURCE_DIR}/gles_stub
            -DSRC_DIR=${CMAKE_SOURCE_DIR}/src
            "-DEXTRA_INCLUDES=${SDL2_INCLUDE_DIRS};${CMAKE_SOURCE_DIR}/third_party"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_gles_typecheck.cmake)
    # And the Apple halves, which nothing but Xcode compiled before. The ES
    # headers are needed here too: the texture cache is an OpenGL ES API.
    add_test(NAME apple_typecheck
        COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=${CMAKE_CXX_COMPILER}
            "-DSOURCES=${CMAKE_SOURCE_DIR}/src/video/videotoolbox_decoder.cpp;${CMAKE_SOURCE_DIR}/src/render/texture.cpp;${CMAKE_SOURCE_DIR}/src/render/system_video_layer.cpp"
            -DSTUB_DIR=${CMAKE_CURRENT_SOURCE_DIR}/apple_stub
            -DGLES_STUB_DIR=${CMAKE_CURRENT_SOURCE_DIR}/gles_stub
            -DSRC_DIR=${CMAKE_SOURCE_DIR}/src
            "-DEXTRA_INCLUDES=${SDL2_INCLUDE_DIRS};${CMAKE_SOURCE_DIR}/third_party"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_apple_typecheck.cmake)
else()
    message(STATUS "no GLES2 headers here, so the device-GL typecheck is not registered")
endif()

# And the Objective-C that makes an iOS app launch at all. It needs no UIKit on
# this machine -- tests/uikit_stub is the slice of it that file names -- but it
# does need a clang, which not every host has.
find_program(DUALSCREEN_OBJC_CLANG NAMES clang)
if(DUALSCREEN_OBJC_CLANG)
    add_test(NAME uikit_typecheck
        COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=${DUALSCREEN_OBJC_CLANG}
            -DSOURCE=${CMAKE_SOURCE_DIR}/apple/ios_scene_lifecycle.m
            -DSTUB_DIR=${CMAKE_CURRENT_SOURCE_DIR}/uikit_stub
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_uikit_typecheck.cmake)
    # And the layer streamed video is shown in on those platforms -- see
    # src/render/system_video_layer.hpp. The same stubs, with AVFoundation,
    # QuartzCore and CoreMedia's slices added.
    add_test(NAME video_layer_typecheck
        COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=${DUALSCREEN_OBJC_CLANG}
            -DSOURCE=${CMAKE_SOURCE_DIR}/apple/system_video_layer.m
            -DSTUB_DIR=${CMAKE_CURRENT_SOURCE_DIR}/uikit_stub
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_uikit_typecheck.cmake)
else()
    message(STATUS "no clang here, so the UIKit scene typecheck is not registered")
endif()

dualscreen_add_test(test_net)
dualscreen_add_test(test_session NEEDS_GL)
# Discovery, session and media all live on fixed ports (kDiscoveryPort and the
# two after it), so any two tests that stand a real host up cannot run at the
# same time -- the second one's bind fails and it reads as a broken test rather
# than as two tests wanting one port. The headless tests below are already
# RUN_SERIAL, which excludes everything; these three only need to exclude each
# other, and stay free to run alongside the tests that touch no socket.
set_tests_properties(test_net test_session PROPERTIES RESOURCE_LOCK dualscreen_ports)
dualscreen_add_test(test_config)
dualscreen_add_test(test_game_title)
dualscreen_add_test(test_save_points)
dualscreen_add_test(test_save_store)
dualscreen_add_test(test_folder_saves)
if(ZSTD_FOUND)
    # Its .wua archives are compressed when the core can read them so.
    target_link_libraries(test_folder_saves PRIVATE PkgConfig::ZSTD)
    target_compile_definitions(test_folder_saves PRIVATE DUALSCREEN_HAVE_ZSTD=1)
endif()
dualscreen_add_test(test_melonds_settings)
dualscreen_add_test(test_save_exchange)
dualscreen_add_test(test_data_management)
dualscreen_add_test(test_browser)
dualscreen_add_test(test_list_touch)
dualscreen_add_test(test_forwarder)
# The Switch's forwarder packer is portable, so it is tested here too. It is
# GPL-3.0-or-later (see THIRD_PARTY.md); only this test, never shipped, links it.
dualscreen_add_test(test_forwarder_nca)
target_sources(test_forwarder_nca PRIVATE ${CMAKE_SOURCE_DIR}/switch/forwarder/forwarder_nca.cpp)
target_include_directories(test_forwarder_nca PRIVATE ${CMAKE_SOURCE_DIR}/switch)
dualscreen_add_test(test_quick_settings)
dualscreen_add_test(test_graphic_packs)
dualscreen_add_test(test_touch_hints)
dualscreen_add_test(test_pad_glyphs)
dualscreen_add_test(test_touch_gesture)
dualscreen_add_test(test_controller_mapping)
dualscreen_add_test(test_stylus_cursor)
dualscreen_add_test(test_retro_keymap)
dualscreen_add_test(test_controller_input)
dualscreen_add_test(test_switch_motion)
dualscreen_add_test(test_motion_compare)
dualscreen_add_test(test_core_host NEEDS_GL)
add_dependencies(test_core_host mock_core mock_core_3ds)

# End-to-end smoke test: runs the real binary against the mock core under a
# headless X server and checks the composited output pixel by pixel.
if(XVFB_RUN_EXECUTABLE)
    add_test(NAME smoke_headless
        COMMAND ${CMAKE_COMMAND}
            -DXVFB_RUN=${XVFB_RUN_EXECUTABLE}
            -DDUALSCREEN=$<TARGET_FILE:dualscreen>
            -DMOCK_CORE=${MOCK_CORE_PATH}
            -DMOCK_CORE_3DS=${MOCK_CORE_3DS_PATH}
            -DWORK_DIR=${CMAKE_BINARY_DIR}/smoke
            -DCHECKER=$<TARGET_FILE:check_screenshot>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_smoke_test.cmake)
    set_tests_properties(smoke_headless PROPERTIES TIMEOUT 300
        RESOURCE_LOCK dualscreen_ports)

    # The app with its core linked into it, as a Switch has to carry one --
    # see run_builtin_core_test.cmake.
    add_test(NAME builtin_core_headless
        COMMAND ${CMAKE_COMMAND}
            -DXVFB_RUN=${XVFB_RUN_EXECUTABLE}
            -DDUALSCREEN=$<TARGET_FILE:dualscreen_builtin_core>
            -DWORK_DIR=${CMAKE_BINARY_DIR}/builtin-core
            -DCHECKER=$<TARGET_FILE:check_screenshot>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_builtin_core_test.cmake)
    set_tests_properties(builtin_core_headless PROPERTIES TIMEOUT 300
        RESOURCE_LOCK dualscreen_ports)

    # The Switch's own program playing a game itself -- see
    # run_switch_local_play_test.cmake.
    add_test(NAME switch_local_play_headless
        COMMAND ${CMAKE_COMMAND}
            -DXVFB_RUN=${XVFB_RUN_EXECUTABLE}
            -DPROGRAM=$<TARGET_FILE:dualscreen_companion_local_play>
            -DWORK_DIR=${CMAKE_BINARY_DIR}/switch-local-play
            -DCHECKER=$<TARGET_FILE:check_screenshot>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_switch_local_play_test.cmake)
    set_tests_properties(switch_local_play_headless PROPERTIES TIMEOUT 300
        RESOURCE_LOCK dualscreen_ports)

    # Resume from Save State, end to end: a game played with it on leaves a
    # resume point behind when the app closes, and the next start is from
    # there, paused under the popup that says so.
    add_test(NAME resume_headless
        COMMAND ${CMAKE_COMMAND}
            -DXVFB_RUN=${XVFB_RUN_EXECUTABLE}
            -DDUALSCREEN=$<TARGET_FILE:dualscreen>
            -DMOCK_CORE=${MOCK_CORE_PATH}
            -DWORK_DIR=${CMAKE_BINARY_DIR}/resume
            -DCHECKER=$<TARGET_FILE:check_screenshot>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_resume_test.cmake)
    set_tests_properties(resume_headless PROPERTIES TIMEOUT 300
        RESOURCE_LOCK dualscreen_ports)

    # The compressed screen path, end to end. No console needs it yet, so the
    # host is told to use it -- which is how it can be finished and held to
    # before the console that needs it exists.
    add_test(NAME video_stream_headless
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_video_stream_test.sh
            ${XVFB_RUN_EXECUTABLE}
            $<TARGET_FILE:dualscreen>
            ${MOCK_CORE_PATH}
            ${CMAKE_BINARY_DIR}/video-stream
            $<TARGET_FILE:check_screenshot>)
    set_tests_properties(video_stream_headless PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)

    # A companion leaving from the quick menu, which only two processes and the
    # wire between them can show.
    #
    # Needs xdotool as well as Xvfb: the point of the test is a press made
    # after the companion has gone, and there is no way to make one without it.
    # SKIP_RETURN_CODE rather than silently passing, so a machine without it
    # says so instead of reporting a green tick over an untested app.
    add_test(NAME companion_disconnect_headless
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_companion_disconnect_test.sh
            ${XVFB_RUN_EXECUTABLE}
            $<TARGET_FILE:dualscreen>
            $<TARGET_FILE:dualscreen-companion>
            ${MOCK_CORE_PATH}
            ${CMAKE_BINARY_DIR}/companion-disconnect)
    set_tests_properties(companion_disconnect_headless PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)

    # The settings screen: opened from the menu with -, drawn only by the host,
    # and still answering the other device's presses.
    add_test(NAME host_settings_headless
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_host_settings_test.sh
            ${XVFB_RUN_EXECUTABLE}
            $<TARGET_FILE:dualscreen>
            ${MOCK_CORE_PATH}
            ${CMAKE_BINARY_DIR}/host-settings
            $<TARGET_FILE:check_screenshot>)
    set_tests_properties(host_settings_headless PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)

    # The mirror of the run above: the host stops while the companion is in the
    # menu, which is the one direction that was never handled.
    add_test(NAME host_quit_headless
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_host_quit_test.sh
            ${XVFB_RUN_EXECUTABLE}
            $<TARGET_FILE:dualscreen>
            ${MOCK_CORE_PATH}
            ${CMAKE_BINARY_DIR}/host-quit)
    set_tests_properties(host_quit_headless PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)

    # Driven by a shell script rather than a CMake one: the host has to run in
    # the background while the companion connects, and execute_process cannot.
    add_test(NAME two_device_headless
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_two_device_test.sh
            ${XVFB_RUN_EXECUTABLE}
            $<TARGET_FILE:dualscreen>
            ${MOCK_CORE_PATH}
            ${CMAKE_BINARY_DIR}/two-device
            $<TARGET_FILE:check_screenshot>)
    set_tests_properties(two_device_headless PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE
        ENVIRONMENT "COMPANION_SCREEN=bottom")

    # The same run with the other choice: "connect as top" and "connect as
    # bottom" behaving identically was a real bug, and only a two-process run
    # shows which screen actually ends up where.
    add_test(NAME two_device_top_headless
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_two_device_test.sh
            ${XVFB_RUN_EXECUTABLE}
            $<TARGET_FILE:dualscreen>
            ${MOCK_CORE_PATH}
            ${CMAKE_BINARY_DIR}/two-device-top
            $<TARGET_FILE:check_screenshot>)
    set_tests_properties(two_device_top_headless PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE
        ENVIRONMENT "COMPANION_SCREEN=top")

    # And again with a host whose core renders on the GPU. Serving a screen is a
    # different job there: there is no CPU copy of the frame to point at, so the
    # host has to read one back off the card. Reaching into the frame as if
    # there were sent an empty top screen and read off the end of nothing for
    # the bottom -- which crashed the host the moment a companion needed it.
    add_test(NAME two_device_3ds_headless
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_two_device_test.sh
            ${XVFB_RUN_EXECUTABLE}
            $<TARGET_FILE:dualscreen>
            ${MOCK_CORE_PATH}
            ${CMAKE_BINARY_DIR}/two-device-3ds
            $<TARGET_FILE:check_screenshot>)
    set_tests_properties(two_device_3ds_headless PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE
        ENVIRONMENT "COMPANION_SCREEN=bottom;ROM_NAME=game.3ds;HOST_CORE=${MOCK_CORE_3DS_PATH}")

    # The other screen, for the same reason as the DS pair -- and because this
    # is the one that checks the colours survive the trip. A screen read back
    # off the card arrives as RGBA and leaves as XRGB; get that swap wrong and
    # the upper screen turns red, while the green one below stays green either
    # way and says nothing.
    add_test(NAME two_device_3ds_top_headless
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_two_device_test.sh
            ${XVFB_RUN_EXECUTABLE}
            $<TARGET_FILE:dualscreen>
            ${MOCK_CORE_PATH}
            ${CMAKE_BINARY_DIR}/two-device-3ds-top
            $<TARGET_FILE:check_screenshot>)
    set_tests_properties(two_device_3ds_top_headless PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE
        ENVIRONMENT "COMPANION_SCREEN=top;ROM_NAME=game.3ds;HOST_CORE=${MOCK_CORE_3DS_PATH}")
    # The Switch companion joining a real host. It is the same protocol and the
    # same session code as the desktop companion, but its own window, renderer,
    # host picker and touch mapping -- none of which the other four runs touch.
    add_test(NAME switch_client_headless
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_switch_client_test.sh
            ${XVFB_RUN_EXECUTABLE}
            $<TARGET_FILE:dualscreen>
            $<TARGET_FILE:dualscreen-companion>
            ${MOCK_CORE_PATH}
            ${CMAKE_BINARY_DIR}/switch-client
            $<TARGET_FILE:check_screenshot>)
    set_tests_properties(switch_client_headless PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE
        ENVIRONMENT "COMPANION_SCREEN=bottom")

    # And as the other screen, for the same reason the desktop pair is run both
    # ways round: which screen a device ends up holding is the one thing a
    # companion cannot check for itself.
    add_test(NAME switch_client_top_headless
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_switch_client_test.sh
            ${XVFB_RUN_EXECUTABLE}
            $<TARGET_FILE:dualscreen>
            $<TARGET_FILE:dualscreen-companion>
            ${MOCK_CORE_PATH}
            ${CMAKE_BINARY_DIR}/switch-client-top
            $<TARGET_FILE:check_screenshot>)
    set_tests_properties(switch_client_top_headless PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE
        ENVIRONMENT "COMPANION_SCREEN=top")

    # And once more in an OpenGL 3.3 core context, which is a different
    # renderer path and not a different feature.
    #
    # shader_preamble() has three: GLES, core, and compatibility. Every other
    # test here takes whichever one the machine's driver hands out, which on
    # Linux is GLES -- so the core path was written for a Citra core, used by
    # nobody else, and covered by nothing. DUALSCREEN_FORCE_GL33 existed to
    # exercise it and no test set it, which the comment beside it claimed
    # otherwise about.
    #
    # It is the macOS companion that makes this worth having rather than
    # tidy. Apple's platforms have no GLES at all and a legacy profile frozen
    # at 2.1, so a core context is not a fallback there, it is the only path
    # -- and it is the one path that cannot be run on the machine this is
    # built on. This runs it here instead: a whole session against a real
    # host, drawn through the same shaders a Mac will use, checked by the same
    # screenshot.
    add_test(NAME switch_client_gl33_headless
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_switch_client_test.sh
            ${XVFB_RUN_EXECUTABLE}
            $<TARGET_FILE:dualscreen>
            $<TARGET_FILE:dualscreen-companion>
            ${MOCK_CORE_PATH}
            ${CMAKE_BINARY_DIR}/switch-client-gl33
            $<TARGET_FILE:check_screenshot>)
    set_tests_properties(switch_client_gl33_headless PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE
        ENVIRONMENT "COMPANION_SCREEN=bottom;DUALSCREEN_FORCE_GL33=1")

    # And on a 3DS, whose screens are a different size and shape from a DS's
    # and whose touch screen is narrower than its upper one. The client works
    # all of that out from what the host tells it about the console, so this
    # is where that arithmetic is actually run on the client rather than only
    # on the host.
    add_test(NAME switch_client_3ds_headless
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_switch_client_test.sh
            ${XVFB_RUN_EXECUTABLE}
            $<TARGET_FILE:dualscreen>
            $<TARGET_FILE:dualscreen-companion>
            ${MOCK_CORE_3DS_PATH}
            ${CMAKE_BINARY_DIR}/switch-client-3ds
            $<TARGET_FILE:check_screenshot>)
    set_tests_properties(switch_client_3ds_headless PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE
        ENVIRONMENT "COMPANION_SCREEN=bottom;COMPANION_ROM_EXTENSION=3ds")
    # A console this app does not play itself. Two processes and the shared
    # memory between them, with fake_wii_u standing in for Cemu -- the only
    # piece of the Wii U path that does not exist yet.
    add_test(NAME wii_u_headless
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_wii_u_test.sh
            ${XVFB_RUN_EXECUTABLE}
            $<TARGET_FILE:dualscreen>
            $<TARGET_FILE:fake_wii_u>
            ${CMAKE_BINARY_DIR}/wii-u
            $<TARGET_FILE:check_screenshot>)
    set_tests_properties(wii_u_headless PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
    # And the same console across two devices. Three processes: the emulator
    # stand-in, the host it hands its screens to, and the companion the host
    # hands one of them on to -- which is the arrangement the whole app is for.
    add_test(NAME wii_u_companion_headless
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_wii_u_companion_test.sh
            ${XVFB_RUN_EXECUTABLE}
            $<TARGET_FILE:dualscreen>
            $<TARGET_FILE:fake_wii_u>
            ${CMAKE_BINARY_DIR}/wii-u-companion
            $<TARGET_FILE:check_screenshot>)
    set_tests_properties(wii_u_companion_headless PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)

    # And the same path again with the Switch/Android client as the second
    # device rather than this app. Same sources, but its own window, host
    # picker, panel size and decoder configuration -- none of which the run
    # above exercises, and all of which a console this app does not play
    # itself was never written with in mind.
    add_test(NAME wii_u_client_headless
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_wii_u_client_test.sh
            ${XVFB_RUN_EXECUTABLE}
            $<TARGET_FILE:dualscreen>
            $<TARGET_FILE:dualscreen-companion>
            $<TARGET_FILE:fake_wii_u>
            ${CMAKE_BINARY_DIR}/wii-u-client
            $<TARGET_FILE:check_screenshot>)
    set_tests_properties(wii_u_client_headless PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
else()
    message(STATUS "xvfb-run not found; the headless smoke test will be skipped")
endif()
