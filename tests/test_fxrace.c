/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Step 34b: changing plugins WHILE audio is running (the crash in 0.34: the chain changed under the audio's feet).
 * An audio thread calls mixer_render continuously; the main thread adds/removes/bypasses plugins and calls fxchain_sync. */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/fxchain.h"
#include "../src/plugins.h"
#include <math.h>
#include <pthread.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* the stand-ins for ui.c / plugins.c are in fx_stubs.c */

static Session S;
static atomic_bool stop;
static void *audio(void *a)
{
    (void)a;
    static float out[512 * 2];
    while (!atomic_load(&stop)) { mixer_render(&S, out, 256, 2); mixer_render(&S, out, 512, 2); }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    const char *file = "/tmp/qlim_test_audio/fx.wav";       /* from test_fx */
    char ids[4][1024];
    const char *fmts[4] = { "VST3", "VST", "LV2", "CLAP" };
    snprintf(ids[0], 1024, "%s/qtest_gain.vst3|2046504473616C63316E4771546D6C51", argv[1]);
    snprintf(ids[1], 1024, "%s/qtest_gain-vst.so", argv[1]);
    snprintf(ids[2], 1024, "%s/qtest_gain.lv2|urn:qlim:test:gain", argv[1]);
    snprintf(ids[3], 1024, "%s/qtest_gain.clap|qlim.test.gain", argv[1]);
    streams_init();
    session_init(&S, 48000);
    session_add_channel(&S, NULL); session_add_channel(&S, NULL);
    session_add_scene(&S);
    session_set_clip(&S, 0, 0, file); session_set_clip(&S, 1, 0, file);
    atomic_store(&S.audio_running, true);
    pthread_t th;
    pthread_create(&th, NULL, audio, NULL);
    session_toggle_clip(&S, 0, 0); session_toggle_clip(&S, 1, 0);
    srand(7);
    int ops = 0;
    for (int it = 0; it < 4000; ++it) {
        int c = rand() % 2, r = rand() % 10;
        Channel *ch = &S.channels[c];
        if (r < 4 && ch->fx_count < MAX_CH_FX) { int f = rand() % 4; session_fx_add(&S, c, fmts[f], "QTest", ids[f]); }
        else if (r < 7 && ch->fx_count > 0) session_fx_remove(&S, c, rand() % ch->fx_count);
        else if (ch->fx_count > 0) session_fx_toggle_bypass(&S, c, rand() % ch->fx_count);
        fxchain_sync(&S);
        ++ops;
        if (it % 50 == 0) { struct timespec t = { 0, 2000000 }; nanosleep(&t, NULL); }
    }
    atomic_store(&stop, true);
    pthread_join(th, NULL);
    atomic_store(&S.audio_running, false);
    fxchain_shutdown(&S);
    session_free(&S);
    printf("ok: %d plugin changes while audio was running - no crash\nFXRACE: all ok\n", ops);
    return 0;
}
