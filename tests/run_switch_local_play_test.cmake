# The Switch's own program, built the way a Switch build with melonDS is --
# the companion, with a core linked in -- playing a game itself. The mock core
# stands in for melonDS and this machine for the console; libnx is the only
# part missing (see companion/source/local_play.hpp).
#
# Two ways in, as a Switch has them: a game's forwarder, which starts the
# program with the ROM as its argument, and the menu's Load a game, which
# starts it again with --browse.
#
# Invoked by CTest with -DXVFB_RUN, -DPROGRAM, -DWORK_DIR, -DCHECKER.

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/config")
file(MAKE_DIRECTORY "${WORK_DIR}/data")
file(WRITE "${WORK_DIR}/fake.nds" "dualscreen switch local play test rom")

set(SHOT "${WORK_DIR}/forwarder.ppm")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env
            "XDG_CONFIG_HOME=${WORK_DIR}/config"
            "XDG_DATA_HOME=${WORK_DIR}/data"
            "SDL_AUDIODRIVER=dummy"
            "SDL_VIDEODRIVER=x11"
            "${XVFB_RUN}" -a -s "-screen 0 1600x1000x24"
            "${PROGRAM}" --windowed --exit-after-seconds 3 --screenshot "${SHOT}"
                "${WORK_DIR}/fake.nds"
    RESULT_VARIABLE RUN_RESULT
    OUTPUT_VARIABLE RUN_OUTPUT
    ERROR_VARIABLE RUN_ERROR)
if(NOT RUN_RESULT EQUAL 0)
    message(STATUS "${RUN_OUTPUT}")
    message(STATUS "${RUN_ERROR}")
    message(FATAL_ERROR "a game handed over as an argument did not play (exit ${RUN_RESULT})")
endif()
if(NOT "${RUN_OUTPUT}${RUN_ERROR}" MATCHES "playing here: ")
    message(FATAL_ERROR "the ROM argument never reached local play:\n${RUN_OUTPUT}${RUN_ERROR}")
endif()
execute_process(
    COMMAND "${CHECKER}" "${SHOT}" upper=blue lower=green
    RESULT_VARIABLE CHECK_RESULT
    OUTPUT_VARIABLE CHECK_OUTPUT
    ERROR_VARIABLE CHECK_ERROR)
if(NOT CHECK_RESULT EQUAL 0)
    message(STATUS "${CHECK_OUTPUT}${CHECK_ERROR}")
    message(FATAL_ERROR "the game's two screens did not reach the window")
endif()

# And the game browser, which is where Load a game on the menu lands.
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env
            "XDG_CONFIG_HOME=${WORK_DIR}/config"
            "XDG_DATA_HOME=${WORK_DIR}/data"
            "SDL_AUDIODRIVER=dummy"
            "SDL_VIDEODRIVER=x11"
            "${XVFB_RUN}" -a -s "-screen 0 1600x1000x24"
            "${PROGRAM}" --windowed --exit-after-seconds 2 --browse
    RESULT_VARIABLE RUN_RESULT
    OUTPUT_VARIABLE RUN_OUTPUT
    ERROR_VARIABLE RUN_ERROR)
if(NOT RUN_RESULT EQUAL 0)
    message(STATUS "${RUN_OUTPUT}${RUN_ERROR}")
    message(FATAL_ERROR "--browse did not open the game browser (exit ${RUN_RESULT})")
endif()
if(NOT "${RUN_OUTPUT}${RUN_ERROR}" MATCHES "playing here: opening the game browser")
    message(FATAL_ERROR "--browse never reached the game browser:\n${RUN_OUTPUT}${RUN_ERROR}")
endif()

message(STATUS "the Switch program played a game it was handed, and opened its browser")
