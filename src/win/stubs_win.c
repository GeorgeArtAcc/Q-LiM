/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/*
 * Windows build: the parts that exist only on Linux (direct PipeWire output, LV2 plugins)
 * are replaced by "not available" versions. (Plugin windows: src/win/plugwin_win.c.)
 */
#include "../audio_pw.h"
#include "../plughost.h"
#include <string.h>
#include <stdio.h>

/* ---------------- PipeWire: not on Windows ---------------- */
bool pwb_available(void) { return false; }
void pwb_forget_availability(void) {}
int  pwb_list_sinks(PwbSink *out, int max) { (void)out; (void)max; return 0; }
int  pwb_open(const char *target, int rate, int quantum, int channels, PwbRenderFn fn, void *user,
              int *out_channels, char *err, int err_size)
{
    (void)target; (void)rate; (void)quantum; (void)channels; (void)fn; (void)user; (void)out_channels;
    if (err && err_size > 0) snprintf(err, (size_t)err_size, "PipeWire does not exist on Windows");
    return -1;
}
void pwb_close(void) {}
bool pwb_is_open(void) { return false; }
void pwb_get_status(PwbStatus *st) { if (st) memset(st, 0, sizeof *st); }
bool pwb_graph_supported(void) { return false; }
int  pwb_graph_open(const char *target, int rate, int quantum, int nch, const char *const *names,
                    PwbChannelsFn chfn, PwbMasterFn mfn, void *user, int *out_hw_ports, char *err, int err_size)
{
    (void)target; (void)rate; (void)quantum; (void)nch; (void)names; (void)chfn; (void)mfn; (void)user; (void)out_hw_ports;
    if (err && err_size > 0) snprintf(err, (size_t)err_size, "PipeWire does not exist on Windows");
    return -1;
}
bool pwb_graph_is_open(void) { return false; }
int  pwb_graph_sync(int nch, const char *const *names, PwbRoute *routes, PwbRoute *master, bool quiet)
{
    (void)nch; (void)names; (void)routes; (void)master; (void)quiet;
    return -1;
}
int  pwb_graph_hw_ports(void) { return 0; }
const char *pwb_library_version(void) { return ""; }

/* ---------------- LV2: not on Windows (almost no LV2 plugins exist for Windows) ---------------- */
int lv2_open(const char *id, double rate, int maxb, QInst *out, char *err, int esz)
{
    (void)id; (void)rate; (void)maxb; (void)out;
    if (err && esz > 0) snprintf(err, (size_t)esz, "LV2 plugins are not supported on Windows");
    return -1;
}
int lv2_scan(const char *path) { (void)path; return 1; }
