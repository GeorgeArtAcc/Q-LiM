/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* WSOLA: speed 1 = the same sound; another speed -> the same pitch, correct length, no clicks. */
#define _POSIX_C_SOURCE 200809L
#include "../src/stretch.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SR 48000
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
typedef struct { const float *x; size_t n, pos; } Src;
static size_t pull(void *ud, float *out, size_t n)
{
    Src *s = ud;
    size_t k = 0;
    for (; k < n && s->pos < s->n; ++k, ++s->pos) { out[k*2] = s->x[s->pos*2]; out[k*2+1] = s->x[s->pos*2+1]; }
    return k;
}
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* frequency by counting zero crossings (in the middle of the signal) */
static double freq_of(const float *y, size_t a, size_t b)
{
    int zc = 0; size_t first = 0, last = 0;
    for (size_t i = a + 1; i < b; ++i)
        if (y[(i-1)*2] < 0 && y[i*2] >= 0) { if (!zc) first = i; last = i; zc++; }
    return zc > 1 ? (double)(zc - 1) * SR / (double)(last - first) : 0;
}

int main(void)
{
    size_t n = SR * 6;
    float *x = malloc(n * 2 * sizeof(float));
    /* 1) speed 1: almost identical sound (chord + noise) */
    srand(3);
    for (size_t i = 0; i < n; ++i) {
        double t = (double)i / SR;
        float v = (float)(0.3*sin(2*M_PI*220*t) + 0.2*sin(2*M_PI*331*t) + 0.1*sin(2*M_PI*1234*t)) + 0.05f*((rand()/(float)RAND_MAX)-0.5f);
        x[i*2] = v; x[i*2+1] = v * 0.7f;
    }
    Stretch *st = stretch_create(SR);
    Src s = { x, n, 0 };
    size_t m = SR * 5;
    float *y = malloc(m * 2 * sizeof(float));
    for (size_t d = 0; d < m; d += 512) stretch_process(st, 1.0, y + d*2, (m - d) < 512 ? m - d : 512, pull, &s);
    double err = 0, ref = 0;
    for (size_t i = 0; i < m; ++i) { double e = y[i*2] - x[i*2]; err += e*e; ref += (double)x[i*2]*x[i*2]; }
    double snr = 10*log10(ref / (err + 1e-20));
    CHECK(snr > 60, "speed 1.0 is transparent (SNR %.1f dB)", snr);
    CHECK(fabs(y[0] - x[0]) < 1e-4 && fabs(y[100*2] - x[100*2]) < 1e-4, "starts exactly at the beginning (no fade-in, no delay)");

    /* 2) a 440 Hz tone at different speeds: the pitch is the same, frames pulled ~ speed * output frames */
    for (size_t i = 0; i < n; ++i) { float v = (float)(0.5*sin(2*M_PI*440*(double)i/SR)); x[i*2] = x[i*2+1] = v; }
    double speeds[] = { 0.5, 0.8, 0.95, 1.03, 1.25, 1.6, 2.0 };
    for (int k = 0; k < 7; ++k) {
        double sp = speeds[k];
        stretch_reset(st);
        Src s2 = { x, n, 0 };
        size_t out = SR * 2;
        for (size_t d = 0; d < out; d += 256) stretch_process(st, sp, y + d*2, (out-d) < 256 ? out-d : 256, pull, &s2);
        double f = freq_of(y, SR/4, out - SR/4);
        double used = (double)(s2.pos - stretch_pending(st));
        double ratio = used / (double)out;
        /* clicks: the largest difference between neighbouring samples must not be much above that of the clean tone */
        double maxd = 0; for (size_t i = SR/10; i < out; ++i) { double dd = fabs(y[i*2] - y[(i-1)*2]); if (dd > maxd) maxd = dd; }
        double sine_d = 0.5 * 2*M_PI*440/SR;
        CHECK(fabs(f - 440) < 2.0 && fabs(ratio - sp) < 0.02 * sp && maxd < sine_d * 1.6,
              "speed %.2f: pitch %.1f Hz, source used %.3fx, max step %.4f (sine %.4f)", sp, f, ratio, maxd, sine_d);
    }

    /* 3) speed: how much CPU for 1 s of audio */
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    stretch_reset(st); Src s3 = { x, n, 0 };
    for (size_t d = 0; d < (size_t)SR * 4; d += 256) stretch_process(st, 1.07, y + (d % (SR*2))*2, 256, pull, &s3);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ms = ((b.tv_sec-a.tv_sec)*1e3 + (b.tv_nsec-a.tv_nsec)/1e6) / 4.0;
    CHECK(ms < 15, "CPU: %.2f ms per 1 s of audio per clip (%.2f%% of one core)", ms, ms/10);

    stretch_free(st); free(x); free(y);
    printf(fails ? "STRETCH: %d FAILED\n" : "STRETCH: all ok\n", fails);
    return fails != 0;
}
