/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* CLAP plugins (the CLAP headers are MIT, in third_party/clap).
 * Step 35: window (gui, x11), timers and descriptors for the window, state, on_main_thread. */
#define _GNU_SOURCE
#include "plughost.h"
#include "version.h"
#include <clap/clap.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static bool ev_push(const clap_output_events_t *l, const clap_event_header_t *e)
{
    (void)l;
    if (e && e->space_id == CLAP_CORE_EVENT_SPACE_ID && e->type == CLAP_EVENT_PARAM_VALUE) qplug_note_edit();
    return true;
}
static const clap_output_events_t g_out_ev = { NULL, ev_push };

/* 1.2: the input events of one process call - MIDI from the keyboard and parameter changes from the device panel */
typedef union { clap_event_header_t h; clap_event_note_t note; clap_event_midi_t midi; clap_event_param_value_t pv; } CEv;
#define CL_PQ 128
typedef struct { clap_id id; void *cookie; double min, max, def; uint32_t flags; char name[CLAP_NAME_SIZE]; } CParam;

typedef struct {
    clap_host_t host;                /* host_data -> this CL */
    void *lib;
    const clap_plugin_entry_t *entry;
    const clap_plugin_t *pl;
    bool started;
    uint32_t nin, nout;
    atomic_bool want_callback;
    PlugWin *win;
    bool gui_created;
    const clap_plugin_gui_t *gui;
    const clap_plugin_timer_support_t *timers;
    const clap_plugin_posix_fd_support_t *fds;
    unsigned tm_ids[64]; int ntm;    /* our timer ids */
    /* 1.2 */
    int dialect;                     /* 0 = no MIDI, 1 = raw MIDI, 2 = CLAP notes */
    int16_t note_port;
    CEv ev[QMIDI_MAX + CL_PQ];
    const clap_event_header_t *evp[QMIDI_MAX + CL_PQ];
    uint32_t nev;
    clap_input_events_t in_ev;
    const clap_plugin_params_t *params;
    CParam *par; int npar;           /* the visible parameters */
    struct { clap_id id; void *cookie; double v; } pq[CL_PQ];   /* UI -> audio: values to send */
    atomic_uint pq_head, pq_tail;
} CL;

static uint32_t cin_size(const clap_input_events_t *l) { return ((const CL *)l->ctx)->nev; }
static const clap_event_header_t *cin_get(const clap_input_events_t *l, uint32_t i)
{
    const CL *c = l->ctx;
    return i < c->nev ? c->evp[i] : NULL;
}

static pthread_t g_main_thread;
static bool      g_main_known;
void clap_host_set_main_thread(void) { if (!g_main_known) { g_main_thread = pthread_self(); g_main_known = true; } }

#define CLOF(h) ((CL *)(h)->host_data)

/* ---- host extensions ---- */
static void hg_resize_hints_changed(const clap_host_t *h) { (void)h; }
static bool hg_request_resize(const clap_host_t *h, uint32_t w, uint32_t hh)
{
    CL *c = CLOF(h);
    if (!c->win) return false;
    plugwin_resize(c->win, (int)w, (int)hh);
    if (c->gui && c->gui->set_size) c->gui->set_size(c->pl, w, hh);
    return true;
}
static bool hg_request_show(const clap_host_t *h) { CL *c = CLOF(h); if (c->win) plugwin_raise(c->win); return c->win != NULL; }
static bool hg_request_hide(const clap_host_t *h) { (void)h; return false; }
static void hg_closed(const clap_host_t *h, bool destroyed) { (void)h; (void)destroyed; }
static const clap_host_gui_t g_hgui = { hg_resize_hints_changed, hg_request_resize, hg_request_show, hg_request_hide, hg_closed };

