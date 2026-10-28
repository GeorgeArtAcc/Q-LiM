/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* The common wrapper around plugins - see plughost.h */
#define _GNU_SOURCE
#include "plughost.h"
#include <math.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct QPlug {
    QInst in[2];
    int   ninst;                 /* 2 = mono plugin run twice (left and right) */
    int   maxb;
    int   nbi, nbo;              /* number of input/output buffers of one instance */
    float *mem;
    float *bi[16], *bo[16];
    PlugWin *win;                /* open window (or NULL) */
};

/* all loaded ones (for qplug_idle_all) - main thread only */
#define MAX_QP 512
static QPlug *g_all[MAX_QP];
static int    g_nall;
static pthread_mutex_t g_all_mu = PTHREAD_MUTEX_INITIALIZER;   /* step 35d: loading on a background thread too */

static int open_one(const char *fmt, const char *id, double rate, int maxb, QInst *q, char *err, int esz)
{
    memset(q, 0, sizeof *q);
    if (!strcmp(fmt, "VST3")) return vst3_open(id, rate, maxb, q, err, esz);
    if (!strcmp(fmt, "VST") || !strcmp(fmt, "VST2")) return vst2_open(id, rate, maxb, q, err, esz);
    if (!strcmp(fmt, "LV2")) return lv2_open(id, rate, maxb, q, err, esz);
    if (!strcmp(fmt, "CLAP")) return clap_open(id, rate, maxb, q, err, esz);
    snprintf(err, (size_t)esz, "unknown plugin format %s", fmt);
    return -1;
}

QPlug *qplug_load(const char *fmt, const char *id, double rate, int max_block, char *err, int esz)
{
    if (max_block < 16) max_block = 16;
    QPlug *p = calloc(1, sizeof *p);
    if (!p) { snprintf(err, (size_t)esz, "out of memory"); return NULL; }
    p->maxb = max_block;
    if (open_one(fmt, id, rate, max_block, &p->in[0], err, esz) != 0) { free(p); return NULL; }
    p->ninst = 1;
    if (p->in[0].nout <= 0) {
        snprintf(err, (size_t)esz, "plugin has no audio output");
        qplug_free(p); return NULL;
    }
    if (p->in[0].nin > 16 || p->in[0].nout > 16) {
        snprintf(err, (size_t)esz, "too many channels (%d in / %d out)", p->in[0].nin, p->in[0].nout);
        qplug_free(p); return NULL;
    }
    if (p->in[0].nin == 1 && p->in[0].nout == 1) {          /* mono: a second instance for the right channel */
        if (open_one(fmt, id, rate, max_block, &p->in[1], err, esz) != 0) { qplug_free(p); return NULL; }
        p->ninst = 2;
    }
    p->nbi = p->in[0].nin < 2 ? 2 : p->in[0].nin;
    p->nbo = p->in[0].nout < 2 ? 2 : p->in[0].nout;
    p->mem = calloc((size_t)(p->nbi + p->nbo) * (size_t)max_block, sizeof(float));
    if (!p->mem) { snprintf(err, (size_t)esz, "out of memory"); qplug_free(p); return NULL; }
    for (int i = 0; i < p->nbi; ++i) p->bi[i] = p->mem + (size_t)i * (size_t)max_block;
    for (int i = 0; i < p->nbo; ++i) p->bo[i] = p->mem + (size_t)(p->nbi + i) * (size_t)max_block;
    pthread_mutex_lock(&g_all_mu);
    if (g_nall < MAX_QP) g_all[g_nall++] = p;
    pthread_mutex_unlock(&g_all_mu);
    return p;
}

static void clean(float *x, unsigned n)               /* NaN/Inf/huge values -> 0 (protects the speakers) */
{
    for (unsigned i = 0; i < n; ++i)
        if (!(fabsf(x[i]) < 64.0f)) x[i] = 0.0f;
}

