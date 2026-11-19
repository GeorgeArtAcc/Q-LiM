/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 2.0.0: a tiny CLAP test plugin for the separate-process tests (tests/test_bridge.c).
 *   output L = gain * input R, output R = gain * input L   (gain = parameter 0, default 0.5)
 *   a block whose first input sample is exactly 0.875  -> the plugin CRASHES (segfault)
 *   a block whose first input sample is exactly 0.625  -> the plugin FREEZES (sleeps for ever)
 *   state = the gain as text
 *   MIDI (note port, raw MIDI): from the frame of a note-on on, 0.25 is ADDED to both outputs (this block)
 *   Windows: a small window (CLAP gui, "win32") with the text "QTestBridge window" - for the window tests */
#include <clap/clap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { clap_plugin_t pl; double gain; } P;

static bool p_init(const clap_plugin_t *pl) { (void)pl; return true; }
static void p_destroy(const clap_plugin_t *pl) { free(pl->plugin_data); }
static bool p_activate(const clap_plugin_t *pl, double sr, uint32_t a, uint32_t b) { (void)pl; (void)sr; (void)a; (void)b; return true; }
static void p_deactivate(const clap_plugin_t *pl) { (void)pl; }
static bool p_start(const clap_plugin_t *pl) { (void)pl; return true; }
static void p_stop(const clap_plugin_t *pl) { (void)pl; }
static void p_reset(const clap_plugin_t *pl) { (void)pl; }
static void p_main(const clap_plugin_t *pl) { (void)pl; }

static clap_process_status p_process(const clap_plugin_t *pl, const clap_process_t *pr)
{
    P *p = pl->plugin_data;
    const clap_input_events_t *ev = pr->in_events;
    int note_at = -1;
    for (uint32_t i = 0; ev && i < ev->size(ev); ++i) {
        const clap_event_header_t *h = ev->get(ev, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
            p->gain = ((const clap_event_param_value_t *)h)->value;
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_MIDI) {
            const clap_event_midi_t *m = (const clap_event_midi_t *)h;
            if ((m->data[0] & 0xF0) == 0x90 && m->data[2] > 0 && note_at < 0) note_at = (int)h->time;
        }
    }
    float **in = pr->audio_inputs[0].data32, **out = pr->audio_outputs[0].data32;
    if (pr->frames_count > 0 && in[0][0] == 0.875f) { volatile int *z = NULL; *z = 1; }        /* crash */
    if (pr->frames_count > 0 && in[0][0] == 0.625f) for (;;) sleep(1);                          /* freeze */
    for (uint32_t i = 0; i < pr->frames_count; ++i) {
        float l = in[0][i], r = in[1][i];
        float add = note_at >= 0 && (int)i >= note_at ? 0.25f : 0.0f;
        out[0][i] = (float)(p->gain * r) + add;
        out[1][i] = (float)(p->gain * l) + add;
    }
    return CLAP_PROCESS_CONTINUE;
}

static uint32_t ap_count(const clap_plugin_t *pl, bool is_in) { (void)pl; (void)is_in; return 1; }
static bool ap_get(const clap_plugin_t *pl, uint32_t i, bool is_in, clap_audio_port_info_t *o)
{
    (void)pl; if (i) return false;
    memset(o, 0, sizeof *o);
    o->id = is_in ? 0 : 1; snprintf(o->name, sizeof o->name, "main"); o->flags = CLAP_AUDIO_PORT_IS_MAIN;
    o->channel_count = 2; o->port_type = CLAP_PORT_STEREO; o->in_place_pair = CLAP_INVALID_ID;
    return true;
}
static const clap_plugin_audio_ports_t g_ap = { ap_count, ap_get };

static uint32_t pa_count(const clap_plugin_t *pl) { (void)pl; return 1; }
static bool pa_info(const clap_plugin_t *pl, uint32_t i, clap_param_info_t *o)
{
    (void)pl; if (i) return false;
    memset(o, 0, sizeof *o);
    o->id = 0; o->flags = CLAP_PARAM_IS_AUTOMATABLE; snprintf(o->name, sizeof o->name, "Gain");
    o->min_value = 0; o->max_value = 1; o->default_value = 0.5;
    return true;
}
static bool pa_value(const clap_plugin_t *pl, clap_id id, double *v) { (void)id; *v = ((P *)pl->plugin_data)->gain; return true; }
static bool pa_v2t(const clap_plugin_t *pl, clap_id id, double v, char *o, uint32_t n) { (void)pl; (void)id; snprintf(o, n, "gain %.2f", v); return true; }
static bool pa_t2v(const clap_plugin_t *pl, clap_id id, const char *t, double *v) { (void)pl; (void)id; *v = atof(t); return true; }
static void pa_flush(const clap_plugin_t *pl, const clap_input_events_t *in, const clap_output_events_t *out)
{
    (void)out; P *p = pl->plugin_data;
    for (uint32_t i = 0; in && i < in->size(in); ++i) {
        const clap_event_header_t *h = in->get(in, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) p->gain = ((const clap_event_param_value_t *)h)->value;
    }
}
static const clap_plugin_params_t g_pa = { pa_count, pa_info, pa_value, pa_v2t, pa_t2v, pa_flush };