static void tm_cb(void *ud, unsigned id)
{
    CL *c = ud;
    if (c->timers) c->timers->on_timer(c->pl, (clap_id)id);
}
static bool ht_register(const clap_host_t *h, uint32_t ms, clap_id *id)
{
    CL *c = CLOF(h);
    unsigned t = plugwin_add_timer(ms ? ms : 30, tm_cb, c);
    if (!t) return false;
    *id = (clap_id)t;
    if (c->ntm < 64) c->tm_ids[c->ntm++] = t;
    return true;
}
static bool ht_unregister(const clap_host_t *h, clap_id id) { (void)h; plugwin_remove_timer((unsigned)id); return true; }
static const clap_host_timer_support_t g_htimer = { ht_register, ht_unregister };

static void fd_cb(void *ud, int fd)
{
    CL *c = ud;
    if (c->fds) c->fds->on_fd(c->pl, fd, CLAP_POSIX_FD_READ);
}
static bool hf_register(const clap_host_t *h, int fd, clap_posix_fd_flags_t fl) { (void)fl; plugwin_add_fd(fd, fd_cb, CLOF(h)); return true; }
static bool hf_modify(const clap_host_t *h, int fd, clap_posix_fd_flags_t fl) { (void)h; (void)fd; (void)fl; return true; }
static bool hf_unregister(const clap_host_t *h, int fd) { plugwin_remove_fd(fd, CLOF(h)); return true; }
static const clap_host_posix_fd_support_t g_hfd __attribute__((unused)) = { hf_register, hf_modify, hf_unregister };

static bool hth_main(const clap_host_t *h) { (void)h; return g_main_known && pthread_equal(pthread_self(), g_main_thread); }
static bool hth_audio(const clap_host_t *h) { return !hth_main(h); }
static const clap_host_thread_check_t g_hthread = { hth_main, hth_audio };

static void hlog(const clap_host_t *h, clap_log_severity s, const char *msg) { (void)h; if (s >= CLAP_LOG_WARNING) printf(">>> CLAP: %s\n", msg); }
static const clap_host_log_t g_hlog = { hlog };

static void hst_dirty(const clap_host_t *h) { (void)h; qplug_note_edit(); }
static const clap_host_state_t g_hstate = { hst_dirty };

static void hp_rescan(const clap_host_t *h, clap_param_rescan_flags f) { (void)h; (void)f; }
static void hp_clear(const clap_host_t *h, clap_id id, clap_param_clear_flags f) { (void)h; (void)id; (void)f; }
static void hp_flush(const clap_host_t *h) { (void)h; }   /* processing runs all the time */
static const clap_host_params_t g_hparams = { hp_rescan, hp_clear, hp_flush };

static void hl_changed(const clap_host_t *h) { (void)h; }
static const clap_host_latency_t g_hlat = { hl_changed };

static const void *h_get_ext(const clap_host_t *h, const char *id)
{
    (void)h;
    if (!strcmp(id, CLAP_EXT_GUI)) return &g_hgui;
    if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &g_htimer;
#ifndef _WIN32
    if (!strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT)) return &g_hfd;
#endif
    if (!strcmp(id, CLAP_EXT_THREAD_CHECK)) return &g_hthread;
    if (!strcmp(id, CLAP_EXT_LOG)) return &g_hlog;
    if (!strcmp(id, CLAP_EXT_STATE)) return &g_hstate;
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &g_hparams;
    if (!strcmp(id, CLAP_EXT_LATENCY)) return &g_hlat;
    return NULL;
}
static void h_noop(const clap_host_t *h) { (void)h; }
static void h_request_callback(const clap_host_t *h) { if (h->host_data) atomic_store(&CLOF(h)->want_callback, true); }

static void host_init(CL *c)
{
    clap_host_t hh = { CLAP_VERSION_INIT, c, "Q-LiM", "Georgi Stanimirov Ivanov", "", QLIM_VERSION, h_get_ext, h_noop, h_noop, h_request_callback };
    memcpy(&c->host, &hh, sizeof hh);
}

