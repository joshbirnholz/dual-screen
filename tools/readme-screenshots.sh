#!/usr/bin/env bash
#
# Renders the screenshots the README and docs/guide.md show, from the app as
# it is now, into docs/images/. Run it after a change to anything those
# pictures show, so they never drift from the app.
#
# Needs a desktop build (cmake -G Ninja -B build && cmake --build build),
# xvfb-run, python3 and ImageMagick's convert. Everything runs offscreen with
# the mock cores from tests/, so no emulator, ROM or display is needed: the
# "game" in the pictures is the mock core's test pattern, named Example Game.
#
# Usage: tools/readme-screenshots.sh [build dir]

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(cd "${1:-$ROOT/build}" && pwd)"
APP="$BUILD/dualscreen"
MOCK_DS="$BUILD/tests/mock_ds_libretro.so"
FAKE_WII_U="$BUILD/fake_wii_u"
OUT="$ROOT/docs/images"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

for need in "$APP" "$MOCK_DS" "$FAKE_WII_U"; do
    [ -e "$need" ] || { echo "missing $need: build the desktop app first" >&2; exit 1; }
done
mkdir -p "$OUT"

python3 "$ROOT/tests/make_test_nds.py" "$WORK/Example Game.nds"
printf 'x' > "$WORK/Example Wii U Game.wud"

# A pause for the app to settle between simulated presses: keys go every six
# frames, so a run of a key nothing listens to is a wait.
WAIT="$(python3 -c "print(','.join(['F15'] * 40))")"

# shot <name> <seconds> <keys> -- <dualscreen arguments...>
shot() {
    local name="$1" seconds="$2" keys="$3"
    shift 4
    local home="$WORK/home-$name"
    mkdir -p "$home"
    local keys_args=()
    [ -n "$keys" ] && keys_args=(--simulate-key "$keys")
    HOME="$home" SDL_AUDIODRIVER=dummy \
        xvfb-run -a -s "-screen 0 1280x800x24" "$APP" "${keys_args[@]}" \
            --exit-after-seconds $((seconds + 1)) --screenshot-at-seconds "$seconds" \
            --screenshot "$WORK/$name.ppm" "$@" > "$WORK/$name.log" 2>&1
    convert "$WORK/$name.ppm" -strip "$OUT/$name.png"
    echo "wrote docs/images/$name.png"
}

GAME=(--core "$MOCK_DS" --game "$WORK/Example Game.nds")

shot home 4 "" --
shot quick-menu-layouts 8 "$WAIT,F1,$WAIT,Down,$WAIT" -- "${GAME[@]}"
shot quick-menu-options 8 "$WAIT,F1,$WAIT,Down,Down,Down,Down,Down,$WAIT" -- "${GAME[@]}"
shot save-states 9 "$WAIT,F1,$WAIT,Down,Down,Right,$WAIT,Return,$WAIT,Return,$WAIT" -- "${GAME[@]}"
shot change-controls 8 "$WAIT,F1,$WAIT,Down,Down,Down,Down,Down,Down,Right,Return,$WAIT" -- "${GAME[@]}"
shot settings 5 "Down,Down" -- --start-screen settings

# A Wii U game: fake_wii_u stands in for Cemu, started a moment after the app
# as a real Cemu would be.
home="$WORK/home-wii-u"
mkdir -p "$home"
HOME="$home" SDL_AUDIODRIVER=dummy \
    xvfb-run -a -s "-screen 0 1280x800x24" "$APP" \
        --game "$WORK/Example Wii U Game.wud" --external-socket "$WORK/wii-u.sock" \
        --exit-after-seconds 13 --screenshot-at-seconds 11 \
        --screenshot "$WORK/wii-u-menu.ppm" > "$WORK/wii-u-menu.log" 2>&1 &
app_pid=$!
sleep 3
"$FAKE_WII_U" --socket "$WORK/wii-u.sock" --seconds 10 > "$WORK/fake_wii_u.log" 2>&1 || true
wait "$app_pid" || true
convert "$WORK/wii-u-menu.ppm" -strip "$OUT/wii-u-menu.png"
echo "wrote docs/images/wii-u-menu.png"
