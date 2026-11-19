/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.17: scene transitions (fade out what plays, fade in the next scene - equal power), "back", "next" skipping
 * empty scenes, "Stop automations" (hold where it is, then go on), the fade time; launches at a clock time
 * (on the second, not quantized; held -> missed; daily; they follow a moved clip; the project keeps them). */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/project.h"
#include "../src/schedule.h"
#include "../src/stream.h"
#include "../src/export.h"
#include <math.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void nap(long ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
#define SR 48000

static void tone(const char *p, double f, double secs)
{
    SF_INFO i = { 0 };
    i.samplerate = SR; i.channels = 2; i.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
    SNDFILE *o = sf_open(p, SFM_WRITE, &i);
    long n = (long)(secs * SR);
    float *b = malloc((size_t)n * 2 * sizeof(float));
    for (long k = 0; k < n; ++k) b[k * 2] = b[k * 2 + 1] = 0.3f * (float)sin(2 * M_PI * f * k / SR);
    sf_writef_float(o, b, n);
    sf_close(o);
    free(b);
}

/* renders `secs`; returns the amount of each frequency over windows of 50 ms */
static Session S;
static double amp(const float *o, long from, long len, double f)
{
    double re = 0, im = 0;
    for (long k = 0; k < len; ++k) { double w = 2 * M_PI * f * (from + k) / SR; re += o[(from + k) * 2] * cos(w); im += o[(from + k) * 2] * sin(w); }
    return 2.0 * sqrt(re * re + im * im) / (double)len;
}
static float *run(double secs, long *n)
{
    long total = (long)(secs * SR);
    float *out = calloc((size_t)(total + 256) * 2, sizeof(float));
    for (long p = 0; p < total; p += 256) { nap(2); mixer_render(&S, out + p * 2, 256, 2); }
    *n = total;
    return out;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (system("rm -rf /tmp/qlim_tr && mkdir -p /tmp/qlim_tr") != 0) return 2;
    tone("/tmp/qlim_tr/a.wav", 500.0, 30.0);       /* scene 1 */
    tone("/tmp/qlim_tr/b.wav", 1500.0, 30.0);      /* scene 3 (scene 2 is empty) */
    tone("/tmp/qlim_tr/c.wav", 3000.0, 30.0);      /* scene 4 */
    streams_init();
    Session *s = &S;
    session_init(s, SR);
    atomic_store(&s->audio_running, false);
    session_add_channel(s, "A");
    session_add_channel(s, "B");
    for (int i = 0; i < 4; ++i) session_add_scene(s);
    session_set_quant_mode(s, 0);                   /* (a plain launch at once: the test is about the fades) */
    CHECK(session_set_clip(s, 0, 0, "/tmp/qlim_tr/a.wav") == 0 && session_set_clip(s, 1, 2, "/tmp/qlim_tr/b.wav") == 0 &&
          session_set_clip(s, 0, 3, "/tmp/qlim_tr/c.wav") == 0, "three clips: scene 1 (ch A), scene 3 (ch B), scene 4 (ch A)");
    for (int w = 0; w < 400 && !(s->channels[0].clips[0].stream && s->channels[1].clips[2].stream && s->channels[0].clips[3].stream); ++w) nap(5);
    CHECK(fabsf(session_get_transition_sec(s) - 4.0f) < 0.01f, "the fade time is 4 s at first");
    session_set_transition_sec(s, 1.0f);
    session_set_transition_sec(s, 500.0f);
    CHECK(session_get_transition_sec(s) == TR_SEC_MAX, "it is limited (%.0f s max)", (double)TR_SEC_MAX);
    session_set_transition_sec(s, 1.0f);

    session_play_scene(s, 0);
    long n;
    float *o = run(0.5, &n);
    CHECK(amp(o, n - 4800, 4800, 500) > 0.25 && session_current_scene(s) == 0, "scene 1 plays (%.3f)", amp(o, n - 4800, 4800, 500));
    free(o);

    /* 1) next: scene 2 is empty -> scene 3; 1 s equal-power transition */
    CHECK(session_transition_next(s) == 0 && session_current_scene(s) == 2, "next = scene 3 (the empty scene 2 is skipped)");
    o = run(1.4, &n);
    double a0 = amp(o, 0, 2400, 500), b0 = amp(o, 0, 2400, 1500);
    double am = amp(o, 21600, 4800, 500), bm = amp(o, 21600, 4800, 1500);     /* around 0.5 s */
    double ae = amp(o, n - 9600, 4800, 500), be = amp(o, n - 9600, 4800, 1500);
    CHECK(a0 > 0.25 && b0 < 0.08, "at the start: A full (%.3f), B almost silent (%.3f)", a0, b0);
    CHECK(fabs(am - bm) < 0.04 && fabs(am * am + bm * bm - 0.09) < 0.012, "in the middle: both ~0.707 of full, equal power (%.3f / %.3f)", am, bm);
    CHECK(ae < 0.003 && be > 0.29, "at the end: A gone (%.4f), B full (%.3f)", ae, be);
    CHECK(!atomic_load(&s->channels[0].clips[0].playing) && atomic_load(&s->channels[1].clips[2].playing), "and A has stopped by itself");
    free(o);
    nap(1100);                                       /* (the progress shown follows the clock; the test renders faster) */
    CHECK(session_transition_progress(s) < 0, "the transition is over (the top bar shows it)");

    /* 2) stop automations in the middle: it stands still; then it goes on */
    session_set_transition_sec(s, 2.0f);
    CHECK(session_transition_next(s) == 0 && session_current_scene(s) == 3, "next = scene 4 (2 s)");
    o = run(1.0, &n);
    free(o);
    session_automation_hold(s, true);
    CHECK(session_automation_held(s), "automations stopped half way");
    float p1 = session_transition_progress(s);
    o = run(1.5, &n);
    double h1 = amp(o, 4800, 4800, 3000), h2 = amp(o, n - 4800, 4800, 3000), hb = amp(o, n - 4800, 4800, 1500);
    CHECK(fabs(h1 - h2) < 0.01 && h1 > 0.1 && h1 < 0.29 && hb > 0.1, "held: both stay where they are (scene 4 %.3f -> %.3f, scene 3 %.3f)", h1, h2, hb);
    CHECK(fabsf(session_transition_progress(s) - p1) < 0.01f, "the progress stands still too (%.2f)", (double)p1);
    free(o);
    session_automation_hold(s, false);
    o = run(1.4, &n);
    CHECK(amp(o, n - 4800, 4800, 3000) > 0.29 && amp(o, n - 4800, 4800, 1500) < 0.003, "then it goes on to the end (%.3f)", amp(o, n - 4800, 4800, 3000));
    free(o);

    /* 3) back: to scene 3 again; and once more back: scene 1 */
    session_set_transition_sec(s, 0.5f);
    CHECK(session_transition_back(s) == 0 && session_current_scene(s) == 2, "back = scene 3");
    o = run(0.8, &n);
    CHECK(amp(o, n - 4800, 4800, 1500) > 0.29 && amp(o, n - 4800, 4800, 3000) < 0.003, "scene 3 is back (%.3f)", amp(o, n - 4800, 4800, 1500));
    free(o);
    CHECK(session_transition_back(s) == 0 && session_current_scene(s) == 0, "back again = scene 1");
    o = run(0.8, &n);
    CHECK(amp(o, n - 4800, 4800, 500) > 0.29 && amp(o, n - 4800, 4800, 1500) < 0.003, "scene 1 plays again (%.3f)", amp(o, n - 4800, 4800, 500));
    free(o);
    CHECK(session_transition_back(s) == 2, "nothing more to go back to");
    /* a manual launch in the middle of a fade: full level at once (the fade is cancelled) */
    session_set_transition_sec(s, 3.0f);
    session_transition_to(s, 2);
    o = run(0.3, &n); free(o);
    session_stop_all(s);
    session_play_scene(s, 2);
    o = run(0.4, &n);
    CHECK(amp(o, n - 4800, 4800, 1500) > 0.29, "a manual launch during a fade plays at full level (%.3f)", amp(o, n - 4800, 4800, 1500));
    free(o);
    CHECK(session_transition_next(s) == 0 && session_current_scene(s) == 3, "next from there = scene 4");
    session_stop_all(s);
    o = run(0.2, &n); free(o);

    /* 4) launches at a clock time: the clip of scene 4 at the next second + 2 */
    session_set_quant_mode(s, 1);                   /* (quantized: a launch at a time must ignore it) */
    double now = sched_now();
    int hh, mm, ss;
    sched_clock(now + 2.0, &hh, &mm, &ss);
    int i = sched_add(SCHED_CLIP, 0, 3, hh, mm, ss, false, false);
    CHECK(i == 0 && sched_count() == 1 && sched_find(SCHED_CLIP, 0, 3) == 0, "a launch at %02d:%02d:%02d is planned", hh, mm, ss);
    double left = sched_seconds_left(0);
    CHECK(left > 0.9 && left <= 2.0, "%.2f s before it", left);
    CHECK(sched_tick(s) == 0 && !atomic_load(&s->channels[0].clips[3].playing), "not yet");
    while (sched_now() < sched_get(0)->at) nap(1);
    double t_fire = sched_now();
    CHECK(sched_tick(s) == 1 && atomic_load(&s->channels[0].clips[3].playing), "on the second it starts (%.1f ms late)", (t_fire - floor(t_fire)) * 1000.0);
    CHECK(session_clip_launch_state(s, 0, 3) == 0, "at once, not waiting for the bar (the launch quantization is on)");
    CHECK(sched_count() == 0, "a one-time launch is gone after it started");
    session_stop_all(s);

    /* 5) a scene with the transition; and "Stop automations" -> missed */
    session_set_transition_sec(s, 0.5f);
    sched_clock(sched_now() + 1.0, &hh, &mm, &ss);
    sched_add(SCHED_SCENE, -1, 2, hh, mm, ss, true, false);
    while (sched_now() < sched_get(0)->at) nap(1);
    CHECK(sched_tick(s) == 1 && session_current_scene(s) == 2 && session_transition_progress(s) >= 0.0f, "scene 3 starts on the second, with the transition");
    o = run(0.8, &n);
    CHECK(amp(o, n - 4800, 4800, 1500) > 0.29, "and it is heard (%.3f)", amp(o, n - 4800, 4800, 1500));
    free(o);
    session_automation_hold(s, true);
    sched_clock(sched_now() + 1.0, &hh, &mm, &ss);
    sched_add(SCHED_SCENE, -1, 0, hh, mm, ss, true, false);
    while (sched_now() < sched_get(0)->at) nap(1);
    CHECK(sched_tick(s) == 0 && sched_get(0)->state == SCHED_MISSED && session_current_scene(s) == 2, "held: it does not start - it is \"missed\"");
    session_automation_hold(s, false);
    CHECK(sched_tick(s) == 0, "and does not start late when the automations go on");
    sched_clear();

    /* 6) daily; it follows a moved clip; a deleted scene takes it along; the project keeps it */
    sched_add(SCHED_CLIP, 0, 3, 7, 30, 0, false, true);
    sched_add(SCHED_SCENE, -1, 1, 22, 0, 5, true, false);
    double d = sched_seconds_left(0);
    CHECK(d > 0 && d <= 86400.0, "07:30:00 daily: the next one is within a day (%.0f s)", d);
    session_move_clip(s, 0, 3, 1, 1);
    CHECK(sched_find(SCHED_CLIP, 1, 1) == 0 && sched_find(SCHED_CLIP, 0, 3) < 0, "the clip was moved: its launch went with it");
    CHECK(project_save(s, "/tmp/qlim_tr/p.limproject") == 0, "saved");
    sched_clear();
    static Session T;
    session_init(&T, SR);
    atomic_store(&T.audio_running, false);
    CHECK(project_load(&T, "/tmp/qlim_tr/p.limproject") == 0 && sched_count() == 2, "loaded: %d launches at a time", sched_count());
    const SchedItem *a = sched_get(sched_find(SCHED_CLIP, 1, 1)), *b = sched_get(sched_find(SCHED_SCENE, -1, 1));
    CHECK(a && a->hh == 7 && a->mm == 30 && a->daily && b && b->hh == 22 && b->ss == 5 && b->fade && !b->daily,
          "the same times, fade and daily");
    CHECK(fabsf(session_get_transition_sec(&T) - 0.5f) < 0.01f, "the fade time is kept (%.1f s)", (double)session_get_transition_sec(&T));
    session_delete_scene(&T, 0);
    CHECK(sched_find(SCHED_SCENE, -1, 0) >= 0 && sched_find(SCHED_CLIP, 1, 0) >= 0, "scene 1 deleted: the others moved up with their launches");
    session_delete_scene(&T, 0);
    CHECK(sched_count() == 0, "their scene deleted: they are gone");

    /* 7) Export Scene: the longest clip without loop sets the length; recorded from the first sound of the scene */
    {
        static Session E;
        session_init(&E, SR);
        atomic_store(&E.audio_running, false);
        session_add_channel(&E, "X"); session_add_channel(&E, "Y");
        session_add_scene(&E);
        tone("/tmp/qlim_tr/short.wav", 700.0, 1.5);
        tone("/tmp/qlim_tr/loop.wav", 300.0, 0.5);
        session_set_clip(&E, 0, 0, "/tmp/qlim_tr/short.wav");
        session_set_clip(&E, 1, 0, "/tmp/qlim_tr/loop.wav");
        for (int w = 0; w < 400 && !(E.channels[0].clips[0].stream && E.channels[1].clips[0].stream); ++w) nap(5);
        session_toggle_loop(&E, 1, 0);
        int nc; bool al; char lg[256];
        double len = session_scene_length(&E, 0, &nc, lg, sizeof lg, &al);
        CHECK(nc == 2 && fabs(len - 1.5) < 0.001 && !al && strstr(lg, "short"), "scene length = the clip without loop: %.3f s (%s)", len, lg);
        atomic_store(&E.metronome, true);                 /* (must not be in the export) */
        ExportOpts eo = { EXP_WAV, 32, 0, 0, 0 };
        char err[300] = "";
        ExportJob *j = export_scene_start(&E, 0, len, &eo, "/tmp/qlim_tr/scene.wav", err, sizeof err);
        CHECK(j != NULL, "Export Scene starts (%s)", err);
        float blk[256 * 2];
        for (int k = 0; j && !export_finished(j) && k < 4000; ++k) { mixer_render(&E, blk, 256, 2); nap(1); }  /* the "sound card" */
        char msg[400] = "";
        int rc = j ? export_result(j, msg, sizeof msg) : 1;
        export_free(j);
        CHECK(rc == 0, "exported (%s)", msg);
        SF_INFO si = { 0 };
        SNDFILE *f = sf_open("/tmp/qlim_tr/scene.wav", SFM_READ, &si);
        long n2 = f ? (long)si.frames : 0;
        float *b = f ? malloc((size_t)n2 * 2 * sizeof(float)) : NULL;
        if (f) { sf_readf_float(f, b, n2); sf_close(f); }
        CHECK(n2 == (long)(1.5 * SR + 0.5), "exactly 1.5 s long (%ld frames)", n2);
        double a7 = b ? amp(b, SR / 2, 4800, 700) : 0, a3 = b ? amp(b, SR, 4800, 300) : 0, a1 = b ? amp(b, SR / 2, 4800, 1000) : 0;
        double first = b ? fabs(b[200]) + fabs(b[400]) : 0;
        CHECK(a7 > 0.1 && a3 > 0.1, "both clips are in it, the loop too (%.3f / %.3f)", a7, a3);
        CHECK(first > 0.001, "it starts with the sound, not with silence");
        CHECK(a1 < 0.01, "no metronome click in it (%.4f at 1 kHz)", a1);
        free(b);
        CHECK(atomic_load(&E.capture) == NULL, "the recording is closed");
        session_toggle_loop(&E, 0, 0);
        len = session_scene_length(&E, 0, &nc, lg, sizeof lg, &al);
        CHECK(al && fabs(len - 1.5) < 0.001, "all loop: one pass of the longest (%.3f s)", len);
    }

    printf("TRANSITION: %s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
