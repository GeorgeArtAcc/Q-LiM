/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _POSIX_C_SOURCE 200809L
#include "dsp_meter.h"
#include <stdatomic.h>
#include <time.h>

/* The load is kept in units of 1/10000 (integers -> atomic without locking). */
static atomic_ullong g_sum;        /* sum of the loads */
static atomic_uint   g_cnt;
static atomic_uint   g_max;
static atomic_uint   g_xruns;
static atomic_uint   g_over;

uint64_t dsp_meter_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void dsp_meter_end(uint64_t t0_ns, unsigned long frames, int sample_rate)
{
    if (sample_rate <= 0 || frames == 0) return;
    uint64_t dt = dsp_meter_now_ns() - t0_ns;
    double block_ns = 1e9 * (double)frames / (double)sample_rate;
    double load = (double)dt / block_ns;
    if (load > 9.99) load = 9.99;
    unsigned v = (unsigned)(load * 10000.0);
    atomic_fetch_add_explicit(&g_sum, v, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_cnt, 1u, memory_order_relaxed);
    unsigned m = atomic_load_explicit(&g_max, memory_order_relaxed);
    while (v > m && !atomic_compare_exchange_weak_explicit(&g_max, &m, v, memory_order_relaxed, memory_order_relaxed)) {}
    if (v >= 10000u) atomic_fetch_add_explicit(&g_over, 1u, memory_order_relaxed);
}

void dsp_meter_xrun(void) { atomic_fetch_add_explicit(&g_xruns, 1u, memory_order_relaxed); }

void dsp_meter_take(DspSample *out)
{
    unsigned long long sum = atomic_exchange(&g_sum, 0);
    unsigned cnt = atomic_exchange(&g_cnt, 0);
    unsigned mx = atomic_exchange(&g_max, 0);
    out->blocks = cnt;
    out->avg = cnt ? (float)((double)sum / (double)cnt / 10000.0) : 0.0f;
    out->peak = (float)mx / 10000.0f;
}

unsigned dsp_meter_xruns(void) { return atomic_load(&g_xruns); }
unsigned dsp_meter_overloads(void) { return atomic_load(&g_over); }

void dsp_meter_reset(void)
{
    atomic_store(&g_sum, 0); atomic_store(&g_cnt, 0); atomic_store(&g_max, 0);
    atomic_store(&g_xruns, 0); atomic_store(&g_over, 0);
}
