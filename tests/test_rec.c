/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4: recording. The input is a "frame counter" (sample = frame number x 1e-6), so the file shows exactly which frames
 * were recorded: the start and the stop on the beat, the card's delay compensated, the clip then plays on in time.
 * Also: a quantized start while something plays, mono input, monitoring, MIDI recording (notes at the right beats,
 * the clip plays on at once), cancel before the start, Stop all. */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/midiq.h"
#include <math.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define SR 48000
#define BLK 256
static unsigned long long g_frame;               /* the frame number of the input */
static float g_out_peak;

static void run(Session *s, double sec, bool with_input)
{
    unsigned long long end = g_frame + (unsigned long long)(sec * SR);
    float out[BLK * 2], in[BLK * 4];
    g_out_peak = 0;
    while (g_frame < end) {
        for (int i = 0; i < BLK; ++i) {          /* 4 inputs: 1 = counter, 2 = -counter, 3 = 0.25, 4 = -0.25 */
            float v = (float)((g_frame + (unsigned long long)i) % 1000000ull) * 1e-6f;
            in[i * 4 + 0] = v; in[i * 4 + 1] = -v; in[i * 4 + 2] = 0.25f; in[i * 4 + 3] = -0.25f;
        }
        if (with_input) mixer_set_input(in, 4, BLK);
        mixer_render(s, out, BLK, 2);
        for (int i = 0; i < BLK * 2; ++i) if (fabsf(out[i]) > g_out_peak) g_out_peak = fabsf(out[i]);
        g_frame += BLK;
        session_quant_tick(s); session_midi_tick(s); session_rec_tick(s);
    }
}

static float *read_wav(const char *path, long *frames, int *chans)
{
    SF_INFO info; memset(&info, 0, sizeof info);
    SNDFILE *f = sf_open(path, SFM_READ, &info);
    if (!f) { *frames = 0; return NULL; }
    float *b = malloc(sizeof(float) * (size_t)info.frames * (size_t)info.channels);
    sf_readf_float(f, b, info.frames);
    sf_close(f);
    *frames = (long)info.frames; *chans = info.channels;
    return b;
}

