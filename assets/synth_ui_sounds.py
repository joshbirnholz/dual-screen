#!/usr/bin/env python3
"""Synthesises the menus' sounds: assets/ui_sounds/click/*.wav.

The sounds the shell plays -- move_cursor, confirm, cancel, error,
open_quick_menu -- made here from sine waves and noise, so they are this
project's own and carry nobody else's licence. They aim at the feel of a
console's system menus -- dry plastic ticks and pops, short, quiet, tactile,
pitched a little up for yes and down for no -- without copying any.

Deterministic -- the noise is seeded -- so running it again gives the same
files. Then run assets/make_ui_sounds.py to pack them into ui_sounds.bin.

Run from the repository root:  python3 assets/synth_ui_sounds.py
"""

import math
import os
import random
import struct
import wave

RATE = 48000


def silence(seconds):
    return [0.0] * int(seconds * RATE)


def mix_into(target, source, at_seconds, gain=1.0):
    start = int(at_seconds * RATE)
    if len(target) < start + len(source):
        target.extend([0.0] * (start + len(source) - len(target)))
    for i, v in enumerate(source):
        target[start + i] += v * gain
    return target


def tone(f0, f1, seconds, tau, glide=0.02, attack=0.001, partials=((1.0, 1.0),)):
    """A pitched sound gliding from f0 to f1 over `glide` seconds, dying away
    with time constant `tau`; `partials` are (frequency ratio, level)."""
    out = []
    phases = [0.0] * len(partials)
    n = int(seconds * RATE)
    for i in range(n):
        t = i / RATE
        k = min(1.0, t / glide) if glide > 0 else 1.0
        # Glides in pitch, not frequency: exponential between the two.
        f = f0 * (f1 / f0) ** (1 - (1 - k) ** 2)
        env = (min(1.0, t / attack) if attack > 0 else 1.0) * math.exp(-t / tau)
        v = 0.0
        for p, (ratio, level) in enumerate(partials):
            phases[p] += 2 * math.pi * f * ratio / RATE
            v += math.sin(phases[p]) * level
        out.append(v * env)
    return out


def mallet(f, seconds, tau, level=1.0):
    """A struck bar: a fundamental and the 4x and 10x partials a marimba has,
    each dying faster than the one below it."""
    out = []
    for i in range(int(seconds * RATE)):
        t = i / RATE
        attack = min(1.0, t / 0.0015)
        v = (math.sin(2 * math.pi * f * t) * math.exp(-t / tau)
             + 0.28 * math.sin(2 * math.pi * f * 3.93 * t) * math.exp(-t / (tau * 0.25))
             + 0.06 * math.sin(2 * math.pi * f * 9.9 * t) * math.exp(-t / (tau * 0.08)))
        out.append(v * attack * level)
    return out


def noise(seconds, tau, low_cut, high_cut, seed):
    """A burst of band-limited noise: the tick at the front of a click."""
    rng = random.Random(seed)
    out = []
    lp = hp_in = hp_out = 0.0
    a_lp = 1 - math.exp(-2 * math.pi * high_cut / RATE)
    a_hp = math.exp(-2 * math.pi * low_cut / RATE)
    for i in range(int(seconds * RATE)):
        t = i / RATE
        x = rng.uniform(-1, 1)
        lp += a_lp * (x - lp)
        hp_out = a_hp * (hp_out + lp - hp_in)
        hp_in = lp
        out.append(hp_out * math.exp(-t / tau))
    return out


def soften(samples, cutoff):
    """One-pole low-pass: takes the edge off a tone with harmonics."""
    a = 1 - math.exp(-2 * math.pi * cutoff / RATE)
    out, y = [], 0.0
    for x in samples:
        y += a * (x - y)
        out.append(y)
    return out


def normalise(samples, peak_db):
    peak = max(1e-9, max(abs(v) for v in samples))
    gain = 10 ** (peak_db / 20) / peak
    return [v * gain for v in samples]


def stereo(samples, width_ms=0.25):
    """A hair of width: the right side a fraction of a millisecond late."""
    delay = int(width_ms / 1000 * RATE)
    right = [0.0] * delay + samples[: len(samples) - delay]
    return samples, right


def write(path, samples, peak_db):
    samples = normalise(samples + silence(0.03), peak_db)
    left, right = stereo(samples)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        frames = bytearray()
        for l, r in zip(left, right):
            frames += struct.pack("<hh", int(max(-1, min(1, l)) * 32767), int(max(-1, min(1, r)) * 32767))
        w.writeframes(bytes(frames))


# --- click: dry ticks and pops ----------------------------------------------

def click_style():
    s = {}
    move = mix_into(noise(0.012, 0.0012, 1800, 9000, 1), tone(2600, 2600, 0.02, 0.003), 0, 0.5)
    mix_into(move, tone(1100, 1100, 0.02, 0.004), 0, 0.35)
    s["move_cursor"] = (move, -14)

    confirm = noise(0.012, 0.0015, 1500, 8000, 2)
    mix_into(confirm, tone(820, 1250, 0.09, 0.022, glide=0.018, partials=((1, 1), (2, 0.18))), 0, 0.9)
    mix_into(confirm, tone(2500, 2500, 0.03, 0.005), 0.0, 0.25)
    s["confirm"] = (confirm, -7)

    cancel = noise(0.01, 0.0012, 1200, 6000, 3)
    mix_into(cancel, tone(1050, 640, 0.08, 0.02, glide=0.025, partials=((1, 1), (2, 0.12))), 0, 0.9)
    s["cancel"] = (cancel, -9)

    error = []
    for at in (0.0, 0.085):
        bonk = soften(tone(190, 170, 0.08, 0.028, glide=0.05,
                           partials=((1, 1), (3, 0.35), (5, 0.18))), 1400)
        mix_into(error, bonk, at)
        mix_into(error, noise(0.01, 0.0015, 300, 3000, 4), at, 0.4)
    s["error"] = (error, -8)

    opening = soften(noise(0.16, 0.06, 1500, 9000, 5), 6000)
    opening = [v * min(1.0, i / (0.06 * RATE)) for i, v in enumerate(opening)]
    for at, f in ((0.0, 880), (0.045, 1175), (0.09, 1760)):
        mix_into(opening, tone(f * 0.94, f, 0.16, 0.05, glide=0.012, partials=((1, 1), (2, 0.1))), at, 0.7)
    s["open_quick_menu"] = (opening, -8)
    return s


STYLES = {"click": click_style}


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ui_sounds")
    for style, make in STYLES.items():
        for name, (samples, peak_db) in make().items():
            write(os.path.join(root, style, name + ".wav"), samples, peak_db)
        print("wrote", style)


if __name__ == "__main__":
    main()
