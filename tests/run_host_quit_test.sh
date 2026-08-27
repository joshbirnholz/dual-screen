#!/usr/bin/env bash
#
# The host stops while the companion is sitting in the quick menu.
#
# That menu belongs to the host: the companion draws what it is told and
# forwards its presses rather than answering them. So a host that goes away
# leaves the companion holding a menu about a session that no longer exists --
# and, worse, one it now starts answering itself, because there is nobody left
# to forward to. Choosing Quit from the companion hit exactly that: the game
# stopped on the host, the confirmation stayed up over here, and pressing A
# again quit this device instead of doing what it looked like it would do.
#
# Both devices are in the menu when the host stops, which is the state the bug
# needed -- the game being on screen was always handled.
#
# Usage: run_host_quit_test.sh <xvfb-run> <dualscreen> <mock core> <work dir>

set -uo pipefail

XVFB_RUN="$1"
DUALSCREEN="$2"
MOCK_CORE="$3"
WORK_DIR="$4"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/host/config" "$WORK_DIR/companion/config"
printf 'dualscreen host quit test rom' > "$WORK_DIR/fake.nds"

cleanup() {
    [ -n "${HOST_PID:-}" ] && kill "$HOST_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

# --start-screen layout puts the host in the menu, and the companion follows it
# there, since the host holds the one menu and says when it is up.
XDG_CONFIG_HOME="$WORK_DIR/host/config" \
XDG_DATA_HOME="$WORK_DIR/host/data" \
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
"$XVFB_RUN" -a -s "-screen 0 1280x800x24" \
    "$DUALSCREEN" \
        --game "$WORK_DIR/fake.nds" \
        --core "$MOCK_CORE" \
        --layout upper_on_tv \
        --start-screen layout \
        --windowed \
        --exit-after-seconds 7 \
    > "$WORK_DIR/host.log" 2>&1 &
HOST_PID=$!

sleep 2

# Outliving the host by several seconds on purpose: what is being checked is
# what this device does with the time after the other one has gone.
XDG_CONFIG_HOME="$WORK_DIR/companion/config" \
XDG_DATA_HOME="$WORK_DIR/companion/data" \
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
"$XVFB_RUN" -a -s "-screen 0 1280x800x24" \
    "$DUALSCREEN" \
        --connect bottom \
        --windowed \
        --exit-after-seconds 13 \
    > "$WORK_DIR/companion.log" 2>&1
COMPANION_RESULT=$?

wait "$HOST_PID"
HOST_RESULT=$?
HOST_PID=""

echo "--- the host ---"
cat "$WORK_DIR/host.log" 2>/dev/null
echo "--- the companion ---"
cat "$WORK_DIR/companion.log" 2>/dev/null

FAILED=0
if [ "$HOST_RESULT" -ne 0 ]; then
    echo "the host exited with $HOST_RESULT" >&2
    FAILED=1
fi
if [ "$COMPANION_RESULT" -ne 0 ]; then
    echo "the companion exited with $COMPANION_RESULT" >&2
    FAILED=1
fi

# Premises first, so a run that proved nothing says so rather than passing.
if ! grep -q "connected: showing the" "$WORK_DIR/companion.log" 2>/dev/null; then
    echo "the two devices were never in a session, so nothing here was tested" >&2
    FAILED=1
fi
if ! grep -q "the quick menu is up" "$WORK_DIR/companion.log" 2>/dev/null; then
    echo "the companion never followed the host into the menu, so it was not in the state the "\
"bug needed" >&2
    FAILED=1
fi

if ! grep -q "the session this device joined has ended" "$WORK_DIR/companion.log" 2>/dev/null; then
    echo "the companion never noticed the host had gone, so it is still sitting in a menu "\
"belonging to a session that ended" >&2
    FAILED=1
fi

if [ "$FAILED" -eq 0 ]; then
    echo "ok - a companion leaves the menu when the session under it ends"
fi
exit "$FAILED"
