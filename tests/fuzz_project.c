/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Stress: loading damaged/odd projects + playing, saving and reloading (for ASan/UBSan).
 * 2.1: all the newer keys too (surround, paths, surround files, Speaker Map masks, MIDI notes, cuts, stems, fx ...). */
#define _POSIX_C_SOURCE 200809L
#include "../src/session.h"
#include "../src/mixer.h"
#include "../src/project.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned rs;
static unsigned rnd(void) { rs = rs * 1103515245u + 12345u; return (rs >> 8) & 0xffffff; }
static int R(int n) { return n > 0 ? (int)(rnd() % (unsigned)n) : 0; }

static const char *weird_num(char *b)
{
    static const char *w[] = { "0", "-1", "1", "nan", "inf", "-inf", "1e30", "-1e30", "999999999999999999999",
                                "18446744073709551615", "-9223372036854775808", "2147483648", "abc", "", "0.5",
                                "3.4e38", "1e-40", " 12", "7x" };
    if (R(3) == 0) return w[R((int)(sizeof w / sizeof *w))];
    sprintf(b, "%d", R(3) ? R(400000) : -R(1000));
    return b;
}

/* a random value made of the pieces the newer keys use: numbers, '|' ',' ':' ' ' separated lists, hex */
static const char *weird_list(char *b, size_t n)
{
    static const char SEP[] = "|,: ;x=";
    size_t k = 0;
    int parts = R(4) == 0 ? 200 + R(400) : R(24);
    char nb[64];
    for (int i = 0; i < parts && k + 40 < n; ++i) {
        const char *v = R(5) == 0 ? (R(2) ? "ffffffffffffffff" : "8000000000000000") : weird_num(nb);
        k += (size_t)snprintf(b + k, n - k, "%s%c", v, SEP[R((int)sizeof SEP - 1)]);
    }
    b[k] = 0;
    return b;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "fuzz_project A.wav B.wav iterations [seed]\n"); return 2; }
    const char *files[] = { argv[1], argv[2], "/nonexistent/x.wav", "relative.wav", "samples/missing.wav" };
    int iters = atoi(argv[3]);
    rs = argc > 4 ? (unsigned)atoi(argv[4]) : 1u;
    static Session S; Session *s = &S;
    session_init(s, 48000);
    streams_init();
    atomic_store(&s->audio_running, false);
    const char *p = "/tmp/qlim_fuzz.limproject", *p2 = "/tmp/qlim_fuzz2.limproject";
    static float buf[512 * 16];
    for (int it = 0; it < iters; ++it) {
        FILE *f = fopen(p, "w");
        char b[64];
        fprintf(f, "# Q-LiM project\n[meta]\nversion=3\n");
        if (R(2)) fprintf(f, "sample_rate=%s\n", weird_num(b));
        fprintf(f, "channel_count=%s\nscene_count=%s\ntempo=%s\nmaster_gain=%s\n",
                weird_num(b), R(2) ? weird_num(b) : "8", weird_num(b), weird_num(b));
        int nch = R(6) + (R(10) == 0 ? 60 : 0);
        for (int c = 0; c < nch; ++c) {
            int ci = R(8) == 0 ? R(80) - 10 : c;
            fprintf(f, "[channel %d]\n", ci);
            if (R(2)) { fprintf(f, "name="); int L = R(5) == 0 ? 3000 : R(20); for (int k = 0; k < L; ++k) fputc('a' + R(26), f); fputc('\n', f); }
            const char *keys[] = { "gain", "pan", "muted", "soloed", "color_id", "color", "width", "midi_cc", "pan_midi_cc",
                                   "route_l", "route_r", "mute_bind", "solo_bind" };
            for (int k = 0; k < 6; ++k) fprintf(f, "%s=%s\n", keys[R(13)], weird_num(b));
            static char lb[8192];
            const char *lkeys[] = { "surround", "rec_in", "monitor", "fx", "fxmidi", "fxstate", "fxref" };   /* 2.1 */
            for (int k = 0, nk = R(4); k < nk; ++k) fprintf(f, "%s=%s\n", lkeys[R(7)], weird_list(lb, sizeof lb));
            if (R(3) == 0) fprintf(f, "surround=%d|0,1,2,3,4,5|-30,30,0,-110,110,180|60|%s|0.2|%d|%d|16|26|4|0,1,2|3,4,5|0|5|2|om:%s\n",
                                   R(20) - 2, weird_num(b), R(12) - 2, R(3) - 1, R(2) ? "1,2,8000000000000000,0" : weird_list(lb, 200));
        }
        int ncl = R(12);
        for (int k = 0; k < ncl; ++k) {
            int c = R(8) == 0 ? R(70) - 5 : R(nch > 0 ? nch : 1), sc = R(8) == 0 ? R(300) - 10 : R(20);
            fprintf(f, "[clip %d %d]\n", c, sc);
            fprintf(f, "type=%s\n", R(5) == 0 ? "note" : "clip");
            fprintf(f, "path=%s\n", files[R(5)]);
            if (R(2)) fprintf(f, "name=%s\n", R(3) ? "x" : "");
            const char *keys[] = { "position", "loop", "loop_start", "loop_end", "start_marker", "end_marker", "wave_zoom",
                                   "color", "bind", "wave_scroll", "bpm_detected", "bpm_fix", "bpm_play", "warp" };
            for (int j = 0; j < 8; ++j) fprintf(f, "%s=%s\n", keys[R(14)], weird_num(b));
            static char cb[8192];
            const char *ckeys[] = { "clip_gain", "clip_pan", "clip_pitch", "clip_formants", "sur", "mc", "mc_map", "mc_orig",
                                    "stem", "stems", "stem_level", "stem_mute", "edit_len", "edit_seg", "midi_len", "mn" };   /* 2.1 */
            for (int j = 0, nk = R(8); j < nk; ++j) fprintf(f, "%s=%s\n", ckeys[R(16)], R(2) ? weird_num(b) : weird_list(cb, sizeof cb));
            if (R(4) == 0) {
                fprintf(f, "type=midi\nmidi_len=%s\n", weird_num(b));
                for (int j = 0, nn = R(40); j < nn; ++j) fprintf(f, "mn=%s %s %d %d\n", weird_num(b), weird_num(cb), R(200) - 30, R(300) - 50);
            }
            if (R(4) == 0) fprintf(f, "sur=%s\n", weird_list(cb, sizeof cb));
            if (R(6) == 0) fprintf(f, "mc=%d,%d\nmc_orig=%s\nmc_map=%s\n", R(5) - 1, R(80) - 5, R(2) ? "/tmp/qlim_fuzz_mc6.wav" : "/nope.wav", weird_list(cb, 400));
        }
        if (R(4) == 0) { for (int k = 0; k < 200; ++k) fputc(1 + R(255), f); }   /* garbage */
        if (R(4) == 0) fprintf(f, "[clip");                                      /* unfinished line */
        fclose(f);

        project_load(s, p);
        /* play whatever there is: all clips, a little audio, scenes, queue */
        for (int c = 0; c < s->channel_count; ++c)
            for (int sc = 0; sc < s->scene_count && sc < 20; ++sc)
                if (R(3) == 0) session_toggle_clip(s, c, sc);
        if (R(2)) session_set_crossfade(s, (float)R(5));
        for (int k = 0; k < 40; ++k) {
            int no = 2 + 2 * R(5);                                     /* 2.1: 2..10 outputs (surround) */
            mixer_render(s, buf, 512, no);
            for (int i = 0; i < 512 * no; ++i) if (!(buf[i] == buf[i])) { printf("FAIL: NaN in output (iter %d)\n", it); return 1; }
            if (k % 8 == 0) session_queue_tick(s);
        }
        project_save(s, p2);
        project_load(s, p2);
        for (int k = 0; k < 5; ++k) mixer_render(s, buf, 512, 2);
        if (it % 50 == 0) printf("iter %d ok (ch=%d sc=%d)\n", it, s->channel_count, s->scene_count);
    }
    session_free(s);
    printf("FUZZ: all ok\n");
    return 0;
}
