/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Step 35d: a Windows plugin through yabridge takes seconds to start. Q-LiM must not stop while it waits:
 * loading happens on a background thread and the main thread keeps going (every frame - fast).
 * Argument: <path to slowplug.clap> */
#define _GNU_SOURCE
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/fxchain.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    char id[1024];
    snprintf(id, sizeof id, "%s|qlim.test.slow", argv[1]);
    streams_init();
    static Session S;
    session_init(&S, 48000);
    session_add_channel(&S, NULL);
    session_add_scene(&S);
    session_fx_add(&S, 0, "CLAP", "Slow", id);
    session_fx_add(&S, 0, "CLAP", "Slow", id);        /* the same one twice - the second after the first */
    double t0 = now(), worst = 0, loaded_at = -1;
    bool saw_loading = false;
    static float o[256 * 2];
    while (now() - t0 < 15.0) {
        double a = now();
        fxchain_sync(&S);
        qplug_idle_all();
        double d = now() - a;
        if (d > worst) worst = d;
        if (fxchain_slot_loading(&S, 0, 0)) saw_loading = true;
        mixer_render(&S, o, 256, 2);
        if (fxchain_find(&S, 0, 0) && fxchain_find(&S, 0, 1)) { loaded_at = now() - t0; break; }
        struct timespec t = { 0, 16000000 }; nanosleep(&t, NULL);
    }
    printf("slowest frame %.0f ms, both plugins ready after %.1f s, 'starting...' shown: %s\n", worst * 1000, loaded_at, saw_loading ? "yes" : "no");
    bool ok = worst < 0.25 && loaded_at > 0 && saw_loading;
    printf("%s: slow (Windows) plugins load in the background - the interface keeps running\nFXSLOW: %s\n", ok ? "ok" : "FAIL", ok ? "all ok" : "FAILED");
    fxchain_shutdown(&S);
    return ok ? 0 : 1;
}
