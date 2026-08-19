# Drives the real dualscreen binary against the mock libretro core inside a
# headless X server, capturing one screenshot per layout and checking that each
# screen actually landed where the layout says it should.
#
# Invoked by CTest with -DXVFB_RUN, -DDUALSCREEN, -DMOCK_CORE, -DWORK_DIR,
# -DCHECKER.

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/config")
file(MAKE_DIRECTORY "${WORK_DIR}/data")

# The mock core ignores ROM contents, but the app hashes the file to key the
# per-game config, so it has to be a real file with stable bytes.
file(WRITE "${WORK_DIR}/fake.nds" "dualscreen smoke test rom")

set(FRAME_COUNT 12)

# The mock core dirties its save RAM as it runs, so every launch leaves a save
# behind. The layout and touch sections must not depend on that, so they start
# from a clean slate each time; only the save section below cares about it.
set(SAVE_FILE "${WORK_DIR}/data/dualscreen/saves/fake.srm")

# layout id -> expectations passed to check_screenshot. Expectations are joined
# with '+': a semicolon would split the CMake list itself, and a comma appears
# inside the crosshair expectations.
set(CASES
    "vertical|upper=blue+lower=green+topleft=black"
    "vertical_swapped|upper=green+lower=blue"
    "horizontal|left=blue+right=green+topleft=black"
    "horizontal_swapped|left=green+right=blue"
    "top_only|center=blue"
    "bottom_only|center=green"
)

set(ANY_FAILED FALSE)

foreach(CASE IN LISTS CASES)
    string(REPLACE "|" ";" PARTS "${CASE}")
    list(GET PARTS 0 LAYOUT_ID)
    list(GET PARTS 1 EXPECTATIONS_RAW)
    string(REPLACE "+" ";" EXPECTATIONS "${EXPECTATIONS_RAW}")

    set(SHOT "${WORK_DIR}/${LAYOUT_ID}.ppm")
    file(REMOVE "${SAVE_FILE}")

    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env
                "XDG_CONFIG_HOME=${WORK_DIR}/config"
                "XDG_DATA_HOME=${WORK_DIR}/data"
                "SDL_AUDIODRIVER=dummy"
                "SDL_VIDEODRIVER=x11"
                "${XVFB_RUN}" -a -s "-screen 0 1600x1000x24"
                "${DUALSCREEN}"
                    --game "${WORK_DIR}/fake.nds"
                    --core "${MOCK_CORE}"
                    --layout "${LAYOUT_ID}"
                    --windowed
                    --exit-after-frames ${FRAME_COUNT}
                    --screenshot "${SHOT}"
        RESULT_VARIABLE RUN_RESULT
        OUTPUT_VARIABLE RUN_OUTPUT
        ERROR_VARIABLE RUN_ERROR)

    if(NOT RUN_RESULT EQUAL 0)
        message(STATUS "${RUN_OUTPUT}")
        message(STATUS "${RUN_ERROR}")
        message(SEND_ERROR "dualscreen exited with ${RUN_RESULT} for layout ${LAYOUT_ID}")
        set(ANY_FAILED TRUE)
        continue()
    endif()

    if(NOT EXISTS "${SHOT}")
        message(STATUS "${RUN_OUTPUT}")
        message(STATUS "${RUN_ERROR}")
        message(SEND_ERROR "no screenshot was written for layout ${LAYOUT_ID}")
        set(ANY_FAILED TRUE)
        continue()
    endif()

    execute_process(
        COMMAND "${CHECKER}" "${SHOT}" ${EXPECTATIONS}
        RESULT_VARIABLE CHECK_RESULT
        OUTPUT_VARIABLE CHECK_OUTPUT
        ERROR_VARIABLE CHECK_ERROR)
    message(STATUS "layout ${LAYOUT_ID}:\n${CHECK_OUTPUT}")
    if(NOT CHECK_RESULT EQUAL 0)
        message(STATUS "${CHECK_ERROR}")
        message(SEND_ERROR "layout ${LAYOUT_ID} did not render its screens where expected")
        set(ANY_FAILED TRUE)
    endif()
