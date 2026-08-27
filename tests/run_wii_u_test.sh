#!/usr/bin/env bash
#
# A console this app does not play itself: two real processes, the real shared
# memory, and a picture on screen that came out of the other one.
#
# fake_wii_u stands in for Cemu -- see tools/fake_wii_u.cpp. It publishes a blue
# television screen and a green GamePad screen over the frame link, and this
# checks that the shell showed them. Which is the point of the whole exercise:
# every piece on this side of the link can be finished and held to before the
# emulator on the other side of it exists.
#
# Two runs, because a Wii U alone shows one screen at a time (SoloScreens::One).
# The first is left as it starts and should be the television; the second taps
# the swap key and should be the GamePad. Together they say both screens crossed
# the link, that the shell picked a layout that can hold them, and that the key
# a Wii U player needs -- the pad has no button to spare for it -- works.
#
# Usage: run_wii_u_test.sh <xvfb-run> <dualscreen> <fake_wii_u> <work dir> <checker>

set -uo pipefail

XVFB_RUN="$1"
DUALSCREEN="$2"
FAKE_WII_U="$3"
WORK_DIR="$4"
CHECKER="$5"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"
# Never read: a Wii U game is loaded by the program playing it, and this file
# exists only so the shell has something to name the session after and hash a
# per-game layout against.
printf 'dualscreen wii u test image' > "$WORK_DIR/game.wud"

FAILED=0

cleanup() {
    [ -n "${SHELL_PID:-}" ] && kill "$SHELL_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

# $1 run name, $2 expected colour, $3... extra dualscreen arguments
run_session() {
    local name="$1"
    local expect="$2"
    shift 2

    local socket="$WORK_DIR/$name.sock"
    local shot="$WORK_DIR/$name.ppm"
    local shell_log="$WORK_DIR/$name-shell.log"
    local game_log="$WORK_DIR/$name-game.log"

    XDG_CONFIG_HOME="$WORK_DIR/$name/config" \
    XDG_DATA_HOME="$WORK_DIR/$name/data" \
    SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
    "$XVFB_RUN" -a -s "-screen 0 1600x1000x24" \
        "$DUALSCREEN" \
            --game "$WORK_DIR/game.wud" \
            --external-socket "$socket" \
            --windowed \
            --exit-after-seconds 12 \
            --screenshot-at-seconds 8 \
            --screenshot "$shot" \
            "$@" \
        > "$shell_log" 2>&1 &
    SHELL_PID=$!

    # The emulator starts second on purpose: the shell has to cope with opening
    # a game that is not there yet, which is what starting Cemu separately
    # always looks like.
    sleep 3
    "$FAKE_WII_U" --socket "$socket" --seconds 7 > "$game_log" 2>&1
    local game_result=$?

    wait "$SHELL_PID"
    local shell_result=$?
    SHELL_PID=""

    echo "--- $name: the shell ---"
    cat "$shell_log" 2>/dev/null
    echo "--- $name: the game ---"
    cat "$game_log" 2>/dev/null

    if [ "$shell_result" -ne 0 ]; then
        echo "$name: the shell exited with $shell_result" >&2
        FAILED=1
    fi
    if [ "$game_result" -ne 0 ]; then
        echo "$name: the game exited with $game_result" >&2
        FAILED=1
    fi

    # Premises first, so a run that proved nothing says so rather than passing.
    if ! grep -q "a game connected" "$shell_log" 2>/dev/null; then
        echo "$name: nothing ever connected over the frame link, so this run says nothing" >&2
        FAILED=1
    fi
    if ! grep -q "sending 1280x720 and 854x480" "$shell_log" 2>/dev/null; then
        echo "$name: the shell did not take the two screen sizes the game announced" >&2
        FAILED=1
    fi
    if ! grep -q "compositing a screen" "$shell_log" 2>/dev/null; then
        echo "$name: the shell never drew a screen, so nothing reached the compositor" >&2
        FAILED=1
    fi

    if [ ! -f "$shot" ]; then
        echo "$name: no screenshot" >&2
        FAILED=1
    elif ! "$CHECKER" "$shot" "center=$expect"; then
        echo "$name: the shell was not showing the $expect screen" >&2
        FAILED=1
    fi
}

# The television, which is where a Wii U starts: it is the game, and the
# GamePad is a second view of it.
run_session television blue

# F3 is the swap key. A Wii U game reads both stick presses itself, so this is
# the one console where the shortcut cannot live on the pad -- see
# SystemProfile::game_uses_every_control.
#
# With a finger held on it, because a GamePad is a touch screen and the whole
# point of showing it here is being able to use it.
run_session gamepad green --simulate-key F3 --simulate-touch 0.5,0.5

# And the touch reached the program playing the game, in that screen's own
# pixels rather than in this window's -- the middle of 854x480. Which is the
# whole input path: a finger on the glass here, through the shell's own touch
# mapping, across the link, to the emulator.
if ! grep -q "touch(427,240)" "$WORK_DIR/gamepad-game.log" 2>/dev/null; then
    echo "the touch did not reach the game at the middle of its own 854x480 screen" >&2
    FAILED=1
fi

# And with nothing on the other end at all. The emulator is a separate program
# and may simply not have been started, and a blank window is indistinguishable
# from a broken one -- so the shell has to say what it is waiting for.
{
    shot="$WORK_DIR/waiting.ppm"
    log="$WORK_DIR/waiting.log"
    XDG_CONFIG_HOME="$WORK_DIR/waiting/config" \
    XDG_DATA_HOME="$WORK_DIR/waiting/data" \
    SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
    "$XVFB_RUN" -a -s "-screen 0 1600x1000x24" \
        "$DUALSCREEN" \
            --game "$WORK_DIR/game.wud" \
            --external-socket "$WORK_DIR/nobody.sock" \
            --windowed \
            --exit-after-seconds 5 \
            --screenshot-at-seconds 3 \
            --screenshot "$shot" \
        > "$log" 2>&1
    result=$?

    echo "--- waiting: the shell ---"
    cat "$log" 2>/dev/null

    if [ "$result" -ne 0 ]; then
        echo "waiting: the shell exited with $result rather than sitting there" >&2
        FAILED=1
    fi
    if grep -q "a game connected" "$log" 2>/dev/null; then
        echo "waiting: something connected, so this run is not the case being tested" >&2
        FAILED=1
    fi
    if [ ! -f "$shot" ]; then
        echo "waiting: no screenshot" >&2
        FAILED=1
    elif ! "$CHECKER" "$shot" "text=0.1,0.35,0.9,0.60"; then
        echo "waiting: the shell showed a blank window rather than saying what it was waiting "\
"for" >&2
        FAILED=1
    fi
}

if [ "$FAILED" -eq 0 ]; then
    echo "ok - a console played by another program showed both of its screens"
fi
exit "$FAILED"
