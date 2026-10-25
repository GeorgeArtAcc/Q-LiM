# Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE)
# two 440 Hz sines, 0.5 amplitude, 3 s: 48 kHz and 44.1 kHz
import numpy as np, wave, sys
for sr,name in [(48000,'sine48'),(44100,'sine44')]:
    t=np.arange(3*sr)/sr; x=0.5*np.sin(2*np.pi*440*t)
    d=(np.stack([x,x],1)*32767).astype(np.int16)
    with wave.open(f'{sys.argv[1]}/{name}.wav','wb') as w: w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr); w.writeframes(d.tobytes())
