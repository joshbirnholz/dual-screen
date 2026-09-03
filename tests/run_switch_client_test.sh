#!/usr/bin/env bash
#
# Runs the Switch companion against a real host: one dualscreen process playing
# a game, one dualscreen-companion joining it as the other screen.
#
# The client is one program with a small guarded section for the console's own
# start-up, so everything below the window -- discovery, the handshake, the
# layout arriving, frames being decoded and drawn, buttons and touches going
# back -- is the same code that runs on the Switch. This is where it actually
# gets run.
#
# The host is pinned to the split-across-devices layout, so a correct session
# leaves the host showing only the top screen (blue) and the companion showing
# only the bottom (green), or the other way round when the companion asks for
# the top.
#
# Usage: run_switch_client_test.sh <xvfb-run> <dualscreen> <companion> <mock core> <work dir> <checker>

set -uo pipefail

COMPANION_SCREEN="${COMPANION_SCREEN:-bottom}"
# Which console the host is playing. Only the extension decides it -- see
# system_for_rom() -- so running this against another one is a mock core and
# a filename, not a second copy of this script. The client has to work on
# every console the host can play, and each of them lays its screens out and
# sizes its touch space differently.
ROM_EXTENSION="${COMPANION_ROM_EXTENSION:-nds}"

XVFB_RUN="$1"
DUALSCREEN="$2"
COMPANION="$3"
MOCK_CORE="$4"
WORK_DIR="$5"
CHECKER="$6"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/host/config/dualscreen"
printf 'dualscreen switch client test rom' > "$WORK_DIR/fake.$ROM_EXTENSION"

HOST_SHOT="$WORK_DIR/host.ppm"
COMPANION_SHOT="$WORK_DIR/companion.ppm"

cleanup() {
    [ -n "${HOST_PID:-}" ] && kill "$HOST_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

XDG_CONFIG_HOME="$WORK_DIR/host/config" \
XDG_DATA_HOME="$WORK_DIR/host/data" \
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
"$XVFB_RUN" -a -s "-screen 0 1600x1000x24" \
    "$DUALSCREEN" \
        --game "$WORK_DIR/fake.$ROM_EXTENSION" \
        --core "$MOCK_CORE" \
        --layout upper_on_tv \
        --windowed \
        --exit-after-seconds 16 \
        --screenshot-at-seconds 9 \
        --screenshot "$HOST_SHOT" \
    > "$WORK_DIR/host.log" 2>&1 &
HOST_PID=$!

# Let it start listening and announcing before anyone looks for it.
sleep 2

# The companion runs no core and is given none: it has no ROM, no BIOS and no
# emulator, which is the whole point of the thing.
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
"$XVFB_RUN" -a -s "-screen 0 1280x720x24" \
    "$COMPANION" \
        --connect "$COMPANION_SCREEN" \
        --name "Test Switch" \
        --windowed \
        --exit-after-seconds 9 \
        --screenshot-at-seconds 6 \
        --screenshot "$COMPANION_SHOT" \
    > "$WORK_DIR/companion.log" 2>&1
COMPANION_RESULT=$?

wait "$HOST_PID"
HOST_RESULT=$?
HOST_PID=""

FAILED=0

if [ "$HOST_RESULT" -ne 0 ]; then
    echo "the host process exited with $HOST_RESULT" >&2
    FAILED=1
fi
if [ "$COMPANION_RESULT" -ne 0 ]; then
    echo "the companion process exited with $COMPANION_RESULT" >&2
    FAILED=1
fi

echo "--- host log ---"
cat "$WORK_DIR/host.log" 2>/dev/null
echo "--- companion log ---"
cat "$WORK_DIR/companion.log" 2>/dev/null

if ! grep -q "connected to" "$WORK_DIR/companion.log" 2>/dev/null; then
    echo "the companion never reported connecting to the host" >&2
    FAILED=1
fi
if ! grep -q "connected as the" "$WORK_DIR/host.log" 2>/dev/null; then
    echo "the host never reported accepting a companion" >&2
    FAILED=1
fi
# The host names whichever device joined, which is how this run proves the
# companion it saw was this client and not something left over.
if ! grep -q "Test Switch" "$WORK_DIR/host.log" 2>/dev/null; then
    echo "the host did not name the companion that joined" >&2
    FAILED=1
fi

check_shot() {
    local shot="$1" expectation="$2" who="$3"
    if [ ! -f "$shot" ]; then
        echo "no screenshot from the $who" >&2
        FAILED=1
        return
    fi
    echo "--- $who ---"
    if ! "$CHECKER" "$shot" "$expectation"; then
        echo "the $who did not show the screen it owns" >&2
        FAILED=1
    fi
}

# Under the split layout each device fills itself with its own screen. The mock
# core paints the top screen blue and the bottom one green, so which colour
# lands where is exactly the question of whether the client got the screen it
# asked for -- and that it decoded and drew the frames at all.
if [ "$COMPANION_SCREEN" = "top" ]; then
    check_shot "$HOST_SHOT" "center=green" "host"
    check_shot "$COMPANION_SHOT" "center=blue" "companion"
else
    check_shot "$HOST_SHOT" "center=blue" "host"
    check_shot "$COMPANION_SHOT" "center=green" "companion"
fi

if [ "$FAILED" -ne 0 ]; then
    echo "switch client test failed" >&2
    exit 1
fi
echo "switch client test passed"
