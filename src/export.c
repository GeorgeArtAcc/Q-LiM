/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.14: audio export of a clip (see export.h) */
#define _POSIX_C_SOURCE 200809L
#include "export.h"
#include "mcfile.h"
#include "wav.h"
#include "stretch.h"
#include "edit.h"
#include <sndfile.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BLOCK 4096

/* ---------------- formats ---------------- */

static const char *const FMT_NAME[EXP_NFMT] = { "WAV", "AIFF", "FLAC", "Ogg Vorbis", "Opus", "MP3" };
static const char *const FMT_EXT[EXP_NFMT]  = { "wav", "aiff", "flac", "ogg", "opus", "mp3" };

const char *export_format_name(int f) { return f >= 0 && f < EXP_NFMT ? FMT_NAME[f] : "?"; }
const char *export_format_ext(int f)  { return f >= 0 && f < EXP_NFMT ? FMT_EXT[f] : "wav"; }
bool export_format_lossy(int f)       { return f == EXP_OGG || f == EXP_OPUS || f == EXP_MP3; }
bool export_format_has_bits(int f)    { return f == EXP_WAV || f == EXP_AIFF || f == EXP_FLAC; }

int export_fix_bits(int f, int bits)
{
    if (!export_format_has_bits(f)) return 0;
    if (bits != 16 && bits != 24 && bits != 32) bits = 24;
    if (f == EXP_FLAC && bits == 32) bits = 24;           /* FLAC has no float */
    return bits;
}

int export_fix_rate(int f, int sr)
{
    if (sr <= 0) sr = 48000;
    if (f == EXP_OPUS) return 48000;                      /* Opus works at 48 kHz (inside it is always 48 kHz) */
    if (f == EXP_MP3) {                                   /* MPEG-1 layer III */
        if (sr >= 46000) return 48000;
        if (sr >= 38000) return 44100;
        return 32000;
    }
    if (f == EXP_FLAC && sr > 655350) return 655350;
    return sr;
}

static int sf_format_for(const ExportOpts *o, int bits)
{
    int sub = bits == 16 ? SF_FORMAT_PCM_16 : bits == 24 ? SF_FORMAT_PCM_24 : SF_FORMAT_FLOAT;
    switch (o->format) {
    case EXP_WAV:  return SF_FORMAT_WAV | sub;
    case EXP_AIFF: return SF_FORMAT_AIFF | sub;
    case EXP_FLAC: return SF_FORMAT_FLAC | (bits == 16 ? SF_FORMAT_PCM_16 : SF_FORMAT_PCM_24);
    case EXP_OGG:  return SF_FORMAT_OGG | SF_FORMAT_VORBIS;
    case EXP_OPUS: return SF_FORMAT_OGG | SF_FORMAT_OPUS;
    case EXP_MP3:  return SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III;
    default:       return SF_FORMAT_WAV | sub;
    }
}

bool export_format_available_ch(int f, int nch)
{
    if (f < 0 || f >= EXP_NFMT || nch < 1) return false;
    ExportOpts o = { f, 24, 48000, EXP_Q_HIGH, 0, 0 };
    SF_INFO i;
    memset(&i, 0, sizeof i);
    i.samplerate = export_fix_rate(f, 48000);
    i.channels = nch;
    i.format = sf_format_for(&o, export_fix_bits(f, 24));
    if (nch > 2 && f == EXP_WAV) i.format = (i.format & ~SF_FORMAT_TYPEMASK) | SF_FORMAT_WAVEX;
    return sf_format_check(&i) != 0;
}

bool export_format_available(int f)
{
    if (f < 0 || f >= EXP_NFMT) return false;
    ExportOpts o = { f, 24, 48000, EXP_Q_HIGH };
    SF_INFO i;
    memset(&i, 0, sizeof i);
    i.samplerate = export_fix_rate(f, 48000);
    i.channels = 2;
    i.format = sf_format_for(&o, export_fix_bits(f, 24));
    return sf_format_check(&i) != 0;
}

/* ---------------- the writer ---------------- */

typedef struct {
    SNDFILE *sf;
    int      bits;
    int      nch;                     /* 1.4.23: channels in the file */
    uint32_t rng;
    double   peak;
    float   *tmp;                     /* BLOCK x nch (dither) */
} Writer;

static inline float rnd01(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return (float)(*s >> 8) * (1.0f / 16777216.0f);
}

static int writer_open_ch(Writer *w, const char *path, const ExportOpts *o, int sr, int nch, const int *chmap,
                          const char *title, char *err, size_t en)
{
    memset(w, 0, sizeof *w);
    w->bits = export_fix_bits(o->format, o->bits);
    w->rng = 0x51A1u;
    w->nch = nch < 1 ? 2 : nch;
    SF_INFO i;
    memset(&i, 0, sizeof i);
    i.samplerate = sr;
    i.channels = w->nch;
    i.format = sf_format_for(o, w->bits);
    if (w->nch > 2 && o->format == EXP_WAV) i.format = (i.format & ~SF_FORMAT_TYPEMASK) | SF_FORMAT_WAVEX;   /* > 2 channels: WAVE_FORMAT_EXTENSIBLE */
    if (!sf_format_check(&i)) {
        if (w->nch > 2) snprintf(err, en, "%s cannot hold %d channels - use WAV or a file per speaker", export_format_name(o->format), w->nch);
        else snprintf(err, en, "%s at %d Hz cannot be written by this build", export_format_name(o->format), sr);
        return 1;
    }
    w->tmp = malloc(sizeof(float) * (size_t)BLOCK * (size_t)w->nch);
    if (!w->tmp) { snprintf(err, en, "Out of memory"); return 1; }
    w->sf = sf_open(path, SFM_WRITE, &i);
    if (!w->sf) { free(w->tmp); w->tmp = NULL; snprintf(err, en, "Cannot write '%s': %s", path, sf_strerror(NULL)); return 1; }
    if (chmap) sf_command(w->sf, SFC_SET_CHANNEL_MAP_INFO, (void *)chmap, (int)(sizeof(int) * (size_t)w->nch));
    if (export_format_has_bits(o->format) && w->bits != 32) sf_command(w->sf, SFC_SET_CLIPPING, NULL, SF_TRUE);
    if (export_format_lossy(o->format)) {
        /* "compression level": 0 = the best quality (the most bits) .. 1 = the smallest file */
        static const double LEVEL[3][3] = {
            /* Vorbis */ { 0.10, 0.40, 0.60 },
            /* Opus   */ { 0.45, 0.70, 0.85 },
            /* MP3    */ { 0.00, 0.35, 0.60 } };
        int row = o->format == EXP_OGG ? 0 : o->format == EXP_OPUS ? 1 : 2;
        int q = o->quality < 0 || o->quality > 2 ? 0 : o->quality;
        double lv = LEVEL[row][q];
        if (o->format == EXP_MP3) {
            int mode = q == EXP_Q_HIGH ? SF_BITRATE_MODE_CONSTANT : SF_BITRATE_MODE_VARIABLE;
            sf_command(w->sf, SFC_SET_BITRATE_MODE, &mode, sizeof mode);
        }
        sf_command(w->sf, SFC_SET_COMPRESSION_LEVEL, &lv, sizeof lv);
    }
    if (title && *title) sf_set_string(w->sf, SF_STR_TITLE, title);
    sf_set_string(w->sf, SF_STR_SOFTWARE, "Q-LiM");
    return 0;
}
static int writer_open(Writer *w, const char *path, const ExportOpts *o, int sr, const char *title, char *err, size_t en)
{
    return writer_open_ch(w, path, o, sr, 2, NULL, title, err, en);
}

