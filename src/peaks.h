/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef PEAKS_H
#define PEAKS_H

/*
 * A "sketch" table of the waveform: the highest value of every PEAKS_BLOCK frames.
 * Computed in the background (one thread for all clips) from its own file handle,
 * so it does not disturb playback and does not block the screen. The UI draws whatever is already ready.
 *
 * Ownership: the clip holds one reference; the worker holds another while computing.
 * peaks_release() is safe at any time (cancels the computation).
 */

#include <stdbool.h>
#include <stddef.h>

#define PEAKS_BLOCK 256u

typedef struct Peaks Peaks;

Peaks *peaks_start(const char *path, int sample_rate, size_t frames);
void   peaks_release(Peaks *p);
void   peaks_shutdown(void);

size_t peaks_ready(const Peaks *p);   /* how many blocks are ready */
size_t peaks_total(const Peaks *p);
bool   peaks_done(const Peaks *p);

/* Maximum over blocks b0..b1 (inclusive). Invalid/unready ones are skipped. */
float  peaks_max(const Peaks *p, size_t b0, size_t b1);

/* Detailed value for a section: lowest/highest and average level (RMS) separately for the left and right channel.
 * (step 14: for a more detailed and stereo waveform) */
typedef struct {
    float lmin, lmax, rmin, rmax;   /* -1 .. 1 */
    float lrms, rrms;               /*  0 .. 1 */
} PeakVal;

/* Merges blocks b0..b1. false = none of them is ready yet. */
bool   peaks_range(const Peaks *p, size_t b0, size_t b1, PeakVal *out);
/* Like peaks_detail, but with PeakVal (the real samples from the file, for deep zoom). */
bool   peaks_detail_val(Peaks *p, size_t start, double frames_per_bar, int n_bars, PeakVal *out);

/* Detailed waveform for deep zoom (when one pixel is less than PEAKS_BLOCK frames):
 * reads the samples themselves from the file (own handle, UI thread only).
 * Fills out[0..n_bars-1] with the maximum of |average(L,R)| over frames_per_bar output frames per bar,
 * starting from output frame `start`. Returns true on success. */
bool   peaks_detail(Peaks *p, size_t start, double frames_per_bar, int n_bars, float *out);

#endif
