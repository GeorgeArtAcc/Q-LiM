/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Step 36:
 *  1) the level: channel 0 dB + master 0 dB + centre pan -> the output is EXACTLY what is in the file
 *     (frame by frame, both L and R). -20 dB on the fader -> exactly -20 dB.
 *  2) the fader (Bitwig): 0.8 = 0 dB, 1.0 = +6 dB, reversibility, monotonicity; old projects are converted.
 *  3) moving channels (session_move_channel): clips and playback move with the channel; undo restores the order. */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/project.h"
#include "../src/undo.h"
#include <math.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void nap(long us) { struct timespec t = { us / 1000000, (us % 1000000) * 1000 }; nanosleep(&t, NULL); }

#define N 960000
static float vl(long f) { return 0.1f * sinf((float)f * 0.0573f); }              /* -20 dBFS sine */
static float vr(long f) { return 0.1f * sinf((float)f * 0.0311f + 1.0f); }       /* a different one on the right */

static void write_file(const char *path)
{
    SF_INFO in = { .samplerate = 48000, .channels = 2, .format = SF_FORMAT_WAV | SF_FORMAT_FLOAT };
    SNDFILE *f = sf_open(path, SFM_WRITE, &in);
    static float b[N * 2];
    for (long i = 0; i < N; ++i) { b[i * 2] = vl(i); b[i * 2 + 1] = vr(i); }
    sf_writef_float(f, b, N);
    sf_close(f);
}

/* plays `frames` frames and returns L,R */
static float *play(Session *s, size_t frames)
{
    size_t n = (frames + 255) / 256 * 256;
    float *o = calloc(n * 2, sizeof(float)), buf[256 * 2];
    for (size_t d = 0; d < n; d += 256) {
        mixer_render(s, buf, 256, 2);
        memcpy(o + d * 2, buf, sizeof buf);
        nap(1500);
    }
    return o;
}

/* the largest difference between the output and the file * g (from frame `from`; the output is shifted from the file by off) */
static long g_guess = 0;      /* approximate offset (file frame = output frame - off) */
static double max_err(const float *y, size_t from, size_t n, double gl, double gr, long *off_out)
{
    /* find the offset: where in the file frame `from` is */
    long best = -1; double be = 1e9;
    for (long off = g_guess - 4096; off < g_guess + 4096; ++off) {
        double e = 0;
        for (size_t i = 0; i < 64; ++i) e += fabs(y[(from + i) * 2] - gl * vl((long)(from + i) - off));
        if (e < be) { be = e; best = off; }
    }
    *off_out = best;
    double m = 0;
    for (size_t i = from; i < from + n; ++i) {
        double a = fabs(y[i * 2] - gl * vl((long)i - best));
        double b = fabs(y[i * 2 + 1] - gr * vr((long)i - best));
        if (a > m) m = a;
        if (b > m) m = b;
    }
    return m;
}

