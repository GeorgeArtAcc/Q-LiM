/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.19: surround - the panning law (constant power, on a speaker = only that one, spread), the automation (the short
 * way round), the layouts, the text forms, and the mixer: a channel through 4 / 8 outputs, a clip moving the sound
 * from one speaker to another over time, nothing to the master, the project keeps it. */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/project.h"
#include "../src/stream.h"
#include "../src/export.h"
#include <math.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void nap(long ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
#define SR 48000
#define OUTS 10

static double pw(const float *g, int n) { double s = 0; for (int k = 0; k < n; ++k) s += (double)g[k] * g[k]; return s; }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    /* 1) the law */
    SurSetup q;
    memset(&q, 0, sizeof q);
    sur_layout(&q, SUR_L_QUAD, 2);
    CHECK(q.n == 4 && q.out[0] == 2 && q.out[3] == 5 && q.az[0] == -45 && q.az[3] == 135, "Quad from output 3: FL FR RL RR on outputs 3-6");
    float g[SUR_MAX_SPK];
    sur_gains(&q, -45, 0, g);
    CHECK(fabsf(g[0] - 1) < 1e-6f && g[1] == 0 && g[2] == 0 && g[3] == 0, "on a speaker: only that one");
    sur_gains(&q, 0, 0, g);
    CHECK(fabsf(g[0] - 0.7071f) < 1e-3f && fabsf(g[1] - 0.7071f) < 1e-3f && g[2] == 0, "front, between FL and FR: 0.707 each");
    sur_gains(&q, 180, 0, g);
    CHECK(fabsf(g[2] - 0.7071f) < 1e-3f && fabsf(g[3] - 0.7071f) < 1e-3f && g[0] == 0, "behind: the two rear ones");
    sur_gains(&q, -90, 0, g);
    CHECK(fabsf(g[0] - 0.7071f) < 1e-3f && fabsf(g[2] - 0.7071f) < 1e-3f, "left: FL and RL");
    bool cp = true;
    for (int a = -180; a <= 180; a += 7)
        for (int sp = 0; sp <= 4; ++sp) { sur_gains(&q, (float)a, sp * 0.25f, g); if (fabs(pw(g, 4) - 1.0) > 1e-4) cp = false; }
    CHECK(cp, "every angle and spread: the power is always 1 (constant power)");
    sur_gains(&q, 30, 1.0f, g);
    CHECK(fabsf(g[0] - 0.5f) < 1e-4f && fabsf(g[3] - 0.5f) < 1e-4f, "spread 100%%: all four the same");
    SurSetup e;
    memset(&e, 0, sizeof e);
    sur_layout(&e, SUR_L_51, 0);
    CHECK(e.n == 5 && e.out[2] == 2 && e.out[3] == 4 && e.out[4] == 5, "5.1: L R C on 1-3, the LFE (4) free, Ls Rs on 5-6");
    sur_gains(&e, 0, 0, g);
    CHECK(fabsf(g[2] - 1) < 1e-6f, "5.1 front = the centre speaker");
    memset(&e, 0, sizeof e);
    sur_layout(&e, SUR_L_RING8, 0);
    sur_gains(&e, 90, 0, g);
    CHECK(fabsf(g[2] - 0.7071f) < 1e-3f && fabsf(g[3] - 0.7071f) < 1e-3f, "8 in a ring: right = speakers 3 and 4");

    /* 2) the automation */
    SurPt p[3] = { { 1.0f, 170, 0 }, { 0.0f, -45, 0 }, { 2.0f, -170, 1 } };
    sur_sort(p, 3);
    float az, sp;
    sur_eval(p, 3, 0.5f, 0, 0, &az, &sp);
    CHECK(fabsf(az + 117.5f) < 0.01f, "between -45 and 170 at the middle: %.1f (the short way: 145 deg over the left-rear)", az);
    sur_eval(p, 3, 1.5f, 0, 0, &az, &sp);
    CHECK(fabsf(fabsf(az) - 180.0f) < 0.01f && fabsf(sp - 0.5f) < 1e-4f, "170 -> -170: through behind (%.1f), the spread too (%.2f)", az, sp);
    sur_eval(p, 3, 5.0f, 0, 0, &az, &sp);
    CHECK(fabsf(az + 170) < 0.01f, "after the last point: it stays there");
    sur_eval(NULL, 0, 1.0f, 33, 0.2f, &az, &sp);
    CHECK(az == 33 && sp == 0.2f, "no points: the channel's position");
    char tx[600];
    sur_points_text(p, 3, tx, sizeof tx);
    SurPt p2[8];
    int n2 = sur_points_parse(tx, p2, 8);
    CHECK(n2 == 3 && fabsf(p2[1].az - 170) < 0.01f && fabsf(p2[2].spread - 1) < 1e-3f, "the points as text and back (%s)", tx);
    q.width = 60; q.az0 = -30; q.spread0 = 0.25f;
    sur_setup_text(&q, tx, sizeof tx);
    SurSetup q2;
    CHECK(sur_setup_parse(tx, &q2) && q2.n == 4 && q2.out[1] == 3 && q2.width == 60 && q2.az0 == -30 && fabsf(q2.spread0 - 0.25f) < 1e-3f && q2.layout == SUR_L_QUAD,
          "the setup as text and back (%s)", tx);

    /* 3) the mixer: a tone in a channel set to Quad on outputs 3-6; the clip moves it FL -> FR over 1 s */
    {
        SF_INFO si = { 0 };
        si.samplerate = SR; si.channels = 2; si.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
        SNDFILE *f = sf_open("/tmp/qlim_sur.wav", SFM_WRITE, &si);
        static float b[SR * 4 * 2];
        for (long i = 0; i < SR * 4; ++i) b[i * 2] = b[i * 2 + 1] = 0.3f * (float)sin(2 * M_PI * 440.0 * i / SR);
        sf_writef_float(f, b, SR * 4);
        sf_close(f);
    }
    streams_init();
    static Session S;
    Session *s = &S;
    session_init(s, SR);
    atomic_store(&s->audio_running, false);
    session_add_channel(s, "Move");
    session_add_scene(s);
    session_set_quant_mode(s, 0);
    session_set_clip(s, 0, 0, "/tmp/qlim_sur.wav");
    for (int w = 0; w < 400 && !s->channels[0].clips[0].stream; ++w) nap(5);
    SurSetup su;
    memset(&su, 0, sizeof su);
    sur_layout(&su, SUR_L_QUAD, 2);
    CHECK(session_set_surround(s, 0, &su) == 0 && session_get_surround(s, 0, NULL), "the channel is in surround");
    SurPt mv[2] = { { 0.5f, -45, 0 }, { 1.5f, 45, 0 } };
    session_clip_set_sur_points(s, 0, 0, mv, 2);
    CHECK(session_clip_get_sur_points(s, 0, 0, NULL, 99) == 2, "the clip has 2 points");
    session_play_clip_from(s, 0, 0, 0);
    long total = SR * 2;
    static float out[(SR * 2 + 512) * OUTS];
    for (long pp = 0; pp < total; pp += 256) { nap(2); mixer_render(s, out + pp * OUTS, 256, OUTS); }
    double e_[OUTS], e2[OUTS], e3[OUTS];
    for (int k = 0; k < OUTS; ++k) { e_[k] = e2[k] = e3[k] = 0; }
    for (long i = SR / 4; i < SR / 2; ++i) for (int k = 0; k < OUTS; ++k) e_[k] += (double)out[i * OUTS + k] * out[i * OUTS + k];
    for (long i = SR - 1200; i < SR + 1200; ++i) for (int k = 0; k < OUTS; ++k) e2[k] += (double)out[i * OUTS + k] * out[i * OUTS + k];
    for (long i = SR * 7 / 4; i < SR * 2; ++i) for (int k = 0; k < OUTS; ++k) e3[k] += (double)out[i * OUTS + k] * out[i * OUTS + k];
    CHECK(e_[2] > 1.0 && e_[3] < e_[2] * 1e-6 && e_[4] < e_[2] * 1e-6 && e_[5] < e_[2] * 1e-6, "at the start: only FL (output 3) - %.1f / %.1g", e_[2], e_[3]);
    CHECK(e_[0] == 0.0 && e_[1] == 0.0 && e3[0] == 0.0, "nothing goes to the master (outputs 1-2)");
    CHECK(fabs(e2[2] / e2[3] - 1.0) < 0.15 && e2[4] < e2[2] * 1e-6, "in the middle: FL and FR equal (%.2f), not the rear", e2[2] / e2[3]);
    CHECK(e3[3] > 1.0 && e3[2] < e3[3] * 1e-6, "at the end: only FR (output 4)");
    double tot1 = e_[2] + e_[3] + e_[4] + e_[5], tot2 = (e2[2] + e2[3] + e2[4] + e2[5]) * (SR / 4.0) / 2400.0;
    CHECK(fabs(tot2 / tot1 - 1.0) < 0.05, "the same loudness while moving (%.3f)", tot2 / tot1);
    float naz, nsp;
    CHECK(session_surround_now(s, 0, &naz, &nsp) && fabsf(naz - 45) < 1, "the UI sees where it is (%.0f deg)", naz);
    /* the gains glide: no jump between two blocks (a click) */
    double maxd = 0;
    for (long i = SR / 2; i < SR * 3 / 2; ++i) { double d = fabs(out[i * OUTS + 2] - out[(i - 1) * OUTS + 2]); if (d > maxd) maxd = d; }
    CHECK(maxd < 0.3 * 2 * M_PI * 440.0 / SR * 1.2, "no clicks while moving (largest step %.4f)", maxd);

    /* 4) the project */
    CHECK(project_save(s, "/tmp/qlim_sur.limproject") == 0, "saved");
    static Session T;
    session_init(&T, SR);
    atomic_store(&T.audio_running, false);
    SurSetup tu;
    SurPt tp[8];
    CHECK(project_load(&T, "/tmp/qlim_sur.limproject") == 0 && session_get_surround(&T, 0, &tu) && tu.n == 4 && tu.out[0] == 2 &&
          session_clip_get_sur_points(&T, 0, 0, tp, 8) == 2 && fabsf(tp[1].az - 45) < 0.01f, "loaded: the surround and the points");
    session_set_surround(&T, 0, NULL);
    CHECK(!session_get_surround(&T, 0, NULL), "off again");
    session_stop_all(s);

    /* 5) 1.4.22: the room mode (a theatre: back, house, front, stage L/R, stage centre) */
    {
        SurSetup r;
        memset(&r, 0, sizeof r);
        sur_room_layout(&r, SUR_R_THEATER9, 0);
        CHECK(r.mode == SUR_MODE_ROOM && r.n == 9 && !strcmp(sur_room_speaker_name(SUR_R_THEATER9, 8), "Stage C") && r.out[8] == 8,
              "Theater (9): 9 speakers, Stage C on output 9");
        float gr_[SUR_MAX_SPK];
        sur_gains_room(&r, r.x[8], r.y[8], 0, gr_);
        double others = 0; for (int k = 0; k < 8; ++k) others += (double)gr_[k] * gr_[k];
        CHECK(gr_[8] > 0.97f && others < 0.06, "on the Stage C speaker: nearly only it (%.3f, the rest %.1f dB)", gr_[8], 10 * log10(others));
        sur_gains_room(&r, 0, 10, 0, gr_);
        CHECK(fabsf(gr_[2] - gr_[3]) < 1e-5f && gr_[2] > gr_[0] && gr_[2] > gr_[4], "in the middle of the hall: House L = House R, the loudest");
        bool cpr = true;
        for (float xx = -8; xx <= 8; xx += 1.7f) for (float yy = 0; yy <= 26; yy += 2.3f) for (int sp = 0; sp <= 4; ++sp) {
            sur_gains_room(&r, xx, yy, sp * 0.25f, gr_);
            if (fabs(pw(gr_, 9) - 1.0) > 1e-4) cpr = false;
        }
        CHECK(cpr, "everywhere in the room, every spread: the power is 1");
        sur_gains_room(&r, 0, 13, 1.0f, gr_);
        float mn = 1, mx = 0; for (int k = 0; k < 9; ++k) { if (gr_[k] < mn) mn = gr_[k]; if (gr_[k] > mx) mx = gr_[k]; }
        CHECK(mx / mn < 1.6f, "spread 100%%: every speaker nearly the same (%.2f)", mx / mn);
        /* the text form keeps the room */
        r.x[2] = -7.25f; r.x0 = 1.5f; r.y0 = 2.5f; r.w0 = 12.0f;
        char tx2[1600];
        sur_setup_text(&r, tx2, sizeof tx2);
        SurSetup r2;
        CHECK(sur_setup_parse(tx2, &r2) && r2.mode == SUR_MODE_ROOM && r2.n == 9 && fabsf(r2.x[2] + 7.25f) < 0.01f && fabsf(r2.y[8] - 25.5f) < 0.01f &&
              fabsf(r2.w0 - 12) < 0.01f && fabsf(r2.room_d - 26) < 0.01f, "the room as text and back");
        /* an old (1.4.19) text: circle, a plan made from the angles */
        CHECK(sur_setup_parse("4|2,3,4,5|-45.0,45.0,-135.0,135.0|60.0|0.0|0.000|0", &r2) && r2.mode == SUR_MODE_CIRCLE && r2.x[0] < 0 && r2.y[0] > 10,
              "a 1.4.19 setup still loads (circle)");
        /* points with x y w */
        SurPt rp[2] = { { 0, 0, 0, 0, 1, 12 }, { 4, 0, 0, 0, 25.5f, 0 } };
        float ex, ey, ew, es;
        sur_eval_room(rp, 2, 2.0f, &r, &ex, &ey, &ew, &es);
        CHECK(fabsf(ey - 13.25f) < 1e-3f && fabsf(ew - 6) < 1e-3f, "halfway: in the middle of the hall, half as wide");
        char pt[400];
        sur_points_text(rp, 2, pt, sizeof pt);
        SurPt rq[4];
        CHECK(sur_points_parse(pt, rq, 4) == 2 && fabsf(rq[0].w - 12) < 0.01f && fabsf(rq[1].y - 25.5f) < 0.01f, "room points as text and back (%s)", pt);
        SurPt oldp[2];
        CHECK(sur_points_parse("0.5000:-45.0:0.000 1.5:45.0:0.2", oldp, 2) == 2 && oldp[1].w == 0 && fabsf(oldp[1].spread - 0.2f) < 1e-4f, "1.4.19 points still load");

        /* the mixer: the theatre on outputs 1-9; the clip goes from the back (stereo 12 m) to Stage C (mono) in 1.5 s */
        static Session U;
        session_init(&U, SR);
        atomic_store(&U.audio_running, false);
        session_add_channel(&U, "Walk");
        session_add_scene(&U);
        session_set_quant_mode(&U, 0);
        session_set_clip(&U, 0, 0, "/tmp/qlim_sur.wav");
        for (int w2 = 0; w2 < 400 && !U.channels[0].clips[0].stream; ++w2) nap(5);
        session_set_surround(&U, 0, &r);
        SurPt walk[2] = { { 0.25f, 0, 0, 0, 0.5f, 12 }, { 1.75f, 0, 0, 0, 25.5f, 0 } };
        session_clip_set_sur_points(&U, 0, 0, walk, 2);
        session_play_clip_from(&U, 0, 0, 0);
        static float o2[(SR * 2 + 512) * OUTS];
        for (long pp = 0; pp < SR * 2; pp += 256) { nap(2); mixer_render(&U, o2 + pp * OUTS, 256, OUTS); }
        double eb[OUTS], ee[OUTS];
        for (int k = 0; k < OUTS; ++k) eb[k] = ee[k] = 0;
        for (long i = SR / 16; i < SR / 4; ++i) for (int k = 0; k < OUTS; ++k) eb[k] += (double)o2[i * OUTS + k] * o2[i * OUTS + k];
        for (long i = SR * 7 / 4 + 1000; i < SR * 2; ++i) for (int k = 0; k < OUTS; ++k) ee[k] += (double)o2[i * OUTS + k] * o2[i * OUTS + k];
        double tb = 0, te = 0; for (int k = 0; k < 9; ++k) { tb += eb[k]; te += ee[k]; }
        CHECK((eb[4] + eb[5]) / tb > 0.75, "at the start: mostly Back L + Back R (%.0f%%)", 100 * (eb[4] + eb[5]) / tb);
        CHECK(ee[8] / te > 0.9, "at the end: Stage C (%.0f%%)", 100 * ee[8] / te);
        float vx, vy, vw;
        CHECK(session_surround_now_room(&U, 0, &vx, &vy, &vw) && fabsf(vy - 25.5f) < 0.1f && vw < 0.01f, "the UI sees it on the stage (%.1f m)", vy);
        CHECK(project_save(&U, "/tmp/qlim_sur2.limproject") == 0, "saved");
        static Session V;
        session_init(&V, SR);
        atomic_store(&V.audio_running, false);
        SurSetup vu; SurPt vp[4];
        CHECK(project_load(&V, "/tmp/qlim_sur2.limproject") == 0 && session_get_surround(&V, 0, &vu) && vu.mode == SUR_MODE_ROOM && vu.n == 9 &&
              session_clip_get_sur_points(&V, 0, 0, vp, 4) == 2 && fabsf(vp[0].w - 12) < 0.01f && fabsf(vp[1].y - 25.5f) < 0.01f,
              "loaded: the room and the path");
        session_stop_all(&U);
    }

    /* 6) 1.4.23: export in surround */
    {
        static Session X;
        session_init(&X, SR);
        atomic_store(&X.audio_running, false);
        session_add_channel(&X, "Walk");
        session_add_channel(&X, "Plain");
        session_add_scene(&X);
        session_set_quant_mode(&X, 0);
        session_set_clip(&X, 0, 0, "/tmp/qlim_sur.wav");
        session_set_clip(&X, 1, 0, "/tmp/qlim_sur.wav");
        for (int w2 = 0; w2 < 400 && (!X.channels[0].clips[0].stream || !X.channels[1].clips[0].stream); ++w2) nap(5);
        SurSetup r;
        memset(&r, 0, sizeof r);
        sur_room_layout(&r, SUR_R_THEATER9, 0);
        session_set_surround(&X, 0, &r);
        SurPt walk[2] = { { 0.5f, 0, 0, 0, 0.5f, 12 }, { 3.5f, 0, 0, 0, 25.5f, 0 } };
        session_clip_set_sur_points(&X, 0, 0, walk, 2);
        ExportOpts eo = { EXP_WAV, 32, 0, 0, 0, EXP_SUR_ONE };
        char err[300] = "", msg[400] = "";
        ExportJob *j = export_clip_start(&X, 0, 0, &eo, "/tmp/qlim_surx/walk.wav", err, sizeof err);
        if (system("mkdir -p /tmp/qlim_surx && rm -f /tmp/qlim_surx/*")) {}
        if (j) { while (!export_finished(j)) nap(5); export_result(j, msg, sizeof msg); export_free(j); }
        j = export_clip_start(&X, 0, 0, &eo, "/tmp/qlim_surx/walk.wav", err, sizeof err);
        int rc = 1;
        if (j) { while (!export_finished(j)) nap(5); rc = export_result(j, msg, sizeof msg); export_free(j); }
        CHECK(rc == 0, "a clip through its path: one file (%s%s)", msg, err);
        SF_INFO si = { 0 };
        SNDFILE *f = sf_open("/tmp/qlim_surx/walk.wav", SFM_READ, &si);
        long fr = f ? (long)si.frames : 0;
        float *b = f ? malloc(sizeof(float) * (size_t)fr * 9) : NULL;
        if (f) { sf_readf_float(f, b, fr); sf_close(f); }
        CHECK(si.channels == 9 && (si.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_WAVEX && fr == SR * 4, "9 channels (WAVE extensible), 4 s");
        double e0[9] = { 0 }, e1[9] = { 0 };
        for (long i = SR / 8; i < SR / 2 && b; ++i) for (int k = 0; k < 9; ++k) e0[k] += (double)b[i * 9 + k] * b[i * 9 + k];
        for (long i = SR * 15 / 4; i < SR * 4 && b; ++i) for (int k = 0; k < 9; ++k) e1[k] += (double)b[i * 9 + k] * b[i * 9 + k];
        double t0 = 0, t1 = 0; for (int k = 0; k < 9; ++k) { t0 += e0[k]; t1 += e1[k]; }
        CHECK(t0 > 0 && (e0[4] + e0[5]) / t0 > 0.75 && t1 > 0 && e1[8] / t1 > 0.9, "the start on Back L/R (%.0f%%), the end on Stage C (%.0f%%)",
              t0 > 0 ? 100 * (e0[4] + e0[5]) / t0 : 0, t1 > 0 ? 100 * e1[8] / t1 : 0);
        /* the same as the mixer plays it */
        session_play_clip_from(&X, 0, 0, 0);
        static float o3[(SR * 4 + 512) * OUTS];
        for (long pp = 0; pp < SR * 4; pp += 256) { nap(1); mixer_render(&X, o3 + pp * OUTS, 256, OUTS); }
        session_stop_all(&X);
        double dm = 0;
        int lag = -1;                                         /* (the mixer may start a block later) */
        for (int L2 = 0; L2 <= 1024 && b; L2 += 256) {
            double d = 0;
            for (long i = SR; i < SR * 3; ++i) for (int k = 0; k < 9; ++k) { double v = fabs(b[i * 9 + k] - o3[(i + L2) * OUTS + k]); if (v > d) d = v; }
            if (lag < 0 || d < dm) { dm = d; lag = L2; }
        }
        CHECK(dm < 2e-3, "exactly what the mixer plays (difference %.1e)", dm);
        free(b);
        /* a file per speaker */
        eo.sur = EXP_SUR_SPLIT; eo.bits = 24;
        j = export_clip_start(&X, 0, 0, &eo, "/tmp/qlim_surx/walk.wav", err, sizeof err);
        rc = 1;
        if (j) { while (!export_finished(j)) nap(5); rc = export_result(j, msg, sizeof msg); export_free(j); }
        SF_INFO s2 = { 0 };
        SNDFILE *g = sf_open("/tmp/qlim_surx/walk - Stage C.wav", SFM_READ, &s2);
        CHECK(rc == 0 && g && s2.channels == 1 && s2.frames == SR * 4, "a file per speaker: \"walk - Stage C.wav\" mono (%s)", msg);
        if (g) sf_close(g);
        int nfiles = 0;
        for (int k = 0; k < 9; ++k) {
            char pth[300], lb[48];
            sur_speaker_label(&r, k, lb, sizeof lb);
            snprintf(pth, sizeof pth, "/tmp/qlim_surx/walk - %s.wav", lb);
            SF_INFO s3 = { 0 };
            SNDFILE *h = sf_open(pth, SFM_READ, &s3);
            if (h) { nfiles++; sf_close(h); }
        }
        CHECK(nfiles == 9, "all 9 speakers' files");
        /* 5.1: 6 channels, the LFE empty, the standard layout */
        SurSetup c51;
        memset(&c51, 0, sizeof c51);
        sur_layout(&c51, SUR_L_51, 0);
        session_set_surround(&X, 0, &c51);
        SurPt sw[2] = { { 0, -30, 0, 0, 0, 0 }, { 4, 110, 0, 0, 0, 0 } };
        session_clip_set_sur_points(&X, 0, 0, sw, 2);
        eo.sur = EXP_SUR_ONE;
        j = export_clip_start(&X, 0, 0, &eo, "/tmp/qlim_surx/c51.wav", err, sizeof err);
        rc = 1;
        if (j) { while (!export_finished(j)) nap(5); rc = export_result(j, msg, sizeof msg); export_free(j); }
        SF_INFO s4 = { 0 };
        SNDFILE *h = sf_open("/tmp/qlim_surx/c51.wav", SFM_READ, &s4);
        int cmap[8] = { 0 };
        bool mapok = h && sf_command(h, SFC_GET_CHANNEL_MAP_INFO, cmap, sizeof(int) * 6) == SF_TRUE;
        float *c5 = h ? malloc(sizeof(float) * (size_t)s4.frames * 6) : NULL;
        if (h) { sf_readf_float(h, c5, s4.frames); sf_close(h); }
        double lfe = 0, lft = 0, rs = 0;
        for (long i = 0; c5 && i < SR / 4; ++i) { lfe += fabs(c5[i * 6 + 3]); lft += fabs(c5[i * 6 + 0]); }
        for (long i = SR * 15 / 4; c5 && i < SR * 4; ++i) rs += fabs(c5[i * 6 + 5]);
        CHECK(rc == 0 && s4.channels == 6 && lfe == 0.0 && lft > 100 && rs > 100, "5.1: 6 channels, the LFE silent, L at the start, Rs at the end");
        CHECK(mapok && cmap[0] == SF_CHANNEL_MAP_LEFT && cmap[3] == SF_CHANNEL_MAP_LFE && cmap[5] == SF_CHANNEL_MAP_REAR_RIGHT, "5.1: the channel layout is written");
        free(c5);
        /* a scene: every output as it plays (the theatre on 1-9 and the plain channel to the master 1-2) */
        session_set_surround(&X, 0, &r);
        session_clip_set_sur_points(&X, 0, 0, walk, 2);
        CHECK(export_scene_outputs(&X) == 9, "the scene uses outputs 1-9");
        eo.sur = EXP_SUR_ONE; eo.bits = 32;
        j = export_scene_start(&X, 0, 2.0, &eo, "/tmp/qlim_surx/scene.wav", err, sizeof err);
        CHECK(j != NULL, "Export Scene in surround starts (%s)", err);
        float blk[256 * OUTS];
        for (int k = 0; j && !export_finished(j) && k < 4000; ++k) { mixer_render(&X, blk, 256, OUTS); nap(1); }
        rc = j ? export_result(j, msg, sizeof msg) : 1;
        export_free(j);
        session_stop_all(&X);
        SF_INFO s5 = { 0 };
        SNDFILE *sc5 = sf_open("/tmp/qlim_surx/scene.wav", SFM_READ, &s5);
        float *d5 = sc5 ? malloc(sizeof(float) * (size_t)s5.frames * 9) : NULL;
        if (sc5) { sf_readf_float(sc5, d5, s5.frames); sf_close(sc5); }
        double eb = 0, ef = 0, eh = 0;
        for (long i = SR / 8; d5 && i < SR / 2; ++i) { eb += fabs(d5[i * 9 + 4]); ef += fabs(d5[i * 9 + 0]); eh += fabs(d5[i * 9 + 2]); }
        CHECK(rc == 0 && s5.channels == 9 && s5.frames == SR * 2, "the scene: 9 channels, 2 s (%s)", msg);
        CHECK(eb > 100 && ef > 100, "Back L has the walk, output 1 has the plain channel (the master) too");
        free(d5);
        eo.sur = EXP_SUR_SPLIT;
        j = export_scene_start(&X, 0, 1.0, &eo, "/tmp/qlim_surx/scene.wav", err, sizeof err);
        for (int k = 0; j && !export_finished(j) && k < 4000; ++k) { mixer_render(&X, blk, 256, OUTS); nap(1); }
        rc = j ? export_result(j, msg, sizeof msg) : 1;
        export_free(j);
        session_stop_all(&X);
        SF_INFO s6 = { 0 };
        SNDFILE *o9 = sf_open("/tmp/qlim_surx/scene - Out 9.wav", SFM_READ, &s6);
        CHECK(rc == 0 && o9 && s6.channels == 1, "the scene, a file per output: \"scene - Out 9.wav\" (%s)", msg);
        if (o9) sf_close(o9);
    }

    printf("SURROUND: %s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