static bool st_save(const clap_plugin_t *pl, const clap_ostream_t *s)
{
    char b[64]; int n = snprintf(b, sizeof b, "%.9f", ((P *)pl->plugin_data)->gain);
    return s->write(s, b, (uint64_t)n) == n;
}
static bool st_load(const clap_plugin_t *pl, const clap_istream_t *s)
{
    char b[64] = ""; int64_t n = s->read(s, b, sizeof b - 1);
    if (n <= 0) return false;
    b[n] = 0; ((P *)pl->plugin_data)->gain = atof(b);
    return true;
}
static const clap_plugin_state_t g_st = { st_save, st_load };

static uint32_t np_count(const clap_plugin_t *pl, bool is_in) { (void)pl; return is_in ? 1 : 0; }
static bool np_get(const clap_plugin_t *pl, uint32_t i, bool is_in, clap_note_port_info_t *o)
{
    (void)pl; if (i || !is_in) return false;
    memset(o, 0, sizeof *o);
    o->id = 0; o->supported_dialects = CLAP_NOTE_DIALECT_MIDI; o->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
    snprintf(o->name, sizeof o->name, "midi");
    return true;
}
static const clap_plugin_note_ports_t g_np = { np_count, np_get };

#ifdef _WIN32
#include <windows.h>
static HWND g_gui;
static bool gu_api(const clap_plugin_t *pl, const char *api, bool fl) { (void)pl; return !fl && !strcmp(api, CLAP_WINDOW_API_WIN32); }
static bool gu_pref(const clap_plugin_t *pl, const char **api, bool *fl) { (void)pl; *api = CLAP_WINDOW_API_WIN32; *fl = false; return true; }
static bool gu_create(const clap_plugin_t *pl, const char *api, bool fl) { (void)pl; (void)api; return !fl; }
static void gu_destroy(const clap_plugin_t *pl) { (void)pl; if (g_gui) DestroyWindow(g_gui); g_gui = NULL; }
static bool gu_scale(const clap_plugin_t *pl, double s) { (void)pl; (void)s; return false; }
static bool gu_size(const clap_plugin_t *pl, uint32_t *w, uint32_t *h) { (void)pl; *w = 320; *h = 120; return true; }
static bool gu_canres(const clap_plugin_t *pl) { (void)pl; return false; }
static bool gu_hints(const clap_plugin_t *pl, clap_gui_resize_hints_t *h) { (void)pl; (void)h; return false; }
static bool gu_adjust(const clap_plugin_t *pl, uint32_t *w, uint32_t *h) { (void)pl; (void)w; (void)h; return false; }
static bool gu_setsize(const clap_plugin_t *pl, uint32_t w, uint32_t h) { (void)pl; (void)w; (void)h; return false; }
static bool gu_parent(const clap_plugin_t *pl, const clap_window_t *win)
{
    (void)pl;
    g_gui = CreateWindowExW(0, L"STATIC", L"QTestBridge window", WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 0, 320, 120,
                            (HWND)win->win32, NULL, GetModuleHandleW(NULL), NULL);
    return g_gui != NULL;
}
static bool gu_trans(const clap_plugin_t *pl, const clap_window_t *win) { (void)pl; (void)win; return false; }
static void gu_title(const clap_plugin_t *pl, const char *t) { (void)pl; (void)t; }
static bool gu_show(const clap_plugin_t *pl) { (void)pl; return true; }
static bool gu_hide(const clap_plugin_t *pl) { (void)pl; return true; }
static const clap_plugin_gui_t g_gu = { gu_api, gu_pref, gu_create, gu_destroy, gu_scale, gu_size, gu_canres, gu_hints,
                                        gu_adjust, gu_setsize, gu_parent, gu_trans, gu_title, gu_show, gu_hide };
#endif

static const void *p_ext(const clap_plugin_t *pl, const char *id)
{
    (void)pl;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &g_ap;
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &g_pa;
    if (!strcmp(id, CLAP_EXT_STATE)) return &g_st;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &g_np;
#ifdef _WIN32
    if (!strcmp(id, CLAP_EXT_GUI)) return &g_gu;
#endif
    return NULL;
}

static const char *g_feat[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL };
static const clap_plugin_descriptor_t g_desc = { CLAP_VERSION_INIT, "qlim.test.bridge", "QTestBridge", "Q-LiM", "", "", "", "1.0", "", g_feat };

static const clap_plugin_t *f_create(const clap_plugin_factory_t *f, const clap_host_t *h, const char *id)
{
    (void)f; (void)h;
    if (strcmp(id, g_desc.id)) return NULL;
    P *p = calloc(1, sizeof *p);
    p->gain = 0.5;
    p->pl = (clap_plugin_t){ &g_desc, p, p_init, p_destroy, p_activate, p_deactivate, p_start, p_stop, p_reset, p_process, p_ext, p_main };
    return &p->pl;
}
static uint32_t f_count(const clap_plugin_factory_t *f) { (void)f; return 1; }
static const clap_plugin_descriptor_t *f_desc(const clap_plugin_factory_t *f, uint32_t i) { (void)f; return i ? NULL : &g_desc; }
static const clap_plugin_factory_t g_fac = { f_count, f_desc, f_create };

static bool e_init(const char *path) { (void)path; return true; }
static void e_deinit(void) {}
static const void *e_factory(const char *id) { return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &g_fac : NULL; }
CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, e_init, e_deinit, e_factory };