static int writer_write(Writer *w, const float *buf, size_t n)
{
    size_t nn = n * (size_t)w->nch;
    for (size_t i = 0; i < nn; ++i) {
        double a = fabs((double)buf[i]);
        if (a > w->peak) w->peak = a;
    }
    const float *src = buf;
    if (w->bits == 16) {                                  /* TPDF dither, 1 LSB */
        const float lsb = 1.0f / 32768.0f;
        for (size_t i = 0; i < nn; ++i) w->tmp[i] = buf[i] + (rnd01(&w->rng) - rnd01(&w->rng)) * lsb;
        src = w->tmp;
    }
    return sf_writef_float(w->sf, src, (sf_count_t)n) == (sf_count_t)n ? 0 : 1;
}

static int writer_close(Writer *w)
{
    int r = 0;
    if (w->sf) r = sf_close(w->sf);
    w->sf = NULL;
    free(w->tmp);
    w->tmp = NULL;
    return r;
}

/* ---------------- 1.4.23: surround - several files / channels ---------------- */

#define OUTS_MAX (MAX_HW_OUT > SUR_MAX_SPK + 3 ? MAX_HW_OUT : SUR_MAX_SPK + 3)
typedef struct {
    int     nch;                      /* the channels coming in (speakers / outputs) */
    bool    split;                    /* a mono file for each */
    int     nfile;                    /* split: = nch; one: 1 */
    Writer  w[OUTS_MAX];
    int     fch[OUTS_MAX];            /* one file: its channel c comes from input fch[c] (-1 = silent, the LFE) */
    int     fn;                       /* one file: its channels */
    float  *mono;                     /* BLOCK */
    float  *mix;                      /* BLOCK x fn */
    char    path[OUTS_MAX][1100];
} OutSet;

/* "dir/name.wav" + "Stage C" -> "dir/name - Stage C.wav" */
static void path_with(const char *path, const char *label, char *out, size_t n)
{
    char base[1100];
    snprintf(base, sizeof base, "%s", path);
    char *d = strrchr(base, '.'), *sl = strrchr(base, '/');
    char ext[32] = "";
    if (d && (!sl || d > sl)) { snprintf(ext, sizeof ext, "%s", d); *d = 0; }
    snprintf(out, n, "%s - %s%s", base, label, ext);
}

static int outset_open(OutSet *o, const char *path, const ExportOpts *opt, int sr, int nch, bool split, const int *fch, int fn,
                       const int *chmap, char labels[][48], const char *title, char *err, size_t en)
{
    memset(o, 0, sizeof *o);
    o->nch = nch; o->split = split;
    o->mono = malloc(sizeof(float) * BLOCK);
    if (!o->mono) { snprintf(err, en, "Out of memory"); return 1; }
    if (split) {
        o->nfile = nch;
        for (int k = 0; k < nch; ++k) {
            path_with(path, labels[k], o->path[k], sizeof o->path[k]);
            char t[300];
            snprintf(t, sizeof t, "%s - %s", title ? title : "", labels[k]);
            if (writer_open_ch(&o->w[k], o->path[k], opt, sr, 1, NULL, t, err, en)) {
                for (int j = 0; j < k; ++j) { writer_close(&o->w[j]); remove(o->path[j]); }
                o->nfile = 0;
                return 1;
            }
        }
    } else {
        o->nfile = 1;
        o->fn = fn;
        for (int c = 0; c < fn; ++c) o->fch[c] = fch ? fch[c] : c;
        o->mix = malloc(sizeof(float) * (size_t)BLOCK * (size_t)fn);
        snprintf(o->path[0], sizeof o->path[0], "%s", path);
        if (!o->mix || writer_open_ch(&o->w[0], path, opt, sr, fn, chmap, title, err, en)) { o->nfile = 0; return 1; }
    }
    return 0;
}

/* in: n frames x nch interleaved */
static int outset_write(OutSet *o, const float *in, size_t n)
{
    if (o->split) {
        for (int k = 0; k < o->nfile; ++k) {
            for (size_t i = 0; i < n; ++i) o->mono[i] = in[i * (size_t)o->nch + (size_t)k];
            if (writer_write(&o->w[k], o->mono, n)) return 1;
        }
        return 0;
    }
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < o->fn; ++c)
            o->mix[i * (size_t)o->fn + (size_t)c] = o->fch[c] >= 0 ? in[i * (size_t)o->nch + (size_t)o->fch[c]] : 0.0f;
    return writer_write(&o->w[0], o->mix, n);
}

