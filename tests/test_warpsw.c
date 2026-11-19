/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Turning warp (Rubber Band) on/off WHILE a clip plays: no click (no sample jump bigger than the sine's normal
 * step) and no audible hole (no run of silence). Also with a changed tempo.
 * 1.4.13: the same for the clip's pitch (on/off, changed live, with warp, dragged) + the pitch is really right. */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/stretch.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void nap(long us) { struct timespec t = { us / 1000000, (us % 1000000) * 1000 }; nanosleep(&t, NULL); }
#define BLK 256
static void render(Session *s, float *o, size_t n)
{
    float buf[BLK * 2];
    for (size_t d = 0; d < n; d += BLK) {
        mixer_render(s, buf, BLK, 2);
        for (size_t i = 0; i < BLK && d + i < n; ++i) o[d + i] = buf[i * 2];
        nap(5333);
    }
}
/* the frequency from the upward zero crossings (interpolated) */
static double freq_of(const float *y, size_t a, size_t b)
{
    double first = -1, last = -1; long cnt = 0;
    for (size_t i = a + 1; i < b; ++i)
        if (y[i - 1] < 0 && y[i] >= 0) { double t = (double)(i - 1) + (double)(-y[i - 1] / (y[i] - y[i - 1])); if (first < 0) first = t; last = t; cnt++; }
    return cnt > 2 ? (double)(cnt - 1) / ((last - first) / 48000.0) : 0.0;
}
static double max_step(const float *y, size_t a, size_t b, size_t *w)
{
    double m = 0; for (size_t i = a + 1; i < b; ++i) { double d = fabs(y[i] - y[i - 1]); if (d > m) { m = d; if (w) *w = i; } }
    return m;
}
/* the longest stretch where the sine's envelope is (almost) gone, in ms */
static double longest_hole_ms(const float *y, size_t a, size_t b)
{
    size_t run = 0, best = 0;
    for (size_t i = a; i < b; ++i) {
        double pk = 0; for (size_t k = i; k < i + 120 && k < b; ++k) if (fabs(y[k]) > pk) pk = fabs(y[k]);
        if (pk < 0.1) { run++; if (run > best) best = run; } else run = 0;
    }
    return best * 1000.0 / 48000.0;
}
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    static Session S; Session *s = &S;
    session_init(s, 48000); streams_init();
    session_add_channel(s, NULL); session_add_scene(s);
    atomic_store(&s->audio_running, false);
    session_set_clip(s, 0, 0, argv[1]);                 /* 440 Hz sine, 0.5 amplitude */
    nap(150000);
    Clip *cl = &s->channels[0].clips[0];
    if (!atomic_load(&cl->loop)) session_toggle_loop(s, 0, 0);
    session_set_clip_bpm_fields(s, 0, 0, 120, 0, 0);
    const size_t PRE = 94 * BLK, POST = 188 * BLK;      /* whole blocks: nothing rendered is thrown away */
    float *y = calloc(PRE + POST, sizeof(float));
    session_toggle_clip(s, 0, 0);
    render(s, y, PRE);
    double norm = max_step(y, 4800, PRE, NULL);
    struct { const char *name; int on; float bpm; } cases[] = {
        { "warp ON while playing (same tempo)", 1, 120 }, { "warp OFF while playing (same tempo)", 0, 120 },
        { "warp ON while playing (tempo 126)", 1, 126 },  { "warp OFF while playing (tempo 126)", 0, 126 },
    };
    for (int c = 0; c < 4; ++c) {
        if (cases[c].on) { session_set_clip_bpm_value(s, 0, 0, cases[c].bpm); }
        render(s, y, PRE);                               /* steady before the switch */
        session_set_clip_warp(s, 0, 0, cases[c].on);
        if (cases[c].on) session_set_clip_bpm_value(s, 0, 0, cases[c].bpm);
        render(s, y + PRE, POST);
        if (getenv("DUMP")) { char fn[64]; snprintf(fn,sizeof fn,"/tmp/wsw_%d.txt",c); FILE *f=fopen(fn,"w"); for (size_t i=PRE-300;i<PRE+1500;++i) fprintf(f,"%f\n",y[i]); fclose(f); }
        size_t w = 0;
        double m = max_step(y, PRE - 2400, PRE + POST, &w);
        double hole = longest_hole_ms(y, PRE - 2400, PRE + POST - 200);
        CHECK(m <= norm * 1.6, "%s: no click (max step %.4f, normal %.4f, at %+.1f ms)", cases[c].name, m, norm,
              ((double)w - PRE) * 1000.0 / 48000);
        CHECK(hole < (cases[c].on ? 20.0 : 5.0), "%s: only a short smooth dip (longest quiet part %.1f ms)", cases[c].name, hole);
    }
    /* warp on: drag the tempo while it plays (like dragging the BPM field), then hold */
    session_set_clip_warp(s, 0, 0, true);
    render(s, y, PRE);
    unsigned u0 = stretch_underruns(cl->stretch);
    float *z = calloc(48000 * 3, sizeof(float));
    size_t done = 0;
    for (int k = 0; k <= 40; ++k) {                       /* 120 -> 100 BPM in 40 small steps, one per ~1.3 blocks */
        session_set_clip_bpm_value(s, 0, 0, 120.0f - 0.5f * (float)k);
        render(s, z + done, 2 * BLK); done += 2 * BLK;
    }
    render(s, z + done, 48000 * 3 - done - (48000 * 3 - done) % BLK);
    double mz = max_step(z, 0, 48000 * 2, NULL);
    double hz = longest_hole_ms(z, 2400, 48000 * 2);
    unsigned und = stretch_underruns(cl->stretch) - u0;
    CHECK(mz <= norm * 1.6, "tempo dragged while playing: no crackle (max step %.4f, normal %.4f)", mz, norm);
    CHECK(hz < 3.0 && und == 0, "tempo dragged while playing: no dropouts (quiet %.1f ms, underruns %u)", hz, und);
    free(z);

    /* ===== 1.4.13: the clip's pitch ===== */
    session_set_clip_warp(s, 0, 0, false);
    render(s, y, PRE);
    struct { const char *name; float pitch; int warp; } pc[] = {
        { "pitch 0 -> +3 while playing (warp off)", 3, 0 },
        { "pitch +3 -> +5 while playing (live change)", 5, 0 },
        { "warp ON with pitch +5 (tempo 126)", 5, 1 },
        { "pitch +5 -> 0 with warp on (pitch engine -> Rubber Band)", 0, 1 },
        { "pitch 0 -> -2 with warp on (Rubber Band -> pitch engine)", -2, 1 },
        { "warp OFF with pitch -2", -2, 0 },
        { "pitch -2 -> 0 while playing (warp off: straight from the file again)", 0, 0 },
    };
    for (int c = 0; c < (int)(sizeof pc / sizeof *pc); ++c) {
        render(s, y, PRE);
        float before = session_get_clip_pitch(s, 0, 0);
        double fmax = pow(2.0, (fabsf(before) > fabsf(pc[c].pitch) ? fabsf(before) : fabsf(pc[c].pitch)) / 12.0);
        if (pc[c].warp != (int)session_get_clip_warp(s, 0, 0)) {
            session_set_clip_bpm_value(s, 0, 0, 126);
            session_set_clip_warp(s, 0, 0, pc[c].warp);
        }
        session_set_clip_pitch(s, 0, 0, pc[c].pitch);
        render(s, y + PRE, POST);
        size_t w = 0;
        /* (only around the switch: at the loop seam the test sine dips - the loop crossfade blends into the silence
         * before the file - and a spectral pitch shift answers that dip with a small, smooth swell) */
        double m = max_step(y, PRE - 2400, PRE + 9600, &w);
        double hole = longest_hole_ms(y, PRE - 2400, PRE + POST - 200);
        double f = freq_of(y, PRE + POST / 2, PRE + POST);
        double want = 440.0 * pow(2.0, pc[c].pitch / 12.0);
        CHECK(m <= norm * fmax * 1.6, "%s: no click (max step %.4f, normal %.4f, at %+.1f ms)", pc[c].name, m, norm * fmax,
              ((double)w - PRE) * 1000.0 / 48000);
        if (getenv("DUMP")) { char fn[64]; snprintf(fn, sizeof fn, "/tmp/psw_%d.txt", c); FILE *f = fopen(fn, "w"); for (size_t i = 0; i < PRE + POST; ++i) fprintf(f, "%f\n", y[i]); fclose(f); }
        CHECK(hole < 30.0, "%s: only a short smooth dip (longest quiet part %.1f ms)", pc[c].name, hole);
        CHECK(fabs(1200.0 * log2(f / want)) < 1.0, "%s: plays at %.3f Hz (wanted %.3f Hz)", pc[c].name, f, want);
    }
    /* drag the pitch while it plays (like dragging the field with Shift: 1 cent steps), then hold */
    unsigned u1 = stretch_underruns_total();
    done = 0;
    z = calloc(48000 * 3, sizeof(float));
    session_set_clip_pitch(s, 0, 0, 0.5f);
    render(s, y, PRE);
    for (int k = 0; k <= 100; ++k) {                      /* +0.5 -> +1.5 semitones */
        session_set_clip_pitch(s, 0, 0, 0.5f + 0.01f * (float)k);
        render(s, z + done, BLK); done += BLK;
    }
    render(s, z + done, 48000 * 3 - done - (48000 * 3 - done) % BLK);
    mz = max_step(z, 0, 48000 * 2, NULL);
    hz = longest_hole_ms(z, 0, 48000 * 2);
    und = stretch_underruns_total() - u1;
    double fz = freq_of(z, 48000 * 2, 48000 * 3 - BLK);
    CHECK(mz <= norm * pow(2.0, 1.5 / 12.0) * 1.6, "pitch dragged while playing: no crackle (max step %.4f)", mz);
    CHECK(hz < 3.0 && und == 0, "pitch dragged while playing: no dropouts (quiet %.1f ms, underruns %u)", hz, und);
    CHECK(fabs(1200.0 * log2(fz / (440.0 * pow(2.0, 1.5 / 12.0)))) < 1.0, "after the drag it holds +1.50 st exactly (%.3f Hz)", fz);
    free(z);
    session_stop_all(s);
    printf("WARPSW: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(y);
    return fails ? 1 : 0;
}
