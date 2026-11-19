/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 2.0.0: a tiny LV2 test plugin whose ONLY window is Gtk 3 (like Calf's are Gtk) - for the separate-process tests.
 * DSP: stereo, out = in * gain (control port 4). UI: a Gtk label "QTestGtk window" (Gtk is found at run time). */
#include <lv2/core/lv2.h>
#include <lv2/ui/ui.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const float *in[2]; float *out[2]; const float *gain; } G;
static LV2_Handle inst(const LV2_Descriptor *d, double r, const char *b, const LV2_Feature *const *f) { (void)d; (void)r; (void)b; (void)f; return calloc(1, sizeof(G)); }
static void conn(LV2_Handle h, uint32_t p, void *d)
{
    G *g = h;
    if (p < 2) g->in[p] = d; else if (p < 4) g->out[p - 2] = d; else g->gain = d;
}
static void run(LV2_Handle h, uint32_t n)
{
    G *g = h;
    float k = g->gain ? *g->gain : 1.0f;
    for (int c = 0; c < 2; ++c) for (uint32_t i = 0; i < n; ++i) g->out[c][i] = g->in[c][i] * k;
}
static void cleanup(LV2_Handle h) { free(h); }
static const LV2_Descriptor g_d = { "urn:qlim:test:gtk", inst, conn, NULL, run, NULL, cleanup, NULL };
LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t i) { return i ? NULL : &g_d; }

/* ---- the Gtk 3 window ---- */
typedef struct { void *label; } U;
static LV2UI_Handle ui_inst(const LV2UI_Descriptor *d, const char *uri, const char *bundle, LV2UI_Write_Function w,
                            LV2UI_Controller c, LV2UI_Widget *widget, const LV2_Feature *const *f)
{
    (void)d; (void)uri; (void)bundle; (void)w; (void)c; (void)f;
    void *(*label_new)(const char *) = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "gtk_label_new");
    if (!label_new) return NULL;                       /* the host must have Gtk 3 loaded */
    U *u = calloc(1, sizeof *u);
    u->label = label_new("QTestGtk window");
    *widget = u->label;
    return u;
}
static void ui_cleanup(LV2UI_Handle h) { free(h); }
static void ui_port(LV2UI_Handle h, uint32_t p, uint32_t s, uint32_t f, const void *b) { (void)h; (void)p; (void)s; (void)f; (void)b; }
static const LV2UI_Descriptor g_u = { "urn:qlim:test:gtk#ui", ui_inst, ui_cleanup, ui_port, NULL };
LV2_SYMBOL_EXPORT const LV2UI_Descriptor *lv2ui_descriptor(uint32_t i) { return i ? NULL : &g_u; }
