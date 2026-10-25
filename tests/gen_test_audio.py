# Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE)
import numpy as np, sys, wave
rng=np.random.default_rng(1)
def write(path, x, sr):
    x=np.clip(x,-1,1); d=(x*32767).astype(np.int16)
    with wave.open(path,'wb') as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr); w.writeframes(d.tobytes())
def track(bpm, style, sr=44100, secs=60):
    n=int(secs*sr); L=np.zeros(n); R=np.zeros(n)
    beat=60/bpm
    t=np.arange(int(0.5*sr))/sr
    kick=np.sin(2*np.pi*(50+80*np.exp(-t*30))*t)*np.exp(-t*8)
    snare=rng.normal(0,1,len(t))*np.exp(-t*20)*0.5+np.sin(2*np.pi*190*t)*np.exp(-t*25)*0.3
    hat=rng.normal(0,1,int(0.05*sr)); hat=np.diff(np.r_[0,hat])*np.exp(-np.arange(len(hat))/sr*90)*0.25
    def add(sig, at, g=1.0, pan=0.0):
        i=int(at*sr); 
        if i>=n: return
        m=min(len(sig), n-i); L[i:i+m]+=sig[:m]*g*(1-pan); R[i:i+m]+=sig[:m]*g*(1+pan)
    nb=int(secs/beat)
    for b in range(nb):
        tb=b*beat
        if style=='house':
            add(kick,tb); add(hat,tb+beat/2,0.8,0.3)
            if b%2==1: add(snare,tb,0.6)
        elif style=='hiphop':  # kick on 1, 3.5 ; snare 2,4; 16th hats swing
            bar=b%4
            if bar in (0,): add(kick,tb)
            if bar==2: add(kick,tb+beat/2)
            if bar in (1,3): add(snare,tb,0.9)
            for k in range(4): add(hat,tb+k*beat/4+(0.02 if k%2 else 0),0.5 if k%2 else 0.8,-0.2)
        elif style=='dnb':  # breakbeat kick 1, 2.5 snare 2,4 fast hats
            bar=b%4
            if bar==0: add(kick,tb)
            if bar==2: add(kick,tb+beat/2)
            if bar in (1,3): add(snare,tb,1.0)
            for k in range(2): add(hat,tb+k*beat/2,0.6)
        elif style=='techno':
            add(kick,tb); 
            for k in range(4): add(hat,tb+k*beat/4,0.4+0.4*(k==2))
    # bass + pad (tonal content)
    tt=np.arange(n)/sr
    notes=[55,55,65.4,49]
    bar_len=4*beat
    f=np.array([notes[int(x/bar_len)%4] for x in tt[::sr//100]])
    f=np.repeat(f, sr//100+1)[:n]
    ph=2*np.pi*np.cumsum(f)/sr
    bass=0.25*np.sin(ph)*(0.6+0.4*np.cos(2*np.pi*tt/beat)**2)
    pad=0.05*(np.sin(2*np.pi*220*tt)+np.sin(2*np.pi*277.2*tt)+np.sin(2*np.pi*329.6*tt))
    L+=bass+pad; R+=bass+pad
    m=max(np.abs(L).max(),np.abs(R).max()); 
    return np.stack([L,R],1)/m*0.9
out=sys.argv[1]
for bpm,style in [(128,'house'),(124,'house'),(126.5,'house'),(140,'techno'),(90,'hiphop'),(95,'hiphop'),(174,'dnb'),(100,'house'),(120,'techno'),(133.33,'techno')]:
    sr=48000 if bpm in (124,140) else 44100
    write(f"{out}/t_{bpm}_{style}.wav", track(bpm,style,sr), sr)
print("ok")
