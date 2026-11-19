/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.13: the quality of the pitch shifter (src/pitchshift.cpp: Signalsmith time-stretch + sinc resampler, via src/stretch.c).
 * Pure tones and a chord are shifted; measured: the frequency error (cents) and how much of the output is NOT
 * the wanted tones (distortion, aliasing, "phasiness") - in dB below the music. Also with warp (tempo) at once. */
#define _POSIX_C_SOURCE 200809L
#include "stretch.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SR 48000
#define NFFT 32768
static int g_fail = 0;
#define CHECK(c, ...) do { if (c) { printf("ok   "); } else { printf("FAIL "); g_fail = 1; } printf(__VA_ARGS__); printf("\n"); } while (0)

typedef struct { double f[4], a[4]; int n; double ph[4]; } Gen;
static size_t gen_pull(void *ud, float *out, size_t n)
{
    Gen *g = ud;
    for (size_t i = 0; i < n; ++i) {
        double v = 0.0;
        for (int k = 0; k < g->n; ++k) { v += g->a[k] * sin(g->ph[k]); g->ph[k] += 2.0 * M_PI * g->f[k] / SR; }
        out[i * 2] = out[i * 2 + 1] = (float)v;
    }
    return n;
}

static void fft(double *re, double *im, int n)
{
    for (int i = 1, j = 0; i < n; ++i) {
        int b = n >> 1;
        for (; j & b; b >>= 1) j ^= b;
        j ^= b;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a = -2.0 * M_PI / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; ++k) {
                double wr = cos(a * k), wi = sin(a * k);
                double xr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
                double xi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                re[i + k + len / 2] = re[i + k] - xr; im[i + k + len / 2] = im[i + k] - xi;
                re[i + k] += xr; im[i + k] += xi;
            }
    }
}

static void sleep_ms(int ms) { struct timespec t = { 0, ms * 1000000L }; nanosleep(&t, NULL); }

/* shifts gen by semis (at speed `speed`); returns the residual (dB below the wanted tones) and the worst error (cents) */
static unsigned g_underruns;
static void run_sp(const char *name, Gen g, double semis, bool fmt, double speed, double *resid_db, double *cents)
{
    Stretch *st = stretch_create(SR);
    stretch_set_pitch(st, semis, fmt);
    stretch_prepare(st, speed, gen_pull, &g);
    size_t total = SR * 3, got = 0;
    float *out = calloc(total * 2, sizeof(float));
    while (got < total) {                       /* like the audio thread: 256-frame blocks */
        size_t n = total - got < 256 ? total - got : 256;
        sleep_ms(5);                            /* ~ real time (256 frames = 5.3 ms) */
        stretch_process(st, speed, out + got * 2, n, gen_pull, &g);
        got += n;
    }
    g_underruns += stretch_underruns(st);
    stretch_free(st);
    /* analyse the last NFFT frames (after the start), Blackman-Harris window */
    static double re[NFFT], im[NFFT];
    size_t off = total - NFFT;
    for (int i = 0; i < NFFT; ++i) {
        double w = 0.35875 - 0.48829 * cos(2 * M_PI * i / NFFT) + 0.14128 * cos(4 * M_PI * i / NFFT) - 0.01168 * cos(6 * M_PI * i / NFFT);
        re[i] = out[(off + i) * 2] * w; im[i] = 0.0;
    }
    free(out);
    fft(re, im, NFFT);
    double ratio = pow(2.0, semis / 12.0), sig = 0.0, all = 0.0, worst = 0.0;
    static char used[NFFT / 2];
    memset(used, 0, sizeof used);
    for (int k = 0; k < g.n; ++k) {
        double want = g.f[k] * ratio, bin = want * NFFT / SR;
        int c = (int)lround(bin), best = c;
        for (int b = c - 4; b <= c + 4; ++b) if (re[b] * re[b] + im[b] * im[b] > re[best] * re[best] + im[best] * im[best]) best = b;
        double ym = log(re[best - 1] * re[best - 1] + im[best - 1] * im[best - 1] + 1e-30);
        double y0 = log(re[best] * re[best] + im[best] * im[best] + 1e-30);
        double yp = log(re[best + 1] * re[best + 1] + im[best + 1] * im[best + 1] + 1e-30);
        double d = 0.5 * (ym - yp) / (ym - 2 * y0 + yp);
        double fgot = (best + d) * SR / NFFT;
        double ce = fabs(1200.0 * log2(fgot / want));
        if (ce > worst) worst = ce;
        for (int b = c - 8; b <= c + 8; ++b) if (b > 0 && b < NFFT / 2) used[b] = 1;   /* the tone + the window's main lobe */
    }
    for (int b = 1; b < NFFT / 2; ++b) {
        double e = re[b] * re[b] + im[b] * im[b];
        all += e;
        if (used[b]) sig += e;
    }
    *resid_db = 10.0 * log10((all - sig + 1e-30) / (sig + 1e-30));
    if (getenv("PITCH_DEBUG")) {                 /* the strongest unwanted peaks */
        for (int t = 0; t < 5; ++t) {
            int bb = -1; double be = 0;
            for (int b = 2; b < NFFT / 2 - 2; ++b) {
                double e = re[b] * re[b] + im[b] * im[b];
                if (!used[b] && e > be) { be = e; bb = b; }
            }
            if (bb < 0) break;
            printf("        spur %8.1f Hz %6.1f dB\n", bb * (double)SR / NFFT, 10 * log10(be / (sig + 1e-30)));
            for (int b = bb - 6; b <= bb + 6; ++b) if (b > 0 && b < NFFT / 2) used[b] = 2;
        }
    }
    *cents = worst;
    printf("     %-34s %+6.2f st%s%s: residual %6.1f dB, pitch error %.3f cents\n", name, semis, fmt ? " (formants)" : "",
           speed != 1.0 ? " at speed 1.25" : "", *resid_db, worst);
}
static void run(const char *name, Gen g, double semis, bool fmt, double *resid_db, double *cents)
{
    run_sp(name, g, semis, fmt, 1.0, resid_db, cents);
}

