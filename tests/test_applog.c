/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Log: classification of lines (information / warning / error) */
#include "../src/applog.h"
#include <stdio.h>
static int fails = 0;
#define T(line, want) do { int g = applog_classify(line); if (g != (want)) { printf("FAIL: '%s' -> %d (want %d)\n", line, g, want); fails++; } } while (0)
int main(void)
{
    T("ALSA lib pcm_dmix.c:1000:(snd_pcm_dmix_open) unable to open slave", APPLOG_INFO);
    T(">>> Audio STARTED", APPLOG_INFO);
    T("bpm: 'x.wav' -> 128.00 BPM", APPLOG_INFO);
    T(">>> AUDIO DEVICE LOST: Scarlett 2i2 (sound card disappeared)", APPLOG_ERR);
    T("Audio device Scarlett failed: Device unavailable", APPLOG_ERR);
    T("project_load: cannot open '/x'", APPLOG_ERR);
    T("WARNING: audio dropout (xrun) x3 (total 3)", APPLOG_WARN);
    T(">>> Disk buffer ran dry (HDD disk) -> raising read buffer", APPLOG_WARN);
    T("WARNING: MIDI init failed (continuing anyway)", APPLOG_WARN);
    T(">>> Saved audio device [3] no longer exists -> choosing automatically", APPLOG_WARN);
    printf(fails ? "APPLOG: %d FAILED\n" : "APPLOG: all ok\n", fails);
    return fails != 0;
}
