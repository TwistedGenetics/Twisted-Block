#!/usr/bin/env python3
"""Writes plugin/Content/Samples/test_break.wav: a synthetic 2-bar break at 170 BPM.

Generated from scratch (no third-party audio), mono, 16-bit, 44.1 kHz.
Used by the Phase 0 proof of concept to draw the waveform overview.
"""
import math
import os
import random
import struct
import sys
import wave

SR = 44100
BPM = 170.0
STEP = 60.0 / BPM / 4.0          # one 16th note in seconds
STEPS = 32                       # 2 bars

# Classic two-step-ish break: K = kick, S = snare, h = hat
PATTERN = "K.h.S.hKh.KhS.h." "K.h.S.h.h.KhS.hS"


def render():
    n = int(SR * STEP * STEPS)
    out = [0.0] * n
    rnd = random.Random(1)

    def add(start, samples):
        for i, v in enumerate(samples):
            if start + i < n:
                out[start + i] += v

    def kick():
        length = int(SR * 0.35)
        ph = 0.0
        res = []
        for i in range(length):
            t = i / SR
            f = 50 + 110 * math.exp(-t * 30)
            ph += 2 * math.pi * f / SR
            res.append(math.sin(ph) * math.exp(-t * 9) * 0.95)
        return res

    def snare():
        length = int(SR * 0.25)
        res = []
        for i in range(length):
            t = i / SR
            tone = math.sin(2 * math.pi * 190 * t) * math.exp(-t * 25) * 0.4
            noise = (rnd.random() * 2 - 1) * math.exp(-t * 14) * 0.6
            res.append(tone + noise)
        return res

    def hat():
        length = int(SR * 0.06)
        res = []
        prev = 0.0
        for i in range(length):
            t = i / SR
            x = rnd.random() * 2 - 1
            hp = x - prev
            prev = x
            res.append(hp * math.exp(-t * 60) * 0.25)
        return res

    sounds = {"K": kick(), "S": snare(), "h": hat()}
    for s, ch in enumerate(PATTERN):
        if ch in sounds:
            add(int(s * STEP * SR), sounds[ch])
    peak = max(abs(v) for v in out) or 1.0
    return [v / peak * 0.9 for v in out]


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(root, "plugin", "Content", "Samples", "test_break.wav")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data = render()
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1, min(1, v)) * 32767)) for v in data))
    print("wrote", path, len(data), "frames")
    return 0


if __name__ == "__main__":
    sys.exit(main())
