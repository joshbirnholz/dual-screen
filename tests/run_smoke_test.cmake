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
# Every layout the picker offers, in its order. Where a screen lands somewhere
# none of the named regions reach -- the small screen of the two focus layouts --
# the point is given outright.
set(CASES
    "standard|upper=blue+lower=green+at=0.03,0.06,bezel+at=0.30,0.20,shell"
    "upper_focus|left=blue+at=0.86,0.785,green"
    "touch_focus|right=green+at=0.115,0.145,blue"
    "vertical|upper=blue+lower=green+topleft=black"
    "side_by_side|left=blue+right=green"
    "upper_on_tv|upper=blue+lower=green"
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
                    --layout vertical
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
        set(EXPECTATION "upper=blue")
        if(NOT EXISTS "${SAVE_FILE}")
            message(SEND_ERROR "no save file was written to ${SAVE_FILE}")
            set(ANY_FAILED TRUE)
        endif()
    else()
        set(EXPECTATION "upper=orange")
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
# A touch has to come back out where it went in, whatever the layout did to the
# screen on the way. Side-by-Side is the one that matters most here: it turns
# both screens a quarter turn, so the drawing code and the touch code have to
# agree about which way round that is, and nothing else would notice if they
# stopped agreeing.
set(TOUCH_CASES
    "vertical|0.45,0.80|crosshair=0.45,0.80"
    "vertical|0.50,0.25|nocrosshair=1"
    "standard|0.50,0.72|crosshair=0.50,0.72"
    "standard|0.50,0.20|nocrosshair=1"
    "side_by_side|0.75,0.40|crosshair=0.75,0.40"
    "side_by_side|0.25,0.40|nocrosshair=1"
    "touch_focus|0.70,0.50|crosshair=0.70,0.50"
    "upper_focus|0.86,0.785|crosshair=0.86,0.785"
    "upper_focus|0.30,0.50|nocrosshair=1"
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

# The pointer mode has to survive the trip from settings.json into the running
# app. It was read and stored and then never handed to the thing that acts on
# it, so the setting did nothing at all; nothing on screen shows that, but the
# app says which mode it came up in.
set(POINTER_HOME "${WORK_DIR}/pointer")
file(REMOVE_RECURSE "${POINTER_HOME}")
file(WRITE "${POINTER_HOME}/config/dualscreen/settings.json" "{\"cursor_mode\": \"hidden\"}")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env
            "XDG_CONFIG_HOME=${POINTER_HOME}/config"
            "XDG_DATA_HOME=${POINTER_HOME}/data"
            "SDL_AUDIODRIVER=dummy"
            "SDL_VIDEODRIVER=x11"
            "${XVFB_RUN}" -a -s "-screen 0 1600x1000x24"
            "${DUALSCREEN}"
                --game "${WORK_DIR}/fake.nds"
                --core "${MOCK_CORE}"
                --windowed
                --exit-after-frames 10
    RESULT_VARIABLE POINTER_RESULT
    OUTPUT_VARIABLE POINTER_OUTPUT
    ERROR_VARIABLE POINTER_ERROR)
if(NOT POINTER_OUTPUT MATCHES "pointer: Off")
    message(STATUS "${POINTER_OUTPUT}")
    message(STATUS "${POINTER_ERROR}")
    message(SEND_ERROR "the stored pointer mode did not reach the running app")
    set(ANY_FAILED TRUE)
else()
    message(STATUS "pointer mode read from settings.json: ok")
endif()

# Smoothing: the same frame drawn with nearest and with linear sampling must not
# come out identical. Checking that the toggle reaches the screen takes a real
# run -- a unit test can see the flag being stored, but not the texture being
# sampled differently. Separate config homes keep this off the persistence
# check below, which expects the last run to have been vertical.
set(SMOOTHING_LAYOUT "touch_focus")
set(FILTER_SHOTS "")
foreach(FILTERING true false)
    set(FILTER_HOME "${WORK_DIR}/filter-${FILTERING}")
    set(FILTER_LAYOUT "${SMOOTHING_LAYOUT}")
    file(REMOVE_RECURSE "${FILTER_HOME}")
    file(WRITE "${FILTER_HOME}/config/dualscreen/settings.json"
        "{\"default_bilinear_filtering\": ${FILTERING}}")
    set(SHOT "${FILTER_HOME}/shot.ppm")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env
                "XDG_CONFIG_HOME=${FILTER_HOME}/config"
                "XDG_DATA_HOME=${FILTER_HOME}/data"
                "SDL_AUDIODRIVER=dummy"
                "SDL_VIDEODRIVER=x11"
                "${XVFB_RUN}" -a -s "-screen 0 1600x1000x24"
                "${DUALSCREEN}"
                    --game "${WORK_DIR}/fake.nds"
                    --core "${MOCK_CORE}"
                    --layout "${FILTER_LAYOUT}"
                    --windowed
                    --exit-after-frames ${FRAME_COUNT}
                    --screenshot "${SHOT}"
        RESULT_VARIABLE RUN_RESULT
        OUTPUT_VARIABLE RUN_OUTPUT
        ERROR_VARIABLE RUN_ERROR)
    if(NOT RUN_RESULT EQUAL 0 OR NOT EXISTS "${SHOT}")
        message(STATUS "${RUN_OUTPUT}")
        message(STATUS "${RUN_ERROR}")
        message(SEND_ERROR "smoothing run (${FILTERING}) failed")
        set(ANY_FAILED TRUE)
    else()
        list(APPEND FILTER_SHOTS "${SHOT}")
    endif()
