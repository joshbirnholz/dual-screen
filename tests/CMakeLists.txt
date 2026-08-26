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
add_executable(check_screenshot check_screenshot.cpp)
target_compile_options(check_screenshot PRIVATE -Wall -Wextra)

dualscreen_add_test(test_layout)
dualscreen_add_test(test_text)
dualscreen_add_test(test_ui_sounds)
dualscreen_add_test(test_image)
dualscreen_add_test(test_protocol)
dualscreen_add_test(test_net)
dualscreen_add_test(test_session)
dualscreen_add_test(test_config)
dualscreen_add_test(test_browser)
dualscreen_add_test(test_quick_settings)
dualscreen_add_test(test_controller_mapping)
dualscreen_add_test(test_retro_keymap)
dualscreen_add_test(test_controller_input)
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
    set_tests_properties(smoke_headless PROPERTIES TIMEOUT 300)

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
else()
    message(STATUS "xvfb-run not found; the headless smoke test will be skipped")
endif()
