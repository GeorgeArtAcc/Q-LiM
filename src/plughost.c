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
#ifndef _WIN32
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#if defined(__GLIBC__)
#include <execinfo.h>
#endif

/* 2.0.0: which plugin is running right now (for "which plugin crashed / froze Q-LiM") */
typedef struct { char fmt[8]; char id[1300]; } PlugRef;

struct QPlug {
    PlugRef ref;
    QInst in[2];
    int   ninst;                 /* 2 = mono plugin run twice (left and right) */
    int   maxb;
    int   nbi, nbo;              /* number of input/output buffers of one instance */
    float *mem;
    float **bi, **bo;            /* nbi / nbo buffers (any number of channels) */
    PlugWin *win;                /* open window (or NULL) */
};

static __thread PlugRef *t_cur;                     /* this thread is inside this plugin */
static PlugRef *volatile g_cur_main, *volatile g_cur_audio;
static PlugRef g_loading;                           /* being loaded (main thread) */
#define ENTER_MAIN(r)  PlugRef *prev_ = t_cur; t_cur = (r); g_cur_main = (r)
#define LEAVE_MAIN()   t_cur = prev_; g_cur_main = prev_

/* all loaded ones (for qplug_idle_all) - main thread only */
#define MAX_QP 512
static QPlug *g_all[MAX_QP];
static int    g_nall;
static pthread_mutex_t g_all_mu = PTHREAD_MUTEX_INITIALIZER;   /* step 35d: loading on a background thread too */

int qplug_open_inst_here(const char *fmt, const char *id, double rate, int maxb, QInst *q, char *err, int esz)
{
    memset(q, 0, sizeof *q);
    if (!strcmp(fmt, "VST3")) return vst3_open(id, rate, maxb, q, err, esz);
    if (!strcmp(fmt, "VST") || !strcmp(fmt, "VST2")) return vst2_open(id, rate, maxb, q, err, esz);
    if (!strcmp(fmt, "LV2")) return lv2_open(id, rate, maxb, q, err, esz);
    if (!strcmp(fmt, "CLAP")) return clap_open(id, rate, maxb, q, err, esz);
    snprintf(err, (size_t)esz, "unknown plugin format %s", fmt);
    return -1;
}

static bool g_child;
static int g_nsep_peek(void);
void qplug_mark_child(void) { g_child = true; }
bool qplug_is_child(void) { return g_child; }

static void (*g_report)(const char *);
void qplug_set_report_hook(void (*fn)(const char *msg)) { g_report = fn; }
void qplug_report(const char *msg) { if (g_report) g_report(msg); }

static int open_one(const char *fmt, const char *id, double rate, int maxb, QInst *q, char *err, int esz, bool sep)
{
    if (sep) return bridge_open(fmt, id, rate, maxb, q, err, esz);
    return qplug_open_inst_here(fmt, id, rate, maxb, q, err, esz);
}

