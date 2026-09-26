# The app with its core linked in rather than loaded from a file -- the way a
# Switch has to carry melonDS, since it cannot load code at run time -- driven
# through a whole game with the mock core standing in for melonDS.
#
# No --core is given, and nothing on this machine is where a core is looked
# for: the only core this program can have is the one inside it. A game from
# another console is refused, since that one core is a DS one.
#
# Invoked by CTest with -DXVFB_RUN, -DDUALSCREEN, -DWORK_DIR, -DCHECKER.

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/config")
file(MAKE_DIRECTORY "${WORK_DIR}/data")
file(WRITE "${WORK_DIR}/fake.nds" "dualscreen built-in core test rom")
file(WRITE "${WORK_DIR}/fake.3ds" "dualscreen built-in core test rom, 3ds")

set(SHOT "${WORK_DIR}/vertical.ppm")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env
            "XDG_CONFIG_HOME=${WORK_DIR}/config"
            "XDG_DATA_HOME=${WORK_DIR}/data"
            "SDL_AUDIODRIVER=dummy"
            "SDL_VIDEODRIVER=x11"
            "${XVFB_RUN}" -a -s "-screen 0 1600x1000x24"
            "${DUALSCREEN}"
                --game "${WORK_DIR}/fake.nds"
                --layout vertical
                --windowed
                --exit-after-frames 12
                --screenshot "${SHOT}"
    RESULT_VARIABLE RUN_RESULT
    OUTPUT_VARIABLE RUN_OUTPUT
    ERROR_VARIABLE RUN_ERROR)
if(NOT RUN_RESULT EQUAL 0)
    message(STATUS "${RUN_OUTPUT}")
    message(STATUS "${RUN_ERROR}")
    message(FATAL_ERROR "the app with a built-in core exited with ${RUN_RESULT} on a DS game")
endif()

execute_process(
    COMMAND "${CHECKER}" "${SHOT}" upper=blue lower=green
    RESULT_VARIABLE CHECK_RESULT
    OUTPUT_VARIABLE CHECK_OUTPUT
    ERROR_VARIABLE CHECK_ERROR)
if(NOT CHECK_RESULT EQUAL 0)
    message(STATUS "${CHECK_OUTPUT}")
    message(STATUS "${CHECK_ERROR}")
    message(FATAL_ERROR "the built-in core's screens did not reach the window")
endif()

# The log says where the core came from.
file(READ "${WORK_DIR}/data/dualscreen/dualscreen.log" LOG_TEXT)
if(NOT LOG_TEXT MATCHES "built in")
    message(FATAL_ERROR "the log never says the core is built in:\n${LOG_TEXT}")
endif()

# And a game for another console is turned away with a reason, not handed to
# a DS core.
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env
            "XDG_CONFIG_HOME=${WORK_DIR}/config"
            "XDG_DATA_HOME=${WORK_DIR}/data"
            "SDL_AUDIODRIVER=dummy"
            "SDL_VIDEODRIVER=x11"
            "${XVFB_RUN}" -a -s "-screen 0 1600x1000x24"
            "${DUALSCREEN}"
                --game "${WORK_DIR}/fake.3ds"
                --windowed
                --exit-after-frames 12
    RESULT_VARIABLE RUN_RESULT
    OUTPUT_VARIABLE RUN_OUTPUT
    ERROR_VARIABLE RUN_ERROR)
file(READ "${WORK_DIR}/data/dualscreen/dualscreen.log" LOG_TEXT)
if(NOT LOG_TEXT MATCHES "plays Nintendo DS games only")
    message(FATAL_ERROR "a 3DS game was not refused with a reason:\n${LOG_TEXT}")
endif()

message(STATUS "a DS game played through the built-in core, and a 3DS one was refused")
