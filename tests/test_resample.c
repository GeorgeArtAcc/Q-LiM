/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.7: quality of the sample rate converter (src/stream_resample.c): a sine goes in, the sine is fitted out of the
 * result, the rest is noise + distortion + aliasing. 0..20 kHz must stay flat (0.02 dB) and the rest >= 120 dB down. */
#include "stream.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
typedef struct { double f; int sr; size_t n; } Src;
static size_t s_frames(void *c) { return ((Src *)c)->n; }
static size_t s_read(void *c, size_t pos, float *out, size_t n)
{
    Src *s = c;
    for (size_t i = 0; i < n; ++i) {
        float v = (float)(0.5 * sin(2 * M_PI * s->f * (double)(pos + i) / s->sr));
        out[2 * i] = v; out[2 * i + 1] = -v;
    }
    return n;
}
static void s_close(void *c) { (void)c; }

static int g_fail;

static void one(int src, int dst, double f)
{
    Src s = { f, src, (size_t)src * 3 };
    StreamSource in = { s_frames, s_read, s_close, &s }, out;
    if (stream_source_resample(&in, src, dst, &out) != 0) { printf("FAIL open\n"); g_fail++; return; }
    size_t N = (size_t)dst, off = (size_t)dst;            /* 1 s from the middle */
    float *b = malloc(N * 2 * sizeof(float));
    /* read in uneven pieces, like the audio thread */
    size_t got = 0;
    while (got < N) { size_t m = 1 + (got * 7919) % 700; if (m > N - got) m = N - got; got += out.read(out.ctx, off + got, b + got * 2, m); }
    for (int ch = 0; ch < 2; ++ch) {
        double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
        for (size_t i = 0; i < N; ++i) {
            double t = 2 * M_PI * f * (double)(off + i) / dst, sn = sin(t), cs = cos(t), y = b[2 * i + ch];
            ss += sn * sn; sc += sn * cs; cc += cs * cs; ys += y * sn; yc += y * cs;
        }
        double det = ss * cc - sc * sc, A = (ys * cc - yc * sc) / det, B = (yc * ss - ys * sc) / det;
        double e = 0, p = 0;
        for (size_t i = 0; i < N; ++i) {
            double t = 2 * M_PI * f * (double)(off + i) / dst, fit = A * sin(t) + B * cos(t), y = b[2 * i + ch];
            e += (y - fit) * (y - fit); p += fit * fit;
        }
        double lvl = 20 * log10(sqrt(A * A + B * B) / 0.5), snr = 10 * log10(p / (e + 1e-300));
        bool ok = fabs(lvl) < 0.02 && snr >= 120.0;
        if (!ok) g_fail++;
        if (ch == 0 || !ok)
            printf("%s %6d -> %6d Hz  %5.0f Hz: level %+.4f dB, rest %.1f dB down\n", ok ? "  ok" : "FAIL", src, dst, f, lvl, snr);
    }
    out.close(out.ctx);
    free(b);
}

int main(void)
{
    const int pairs[][2] = { { 44100, 48000 }, { 48000, 44100 }, { 44100, 96000 }, { 96000, 44100 }, { 48000, 96000 },
                             { 96000, 48000 }, { 22050, 48000 }, { 32000, 44100 }, { 192000, 48000 } };
    const double fs[] = { 50, 1000, 5000, 10000, 15000, 19000, 20000 };
    for (size_t k = 0; k < sizeof pairs / sizeof pairs[0]; ++k)
        for (size_t j = 0; j < sizeof fs / sizeof fs[0]; ++j) {
            double nyq = 0.5 * (pairs[k][0] < pairs[k][1] ? pairs[k][0] : pairs[k][1]);
            if (fs[j] <= nyq * 0.907) one(pairs[k][0], pairs[k][1], fs[j]);    /* 0.907 * 22050 = 20 kHz */
        }
    stream_resample_shutdown();
    printf(g_fail ? "test_resample: %d FAILED\n" : "test_resample: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