endforeach()

list(LENGTH FILTER_SHOTS FILTER_SHOT_COUNT)
if(FILTER_SHOT_COUNT EQUAL 2)
    list(GET FILTER_SHOTS 0 FILTER_ON_SHOT)
    list(GET FILTER_SHOTS 1 FILTER_OFF_SHOT)
    execute_process(
        COMMAND "${CHECKER}" "${FILTER_ON_SHOT}" "differs=${FILTER_OFF_SHOT}"
        RESULT_VARIABLE CHECK_RESULT
        OUTPUT_VARIABLE CHECK_OUTPUT
        ERROR_VARIABLE CHECK_ERROR)
    message(STATUS "smoothing on vs off:\n${CHECK_OUTPUT}")
    if(NOT CHECK_RESULT EQUAL 0)
        message(STATUS "${CHECK_ERROR}")
        message(SEND_ERROR "smoothing made no difference to what was drawn")
        set(ANY_FAILED TRUE)
    endif()
endif()

# ...except on the Standard layout at this size. The smoke test's window is
# 1280x800, where that layout locks to the artwork's own pixel size and the
# screens come out one-to-one -- and a screen drawn one-to-one is never smoothed,
# whatever the setting says, because there is nothing to resample.
set(STANDARD_SHOTS "")
foreach(FILTERING true false)
    set(STANDARD_HOME "${WORK_DIR}/standard-${FILTERING}")
    file(REMOVE_RECURSE "${STANDARD_HOME}")
    file(WRITE "${STANDARD_HOME}/config/dualscreen/settings.json"
        "{\"default_bilinear_filtering\": ${FILTERING}}")
    set(SHOT "${STANDARD_HOME}/shot.ppm")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env
                "XDG_CONFIG_HOME=${STANDARD_HOME}/config"
                "XDG_DATA_HOME=${STANDARD_HOME}/data"
                "SDL_AUDIODRIVER=dummy"
                "SDL_VIDEODRIVER=x11"
                "${XVFB_RUN}" -a -s "-screen 0 1600x1000x24"
                "${DUALSCREEN}"
                    --game "${WORK_DIR}/fake.nds"
                    --core "${MOCK_CORE}"
                    --layout standard
                    --windowed
                    --exit-after-frames ${FRAME_COUNT}
                    --screenshot "${SHOT}"
        RESULT_VARIABLE RUN_RESULT
        OUTPUT_VARIABLE RUN_OUTPUT
        ERROR_VARIABLE RUN_ERROR)
    if(NOT RUN_RESULT EQUAL 0 OR NOT EXISTS "${SHOT}")
        message(STATUS "${RUN_OUTPUT}")
        message(STATUS "${RUN_ERROR}")
        message(SEND_ERROR "standard smoothing run (${FILTERING}) failed")
        set(ANY_FAILED TRUE)
    else()
        list(APPEND STANDARD_SHOTS "${SHOT}")
    endif()
endforeach()

