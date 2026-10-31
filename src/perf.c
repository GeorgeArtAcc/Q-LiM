/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.3.7: performance settings + the automatic frame rate + "sound first" - see perf.h */
#define _GNU_SOURCE
#include "perf.h"
#include "sdlgfx.h"
#include "dsp_meter.h"
#include "stream.h"
#include "stretch.h"
#include "settings.h"
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI                   /* windows.h without its own Rectangle / CloseWindow (sdlgfx.h has them) */
#define NOUSER
#include <windows.h>
#endif

static PerfSettings g_ps = { PERF_DRAW_AUTO, 0, true, true, 0, true, 1.0f, 0 };

PerfSettings *perf_settings(void) { return &g_ps; }

static void perf_path(char *out, size_t sz)
{
    const char *home = getenv("HOME");
    snprintf(out, sz, "%s/.config/qlim/performance.conf", home ? home : "/tmp");
}

void perf_load(void)
{
    char p[1024], line[256];
    perf_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char v[64];
        if (sscanf(line, "drawing=%63s", v) == 1)
            g_ps.draw = !strcmp(v, "gpu") ? PERF_DRAW_GPU : !strcmp(v, "light") ? PERF_DRAW_LIGHT : PERF_DRAW_AUTO;
        else if (sscanf(line, "fps=%63s", v) == 1) g_ps.fps = !strcmp(v, "auto") ? 0 : atoi(v);
        else if (sscanf(line, "idle_save=%63s", v) == 1) g_ps.idle_save = atoi(v) != 0;
        else if (sscanf(line, "sound_first=%63s", v) == 1) g_ps.sound_first = atoi(v) != 0;
        else if (sscanf(line, "meters=%63s", v) == 1) g_ps.meters = atoi(v) != 0;
        else if (sscanf(line, "zoom=%63s", v) == 1) { g_ps.zoom = (float)atof(v); if (!(g_ps.zoom >= 0.5f && g_ps.zoom <= 2.5f)) g_ps.zoom = 1.0f; }
        else if (sscanf(line, "waveform=%63s", v) == 1)
            g_ps.wave = !strcmp(v, "light") ? 2 : !strcmp(v, "smooth") ? 1 : 0;
        else if (sscanf(line, "interface=%63s", v) == 1)
            g_ps.eco = !strcmp(v, "eco") ? 2 : !strcmp(v, "normal") ? 1 : 0;
    }
    fclose(f);
    if (g_ps.fps < 0 || g_ps.fps > 500) g_ps.fps = 0;
}

void perf_save(void)
{
    char p[1024];
    perf_path(p, sizeof p);
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", p);
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = 0; settings_ensure_dir(dir); }
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "# Q-LiM performance settings (Edit > Performance)\n");
    fprintf(f, "drawing=%s\n", g_ps.draw == PERF_DRAW_GPU ? "gpu" : g_ps.draw == PERF_DRAW_LIGHT ? "light" : "auto");
    if (g_ps.fps > 0) fprintf(f, "fps=%d\n", g_ps.fps); else fprintf(f, "fps=auto\n");
    fprintf(f, "idle_save=%d\nsound_first=%d\n", g_ps.idle_save ? 1 : 0, g_ps.sound_first ? 1 : 0);
    fprintf(f, "interface=%s\n", g_ps.eco == 2 ? "eco" : g_ps.eco == 1 ? "normal" : "auto");
    fprintf(f, "meters=%d\nzoom=%.2f\n", g_ps.meters ? 1 : 0, (double)g_ps.zoom);
    fprintf(f, "waveform=%s\n", g_ps.wave == 2 ? "light" : g_ps.wave == 1 ? "smooth" : "auto");
    fclose(f);
}

/* the CPU time of THIS thread (the main thread draws) in seconds */
static double thread_cpu_s(void)
{
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0.0;
    unsigned long long kk = ((unsigned long long)k.dwHighDateTime << 32) | k.dwLowDateTime;
    unsigned long long uu = ((unsigned long long)u.dwHighDateTime << 32) | u.dwLowDateTime;
    return (double)(kk + uu) * 1e-7;
#else
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
#endif
}
static double wall_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

#define UI_BUDGET 0.40                  /* the drawing may use this share of one CPU core */
static const int LEVELS[] = { 60, 45, 30, 24, 20, 15, 12 };
#define NLEV ((int)(sizeof LEVELS / sizeof LEVELS[0]))

static double g_cost = -1.0;            /* CPU seconds per frame (smoothed) */
static double g_last_cpu, g_last_wall, g_next_check;
static int    g_auto_cap = 0;           /* 0 = no limit */
static int    g_cap_now;
static double g_sound_until;            /* "sound first" holds the rate down until then */
static unsigned g_seen[4];
static bool   g_seen_init;

