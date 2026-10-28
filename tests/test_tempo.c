/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* The whole path: clip -> BPM detection -> warp tick + new tempo -> mixer (stretch) -> detect the output.
 * Plus: saving/loading the project and undo of the tempo. */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/project.h"
#include "../src/undo.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sndfile.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void wait_bpm(Session *s)
{
    for (int i = 0; i < 400; ++i) {
        session_poll_bpm(s);
        if (!session_clip_bpm_busy(s, 0, 0) && !session_clip_bpm_busy(s, 1, 0)) return;
        struct timespec ts = { 0, 20000000L }; nanosleep(&ts, NULL);
    }
}

/* renders secs seconds through the mixer; returns a mono buffer */
static float *render(Session *s, double secs, size_t *n_out)
{
    size_t n = (size_t)(secs * s->sample_rate), blk = 256;
    float *out = malloc(n * sizeof(float));
    float buf[256 * 2];
    for (size_t d = 0; d < n; d += blk) {
        mixer_render(s, buf, blk, 2);
        for (size_t i = 0; i < blk && d + i < n; ++i) out[d + i] = (buf[i * 2] + buf[i * 2 + 1]) * 0.5f;
        { struct timespec ts = { 0, (long)(1e9 * blk / s->sample_rate) }; nanosleep(&ts, NULL); }  /* real time (like a sound card) */
    }
    *n_out = n;
    return out;
}

static double freq_of(const float *y, size_t a, size_t b, int sr)
{
    int zc = 0; size_t first = 0, last = 0;
    for (size_t i = a + 1; i < b; ++i) if (y[i-1] < 0 && y[i] >= 0) { if (!zc) first = i; last = i; zc++; }
    return zc > 1 ? (double)(zc - 1) * sr / (double)(last - first) : 0;
}

