/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef PLUGINS_H
#define PLUGINS_H
/*
 * List of the installed plugins (step 34).
 * Every file/bundle is checked in a SEPARATE process (qlim --plugin-scan ...): if the plugin crashes
 * or "hangs", Q-LiM does not go down, the plugin goes into the "Problems" list. The results are kept in
 * ~/.config/qlim/plugin-cache.txt (only a file that has changed is checked again).
 * The search locations are in ~/.config/qlim/plugins.conf (Edit -> Plugins...).
 * Order: VST3, VST2, LV2, CLAP, then by name.
 */
#include <stdbool.h>

#define PLUGIN_NAME_LEN 96
#define PLUGIN_PATH_LEN 512

enum { PF_VST3, PF_VST2, PF_LV2, PF_CLAP, PF_COUNT };

typedef struct {
    char fmt[8];                     /* "VST3", "VST" (= VST2), "LV2", "CLAP" */
    char name[PLUGIN_NAME_LEN];
    char path[PLUGIN_PATH_LEN];      /* id: see plughost.h */
    bool inst;                       /* instrument (wants MIDI) */
} PluginInfo;

typedef struct {
    char fmt[8];
    char name[PLUGIN_NAME_LEN];
    char path[PLUGIN_PATH_LEN];
    char reason[200];
} PluginProblem;

/* The usable effects. Instruments (they want MIDI) are in "Problems" for now.
 * The first query starts a scan in the background (plugins_start() starts it at startup). */
void              plugins_start(void);
int               plugins_count(void);
const PluginInfo *plugins_get(int i);
int                  plugins_problem_count(void);
const PluginProblem *plugins_problem_get(int i);

void plugins_rescan(bool full);              /* full = without the cache (checks everything again) */
bool plugins_scanning(int *done, int *total, char *current, int csz);
int  plugins_generation(void);               /* grows with every new scan (for redrawing) */
/* By id (or an old entry from step 33: only the bundle path) finds the full id. false = not found. */
bool plugins_resolve(const char *fmt, const char *path_or_id, const char *name, char *id_out, int sz);

/* Search locations */
const char *plugins_fmt_name(int pf);        /* "VST3", "VST2", "LV2", "CLAP" */
const char *plugins_fmt_code(int pf);        /* "VST3", "VST", "LV2", "CLAP" */
int         plugins_fmt_index(const char *code);
int         plugins_loc_count(int pf);
const char *plugins_loc_get(int pf, int i);
void        plugins_loc_add(int pf, const char *dir);
void        plugins_loc_remove(int pf, int i);
void        plugins_loc_reset(int pf);       /* returns the standard locations */
bool        plugins_loc_exists(int pf, int i);
int         plugins_ask_folder(char *out, int sz);   /* zenity; 0 = a folder was chosen */

/* the child process mode (called by main before anything else) */
int plugins_scan_child_main(int argc, char **argv);

#endif
