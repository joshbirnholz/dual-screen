# Resume from Save State, through the real binary and the mock core.
#
#   1. A first start writes the game's config; the option is then turned on
#      in it, the way the quick menu would.
#   2. A start with it on and nothing yet to resume starts the game as usual
#      -- and leaves a resume point behind when the app closes.
#   3. The next start is from that resume point, paused, under the popup: its
#      box is on screen with the game still there behind it.
#   4. With it off again, the game starts as usual whatever is on the disk.
#
# The mock core's state is its frame counter, so resuming is resuming for
# real, not just reading a file. And like melonDS DS, it will not take a state
# before it has run a frame, so resuming too early fails here.
#
# Invoked by CTest with -DXVFB_RUN, -DDUALSCREEN, -DMOCK_CORE, -DWORK_DIR,
# -DCHECKER.

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/config")
file(MAKE_DIRECTORY "${WORK_DIR}/data")
file(WRITE "${WORK_DIR}/game.nds" "dualscreen resume test rom")
set(RESUME_FILE "${WORK_DIR}/data/dualscreen/saves/game.resume")

function(run_app OUT_VAR)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env
                "XDG_CONFIG_HOME=${WORK_DIR}/config"
                "XDG_DATA_HOME=${WORK_DIR}/data"
                "SDL_AUDIODRIVER=dummy"
                "SDL_VIDEODRIVER=x11"
                "${XVFB_RUN}" -a -s "-screen 0 1600x1000x24"
                "${DUALSCREEN}"
                    --game "${WORK_DIR}/game.nds"
                    --core "${MOCK_CORE}"
                    --layout vertical
                    --windowed
                    ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "dualscreen exited with ${result}:\n${output}\n${errors}")
    endif()
    set(${OUT_VAR} "${output}${errors}" PARENT_SCOPE)
endfunction()

function(set_resume VALUE)
    file(GLOB configs "${WORK_DIR}/config/dualscreen/games/*.json")
    list(LENGTH configs count)
    if(NOT count EQUAL 1)
        message(FATAL_ERROR "expected one game config, found ${count}")
    endif()
    file(READ "${configs}" text)
    string(JSON text SET "${text}" resume_from_save_state ${VALUE})
    file(WRITE "${configs}" "${text}")
endfunction()

# 1.
run_app(output --exit-after-frames 10)
if(EXISTS "${RESUME_FILE}")
    message(FATAL_ERROR "a resume point was written for a game without the option on")
endif()
set_resume(true)

# 2.
run_app(output --exit-after-frames 60)
if(NOT output MATCHES "nothing to resume yet")
    message(FATAL_ERROR "a first start with the option on did not say so:\n${output}")
endif()
if(NOT EXISTS "${RESUME_FILE}")
    message(FATAL_ERROR "closing the app left no resume point at ${RESUME_FILE}:\n${output}")
endif()

# 3.
set(SHOT "${WORK_DIR}/resumed.ppm")
run_app(output --exit-after-frames 20 --screenshot "${SHOT}")
if(NOT output MATCHES "resumed from [^\n]*game\\.resume")
    message(FATAL_ERROR "the next start did not resume:\n${output}")
endif()
if(output MATCHES "the resumed game is running")
    message(FATAL_ERROR "the resumed game ran without anybody pressing OK:\n${output}")
endif()
execute_process(
    COMMAND "${CHECKER}" "${SHOT}" "at=0.24,0.5,dialog" "at=0.5,0.36,blue" "lower=green"
    RESULT_VARIABLE check_result
    OUTPUT_VARIABLE check_output
    ERROR_VARIABLE check_errors)
message(STATUS "resumed:\n${check_output}")
if(NOT check_result EQUAL 0)
    message(FATAL_ERROR "the popup is not over the resumed game:\n${check_errors}")
endif()

# 4.
set_resume(false)
run_app(output --exit-after-frames 10)
if(output MATCHES "resumed from" OR output MATCHES "nothing to resume")
    message(FATAL_ERROR "a start with the option off tried to resume:\n${output}")
endif()

message(STATUS "a game with Resume from Save State on starts where it was left, under the popup")
