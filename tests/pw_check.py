# Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE)
# Checks the recording from pw_capture: every channel must be a continuous "sawtooth" (no dropped/repeated frames),
# and the channels in the right order (channel c is shifted by c*977 frames).
import sys, array
path, ch = sys.argv[1], int(sys.argv[2])
raw = open(path, 'rb').read()
a = array.array('f'); a.frombytes(raw[: (len(raw) // (4*ch)) * 4*ch])
n = len(a) // ch
step = 0.5 / 4800
bad = 0; first_bad = None; starts = []; ends = []
for c in range(ch):
    s = next((i for i in range(n) if a[i*ch+c] != 0.0), None)
    if s is None: print("NO SIGNAL on channel", c); sys.exit(1)
    e = n
    while e > s and a[(e-1)*ch+c] == 0.0: e -= 1
    # the channels are linked one by one -> the start may be in the middle of a sawtooth; we take the phase from the first frame
    k0 = round(a[s*ch+c] / step)
    for i in range(s, e):
        exp = ((k0 + (i - s)) % 4800) * step
        v = a[i*ch+c]
        if abs(v - exp) > 1e-6 and not (v == 0.0 and exp < 1e-9):
            bad += 1
            if first_bad is None: first_bad = (c, i, v, exp)
    starts.append(s); ends.append(e)
m = max(starts)
offs = [(round(a[m*ch+c]/step) - round(a[m*ch]/step)) % 4800 for c in range(ch)]
print(f"frames checked: {min(ends)-m} ({(min(ends)-m)/48000:.2f}s @48k)  bad samples: {bad}  channel offsets: {offs}")
if first_bad: print("first bad (channel, frame, got, expected):", first_bad)
ok = bad == 0 and all(offs[c] == (c*977) % 4800 for c in range(ch))
print("RESULT:", "OK" if ok else "FAIL"); sys.exit(0 if ok else 1)
