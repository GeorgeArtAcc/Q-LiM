/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Step 36m: a clip with stretch (the warp tick) is stopped and its buffer is freed right away (streams_trim_idle) -
 * the late background stretch thread must not read a freed buffer. */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/stream.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static Session S;
static atomic_bool run = true;
static void nap(long us) { struct timespec t = { us / 1000000, (us % 1000000) * 1000 }; nanosleep(&t, NULL); }
static void *audio(void *u) { (void)u; float b[256 * 2]; while (atomic_load(&run)) { mixer_render(&S, b, 256, 2); nap(2000); } return NULL; }
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    int iters = argc > 2 ? atoi(argv[2]) : 1500;
    session_init(&S, 48000); streams_init();
    session_add_channel(&S, "A"); session_add_channel(&S, "B"); session_add_scene(&S); session_add_scene(&S);
    for (int c = 0; c < 2; ++c) for (int sc = 0; sc < 2; ++sc) {
        if (session_set_clip(&S, c, sc, argv[1]) != 0) return 2;
        session_set_clip_bpm_fields(&S, c, sc, 128.0f, 0.0f, 128.0f);
        session_set_clip_warp(&S, c, sc, true);
    }
    /* 1) exact: a played clip (the preroll is over, it reads from the buffer) -> stop -> the buffer is freed ->
     *    a late reader (like the stretch background thread) */
    {
        session_set_clip_warp(&S, 0, 0, false);
        session_toggle_clip(&S, 0, 0);
        float b[256 * 2];
        for (int i = 0; i < 2000; ++i) { mixer_render(&S, b, 256, 2); if (i % 8 == 0) nap(1000); }
        ClipStream *cs = S.channels[0].clips[0].stream;
        stream_stop(cs);                               /* stopped stream (the mixer no longer reads it) */
        streams_trim_idle(0.0);
        size_t got = stream_read(cs, b, 256);
        printf("ok: late read after the buffer was freed returns silence (%zu)\n", got);
        session_stop_all(&S);
        for (int i = 0; i < 50; ++i) mixer_render(&S, b, 256, 2);
        session_set_clip_warp(&S, 0, 0, true);
    }
    session_set_tempo(&S, 97.0f);
    pthread_t th; pthread_create(&th, NULL, audio, NULL);
    for (int i = 0; i < iters; ++i) {
        int c = rand() % 2, sc = rand() % 2;
        session_toggle_clip(&S, c, sc);
        nap((long)(rand() % 30000));
        if (rand() % 2) session_stop_all(&S); else session_toggle_clip(&S, c, sc);
        streams_trim_idle(0.0);
        if (rand() % 5 == 0) session_set_tempo(&S, 80.0f + (float)(rand() % 80));
    }
    atomic_store(&run, false); pthread_join(th, NULL);
    session_stop_all(&S);
    printf("TRIMRACE: all ok (%d)\n", iters);
    return 0;
}
