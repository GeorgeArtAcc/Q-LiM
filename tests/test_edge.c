/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Edge cases for stability (for ASan/UBSan): a file deleted/truncated/replaced WHILE playing; 50 channels x 250 scenes;
 * queue over the maximum; crossfade with very short clips; 1-frame loop; warp tick on a 10 ms clip; sample rate change. */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/project.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void nap(long us) { struct timespec t = { us / 1000000, (us % 1000000) * 1000 }; nanosleep(&t, NULL); }
static float buf[1024 * 2];
static int render(Session *s, double sec)        /* real time; returns 1 if the output is fine */
{
    int ok = 1; double acc = 0;
    for (double t = 0; t < sec; t += 256.0 / 48000) {
        mixer_render(s, buf, 256, 2);
        for (int i = 0; i < 512; ++i) if (!isfinite(buf[i]) || fabsf(buf[i]) > 1.0001f) ok = 0;
        acc += 256.0 / 48000; if (acc > 0.016) { session_queue_tick(s); session_poll_bpm(s); acc = 0; }
        nap(5333);
    }
    return ok;
}
static void cp(const char *a, const char *b)
{
    FILE *i = fopen(a, "rb"), *o = fopen(b, "wb"); char t[65536]; size_t n;
    while ((n = fread(t, 1, sizeof t, i)) > 0) fwrite(t, 1, n, o);
    fclose(i); fclose(o);
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "test_edge long.wav short10ms.wav normal.wav\n"); return 2; }
    const char *LONG = argv[1], *SHORT = argv[2], *NORM = argv[3];
    static Session S; Session *s = &S;
    session_init(s, 48000);
    streams_init();
    atomic_store(&s->audio_running, false);
    session_add_channel(s, NULL); session_add_channel(s, NULL);
    for (int i = 0; i < 4; ++i) session_add_scene(s);

    /* 1) the file disappears / is truncated / is replaced while playing */
    const char *tmp = "/tmp/qlim_edge_vanish.wav";
    cp(LONG, tmp);
    session_set_clip(s, 0, 0, tmp);
    session_toggle_clip(s, 0, 0);
    CHECK(render(s, 1.0), "plays long file");
    if (truncate(tmp, 200000) != 0) perror("truncate");                                       /* truncated (e.g. not fully copied) */
    session_seek_clip(s, 0, 0, 48000 * 300);
    CHECK(render(s, 2.0), "file truncated while playing + seek past new end: no crash, sane output");
    unlink(tmp);                                                 /* deleted (e.g. a USB stick pulled out) */
    session_seek_clip(s, 0, 0, 1000);
    CHECK(render(s, 1.0), "file deleted while playing: no crash");
    FILE *g = fopen(tmp, "wb"); for (int i = 0; i < 300000; ++i) fputc(rand() & 255, g); fclose(g);
    session_seek_clip(s, 0, 0, 5000);
    CHECK(render(s, 1.0), "file replaced by garbage while playing: no crash");
    session_stop_all(s); session_clear_clip(s, 0, 0); unlink(tmp);

    /* 2) 1-frame loop, crossed markers, 10 ms clip with warp at extreme BPMs */
    session_set_clip(s, 0, 1, SHORT);
    session_set_loop_region(s, 0, 1, 100, 101);
    session_toggle_loop(s, 0, 1);
    session_toggle_clip(s, 0, 1);
    CHECK(render(s, 0.5), "1-frame loop");
    session_set_start_marker(s, 0, 1, 400); session_set_end_marker(s, 0, 1, 10);
    CHECK(render(s, 0.3), "crossed start/end markers");
    session_toggle_loop(s, 0, 1);
    session_set_clip_bpm_fields(s, 0, 1, 120, 0, 0);
    session_set_clip_warp(s, 0, 1, true);
    for (int k = 0; k < 6; ++k) {
        float bpm[] = { 20, 999, 60, 300, 1, 1e9f };
        session_set_clip_bpm_value(s, 0, 1, bpm[k]);
        session_toggle_clip(s, 0, 1); session_toggle_clip(s, 0, 1);
        CHECK(render(s, 0.3), "10 ms clip, warp at %g BPM", bpm[k]);
    }
    session_stop_all(s);

    /* 3) 30 s crossfade between 10 ms clips; queue over the maximum */
    session_set_clip(s, 1, 0, SHORT); session_set_clip(s, 1, 1, SHORT); session_set_clip(s, 1, 2, NORM);
    session_set_crossfade(s, 30.0f);
    session_toggle_clip(s, 1, 0);
    int added = 0;
    for (int k = 0; k < 300; ++k) added += session_queue_add(s, 1, k % 3);
    CHECK(session_queue_len(s) <= QUEUE_MAX, "queue capped at %d (tried 300, accepted %d)", QUEUE_MAX, added);
    CHECK(render(s, 3.0), "30 s crossfade between 10 ms clips: no crash, sane output");
    session_stop_all(s); session_queue_tick(s);
    session_set_crossfade(s, 0);

    /* 4) 50 channels x 250 scenes, all with clips; play everything; delete everything */
    while (s->channel_count < MAX_CHANNELS) if (session_add_channel(s, NULL) < 0) break;
    while (s->scene_count < MAX_SCENES) if (session_add_scene(s) < 0) break;
    CHECK(s->channel_count == MAX_CHANNELS && s->scene_count == MAX_SCENES, "grid %d x %d", s->channel_count, s->scene_count);
    CHECK(session_add_channel(s, NULL) < 0 && session_add_scene(s) < 0, "cannot exceed the maximum");
    int loaded = 0;
    for (int c = 0; c < MAX_CHANNELS; ++c)
        for (int k = 0; k < MAX_SCENES; k += 2) loaded += session_set_clip(s, c, k, (c + k) % 10 ? SHORT : NORM) == 0;
    printf("   loaded %d clips\n", loaded);
    session_play_scene(s, 0);
    session_play_all(s);
    CHECK(render(s, 2.0), "all channels playing");
    char *snap = project_snapshot(s, 1);
    CHECK(snap && strlen(snap) > 1000, "snapshot of a full project (%zu bytes)", snap ? strlen(snap) : 0);
    free(snap);
    project_save(s, "/tmp/qlim_edge_full.limproject");
    CHECK(project_load(s, "/tmp/qlim_edge_full.limproject") == 0, "full project save + load");
    session_play_all(s);
    CHECK(render(s, 1.0), "reloaded project playing");
    for (int c = s->channel_count - 1; c > 0; --c) session_delete_channel(s, c);
    for (int k = s->scene_count - 1; k > 0; --k) session_delete_scene(s, k);
    CHECK(render(s, 0.3), "deleted everything while playing");

    /* 5) sample rate change while playing */
    session_set_clip(s, 0, 0, NORM);
    session_toggle_clip(s, 0, 0);
    session_set_sample_rate(s, 44100);
    CHECK(render(s, 0.5), "sample rate 48000 -> 44100 while playing");
    session_set_sample_rate(s, 96000);
    CHECK(render(s, 0.5), "sample rate -> 96000 while playing");

    session_free(s);
    printf(fails ? "EDGE: %d FAILED\n" : "EDGE: all ok\n", fails);
    return fails != 0;
}
