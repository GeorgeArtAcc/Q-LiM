/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Cutting through the whole audio path (step 27): file -> the cut wrapper -> stream (disk, ring, preroll) -> mixer.
 * The file is a "sawtooth": every frame knows its number -> we check frame by frame that exactly the right part plays.
 * Also: changes WHILE playing, saving/loading a project, undo text, sample rate change. */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/project.h"
#include <math.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void nap(long us) { struct timespec t = { us / 1000000, (us % 1000000) * 1000 }; nanosleep(&t, NULL); }

#define N 240000
static float val(long f) { return (float)((f % 2000) + 1) / 2001.0f * 0.5f; }   /* L; R = -L */

static void write_ramp(const char *path)
{
    SF_INFO in = { .samplerate = 48000, .channels = 2, .format = SF_FORMAT_WAV | SF_FORMAT_FLOAT };
    SNDFILE *f = sf_open(path, SFM_WRITE, &in);
    static float b[N * 2];
    for (long i = 0; i < N; ++i) { b[i * 2] = val(i); b[i * 2 + 1] = -val(i); }
    sf_writef_float(f, b, N);
    sf_close(f);
}

/* plays from the start (no crossfades/fades - we only use the middle) and returns the L channel */
static float *play(Session *s, size_t frames)
{
    size_t n = (frames + 255) / 256 * 256;
    float *o = calloc(n, sizeof(float)), buf[256 * 2];
    for (size_t d = 0; d < n; d += 256) {
        mixer_render(s, buf, 256, 2);
        for (size_t i = 0; i < 256; ++i) o[d + i] = buf[i * 2];
        nap(1500);
    }
    return o;
}

/* do master/channel have gain 1? -> compare relatively: find the scale from the first non-zero frame */
static double g_scale = 1.0;
static int compare(const float *y, size_t n, const EditList *e, size_t from, double *maxerr, size_t *first_bad)
{
    int bad = 0; *maxerr = 0; *first_bad = (size_t)-1;
    for (size_t i = 0; i < n; ++i) {
        int64_t t = (int64_t)(from + i);
        int k = edit_find(e, t);
        float want = 0.0f;
        if (k >= 0) want = val((long)(e->seg[k].src + (t - e->seg[k].t))) * edit_gain(e, k, t - e->seg[k].t);
        double d = fabs((double)y[i] / g_scale - want);
        if (d > *maxerr) *maxerr = d;
        if (d > 1e-4) { if (!bad) *first_bad = i; ++bad; }
    }
    return bad;
}

