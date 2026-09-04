# Type-checks the parts of this app that only exist on a Switch, on a machine
# with no devkitPro.
#
# The Switch build is the one nothing here can run, and it has broken twice:
# once on sources missing from its own Makefile, once on an argument spelled as
# the integer underneath a libnx enum. Both were compile errors that any
# compiler would have caught, and neither could be caught because there was no
# compiler to run.
#
# So: the real signatures, copied into tests/switch_stub/switch.h, and the
# ordinary host compiler pointed at the __SWITCH__ path. This proves nothing
# about whether the code *works* on a Switch -- only that it agrees with the
# shape of the API it calls, which is exactly what was wrong both times.
#
# Invoked by CTest with -DCOMPILER, -DSOURCES, -DSTUB_DIR, -DSRC_DIR,
# -DEXTRA_INCLUDES.

separate_arguments(SOURCE_LIST UNIX_COMMAND "${SOURCES}")
set(INCLUDE_FLAGS "-I${STUB_DIR}" "-I${SRC_DIR}")
foreach(dir IN LISTS EXTRA_INCLUDES)
    if(dir)
        list(APPEND INCLUDE_FLAGS "-I${dir}")
    endif()
endforeach()

foreach(source IN LISTS SOURCE_LIST)
    execute_process(
        COMMAND "${COMPILER}" -fsyntax-only -std=c++20 -D__SWITCH__
                ${INCLUDE_FLAGS} "${source}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "the Switch-only path of ${source} does not compile:\n${errors}${output}")
    endif()
endforeach()

message(STATUS "the Switch-only paths agree with libnx's signatures")
