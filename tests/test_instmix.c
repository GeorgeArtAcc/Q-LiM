/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.2: instruments in the mixer - the MIDI keyboard plays the armed channel only; a note held while another channel
 * is armed still ends on its own channel; pads bound to clips do not play notes; panic; the instrument's settings
 * survive a project save + load. Uses the DPF test instruments Kars (CLAP) and Nekobi (VST3).
 * Argument: the folder with Kars.clap and Nekobi.vst3 */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/fxchain.h"
#include "../src/midiq.h"
#include "../src/project.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void nap(long us) { struct timespec t = { us / 1000000, (us % 1000000) * 1000 }; nanosleep(&t, NULL); }
#define BLK 256

/* renders 'blocks' blocks; returns the loudest block peak of every channel */
static void run(Session *s, int blocks, float pk[3])
{
    static float out[BLK * 2];
    for (int c = 0; c < 3; ++c) pk[c] = 0;
    for (int b = 0; b < blocks; ++b) {
        for (int c = 0; c < 3; ++c) atomic_store(&s->channels[c].peak_level, 0.0f);
        mixer_render(s, out, BLK, 2);
        for (int c = 0; c < 3; ++c) { float v = atomic_load(&s->channels[c].peak_level); if (v > pk[c]) pk[c] = v; }
    }
}
static void wait_loaded(Session *s)
{
    for (int i = 0; i < 400; ++i) {
        fxchain_sync(s);
        if (fxchain_has_instrument(s, 0) && fxchain_has_instrument(s, 1)) return;
        nap(5000);
    }
}
static void key(int on, int note, int vel) { midiq_push(MIDIQ_UI, (unsigned char)(on ? 0x90 : 0x80), (unsigned char)note, (unsigned char)vel); }

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: test_instmix <folder with Kars.clap, Nekobi.vst3>\n"); return 2; }
    char kars[1200], neko[1200];
    snprintf(kars, sizeof kars, "%s/Kars.clap|studio.kx.distrho.Kars", argv[1]);
    snprintf(neko, sizeof neko, "%s/Nekobi.vst3|2046504473616C636B654E4400000000", argv[1]);
    static Session S; Session *s = &S;
    session_init(s, 48000);
    streams_init();
    for (int c = 0; c < 3; ++c) session_add_channel(s, NULL);
    session_add_scene(s);
    atomic_store(&s->audio_running, false);
    session_fx_add(s, 0, "CLAP", "Kars", kars);
    session_fx_add(s, 1, "VST3", "Nekobi", neko);
    wait_loaded(s);
    CHECK(fxchain_has_instrument(s, 0) && fxchain_has_instrument(s, 1) && !fxchain_has_instrument(s, 2), "two instrument channels + one audio channel");
    float pk[3];
    run(s, 10, pk);
    CHECK(pk[0] < 1e-6 && pk[1] < 1e-6, "silence before any note");

    /* nothing armed -> the keyboard plays nothing */
    key(1, 60, 100);
    run(s, 20, pk);
    CHECK(pk[0] < 1e-6 && pk[1] < 1e-6, "no armed channel -> no sound");
    key(0, 60, 0);

    /* arm channel 1 (Kars) -> only it sounds */
    session_set_midi_target(s, 0);
    key(1, 60, 110);
    run(s, 20, pk);
    CHECK(pk[0] > 0.01 && pk[1] < 1e-6 && pk[2] < 1e-6, "armed channel 1 sounds (%.3f), the others silent (%.4f %.4f)", pk[0], pk[1], pk[2]);

    /* arm channel 2 while the note is still held: the note-off still reaches channel 1 (no hanging note) */
    session_set_midi_target(s, 1);
    key(1, 64, 110);
    run(s, 20, pk);
    CHECK(pk[1] > 0.01, "channel 2 (Nekobi) plays the new note (%.3f)", pk[1]);
    key(0, 60, 0);                              /* key up of the first note */
    key(0, 64, 0);
    run(s, 300, pk);                            /* ~1.6 s for the release */
    run(s, 10, pk);
    CHECK(pk[0] < 0.002 && pk[1] < 0.002, "both notes end on their own channels (%.4f %.4f)", pk[0], pk[1]);

    /* a pad bound to a clip (note 36) launches the clip, it must not play the instrument */
    session_set_midi_target(s, 0);
    session_set_clip_bind(s, 2, 0, MIDI_BIND_MAKE(MIDI_BIND_NOTE, 36));
    session_midi_tick(s);
    key(1, 36, 120);
    run(s, 20, pk);
    CHECK(pk[0] < 1e-6, "a note bound to a clip does not play the instrument (%.4f)", pk[0]);
    key(0, 36, 0);
    key(1, 37, 120);
    run(s, 20, pk);
    CHECK(pk[0] > 0.01, "the next key plays normally (%.3f)", pk[0]);

    /* "learn" waiting -> notes are not played */
    key(0, 37, 0); run(s, 300, pk);
    atomic_store(&s->btn_learn_kind, BTN_LEARN_CLIP);
    session_midi_tick(s);
    key(1, 50, 120);
    run(s, 20, pk);
    CHECK(pk[0] < 0.002, "while a MIDI learn waits, keys do not play (%.4f)", pk[0]);
    key(0, 50, 0);
    atomic_store(&s->btn_learn_kind, BTN_LEARN_NONE);
    session_midi_tick(s);

    /* panic: a stuck note (key-up lost) is cut */
    key(1, 70, 120);
    run(s, 20, pk);
    CHECK(pk[0] > 0.01, "stuck note sounds (%.3f)", pk[0]);
    session_midi_panic(s);
    run(s, 300, pk);
    run(s, 10, pk);
    CHECK(pk[0] < 0.002, "panic: all notes off (%.4f)", pk[0]);

    /* a bypassed instrument is silent and gets no notes */
    session_fx_toggle_bypass(s, 0, 0);
    for (int i = 0; i < 50; ++i) { fxchain_sync(s); nap(2000); }
    key(1, 72, 120);
    run(s, 20, pk);
    CHECK(pk[0] < 1e-6, "bypassed instrument: silent (%.4f)", pk[0]);
    key(0, 72, 0);
    session_fx_toggle_bypass(s, 0, 0);
    for (int i = 0; i < 50; ++i) { fxchain_sync(s); nap(2000); }

    /* the settings of the instrument go into the project */
    QPlug *qp = fxchain_find(s, 1, 0);
    int np = qplug_param_count(qp);
    CHECK(np >= 8, "Nekobi has %d parameters", np);
    qplug_param_set(qp, 2, 0.83f);              /* Cutoff */
    run(s, 2, pk);
    const char *pp = "/tmp/qlim_test_audio/inst.limproject";
    if (system("mkdir -p /tmp/qlim_test_audio") != 0) return 2;
    CHECK(project_save(s, pp) == 0, "project with instruments saved");
    fxchain_shutdown(s);
    session_free(s);
    static Session T; Session *t = &T;
    session_init(t, 48000);
    atomic_store(&t->audio_running, false);
    CHECK(project_load(t, pp) == 0 && t->channel_count == 3, "project loaded");
    for (int i = 0; i < 400 && !fxchain_has_instrument(t, 1); ++i) { fxchain_sync(t); nap(5000); }
    QPlug *q2 = fxchain_find(t, 1, 0);
    float v = q2 ? qplug_param_get(q2, 2) : -1;
    CHECK(q2 && fabsf(v - 0.83f) < 0.01f, "the instrument's knob comes back (%.3f)", v);
    session_set_midi_target(t, 1);
    key(1, 48, 120);
    run(t, 20, pk);
    CHECK(pk[1] > 0.005, "and it plays after loading (%.3f)", pk[1]);
    key(0, 48, 0);
    run(t, 20, pk);
    fxchain_shutdown(t);
    session_free(t);
    printf(fails ? "INSTMIX: %d FAILED\n" : "INSTMIX: all ok\n", fails);
    return fails != 0;
}
