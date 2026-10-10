#!/usr/bin/env bash
#
# The settings screen, reached from the quick menu and worked from either pad.
#
# Unlike the menu it is not mirrored: it is about the machine running the game
# -- where its BIOS files are, where its emulators are -- and half of that is
# nothing the second device could act on. So the host draws it, the companion is
# told only that it is up and says where to look, and the presses still cross.
#
# The host opens it with F4, which is the keyboard's name for the - button on a
# pad; the companion closes it again with Escape, which is Cancel. That the host
# goes back to the menu it came from is the proof the press arrived, since
# nothing else in this run could have sent it back there.
#
# Usage: run_host_settings_test.sh <xvfb-run> <dualscreen> <mock core> <work dir> <checker>

set -uo pipefail

XVFB_RUN="$1"
DUALSCREEN="$2"
MOCK_CORE="$3"
WORK_DIR="$4"
CHECKER="$5"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/host/config" "$WORK_DIR/companion/config"
printf 'dualscreen host settings test rom' > "$WORK_DIR/fake.nds"

COMPANION_SHOT="$WORK_DIR/companion.ppm"

cleanup() {
    [ -n "${HOST_PID:-}" ] && kill "$HOST_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

XDG_CONFIG_HOME="$WORK_DIR/host/config" \
XDG_DATA_HOME="$WORK_DIR/host/data" \
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
"$XVFB_RUN" -a -s "-screen 0 1280x800x24" \
    "$DUALSCREEN" \
        --game "$WORK_DIR/fake.nds" \
        --core "$MOCK_CORE" \
        --layout upper_on_tv \
        --start-screen layout \
        --simulate-key F4 \
        --windowed \
        --exit-after-seconds 14 \
    > "$WORK_DIR/host.log" 2>&1 &
HOST_PID=$!

sleep 3

# Its screenshot is taken while the host's settings are up, and its Escape is
# sent once it has actually joined -- both of which need the host to have got
# there first, which is why it starts second.
XDG_CONFIG_HOME="$WORK_DIR/companion/config" \
XDG_DATA_HOME="$WORK_DIR/companion/data" \
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
"$XVFB_RUN" -a -s "-screen 0 1280x800x24" \
    "$DUALSCREEN" \
        --connect bottom \
        --windowed \
        --exit-after-seconds 6 \
        --screenshot-at-seconds 4 \
        --screenshot "$COMPANION_SHOT" \
    > "$WORK_DIR/companion.log" 2>&1
LOOKING_RESULT=$?

# A second one, whose only job is to send the press. Separate because a run can
# take one screenshot, and what the first proves -- that this device says where
# to look -- is not what the second proves.
XDG_CONFIG_HOME="$WORK_DIR/companion/config" \
XDG_DATA_HOME="$WORK_DIR/companion/data" \
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
"$XVFB_RUN" -a -s "-screen 0 1280x800x24" \
    "$DUALSCREEN" \
        --connect bottom \
        --simulate-key Escape \
        --windowed \
        --exit-after-seconds 5 \
    > "$WORK_DIR/pressing.log" 2>&1
PRESSING_RESULT=$?

wait "$HOST_PID"
HOST_RESULT=$?
HOST_PID=""

echo "--- the host ---"
cat "$WORK_DIR/host.log" 2>/dev/null
echo "--- the companion that looked ---"
cat "$WORK_DIR/companion.log" 2>/dev/null
echo "--- the companion that pressed ---"
cat "$WORK_DIR/pressing.log" 2>/dev/null

FAILED=0
for result in "$HOST_RESULT" "$LOOKING_RESULT" "$PRESSING_RESULT"; do
    if [ "$result" -ne 0 ]; then
        echo "one of the three runs exited with $result" >&2
        FAILED=1
    fi
done

# Premises first, so a run that proved nothing says so rather than passing.
if ! grep -q "the quick menu is up" "$WORK_DIR/host.log" 2>/dev/null; then
    echo "the host never opened its menu, so - was never pressed from one" >&2
    FAILED=1
fi
if ! grep -q "connected: showing the" "$WORK_DIR/pressing.log" 2>/dev/null; then
    echo "the second device never joined, so its press went nowhere" >&2
    FAILED=1
fi

# - from the menu put the settings up.
if ! grep -q "the settings screen is up" "$WORK_DIR/host.log" 2>/dev/null; then
    echo "pressing - in the quick menu did not open the settings" >&2
    FAILED=1
fi

# The companion said where to look rather than showing a frozen game: one line,
# in the middle of the screen.
if [ ! -f "$COMPANION_SHOT" ]; then
    echo "no screenshot from the companion" >&2
    FAILED=1
elif ! "$CHECKER" "$COMPANION_SHOT" "text=0.25,0.44,0.75,0.56"; then
    echo "the companion did not say where to look while the host's settings were up" >&2
    FAILED=1
fi

# And the far pad worked the screen it was not drawing: the host is back in the
# menu it came from, which only that press could have done.
if [ "$(grep -c "the quick menu is up" "$WORK_DIR/host.log" 2>/dev/null)" -lt 2 ]; then
    echo "the companion's press never reached the host's settings, so a device that cannot see "\
"that screen also cannot work it" >&2
    FAILED=1
fi

if [ "$FAILED" -eq 0 ]; then
    echo "ok - the host's settings open from the menu and answer either pad"
fi
exit "$FAILED"