static void run_chunk(QPlug *p, float *l, float *r, unsigned n)
{
    QInst *a = &p->in[0];
    if (p->ninst == 2) {                                  /* dual mono */
        float *in1[1] = { p->bi[0] }, *out1[1] = { p->bo[0] };
        float *in2[1] = { p->bi[1] }, *out2[1] = { p->bo[1] };
        memcpy(p->bi[0], l, n * sizeof(float));
        memcpy(p->bi[1], r, n * sizeof(float));
        a->process(a->h, in1, out1, n);
        p->in[1].process(p->in[1].h, in2, out2, n);
        memcpy(l, p->bo[0], n * sizeof(float));
        memcpy(r, p->bo[1], n * sizeof(float));
    } else {
        if (a->nin == 1) {
            for (unsigned i = 0; i < n; ++i) p->bi[0][i] = 0.5f * (l[i] + r[i]);
        } else if (a->nin >= 2) {
            memcpy(p->bi[0], l, n * sizeof(float));
            memcpy(p->bi[1], r, n * sizeof(float));
            for (int c = 2; c < a->nin; ++c) memset(p->bi[c], 0, n * sizeof(float));
        }
        for (int c = 0; c < a->nout; ++c) memset(p->bo[c], 0, n * sizeof(float));
        a->process(a->h, p->bi, p->bo, n);
        const float *ol = p->bo[0], *orr = p->bo[a->nout >= 2 ? 1 : 0];
        if (a->nin == 0) {                                /* 1.2: instrument - its sound is added to what came in */
            for (unsigned i = 0; i < n; ++i) { l[i] += ol[i]; r[i] += orr[i]; }
        } else {
            memcpy(l, ol, n * sizeof(float));
            memcpy(r, orr, n * sizeof(float));
        }
    }
    clean(l, n);
    clean(r, n);
}

void qplug_midi(QPlug *p, const QMidiEv *ev, int n)
{
    if (!p || n <= 0) return;
    for (int i = 0; i < p->ninst; ++i) if (p->in[i].midi) p->in[i].midi(p->in[i].h, ev, n);
}
bool qplug_wants_midi(QPlug *p) { return p && p->in[0].midi != NULL; }
bool qplug_is_instrument(QPlug *p) { return p && p->in[0].nin == 0; }

int qplug_param_count(QPlug *p) { return p && p->in[0].param_count ? p->in[0].param_count(p->in[0].h) : 0; }
bool qplug_param_name(QPlug *p, int i, char *name, int nsz)
{
    if (nsz > 0) name[0] = 0;
    return p && p->in[0].param_name ? p->in[0].param_name(p->in[0].h, i, name, nsz) : false;
}
float qplug_param_get(QPlug *p, int i) { return p && p->in[0].param_get ? p->in[0].param_get(p->in[0].h, i) : 0.0f; }
void qplug_param_set(QPlug *p, int i, float v)
{
    if (!p) return;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    for (int k = 0; k < p->ninst; ++k) if (p->in[k].param_set) p->in[k].param_set(p->in[k].h, i, v);
    qplug_note_edit();
}
void qplug_param_text(QPlug *p, int i, char *out, int sz)
{
    if (sz > 0) out[0] = 0;
    if (p && p->in[0].param_text) p->in[0].param_text(p->in[0].h, i, out, sz);
    if (sz > 0 && !out[0]) { snprintf(out, (size_t)sz, "%.0f%%", 100.0 * qplug_param_get(p, i)); return; }
    /* "75.000000 %" -> "75 %", "0.010000 s" -> "0.01 s": a number with too many digits is shortened */
    char *end;
    while (*out == ' ') memmove(out, out + 1, strlen(out));
    double x = strtod(out, &end);
    if (end != out && (*end == 0 || *end == ' ') && strlen(out) > 1) {
        char unit[64];
        snprintf(unit, sizeof unit, "%s", end);
        double ax = fabs(x);
        int dec = ax >= 100.0 ? 0 : ax >= 10.0 ? 1 : 2;
        char num[48];
        snprintf(num, sizeof num, "%.*f", dec, x);
        if (strchr(num, '.')) {                       /* drop the zeros at the end: 75.0 -> 75, 0.50 -> 0.5 */
            size_t L = strlen(num);
            while (L > 0 && num[L - 1] == '0') num[--L] = 0;
            if (L > 0 && num[L - 1] == '.') num[--L] = 0;
        }
        if (!strcmp(num, "-0")) strcpy(num, "0");
        snprintf(out, (size_t)sz, "%s%s", num, unit);
    }
}

void qplug_process(QPlug *p, float *l, float *r, unsigned n)
{
    while (n > 0) {
        unsigned k = n > (unsigned)p->maxb ? (unsigned)p->maxb : n;
        run_chunk(p, l, r, k);
        l += k; r += k; n -= k;
    }
}

