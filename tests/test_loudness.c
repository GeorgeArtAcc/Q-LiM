/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.11: the loudness meter (src/peaks.c, ITU-R BS.1770-4 / EBU R128) against the EBU Tech 3341 test cases 1-4
 * (1 kHz sine in both channels; expected -23.0 / -33.0 LUFS within 0.1 LU) at 44.1, 48 and 96 kHz. */
#include "peaks.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_fail;

typedef struct { double db, sec; } Seg;

static float run(int sr, const Seg *seg, int nseg, float *peak)
{
    LoudMeter *m = loud_new(sr);
    double ph = 0.0;
    float buf[2 * 4096];
    for (int k = 0; k < nseg; ++k) {
        double a = pow(10.0, seg[k].db / 20.0);
        size_t n = (size_t)llround(seg[k].sec * sr);
        while (n) {
            size_t c = n > 4096 ? 4096 : n;
            for (size_t i = 0; i < c; ++i) {
                float v = (float)(a * sin(ph));
                ph += 2 * M_PI * 1000.0 / sr;
                buf[2 * i] = buf[2 * i + 1] = v;
            }
            loud_feed(m, buf, c);
            n -= c;
        }
    }
    float l, p;
    loud_result(m, &l, &p);
    loud_free(m);
    if (peak) *peak = p;
    return l;
}

static void check(const char *name, int sr, const Seg *seg, int nseg, double want)
{
    float pk;
    float l = run(sr, seg, nseg, &pk);
    bool ok = fabs(l - want) <= 0.1;
    if (!ok) g_fail++;
    printf("%s %-28s %6d Hz: %.2f LUFS (want %.1f)\n", ok ? "  ok" : "FAIL", name, sr, l, want);
}

int main(void)
{
    const int rates[] = { 44100, 48000, 96000 };
    const Seg c1[] = { { -23, 20 } };
    const Seg c2[] = { { -33, 20 } };
    const Seg c3[] = { { -36, 10 }, { -23, 60 }, { -36, 10 } };
    const Seg c4[] = { { -72, 10 }, { -36, 10 }, { -23, 60 }, { -36, 10 }, { -72, 10 } };
    for (int r = 0; r < 3; ++r) {
        check("Tech 3341 case 1 (-23 dBFS)", rates[r], c1, 1, -23.0);
        check("Tech 3341 case 2 (-33 dBFS)", rates[r], c2, 1, -33.0);
        check("Tech 3341 case 3 (gating)", rates[r], c3, 3, -23.0);
        check("Tech 3341 case 4 (gating)", rates[r], c4, 5, -23.0);
    }
    float pk;
    run(48000, c1, 1, &pk);
    if (fabs(pk + 23.0) > 0.05) { g_fail++; printf("FAIL peak %.2f dBFS (want -23)\n", pk); }
    else printf("  ok peak %.2f dBFS\n", pk);
    const Seg sil[] = { { -200, 5 } };
    float ls = run(48000, sil, 1, NULL);
    if (!isinf(ls)) { g_fail++; printf("FAIL silence = %.2f\n", ls); } else printf("  ok silence = -inf\n");
    printf(g_fail ? "test_loudness: %d FAILED\n" : "test_loudness: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
