#!/usr/bin/env python3
"""Writes the demo instruments to plugin/Content/Samples/ (all synthesised, no third-party audio):

  inst01_break.wav  2-bar break at 170 BPM
  inst02_sub.wav    sub bass, pitched at C-3
  inst03_hat.wav    closed hat
  inst04_stab.wav   minor chord stab, root C-3

Mono, 16-bit, 44.1 kHz.
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


C3 = 130.8128


def sub():
    n = int(SR * 1.6)
    return [math.sin(2 * math.pi * C3 * i / SR) * min(1.0, i / 200.0) * math.exp(-i / SR * 1.2) * 0.9 for i in range(n)]


def hat():
    rnd = random.Random(7)
    n = int(SR * 0.09)
    out, prev = [], 0.0
    for i in range(n):
        x = rnd.random() * 2 - 1
        out.append((x - prev) * math.exp(-i / SR * 45) * 0.8)
        prev = x
    return out


def stab():
    n = int(SR * 0.7)
    freqs = [C3 * 2, C3 * 2 * 2 ** (3 / 12), C3 * 2 * 2 ** (7 / 12)]
    out = []
    for i in range(n):
        t = i / SR
        s = 0.0
        for f in freqs:
            for h in range(1, 8):   # soft saw
                s += math.sin(2 * math.pi * f * h * t) / h * (0.8 ** h)
        out.append(s * math.exp(-t * 5) * min(1.0, i / 100.0))
    peak = max(abs(v) for v in out)
    return [v / peak * 0.8 for v in out]


def write(path, data):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1, min(1, v)) * 32767)) for v in data))
    print("wrote", path, len(data), "frames")


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    folder = os.path.join(root, "plugin", "Content", "Samples")
    os.makedirs(folder, exist_ok=True)
    for old in os.listdir(folder):
        if old.endswith(".wav"):
            os.remove(os.path.join(folder, old))
    write(os.path.join(folder, "inst01_break.wav"), render())
    write(os.path.join(folder, "inst02_sub.wav"), sub())
    write(os.path.join(folder, "inst03_hat.wav"), hat())
    write(os.path.join(folder, "inst04_stab.wav"), stab())
    return 0


if __name__ == "__main__":
    sys.exit(main())
