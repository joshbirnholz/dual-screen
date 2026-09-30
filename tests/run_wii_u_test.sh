#!/usr/bin/env bash
#
# A console this app does not play itself: two real processes, the real shared
# memory, and a picture on screen that came out of the other one.
#
# fake_wii_u stands in for Cemu -- see tools/fake_wii_u.cpp. It publishes a blue
# television screen and a green GamePad screen over the frame link, but only
# the one this app actually asks for over ExternalControl::RequestScreen (see
# ShouldCapture on the real Cemu fork's side of the same idea) -- and alone,
# with nobody else connected, this app asks for nothing at all: Cemu's own
# window is the television now, which nothing here can screenshot, and there
# is no second device yet for either screen to go to. So what a solo run
# proves is the opposite of what it used to: that nothing is requested and
# nothing is composited, which is what actually removes the cost a real
# Cemu would otherwise pay for a screen nobody is watching through this app.
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

# $1 run name, $2... extra dualscreen arguments
run_session() {
    local name="$1"
    shift 1

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
    # There used to be a check here that the shell "took the two screen sizes
    # the game announced". It could not have caught anything: those sizes are
    # declared before the game has drawn a pixel, nothing clamps a frame to
    # them, and a title rendering its television above them publishes above
    # them. The number that means something is the size a frame arrives at --
    # and in this scenario nobody is watching either screen, so deliberately
    # no frame ever arrives. It is checked where frames actually flow, in
    # run_wii_u_companion_test.sh.

    # Alone, with nothing asked for, neither screen should ever reach the
    # compositor -- see the top-of-file note on what a solo run proves now.
    if grep -q "compositing a screen" "$shell_log" 2>/dev/null; then
        echo "$name: the shell composited a screen with nobody connected to watch it" >&2
        FAILED=1
    fi

    if [ ! -f "$shot" ]; then
        echo "$name: no screenshot" >&2
        FAILED=1
    # What is on screen is the Wii U menu, shown in place of a "Playing on
    # Cemu" notice -- its rows, Settings and Quit, sit in this band, under the
    # heading every page has.
    elif ! "$CHECKER" "$shot" "text=0.06,0.21,0.4,0.32"; then
        echo "$name: the shell showed something other than its menu beside the game with "\
"nothing connected" >&2
        FAILED=1
    fi
}

# Nobody is watching either screen through this app, so neither should ever be
# requested from the far end or drawn here -- Cemu's own window is the
# television now, and there is no second device for the GamePad to go to.
run_session solo

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
    elif ! "$CHECKER" "$shot" "text=0.06,0.05,0.9,0.30"; then
        echo "waiting: the shell showed a blank window rather than saying what it was waiting "\
"for" >&2
        FAILED=1
    fi
}

# And the whole session from one command. In Game Mode there is no desktop to go
# and start an emulator from, which is the entire reason this app exists -- so
# the shell starts it, from a command the player configured. This is the
# arrangement a Steam shortcut actually gets.
{
    shot="$WORK_DIR/launched.ppm"
    log="$WORK_DIR/launched.log"
    socket="$WORK_DIR/launched.sock"
    mkdir -p "$WORK_DIR/launched/config/dualscreen"
    # Nothing here knows what a fork of Cemu will call its own flags, so the
    # command is written out in full and the shell fills in the two things only
    # it knows.
    cat > "$WORK_DIR/launched/config/dualscreen/settings.json" <<JSON
{"wii_u_program": "$FAKE_WII_U",
 "wii_u_arguments": ["--socket", "{socket}", "--seconds", "300"]}
JSON

    XDG_CONFIG_HOME="$WORK_DIR/launched/config" \
    XDG_DATA_HOME="$WORK_DIR/launched/data" \
    SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
    "$XVFB_RUN" -a -s "-screen 0 1600x1000x24" \
        "$DUALSCREEN" \
            --game "$WORK_DIR/game.wud" \
            --external-socket "$socket" \
            --windowed \
            --exit-after-seconds 8 \
            --screenshot-at-seconds 5 \
            --screenshot "$shot" \
        > "$log" 2>&1
    result=$?

    echo "--- launched: the shell ---"
    cat "$log" 2>/dev/null

    if [ "$result" -ne 0 ]; then
        echo "launched: the shell exited with $result" >&2
        FAILED=1
    fi
    if ! grep -q "a game connected" "$log" 2>/dev/null; then
        echo "launched: the shell started no emulator, or the one it started never connected" >&2
        FAILED=1
    fi
    # Same as every other solo run in this file: alone, nothing is requested
    # or composited, so what proves the emulator actually connected is the
    # log line above, not a screenshot of a screen this app no longer draws.
    if grep -q "compositing a screen" "$log" 2>/dev/null; then
        echo "launched: the shell composited a screen with nobody connected to watch it" >&2
        FAILED=1
    fi
    if [ ! -f "$shot" ]; then
        echo "launched: no screenshot" >&2
        FAILED=1
    fi

}

# Stopping it again is as much the point as starting it: the shell quitting has
# to take the emulator with it, or a player in Game Mode is left looking at a
# window with nothing left running that could close it.
#
# Checked with a program that has no opinion rather than with fake_wii_u, which
# notices the link go and exits on its own -- so it would look stopped whether
# or not anything stopped it.
{
    log="$WORK_DIR/stopped.log"
    mkdir -p "$WORK_DIR/stopped/config/dualscreen"
    cat > "$WORK_DIR/stopped/config/dualscreen/settings.json" <<'JSON'
{"wii_u_program": "/bin/sleep", "wii_u_arguments": ["1471"]}
JSON

    XDG_CONFIG_HOME="$WORK_DIR/stopped/config" \
    XDG_DATA_HOME="$WORK_DIR/stopped/data" \
    SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
    "$XVFB_RUN" -a -s "-screen 0 1600x1000x24" \
        "$DUALSCREEN" \
            --game "$WORK_DIR/game.wud" \
            --external-socket "$WORK_DIR/stopped.sock" \
            --windowed \
            --exit-after-seconds 4 \
        > "$log" 2>&1

    if ! grep -q "started /bin/sleep" "$log" 2>/dev/null; then
        echo "stopped: the shell never started the configured program, so this run says nothing" >&2
        FAILED=1
    fi
    # Matched on the program and its argument rather than by pattern, because
    # a pattern that says "sleep 1471" also matches any shell whose command
    # line happens to mention it -- including the one running this script.
    stray=$(ps -eo pid=,args= | awk '$2 == "/bin/sleep" && $3 == "1471" { print $1 }')
    if [ -n "$stray" ]; then
        echo "stopped: the program the shell started outlived the shell" >&2
        kill $stray 2>/dev/null
        FAILED=1
    fi
}

if [ "$FAILED" -eq 0 ]; then
    echo "ok - a console played by another program showed both of its screens"
fi
exit "$FAILED"
