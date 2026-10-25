/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _POSIX_C_SOURCE 200809L
#include "peaks.h"
#include "stream.h"

#include <pthread.h>
#include <stdatomic.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define READ_FRAMES 16384u
#define L2_BLOCKS   16u                 /* second level: 1 entry per 16 blocks (for fast drawing of the whole clip) */

/* one block: min/max for L and R + RMS (16-bit, so it does not eat much memory: 12 bytes per 256 frames) */
typedef struct { int16_t lmin, lmax, rmin, rmax; uint16_t lrms, rrms; } Blk;

static inline int16_t q16(float v)
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)lrintf(v * 32767.0f);
}
struct Peaks {
    char          *path;
    int            sr;
    size_t         frames;
    size_t         nblocks;
    float         *data;                /* mono |max| (the old one, for peaks_max) */
    Blk           *blk;                 /* detailed, per block */
    Blk           *l2;                  /* merged per L2_BLOCKS blocks */
    size_t         nl2;
    atomic_size_t  ready;
    atomic_bool    cancel;
    atomic_bool    failed;
    atomic_int     refs;
    struct Peaks  *next;
    /* detailed reading (UI thread only) */
    StreamSource   dsrc;
    bool           dopen;
    int            dfile_sr;
    float         *dbuf;
    size_t         dcap;
};

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static Peaks          *g_head, *g_tail;
static pthread_t       g_thr;
static bool            g_started, g_quit;

static void peaks_unref(Peaks *p)
{
    if (atomic_fetch_sub(&p->refs, 1) == 1) {
        if (p->dopen) p->dsrc.close(p->dsrc.ctx);
        free(p->dbuf);
        free(p->path);
        free(p->data);
        free(p->blk);
        free(p->l2);
        free(p);
    }
}

/* second level: merges the blocks of group g (called when the last one of the group is ready) */
static void l2_close(Peaks *p, size_t g)
{
    size_t b0 = g * L2_BLOCKS, b1 = b0 + L2_BLOCKS;
    if (b1 > p->nblocks) b1 = p->nblocks;
    Blk o = { 0, 0, 0, 0, 0, 0 };
    double ls = 0, rs = 0;
    for (size_t b = b0; b < b1; ++b) {
        const Blk *k = &p->blk[b];
        if (k->lmin < o.lmin) o.lmin = k->lmin;
        if (k->lmax > o.lmax) o.lmax = k->lmax;
        if (k->rmin < o.rmin) o.rmin = k->rmin;
        if (k->rmax > o.rmax) o.rmax = k->rmax;
        ls += (double)k->lrms * k->lrms; rs += (double)k->rrms * k->rrms;
    }
    size_t n = b1 - b0;
    o.lrms = (uint16_t)lrint(sqrt(ls / (double)n));
    o.rrms = (uint16_t)lrint(sqrt(rs / (double)n));
    p->l2[g] = o;
}

