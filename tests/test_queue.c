/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Queue: the next clip starts EXACTLY after the end of the current one (no gap), manual play clears the queue,
 * stopping everything clears the queue, reordering. With and without the warp tick (Rubber Band). */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void nap(long us) { struct timespec t = { us / 1000000, (us % 1000000) * 1000 }; nanosleep(&t, NULL); }

/* renders secs seconds in real time, calls tick every ~16 ms like the real main */
static float *render(Session *s, double secs, size_t *n_out, unsigned blk)
{
    size_t n = (size_t)(secs * s->sample_rate);
    float *o = malloc(n * sizeof(float)), buf[1024 * 2];
    double acc = 0;
    for (size_t d = 0; d < n; d += blk) {
        mixer_render(s, buf, blk, 2);
        for (size_t i = 0; i < blk && d + i < n; ++i) o[d + i] = buf[i * 2];
        acc += (double)blk / s->sample_rate;
        if (acc > 0.016) { session_queue_tick(s); acc = 0; }
        nap((long)(1e6 * blk / s->sample_rate));
    }
    *n_out = n;
    return o;
}

/* the longest run of zeros between the first and the last non-zero sample */
static size_t longest_gap(const float *y, size_t a, size_t b)
{
    size_t best = 0, cur = 0;
    for (size_t i = a; i < b; ++i) { if (fabsf(y[i]) < 1e-6f) { if (++cur > best) best = cur; } else cur = 0; }
    return best;
}

int main(int argc, char **argv)
{
    const char *A = argv[1], *B = argv[2];      /* two short files with constant (non-zero) sound */
    static Session S; Session *s = &S;
    session_init(s, 48000);
    streams_init();
    session_add_channel(s, NULL); session_add_channel(s, NULL);
    for (int i = 0; i < 4; ++i) session_add_scene(s);
    session_set_clip(s, 0, 0, A);
    session_set_clip(s, 1, 1, B);
    session_set_clip(s, 0, 2, B);
    atomic_store(&s->audio_running, false);

    CHECK(!session_queue_add(s, 1, 1), "nothing playing -> cannot add to queue");

    for (int warp = 0; warp <= 1; ++warp) {
        session_set_clip_warp(s, 0, 0, warp);
        session_set_clip_warp(s, 1, 1, warp);
        session_set_clip_warp(s, 0, 2, warp);
        if (warp) { session_set_clip_bpm_fields(s, 0, 0, 120, 0, 0); session_set_clip_bpm_value(s, 0, 0, 120); }
        session_toggle_clip(s, 0, 0);
        nap(50000);
        CHECK(session_queue_add(s, 1, 1) && session_queue_add(s, 0, 2) && session_queue_len(s) == 2, "[warp %d] two clips queued", warp);
        int ach, asc;
        CHECK(session_queue_anchor(s, &ach, &asc) && ach == 0 && asc == 0, "[warp %d] 'now playing' = the playing clip", warp);
        size_t n; float *y = render(s, 2.0 + 2.0 + 0.8, &n, 256);   /* A = 2 s, then B = 2 s, then B again */
        size_t first = 0; while (first < n && fabsf(y[first]) < 1e-6f) first++;
        size_t gap = longest_gap(y, first, (size_t)(3.9 * 48000));
        /* with the Rubber Band warp there is a ~0.5 ms smooth start/end of the frame - allow a tiny bit */
        CHECK(gap <= (warp ? 64u : 1u), "[warp %d] A -> B with no gap (longest silence %zu samples)", warp, gap);
        CHECK(atomic_load(&s->channels[0].clips[2].playing) && session_queue_len(s) == 0,
              "[warp %d] then the 2nd queued clip plays, queue empty", warp);
        free(y);
        session_stop_all(s);
        session_queue_tick(s);
        { size_t nn; float *t = render(s, 0.05, &nn, 256); free(t); }   /* let the fade after stop (~3 ms) pass */
    }
    session_set_clip_warp(s, 0, 0, false);
    session_set_clip_warp(s, 1, 1, false);
    session_set_clip_warp(s, 0, 2, false);

    /* manual play clears */
    session_toggle_clip(s, 0, 0); nap(20000);
    session_queue_add(s, 1, 1); session_queue_add(s, 0, 2);
    session_toggle_clip(s, 1, 1);
    CHECK(session_queue_len(s) == 0, "manual play clears the queue");

    /* stopping everything clears */
    session_queue_add(s, 0, 2);
    session_stop_all(s); session_queue_tick(s);
    CHECK(session_queue_len(s) == 0, "stop all clears the queue");

    /* pause does NOT clear */
    session_toggle_clip(s, 0, 0); nap(20000);
    session_queue_add(s, 1, 1);
    session_pause_all(s); session_queue_tick(s);
    CHECK(session_queue_len(s) == 1, "pause keeps the queue");
    session_resume_all(s);

    /* reordering */
    session_queue_add(s, 0, 2);
    session_queue_move(s, 1, 0);
    int c0, s0; session_queue_get(s, 0, &c0, &s0);
    CHECK(c0 == 0 && s0 == 2 && session_queue_len(s) == 2, "move: 2nd item becomes 1st");
    session_queue_remove(s, 0);
    session_queue_get(s, 0, &c0, &s0);
    CHECK(session_queue_len(s) == 1 && c0 == 1 && s0 == 1, "remove");

    /* a deleted clip disappears from the queue */
    session_clear_clip(s, 1, 1); session_queue_tick(s);
    CHECK(session_queue_len(s) == 0, "deleted clip leaves the queue");

    session_free(s);
    printf(fails ? "QUEUE: %d FAILED\n" : "QUEUE: all ok\n", fails);
    return fails != 0;
}
