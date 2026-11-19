/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef PITCHSHIFT_H
#define PITCHSHIFT_H
/*
 * 1.4.13: pitch shifting (and, at the same time, tempo) with Signalsmith Stretch (third_party/signalsmith, MIT).
 * Chosen by measurement (tests/test_pitch.c + real stems): for pitch it is clearly cleaner than Rubber Band R3
 * on chords, bass, voice and full mixes, and needs ~10x less CPU (important for old laptops).
 * Plain C interface for src/stretch.c. Not real-time safe to create; ps_process is (no allocations).
 */
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct PitchShift PitchShift;

PitchShift *ps_new(int sample_rate);            /* stereo */
void ps_free(PitchShift *p);
void ps_reset(PitchShift *p);
/* semitones (fractions = cents); formants = keep the formants (voices do not become "chipmunks") */
void ps_set(PitchShift *p, double semitones, bool formants);
/* nin input frames -> nout output frames (nout = nin / speed gives the tempo change) */
void ps_process(PitchShift *p, const float *inL, const float *inR, int nin, float *outL, float *outR, int nout);
int  ps_latency_in(const PitchShift *p);         /* input frames ahead of the processing time */
int  ps_latency_out(const PitchShift *p);        /* output frames behind it */

#ifdef __cplusplus
}
#endif
#endif