int main(int argc, char **argv)
{
    const char *drums = argc > 1 ? argv[1] : "t_128_house.wav";
    const char *tone  = argc > 2 ? argv[2] : "tone.wav";
    static Session S;
    Session *s = &S;
    session_init(s, 48000);
    streams_init();
    session_add_channel(s, NULL); session_add_channel(s, NULL);
    session_add_scene(s);
    CHECK(session_get_tempo(s) == 120.0f, "new session tempo = 120");

    CHECK(session_set_clip(s, 0, 0, drums) == 0, "drum clip loaded");
    CHECK(session_set_clip(s, 1, 0, tone) == 0, "tone clip loaded");
    CHECK(session_clip_bpm_busy(s, 0, 0), "detection runs in background right after loading");
    wait_bpm(s);
    float orig = session_clip_bpm_orig(s, 0, 0);
    CHECK(fabsf(orig - 128.0f) < 0.05f, "detected %.2f BPM (want 128)", orig);
    CHECK(!session_get_clip_warp(s, 0, 0), "tick is OFF by default");
    CHECK(session_clip_speed(&s->channels[0].clips[0]) == 1.0, "tick OFF -> speed 1");

    /* 1) warp tick + 120 BPM -> the mixer must give 120 */
    session_set_clip_warp(s, 0, 0, true);
    session_set_tempo(s, 120.0f);
    CHECK(fabsf(session_clip_bpm_play(s, 0, 0) - 120.0f) < 0.001f && fabsf(session_clip_bpm_orig(s, 0, 0) - 128.0f) < 0.05f,
          "tick ON: clip follows master 120, original stays %.2f", session_clip_bpm_orig(s, 0, 0));
    atomic_store(&s->audio_running, false);
    session_toggle_clip(s, 0, 0);
    { struct timespec ts = { 0, 100000000L }; nanosleep(&ts, NULL); }
    size_t n; float *y = render(s, 24.0, &n);
    float got = bpm_detect_mono(y, n, s->sample_rate);
    CHECK(fabsf(got - 120.0f) < 0.1f, "rendered output tempo %.2f BPM (want 120) [%s]", got, stretch_engine_name());
    CHECK(stretch_underruns(s->channels[0].clips[0].stretch) == 0, "no stretch underruns (%u)", stretch_underruns(s->channels[0].clips[0].stretch));
    size_t pos = atomic_load(&s->channels[0].clips[0].position);
    double expect = 24.0 * 48000 * (120.0 / 128.0);
    CHECK(fabs((double)pos - expect) < 48000 * 0.08, "playhead at %.2f s of file (want ~%.2f s)", pos / 48000.0, expect / 48000.0);
    free(y);
    session_stop_all(s);

    /* 2) a 440 Hz tone at 100 BPM, played at 125 BPM -> the pitch stays 440 */
    session_set_clip_bpm_fields(s, 1, 0, 100.0f, 0, 0);
    session_set_clip_warp(s, 1, 0, true);
    session_set_tempo(s, 125.0f);
    session_toggle_clip(s, 1, 0);
    { struct timespec ts = { 0, 100000000L }; nanosleep(&ts, NULL); }
    y = render(s, 3.0, &n);
    double f = freq_of(y, 48000/2, n, 48000);
    CHECK(fabs(f - 440.0) < 1.5, "1.25x faster, pitch still %.1f Hz", f);
    free(y);
    /* warp tick turned off while playing -> speed 1 */
    session_set_clip_warp(s, 1, 0, false);
    CHECK(session_clip_speed(&s->channels[1].clips[0]) == 1.0, "tick OFF while playing -> speed back to 1");
    y = render(s, 1.0, &n); free(y);
    session_stop_all(s);

    /* 3) the clip field = correction of the original (e.g. 87 -> 174) */
    session_set_clip_bpm_value(s, 1, 0, 174.0f);
    CHECK(fabsf(session_clip_bpm_orig(s, 1, 0) - 174.0f) < 0.001f && session_clip_speed(&s->channels[1].clips[0]) == 1.0,
          "tick OFF: edit corrects the original tempo (174), speed unchanged");

    /* 4) project tempo: limits and rounding */
    session_set_tempo(s, 127.456f);
    CHECK(fabsf(session_get_tempo(s) - 127.46f) < 0.001f, "tempo rounds to 0.01 (%.2f)", session_get_tempo(s));
    session_set_tempo(s, 5000.0f);
    CHECK(session_get_tempo(s) == BPM_MAX, "tempo clamps to max");
    session_set_tempo(s, 126.0f);

    /* 5) save and load */
    session_set_clip_warp(s, 0, 0, true);
    const char *pp = "/tmp/qlim_tempo_test.limproject";
    CHECK(project_save(s, pp) == 0, "project saved");
    project_new(s);
    CHECK(session_get_tempo(s) == 120.0f, "new project resets tempo to 120");
    CHECK(project_load(s, pp) == 0, "project loaded");
    CHECK(session_get_tempo(s) == 126.0f, "tempo restored (%.2f)", session_get_tempo(s));
    CHECK(session_get_clip_warp(s, 0, 0) && fabsf(session_clip_bpm_play(s, 0, 0) - 126.0f) < 0.001f,
          "clip tick restored, plays at master (%.2f)", session_clip_bpm_play(s, 0, 0));
    CHECK(!session_clip_bpm_busy(s, 0, 0) && fabsf(session_clip_bpm_orig(s, 0, 0) - 128.0f) < 0.05f,
          "detected tempo comes from the file, no re-analysis (%.2f)", session_clip_bpm_orig(s, 0, 0));
    CHECK(fabsf(session_clip_bpm_orig(s, 1, 0) - 174.0f) < 0.001f, "corrected original restored");

    /* 6) undo of the tempo and of the warp tick */
    undo_reset(s);
    session_set_tempo(s, 140.0f);
    undo_commit_now(s);
    session_set_clip_warp(s, 0, 0, false);
    undo_commit_now(s);
    CHECK(undo_undo(s) && session_get_clip_warp(s, 0, 0), "undo brings the tick back");
    CHECK(undo_undo(s) && session_get_tempo(s) == 126.0f, "undo restores tempo (%.2f)", session_get_tempo(s));
    CHECK(undo_redo(s) && session_get_tempo(s) == 140.0f, "redo tempo");
    CHECK(fabsf(session_clip_bpm_orig(s, 0, 0) - 128.0f) < 0.05f, "undo keeps detected tempo");

    /* a detected tempo arriving after load -> must not become an undo step */
    undo_reset(s);
    session_set_clip(s, 1, 0, drums);
    undo_commit_now(s);
    int d0 = undo_depth();
    wait_bpm(s);
    undo_commit_now(s);
    CHECK(undo_depth() == d0, "finished detection does not create an undo step");

    session_free(s);
    printf(fails ? "TEMPO: %d FAILED\n" : "TEMPO: all ok\n", fails);
    return fails != 0;
}
