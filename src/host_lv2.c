/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* LV2 plugins through liblilv (loaded with dlopen: compiling needs no lilv-dev, the headers are in
 * third_party/lilv and third_party/lv2; if the machine has no liblilv-0.so.0 - LV2 simply does not work). */
#define _GNU_SOURCE
#include "plughost.h"
#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/log/log.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/urid/urid.h>
#include <lv2/ui/ui.h>
#include <lv2/state/state.h>
#include <lv2/instance-access/instance-access.h>
#include <lv2/data-access/data-access.h>
#include <stdatomic.h>
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    void *h;
    __typeof__(&lilv_world_new) world_new;
    __typeof__(&lilv_world_load_bundle) world_load_bundle;
    __typeof__(&lilv_world_get_all_plugins) world_get_all_plugins;
    __typeof__(&lilv_new_file_uri) new_file_uri;
    __typeof__(&lilv_new_uri) new_uri;
    __typeof__(&lilv_node_free) node_free;
    __typeof__(&lilv_node_as_string) node_as_string;
    __typeof__(&lilv_node_as_uri) node_as_uri;
    __typeof__(&lilv_node_equals) node_equals;
    __typeof__(&lilv_plugins_begin) plugins_begin;
    __typeof__(&lilv_plugins_next) plugins_next;
    __typeof__(&lilv_plugins_is_end) plugins_is_end;
    __typeof__(&lilv_plugins_get) plugins_get;
    __typeof__(&lilv_plugins_get_by_uri) plugins_get_by_uri;
    __typeof__(&lilv_plugin_get_uri) plugin_get_uri;
    __typeof__(&lilv_plugin_get_name) plugin_get_name;
    __typeof__(&lilv_plugin_get_class) plugin_get_class;
    __typeof__(&lilv_plugin_class_get_uri) plugin_class_get_uri;
    __typeof__(&lilv_plugin_get_num_ports) plugin_get_num_ports;
    __typeof__(&lilv_plugin_get_port_by_index) plugin_get_port_by_index;
    __typeof__(&lilv_plugin_get_port_ranges_float) plugin_get_port_ranges_float;
    __typeof__(&lilv_plugin_get_required_features) plugin_get_required_features;
    __typeof__(&lilv_plugin_get_bundle_uri) plugin_get_bundle_uri;
    __typeof__(&lilv_port_is_a) port_is_a;
    __typeof__(&lilv_nodes_begin) nodes_begin;
    __typeof__(&lilv_nodes_next) nodes_next;
    __typeof__(&lilv_nodes_is_end) nodes_is_end;
    __typeof__(&lilv_nodes_get) nodes_get;
    __typeof__(&lilv_nodes_free) nodes_free;
    __typeof__(&lilv_plugin_instantiate) plugin_instantiate;
    __typeof__(&lilv_instance_free) instance_free;
    /* step 35: interface and state */
    __typeof__(&lilv_plugin_get_uis) plugin_get_uis;
    __typeof__(&lilv_uis_begin) uis_begin;
    __typeof__(&lilv_uis_next) uis_next;
    __typeof__(&lilv_uis_is_end) uis_is_end;
    __typeof__(&lilv_uis_get) uis_get;
    __typeof__(&lilv_uis_free) uis_free;
    __typeof__(&lilv_ui_get_uri) ui_get_uri;
    __typeof__(&lilv_ui_is_a) ui_is_a;
    __typeof__(&lilv_ui_get_binary_uri) ui_get_binary_uri;
    __typeof__(&lilv_ui_get_bundle_uri) ui_get_bundle_uri;
    __typeof__(&lilv_file_uri_parse) file_uri_parse;
    __typeof__(&lilv_free) lfree;
    __typeof__(&lilv_port_get_symbol) port_get_symbol;
    __typeof__(&lilv_world_load_resource) world_load_resource;
    /* 1.2: MIDI port and parameter names */
    __typeof__(&lilv_port_supports_event) port_supports_event;
    __typeof__(&lilv_port_get_name) port_get_name;
    __typeof__(&lilv_port_has_property) port_has_property;
} L;