/* closes; rc != 0 removes the files. Returns the highest peak */
static double outset_close(OutSet *o, int *rc)
{
    double pk = 0.0;
    for (int k = 0; k < o->nfile; ++k) {
        if (writer_close(&o->w[k]) != 0 && *rc == 0) *rc = 1;
        if (o->w[k].peak > pk) pk = o->w[k].peak;
    }
    if (*rc) for (int k = 0; k < o->nfile; ++k) remove(o->path[k]);
    free(o->mono); free(o->mix);
    o->mono = o->mix = NULL;
    return pk;
}

static void sur_msg(double peak, int nfile, int nch, const char *path, char *msg, size_t n)
{
    const char *b = strrchr(path, '/');
    double db = peak > 1e-9 ? 20.0 * log10(peak) : -180.0;
    char what[120];
    if (nfile > 1) {
        char base[96];
        snprintf(base, sizeof base, "%s", b ? b + 1 : path);
        char *dot = strrchr(base, '.');
        if (dot) *dot = 0;
        snprintf(what, sizeof what, "%d files (\"%s - ...\")", nfile, base);
    }
    else snprintf(what, sizeof what, "%s (%d channels)", b ? b + 1 : path, nch);
    if (peak > 1.0) snprintf(msg, n, "Exported %s - peak +%.1f dBFS: it CLIPS", what, db);
    else snprintf(msg, n, "Exported %s (peak %.1f dBFS)", what, db);
}

/* ---------------- the render ---------------- */

typedef struct {
    const ClipSnapshot *c;
    StreamSource *src;
    int64_t pos, end;                 /* clip frames */
} PullCtx;

static size_t src_read(void *ctx, size_t pos, float *out, size_t n)
{
    StreamSource *s = ctx;
    return s->read(s->ctx, pos, out, n);
}

static size_t pull_clip(void *ud, float *out, size_t n)
{
    PullCtx *p = ud;
    if (p->pos >= p->end) return 0;
    size_t k = n;
    if ((int64_t)k > p->end - p->pos) k = (size_t)(p->end - p->pos);
    edit_read(p->c->edits, src_read, p->src, p->pos, out, k);
    p->pos += (int64_t)k;
    return k;
}

static inline void pan_law(float p, float *pl, float *pr)   /* the same balance law as the mixer */
{
    if (p > 0.0f)      { *pl = cosf(p * (float)(M_PI * 0.5)); *pr = 1.0f; }
    else if (p < 0.0f) { *pl = 1.0f; *pr = cosf(-p * (float)(M_PI * 0.5)); }
    else               { *pl = 1.0f; *pr = 1.0f; }
}

typedef struct {
    _Atomic float progress;
    volatile int  cancel;
} Prog;

/* where render_clip puts every block (stereo, `done` = frames before it) - a writer, or the surround panner */
typedef int (*SinkFn)(void *ud, const float *stereo, size_t n, size_t done);
static int sink_writer(void *ud, const float *b, size_t n, size_t done) { (void)done; return writer_write((Writer *)ud, b, n); }

/* renders the clip into the sink (at the session rate) */
static int render_clip_to(const ClipSnapshot *c, StreamSource *src, SinkFn sink, void *sud, Prog *pr, float pmax, char *err, size_t en)
{
    PullCtx pc = { c, src, (int64_t)c->start, (int64_t)c->end };
    float *buf = malloc((size_t)BLOCK * 2 * sizeof(float));
    if (!buf) { snprintf(err, en, "Out of memory"); return 1; }
    float gl = 1.0f, gr = 1.0f;
    {
        float g = c->gain_db <= CLIP_GAIN_MIN_DB ? 0.0f : powf(10.0f, c->gain_db / 20.0f), pl, prr;
        pan_law(c->pan, &pl, &prr);
        gl = g * pl; gr = g * prr;
    }
    bool unity = gl == 1.0f && gr == 1.0f;
    double speed = c->stretch && c->speed > 0.0 ? c->speed : 1.0;
    double expect = (double)(c->end - c->start) / speed;
    if (expect < 1.0) expect = 1.0;
    Stretch *st = NULL;
    if (c->stretch) {
        st = stretch_create(c->sample_rate);
        if (!st) { free(buf); snprintf(err, en, "Out of memory"); return 1; }
        stretch_set_offline(st);
        stretch_set_pitch(st, c->pitch_st, c->formants);
        stretch_prepare(st, speed, pull_clip, &pc);
    }
    size_t limit = st && !stretch_threaded(st) ? (size_t)ceil(expect) : (size_t)-1;   /* WSOLA never says "the end" */
    size_t done = 0;
    int rc = 0;
    for (;;) {
        if (pr && pr->cancel) { rc = 2; break; }
        size_t want = BLOCK, got;
        if (limit != (size_t)-1 && limit - done < want) want = limit - done;
        if (want == 0) break;
        if (st) got = stretch_read_offline(st, speed, buf, want, pull_clip, &pc, pr ? &pr->cancel : NULL);
        else got = pull_clip(&pc, buf, want);
        if (got == 0) break;
        if (!unity) for (size_t i = 0; i < got; ++i) { buf[i * 2] *= gl; buf[i * 2 + 1] *= gr; }
        if (sink(sud, buf, got, done)) { snprintf(err, en, "Writing failed (disk full?)"); rc = 1; break; }
        done += got;
        if (pr) {
            float f = (float)((double)done / expect);
            atomic_store(&pr->progress, (f > 1.0f ? 1.0f : f) * pmax);
        }
        if (got < want) break;
    }
    if (st) { stretch_release(st); stretch_free(st); }
    free(buf);
    if (rc == 0 && done == 0) { snprintf(err, en, "Nothing to export (the clip is empty between its markers)"); rc = 1; }
    return rc;
}

static int render_clip(const ClipSnapshot *c, StreamSource *src, Writer *w, Prog *pr, float pmax, char *err, size_t en)
{
    return render_clip_to(c, src, sink_writer, w, pr, pmax, err, en);
}

/* 1.4.23: the clip through its surround path - the same gains as the mixer (sur_pair_gains), recomputed every
 * 256 frames and glided in between */
typedef struct {
    const SurSetup *su;
    const SurPt    *pts;
    int             npts;
    double          t0, dt;               /* the clip time (s) of output frame 0, and of one output frame */
    float           ga[SUR_MAX_SPK], gb[SUR_MAX_SPK];
    bool            init;
    float          *buf;                  /* BLOCK x n speakers */
    OutSet         *os;
} SurSink;