/* one and the same .clap file: init() once, deinit() after the last instance */
typedef struct { char path[1024]; void *lib; const clap_plugin_entry_t *e; int refs; bool loading; } Mod;
static Mod g_mods[64];

/* Loading a module (dlopen + init) happens WITHOUT a lock - it can be slow (Wine). If the same file
 * is already being loaded by another thread, we wait for it (condition variable); different files do not wait. */
static pthread_mutex_t g_mod_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_mod_cv = PTHREAD_COND_INITIALIZER;
static const clap_plugin_entry_t *cl_entry_raw(const char *path, void **lib, char *err, int esz);
static const clap_plugin_entry_t *mod_get(const char *path, void **lib, char *err, int esz)
{
    pthread_mutex_lock(&g_mod_mu);
    for (;;) {
        Mod *m = NULL;
        for (int i = 0; i < 64; ++i) if ((g_mods[i].refs > 0 || g_mods[i].loading) && !strcmp(g_mods[i].path, path)) m = &g_mods[i];
        if (!m) break;
        if (!m->loading) { ++m->refs; *lib = m->lib; pthread_mutex_unlock(&g_mod_mu); return m->e; }
        pthread_cond_wait(&g_mod_cv, &g_mod_mu);
    }
    Mod *slot = NULL;
    for (int i = 0; i < 64; ++i) if (g_mods[i].refs == 0 && !g_mods[i].loading) { slot = &g_mods[i]; break; }
    if (slot) { snprintf(slot->path, sizeof slot->path, "%s", path); slot->loading = true; }
    pthread_mutex_unlock(&g_mod_mu);
    const clap_plugin_entry_t *e = cl_entry_raw(path, lib, err, esz);
    pthread_mutex_lock(&g_mod_mu);
    if (slot) {
        slot->loading = false;
        if (e) { slot->lib = *lib; slot->e = e; slot->refs = 1; }
        else slot->path[0] = 0;
        pthread_cond_broadcast(&g_mod_cv);
    }
    pthread_mutex_unlock(&g_mod_mu);
    return e;
}
static void mod_put(void *lib, const clap_plugin_entry_t *e)
{
    pthread_mutex_lock(&g_mod_mu);
    for (int i = 0; i < 64; ++i) if (g_mods[i].refs > 0 && g_mods[i].lib == lib) {
        if (--g_mods[i].refs == 0) { g_mods[i].path[0] = 0; e->deinit(); dlclose(lib); }
        pthread_mutex_unlock(&g_mod_mu);
        return;
    }
    e->deinit(); dlclose(lib);
    pthread_mutex_unlock(&g_mod_mu);
}

static const clap_plugin_entry_t *cl_entry_raw(const char *path, void **lib, char *err, int esz)
{
    *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!*lib) { snprintf(err, (size_t)esz, "cannot load: %s", dlerror()); return NULL; }
    const clap_plugin_entry_t *e = dlsym(*lib, "clap_entry");
    if (!e) { snprintf(err, (size_t)esz, "not a CLAP plugin"); dlclose(*lib); return NULL; }
#ifdef _WIN32
    char native[1024];                                /* Windows plugins expect C:\...\x.clap */
    snprintf(native, sizeof native, "%s", path);
    for (char *q = native; *q; ++q) if (*q == '/') *q = '\\';
    if (!clap_version_is_compatible(e->clap_version) || !e->init(native)) {
#else
    if (!clap_version_is_compatible(e->clap_version) || !e->init(path)) {
#endif
        snprintf(err, (size_t)esz, "incompatible CLAP version / init failed");
        dlclose(*lib); return NULL;
    }
    return e;
}

/* number of channels of the main audio port (stereo if the plugin does not say) */
static uint32_t main_port_channels(const clap_plugin_t *pl, bool input)
{
    const clap_plugin_audio_ports_t *ap = pl->get_extension(pl, CLAP_EXT_AUDIO_PORTS);
    if (!ap) return 2;
    if (ap->count(pl, input) == 0) return 0;
    clap_audio_port_info_t info;
    if (!ap->get(pl, 0, input, &info)) return 2;
    return info.channel_count;
}

