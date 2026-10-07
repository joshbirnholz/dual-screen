#!/usr/bin/env python3
"""Regenerates assets/ui_sounds.bin: the shell's own sounds, in one blob.

The sources are assets/ui_sounds/*.wav, as the owner gave them: a mix of 44.1
and 48 kHz, mono and stereo, most of them a second long with the sound itself
in the first few hundredths and silence after. Played as they are, that
silence is a voice held for nothing, and a rate the device is not running at
is a conversion on every press. So each is made what the player wants, once,
here:

  - 48 kHz, which every device this runs on plays at (a windowed-sinc
    resampler, for the ones at 44.1);
  - stereo, a mono one copied to both sides;
  - trimmed to the sound: from the first 10 ms above -50 dB to where it has
    died below -60 dB and stayed there for 50 ms -- several end on a click
    after near-silence, which is not part of the sound -- with 30 ms kept
    after, faded out over the last 20 ms and in over the first millisecond
    where anything was cut, so neither end clicks.

The blob, little-endian throughout -- see decode_ui_sound_pack():

    "DSUS", version (u32, 1), rate (u32), count (u32)
    count times: name length (u8), name (ASCII), frames (u32)
    the frames, int16 stereo interleaved, one sound after another

The names are the files' stems; the player looks them up by name, so the
order here is not something it relies on.

Run from the repository root:  python3 assets/make_ui_sounds.py
"""

import math
import os
import struct
import sys
import wave

RATE = 48000
SOUNDS = ["move_cursor", "confirm", "cancel", "error", "open_quick_menu"]
THRESHOLD = 10 ** (-50 / 20)
THRESHOLD_QUIET = 10 ** (-60 / 20)
KEEP_AFTER = 0.030
FADE_OUT = 0.020
FADE_IN = 0.001
TAPS = 32  # each side


def read_wav(path):
    with wave.open(path, "rb") as w:
        channels, width, rate, count = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(count)
    if width != 2:
        sys.exit(f"{path}: only 16-bit PCM is read here")
    values = struct.unpack("<%dh" % (len(raw) // 2), raw)
    left = [values[i] / 32768.0 for i in range(0, len(values), channels)]
    right = [values[i + (1 if channels > 1 else 0)] / 32768.0 for i in range(0, len(values), channels)]
    return rate, [left, right]


def resample(samples, source, target):
    if source == target:
        return samples
    ratio = target / source
    cutoff = min(1.0, ratio) * 0.95
    length = int(len(samples) * ratio)
    out = []
    for n in range(length):
        position = n / ratio
        centre = int(math.floor(position))
        total = 0.0
        for k in range(centre - TAPS + 1, centre + TAPS + 1):
            if k < 0 or k >= len(samples):
                continue
            x = position - k
            sinc = 1.0 if x == 0 else math.sin(math.pi * cutoff * x) / (math.pi * cutoff * x)
            window = 0.42 + 0.5 * math.cos(math.pi * x / TAPS) + 0.08 * math.cos(2 * math.pi * x / TAPS)
            if abs(x) >= TAPS:
                window = 0.0
            total += samples[k] * cutoff * sinc * window
        out.append(total)
    return out


def trim(channels):
    frames = len(channels[0])
    window = int(RATE * 0.010)
    levels = []
    for start in range(0, frames, window):
        total = 0.0
        count = 0
        for c in channels:
            for x in c[start:start + window]:
                total += x * x
                count += 1
        levels.append(math.sqrt(total / max(1, count)))
    loud = THRESHOLD
    first_window = next((i for i, level in enumerate(levels) if level > loud), 0)
    # The end is where the sound has died away and stays away for 50 ms --
    # not the last sample above the threshold: several of these end on a
    # click after a stretch of near-silence, which is not part of the sound.
    peak = max(range(len(levels)), key=lambda i: levels[i])
    quiet = THRESHOLD_QUIET
    end_window = len(levels)
    for i in range(peak, len(levels)):
        if all(level < quiet for level in levels[i:i + 5]):
            end_window = i
            break
    start = max(0, first_window * window - int(RATE * 0.001))
    end = min(frames, end_window * window + int(RATE * KEEP_AFTER))
    out = [c[start:end] for c in channels]
    length = len(out[0])
    fade_out = min(length, int(RATE * FADE_OUT))
    for i in range(fade_out):
        gain = 0.5 * (1 + math.cos(math.pi * (i + 1) / fade_out))
        for c in out:
            c[length - fade_out + i] *= gain
    if start > 0:
        fade_in = min(length, int(RATE * FADE_IN))
        for i in range(fade_in):
            gain = 0.5 * (1 - math.cos(math.pi * i / fade_in))
            for c in out:
                c[i] *= gain
    return out


def main():
    root = os.path.dirname(os.path.abspath(__file__))
    header = bytearray(b"DSUS")
    header += struct.pack("<III", 1, RATE, len(SOUNDS))
    body = bytearray()
    for name in SOUNDS:
        rate, channels = read_wav(os.path.join(root, "ui_sounds", name + ".wav"))
        channels = [resample(c, rate, RATE) for c in channels]
        channels = trim(channels)
        frames = len(channels[0])
        header += struct.pack("<B", len(name)) + name.encode("ascii") + struct.pack("<I", frames)
        for i in range(frames):
            for c in channels:
                body += struct.pack("<h", max(-32768, min(32767, int(round(c[i] * 32767)))))
        print(f"{name}: {frames / RATE * 1000:.0f} ms")
    with open(os.path.join(root, "ui_sounds.bin"), "wb") as out:
        out.write(header + body)


if __name__ == "__main__":
    main()
