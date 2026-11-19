/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.21: extra sound cards + a delay for every output (see outx.h) */
#define _POSIX_C_SOURCE 200809L
#include "outx.h"
#include "mixer.h"
#include "settings.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ===================== the configuration ===================== */

static OutxDevCfg g_cfg[OUTX_MAX];
static int        g_ncfg;
static float      g_delay_ms[MAX_HW_OUT];
static char       g_path_override[1024];

static void cfg_path(char *out, size_t n)
{
    if (g_path_override[0]) { snprintf(out, n, "%s", g_path_override); return; }
    char p[1024];
    settings_get_config_path(p, sizeof p);              /* .../qlim/audio.conf -> .../qlim/outputs.conf */
    char *sl = strrchr(p, '/');
    if (!sl) sl = strrchr(p, '\\');
    if (sl) sl[1] = 0; else p[0] = 0;
    snprintf(out, n, "%soutputs.conf", p);
}

void outx_load(void)
{
    char path[1100];
    cfg_path(path, sizeof path);
    g_ncfg = 0;
    for (int k = 0; k < MAX_HW_OUT; ++k) g_delay_ms[k] = 0.0f;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[800];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "dev=", 4) && g_ncfg < OUTX_MAX) {
            /* dev=kind<TAB>channels<TAB>api<TAB>name<TAB>desc */
            OutxDevCfg c;
            memset(&c, 0, sizeof c);
            char *fld[5] = { 0 };
            int nf = 0;
            char *p = line + 4;
            fld[nf++] = p;
            while (*p && nf < 5) { if (*p == '\t') { *p = 0; fld[nf++] = p + 1; } ++p; }
            if (nf < 5) continue;
            c.kind = atoi(fld[0]) == OUTX_KIND_PW ? OUTX_KIND_PW : OUTX_KIND_PA;
            c.channels = atoi(fld[1]);
            if (c.channels < 1) c.channels = 2;
            if (c.channels > MAX_HW_OUT) c.channels = MAX_HW_OUT;
            snprintf(c.api, sizeof c.api, "%s", fld[2]);
            snprintf(c.name, sizeof c.name, "%s", fld[3]);
            snprintf(c.desc, sizeof c.desc, "%s", fld[4]);
            if (c.name[0]) g_cfg[g_ncfg++] = c;
        } else if (!strncmp(line, "delay=", 6)) {
            int k = -1;
            float ms = 0;
            if (sscanf(line + 6, "%d:%f", &k, &ms) == 2 && k >= 1 && k <= MAX_HW_OUT && ms == ms)
                g_delay_ms[k - 1] = ms < 0 ? 0 : ms > OUTX_DELAY_MAX_MS ? OUTX_DELAY_MAX_MS : ms;
        }
    }
    fclose(f);
    printf("outx: %d extra output device(s), delays %s ('%s')\n", g_ncfg, outx_any_delay() ? "set" : "none", path);
}

