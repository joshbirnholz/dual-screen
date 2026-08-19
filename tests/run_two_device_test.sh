#!/usr/bin/env bash
#
# Runs two real dualscreen processes against the mock core -- one hosting, one
# joining -- and checks each ends up showing its own half of the game.
#
# The host is pinned to the split-across-devices layout, so a correct session
# leaves the host showing only the top screen (blue) and the companion showing
# only the bottom (green). That requires discovery, the handshake, the layout
# being carried across and frames actually arriving, which is why it is worth
# running as a whole and not only in pieces.
#
# Usage: run_two_device_test.sh <xvfb-run> <dualscreen> <mock core> <work dir> <checker>

set -uo pipefail

XVFB_RUN="$1"
DUALSCREEN="$2"
MOCK_CORE="$3"
WORK_DIR="$4"
CHECKER="$5"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/host/config" "$WORK_DIR/companion/config"
printf 'dualscreen two device test rom' > "$WORK_DIR/fake.nds"

HOST_SHOT="$WORK_DIR/host.ppm"
COMPANION_SHOT="$WORK_DIR/companion.ppm"

cleanup() {
    [ -n "${HOST_PID:-}" ] && kill "$HOST_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

# Both are bounded by wall time rather than a frame count: without vsync the
# frame rate is unbounded, so a frame count is not a duration and the two
# processes would not reliably overlap. Screenshots are taken part-way through
# rather than at exit, because what each device draws depends on the other one
# still being connected at that moment.
XDG_CONFIG_HOME="$WORK_DIR/host/config" \
XDG_DATA_HOME="$WORK_DIR/host/data" \
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
"$XVFB_RUN" -a -s "-screen 0 1600x1000x24" \
    "$DUALSCREEN" \
        --game "$WORK_DIR/fake.nds" \
        --core "$MOCK_CORE" \
        --layout split_devices \
        --windowed \
        --exit-after-seconds 16 \
        --screenshot-at-seconds 9 \
        --screenshot "$HOST_SHOT" \
    > "$WORK_DIR/host.log" 2>&1 &
HOST_PID=$!

# Let it start listening and announcing before anyone looks for it.
sleep 2

XDG_CONFIG_HOME="$WORK_DIR/companion/config" \
XDG_DATA_HOME="$WORK_DIR/companion/data" \
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
"$XVFB_RUN" -a -s "-screen 0 1600x1000x24" \
    "$DUALSCREEN" \
        --connect bottom \
        --core "$MOCK_CORE" \
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

# The logs are the clearest evidence that a session really happened.
if ! grep -q "connected" "$WORK_DIR/companion.log" 2>/dev/null; then
    echo "the companion never reported connecting to the host" >&2
    FAILED=1
fi
if ! grep -q "connected as the" "$WORK_DIR/host.log" 2>/dev/null; then
    echo "the host never reported accepting a companion" >&2
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

# Under the split layout each device fills itself with its own screen.
check_shot "$HOST_SHOT" "center=blue" "host"
check_shot "$COMPANION_SHOT" "center=green" "companion"

if [ "$FAILED" -ne 0 ]; then
    echo "two-device test failed" >&2
    exit 1
fi
echo "two-device test passed"
