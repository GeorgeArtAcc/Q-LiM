/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef STRETCH_H
#define STRETCH_H

/*
 * Changing the speed WITHOUT changing the pitch (time-stretch).
 * Main algorithm (step 12b): Rubber Band R3 — bundled in third_party/rubberband (HAVE_RUBBERBAND).
 * Fallback: WSOLA (described below).
 *
 * How it works (simply): the audio is cut into pieces of ~42 ms that are overlapped with a smooth crossfade.
 * To play faster the pieces are taken from the file less often, to play slower — more often.
 * Every next piece is searched for in a small window (±12 ms) where it best "continues"
 * the previous one — so there are no clicks and the pitch stays the same (the same principle as "keylock" in Mixxx).
 *
 * Threads: stretch_create/free on the UI thread; stretch_process/reset ONLY on the audio thread
 * (no malloc and no locking). All buffers are allocated at creation.
 * With Rubber Band the heavy work is on a separate thread for every clip (see stretch.c).
 */

#include <stdbool.h>
#include <stddef.h>

#define STRETCH_SPEED_MIN 0.25
#define STRETCH_SPEED_MAX 4.0

typedef struct Stretch Stretch;

/* Reads n stereo frames; returns how many are real (the rest must be zeros). */
typedef size_t (*StretchPull)(void *ud, float *out, size_t n);

Stretch *stretch_create(int sample_rate);
void     stretch_free(Stretch *st);
void     stretch_reset(Stretch *st);

/* Produces exactly n stereo frames (interleaved) at speed `speed` (1 = normal, 1.05 = 5% faster).
 * Returns n. */
size_t   stretch_process(Stretch *st, double speed, float *out, size_t n,
                         StretchPull pull, void *ud);

/* How many source frames have been pulled but not yet "heard" (for showing the position). */
size_t   stretch_pending(const Stretch *st);

/* Rubber Band runs on a background thread that reads the clip's stream by itself while it plays with warp.
 * release: "I will not want audio any more" (pause/stop/warp off). Returns true when the thread is certainly
 *          not touching the stream — only then may the mixer read the stream itself again (otherwise: silence this block).
 * finished: the file has ended AND the whole tail has been played (background thread only; always false with WSOLA). */
bool     stretch_release(Stretch *st);
/* UI thread only, for a clip that is NOT playing: resets and starts computing ahead (the queue prepares the next one).
 * Does nothing with WSOLA. */
void     stretch_prepare(Stretch *st, double speed, StretchPull pull, void *ud);
bool     stretch_finished(const Stretch *st);
bool     stretch_threaded(const Stretch *st);
unsigned stretch_underruns(const Stretch *st);
/* Background thread only: after how many output frames the music ends (-1 = not known yet). For the queue. */
long     stretch_frames_to_end(const Stretch *st);

/* How the background thread tells whether a "short" read is the real end of the file or the disk simply has not
 * delivered the audio yet (e.g. a start from the middle of the file). Without this the latter was mistaken for the end and the clip stopped. */
void     stretch_set_eof_check(bool (*is_eof)(void *ud));

/* Which algorithm is used: "Rubber Band R3" (bundled, best quality) or "WSOLA" (fallback). */
const char *stretch_engine_name(void);

#endif