void outx_save(void)
{
    char path[1100], tmp[1200];
    cfg_path(path, sizeof path);
    {   /* the folder may not exist yet */
        char dir[1100];
        snprintf(dir, sizeof dir, "%s", path);
        char *sl = strrchr(dir, '/');
        if (sl) { *sl = 0; settings_ensure_dir(dir); }
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { fprintf(stderr, "outx: cannot write '%s'\n", tmp); return; }
    fprintf(f, "# Q-LiM: extra sound cards (their outputs come after the main card's) and the delay of every output\n");
    for (int i = 0; i < g_ncfg; ++i)
        fprintf(f, "dev=%d\t%d\t%s\t%s\t%s\n", g_cfg[i].kind, g_cfg[i].channels, g_cfg[i].api, g_cfg[i].name, g_cfg[i].desc);
    for (int k = 0; k < MAX_HW_OUT; ++k)
        if (g_delay_ms[k] > 0.0f) fprintf(f, "delay=%d:%.2f\n", k + 1, (double)g_delay_ms[k]);
    fclose(f);
    if (rename(tmp, path) != 0) { remove(path); rename(tmp, path); }
}

int outx_cfg_count(void) { return g_ncfg; }
const OutxDevCfg *outx_cfg_get(int i) { return i >= 0 && i < g_ncfg ? &g_cfg[i] : NULL; }
static void total_update(void);
int outx_cfg_add(const OutxDevCfg *c)
{
    if (!c || g_ncfg >= OUTX_MAX) return -1;
    g_cfg[g_ncfg] = *c;
    if (g_cfg[g_ncfg].channels < 1) g_cfg[g_ncfg].channels = 2;
    if (g_cfg[g_ncfg].channels > MAX_HW_OUT) g_cfg[g_ncfg].channels = MAX_HW_OUT;
    int i = g_ncfg++;
    total_update();
    return i;
}
/* (the extra cards must be detached first: the ones after it get new numbers) */
void outx_cfg_remove(int i)
{
    if (i < 0 || i >= g_ncfg) return;
    memmove(&g_cfg[i], &g_cfg[i + 1], (size_t)(g_ncfg - i - 1) * sizeof g_cfg[0]);
    g_ncfg--;
    total_update();
}
int outx_first(int i, int main_ch)
{
    if (main_ch < 2) main_ch = 2;
    int f = main_ch;
    for (int j = 0; j < i && j < g_ncfg; ++j) f += g_cfg[j].channels;
    return f < MAX_HW_OUT ? f : -1;
}
int outx_total(int main_ch)
{
    if (main_ch < 2) main_ch = 2;
    int t = main_ch;
    for (int j = 0; j < g_ncfg; ++j) t += g_cfg[j].channels;
    return t < MAX_HW_OUT ? t : MAX_HW_OUT;
}
float outx_delay_get(int k) { return k >= 0 && k < MAX_HW_OUT ? g_delay_ms[k] : 0.0f; }
bool outx_any_delay(void)
{
    for (int k = 0; k < MAX_HW_OUT; ++k) if (g_delay_ms[k] > 0.0f) return true;
    return false;
}

/* ===================== the engine ===================== */

#define SCR_FRAMES 8192u                 /* the longest main callback handled (longer: the extras get silence) */
#define XF_FRAMES  480u                  /* a delay change glides over this many frames */
#define RS_W       16                    /* resampler: half width (input frames) */
#define RS_TAPS    (2 * RS_W)
#define RS_PH      256
#define RS_BETA    8.0

static OutxRenderFn g_test_fn;
static void        *g_test_user;
void outx_test_set_render(OutxRenderFn fn, void *user) { g_test_fn = fn; g_test_user = user; }
static double mono_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
static double (*g_now)(void) = mono_now;
void outx_test_set_clock(double (*fn)(void)) { g_now = fn ? fn : mono_now; }

static atomic_bool g_on;                 /* the wrapper does anything at all (an extra card or a delay) */
static bool        g_enabled;            /* not in the PipeWire graph mode */
static int         g_rate = 48000, g_main_ch = 2;
static atomic_int  g_total = 2;
static float      *g_scr;                /* SCR_FRAMES x MAX_HW_OUT, interleaved */

/* delays */
static _Atomic(float *) g_line[MAX_HW_OUT];
static unsigned    g_line_len, g_line_mask; /* frames (power of 2) */
static unsigned    g_line_w;              /* RT */
static atomic_int  g_d_target[MAX_HW_OUT];/* frames */
static int         g_d_cur[MAX_HW_OUT], g_d_from[MAX_HW_OUT];
static unsigned    g_xf[MAX_HW_OUT];      /* RT: frames left of the glide */

typedef struct {
    atomic_int      on;                   /* 1 = the main callback feeds it */
    int             first, nch;           /* its outputs in the mixer (0-based) and how many */
    int             dev_rate, dev_ch;
    float          *ring;                 /* cap x nch, interleaved */
    unsigned        cap, mask;
    _Atomic uint64_t w, rel;              /* written / released (frames) */
    _Atomic double  w_time;               /* when w was last advanced (the producer's clock, seconds) */
    atomic_bool     reset;                /* the producer had to drop (the card stopped taking) */
    atomic_uint     bm_max;               /* the largest main callback (frames) */
    float          *kern;                 /* (RS_PH + 1) x RS_TAPS */
    double          step_nom;             /* input frames per output frame */
    /* the consumer (the card's thread) */
    bool            run;
    uint64_t        rp;
    double          frac, step, avg, fest, age, life, margin;
    unsigned        bd_max;
    atomic_uint     underruns, overruns;
    _Atomic uint64_t pulls;
    _Atomic double  lat_ms, ppm;
} Slot;
static Slot g_slot[OUTX_MAX];

static void total_update(void) { atomic_store(&g_total, g_enabled ? outx_total(g_main_ch) : g_main_ch); }

static unsigned pow2_at_least(unsigned v) { unsigned p = 1; while (p < v) p <<= 1; return p; }

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0, q = x * x * 0.25;
    for (int k = 1; k < 50; ++k) { term *= q / ((double)k * (double)k); sum += term; if (term < sum * 1e-15) break; }
    return sum;
}