int main(void)
{
    printf("PITCH: tempo engine %s, pitch: %s\n", stretch_engine_name(), stretch_has_pitch() ? "Signalsmith + resampler" : "none");
    CHECK(stretch_has_pitch(), "the pitch engine is built in");
    Gen tone = { { 440.0 }, { 0.5 }, 1, { 0 } };
    Gen chord = { { 261.63, 329.63, 392.0, 523.25 }, { 0.2, 0.2, 0.2, 0.2 }, 4, { 0 } };
    Gen high = { { 7000.0, 9500.0 }, { 0.3, 0.3 }, 2, { 0 } };
    double r, c, worst_r = -999, worst_c = 0;
    const double steps[] = { -12, -7, -5, -1, -0.5, 0.5, 1, 5, 7, 12 };
    for (size_t i = 0; i < sizeof steps / sizeof *steps; ++i) {
        run("440 Hz tone", tone, steps[i], false, &r, &c);
        if (r > worst_r) worst_r = r;
        if (c > worst_c) worst_c = c;
    }
    CHECK(worst_r < -46.0, "a pure tone: everything that is not the tone is at least 46 dB down (worst %.1f dB)", worst_r);
    CHECK(worst_c < 0.25, "a pure tone: the new pitch is exact to 0.25 cent (worst %.3f cents)", worst_c);
    worst_r = -999; worst_c = 0;
    for (size_t i = 0; i < sizeof steps / sizeof *steps; ++i) {
        run("C major chord", chord, steps[i], false, &r, &c);
        if (r > worst_r) worst_r = r;
        if (c > worst_c) worst_c = c;
    }
    CHECK(worst_r < -45.0, "a chord: the other energy is at least 45 dB down (worst %.1f dB)", worst_r);
    CHECK(worst_c < 0.25, "a chord: every note is exact to 0.25 cent (worst %.3f cents)", worst_c);
    run("7 + 9.5 kHz up 12 (-> 14 + 19 kHz)", high, 12, false, &r, &c);
    CHECK(r < -44.0, "high tones shifted up near the top: nothing else above -44 dB (%.1f dB)", r);
    run("7 + 9.5 kHz down 12", high, -12, false, &r, &c);
    CHECK(r < -44.0, "high tones shifted down: clean (%.1f dB)", r);
    run("C major chord", chord, 5, true, &r, &c);
    CHECK(c < 0.5 && r < -44.0, "with formants kept the notes are still right (%.1f dB, %.3f cents)", r, c);
    run_sp("C major chord", chord, 3, false, 1.25, &r, &c);
    CHECK(c < 0.25 && r < -50.0, "pitch + faster tempo (warp) together: the notes are right (%.1f dB, %.3f cents)", r, c);
    CHECK(g_underruns == 0, "the background thread always kept up (%u underruns)", g_underruns);
    printf("PITCH: %s\n", g_fail ? "FAILED" : "all ok");
    return g_fail;
}
