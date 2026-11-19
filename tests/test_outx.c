/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.21: extra sound cards (outx.c) - two clocks that drift apart, different rates, irregular (Bluetooth-like)
 * blocks, the delays of the outputs, the configuration file. Everything simulated in time (no sound card). */
#define _POSIX_C_SOURCE 200809L
#include "../src/outx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* stubs (outx.c links to the mixer and the settings; not needed here) */
void mixer_render(Session *s, float *out, unsigned long frames, int out_ch) { (void)s; memset(out, 0, frames * (size_t)out_ch * sizeof(float)); }
void settings_get_config_path(char *out, size_t n) { snprintf(out, n, "/tmp/qlim_outx_test/audio.conf"); }
int settings_ensure_dir(const char *p) { (void)p; return 0; }

/* the "mixer": a sine on every output k (frequency 500 + 250 k Hz, main clock), or an impulse */
static double g_t;                 /* main frames rendered */
static int    g_imp = 0;           /* 1 = impulses: one every 4800 frames on all outputs */
static void fake_render(float *out, unsigned n, int nch, void *u)
{
    (void)u;
    for (unsigned i = 0; i < n; ++i) {
        double t = g_t + i;
        for (int k = 0; k < nch; ++k) {
            float v;
            if (g_imp) v = ((long)t % 4800 == 0) ? 1.0f : 0.0f;
            else v = 0.5f * (float)sin(2 * M_PI * (500.0 + 250.0 * k) * t / 48000.0);
            out[(size_t)i * nch + k] = v;
        }
    }
    g_t += n;
}

/* SNR of a sine of (unknown, near f0) frequency in x[0..n): best least-squares fit over a small frequency search */
static double fit_snr(const float *x, int n, int stride, double f)
{
    {
        double sc = 0, ss = 0, cc = 0, xs = 0, xc = 0, sx = 0;
        for (int i = 0; i < n; ++i) {
            double s = sin(2 * M_PI * f * i), c = cos(2 * M_PI * f * i), v = x[(size_t)i * stride];
            ss += s * s; cc += c * c; sc += s * c; xs += v * s; xc += v * c; sx += v * v;
        }
        double det = ss * cc - sc * sc;
        double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
        double fit = a * xs + b * xc;
        double res = sx - fit;
        return 10 * log10(fit / (res > 1e-30 ? res : 1e-30));
    }
}
static double sine_snr(const float *x, int n, int stride, double f0)
{
    double best = -1, bf = f0;
    for (int it = -120; it <= 120; ++it) { double f = f0 * (1.0 + it * 5e-6); double v = fit_snr(x, n, stride, f); if (v > best) { best = v; bf = f; } }
    double c = bf;
    for (int it = -100; it <= 100; ++it) { double f = c * (1.0 + it * 5e-8); double v = fit_snr(x, n, stride, f); if (v > best) { best = v; bf = f; } }
    return best;
}
static double g_simt;
static double sim_clock(void) { return g_simt; }

/* runs main (48000 * (1+0) frames/s, block bm) and the extra card (dev_rate * (1 + ppm), blocks from bd_fn) for secs */
typedef unsigned (*BlockFn)(unsigned j);
static unsigned blk_441(unsigned j) { (void)j; return 441; }
static unsigned blk_bt(unsigned j) { static unsigned x = 12345; (void)j; x = x * 1103515245u + 12345u; return 512 + (x >> 8) % 3585; }  /* 512..4096 */
static unsigned blk_256(unsigned j) { (void)j; return 256; }

