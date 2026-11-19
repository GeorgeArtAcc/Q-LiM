/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 2.1: save -> open -> save gives the same project file, with every newer feature in it (surround circle + room +
 * paths + Speaker Map, surround files multichannel / path + their maps, MIDI, cuts, clip gain / pan / pitch,
 * tempo, time signature, transitions) - nothing is lost or changed on the way. */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/project.h"
#include "../src/stream.h"
#include "../src/surround.h"
#include "../src/edit.h"
#include <sndfile.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void nap(long ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }

static void write_wav(const char *p, int ch, int secs)
{
    SF_INFO si = { .samplerate = 48000, .channels = ch, .format = SF_FORMAT_WAV | SF_FORMAT_FLOAT };
    SNDFILE *f = sf_open(p, SFM_WRITE, &si);
    static float b[4800 * 8];
    for (int i = 0; i < 4800 * ch; ++i) b[i] = 0.1f * sinf((float)i * 0.01f);
    for (int k = 0; k < 10 * secs; ++k) sf_writef_float(f, b, 4800);
    sf_close(f);
}

static char *slurp(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) n = 0;
    b[n] = 0; fclose(f);
    return b;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    write_wav("/tmp/qlim_rt_st.wav", 2, 4);
    write_wav("/tmp/qlim_rt_51.wav", 6, 4);
    streams_init();
    static Session S, T, U;
    session_init(&S, 48000);
    atomic_store(&S.audio_running, false);
    for (int i = 0; i < 4; ++i) session_add_channel(&S, NULL);
    for (int i = 0; i < 3; ++i) session_add_scene(&S);
    session_set_tempo(&S, 126.5f);
    session_set_time_sig(&S, 7, 8);
    session_set_transition_sec(&S, 6.5f);
    session_rename_channel(&S, 0, "Circle");
    session_rename_channel(&S, 1, "Room");

    /* 0: a circle surround with a Speaker Map, a clip with a path, gain / pan / pitch and a cut */
    SurSetup su; memset(&su, 0, sizeof su);
    sur_layout(&su, SUR_L_51, 0);
    su.width = 60; su.az0 = 30; su.spread0 = 0.25f;
    su.omask[2] = (1ull << 2) | (1ull << 9); su.omask[3] = SUR_MUTED;
    session_set_surround(&S, 0, &su);
    session_set_clip(&S, 0, 0, "/tmp/qlim_rt_st.wav");
    SurPt p[3] = { { 0, -30, 0, 0, 0, 0 }, { 1.5f, 90, 0.2f, 0, 0, 0 }, { 3, 180, 0.5f, 0, 0, 0 } };
    session_clip_set_sur_points(&S, 0, 0, p, 3);
    session_set_clip_gain(&S, 0, 0, -3.5f);
    session_set_clip_pan(&S, 0, 0, 0.25f);
    session_set_clip_pitch(&S, 0, 0, 2.0f);
    const EditList *e = session_clip_edits(&S, 0, 0);
    if (e) { EditList *nl = edit_split(e, 48000); if (nl) session_clip_apply_edits(&S, 0, 0, nl, -1); }

    /* 1: a room surround with a path on the plan */
    SurSetup rm; memset(&rm, 0, sizeof rm);
    sur_room_layout(&rm, SUR_R_THEATER9, 2);
    rm.mode = SUR_MODE_ROOM; rm.x0 = 1; rm.y0 = 5; rm.w0 = 4;
    session_set_surround(&S, 1, &rm);
    session_set_clip(&S, 1, 1, "/tmp/qlim_rt_st.wav");
    SurPt q[2] = { { 0, 0, 0.1f, -2, 3, 4 }, { 2, 0, 0.3f, 1.5f, 20, 0 } };
    session_clip_set_sur_points(&S, 1, 1, q, 2);

    /* 2: a surround file, multichannel from output 3, with a map; 3: the same file as a path */
    session_load_mc_next(CLIP_MC_MULTI, 2, NULL);
    session_set_clip(&S, 2, 0, "/tmp/qlim_rt_51.wav");
    session_clip_set_mc_map(&S, 2, 0, 2, (1ull << 0) | (1ull << 5));
    session_clip_set_mc_map(&S, 2, 0, 3, SUR_MUTED);
    session_load_mc_next(CLIP_MC_PATH, 0, "/tmp/qlim_rt_51.wav");
    session_set_clip(&S, 3, 0, "/tmp/qlim_rt_st.wav");

    /* 3, scene 2: a MIDI clip */
    session_midi_set_text(&S, 3, 2, "midi_len=8\nmn=0 0.5 60 100\nmn=1 0.25 64 90\nmn=2.5 1 67 80\n");
    nap(300);

    CHECK(project_save(&S, "/tmp/qlim_rt1.limproject") == 0, "saved");
    session_init(&T, 48000);
    atomic_store(&T.audio_running, false);
    CHECK(project_load(&T, "/tmp/qlim_rt1.limproject") == 0, "opened");
    nap(300);
    CHECK(project_save(&T, "/tmp/qlim_rt2.limproject") == 0, "saved again");
    char *a = slurp("/tmp/qlim_rt1.limproject"), *b = slurp("/tmp/qlim_rt2.limproject");
    bool same = a && b && strcmp(a, b) == 0;
    if (!same && a && b) {                    /* show the first difference */
        size_t i = 0; while (a[i] && a[i] == b[i]) ++i;
        size_t ls = i; while (ls > 0 && a[ls - 1] != '\n') --ls;
        printf("   first difference at line starting: %.80s\n   vs: %.80s\n", a + ls, b + ls);
    }
    CHECK(same, "save -> open -> save: the same file (%zu bytes)", a ? strlen(a) : 0);
    CHECK(a && strstr(a, "om:") && strstr(a, "mc_map=") && strstr(a, "mc=2,") && strstr(a, "mc_orig=") && strstr(a, "sur=") &&
          strstr(a, "mn=") && strstr(a, "clip_pitch=") && strstr(a, "edit_seg="), "every newer key is in the file");
    /* and the values really came back */
    SurSetup b0; session_get_surround(&T, 0, &b0);
    CHECK(b0.n == su.n && b0.omask[2] == su.omask[2] && b0.omask[3] == SUR_MUTED && fabsf(b0.az0 - 30) < 0.01f, "circle + Speaker Map back");
    SurSetup b1; session_get_surround(&T, 1, &b1);
    CHECK(b1.mode == SUR_MODE_ROOM && b1.n == rm.n && fabsf(b1.y0 - 5) < 0.01f, "room back");
    SurPt bp[8];
    CHECK(session_clip_get_sur_points(&T, 1, 1, bp, 8) == 2 && fabsf(bp[1].y - 20) < 0.01f, "room path back");
    CHECK(session_clip_mc_mode(&T, 2, 0) == CLIP_MC_MULTI && session_clip_mc_out(&T, 2, 0) == 2 &&
          session_clip_mc_map(&T, 2, 0, 3) == SUR_MUTED, "multichannel file + its map back");
    CHECK(session_clip_mc_mode(&T, 3, 0) == CLIP_MC_PATH && session_clip_mc_orig(&T, 3, 0), "path file + its original back");
    CHECK(fabsf(session_get_tempo(&T) - 126.5f) < 0.01f, "tempo back");

    /* an old (1.4.0-like) project without the newer keys still opens */
    FILE *f = fopen("/tmp/qlim_rt_old.limproject", "w");
    fprintf(f, "# Q-LiM project\n[meta]\nversion=3\nsample_rate=48000\nchannel_count=1\nscene_count=1\ntempo=120\n"
               "[channel 0]\nname=Old\ngain=0.8\n[clip 0 0]\ntype=clip\npath=/tmp/qlim_rt_st.wav\nname=x\nloop=1\n");
    fclose(f);
    session_init(&U, 48000);
    atomic_store(&U.audio_running, false);
    CHECK(project_load(&U, "/tmp/qlim_rt_old.limproject") == 0 && U.channel_count == 1 && !session_get_surround(&U, 0, NULL) &&
          session_clip_mc_mode(&U, 0, 0) == CLIP_MC_STEREO, "an old project opens (no surround, plain stereo)");
    free(a); free(b);
    session_free(&S); session_free(&T); session_free(&U);
    printf("ROUNDTRIP: %s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
