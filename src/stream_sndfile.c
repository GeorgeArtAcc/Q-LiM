/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#include "stream.h"
#include <sndfile.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    SNDFILE *sf;
    int      channels;
    size_t   frames;
    size_t   cur;        /* current position in the file */
    float   *buf;        /* only when channels != 2 */
    size_t   buf_frames;
    size_t   limit;      /* step 36i: the real end (MP3 declares more), 0 = unknown */
} SfCtx;

static size_t sf_frames_cb(void *c) { return ((SfCtx *)c)->frames; }

/* Reading a chunk. Step 36i: MP3 (libsndfile + mpg123) sometimes declares slightly MORE frames than it really has;
 * if a read across the real end is requested, it returns 0 for the WHOLE chunk (and the file stays "in error" until a seek).
 * So: we read in chunks; on 0 we seek back and try with half, until we find the real end,
 * and remember it (limit) so we do not search again. */
static sf_count_t read_raw(SfCtx *x, float *dst, size_t m)
{
    if (x->limit && x->cur + m > x->limit) m = x->cur < x->limit ? x->limit - x->cur : 0;
    if (m == 0) return 0;
    for (;;) {
        sf_count_t r = sf_readf_float(x->sf, dst, (sf_count_t)m);
        if (r > 0) return r;
        if (m <= 1) {                                    /* not even 1 frame works -> this is the end */
            if (x->cur < x->frames) x->limit = x->cur > 0 ? x->cur : 1;
            sf_seek(x->sf, (sf_count_t)x->cur, SEEK_SET);
            return 0;
        }
        m /= 2;
        if (sf_seek(x->sf, (sf_count_t)x->cur, SEEK_SET) < 0) return 0;
    }
}

static size_t sf_read_cb(void *c, size_t pos, float *out, size_t n)
{
    SfCtx *x = c;
    if (pos != x->cur) {
        if (sf_seek(x->sf, (sf_count_t)pos, SEEK_SET) < 0) return 0;
        x->cur = pos;
    }

    size_t got = 0;
    while (got < n) {
        size_t m = n - got;
        if (m > 16384) m = 16384;
        sf_count_t r;
        if (x->channels == 2) {
            r = read_raw(x, out + got * 2, m);
        } else {
            if (x->buf_frames < m) {
                float *nb = realloc(x->buf, m * (size_t)x->channels * sizeof(float));
                if (!nb) break;
                x->buf = nb;
                x->buf_frames = m;
            }
            r = read_raw(x, x->buf, m);
            for (sf_count_t i = 0; i < r; ++i) {   /* as in wav_load: mono -> L=R, >2 -> the first 2 */
                float l = x->buf[i * x->channels];
                float rr = x->channels == 1 ? l : x->buf[i * x->channels + 1];
                out[(got + (size_t)i) * 2 + 0] = l;
                out[(got + (size_t)i) * 2 + 1] = rr;
            }
        }
        if (r <= 0) break;
        got += (size_t)r;
        x->cur += (size_t)r;
        if ((size_t)r < m) break;
    }
    /* a damaged float file may contain NaN/inf: they would "poison" the mixer and the time-stretch -> silence */
    for (size_t i = 0; i < got * 2; ++i) {
        float v = out[i];
        if (!(v > -64.0f && v < 64.0f)) out[i] = (v >= 64.0f) ? 64.0f : (v <= -64.0f ? -64.0f : 0.0f);
    }
    return got;
}

static void sf_close_cb(void *c)
{
    SfCtx *x = c;
    if (!x) return;
    if (x->sf) sf_close(x->sf);
    free(x->buf);
    free(x);
}

int stream_source_sndfile_native(const char *path, StreamSource *out, int *file_sr)
{
    if (!path || !out) return -1;

    SF_INFO info = {0};
    SNDFILE *sf = sf_open(path, SFM_READ, &info);
    if (!sf) {
        fprintf(stderr, "stream: cannot open '%s': %s\n", path, sf_strerror(NULL));
        return -1;
    }
    if (file_sr) *file_sr = info.samplerate;
    if (info.frames <= 0 || info.channels < 1 || info.samplerate <= 0) { sf_close(sf); return -1; }

    SfCtx *x = calloc(1, sizeof(*x));
    if (!x) { sf_close(sf); return -1; }
    x->sf = sf;
    x->channels = info.channels;
    x->frames = (size_t)info.frames;
    x->cur = 0;

    out->frames = sf_frames_cb;
    out->read   = sf_read_cb;
    out->close  = sf_close_cb;
    out->ctx    = x;
    return 0;
}

int stream_source_open(const char *path, int target_sr, StreamSource *out, int *file_sr)
{
    StreamSource nat;
    int fsr = 0;
    if (stream_source_sndfile_native(path, &nat, &fsr) != 0) return -1;
    if (file_sr) *file_sr = fsr;
    if (fsr == target_sr) { *out = nat; return 0; }
    if (stream_source_resample(&nat, fsr, target_sr, out) != 0) {
        nat.close(nat.ctx);
        return -1;
    }
    return 0;
}

/* MP3 (MPEG) in libsndfile cannot seek frame-exactly -> loop/markers would click.
 * For such files the session makes (once) an exact WAV copy in the cache. */
int stream_file_needs_cache(const char *path)
{
#ifdef SF_FORMAT_MPEG
    SF_INFO info;
    memset(&info, 0, sizeof info);
    SNDFILE *sf = sf_open(path, SFM_READ, &info);
    if (!sf) return 0;
    int r = (info.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_MPEG;
    sf_close(sf);
    return r;
#else
    (void)path;
    return 0;
#endif
}