static void cl_process(void *h, float **in, float **out, unsigned n)
{
    CL *c = h;
    if (!c->started) { c->started = c->pl->start_processing(c->pl); if (!c->started) return; }
    clap_audio_buffer_t bi = { in, NULL, c->nin, 0, 0 }, bo = { out, NULL, c->nout, 0, 0 };
    clap_process_t p;
    memset(&p, 0, sizeof p);
    p.steady_time = -1;
    p.frames_count = n;
    p.audio_inputs = &bi; p.audio_inputs_count = c->nin ? 1 : 0;
    p.audio_outputs = &bo; p.audio_outputs_count = 1;
    /* 1.2: parameter changes from the panel go first (time 0), then the MIDI (already in c->ev, in time order) */
    uint32_t nm = c->nev, np = 0;
    if (nm > QMIDI_MAX) nm = QMIDI_MAX;
    for (unsigned t = atomic_load_explicit(&c->pq_tail, memory_order_relaxed);
         t != atomic_load_explicit(&c->pq_head, memory_order_acquire) && np < CL_PQ; ++t) {
        clap_event_param_value_t *e = &c->ev[QMIDI_MAX + np].pv;
        memset(e, 0, sizeof *e);
        e->header.size = sizeof *e; e->header.space_id = CLAP_CORE_EVENT_SPACE_ID; e->header.type = CLAP_EVENT_PARAM_VALUE;
        e->param_id = c->pq[t % CL_PQ].id; e->cookie = c->pq[t % CL_PQ].cookie;
        e->note_id = -1; e->port_index = -1; e->channel = -1; e->key = -1;
        e->value = c->pq[t % CL_PQ].v;
        c->evp[np++] = &e->header;
        atomic_store_explicit(&c->pq_tail, t + 1, memory_order_release);
    }
    for (uint32_t i = 0; i < nm; ++i) {
        if (c->ev[i].h.time >= n) c->ev[i].h.time = n - 1;
        c->evp[np + i] = &c->ev[i].h;
    }
    c->nev = np + nm;
    p.in_events = &c->in_ev; p.out_events = &g_out_ev;
    c->pl->process(c->pl, &p);
    c->nev = 0;
}

/* 1.2: MIDI from the keyboard (RT thread) */
static void cl_midi(void *h, const QMidiEv *ev, int n)
{
    CL *c = h;
    for (int i = 0; i < n && c->nev < QMIDI_MAX; ++i) {
        CEv *e = &c->ev[c->nev];
        unsigned char st = ev[i].d[0] & 0xF0, ch = ev[i].d[0] & 0x0F;
        if (c->dialect == 1) {
            memset(&e->midi, 0, sizeof e->midi);
            e->midi.header.size = sizeof e->midi; e->midi.header.type = CLAP_EVENT_MIDI;
            e->midi.port_index = (uint16_t)c->note_port;
            memcpy(e->midi.data, ev[i].d, 3);
        } else if (st == 0x90 || st == 0x80) {
            bool on = st == 0x90 && ev[i].d[2] > 0;
            memset(&e->note, 0, sizeof e->note);
            e->note.header.size = sizeof e->note; e->note.header.type = on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF;
            e->note.note_id = -1; e->note.port_index = c->note_port; e->note.channel = ch; e->note.key = ev[i].d[1];
            e->note.velocity = ev[i].d[2] / 127.0;
        } else continue;                                /* CLAP notes only: CC/pitch bend are not sent */
        e->h.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e->h.time = ev[i].frame;
        e->h.flags = 0;
        c->nev++;
    }
}

