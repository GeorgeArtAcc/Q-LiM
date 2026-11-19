/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 2.0: opens one plugin's window the way Q-LiM does (main thread + the plugin run loop + an audio thread) and reports
 * how long the opening took and whether the window ran.   plugwin_try FORMAT ID [SECONDS] */
#define _GNU_SOURCE
#include "../src/plughost.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static QPlug *g_p;
static atomic_int g_run = 1;
static atomic_long g_blocks;
static void *audio(void *a)
{
    (void)a;
    static float l[512], r[512];
    while (atomic_load(&g_run)) {
        memset(l, 0, sizeof l); memset(r, 0, sizeof r);
        qplug_process(g_p, l, r, 512);
        atomic_fetch_add(&g_blocks, 1);
        usleep(10666);
    }
    return NULL;
}
int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: plugwin_try FORMAT ID [SECONDS]   |   plugwin_try scan FORMAT PATH\n"); return 2; }
    if (!strcmp(argv[1], "scan") && argc > 3) return qplug_scan_child(argv[2], argv[3], 0);
    double secs = argc > 3 ? atof(argv[3]) : 8;
    char err[512] = "";
    double t0 = now();
    g_p = qplug_load(argv[1], argv[2], 48000, 512, err, sizeof err);
    printf("load: %s (%.2f s)\n", g_p ? "ok" : err, now() - t0);
    if (!g_p) return 1;
    pthread_t th; pthread_create(&th, NULL, audio, NULL);
    t0 = now();
    int rc = qplug_editor_open(g_p, "plugwin_try", err, sizeof err);
    printf("editor_open: %s (%.2f s)\n", rc == 0 ? "ok" : err, now() - t0);
    double end = now() + secs, last = now();
    long frames = 0; double worst = 0;
    while (now() < end) {
        double a = now();
        qplug_idle_all();
        double d = now() - a;
        if (d > worst) worst = d;
        ++frames;
        usleep(16000);
        (void)last;
    }
    printf("idle: %ld frames, slowest %.1f ms, audio blocks %ld\n", frames, worst * 1000, (long)atomic_load(&g_blocks));
    if (getenv("SHOT")) { char c[300]; snprintf(c, sizeof c, "import -window root %s", getenv("SHOT")); if (system(c)) {} }
    qplug_editor_close(g_p);
    atomic_store(&g_run, 0); pthread_join(th, NULL);
    qplug_free(g_p);
    printf("closed ok\n");
    return 0;
}
