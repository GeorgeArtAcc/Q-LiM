/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 2.1: Collect All & Save also copies a surround path's original (and the separated parts) and the project finds them.
 * 2.0: it can turn compressed files (here an MP3 at 44.1 kHz) into WAV / FLAC at the project's rate - the same samples. */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/project.h"
#include "../src/stream.h"
#include "../src/wav.h"
#include <sndfile.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void write_wav(const char *p, int ch)
{
    SF_INFO si = { .samplerate = 48000, .channels = ch, .format = SF_FORMAT_WAV | SF_FORMAT_FLOAT };
    SNDFILE *f = sf_open(p, SFM_WRITE, &si);
    static float b[4800 * 8];
    for (int i = 0; i < 4800 * ch; ++i) b[i] = 0.1f * sinf((float)i * 0.01f);
    for (int k = 0; k < 10; ++k) sf_writef_float(f, b, 4800);
    sf_close(f);
}

static bool file_has(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, needle) != NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (system("rm -rf /tmp/qlim_collect_src /tmp/qlim_collect_out && mkdir -p /tmp/qlim_collect_src") != 0) return 2;
    write_wav("/tmp/qlim_collect_src/song51.wav", 6);
    write_wav("/tmp/qlim_collect_src/song51_mix.wav", 2);
    streams_init();
    static Session S, T;
    session_init(&S, 48000);
    atomic_store(&S.audio_running, false);
    session_add_channel(&S, "A");
    session_add_scene(&S);
    /* a surround path clip: plays the stereo mix, remembers the 6-channel original */
    session_load_mc_next(CLIP_MC_PATH, 0, "/tmp/qlim_collect_src/song51.wav");
    CHECK(session_set_clip(&S, 0, 0, "/tmp/qlim_collect_src/song51_mix.wav") == 0, "the path clip loads");
    CHECK(session_clip_mc_mode(&S, 0, 0) == CLIP_MC_PATH && session_clip_mc_orig(&S, 0, 0), "it remembers the original");

    int conv = -1;
    CHECK(project_collect_and_save(&S, "/tmp/qlim_collect_out", "c", WAV_OUT_KEEP, &conv) == 0 && conv == 0, "Collect All & Save");
    CHECK(access("/tmp/qlim_collect_out/samples/song51_mix.wav", R_OK) == 0, "the mix is copied");
    CHECK(access("/tmp/qlim_collect_out/samples/song51.wav", R_OK) == 0, "the original is copied too");
    CHECK(file_has("/tmp/qlim_collect_out/c.limproject", "mc_orig=samples/song51.wav"), "the project points to the copy");

    /* the source folder is gone: the collected project still has everything */
    if (system("rm -rf /tmp/qlim_collect_src") != 0) return 2;
    session_init(&T, 48000);
    atomic_store(&T.audio_running, false);
    CHECK(project_load(&T, "/tmp/qlim_collect_out/c.limproject") == 0, "the collected project loads");
    const char *o = session_clip_mc_orig(&T, 0, 0);
    CHECK(session_clip_mc_mode(&T, 0, 0) == CLIP_MC_PATH && o && strcmp(o, "/tmp/qlim_collect_out/samples/song51.wav") == 0,
          "its original is the copy (%s)", o ? o : "(none)");
    CHECK(session_clip_mc_channels(&T, 0, 0) == 6, "and it knows the original's 6 channels (%d)", session_clip_mc_channels(&T, 0, 0));

    /* 2.0: an MP3 (44.1 kHz) -> WAV float / FLAC 24 at 48 kHz in the collected folder */
    if (system("mkdir -p /tmp/qlim_collect_src") != 0) return 2;
    {
        SF_INFO si = { .samplerate = 44100, .channels = 2, .format = SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III };
        SNDFILE *f = sf_open("/tmp/qlim_collect_src/tune.mp3", SFM_WRITE, &si);
        CHECK(f != NULL, "an MP3 is written for the test");
        if (f) {
            static float b[4410 * 2];
            for (int k = 0; k < 20; ++k) {
                for (int i = 0; i < 4410; ++i) b[2 * i] = b[2 * i + 1] = 0.3f * sinf((float)(k * 4410 + i) * 0.0627f);
                sf_writef_float(f, b, 4410);
            }
            sf_close(f);
        }
    }
    session_add_channel(&T, "MP3");
    CHECK(session_set_clip(&T, 1, 0, "/tmp/qlim_collect_src/tune.mp3") == 0, "the MP3 clip loads");
    CHECK(project_collect_count_compressed(&T) == 1, "one compressed file is counted (%d)", project_collect_count_compressed(&T));
    for (int fmt = WAV_OUT_FLOAT; fmt <= WAV_OUT_FLAC24; ++fmt) {
        char dir[128], proj[200], conv_path[200];
        snprintf(dir, sizeof dir, "/tmp/qlim_collect_conv%d", fmt);
        snprintf(proj, sizeof proj, "%s/v.limproject", dir);
        snprintf(conv_path, sizeof conv_path, "%s/samples/tune.%s", dir, fmt == WAV_OUT_FLAC24 ? "flac" : "wav");
        char cmd[200]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
        if (system(cmd) != 0) return 2;
        conv = 0;
        CHECK(project_collect_and_save(&T, dir, "v", fmt, &conv) == 0 && conv == 1, "collect with format %d: 1 converted (%d)", fmt, conv);
        CHECK(access(conv_path, R_OK) == 0, "the converted file is there");
        snprintf(cmd, sizeof cmd, "%s/samples/tune.mp3", dir);
        CHECK(access(cmd, F_OK) != 0, "the MP3 itself is not copied");
        CHECK(file_has(proj, fmt == WAV_OUT_FLAC24 ? "path=samples/tune.flac" : "path=samples/tune.wav"), "the project points to it");
        /* the same samples as the MP3 decoded and converted live (24-bit: within its step) */
        StreamSource a, b2;
        int fsr = 0;
        bool oa = session_open_clip_source(&T, "/tmp/qlim_collect_src/tune.mp3", &a) == 0;
        bool ob = stream_source_open(conv_path, 48000, &b2, &fsr) == 0;
        CHECK(oa && ob && fsr == 48000, "both open, the copy is at 48 kHz (%d)", fsr);
        if (oa && ob) {
            size_t na = a.frames(a.ctx), nb = b2.frames(b2.ctx);
            CHECK(na == nb, "the same length (%zu / %zu)", na, nb);
            static float x[4096 * 2], y[4096 * 2];
            double maxd = 0;
            for (size_t p = 0; p + 4096 <= na && p + 4096 <= nb; p += 4096) {
                a.read(a.ctx, p, x, 4096); b2.read(b2.ctx, p, y, 4096);
                for (int i = 0; i < 4096 * 2; ++i) { double d = fabs((double)x[i] - y[i]); if (d > maxd) maxd = d; }
            }
            double lim = fmt == WAV_OUT_FLOAT ? 0.0 : 1.0 / 8388608.0 * 1.01;
            CHECK(maxd <= lim, "the same samples (largest difference %.3g)", maxd);
        }
        if (oa) a.close(a.ctx);
        if (ob) b2.close(b2.ctx);
    }
    session_free(&S);
    session_free(&T);
    printf("COLLECT: %s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
