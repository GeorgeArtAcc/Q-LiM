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

/* Quality: 32 zero crossings on each side with cutoff = Nyquist of the lower rate,
 * Kaiser beta 9 (~ -90 dB side lobes), 256 phases with linear interpolation between them. */
#define RS_HW   32
#define RS_NPH  256
#define RS_BETA 9.0

typedef struct {
    int    src, dst;
    int    W;          /* half width in input frames */
    int    taps;       /* 2*W */
    float *tab;        /* (RS_NPH + 1) * taps */
    bool   owned;      /* not in the global cache -> ctx frees it */
} Kernel;

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

static Kernel *kernel_build(int src, int dst)
{
    double c = dst < src ? (double)dst / (double)src : 1.0;   /* cutoff relative to the input Nyquist */
    int W = (int)ceil((double)RS_HW / c);
    int taps = 2 * W;

    Kernel *k = calloc(1, sizeof(*k));
    if (!k) return NULL;
    k->tab = malloc((size_t)(RS_NPH + 1) * (size_t)taps * sizeof(float));
    if (!k->tab) { free(k); return NULL; }
    k->src = src; k->dst = dst; k->W = W; k->taps = taps;

    double i0b = bessel_i0(RS_BETA);
    double *row = malloc((size_t)taps * sizeof(double));
    if (!row) { free(k->tab); free(k); return NULL; }

    for (int p = 0; p <= RS_NPH; ++p) {
        double frac = (double)p / (double)RS_NPH;
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
        for (int m = 0; m < taps; ++m)
            k->tab[(size_t)p * (size_t)taps + (size_t)m] = (float)(row[m] / sum);   /* DC = exactly 1 */
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
    for (int i = 0; i < g_kn; ++i) { free(g_kc[i]->tab); free(g_kc[i]); g_kc[i] = NULL; }
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
    size_t i = 0;
    while (i < n) {
        int64_t f = from + (int64_t)i;
        if (f < 0) {
            size_t z = (size_t)(-f);
            if (z > n - i) z = n - i;
            memset(dst + i * 2, 0, z * 2 * sizeof(float));
            i += z;
        } else if ((size_t)f >= r->in_frames) {
            memset(dst + i * 2, 0, (n - i) * 2 * sizeof(float));
            i = n;
        } else {
            size_t want = n - i;
            if (want > r->in_frames - (size_t)f) want = r->in_frames - (size_t)f;
            size_t got = r->inner.read(r->inner.ctx, (size_t)f, dst + i * 2, want);
            if (got < want) {                       /* short file / error -> silence to the end */
                memset(dst + (i + got) * 2, 0, (want - got) * 2 * sizeof(float));
            }
            i += want;
        }
    }
    return true;
}

static size_t rs_read(void *c, size_t pos, float *out, size_t n)
{
    Rs *r = c;
    if (pos >= r->out_frames) return 0;
    if (n > r->out_frames - pos) n = r->out_frames - pos;
    if (n == 0) return 0;

    const int W = r->k->W, taps = r->k->taps;

    double t_first = (double)pos * r->ratio;
    double t_last  = (double)(pos + n - 1) * r->ratio;
    int64_t lo = (int64_t)floor(t_first) - (W - 1);
    int64_t hi = (int64_t)floor(t_last) + W + 1;          /* exclusive */
    size_t need = (size_t)(hi - lo);

    if (need > r->buf_cap) {
        size_t cap = need + 1024;
        float *nb = malloc(cap * 2 * sizeof(float));
        if (!nb) return 0;
        if (r->buf_n) memcpy(nb, r->buf, r->buf_n * 2 * sizeof(float));
        free(r->buf);
        r->buf = nb;
        r->buf_cap = cap;
    }

    /* reuse the overlap with the previous window */
    int64_t old_hi = r->buf_lo + (int64_t)r->buf_n;
    if (r->buf_n && lo >= r->buf_lo && lo <= old_hi) {
        size_t drop = (size_t)(lo - r->buf_lo);
        size_t keep = r->buf_n - drop;
        if (drop) memmove(r->buf, r->buf + drop * 2, keep * 2 * sizeof(float));
        if (keep > need) keep = need;
        rs_fill(r, r->buf + keep * 2, lo + (int64_t)keep, need - keep);
    } else {
        rs_fill(r, r->buf, lo, need);
    }
    r->buf_lo = lo;
    r->buf_n = need;

    const float *tab = r->k->tab;
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
    if (r->k && r->k->owned) { free(r->k->tab); free(r->k); }
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
    return 0;
}
