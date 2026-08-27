# The frame link is meant to be dropped into a fork of somebody else's emulator
# with nothing else of this app -- see docs/cemu-integration.md. An include of
# anything else in here would compile perfectly well in this tree and only be
# discovered by whoever tried to vendor it, so this copies the files it promises
# and compiles them on their own.
#
# cmake -DCOMPILER=... -DSOURCE_DIR=.../src -DWORK_DIR=... -P run_standalone_link_test.cmake

set(FILES
    core/external_frame_link.hpp
    core/external_frame_link.cpp
    layout/screen_id.hpp)

file(REMOVE_RECURSE "${WORK_DIR}")
foreach(file ${FILES})
    get_filename_component(directory "${file}" DIRECTORY)
    file(MAKE_DIRECTORY "${WORK_DIR}/${directory}")
    if(NOT EXISTS "${SOURCE_DIR}/${file}")
        message(FATAL_ERROR "docs/cemu-integration.md promises ${file}, which does not exist")
    endif()
    configure_file("${SOURCE_DIR}/${file}" "${WORK_DIR}/${file}" COPYONLY)
endforeach()

execute_process(
    COMMAND "${COMPILER}" -std=c++17 -Wall -Wextra -Werror
            -I "${WORK_DIR}"
            -c "${WORK_DIR}/core/external_frame_link.cpp"
            -o "${WORK_DIR}/external_frame_link.o"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)

if(NOT result EQUAL 0)
    message("${output}")
    message(FATAL_ERROR
        "the frame link no longer compiles on its own, so it can no longer be dropped into a "
        "fork of Cemu. Whatever it now needs from the rest of this app has to move, or move "
        "into these files.")
endif()

message(STATUS "ok - the frame link compiles with nothing else from this app")
