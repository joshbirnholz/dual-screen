#!/usr/bin/env bash
#
# Checks that the companion's own screens -- the opening screen and the host
# list -- make a sound when the cursor moves on them.
#
# Worth a whole process to check, because what goes wrong here is not the
# decision but where it is reached from: both of those screens answer a press
# and hand it back, and for a while they returned past the point where the
# sound of the cursor moving was decided. Nothing below the level of the real
# input loop can see that, so this drives the real binary with a real key.
#
# Usage: run_menu_sound_test.sh <companion> <work dir>

set -uo pipefail

COMPANION="$1"
WORK_DIR="$2"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/config" "$WORK_DIR/data"

# A display of its own rather than xvfb-run's, because the key has to be sent
# into the same server the app is drawing on and xvfb-run picks its own.
DISPLAY_NUMBER=79
export DISPLAY=":$DISPLAY_NUMBER"

cleanup() {
    [ -n "${APP_PID:-}" ] && kill "$APP_PID" 2>/dev/null
    [ -n "${XVFB_PID:-}" ] && kill "$XVFB_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

Xvfb "$DISPLAY" -screen 0 640x480x24 >/dev/null 2>&1 &
XVFB_PID=$!
sleep 2

LOG="$WORK_DIR/companion.log"
XDG_CONFIG_HOME="$WORK_DIR/config" XDG_DATA_HOME="$WORK_DIR/data" \
    SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
    "$COMPANION" --windowed --verbose --exit-after-seconds 8 > "$LOG" 2>&1 &
APP_PID=$!

# Long enough for the window to exist and the opening screen to be up.
sleep 3
if ! xdotool search --name Dualscreen key Down >/dev/null 2>&1; then
    echo "could not send a key to the companion's window" >&2
    exit 1
fi
sleep 1
xdotool search --name Dualscreen key Down >/dev/null 2>&1

wait "$APP_PID"
APP_PID=

if ! grep -q "key down: Down" "$LOG"; then
    echo "the key never reached the app, so this proves nothing about its sound" >&2
    sed -n '1,40p' "$LOG" >&2
    exit 1
fi
if ! grep -q "effect: move cursor" "$LOG"; then
    echo "the cursor moved on the opening screen and no sound was asked for" >&2
    sed -n '1,40p' "$LOG" >&2
    exit 1
fi

echo "ok - the opening screen sounds when its cursor moves"
