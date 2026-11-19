/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _POSIX_C_SOURCE 200809L
#include "stream.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Quality (1.4.7, "as clean as it gets"): 64 zero crossings on each side, Kaiser beta 13, 1024 phases with linear
 * interpolation between them, cutoff 95.5 % of the Nyquist of the lower rate (44.1 kHz -> passes 0..20 kHz flat
 * within 0.01 dB, stops what would fold back). Measured (tests/test_resample.c): noise + distortion + aliasing
 * >= 128 dB below the signal from 0 to 20 kHz, 44.1 <-> 48 kHz (1.4.6: 92..123 dB, an image at 21 kHz only 25 dB down).
 * Cost: ~0.3 % of one core per playing clip (x86-64), ~1 % on a 32-bit x87 build (2.0: about 3x less, see Kernel). */
#define RS_HW   64
#define RS_NPH  1024
#define RS_BETA 13.0
#define RS_CUT  0.955

/* 2.0: the usual rates are in a small whole-number ratio (44100 -> 48000 = 147 : 160). Then only L different
 * fractional positions ever occur (160 here), so the filter is stored exactly for each of them (no interpolation
 * between 1024 phases = fewer operations and an even more exact result), the stereo taps side by side (L L R R ...
 * -> 4 multiplications at once on every CPU with SSE). About 3x cheaper per playing clip. */
#define RS_MAXL     4096
#define RS_MAXPTAB  (1u << 20)         /* floats: at most 4 MB of exact tables, otherwise the general way */

typedef struct {
    int    src, dst;
    int    W;          /* half width in input frames */
    int    taps;       /* 2*W */
    float *tab;        /* (RS_NPH + 1) * taps  (the general way, only when L == 0) */
    int    L, M;       /* 2.0: dst/g and src/g when the exact tables exist, else 0 */
    float *ptab2;      /* L * taps * 2: per phase, every tap twice (stereo) */
    float *ptab1;      /* L * taps: per phase (multichannel sources) */
    bool   owned;      /* not in the global cache -> ctx frees it */
} Kernel;

static void kernel_free(Kernel *k)
{
    if (!k) return;
    free(k->tab); free(k->ptab2); free(k->ptab1); free(k);
}