/* the kernel: phase p (frac = p / RS_PH), tap j -> the input frame (rp - RS_W + 1 + j) for an output at rp + frac */
static float *kernel_make(double fc)
{
    float *k = malloc(sizeof(float) * (size_t)(RS_PH + 1) * RS_TAPS);
    if (!k) return NULL;
    double i0b = bessel_i0(RS_BETA);
    for (int p = 0; p <= RS_PH; ++p) {
        double fr = (double)p / RS_PH, sum = 0, row[RS_TAPS];
        for (int j = 0; j < RS_TAPS; ++j) {
            double t = fr + RS_W - 1 - j;                       /* the distance in input frames */
            double x = t / (double)RS_W;
            double win = fabs(x) >= 1.0 ? 0.0 : bessel_i0(RS_BETA * sqrt(1.0 - x * x)) / i0b;
            double a = M_PI * fc * t;
            double si = fabs(a) < 1e-9 ? 1.0 : sin(a) / a;
            row[j] = fc * si * win;
            sum += row[j];
        }
        for (int j = 0; j < RS_TAPS; ++j) k[p * RS_TAPS + j] = (float)(row[j] / sum);   /* DC gain exactly 1 */
    }
    return k;
}

static void update_on(void)
{
    bool any = false;
    for (int i = 0; i < OUTX_MAX; ++i) if (atomic_load(&g_slot[i].on)) any = true;
    for (int k = 0; k < MAX_HW_OUT; ++k) if (atomic_load(&g_line[k])) any = true;
    atomic_store(&g_on, g_enabled && g_scr && any);
}

static void line_ensure(int k)
{
    if (atomic_load(&g_line[k]) || !g_line_len) return;
    float *l = calloc(g_line_len, sizeof(float));
    if (l) atomic_store(&g_line[k], l);
}

void outx_delay_set(int k, float ms)
{
    if (k < 0 || k >= MAX_HW_OUT) return;
    if (!(ms == ms) || ms < 0) ms = 0;
    if (ms > OUTX_DELAY_MAX_MS) ms = OUTX_DELAY_MAX_MS;
    g_delay_ms[k] = ms;
    if (!g_enabled || !g_scr) return;
    if (ms > 0) line_ensure(k);              /* (once there, the line stays until the audio stops - a glide back to 0) */
    atomic_store(&g_d_target[k], (int)lround((double)ms * g_rate / 1000.0));
    update_on();
}

void outx_engine_start(int main_rate, int main_ch, bool enabled)
{
    outx_engine_stop();
    g_rate = main_rate > 0 ? main_rate : 48000;
    g_main_ch = main_ch < 2 ? 2 : main_ch;
    g_enabled = enabled;
    atomic_store(&g_total, enabled ? outx_total(g_main_ch) : g_main_ch);
    if (!enabled) return;
    g_scr = calloc((size_t)SCR_FRAMES * MAX_HW_OUT, sizeof(float));
    g_line_len = pow2_at_least((unsigned)(OUTX_DELAY_MAX_MS / 1000.0f * (float)g_rate) + SCR_FRAMES + XF_FRAMES + 16);
    g_line_mask = g_line_len - 1;
    g_line_w = 0;
    for (int k = 0; k < MAX_HW_OUT; ++k) {
        g_d_cur[k] = g_d_from[k] = 0; g_xf[k] = 0;
        int d = (int)lround((double)g_delay_ms[k] * g_rate / 1000.0);
        if (d > 0) { line_ensure(k); g_d_cur[k] = d; }     /* starts already delayed (no glide from 0) */
        atomic_store(&g_d_target[k], d);
    }
    update_on();
}