int main(void)
{
    const char *path = "/tmp/qlim_test_audio/level.wav";
    if (system("mkdir -p /tmp/qlim_test_audio") != 0) return 2;
    write_file(path);

    /* ---- 2) the fader ---- */
    CHECK(session_pos_to_gain(FADER_UNITY_POS) == 1.0f, "fader 0.8 = gain exactly 1.0");
    CHECK(fabsf(session_gain_to_db(1.0f) - 6.0f) < 1e-4f, "fader top = +6 dB (%.3f)", session_gain_to_db(1.0f));
    CHECK(session_pos_to_gain(0.0f) == 0.0f, "fader bottom = silence");
    {
        bool mono = true, inv = true;
        float prev = -2000.0f, worst = 0;
        for (int i = 1; i <= 1000; ++i) {
            float p = (float)i / 1000.0f, db = session_gain_to_db(p);
            if (db <= prev) mono = false;
            prev = db;
            float back = session_db_to_pos(db);
            if (fabsf(back - p) > worst) worst = fabsf(back - p);
            if (fabsf(back - p) > 2e-4f) inv = false;
        }
        CHECK(mono, "fader is monotonic");
        CHECK(inv, "dB -> position -> dB is reversible (worst %.2g)", worst);
    }
    float p20 = session_db_to_pos(-20.0f);
    CHECK(fabsf(20.0f * log10f(session_pos_to_gain(p20)) + 20.0f) < 0.001f, "-20 dB position %.4f gives -20 dB", p20);
    CHECK(session_old_pos_to_pos(0.909f) == FADER_UNITY_POS, "old project 0 dB (0.909) -> exactly 0 dB");
    CHECK(fabsf(session_gain_to_db(session_old_pos_to_pos(0.5f)) - (0.5f * 66.0f - 60.0f)) < 0.01f,
          "old project -27 dB stays -27 dB (%.2f)", session_gain_to_db(session_old_pos_to_pos(0.5f)));

    /* ---- 1) the level ---- */
    static Session S; Session *s = &S;
    session_init(s, 48000);
    streams_init();
    session_add_channel(s, "One");
    session_add_channel(s, "Two");
    session_add_scene(s);
    session_add_scene(s);
    atomic_store(&s->audio_running, false);
    CHECK(atomic_load(&s->channels[1].gain) == FADER_UNITY_POS && atomic_load(&s->master_gain) == FADER_UNITY_POS,
          "new channel and master start at 0 dB");
    CHECK(session_set_clip(s, 1, 0, path) == 0, "file loaded on channel 2");
    nap(200000);
    for (int i = 0; i < 50; ++i) mixer_render(s, (float[512]){0}, 256, 2);
    session_toggle_clip(s, 1, 0);
    float *y = play(s, 30000);
    long off;
    double e = max_err(y, 2048, 20000, 1.0, 1.0, &off);
    CHECK(e < 1e-6, "0 dB channel + 0 dB master + centre pan: output = file, L and R (max err %.2g)", e);
    free(y);

    session_set_channel_gain(s, 1, p20);
    for (int i = 0; i < 400; ++i) mixer_render(s, (float[512]){0}, 256, 2);     /* the smoothing */
    g_guess = -(long)atomic_load(&s->channels[1].clips[0].position);
    y = play(s, 20000);
    e = max_err(y, 2048, 15000, 0.1, 0.1, &off);
    CHECK(e < 1e-5, "-20 dB on the fader = exactly -20 dB out (max err %.2g)", e);
    free(y);

    session_set_channel_gain(s, 1, FADER_UNITY_POS);
    session_set_channel_pan(s, 1, 1.0f);
    for (int i = 0; i < 400; ++i) mixer_render(s, (float[512]){0}, 256, 2);
    g_guess = -(long)atomic_load(&s->channels[1].clips[0].position);
    y = play(s, 8000);
    double lmax = 0, rerr = 0;
    for (size_t i = 2048; i < 7000; ++i) { if (fabs(y[i * 2]) > lmax) lmax = fabs(y[i * 2]); }
    { long best = -1; double be = 1e9;
      for (long k = g_guess - 4096; k < g_guess + 4096; ++k) { double ee = 0; for (int i = 0; i < 64; ++i) ee += fabs(y[(2048 + i) * 2 + 1] - vr(2048 + i - k)); if (ee < be) { be = ee; best = k; } }
      for (size_t i = 2048; i < 7000; ++i) { double d = fabs(y[i * 2 + 1] - vr((long)i - best)); if (d > rerr) rerr = d; } }
    CHECK(lmax < 1e-6 && rerr < 1e-6, "pan hard right: L silent (%.2g), R unchanged (err %.2g)", lmax, rerr);
    free(y);
    session_set_channel_pan(s, 1, 0.0f);

    /* ---- 3) moving channels ---- */
    for (int i = 0; i < 400; ++i) mixer_render(s, (float[512]){0}, 256, 2);
    undo_reset(s);
    session_add_channel(s, "Three");
    session_add_channel(s, "Four");
    undo_commit_now(s);
    CHECK(atomic_load(&s->channels[1].clips[0].playing), "channel 2 is playing before the move");
    CHECK(session_move_channel(s, 1, 3), "move channel 2 to place 4");
    CHECK(!strcmp(s->channels[0].name, "One") && !strcmp(s->channels[1].name, "Three") &&
          !strcmp(s->channels[2].name, "Four") && !strcmp(s->channels[3].name, "Two"), "order is One Three Four Two");
    CHECK(atomic_load(&s->channels[3].clips[0].has_clip) && atomic_load(&s->channels[3].clips[0].playing),
          "the clip moved with the channel and still plays");
    g_guess = -(long)atomic_load(&s->channels[3].clips[0].position);
    y = play(s, 12000);
    e = max_err(y, 4096, 6000, 1.0, 1.0, &off);
    CHECK(e < 1e-6, "sound continues exactly after the move (max err %.2g)", e);
    free(y);
    undo_commit_now(s);
    CHECK(undo_undo(s), "undo");
    CHECK(!strcmp(s->channels[1].name, "Two") && !strcmp(s->channels[3].name, "Four") &&
          atomic_load(&s->channels[1].clips[0].has_clip), "undo puts channel 2 back (with its clip)");
    CHECK(atomic_load(&s->channels[1].clips[0].playing), "...and it did not stop playing");
    CHECK(session_move_channel(s, 3, 0) && !strcmp(s->channels[0].name, "Four") && !strcmp(s->channels[1].name, "One"),
          "move the last channel to the front");
    CHECK(!session_move_channel(s, 0, 0) && !session_move_channel(s, 0, 9), "bad moves are refused");

    /* ---- project: save (version 4) and an old project (version 3) ---- */
    const char *pp = "/tmp/qlim_test_audio/level_v4.qlim";
    session_set_channel_gain(s, 1, p20);
    CHECK(project_save(s, pp) == 0, "project saved");
    static Session T; session_init(&T, 48000);
    CHECK(project_load(&T, pp) == 0, "project loaded");
    CHECK(fabsf(atomic_load(&T.channels[1].gain) - p20) < 1e-4f && atomic_load(&T.master_gain) == FADER_UNITY_POS,
          "new project keeps the fader positions");
    FILE *f = fopen("/tmp/qlim_test_audio/level_v3.qlim", "w");
    fprintf(f, "# Q-LiM project\n[meta]\nversion=3\nsample_rate=48000\nchannel_count=2\nscene_count=1\nmaster_gain=0.9090\n"
               "[channel 0]\nname=A\ngain=0.9090\n[channel 1]\nname=B\ngain=0.6000\n");
    fclose(f);
    static Session U; session_init(&U, 48000);
    CHECK(project_load(&U, "/tmp/qlim_test_audio/level_v3.qlim") == 0, "old project loaded");
    CHECK(atomic_load(&U.master_gain) == FADER_UNITY_POS && atomic_load(&U.channels[0].gain) == FADER_UNITY_POS,
          "old 0 dB faders -> exactly 0 dB");
    CHECK(fabsf(session_gain_to_db(atomic_load(&U.channels[1].gain)) - (0.6f * 66.0f - 60.0f)) < 0.01f,
          "old -20.4 dB fader keeps its level (%.2f dB)", session_gain_to_db(atomic_load(&U.channels[1].gain)));

    session_stop_all(s);
    nap(50000);
    printf("LEVEL: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
