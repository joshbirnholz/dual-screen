#!/usr/bin/env bash
#
# The Switch/Android client on a console this app does not play itself.
#
# run_wii_u_companion_test.sh already runs the whole Wii U path across two
# devices, but with the desktop app standing on both ends of it. This runs the
# same path with dualscreen-companion as the second device -- the one program
# that is also the Switch homebrew and the Android app, differing from what
# runs here only in start-up, shutdown and how it asks its own panel how big
# it is.
#
# That difference is small and the sources are shared, which is exactly why
# this is worth running: nothing about the Wii U path was written with this
# client in mind, and the pieces it does not share -- its own window, its own
# host picker, its own idea of its display size, its own decoder
# configuration -- are the pieces that would quietly stop working.
#
# Three processes and every seam between them real: the frame link, the wire
# protocol, the video encoder and the video decoder.
#
# Usage: run_wii_u_client_test.sh <xvfb-run> <dualscreen> <companion> <fake_wii_u> <work dir> <checker>

set -uo pipefail

XVFB_RUN="$1"
DUALSCREEN="$2"
COMPANION="$3"
FAKE_WII_U="$4"
WORK_DIR="$5"
CHECKER="$6"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/host/config" "$WORK_DIR/companion/config"
printf 'dualscreen wii u client test image' > "$WORK_DIR/game.wud"

SOCKET="$WORK_DIR/link.sock"
COMPANION_SHOT="$WORK_DIR/companion.ppm"

cleanup() {
    [ -n "${HOST_PID:-}" ] && kill "$HOST_PID" 2>/dev/null
    [ -n "${COMPANION_PID:-}" ] && kill "$COMPANION_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

XDG_CONFIG_HOME="$WORK_DIR/host/config" \
XDG_DATA_HOME="$WORK_DIR/host/data" \
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
"$XVFB_RUN" -a -s "-screen 0 1600x1000x24" \
    "$DUALSCREEN" \
        --game "$WORK_DIR/game.wud" \
        --external-socket "$SOCKET" \
        --windowed \
        --exit-after-seconds 18 \
    > "$WORK_DIR/host.log" 2>&1 &
HOST_PID=$!

sleep 2

# A 1280x720 screen, which is what a handheld Switch's panel is -- so what
# this asks the host for is what that console would ask for.
XDG_CONFIG_HOME="$WORK_DIR/companion/config" \
XDG_DATA_HOME="$WORK_DIR/companion/data" \
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
"$XVFB_RUN" -a -s "-screen 0 1280x720x24" \
    "$COMPANION" \
        --connect bottom \
        --name "Test Switch" \
        --windowed \
        --exit-after-seconds 15 \
        --screenshot-at-seconds 12 \
        --screenshot "$COMPANION_SHOT" \
    > "$WORK_DIR/companion.log" 2>&1 &
COMPANION_PID=$!

# The emulator arrives last, the way starting Cemu by hand does.
sleep 2
"$FAKE_WII_U" --socket "$SOCKET" --seconds 13 > "$WORK_DIR/game.log" 2>&1
GAME_RESULT=$?

wait "$COMPANION_PID"
COMPANION_RESULT=$?
COMPANION_PID=""
wait "$HOST_PID"
HOST_RESULT=$?
HOST_PID=""

echo "--- the host ---"
cat "$WORK_DIR/host.log" 2>/dev/null
echo "--- the client ---"
cat "$WORK_DIR/companion.log" 2>/dev/null
echo "--- the game ---"
cat "$WORK_DIR/game.log" 2>/dev/null

FAILED=0
if [ "$HOST_RESULT" -ne 0 ]; then
    echo "the host exited with $HOST_RESULT" >&2
    FAILED=1
fi
if [ "$COMPANION_RESULT" -ne 0 ]; then
    echo "the client exited with $COMPANION_RESULT" >&2
    FAILED=1
fi
if [ "$GAME_RESULT" -ne 0 ]; then
    echo "the game exited with $GAME_RESULT" >&2
    FAILED=1
fi

# Premises first, so a run that proved nothing says so rather than passing.
require() {
    local file="$1"
    local phrase="$2"
    local why="$3"
    if ! grep -q "$phrase" "$file" 2>/dev/null; then
        echo "$why" >&2
        FAILED=1
    fi
}

require "$WORK_DIR/host.log" "a game connected" \
    "nothing ever connected over the frame link"
require "$WORK_DIR/host.log" "Test Switch connected as the bottom screen" \
    "the host never accepted this client, so nothing crossed the network"
require "$WORK_DIR/companion.log" "connected to .* as the bottom screen" \
    "the client never reported joining the session"
# The client asks for the screen at its own panel's size, which is what stops
# a high-resolution graphics pack being carried whole to a handheld -- see
# ds::fit_within(). A Switch panel is 1280x720.
require "$WORK_DIR/host.log" "a companion wants the bottom screen at up to [1-9][0-9]*x[1-9][0-9]*; asking Cemu for it" \
    "the host was never told what size this client's panel is"
require "$WORK_DIR/host.log" "video encoder: " \
    "the host never opened a video encoder, so a Wii U's screens went out as tiles"
require "$WORK_DIR/companion.log" "video decoder: " \
    "the client never opened a video decoder, which on a handheld is the difference between "\
"2.45 Mbit/s and 42.8"

if grep -q "falling back to tiles" "$WORK_DIR/host.log" 2>/dev/null; then
    echo "the host gave up on video and sent tiles instead" >&2
    FAILED=1
fi

# And the picture actually arrived. fake_wii_u's GamePad screen is green; the
# television is not, so this also says the client got the screen it asked for
# rather than the other one.
if [ ! -f "$COMPANION_SHOT" ]; then
    echo "no screenshot from the client" >&2
    FAILED=1
elif ! "$CHECKER" "$COMPANION_SHOT" center=green; then
    echo "the client was not showing the GamePad screen it was handed" >&2
    FAILED=1
fi

if [ "$FAILED" -eq 0 ]; then
    echo "ok - the Switch/Android client played a Wii U as the GamePad screen, over video"
fi
exit "$FAILED"