static pthread_mutex_t g_kmu = PTHREAD_MUTEX_INITIALIZER;
#define KCACHE 16
static Kernel *g_kc[KCACHE];
static int     g_kn;

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0, q = x * x * 0.25;
    for (int k = 1; k < 60; ++k) {
        term *= q / ((double)k * (double)k);
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

/* one row of the filter for the fractional position frac (0..1), normalised to DC = exactly 1 */
static void kernel_row(double *row, int W, int taps, double c, double i0b, double frac)
{
    double sum = 0.0;
    for (int m = 0; m < taps; ++m) {
        double x = frac - (double)(m - (W - 1));          /* t - i */
        double u = x / (double)W;
        double h = 0.0;
        if (u > -1.0 && u < 1.0) {
            double a = M_PI * c * x;
            double sinc = fabs(a) < 1e-12 ? 1.0 : sin(a) / a;
            double win = bessel_i0(RS_BETA * sqrt(1.0 - u * u)) / i0b;
            h = c * sinc * win;
        }
        row[m] = h;
        sum += h;
    }
    for (int m = 0; m < taps; ++m) row[m] /= sum;
}

static int gcd_i(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }

static Kernel *kernel_build(int src, int dst)
{
    double c = (dst < src ? (double)dst / (double)src : 1.0) * RS_CUT;   /* cutoff relative to the input Nyquist */
    int W = (int)ceil((double)RS_HW / c);
    int taps = 2 * W;

    Kernel *k = calloc(1, sizeof(*k));
    if (!k) return NULL;
    k->src = src; k->dst = dst; k->W = W; k->taps = taps;
    double i0b = bessel_i0(RS_BETA);
    double *row = malloc((size_t)taps * sizeof(double));
    if (!row) { free(k); return NULL; }

    int g = gcd_i(src, dst), L = dst / g, M = src / g;
    if (L <= RS_MAXL && (size_t)L * (size_t)taps * 2 <= RS_MAXPTAB) {
        k->ptab2 = malloc((size_t)L * (size_t)taps * 2 * sizeof(float));
        k->ptab1 = malloc((size_t)L * (size_t)taps * sizeof(float));
        if (k->ptab2 && k->ptab1) {
            for (int p = 0; p < L; ++p) {
                kernel_row(row, W, taps, c, i0b, (double)p / (double)L);
                float *r2 = k->ptab2 + (size_t)p * (size_t)taps * 2, *r1 = k->ptab1 + (size_t)p * (size_t)taps;
                for (int m = 0; m < taps; ++m) { r1[m] = (float)row[m]; r2[2 * m] = r2[2 * m + 1] = (float)row[m]; }
            }
            k->L = L; k->M = M;
            free(row);
            return k;
        }
        free(k->ptab2); free(k->ptab1); k->ptab2 = k->ptab1 = NULL;
    }

    k->tab = malloc((size_t)(RS_NPH + 1) * (size_t)taps * sizeof(float));
    if (!k->tab) { free(row); free(k); return NULL; }
    for (int p = 0; p <= RS_NPH; ++p) {
        kernel_row(row, W, taps, c, i0b, (double)p / (double)RS_NPH);
        for (int m = 0; m < taps; ++m) k->tab[(size_t)p * (size_t)taps + (size_t)m] = (float)row[m];
    }
    free(row);
    return k;
}

static Kernel *kernel_get(int src, int dst)
{
    pthread_mutex_lock(&g_kmu);
    for (int i = 0; i < g_kn; ++i)
        if (g_kc[i]->src == src && g_kc[i]->dst == dst) {
            Kernel *k = g_kc[i];
            pthread_mutex_unlock(&g_kmu);
            return k;
        }
    Kernel *k = kernel_build(src, dst);
    if (k && g_kn < KCACHE) g_kc[g_kn++] = k;
    else if (k) k->owned = true;
    pthread_mutex_unlock(&g_kmu);
    return k;
}

void stream_resample_shutdown(void)
{
    pthread_mutex_lock(&g_kmu);
    for (int i = 0; i < g_kn; ++i) { kernel_free(g_kc[i]); g_kc[i] = NULL; }
    g_kn = 0;
    pthread_mutex_unlock(&g_kmu);
}

typedef struct {
    StreamSource inner;
    Kernel      *k;
    double       ratio;        /* src / dst */
    size_t       in_frames, out_frames;

    float       *buf;          /* window of input frames [buf_lo, buf_lo + buf_n) */
    size_t       buf_cap;
    int          nch;          /* 1.4.24: 2 or the multichannel source's channels */
    int64_t      buf_lo;
    size_t       buf_n;

    float       *tmp;          /* reading from inner */
    size_t       tmp_cap;
    size_t       inner_pos;    /* where inner is after the last read */
    bool         inner_pos_valid;
} Rs;

static size_t rs_frames(void *c) { return ((Rs *)c)->out_frames; }

/* Fills buf[off .. off+n) with the input frames [from, from+n); outside the file = silence. */
static bool rs_fill(Rs *r, float *dst, int64_t from, size_t n)
{
    const size_t C = (size_t)r->nch;
    size_t i = 0;
    while (i < n) {
        int64_t f = from + (int64_t)i;
        if (f < 0) {
            size_t z = (size_t)(-f);
            if (z > n - i) z = n - i;
            memset(dst + i * C, 0, z * C * sizeof(float));
            i += z;
        } else if ((size_t)f >= r->in_frames) {
            memset(dst + i * C, 0, (n - i) * C * sizeof(float));
            i = n;
        } else {
            size_t want = n - i;
            if (want > r->in_frames - (size_t)f) want = r->in_frames - (size_t)f;
            size_t got = r->inner.read(r->inner.ctx, (size_t)f, dst + i * C, want);
            if (got < want) {                       /* short file / error -> silence to the end */
                memset(dst + (i + got) * C, 0, (want - got) * C * sizeof(float));
            }
            i += want;
        }
    }
    return true;
}

/* 2.0: the exact tables (see Kernel). Output frame o sits at input position o*M/L: whole part = o*M / L,
 * phase = o*M % L - counted up step by step, no division per frame. The summation order is fixed -> every read
 * and seek gives exactly the same result. */
typedef float rs_v4 __attribute__((vector_size(16)));
static size_t rs_exact(Rs *r, size_t pos, float *out, size_t n, int64_t lo)
{
    const Kernel *k = r->k;
    const int W = k->W, taps = k->taps, L = k->L, M = k->M;
    uint64_t idx = (uint64_t)pos * (uint64_t)M;
    int64_t fl = (int64_t)(idx / (uint64_t)L);
    int ph = (int)(idx % (uint64_t)L);
    if (r->nch == 2) {
        const int nf = taps * 2;                          /* floats per row (a multiple of 4: taps is even) */
        for (size_t j = 0; j < n; ++j) {
            const float *h = k->ptab2 + (size_t)ph * (size_t)nf;
            const float *in = r->buf + (size_t)(fl - (W - 1) - lo) * 2;
            rs_v4 a0 = { 0, 0, 0, 0 }, a1 = { 0, 0, 0, 0 }, a2 = { 0, 0, 0, 0 }, a3 = { 0, 0, 0, 0 };
            int m = 0;
            for (; m + 16 <= nf; m += 16) {
                rs_v4 x0, x1, x2, x3, h0, h1, h2, h3;
                memcpy(&x0, in + m, 16); memcpy(&x1, in + m + 4, 16); memcpy(&x2, in + m + 8, 16); memcpy(&x3, in + m + 12, 16);
                memcpy(&h0, h + m, 16);  memcpy(&h1, h + m + 4, 16);  memcpy(&h2, h + m + 8, 16);  memcpy(&h3, h + m + 12, 16);
                a0 += x0 * h0; a1 += x1 * h1; a2 += x2 * h2; a3 += x3 * h3;
            }
            for (; m + 8 <= nf; m += 8) {
                rs_v4 x0, x1, h0, h1;
                memcpy(&x0, in + m, 16); memcpy(&x1, in + m + 4, 16);
                memcpy(&h0, h + m, 16);  memcpy(&h1, h + m + 4, 16);
                a0 += x0 * h0; a1 += x1 * h1;
            }
            for (; m + 4 <= nf; m += 4) {
                rs_v4 x0, h0;
                memcpy(&x0, in + m, 16); memcpy(&h0, h + m, 16);
                a0 += x0 * h0;
            }
            rs_v4 a = (a0 + a1) + (a2 + a3);
            out[j * 2]     = a[0] + a[2];
            out[j * 2 + 1] = a[1] + a[3];
            ph += M;
            while (ph >= L) { ph -= L; ++fl; }
        }
        return n;
    }
    const int C = r->nch;
    for (size_t j = 0; j < n; ++j) {
        const float *h = k->ptab1 + (size_t)ph * (size_t)taps;
        const float *in = r->buf + (size_t)(fl - (W - 1) - lo) * (size_t)C;
        float acc[STREAM_MAX_CH] = { 0 };
        for (int m = 0; m < taps; ++m) {
            const float *q = in + (size_t)m * (size_t)C;
            for (int c = 0; c < C; ++c) acc[c] += h[m] * q[c];
        }
        for (int c = 0; c < C; ++c) out[j * (size_t)C + (size_t)c] = acc[c];
        ph += M;
        while (ph >= L) { ph -= L; ++fl; }
    }
    return n;
}

static size_t rs_read(void *c, size_t pos, float *out, size_t n)
{
    Rs *r = c;
    if (pos >= r->out_frames) return 0;
    if (n > r->out_frames - pos) n = r->out_frames - pos;
    if (n == 0) return 0;

    const int W = r->k->W, taps = r->k->taps;

    int64_t f_first, f_last;
    if (r->k->L) {                                         /* 2.0: exact (the same arithmetic as rs_exact) */
        f_first = (int64_t)((uint64_t)pos * (uint64_t)r->k->M / (uint64_t)r->k->L);
        f_last  = (int64_t)((uint64_t)(pos + n - 1) * (uint64_t)r->k->M / (uint64_t)r->k->L);
    } else {
        f_first = (int64_t)floor((double)pos * r->ratio);
        f_last  = (int64_t)floor((double)(pos + n - 1) * r->ratio);
    }
    int64_t lo = f_first - (W - 1);
    int64_t hi = f_last + W + 1;                           /* exclusive */
    size_t need = (size_t)(hi - lo);

    if (need > r->buf_cap) {
        size_t cap = need + 1024;
        float *nb = malloc(cap * (size_t)r->nch * sizeof(float));
        if (!nb) return 0;
        if (r->buf_n) memcpy(nb, r->buf, r->buf_n * (size_t)r->nch * sizeof(float));
        free(r->buf);
        r->buf = nb;
        r->buf_cap = cap;
    }

    /* reuse the overlap with the previous window */
    int64_t old_hi = r->buf_lo + (int64_t)r->buf_n;
    if (r->buf_n && lo >= r->buf_lo && lo <= old_hi) {
        size_t drop = (size_t)(lo - r->buf_lo);
        size_t keep = r->buf_n - drop;
        if (drop) memmove(r->buf, r->buf + drop * (size_t)r->nch, keep * (size_t)r->nch * sizeof(float));
        if (keep > need) keep = need;
        rs_fill(r, r->buf + keep * (size_t)r->nch, lo + (int64_t)keep, need - keep);
    } else {
        rs_fill(r, r->buf, lo, need);
    }
    r->buf_lo = lo;
    r->buf_n = need;

    if (r->k->L) return rs_exact(r, pos, out, n, lo);
    const float *tab = r->k->tab;
    if (r->nch != 2) {                                     /* 1.4.24: a multichannel source (the same filter) */
        const int C = r->nch;
        for (size_t j = 0; j < n; ++j) {
            double t = (double)(pos + j) * r->ratio;
            double fl = floor(t);
            double ph = (t - fl) * (double)RS_NPH;
            int p0 = (int)ph;
            float a = (float)(ph - (double)p0);
            const float *r0 = tab + (size_t)p0 * (size_t)taps;
            const float *r1 = r0 + taps;
            const float *in = r->buf + (size_t)((int64_t)fl - (W - 1) - lo) * (size_t)C;
            float acc[STREAM_MAX_CH] = { 0 };
            for (int m = 0; m < taps; ++m) {
                float h = r0[m] + a * (r1[m] - r0[m]);
                const float *q = in + (size_t)m * (size_t)C;
                for (int k = 0; k < C; ++k) acc[k] += h * q[k];
            }
            for (int k = 0; k < C; ++k) out[j * (size_t)C + (size_t)k] = acc[k];
        }
        return n;
    }
    for (size_t j = 0; j < n; ++j) {
        double t = (double)(pos + j) * r->ratio;
        double fl = floor(t);
        double ph = (t - fl) * (double)RS_NPH;
        int p0 = (int)ph;
        float a = (float)(ph - (double)p0);
        const float *r0 = tab + (size_t)p0 * (size_t)taps;
        const float *r1 = r0 + taps;
        const float *in = r->buf + (size_t)((int64_t)fl - (W - 1) - lo) * 2;

        /* 4 independent accumulators per channel: 2-3x faster on an old CPU (ILP) without -ffast-math.
         * The summation order is fixed -> the result is the same on every read/seek. */
        float aL0 = 0, aL1 = 0, aL2 = 0, aL3 = 0, aR0 = 0, aR1 = 0, aR2 = 0, aR3 = 0;
        int m = 0;
        for (; m + 4 <= taps; m += 4) {
            float h0 = r0[m]     + a * (r1[m]     - r0[m]);
            float h1 = r0[m + 1] + a * (r1[m + 1] - r0[m + 1]);
            float h2 = r0[m + 2] + a * (r1[m + 2] - r0[m + 2]);
            float h3 = r0[m + 3] + a * (r1[m + 3] - r0[m + 3]);
            const float *q = in + m * 2;
            aL0 += h0 * q[0]; aR0 += h0 * q[1];
            aL1 += h1 * q[2]; aR1 += h1 * q[3];
            aL2 += h2 * q[4]; aR2 += h2 * q[5];
            aL3 += h3 * q[6]; aR3 += h3 * q[7];
        }
        for (; m < taps; ++m) {
            float h = r0[m] + a * (r1[m] - r0[m]);
            aL0 += h * in[m * 2];
            aR0 += h * in[m * 2 + 1];
        }
        float accL = (aL0 + aL1) + (aL2 + aL3);
        float accR = (aR0 + aR1) + (aR2 + aR3);
        out[j * 2]     = accL;
        out[j * 2 + 1] = accR;
    }
    return n;
}

static void rs_close(void *c)
{
    Rs *r = c;
    if (!r) return;
    if (r->inner.close) r->inner.close(r->inner.ctx);
    if (r->k && r->k->owned) kernel_free(r->k);
    free(r->buf);
    free(r->tmp);
    free(r);
}

int stream_source_resample(const StreamSource *inner, int src_sr, int dst_sr, StreamSource *out)
{
    if (!inner || !out || src_sr <= 0 || dst_sr <= 0) return -1;
    Rs *r = calloc(1, sizeof(*r));
    if (!r) return -1;
    r->inner = *inner;
    r->nch = stream_src_nch(inner);
    r->k = kernel_get(src_sr, dst_sr);
    if (!r->k) { free(r); return -1; }
    r->ratio = (double)src_sr / (double)dst_sr;
    r->in_frames = inner->frames(inner->ctx);
    r->out_frames = (size_t)((double)r->in_frames / r->ratio);
    if (r->out_frames == 0) r->out_frames = 1;

    out->frames = rs_frames;
    out->read   = rs_read;
    out->close  = rs_close;
    out->ctx    = r;
    out->nch    = inner->nch;
    return 0;
}