static void run(const char *name, int dev_rate, double ppm, unsigned bm, BlockFn bd_fn, double secs, double min_snr)
{
    outx_engine_stop();
    g_t = 0; g_imp = 0;
    outx_engine_start(48000, 2, true);
    outx_slot_attach(0, dev_rate, 2);
    double dev_clock = dev_rate * (1.0 + ppm * 1e-6);    /* its real speed */
    double tm = 0, td = 0;                               /* the time of the next callback of each */
    unsigned j = 0;
    static float mo[8192 * 2], dv[16384 * 2];
    size_t cap = (size_t)(secs * dev_rate) + 20000, got = 0;
    float *rec = malloc(cap * 2 * sizeof(float));
    unsigned u_after = 0, u0 = 0;
    double lat_min = 1e9, lat_max = 0;
    while (tm < secs || td < secs) {
        /* the callbacks come late by a random 0..2 ms (an ordinary desktop) - their audio does not move, only the call */
        static unsigned jx = 777;
        if (tm <= td) { jx = jx * 1103515245u + 12345u; g_simt = tm + (jx >> 16) % 2000 * 1e-6; outx_render(NULL, mo, bm, 2); tm += bm / 48000.0; }
        else {
            jx = jx * 1103515245u + 12345u;
            g_simt = td + (jx >> 16) % 2000 * 1e-6;
            unsigned n = bd_fn(j++);
            outx_pull(0, dv, n, 2);
            if (got + n <= cap) { memcpy(rec + got * 2, dv, n * 2 * sizeof(float)); got += n; }
            td += n / dev_clock;
            OutxStat st; outx_slot_stat(0, &st);
            if (td > 10.0 && td < 10.0 + n / dev_clock + 1e-9) u0 = st.underruns;   /* after 10 s settled */
            if (td > (secs > 200 ? 150.0 : 40.0)) { if (st.latency_ms < lat_min) lat_min = st.latency_ms; if (st.latency_ms > lat_max) lat_max = st.latency_ms; }
        }
    }
    OutxStat st; outx_slot_stat(0, &st);
    u_after = st.underruns - u0;
    /* outputs 3-4 of the mixer are on the extra card: 1000 Hz and 1250 Hz at the main clock */
    double f_main = 1000.0 / 48000.0;                     /* cycles per main frame */
    double f_dev = f_main * 48000.0 / dev_clock * (dev_rate * (1.0 + ppm * 1e-6)) / dev_rate;   /* per device frame */
    f_dev = 1000.0 / dev_clock;                           /* the device frame lasts 1/dev_clock s */
    int nfit = dev_rate / 4;
    size_t at = got > (size_t)nfit + 100 ? got - (size_t)nfit - 50 : 0;
    double snr = sine_snr(rec + at * 2, nfit, 2, f_dev);
    printf("   [%s] underruns after 10 s: %u (total %u), ring latency %.1f..%.1f ms, clock found %.0f ppm (real %.0f), SNR %.1f dB\n",
           name, u_after, st.underruns, lat_min, lat_max, st.ppm, ppm, snr);
    CHECK(u_after == 0, "%s: no dropouts once running", name);
    CHECK(lat_max - lat_min < 2.0, "%s: the latency is steady after 40 s (%.2f ms)", name, lat_max - lat_min);
    CHECK(fabs(st.ppm - ppm) < 30.0, "%s: the clock difference found (%.1f ppm, the rest is in the P part)", name, st.ppm);
    CHECK(snr > min_snr, "%s: clean (SNR %.1f dB > %.0f)", name, snr, min_snr);
    double drift_ok = fabs(st.ppm - (dev_rate == 48000 ? ppm : st.ppm));
    (void)drift_ok;
    free(rec);
    outx_slot_detach(0);
    outx_slot_free(0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (system("mkdir -p /tmp/qlim_outx_test && rm -f /tmp/qlim_outx_test/outputs.conf")) {}
    /* 1) the configuration */
    outx_load();
    CHECK(outx_cfg_count() == 0 && !outx_any_delay(), "nothing configured at first");
    OutxDevCfg c = { 0 };
    c.kind = OUTX_KIND_PW; c.channels = 2;
    snprintf(c.name, sizeof c.name, "bluez_output.AA_BB.1"); snprintf(c.desc, sizeof c.desc, "JBL Flip 5");
    CHECK(outx_cfg_add(&c) == 0, "a Bluetooth speaker added");
    c.kind = OUTX_KIND_PA; c.channels = 4;
    snprintf(c.api, sizeof c.api, "ALSA"); snprintf(c.name, sizeof c.name, "HDA Intel PCH: ALC233 Analog (hw:3,0)");
    snprintf(c.desc, sizeof c.desc, "Headphones");
    outx_cfg_add(&c);
    CHECK(outx_first(0, 2) == 2 && outx_first(1, 2) == 4 && outx_total(2) == 8, "numbering: main 1-2, the speaker 3-4, the card 5-8");
    CHECK(outx_first(1, 8) == 10 && outx_total(60) == 62, "after an 8-output card: 11-14; never past 62");
    outx_delay_set(0, 120.5f); outx_delay_set(4, 3000.0f);
    CHECK(fabsf(outx_delay_get(0) - 120.5f) < 1e-3f && outx_delay_get(4) == OUTX_DELAY_MAX_MS, "delays (capped at 2000 ms)");
    outx_save();
    outx_cfg_remove(0); outx_cfg_remove(0); outx_delay_set(0, 0); outx_delay_set(4, 0);
    outx_load();
    const OutxDevCfg *r0 = outx_cfg_get(0), *r1 = outx_cfg_get(1);
    CHECK(outx_cfg_count() == 2 && r0 && r0->kind == OUTX_KIND_PW && !strcmp(r0->desc, "JBL Flip 5") && r1 && r1->channels == 4 &&
          !strcmp(r1->name, "HDA Intel PCH: ALC233 Analog (hw:3,0)") && fabsf(outx_delay_get(0) - 120.5f) < 0.01f,
          "saved and loaded back");
    outx_cfg_remove(1);
    outx_delay_set(0, 0); outx_delay_set(4, 0);

    outx_test_set_render(fake_render, NULL);
    outx_test_set_clock(sim_clock);

    /* 2) nothing configured -> the same path as before */
    {
        outx_cfg_remove(0);
        outx_engine_start(48000, 2, true);
        static float o[512 * 2];
        g_t = 0;
        outx_render(NULL, o, 512, 2);
        CHECK(fabsf(o[2 * 100] - 0.5f * (float)sin(2 * M_PI * 500.0 * 100 / 48000.0)) < 1e-6f, "no extra card, no delay: untouched");
        outx_engine_stop();
        c.kind = OUTX_KIND_PW; c.channels = 2;
        outx_cfg_add(&c);
    }

    /* 3) the delays: an impulse every 4800 frames; output 1 delayed 10 ms = 480 frames */
    {
        outx_engine_start(48000, 2, true);
        g_t = 0; g_imp = 1;
        outx_delay_set(0, 10.0f);
        static float o[9600 * 2];
        outx_render(NULL, o, 4800, 2);
        outx_render(NULL, o + 4800 * 2, 4800, 2);
        int p0 = -1, p1 = -1;
        for (int i = 4800; i < 9600; ++i) { if (o[i * 2] > 0.5f && p0 < 0) p0 = i; if (o[i * 2 + 1] > 0.5f && p1 < 0) p1 = i; }
        CHECK(p1 == 4800 && p0 == 4800 + 480, "output 1 is 480 frames late, output 2 is not (%d / %d)", p0, p1);
        outx_delay_set(0, 20.0f);                 /* change: a glide, then 960 */
        for (int b = 0; b < 3; ++b) outx_render(NULL, o, 4800, 2);
        outx_render(NULL, o, 4800, 2);
        p0 = -1;
        for (int i = 0; i < 4800; ++i) if (o[i * 2] > 0.5f && p0 < 0) p0 = i;
        CHECK(p0 == 960, "after a change: 960 frames (%d)", p0);
        outx_delay_set(0, 0);
        outx_engine_stop();
        g_imp = 0;
    }

    /* 4) an extra card: same rate, its clock 150 ppm fast, its blocks 441 (not the main's 256) */
    run("48k +150 ppm", 48000, 150.0, 256, blk_441, 120.0, 72.0);
    /* 5) its clock slow, other blocks */
    run("48k -320 ppm", 48000, -320.0, 512, blk_256, 120.0, 72.0);
    /* 6) another rate: 44.1 kHz (+80 ppm) */
    run("44.1k +80 ppm", 44100, 80.0, 256, blk_441, 120.0, 70.0);
    /* 7) Bluetooth-like: irregular large blocks 512..4096, 48 kHz, -90 ppm */
    run("BT irregular", 48000, -90.0, 256, blk_bt, 120.0, 70.0);
    /* 8) a very bad clock (+700 ppm), small blocks */
    run("+700 ppm", 48000, 700.0, 128, blk_256, 300.0, 70.0);

    printf("OUTX: %s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
