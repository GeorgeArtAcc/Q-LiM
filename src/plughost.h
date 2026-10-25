/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef PLUGHOST_H
#define PLUGHOST_H
/*
 * Loading and running plugins (step 34): VST3, VST2 (Linux .so), LV2 (through liblilv), CLAP.
 * Every plugin is seen as a stereo effect: qplug_process() processes L/R in place.
 * Mono plugins (1 input / 1 output) are loaded twice (left and right).
 *
 * Plugin identifier ("id"):  VST2 = the path to the .so;  VST3 = "<.vst3 folder>|<class id>";
 *                            LV2  = "<.lv2 folder>|<URI>"; CLAP = "<.clap file>|<plugin id>".
 */
#include <stdbool.h>
#include <stddef.h>
#include "plugwin.h"

typedef struct QPlug QPlug;

/* Loads and activates the plugin (main thread). NULL + err on error. */
QPlug *qplug_load(const char *fmt, const char *id, double rate, int max_block, char *err, int err_size);
/* The RT thread: n <= max_block frames, in place. */
void   qplug_process(QPlug *p, float *l, float *r, unsigned n);
/* Main thread (after the RT thread no longer uses it). Also closes its window. */
void   qplug_free(QPlug *p);

/* --- plugin window (step 35), main thread only --- */
bool   qplug_has_editor(QPlug *p);
int    qplug_editor_open(QPlug *p, const char *title, char *err, int esz);   /* already open -> comes to the front */
void   qplug_editor_close(QPlug *p);
bool   qplug_editor_is_open(QPlug *p);
void   qplug_idle_all(void);          /* every frame: window events, timers, closing with X */

/* step 35d: plugin through yabridge (Wine) - loads slowly, so on a background thread (see fxchain.c) */
bool   qplug_is_bridged(const char *fmt, const char *id);

/* counter of changes made in plugin windows (for "there are unsaved changes"); called from any thread */
void     qplug_note_edit(void);
unsigned qplug_edit_count(void);

/* --- state (the plugin's settings) for saving into the project, main thread only --- */
int    qplug_get_state(QPlug *p, void **data, size_t *len);    /* malloc; 0 = success */
int    qplug_set_state(QPlug *p, const void *data, size_t len);

/* "Scan" mode (a separate process: qlim --plugin-scan <format> <path>): writes lines to stdout
 *   PLUG<TAB>format<TAB>name<TAB>id<TAB>fx|inst<TAB>inputs<TAB>outputs
 *   ERR<TAB>format<TAB>name/path<TAB>reason
 * Returns 0. */
int    qplug_scan_child(const char *fmt, const char *path);

/* --- internal: one plugin instance in a given format --- */
typedef struct {
    void *h;
    int   nin, nout;                                   /* audio channels of the main input/output */
    void (*process)(void *h, float **in, float **out, unsigned n);
    void (*destroy)(void *h);
    /* window (NULL = none) */
    bool (*has_editor)(void *h);
    int  (*ed_open)(void *h, PlugWin *w, int *width, int *height, char *err, int esz);
    void (*ed_close)(void *h);
    void (*ed_idle)(void *h);                          /* every frame while the window is open */
    void (*main_idle)(void *h);                        /* every frame, always (CLAP on_main_thread etc.) */
    /* state */
    int  (*get_state)(void *h, void **data, size_t *len);
    int  (*set_state)(void *h, const void *data, size_t len);
} QInst;

int vst2_open(const char *id, double rate, int maxb, QInst *out, char *err, int esz);
int vst2_scan(const char *path);
int clap_open(const char *id, double rate, int maxb, QInst *out, char *err, int esz);
int clap_scan(const char *path);
void clap_host_set_main_thread(void);       /* called from the main thread (qplug_idle_all) */
int lv2_open(const char *id, double rate, int maxb, QInst *out, char *err, int esz);
int lv2_scan(const char *bundle);
#ifdef __cplusplus
extern "C" {
#endif
int vst3_open(const char *id, double rate, int maxb, QInst *out, char *err, int esz);
int vst3_scan(const char *bundle);
int vst3_binary(const char *id, char *out, size_t sz);      /* the path of the .so file inside the bundle (0 = success) */
#ifdef __cplusplus
}
#endif

/* common test: loads and processes one block of silence (in the scan process) */
bool qplug_selftest(const char *fmt, const char *id, char *err, int esz);
void plug_print_line(const char *kind, const char *fmt, const char *name, const char *id, const char *extra);

#endif
