/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.17: a clip with N separated parts (no model needed - the parts are made here):
 * all parts = the original, mute, solo (one / two / Ctrl-style), levels, the snapshot, export of one part,
 * MIDI CC on a part's level, the project lines (name|level|mute|solo|cc|path) and the old 1.4.14 "stems=" lines,
 * "Split to clips" (capture + place one under another, only into empty cells, new scenes added). */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/export.h"
#include "../src/project.h"
#include "../src/stream.h"
#include <math.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void nap(long ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
#define SR 44100
#define NP 6
#define LEN (SR * 4)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *NAMES[NP] = { "Vocals", "Drums", "Bass", "Guitar", "Piano", "Other" };
static const double FREQ[NP] = { 440.0, 1210.0, 82.5, 330.0, 660.0, 2500.0 };
static float *g_part[NP];

static void write_wav(const char *p, const float *b, long n)
{
    SF_INFO i = { 0 };
    i.samplerate = SR; i.channels = 2; i.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
    SNDFILE *f = sf_open(p, SFM_WRITE, &i);
    sf_writef_float(f, b, n);
    sf_close(f);
}
static float *load(const char *p, long *n)
{
    SF_INFO i = { 0 };
    SNDFILE *f = sf_open(p, SFM_READ, &i);
    if (!f) return NULL;
    float *b = malloc((size_t)i.frames * 2 * sizeof(float));
    *n = (long)sf_readf_float(f, b, i.frames);
    sf_close(f);
    return b;
}
static float *render(Session *s, int ch, int sc, double secs, long *n)
{
    session_stop_all(s);
    for (int i = 0; i < 20; ++i) { static float z[512]; mixer_render(s, z, 256, 2); }
    session_play_clip_from(s, ch, sc, 0);
    long total = (long)(secs * SR);
    float *out = calloc((size_t)(total + 256) * 2, sizeof(float));
    for (long p = 0; p < total; p += 256) { nap(3); mixer_render(s, out + p * 2, 256, 2); }
    *n = total;
    return out;
}
/* how much of each part is in the render (correlation with the part's sine, aligned) */
static double amount(const float *out, long n, int k)
{
    double best = 0;
    for (int lag = 0; lag <= 3000; lag += 1) {
        double xy = 0, yy = 0;
        for (long i = SR; i < SR * 2 && i + lag < n; i += 3) {
            double y = g_part[k][i * 2];
            xy += out[(i + lag) * 2] * y; yy += y * y;
        }
        double a = yy > 0 ? xy / yy : 0;
        if (fabs(a) > fabs(best)) best = a;
    }
    return fabs(best);            /* (a pure tone: the best lag may be half a period off - the amount is what counts) */
}
static void amounts(Session *s, double *a)
{
    long n;
    float *o = render(s, 0, 0, 2.5, &n);
    for (int k = 0; k < NP; ++k) a[k] = amount(o, n, k);
    free(o);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (system("rm -rf /tmp/qlim_stems && mkdir -p /tmp/qlim_stems") != 0) return 2;
    float *mix = calloc((size_t)LEN * 2, sizeof(float));
    StemSet set;
    memset(&set, 0, sizeof set);
    set.n = NP;
    for (int k = 0; k < NP; ++k) {
        g_part[k] = calloc((size_t)LEN * 2, sizeof(float));
        for (long i = 0; i < LEN; ++i) {
            float v = 0.08f * (float)sin(2 * M_PI * FREQ[k] * i / SR);
            g_part[k][i * 2] = g_part[k][i * 2 + 1] = v;
            mix[i * 2] += v; mix[i * 2 + 1] += v;
        }
        snprintf(set.path[k], sizeof set.path[k], "/tmp/qlim_stems/song (%s).wav", NAMES[k]);
        snprintf(set.name[k], sizeof set.name[k], "%s", NAMES[k]);
        write_wav(set.path[k], g_part[k], LEN);
    }
    write_wav("/tmp/qlim_stems/song.wav", mix, LEN);

    static Session S;
    Session *s = &S;
    streams_init();                                     /* the disk thread */
    session_init(s, SR);
    atomic_store(&s->audio_running, false);
    session_add_channel(s, "Song");
    session_add_scene(s);
    CHECK(session_set_clip(s, 0, 0, "/tmp/qlim_stems/song.wav") == 0, "the song clip");
    for (int w = 0; w < 400 && !s->channels[0].clips[0].stream; ++w) nap(5);

    /* 1) N parts */
    CHECK(session_clip_set_stem_set(s, 0, 0, &set) == 0 && session_clip_stem_count(s, 0, 0) == NP, "%d parts put into the clip", NP);
    CHECK(!strcmp(session_clip_stem_name(s, 0, 0, 2), "Bass"), "their names are kept");
    double a[NP];
    amounts(s, a);
    bool all = true;
    for (int k = 0; k < NP; ++k) if (fabs(a[k] - 1.0) > 0.03) all = false;
    CHECK(all, "all at 0 dB: every part plays fully (%.2f %.2f %.2f %.2f %.2f %.2f)", a[0], a[1], a[2], a[3], a[4], a[5]);

    /* 2) mute */
    session_clip_set_stem_mute(s, 0, 0, 1, true);
    session_clip_stems_apply(s, 0, 0);
    amounts(s, a);
    CHECK(fabs(a[1]) < 0.02 && fabs(a[0] - 1) < 0.03 && fabs(a[2] - 1) < 0.03, "drums muted: gone, the rest stays (%.2f / %.2f)", a[1], a[0]);
    CHECK(!session_clip_stem_audible(s, 0, 0, 1) && session_clip_stem_audible(s, 0, 0, 0), "audible() knows it");

    /* 3) solo: only the soloed parts; a muted part stays muted even when soloed */
    session_clip_set_stem_solo(s, 0, 0, 2, true);
    session_clip_stems_apply(s, 0, 0);
    amounts(s, a);
    CHECK(fabs(a[2] - 1) < 0.03 && fabs(a[0]) < 0.02 && fabs(a[3]) < 0.02 && fabs(a[5]) < 0.02, "bass solo: only the bass (%.2f, vocals %.2f)", a[2], a[0]);
    session_clip_set_stem_solo(s, 0, 0, 4, true);
    session_clip_set_stem_solo(s, 0, 0, 1, true);
    session_clip_stems_apply(s, 0, 0);
    amounts(s, a);
    CHECK(fabs(a[2] - 1) < 0.03 && fabs(a[4] - 1) < 0.03 && fabs(a[1]) < 0.02 && fabs(a[0]) < 0.02,
          "bass + piano solo (drums soloed but muted): bass %.2f piano %.2f drums %.2f", a[2], a[4], a[1]);
    ClipSnapshot cs;
    session_clip_snapshot(s, 0, 0, &cs);
    CHECK(cs.stems && cs.stem_n == NP && cs.stem_gain[2] == 1.0f && cs.stem_gain[4] == 1.0f && cs.stem_gain[0] == 0.0f && cs.stem_gain[1] == 0.0f,
          "the snapshot has the same gains");
    edit_free(cs.edits);
    for (int k = 0; k < NP; ++k) session_clip_set_stem_solo(s, 0, 0, k, false);
    session_clip_set_stem_mute(s, 0, 0, 1, false);

    /* 4) a level */
    session_clip_set_stem_level(s, 0, 0, 3, -6.0f);
    session_clip_stems_apply(s, 0, 0);
    amounts(s, a);
    CHECK(fabs(a[3] - 0.501) < 0.03 && fabs(a[0] - 1) < 0.03, "guitar at -6 dB: %.3f of it", a[3]);

    /* 5) export of one part (k + 1), at full level */
    {
        ExportOpts o = { EXP_WAV, 32, SR, 0, 4 };       /* part 3 = Guitar */
        char err[200], text[300];
        ExportJob *j = export_clip_start(s, 0, 0, &o, "/tmp/qlim_stems/guitar.wav", err, sizeof err);
        while (j && !export_finished(j)) nap(20);
        int er = j ? export_result(j, text, sizeof text) : 1;
        export_free(j);
        long n = 0;
        float *g = load("/tmp/qlim_stems/guitar.wav", &n);
        double e = 0, r = 0;
        for (long i = 0; g && i < n * 2 && i < LEN * 2; ++i) { double d = g[i] - g_part[3][i]; e += d * d; r += (double)g_part[3][i] * g_part[3][i]; }
        CHECK(er == 0 && g && n == LEN && e <= r * 1e-10, "export part 4 (Guitar) = the guitar alone, exactly (%s)", er ? err : text);
        free(g);
    }

    /* 6) MIDI CC on a level */
    session_clip_set_stem_cc(s, 0, 0, 0, 21);
    CHECK(session_clip_get_stem_cc(s, 0, 0, 0) == 21 && session_clip_get_stem_cc(s, 0, 0, 1) == -1, "CC 21 bound to the vocals level");
    CHECK(session_stems_midi_cc(s, 21, 50) == 1 && fabsf(session_clip_get_stem_level(s, 0, 0, 0) + 12.04f) < 0.05f,
          "CC 21 = 50 -> vocals %.2f dB", session_clip_get_stem_level(s, 0, 0, 0));
    CHECK(session_stems_midi_cc(s, 22, 50) == 0, "another CC moves nothing");
    session_stems_midi_cc(s, 21, 100);
    CHECK(session_clip_get_stem_level(s, 0, 0, 0) == 0.0f, "100 = exactly 0 dB");
    session_stems_midi_cc(s, 21, 0);
    CHECK(session_clip_get_stem_level(s, 0, 0, 0) <= CLIP_GAIN_MIN_DB, "0 = -inf");
    session_stems_poll(s, 100.0);                       /* the MIDI-moved clip gets re-mixed */
    amounts(s, a);
    CHECK(fabs(a[0]) < 0.02 && fabs(a[1] - 1) < 0.03, "and it is heard (vocals %.3f)", a[0]);
    session_stems_midi_cc(s, 21, 100);
    session_stem_learn_arm(0, 0, 5);
    int lc, ls, lk;
    CHECK(session_stem_learn_armed(&lc, &ls, &lk) && lk == 5, "MIDI Learn waits for part 6");
    session_stem_learn_arm(-1, -1, -1);
    CHECK(!session_stem_learn_armed(NULL, NULL, NULL), "and can be cancelled");

    /* 7) the project */
    session_clip_set_stem_solo(s, 0, 0, 4, true);
    session_clip_set_stem_mute(s, 0, 0, 5, true);
    CHECK(project_save(s, "/tmp/qlim_stems/p.limproject") == 0, "saved");
    static Session T;
    session_init(&T, SR);
    atomic_store(&T.audio_running, false);
    CHECK(project_load(&T, "/tmp/qlim_stems/p.limproject") == 0 && session_clip_stem_count(&T, 0, 0) == NP, "loaded with %d parts", session_clip_stem_count(&T, 0, 0));
    CHECK(!strcmp(session_clip_stem_name(&T, 0, 0, 4), "Piano") && fabsf(session_clip_get_stem_level(&T, 0, 0, 3) + 6.0f) < 0.01f &&
          session_clip_get_stem_solo(&T, 0, 0, 4) && session_clip_get_stem_mute(&T, 0, 0, 5) && !session_clip_get_stem_mute(&T, 0, 0, 1) &&
          session_clip_get_stem_cc(&T, 0, 0, 0) == 21 && session_clip_get_stem_cc(&T, 0, 0, 2) == -1,
          "names, levels, solo, mute and the MIDI CC kept");
    {   /* an old 1.4.14 project (stems= / stem_level= / stem_mute=) */
        FILE *f = fopen("/tmp/qlim_stems/p.limproject", "r"), *g = fopen("/tmp/qlim_stems/old.limproject", "w");
        char line[4096];
        bool done = false;
        while (f && g && fgets(line, sizeof line, f)) {
            if (!strncmp(line, "stem=", 5)) {
                if (!done)
                    fprintf(g, "stems=%s|%s\nstem_level=-2.5 0\nstem_mute=0 1\n", set.path[0], set.path[5]);
                done = true;
                continue;
            }
            fputs(line, g);
        }
        if (f) fclose(f);
        if (g) fclose(g);
        static Session U;
        session_init(&U, SR);
        atomic_store(&U.audio_running, false);
        CHECK(project_load(&U, "/tmp/qlim_stems/old.limproject") == 0 && session_clip_stem_count(&U, 0, 0) == 2 &&
              !strcmp(session_clip_stem_name(&U, 0, 0, 1), "Instrumental") && fabsf(session_clip_get_stem_level(&U, 0, 0, 0) + 2.5f) < 0.01f &&
              session_clip_get_stem_mute(&U, 0, 0, 1) && session_clip_get_stem_cc(&U, 0, 0, 0) == -1,
              "an old (1.4.14) project still loads its 2 parts, no MIDI CC");
    }

    /* 8) split to clips: the parts go one under another, into empty cells only */
    {
        session_add_channel(&T, "Parts");
        session_set_clip(&T, 1, 2, "/tmp/qlim_stems/song.wav");                     /* something in the way */
        ClipPart p[NP];
        int got = 0;
        for (int k = 0; k < NP; ++k) if (session_clip_capture_part(&T, 0, 0, k, &p[got]) == 0) ++got;
        CHECK(got == NP && strstr(p[1].name, "(Drums)") && strstr(p[1].path, "(Drums).wav"), "all parts taken (%s)", p[1].name);
        CHECK(p[3].gain < -5.9f && p[3].gain > -6.1f && p[5].gain <= CLIP_GAIN_MIN_DB, "they carry their level (guitar -6, muted other = -inf)");
        CHECK(session_place_part(&T, 1, 2, &p[0]) == 2, "a full cell is refused");
        int sc0 = T.scene_count, ok = 0;
        for (int k = 0; k < NP; ++k) if (session_place_part(&T, 1, 3 + k, &p[k]) == 0) ++ok;
        CHECK(ok == NP, "dropped from scene 4 down: %d clips", ok);
        CHECK(T.scene_count >= 3 + NP && T.scene_count >= sc0, "missing scenes were added (%d)", T.scene_count);
        bool names = true;
        for (int k = 0; k < NP; ++k) if (!strstr(T.channels[1].clips[3 + k].path, NAMES[k])) names = false;
        CHECK(names, "each cell holds its part, in order");
        for (int k = 0; k < NP; ++k) clip_part_free(&p[k]);
        for (int w = 0; w < 400 && !T.channels[1].clips[6].stream; ++w) nap(5);
        long n;
        float *o = render(&T, 1, 6, 2.0, &n);                                      /* the guitar clip */
        double g = amount(o, n, 3), v = amount(o, n, 0);
        free(o);
        CHECK(fabs(g - 0.501) < 0.03 && fabs(v) < 0.02, "the guitar clip plays only the guitar, at -6 dB (%.3f)", g);
    }

    /* 9) back to the file */
    CHECK(session_clip_set_stem_set(s, 0, 0, NULL) == 0 && !session_clip_has_stems(s, 0, 0), "remove: the clip plays its file");

    printf("STEMS: %s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
