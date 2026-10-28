/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.2: instrument plugins - MIDI in, sound out, parameters (for the device panel), in all 4 formats.
 * Test instruments: Kars and Nekobi from DISTRHO DPF-Plugins (open source). Argument: the folder with
 * Kars.vst3, Kars-vst.so, Kars.lv2, Kars.clap, Nekobi.* */
#define _POSIX_C_SOURCE 200809L
#include "../src/plughost.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define BLK 256
#define SR 48000

static double rms_run(QPlug *p, int blocks, double *peak_out)
{
    static float l[BLK], r[BLK];
    double s = 0, pk = 0;
    for (int b = 0; b < blocks; ++b) {
        memset(l, 0, sizeof l); memset(r, 0, sizeof r);
        qplug_process(p, l, r, BLK);
        for (int i = 0; i < BLK; ++i) { s += l[i] * l[i] + r[i] * r[i]; if (fabs(l[i]) > pk) pk = fabs(l[i]); }
    }
    if (peak_out) *peak_out = pk;
    return sqrt(s / (2.0 * blocks * BLK));
}

static void note(QPlug *p, int key, int vel, unsigned frame)
{
    QMidiEv e = { frame, { (unsigned char)(vel ? 0x90 : 0x80), (unsigned char)key, (unsigned char)vel } };
    qplug_midi(p, &e, 1);
}

static void one(const char *fmt, const char *id, const char *what, bool sustains)
{
    char err[300] = "";
    QPlug *p = qplug_load(fmt, id, SR, BLK, err, sizeof err);
    CHECK(p != NULL, "[%s %s] loads (%s)", fmt, what, err);
    if (!p) return;
    CHECK(qplug_is_instrument(p), "[%s %s] seen as an instrument (no audio input)", fmt, what);
    CHECK(qplug_wants_midi(p), "[%s %s] takes MIDI", fmt, what);
    double pk;
    double quiet = rms_run(p, 20, &pk);
    CHECK(quiet < 1e-5, "[%s %s] silent without notes (%.6f)", fmt, what, quiet);
    note(p, 60, 110, 37);
    double on = rms_run(p, 40, &pk);
    CHECK(on > 0.003 && pk < 4.0, "[%s %s] note on -> sound (rms %.4f, peak %.3f)", fmt, what, on, pk);
    note(p, 60, 0, 0);
    rms_run(p, 400, NULL);                                  /* ~2 s of release */
    double after = rms_run(p, 20, NULL);
    if (sustains) CHECK(after < on * 0.05, "[%s %s] note off -> it fades (%.5f)", fmt, what, after);

    int n = qplug_param_count(p);
    CHECK(n > 0, "[%s %s] %d parameters", fmt, what, n);
    char name[128], txt[128];
    for (int i = 0; i < n && i < 3; ++i) {
        qplug_param_name(p, i, name, sizeof name);
        qplug_param_text(p, i, txt, sizeof txt);
        printf("      %d: '%s' = %.3f (%s)\n", i, name, qplug_param_get(p, i), txt);
    }
    if (n > 0) {
        qplug_param_name(p, n - 1, name, sizeof name);
        CHECK(name[0] != 0, "[%s %s] parameter names ('%s')", fmt, what, name);
        float before = qplug_param_get(p, n - 1);
        float want = before > 0.5f ? 0.1f : 0.9f;
        qplug_param_set(p, n - 1, want);
        rms_run(p, 2, NULL);                                 /* CLAP: the change goes in with the next block */
        float got = qplug_param_get(p, n - 1);
        CHECK(fabsf(got - want) < 0.06f, "[%s %s] set '%s' %.2f -> %.2f", fmt, what, name, before, got);
    }
    /* chord + sustain-free retrigger, then a very full block of events (no crash, no overflow) */
    QMidiEv many[QMIDI_MAX];
    for (int i = 0; i < QMIDI_MAX; ++i) { many[i].frame = (unsigned)(i % BLK); many[i].d[0] = (i & 1) ? 0x80 : 0x90; many[i].d[1] = (unsigned char)(40 + i % 40); many[i].d[2] = 90; }
    qplug_midi(p, many, QMIDI_MAX);
    qplug_midi(p, many, QMIDI_MAX);                          /* more than fits: the rest is dropped */
    double r2 = rms_run(p, 4, &pk);
    CHECK(isfinite(r2) && pk < 8.0, "[%s %s] 2 x %d events in one block: no overflow", fmt, what, QMIDI_MAX);
    qplug_free(p);
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: test_inst <folder with Kars/Nekobi>\n"); return 2; }
    const char *d = argv[1];
    char id[1200];
#ifdef _WIN32
#define V2EXT "-vst.dll"                                 /* Windows: no LV2 */
#else
#define V2EXT "-vst.so"
#endif
    snprintf(id, sizeof id, "%s/Kars.clap|studio.kx.distrho.Kars", d);                  one("CLAP", id, "Kars", true);
#ifndef _WIN32
    snprintf(id, sizeof id, "%s/Kars.lv2|http://distrho.sf.net/plugins/Kars", d);       one("LV2", id, "Kars", true);
#endif
    snprintf(id, sizeof id, "%s/Kars" V2EXT, d);                                         one("VST", id, "Kars", true);
    snprintf(id, sizeof id, "%s/Kars.vst3|2046504473616C6373724B4400000000", d);         one("VST3", id, "Kars", true);
    snprintf(id, sizeof id, "%s/Nekobi.clap|studio.kx.distrho.Nekobi", d);              one("CLAP", id, "Nekobi", false);
#ifndef _WIN32
    snprintf(id, sizeof id, "%s/Nekobi.lv2|http://distrho.sf.net/plugins/Nekobi", d);   one("LV2", id, "Nekobi", false);
#endif
    snprintf(id, sizeof id, "%s/Nekobi" V2EXT, d);                                       one("VST", id, "Nekobi", false);
    snprintf(id, sizeof id, "%s/Nekobi.vst3|2046504473616C636B654E4400000000", d);       one("VST3", id, "Nekobi", false);
    printf(fails ? "INST: %d FAILED\n" : "INST: all ok\n", fails);
    return fails != 0;
}
