#!/usr/bin/env python3
# Sample Grabber - https://github.com/mythenstudios/sample-grabber
# Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

# Synthetic test loop for the CI self tests (no recorded audio in the repo):
# 124 BPM, kick + hi-hat + a two-chord pad, stereo, 44.1 kHz, 16 bit.
# Usage: make-test-wav.py <out.wav> [seconds]
import array
import math
import random
import sys
import wave

RATE = 44100
BPM = 124.0


def main():
    out = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
    n = int(seconds * RATE)
    beat = 60.0 / BPM
    rng = random.Random(124)
    chords = [(220.0, 277.18, 329.63), (196.0, 246.94, 293.66)]   # A, G
    data = array.array("h")
    for i in range(n):
        t = i / RATE
        pos = t % beat
        # Kick on every beat: falling sine, short decay
        kick = math.sin(2 * math.pi * (50 + 90 * math.exp(-pos * 30)) * pos) * math.exp(-pos * 9)
        # Hi-hat on the off-beat: noise burst
        off = (t + beat / 2) % beat
        hat = (rng.random() * 2 - 1) * math.exp(-off * 60) * 0.25
        # Pad: chord changes every bar
        bar = int(t / (4 * beat)) % 2
        pad = sum(math.sin(2 * math.pi * f * t) for f in chords[bar]) * 0.08
        left = 0.7 * kick + hat * 0.6 + pad
        right = 0.7 * kick + hat * 1.0 + pad * 0.9
        data.append(int(max(-1.0, min(1.0, left)) * 32000))
        data.append(int(max(-1.0, min(1.0, right)) * 32000))
    if sys.byteorder == "big":
        data.byteswap()
    with wave.open(out, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(data.tobytes())


if __name__ == "__main__":
    main()
