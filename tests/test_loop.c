/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Loop without clicks (step 23): a sine, a loop with "awkward" points (the jump in the signal is maximal) ->
 * the largest difference between two neighbouring samples must not be bigger than normal for the sine.
 * Also: an End marker in the middle (no loop) ends smoothly, a Start marker in the middle starts smoothly. */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void nap(long us) { struct timespec t = { us / 1000000, (us % 1000000) * 1000 }; nanosleep(&t, NULL); }

static float *render(Session *s, double secs, size_t *n_out)
{
    size_t n = ((size_t)(secs * 48000) + 255) / 256 * 256;   /* whole blocks: nothing played is thrown away */
    float *o = calloc(n, sizeof(float)), buf[256 * 2];
    for (size_t d = 0; d < n; d += 256) {
        mixer_render(s, buf, 256, 2);
        for (size_t i = 0; i < 256 && d + i < n; ++i) o[d + i] = buf[i * 2];
        nap(5333);
    }
    *n_out = n;
    return o;
}
static double max_step(const float *y, size_t a, size_t b, size_t *where)
{
    double m = 0; for (size_t i = a + 1; i < b; ++i) { double d = fabs(y[i] - y[i - 1]); if (d > m) { m = d; if (where) *where = i; } }
    return m;
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    const char *files[2] = { argv[1], argv[2] };           /* 48 kHz and 44.1 kHz sine 440 Hz, 0.5 amplitude */
    static Session S; Session *s = &S;
    session_init(s, 48000);
    streams_init();
    session_add_channel(s, NULL);
    for (int i = 0; i < 2; ++i) session_add_scene(s);
    atomic_store(&s->audio_running, false);
    for (int fi = 0; fi < 2; ++fi) {
        session_set_clip(s, 0, 0, files[fi]);
        Clip *cl = &s->channels[0].clips[0];
        nap(100000);
        /* the smooth sine: the largest normal step */
        session_toggle_clip(s, 0, 0);
        size_t n; float *y = render(s, 0.5, &n);
        double norm = max_step(y, 4800, n, NULL);
        free(y);
        session_stop_all(s);
        for (int wi = 0; wi < 4; ++wi) { int warp = wi > 0;
            session_set_clip_warp(s, 0, 0, warp);
            if (warp) { session_set_clip_bpm_fields(s, 0, 0, 120, 0, 0); session_set_clip_bpm_value(s, 0, 0, 120); }
            /* loop with "bad" points: 10007 .. 20011 (not a whole number of periods -> big jump with a butt splice) */
            session_set_loop_region(s, 0, 0, 10007, 20011);
            if (!atomic_load(&cl->loop)) session_toggle_loop(s, 0, 0);
            session_toggle_clip(s, 0, 0);
            y = render(s, 2.0, &n);
            size_t w = 0;
            double m = max_step(y, 2400, n, &w);
            double lim = warp ? norm * 1.6 : norm * 1.15;
            if (m > lim) {
                size_t z = 0; for (size_t i = 2400; i < n; ++i) if (fabsf(y[i]) < 1e-7f) z++;
                for (size_t i = 2401; i < n; ++i) if (fabs(y[i] - y[i - 1]) > lim) printf("   jump at %.4f s (%.3f) speed %.4f\n", (double)i / 48000, fabs(y[i] - y[i - 1]), session_clip_speed(cl));
                FILE *df = fopen("/tmp/loopdump.txt", "w"); for (size_t i = w - 600; i < w + 600; ++i) fprintf(df, "%f\n", y[i]); fclose(df);
                printf("   worst at %.4f s: %.4f %.4f %.4f %.4f | exact zeros after 50ms: %zu\n", (double)w / 48000,
                       y[w - 2], y[w - 1], y[w], y[w + 1], z);
            }
            CHECK(m <= lim, "[%s, warp %d] loop wrap is smooth: max step %.4f (normal sine %.4f)",
                  fi ? "44.1k->48k" : "48k", warp, m, norm);
            free(y);
            session_stop_all(s);
            session_toggle_loop(s, 0, 0);
        }
        session_set_clip_warp(s, 0, 0, false);
        /* End marker in the middle, no loop: the last samples fade out */
        session_set_start_marker(s, 0, 0, 0);
        session_set_end_marker(s, 0, 0, 24011);
        session_toggle_clip(s, 0, 0);
        y = render(s, 1.0, &n);
        size_t last = 0; for (size_t i = 0; i < n; ++i) if (fabsf(y[i]) > 1e-6f) last = i;
        double m = max_step(y, 2400, last + 50, NULL);
        CHECK(m <= norm * 1.15 && fabsf(y[last]) < 0.02f, "[%s] end marker mid-wave: fades out (last %.4f, max step %.4f)",
              fi ? "44.1k" : "48k", fabsf(y[last]), m);
        free(y);
        session_stop_all(s);
        /* Start marker in the middle: starts smoothly */
        session_set_start_marker(s, 0, 0, 7013);
        session_reset_end_marker(s, 0, 0);
        nap(200000);
        session_toggle_clip(s, 0, 0);
        y = render(s, 0.3, &n);
        size_t first = 0; while (first < n && fabsf(y[first]) < 1e-6f) first++;
        m = max_step(y, first > 0 ? first - 1 : 0, n, NULL);
        CHECK(first < n && m <= norm * 1.15 && fabsf(y[first]) < 0.05f, "[%s] start marker mid-wave: fades in (first %.4f, max step %.4f)",
              fi ? "44.1k" : "48k", first < n ? fabsf(y[first]) : -1.0f, m);
        free(y);
        session_stop_all(s);
        session_reset_start_marker(s, 0, 0);
        free(render(s, 0.05, &n));
        /* stop, pause/resume and seeking IN THE MIDDLE of the waveform: no abrupt jump */
        session_toggle_clip(s, 0, 0);
        float *a = render(s, 0.3, &n);
        size_t n1, n2, n3;
        /* the jump is also measured ACROSS the boundary between the two parts (the last sample before the action) */
        float l0 = a[n - 1]; free(a);
        session_pause_all(s);   float *b1 = render(s, 0.2, &n1);
        session_resume_all(s);  float *b2 = render(s, 0.2, &n2);
        session_seek_clip(s, 0, 0, 33333); float *b3 = render(s, 0.2, &n3);
        session_stop_all(s);    float *b4 = render(s, 0.2, &n);
        #define JOIN(prev, cur) fmax(fabs((double)(cur)[0] - (double)(prev)), 0.0)
        double m1 = fmax(max_step(b1, 0, n1, NULL), JOIN(l0, b1)), m2 = fmax(max_step(b2, 0, n2, NULL), JOIN(b1[n1 - 1], b2));
        double m3 = fmax(max_step(b3, 0, n3, NULL), JOIN(b2[n2 - 1], b3)), m4 = fmax(max_step(b4, 0, n, NULL), JOIN(b3[n3 - 1], b4));
        double lim = norm * 1.6;
        CHECK(m1 <= lim && m2 <= lim && m3 <= lim && m4 <= lim,
              "[%s] pause / resume / seek / stop mid-wave: no clicks (max steps %.4f %.4f %.4f %.4f, normal %.4f)",
              fi ? "44.1k" : "48k", m1, m2, m3, m4, norm);
        free(b1); free(b2); free(b3); free(b4);
        free(render(s, 0.05, &n));
    }
    /* master: balance and MUTE (step 24) — smooth, no jump */
    {
        size_t n;
        session_set_clip(s, 0, 0, files[0]);
        nap(100000);
        session_toggle_clip(s, 0, 0);
        free(render(s, 0.2, &n));
        float buf2[256 * 2];
        atomic_store(&s->master_pan, 1.0f);                 /* fully right -> the left fades out smoothly */
        double maxL = 0, stepL = 0, prevL = 0;
        for (int b = 0; b < 80; ++b) {
            mixer_render(s, buf2, 256, 2);
            for (int i = 0; i < 256; ++i) {
                double v = buf2[i * 2];
                if (b > 0 || i > 0) { double d = fabs(v - prevL); if (d > stepL) stepL = d; }
                prevL = v;
                if (b >= 70 && fabs(v) > maxL) maxL = fabs(v);
            }
        }
        CHECK(maxL < 0.001 && stepL < 0.03, "master balance right: left side silent (%.5f), smooth (step %.4f)", maxL, stepL);
        atomic_store(&s->master_pan, 0.0f);
        free(render(s, 0.2, &n));
        atomic_store(&s->master_muted, true);
        double maxA = 0, stepA = 0, prev = 0;
        for (int b = 0; b < 60; ++b) {
            mixer_render(s, buf2, 256, 2);
            for (int i = 0; i < 512; ++i) {
                double v = buf2[i];
                if (i >= 2) { double d = fabs(v - buf2[i - 2]); if (d > stepA) stepA = d; }
                if (b >= 50 && fabs(v) > maxA) maxA = fabs(v);
            }
            (void)prev;
        }
        CHECK(maxA < 0.001 && stepA < 0.03, "master MUTE: silent (%.5f), no click (max step %.4f)", maxA, stepA);
        atomic_store(&s->master_muted, false);
        session_stop_all(s);
    }
    /* many outputs (step 26): the masks are 64-bit -> channel to output 62, master to 31-32 */
    {
        size_t n;
        enum { OC = MAX_HW_OUT };
        static float big[256 * OC];
        session_set_clip(s, 0, 0, files[0]);
        nap(100000);
        session_route_set(s, 0, 0, ROUTE_HW(OC - 1));       /* channel 1 L -> the last output */
        session_route_set(s, 0, 1, ROUTE_HW(OC - 1));
        session_master_route_set(s, 0, MASTER_ROUTE_HW(30));
        session_master_route_set(s, 1, MASTER_ROUTE_HW(31));
        session_toggle_clip(s, 0, 0);
        free(render(s, 0.1, &n));
        double peak_last = 0, peak_other = 0;
        for (int b = 0; b < 40; ++b) {
            mixer_render(s, big, 256, OC);
            for (int i = 0; i < 256; ++i)
                for (int c = 0; c < OC; ++c) {
                    double v = fabs(big[i * OC + c]);
                    if (c == OC - 1) { if (v > peak_last) peak_last = v; }
                    else if (v > peak_other) peak_other = v;
                }
        }
        CHECK(peak_last > 0.1 && peak_other < 1e-6, "%d outputs: channel goes only to output %d (peak %.3f, others %.6f)",
              OC, OC, peak_last, peak_other);
        session_route_set(s, 0, 0, ROUTE_DEFAULT_L);         /* back: channel -> Master -> 31-32 */
        session_route_set(s, 0, 1, ROUTE_DEFAULT_R);
        free(render(s, 0.1, &n));
        double p31 = 0, p32 = 0, pother = 0;
        for (int b = 0; b < 40; ++b) {
            mixer_render(s, big, 256, OC);
            for (int i = 0; i < 256; ++i)
                for (int c = 0; c < OC; ++c) {
                    double v = fabs(big[i * OC + c]);
                    if (c == 30) { if (v > p31) p31 = v; } else if (c == 31) { if (v > p32) p32 = v; } else if (v > pother) pother = v;
                }
        }
        CHECK(p31 > 0.1 && p32 > 0.1 && pother < 1e-6, "master -> outputs 31-32 (%.3f / %.3f, others %.6f)", p31, p32, pother);
        session_routing_reset(s);
        session_stop_all(s);
    }
    session_free(s);
    printf(fails ? "LOOP: %d FAILED\n" : "LOOP: all ok\n", fails);
    return fails != 0;
}
