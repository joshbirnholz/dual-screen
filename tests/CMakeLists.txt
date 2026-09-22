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

add_executable(check_screenshot check_screenshot.cpp)
target_compile_options(check_screenshot PRIVATE -Wall -Wextra)

dualscreen_add_test(test_layout)
dualscreen_add_test(test_video_codec)
dualscreen_add_test(test_h264_bitstream)
dualscreen_add_test(test_external_link)
dualscreen_add_test(test_text)
dualscreen_add_test(test_launcher)
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
# And the configuration iOS and tvOS actually ship: no FFmpeg, VideoToolbox
# doing the decoding. Linux always finds FFmpeg, so that half of the fork is
# dead code to every build here and would rot unwatched.
add_test(NAME no_codec_typecheck
    COMMAND ${CMAKE_COMMAND}
        -DCOMPILER=${CMAKE_CXX_COMPILER}
        "-DSOURCES=${CMAKE_SOURCE_DIR}/src/video/video_decoder.cpp;${CMAKE_SOURCE_DIR}/src/video/videotoolbox_decoder.cpp;${CMAKE_SOURCE_DIR}/src/video/h264_bitstream.cpp;${CMAKE_SOURCE_DIR}/src/net/companion_session.cpp;${CMAKE_SOURCE_DIR}/companion/source/main.cpp"
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
else()
    message(STATUS "no clang here, so the UIKit scene typecheck is not registered")
endif()

dualscreen_add_test(test_net)
dualscreen_add_test(test_session)
# Discovery, session and media all live on fixed ports (kDiscoveryPort and the
# two after it), so any two tests that stand a real host up cannot run at the
# same time -- the second one's bind fails and it reads as a broken test rather
# than as two tests wanting one port. The headless tests below are already
# RUN_SERIAL, which excludes everything; these three only need to exclude each
# other, and stay free to run alongside the tests that touch no socket.
set_tests_properties(test_net test_session PROPERTIES RESOURCE_LOCK dualscreen_ports)
dualscreen_add_test(test_config)
dualscreen_add_test(test_game_title)
dualscreen_add_test(test_browser)
dualscreen_add_test(test_quick_settings)
dualscreen_add_test(test_touch_hints)
dualscreen_add_test(test_touch_gesture)
dualscreen_add_test(test_controller_mapping)
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
    set_tests_properties(video_stream_headless PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE
        ENVIRONMENT "ASKED_BY=flag")

    # And the same session asked for the other way round. The setting the player
    # can reach is the one that matters; the flag is for a run that has to
    # differ from it without editing a file.
    add_test(NAME video_stream_setting_headless
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_video_stream_test.sh
            ${XVFB_RUN_EXECUTABLE}
            $<TARGET_FILE:dualscreen>
            ${MOCK_CORE_PATH}
            ${CMAKE_BINARY_DIR}/video-stream-setting
            $<TARGET_FILE:check_screenshot>)
    set_tests_properties(video_stream_setting_headless PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE
        ENVIRONMENT "ASKED_BY=setting")

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
