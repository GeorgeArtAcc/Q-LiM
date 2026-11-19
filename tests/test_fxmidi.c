/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.14: MIDI Learn of plugin parameters, in all 4 formats (the DPF test instrument Nekobi):
 * learning the parameter you touch (as from the plugin's window) or a chosen one, a CC moves it, a preset change is
 * not a "touch", Forget, the binding moves with the plugin and goes with it, and it is saved in the project.
 * Argument: the folder with Nekobi.vst3, Nekobi-vst.so, Nekobi.lv2, Nekobi.clap */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/fxchain.h"
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

static QPlug *wait_plug(Session *s, int ch, int k)
{
    for (int i = 0; i < 600; ++i) {
        fxchain_sync(s);
        QPlug *p = fxchain_find(s, ch, k);
        if (p) return p;
        nap(5000);
    }
    return NULL;
}
/* the main loop: fxchain_sync every frame, the audio thread runs the plugins (VST3 / CLAP take a new value there) */
static void frames(Session *s, int n)
{
    static float out[256 * 2];
    for (int i = 0; i < n; ++i) { mixer_render(s, out, 256, 2); mixer_render(s, out, 256, 2); fxchain_sync(s); nap(2000); }
}

static void one(const char *fmt, const char *id)
{
    static Session S;
    Session *s = &S;
    session_init(s, 48000);
    session_add_channel(s, NULL);
    session_add_scene(s);
    atomic_store(&s->audio_running, false);
    session_fx_add(s, 0, fmt, "Nekobi", id);
    QPlug *p = wait_plug(s, 0, 0);
    CHECK(p != NULL, "[%s] loaded", fmt);
    if (!p) return;
    int np = qplug_param_count(p);
    CHECK(np >= 4, "[%s] %d parameters", fmt, np);
    char msg[200];

    /* 1) learn "the one you touch": a CC before any touch binds nothing */
    fxchain_learn(s, 0, 0, -1);
    frames(s, 2);
    CHECK(fxchain_learn_cc(s, 21, msg, sizeof msg) && session_fx_midi_count(s, 0, 0) == 0,
          "[%s] a CC before touching a parameter is swallowed, nothing bound (%s)", fmt, msg);
    /* "touch" parameter 2 (what the plugin's window does: the value changes) */
    float v2 = qplug_param_get(p, 2);
    qplug_param_set(p, 2, v2 > 0.5f ? 0.2f : 0.8f);
    frames(s, 2);
    int lch, lk, lp;
    CHECK(fxchain_learning(&lch, &lk, &lp) && lp == 2, "[%s] the touched parameter becomes the target (%d)", fmt, lp);
    /* a "preset" (many at once) does not change the target */
    for (int i = 0; i < np && i < 6; ++i) qplug_param_set(p, i, 0.37f + 0.05f * i);
    frames(s, 2);
    fxchain_learning(NULL, NULL, &lp);
    CHECK(lp == 2, "[%s] many parameters moving at once (a preset) is not a touch (target %d)", fmt, lp);
    CHECK(fxchain_learn_cc(s, 21, msg, sizeof msg) && session_fx_midi_get(s, 0, 0, 2) == 21 && !fxchain_learning(NULL, NULL, NULL),
          "[%s] then a CC binds: %s", fmt, msg);

    /* 2) the CC moves it */
    fxchain_midi_cc(s, 21, 127); frames(s, 2);
    float a = qplug_param_get(p, 2);
    fxchain_midi_cc(s, 21, 0); frames(s, 2);
    float b = qplug_param_get(p, 2);
    fxchain_midi_cc(s, 21, 64); frames(s, 2);
    float c = qplug_param_get(p, 2);
    CHECK(a > 0.99f && b < 0.01f && fabsf(c - 64.0f / 127.0f) < 0.02f, "[%s] CC 21: 127 -> %.3f, 0 -> %.3f, 64 -> %.3f", fmt, a, b, c);
    float other = qplug_param_get(p, 3);
    fxchain_midi_cc(s, 22, 127); frames(s, 2);
    CHECK(fabsf(qplug_param_get(p, 3) - other) < 1e-6f, "[%s] another CC moves nothing", fmt);

    /* 3) learn a chosen parameter (right click on a knob) */
    fxchain_learn(s, 0, 0, 3);
    CHECK(fxchain_learn_cc(s, 22, msg, sizeof msg) && session_fx_midi_get(s, 0, 0, 3) == 22, "[%s] a chosen knob: %s", fmt, msg);
    fxchain_midi_cc(s, 22, 127); frames(s, 2);
    CHECK(qplug_param_get(p, 3) > 0.99f, "[%s] CC 22 moves it", fmt);

    /* 4) saved in the project and loaded back */
    char pp[256];
    snprintf(pp, sizeof pp, "/tmp/qlim_fxmidi_%s.limproject", fmt);
    CHECK(project_save(s, pp) == 0, "[%s] saved", fmt);
    static Session T;
    session_init(&T, 48000);
    atomic_store(&T.audio_running, false);
    CHECK(project_load(&T, pp) == 0 && session_fx_midi_get(&T, 0, 0, 2) == 21 && session_fx_midi_get(&T, 0, 0, 3) == 22,
          "[%s] the bindings come back with the project", fmt);
    QPlug *tp = wait_plug(&T, 0, 0);
    if (tp) {
        fxchain_midi_cc(&T, 21, 127); frames(&T, 2);
        CHECK(qplug_param_get(tp, 2) > 0.99f, "[%s] and they work after loading", fmt);
    }
    fxchain_shutdown(&T);
    session_free(&T);

    /* 5) the binding moves with the plugin and goes with it */
    session_fx_add(s, 0, fmt, "Nekobi 2", id);
    wait_plug(s, 0, 1);
    session_fx_move(s, 0, 0, 1);
    frames(s, 5);
    CHECK(session_fx_midi_get(s, 0, 1, 2) == 21 && session_fx_midi_get(s, 0, 0, 2) < 0, "[%s] moved to the right: the CCs went with it", fmt);
    QPlug *moved = wait_plug(s, 0, 1);
    if (moved) { fxchain_midi_cc(s, 21, 0); frames(s, 2); CHECK(qplug_param_get(moved, 2) < 0.01f, "[%s] and still move it", fmt); }
    /* learning a plugin that is then removed stops by itself */
    fxchain_learn(s, 0, 1, -1);
    session_fx_remove(s, 0, 1);
    frames(s, 3);
    CHECK(!fxchain_learning(NULL, NULL, NULL), "[%s] removing the plugin ends its MIDI Learn", fmt);
    CHECK(session_fx_midi_count(s, 0, 0) == 0, "[%s] the removed plugin's CCs are gone", fmt);

    /* 6) Forget */
    fxchain_learn(s, 0, 0, 1);
    fxchain_learn_cc(s, 30, msg, sizeof msg);
    session_fx_midi_set(s, 0, 0, 1, -1);
    CHECK(session_fx_midi_get(s, 0, 0, 1) < 0, "[%s] Forget", fmt);
    fxchain_learn(s, 0, 0, 1);
    fxchain_learn_cc(s, 30, msg, sizeof msg);
    session_fx_midi_clear(s, 0, 0);
    CHECK(session_fx_midi_count(s, 0, 0) == 0, "[%s] Forget all", fmt);

    fxchain_shutdown(s);
    session_free(s);
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: test_fxmidi <folder with the Nekobi plugins>\n"); return 2; }
    streams_init();
    char id[1200];
    snprintf(id, sizeof id, "%s/Nekobi.vst3|2046504473616C636B654E4400000000", argv[1]); one("VST3", id);
    snprintf(id, sizeof id, "%s/Nekobi-vst.so", argv[1]);                              one("VST", id);
    snprintf(id, sizeof id, "%s/Nekobi.lv2|http://distrho.sf.net/plugins/Nekobi", argv[1]);               one("LV2", id);
    snprintf(id, sizeof id, "%s/Nekobi.clap|studio.kx.distrho.Nekobi", argv[1]);        one("CLAP", id);
    printf("FXMIDI: %s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