list(LENGTH STANDARD_SHOTS STANDARD_SHOT_COUNT)
if(STANDARD_SHOT_COUNT EQUAL 2)
    list(GET STANDARD_SHOTS 0 STANDARD_ON_SHOT)
    list(GET STANDARD_SHOTS 1 STANDARD_OFF_SHOT)
    execute_process(
        COMMAND "${CHECKER}" "${STANDARD_ON_SHOT}" "same=${STANDARD_OFF_SHOT}"
        RESULT_VARIABLE CHECK_RESULT
        OUTPUT_VARIABLE CHECK_OUTPUT
        ERROR_VARIABLE CHECK_ERROR)
    message(STATUS "standard, smoothing on vs off:\n${CHECK_OUTPUT}")
    if(NOT CHECK_RESULT EQUAL 0)
        message(STATUS "${CHECK_ERROR}")
        message(SEND_ERROR "the Standard layout was smoothed at its own pixel size")
        set(ANY_FAILED TRUE)
    endif()
endif()

# The same app against a 3DS game: a different core, a different ROM extension,
# and two screens that are different shapes from each other. The upper screen is
# 5:3 and the touch screen 4:3, so a frontend still assuming they match would
# stretch one of them -- which is what the aspect checks below catch.
set(THREE_DS_HOME "${WORK_DIR}/three-ds")
file(REMOVE_RECURSE "${THREE_DS_HOME}")
file(WRITE "${THREE_DS_HOME}/game.3ds" "dualscreen 3ds smoke test rom")

set(THREE_DS_CASES
    # Stacked: the upper screen is wider than the touch screen below it.
    "vertical|upper=blue+lower=green+topleft=black"
    # The console's own arrangement, with nothing drawn around the screens yet.
    "standard|upper=blue+lower=green+topleft=black"
    "side_by_side|left=blue+right=green"
)

foreach(CASE IN LISTS THREE_DS_CASES)
    string(REPLACE "|" ";" PARTS "${CASE}")
    list(GET PARTS 0 LAYOUT_ID)
    list(GET PARTS 1 EXPECTATION_STRING)
    string(REPLACE "+" ";" EXPECTATIONS "${EXPECTATION_STRING}")

    # The mock core dirties its save RAM as it runs, and paints a band across
    # the upper screen when it finds one on the next launch. That band would sit
    # exactly where these checks look, so each case starts from a clean slate.
    file(REMOVE "${THREE_DS_HOME}/data/dualscreen/saves/game.srm")

    set(SHOT "${THREE_DS_HOME}/${LAYOUT_ID}.ppm")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env
                "XDG_CONFIG_HOME=${THREE_DS_HOME}/config"
                "XDG_DATA_HOME=${THREE_DS_HOME}/data"
                "SDL_AUDIODRIVER=dummy"
                "SDL_VIDEODRIVER=x11"
                "${XVFB_RUN}" -a -s "-screen 0 1600x1000x24"
                "${DUALSCREEN}"
                    --game "${THREE_DS_HOME}/game.3ds"
                    --core "${MOCK_CORE_3DS}"
                    --layout "${LAYOUT_ID}"
                    --windowed
                    --exit-after-frames ${FRAME_COUNT}
                    --screenshot "${SHOT}"
        RESULT_VARIABLE RUN_RESULT
        OUTPUT_VARIABLE RUN_OUTPUT
        ERROR_VARIABLE RUN_ERROR)

    if(NOT RUN_RESULT EQUAL 0 OR NOT EXISTS "${SHOT}")
        message(STATUS "${RUN_OUTPUT}")
        message(STATUS "${RUN_ERROR}")
        message(SEND_ERROR "the 3DS run failed for layout ${LAYOUT_ID}")
        set(ANY_FAILED TRUE)
        continue()
    endif()
    # The core says so itself: it paints a magenta band across the upper screen
    # when the layout option did not reach it.
    if(NOT RUN_OUTPUT MATCHES "splitting 400x480 into a 400x240 upper screen and a 320x240 touch screen")
        message(STATUS "${RUN_OUTPUT}")
        message(SEND_ERROR "the 3DS framebuffer was not split into two differently shaped screens")
        set(ANY_FAILED TRUE)
    endif()
    # The 3DS core renders on the GPU, as Citra does. If the frontend ever went
    # back to declining that, the core would simply refuse to load the game --
    # so the handshake is checked outright rather than inferred from the picture.
    if(NOT RUN_OUTPUT MATCHES "the core renders on the GPU")
        message(STATUS "${RUN_OUTPUT}")
        message(SEND_ERROR "the 3DS core did not get a hardware-rendering context")
        set(ANY_FAILED TRUE)
    endif()
    if(NOT RUN_OUTPUT MATCHES "OpenGL 3.3 core context")
        message(STATUS "${RUN_OUTPUT}")
        message(SEND_ERROR "a 3DS game did not get the OpenGL 3.3 context its core needs")
        set(ANY_FAILED TRUE)
    endif()

    execute_process(
        COMMAND "${CHECKER}" "${SHOT}" ${EXPECTATIONS}
        RESULT_VARIABLE CHECK_RESULT
        OUTPUT_VARIABLE CHECK_OUTPUT
        ERROR_VARIABLE CHECK_ERROR)
    message(STATUS "3DS layout ${LAYOUT_ID}:\n${CHECK_OUTPUT}")
    if(NOT CHECK_RESULT EQUAL 0)
        message(STATUS "${CHECK_ERROR}")
        message(SEND_ERROR "the 3DS layout ${LAYOUT_ID} did not render its screens where expected")
        set(ANY_FAILED TRUE)
    endif()
