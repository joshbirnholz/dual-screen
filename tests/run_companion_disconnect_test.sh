#!/usr/bin/env bash
#
# A companion leaves the session from the quick menu, and the host has to stay
# in that menu rather than dropping back into the game.
#
# Nobody on the host asked for their game back: they are still sitting in the
# menu, and what it offers has just changed under them -- one screen where there
# were two. That the press came from the other device is the whole of what
# decides it, so this needs two real processes and the wire between them.
#
# Usage: run_companion_disconnect_test.sh <xvfb-run> <dualscreen> <companion> <mock core> <work dir>

set -uo pipefail

XVFB_RUN="$1"
DUALSCREEN="$2"
COMPANION="$3"
MOCK_CORE="$4"
WORK_DIR="$5"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/host/config/dualscreen"
printf 'dualscreen companion disconnect test rom' > "$WORK_DIR/fake.nds"

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
        --game "$WORK_DIR/fake.nds" \
        --core "$MOCK_CORE" \
        --layout upper_on_tv \
        --windowed \
        --exit-after-seconds 14 \
    > "$WORK_DIR/host.log" 2>&1 &
HOST_PID=$!

# Let it start listening and announcing before anyone looks for it.
sleep 2

SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
"$XVFB_RUN" -a -s "-screen 0 1280x720x24" \
    "$COMPANION" \
        --connect bottom \
        --name "Test Switch" \
        --windowed \
        --disconnect-at-seconds 4 \
        --exit-after-seconds 8 \
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

# Everything below is only meaningful if the session and the press really
# happened, so each step is checked before what it is a premise for.
if ! grep -q "connected as the" "$WORK_DIR/host.log" 2>/dev/null; then
    echo "the host never accepted a companion, so nothing here was tested" >&2
    FAILED=1
fi
if ! grep -q "asked to leave the session from the quick menu" "$WORK_DIR/companion.log" 2>/dev/null
then
    echo "the companion never pressed Disconnect, so nothing here was tested" >&2
    FAILED=1
fi
if ! grep -q "the quick menu is up" "$WORK_DIR/host.log" 2>/dev/null; then
    echo "the host never put the quick menu up, so nothing here was tested" >&2
    FAILED=1
fi
if ! grep -q "the session ended" "$WORK_DIR/companion.log" 2>/dev/null; then
    echo "the companion never left the session" >&2
    FAILED=1
fi

# The point of the whole run: the menu went up and never came back down.
if grep -q "the quick menu is down" "$WORK_DIR/host.log" 2>/dev/null; then
    echo "the host left the quick menu when the companion disconnected, dropping the player back "\
"into the game they did not ask to go back to" >&2
    FAILED=1
fi

if [ "$FAILED" -eq 0 ]; then
    echo "ok - the host stayed in the quick menu after the companion left"
fi
exit "$FAILED"