QPlug *qplug_load(const char *fmt, const char *id, double rate, int max_block, char *err, int esz)
{
    if (max_block < 16) max_block = 16;
    QPlug *p = calloc(1, sizeof *p);
    if (!p) { snprintf(err, (size_t)esz, "out of memory"); return NULL; }
    p->maxb = max_block;
    snprintf(p->ref.fmt, sizeof p->ref.fmt, "%s", fmt);
    snprintf(p->ref.id, sizeof p->ref.id, "%s", id);
    bool sep = !g_child && qplug_wants_separate(fmt, id);
    if (sep && qplug_separate_mode() != QSEP_ALL) {
        static char told[64][160]; static int ntold;
        bool seen = false;
        for (int i = 0; i < ntold; ++i) if (!strncmp(told[i], id, sizeof told[i] - 1)) seen = true;
        if (!seen && ntold < 64) {
            snprintf(told[ntold++], sizeof told[0], "%s", id);
            char nm[300], why[80] = "";
            snprintf(nm, sizeof nm, "%s", id);
            char *bar = strchr(nm, '|');
            if (bar) *bar = 0;
            size_t nl = strlen(nm);
            while (nl > 1 && nm[nl - 1] == '/') nm[--nl] = 0;
            char *sl = strrchr(nm, '/'), *base = sl ? sl + 1 : nm, *dot = strrchr(base, '.');
            if (dot && dot != base) *dot = 0;
            for (int i = 0; i < g_nsep_peek(); ++i) {
                char f[8], pid[1300];
                if (qplug_separate_get(i, f, sizeof f, pid, sizeof pid, why, sizeof why) && !strcmp(f, fmt) && !strcmp(pid, id)) break;
                why[0] = 0;
            }
            printf(">>> Plugin '%s' runs in a separate process (reason: '%s')\n", base,
                   !why[0] ? "its window needs Gtk" : !strcmp(why, "chosen by you") ? "your choice" : why);
        }
    }
    g_loading = p->ref;
    ENTER_MAIN(&g_loading);
    int orc = open_one(fmt, id, rate, max_block, &p->in[0], err, esz, sep);
    LEAVE_MAIN();
    if (orc != 0) { free(p); return NULL; }
    p->ninst = 1;
    if (p->in[0].nout <= 0) {
        snprintf(err, (size_t)esz, "plugin has no audio output");
        qplug_free(p); return NULL;
    }
    if (p->in[0].nin > QPLUG_MAXCH || p->in[0].nout > QPLUG_MAXCH) {
        snprintf(err, (size_t)esz, "too many channels (%d in / %d out)", p->in[0].nin, p->in[0].nout);
        qplug_free(p); return NULL;
    }
    if (p->in[0].nin == 1 && p->in[0].nout == 1) {          /* mono: a second instance for the right channel */
        ENTER_MAIN(&g_loading);
        orc = open_one(fmt, id, rate, max_block, &p->in[1], err, esz, sep);
        LEAVE_MAIN();
        if (orc != 0) { qplug_free(p); return NULL; }
        p->ninst = 2;
    }
    p->nbi = p->in[0].nin < 2 ? 2 : p->in[0].nin;
    p->nbo = p->in[0].nout < 2 ? 2 : p->in[0].nout;
    p->mem = calloc((size_t)(p->nbi + p->nbo) * (size_t)max_block, sizeof(float));
    p->bi = calloc((size_t)p->nbi, sizeof(float *));
    p->bo = calloc((size_t)p->nbo, sizeof(float *));
    if (!p->mem || !p->bi || !p->bo) { snprintf(err, (size_t)esz, "out of memory"); qplug_free(p); return NULL; }
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

bool qplug_is_separate(QPlug *p) { return p && p->in[0].separate; }

void qplug_process(QPlug *p, float *l, float *r, unsigned n)
{
    t_cur = &p->ref; g_cur_audio = &p->ref;
    while (n > 0) {
        unsigned k = n > (unsigned)p->maxb ? (unsigned)p->maxb : n;
        run_chunk(p, l, r, k);
        l += k; r += k; n -= k;
    }
    t_cur = NULL; g_cur_audio = NULL;
}

void qplug_free(QPlug *p)
{
    if (!p) return;
    qplug_editor_close(p);
    pthread_mutex_lock(&g_all_mu);
    for (int i = 0; i < g_nall; ++i) if (g_all[i] == p) { g_all[i] = g_all[--g_nall]; break; }
    pthread_mutex_unlock(&g_all_mu);
    ENTER_MAIN(&p->ref);
    for (int i = 0; i < p->ninst; ++i) if (p->in[i].destroy) p->in[i].destroy(p->in[i].h);
    LEAVE_MAIN();
    free(p->mem);
    free(p->bi);
    free(p->bo);
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
    ENTER_MAIN(&p->ref);
    int erc = p->in[0].ed_open(p->in[0].h, w, &W, &H, err, esz);
    LEAVE_MAIN();
    if (erc != 0) { plugwin_destroy(w); return -1; }
    if (W <= 0 || H <= 0) plugwin_child_size(w, &W, &H);
    if (W > 0 && H > 0) plugwin_resize(w, W, H);
    plugwin_show(w);
    p->win = w;
    return 0;
}

void qplug_editor_close(QPlug *p)
{
    if (!p || !p->win) return;
    ENTER_MAIN(&p->ref);
    if (p->in[0].ed_close) p->in[0].ed_close(p->in[0].h);
    LEAVE_MAIN();
    plugwin_destroy(p->win);
    p->win = NULL;
}

int qplug_editors_open(void)
{
    int k = 0;
    pthread_mutex_lock(&g_all_mu);
    for (int i = 0; i < g_nall; ++i) if (g_all[i] && g_all[i]->win) ++k;
    pthread_mutex_unlock(&g_all_mu);
    return k;
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
        ENTER_MAIN(&p->ref);
        for (int k = 0; k < p->ninst; ++k) if (p->in[k].main_idle) p->in[k].main_idle(p->in[k].h);
        if (p->win && p->in[0].ed_idle) p->in[0].ed_idle(p->in[0].h);
        LEAVE_MAIN();
    }
}