int main(void)
{
    if (system("mkdir -p /tmp/qlim_test_audio && rm -f /tmp/qlim_test_audio/rec*.wav") != 0) return 2;
    static Session S; Session *s = &S;
    session_init(s, SR);
    streams_init();
    for (int c = 0; c < 3; ++c) session_add_channel(s, NULL);
    for (int i = 0; i < 4; ++i) session_add_scene(s);
    atomic_store(&s->audio_running, false);
    session_set_quantize(s, 4.0f);
    session_set_tempo(s, 120.0f);                  /* 1 bar = 2 s = 96000 frames */
    char err[200];

    /* 1) not armed -> refused */
    CHECK(session_rec_cell(s, 0, 0, false, "/tmp/qlim_test_audio/rec1.wav", 480, err, sizeof err) != 0, "not armed: refused (%s)", err);
    session_toggle_arm(s, 0);
    CHECK(session_is_armed(s, 0), "armed");

    /* 2) nothing plays -> starts at once; stop pressed in bar 1 -> stops exactly at the end of bar 1 */
    unsigned long long start = g_frame;
    CHECK(session_rec_cell(s, 0, 0, false, "/tmp/qlim_test_audio/rec1.wav", 480, err, sizeof err) == 0, "recording starts (%s)", err);
    run(s, 1.3, true);
    double beats = 0;
    CHECK(session_rec_state(s, 0, 0, &beats) == REC_ON && fabs(beats - 2.6) < 0.02, "recording, %.2f beats", beats);
    CHECK(session_rec_cell(s, 0, 0, false, NULL, 0, err, sizeof err) == 0 && session_rec_state(s, 0, 0, NULL) == REC_STOPPING,
          "stop pressed -> waits for the bar");
    run(s, 0.6, true);                             /* 1.9 s */
    size_t pos0 = 0; double since0 = 0;
    for (int k = 0; k < 400 && session_rec_state(s, 0, 0, NULL) != REC_IDLE; ++k) {   /* block by block until it is a clip */
        run(s, (double)BLK / SR, true);
        if (session_rec_state(s, 0, 0, NULL) == REC_IDLE) {
            pos0 = atomic_load(&s->channels[0].clips[0].position);
            since0 = (double)(g_frame - (start + 96000));
        }
    }
    CHECK(fabs((double)pos0 - since0) <= 2 * BLK, "the clip starts in time (position %zu, time since the bar %.0f)", pos0, since0);
    run(s, 0.2, true);
    CHECK(session_rec_state(s, 0, 0, NULL) == REC_IDLE && atomic_load(&s->channels[0].clips[0].has_clip), "done -> a clip in the cell");
    Clip *cl = &s->channels[0].clips[0];
    CHECK(atomic_load(&cl->playing) && atomic_load(&cl->loop), "it plays on, looped");
    long fr; int chn;
    float *w = read_wav("/tmp/qlim_test_audio/rec1.wav", &fr, &chn);
    CHECK(w && fr == 96000 && chn == 2, "exactly one bar recorded (%ld frames)", fr);
    if (w && fr > 10) {
        double first = w[0] * 1e6, last = w[(fr - 1) * 2] * 1e6;
        CHECK(fabs(first - (double)(start + 480)) < 1.0, "the delay of the card is compensated: the first frame is input frame %.0f (want %llu)",
              first, start + 480);
        CHECK(fabs(last - (double)(start + 480 + 95999)) < 1.0, "and the last one %.0f (want %llu)", last, start + 480 + 95999);
        CHECK(fabs(w[1] + w[0]) < 1e-6, "input 2 on the right");
        bool cont = true;
        for (long i = 1; i < fr; ++i) if (fabs((w[i * 2] - w[(i - 1) * 2]) * 1e6 - 1.0) > 0.6) { cont = false; printf("    gap at %ld\n", i); break; }
        CHECK(cont, "no frame lost or doubled");
    }
    free(w);
    CHECK(fabs(session_clip_bpm_orig(s, 0, 0) - 120.0f) < 0.01f, "its tempo is the master tempo (%.2f)", session_clip_bpm_orig(s, 0, 0));

    /* 3) while something plays: the start waits for the next bar; mono input 3 */
    session_set_rec_input(s, 0, 2, false);
    run(s, 0.5, true);
    double tb; session_transport(s, &tb);
    unsigned long long bar = g_frame + (unsigned long long)((ceil(tb / 4.0) * 4.0 - tb) * SR / 2.0 + 0.5);
    CHECK(session_rec_cell(s, 0, 1, false, "/tmp/qlim_test_audio/rec2.wav", 0, err, sizeof err) == 0 && session_rec_state(s, 0, 1, NULL) == REC_WAIT,
          "second recording waits for the bar");
    run(s, 2.1, true);
    CHECK(session_rec_state(s, 0, 1, NULL) == REC_ON, "now recording");
    session_rec_cell(s, 0, 1, false, NULL, 0, err, sizeof err);
    run(s, 2.2, true);
    w = read_wav("/tmp/qlim_test_audio/rec2.wav", &fr, &chn);
    CHECK(w && fr == 96000, "one bar (%ld)", fr);
    CHECK(w && fabsf(w[0] - 0.25f) < 1e-4f && fabsf(w[1] - 0.25f) < 1e-4f, "mono input 3 on both sides (%.3f %.3f)", w ? w[0] : 0, w ? w[1] : 0);
    (void)bar;
    free(w);

    /* 4) cancel before the start: nothing left */
    run(s, 0.3, true);
    CHECK(session_rec_cell(s, 0, 2, false, "/tmp/qlim_test_audio/rec3.wav", 0, err, sizeof err) == 0 && session_rec_state(s, 0, 2, NULL) == REC_WAIT, "third waits");
    session_rec_cell(s, 0, 2, false, NULL, 0, err, sizeof err);
    CHECK(session_rec_state(s, 0, 2, NULL) == REC_IDLE && !atomic_load(&s->channels[0].clips[2].has_clip), "cancelled, the cell stays empty");
    FILE *t3 = fopen("/tmp/qlim_test_audio/rec3.wav", "rb");
    CHECK(!t3, "and no file left behind");
    if (t3) fclose(t3);

    /* 5) monitoring: the input is heard through the channel */
    session_stop_all(s);
    run(s, 0.3, true);
    session_set_rec_input(s, 1, 2, true);         /* inputs 3+4 = +0.25 / -0.25 */
    session_toggle_arm(s, 1);
    run(s, 0.2, true);
    CHECK(g_out_peak < 1e-6, "armed without monitoring: silence");
    session_set_monitor(s, 1, true);
    run(s, 0.3, true);
    CHECK(g_out_peak > 0.05f, "monitoring on: the input is heard (%.3f)", g_out_peak);
    run(s, 0.2, false);
    CHECK(g_out_peak < 1e-6, "no input from the card: silence");
    session_set_monitor(s, 1, false);

    /* 6) MIDI recording: notes played on channel 3 become a MIDI clip; it plays on at once */
    session_toggle_arm(s, 2);
    session_set_midi_target(s, 2);
    CHECK(session_rec_cell(s, 2, 0, true, NULL, 0, err, sizeof err) == 0, "MIDI recording starts (%s)", err);
    run(s, 0.5, true);                               /* beat 1 */
    midiq_push(MIDIQ_UI, 0x90, 60, 100);
    run(s, 0.25, true);                              /* half a beat */
    midiq_push(MIDIQ_UI, 0x80, 60, 0);
    run(s, 0.5, true);                               /* beat 2.5 */
    midiq_push(MIDIQ_UI, 0x90, 64, 80);              /* held over the end */
    run(s, 0.3, true);
    const MidiSeq *live = session_midi_get(s, 2, 0);
    CHECK(live && live->n == 1, "the first note appears while recording (%d)", live ? live->n : -1);
    session_rec_cell(s, 2, 0, true, NULL, 0, err, sizeof err);   /* stop -> at the bar (beat 4) */
    run(s, 0.6, true);
    run(s, 0.1, true);
    const MidiSeq *m = session_midi_get(s, 2, 0);
    CHECK(session_rec_state(s, 2, 0, NULL) == REC_IDLE && m, "MIDI recording done");
    if (m) {
        CHECK(m->length == 4.0f, "length 1 bar (%.3f)", (double)m->length);
        CHECK(m->n == 2, "2 notes (%d)", m->n);
        if (m->n == 2) {
            CHECK(fabsf(m->note[0].start - 1.0f) < 0.02f && fabsf(m->note[0].len - 0.5f) < 0.02f && m->note[0].key == 60 && m->note[0].vel == 100,
                  "note 1: beat %.3f, %.3f long", (double)m->note[0].start, (double)m->note[0].len);
            CHECK(fabsf(m->note[1].start - 2.5f) < 0.02f && fabsf(m->note[1].start + m->note[1].len - 4.0f) < 0.02f && m->note[1].key == 64,
                  "note 2 (held over the end) ends at the end (%.3f + %.3f)", (double)m->note[1].start, (double)m->note[1].len);
        }
    }
    CHECK(atomic_load(&s->channels[2].clips[0].playing), "the MIDI clip plays on at once");
    midiq_push(MIDIQ_UI, 0x80, 64, 0);
    double ph = session_midi_playhead(s, 2, 0);
    CHECK(fabs(ph - 0.5) < 0.06, "and it is in time (playhead %.3f beats, 0.25 s = 0.5 beats after the stop)", ph);

    /* 7) Stop all while recording: it ends at once, the cell gets what was recorded */
    session_stop_all(s);
    run(s, 0.2, true);
    session_set_rec_input(s, 0, 0, true);
    CHECK(session_rec_cell(s, 0, 3, false, "/tmp/qlim_test_audio/rec4.wav", 0, err, sizeof err) == 0, "fourth starts");
    run(s, 0.7, true);
    session_stop_all(s);
    run(s, 0.1, true);
    w = read_wav("/tmp/qlim_test_audio/rec4.wav", &fr, &chn);
    CHECK(session_rec_state(s, 0, 3, NULL) == REC_IDLE && w && fr > 30000 && fr < 34600, "Stop all ends it at once (%ld frames)", fr);
    free(w);

    session_free(s);
    printf(fails ? "REC: %d FAILED\n" : "REC: all ok\n", fails);
    return fails != 0;
}