void outx_engine_stop(void)
{
    atomic_store(&g_on, false);
    for (int i = 0; i < OUTX_MAX; ++i) { outx_slot_detach(i); outx_slot_free(i); }
    for (int k = 0; k < MAX_HW_OUT; ++k) { free(atomic_exchange(&g_line[k], NULL)); }
    free(g_scr);
    g_scr = NULL;
    g_line_len = 0;
}

void outx_slot_attach(int i, int dev_rate, int dev_ch)
{
    if (i < 0 || i >= g_ncfg || !g_enabled || !g_scr) return;
    Slot *S = &g_slot[i];
    outx_slot_detach(i);
    outx_slot_free(i);
    int first = outx_first(i, g_main_ch);
    if (first < 0) return;
    int nch = g_cfg[i].channels;
    if (first + nch > MAX_HW_OUT) nch = MAX_HW_OUT - first;
    if (nch < 1) return;
    if (dev_rate <= 0) dev_rate = g_rate;
    unsigned cap = pow2_at_least((unsigned)g_rate + 4 * SCR_FRAMES);
    float *ring = calloc((size_t)cap * (size_t)nch, sizeof(float));
    double fc = 0.92 * (dev_rate < g_rate ? (double)dev_rate / g_rate : 1.0);
    float *kern = kernel_make(fc);
    if (!ring || !kern) { free(ring); free(kern); return; }
    S->first = first; S->nch = nch; S->dev_rate = dev_rate; S->dev_ch = dev_ch;
    S->ring = ring; S->cap = cap; S->mask = cap - 1; S->kern = kern;
    atomic_store(&S->w, 0); atomic_store(&S->rel, 0); atomic_store(&S->reset, false);
    atomic_store(&S->bm_max, 0);
    S->step_nom = (double)g_rate / (double)dev_rate;
    S->run = false; S->rp = 0; S->frac = 0; S->step = S->step_nom; S->avg = 0; S->fest = 0; S->age = 0; S->life = 0;
    S->margin = 0.002 * g_rate; S->bd_max = 0;
    atomic_store(&S->underruns, 0); atomic_store(&S->overruns, 0); atomic_store(&S->pulls, 0);
    atomic_store(&S->lat_ms, 0.0); atomic_store(&S->ppm, 0.0);
    atomic_store(&S->on, 1);
    update_on();
}

void outx_slot_detach(int i)
{
    if (i < 0 || i >= OUTX_MAX) return;
    atomic_store(&g_slot[i].on, 0);
    update_on();
}

/* after the card's stream is closed and one main callback has passed */
void outx_slot_free(int i)
{
    if (i < 0 || i >= OUTX_MAX) return;
    Slot *S = &g_slot[i];
    if (atomic_load(&S->on)) return;
    free(S->ring); S->ring = NULL;
    free(S->kern); S->kern = NULL;
}

void outx_slot_stat(int i, OutxStat *st)
{
    memset(st, 0, sizeof *st);
    if (i < 0 || i >= OUTX_MAX) return;
    Slot *S = &g_slot[i];
    st->attached = atomic_load(&S->on) != 0;
    st->latency_ms = atomic_load(&S->lat_ms);
    st->running = st->attached && st->latency_ms > 0.0;
    st->ppm = atomic_load(&S->ppm);
    st->underruns = atomic_load(&S->underruns);
    st->overruns = atomic_load(&S->overruns);
    st->pulls = atomic_load(&S->pulls);
}

/* ---- RT: the main card's callback ---- */
static void render_mix(Session *s, float *out, unsigned frames, int nch)
{
    if (g_test_fn) { g_test_fn(out, frames, nch, g_test_user); return; }
    mixer_render(s, out, frames, nch);
}

