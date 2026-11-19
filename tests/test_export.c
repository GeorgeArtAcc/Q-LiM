/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.14: audio export (src/export.c): markers, cuts, gain/pan, warp, pitch, sample rate, every format, dither,
 * the background job with progress and cancel. The results are read back and measured. */
#define _POSIX_C_SOURCE 200809L
#include "export.h"
#include "edit.h"
#include <sndfile.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_fail = 0;
#define CHECK(c, ...) do { if (c) { printf("ok   "); } else { printf("FAIL "); g_fail = 1; } printf(__VA_ARGS__); printf("\n"); } while (0)

#define SR 48000
#define SECS 4
static const char *DIR_ = "/tmp/qlim_export";

/* export.c's job needs these from session.c; the tests use export_clip_now / export_file_start */
int session_clip_snapshot(Session *s, int ch, int scene, ClipSnapshot *o) { (void)s; (void)ch; (void)scene; (void)o; return 1; }
int session_open_snapshot_source(Session *s, const ClipSnapshot *c, StreamSource *o) { (void)s; (void)c; (void)o; return 1; }
void session_stop_all(Session *s) { (void)s; }
void session_play_scene_now(Session *s, int scene) { (void)s; (void)scene; }
/* 1.4.23 (the surround export is tested in test_surround) */
int wav_cache_path(const char *path, const char *kind, char *out, size_t n) { (void)path; (void)kind; snprintf(out, n, "/tmp/x"); return 0; }
const char *wav_readable_all_channels(const char *path, int sr, char *buf, size_t n) { (void)sr; (void)buf; (void)n; return path; }
bool session_get_surround(Session *s, int ch, SurSetup *out) { (void)s; (void)ch; (void)out; return false; }
int  session_clip_get_sur_points(Session *s, int ch, int sc, SurPt *out, int max) { (void)s; (void)ch; (void)sc; (void)out; (void)max; return 0; }
RouteMask session_route_get(Session *s, int ch, int side) { (void)s; (void)ch; (void)side; return 0; }
RouteMask session_master_route_get(Session *s, int side) { (void)s; (void)side; return 0; }

/* the source: 440 Hz left, 660 Hz right, -6 dBFS */
static void make_source(const char *path)
{
    SF_INFO i = { 0 };
    i.samplerate = SR; i.channels = 2; i.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
    SNDFILE *f = sf_open(path, SFM_WRITE, &i);
    float *b = malloc((size_t)SR * SECS * 2 * sizeof(float));
    for (int n = 0; n < SR * SECS; ++n) {
        b[n * 2] = 0.5f * (float)sin(2 * M_PI * 440.0 * n / SR);
        b[n * 2 + 1] = 0.5f * (float)sin(2 * M_PI * 660.0 * n / SR);
    }
    sf_writef_float(f, b, SR * SECS);
    sf_close(f);
    free(b);
}

typedef struct { float *x; long n; int sr; int format; } Audio;
static Audio load(const char *path)
{
    Audio a = { 0 };
    SF_INFO i = { 0 };
    SNDFILE *f = sf_open(path, SFM_READ, &i);
    if (!f) return a;
    a.sr = i.samplerate; a.format = i.format;
    a.x = malloc((size_t)(i.frames + 1) * 2 * sizeof(float));
    a.n = (long)sf_readf_float(f, a.x, i.frames);
    sf_close(f);
    return a;
}

/* frequency of channel ch in [a, b) frames, by zero crossings (with interpolation) */
static double freq(const Audio *a, int ch, long s0, long s1)
{
    double first = -1, last = -1; int cnt = 0;
    for (long i = s0 + 1; i < s1 && i < a->n; ++i) {
        float p = a->x[(i - 1) * 2 + ch], q = a->x[i * 2 + ch];
        if (p < 0 && q >= 0) {
            double t = (double)(i - 1) + p / (p - q);
            if (first < 0) first = t; else ++cnt;
            last = t;
        }
    }
    return cnt > 0 ? cnt * (double)a->sr / (last - first) : 0.0;
}
static double rms(const Audio *a, int ch, long s0, long s1)
{
    double e = 0; long k = 0;
    for (long i = s0; i < s1 && i < a->n; ++i) { e += (double)a->x[i * 2 + ch] * a->x[i * 2 + ch]; ++k; }
    return k ? sqrt(e / k) : 0.0;
}
static double db(double v) { return v > 1e-12 ? 20 * log10(v) : -240; }

