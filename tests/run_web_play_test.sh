#!/usr/bin/env bash
#
# Web Play, end to end: this app hosting a Wii U game played by fake_wii_u,
# with Web Play on, and a stand-in browser (web_play_client.py) joining over
# HTTPS -- served the page, sent a keyframe of the television that the fake
# Cemu sent only because a browser asked for it (a mirrored screen), and its
# pad taken as one of the game's controllers.
#
# Usage: run_web_play_test.sh <xvfb-run> <dualscreen> <fake_wii_u> <python3> <client.py> <work dir>

set -uo pipefail

XVFB_RUN="$1"
DUALSCREEN="$2"
FAKE_WII_U="$3"
PYTHON="$4"
CLIENT="$5"
WORK_DIR="$6"
PORT=18443

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/config/dualscreen" "$WORK_DIR/data"
printf 'dualscreen wii u test image' > "$WORK_DIR/game.wud"
printf '{"web_play": true, "web_play_port": %d, "device_name": "Test Deck"}\n' "$PORT" \
    > "$WORK_DIR/config/dualscreen/settings.json"

cleanup() {
    [ -n "${SHELL_PID:-}" ] && kill "$SHELL_PID" 2>/dev/null
    [ -n "${GAME_PID:-}" ] && kill "$GAME_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

XDG_CONFIG_HOME="$WORK_DIR/config" XDG_DATA_HOME="$WORK_DIR/data" \
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
"$XVFB_RUN" -a -s "-screen 0 1280x800x24" \
    "$DUALSCREEN" --game "$WORK_DIR/game.wud" --external-socket "$WORK_DIR/game.sock" \
        --windowed --exit-after-seconds 20 > "$WORK_DIR/shell.log" 2>&1 &
SHELL_PID=$!
sleep 3
"$FAKE_WII_U" --socket "$WORK_DIR/game.sock" --seconds 15 > "$WORK_DIR/game.log" 2>&1 &
GAME_PID=$!
sleep 3

"$PYTHON" "$CLIENT" "$PORT" 10 > "$WORK_DIR/client.log" 2>&1
CLIENT_RESULT=$?

wait "$GAME_PID"
GAME_PID=""
wait "$SHELL_PID"
SHELL_RESULT=$?
SHELL_PID=""

echo "--- the browser ---"; cat "$WORK_DIR/client.log"
echo "--- the shell ---"; grep -i "web play\|controllers:\|browser\|Cemu to send" "$WORK_DIR/shell.log"

FAILED=0
if [ "$CLIENT_RESULT" -ne 0 ]; then
    echo "the browser didn't get what it came for" >&2
    FAILED=1
fi
if [ "$SHELL_RESULT" -ne 0 ]; then
    echo "the shell exited with $SHELL_RESULT" >&2
    cat "$WORK_DIR/shell.log"
    FAILED=1
fi
if ! grep -q "asking Cemu to send it as well" "$WORK_DIR/shell.log"; then
    echo "the television was never asked for as a mirror" >&2
    FAILED=1
fi
if ! grep -q "controllers: .*Test Pad" "$WORK_DIR/shell.log"; then
    echo "the browser's pad never became a controller" >&2
    FAILED=1
fi
exit "$FAILED"
