# Which build this is, in the first lines of the log.
#
# A binary gives no clue what is inside it, and "I rebuilt it and it behaves the
# same" has two completely different causes -- a build that did not pick up the
# change, and a change that did not fix anything. Without this there is no way
# to tell them apart from a log, which is the only thing that crosses the
# distance between the machine that built it and the one it runs on. With two
# devices in a session it matters twice over, since only one of them may have
# been updated, and the one that was not is generally the one being diagnosed.
#
# Taken at configure time, so re-running cmake is what refreshes it -- which is
# the first of the two build commands anyway. A build that skipped the configure
# step reports the hash it was last configured at, and says so by saying nothing
# about the changes since.
#
# Call dualscreen_stamp_build(<source file>) with the one source that reads
# DS_BUILD_STAMP. On that file alone, never on a target: the stamp carries a
# timestamp and so changes at every configure, and a definition on the target
# would recompile everything each time for a string one file reads.

if(NOT DEFINED DUALSCREEN_ROOT)
    message(FATAL_ERROR "build_stamp.cmake needs DUALSCREEN_ROOT set")
endif()

execute_process(
    COMMAND git -C "${DUALSCREEN_ROOT}" rev-parse --short HEAD
    OUTPUT_VARIABLE DUALSCREEN_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE DUALSCREEN_GIT_RESULT)
if(NOT DUALSCREEN_GIT_RESULT EQUAL 0 OR DUALSCREEN_GIT_HASH STREQUAL "")
    set(DUALSCREEN_GIT_HASH "no git here")
else()
    # Uncommitted changes matter as much as the commit: a hash on its own would
    # read as an exact claim about source that has been edited since.
    execute_process(
        COMMAND git -C "${DUALSCREEN_ROOT}" status --porcelain --untracked-files=no
        OUTPUT_VARIABLE DUALSCREEN_GIT_DIRTY
        ERROR_QUIET)
    if(NOT DUALSCREEN_GIT_DIRTY STREQUAL "")
        set(DUALSCREEN_GIT_HASH "${DUALSCREEN_GIT_HASH} with local changes")
    endif()
endif()
string(TIMESTAMP DUALSCREEN_CONFIGURED_AT "%Y-%m-%d %H:%M" UTC)
set(DUALSCREEN_BUILD_STAMP "${DUALSCREEN_GIT_HASH}, configured ${DUALSCREEN_CONFIGURED_AT}")
message(STATUS "build stamp: ${DUALSCREEN_BUILD_STAMP}")

# Which nightly this is, for the updater to compare with the newest one -- see
# src/update/. Set by CI (.github/workflows/builds.yml), through the
# environment or -DDUALSCREEN_BUILD_NUMBER; 0, a build made anywhere else,
# is one the updater leaves alone unless asked.
if(NOT DEFINED DUALSCREEN_BUILD_NUMBER)
    if(DEFINED ENV{DUALSCREEN_BUILD_NUMBER} AND NOT "$ENV{DUALSCREEN_BUILD_NUMBER}" STREQUAL "")
        set(DUALSCREEN_BUILD_NUMBER "$ENV{DUALSCREEN_BUILD_NUMBER}")
    else()
        set(DUALSCREEN_BUILD_NUMBER 0)
    endif()
endif()
if(NOT DUALSCREEN_BUILD_NUMBER MATCHES "^[0-9]+$")
    message(FATAL_ERROR "DUALSCREEN_BUILD_NUMBER is \"${DUALSCREEN_BUILD_NUMBER}\", not a number")
endif()
if(NOT DUALSCREEN_BUILD_NUMBER EQUAL 0)
    message(STATUS "nightly build ${DUALSCREEN_BUILD_NUMBER}")
endif()

function(dualscreen_stamp_build source)
    set_source_files_properties("${source}" PROPERTIES
        COMPILE_DEFINITIONS "DS_BUILD_STAMP=\"${DUALSCREEN_BUILD_STAMP}\";DS_BUILD_NUMBER=${DUALSCREEN_BUILD_NUMBER}")
endfunction()