static ClipSnapshot snap(EditList *el)
{
    ClipSnapshot c;
    memset(&c, 0, sizeof c);
    snprintf(c.name, sizeof c.name, "song.wav");
    c.edits = el;
    c.start = 0; c.end = (size_t)el->len;
    c.speed = 1.0;
    c.sample_rate = SR;
    return c;
}

static int run(const ClipSnapshot *c, const char *src_path, const ExportOpts *o, const char *out, float *peak)
{
    StreamSource s; int fsr;
    if (stream_source_open(src_path, SR, &s, &fsr) != 0) return 9;
    char msg[400];
    int r = export_clip_now(c, &s, o, out, peak, msg, sizeof msg);
    printf("     %s\n", msg);
    return r;
}

int main(void)
{
    char src[256], out[256];
    snprintf(src, sizeof src, "%s/src.wav", DIR_);
    if (system("mkdir -p /tmp/qlim_export") != 0) return 2;
    make_source(src);
    const long LEN = SR * SECS;
    float pk;

    /* 1) as it is, WAV 32 float: identical to the source */
    {
        ClipSnapshot c = snap(edit_new(LEN));
        ExportOpts o = { EXP_WAV, 32, 0, 0 };
        snprintf(out, sizeof out, "%s/plain.wav", DIR_);
        int r = run(&c, src, &o, out, &pk);
        Audio a = load(out), b = load(src);
        double md = 0;
        for (long i = 0; i < a.n * 2 && i < b.n * 2; ++i) { double d = fabs((double)a.x[i] - b.x[i]); if (d > md) md = d; }
        CHECK(r == 0 && a.n == LEN && md == 0.0, "untouched clip -> WAV float: bit-exact (%ld frames, max diff %g)", a.n, md);
        CHECK(fabs(pk - (-6.02f)) < 0.1, "the reported peak is right (%.2f dBFS)", pk);
        free(a.x); free(b.x); edit_free(c.edits);
    }
    /* 2) markers + a cut + gain + pan */
    {
        EditList *e0 = edit_new(LEN), *e1 = edit_split(e0, SR * 2), *e2 = edit_split(e1, SR * 3);
        EditList *e3 = edit_delete(e2, 1);                      /* 2..3 s become silence */
        edit_free(e0); edit_free(e1); edit_free(e2);
        ClipSnapshot c = snap(e3);
        c.start = SR / 2; c.end = SR * 7 / 2;                   /* 0.5 .. 3.5 s */
        c.gain_db = -6.0f; c.pan = 0.5f;                        /* right stays, left = cos(45 deg) */
        ExportOpts o = { EXP_WAV, 24, 0, 0 };
        snprintf(out, sizeof out, "%s/cut.wav", DIR_);
        int r = run(&c, src, &o, out, &pk);
        Audio a = load(out);
        CHECK(r == 0 && a.n == SR * 3, "start..stop markers: 3.0 s exported (%ld frames)", a.n);
        double l = db(rms(&a, 0, SR / 10, SR)), rr = db(rms(&a, 1, SR / 10, SR));
        double want_r = db(0.5 / sqrt(2.0)) - 6.0, want_l = want_r + db(cos(M_PI / 4));
        CHECK(fabs(rr - want_r) < 0.05 && fabs(l - want_l) < 0.05, "clip gain -6 dB + pan 0.5: L %.2f / R %.2f dB (want %.2f / %.2f)", l, rr, want_l, want_r);
        double hole = db(rms(&a, 0, SR * 3 / 2 + 2000, SR * 5 / 2 - 2000));
        CHECK(hole < -120.0, "the cut out part is silent (%.1f dB)", hole);
        CHECK(fabs(freq(&a, 0, SR / 10, SR) - 440.0) < 0.05, "and the tone is untouched (%.3f Hz)", freq(&a, 0, SR / 10, SR));
        free(a.x); edit_free(c.edits);
    }
    /* 3) warp (tempo x1.25): shorter, same pitch */
    {
        ClipSnapshot c = snap(edit_new(LEN));
        c.stretch = true; c.speed = 1.25;
        ExportOpts o = { EXP_WAV, 32, 0, 0 };
        snprintf(out, sizeof out, "%s/warp.wav", DIR_);
        int r = run(&c, src, &o, out, &pk);
        Audio a = load(out);
        double f = freq(&a, 0, SR / 2, SR * 2), want = LEN / 1.25;
        CHECK(r == 0 && fabs(a.n - want) < SR * 0.12, "warp x1.25: %.3f s (want %.3f s)", a.n / (double)SR, want / SR);
        CHECK(fabs(f - 440.0) < 0.5, "warp keeps the pitch (%.2f Hz)", f);
        free(a.x); edit_free(c.edits);
    }
    /* 4) pitch +12: one octave up, same length; with warp at once */
    {
        ClipSnapshot c = snap(edit_new(LEN));
        c.stretch = true; c.speed = 1.0; c.pitch_st = 12.0f;
        ExportOpts o = { EXP_WAV, 32, 0, 0 };
        snprintf(out, sizeof out, "%s/pitch.wav", DIR_);
        int r = run(&c, src, &o, out, &pk);
        Audio a = load(out);
        double f = freq(&a, 0, SR / 2, SR * 3), f2 = freq(&a, 1, SR / 2, SR * 3);
        CHECK(r == 0 && labs(a.n - LEN) < SR / 10, "pitch +12: the length stays (%.3f s)", a.n / (double)SR);
        CHECK(fabs(f - 880.0) < 0.5 && fabs(f2 - 1320.0) < 0.8, "pitch +12: 440 -> %.2f Hz, 660 -> %.2f Hz", f, f2);
        free(a.x);
        c.speed = 0.8; c.pitch_st = -5.0f;
        snprintf(out, sizeof out, "%s/pitchwarp.wav", DIR_);
        r = run(&c, src, &o, out, &pk);
        a = load(out);
        f = freq(&a, 0, SR / 2, SR * 3);
        double want = 440.0 * pow(2.0, -5.0 / 12.0);
        CHECK(r == 0 && fabs(a.n - LEN / 0.8) < SR * 0.12 && fabs(f - want) < 0.5, "pitch -5 + tempo x0.8: %.3f s, %.2f Hz (want %.2f)", a.n / (double)SR, f, want);
        free(a.x); edit_free(c.edits);
    }
    /* 5) another sample rate: 44.1 kHz and 96 kHz */
    {
        ClipSnapshot c = snap(edit_new(LEN));
        int rates[2] = { 44100, 96000 };
        for (int k = 0; k < 2; ++k) {
            ExportOpts o = { EXP_FLAC, 24, rates[k], 0 };
            snprintf(out, sizeof out, "%s/rate%d.flac", DIR_, rates[k]);
            int r = run(&c, src, &o, out, &pk);
            Audio a = load(out);
            double f = freq(&a, 1, a.sr / 2, a.sr * 3);
            CHECK(r == 0 && a.sr == rates[k] && labs(a.n - (long)SECS * rates[k]) <= 2 && fabs(f - 660.0) < 0.05,
                  "FLAC at %d Hz: %ld frames, 660 Hz -> %.3f Hz", rates[k], a.n, f);
            free(a.x);
        }
        snprintf(out, sizeof out, "%s/rate44100.flac.qlim-tmp.w64", DIR_);
        CHECK(access(out, F_OK) != 0, "no temporary file is left behind");
        edit_free(c.edits);
    }
    /* 6) every format: it is written, it reads back, the music is right */
    {
        ClipSnapshot c = snap(edit_new(LEN));
        for (int fmt = 0; fmt < EXP_NFMT; ++fmt) {
            if (!export_format_available(fmt)) { printf("     (%s: not in this build)\n", export_format_name(fmt)); continue; }
            for (int q = 0; q < (export_format_lossy(fmt) ? 3 : 1); ++q) {
                ExportOpts o = { fmt, 16, 44100, q };
                snprintf(out, sizeof out, "%s/fmt%d_%d.%s", DIR_, fmt, q, export_format_ext(fmt));
                int r = run(&c, src, &o, out, &pk);
                Audio a = load(out);
                double f = freq(&a, 0, a.sr / 2, a.sr * 3), lv = db(rms(&a, 0, a.sr / 2, a.sr * 3));
                FILE *fp = fopen(out, "rb"); long sz = 0;
                if (fp) { fseek(fp, 0, SEEK_END); sz = ftell(fp); fclose(fp); }
                double kbps = sz * 8.0 / 1000.0 / SECS;
                CHECK(r == 0 && a.n > 0 && fabs(f - 440.0) < 1.0 && fabs(lv - db(0.5 / sqrt(2.0))) < 0.5 &&
                      labs(a.n - (long)SECS * a.sr) < a.sr / 10,
                      "%-10s %s: %d Hz, %.3f s, 440 Hz -> %.2f Hz, level %.2f dB, %.0f kbit/s", export_format_name(fmt),
                      export_format_lossy(fmt) ? (q == 0 ? "High  " : q == 1 ? "Medium" : "Small ") : "16 bit", a.sr, a.n / (double)a.sr, f, lv, kbps);
                if (fmt == EXP_MP3 && q == 0) CHECK(kbps > 300, "MP3 High = 320 kbit/s (%.0f)", kbps);
                if (fmt == EXP_OPUS && q == 0) CHECK(kbps > 150, "Opus High >= ~160 kbit/s (%.0f)", kbps);
                free(a.x);
            }
        }
        edit_free(c.edits);
    }
    /* 7) 16 bit: dithered (the quantization noise is noise, ~ -98 dB, not a distortion of the tone) */
    {
        EditList *e = edit_new(LEN);
        ClipSnapshot c = snap(e);
        c.gain_db = -60.0f + 0.5f;                              /* a very quiet tone: without dither it would be "stair steps" */
        ExportOpts o = { EXP_WAV, 16, 0, 0 };
        snprintf(out, sizeof out, "%s/d16.wav", DIR_);
        int r = run(&c, src, &o, out, &pk);
        Audio a = load(out);
        long z = 0;
        for (long i = SR; i < SR * 2; ++i) z += a.x[i * 2] == 0.0f;
        CHECK(r == 0 && z < SR / 50, "16 bit with dither: the quiet tone is not chopped to zero (%ld zero samples of %d)", z, SR);
        free(a.x); edit_free(e);
    }
    /* 8) the background job: progress, result, cancel */
    {
        snprintf(out, sizeof out, "%s/job.ogg", DIR_);
        ExportOpts o = { EXP_OGG, 0, 0, EXP_Q_HIGH };
        char err[200];
        ExportJob *j = export_file_start(src, &o, out, err, sizeof err);
        float lastp = 0; int steps = 0;
        while (j && !export_finished(j)) { float p = export_progress(j); if (p > lastp) { lastp = p; ++steps; } struct timespec t = { 0, 1000000 }; nanosleep(&t, NULL); }
        char msg[400];
        int r = export_result(j, msg, sizeof msg);
        CHECK(j && r == 0 && export_progress(j) == 1.0f, "a file converted in the background: %s", msg);
        export_free(j);
        snprintf(out, sizeof out, "%s/cancel.flac", DIR_);
        ExportOpts o2 = { EXP_FLAC, 24, 96000, 0 };
        j = export_file_start(src, &o2, out, err, sizeof err);
        export_cancel(j);
        while (j && !export_finished(j)) { struct timespec t = { 0, 1000000 }; nanosleep(&t, NULL); }
        r = export_result(j, msg, sizeof msg);
        CHECK(j && r == 2 && access(out, F_OK) != 0, "cancel: stops and removes the half file (%s)", msg);
        export_free(j);
    }
    printf("EXPORT: %s\n", g_fail ? "FAILED" : "all ok");
    return g_fail;
}