void qplug_free(QPlug *p)
{
    if (!p) return;
    qplug_editor_close(p);
    pthread_mutex_lock(&g_all_mu);
    for (int i = 0; i < g_nall; ++i) if (g_all[i] == p) { g_all[i] = g_all[--g_nall]; break; }
    pthread_mutex_unlock(&g_all_mu);
    for (int i = 0; i < p->ninst; ++i) if (p->in[i].destroy) p->in[i].destroy(p->in[i].h);
    free(p->mem);
    free(p);
}

static atomic_uint g_edits;
void qplug_note_edit(void) { atomic_fetch_add(&g_edits, 1); }
unsigned qplug_edit_count(void) { return atomic_load(&g_edits); }

/* ---- plugin through yabridge (Windows plugin in Wine): loading takes seconds -> on a background thread ---- */
static bool file_has(const char *path, const char *needle)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    static char buf[1 << 16];
    size_t nl = strlen(needle), keep = 0;
    bool found = false;
    for (;;) {
        size_t n = fread(buf + keep, 1, sizeof buf - keep, f);
        if (n == 0) break;
        n += keep;
        for (size_t i = 0; i + nl <= n; ++i) if (buf[i] == needle[0] && !memcmp(buf + i, needle, nl)) { found = true; break; }
        if (found) break;
        keep = nl - 1 < n ? nl - 1 : n;
        memmove(buf, buf + n - keep, keep);
    }
    fclose(f);
    return found;
}

bool qplug_is_bridged(const char *fmt, const char *id)
{
    static struct { char path[1100]; bool yes; } cache[256];
    static int nc;
    char bin[1400] = "";
    if (!strcmp(fmt, "VST3")) { if (vst3_binary(id, bin, sizeof bin) != 0) return false; }
    else if (!strcmp(fmt, "VST") || !strcmp(fmt, "VST2")) snprintf(bin, sizeof bin, "%s", id);
    else if (!strcmp(fmt, "CLAP")) { snprintf(bin, sizeof bin, "%s", id); char *b = strrchr(bin, '|'); if (b) *b = 0; }
    else return false;
    for (int i = 0; i < nc; ++i) if (!strcmp(cache[i].path, bin)) return cache[i].yes;
    bool yes = file_has(bin, "yabridge");
    if (nc < 256) { snprintf(cache[nc].path, sizeof cache[nc].path, "%s", bin); cache[nc++].yes = yes; }
    return yes;
}

/* ---- window ---- */
bool qplug_has_editor(QPlug *p)
{
    return p && p->in[0].has_editor && p->in[0].ed_open && p->in[0].has_editor(p->in[0].h);
}

bool qplug_editor_is_open(QPlug *p) { return p && p->win; }

int qplug_editor_open(QPlug *p, const char *title, char *err, int esz)
{
    if (!p) return -1;
    if (p->win) { plugwin_raise(p->win); return 0; }
    if (!qplug_has_editor(p)) { snprintf(err, (size_t)esz, "this plugin has no window of its own"); return -1; }
    if (!plugwin_available()) { snprintf(err, (size_t)esz, "plugin windows need X11 (no X11 display)"); return -1; }
    PlugWin *w = plugwin_create(title, 640, 400);
    if (!w) { snprintf(err, (size_t)esz, "cannot create a window"); return -1; }
    int W = 0, H = 0;
    if (p->in[0].ed_open(p->in[0].h, w, &W, &H, err, esz) != 0) { plugwin_destroy(w); return -1; }
    if (W <= 0 || H <= 0) plugwin_child_size(w, &W, &H);
    if (W > 0 && H > 0) plugwin_resize(w, W, H);
    plugwin_show(w);
    p->win = w;
    return 0;
}

void qplug_editor_close(QPlug *p)
{
    if (!p || !p->win) return;
    if (p->in[0].ed_close) p->in[0].ed_close(p->in[0].h);
    plugwin_destroy(p->win);
    p->win = NULL;
}

void qplug_idle_all(void)
{
    clap_host_set_main_thread();
    plugwin_poll();
    static QPlug *list[MAX_QP];                      /* copy: a background thread may add while we iterate */
    pthread_mutex_lock(&g_all_mu);
    int n = g_nall;
    memcpy(list, g_all, sizeof(QPlug *) * (size_t)n);
    pthread_mutex_unlock(&g_all_mu);
    for (int i = 0; i < n; ++i) {
        QPlug *p = list[i];
        if (p->win && plugwin_close_requested(p->win)) { qplug_editor_close(p); continue; }
        for (int k = 0; k < p->ninst; ++k) if (p->in[k].main_idle) p->in[k].main_idle(p->in[k].h);
        if (p->win && p->in[0].ed_idle) p->in[0].ed_idle(p->in[0].h);
    }
}

