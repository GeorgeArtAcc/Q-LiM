/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* "Monkey" for the audio engine: random actions (play, stop, scenes, loop, markers, seeking,
 * warp tick/BPM, queue, crossfade, delete/move/insert, loading odd files) + audio between them.
 * Checks: no crash (ASan/UBSan), no NaN/inf, output within [-1, 1]. */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/project.h"
#include "../src/stream.h"
#include "../src/undo.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned rs;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static int R(int n) { return n > 0 ? (int)(rnd() % (unsigned)n) : 0; }
static void nap(long us) { struct timespec t = { us / 1000000, (us % 1000000) * 1000 }; nanosleep(&t, NULL); }

/* "Like in the program" mode: audio is computed on a SEPARATE thread (like PortAudio), the actions on the main (UI) one. */
#include <pthread.h>
static atomic_bool g_run_audio;
static atomic_int  g_bad;
static void *audio_main(void *arg)
{
    Session *s = arg;
    static float ab[512 * 2];
    while (atomic_load(&g_run_audio)) {
        mixer_render(s, ab, 512, 2);
        for (int i = 0; i < 1024; ++i)
            if (!isfinite(ab[i]) || ab[i] > 1.0001f || ab[i] < -1.0001f) atomic_store(&g_bad, 1);
        nap(10000);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "fuzz_engine iterations seed file...\n"); return 2; }
    int iters = atoi(argv[1]);
    rs = (unsigned)atoi(argv[2]) * 2654435761u + 1;
    char **files = argv + 3; int nf = argc - 3;
    static Session S; Session *s = &S;
    session_init(s, 48000);
    streams_init();
    atomic_store(&s->audio_running, false);
    for (int i = 0; i < 4; ++i) session_add_channel(s, NULL);
    for (int i = 0; i < 8; ++i) session_add_scene(s);
    float buf[1024 * 2];
    long checks = 0;
    undo_reset(s);
    bool conc = getenv("CONCURRENT") != NULL;
    pthread_t at;
    if (conc) {
        atomic_store(&s->audio_running, true);
        atomic_store(&g_run_audio, true);
        pthread_create(&at, NULL, audio_main, s);
    }
    for (int it = 0; it < iters; ++it) {
        int nc = s->channel_count, ns = s->scene_count;
        int ch = nc > 0 ? R(nc + 1) - (R(20) == 0) : 0;      /* sometimes out of bounds */
        int sc = ns > 0 ? R(ns + 1) - (R(20) == 0) : 0;
        int lastop = R(43);
        switch (lastop) {
        case 0: case 1: case 2: session_set_clip(s, ch, sc, files[R(nf)]); break;
        case 3: case 4: case 5: session_toggle_clip(s, ch, sc); break;
        case 6: session_play_scene(s, sc); break;
        case 7: session_stop_all(s); break;
        case 8: session_pause_all(s); break;
        case 9: session_resume_all(s); break;
        case 10: session_toggle_loop(s, ch, sc); break;
        case 11: session_set_loop_region(s, ch, sc, (size_t)R(400000), (size_t)R(400000)); break;
        case 12: session_set_start_marker(s, ch, sc, (size_t)R(600000)); break;
        case 13: session_set_end_marker(s, ch, sc, (size_t)R(600000)); break;
        case 14: session_seek_clip(s, ch, sc, R(10) == 0 ? (size_t)-1 : (size_t)R(600000)); break;
        case 15: session_set_clip_warp(s, ch, sc, R(2)); break;
        case 16: { float v[] = { 0, -5, 1, 30, 60, 128, 200, 300, 999, NAN, INFINITY };
                   session_set_clip_bpm_value(s, ch, sc, v[R(11)]); break; }
        case 17: session_set_clip_bpm_fields(s, ch, sc, (float)R(300), (float)R(300), (float)R(300)); break;
        case 18: case 19: session_queue_add(s, ch, sc); break;
        case 20: session_queue_move(s, R(10) - 1, R(10) - 1); break;
        case 21: session_queue_remove(s, R(10) - 1); break;
        case 22: { float v[] = { 0, 0.05f, 0.5f, 1, 3, 10, 30, 100, -1, NAN }; session_set_crossfade(s, v[R(10)]); break; }
        case 23: if (R(4) == 0) session_clear_clip(s, ch, sc); break;
        case 24: if (R(3) == 0) session_move_clip(s, ch, sc, R(nc + 1), R(ns + 1)); break;
        case 25: if (R(8) == 0 && nc > 1) session_delete_channel(s, ch); else if (R(2) == 0) session_move_channel(s, ch, R(nc + 1) - (R(20) == 0)); break;   /* step 36 */
        case 26: if (R(8) == 0 && ns > 1) session_delete_scene(s, sc); break;
        case 27: if (R(4) == 0) session_insert_channel(s, R(nc + 1)); break;
        case 28: if (R(4) == 0) session_insert_scene(s, R(ns + 1)); break;
        case 29: session_set_channel_gain(s, ch, (float)R(120) / 100.0f); break;
        case 30: session_set_channel_pan(s, ch, (float)R(300) / 100.0f - 1.5f); break;
        case 31: session_toggle_mute(s, ch); session_toggle_solo(s, R(nc)); break;
        case 32: session_set_note(s, ch, sc, "note"); break;
        case 33: if (R(30) == 0) { char *snap = project_snapshot(s, 1); free(snap); }
                 else streams_trim_idle(R(2) ? 0.0 : 0.02);   /* free the buffers of unused clips */
                 break;
        case 34: undo_commit_now(s); break;
        case 35: undo_undo(s); break;
        case 36: undo_redo(s); break;
        /* step 27: cutting - while playing, with undo, with moving clips */
        case 37: case 38: {
            const EditList *e = session_clip_edits(s, ch, sc);
            if (!e) break;
            EditList *nl = NULL;
            int64_t t = e->len > 0 ? (int64_t)R((int)(e->len > 2000000000 ? 2000000000 : e->len)) : 0;
            switch (R(14)) {
            case 6: if (e->n) nl = edit_trim(e, R(e->n), R(2), t); break;
            case 7: if (e->n) nl = edit_copy(e, R(e->n), t - 50000); break;
            case 8: if (e->n) nl = edit_set_fades(e, R(e->n), R(30000), R(30000)); break;
            case 0: case 1: nl = edit_split(e, t); break;
            case 2: if (e->n > 1) nl = edit_delete(e, R(e->n)); break;
            case 3: if (e->n) nl = edit_move(e, R(e->n), t + R(200000) - 100000); break;
            case 4: nl = edit_fade(e, t, t + R(100000), R(2) ? 1 : -1); break;
            case 5: if (R(5) == 0) nl = edit_crop(e, t, t + 1000 + R(300000)); break;
            /* step 36j/k: outside the clip, pasting, extending */
            case 9: if (e->n) nl = edit_move(e, R(e->n), (R(2) ? -1 : 1) * (int64_t)R((int)(e->len > 0 ? (e->len > 1000000000 ? 1000000000 : e->len) : 1)) * 2); break;
            case 10: if (e->n) { EditList *cp = edit_extract(e, R(e->n)); nl = cp ? edit_paste(e, cp, t - e->len / 2 + R(1000)) : NULL; edit_free(cp); } break;
            case 11: nl = edit_set_len(e, R(3) == 0 ? 1 + R(1000) : e->len + R(400000) - 100000); break;
            case 12: if (e->n) nl = edit_trim(e, R(e->n), R(2), t * 2 - e->len); break;
            case 13: if (e->n) { EditList *cp = edit_extract(e, R(e->n)); nl = cp ? edit_paste(e, cp, e->len + R(1000)) : NULL; edit_free(cp); } break;
            }
            if (nl && !edit_valid(nl)) { printf("FAIL: invalid edit list from op (iter %d)\n", it); return 1; }
            if (nl) session_clip_apply_edits(s, ch, sc, nl, -1);
            break;
        }
        case 39: if (R(6) == 0) session_clip_set_edits_text(s, ch, sc, NULL); break;
        case 41: { int64_t sc0 = (int64_t)R(2000000) - 1000000;            /* step 36j: the view also outside the clip */
                   session_set_wave_scroll(s, ch, sc, (size_t)sc0);
                   session_set_wave_zoom(s, ch, sc, (float)(R(1000)) / 100.0f); break; }
        case 42: if (nc > 1) session_move_channel(s, R(nc), R(nc)); break;
        case 40: { char *t = session_clip_edits_text(s, ch, sc);
                   if (t && R(2)) session_clip_set_edits_text(s, R(nc + 1), R(ns + 1), t);   /* "copy" into another cell */
                   free(t); break; }
        }
        if (getenv("CHECK_INV")) {        /* an empty cell must not hold resources (otherwise they leak) */
            for (int c = 0; c < MAX_CHANNELS; ++c) for (int k = 0; k < MAX_SCENES; ++k) {
                Clip *q = &s->channels[c].clips[k];
                bool dead = c >= s->channel_count || !atomic_load(&q->has_clip) || atomic_load(&q->is_note);
                if (dead && (q->peaks || q->stream || q->bpm_job || q->stretch)) {
                    printf("LEAK-INV: op %d left resources in [%d][%d] (count %d/%d)\n", lastop, c, k, s->channel_count, s->scene_count);
                    return 3;
                }
            }
        }
        session_poll_bpm(s);
        int blocks = conc ? 0 : 1 + R(6);
        if (conc) { nap(1000 + R(4000)); if (atomic_load(&g_bad)) { printf("FAIL: bad sample (audio thread) iter %d\n", it); return 1; } }
        unsigned n = (unsigned)(64 << R(5));                 /* 64..1024 */
        for (int b = 0; b < blocks; ++b) {
            mixer_render(s, buf, n, 2);
            for (unsigned i = 0; i < n * 2; ++i) {
                float v = buf[i];
                if (!isfinite(v) || v > 1.0001f || v < -1.0001f) {
                    printf("FAIL: bad sample %g at iter %d\n", v, it); return 1;
                }
            }
            checks += n;
        }
        session_queue_tick(s);
        if (R(50) == 0) nap(20000);                          /* gives time to the disk/background threads */
        if (it % 2000 == 0) printf("iter %d (ch=%d sc=%d queue=%d)\n", it, s->channel_count, s->scene_count, session_queue_len(s));
    }
    if (conc) { atomic_store(&g_run_audio, false); pthread_join(at, NULL); atomic_store(&s->audio_running, false); }
    undo_shutdown();
    session_free(s);
    printf("ENGINE FUZZ: all ok (%ld frames checked)\n", checks);
    return 0;
}