static int sink_sur(void *ud, const float *b, size_t n, size_t done)
{
    SurSink *S = ud;
    int ns = S->su->n;
    memset(S->buf, 0, sizeof(float) * n * (size_t)ns);
    for (size_t i0 = 0; i0 < n; i0 += 256) {
        size_t m = n - i0 < 256 ? n - i0 : 256;
        float gl[SUR_MAX_SPK], gr[SUR_MAX_SPK];
        float t = (float)(S->t0 + (double)(done + i0) * S->dt);
        sur_pair_gains(S->su, S->pts, S->npts, t, gl, gr, NULL);
        if (!S->init) { memcpy(S->ga, gl, sizeof gl); memcpy(S->gb, gr, sizeof gr); S->init = true; }
        float inv = 1.0f / (float)m;
        for (int k = 0; k < ns; ++k) {
            float a = S->ga[k], bb = S->gb[k], da = (gl[k] - a) * inv, db = (gr[k] - bb) * inv;
            for (size_t i = 0; i < m; ++i) {
                a += da; bb += db;
                S->buf[(i0 + i) * (size_t)ns + (size_t)k] = b[(i0 + i) * 2] * a + b[(i0 + i) * 2 + 1] * bb;
            }
            S->ga[k] = gl[k]; S->gb[k] = gr[k];
        }
    }
    return outset_write(S->os, S->buf, n);
}

/* reads a finished float file (at any rate), converts it to `sr` and writes it */
static int convert_file(const char *in, int sr, Writer *w, Prog *pr, float p0, char *err, size_t en)
{
    StreamSource s;
    int fsr = 0;
    if (stream_source_open(in, sr, &s, &fsr) != 0) { snprintf(err, en, "Cannot read '%s'", in); return 1; }
    size_t total = s.frames(s.ctx);
    float *buf = malloc((size_t)BLOCK * 2 * sizeof(float));
    if (!buf) { s.close(s.ctx); snprintf(err, en, "Out of memory"); return 1; }
    int rc = 0;
    for (size_t pos = 0; pos < total;) {
        if (pr && pr->cancel) { rc = 2; break; }
        size_t k = total - pos < BLOCK ? total - pos : BLOCK;
        s.read(s.ctx, pos, buf, k);
        if (writer_write(w, buf, k)) { snprintf(err, en, "Writing failed (disk full?)"); rc = 1; break; }
        pos += k;
        if (pr) atomic_store(&pr->progress, p0 + (1.0f - p0) * (float)((double)pos / (double)total));
    }
    free(buf);
    s.close(s.ctx);
    return rc;
}

static void peak_line(double peak, char *msg, size_t n, const char *path)
{
    const char *b = strrchr(path, '/');
    double db = peak > 1e-9 ? 20.0 * log10(peak) : -180.0;
    if (peak > 1.0) snprintf(msg, n, "Exported %s - peak +%.1f dBFS: it CLIPS, lower the clip gain", b ? b + 1 : path, db);
    else snprintf(msg, n, "Exported %s (peak %.1f dBFS)", b ? b + 1 : path, db);
}

static int do_export(const ClipSnapshot *c, StreamSource *src, const char *in_file, const ExportOpts *o0,
                     const char *out_path, Prog *pr, float *peak_db, char *err, size_t en)
{
    ExportOpts o = *o0;
    int ssr = c ? c->sample_rate : 0;
    int want = o.sample_rate > 0 ? o.sample_rate : ssr;
    if (want <= 0) {                                      /* a file: keep its own rate */
        SF_INFO i;
        memset(&i, 0, sizeof i);
        SNDFILE *f = in_file ? sf_open(in_file, SFM_READ, &i) : NULL;
        if (f) { want = i.samplerate; sf_close(f); }
        if (want <= 0) want = 48000;
    }
    int sr = export_fix_rate(o.format, want);
    o.bits = export_fix_bits(o.format, o.bits);
    const char *title = c ? c->name : NULL;
    char tname[256] = "";
    if (title) {
        snprintf(tname, sizeof tname, "%s", title);
        char *dot = strrchr(tname, '.');
        if (dot && dot != tname) *dot = 0;
    }
    Writer w;
    int rc;
    if (c && sr == ssr) {                                 /* straight into the file */
        if (writer_open(&w, out_path, &o, sr, tname, err, en)) { if (src) src->close(src->ctx); return 1; }
        rc = render_clip(c, src, &w, pr, 1.0f, err, en);
        src->close(src->ctx);
    } else {
        /* 1) the clip at the session rate into a temporary 32-bit float file next to the target, 2) converted */
        char tmp[1100];
        const char *from = in_file;
        rc = 0;
        if (c) {
            snprintf(tmp, sizeof tmp, "%s.qlim-tmp.w64", out_path);
            SF_INFO i;
            memset(&i, 0, sizeof i);
            i.samplerate = ssr; i.channels = 2; i.format = SF_FORMAT_W64 | SF_FORMAT_FLOAT;
            Writer tw;
            memset(&tw, 0, sizeof tw);
            tw.bits = 32;
            tw.nch = 2;
            tw.sf = sf_open(tmp, SFM_WRITE, &i);
            if (!tw.sf) { src->close(src->ctx); snprintf(err, en, "Cannot write next to '%s'", out_path); return 1; }
            rc = render_clip(c, src, &tw, pr, 0.6f, err, en);
            src->close(src->ctx);
            writer_close(&tw);
            from = tmp;
        }
        if (rc == 0) {
            if (writer_open(&w, out_path, &o, sr, tname, err, en)) { if (c) remove(tmp); return 1; }
            rc = convert_file(from, sr, &w, pr, c ? 0.6f : 0.0f, err, en);
        } else {
            memset(&w, 0, sizeof w);
        }
        if (c) remove(tmp);
    }
    if (writer_close(&w) != 0 && rc == 0) { snprintf(err, en, "Could not finish the file (disk full?)"); rc = 1; }
    if (rc != 0) {
        remove(out_path);
        if (rc == 2) snprintf(err, en, "Export cancelled");
        return rc;
    }
    if (peak_db) *peak_db = w.peak > 1e-9 ? (float)(20.0 * log10(w.peak)) : -180.0f;
    peak_line(w.peak, err, en, out_path);
    if (pr) atomic_store(&pr->progress, 1.0f);
    return 0;
}

