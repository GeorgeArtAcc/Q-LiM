/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#include "stream.h"
#include <sndfile.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

typedef struct {
    SNDFILE *sf;
    int      channels;
    size_t   frames;
    size_t   cur;        /* current position in the file */
    float   *buf;        /* only when channels != 2 */
    bool     mc;         /* 1.4.24: every channel as it is (not mixed to stereo) */
    int      pick;       /* 1.4.24: >= 0 = only this channel (L = R = it), for its waveform */
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
        if ((x->channels == 2 && x->pick < 0) || x->mc) {
            r = read_raw(x, out + got * (size_t)x->channels, m);
        } else {
            if (x->buf_frames < m) {
                float *nb = realloc(x->buf, m * (size_t)x->channels * sizeof(float));
                if (!nb) break;
                x->buf = nb;
                x->buf_frames = m;
            }
            r = read_raw(x, x->buf, m);
            for (sf_count_t i = 0; i < r; ++i) {   /* as in wav_load: mono -> L=R, >2 -> the first 2 */
                float l = x->buf[i * x->channels + (x->pick > 0 ? x->pick : 0)];
                float rr = x->channels == 1 || x->pick >= 0 ? l : x->buf[i * x->channels + 1];
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
    for (size_t i = 0; i < got * (size_t)(x->mc ? x->channels : 2); ++i) {
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

    if (!audio_sndfile_can_open(path)) return -1;    /* 1.4.14: missing, or a format only ffmpeg reads: no noise */
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
    x->pick = -1;

    out->frames = sf_frames_cb;
    out->read   = sf_read_cb;
    out->close  = sf_close_cb;
    out->ctx    = x;
    out->nch    = 0;
    return 0;
}

/* 1.4.24: one channel of the file as a stereo source (L = R = that channel), at the file's rate */
int stream_source_sndfile_channel(const char *path, StreamSource *out, int *file_sr, int ch)
{
    if (stream_source_sndfile_native(path, out, file_sr) != 0) return -1;
    SfCtx *x = out->ctx;
    if (ch >= 0 && ch < x->channels) x->pick = ch;
    return 0;
}

int stream_file_channels(const char *path)
{
    if (!path || !audio_sndfile_can_open(path)) return 0;
    SF_INFO info;
    memset(&info, 0, sizeof info);
    SNDFILE *sf = sf_open(path, SFM_READ, &info);
    if (!sf) return 0;
    sf_close(sf);
    return info.channels;
}

int stream_source_open_mc(const char *path, int target_sr, StreamSource *out, int *file_sr, int *nch)
{
    StreamSource nat;
    int fsr = 0;
    if (stream_source_sndfile_native(path, &nat, &fsr) != 0) return -1;
    SfCtx *x = nat.ctx;
    if (x->channels > 2 && x->channels <= STREAM_MAX_CH) { x->mc = true; nat.nch = x->channels; }
    if (nch) *nch = stream_src_nch(&nat);
    if (file_sr) *file_sr = fsr;
    if (fsr == target_sr) { *out = nat; return 0; }
    if (stream_source_resample(&nat, fsr, target_sr, out) != 0) {
        nat.close(nat.ctx);
        return -1;
    }
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

/* ---------- 1.4.14: what the file really is, by its content ---------- */

/* a valid MPEG audio frame header at b[0..3] -> its length in bytes (0 = not a header) */
static size_t mpeg_frame_len(const unsigned char *b)
{
    if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0) return 0;
    int ver = (b[1] >> 3) & 3, layer = (b[1] >> 1) & 3, bri = (b[2] >> 4) & 15, sri = (b[2] >> 2) & 3, pad = (b[2] >> 1) & 1;
    if (ver == 1 || layer == 0 || bri == 0 || bri == 15 || sri == 3) return 0;
    static const int br[2][3][16] = {
        { { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0 },     /* MPEG1 L1 */
          { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0 },       /* MPEG1 L2 */
          { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 } },      /* MPEG1 L3 */
        { { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0 },       /* MPEG2 L1 */
          { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 },            /* MPEG2 L2 */
          { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 } } };        /* MPEG2 L3 */
    static const int srt[3] = { 44100, 48000, 32000 };
    int v1 = ver == 3, L = 3 - layer;                     /* L: 0 = layer 1 .. 2 = layer 3 */
    int sr = srt[sri] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
    int kbps = br[v1 ? 0 : 1][L][bri];
    if (L == 0) return (size_t)((12 * kbps * 1000 / sr + pad) * 4);
    int coef = (L == 2 && !v1) ? 72 : 144;
    return (size_t)(coef * kbps * 1000 / sr + pad);
}

int audio_sniff(const char *path, char *what, size_t what_size)
{
    #define WHAT(s) do { if (what && what_size) snprintf(what, what_size, "%s", s); } while (0)
    WHAT("");
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) { WHAT("missing"); return SNIFF_MISSING; }
    enum { N = 65536 };
    unsigned char *b = malloc(N);
    if (!b) { fclose(f); return SNIFF_UNKNOWN; }
    size_t n = fread(b, 1, N, f);
    size_t off = 0;
    /* ID3v2 tag in front (MP3, sometimes FLAC/AAC): jump over it and look at what follows */
    if (n >= 10 && memcmp(b, "ID3", 3) == 0) {
        size_t sz = ((size_t)(b[6] & 0x7F) << 21) | ((size_t)(b[7] & 0x7F) << 14) | ((size_t)(b[8] & 0x7F) << 7) | (b[9] & 0x7F);
        off = 10 + sz + ((b[5] & 0x10) ? 10 : 0);
        if (off + 16 > n) {                                /* a big tag (cover picture): read past it */
            if (fseek(f, (long)off, SEEK_SET) == 0) { n = fread(b, 1, N, f); off = 0; }
            else n = 0;
        }
    }
    fclose(f);
    const unsigned char *p = b + off;
    size_t m = n > off ? n - off : 0;
    int r = SNIFF_UNKNOWN;
    if (m < 12) r = SNIFF_UNKNOWN;
    else if ((!memcmp(p, "RIFF", 4) || !memcmp(p, "RIFX", 4)) && !memcmp(p + 8, "WAVE", 4)) { WHAT("WAV"); r = SNIFF_SNDFILE; }
    else if (!memcmp(p, "RF64", 4) || !memcmp(p, "BW64", 4)) { WHAT("WAV (RF64)"); r = SNIFF_SNDFILE; }
    else if (!memcmp(p, "riff", 4)) { WHAT("W64"); r = SNIFF_SNDFILE; }
    else if (!memcmp(p, "FORM", 4) && (!memcmp(p + 8, "AIFF", 4) || !memcmp(p + 8, "AIFC", 4))) { WHAT("AIFF"); r = SNIFF_SNDFILE; }
    else if (!memcmp(p, "fLaC", 4)) { WHAT("FLAC"); r = SNIFF_SNDFILE; }
    else if (!memcmp(p, "OggS", 4)) {
        /* Vorbis / Opus / FLAC in Ogg: libsndfile; Speex and others: ffmpeg */
        int vorbis = 0, opus = 0, flac = 0;
        for (size_t i = 0; i + 8 <= m && i < 256; ++i) {
            if (!memcmp(p + i, "\x01vorbis", 7)) vorbis = 1;
            if (!memcmp(p + i, "OpusHead", 8)) opus = 1;
            if (!memcmp(p + i, "\x7F" "FLAC", 5)) flac = 1;
        }
        if (vorbis || opus || flac) { WHAT(opus ? "Opus" : flac ? "FLAC (Ogg)" : "Ogg Vorbis"); r = SNIFF_SNDFILE; }
        else { WHAT("Ogg"); r = SNIFF_FFMPEG; }
    }
    else if (!memcmp(p, "caff", 4)) { WHAT("CAF"); r = SNIFF_SNDFILE; }
    else if (!memcmp(p, ".snd", 4) || !memcmp(p, "FORM", 4) || (m >= 14 && !memcmp(p, "Creative Voice", 14))
             || !memcmp(p, "NIST_1A", 7) || !memcmp(p, "\x64\xA3\x01\x00", 4)) { r = SNIFF_SNDFILE; }
    else if (!memcmp(p + 4, "ftyp", 4)) { WHAT("M4A/AAC"); r = SNIFF_FFMPEG; }
    else if (!memcmp(p, "\x30\x26\xB2\x75\x8E\x66\xCF\x11", 8)) { WHAT("WMA"); r = SNIFF_FFMPEG; }
    else if (!memcmp(p, "MAC ", 4)) { WHAT("APE"); r = SNIFF_FFMPEG; }
    else if (!memcmp(p, "wvpk", 4)) { WHAT("WavPack"); r = SNIFF_FFMPEG; }
    else if (!memcmp(p, "TTA1", 4)) { WHAT("TTA"); r = SNIFF_FFMPEG; }
    else if (!memcmp(p, "MPCK", 4) || !memcmp(p, "MP+", 3)) { WHAT("Musepack"); r = SNIFF_FFMPEG; }
    else if (!memcmp(p, "\x1A\x45\xDF\xA3", 4)) { WHAT("Matroska/WebM"); r = SNIFF_FFMPEG; }
    else if (!memcmp(p, "#!AMR", 5)) { WHAT("AMR"); r = SNIFF_FFMPEG; }
    else if (!memcmp(p, "DSD ", 4) || !memcmp(p, "FRM8", 4)) { WHAT("DSD"); r = SNIFF_FFMPEG; }
    else if (!memcmp(p, ".RMF", 4)) { WHAT("RealAudio"); r = SNIFF_FFMPEG; }
    else if (!memcmp(p, "\x00\x00\x01\xBA", 4) || !memcmp(p, "\x00\x00\x01\xB3", 4)) { WHAT("MPEG video"); r = SNIFF_FFMPEG; }
    else if (!memcmp(p, "RIFF", 4)) { WHAT("RIFF (not WAV)"); r = SNIFF_FFMPEG; }
    else {
        /* MPEG audio or ADTS AAC: look for two frames in a row (an MP3 can start with some junk) */
        for (size_t i = 0; i + 4 <= m && i < 32768; ++i) {
            if (p[i] != 0xFF || (p[i + 1] & 0xE0) != 0xE0) continue;
            if ((p[i + 1] & 0xF6) == 0xF0) {               /* ADTS: layer bits 00 */
                if (i + 6 > m) break;
                size_t fl = ((size_t)(p[i + 3] & 3) << 11) | ((size_t)p[i + 4] << 3) | (p[i + 5] >> 5);
                if (fl > 7 && i + fl + 2 <= m && p[i + fl] == 0xFF && (p[i + fl + 1] & 0xF6) == 0xF0) {
                    WHAT("AAC (ADTS)"); r = SNIFF_FFMPEG; break;
                }
                continue;
            }
            size_t fl = mpeg_frame_len(p + i);
            if (fl >= 24 && i + fl + 4 <= m && mpeg_frame_len(p + i + fl)) {
                int layer = (p[i + 1] >> 1) & 3;
                WHAT(layer == 1 ? "MP3" : layer == 2 ? "MP2" : "MP1");
                r = SNIFF_MP3; break;
            }
        }
    }
    free(b);
    return r;
    #undef WHAT
}

int audio_sndfile_has_mp3(void)
{
    static int v = -1;
    if (v < 0) {
        SF_INFO i;
        memset(&i, 0, sizeof i);
        i.samplerate = 44100; i.channels = 2; i.format = SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III;
        v = sf_format_check(&i) ? 1 : 0;
    }
    return v;
}

int audio_sndfile_can_open(const char *path)
{
    int k = audio_sniff(path, NULL, 0);
    if (k == SNIFF_MISSING || k == SNIFF_FFMPEG) return 0;
    if (k == SNIFF_MP3 && !audio_sndfile_has_mp3()) return 0;   /* this libsndfile has no MP3 decoder */
    if (k == SNIFF_UNKNOWN) {
        /* nothing recognised: an unknown file NAMED .mp3 would only make mpg123 search and complain */
        const char *dot = strrchr(path, '.');
        if (dot && (!strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".mp2") || !strcasecmp(dot, ".mpga"))) return 0;
    }
    return 1;
}