int  perf_max_fps_now(void) { return g_cap_now; }
bool perf_slowed_for_sound(void) { return wall_s() < g_sound_until; }
bool perf_wave_light(void) { return g_ps.wave == 2 || (g_ps.wave == 0 && GfxEco()); }
float perf_ui_load(void)
{
    if (g_cost < 0.0) return 0.0f;
    int f = g_cap_now > 0 ? g_cap_now : 60;
    return (float)(g_cost * f);
}

void perf_problem_counts(unsigned *card, unsigned *disk, unsigned *warp, unsigned *over)
{
    *card = dsp_meter_xruns();
    *disk = (unsigned)streams_starved_total();
    *warp = stretch_underruns_total();
    *over = dsp_meter_overloads();
}

static void check_sound(double now)
{
    unsigned c[4];
    perf_problem_counts(&c[0], &c[1], &c[2], &c[3]);
    if (!g_seen_init) { memcpy(g_seen, c, sizeof c); g_seen_init = true; return; }
    const char *why = NULL;
    for (int i = 0; i < 4; ++i) {
        if (c[i] < g_seen[i]) { g_seen[i] = c[i]; continue; }       /* the counters were reset (audio restart) */
        if (c[i] > g_seen[i] && !why)
            why = i == 0 ? "the sound card / system" : i == 1 ? "reading from the disk" : i == 2 ? "WARP (time-stretch)"
                                                                                       : "the audio processing (over 100 %)";
        g_seen[i] = c[i];
    }
    if (!why) return;
    static double last_msg = -1e9;
    char msg[200];
    if (g_ps.sound_first) {
        g_sound_until = now + 30.0;
        snprintf(msg, sizeof msg, "Sound dropout (%s) - fewer frames for 30 s to give the sound room", why);
    } else {
        snprintf(msg, sizeof msg, "Sound dropout (%s) - see Edit > Performance", why);
    }
    if (now - last_msg > 10.0) { ui_flash_message(msg); last_msg = now; }
}

void perf_frame(bool playing, bool plugin_windows_open)
{
    double now = wall_s(), cpu = thread_cpu_s();
    if (g_last_wall > 0.0) {
        double d = cpu - g_last_cpu;
        if (d >= 0.0 && d < 1.0) g_cost = g_cost < 0.0 ? d : g_cost * 0.95 + d * 0.05;
    }
    g_last_cpu = cpu; g_last_wall = now;

    if (now >= g_next_check) {                  /* once a second: the automatic cap + the sound */
        g_next_check = now + 1.0;
        int cap = LEVELS[NLEV - 1];
        if (g_cost >= 0.0) {
            for (int i = 0; i < NLEV; ++i) if (g_cost * LEVELS[i] <= UI_BUDGET) { cap = LEVELS[i]; break; }
        } else cap = 0;
        if (cap == LEVELS[0]) cap = 0;           /* fits at 60: no limit (a 120/144 Hz screen keeps its rate) */
        /* a little reluctance to go up again (no back and forth every second) */
        if (g_auto_cap > 0 && (cap == 0 || cap > g_auto_cap)) {
            int up = 0;
            for (int i = 0; i < NLEV; ++i) if (LEVELS[i] == g_auto_cap) up = i > 0 ? LEVELS[i - 1] : 0;
            if (g_cost * (up ? up : 60) > UI_BUDGET * 0.8) cap = g_auto_cap;
            else cap = up;
        }
        g_auto_cap = cap;
        check_sound(now);
    }

    static int manual = -1;                     /* QLIM_PLAY_FPS (older test switch) = a fixed maximum */
    if (manual < 0) { const char *e = getenv("QLIM_PLAY_FPS"); manual = e && atoi(e) > 0 ? atoi(e) : 0; }
    int cap = manual > 0 ? manual : g_ps.fps > 0 ? g_ps.fps : g_auto_cap;
    if (g_ps.sound_first && now < g_sound_until && (cap == 0 || cap > 15)) cap = 15;
    if (plugin_windows_open && cap > 0 && cap < 30) cap = 30;      /* plugin windows need their timers */
    if (plugin_windows_open && g_ps.fps == 0 && manual == 0 && !(now < g_sound_until)) cap = 0;
    g_cap_now = cap;
    GfxSetZoom(g_ps.zoom);
    GfxSetEco(g_ps.eco == 2 || (g_ps.eco == 0 && GfxSoftwareRendering()) || getenv("QLIM_ECO") != NULL);
    { static int sm = -1; if (sm < 0) { const char *e = getenv("QLIM_SMART"); sm = !(e && e[0] == '0'); } GfxSetSmart(sm != 0); }
    GfxSetMaxFps(cap);
    GfxSetBusyFps(plugin_windows_open || playing || !g_ps.idle_save ? 0 : 10);
}