static void compute(Peaks *p)
{
    /* The sketch is computed from the ORIGINAL file (no resampling -> cheap) and mapped onto
     * blocks of OUTPUT frames (the session rate), so it matches the waveform on screen. */
    StreamSource src;
    int file_sr = 0;
    if (stream_source_sndfile_native(p->path, &src, &file_sr) != 0 || file_sr <= 0) {
        atomic_store(&p->failed, true);
        return;
    }
    size_t in_frames = src.frames(src.ctx);
    double ratio = (double)p->sr / (double)file_sr;     /* output frames per input frame */

    float *buf = malloc((size_t)READ_FRAMES * 2 * sizeof(float));
    if (!buf) { atomic_store(&p->failed, true); src.close(src.ctx); return; }

    size_t pos = 0, cur_b = 0;
    float cur = 0.0f;
    float lmn = 0, lmx = 0, rmn = 0, rmx = 0;
    double lsq = 0, rsq = 0;
    size_t cnt = 0;
    #define CLOSE_BLOCK() do { \
        p->data[cur_b] = cur; \
        Blk *bk = &p->blk[cur_b]; \
        bk->lmin = q16(lmn); bk->lmax = q16(lmx); bk->rmin = q16(rmn); bk->rmax = q16(rmx); \
        bk->lrms = (uint16_t)lrintf((cnt ? sqrtf((float)(lsq / cnt)) : 0.0f) * 65535.0f); \
        bk->rrms = (uint16_t)lrintf((cnt ? sqrtf((float)(rsq / cnt)) : 0.0f) * 65535.0f); \
        if (cur_b % L2_BLOCKS == L2_BLOCKS - 1 || cur_b + 1 == p->nblocks) l2_close(p, cur_b / L2_BLOCKS); \
        ++cur_b; \
        atomic_store_explicit(&p->ready, cur_b, memory_order_release); \
        cur = 0.0f; lmn = lmx = rmn = rmx = 0; lsq = rsq = 0; cnt = 0; \
    } while (0)
    while (pos < in_frames && !atomic_load(&p->cancel)) {
        size_t n = READ_FRAMES;
        if (n > in_frames - pos) n = in_frames - pos;
        size_t got = src.read(src.ctx, pos, buf, n);
        if (got == 0) break;
        for (size_t i = 0; i < got; ++i) {
            size_t b = (size_t)((double)(pos + i) * ratio) / PEAKS_BLOCK;
            if (b >= p->nblocks) b = p->nblocks - 1;
            while (cur_b < b) CLOSE_BLOCK();             /* close the finished blocks */
            float L = buf[i * 2], R = buf[i * 2 + 1];
            if (L < lmn) lmn = L;
            if (L > lmx) lmx = L;
            if (R < rmn) rmn = R;
            if (R > rmx) rmx = R;
            lsq += (double)L * L; rsq += (double)R * R; cnt++;
            float m = (buf[i * 2] + buf[i * 2 + 1]) * 0.5f;
            if (m < 0) m = -m;
            if (m > cur) cur = m;
        }
        pos += got;
    }
    if (!atomic_load(&p->cancel)) {                      /* the last (and possibly skipped) blocks */
        while (cur_b < p->nblocks) CLOSE_BLOCK();
    #undef CLOSE_BLOCK
    }
    free(buf);
    src.close(src.ctx);
}

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_mu);
    for (;;) {
        while (!g_head && !g_quit) pthread_cond_wait(&g_cv, &g_mu);
        if (g_quit) break;
        Peaks *p = g_head;
        g_head = p->next;
        if (!g_head) g_tail = NULL;
        pthread_mutex_unlock(&g_mu);

        if (!atomic_load(&p->cancel)) compute(p);
        peaks_unref(p);                 /* the worker's reference */

        pthread_mutex_lock(&g_mu);
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

Peaks *peaks_start(const char *path, int sr, size_t frames)
{
    if (!path || frames == 0) return NULL;
    Peaks *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->nblocks = (frames + PEAKS_BLOCK - 1) / PEAKS_BLOCK;
    p->data = calloc(p->nblocks, sizeof(float));
    p->nl2 = (p->nblocks + L2_BLOCKS - 1) / L2_BLOCKS;
    p->blk = calloc(p->nblocks, sizeof(Blk));
    p->l2 = calloc(p->nl2, sizeof(Blk));
    p->path = strdup(path);
    if (!p->data || !p->blk || !p->l2 || !p->path) { free(p->data); free(p->blk); free(p->l2); free(p->path); free(p); return NULL; }
    p->sr = sr;
    p->frames = frames;
    atomic_init(&p->refs, 2);           /* clip + worker */

    pthread_mutex_lock(&g_mu);
    if (!g_started) {
        g_quit = false;
        if (pthread_create(&g_thr, NULL, worker, NULL) != 0) {
            pthread_mutex_unlock(&g_mu);
            free(p->data); free(p->blk); free(p->l2); free(p->path); free(p);
            return NULL;
        }
        g_started = true;
    }
    if (g_tail) g_tail->next = p; else g_head = p;
    g_tail = p;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return p;
}

void peaks_release(Peaks *p)
{
    if (!p) return;
    atomic_store(&p->cancel, true);
    peaks_unref(p);
}

void peaks_shutdown(void)
{
    pthread_mutex_lock(&g_mu);
    if (!g_started) { pthread_mutex_unlock(&g_mu); return; }
    g_quit = true;
    for (Peaks *p = g_head; p;) {       /* the jobs not started yet */
        Peaks *n = p->next;
        atomic_store(&p->cancel, true);
        peaks_unref(p);
        p = n;
    }
    g_head = g_tail = NULL;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    pthread_join(g_thr, NULL);
    g_started = false;
}

size_t peaks_ready(const Peaks *p)
{
    return p ? atomic_load_explicit(&((Peaks *)p)->ready, memory_order_acquire) : 0;
}
size_t peaks_total(const Peaks *p) { return p ? p->nblocks : 0; }
bool   peaks_done(const Peaks *p)
{
    return p && (peaks_ready(p) >= p->nblocks || atomic_load(&((Peaks *)p)->failed));
}

float peaks_max(const Peaks *p, size_t b0, size_t b1)
{
    if (!p) return 0.0f;
    size_t ready = peaks_ready(p);
    if (b1 >= ready) { if (ready == 0) return 0.0f; b1 = ready - 1; }
    float m = 0.0f;
    for (size_t b = b0; b <= b1; ++b) if (p->data[b] > m) m = p->data[b];
    return m;
}

bool peaks_detail(Peaks *p, size_t start, double fpb, int n_bars, float *out)
{
    if (!p || !out || n_bars <= 0 || fpb <= 0.0) return false;
    if (!p->dopen) {
        if (stream_source_sndfile_native(p->path, &p->dsrc, &p->dfile_sr) != 0 || p->dfile_sr <= 0) return false;
        p->dopen = true;
    }
    double ratio = (double)p->sr / (double)p->dfile_sr;       /* output frames per input frame */
    size_t in_total = p->dsrc.frames(p->dsrc.ctx);
    size_t in_start = (size_t)((double)start / ratio);
    size_t in_len   = (size_t)(fpb * (double)n_bars / ratio) + 2;
    if (in_start >= in_total) { for (int i = 0; i < n_bars; ++i) out[i] = 0.0f; return true; }
    if (in_len > in_total - in_start) in_len = in_total - in_start;
    if (in_len > (size_t)8 * 1024 * 1024) return false;       /* protection: do not read gigantic windows */

    if (p->dcap < in_len) {
        float *nb = realloc(p->dbuf, in_len * 2 * sizeof(float));
        if (!nb) return false;
        p->dbuf = nb; p->dcap = in_len;
    }
    size_t got = p->dsrc.read(p->dsrc.ctx, in_start, p->dbuf, in_len);

    for (int b = 0; b < n_bars; ++b) {
        size_t f0 = (size_t)((double)b * fpb / ratio);
        size_t f1 = (size_t)((double)(b + 1) * fpb / ratio);
        if (f1 <= f0) f1 = f0 + 1;
        float m = 0.0f;
        for (size_t f = f0; f < f1 && f < got; ++f) {
            float v = (p->dbuf[f * 2] + p->dbuf[f * 2 + 1]) * 0.5f;
            if (v < 0) v = -v;
            if (v > m) m = v;
        }
        out[b] = m;
    }
    return true;
}

static void acc_blk(const Blk *k, PeakVal *o, double *ls, double *rs)
{
    float a;
    if ((a = k->lmin / 32767.0f) < o->lmin) o->lmin = a;
    if ((a = k->lmax / 32767.0f) > o->lmax) o->lmax = a;
    if ((a = k->rmin / 32767.0f) < o->rmin) o->rmin = a;
    if ((a = k->rmax / 32767.0f) > o->rmax) o->rmax = a;
    double l = k->lrms / 65535.0, r = k->rrms / 65535.0;
    *ls += l * l; *rs += r * r;
}

bool peaks_range(const Peaks *p, size_t b0, size_t b1, PeakVal *out)
{
    memset(out, 0, sizeof(*out));
    if (!p) return false;
    size_t ready = peaks_ready(p);
    if (ready == 0 || b0 >= ready) return false;
    if (b1 >= ready) b1 = ready - 1;
    if (b1 < b0) b1 = b0;
    double ls = 0, rs = 0;
    size_t n = 0;
    size_t b = b0;
    while (b <= b1) {
        /* whole groups of the second level (fast when zoomed far out) */
        if (b % L2_BLOCKS == 0 && b + L2_BLOCKS - 1 <= b1 && b + L2_BLOCKS <= ready) {
            const Blk *k = &p->l2[b / L2_BLOCKS];
            double l0 = ls, r0 = rs;
            acc_blk(k, out, &ls, &rs);
            ls = l0 + (ls - l0) * L2_BLOCKS;           /* a group weighs as much as L2_BLOCKS blocks */
            rs = r0 + (rs - r0) * L2_BLOCKS;
            n += L2_BLOCKS;
            b += L2_BLOCKS;
        } else {
            acc_blk(&p->blk[b], out, &ls, &rs);
            n++;
            b++;
        }
    }
    out->lrms = (float)sqrt(ls / (double)n);
    out->rrms = (float)sqrt(rs / (double)n);
    return true;
}

bool peaks_detail_val(Peaks *p, size_t start, double fpb, int n_bars, PeakVal *out)
{
    if (!p || !out || n_bars <= 0 || fpb <= 0.0) return false;
    if (!p->dopen) {
        if (stream_source_sndfile_native(p->path, &p->dsrc, &p->dfile_sr) != 0 || p->dfile_sr <= 0) return false;
        p->dopen = true;
    }
    double ratio = (double)p->sr / (double)p->dfile_sr;
    size_t in_total = p->dsrc.frames(p->dsrc.ctx);
    size_t in_start = (size_t)((double)start / ratio);
    size_t in_len   = (size_t)(fpb * (double)n_bars / ratio) + 2;
    memset(out, 0, (size_t)n_bars * sizeof(*out));
    if (in_start >= in_total) return true;
    if (in_len > in_total - in_start) in_len = in_total - in_start;
    if (in_len > (size_t)8 * 1024 * 1024) return false;
    if (p->dcap < in_len) {
        float *nb = realloc(p->dbuf, in_len * 2 * sizeof(float));
        if (!nb) return false;
        p->dbuf = nb; p->dcap = in_len;
    }
    size_t got = p->dsrc.read(p->dsrc.ctx, in_start, p->dbuf, in_len);
    for (int b = 0; b < n_bars; ++b) {
        size_t f0 = (size_t)((double)b * fpb / ratio);
        size_t f1 = (size_t)((double)(b + 1) * fpb / ratio);
        if (f1 <= f0) f1 = f0 + 1;
        PeakVal *o = &out[b];
        double ls = 0, rs = 0;
        size_t n = 0;
        for (size_t f = f0; f < f1 && f < got; ++f) {
            float L = p->dbuf[f * 2], R = p->dbuf[f * 2 + 1];
            if (L < o->lmin) o->lmin = L;
            if (L > o->lmax) o->lmax = L;
            if (R < o->rmin) o->rmin = R;
            if (R > o->rmax) o->rmax = R;
            ls += (double)L * L; rs += (double)R * R; n++;
        }
        if (n) { o->lrms = (float)sqrt(ls / n); o->rrms = (float)sqrt(rs / n); }
    }
    return true;
}