/* 1.2: parameters (main thread) */
static int cl_param_count(void *h) { return ((CL *)h)->npar; }
static bool cl_param_name(void *h, int i, char *name, int nsz)
{
    CL *c = h;
    if (i < 0 || i >= c->npar) return false;
    snprintf(name, (size_t)nsz, "%s", c->par[i].name);
    return true;
}
static float cl_param_get(void *h, int i)
{
    CL *c = h;
    if (i < 0 || i >= c->npar) return 0.0f;
    double v = c->par[i].def;
    c->params->get_value(c->pl, c->par[i].id, &v);
    double r = c->par[i].max - c->par[i].min;
    return r > 0.0 ? (float)((v - c->par[i].min) / r) : 0.0f;
}
static void cl_param_set(void *h, int i, float x)
{
    CL *c = h;
    if (i < 0 || i >= c->npar) return;
    double v = c->par[i].min + (double)x * (c->par[i].max - c->par[i].min);
    if (c->par[i].flags & CLAP_PARAM_IS_STEPPED) v = floor(v + 0.5);
    unsigned hd = atomic_load(&c->pq_head);
    if (hd - atomic_load(&c->pq_tail) >= CL_PQ) return;      /* full: the audio thread is behind */
    c->pq[hd % CL_PQ].id = c->par[i].id; c->pq[hd % CL_PQ].cookie = c->par[i].cookie; c->pq[hd % CL_PQ].v = v;
    atomic_store_explicit(&c->pq_head, hd + 1, memory_order_release);
}
static void cl_param_text(void *h, int i, char *out, int sz)
{
    CL *c = h;
    if (i < 0 || i >= c->npar || !c->params->value_to_text) return;
    double v = c->par[i].def;
    c->params->get_value(c->pl, c->par[i].id, &v);
    if (!c->params->value_to_text(c->pl, c->par[i].id, v, out, (uint32_t)sz)) out[0] = 0;
}

/* ---- window ---- */
#ifdef _WIN32
#define QCLAP_API CLAP_WINDOW_API_WIN32
#else
#define QCLAP_API CLAP_WINDOW_API_X11
#endif
static bool cl_has_editor(void *h)
{
    CL *c = h;
    return c->gui && c->gui->is_api_supported(c->pl, QCLAP_API, false);
}

static int cl_ed_open(void *h, PlugWin *w, int *W, int *H, char *err, int esz)
{
    CL *c = h;
    c->win = w;
    if (!c->gui->create(c->pl, QCLAP_API, false)) {
        c->win = NULL;
        snprintf(err, (size_t)esz, "the plugin could not create its window");
        return -1;
    }
    c->gui_created = true;
    if (c->gui->set_scale) c->gui->set_scale(c->pl, 1.0);
    uint32_t ww = 0, hh = 0;
    if (c->gui->get_size(c->pl, &ww, &hh) && ww > 0 && hh > 0) { plugwin_resize(w, (int)ww, (int)hh); *W = (int)ww; *H = (int)hh; }
    clap_window_t win;
    win.api = QCLAP_API;
#ifdef _WIN32
    win.win32 = plugwin_native(w);
#else
    win.x11 = (clap_xwnd)plugwin_xid(w);
#endif
    if (!c->gui->set_parent(c->pl, &win)) {
        c->gui->destroy(c->pl);
        c->gui_created = false; c->win = NULL;
        snprintf(err, (size_t)esz, "the plugin could not attach its window");
        return -1;
    }
    c->gui->show(c->pl);
    if (c->gui->get_size(c->pl, &ww, &hh) && ww > 0 && hh > 0) { *W = (int)ww; *H = (int)hh; }
    return 0;
}

static void cl_ed_close(void *h)
{
    CL *c = h;
    if (c->gui_created) { c->gui->hide(c->pl); c->gui->destroy(c->pl); }
    c->gui_created = false;
    c->win = NULL;
}