endforeach()

# --- cartridge saves --------------------------------------------------------
#
# In libretro the frontend owns save RAM entirely: the core never touches the
# filesystem for it. This runs the real binary twice against the same save
# directory. The mock core paints an orange band across the top screen when it
# finds its magic value in save RAM on the first frame, so the band appearing
# on the second run -- and only the second -- proves the save was written on
# exit and read back before emulation started.
file(REMOVE "${SAVE_FILE}")

foreach(RUN_INDEX RANGE 1 2)
    set(SHOT "${WORK_DIR}/save_run${RUN_INDEX}.ppm")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env
                "XDG_CONFIG_HOME=${WORK_DIR}/config"
                "XDG_DATA_HOME=${WORK_DIR}/data"
                "SDL_AUDIODRIVER=dummy"
                "SDL_VIDEODRIVER=x11"
                "${XVFB_RUN}" -a -s "-screen 0 1600x1000x24"
                "${DUALSCREEN}"
                    --game "${WORK_DIR}/fake.nds"
                    --core "${MOCK_CORE}"
                    --layout top_only
                    --windowed
                    --exit-after-frames ${FRAME_COUNT}
                    --screenshot "${SHOT}"
        RESULT_VARIABLE RUN_RESULT
        OUTPUT_VARIABLE RUN_OUTPUT
        ERROR_VARIABLE RUN_ERROR)

    if(NOT RUN_RESULT EQUAL 0 OR NOT EXISTS "${SHOT}")
        message(STATUS "${RUN_OUTPUT}")
        message(STATUS "${RUN_ERROR}")
        message(SEND_ERROR "save run ${RUN_INDEX} failed")
        set(ANY_FAILED TRUE)
        continue()
    endif()

    if(RUN_INDEX EQUAL 1)
        # Nothing to restore yet, so the plain top screen shows through.
        set(EXPECTATION "center=blue")
        if(NOT EXISTS "${SAVE_FILE}")
            message(SEND_ERROR "no save file was written to ${SAVE_FILE}")
            set(ANY_FAILED TRUE)
        endif()
    else()
        set(EXPECTATION "center=orange")
    endif()

    execute_process(
        COMMAND "${CHECKER}" "${SHOT}" ${EXPECTATION}
        RESULT_VARIABLE CHECK_RESULT
        OUTPUT_VARIABLE CHECK_OUTPUT
        ERROR_VARIABLE CHECK_ERROR)
    message(STATUS "save run ${RUN_INDEX}:\n${CHECK_OUTPUT}")
    if(NOT CHECK_RESULT EQUAL 0)
        message(STATUS "${CHECK_ERROR}")
        if(RUN_INDEX EQUAL 1)
            message(SEND_ERROR "the first run should have started fresh")
        else()
            message(SEND_ERROR "the save was not restored on the second run")
        endif()
        set(ANY_FAILED TRUE)
    endif()
endforeach()

# --- touch mapping ----------------------------------------------------------
#
# The app is told to hold a touch at a fixed fraction of the window. The mock
# core draws its crosshair at whatever DS coordinate the frontend reported, and
# the compositor draws that screen back into the same place -- so a correct
# mapping puts the crosshair exactly under the touch. A layout that shows no
# bottom screen, or a touch that lands on the top screen, must pass nothing
# through at all.
set(TOUCH_CASES
    "vertical|0.45,0.80|crosshair=0.45,0.80"
    "vertical|0.50,0.25|nocrosshair=1"
    "vertical_swapped|0.50,0.25|crosshair=0.50,0.25"
    "horizontal|0.75,0.50|crosshair=0.75,0.50"
    "horizontal_swapped|0.25,0.50|crosshair=0.25,0.50"
    "bottom_only|0.60,0.40|crosshair=0.60,0.40"
    "top_only|0.50,0.50|nocrosshair=1"
)

