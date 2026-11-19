/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.1 launch quantization ("Global Quantization" like Ableton/Bitwig): with 120 BPM and 1 bar (4 beats = 2 s),
 * a clip pressed in the middle of a bar starts EXACTLY on the next bar line (sample accurate); a scene starts
 * together and stops the channels it has no clip in; restart of a playing clip waits too; the grid follows
 * the master tempo and the chosen length; stopping everything resets the count; "Now" launches at once. */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/project.h"
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

    /* 9) 1.4.8: the time signature. 7/8 at 120 BPM: a bar = 3.5 quarter beats = 1.75 s; "1 Bar" follows it */
    session_set_clip_warp(s, 1, 0, false);
    session_set_quant_mode(s, QUANT_BAR);
    CHECK(session_set_time_sig(s, 7, 8) == 0 && fabs(session_get_quantize(s) - 3.5f) < 1e-6, "7/8: 1 Bar = 3.5 beats (%.3f)", session_get_quantize(s));
    CHECK(session_set_time_sig(s, 7, 6) != 0 && session_set_time_sig(s, 0, 4) != 0 && session_set_time_sig(s, 33, 4) != 0, "invalid signatures refused");
    { int n, d; session_get_time_sig(s, &n, &d); CHECK(n == 7 && d == 8, "still 7/8 after the refused ones"); }
    session_set_quant_mode(s, QUANT_BEAT);
    CHECK(fabs(session_get_quantize(s) - 0.5f) < 1e-6, "7/8: 1 Beat = an eighth (%.3f)", session_get_quantize(s));
    session_set_quant_mode(s, QUANT_2BARS);
    CHECK(fabs(session_get_quantize(s) - 7.0f) < 1e-6, "7/8: 2 Bars = 7 beats");
    session_set_quant_mode(s, QUANT_BAR);
    session_set_tempo(s, 120.0f);
    session_toggle_mute(s, 0);
    session_toggle_clip(s, 0, 0);               /* beat 1 (muted): the transport starts */
    run_to(s, tape_len / (double)SR + 0.6);
    CHECK(session_transport(s, &beats), "transport running in 7/8");
    double tb = tape_len / (double)SR + (3.5 - beats) * 0.5;   /* the next 7/8 bar line */
    mark = tape_len;
    session_toggle_clip(s, 1, 0);
    run_to(s, tb + 0.3);
    on = first_sound(mark, tape_len);
    CHECK(on != (size_t)-1 && fabs(on / (double)SR - tb) < 0.001, "7/8: the clip starts on the bar line %.4f s (want %.4f)", on / (double)SR, tb);
    session_toggle_mute(s, 0);
    session_stop_all(s);
    run_to(s, tape_len / (double)SR + 0.1);
    /* the old way of setting it (a number of beats, old projects) finds the mode in the signature */
    session_set_time_sig(s, 4, 4);
    session_set_quantize(s, 2.0f);
    CHECK(session_get_quant_mode(s) == QUANT_HALF_BAR, "4/4: 2 beats = 1/2 Bar");
    session_set_quantize(s, 3.0f);
    CHECK(session_get_quant_mode(s) == -1 && session_get_quantize(s) == 3.0f, "3 beats in 4/4: kept as a number");

    /* 10) the metronome in 7/8: a click every eighth (0.25 s at 120 BPM), the loud one every 7th */
    session_set_quant_mode(s, QUANT_BAR);
    session_set_time_sig(s, 7, 8);
    for (int c = 0; c < s->channel_count; ++c) session_toggle_mute(s, c);   /* only the clicks */
    atomic_store(&s->metronome, true);
    session_toggle_clip(s, 0, 0);
    size_t m0 = tape_len;
    run_to(s, tape_len / (double)SR + 3.6);     /* 2 bars + */
    atomic_store(&s->metronome, false);
    {
        size_t on_at[64]; float pk[64]; int nk = 0;
        for (size_t i = m0; i < tape_len && nk < 64; ) {
            if (fabsf(tape[i]) > 0.02f) {
                float p = 0; for (size_t j = i; j < i + 2000 && j < tape_len; ++j) if (fabsf(tape[j]) > p) p = fabsf(tape[j]);
                on_at[nk] = i; pk[nk] = p; ++nk; i += SR / 10;
            } else ++i;
        }
        CHECK(nk >= 14, "metronome: %d clicks in 3.6 s (want >= 14)", nk);
        bool even = true, acc_ok = true;
        for (int k = 1; k < nk; ++k) if (fabs((double)(on_at[k] - on_at[k - 1]) / SR - 0.25) > 0.003) even = false;
        for (int k = 0; k < nk; ++k) { bool loud = pk[k] > 0.45f; if (loud != (k % 7 == 0)) acc_ok = false; }
        CHECK(even, "metronome: every eighth (0.25 s)");
        CHECK(acc_ok, "metronome: the accent on every 7th click (the 1 of the 7/8 bar)");
    }
    for (int c = 0; c < s->channel_count; ++c) session_toggle_mute(s, c);
    session_stop_all(s);
    run_to(s, tape_len / (double)SR + 0.1);
    /* 12) 1.4.9: the clip's own gain and pan. Channel 1 alone, clip (0,0): 0 dB / C = bit exact, -6 dB, full left */
    {
        session_stop_all(s);
        run_to(s, tape_len / (double)SR + 0.1);
        session_set_quant_mode(s, QUANT_NOW);
        session_toggle_mute(s, 1);
        session_toggle_clip(s, 0, 0);
        float b0[BLK * 2], b1[BLK * 2];
        double e0 = 0, e1 = 0, eL = 0, eR = 0;
        for (int k = 0; k < 40; ++k) mixer_render(s, b0, BLK, 2);      /* past the fade-in */
        for (int k = 0; k < 20; ++k) { mixer_render(s, b0, BLK, 2); for (int i = 0; i < BLK; ++i) e0 += (double)b0[2*i] * b0[2*i]; }
        session_set_clip_gain(s, 0, 0, -6.0f);
        CHECK(fabsf(session_get_clip_gain(s, 0, 0) + 6.0f) < 1e-6, "clip gain set to -6 dB");
        mixer_render(s, b1, BLK, 2);                                  /* the glide */
        for (int k = 0; k < 20; ++k) { mixer_render(s, b1, BLK, 2); for (int i = 0; i < BLK; ++i) e1 += (double)b1[2*i] * b1[2*i]; }
        double db = 10 * log10(e1 / e0);
        CHECK(fabs(db + 6.0) < 0.3, "clip gain -6 dB -> level %.2f dB", db);
        session_set_clip_gain(s, 0, 0, 0.0f);
        session_set_clip_pan(s, 0, 0, -1.0f);
        mixer_render(s, b1, BLK, 2);
        for (int k = 0; k < 20; ++k) { mixer_render(s, b1, BLK, 2); for (int i = 0; i < BLK; ++i) { eL += (double)b1[2*i] * b1[2*i]; eR += (double)b1[2*i+1] * b1[2*i+1]; } }
        CHECK(eR < 1e-9 && fabs(10 * log10(eL / e0)) < 0.3, "clip pan full left: right silent, left as before (%.2f dB)", 10 * log10(eL / e0));
        session_set_clip_pan(s, 0, 0, 0.0f);
        session_set_clip_gain(s, 0, 0, -100.0f);
        CHECK(session_get_clip_gain(s, 0, 0) == CLIP_GAIN_MIN_DB, "clip gain clamps at -60 (= silence)");
        session_set_clip_gain(s, 0, 0, 0.0f);
        for (int k = 0; k < 2; ++k) mixer_render(s, b1, BLK, 2);
        CHECK(project_save(s, "/tmp/qlim_cgain.limproject") == 0, "saved");
        session_set_clip_gain(s, 0, 0, 3.5f); session_set_clip_pan(s, 0, 0, 0.25f);
        CHECK(project_save(s, "/tmp/qlim_cgain2.limproject") == 0 && project_load(s, "/tmp/qlim_cgain2.limproject") == 0, "saved + loaded with gain/pan");
        CHECK(fabsf(session_get_clip_gain(s, 0, 0) - 3.5f) < 0.01f && fabsf(session_get_clip_pan(s, 0, 0) - 0.25f) < 0.002f,
              "clip gain/pan come back from the project (%.2f dB, %.3f)", session_get_clip_gain(s, 0, 0), session_get_clip_pan(s, 0, 0));
        session_set_clip_gain(s, 0, 0, 0.0f); session_set_clip_pan(s, 0, 0, 0.0f);

        /* 13) 1.4.11: loudness normalization - measured in the background, put into effect by session_update_loudness */
        float lu = 0, pk = 0, nd = 0;
        for (int w = 0; w < 400 && !session_clip_loudness(s, 0, 0, &lu, &pk, &nd); ++w) { struct timespec t = { 0, 10000000 }; nanosleep(&t, NULL); }
        CHECK(session_clip_loudness(s, 0, 0, &lu, &pk, &nd) && isfinite(lu), "clip measured: %.2f LUFS, peak %.2f dBFS", lu, pk);
        session_set_loudness(s, -30, false);                  /* a target below it: turned down by (lu + 30) */
        session_clip_loudness(s, 0, 0, &lu, &pk, &nd);
        CHECK(fabsf(nd - (-30.0f - lu)) < 0.02f, "normalization -30 LUFS: %.2f dB (want %.2f)", nd, -30.0f - lu);
        session_toggle_clip(s, 0, 0);
        double e2 = 0;
        /* 2.0.0: in real time (like the sound card) - the disk thread needs a moment to bring the clip in */
        for (int k = 0; k < 41; ++k) { mixer_render(s, b1, BLK, 2); struct timespec t = { 0, (long)BLK * 1000000000L / 48000 }; nanosleep(&t, NULL); }
        for (int k = 0; k < 20; ++k) { mixer_render(s, b1, BLK, 2); for (int i = 0; i < BLK; ++i) e2 += (double)b1[2*i] * b1[2*i]; }
        CHECK(fabs(10 * log10(e2 / e0) - nd) < 0.3, "normalized output %.2f dB (want %.2f)", 10 * log10(e2 / e0), nd);
        session_set_loudness(s, -5, false);                   /* far above: turned up only to -1 dBFS peak */
        session_clip_loudness(s, 0, 0, &lu, &pk, &nd);
        CHECK(fabsf(nd - (-1.0f - pk)) < 0.02f || nd == 0.0f, "turned up only to a -1 dBFS peak (%.2f dB, peak %.2f)", nd, pk);
        session_set_loudness(s, -5, true);
        session_clip_loudness(s, 0, 0, &lu, &pk, &nd);
        CHECK(nd == 0.0f, "down only: never up");
        session_set_loudness(s, 0, false);
        session_clip_loudness(s, 0, 0, &lu, &pk, &nd);
        CHECK(nd == 0.0f, "off: 0 dB");
        session_stop_all(s);
        run_to(s, tape_len / (double)SR + 0.1);
    }

    /* 11) saved and loaded with the project (7/8, 2 Bars) */
    session_set_time_sig(s, 7, 8);
    session_set_quant_mode(s, QUANT_2BARS);
    CHECK(project_save(s, "/tmp/qlim_timesig.limproject") == 0, "project saved");
    session_set_time_sig(s, 4, 4);
    session_set_quant_mode(s, QUANT_NOW);
    CHECK(project_load(s, "/tmp/qlim_timesig.limproject") == 0, "project loaded");
    { int n, d; session_get_time_sig(s, &n, &d);
      CHECK(n == 7 && d == 8 && session_get_quant_mode(s) == QUANT_2BARS && fabs(session_get_quantize(s) - 7.0f) < 1e-6,
            "the project brings back 7/8 and 2 Bars (%d/%d, mode %d, %.2f beats)", n, d, session_get_quant_mode(s), session_get_quantize(s)); }
    session_stop_all(s);
    run_to(s, tape_len / (double)SR + 0.1);
    session_set_time_sig(s, 4, 4);

    session_free(s);
    free(tape);
    printf(fails ? "QUANT: %d FAILED\n" : "QUANT: all ok\n", fails);
    return fails != 0;
}
