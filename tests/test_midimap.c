/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.2: the controller buttons (src/midimap.c): learning, presses, jog wheels, saving.
 *   cc -std=c11 -Isrc tests/test_midimap.c src/midimap.c -o /tmp/test_midimap && /tmp/test_midimap */
#define _POSIX_C_SOURCE 200809L
#include "midimap.h"
#include "session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_dir[256];
void settings_get_config_path(char *out, size_t n) { snprintf(out, n, "%s/audio.conf", g_dir); }

static int fails, got_a, got_d, calls;
static void h(int a, int d, void *u) { (void)u; got_a = a; got_d = d; calls++; }
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } else printf("ok:   %s\n", m); } while (0)

int main(void)
{
    snprintf(g_dir, sizeof g_dir, "/tmp/qlim_mm_%d", (int)getpid());
    char cmd[300]; snprintf(cmd, sizeof cmd, "mkdir -p %s", g_dir); if (system(cmd)) return 1;
    midimap_set_handler(h, NULL);

    /* wheel decoding: both styles */
    CHECK(midimap_wheel_delta(1) == 1 && midimap_wheel_delta(127) == -1, "two's complement 1 / 127");
    CHECK(midimap_wheel_delta(65) == 1 && midimap_wheel_delta(63) == -1, "binary offset 65 / 63");
    CHECK(midimap_wheel_delta(64) == 0 && midimap_wheel_delta(0) == 0, "64 and 0 = no move");

    /* learn a jog wheel: a note is ignored, a CC is taken */
    midimap_learn(MM_WHEEL_V);
    CHECK(midimap_handle(MIDI_BIND_NOTE, 40, 100) && midimap_learning() == MM_WHEEL_V, "wheel learning ignores notes");
    CHECK(midimap_handle(MIDI_BIND_CC, 60, 1) && midimap_learning() == -1, "wheel learned from CC 60");
    /* 1.4.4: the real Korg nanoKONTROL Studio jog wheel (from the user's log): ABSOLUTE values 0..127 */
    midimap_set_wheel_ticks(1);
    {
        static const int seq[] = { 1, 2, 3, 4, 5, 6, 7, 10, 11, 12, 13, 14, 15, 16, 17, 18 };   /* turning right */
        int sum = 0; calls = 0;
        for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; ++i) { got_d = 0; midimap_handle(MIDI_BIND_CC, 60, seq[i]); sum += got_d; }
        /* 1 -> 18 = 17 clicks; the first value only sets the start; 7 -> 10 = 3 clicks */
        CHECK(calls == 17 && got_d == 1 && sum > 0, "absolute wheel 1..18 = 17 steps down, every click one step");
        static const int back[] = { 17, 16, 15, 12, 11, 10, 2, 0 };                             /* turning left */
        sum = 0; calls = 0;
        for (unsigned i = 0; i < sizeof back / sizeof back[0]; ++i) { got_d = 0; midimap_handle(MIDI_BIND_CC, 60, back[i]); sum += (got_d < 0 ? -1 : got_d > 0 ? 1 : 0); }
        CHECK(sum < 0 && got_d == -1, "turning back: only steps up (the low values 1..15 are NOT read as relative)");
        CHECK(calls == 18, "18 -> 0 = 18 clicks (a jump of 8 values = 8 clicks)");
    }
    /* a relative wheel (value 1 / 127 again and again) on another CC */
    midimap_set(MM_WHEEL_H, MIDI_BIND_MAKE(MIDI_BIND_CC, 70));
    calls = 0;
    for (int i = 0; i < 5; ++i) midimap_handle(MIDI_BIND_CC, 70, 1);
    CHECK(calls == 4 && got_d == 1, "relative wheel: 1,1,1,1,1 = 4 steps (the 1st message finds out the kind)");
    calls = 0;
    for (int i = 0; i < 3; ++i) midimap_handle(MIDI_BIND_CC, 70, 127);
    CHECK(calls == 3 && got_d == -1, "relative wheel: 127 x3 = 3 steps back");
    calls = 0;
    for (int i = 0; i < 2; ++i) midimap_handle(MIDI_BIND_CC, 70, 9);
    CHECK(calls == 2 && got_d == 1, "relative wheel with acceleration (9) = 1 step per message");
    midimap_set_wheel_ticks(3);
    calls = 0;
    for (int i = 0; i < 6; ++i) midimap_handle(MIDI_BIND_CC, 70, 1);
    CHECK(calls == 2, "clicks per step 3: 6 clicks = 2 steps");
    midimap_set_wheel_ticks(1);
    /* Korg "Sign Magnitude" (1.4.5): right = 1, left = 65 */
    midimap_set(MM_WHEEL_H, 0); midimap_set(MM_WHEEL_H, MIDI_BIND_MAKE(MIDI_BIND_CC, 82));
    {
        static const int v[] = { 1, 1, 1, 65, 65, 1, 65 };
        static const int want[] = { 0, 1, 1, -1, -1, 1, -1 };     /* the first only finds out the kind */
        int ok = 1;
        for (int i = 0; i < 7; ++i) { calls = 0; got_d = 0; midimap_handle(MIDI_BIND_CC, 82, v[i]); if (got_d != want[i]) ok = 0; }
        CHECK(ok, "sign magnitude: 1 = down, 65 = up, every click");
    }
    /* binary offset: right = 65, left = 63 */
    midimap_set(MM_WHEEL_H, 0); midimap_set(MM_WHEEL_H, MIDI_BIND_MAKE(MIDI_BIND_CC, 84));
    {
        static const int v[] = { 65, 65, 65, 63, 63, 65 };
        static const int want[] = { 0, 1, 1, -1, -1, 1 };
        int ok = 1;
        for (int i = 0; i < 6; ++i) { got_d = 0; midimap_handle(MIDI_BIND_CC, 84, v[i]); if (got_d != want[i]) ok = 0; }
        CHECK(ok, "binary offset: 65 = down, 63 = up");
    }
    midimap_set(MM_WHEEL_H, 0);

    /* buttons: CC press only on the way up, the release does nothing */
    midimap_learn(MM_LAUNCH);
    midimap_handle(MIDI_BIND_CC, 41, 0);                          /* a release does not get learned */
    CHECK(midimap_learning() == MM_LAUNCH, "a release (value 0) is not learned");
    midimap_handle(MIDI_BIND_CC, 41, 127);
    CHECK(midimap_get(MM_LAUNCH) == MIDI_BIND_MAKE(MIDI_BIND_CC, 41), "Launch learned = CC 41");
    midimap_handle(MIDI_BIND_CC, 41, 0);                          /* release after the learning press */
    calls = 0;
    midimap_handle(MIDI_BIND_CC, 41, 127); midimap_handle(MIDI_BIND_CC, 41, 127); midimap_handle(MIDI_BIND_CC, 41, 0);
    CHECK(calls == 1 && got_a == MM_LAUNCH, "press-hold-release = one launch");

    /* notes, direction of up/left */
    midimap_learn(MM_UP); midimap_handle(MIDI_BIND_NOTE, 50, 90);
    calls = 0; midimap_handle(MIDI_BIND_NOTE, 50, 90);
    CHECK(calls == 1 && got_a == MM_UP && got_d == -1, "Select up (note 50) = -1");

    /* one control = one job: binding CC 41 to Stop takes it away from Launch */
    midimap_set(MM_STOP, MIDI_BIND_MAKE(MIDI_BIND_CC, 41));
    CHECK(midimap_get(MM_LAUNCH) == 0 && midimap_get(MM_STOP) != 0, "a control moves to the new job");
    CHECK(!midimap_handle(MIDI_BIND_CC, 99, 127), "an unbound CC is not taken");

    /* saved (every change is written at once) and loaded back */
    midimap_set(MM_LAUNCH, 0); midimap_set(MM_STOP, 0);
    int wheel = midimap_get(MM_WHEEL_V), up = midimap_get(MM_UP);
    {
        char p[300]; snprintf(p, sizeof p, "%s/controls.conf", g_dir);
        FILE *f = fopen(p, "r"); char buf[512] = {0}; if (f) { size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); }
        CHECK(strstr(buf, "wheel_v=2:60") && strstr(buf, "up=1:50") && strstr(buf, "wheel_clicks=1") && strstr(buf, "wheel_v_mode=abs"), "controls.conf has wheel_v=2:60, up=1:50, wheel_clicks=1, wheel_v_mode=abs");
        midimap_load();
        CHECK(midimap_get(MM_WHEEL_V) == wheel && midimap_get(MM_UP) == up, "loaded back");
    }
    char d[64]; midimap_describe(MM_WHEEL_V, d, sizeof d);
    CHECK(!strcmp(d, "CC 60"), "describe = CC 60");
    /* 1.4.8: the pad box - 4 x 2 pads (like an MPK mini: notes 40-43 on top, 36-39 below), learned in box order */
    midimap_set_pad_box(4, 2);
    CHECK(!midimap_pads_active(), "no pads learned: no box");
    midimap_pads_learn(true);
    const int order[8] = { 40, 41, 42, 43, 36, 37, 38, 39 };
    calls = 0;
    for (int i = 0; i < 8; ++i) {
        CHECK(midimap_pads_learning() == i, "waiting for the next pad");
        midimap_handle(MIDI_BIND_NOTE, order[i], 100);
        midimap_handle(MIDI_BIND_NOTE, order[i], 0);
        if (i == 2) { midimap_handle(MIDI_BIND_NOTE, order[1], 90); CHECK(midimap_pads_learning() == 3, "the same pad twice is not taken"); }
    }
    CHECK(midimap_pads_learning() == -1 && midimap_pad_count() == 8 && midimap_pads_active() && calls == 0, "8 pads learned, nothing launched while learning");
    calls = 0; midimap_handle(MIDI_BIND_NOTE, 36, 110);
    CHECK(calls == 1 && got_a == MM_PAD && got_d == 4, "note 36 = pad 5 (bottom row, left)");
    calls = 0; midimap_handle(MIDI_BIND_NOTE, 43, 110);
    CHECK(calls == 1 && got_a == MM_PAD && got_d == 3, "note 43 = pad 4 (top row, right)");
    calls = 0; midimap_handle(MIDI_BIND_NOTE, 43, 0);
    CHECK(calls == 0, "the release does nothing");
    {
        unsigned m[4]; midimap_note_mask(m);
        bool all = true; for (int i = 0; i < 8; ++i) if (!((m[order[i] >> 5] >> (order[i] & 31)) & 1u)) all = false;
        CHECK(all && !((m[44 >> 5] >> (44 & 31)) & 1u), "the pad notes are kept from the instruments (and only they)");
    }
    midimap_load();
    { int w, hh; midimap_pad_box(&w, &hh); CHECK(w == 4 && hh == 2 && midimap_pad_count() == 8, "pad box loaded back (4 x 2, 8 pads)"); }
    calls = 0; midimap_handle(MIDI_BIND_NOTE, 38, 100);
    CHECK(calls == 1 && got_a == MM_PAD && got_d == 6, "after loading: note 38 = pad 7");
    midimap_set_pad_box(2, 2);
    CHECK(midimap_pad_count() == 4, "a smaller box keeps the first pads");
    calls = 0; midimap_handle(MIDI_BIND_NOTE, 36, 100);
    CHECK(calls == 0 || got_a != MM_PAD, "a pad outside the smaller box does nothing");
    midimap_pads_clear();
    CHECK(!midimap_pads_active() && midimap_pad_count() == 0, "cleared");

    snprintf(cmd, sizeof cmd, "rm -rf %s", g_dir); if (system(cmd)) {}
    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}