/* ---- state: with dual mono the left one is saved and given to both ---- */
int qplug_get_state(QPlug *p, void **data, size_t *len)
{
    *data = NULL; *len = 0;
    if (!p || !p->in[0].get_state) return -1;
    return p->in[0].get_state(p->in[0].h, data, len);
}

int qplug_set_state(QPlug *p, const void *data, size_t len)
{
    if (!p || !data || !len || !p->in[0].set_state) return -1;
    int r = 0;
    unsigned before = atomic_load(&g_edits);          /* loading is not "touching" */
    for (int k = 0; k < p->ninst; ++k) if (p->in[k].set_state && p->in[k].set_state(p->in[k].h, data, len) != 0) r = -1;
    atomic_store(&g_edits, before);
    return r;
}

bool qplug_selftest(const char *fmt, const char *id, char *err, int esz)
{
    QPlug *p = qplug_load(fmt, id, 48000.0, 512, err, esz);
    if (!p) return false;
    float l[512], r[512];
    for (int blk = 0; blk < 4; ++blk) {
        for (int i = 0; i < 512; ++i) { l[i] = 0.1f * sinf((float)(blk * 512 + i) * 0.05f); r[i] = -l[i]; }
        qplug_process(p, l, r, 512);
    }
    qplug_free(p);
    return true;
}

/* ---- "scan" mode: lines are collected and then every effect is tried ---- */
typedef struct { char kind[8], fmt[8], name[160], id[1300], extra[300]; } Line;
static Line *g_lines; static int g_nl;

static void sanitize(char *dst, size_t sz, const char *s)
{
    snprintf(dst, sz, "%s", s ? s : "");
    for (char *c = dst; *c; ++c) if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
}

void plug_print_line(const char *kind, const char *fmt, const char *name, const char *id, const char *extra)
{
    Line *n = realloc(g_lines, sizeof(Line) * (size_t)(g_nl + 1));
    if (!n) return;
    g_lines = n;
    Line *L = &g_lines[g_nl++];
    sanitize(L->kind, sizeof L->kind, kind);
    sanitize(L->fmt, sizeof L->fmt, fmt);
    sanitize(L->name, sizeof L->name, name);
    sanitize(L->id, sizeof L->id, id);
    snprintf(L->extra, sizeof L->extra, "%s", extra ? extra : "");     /* contains deliberate tabs */
    for (char *c = L->extra; *c; ++c) if (*c == '\n' || *c == '\r') *c = ' ';
}

int qplug_scan_child(const char *fmt, const char *path)
{
    if (!strcmp(fmt, "VST3")) vst3_scan(path);
    else if (!strcmp(fmt, "VST") || !strcmp(fmt, "VST2")) vst2_scan(path);
    else if (!strcmp(fmt, "LV2")) lv2_scan(path);
    else if (!strcmp(fmt, "CLAP")) clap_scan(path);
    else plug_print_line("ERR", fmt, path, path, "unknown format");
    for (int i = 0; i < g_nl; ++i) {
        Line *L = &g_lines[i];
        if (!strcmp(L->kind, "PLUG") && !strncmp(L->extra, "fx", 2)) {
            char err[300] = "";
            /* the line is printed before the probe: if the plugin crashes the process, the parent sees which one it was */
            printf("TRY\t%s\t%s\t%s\n", L->fmt, L->name, L->id);
            fflush(stdout);
            if (!qplug_selftest(L->fmt, L->id, err, (int)sizeof err)) {
                printf("ERR\t%s\t%s\t%s\t%s\n", L->fmt, L->name, L->id, err[0] ? err : "failed to start");
                fflush(stdout);
                continue;
            }
        }
        if (!strcmp(L->kind, "PLUG")) printf("PLUG\t%s\t%s\t%s\t%s\n", L->fmt, L->name, L->id, L->extra);
        else printf("ERR\t%s\t%s\t%s\t%s\n", L->fmt, L->name, L->id, L->extra);
        fflush(stdout);
    }
    return 0;
}