static void cl_main_idle(void *h)
{
    CL *c = h;
    if (atomic_exchange(&c->want_callback, false)) c->pl->on_main_thread(c->pl);
}

/* ---- state ---- */
typedef struct { unsigned char *b; size_t n, cap; } MemBuf;
typedef struct { const unsigned char *b; size_t n, pos; } MemIn;
static int64_t os_write(const clap_ostream_t *s, const void *buf, uint64_t n)
{
    MemBuf *m = s->ctx;
    if (m->n + n > m->cap) {
        size_t nc = (m->cap ? m->cap * 2 : 4096);
        while (nc < m->n + n) nc *= 2;
        unsigned char *nb = realloc(m->b, nc);
        if (!nb) return -1;
        m->b = nb; m->cap = nc;
    }
    memcpy(m->b + m->n, buf, (size_t)n);
    m->n += (size_t)n;
    return (int64_t)n;
}
static int64_t is_read(const clap_istream_t *s, void *buf, uint64_t n)
{
    MemIn *m = s->ctx;
    size_t left = m->n - m->pos;
    if (n > left) n = left;
    memcpy(buf, m->b + m->pos, (size_t)n);
    m->pos += (size_t)n;
    return (int64_t)n;
}

static int cl_get_state(void *h, void **data, size_t *len)
{
    CL *c = h;
    const clap_plugin_state_t *st = c->pl->get_extension(c->pl, CLAP_EXT_STATE);
    if (!st) return -1;
    MemBuf m = { 0 };
    clap_ostream_t os = { &m, os_write };
    if (!st->save(c->pl, &os)) { free(m.b); return -1; }
    *data = m.b; *len = m.n;
    return 0;
}

static int cl_set_state(void *h, const void *data, size_t len)
{
    CL *c = h;
    const clap_plugin_state_t *st = c->pl->get_extension(c->pl, CLAP_EXT_STATE);
    if (!st) return -1;
    MemIn m = { data, len, 0 };
    clap_istream_t is = { &m, is_read };
    return st->load(c->pl, &is) ? 0 : -1;
}

static void cl_destroy(void *h)
{
    CL *c = h;
    cl_ed_close(c);
    plugwin_remove_timers_of(c);
    plugwin_remove_fds_of(c);
    if (c->started) c->pl->stop_processing(c->pl);
    c->pl->deactivate(c->pl);
    c->pl->destroy(c->pl);
    mod_put(c->lib, c->entry);
    free(c->par);
    free(c);
}

