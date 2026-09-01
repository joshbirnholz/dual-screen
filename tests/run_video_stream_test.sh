#!/usr/bin/env bash
#
# A whole session with the screens going as H.264 rather than as tiles: two real
# processes, the real handshake, the real encoder and the real decoder.
#
# There is no console yet whose screens need this -- see docs/research-notes.md
# for the measurement that says a Wii U GamePad will -- so the host is told to
# use it. That is the point: the wire path can be finished and held to before the
# thing it exists for arrives, and the same flag lets it be felt on real hardware.
#
# What the run proves is that the two ends agreed on video and that a picture
# came out the far side. The companion draws the screen it was granted, and the
# mock core paints the two screens different colours, so the colour on screen is
# the frame having survived encode, wire, decode and upload.
#
# ASKED_BY says how the host was told to use video: "flag" for --video-codec, or
# "setting" for the DS game's own "screen_codec_ds" entry in settings.json. The
# two have to reach the same place -- the flag exists so a run can differ from
# the setting without editing a file, and a setting nobody could act on would
# be worse than none.
#
# Usage: run_video_stream_test.sh <xvfb-run> <dualscreen> <mock core> <work dir> <checker>

set -uo pipefail

XVFB_RUN="$1"
DUALSCREEN="$2"
MOCK_CORE="$3"
WORK_DIR="$4"
CHECKER="$5"

ASKED_BY="${ASKED_BY:-flag}"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/host/config/dualscreen" "$WORK_DIR/companion/config/dualscreen"
printf 'dualscreen video stream test rom' > "$WORK_DIR/fake.nds"

CODEC_ARGUMENTS=()
if [ "$ASKED_BY" = "setting" ]; then
    printf '{"screen_codec_ds": "h264"}' > "$WORK_DIR/host/config/dualscreen/settings.json"
else
    CODEC_ARGUMENTS=(--video-codec h264)
fi

COMPANION_SHOT="$WORK_DIR/companion.ppm"

cleanup() {
    [ -n "${HOST_PID:-}" ] && kill "$HOST_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

XDG_CONFIG_HOME="$WORK_DIR/host/config" \
XDG_DATA_HOME="$WORK_DIR/host/data" \
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
"$XVFB_RUN" -a -s "-screen 0 1600x1000x24" \
    "$DUALSCREEN" \
        --game "$WORK_DIR/fake.nds" \
        --core "$MOCK_CORE" \
        --layout upper_on_tv \
        "${CODEC_ARGUMENTS[@]}" \
        --windowed \
        --exit-after-seconds 16 \
    > "$WORK_DIR/host.log" 2>&1 &
HOST_PID=$!

sleep 2

XDG_CONFIG_HOME="$WORK_DIR/companion/config" \
XDG_DATA_HOME="$WORK_DIR/companion/data" \
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
"$XVFB_RUN" -a -s "-screen 0 1600x1000x24" \
    "$DUALSCREEN" \
        --connect bottom \
        --core "$MOCK_CORE" \
        --windowed \
        --exit-after-seconds 10 \
        --screenshot-at-seconds 7 \
        --screenshot "$COMPANION_SHOT" \
    > "$WORK_DIR/companion.log" 2>&1
COMPANION_RESULT=$?

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

# Premises first, so a run that proved nothing says so rather than passing.
if ! grep -q "connected as the" "$WORK_DIR/host.log" 2>/dev/null; then
    echo "the host never accepted a companion, so nothing here was tested" >&2
    FAILED=1
fi
if ! grep -q "that device can decode video" "$WORK_DIR/host.log" 2>/dev/null; then
    echo "the companion did not offer to decode video, so the session fell back to tiles and "\
"this run says nothing about the video path" >&2
    FAILED=1
fi
if ! grep -q "video encoder: " "$WORK_DIR/host.log" 2>/dev/null; then
    echo "the host never opened a video encoder" >&2
    FAILED=1
fi
if ! grep -q "video decoder: " "$WORK_DIR/companion.log" 2>/dev/null; then
    echo "the companion never opened a video decoder, so whatever it drew did not come through "\
"the video path" >&2
    FAILED=1
fi
# There is no fallback to tiles any more -- see ScreenCodec's own comment --
# so a video failure here would otherwise only show up as a screenshot that
# never got its picture, which says nothing about why.
if grep -q "video streaming is not working" "$WORK_DIR/host.log" 2>/dev/null; then
    echo "the host gave up on video instead of sending it" >&2
    FAILED=1
fi

if [ ! -f "$COMPANION_SHOT" ]; then
    echo "no screenshot from the companion" >&2
    FAILED=1
else
    echo "--- companion ---"
    # Under the split layout the companion fills itself with the screen it owns,
    # and the mock core paints the bottom one green. Lossy compression moves the
    # exact value, which is what the checker's tolerance is for.
    if ! "$CHECKER" "$COMPANION_SHOT" "center=green"; then
        echo "the companion did not show the screen it owns, so the picture did not survive the "\
"round trip" >&2
        FAILED=1
    fi
fi

if [ "$FAILED" -eq 0 ]; then
    echo "ok - a session ran with its screens going as video, asked for by $ASKED_BY"
fi
exit "$FAILED"