/* 1.4.23: a clip through its surround path (at the session rate): one channel / file per speaker */
static int do_export_sur_clip(const ClipSnapshot *c, StreamSource *src, const SurSetup *su, const SurPt *pts, int npts,
                              const ExportOpts *o0, const char *out_path, Prog *pr, char *err, size_t en)
{
    ExportOpts o = *o0;
    o.bits = export_fix_bits(o.format, o.bits);
    int sr = c->sample_rate;
    int ns = su->n;
    char tname[256];
    snprintf(tname, sizeof tname, "%s", c->name);
    char *dot = strrchr(tname, '.');
    if (dot && dot != tname) *dot = 0;
    static char labels[OUTS_MAX][48];
    for (int k = 0; k < ns; ++k) sur_speaker_label(su, k, labels[k], sizeof labels[k]);
    /* one file: the standard layouts get their channel layout (5.1 / 7.1 with an empty LFE) */
    int fch[OUTS_MAX], fn = ns, map[OUTS_MAX];
    const int *chmap = NULL;
    for (int k = 0; k < ns; ++k) fch[k] = k;
    if (su->mode == SUR_MODE_CIRCLE && o.sur == EXP_SUR_ONE) {
        enum { L_ = SF_CHANNEL_MAP_LEFT, R_ = SF_CHANNEL_MAP_RIGHT, C_ = SF_CHANNEL_MAP_CENTER, LFE_ = SF_CHANNEL_MAP_LFE,
               BL_ = SF_CHANNEL_MAP_REAR_LEFT, BR_ = SF_CHANNEL_MAP_REAR_RIGHT, SL_ = SF_CHANNEL_MAP_SIDE_LEFT, SR_ = SF_CHANNEL_MAP_SIDE_RIGHT };
        if (su->layout == SUR_L_QUAD && ns == 4) { int m[4] = { L_, R_, BL_, BR_ }; memcpy(map, m, sizeof m); chmap = map; }
        else if (su->layout == SUR_L_50 && ns == 5) { int m[5] = { L_, R_, C_, BL_, BR_ }; memcpy(map, m, sizeof m); chmap = map; }
        else if (su->layout == SUR_L_51 && ns == 5) {
            int f[6] = { 0, 1, 2, -1, 3, 4 }, m[6] = { L_, R_, C_, LFE_, BL_, BR_ };
            memcpy(fch, f, sizeof f); memcpy(map, m, sizeof m); fn = 6; chmap = map;
        } else if (su->layout == SUR_L_70 && ns == 7) {
            int f[7] = { 0, 1, 2, 5, 6, 3, 4 }, m[7] = { L_, R_, C_, BL_, BR_, SL_, SR_ };
            memcpy(fch, f, sizeof f); memcpy(map, m, sizeof m); chmap = map;
        } else if (su->layout == SUR_L_71 && ns == 7) {
            int f[8] = { 0, 1, 2, -1, 5, 6, 3, 4 }, m[8] = { L_, R_, C_, LFE_, BL_, BR_, SL_, SR_ };
            memcpy(fch, f, sizeof f); memcpy(map, m, sizeof m); fn = 8; chmap = map;
        }
    }
    static OutSet os;
    if (outset_open(&os, out_path, &o, sr, ns, o.sur == EXP_SUR_SPLIT, fch, fn, chmap, labels, tname, err, en)) { src->close(src->ctx); return 1; }
    SurSink S;
    memset(&S, 0, sizeof S);
    S.su = su; S.pts = pts; S.npts = npts; S.os = &os;
    double speed = c->stretch && c->speed > 0.0 ? c->speed : 1.0;
    S.t0 = (double)c->start / (double)sr;
    S.dt = speed / (double)sr;
    S.buf = malloc(sizeof(float) * (size_t)BLOCK * (size_t)(ns > 0 ? ns : 1));
    int rc = S.buf ? render_clip_to(c, src, sink_sur, &S, pr, 1.0f, err, en) : 1;
    if (!S.buf) snprintf(err, en, "Out of memory");
    src->close(src->ctx);
    free(S.buf);
    double pk = outset_close(&os, &rc);
    if (rc) { if (rc == 2) snprintf(err, en, "Export cancelled"); return rc; }
    sur_msg(pk, os.split ? ns : 1, fn, out_path, err, en);
    if (pr) atomic_store(&pr->progress, 1.0f);
    return 0;
}

/* 1.4.24: a multichannel clip: its channels as they are (with its cuts, markers and gain) - one file or a file each */
static int do_export_mc_clip(const ClipSnapshot *c, StreamSource *src, int nch, const ExportOpts *o0, const char *out_path,
                             Prog *pr, char *err, size_t en)
{
    ExportOpts o = *o0;
    o.bits = export_fix_bits(o.format, o.bits);
    int sr = c->sample_rate;
    char tname[256];
    snprintf(tname, sizeof tname, "%s", c->name);
    char *dot = strrchr(tname, '.');
    if (dot && dot != tname) *dot = 0;
    static char labels[OUTS_MAX][48];
    for (int k = 0; k < nch; ++k) snprintf(labels[k], sizeof labels[k], "%s", mc_channel_name(nch, k));
    int map[STREAM_MAX_CH];                              /* the original's channel layout */
    const int *chmap = NULL;
    {
        SF_INFO si;
        memset(&si, 0, sizeof si);
        SNDFILE *f = sf_open(c->path, SFM_READ, &si);
        if (f) {
            if (si.channels == nch && sf_command(f, SFC_GET_CHANNEL_MAP_INFO, map, (int)(sizeof(int) * (size_t)nch)) == SF_TRUE) chmap = map;
            sf_close(f);
        }
    }
    static OutSet os;
    if (outset_open(&os, out_path, &o, sr, nch, o.sur == EXP_SUR_SPLIT, NULL, nch, chmap, labels, tname, err, en)) { src->close(src->ctx); return 1; }
    float *buf = malloc(sizeof(float) * (size_t)BLOCK * (size_t)nch);
    float g = c->gain_db <= CLIP_GAIN_MIN_DB ? 0.0f : powf(10.0f, c->gain_db / 20.0f);
    int64_t pos = (int64_t)c->start, end = (int64_t)c->end;
    double total = (double)(end - pos > 0 ? end - pos : 1);
    int rc = buf ? 0 : 1;
    if (!buf) snprintf(err, en, "Out of memory");
    while (rc == 0 && pos < end) {
        if (pr && pr->cancel) { rc = 2; break; }
        size_t k = BLOCK;
        if ((int64_t)k > end - pos) k = (size_t)(end - pos);
        edit_read_n(c->edits, src_read, src, pos, buf, k, nch);
        if (g != 1.0f) for (size_t i = 0; i < k * (size_t)nch; ++i) buf[i] *= g;
        if (outset_write(&os, buf, k)) { snprintf(err, en, "Writing failed (disk full?)"); rc = 1; break; }
        pos += (int64_t)k;
        if (pr) atomic_store(&pr->progress, (float)((double)(pos - (int64_t)c->start) / total));
    }
    src->close(src->ctx);
    free(buf);
    double pk = outset_close(&os, &rc);
    if (rc) { if (rc == 2) snprintf(err, en, "Export cancelled"); return rc; }
    sur_msg(pk, os.split ? nch : 1, nch, out_path, err, en);
    if (pr) atomic_store(&pr->progress, 1.0f);
    return 0;
}

