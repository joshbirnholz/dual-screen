# The mock libretro core: a real .so with the libretro C ABI, so the frontend's
# dlopen path is exercised exactly as it would be with melonDS DS.
add_library(mock_core MODULE mock_core/mock_core.cpp)
set_target_properties(mock_core PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mock_ds_libretro"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
target_include_directories(mock_core PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
target_compile_options(mock_core PRIVATE -Wall -Wextra)

set(MOCK_CORE_PATH "${CMAKE_BINARY_DIR}/tests/mock_ds_libretro.so")

function(dualscreen_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE dualscreen_core)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    add_test(NAME ${name} COMMAND ${name})
    # These need no display. Pinning the driver keeps them independent of
    # whatever SDL_VIDEODRIVER happens to be set in the caller's environment.
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "DUALSCREEN_MOCK_CORE=${MOCK_CORE_PATH};SDL_VIDEODRIVER=dummy")
endfunction()

# A helper binary, not a test itself: the smoke test uses it to inspect output.
add_executable(check_screenshot check_screenshot.cpp)
target_compile_options(check_screenshot PRIVATE -Wall -Wextra)

dualscreen_add_test(test_layout)
dualscreen_add_test(test_image)
dualscreen_add_test(test_protocol)
dualscreen_add_test(test_net)
dualscreen_add_test(test_session)
dualscreen_add_test(test_config)
dualscreen_add_test(test_browser)
dualscreen_add_test(test_controller_mapping)
dualscreen_add_test(test_controller_input)
dualscreen_add_test(test_core_host)
add_dependencies(test_core_host mock_core)

# End-to-end smoke test: runs the real binary against the mock core under a
# headless X server and checks the composited output pixel by pixel.
find_program(XVFB_RUN_EXECUTABLE xvfb-run)
if(XVFB_RUN_EXECUTABLE)
    add_test(NAME smoke_headless
        COMMAND ${CMAKE_COMMAND}
            -DXVFB_RUN=${XVFB_RUN_EXECUTABLE}
            -DDUALSCREEN=$<TARGET_FILE:dualscreen>
            -DMOCK_CORE=${MOCK_CORE_PATH}
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
else()
    message(STATUS "xvfb-run not found; the headless smoke test will be skipped")
endif()
