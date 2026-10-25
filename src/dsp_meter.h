/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef DSP_METER_H
#define DSP_METER_H
/*
 * DSP indicator (as in DAWs): how much of the audio block's time goes into processing.
 * Load = time in the callback / block length. 100% = on the edge of crackling.
 * No malloc and no locks - safe for the audio thread.
 */
#include <stdint.h>

/* --- audio thread --- */
uint64_t dsp_meter_now_ns(void);                                              /* starts a measurement */
void     dsp_meter_end(uint64_t t0_ns, unsigned long frames, int sample_rate); /* finishes measuring one block */
void     dsp_meter_xrun(void);                                                /* the card has reported a dropout */

/* --- interface --- */
typedef struct {
    float    avg;       /* average load since the last call (0..1+) */
    float    peak;      /* the worst block since the last call */
    unsigned blocks;    /* how many blocks were measured */
} DspSample;

void     dsp_meter_take(DspSample *out);          /* takes and resets the accumulated values (called by the interface) */
unsigned dsp_meter_xruns(void);                   /* total number of dropouts since start */
unsigned dsp_meter_overloads(void);               /* total number of blocks that took longer than 100% */
void     dsp_meter_reset(void);                   /* for tests / a fresh audio start */
#endif
