/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _POSIX_C_SOURCE 200809L
#include "bpm.h"
#include "stream.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define ANA_SR_TARGET 11025.0     /* the audio is downsampled to about this rate */
#define FFT_N         512
#define HOP           64          /* ~5.8 ms at 11 kHz -> ~180 values per second */

/* ===================== small FFT (radix-2, in place) ===================== */

static void fft_inplace(float *re, float *im, int n)
{
    for (int i = 1, j = 0; i < n; ++i) {               /* bit reversal */
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / len;
        float wr = (float)cos(ang), wi = (float)sin(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; ++k) {
                int a = i + k, b = i + k + len / 2;
                float xr = re[b] * cr - im[b] * ci;
                float xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

/* ===================== onset envelope ===================== */

/* x: mono at rate sr (already downsampled). Returns an array of "new sound strength" (onset), *out_n values. */
static float *onset_envelope(const float *x, size_t n, size_t *out_n)
{
    *out_n = 0;
    if (n < FFT_N * 4) return NULL;
    size_t frames = (n - FFT_N) / HOP + 1;
    float *env = calloc(frames, sizeof(float));
    float *re = malloc(FFT_N * sizeof(float)), *im = malloc(FFT_N * sizeof(float));
    float *win = malloc(FFT_N * sizeof(float));
    float *prev = calloc(FFT_N / 2 + 1, sizeof(float));
    if (!env || !re || !im || !win || !prev) {
        free(env); free(re); free(im); free(win); free(prev);
        return NULL;
    }
    for (int i = 0; i < FFT_N; ++i) win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / FFT_N);

    for (size_t f = 0; f < frames; ++f) {
        const float *p = x + f * HOP;
        for (int i = 0; i < FFT_N; ++i) { re[i] = p[i] * win[i]; im[i] = 0.0f; }
        fft_inplace(re, im, FFT_N);
        float flux = 0.0f;
        for (int k = 1; k <= FFT_N / 2; ++k) {
            float mag = sqrtf(re[k] * re[k] + im[k] * im[k]) * (4.0f / FFT_N);
            float l = logf(1.0f + 1000.0f * mag);       /* logarithmic: quiet hits count too */
            float d = l - prev[k];
            if (d > 0.0f && f > 0) flux += d;
            prev[k] = l;
        }
        env[f] = flux;
    }
    free(re); free(im); free(win); free(prev);

    /* remove the slow changes (overall level): subtract from every value the average around it (~0.4 s) */
    float *hp = malloc(frames * sizeof(float));
    if (!hp) { free(env); return NULL; }
    int w = 36;
    double run = 0.0;
    size_t lo = 0, hi = 0;
    for (size_t t = 0; t < frames; ++t) {
        size_t want_lo = t > (size_t)w ? t - (size_t)w : 0;
        size_t want_hi = t + (size_t)w + 1 < frames ? t + (size_t)w + 1 : frames;
        while (hi < want_hi) run += env[hi++];
        while (lo < want_lo) run -= env[lo++];
        float mean = (float)(run / (double)(hi - lo));
        float v = env[t] - mean;
        hp[t] = v > 0.0f ? v : 0.0f;
    }
    free(env);
    *out_n = frames;
    return hp;
}

/* array value at a fractional position (linear) */
static inline float lerp_at(const float *a, size_t n, double pos)
{
    if (pos < 0.0) return 0.0f;
    size_t i = (size_t)pos;
    if (i + 1 >= n) return 0.0f;
    float f = (float)(pos - (double)i);
    return a[i] * (1.0f - f) + a[i + 1] * f;
}

/* How well a grid with period P (in envelope values) "fits" the hits:
 * fold the envelope by phase into M bins and look at the fullest one (with its two neighbours). */
static double phase_score(const float *e, size_t n, double P)
{
    enum { M = 64 };
    double hist[M] = { 0 };
    double tot = 0.0;
    double inv = 1.0 / P;
    for (size_t t = 0; t < n; ++t) {
        if (e[t] <= 0.0f) continue;
        double ph = (double)t * inv;
        ph -= floor(ph);
        int b = (int)(ph * M);
        if (b >= M) b = M - 1;
        hist[b] += e[t];
        tot += e[t];
    }
    if (tot <= 0.0) return 0.0;
    double best = 0.0;
    for (int b = 0; b < M; ++b) {
        double v = hist[(b + M - 1) % M] * 0.5 + hist[b] + hist[(b + 1) % M] * 0.5;
        if (v > best) best = v;
    }
    return best / tot;
}

/* x: mono, downsampled, at rate sr. */
static float detect_core(const float *x, size_t n, double sr)
{
    size_t ne = 0;
    float *e = onset_envelope(x, n, &ne);
    if (!e) return 0.0f;
    double fr = sr / HOP;                                   /* envelope values per second */

    /* nothing happens (silence / steady tone) -> no tempo */
    double sum = 0.0;
    for (size_t t = 0; t < ne; ++t) sum += e[t];
    if (sum <= 1e-6 || ne < (size_t)(fr * 4)) { free(e); return 0.0f; }

    /* 1) autocorrelation up to 4 beats at the slowest tempo */
    size_t maxlag = (size_t)ceil(4.0 * 60.0 * fr / BPM_DETECT_LO) + 3;
    if (maxlag >= ne / 2) maxlag = ne / 2;
    float *acf = calloc(maxlag + 1, sizeof(float));
    if (!acf) { free(e); return 0.0f; }
    for (size_t l = 1; l <= maxlag; ++l) {
        double a = 0.0;
        for (size_t t = 0; t + l < ne; ++t) a += (double)e[t] * e[t + l];
        acf[l] = (float)(a / (double)(ne - l));
    }

    /* 2) comb: tempo b gets points from hits at 1, 2, 3 and 4 periods; we slightly prefer ~120 BPM
     *    (otherwise e.g. 90 and 180 would be almost equal) */
    double best_b = 0.0, best_s = -1.0;
    for (double b = BPM_DETECT_LO; b <= BPM_DETECT_HI + 1e-9; b += 0.1) {
        double L = 60.0 * fr / b;
        double s = 0.0;
        static const double wt[5] = { 0, 1.0, 1.0, 0.8, 0.8 };
        for (int h = 1; h <= 4; ++h) s += wt[h] * lerp_at(acf, maxlag + 1, L * h);
        double oct = log2(b / 120.0);
        s *= exp(-0.5 * (oct / 0.9) * (oct / 0.9));
        if (s > best_s) { best_s = s; best_b = b; }
    }
    free(acf);
    if (best_b <= 0.0) { free(e); return 0.0f; }

    /* 3) refinement: the best alignment of the beats over the whole excerpt, in 0.01 BPM steps */
    double fine_b = best_b, fine_s = -1.0;
    for (double b = best_b - 1.5; b <= best_b + 1.5 + 1e-9; b += 0.01) {
        if (b < BPM_DETECT_LO - 2 || b > BPM_DETECT_HI + 2) continue;
        double sc = phase_score(e, ne, 60.0 * fr / b);
        if (sc > fine_s) { fine_s = sc; fine_b = b; }
    }
    free(e);

    /* most electronic music is at a whole tempo: if we are very close — round it */
    double r = floor(fine_b + 0.5);
    if (fabs(fine_b - r) < 0.05) fine_b = r;
    return (float)fine_b;
}

static int decim_factor(int sr)
{
    int d = (int)floor((double)sr / ANA_SR_TARGET + 0.5);
    return d < 1 ? 1 : d;
}

float bpm_detect_mono(const float *mono, size_t n, int sample_rate)
{
    if (!mono || sample_rate <= 0) return 0.0f;
    int d = decim_factor(sample_rate);
    size_t lim = (size_t)(BPM_MAX_SECONDS * sample_rate);
    if (n > lim) n = lim;
    size_t m = n / (size_t)d;
    float *x = malloc((m ? m : 1) * sizeof(float));
    if (!x) return 0.0f;
    for (size_t i = 0; i < m; ++i) {
        float a = 0.0f;
        for (int k = 0; k < d; ++k) a += mono[i * (size_t)d + (size_t)k];
        x[i] = a / (float)d;
    }
    float r = detect_core(x, m, (double)sample_rate / d);
    free(x);
    return r;
}

float bpm_detect_file(const char *path, const atomic_int *cancel)
{
    StreamSource src;
    int sr = 0;
    if (!path || stream_source_sndfile_native(path, &src, &sr) != 0 || sr <= 0) return 0.0f;

    size_t total = src.frames(src.ctx);
    size_t lim = (size_t)(BPM_MAX_SECONDS * sr);
    if (total > lim) total = lim;
    int d = decim_factor(sr);
    size_t m = total / (size_t)d;
    float *x = malloc((m ? m : 1) * sizeof(float));
    enum { CHUNK = 16384 };
    float *buf = malloc((size_t)CHUNK * 2 * sizeof(float));
    if (!x || !buf) { free(x); free(buf); src.close(src.ctx); return 0.0f; }

    size_t pos = 0, oi = 0;
    float acc = 0.0f;
    int acc_n = 0;
    while (pos < total) {
        if (cancel && atomic_load(cancel)) { free(x); free(buf); src.close(src.ctx); return 0.0f; }
        size_t want = total - pos < CHUNK ? total - pos : CHUNK;
        size_t got = src.read(src.ctx, pos, buf, want);
        if (got == 0) break;
        for (size_t i = 0; i < got && oi < m; ++i) {
            acc += (buf[i * 2] + buf[i * 2 + 1]) * 0.5f;
            if (++acc_n == d) { x[oi++] = acc / (float)d; acc = 0.0f; acc_n = 0; }
        }
        pos += got;
    }
    src.close(src.ctx);
    free(buf);

    float r = (cancel && atomic_load(cancel)) ? 0.0f : detect_core(x, oi, (double)sr / d);
    free(x);
    return r;
}

float bpm_round(float bpm)
{
    if (bpm <= 0.0f) return 0.0f;
    double r = floor((double)bpm + 0.5);
    if (fabs((double)bpm - r) < 0.03) return (float)r;
    return (float)(floor((double)bpm * 100.0 + 0.5) / 100.0);
}

/* ===================== background thread ===================== */

struct BpmJob {
    char         *path;
    atomic_int    cancel;
    atomic_int    done;
    _Atomic float result;
    atomic_int    refs;
    struct BpmJob *next;
};

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static BpmJob         *g_head, *g_tail;
static pthread_t       g_thr;
static bool            g_started, g_quit;

static void job_unref(BpmJob *j)
{
    if (atomic_fetch_sub(&j->refs, 1) == 1) {
        free(j->path);
        free(j);
    }
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (!g_head && !g_quit) pthread_cond_wait(&g_cv, &g_mu);
        if (g_quit) { pthread_mutex_unlock(&g_mu); break; }
        BpmJob *j = g_head;
        g_head = j->next;
        if (!g_head) g_tail = NULL;
        pthread_mutex_unlock(&g_mu);

        float r = 0.0f;
        if (!atomic_load(&j->cancel)) {
            r = bpm_detect_file(j->path, &j->cancel);
            if (!atomic_load(&j->cancel))
                printf("bpm: '%s' -> %.2f BPM\n", j->path, r);
        }
        atomic_store(&j->result, r);
        atomic_store(&j->done, 1);
        job_unref(j);
    }
    return NULL;
}

BpmJob *bpm_job_start(const char *path)
{
    if (!path || !path[0]) return NULL;
    BpmJob *j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    j->path = strdup(path);
    if (!j->path) { free(j); return NULL; }
    atomic_store(&j->refs, 2);                     /* the clip + the thread */

    pthread_mutex_lock(&g_mu);
    if (!g_started) {
        if (pthread_create(&g_thr, NULL, worker, NULL) != 0) {
            pthread_mutex_unlock(&g_mu);
            free(j->path); free(j);
            return NULL;
        }
        g_started = true;
    }
    if (g_tail) g_tail->next = j; else g_head = j;
    g_tail = j;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return j;
}

bool bpm_job_poll(BpmJob *j, float *out)
{
    if (!j || !atomic_load(&j->done)) return false;
    if (out) *out = atomic_load(&j->result);
    return true;
}

void bpm_job_release(BpmJob *j)
{
    if (!j) return;
    atomic_store(&j->cancel, 1);
    job_unref(j);
}

void bpm_shutdown(void)
{
    pthread_mutex_lock(&g_mu);
    if (!g_started) { pthread_mutex_unlock(&g_mu); return; }
    g_quit = true;
    /* waiting jobs are cancelled */
    BpmJob *j = g_head;
    g_head = g_tail = NULL;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    while (j) { BpmJob *n = j->next; atomic_store(&j->done, 1); job_unref(j); j = n; }
    pthread_join(g_thr, NULL);
    g_started = false;
    g_quit = false;
}
