/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.24: surround (multichannel) files - every channel on its own output (also through the live resampler and the
 * cuts), the fades, the switch to stereo and back, the project and undo keep it, and the "path" analysis. */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/project.h"
#include "../src/stream.h"
#include "../src/mcfile.h"
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
#define OUTS 12
#define NCH 6

/* channel k: a sine of 200 + 100 k Hz at 0.1 * (k + 1) */
static void write_mc(const char *path, int sr, int secs)
{
    SF_INFO si = { 0 };
    si.samplerate = sr; si.channels = NCH; si.format = SF_FORMAT_WAVEX | SF_FORMAT_FLOAT;
    SNDFILE *f = sf_open(path, SFM_WRITE, &si);
    if (!f) return;
    int map[NCH] = { SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_CENTER, SF_CHANNEL_MAP_LFE,
                     SF_CHANNEL_MAP_REAR_LEFT, SF_CHANNEL_MAP_REAR_RIGHT };
    sf_command(f, SFC_SET_CHANNEL_MAP_INFO, map, sizeof map);
    long n = (long)sr * secs;
    float *b = malloc(sizeof(float) * (size_t)n * NCH);
    for (long i = 0; i < n; ++i)
        for (int k = 0; k < NCH; ++k) b[i * NCH + k] = 0.1f * (float)(k + 1) * (float)sin(2 * M_PI * (200 + 100 * k) * (double)i / sr);
    sf_writef_float(f, b, n);
    sf_close(f);
    free(b);
}

/* the amplitude of each output over [a, b) (RMS * sqrt 2) */
static void amps(const float *o, long a, long b, double *am)
{
    for (int k = 0; k < OUTS; ++k) {
        double s = 0;
        for (long i = a; i < b; ++i) s += (double)o[i * OUTS + k] * o[i * OUTS + k];
        am[k] = sqrt(2.0 * s / (double)(b - a));
    }
}

static float out[(SR * 2 + 1024) * OUTS];
static const char *s_path_of(Session *s, int ch, int sc) { return s->channels[ch].clips[sc].path; }

