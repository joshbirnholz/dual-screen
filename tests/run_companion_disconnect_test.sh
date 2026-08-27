#!/usr/bin/env bash
#
# A companion leaves the session from the quick menu, and the host has to stay
# in that menu rather than dropping back into the game -- and the menu it stays
# in has to be a real one, that draws and that still answers a press.
#
# Nobody on the host asked for their game back: they are still sitting in the
# menu, and what it offers has just changed under them -- one screen where there
# were two. That the press came from the other device is the whole of what
# decides it, so this needs two real processes and the wire between them.
#
# The host runs on a display of its own rather than one xvfb-run picked, so that
# a key can be sent to it afterwards. A menu nobody can leave is the same to the
# player as no menu at all, and only a real press tells those two apart.
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
    [ -n "${XVFB_PID:-}" ] && kill "$XVFB_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

HOST_DISPLAY=":78"
Xvfb "$HOST_DISPLAY" -screen 0 1600x1000x24 >/dev/null 2>&1 &
XVFB_PID=$!
sleep 2

DISPLAY="$HOST_DISPLAY" \
XDG_CONFIG_HOME="$WORK_DIR/host/config" \
XDG_DATA_HOME="$WORK_DIR/host/data" \
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
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

# The companion is gone by now. Escape is Cancel, which is how a player leaves
# the quick menu: if the host answers it, there was a menu there to leave.
DISPLAY="$HOST_DISPLAY" xdotool search --name Dualscreen key Escape >/dev/null 2>&1
sleep 2

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
# happened, so each premise is checked before what rests on it.
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

# The point of the whole run. The menu has to have still been up after the
# companion left -- and to have been a menu the player could get out of, which
# only the press made after the disconnect can show.
MENU_DOWN=$(grep -c "the quick menu is down" "$WORK_DIR/host.log" 2>/dev/null)
if [ "$MENU_DOWN" -gt 1 ]; then
    echo "the host put the quick menu down $MENU_DOWN times: it did not stay up when the "\
"companion left" >&2
    FAILED=1
elif [ "$MENU_DOWN" -eq 0 ]; then
    echo "the host never left the quick menu even when asked to, so the menu it was left in was "\
"not one the player could get out of: a blurred game with no way back to it" >&2
    FAILED=1
fi

if [ "$FAILED" -eq 0 ]; then
    echo "ok - the host stayed in a working quick menu after the companion left"
fi
exit "$FAILED"
