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

/* 1.2: MIDI for instruments - a raw MIDI message (note on/off, CC, pitch bend...) at a frame of the block */
/* 2.0.0: plugins with many inputs / outputs (mixers, crossovers, multi-out instruments): Q-LiM's stereo goes to the
 * first two inputs (the others get silence) and comes from the first two outputs (the main ones).
 * QPLUG_MAXCH is only a sanity limit (a broken plugin that reports millions of ports); buffers are made for the real count. */
#define QPLUG_MAXCH 4096

typedef struct { unsigned frame; unsigned char d[3]; } QMidiEv;
#define QMIDI_MAX 256

/* Loads and activates the plugin (main thread). NULL + err on error. */
QPlug *qplug_load(const char *fmt, const char *id, double rate, int max_block, char *err, int err_size);
/* The RT thread: n <= max_block frames, in place. An instrument without audio inputs ADDS its sound to l/r. */
void   qplug_process(QPlug *p, float *l, float *r, unsigned n);
/* 1.2, RT thread: MIDI for the next qplug_process (the events of one block, sorted by frame) */
void   qplug_midi(QPlug *p, const QMidiEv *ev, int n);
bool   qplug_wants_midi(QPlug *p);         /* the format can take MIDI (instruments; also some effects) */
bool   qplug_is_instrument(QPlug *p);      /* no audio input (a synth): its sound is added */

/* 1.2: parameters (for the device panel, like Bitwig). Main thread. Values are normalized 0..1. */
int    qplug_param_count(QPlug *p);
bool   qplug_param_name(QPlug *p, int i, char *name, int nsz);
float  qplug_param_get(QPlug *p, int i);
void   qplug_param_set(QPlug *p, int i, float v);           /* a mono plugin run twice: both instances */
void   qplug_param_text(QPlug *p, int i, char *out, int sz);  /* the current value as the plugin shows it */
/* Main thread (after the RT thread no longer uses it). Also closes its window. */
void   qplug_free(QPlug *p);

/* --- plugin window (step 35), main thread only --- */
bool   qplug_has_editor(QPlug *p);
int    qplug_editor_open(QPlug *p, const char *title, char *err, int esz);   /* already open -> comes to the front */
void   qplug_editor_close(QPlug *p);
bool   qplug_editor_is_open(QPlug *p);
int    qplug_editors_open(void);          /* 1.3.4: how many plugin windows are open (main thread) */
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
int qplug_scan_child(const char *fmt, const char *path, int skip);

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
    /* 1.2: MIDI (RT thread; the events are used by the next process call) - NULL = takes no MIDI */
    void (*midi)(void *h, const QMidiEv *ev, int n);
    /* 1.2: parameters (main thread; normalized 0..1). index = 0..param_count-1 of the VISIBLE parameters */
    int  (*param_count)(void *h);
    bool (*param_name)(void *h, int i, char *name, int nsz);
    float (*param_get)(void *h, int i);
    void (*param_set)(void *h, int i, float v);
    void (*param_text)(void *h, int i, char *out, int sz);
    bool separate;                                     /* 2.0.0: it runs in a separate process (plugbridge.c) */
} QInst;

/* ---- 2.0.0: plugins in a separate process ("sandbox") ---- */
int  bridge_open(const char *fmt, const char *id, double rate, int maxb, QInst *out, char *err, int esz);
int  qplug_host_child_main(int argc, char **argv);   /* qlim --plugin-host <socket fd> <memory fd> */
int  qplug_open_inst_here(const char *fmt, const char *id, double rate, int maxb, QInst *q, char *err, int esz);
void qplug_mark_child(void);                         /* this process IS the separate one */
bool qplug_is_child(void);
bool qplug_is_separate(QPlug *p);
/* the choice: 0 = never, 1 = automatic (plugins that crashed or froze Q-LiM before, LV2 with Gtk windows), 2 = all */
enum { QSEP_NEVER = 0, QSEP_AUTO = 1, QSEP_ALL = 2 };
void qplug_separate_init(const char *list_path);      /* ~/.config/qlim/plugins-separate.txt (main, at the start) */
int  qplug_separate_mode(void);
unsigned qplug_separate_gen(void);                   /* changes when the choice or the list changes */
void qplug_set_separate_mode(int mode);
bool qplug_wants_separate(const char *fmt, const char *id);
bool qplug_separate_listed(const char *fmt, const char *id);
void qplug_separate_set(const char *fmt, const char *id, bool on, const char *why);
int  qplug_separate_count(void);
bool qplug_separate_get(int i, char *fmt, int fsz, char *id, int isz, char *why, int wsz);
/* which plugin was running when Q-LiM crashed (signal handler) / froze (the watchdog thread): it goes on the list */
void qplug_crash_note(void);
void qplug_freeze_note(bool audio);
void qplug_crash_note_module(const char *module_file);   /* Windows crash handler: the DLL where it happened */
/* a message for the screen (the program sets it: ui_flash_message) */
void qplug_set_report_hook(void (*fn)(const char *msg));
void qplug_report(const char *msg);
/* LV2: the plugin's window needs Gtk (no X11 / external one) - it can be opened only in a separate process */
bool lv2_ui_needs_separate(const char *id);

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
