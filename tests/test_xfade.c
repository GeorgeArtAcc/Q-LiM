/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Queue crossfade (step 17): the next clip comes in EXACTLY X s before the end of the current one,
 * the total power stays constant (equal power = no "dip" and no "bump"),
 * the silence at the end of the first and at the start of the second is skipped. In different channels and in the same one. */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/peaks.h"
#include "../src/stretch.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void nap(long us) { struct timespec t = { us / 1000000, (us % 1000000) * 1000 }; nanosleep(&t, NULL); }
#define SR 48000

static void render(Session *s, double secs, float *L, float *R, unsigned blk)
{
    size_t n = (size_t)(secs * SR);
    float buf[1024 * 2];
    double acc = 0;
    for (size_t d = 0; d < n; d += blk) {
        mixer_render(s, buf, blk, 2);
        for (size_t i = 0; i < blk && d + i < n; ++i) { L[d + i] = buf[i * 2]; R[d + i] = buf[i * 2 + 1]; }
        acc += (double)blk / SR;
        if (acc > 0.016) { session_queue_tick(s); acc = 0; }
        nap((long)(1e6 * blk / SR));
    }
}
static double rms(const float *y, size_t a, size_t b)
{
    double e = 0; for (size_t i = a; i < b; ++i) e += (double)y[i] * y[i];
    return sqrt(e / (double)(b - a));
}
static long first_nz(const float *y, size_t n) { for (size_t i = 0; i < n; ++i) if (fabsf(y[i]) > 1e-4f) return (long)i; return -1; }
static long last_nz(const float *y, size_t n) { for (size_t i = n; i-- > 0; ) if (fabsf(y[i]) > 1e-4f) return (long)i; return -1; }
static void wait_peaks(Clip *c) { for (int i = 0; i < 500 && !(c->peaks && peaks_done(c->peaks)); ++i) nap(10000); }

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    static Session S; Session *s = &S;
    session_init(s, SR);
    streams_init();
    session_add_channel(s, NULL); session_add_channel(s, NULL);
    for (int i = 0; i < 4; ++i) session_add_scene(s);
    session_set_clip(s, 0, 0, argv[1]);
    session_set_clip(s, 1, 1, argv[2]);
    session_set_clip(s, 0, 2, argv[2]);
    atomic_store(&s->audio_running, false);
    wait_peaks(&s->channels[0].clips[0]); wait_peaks(&s->channels[1].clips[1]); wait_peaks(&s->channels[0].clips[2]);

    const double XF = 1.0, SECS = 7.0;
    size_t N = (size_t)(SECS * SR);
    float *L = calloc(N, sizeof(float)), *R = calloc(N, sizeof(float));
    session_set_crossfade(s, (float)XF);
    CHECK(fabsf(session_get_crossfade(s) - 1.0f) < 1e-3f, "crossfade setting = 1.0 s");

    /* ---- 1) different channels: A on the left, B on the right -> we see each one separately ---- */
    session_set_channel_pan(s, 0, -1.0f);
    session_set_channel_pan(s, 1, 1.0f);
    render(s, 0.02, L, R, 256);                       /* the silent channels: the pan settles in place */
    session_toggle_clip(s, 0, 0);
    nap(30000);
    CHECK(session_queue_add(s, 1, 1), "queued B");
    render(s, SECS, L, R, 256);
    long a0 = first_nz(L, N), a1 = last_nz(L, N), b0 = first_nz(R, N);
    double db0 = (double)(b0 - a0) / SR, da = (double)(a1 - a0) / SR;
    printf("   A starts %.4f s, A ends %.4f s after, B starts %.4f s after A\n", (double)a0 / SR, da, db0);
    CHECK(fabs(db0 - (4.0 - XF)) < 0.01, "B starts %.1f s before A's music ends (trailing silence skipped): %.4f s", XF, db0);
    CHECK(fabs(da - 4.0) < 0.01, "A fades out exactly at its music end: %.4f s", da);
    /* B has 0.5 s of silence in front: with skipping, the sound starts right away, without a gap */
    double rA = rms(L, a0 + SR / 2, a0 + 2 * SR), rB = rms(R, b0 + 2 * SR, b0 + 3 * SR);
    double worst = 0;
    for (long w = a0 + SR / 10; w + 960 < b0 + 2 * SR; w += 960) {
        double pa = rms(L, w, w + 960) / rA, pb = rms(R, w, w + 960) / rB;
        double db = 10.0 * log10(pa * pa + pb * pb);
        if (fabs(db) > fabs(worst)) worst = db;
    }
    CHECK(fabs(worst) < 1.0, "equal power during the crossfade (worst deviation %.2f dB)", worst);
    double mid = rms(R, b0 + (long)(XF * SR * 0.5) - 480, b0 + (long)(XF * SR * 0.5) + 480) / rB;
    CHECK(fabs(mid - 0.7071) < 0.06, "half-way the new one is at -3 dB (equal power curve): %.3f", mid);
    CHECK(!atomic_load(&s->channels[0].clips[0].playing) && atomic_load(&s->channels[1].clips[1].playing),
          "A stopped after the fade, B playing");
    session_stop_all(s); session_queue_tick(s);
    session_set_channel_pan(s, 0, 0.0f);
    session_set_channel_pan(s, 1, 0.0f);
    render(s, 0.05, L, R, 256);

    /* ---- 2) the same channel (A and B in channel 1): the transition must not stop A abruptly ---- */
    for (int warp = 0; warp <= 1; ++warp) {
        session_set_clip_warp(s, 0, 0, warp);
        session_set_clip_warp(s, 0, 2, warp);
        if (warp) {
            session_set_clip_bpm_fields(s, 0, 0, 120, 0, 0); session_set_clip_bpm_value(s, 0, 0, 120);
            session_set_clip_bpm_fields(s, 0, 2, 120, 0, 0); session_set_clip_bpm_value(s, 0, 2, 120);
        }
        memset(L, 0, N * sizeof(float));
        session_toggle_clip(s, 0, 0);
        nap(30000);
        CHECK(session_queue_add(s, 0, 2), "[warp %d] queued B in the same channel", warp);
        render(s, SECS, L, R, 256);
        long s0 = first_nz(L, N);
        double ref = rms(L, s0 + SR / 2, s0 + 2 * SR);
        double w2 = 0, low = 1e9; long wpos = -1;
        for (long w = s0 + SR / 5; w + 960 < s0 + 5 * SR; w += 960) {
            double p = rms(L, w, w + 960) / ref;
            double db = 20.0 * log10(p > 1e-9 ? p : 1e-9);
            if (fabs(db) > fabs(w2)) { w2 = db; wpos = w - s0; }
            if (p < low) low = p;
        }
        double lim = warp ? 1.5 : 1.0;
        if (fabs(w2) >= lim)
            printf("   worst window at %.3f s after start; underruns A=%u B=%u; A playing=%d B playing=%d\n",
                   (double)wpos / SR, s->channels[0].clips[0].stretch ? stretch_underruns(s->channels[0].clips[0].stretch) : 0,
                   s->channels[0].clips[2].stretch ? stretch_underruns(s->channels[0].clips[2].stretch) : 0,
                   atomic_load(&s->channels[0].clips[0].playing), atomic_load(&s->channels[0].clips[2].playing));
        CHECK(fabs(w2) < lim, "[warp %d] same channel: level stays even through the crossfade (worst %.2f dB)", warp, w2);
        CHECK(atomic_load(&s->channels[0].clips[2].playing) && !atomic_load(&s->channels[0].clips[0].playing),
              "[warp %d] B playing, A stopped", warp);
        session_stop_all(s); session_queue_tick(s);
        render(s, 0.05, L, R, 256);
    }
    session_set_clip_warp(s, 0, 0, false);
    session_set_clip_warp(s, 0, 2, false);

    /* ---- 3) manual play during the transition: cancels it (no "stuck" fade) ---- */
    session_toggle_clip(s, 0, 0);
    nap(30000);
    session_queue_add(s, 1, 1);
    render(s, 3.5, L, R, 256);                        /* in the middle of the transition */
    CHECK(atomic_load(&s->channels[0].clips[0].xf_dir) == -1, "mid-crossfade: A is fading out");
    session_toggle_clip(s, 0, 2);                      /* manual: plays B2 in channel 1 (stops A) */
    render(s, 0.5, L, R, 256);
    CHECK(atomic_load(&s->channels[0].clips[2].playing) && atomic_load(&s->channels[0].clips[2].xf_dir) == 0,
          "manual play during a crossfade: plays at full volume");
    session_stop_all(s); session_queue_tick(s);

    /* ---- 4) crossfade off = seamless, as before ---- */
    session_set_crossfade(s, 0.0f);
    CHECK(session_get_crossfade(s) == 0.0f, "crossfade off");

    free(L); free(R);
    session_free(s);
    printf(fails ? "XFADE: %d FAILED\n" : "XFADE: all ok\n", fails);
    return fails != 0;
}