int main(void)
{
    const char *path = "/tmp/qlim_test_audio/cut_ramp.wav";
    mkdir("/tmp", 0755); mkdir("/tmp/qlim_test_audio", 0755);
    write_ramp(path);
    static Session S; Session *s = &S;
    session_init(s, 48000);
    streams_init();
    session_add_channel(s, NULL);
    session_add_scene(s);
    session_add_scene(s);
    atomic_store(&s->audio_running, false);
    /* gain 1 (0 dB), centre pan (step 36: = 0 dB) */
    session_set_channel_gain(s, 0, FADER_UNITY_POS);
    atomic_store(&s->master_gain, FADER_UNITY_POS);
    CHECK(session_set_clip(s, 0, 0, path) == 0, "ramp file loaded");
    Clip *cl = &s->channels[0].clips[0];
    nap(200000);
    for (int i = 0; i < 200; ++i) mixer_render(s, (float[512]){0}, 256, 2);     /* let the gain smoothing settle */

    /* 1) uncut: bit-exact */
    const EditList *e0 = session_clip_edits(s, 0, 0);
    CHECK(e0 && edit_is_identity(e0) && !session_clip_is_edited(s, 0, 0), "new clip is not cut");
    double me; size_t fb;
    session_toggle_clip(s, 0, 0);
    float *y = play(s, 48000);
    /* channel+master gain (pan law etc.): measured from the uncut audio */
    { double sum = 0; int cnt = 0;
      for (int q = 4000; q < 30000; ++q) if (val(q) > 0.1f) { sum += y[q] / val(q); ++cnt; }
      g_scale = cnt ? sum / cnt : 1.0; printf("   (channel gain %.5f)\n", g_scale); }
    int bad = compare(y + 2048, 40000, e0, 2048, &me, &fb);     /* after the smooth start of playback */
    CHECK(bad == 0, "uncut clip plays exactly the file (max err %.2g)", me);
    free(y);
    session_stop_all(s);
    nap(50000);

    /* 2) cuts, moves, deletes -> plays exactly according to the parts */
    EditList *a = edit_split(e0, 30000);
    EditList *b = edit_split(a, 70000);
    EditList *c = edit_delete(b, 1);                       /* 30000..70000 = silence */
    EditList *d = edit_move(c, 1, 40000);                  /* the part from 70000 goes to 40000 */
    edit_free(a); edit_free(b); edit_free(c);
    CHECK(session_clip_apply_edits(s, 0, 0, d, -1) == 0 && session_clip_is_edited(s, 0, 0), "edits applied");
    const EditList *cur = session_clip_edits(s, 0, 0);
    nap(200000);                                          /* the disk thread rebuilds the preroll */
    session_toggle_clip(s, 0, 0);
    y = play(s, 120000);
    bad = compare(y + 2048, 100000, cur, 2048, &me, &fb);
    CHECK(bad == 0, "cut + delete + move: every frame is the right part of the file (max err %.2g, first bad %zu)", me, fb);
    /* the silence 30000..40000 */
    double sil = 0; for (size_t i = 30100; i < 39900; ++i) if (fabs(y[i]) > sil) sil = fabs(y[i]);
    CHECK(sil < 1e-6, "deleted part is silent (%.2g)", sil);
    free(y);

    /* 3) a change WHILE playing: crop -> continues right away with the new one, without stopping */
    bool was = atomic_load(&cl->playing);
    EditList *cr = edit_crop(session_clip_edits(s, 0, 0), 10000, 60000);
    size_t posb = atomic_load(&cl->position);
    CHECK(was && session_clip_apply_edits(s, 0, 0, cr, 10000) == 0 && atomic_load(&cl->playing) &&
          cl->wav.frame_count == 50000 && atomic_load(&cl->end_marker) == 50000,
          "crop while playing: still playing, clip is now 50000 frames (was at %zu)", posb);
    session_stop_all(s);

    /* 4) saving / loading a project: the parts stay */
    char *txt = session_clip_edits_text(s, 0, 0);
    CHECK(txt && strstr(txt, "edit_len=50000"), "cut parts go into the project text");
    CHECK(project_save(s, "/tmp/qlim_test_audio/cut.limproject") == 0, "project saved");
    static Session S2; Session *s2 = &S2;
    session_init(s2, 48000);
    CHECK(project_load(s2, "/tmp/qlim_test_audio/cut.limproject") == 0, "project loaded");
    char *txt2 = session_clip_edits_text(s2, 0, 0);
    CHECK(txt2 && strcmp(txt, txt2) == 0 && s2->channels[0].clips[0].wav.frame_count == 50000,
          "after loading: the same parts, same length");
    free(txt2);

    /* 5) sample rate change 48k -> 44.1k: the parts are scaled and stay valid */
    session_set_sample_rate(s2, 44100);
    const EditList *sc = session_clip_edits(s2, 0, 0);
    CHECK(sc && edit_valid(sc) && llabs(sc->len - (int64_t)llround(50000 * 44100.0 / 48000.0)) <= 1 &&
          session_clip_is_edited(s2, 0, 0), "sample rate change keeps the cut (len %lld)", (long long)(sc ? sc->len : 0));
    session_free(s2);

    /* 6) back to the original */
    CHECK(session_clip_set_edits_text(s, 0, 0, NULL) == 0 && !session_clip_is_edited(s, 0, 0) &&
          cl->wav.frame_count == N, "restore original: whole file again");
    free(txt);

    session_free(s);
    printf(fails ? "CUT: %d FAILED\n" : "CUT: all ok\n", fails);
    return fails != 0;
}