static long play(Session *s, int ch, long frames)
{
    session_play_clip_from(s, ch, 0, 0);
    long pp = 0;
    for (; pp < frames; pp += 256) { nap(1); mixer_render(s, out + pp * OUTS, 256, OUTS); }
    session_stop_all(s);
    return pp;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    streams_init();
    write_mc("/tmp/qlim_mc6.wav", SR, 2);
    write_mc("/tmp/qlim_mc6_44.wav", 44100, 2);
    CHECK(stream_file_channels("/tmp/qlim_mc6.wav") == NCH, "the file has 6 channels");

    static Session S;
    session_init(&S, SR);
    atomic_store(&S.audio_running, false);
    session_add_channel(&S, "Surround");
    session_add_scene(&S);
    session_set_quant_mode(&S, 0);

    /* 1) as always: the first two channels, in stereo */
    session_set_clip(&S, 0, 0, "/tmp/qlim_mc6.wav");
    CHECK(session_clip_mc_mode(&S, 0, 0) == CLIP_MC_STEREO && session_clip_mc_channels(&S, 0, 0) == NCH, "loaded as stereo, it knows it has 6 channels");
    nap(50);
    long n = play(&S, 0, SR);
    double am[OUTS];
    amps(out, SR / 4, n - 256, am);
    CHECK(fabs(am[0] - 0.1) < 0.01 && fabs(am[1] - 0.2) < 0.01 && am[2] < 1e-4, "stereo: L R of the file on 1-2 (%.3f %.3f)", am[0], am[1]);

    /* 2) multichannel, from output 3 */
    CHECK(session_clip_set_mc(&S, 0, 0, CLIP_MC_MULTI, NULL, NULL) == 0, "switched to multichannel");
    session_clip_set_mc_out(&S, 0, 0, 2);
    CHECK(session_clip_mc_mode(&S, 0, 0) == CLIP_MC_MULTI && session_clip_mc_out(&S, 0, 0) == 2, "multichannel, from output 3");
    nap(50);
    n = play(&S, 0, SR);
    amps(out, SR / 4, n - 256, am);
    bool okm = am[0] < 1e-4 && am[1] < 1e-4 && am[8] < 1e-4;
    for (int k = 0; k < NCH; ++k) if (fabs(am[2 + k] - 0.1 * (k + 1)) > 0.01) okm = false;
    CHECK(okm, "channel k on output 3 + k (%.2f %.2f %.2f %.2f %.2f %.2f), nothing on 1-2", am[2], am[3], am[4], am[5], am[6], am[7]);
    /* the fader moves all of them */
    float g0 = atomic_load(&S.channels[0].gain);
    atomic_store(&S.channels[0].gain, 0.6f);          /* -10 dB */
    atomic_store(&S.channels[0].smooth_gain, 0.6f);
    n = play(&S, 0, SR / 2);
    amps(out, SR / 8, n - 256, am);
    double want = 0.1 * pow(10.0, session_gain_to_db(0.6f) / 20.0);
    CHECK(fabs(am[2] - want) < 0.005 && fabs(am[7] - 6 * want) < 0.02, "the channel's fader moves every output (%.3f, want %.3f)", am[2], want);
    atomic_store(&S.channels[0].gain, g0);
    atomic_store(&S.channels[0].smooth_gain, g0);
    /* no clicks on a restart: the old sound fades out, the new one in (no step bigger than the sine's own) */
    n = play(&S, 0, 2048);
    float step = 0, later = 0;
    for (int i = 0; i < 600; ++i) { float d = fabsf(out[(i + 1) * OUTS + 7] - out[i * OUTS + 7]); if (d > step) step = d; }
    for (int i = 400; i < 800; ++i) if (fabsf(out[i * OUTS + 7]) > later) later = fabsf(out[i * OUTS + 7]);
    CHECK(step < 0.08f && later > 0.55f, "a restart without a click (largest step %.3f, the sine's own 0.055)", step);
    /* warp / pitch do not apply */
    session_set_clip_warp(&S, 0, 0, true);
    session_set_clip_pitch(&S, 0, 0, 3.0f);
    CHECK(!session_get_clip_warp(&S, 0, 0) && session_get_clip_pitch(&S, 0, 0) == 0.0f, "no warp / pitch on a multichannel clip");

    /* 2b) 2.0: the Speaker Map - C (channel 3) on outputs 10 and 11, the LFE silent, Rs on output 1 */
    session_clip_set_mc_map(&S, 0, 0, 2, (1ull << 9) | (1ull << 10));
    session_clip_set_mc_map(&S, 0, 0, 3, SUR_MUTED);
    session_clip_set_mc_map(&S, 0, 0, 5, 1ull << 0);
    n = play(&S, 0, SR);
    amps(out, SR / 4, n - 256, am);
    CHECK(fabs(am[9] - 0.3) < 0.01 && fabs(am[10] - 0.3) < 0.01 && am[4] < 1e-4 && am[5] < 1e-4 && fabs(am[0] - 0.6) < 0.01 &&
          fabs(am[2] - 0.1) < 0.01 && fabs(am[6] - 0.5) < 0.01 && am[7] < 1e-4,
          "Speaker Map: C on 10 + 11 (%.2f %.2f), LFE silent, Rs on 1 (%.2f), the rest as before", am[9], am[10], am[0]);

    /* 3) cut: the second half out -> the clip is 1 s */
    {
        const EditList *el = session_clip_edits(&S, 0, 0);
        EditList *c2 = el ? edit_crop(el, 0, SR) : NULL;
        if (c2) session_clip_apply_edits(&S, 0, 0, c2, -1);
        nap(50);
        n = play(&S, 0, SR + SR / 2);
        amps(out, SR / 4, SR - 2048, am);
        double tail[OUTS];
        amps(out, SR + 1024, n - 256, tail);
        CHECK(fabs(am[6] - 0.5) < 0.01 && tail[6] < 1e-4 && !atomic_load(&S.channels[0].clips[0].playing), "the cuts work on every channel (1 s, then it ends)");
    }

    /* 3b) export: all 6 channels (one file / a file each), as cut; the scene counts its outputs */
    {
        if (system("mkdir -p /tmp/qlim_mcx && rm -f /tmp/qlim_mcx/*")) {}
        ExportOpts eo = { EXP_WAV, 32, 0, 0, 0, EXP_SUR_ONE };
        char err[300] = "", msg[400] = "";
        ExportJob *j = export_clip_start(&S, 0, 0, &eo, "/tmp/qlim_mcx/m.wav", err, sizeof err);
        int rc = 1;
        if (j) { while (!export_finished(j)) nap(5); rc = export_result(j, msg, sizeof msg); export_free(j); }
        SF_INFO si = { 0 };
        SNDFILE *f = sf_open("/tmp/qlim_mcx/m.wav", SFM_READ, &si);
        float *b = f ? malloc(sizeof(float) * (size_t)si.frames * NCH) : NULL;
        if (f) { sf_readf_float(f, b, si.frames); sf_close(f); }
        double e5 = 0;
        for (long i = 0; b && i < si.frames; ++i) e5 += (double)b[i * NCH + 5] * b[i * NCH + 5];
        CHECK(rc == 0 && si.channels == NCH && si.frames == SR && fabs(sqrt(2 * e5 / (si.frames ? si.frames : 1)) - 0.6) < 0.01,
              "export: one file, 6 channels, 1 s (cut), channel 6 at 0.6 (%s%s)", msg, err);
        free(b);
        eo.sur = EXP_SUR_SPLIT;
        j = export_clip_start(&S, 0, 0, &eo, "/tmp/qlim_mcx/m.wav", err, sizeof err);
        rc = 1;
        if (j) { while (!export_finished(j)) nap(5); rc = export_result(j, msg, sizeof msg); export_free(j); }
        SF_INFO s2 = { 0 };
        SNDFILE *g = sf_open("/tmp/qlim_mcx/m - LFE.wav", SFM_READ, &s2);
        CHECK(rc == 0 && g && s2.channels == 1 && s2.frames == SR, "export: a file per channel (\"m - LFE.wav\") (%s)", msg);
        if (g) sf_close(g);
        CHECK(export_scene_outputs(&S) == 11, "the scene export counts the outputs through the Speaker Map: 1-11 (%d)", export_scene_outputs(&S));
    }

    /* 4) project + undo keep it */
    CHECK(project_save(&S, "/tmp/qlim_mc.limproject") == 0, "saved");
    static Session T;
    session_init(&T, SR);
    atomic_store(&T.audio_running, false);
    CHECK(project_load(&T, "/tmp/qlim_mc.limproject") == 0 && session_clip_mc_mode(&T, 0, 0) == CLIP_MC_MULTI && session_clip_mc_out(&T, 0, 0) == 2 &&
          T.channels[0].clips[0].wav.frame_count == SR, "loaded: multichannel from output 3, still cut");
    CHECK(session_clip_mc_map(&T, 0, 0, 2) == ((1ull << 9) | (1ull << 10)) && session_clip_mc_map(&T, 0, 0, 3) == SUR_MUTED &&
          session_clip_mc_map(&T, 0, 0, 0) == 0, "loaded: the Speaker Map too");

    /* 5) a 44.1 kHz file: resampled live, every channel */
    session_load_mc_next(CLIP_MC_MULTI, 0, NULL);
    session_set_clip(&S, 0, 0, "/tmp/qlim_mc6_44.wav");
    CHECK(session_clip_mc_mode(&S, 0, 0) == CLIP_MC_MULTI && stream_channels(S.channels[0].clips[0].stream) == NCH, "44.1 kHz: multichannel at 48 kHz");
    nap(50);
    n = play(&S, 0, SR);
    amps(out, SR / 4, n - 256, am);
    okm = true;
    for (int k = 0; k < NCH; ++k) if (fabs(am[k] - 0.1 * (k + 1)) > 0.01) okm = false;
    CHECK(okm, "44.1 kHz: each channel on its output (%.2f %.2f %.2f %.2f %.2f %.2f)", am[0], am[1], am[2], am[3], am[4], am[5]);
    /* the frequency survives the resampling: channel 5 = 700 Hz -> zero crossings */
    int zc = 0;
    for (long i = SR / 4; i < SR / 4 + SR / 2; ++i) if ((out[i * OUTS + 5] >= 0) != (out[(i + 1) * OUTS + 5] >= 0)) ++zc;
    CHECK(abs(zc - 700) <= 2, "the pitch is right after the resampling (%d crossings in 0.5 s, want 700)", zc);

    /* 6) back to stereo */
    CHECK(session_clip_set_mc(&S, 0, 0, CLIP_MC_STEREO, NULL, NULL) == 0 && stream_channels(S.channels[0].clips[0].stream) == 2, "back to stereo");

    /* 7) the path analysis: a sound that walks from L (channel 0) to Rs (channel 5) */
    {
        SF_INFO si = { 0 };
        si.samplerate = SR; si.channels = NCH; si.format = SF_FORMAT_WAVEX | SF_FORMAT_FLOAT;
        SNDFILE *f = sf_open("/tmp/qlim_walk6.wav", SFM_WRITE, &si);
        int map[NCH] = { SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_CENTER, SF_CHANNEL_MAP_LFE,
                         SF_CHANNEL_MAP_REAR_LEFT, SF_CHANNEL_MAP_REAR_RIGHT };
        sf_command(f, SFC_SET_CHANNEL_MAP_INFO, map, sizeof map);
        long len = SR * 4;
        float *b = calloc((size_t)len * NCH, sizeof(float));
        for (long i = 0; i < len; ++i) {
            double t = (double)i / len, v = 0.5 * sin(2 * M_PI * 440 * (double)i / SR);
            b[i * NCH + 0] = (float)(v * cos(t * M_PI / 2));
            b[i * NCH + 5] = (float)(v * sin(t * M_PI / 2));
        }
        sf_writef_float(f, b, len);
        sf_close(f);
        free(b);
        McJob *j = mc_job_start("/tmp/qlim_walk6.wav", "/tmp/qlim_walk6_mix.wav");
        CHECK(j != NULL, "the analysis starts");
        while (j && !mc_job_done(j)) nap(5);
        McResult r;
        int rc = j ? mc_job_result(j, &r) : 1;
        CHECK(rc == 0 && r.layout == SUR_L_51 && r.npts >= 2, "analysed: 5.1, %d points", rc == 0 ? r.npts : 0);
        if (rc == 0 && r.npts >= 2) {
            CHECK(fabsf(r.pts[0].az - (-30.0f)) < 6.0f && fabsf(r.pts[r.npts - 1].az - 110.0f) < 8.0f,
                  "from L (%.0f) to Rs (%.0f)", r.pts[0].az, r.pts[r.npts - 1].az);
            bool mono = true;
            for (int k = 0; k < r.npts; ++k) if (r.pts[k].az < -40.0f || r.pts[k].az > 120.0f || (k && r.pts[k].t <= r.pts[k - 1].t)) mono = false;
            CHECK(mono, "the way round the right side, in time order");
            CHECK(r.npts < 40, "simplified: %d points for 4 s", r.npts);
        }
        mc_job_free(j);
        SF_INFO mi = { 0 };
        SNDFILE *m = sf_open("/tmp/qlim_walk6_mix.wav", SFM_READ, &mi);
        CHECK(m && mi.channels == 2 && mi.frames == SR * 4, "the stereo mix: 2 channels, as long");
        if (m) sf_close(m);
        /* in the session: PATH mode, the original kept, back to the original */
        session_set_clip(&S, 0, 0, "/tmp/qlim_walk6.wav");
        CHECK(session_clip_set_mc(&S, 0, 0, CLIP_MC_PATH, "/tmp/qlim_walk6_mix.wav", "/tmp/qlim_walk6.wav") == 0 &&
              session_clip_mc_mode(&S, 0, 0) == CLIP_MC_PATH && !strcmp(session_clip_mc_orig(&S, 0, 0), "/tmp/qlim_walk6.wav") &&
              session_clip_mc_channels(&S, 0, 0) == NCH, "path mode: plays the mix, keeps the original");
        CHECK(project_save(&S, "/tmp/qlim_mc2.limproject") == 0, "saved");
        static Session U;
        session_init(&U, SR);
        atomic_store(&U.audio_running, false);
        CHECK(project_load(&U, "/tmp/qlim_mc2.limproject") == 0 && session_clip_mc_mode(&U, 0, 0) == CLIP_MC_PATH &&
              session_clip_mc_orig(&U, 0, 0) && !strcmp(session_clip_mc_orig(&U, 0, 0), "/tmp/qlim_walk6.wav"), "loaded: path mode with its original");
        CHECK(session_clip_set_mc(&S, 0, 0, CLIP_MC_MULTI, NULL, NULL) == 0 && session_clip_mc_mode(&S, 0, 0) == CLIP_MC_MULTI &&
              !strcmp(S.channels[0].clips[0].path, "/tmp/qlim_walk6.wav"), "from the path back to the multichannel original");
    }

    /* 8) 2.0: the Speaker Map of surround speakers: the text keeps it, the mixer plays a speaker on two outputs */
    {
        SurSetup su, back;
        memset(&su, 0, sizeof su);
        sur_layout(&su, SUR_L_50, 0);
        su.omask[2] = (1ull << 7) | (1ull << 8);       /* C -> 8 + 9 */
        su.omask[3] = SUR_MUTED;
        char t[2000];
        sur_setup_text(&su, t, sizeof t);
        CHECK(sur_setup_parse(t, &back) && back.omask[2] == su.omask[2] && back.omask[3] == SUR_MUTED && back.omask[0] == 0, "surround text keeps the Speaker Map");
        SurSetup old;
        memset(&old, 0, sizeof old);
        sur_layout(&old, SUR_L_50, 0);
        char t2[2000];
        sur_setup_text(&old, t2, sizeof t2);
        CHECK(strstr(t2, "om:") == NULL, "no Speaker Map = the text as before");
        session_set_clip(&S, 0, 0, "/tmp/qlim_mc6.wav");    /* stereo clip through the surround */
        su.az0 = 0.0f; su.spread0 = 0.0f; su.width = 0.0f;
        session_set_surround(&S, 0, &su);
        nap(50);
        n = play(&S, 0, SR / 2);
        amps(out, SR / 8, n - 256, am);
        CHECK(am[7] > 0.05 && fabs(am[7] - am[8]) < 1e-4 && am[2] < 1e-4, "a speaker on two outputs (C -> 8, 9: %.3f %.3f; its own output 3 silent)", am[7], am[8]);
        session_set_surround(&S, 0, NULL);
    }

    /* 9) 2.1: a surround file only ffmpeg reads (M4A, AAC 5.1): its copy with all the channels stands in for it */
    if (system("ffmpeg -v error -nostdin -y -i /tmp/qlim_mc6.wav -c:a aac -b:a 384k /tmp/qlim_mc6.m4a 2>/dev/null") == 0) {
        session_set_surround(&S, 0, NULL);
        session_set_clip(&S, 0, 0, "/tmp/qlim_mc6.m4a");
        CHECK(session_clip_mc_channels(&S, 0, 0) == NCH && session_clip_mc_mode(&S, 0, 0) == CLIP_MC_STEREO,
              "an M4A 5.1 is known as a 6-channel file (%d)", session_clip_mc_channels(&S, 0, 0));
        CHECK(session_clip_set_mc(&S, 0, 0, CLIP_MC_MULTI, NULL, NULL) == 0 && session_clip_mc_mode(&S, 0, 0) == CLIP_MC_MULTI,
              "the M4A plays every channel on its own output");
        session_clip_set_mc_out(&S, 0, 0, 0);
        for (int k = 0; k < NCH; ++k) session_clip_set_mc_map(&S, 0, 0, k, 0);
        nap(80);
        n = play(&S, 0, SR);
        amps(out, SR / 4, n - 256, am);
        /* AAC is lossy and its channel order is 5.1 (L R C LFE Ls Rs, the same as the WAV): about the same levels */
        bool ok9 = true;
        for (int k = 0; k < NCH; ++k) if (k != 3 && fabs(am[k] - 0.1 * (k + 1)) > 0.03) ok9 = false;   /* (LFE: low-passed) */
        CHECK(ok9, "M4A channels on outputs 1-6 (%.2f %.2f %.2f %.2f %.2f %.2f)", am[0], am[1], am[2], am[3], am[4], am[5]);
        CHECK(strcmp(s_path_of(&S, 0, 0), "/tmp/qlim_mc6.m4a") == 0, "the clip still points to the M4A (the project keeps it)");
    } else printf("ok: (no ffmpeg here - the M4A part skipped)\n");

    printf("MC: %s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
