#!/usr/bin/env bash
#
# The whole Wii U path across two machines: fake_wii_u hands only the screen a
# companion actually asked for to a host over shared memory (see
# ExternalControl::RequestScreen), and the host hands that on to the companion
# over the network. Cemu's own window is where the other screen appears now,
# which nothing here can screenshot -- fake_wii_u stands in for Cemu, not for a
# window of its own -- so what this checks on the host's side is that it never
# composites either screen locally any more, only that it asked for and
# forwarded the right one.
#
# Three processes, and every seam between them is the real one -- the frame
# link, the wire protocol, the video encoder and the video decoder.
#
# It also checks the screens went as video rather than as changed tiles. That is
# not a preference here, it is the measurement: a GamePad screen in motion costs
# 42.8 Mbit/s as tiles and 2.45 as H.264 (docs/research-notes.md), so a Wii U
# session that quietly fell back to tiles would be unusable on a real network
# while looking perfectly correct in a screenshot.
#
# Usage: run_wii_u_companion_test.sh <xvfb-run> <dualscreen> <fake_wii_u> <work dir> <checker>

set -uo pipefail

XVFB_RUN="$1"
DUALSCREEN="$2"
FAKE_WII_U="$3"
WORK_DIR="$4"
CHECKER="$5"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/host/config" "$WORK_DIR/companion/config"
printf 'dualscreen wii u companion test image' > "$WORK_DIR/game.wud"

SOCKET="$WORK_DIR/link.sock"
HOST_SHOT="$WORK_DIR/host.ppm"
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
        --screenshot-at-seconds 13 \
        --screenshot "$HOST_SHOT" \
    > "$WORK_DIR/host.log" 2>&1 &
HOST_PID=$!

sleep 2

XDG_CONFIG_HOME="$WORK_DIR/companion/config" \
XDG_DATA_HOME="$WORK_DIR/companion/data" \
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
"$XVFB_RUN" -a -s "-screen 0 1600x1000x24" \
    "$DUALSCREEN" \
        --connect bottom \
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
echo "--- the companion ---"
cat "$WORK_DIR/companion.log" 2>/dev/null
echo "--- the game ---"
cat "$WORK_DIR/game.log" 2>/dev/null

FAILED=0
if [ "$HOST_RESULT" -ne 0 ]; then
    echo "the host exited with $HOST_RESULT" >&2
    FAILED=1
fi
if [ "$COMPANION_RESULT" -ne 0 ]; then
    echo "the companion exited with $COMPANION_RESULT" >&2
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
require "$WORK_DIR/host.log" "connected as the bottom screen" \
    "the host never accepted a companion, so nothing crossed the network"
require "$WORK_DIR/host.log" "a companion wants the bottom screen; asking Cemu for it" \
    "the host never asked the far end for the screen the companion wanted"
require "$WORK_DIR/companion.log" "following the host: Wii U" \
    "the companion did not learn which console it had joined"
require "$WORK_DIR/host.log" "video encoder: " \
    "the host never opened a video encoder, so a Wii U's screens went out as tiles"
require "$WORK_DIR/companion.log" "video decoder: " \
    "the companion never opened a video decoder"

# A fallback is otherwise silent, and the screenshot would look identical.
if grep -q "falling back to tiles" "$WORK_DIR/host.log" 2>/dev/null; then
    echo "the host gave up on video and sent tiles instead" >&2
    FAILED=1
fi

# The GamePad screen goes to the companion; the television is Cemu's own
# window to show now, not this app's -- so the host never composites either
# screen locally any more (see request_screen() in app.cpp). "compositing a
# screen" only ever logs once real pixels are drawn, so its absence here is
# the proof, not a screenshot color that a placeholder could coincidentally
# match.
if grep -q "compositing a screen" "$WORK_DIR/host.log" 2>/dev/null; then
    echo "the host composited a screen locally; it should have left both to Cemu and the companion" >&2
    FAILED=1
fi
if [ ! -f "$COMPANION_SHOT" ]; then
    echo "no screenshot from the companion" >&2
    FAILED=1
elif ! "$CHECKER" "$COMPANION_SHOT" center=green; then
    echo "the companion was not showing the GamePad screen it was handed" >&2
    FAILED=1
fi

# And with the emulator not started at all. The second device is the one most
# likely to be in somebody's hands, and it would otherwise sit on a blank window
# with no way to tell a session that has not begun from one that is broken. It
# works this out for itself: it knows which console it joined and that nothing
# has arrived, so nothing has to cross the wire to say so.
{
    shot="$WORK_DIR/waiting.ppm"
    XDG_CONFIG_HOME="$WORK_DIR/host/config" \
    XDG_DATA_HOME="$WORK_DIR/host/data" \
    SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
    "$XVFB_RUN" -a -s "-screen 0 1600x1000x24" \
        "$DUALSCREEN" \
            --game "$WORK_DIR/game.wud" \
            --external-socket "$WORK_DIR/nobody.sock" \
            --windowed \
            --exit-after-seconds 9 \
        > "$WORK_DIR/waiting-host.log" 2>&1 &
    HOST_PID=$!

    sleep 2

    XDG_CONFIG_HOME="$WORK_DIR/companion/config" \
    XDG_DATA_HOME="$WORK_DIR/companion/data" \
    SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
    "$XVFB_RUN" -a -s "-screen 0 1600x1000x24" \
        "$DUALSCREEN" \
            --connect bottom \
            --windowed \
            --exit-after-seconds 6 \
            --screenshot-at-seconds 4 \
            --screenshot "$shot" \
        > "$WORK_DIR/waiting-companion.log" 2>&1
    waiting_result=$?

    wait "$HOST_PID"
    HOST_PID=""

    echo "--- waiting: the companion ---"
    cat "$WORK_DIR/waiting-companion.log" 2>/dev/null

    if [ "$waiting_result" -ne 0 ]; then
        echo "waiting: the companion exited with $waiting_result rather than sitting there" >&2
        FAILED=1
    fi
    if ! grep -q "following the host: Wii U" "$WORK_DIR/waiting-companion.log" 2>/dev/null; then
        echo "waiting: the companion never joined, so this run is not the case being tested" >&2
        FAILED=1
    fi
    if [ ! -f "$shot" ]; then
        echo "waiting: no screenshot from the companion" >&2
        FAILED=1
    elif ! "$CHECKER" "$shot" "text=0.1,0.35,0.9,0.60"; then
        echo "waiting: the companion showed a blank window rather than saying what the session "\
"was waiting for" >&2
        FAILED=1
    fi
}

if [ "$FAILED" -eq 0 ]; then
    echo "ok - a companion's requested screen reached it as video, and the host composited neither"
fi
exit "$FAILED"