static void delays_apply(float *buf, unsigned n, int T)
{
    for (int k = 0; k < T; ++k) {
        float *l = atomic_load_explicit(&g_line[k], memory_order_acquire);
        if (!l) continue;
        int tgt = atomic_load_explicit(&g_d_target[k], memory_order_relaxed);
        if (tgt > (int)(g_line_len - SCR_FRAMES - XF_FRAMES - 8)) tgt = (int)(g_line_len - SCR_FRAMES - XF_FRAMES - 8);
        unsigned w = g_line_w;
        for (unsigned i = 0; i < n; ++i, ++w) {
            if (g_xf[k] == 0 && tgt != g_d_cur[k]) { g_d_from[k] = g_d_cur[k]; g_d_cur[k] = tgt; g_xf[k] = XF_FRAMES; }
            float x = buf[(size_t)i * T + k];
            l[w & g_line_mask] = x;
            float y = l[(w - (unsigned)g_d_cur[k]) & g_line_mask];
            if (g_xf[k]) {
                float g = (float)g_xf[k] / (float)XF_FRAMES;     /* 1 -> 0: the old delay fades out */
                float yo = l[(w - (unsigned)g_d_from[k]) & g_line_mask];
                y = yo * g + y * (1.0f - g);
                g_xf[k]--;
            }
            buf[(size_t)i * T + k] = y;
        }
    }
    g_line_w += n;
}

void outx_render(Session *s, float *out, unsigned frames, int main_ch)
{
    if (!atomic_load_explicit(&g_on, memory_order_acquire) || frames > SCR_FRAMES || main_ch != g_main_ch) {
        render_mix(s, out, frames, main_ch);
        return;
    }
    int T = atomic_load_explicit(&g_total, memory_order_relaxed);
    if (T < main_ch) T = main_ch;
    render_mix(s, g_scr, frames, T);
    delays_apply(g_scr, frames, T);
    for (unsigned i = 0; i < frames; ++i)
        memcpy(out + (size_t)i * main_ch, g_scr + (size_t)i * T, sizeof(float) * (size_t)main_ch);
    double now = g_now();
    for (int si = 0; si < OUTX_MAX; ++si) {
        Slot *S = &g_slot[si];
        if (!atomic_load_explicit(&S->on, memory_order_acquire)) continue;
        if (S->first + S->nch > T) continue;
        if (frames > atomic_load_explicit(&S->bm_max, memory_order_relaxed)) atomic_store_explicit(&S->bm_max, frames, memory_order_relaxed);
        uint64_t w = atomic_load_explicit(&S->w, memory_order_relaxed);
        uint64_t rel = atomic_load_explicit(&S->rel, memory_order_acquire);
        if (w - rel + frames > S->cap) {                  /* the card does not take it (stopped / far behind) */
            atomic_store_explicit(&S->reset, true, memory_order_release);
            atomic_fetch_add_explicit(&S->overruns, 1, memory_order_relaxed);
            continue;
        }
        for (unsigned i = 0; i < frames; ++i) {
            float *d = S->ring + (size_t)((w + i) & S->mask) * (size_t)S->nch;
            const float *src = g_scr + (size_t)i * T + S->first;
            for (int c = 0; c < S->nch; ++c) d[c] = src[c];
        }
        atomic_store_explicit(&S->w_time, now, memory_order_relaxed);
        atomic_store_explicit(&S->w, w + frames, memory_order_release);
    }
}

