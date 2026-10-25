# Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE)
# two 2-second tones with a constant offset (never 0) -> any silence at the transition is a gap
import numpy as np, wave, sys
sr=48000; t=np.arange(2*sr)/sr
for n,f in [('qa',440),('qb',660)]:
    x=0.5*np.sin(2*np.pi*f*t)+0.3
    d=(np.stack([x,x],1)*32767*0.9).astype(np.int16)
    with wave.open(f'{sys.argv[1]}/{n}.wav','wb') as w: w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr); w.writeframes(d.tobytes())
