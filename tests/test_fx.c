/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Step 34: a plugin on the channel processes the audio (before volume/pan).
 * The test plugin QTestGain (DPF) in all 4 formats: output L = 0.5 * input R, output R = 0.5 * input L.
 * Two identical sessions: A without the plugin, B with it -> B.L must be 0.5 * A.R and vice versa.
 * Argument: the folder with qtest_gain.vst3 / qtest_gain-vst.so / qtest_gain.lv2 / qtest_gain.clap */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/fxchain.h"
#include "../src/project.h"
#include <math.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void nap(long us) { struct timespec t = { us / 1000000, (us % 1000000) * 1000 }; nanosleep(&t, NULL); }
#define N 96000
#define BLK 256

static void setup(Session *s, const char *file)
{
    session_init(s, 48000);
    session_add_channel(s, NULL);
    session_add_scene(s);
    atomic_store(&s->audio_running, false);
    session_set_clip(s, 0, 0, file);
    session_set_channel_gain(s, 0, 0.85f);
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: test_fx <dir with qtest_gain plugins>\n"); return 2; }
    const char *file = "/tmp/qlim_test_audio/fx.wav";
    if (system("mkdir -p /tmp/qlim_test_audio") != 0) return 2;
    SF_INFO in = { .samplerate = 48000, .channels = 2, .format = SF_FORMAT_WAV | SF_FORMAT_FLOAT };
    SNDFILE *f = sf_open(file, SFM_WRITE, &in);
    static float b[N * 2];
    for (long i = 0; i < N; ++i) { b[i * 2] = (float)(0.3 * sin(2 * M_PI * 220 * i / 48000.0)); b[i * 2 + 1] = (float)(0.2 * sin(2 * M_PI * 330 * i / 48000.0)); }
    sf_writef_float(f, b, N);
    sf_close(f);
    streams_init();

    const char *d = argv[1];
    char ids[4][1024];
    const char *fmts[4] = { "VST3", "VST", "LV2", "CLAP" };
    snprintf(ids[0], 1024, "%s/qtest_gain.vst3|2046504473616C63316E4771546D6C51", d);
    snprintf(ids[1], 1024, "%s/qtest_gain-vst.so", d);
    snprintf(ids[2], 1024, "%s/qtest_gain.lv2|urn:qlim:test:gain", d);
    snprintf(ids[3], 1024, "%s/qtest_gain.clap|qlim.test.gain", d);

    for (int t = 0; t < 4; ++t) {
        static Session A, B;
        setup(&A, file); setup(&B, file);
        char err[300] = "";
        QPlug *p = qplug_load(fmts[t], ids[t], 48000, 1024, err, sizeof err);
        CHECK(p != NULL, "%s: plugin loads (%s)", fmts[t], p ? "ok" : err);
        if (!p) { session_free(&A); session_free(&B); continue; }
        static FxChain fc;
        memset(&fc, 0, sizeof fc);
        fc.n = 1; fc.nactive = 1; fc.p[0] = p;
        atomic_store(&B.channels[0].fxchain, &fc);
        nap(200000);
        session_toggle_clip(&A, 0, 0); session_toggle_clip(&B, 0, 0);
        static float oa[BLK * 2], ob[BLK * 2];
        double maxd = 0, ea = 0, eb = 0;
        for (int blk = 0; blk < 200; ++blk) {
            mixer_render(&A, oa, BLK, 2);
            mixer_render(&B, ob, BLK, 2);
            for (int i = 0; i < BLK; ++i) {
                double dl = fabs(ob[i * 2] - 0.5 * oa[i * 2 + 1]), dr = fabs(ob[i * 2 + 1] - 0.5 * oa[i * 2]);
                if (dl > maxd) maxd = dl;
                if (dr > maxd) maxd = dr;
                ea += oa[i * 2] * oa[i * 2]; eb += ob[i * 2] * ob[i * 2];
            }
            nap(1000);
        }
        CHECK(ea > 1.0 && eb > 0.1 && maxd < 1e-5, "%s: sound goes through the plugin (L/R swapped x0.5) max diff %.2g", fmts[t], maxd);
        /* the plugin also runs when the channel is silent (echo tails) - no crash, silence stays silence */
        session_stop_all(&B);
        for (int blk = 0; blk < 40; ++blk) mixer_render(&B, ob, BLK, 2);
        double tail = 0;
        for (int i = 0; i < BLK * 2; ++i) tail += fabs(ob[i]);
        CHECK(tail < 1e-6, "%s: stopped channel -> silence through the plugin", fmts[t]);
        void *sd; size_t sn;
        int gs = qplug_get_state(p, &sd, &sn);
        CHECK(gs == 0 && sn > 0 && qplug_set_state(p, sd, sn) == 0, "%s: settings can be saved and loaded back (%zu bytes)", fmts[t], gs == 0 ? sn : 0);
        if (gs == 0) free(sd);
        atomic_store(&B.channels[0].fxchain, NULL);
        qplug_free(p);
        session_free(&A); session_free(&B);
    }
    /* settings in the project: base64 in chunks, save and load (10000 random bytes) */
    {
        static Session P;
        session_init(&P, 48000);
        session_add_channel(&P, NULL); session_add_scene(&P);
        session_fx_add(&P, 0, "VST3", "Big", "/x/big.vst3|00");
        unsigned char rnd[10000];
        for (int i = 0; i < 10000; ++i) rnd[i] = (unsigned char)(rand() & 255);
        int ref = fxstate_new();
        fxstate_append(ref, rnd, sizeof rnd);
        P.channels[0].fx[0].state_ref = ref;
        int sv = project_save(&P, "/tmp/qlim_test_audio/fxstate.limproject");
        session_free(&P);
        static Session Q;
        session_init(&Q, 48000);
        int ld = project_load(&Q, "/tmp/qlim_test_audio/fxstate.limproject");
        size_t n = 0;
        const void *d = Q.channel_count > 0 && Q.channels[0].fx_count > 0 ? fxstate_get(Q.channels[0].fx[0].state_ref, &n) : NULL;
        CHECK(sv == 0 && ld == 0 && d && n == sizeof rnd && !memcmp(d, rnd, n), "plugin settings survive project save + load (%zu bytes)", n);
        session_free(&Q);
    }
    printf(fails ? "FX: %d FAILED\n" : "FX: all ok\n", fails);
    return fails != 0;
}