endforeach()

# The layout menu, drawn right after a frame from a core that renders on the GPU
# and hands the context back with its own state in it. That is the case where
# the frontend's own rendering breaks: with the blending the core leaves behind,
# text drawn from a coverage mask fills solid and the menu becomes rectangles.
file(REMOVE "${THREE_DS_HOME}/data/dualscreen/saves/game.srm")
set(MENU_SHOT "${THREE_DS_HOME}/menu.ppm")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env
            "XDG_CONFIG_HOME=${THREE_DS_HOME}/config"
            "XDG_DATA_HOME=${THREE_DS_HOME}/data"
            "SDL_AUDIODRIVER=dummy"
            "SDL_VIDEODRIVER=x11"
            "${XVFB_RUN}" -a -s "-screen 0 1600x1000x24"
            "${DUALSCREEN}"
                --game "${THREE_DS_HOME}/game.3ds"
                --core "${MOCK_CORE_3DS}"
                --layout vertical
                --windowed
                --start-screen layout
                --exit-after-frames ${FRAME_COUNT}
                --screenshot "${MENU_SHOT}"
    RESULT_VARIABLE MENU_RESULT
    OUTPUT_VARIABLE MENU_OUTPUT
    ERROR_VARIABLE MENU_ERROR)

if(NOT MENU_RESULT EQUAL 0 OR NOT EXISTS "${MENU_SHOT}")
    message(STATUS "${MENU_OUTPUT}")
    message(STATUS "${MENU_ERROR}")
    message(SEND_ERROR "the 3DS layout menu run failed")
    set(ANY_FAILED TRUE)
else()
    execute_process(
        COMMAND "${CHECKER}" "${MENU_SHOT}"
                # The title, and the footer prompts along the bottom.
                text=0.02,0.03,0.35,0.09
                text=0.10,0.94,0.70,0.98
        RESULT_VARIABLE MENU_CHECK_RESULT
        OUTPUT_VARIABLE MENU_CHECK_OUTPUT
        ERROR_VARIABLE MENU_CHECK_ERROR)
    message(STATUS "3DS layout menu text:\n${MENU_CHECK_OUTPUT}")
    if(NOT MENU_CHECK_RESULT EQUAL 0)
        message(STATUS "${MENU_CHECK_ERROR}")
        message(SEND_ERROR "the menu text was not legible over a GPU-rendered game")
        set(ANY_FAILED TRUE)
    endif()
endif()