int export_clip_now(const ClipSnapshot *c, StreamSource *src, const ExportOpts *o, const char *out_path,
                    float *peak_db, char *err, size_t en)
{
    char tmp[300];
    if (!err) { err = tmp; en = sizeof tmp; }
    return do_export(c, src, NULL, o, out_path, NULL, peak_db, err, en);
}

int export_file_now(const char *in_path, const ExportOpts *o, const char *out_path, char *err, size_t en)
{
    char tmp[300];
    if (!err) { err = tmp; en = sizeof tmp; }
    return do_export(NULL, NULL, in_path, o, out_path, NULL, NULL, err, en);
}

/* ---------------- the job (own thread) ---------------- */

struct ExportJob {
    Prog         pr;
    pthread_t    thr;
    bool         started;
    atomic_bool  finished;
    int          rc;
    char         msg[400];
    ClipSnapshot c;
    bool         has_clip;
    StreamSource src;
    char         in_file[1024];
    char         out[1024];
    ExportOpts   o;
    /* 1.4.23: a clip through its surround path */
    bool         sur_clip;
    int          mc_nch;                /* 1.4.24: > 0 = a multichannel clip, all its channels */
    SurSetup     su;
    SurPt       *pts;
    int          npts;
    /* 1.4.17: Export Scene (recorded while the scene plays) */
    Session     *s;
    Capture     *cap;
    long         total;
    atomic_int   phase;                 /* 1 = recording, 2 = saving */
};

static void *job_main(void *arg)
{
    ExportJob *j = arg;
    if (j->mc_nch > 2) {
        j->rc = do_export_mc_clip(&j->c, &j->src, j->mc_nch, &j->o, j->out, &j->pr, j->msg, sizeof j->msg);
        atomic_store(&j->finished, true);
        return NULL;
    }
    if (j->sur_clip) {
        j->rc = do_export_sur_clip(&j->c, &j->src, &j->su, j->pts, j->npts, &j->o, j->out, &j->pr, j->msg, sizeof j->msg);
        atomic_store(&j->finished, true);
        return NULL;
    }
    j->rc = do_export(j->has_clip ? &j->c : NULL, j->has_clip ? &j->src : NULL, j->has_clip ? NULL : j->in_file,
                      &j->o, j->out, &j->pr, NULL, j->msg, sizeof j->msg);
    atomic_store(&j->finished, true);
    return NULL;
}

static ExportJob *job_new(const ExportOpts *o, const char *out_path)
{
    ExportJob *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    j->o = *o;
    snprintf(j->out, sizeof j->out, "%s", out_path);
    return j;
}

static ExportJob *job_run(ExportJob *j, char *err, size_t en)
{
    if (pthread_create(&j->thr, NULL, job_main, j) != 0) {
        snprintf(err, en, "Cannot start the export");
        if (j->has_clip) { j->src.close(j->src.ctx); edit_free(j->c.edits); }
        free(j->pts);
        free(j);
        return NULL;
    }
    j->started = true;
    return j;
}

ExportJob *export_clip_start(Session *s, int ch, int scene, const ExportOpts *o, const char *out_path,
                             char *err, size_t en)
{
    if (!o || !out_path || !*out_path) { snprintf(err, en, "No file name"); return NULL; }
    ExportJob *j = job_new(o, out_path);
    if (!j) { snprintf(err, en, "Out of memory"); return NULL; }
    if (session_clip_snapshot(s, ch, scene, &j->c) != 0) { free(j); snprintf(err, en, "Only audio clips can be exported"); return NULL; }
    if (j->c.stems && o->stem >= 1 && o->stem <= j->c.stem_n)     /* one part alone, at full level */
        for (int k = 0; k < j->c.stem_n; ++k) j->c.stem_gain[k] = k == o->stem - 1 ? 1.0f : 0.0f;
    if (o->sur != EXP_SUR_OFF && j->c.mc_nch > 2) {              /* 1.4.24: a multichannel clip: its own channels */
        int n = 0;
        char rbuf[1024];                                          /* 2.1: an M4A ...: its copy with all the channels */
        const char *rp = wav_readable_all_channels(j->c.path, j->c.sample_rate, rbuf, sizeof rbuf);
        if (stream_source_open_mc(rp, j->c.sample_rate, &j->src, NULL, &n) != 0 || n != j->c.mc_nch) {
            edit_free(j->c.edits); free(j);
            snprintf(err, en, "Cannot read the clip's file");
            return NULL;
        }
        j->mc_nch = n;
        j->has_clip = true;
        return job_run(j, err, en);
    }
    if (o->sur != EXP_SUR_OFF) {                                  /* 1.4.23 */
        if (!session_get_surround(s, ch, &j->su) || j->su.n <= 0) {
            edit_free(j->c.edits); free(j);
            snprintf(err, en, "This channel does not play in surround");
            return NULL;
        }
        j->pts = malloc(sizeof(SurPt) * SUR_MAX_PTS);
        if (!j->pts) { edit_free(j->c.edits); free(j); snprintf(err, en, "Out of memory"); return NULL; }
        j->npts = session_clip_get_sur_points(s, ch, scene, j->pts, SUR_MAX_PTS);
        j->sur_clip = true;
    }
    if (session_open_snapshot_source(s, &j->c, &j->src) != 0) {
        edit_free(j->c.edits); free(j->pts); free(j);
        snprintf(err, en, "Cannot read the clip's file");
        return NULL;
    }
    j->has_clip = true;
    return job_run(j, err, en);
}

