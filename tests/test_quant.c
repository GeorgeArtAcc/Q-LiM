/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.1 launch quantization ("Global Quantization" like Ableton/Bitwig): with 120 BPM and 1 bar (4 beats = 2 s),
 * a clip pressed in the middle of a bar starts EXACTLY on the next bar line (sample accurate); a scene starts
 * together and stops the channels it has no clip in; restart of a playing clip waits too; the grid follows
 * the master tempo and the chosen length; stopping everything resets the count; "Now" launches at once. */
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

#define SR 48000
#define BLK 256
static float *tape; static size_t tape_len, tape_cap;   /* the whole output, left channel */

/* renders until the tape reaches 'until' seconds; ticks like the real main (~16 ms) */
static void run_to(Session *s, double until)
{
    size_t end = (size_t)(until * SR);
    float buf[BLK * 2];
    double acc = 0;
    while (tape_len < end) {
        mixer_render(s, buf, BLK, 2);
        if (tape_len + BLK > tape_cap) { tape_cap = (tape_cap + BLK) * 2; tape = realloc(tape, tape_cap * sizeof(float)); }
        for (int i = 0; i < BLK; ++i) tape[tape_len + i] = buf[i * 2];
        tape_len += BLK;
        acc += (double)BLK / SR;
        if (acc > 0.016) { session_queue_tick(s); session_quant_tick(s); acc = 0; }
        nap((long)(1e6 * BLK / SR));
    }
}
static size_t first_sound(size_t from, size_t to)
{
    for (size_t i = from; i < to && i < tape_len; ++i) if (fabsf(tape[i]) > 1e-6f) return i;
    return (size_t)-1;
}
static bool playing(Session *s, int ch, int sc) { return atomic_load(&s->channels[ch].clips[sc].playing); }
static double pos_s(Session *s, int ch, int sc) { return atomic_load(&s->channels[ch].clips[sc].position) / (double)SR; }

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    const char *A = argv[1], *B = argv[2];      /* two 2 s files with constant (non-zero) sound */
    static Session S; Session *s = &S;
    session_init(s, SR);
    streams_init();
    session_add_channel(s, NULL); session_add_channel(s, NULL);
    for (int i = 0; i < 4; ++i) session_add_scene(s);
    session_set_clip(s, 0, 0, A); session_toggle_loop(s, 0, 0);
    session_set_clip(s, 1, 0, B); session_toggle_loop(s, 1, 0);
    session_set_clip(s, 0, 1, B); session_toggle_loop(s, 0, 1);
    session_set_clip(s, 1, 2, A); session_toggle_loop(s, 1, 2);
    atomic_store(&s->audio_running, false);

    CHECK(session_get_quantize(s) == 4.0f && session_get_tempo(s) == 120.0f, "defaults: 1 bar, 120 BPM");
    double beats;
    CHECK(!session_transport(s, &beats), "transport stopped while nothing plays");

    /* 1) the first clip starts at once (it is beat 1); a second one pressed mid-bar waits for the bar line */
    session_toggle_mute(s, 0);                  /* listen only to channel 2 */
    session_toggle_clip(s, 0, 0);
    run_to(s, 0.7);
    CHECK(playing(s, 0, 0), "first clip plays at once (nothing was playing)");
    CHECK(session_transport(s, &beats) && fabs(beats - 1.4) < 0.02, "transport runs, beat %.2f (want 1.40)", beats);
    session_toggle_clip(s, 1, 0);
    CHECK(!playing(s, 1, 0) && session_clip_launch_state(s, 1, 0) == 1, "clip pressed mid-bar waits (blinks)");
    run_to(s, 1.9);
    CHECK(!playing(s, 1, 0) && first_sound(0, tape_len) == (size_t)-1, "still silent before the bar line");
    run_to(s, 2.5);
    size_t on = first_sound(0, tape_len);
    CHECK(on == 2 * SR, "starts exactly on bar 2: sample %zd (want %d)", (ssize_t)on, 2 * SR);
    CHECK(playing(s, 1, 0) && session_clip_launch_state(s, 1, 0) == 0, "then plays, no longer waiting");
    CHECK(fabs(pos_s(s, 1, 0) - 0.5) < 0.03, "its playhead %.3f s (want 0.5)", pos_s(s, 1, 0));
    session_toggle_mute(s, 0);

    /* 2) pressing the playing clip again restarts it on the next bar */
    run_to(s, 2.9);
    double before = pos_s(s, 0, 0);
    session_toggle_clip(s, 0, 0);
    CHECK(session_clip_launch_state(s, 0, 0) == 3, "restart is waiting");
    run_to(s, 3.9);
    CHECK(pos_s(s, 0, 0) > before, "keeps playing until the bar line (%.2f s)", pos_s(s, 0, 0));
    run_to(s, 4.5);
    CHECK(fabs(pos_s(s, 0, 0) - 0.5) < 0.06, "restarted on bar 3 (playhead %.3f, want ~0.5)", pos_s(s, 0, 0));

    /* 3) a scene: everything changes together; channels without a clip in that scene stop on the bar */
    run_to(s, 4.6);
    session_play_scene(s, 1);                   /* scene 2: only channel 1 has a clip */
    CHECK(session_clip_launch_state(s, 0, 1) == 1 && session_clip_launch_state(s, 1, 0) == 2, "scene waits: launch + stop marked");
    run_to(s, 5.9);
    CHECK(playing(s, 0, 0) && playing(s, 1, 0) && !playing(s, 0, 1), "old clips still play before the bar");
    run_to(s, 6.3);
    CHECK(!playing(s, 0, 0) && !playing(s, 1, 0) && playing(s, 0, 1), "on bar 4: scene clip plays, the others stopped");
    CHECK(fabs(pos_s(s, 0, 1) - 0.3) < 0.03, "scene clip playhead %.3f (want 0.3)", pos_s(s, 0, 1));

    /* 4) shorter grid (1 beat = 0.5 s at 120) and the master tempo moves the grid (150 BPM -> beat 0.4 s) */
    session_set_quantize(s, 1.0f);
    session_set_tempo(s, 150.0f);
    run_to(s, 6.35);
    CHECK(session_transport(s, &beats), "transport running");
    double t0 = tape_len / (double)SR, next = ceil(beats - 1e-9) - beats;   /* beats left to the next beat */
    double want = t0 + next * 60.0 / 150.0;
    session_toggle_mute(s, 0);
    size_t mark = tape_len;
    session_toggle_clip(s, 1, 2);
    run_to(s, want + 0.2);
    on = first_sound(mark, tape_len);
    CHECK(on != (size_t)-1 && fabs(on / (double)SR - want) < 1.5 / SR, "1 beat at 150 BPM: starts at %.4f s (want %.4f)",
          on / (double)SR, want);
    session_toggle_mute(s, 0);

    /* 5) stop everything -> the count resets, the next clip starts at once */
    session_stop_all(s);
    run_to(s, tape_len / (double)SR + 0.1);
    CHECK(!session_transport(s, &beats), "stop all: transport stopped");
    session_set_quantize(s, 4.0f);
    session_toggle_clip(s, 0, 0);
    run_to(s, tape_len / (double)SR + 0.05);
    CHECK(playing(s, 0, 0), "after stop the next clip starts at once");
    CHECK(session_transport(s, &beats) && beats < 0.2, "count starts again from it (beat %.2f)", beats);

    /* 6) "Now": no waiting at all */
    session_set_quantize(s, 0.0f);
    session_toggle_clip(s, 1, 0);
    CHECK(session_clip_launch_state(s, 1, 0) == 0, "Now: no waiting");
    run_to(s, tape_len / (double)SR + 0.05);
    CHECK(playing(s, 1, 0), "Now: plays at once");

    /* 7) a newer press in the same channel cancels the older waiting one */
    session_set_quantize(s, 4.0f);
    session_toggle_clip(s, 1, 2);
    session_toggle_clip(s, 1, 0);               /* playing one -> restart wins over the waiting launch */
    CHECK(session_clip_launch_state(s, 1, 2) == 0 && session_clip_launch_state(s, 1, 0) == 3, "newer press replaces the older");
    CHECK(atomic_load(&s->ql_pending) == 1, "one pending action (%d)", atomic_load(&s->ql_pending));
    session_stop_all(s);
    CHECK(atomic_load(&s->ql_pending) == 0 && session_clip_launch_state(s, 1, 0) == 0, "stop all clears waiting launches");
    run_to(s, tape_len / (double)SR + 0.1);

    /* 8) a WARP clip (Rubber Band) is computed ahead while it waits and still lands on the bar line */
    session_set_tempo(s, 120.0f);
    session_set_clip_bpm_fields(s, 1, 0, 100.0f, 0, 0);
    session_set_clip_warp(s, 1, 0, true);       /* 100 -> 120 BPM: 1.2x through Rubber Band */
    session_toggle_mute(s, 0);
    session_toggle_clip(s, 0, 0);               /* beat 1 (muted) */
    run_to(s, tape_len / (double)SR + 0.9);
    session_transport(s, &beats);
    double tw = tape_len / (double)SR + (4.0 - beats) * 0.5;
    mark = tape_len;
    session_toggle_clip(s, 1, 0);
    run_to(s, tw + 0.3);
    on = first_sound(mark, tape_len);
    CHECK(on != (size_t)-1 && fabs(on / (double)SR - tw) < 0.002, "WARP clip starts on the bar: %.4f s (want %.4f)", on / (double)SR, tw);
    size_t gap = 0, cur = 0;
    for (size_t i = on; i < tape_len; ++i) { if (fabsf(tape[i]) < 1e-6f) { if (++cur > gap) gap = cur; } else cur = 0; }
    CHECK(gap < 48, "WARP clip has no holes after the start (longest %zu samples)", gap);
    session_toggle_mute(s, 0);
    session_stop_all(s);
    run_to(s, tape_len / (double)SR + 0.1);

    session_free(s);
    free(tape);
    printf(fails ? "QUANT: %d FAILED\n" : "QUANT: all ok\n", fails);
    return fails != 0;
}