/* ---- state: with dual mono the left one is saved and given to both ---- */
int qplug_get_state(QPlug *p, void **data, size_t *len)
{
    *data = NULL; *len = 0;
    if (!p || !p->in[0].get_state) return -1;
    ENTER_MAIN(&p->ref);
    int rc = p->in[0].get_state(p->in[0].h, data, len);
    LEAVE_MAIN();
    return rc;
}

int qplug_set_state(QPlug *p, const void *data, size_t len)
{
    if (!p || !data || !len || !p->in[0].set_state) return -1;
    int r = 0;
    unsigned before = atomic_load(&g_edits);          /* loading is not "touching" */
    ENTER_MAIN(&p->ref);
    for (int k = 0; k < p->ninst; ++k) if (p->in[k].set_state && p->in[k].set_state(p->in[k].h, data, len) != 0) r = -1;
    LEAVE_MAIN();
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

int qplug_scan_child(const char *fmt, const char *path, int skip)
{
    if (!strcmp(fmt, "VST3")) vst3_scan(path);
    else if (!strcmp(fmt, "VST") || !strcmp(fmt, "VST2")) vst2_scan(path);
    else if (!strcmp(fmt, "LV2")) lv2_scan(path);
    else if (!strcmp(fmt, "CLAP")) clap_scan(path);
    else plug_print_line("ERR", fmt, path, path, "unknown format");
    for (int i = skip; i < g_nl; ++i) {                 /* 2.0.0: skip = the ones a previous child already answered */
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

/* ===================================================================================================
 * 2.0.0: which plugins run in a separate process. The list file (~/.config/qlim/plugins-separate.txt):
 *   mode=0|1|2
 *   <format><TAB><id><TAB><why>        (one plugin per line)
 * =================================================================================================== */
#define SEP_MAX 256
typedef struct { char fmt[8]; char id[1300]; char why[80]; } SepEnt;
static SepEnt *g_sep;
static int     g_nsep;
static int     g_sep_mode = QSEP_AUTO;
static char    g_sep_path[1100];
static unsigned g_sep_gen;
unsigned qplug_separate_gen(void) { return g_sep_gen; }

static void sep_save(void)
{
    if (!g_sep_path[0]) return;
    FILE *f = fopen(g_sep_path, "w");
    if (!f) return;
    fprintf(f, "# Q-LiM: plugins that run in a separate process (Edit -> Plugins -> Safety)\nmode=%d\n", g_sep_mode);
    for (int i = 0; i < g_nsep; ++i) fprintf(f, "%s\t%s\t%s\n", g_sep[i].fmt, g_sep[i].id, g_sep[i].why);
    fclose(f);
}

void qplug_separate_init(const char *list_path)
{
    snprintf(g_sep_path, sizeof g_sep_path, "%s", list_path);
    if (!g_sep) g_sep = calloc(SEP_MAX, sizeof *g_sep);
    g_nsep = 0;
    FILE *f = fopen(g_sep_path, "r");
    if (!f || !g_sep) { if (f) fclose(f); return; }
    char line[1600];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        if (!strncmp(line, "mode=", 5)) { int m = atoi(line + 5); if (m >= QSEP_NEVER && m <= QSEP_ALL) g_sep_mode = m; continue; }
        char *t1 = strchr(line, '\t');
        if (!t1 || g_nsep >= SEP_MAX) continue;
        *t1 = 0;
        char *t2 = strchr(t1 + 1, '\t');
        if (t2) *t2 = 0;
        SepEnt *e = &g_sep[g_nsep++];
        snprintf(e->fmt, sizeof e->fmt, "%s", line);
        snprintf(e->id, sizeof e->id, "%s", t1 + 1);
        snprintf(e->why, sizeof e->why, "%s", t2 ? t2 + 1 : "");
    }
    fclose(f);
}

int  qplug_separate_mode(void) { return g_sep_mode; }
void qplug_set_separate_mode(int mode) { if (mode >= QSEP_NEVER && mode <= QSEP_ALL && mode != g_sep_mode) { g_sep_mode = mode; ++g_sep_gen; sep_save(); } }

bool qplug_separate_listed(const char *fmt, const char *id)
{
    for (int i = 0; i < g_nsep; ++i) if (!strcmp(g_sep[i].fmt, fmt) && !strcmp(g_sep[i].id, id)) return true;
    return false;
}

bool qplug_wants_separate(const char *fmt, const char *id)
{
    if (g_sep_mode == QSEP_NEVER) return false;
    if (g_sep_mode == QSEP_ALL) return true;
    if (qplug_separate_listed(fmt, id)) return true;
#ifndef _WIN32
    if (!strcmp(fmt, "LV2")) {                      /* asked once per plugin */
        static struct { char id[300]; bool yes; } cache[128];
        static int nc;
        for (int i = 0; i < nc; ++i) if (!strncmp(cache[i].id, id, sizeof cache[i].id - 1)) return cache[i].yes;
        bool yes = lv2_ui_needs_separate(id);
        if (nc < 128) { snprintf(cache[nc].id, sizeof cache[nc].id, "%s", id); cache[nc++].yes = yes; }
        return yes;
    }
#endif
    return false;
}

void qplug_separate_set(const char *fmt, const char *id, bool on, const char *why)
{
    if (!g_sep) g_sep = calloc(SEP_MAX, sizeof *g_sep);
    if (!g_sep) return;
    for (int i = 0; i < g_nsep; ++i)
        if (!strcmp(g_sep[i].fmt, fmt) && !strcmp(g_sep[i].id, id)) {
            if (!on) { g_sep[i] = g_sep[--g_nsep]; ++g_sep_gen; sep_save(); }
            return;
        }
    if (!on || g_nsep >= SEP_MAX) return;
    SepEnt *e = &g_sep[g_nsep++];
    snprintf(e->fmt, sizeof e->fmt, "%s", fmt);
    snprintf(e->id, sizeof e->id, "%s", id);
    snprintf(e->why, sizeof e->why, "%s", why ? why : "");
    ++g_sep_gen;
    sep_save();
}

int qplug_separate_count(void) { return g_nsep; }
static int g_nsep_peek(void) { return g_nsep; }
bool qplug_separate_get(int i, char *fmt, int fsz, char *id, int isz, char *why, int wsz)
{
    if (i < 0 || i >= g_nsep) return false;
    if (fmt) snprintf(fmt, (size_t)fsz, "%s", g_sep[i].fmt);
    if (id) snprintf(id, (size_t)isz, "%s", g_sep[i].id);
    if (why) snprintf(why, (size_t)wsz, "%s", g_sep[i].why);
    return true;
}

/* only async-signal-safe calls (Linux): the line is appended to the file */
static void sep_append_raw(const PlugRef *r, const char *why)
{
    if (!g_sep_path[0] || !r || !r->id[0]) return;
    for (int i = 0; i < g_nsep; ++i) if (!strcmp(g_sep[i].fmt, r->fmt) && !strcmp(g_sep[i].id, r->id)) return;
#ifdef _WIN32
    FILE *f = fopen(g_sep_path, "a");               /* compat.h: UTF-8 file names */
    if (!f) return;
    fprintf(f, "%s\t%s\t%s\n", r->fmt, r->id, why);
    fclose(f);
#else
    int fd = open(g_sep_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    if (write(fd, r->fmt, strlen(r->fmt)) < 0 || write(fd, "\t", 1) < 0 || write(fd, r->id, strlen(r->id)) < 0 ||
        write(fd, "\t", 1) < 0 || write(fd, why, strlen(why)) < 0 || write(fd, "\n", 1) < 0) {}
    close(fd);
#endif
}

static bool same_path_prefix(const char *file, const char *base, size_t bl)   /* Windows: no case, \ = / */
{
#ifdef _WIN32
    for (size_t i = 0; i < bl; ++i) {
        char a = file[i], b = base[i];
        if (!a) return false;
        if (a == '\\') a = '/';
        if (b == '\\') b = '/';
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
#else
    return !strncmp(file, base, bl);
#endif
}

/* the binary of an in-process plugin contains this address? (a crash in the plugin's own thread) */
static const PlugRef *ref_for_file(const char *file)
{
    if (!file) return NULL;
    for (int i = 0; i < g_nall; ++i) {
        QPlug *p = g_all[i];
        if (!p || p->in[0].separate) continue;
        char base[1300];
        snprintf(base, sizeof base, "%s", p->ref.id);
        char *bar = strchr(base, '|');
        if (bar) *bar = 0;                           /* LV2 / VST3: the bundle folder; CLAP: the file */
        size_t bl = strlen(base);
        while (bl > 1 && base[bl - 1] == '/') base[--bl] = 0;
        if (bl > 1 && same_path_prefix(file, base, bl)) return &p->ref;
    }
    return NULL;
}

/* Windows: the crash handler knows the file (DLL) where it happened */
void qplug_crash_note_module(const char *module_file)
{
    const PlugRef *r = t_cur;
    if (!r) r = ref_for_file(module_file);
    if (r) sep_append_raw(r, "crashed Q-LiM");
}

void qplug_crash_note(void)
{
#ifndef _WIN32
    const PlugRef *r = t_cur;
    if (r && r == &g_loading) r = &g_loading;
#if defined(__GLIBC__)
    if (!r) {
        void *bt[48];
        int n = backtrace(bt, 48);
        for (int i = 0; i < n && !r; ++i) {
            Dl_info di;
            if (dladdr(bt[i], &di) && di.dli_fname) r = ref_for_file(di.dli_fname);
        }
    }
#endif
    if (r) sep_append_raw(r, "crashed Q-LiM");
#endif
}

void qplug_freeze_note(bool audio)
{
    {
    PlugRef *r = audio ? g_cur_audio : g_cur_main;
    for (int i = 0; r && i < g_nall; ++i)            /* already in a separate process: it cannot freeze Q-LiM */
        if (g_all[i] && &g_all[i]->ref == r && g_all[i]->in[0].separate) r = NULL;
    if (r) {
        PlugRef copy = *r;
        sep_append_raw(&copy, audio ? "froze the sound" : "froze Q-LiM");
        fprintf(stderr, ">>> Q-LiM: the plugin '%s' (%s) is not answering - next time it will run in a separate process\n", copy.id, copy.fmt);
    }
    }
}
