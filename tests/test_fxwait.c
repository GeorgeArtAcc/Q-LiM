/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Step 35b: a plugin that blocks the audio thread until the main thread runs one of its requests
 * (that is what yabridge does). Meanwhile the main thread adds/deletes channels (waits for the audio).
 * Without "servicing while waiting" the two threads wait for each other (the interface freezes). With it - they don't.
 * Arguments: <path to waitplug.clap> [nopump] */
#define _GNU_SOURCE
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/fxchain.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static Session S;
static atomic_bool stop, done;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void *audio(void *a)
{
    (void)a;
    static float o[256 * 2];
    while (!atomic_load(&stop)) { mixer_render(&S, o, 256, 2); struct timespec t = { 0, 2000000 }; nanosleep(&t, NULL); }
    atomic_store(&done, true);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    bool pump = !(argc > 2 && !strcmp(argv[2], "nopump"));
    char id[1024];
    snprintf(id, sizeof id, "%s|qlim.test.wait", argv[1]);
    streams_init();
    session_init(&S, 48000);
    session_add_channel(&S, NULL); session_add_channel(&S, NULL);
    session_add_scene(&S);
    session_fx_add(&S, 0, "CLAP", "Wait", id);
    if (pump) session_set_wait_pump(qplug_idle_all);
    atomic_store(&S.audio_running, true);
    fxchain_sync(&S);
    pthread_t th;
    pthread_create(&th, NULL, audio, NULL);
    double worst = 0;
    double t_end = now() + 6.0;
    int ops = 0;
    while (now() < t_end) {                         /* main-thread "frames" */
        fxchain_sync(&S);
        qplug_idle_all();
        double t0 = now();
        session_add_channel(&S, NULL);              /* locks and waits for the audio */
        session_delete_channel(&S, S.channel_count - 1);
        double d = now() - t0;
        if (d > worst) worst = d;
        ++ops;
        struct timespec t = { 0, 16000000 }; nanosleep(&t, NULL);
    }
    atomic_store(&stop, true);
    while (!atomic_load(&done)) {                   /* the main thread finishes its work */
        if (pump) qplug_idle_all();
        struct timespec t = { 0, 1000000 }; nanosleep(&t, NULL);
    }
    pthread_join(th, NULL);
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_NOLOAD);
    atomic_int *to = lib ? dlsym(lib, "waitplug_timeouts") : NULL, *sv = lib ? dlsym(lib, "waitplug_served") : NULL;
    int timeouts = to ? atomic_load(to) : -1, served = sv ? atomic_load(sv) : -1;
    atomic_store(&S.audio_running, false);
    fxchain_shutdown(&S);
    printf("%s: %d channel add/delete, slowest %.0f ms, plugin requests served %d, plugin waits that timed out %d\n",
           pump ? "with pump" : "without pump", ops, worst * 1000, served, timeouts);
    bool ok = pump ? (timeouts == 0 && worst < 0.5 && served > 0) : (timeouts > 0 || worst > 1.0);
    printf("%s: %s\nFXWAIT: %s\n", ok ? "ok" : "FAIL",
           pump ? "main thread keeps serving plugins while it waits (no freeze)" : "old behaviour reproduces the freeze",
           ok ? "all ok" : "FAILED");
    return ok ? 0 : 1;
}
