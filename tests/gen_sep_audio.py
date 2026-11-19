#!/usr/bin/env python3
# Q-LiM - test audio for test_separate (it checks the plumbing; the model's quality is measured on real music): a synthetic singer (glottal pulses, vibrato, vowel formants, a melody)
# over synthetic music (kick, hats, bass, saw chords). Writes mix.wav and voice.wav (44.1 kHz float). GPL-3.0-or-later.
import sys, wave, struct
import numpy as np
from scipy.signal import lfilter
out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/qlim_sep_audio"
import os; os.makedirs(out, exist_ok=True)
SR = 44100; D = 10.0; n = int(SR * D); t = np.arange(n) / SR
rng = np.random.default_rng(7)
# --- music: 120 BPM
beat = SR // 2
mus = np.zeros((n, 2))
for b in range(0, n, beat):                                   # kick
    k = np.arange(min(6000, n - b)); f = 50 + 90 * np.exp(-k / 600.0)
    mus[b:b + len(k), :] += (0.7 * np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-k / 3000.0))[:, None]
for b in range(beat // 2, n, beat):                           # hats
    k = np.arange(min(2500, n - b)); h = rng.standard_normal(len(k)) * np.exp(-k / 400.0) * 0.15
    h = np.diff(h, prepend=0); mus[b:b + len(k), 0] += h; mus[b:b + len(k), 1] += h * 0.8
chords = [(45, 52, 57, 60), (41, 48, 53, 57), (43, 50, 55, 59), (40, 47, 52, 56)]
bar = 4 * beat
for i, c in enumerate(chords * 3):
    s0 = i * bar; s1 = min(n, s0 + bar)
    if s0 >= n: break
    tt = np.arange(s1 - s0) / SR
    env = np.minimum(1, tt / 0.02) * np.exp(-tt / 1.5)
    for m in c[1:]:
        f = 440 * 2 ** ((m + 12 - 69) / 12)
        saw = 2 * ((tt * f) % 1.0) - 1
        mus[s0:s1, 0] += 0.05 * saw * env; mus[s0:s1, 1] += 0.05 * (2 * ((tt * f * 1.003) % 1.0) - 1) * env
    fb = 440 * 2 ** ((c[0] - 12 - 69) / 12)
    mus[s0:s1, :] += (0.25 * np.sin(2 * np.pi * fb * tt) * np.minimum(1, tt / 0.01))[:, None]
# --- the singer
notes = [57, 60, 62, 64, 62, 60, 59, 57, 60, 64]
seg = n // len(notes)
f0 = np.zeros(n)
for i, m in enumerate(notes): f0[i * seg:(i + 1) * seg] = 440 * 2 ** ((m - 69) / 12)
f0 *= 1 + 0.012 * np.sin(2 * np.pi * 5.5 * t)
ph = np.cumsum(f0 / SR) % 1.0
src = np.where(ph < 0.6, 0.5 * (1 - np.cos(np.pi * ph / 0.6)), np.cos(np.pi * (ph - 0.6) / 0.8))
src = np.diff(src, prepend=0) + 0.003 * rng.standard_normal(n)
vowels = [(730, 1090, 2440), (530, 1840, 2480), (570, 840, 2410), (300, 870, 2240)]
def res(x, f, bw):
    r = np.exp(-np.pi * bw / SR); th = 2 * np.pi * f / SR
    return lfilter([1 - r], [1, -2 * r * np.cos(th), r * r], x)
voc = np.zeros(n)
for i in range(len(notes)):
    s0, s1 = i * seg, (i + 1) * seg; F = vowels[i % 4]; x = src[s0:s1]
    v = res(x, F[0], 80) + 0.6 * res(x, F[1], 100) + 0.3 * res(x, F[2], 120)
    k = np.arange(len(x)); env = np.minimum(1, np.minimum(k, len(x) - k) / 2000.0)
    voc[s0:s1] = v * env
voc = voc / np.max(np.abs(voc)) * 0.35
vs = np.stack([voc, voc], 1)
mix = mus * 0.6 + vs
def write(path, a):
    a = np.asarray(a, dtype=np.float32)
    with open(path, "wb") as f:                               # WAV float (format 3)
        data = a.tobytes()
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 3, 2, SR, SR * 8, 8, 32))
        f.write(b"data" + struct.pack("<I", len(data)) + data)
write(out + "/mix.wav", mix); write(out + "/voice.wav", vs)
print("ok", out)
