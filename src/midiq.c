/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.2: MIDI queues for the instruments - see midiq.h */
#include "midiq.h"
#include <stdatomic.h>
#include <string.h>

#define MQ_N 1024                                   /* power of 2 */
typedef struct { unsigned char m[MQ_N][4]; atomic_uint w, r; } MQ;
static MQ g_q[MIDIQ_N];

void midiq_push_to(int src, int channel, unsigned char st, unsigned char d1, unsigned char d2)
{
    if (src < 0 || src >= MIDIQ_N || st < 0x80 || st >= 0xF0) return;   /* channel messages only */
    MQ *q = &g_q[src];
    unsigned w = atomic_load_explicit(&q->w, memory_order_relaxed);
    if (w - atomic_load_explicit(&q->r, memory_order_acquire) >= MQ_N) return;   /* full: dropped */
    unsigned char *m = q->m[w & (MQ_N - 1)];
    m[0] = st; m[1] = d1 & 0x7F; m[2] = d2 & 0x7F;
    m[3] = (unsigned char)(channel >= 0 && channel < 255 ? channel + 1 : 0);
    atomic_store_explicit(&q->w, w + 1, memory_order_release);
}

void midiq_push(int src, unsigned char st, unsigned char d1, unsigned char d2) { midiq_push_to(src, -1, st, d1, d2); }

int midiq_pop(unsigned char (*out)[4], int max)
{
    int n = 0;
    for (int s = 0; s < MIDIQ_N; ++s) {
        MQ *q = &g_q[s];
        unsigned r = atomic_load_explicit(&q->r, memory_order_relaxed);
        unsigned w = atomic_load_explicit(&q->w, memory_order_acquire);
        while (r != w && n < max) { memcpy(out[n++], q->m[r & (MQ_N - 1)], 4); ++r; }
        atomic_store_explicit(&q->r, r, memory_order_release);
    }
    return n;
}

void midiq_clear(void)
{
    for (int s = 0; s < MIDIQ_N; ++s) atomic_store(&g_q[s].r, atomic_load(&g_q[s].w));
}
