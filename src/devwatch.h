/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef DEVWATCH_H
#define DEVWATCH_H
/*
 * Watching whether the sound card is still there (unplugged USB card, crashed driver...).
 * PortAudio itself does not say when the card disappears - the stream just goes silent or keeps running "dry",
 * so we collect several independent signs:
 *   1) the card was ALSA "hw:N,M" and the folder <asound>/cardN has disappeared
 *   2) the card's name was in <asound>/cards at startup and is gone now
 *   3) the audio callback has stopped being called (heartbeat)
 * Only the pure, tested parts are here; the PortAudio checks are in audio.c.
 */
#include <stddef.h>

typedef struct {
    int  hw_card;          /* N from "(hw:N,M)" or -1 */
    char tok[2][32];       /* at most 2 significant words of the name (e.g. "Scarlett", "2i2") */
    int  ntok;
    int  monitor_name;     /* 1 = the name was found in cards at startup -> we track it */
    char root[256];        /* usually /proc/asound; for tests - another folder */
} DevWatch;

/* asound_root may be NULL (= /proc/asound). */
void devwatch_init(DevWatch *w, const char *pa_device_name, const char *asound_root);

/* 1 = the card is there, 0 = missing, -1 = we cannot track it (e.g. a virtual device) */
int  devwatch_present(const DevWatch *w);

typedef struct { unsigned last; double last_change; int started; } Heartbeat;

/* Called periodically with the current number of callbacks. 1 = it has not been called for longer than timeout_s. */
int  heartbeat_stalled(Heartbeat *h, unsigned count, double now_s, double timeout_s);
void heartbeat_reset(Heartbeat *h);
#endif