/* ---- RT: an extra card's callback ---- */
void outx_pull(int si, float *out, unsigned n, int dev_ch)
{
    memset(out, 0, sizeof(float) * (size_t)n * (size_t)(dev_ch > 0 ? dev_ch : 1));
    if (si < 0 || si >= OUTX_MAX) return;
    Slot *S = &g_slot[si];
    if (!atomic_load_explicit(&S->on, memory_order_acquire) || !S->ring) return;
    atomic_fetch_add_explicit(&S->pulls, 1, memory_order_relaxed);
    double R = (double)g_rate;
    S->life += (double)n / (double)S->dev_rate;
    uint64_t w = atomic_load_explicit(&S->w, memory_order_acquire);
    if (atomic_exchange_explicit(&S->reset, false, memory_order_acq_rel)) S->run = false;
    unsigned bd = (unsigned)ceil(n * S->step_nom * 1.01);
    if (bd > S->bd_max) S->bd_max = bd;
    unsigned bm = atomic_load_explicit(&S->bm_max, memory_order_relaxed);
    /* the target fill: both blocks + the resampler's half width + a margin (it grows after an underrun) */
    double target = RS_W + 1.0 + 1.25 * ((double)bm + (double)S->bd_max) + S->margin;
    if (!S->run) {
        if ((double)w < target + RS_W + bm) {
            atomic_store_explicit(&S->lat_ms, 0.0, memory_order_relaxed);
            uint64_t keep = (uint64_t)(target + RS_W + bm);      /* the producer must never fill up while we wait */
            if (w > keep) atomic_store_explicit(&S->rel, w - keep, memory_order_release);
            return;
        }
        S->rp = w - (uint64_t)target;
        S->frac = 0.0;
        S->avg = target;
        S->step = S->step_nom * (1.0 + S->fest);   /* (keeps the clock difference found before) */
        S->run = true;
        atomic_store_explicit(&S->rel, S->rp - RS_W, memory_order_release);
    }
    /* the fill now: what was written + what the main card has produced since then (its blocks make w jump; without
     * this the controller would chase a sawtooth) - measured before reading */
    double since = (g_now() - atomic_load_explicit(&S->w_time, memory_order_relaxed)) * R;
    if (since < 0) since = 0;
    if (since > bm) since = bm;
    double fill = (double)w + since - (double)S->rp - S->frac;
    int oc = dev_ch < S->nch ? dev_ch : S->nch;
    const unsigned mask = S->mask;
    const int nch = S->nch;
    float coef[RS_TAPS];
    unsigned i = 0;
    for (; i < n; ++i) {
        if (S->rp + RS_W + 1 >= w) {                          /* not enough: silence until it is filled again */
            S->run = false;
            if (S->life > 3.0) atomic_fetch_add_explicit(&S->underruns, 1, memory_order_relaxed);   /* (the start is not counted) */
            if (S->margin < 0.05 * R) S->margin += 0.004 * R;
            break;
        }
        double ph = S->frac * RS_PH;
        int p0 = (int)ph;
        if (p0 >= RS_PH) p0 = RS_PH - 1;
        float t = (float)(ph - p0);
        const float *k0 = S->kern + p0 * RS_TAPS, *k1 = k0 + RS_TAPS;
        for (int j = 0; j < RS_TAPS; ++j) coef[j] = k0[j] + (k1[j] - k0[j]) * t;
        uint64_t base = S->rp - RS_W + 1;
        for (int c = 0; c < oc; ++c) {
            float acc = 0.0f;
            for (int j = 0; j < RS_TAPS; ++j) acc += S->ring[(size_t)((base + (uint64_t)j) & mask) * (size_t)nch + (size_t)c] * coef[j];
            out[(size_t)i * dev_ch + c] = acc;
        }
        S->frac += S->step;
        double adv = floor(S->frac);
        S->rp += (uint64_t)adv;
        S->frac -= adv;
    }
    if (S->rp >= RS_W) atomic_store_explicit(&S->rel, S->rp - RS_W, memory_order_release);
    if (!S->run) return;
    /* the controller: keeps the fill at the target. PI on a smoothed fill; `fest` is the learned clock difference.
     * Two gears (as a PLL): the first 15 s fast (tau 1 s, poles on the real axis, finds the clock in ~10 s), then calm
     * (tau 2 s, a double pole at ~33 s): the ratio then barely moves when the callbacks come irregularly - clean sound. */
    double dt = (double)n / (double)S->dev_rate;
    bool was_fast = S->age < 15.0;
    S->age += dt;
    bool fast = S->age < 15.0;
    if (was_fast && !fast)                                    /* bumpless: the slow gear starts from the same ratio */
        S->fest += (0.2 - 0.05) * (S->avg - target) / R;
    double a = dt / (fast ? 1.0 : 2.0); if (a > 1.0) a = 1.0;
    S->avg += (fill - S->avg) * a;
    double err = (S->avg - target) / R;                       /* seconds */
    S->fest += (fast ? 0.01 : 0.0008) * err * dt;
    if (S->fest > 0.005) S->fest = 0.005;
    if (S->fest < -0.005) S->fest = -0.005;
    double adj = (fast ? 0.2 : 0.05) * err + S->fest;
    if (adj > 0.006) adj = 0.006;
    if (adj < -0.006) adj = -0.006;
    S->step = S->step_nom * (1.0 + adj);
    atomic_store_explicit(&S->lat_ms, S->avg / R * 1000.0, memory_order_relaxed);
    atomic_store_explicit(&S->ppm, -S->fest * 1e6, memory_order_relaxed);   /* the learned part; > 0: the card's clock is fast */
}