# When a GPU-rendering core announces a frame and puts nothing in it, the app has
# to notice by itself and start recording the core's OpenGL calls. Asking for
# that with a flag is no use: the core resolves its GL functions once, while the
# game is opening, long before anyone knows anything is wrong -- so the wrappers
# have to be in place already and the recording armed by the symptom.
file(REMOVE "${THREE_DS_HOME}/data/dualscreen/saves/game.srm")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env
            "XDG_CONFIG_HOME=${THREE_DS_HOME}/config"
            "XDG_DATA_HOME=${THREE_DS_HOME}/data"
            "SDL_AUDIODRIVER=dummy"
            "SDL_VIDEODRIVER=x11"
            "MOCK_CORE_PRESENT_NOTHING=1"
            "${XVFB_RUN}" -a -s "-screen 0 1600x1000x24"
            "${DUALSCREEN}"
                --game "${THREE_DS_HOME}/game.3ds"
                --core "${MOCK_CORE_3DS}"
                --layout vertical
                --windowed
                --exit-after-frames ${FRAME_COUNT}
    RESULT_VARIABLE EMPTY_RESULT
    OUTPUT_VARIABLE EMPTY_OUTPUT
    ERROR_VARIABLE EMPTY_ERROR)

if(NOT EMPTY_OUTPUT MATCHES "tracing the core's OpenGL calls")
    message(STATUS "${EMPTY_OUTPUT}")
    message(SEND_ERROR "the app did not start tracing a core that presented an empty frame")
    set(ANY_FAILED TRUE)
elseif(NOT EMPTY_OUTPUT MATCHES "glScissor\\(0, 0, -1, -1\\)  ->  GL_INVALID_VALUE")
    message(STATUS "${EMPTY_OUTPUT}")
    message(SEND_ERROR "the trace did not name the call the driver rejected")
    set(ANY_FAILED TRUE)
else()
    message(STATUS "an empty GPU frame armed the call trace, which named the rejected call: ok")
endif()

# A touch on a 3DS lands in a 320x240 space, not a DS's 256x192 -- and the touch
# screen is narrower than the upper one, so a crop that took the wrong columns
# would put the crosshair off to one side.
file(REMOVE "${THREE_DS_HOME}/data/dualscreen/saves/game.srm")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env
            "XDG_CONFIG_HOME=${THREE_DS_HOME}/config"
            "XDG_DATA_HOME=${THREE_DS_HOME}/data"
            "SDL_AUDIODRIVER=dummy"
            "SDL_VIDEODRIVER=x11"
            "${XVFB_RUN}" -a -s "-screen 0 1600x1000x24"
            "${DUALSCREEN}"
                --game "${THREE_DS_HOME}/game.3ds"
                --core "${MOCK_CORE_3DS}"
                --layout vertical
                --windowed
                --exit-after-frames ${FRAME_COUNT}
                --simulate-touch "0.45,0.80"
                --screenshot "${THREE_DS_HOME}/touch.ppm"
    RESULT_VARIABLE RUN_RESULT
    OUTPUT_VARIABLE RUN_OUTPUT
    ERROR_VARIABLE RUN_ERROR)
if(NOT RUN_RESULT EQUAL 0 OR NOT EXISTS "${THREE_DS_HOME}/touch.ppm")
    message(STATUS "${RUN_OUTPUT}")
    message(STATUS "${RUN_ERROR}")
    message(SEND_ERROR "the 3DS touch run failed")
    set(ANY_FAILED TRUE)
else()
    execute_process(
        COMMAND "${CHECKER}" "${THREE_DS_HOME}/touch.ppm" "crosshair=0.45,0.80"
        RESULT_VARIABLE CHECK_RESULT
        OUTPUT_VARIABLE CHECK_OUTPUT
        ERROR_VARIABLE CHECK_ERROR)
    message(STATUS "3DS touch at 0.45,0.80:\n${CHECK_OUTPUT}")
    if(NOT CHECK_RESULT EQUAL 0)
        message(STATUS "${CHECK_ERROR}")
        message(SEND_ERROR "a touch on a 3DS did not come back where it went in")
        set(ANY_FAILED TRUE)
    endif()
endif()

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
    # The last run against this config home was the final touch case, and that
    # is the layout that should have been persisted.
    string(FIND "${SAVED_TEXT}" "\"layout_id\": \"upper_focus\"" LAYOUT_POSITION)
    if(LAYOUT_POSITION EQUAL -1)
        message(SEND_ERROR "the last layout used was not persisted")
        set(ANY_FAILED TRUE)
    endif()
endif()

if(ANY_FAILED)
    message(FATAL_ERROR "smoke test failed")
endif()
message(STATUS "smoke test passed")
