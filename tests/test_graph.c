/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Mixer graph mode (step 28, PipeWire): channels go out separately, the master is a separate step,
 * and the routing is done by "links" (simulated here - exactly the way PipeWire sums the links into one input).
 * Check: the result is THE SAME as plain mixer_render (which does the routing internally).
 * Two identical sessions play the same files: one through mixer_render, the other through the graph path. */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
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
#define HW 6
#define NCH 3
#define BLK 256

static void write_tone(const char *path, double f1, double f2)
{
    SF_INFO in = { .samplerate = 48000, .channels = 2, .format = SF_FORMAT_WAV | SF_FORMAT_FLOAT };
    SNDFILE *f = sf_open(path, SFM_WRITE, &in);
    static float b[N * 2];
    for (long i = 0; i < N; ++i) {
        b[i * 2]     = (float)(0.2 * sin(2 * M_PI * f1 * i / 48000.0));
        b[i * 2 + 1] = (float)(0.2 * sin(2 * M_PI * f2 * i / 48000.0));
    }
    sf_writef_float(f, b, N);
    sf_close(f);
}

static void setup(Session *s, const char **files)
{
    session_init(s, 48000);
    for (int c = 0; c < NCH; ++c) session_add_channel(s, NULL);
    session_add_scene(s);
    atomic_store(&s->audio_running, false);
    for (int c = 0; c < NCH; ++c) session_set_clip(s, c, 0, files[c]);
    /* routes: channel 0 = default; 1 and 2 - mixed: master + directly to the outputs */
    session_route_set(s, 1, 0, ROUTE_MASTER_L | RBIT(2 + 3));
    session_route_set(s, 1, 1, RBIT(2 + 4));
    session_route_set(s, 2, 0, ROUTE_MASTER_R);
    session_route_set(s, 2, 1, ROUTE_MASTER_L | RBIT(2 + 0));
    session_master_route_set(s, 0, RBIT(0) | RBIT(2));
    session_master_route_set(s, 1, RBIT(1));
    session_set_channel_gain(s, 0, 0.85f);
    session_set_channel_gain(s, 1, 0.9f);
    session_set_channel_pan(s, 1, -0.4f);
    session_set_channel_gain(s, 2, 0.8f);
    atomic_store(&s->master_gain, 0.8f);
    atomic_store(&s->master_pan, 0.3f);
}

/* "PipeWire": sums the links according to the masks */
static void graph_render(Session *s, float *out, unsigned n)
{
    static float ch[NCH * 2][BLK], min_l[BLK], min_r[BLK], mo_l[BLK], mo_r[BLK], hw[HW][BLK];
    float *outs[NCH * 2];
    for (int k = 0; k < NCH * 2; ++k) outs[k] = ch[k];
    mixer_render_channels(s, outs, NCH * 2, n);
    memset(min_l, 0, sizeof min_l); memset(min_r, 0, sizeof min_r); memset(hw, 0, sizeof hw);
    for (int c = 0; c < NCH; ++c)
        for (int side = 0; side < 2; ++side) {
            RouteMask m = session_route_get(s, c, side);
            for (unsigned i = 0; i < n; ++i) {
                float v = ch[c * 2 + side][i];
                if (m & ROUTE_MASTER_L) min_l[i] += v;
                if (m & ROUTE_MASTER_R) min_r[i] += v;
                for (int k = 0; k < HW; ++k) if (m & RBIT(2 + k)) hw[k][i] += v;
            }
        }
    mixer_render_master(s, min_l, min_r, mo_l, mo_r, n);
    for (int side = 0; side < 2; ++side) {
        RouteMask m = session_master_route_get(s, side);
        for (int k = 0; k < HW; ++k) if (m & RBIT(k))
            for (unsigned i = 0; i < n; ++i) hw[k][i] += side ? mo_r[i] : mo_l[i];
    }
    for (unsigned i = 0; i < n; ++i)
        for (int k = 0; k < HW; ++k) {
            float v = hw[k][i];
            out[i * HW + k] = v > 1 ? 1 : v < -1 ? -1 : v;
        }
}

static double run(Session *a, Session *b, int blocks, double *energy)
{
    static float oa[BLK * HW], ob[BLK * HW];
    double maxd = 0; *energy = 0;
    for (int blk = 0; blk < blocks; ++blk) {
        mixer_render(a, oa, BLK, HW);
        graph_render(b, ob, BLK);
        for (int i = 0; i < BLK * HW; ++i) {
            double d = fabs((double)oa[i] - ob[i]);
            if (d > maxd) maxd = d;
            *energy += (double)oa[i] * oa[i];
        }
        nap(1200);
    }
    return maxd;
}

int main(void)
{
    if (system("mkdir -p /tmp/qlim_test_audio") != 0) return 2;
    const char *files[NCH] = { "/tmp/qlim_test_audio/g0.wav", "/tmp/qlim_test_audio/g1.wav", "/tmp/qlim_test_audio/g2.wav" };
    write_tone(files[0], 220, 330);
    write_tone(files[1], 440, 550);
    write_tone(files[2], 660, 770);
    streams_init();
    static Session A, B;
    setup(&A, files);
    setup(&B, files);
    nap(300000);
    double e;
    /* let the volume smoothing settle */
    run(&A, &B, 50, &e);

    for (int c = 0; c < NCH; ++c) { session_toggle_clip(&A, c, 0); session_toggle_clip(&B, c, 0); }
    double d = run(&A, &B, 150, &e);
    CHECK(e > 1.0 && d < 1e-5, "graph path = normal path (3 channels, mixed routes, master gain/balance) max diff %.2g, energy %.1f", d, e);

    /* mute + solo + volume change while playing (the smoothing) */
    session_toggle_mute(&A, 0); session_toggle_mute(&B, 0);
    session_set_channel_gain(&A, 1, 0.5f); session_set_channel_gain(&B, 1, 0.5f);
    atomic_store(&A.master_gain, 0.6f); atomic_store(&B.master_gain, 0.6f);
    d = run(&A, &B, 100, &e);
    CHECK(d < 1e-5, "mute + gain moves while playing: same (max diff %.2g)", d);
    session_toggle_solo(&A, 2); session_toggle_solo(&B, 2);
    d = run(&A, &B, 100, &e);
    CHECK(d < 1e-5 && e > 0.1, "solo: same (max diff %.2g)", d);
    atomic_store(&A.master_muted, true); atomic_store(&B.master_muted, true);
    d = run(&A, &B, 100, &e);
    CHECK(d < 1e-5, "master mute fades the same (max diff %.2g)", d);

    /* metering: the meters of both sessions must be equal */
    CHECK(fabsf(atomic_load(&A.channels[2].peak_level) - atomic_load(&B.channels[2].peak_level)) < 1e-6 &&
          fabsf(atomic_load(&A.master_peak_level) - atomic_load(&B.master_peak_level)) < 1e-6,
          "meters: channel and master peaks equal");
    /* NULL buffers and fewer ports than channels - no crash */
    float x[BLK], *few[3] = { x, NULL, x };
    mixer_render_channels(&B, few, 3, BLK);
    mixer_render_master(&B, NULL, NULL, x, NULL, BLK);
    CHECK(1, "NULL buffers / fewer ports than channels: no crash");

    session_free(&A);
    session_free(&B);
    printf(fails ? "GRAPH: %d FAILED\n" : "GRAPH: all ok\n", fails);
    return fails != 0;
}
