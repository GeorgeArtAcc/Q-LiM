# Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE)
# 440 Hz tone, 10 s (for test_tempo)
import numpy as np, wave, sys
sr=48000; t=np.arange(sr*10)/sr; x=(0.5*np.sin(2*np.pi*440*t)*32767).astype(np.int16)
d=np.stack([x,x],1)
with wave.open(sys.argv[1],'wb') as w: w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr); w.writeframes(d.tobytes())