ExportJob *export_file_start(const char *in_path, const ExportOpts *o, const char *out_path, char *err, size_t en)
{
    if (!in_path || !o || !out_path || !*out_path) { snprintf(err, en, "No file name"); return NULL; }
    ExportJob *j = job_new(o, out_path);
    if (!j) { snprintf(err, en, "Out of memory"); return NULL; }
    snprintf(j->in_file, sizeof j->in_file, "%s", in_path);
    return job_run(j, err, en);
}

/* ---------------- 1.4.17: Export Scene ---------------- */

static Capture g_cap;                   /* one at a time; its ring is kept (the audio thread may still look at it) */
static size_t  g_cap_floats;            /* 1.4.23: the ring's size (it grows for more outputs) */

/* 1.4.23: outputs 1 .. the highest one a channel, its surround speakers or the master go to */
int export_scene_outputs(Session *s)
{
    int hi = 1;
    if (!s) return 2;
    for (int ch = 0; ch < s->channel_count; ++ch) {
        for (int q = 0; q < s->scene_count; ++q) {            /* 1.4.24: multichannel clips: outputs mc_out .. + channels */
            Clip *cl = &s->channels[ch].clips[q];
            if (cl->mc_mode == CLIP_MC_MULTI && cl->mc_nch > 2 && atomic_load(&cl->has_clip)) {
                for (int k = 0; k < cl->mc_nch && k < STREAM_MAX_CH; ++k) {     /* 2.0: through the Speaker Map */
                    int oo[62];
                    int no = sur_mask_outputs(atomic_load(&cl->mc_map[k]), atomic_load(&cl->mc_out) + k, oo);
                    for (int q = 0; q < no; ++q) if (oo[q] > hi) hi = oo[q];
                }
            }
        }
        SurSetup su;
        if (session_get_surround(s, ch, &su)) {
            for (int k = 0; k < su.n; ++k) {
                int oo[62];
                int no = sur_mask_outputs(su.omask[k], su.out[k], oo);
                for (int q = 0; q < no; ++q) if (oo[q] > hi) hi = oo[q];
            }
            continue;
        }
        RouteMask m = session_route_get(s, ch, 0) | session_route_get(s, ch, 1);
        for (int b = 2; b < 64; ++b) if (m & RBIT(b)) { if (b - 2 > hi) hi = b - 2; }
    }
    RouteMask mm = session_master_route_get(s, 0) | session_master_route_get(s, 1);
    for (int b = 0; b < MAX_HW_OUT; ++b) if (mm & RBIT(b)) { if (b > hi) hi = b; }
    if (hi >= MAX_HW_OUT) hi = MAX_HW_OUT - 1;
    return hi + 1;
}

/* 1.4.23: the recorded outputs (a float file) -> one multi-channel file or a file per output */
static int save_scene_sur(ExportJob *j, const char *tmp)
{
    SF_INFO si;
    memset(&si, 0, sizeof si);
    SNDFILE *f = sf_open(tmp, SFM_READ, &si);
    if (!f) { snprintf(j->msg, sizeof j->msg, "Cannot read the recording"); return 1; }
    int nch = si.channels;
    ExportOpts o = j->o;
    o.bits = export_fix_bits(o.format, o.bits);
    static char labels[OUTS_MAX][48];
    for (int k = 0; k < nch && k < OUTS_MAX; ++k) snprintf(labels[k], sizeof labels[k], "Out %d", k + 1);
    char tname[200];
    {
        const char *b = strrchr(j->out, '/');
        snprintf(tname, sizeof tname, "%s", b ? b + 1 : j->out);
        char *d = strrchr(tname, '.');
        if (d && d != tname) *d = 0;
    }
    static OutSet os;
    if (outset_open(&os, j->out, &o, si.samplerate, nch, o.sur == EXP_SUR_SPLIT, NULL, nch, NULL, labels, tname, j->msg, sizeof j->msg)) { sf_close(f); return 1; }
    float *buf = malloc(sizeof(float) * (size_t)BLOCK * (size_t)nch);
    int rc = buf ? 0 : 1;
    sf_count_t total = si.frames > 0 ? si.frames : 1, done = 0;
    while (rc == 0) {
        if (j->pr.cancel) { rc = 2; break; }
        sf_count_t k = sf_readf_float(f, buf, BLOCK);
        if (k <= 0) break;
        if (outset_write(&os, buf, (size_t)k)) { snprintf(j->msg, sizeof j->msg, "Writing failed (disk full?)"); rc = 1; break; }
        done += k;
        atomic_store(&j->pr.progress, (float)((double)done / (double)total));
    }
    free(buf);
    sf_close(f);
    double pk = outset_close(&os, &rc);
    if (rc) { if (rc == 2) snprintf(j->msg, sizeof j->msg, "Cancelled"); return rc; }
    sur_msg(pk, os.split ? nch : 1, nch, j->out, j->msg, sizeof j->msg);
    return 0;
}

