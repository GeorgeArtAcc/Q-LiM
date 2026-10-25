/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef BPM_H
#define BPM_H

/*
 * Tempo (BPM) detection of an audio file.
 *
 * How it works (simply):
 *   1) the audio is mixed to mono and downsampled to ~11 kHz;
 *   2) for every ~6 ms we compute how much "new sound" appeared (spectral flux — kicks, hi-hats...);
 *   3) we look for the period at which these hits repeat (autocorrelation + a 1x/2x/3x/4x comb);
 *   4) finally the tempo is refined to 0.01 BPM by looking for the beat grid that best
 *      "fits" the whole analysed excerpt.
 * Up to BPM_MAX_SECONDS seconds are analysed (enough for any song; long mixes do not slow it down).
 *
 * Threads: bpm_job_* are used by the UI thread; the computation runs on a separate background thread (one for all).
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#define BPM_MIN          20.0f     /* limits for manual entry */
#define BPM_MAX         300.0f
#define BPM_DETECT_LO    70.0f     /* the detected tempo is searched in this range */
#define BPM_DETECT_HI   180.0f
#define BPM_MAX_SECONDS 360.0

/* Synchronous detection (used by the background thread and by the tests). 0 = failed. */
float bpm_detect_mono(const float *mono, size_t n, int sample_rate);
float bpm_detect_file(const char *path, const atomic_int *cancel);   /* cancel may be NULL */

/* --- background job --- */
typedef struct BpmJob BpmJob;

BpmJob *bpm_job_start(const char *path);           /* NULL when out of memory */
/* true = done; *out = BPM (0 = failed). Does not free the job. */
bool    bpm_job_poll(BpmJob *j, float *out);
void    bpm_job_release(BpmJob *j);                /* safe at any time (cancels) */
void    bpm_shutdown(void);

/* Rounding for display/saving: to 0.01; if it is within 0.03 of an integer — the integer. */
float   bpm_round(float bpm);

#endif
