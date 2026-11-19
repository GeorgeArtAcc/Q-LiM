/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* "Monkey" for the audio engine: random actions (play, stop, scenes, loop, markers, seeking,
 * warp tick/BPM, queue, crossfade, delete/move/insert, loading odd files) + audio between them.
 * 2.1: + surround (circle / room, paths, the Speaker Map masks), surround files (stereo / multichannel / path),
 * MIDI clips, transitions, pitch / gain / pan of clips, tempo reset, 2..16 outputs.
 * Checks: no crash (ASan/UBSan), no NaN/inf, output within [-1, 1]. */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/project.h"
#include "../src/stream.h"
#include "../src/undo.h"
#include "../src/surround.h"
#include <sndfile.h>
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

/* 2.1: a 6-channel file for the surround-file actions (2 s, a different tone in every channel) */
static const char *mc_file(void)
{
    static const char *p = "/tmp/qlim_fuzz_mc6.wav";
    SF_INFO in = { .samplerate = 48000, .channels = 6, .format = SF_FORMAT_WAV | SF_FORMAT_FLOAT };
    SNDFILE *f = sf_open(p, SFM_WRITE, &in);
    if (!f) return NULL;
    static float fr[6];
    for (int i = 0; i < 96000; ++i) {
        for (int k = 0; k < 6; ++k) fr[k] = 0.2f * sinf((float)i * (0.01f + 0.005f * (float)k));
        sf_writef_float(f, fr, 1);
    }
    sf_close(f);
    return p;
}

static void rand_setup(SurSetup *su, int nout)
{
    memset(su, 0, sizeof *su);
    if (R(3) == 0) sur_room_layout(su, R(SUR_R_N), R(nout));
    else sur_layout(su, R(SUR_L_N), R(nout));
    if (R(3) == 0) { su->mode = SUR_MODE_ROOM; if (su->room_w <= 0) { su->room_w = 16; su->room_d = 26; } }
    su->width = (float)R(4) * 40.0f;
    su->az0 = (float)R(720) - 360.0f; su->spread0 = (float)R(130) / 100.0f - 0.1f;   /* also a bit outside */
    for (int k = 0; k < su->n; ++k) {
        if (R(4) == 0) su->out[k] = R(70) - 2;                        /* also outside the card */
        int m = R(6);
        su->omask[k] = m == 0 ? SUR_MUTED : m == 1 ? (((uint64_t)rnd() << 20) | rnd()) & ~SUR_MUTED : m == 2 ? 1ull << R(62) : 0;
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "fuzz_engine iterations seed file...\n"); return 2; }
    int iters = atoi(argv[1]);
    rs = (unsigned)atoi(argv[2]) * 2654435761u + 1;
    char **files = argv + 3; int nf = argc - 3;
    const char *mcf = mc_file();
    int nout = 2;                                                    /* the outputs of the "card" (2..16) */
    static Session S; Session *s = &S;
    session_init(s, 48000);
    streams_init();
    atomic_store(&s->audio_running, false);
    for (int i = 0; i < 4; ++i) session_add_channel(s, NULL);
    for (int i = 0; i < 8; ++i) session_add_scene(s);
    static float buf[1024 * 16];
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
        int lastop = R(56);
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
        /* 2.1: the newer parts */
        case 43: { SurSetup su;
                   if (R(4) == 0) session_set_surround(s, ch, NULL);
                   else { rand_setup(&su, nout); session_set_surround(s, ch, &su); }
                   break; }
        case 44: { SurPt p[SUR_MAX_PTS]; int n = R(5) == 0 ? 0 : 1 + R(12);
                   for (int i = 0; i < n; ++i) { p[i].t = (float)R(4000) / 100.0f - 1.0f; p[i].az = (float)R(800) - 400.0f;
                       p[i].spread = (float)R(120) / 100.0f; p[i].x = (float)R(40) - 20; p[i].y = (float)R(40) - 5; p[i].w = (float)R(30); }
                   if (R(10) == 0 && n) p[R(n)].az = NAN;
                   session_clip_set_sur_points(s, ch, sc, p, n); break; }
        case 45: if (mcf) { session_load_mc_next(R(3), R(nout + 4), R(2) ? mcf : NULL); session_set_clip(s, ch, sc, mcf); } break;
        case 46: if (R(2)) session_clip_set_mc_out(s, ch, sc, R(20) - 2);
                 else session_clip_set_mc_map(s, ch, sc, R(18) - 1, R(3) == 0 ? SUR_MUTED : R(2) ? 0 : 1ull << R(62));
                 break;
        case 47: if (mcf && R(3) == 0) session_clip_set_mc(s, ch, sc, R(4) - 1, R(2) ? mcf : files[R(nf)], R(2) ? mcf : NULL); break;
        case 48: switch (R(4)) { case 0: session_set_clip_pitch(s, ch, sc, (float)R(50) - 25.0f); break;
                                 case 1: session_set_clip_formants(s, ch, sc, R(2)); break;
                                 case 2: session_set_clip_gain(s, ch, sc, (float)R(100) - 70.0f); break;
                                 default: session_set_clip_pan(s, ch, sc, (float)R(300) / 100.0f - 1.5f); } break;
        case 49: switch (R(3)) { case 0: session_transition_to(s, sc); break; case 1: session_transition_next(s); break; default: session_transition_back(s); } break;
        case 50: if (R(2)) session_new_midi_clip(s, ch, sc, (float)(1 + R(32)), "m");
                 else session_midi_set_text(s, ch, sc, R(2) ? "midi_len=4\nmn=0 0.5 60 100\nmn=1 0.25 64 90\nmn=2.5 1 67 80\n" : "x y z");
                 break;
        case 51: session_midi_play_from(s, ch, sc, (double)R(64) / 4.0 - 1.0); break;
        case 52: session_clip_bpm_reset(s, ch, sc); break;
        case 53: if (R(3) == 0) session_play_scene_now(s, sc);
                 else if (R(2)) session_stop_clip(s, ch, sc);
                 else session_play_clip_from(s, ch, sc, (size_t)R(600000));
                 break;
        case 54: if (!conc) { static const int NO[] = { 2, 2, 4, 6, 8, 10, 16 }; nout = NO[R(7)]; } break;
        case 55: session_midi_tick(s); session_midi_reap(s); if (R(40) == 0) session_midi_panic(s); break;
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
            mixer_render(s, buf, n, nout);
            for (unsigned i = 0; i < n * (unsigned)nout; ++i) {
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