static void *scene_main(void *arg)
{
    ExportJob *j = arg;
    Capture *c = j->cap;
    int sr = j->s->sample_rate > 0 ? j->s->sample_rate : 48000;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.qlim-rec.wav", j->out);
    SF_INFO si;
    memset(&si, 0, sizeof si);
    int cch = c->nch > 0 ? c->nch : 2;                     /* 1.4.23: all the outputs */
    si.samplerate = sr; si.channels = cch;
    si.format = (j->total * (long)cch > 400000000L ? SF_FORMAT_RF64 : cch > 2 ? SF_FORMAT_WAVEX : SF_FORMAT_WAV) | SF_FORMAT_FLOAT;
    SNDFILE *f = sf_open(tmp, SFM_WRITE, &si);
    if (!f) { snprintf(j->msg, sizeof j->msg, "Cannot write %s", tmp); j->rc = 1; goto done; }
    long got = 0;
    double idle = 0.0;
    float *buf = malloc(sizeof(float) * 4096 * (size_t)cch);
    if (!buf) { sf_close(f); snprintf(j->msg, sizeof j->msg, "Out of memory"); j->rc = 1; goto done; }
    while (got < j->total) {
        if (j->pr.cancel) break;
        size_t w = atomic_load_explicit(&c->w, memory_order_acquire), r = atomic_load(&c->r);
        size_t m = w - r;
        if (m == 0) {
            struct timespec ts = { 0, 10000000L };
            nanosleep(&ts, NULL);
            idle += 0.01;
            if (idle > (atomic_load(&c->started) ? 5.0 : 10.0)) {     /* no sound card callbacks / the scene never started */
                snprintf(j->msg, sizeof j->msg, atomic_load(&c->started) ? "The recording stopped (is the sound card running?)"
                                                                         : "The scene did not start (is the sound card running?)");
                j->rc = 1;
                break;
            }
            continue;
        }
        idle = 0.0;
        if (m > 4096) m = 4096;
        for (size_t i = 0; i < m; ++i) {
            size_t k = (r + i) % c->cap;
            memcpy(buf + i * (size_t)cch, c->ring + k * (size_t)cch, sizeof(float) * (size_t)cch);
        }
        atomic_store(&c->r, r + m);
        if (sf_writef_float(f, buf, (sf_count_t)m) != (sf_count_t)m) { snprintf(j->msg, sizeof j->msg, "Writing failed (disk full?)"); j->rc = 1; break; }
        got += (long)m;
        atomic_store(&j->pr.progress, (float)got / (float)j->total);
    }
    free(buf);
    atomic_store(&c->on, false);
    atomic_store(&j->s->capture, NULL);
    sf_close(f);
    if (j->pr.cancel && !j->rc) { snprintf(j->msg, sizeof j->msg, "Cancelled"); j->rc = 2; }
    if (!j->rc) {
        atomic_store(&j->phase, 2);
        atomic_store(&j->pr.progress, 0.0f);
        bool ov = atomic_load(&c->overflow);
        if (c->nch > 0) j->rc = save_scene_sur(j, tmp);             /* 1.4.23 */
        else j->rc = do_export(NULL, NULL, tmp, &j->o, j->out, &j->pr, NULL, j->msg, sizeof j->msg);
        if (!j->rc && ov) {
            size_t l = strlen(j->msg);
            snprintf(j->msg + l, sizeof j->msg - l, " - WARNING: the computer was too slow, a few moments are missing");
        }
    }
    remove(tmp);
    if (j->rc) remove(j->out);
done:
    atomic_store(&j->finished, true);
    return NULL;
}

ExportJob *export_scene_start(Session *s, int scene, double secs, const ExportOpts *o, const char *out_path, char *err, size_t en)
{
    if (!s || !o || !out_path || !*out_path) { snprintf(err, en, "No file name"); return NULL; }
    if (!(secs > 0.0)) { snprintf(err, en, "The scene has no clips to play"); return NULL; }
    if (atomic_load(&s->capture)) { snprintf(err, en, "A scene is being exported already"); return NULL; }
    int sr = s->sample_rate > 0 ? s->sample_rate : 48000;
    int nch = o->sur != EXP_SUR_OFF ? export_scene_outputs(s) : 0;          /* 1.4.23: 0 = the master */
    size_t need = (size_t)sr * 8 * (size_t)(nch > 0 ? nch : 2);             /* 8 s: the writer drains it every 10 ms */
    if (!g_cap.ring || g_cap_floats < need) {
        float *nr = calloc(need, sizeof(float));
        if (!nr) { snprintf(err, en, "Out of memory"); return NULL; }
        free(g_cap.ring);                                    /* (no export runs: the audio thread does not use it) */
        g_cap.ring = nr;
        g_cap_floats = need;
    }
    g_cap.cap = (size_t)sr * 8;
    g_cap.nch = nch;
    ExportJob *j = job_new(o, out_path);
    if (!j) { snprintf(err, en, "Out of memory"); return NULL; }
    j->s = s;
    j->cap = &g_cap;
    j->total = (long)(secs * (double)sr + 0.5);
    atomic_store(&j->phase, 1);
    atomic_store(&g_cap.on, false);
    atomic_store(&g_cap.w, 0);
    atomic_store(&g_cap.r, 0);
    atomic_store(&g_cap.left, j->total);
    atomic_store(&g_cap.started, false);
    atomic_store(&g_cap.overflow, false);
    g_cap.scene = scene;
    session_stop_all(s);                                     /* (only the scene is heard) */
    atomic_store(&s->capture, &g_cap);
    atomic_store(&g_cap.on, true);
    session_play_scene_now(s, scene);                        /* recording starts with its first sound */
    if (pthread_create(&j->thr, NULL, scene_main, j) != 0) {
        atomic_store(&g_cap.on, false);
        atomic_store(&s->capture, NULL);
        free(j);
        snprintf(err, en, "Cannot start the export");
        return NULL;
    }
    j->started = true;
    return j;
}

int export_phase(const ExportJob *j) { return j ? atomic_load(&((ExportJob *)j)->phase) : 0; }

float export_progress(const ExportJob *j) { return j ? atomic_load(&((ExportJob *)j)->pr.progress) : 0.0f; }
bool  export_finished(const ExportJob *j) { return !j || atomic_load(&((ExportJob *)j)->finished); }

int export_result(const ExportJob *j, char *msg, size_t n)
{
    if (!j) return 1;
    if (msg && n) snprintf(msg, n, "%s", j->msg);
    return j->rc;
}

void export_cancel(ExportJob *j) { if (j) j->pr.cancel = 1; }

void export_free(ExportJob *j)
{
    if (!j) return;
    if (j->started) pthread_join(j->thr, NULL);
    if (j->has_clip) edit_free(j->c.edits);
    free(j->pts);
    free(j);
}
