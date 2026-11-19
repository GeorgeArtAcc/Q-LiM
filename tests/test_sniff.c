/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.14: the format is found from the file's content, not from its name (src/stream_sndfile.c audio_sniff).
 * Run with a folder made by gen_sniff.sh (ffmpeg): real files, some of them renamed on purpose. */
#define _POSIX_C_SOURCE 200809L
#include "stream.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c, ...) do { if (c) { printf("ok   "); } else { printf("FAIL "); g_fail = 1; } printf(__VA_ARGS__); printf("\n"); } while (0)

static void one(const char *dir, const char *name, int want, int can_open)
{
    char p[1024], what[32];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    int k = audio_sniff(p, what, sizeof what);
    int c = audio_sndfile_can_open(p);
    CHECK(k == want && c == can_open, "%-22s -> %-14s (kind %d, libsndfile %s)", name, what, k, c ? "yes" : "no");
    if (c) {   /* what libsndfile is given must open without complaint */
        StreamSource s; int sr = 0;
        int r = stream_source_sndfile_native(p, &s, &sr);
        CHECK(r == 0 && sr > 0, "    ... and it opens (%d Hz)", sr);
        if (r == 0) s.close(s.ctx);
    } else {
        StreamSource s; int sr = 0;
        CHECK(stream_source_sndfile_native(p, &s, &sr) != 0, "    ... and libsndfile is not even asked");
    }
}

int main(int argc, char **argv)
{
    const char *d = argc > 1 ? argv[1] : "/tmp/qlim_sniff";
    one(d, "a.wav", SNIFF_SNDFILE, 1);
    one(d, "a.flac", SNIFF_SNDFILE, 1);
    one(d, "a.ogg", SNIFF_SNDFILE, 1);
    one(d, "a.opus", SNIFF_SNDFILE, 1);
    one(d, "a.aiff", SNIFF_SNDFILE, 1);
    one(d, "a.m4a", SNIFF_FFMPEG, 0);
    one(d, "m4a_named.mp3", SNIFF_FFMPEG, 0);           /* the case from the concert: no mpg123 noise */
    one(d, "a.aac", SNIFF_FFMPEG, 0);
    one(d, "wav_named.mp3", SNIFF_SNDFILE, 1);
    one(d, "flac_named.wav", SNIFF_SNDFILE, 1);
    one(d, "junk.mp3", SNIFF_UNKNOWN, 0);
    one(d, "missing.wav", SNIFF_MISSING, 0);
    if (audio_sndfile_has_mp3()) {
        one(d, "a.mp3", SNIFF_MP3, 1);
        one(d, "id3.mp3", SNIFF_MP3, 1);
        one(d, "mp3_named.m4a", SNIFF_MP3, 1);
    } else {
        printf("     (this libsndfile has no MP3: MP3 goes to ffmpeg)\n");
        one(d, "a.mp3", SNIFF_MP3, 0);
    }
    printf("SNIFF: %s\n", g_fail ? "FAILED" : "all ok");
    return g_fail;
}