static int        g_lstate = 0;          /* 0 = not tried, 1 = present, -1 = no liblilv */
static LilvWorld *g_world;
static LilvNode  *n_audio, *n_control, *n_atom, *n_cv, *n_input, *n_output, *n_instr, *n_x11ui;
static LilvNode  *n_midi_ev, *n_integer, *n_toggled, *n_enum, *n_not_gui;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static int lv_load(char *err, int esz)
{
    if (g_lstate) {
        if (g_lstate < 0) snprintf(err, (size_t)esz, "liblilv is not installed (sudo apt install liblilv-0-0)");
        return g_lstate;
    }
    L.h = dlopen("liblilv-0.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!L.h) { g_lstate = -1; snprintf(err, (size_t)esz, "liblilv is not installed (sudo apt install liblilv-0-0)"); return -1; }
#define LD(f, s) do { *(void **)(&L.f) = dlsym(L.h, s); if (!L.f) { g_lstate = -1; snprintf(err, (size_t)esz, "liblilv: missing %s", s); return -1; } } while (0)
    LD(world_new, "lilv_world_new"); LD(world_load_bundle, "lilv_world_load_bundle");
    LD(world_get_all_plugins, "lilv_world_get_all_plugins"); LD(new_file_uri, "lilv_new_file_uri");
    LD(new_uri, "lilv_new_uri"); LD(node_free, "lilv_node_free"); LD(node_as_string, "lilv_node_as_string");
    LD(node_as_uri, "lilv_node_as_uri"); LD(node_equals, "lilv_node_equals");
    LD(plugins_begin, "lilv_plugins_begin"); LD(plugins_next, "lilv_plugins_next"); LD(plugins_is_end, "lilv_plugins_is_end");
    LD(plugins_get, "lilv_plugins_get"); LD(plugins_get_by_uri, "lilv_plugins_get_by_uri");
    LD(plugin_get_uri, "lilv_plugin_get_uri"); LD(plugin_get_name, "lilv_plugin_get_name");
    LD(plugin_get_class, "lilv_plugin_get_class"); LD(plugin_class_get_uri, "lilv_plugin_class_get_uri");
    LD(plugin_get_num_ports, "lilv_plugin_get_num_ports"); LD(plugin_get_port_by_index, "lilv_plugin_get_port_by_index");
    LD(plugin_get_port_ranges_float, "lilv_plugin_get_port_ranges_float");
    LD(plugin_get_required_features, "lilv_plugin_get_required_features");
    LD(plugin_get_bundle_uri, "lilv_plugin_get_bundle_uri");
    LD(port_is_a, "lilv_port_is_a"); LD(nodes_begin, "lilv_nodes_begin"); LD(nodes_next, "lilv_nodes_next");
    LD(nodes_is_end, "lilv_nodes_is_end"); LD(nodes_get, "lilv_nodes_get"); LD(nodes_free, "lilv_nodes_free");
    LD(plugin_instantiate, "lilv_plugin_instantiate"); LD(instance_free, "lilv_instance_free");
    LD(plugin_get_uis, "lilv_plugin_get_uis"); LD(uis_begin, "lilv_uis_begin"); LD(uis_next, "lilv_uis_next");
    LD(uis_is_end, "lilv_uis_is_end"); LD(uis_get, "lilv_uis_get"); LD(uis_free, "lilv_uis_free");
    LD(ui_get_uri, "lilv_ui_get_uri"); LD(ui_is_a, "lilv_ui_is_a"); LD(ui_get_binary_uri, "lilv_ui_get_binary_uri");
    LD(ui_get_bundle_uri, "lilv_ui_get_bundle_uri"); LD(file_uri_parse, "lilv_file_uri_parse"); LD(lfree, "lilv_free");
    LD(port_get_symbol, "lilv_port_get_symbol"); LD(world_load_resource, "lilv_world_load_resource");
    LD(port_supports_event, "lilv_port_supports_event"); LD(port_get_name, "lilv_port_get_name");
    LD(port_has_property, "lilv_port_has_property");
#undef LD
    g_world = L.world_new();
    n_audio = L.new_uri(g_world, LV2_CORE__AudioPort);
    n_control = L.new_uri(g_world, LV2_CORE__ControlPort);
    n_atom = L.new_uri(g_world, LV2_ATOM__AtomPort);
    n_cv = L.new_uri(g_world, LV2_CORE__CVPort);
    n_input = L.new_uri(g_world, LV2_CORE__InputPort);
    n_output = L.new_uri(g_world, LV2_CORE__OutputPort);
    n_instr = L.new_uri(g_world, LV2_CORE__InstrumentPlugin);
    n_x11ui = L.new_uri(g_world, LV2_UI__X11UI);
    n_midi_ev = L.new_uri(g_world, "http://lv2plug.in/ns/ext/midi#MidiEvent");
    n_integer = L.new_uri(g_world, LV2_CORE__integer);
    n_toggled = L.new_uri(g_world, LV2_CORE__toggled);
    n_enum = L.new_uri(g_world, LV2_CORE__enumeration);
    n_not_gui = L.new_uri(g_world, "http://lv2plug.in/ns/ext/port-props#notOnGUI");
    g_lstate = 1;
    return 1;
}

/* bundles are loaded once each */
static char **g_bundles; static int g_nb;
static void load_bundle(const char *dir)
{
    for (int i = 0; i < g_nb; ++i) if (!strcmp(g_bundles[i], dir)) return;
    char p[1100];
    snprintf(p, sizeof p, "%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/");
    LilvNode *b = L.new_file_uri(g_world, NULL, p);
    L.world_load_bundle(g_world, b);
    L.node_free(b);
    char **nb = realloc(g_bundles, sizeof(char *) * (size_t)(g_nb + 1));
    if (nb) { g_bundles = nb; g_bundles[g_nb++] = strdup(dir); }
}

/* ---- URID ---- */
static char **g_uris; static uint32_t g_nuris;
static LV2_URID urid_map(LV2_URID_Map_Handle h, const char *uri)
{
    (void)h;
    pthread_mutex_lock(&g_mu);
    for (uint32_t i = 0; i < g_nuris; ++i) if (!strcmp(g_uris[i], uri)) { pthread_mutex_unlock(&g_mu); return i + 1; }
    char **n = realloc(g_uris, sizeof(char *) * (g_nuris + 1));
    LV2_URID id = 0;
    if (n) { g_uris = n; g_uris[g_nuris++] = strdup(uri); id = g_nuris; }
    pthread_mutex_unlock(&g_mu);
    return id;
}
static const char *urid_unmap(LV2_URID_Unmap_Handle h, LV2_URID u)
{
    (void)h;
    pthread_mutex_lock(&g_mu);
    const char *r = (u >= 1 && u <= g_nuris) ? g_uris[u - 1] : NULL;
    pthread_mutex_unlock(&g_mu);
    return r;
}
static LV2_URID_Map   g_map = { NULL, urid_map };
static LV2_URID_Unmap g_unmap = { NULL, urid_unmap };

static int log_vprintf(LV2_Log_Handle h, LV2_URID type, const char *fmt, va_list ap)
{
    (void)h; (void)type;
    fprintf(stderr, ">>> LV2: ");
    return vfprintf(stderr, fmt, ap);
}
static int log_printf(LV2_Log_Handle h, LV2_URID type, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = log_vprintf(h, type, fmt, ap);
    va_end(ap);
    return r;
}
static LV2_Log_Log g_log = { NULL, log_printf, log_vprintf };

static const char *const k_supported[] = {
    LV2_URID__map, LV2_URID__unmap, LV2_OPTIONS__options, LV2_BUF_SIZE__boundedBlockLength, LV2_LOG__log,
    LV2_CORE__isLive, LV2_CORE__inPlaceBroken, LV2_CORE__hardRTCapable, NULL
};

/* ---- instance ---- */
#define ATOM_CAP 8192
/* messages (atoms) between the interface and the audio - lock-free queue */
#define AMSG_N   64
#define AMSG_MAX 4096
typedef struct { uint32_t port, size; uint8_t data[AMSG_MAX]; } AMsg;
typedef struct { AMsg m[AMSG_N]; atomic_uint w, r; } ARing;

static bool aring_push(ARing *q, uint32_t port, const void *d, uint32_t n)
{
    unsigned w = atomic_load_explicit(&q->w, memory_order_relaxed);
    if (n > AMSG_MAX || w - atomic_load_explicit(&q->r, memory_order_acquire) >= AMSG_N) return false;
    AMsg *m = &q->m[w % AMSG_N];
    m->port = port; m->size = n;
    memcpy(m->data, d, n);
    atomic_store_explicit(&q->w, w + 1, memory_order_release);
    return true;
}
static AMsg *aring_peek(ARing *q)
{
    unsigned r = atomic_load_explicit(&q->r, memory_order_relaxed);
    if (r == atomic_load_explicit(&q->w, memory_order_acquire)) return NULL;
    return &q->m[r % AMSG_N];
}
static void aring_drop(ARing *q) { atomic_fetch_add_explicit(&q->r, 1, memory_order_release); }

typedef struct {
    LilvInstance *inst;
    const LilvPlugin *plug;
    int nports;
    int *ain, *aout, nain, naout;                    /* indices of the audio ports */
    float *ctrl;                                     /* values of the control ports */
    bool *ctrl_in;                                   /* control input (true) / output */
    bool *is_ctrl;
    char **sym;                                      /* the port symbols (for the state) */
    uint8_t **atom_in, **atom_out; int nai, nao;
    int *atom_in_idx, *atom_out_idx;                 /* the numbers of the atom ports */
    float **cv;
    LV2_Options_Option opts[5];
    float rate_f; int32_t maxb, minb;
    LV2_Feature f_map, f_unmap, f_opt, f_bbl, f_log;
    const LV2_Feature *feats[6];
    LV2_URID u_seq, u_chunk, u_event_transfer;
    /* interface */
    ARing *to_plug, *to_ui;
    void *ui_lib;
    const LV2UI_Descriptor *ui_desc;
    LV2UI_Handle ui;
    const LV2UI_Idle_Interface *ui_idle;
    PlugWin *win;
    float *ui_last;                                  /* the values last sent to the interface */
    LV2UI_Resize ui_resize;
    LV2_Extension_Data_Feature ext_data;
    /* 1.2 */
    int midi_port;                                   /* index into atom_in[] of the MIDI input, -1 = none */
    LV2_URID u_midi;
    QMidiEv mev[QMIDI_MAX]; int nmev;
    int *par; int npar;                              /* the port numbers of the visible controls */
    float *pmin, *pmax; unsigned char *pkind;        /* 0 = float, 1 = integer, 2 = toggle */
    char **pname;
} LV;

static void seq_clear(LV *v, uint8_t *b)
{
    LV2_Atom_Sequence *sq = (LV2_Atom_Sequence *)b;
    sq->atom.size = sizeof(LV2_Atom_Sequence_Body);
    sq->atom.type = v->u_seq;
    sq->body.unit = 0; sq->body.pad = 0;
}

static void lv_process(void *h, float **in, float **out, unsigned n)
{
    LV *v = h;
    for (int i = 0; i < v->nain; ++i) lilv_instance_connect_port(v->inst, (uint32_t)v->ain[i], in[i]);
    for (int i = 0; i < v->naout; ++i) lilv_instance_connect_port(v->inst, (uint32_t)v->aout[i], out[i]);
    for (int i = 0; i < v->nao; ++i) {                   /* free space for the output events */
        LV2_Atom *a = (LV2_Atom *)v->atom_out[i];
        a->size = ATOM_CAP - sizeof(LV2_Atom);
        a->type = v->u_chunk;
    }
    /* messages from the interface -> into the plugin's atom input (time 0) */
    AMsg *m;
    while ((m = aring_peek(v->to_plug))) {
        for (int i = 0; i < v->nai; ++i) {
            if ((uint32_t)v->atom_in_idx[i] != m->port) continue;
            LV2_Atom_Sequence *sq = (LV2_Atom_Sequence *)v->atom_in[i];
            uint32_t need = (uint32_t)sizeof(LV2_Atom_Event) + ((m->size + 7u) & ~7u);
            if (sizeof(LV2_Atom) + sq->atom.size + need > ATOM_CAP) break;
            LV2_Atom_Event *ev = (LV2_Atom_Event *)((uint8_t *)&sq->body + sq->atom.size);
            ev->time.frames = 0;
            memcpy(&ev->body, m->data, m->size);
            sq->atom.size += need;
        }
        aring_drop(v->to_plug);
    }
    /* 1.2: MIDI from the keyboard (after the interface messages, which are at time 0) */
    if (v->nmev > 0 && v->midi_port >= 0) {
        LV2_Atom_Sequence *sq = (LV2_Atom_Sequence *)v->atom_in[v->midi_port];
        for (int k = 0; k < v->nmev; ++k) {
            uint32_t need = (uint32_t)sizeof(LV2_Atom_Event) + 8u;
            if (sizeof(LV2_Atom) + sq->atom.size + need > ATOM_CAP) break;
            LV2_Atom_Event *ev = (LV2_Atom_Event *)((uint8_t *)&sq->body + sq->atom.size);
            ev->time.frames = v->mev[k].frame < n ? v->mev[k].frame : n - 1;
            unsigned char st = v->mev[k].d[0] & 0xF0;
            ev->body.size = (st == 0xC0 || st == 0xD0) ? 2 : 3;
            ev->body.type = v->u_midi;
            memset(ev + 1, 0, 8);
            memcpy(ev + 1, v->mev[k].d, ev->body.size);
            sq->atom.size += need;
        }
    }
    v->nmev = 0;
    lilv_instance_run(v->inst, n);
    for (int i = 0; i < v->nai; ++i) seq_clear(v, v->atom_in[i]);
    /* events from the plugin -> to the interface (only if it is open) */
    if (v->ui) {
        for (int i = 0; i < v->nao; ++i) {
            LV2_Atom_Sequence *sq = (LV2_Atom_Sequence *)v->atom_out[i];
            if (sq->atom.type != v->u_seq || sq->atom.size <= sizeof(LV2_Atom_Sequence_Body) || sq->atom.size > ATOM_CAP) continue;
            LV2_ATOM_SEQUENCE_FOREACH(sq, ev) {
                aring_push(v->to_ui, (uint32_t)v->atom_out_idx[i], &ev->body, (uint32_t)sizeof(LV2_Atom) + ev->body.size);
            }
        }
    }
}

static void lv_midi(void *h, const QMidiEv *ev, int n)
{
    LV *v = h;
    for (int i = 0; i < n && v->nmev < QMIDI_MAX; ++i) v->mev[v->nmev++] = ev[i];
}

/* 1.2: parameters = the control input ports (main thread; a float write is atomic enough for the audio) */
static int lv_param_count(void *h) { return ((LV *)h)->npar; }
static bool lv_param_name(void *h, int i, char *name, int nsz)
{
    LV *v = h;
    if (i < 0 || i >= v->npar) return false;
    snprintf(name, (size_t)nsz, "%s", v->pname[i]);
    return true;
}
static float lv_param_get(void *h, int i)
{
    LV *v = h;
    if (i < 0 || i >= v->npar) return 0.0f;
    float r = v->pmax[i] - v->pmin[i];
    return r > 0.0f ? (v->ctrl[v->par[i]] - v->pmin[i]) / r : 0.0f;
}
static void lv_param_set(void *h, int i, float x)
{
    LV *v = h;
    if (i < 0 || i >= v->npar) return;
    float val = v->pmin[i] + x * (v->pmax[i] - v->pmin[i]);
    if (v->pkind[i] == 1) val = floorf(val + 0.5f);
    if (v->pkind[i] == 2) val = x >= 0.5f ? v->pmax[i] : v->pmin[i];
    v->ctrl[v->par[i]] = val;
}
static void lv_param_text(void *h, int i, char *out, int sz)
{
    LV *v = h;
    if (i < 0 || i >= v->npar) return;
    float val = v->ctrl[v->par[i]];
    if (v->pkind[i] == 2) snprintf(out, (size_t)sz, "%s", val > v->pmin[i] ? "On" : "Off");
    else if (v->pkind[i] == 1) snprintf(out, (size_t)sz, "%d", (int)lrintf(val));
    else snprintf(out, (size_t)sz, fabsf(val) >= 100.0f ? "%.0f" : fabsf(val) >= 10.0f ? "%.1f" : "%.2f", val);
}

/* ---- interface (ui:X11UI) ---- */
static void ui_write(LV2UI_Controller c, uint32_t port, uint32_t size, uint32_t proto, const void *buf)
{
    LV *v = c;
    if ((int)port >= v->nports) return;
    if (proto == 0) {
        if (size == sizeof(float) && v->is_ctrl[port] && v->ctrl_in[port]) {
            v->ctrl[port] = *(const float *)buf;
            v->ui_last[port] = v->ctrl[port];
            qplug_note_edit();
        }
    } else if (proto == v->u_event_transfer) {
        aring_push(v->to_plug, port, buf, size);
        qplug_note_edit();
    }
}

static int ui_resize_cb(LV2UI_Feature_Handle h, int w, int hh)
{
    LV *v = h;
    if (v->win && w > 0 && hh > 0) plugwin_resize(v->win, w, hh);
    return 0;
}

static const LilvUI *find_x11_ui(LV *v, LilvUIs **uis_out)
{
    LilvUIs *uis = L.plugin_get_uis(v->plug);
    *uis_out = uis;
    if (!uis) return NULL;
    for (LilvIter *it = L.uis_begin(uis); !L.uis_is_end(uis, it); it = L.uis_next(uis, it)) {
        const LilvUI *u = L.uis_get(uis, it);
        if (L.ui_is_a(u, n_x11ui)) return u;
    }
    return NULL;
}

static bool lv_has_editor(void *h)
{
    LV *v = h;
    LilvUIs *uis;
    bool ok = find_x11_ui(v, &uis) != NULL;
    if (uis) L.uis_free(uis);
    return ok;
}

static int lv_ed_open(void *h, PlugWin *w, int *W, int *H, char *err, int esz)
{
    LV *v = h;
    (void)W; (void)H;
    LilvUIs *uis;
    const LilvUI *u = find_x11_ui(v, &uis);
    if (!u) { if (uis) L.uis_free(uis); snprintf(err, (size_t)esz, "this LV2 plugin has no X11 window (Gtk/Qt windows are not supported)"); return -1; }
    L.world_load_resource(g_world, L.ui_get_uri(u));
    char *bin = L.file_uri_parse(L.node_as_uri(L.ui_get_binary_uri(u)), NULL);
    char *bnd = L.file_uri_parse(L.node_as_uri(L.ui_get_bundle_uri(u)), NULL);
    char ui_uri[1024];
    snprintf(ui_uri, sizeof ui_uri, "%s", L.node_as_uri(L.ui_get_uri(u)));
    L.uis_free(uis);
    if (!bin || !bnd) { if (bin) L.lfree(bin); if (bnd) L.lfree(bnd); snprintf(err, (size_t)esz, "bad LV2 window files"); return -1; }
    char bundle[1100];
    snprintf(bundle, sizeof bundle, "%s%s", bnd, bnd[strlen(bnd) - 1] == '/' ? "" : "/");
    void *lib = dlopen(bin, RTLD_NOW | RTLD_LOCAL);
    L.lfree(bin); L.lfree(bnd);
    if (!lib) { snprintf(err, (size_t)esz, "cannot load the window: %s", dlerror()); return -1; }
    LV2UI_DescriptorFunction df = (LV2UI_DescriptorFunction)dlsym(lib, "lv2ui_descriptor");
    const LV2UI_Descriptor *d = NULL;
    for (uint32_t i = 0; df && (d = df(i)); ++i) if (!strcmp(d->URI, ui_uri)) break;
    if (!d) { dlclose(lib); snprintf(err, (size_t)esz, "the window was not found in its library"); return -1; }

    v->win = w;
    v->ui_resize.handle = v;
    v->ui_resize.ui_resize = ui_resize_cb;
    v->ext_data.data_access = lilv_instance_get_descriptor(v->inst)->extension_data;
    LV2_Feature f_parent = { LV2_UI__parent, (void *)(uintptr_t)plugwin_xid(w) };
    LV2_Feature f_resize = { LV2_UI__resize, &v->ui_resize };
    LV2_Feature f_inst = { LV2_INSTANCE_ACCESS_URI, lilv_instance_get_handle(v->inst) };
    LV2_Feature f_data = { LV2_DATA_ACCESS_URI, &v->ext_data };
    LV2_Feature f_idle = { LV2_UI__idleInterface, NULL };
    const LV2_Feature *feats[] = { &v->f_map, &v->f_unmap, &v->f_opt, &v->f_log, &f_parent, &f_resize, &f_inst, &f_data, &f_idle, NULL };
    LV2UI_Widget widget = NULL;
    const char *plug_uri = L.node_as_uri(L.plugin_get_uri(v->plug));
    v->ui_desc = d;
    v->ui_lib = lib;
    v->ui = d->instantiate(d, plug_uri, bundle, ui_write, v, &widget, feats);
    if (!v->ui) { dlclose(lib); v->ui_lib = NULL; v->ui_desc = NULL; v->win = NULL; snprintf(err, (size_t)esz, "the plugin window failed to start"); return -1; }
    v->ui_idle = d->extension_data ? d->extension_data(LV2_UI__idleInterface) : NULL;
    /* the initial values of the knobs */
    for (int i = 0; i < v->nports; ++i) if (v->is_ctrl[i]) {
        float val = v->ctrl[i];
        d->port_event(v->ui, (uint32_t)i, sizeof(float), 0, &val);
        v->ui_last[i] = val;
    }
    return 0;
}

static void lv_ed_idle(void *h)
{
    LV *v = h;
    if (!v->ui) return;
    AMsg *m;
    while ((m = aring_peek(v->to_ui))) {                 /* events from the plugin */
        if (v->ui_desc->port_event) v->ui_desc->port_event(v->ui, m->port, m->size, v->u_event_transfer, m->data);
        aring_drop(v->to_ui);
    }
    for (int i = 0; i < v->nports; ++i) {                /* output controls (meters) */
        if (!v->is_ctrl[i] || v->ctrl_in[i]) continue;
        float val = v->ctrl[i];
        if (val != v->ui_last[i]) { v->ui_last[i] = val; v->ui_desc->port_event(v->ui, (uint32_t)i, sizeof(float), 0, &val); }
    }
    if (v->ui_idle && v->ui_idle->idle(v->ui) != 0) {   /* the interface wants to be closed */
        /* closed by qplug (there is no close_requested for LV2) - here we just leave it */
    }
}

static void lv_ed_close(void *h)
{
    LV *v = h;
    if (!v->ui) return;
    v->ui_desc->cleanup(v->ui);
    v->ui = NULL;
    dlclose(v->ui_lib);
    v->ui_lib = NULL; v->ui_desc = NULL; v->ui_idle = NULL; v->win = NULL;
    AMsg *m;
    while ((m = aring_peek(v->to_ui))) aring_drop(v->to_ui);
}

/* ---- state: "QLV2" + the values of the control inputs (by symbol) + the properties from state:interface ---- */
typedef struct { unsigned char *b; size_t n, cap; } Buf;
static void buf_put(Buf *o, const void *d, size_t n)
{
    if (o->n + n > o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 1024;
        while (nc < o->n + n) nc *= 2;
        unsigned char *nb = realloc(o->b, nc);
        if (!nb) return;
        o->b = nb; o->cap = nc;
    }
    memcpy(o->b + o->n, d, n);
    o->n += n;
}
static void buf_str(Buf *o, const char *s) { uint32_t l = (uint32_t)strlen(s); buf_put(o, &l, 4); buf_put(o, s, l); }

static LV2_State_Status st_store(LV2_State_Handle h, uint32_t key, const void *val, size_t size, uint32_t type, uint32_t flags)
{
    Buf *o = h;
    const char *k = urid_unmap(NULL, key), *t = urid_unmap(NULL, type);
    if (!k || !t) return LV2_STATE_ERR_UNKNOWN;
    buf_str(o, k); buf_str(o, t);
    uint32_t f = flags, sz = (uint32_t)size;
    buf_put(o, &f, 4); buf_put(o, &sz, 4); buf_put(o, val, size);
    return LV2_STATE_SUCCESS;
}

static int lv_get_state(void *h, void **data, size_t *len)
{
    LV *v = h;
    Buf o = { 0 };
    buf_put(&o, "QLV2", 4);
    uint32_t nc = 0;
    for (int i = 0; i < v->nports; ++i) if (v->is_ctrl[i] && v->ctrl_in[i]) ++nc;
    buf_put(&o, &nc, 4);
    for (int i = 0; i < v->nports; ++i) if (v->is_ctrl[i] && v->ctrl_in[i]) { buf_str(&o, v->sym[i]); buf_put(&o, &v->ctrl[i], 4); }
    size_t mark = o.n;
    uint32_t np = 0;
    buf_put(&o, &np, 4);
    const LV2_State_Interface *si = lilv_instance_get_extension_data(v->inst, LV2_STATE__interface);
    if (si && si->save) {
        size_t before = o.n;
        const LV2_Feature *f[] = { &v->f_map, &v->f_unmap, NULL };
        si->save(lilv_instance_get_handle(v->inst), st_store, &o, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, f);
        /* count the entries */
        size_t p = before;
        while (p + 4 <= o.n) {
            uint32_t l; memcpy(&l, o.b + p, 4); p += 4 + l;            /* key */
            memcpy(&l, o.b + p, 4); p += 4 + l;                         /* type */
            uint32_t sz; memcpy(&sz, o.b + p + 4, 4); p += 8 + sz;      /* flags, size, data */
            ++np;
        }
        memcpy(o.b + mark, &np, 4);
    }
    if (!o.b) return -1;
    *data = o.b; *len = o.n;
    return 0;
}

typedef struct { const unsigned char *b; size_t n; } StIn;
typedef struct { uint32_t key, type, flags, size; const void *val; } StProp;
typedef struct { StProp *p; int n; } StProps;
static const void *st_retrieve(LV2_State_Handle h, uint32_t key, size_t *size, uint32_t *type, uint32_t *flags)
{
    StProps *ps = h;
    for (int i = 0; i < ps->n; ++i) if (ps->p[i].key == key) {
        *size = ps->p[i].size; *type = ps->p[i].type; *flags = ps->p[i].flags;
        return ps->p[i].val;
    }
    return NULL;
}

static bool rd(const unsigned char *b, size_t n, size_t *p, void *out, size_t k)
{
    if (*p + k > n) return false;
    memcpy(out, b + *p, k);
    *p += k;
    return true;
}
static bool rd_str(const unsigned char *b, size_t n, size_t *p, char *out, size_t cap)
{
    uint32_t l;
    if (!rd(b, n, p, &l, 4) || *p + l > n) return false;
    size_t c = l < cap - 1 ? l : cap - 1;
    memcpy(out, b + *p, c); out[c] = 0;
    *p += l;
    return true;
}

static int lv_set_state(void *h, const void *data, size_t len)
{
    LV *v = h;
    const unsigned char *b = data;
    size_t p = 4;
    if (len < 8 || memcmp(b, "QLV2", 4)) return -1;
    uint32_t nc;
    if (!rd(b, len, &p, &nc, 4)) return -1;
    for (uint32_t k = 0; k < nc; ++k) {
        char sym[256]; float val;
        if (!rd_str(b, len, &p, sym, sizeof sym) || !rd(b, len, &p, &val, 4)) return -1;
        for (int i = 0; i < v->nports; ++i) if (v->is_ctrl[i] && v->ctrl_in[i] && !strcmp(v->sym[i], sym)) v->ctrl[i] = val;
    }
    uint32_t np;
    if (!rd(b, len, &p, &np, 4) || np == 0) return 0;
    StProps ps = { calloc(np, sizeof(StProp)), 0 };
    if (!ps.p) return -1;
    for (uint32_t k = 0; k < np; ++k) {
        char key[1024], type[1024];
        StProp *q = &ps.p[ps.n];
        if (!rd_str(b, len, &p, key, sizeof key) || !rd_str(b, len, &p, type, sizeof type) ||
            !rd(b, len, &p, &q->flags, 4) || !rd(b, len, &p, &q->size, 4) || p + q->size > len) break;
        q->val = b + p; p += q->size;
        q->key = urid_map(NULL, key); q->type = urid_map(NULL, type);
        ++ps.n;
    }
    const LV2_State_Interface *si = lilv_instance_get_extension_data(v->inst, LV2_STATE__interface);
    if (si && si->restore) {
        const LV2_Feature *f[] = { &v->f_map, &v->f_unmap, NULL };
        si->restore(lilv_instance_get_handle(v->inst), st_retrieve, &ps, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, f);
    }
    free(ps.p);
    return 0;
}

static void lv_destroy(void *h)
{
    LV *v = h;
    lv_ed_close(v);
    lilv_instance_deactivate(v->inst);
    L.instance_free(v->inst);
    for (int i = 0; i < v->nai; ++i) free(v->atom_in[i]);
    for (int i = 0; i < v->nao; ++i) free(v->atom_out[i]);
    for (int i = 0; i < v->nports; ++i) if (v->cv && v->cv[i]) free(v->cv[i]);
    free(v->atom_in); free(v->atom_out); free(v->atom_out_idx); free(v->atom_in_idx); free(v->cv);
    for (int i = 0; i < v->npar; ++i) free(v->pname[i]);
    free(v->par); free(v->pmin); free(v->pmax); free(v->pkind); free(v->pname);
    for (int i = 0; i < v->nports; ++i) free(v->sym ? v->sym[i] : NULL);
    free(v->sym); free(v->ctrl_in); free(v->is_ctrl); free(v->ui_last);
    free(v->to_plug); free(v->to_ui);
    free(v->ain); free(v->aout); free(v->ctrl);
    free(v);
}

static bool unsupported_feature(const LilvPlugin *p, char *err, int esz)
{
    LilvNodes *req = L.plugin_get_required_features(p);
    bool bad = false;
    if (req) {
        for (LilvIter *it = L.nodes_begin(req); !L.nodes_is_end(req, it); it = L.nodes_next(req, it)) {
            const char *u = L.node_as_uri(L.nodes_get(req, it));
            bool ok = false;
            for (int k = 0; k_supported[k]; ++k) if (!strcmp(k_supported[k], u)) ok = true;
            if (!ok) { snprintf(err, (size_t)esz, "needs an LV2 feature Q-LiM does not have yet: %s", u); bad = true; break; }
        }
        L.nodes_free(req);
    }
    return bad;
}

int lv2_open(const char *id, double rate, int maxb, QInst *out, char *err, int esz)
{
    if (lv_load(err, esz) < 0) return -1;
    char bundle[1024];
    snprintf(bundle, sizeof bundle, "%s", id);
    char *bar = strrchr(bundle, '|');
    if (!bar) { snprintf(err, (size_t)esz, "bad LV2 id"); return -1; }
    *bar = 0;
    const char *uri = bar + 1;
    load_bundle(bundle);
    LilvNode *un = L.new_uri(g_world, uri);
    const LilvPlugin *p = L.plugins_get_by_uri(L.world_get_all_plugins(g_world), un);
    L.node_free(un);
    if (!p) { snprintf(err, (size_t)esz, "plugin not found (%s)", uri); return -1; }
    if (unsupported_feature(p, err, esz)) return -1;

    LV *v = calloc(1, sizeof *v);
    v->rate_f = (float)rate; v->maxb = maxb; v->minb = 1;
    v->u_seq = urid_map(NULL, LV2_ATOM__Sequence);
    v->u_chunk = urid_map(NULL, LV2_ATOM__Chunk);
    v->u_event_transfer = urid_map(NULL, LV2_ATOM__eventTransfer);
    v->plug = p;
    LV2_URID u_float = urid_map(NULL, LV2_ATOM__Float), u_int = urid_map(NULL, LV2_ATOM__Int);
    v->opts[0] = (LV2_Options_Option){ LV2_OPTIONS_INSTANCE, 0, urid_map(NULL, LV2_PARAMETERS__sampleRate), sizeof(float), u_float, &v->rate_f };
    v->opts[1] = (LV2_Options_Option){ LV2_OPTIONS_INSTANCE, 0, urid_map(NULL, LV2_BUF_SIZE__maxBlockLength), sizeof(int32_t), u_int, &v->maxb };
    v->opts[2] = (LV2_Options_Option){ LV2_OPTIONS_INSTANCE, 0, urid_map(NULL, LV2_BUF_SIZE__minBlockLength), sizeof(int32_t), u_int, &v->minb };
    v->opts[3] = (LV2_Options_Option){ LV2_OPTIONS_INSTANCE, 0, urid_map(NULL, LV2_BUF_SIZE__nominalBlockLength), sizeof(int32_t), u_int, &v->maxb };
    memset(&v->opts[4], 0, sizeof v->opts[4]);
    v->f_map = (LV2_Feature){ LV2_URID__map, &g_map };
    v->f_unmap = (LV2_Feature){ LV2_URID__unmap, &g_unmap };
    v->f_opt = (LV2_Feature){ LV2_OPTIONS__options, v->opts };
    v->f_bbl = (LV2_Feature){ LV2_BUF_SIZE__boundedBlockLength, NULL };
    v->f_log = (LV2_Feature){ LV2_LOG__log, &g_log };
    v->feats[0] = &v->f_map; v->feats[1] = &v->f_unmap; v->feats[2] = &v->f_opt;
    v->feats[3] = &v->f_bbl; v->feats[4] = &v->f_log; v->feats[5] = NULL;

    v->inst = L.plugin_instantiate(p, rate, v->feats);
    if (!v->inst) { snprintf(err, (size_t)esz, "plugin failed to start (instantiate)"); free(v); return -1; }
    int np = (int)L.plugin_get_num_ports(p);
    v->nports = np;
    float *mins = calloc((size_t)np, sizeof(float)), *maxs = calloc((size_t)np, sizeof(float)), *defs = calloc((size_t)np, sizeof(float));
    L.plugin_get_port_ranges_float(p, mins, maxs, defs);
    v->ctrl = calloc((size_t)np, sizeof(float));
    v->ain = calloc((size_t)np, sizeof(int)); v->aout = calloc((size_t)np, sizeof(int));
    v->atom_in = calloc((size_t)np, sizeof(uint8_t *)); v->atom_out = calloc((size_t)np, sizeof(uint8_t *));
    v->atom_out_idx = calloc((size_t)np, sizeof(int));
    v->atom_in_idx = calloc((size_t)np, sizeof(int));
    v->cv = calloc((size_t)np, sizeof(float *));
    v->ctrl_in = calloc((size_t)np, sizeof(bool));
    v->is_ctrl = calloc((size_t)np, sizeof(bool));
    v->ui_last = calloc((size_t)np, sizeof(float));
    v->sym = calloc((size_t)np, sizeof(char *));
    v->to_plug = calloc(1, sizeof(ARing));
    v->to_ui = calloc(1, sizeof(ARing));
    v->midi_port = -1;
    v->u_midi = urid_map(NULL, "http://lv2plug.in/ns/ext/midi#MidiEvent");
    v->par = calloc((size_t)np + 1, sizeof(int)); v->pmin = calloc((size_t)np + 1, sizeof(float));
    v->pmax = calloc((size_t)np + 1, sizeof(float)); v->pkind = calloc((size_t)np + 1, 1);
    v->pname = calloc((size_t)np + 1, sizeof(char *));
    for (int i = 0; i < np; ++i) {
        const LilvPort *pt = L.plugin_get_port_by_index(p, (uint32_t)i);
        bool in = L.port_is_a(p, pt, n_input);
        const LilvNode *sn = L.port_get_symbol(p, pt);
        v->sym[i] = strdup(sn ? L.node_as_string(sn) : "");
        if (L.port_is_a(p, pt, n_audio)) {
            if (in) v->ain[v->nain++] = i; else v->aout[v->naout++] = i;
        } else if (L.port_is_a(p, pt, n_control)) {
            v->is_ctrl[i] = true;
            v->ctrl_in[i] = in;
            float d = defs[i];
            if (isnan(d)) d = isnan(mins[i]) ? 0.0f : mins[i];
            v->ctrl[i] = d;
            lilv_instance_connect_port(v->inst, (uint32_t)i, &v->ctrl[i]);
            if (in && !L.port_has_property(p, pt, n_not_gui) && !isnan(mins[i]) && !isnan(maxs[i]) && maxs[i] > mins[i]) {
                int k = v->npar++;
                v->par[k] = i; v->pmin[k] = mins[i]; v->pmax[k] = maxs[i];
                v->pkind[k] = L.port_has_property(p, pt, n_toggled) ? 2
                            : (L.port_has_property(p, pt, n_integer) || L.port_has_property(p, pt, n_enum)) ? 1 : 0;
                LilvNode *pn = L.port_get_name(p, pt);
                v->pname[k] = strdup(pn ? L.node_as_string(pn) : v->sym[i]);
                if (pn) L.node_free(pn);
            }
        } else if (L.port_is_a(p, pt, n_atom)) {
            uint8_t *b = calloc(1, ATOM_CAP);
            if (in) {
                seq_clear(v, b);
                if (v->midi_port < 0 && L.port_supports_event(p, pt, n_midi_ev)) v->midi_port = v->nai;
                v->atom_in_idx[v->nai] = i;
                v->atom_in[v->nai++] = b;
            } else {
                v->atom_out_idx[v->nao] = i;
                v->atom_out[v->nao++] = b;
            }
            lilv_instance_connect_port(v->inst, (uint32_t)i, b);
        } else {                                                     /* CV or something else: empty buffer */
            v->cv[i] = calloc((size_t)maxb, sizeof(float));
            lilv_instance_connect_port(v->inst, (uint32_t)i, v->cv[i]);
        }
    }
    free(mins); free(maxs); free(defs);
    lilv_instance_activate(v->inst);
    out->h = v; out->nin = v->nain; out->nout = v->naout;
    out->process = lv_process; out->destroy = lv_destroy;
    out->has_editor = lv_has_editor; out->ed_open = lv_ed_open; out->ed_close = lv_ed_close; out->ed_idle = lv_ed_idle;
    out->get_state = lv_get_state; out->set_state = lv_set_state;
    if (v->midi_port >= 0) out->midi = lv_midi;
    out->param_count = lv_param_count; out->param_name = lv_param_name; out->param_get = lv_param_get;
    out->param_set = lv_param_set; out->param_text = lv_param_text;
    return 0;
}

int lv2_scan(const char *bundle)
{
    char err[256];
    const char *base = strrchr(bundle, '/');
    base = base ? base + 1 : bundle;
    if (lv_load(err, sizeof err) < 0) { plug_print_line("ERR", "LV2", base, bundle, err); return 0; }
    load_bundle(bundle);
    const LilvPlugins *all = L.world_get_all_plugins(g_world);
    int n = 0;
    for (LilvIter *it = L.plugins_begin(all); !L.plugins_is_end(all, it); it = L.plugins_next(all, it)) {
        const LilvPlugin *p = L.plugins_get(all, it);
        LilvNode *nm = L.plugin_get_name(p);
        int ain = 0, aout = 0, atom_in = 0;
        uint32_t np = L.plugin_get_num_ports(p);
        for (uint32_t i = 0; i < np; ++i) {
            const LilvPort *pt = L.plugin_get_port_by_index(p, i);
            bool in = L.port_is_a(p, pt, n_input);
            if (L.port_is_a(p, pt, n_audio)) { if (in) ++ain; else ++aout; }
            else if (L.port_is_a(p, pt, n_atom) && in) ++atom_in;
        }
        const LilvNode *cls = L.plugin_class_get_uri(L.plugin_get_class(p));
        bool inst = (cls && L.node_equals(cls, n_instr)) || (ain == 0 && atom_in > 0);
        char id[1200], extra[64], ferr[256];
        snprintf(id, sizeof id, "%s|%s", bundle, L.node_as_uri(L.plugin_get_uri(p)));
        snprintf(extra, sizeof extra, "%s\t%d\t%d", inst ? "inst" : "fx", ain, aout);
        const char *name = nm ? L.node_as_string(nm) : base;
        if (unsupported_feature(p, ferr, sizeof ferr)) plug_print_line("ERR", "LV2", name, id, ferr);
        else plug_print_line("PLUG", "LV2", name, id, extra);
        if (nm) L.node_free(nm);
        ++n;
    }
    if (n == 0) plug_print_line("ERR", "LV2", base, bundle, "no plugins in this bundle");
    return 0;
}