foreach(CASE IN LISTS TOUCH_CASES)
    string(REPLACE "|" ";" PARTS "${CASE}")
    list(GET PARTS 0 LAYOUT_ID)
    list(GET PARTS 1 TOUCH_POINT)
    list(GET PARTS 2 EXPECTATIONS_RAW)
    string(REPLACE "+" ";" EXPECTATIONS "${EXPECTATIONS_RAW}")
    string(REPLACE "," "_" TOUCH_TAG "${TOUCH_POINT}")

    set(SHOT "${WORK_DIR}/touch_${LAYOUT_ID}_${TOUCH_TAG}.ppm")
    file(REMOVE "${SAVE_FILE}")

    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env
                "XDG_CONFIG_HOME=${WORK_DIR}/config"
                "XDG_DATA_HOME=${WORK_DIR}/data"
                "SDL_AUDIODRIVER=dummy"
                "SDL_VIDEODRIVER=x11"
                "${XVFB_RUN}" -a -s "-screen 0 1600x1000x24"
                "${DUALSCREEN}"
                    --game "${WORK_DIR}/fake.nds"
                    --core "${MOCK_CORE}"
                    --layout "${LAYOUT_ID}"
                    --windowed
                    --exit-after-frames ${FRAME_COUNT}
                    --simulate-touch "${TOUCH_POINT}"
                    --screenshot "${SHOT}"
        RESULT_VARIABLE RUN_RESULT
        OUTPUT_VARIABLE RUN_OUTPUT
        ERROR_VARIABLE RUN_ERROR)

    if(NOT RUN_RESULT EQUAL 0 OR NOT EXISTS "${SHOT}")
        message(STATUS "${RUN_OUTPUT}")
        message(STATUS "${RUN_ERROR}")
        message(SEND_ERROR "touch run failed for layout ${LAYOUT_ID}")
        set(ANY_FAILED TRUE)
        continue()
    endif()

    execute_process(
        COMMAND "${CHECKER}" "${SHOT}" ${EXPECTATIONS}
        RESULT_VARIABLE CHECK_RESULT
        OUTPUT_VARIABLE CHECK_OUTPUT
        ERROR_VARIABLE CHECK_ERROR)
    message(STATUS "touch ${LAYOUT_ID} at ${TOUCH_POINT}:\n${CHECK_OUTPUT}")
    if(NOT CHECK_RESULT EQUAL 0)
        message(STATUS "${CHECK_ERROR}")
        message(SEND_ERROR "touch mapping is wrong for layout ${LAYOUT_ID}")
        set(ANY_FAILED TRUE)
    endif()
endforeach()

# The per-game config must exist afterwards, keyed by the ROM's SHA-1.
file(GLOB SAVED_CONFIGS "${WORK_DIR}/config/dualscreen/games/*.json")
list(LENGTH SAVED_CONFIGS CONFIG_COUNT)
if(CONFIG_COUNT EQUAL 0)
    message(SEND_ERROR "no per-game config was written to ${WORK_DIR}/config/dualscreen/games")
    set(ANY_FAILED TRUE)
else()
    list(GET SAVED_CONFIGS 0 SAVED_CONFIG)
    file(READ "${SAVED_CONFIG}" SAVED_TEXT)
    message(STATUS "saved config ${SAVED_CONFIG}:\n${SAVED_TEXT}")
    # The last run used top_only, and that is what should have been persisted.
    string(FIND "${SAVED_TEXT}" "\"layout_id\": \"top_only\"" LAYOUT_POSITION)
    if(LAYOUT_POSITION EQUAL -1)
        message(SEND_ERROR "the last layout used was not persisted")
        set(ANY_FAILED TRUE)
    endif()
endif()

if(ANY_FAILED)
    message(FATAL_ERROR "smoke test failed")
endif()
message(STATUS "smoke test passed")
