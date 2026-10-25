# Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE)
# Crossfade test: xa = 4 s noise + 2 s digital silence; xb = 0.5 s silence + 4 s noise (different noise)
import numpy as np, wave, sys
sr=48000
def save(n, x):
    d=(np.stack([x,x],1)*32767).astype(np.int16)
    with wave.open(f'{sys.argv[1]}/{n}.wav','wb') as w: w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr); w.writeframes(d.tobytes())
r=np.random.default_rng(1)
na=np.clip(r.normal(0,0.15,4*sr),-0.9,0.9); nb=np.clip(r.normal(0,0.15,4*sr),-0.9,0.9)
save('xa', np.concatenate([na, np.zeros(2*sr)]))
save('xb', np.concatenate([np.zeros(sr//2), nb]))