int clap_open(const char *id, double rate, int maxb, QInst *out, char *err, int esz)
{
    char path[1024];
    snprintf(path, sizeof path, "%s", id);
    char *bar = strrchr(path, '|');
    if (!bar) { snprintf(err, (size_t)esz, "bad CLAP id"); return -1; }
    *bar = 0;
    const char *pid = bar + 1;
    void *lib;
    const clap_plugin_entry_t *e = mod_get(path, &lib, err, esz);
    if (!e) return -1;
    CL *c = calloc(1, sizeof *c);
    if (!c) { mod_put(lib, e); snprintf(err, (size_t)esz, "out of memory"); return -1; }
    host_init(c);
    const clap_plugin_factory_t *f = e->get_factory(CLAP_PLUGIN_FACTORY_ID);
    const clap_plugin_t *pl = f ? f->create_plugin(f, &c->host, pid) : NULL;
    if (!pl || !pl->init(pl)) {
        snprintf(err, (size_t)esz, "cannot create plugin '%s'", pid);
        if (pl) pl->destroy(pl);
        mod_put(lib, e); free(c); return -1;
    }
    if (!pl->activate(pl, rate, 1, (uint32_t)maxb)) {
        snprintf(err, (size_t)esz, "plugin refused to activate");
        pl->destroy(pl); mod_put(lib, e); free(c); return -1;
    }
    c->lib = lib; c->entry = e; c->pl = pl;
    c->nin = main_port_channels(pl, true);
    c->nout = main_port_channels(pl, false);
    c->gui = pl->get_extension(pl, CLAP_EXT_GUI);
    c->timers = pl->get_extension(pl, CLAP_EXT_TIMER_SUPPORT);
    c->fds = pl->get_extension(pl, CLAP_EXT_POSIX_FD_SUPPORT);
    out->h = c; out->nin = (int)c->nin; out->nout = (int)c->nout;
    out->process = cl_process; out->destroy = cl_destroy;
    out->has_editor = cl_has_editor; out->ed_open = cl_ed_open; out->ed_close = cl_ed_close;
    out->main_idle = cl_main_idle;
    out->get_state = cl_get_state; out->set_state = cl_set_state;
    /* 1.2: MIDI - through the first note input port (raw MIDI if it takes it, otherwise CLAP notes) */
    c->in_ev.ctx = c; c->in_ev.size = cin_size; c->in_ev.get = cin_get;
    const clap_plugin_note_ports_t *npo = pl->get_extension(pl, CLAP_EXT_NOTE_PORTS);
    if (npo && npo->count(pl, true) > 0) {
        clap_note_port_info_t ni;
        if (npo->get(pl, 0, true, &ni)) {
            c->note_port = 0;
            if (ni.supported_dialects & CLAP_NOTE_DIALECT_MIDI) c->dialect = 1;
            else if (ni.supported_dialects & CLAP_NOTE_DIALECT_CLAP) c->dialect = 2;
        }
    }
    if (c->dialect) out->midi = cl_midi;
    /* 1.2: parameters (hidden and read-only ones are skipped) */
    c->params = pl->get_extension(pl, CLAP_EXT_PARAMS);
    if (c->params) {
        uint32_t pc = c->params->count(pl);
        c->par = calloc(pc ? pc : 1, sizeof *c->par);
        for (uint32_t i = 0; c->par && i < pc; ++i) {
            clap_param_info_t pi;
            if (!c->params->get_info(pl, i, &pi)) continue;
            if (pi.flags & (CLAP_PARAM_IS_HIDDEN | CLAP_PARAM_IS_READONLY)) continue;
            CParam *q = &c->par[c->npar++];
            q->id = pi.id; q->cookie = pi.cookie; q->min = pi.min_value; q->max = pi.max_value; q->def = pi.default_value;
            q->flags = pi.flags;
            snprintf(q->name, sizeof q->name, "%s", pi.name);
        }
        out->param_count = cl_param_count; out->param_name = cl_param_name; out->param_get = cl_param_get;
        out->param_set = cl_param_set; out->param_text = cl_param_text;
    }
    return 0;
}

int clap_scan(const char *path)
{
    char err[256];
    void *lib;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const clap_plugin_entry_t *e = mod_get(path, &lib, err, sizeof err);
    if (!e) { plug_print_line("ERR", "CLAP", base, path, err); return 0; }
    const clap_plugin_factory_t *f = e->get_factory(CLAP_PLUGIN_FACTORY_ID);
    uint32_t n = f ? f->get_plugin_count(f) : 0;
    if (n == 0) plug_print_line("ERR", "CLAP", base, path, "no plugins inside");
    for (uint32_t i = 0; i < n; ++i) {
        const clap_plugin_descriptor_t *d = f->get_plugin_descriptor(f, i);
        if (!d) continue;
        bool inst = false;
        for (const char *const *ft = d->features; ft && *ft; ++ft)
            if (!strcmp(*ft, CLAP_PLUGIN_FEATURE_INSTRUMENT)) inst = true;
        char id[1100], extra[64];
        snprintf(id, sizeof id, "%s|%s", path, d->id);
        snprintf(extra, sizeof extra, "%s\t2\t2", inst ? "inst" : "fx");
        plug_print_line("PLUG", "CLAP", d->name, id, extra);
    }
    mod_put(lib, e);
    return 0;
}
