/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Test of the direct PipeWire output (audio_pw.c) without the mixer.
 * ./test_pw list
 * ./test_pw check [target|-] [rate] [quantum]   -> SILENT test (plays silence for 4 s) - safe for headphones/speakers
 * ./test_pw play <target|-> <rate> <quantum> <seconds> [channels]
 *   plays a "sawtooth" (ramp) on every channel: channel c, frame k -> ((k + c*977) % 4800) / 4800 * 0.5
 *   so a recording of the output's monitor can be checked frame by frame (pw_capture + check). */
#define _POSIX_C_SOURCE 200809L
#include "../src/audio_pw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include <stdbool.h>

static uint64_t g_k;
static int g_silent;
static atomic_uint g_maxn, g_minn = 1u << 30;

static void render(float *out, uint32_t n, int ch, void *u)
{
    (void)u;
    if (g_silent) { memset(out, 0, sizeof(float) * n * (uint32_t)ch); g_k += n; }
    else
    for (uint32_t i = 0; i < n; ++i, ++g_k)
        for (int c = 0; c < ch; ++c)
            out[i * (uint32_t)ch + (uint32_t)c] = (float)((g_k + (uint64_t)c * 977u) % 4800u) / 4800.0f * 0.5f;
    if (n > atomic_load(&g_maxn)) atomic_store(&g_maxn, n);
    if (n < atomic_load(&g_minn)) atomic_store(&g_minn, n);
}

static void nap(double s) { struct timespec t = { (time_t)s, (long)((s - (time_t)s) * 1e9) }; nanosleep(&t, NULL); }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage\n"); return 2; }
    bool av = pwb_available();
    printf("PipeWire available: %s (libpipewire %s)\n", av ? "yes" : "no", pwb_library_version());
    if (strcmp(argv[1], "list") == 0) {
        PwbSink s[PWB_MAX_SINKS];
        int n = pwb_list_sinks(s, PWB_MAX_SINKS);
        for (int i = 0; i < n; ++i)
            printf("sink %u '%s' (%s) ch=%d rate=%d%s pos0=%u\n", s[i].id, s[i].name, s[i].desc, s[i].channels, s[i].rate,
                   s[i].is_default ? " DEFAULT" : "", s[i].pos[0]);
        return n > 0 ? 0 : 1;
    }
    if (strcmp(argv[1], "check") == 0) {
        PwbSink s[PWB_MAX_SINKS];
        int n = pwb_list_sinks(s, PWB_MAX_SINKS);
        printf("PipeWire outputs: %d\n", n);
        for (int i = 0; i < n; ++i)
            printf("   %-40s %-50s %d ch%s\n", s[i].desc, s[i].name, s[i].channels, s[i].is_default ? "  (default)" : "");
        const char *tg = (argc > 2 && strcmp(argv[2], "-")) ? argv[2] : "";
        int rate = argc > 3 ? atoi(argv[3]) : 48000, q = argc > 4 ? atoi(argv[4]) : 256;
        g_silent = 1;
        char err[160]; int ch = 0;
        if (pwb_open(tg, rate, q, 0, render, NULL, &ch, err, sizeof err) != 0) { printf("OPEN FAILED: %s\n", err); return 1; }
        PwbStatus st;
        for (int i = 0; i < 8; ++i) { nap(0.5); pwb_get_status(&st); if (st.lost) break; }
        printf("output: %s, %d ch | graph %u Hz, buffer %u (%.1f ms) | callbacks %llu | xruns %llu%s%s\n",
               st.node_desc, ch, st.graph_rate, st.quantum, st.graph_rate ? st.quantum * 1000.0 / st.graph_rate : 0.0,
               (unsigned long long)st.callbacks, (unsigned long long)st.xruns, st.lost ? " | LOST: " : "", st.lost ? st.why : "");
        bool ok = st.callbacks > 0 && !st.lost;
        if (st.state == 2) printf("stream is not connected to any output (is wireplumber running?)\n");
        pwb_close();
        printf("PWCHECK: %s\n", ok ? "ok" : "FAIL");
        return ok ? 0 : 1;
    }
    if (strcmp(argv[1], "play") == 0 && argc >= 6) {
        const char *tg = strcmp(argv[2], "-") ? argv[2] : "";
        int rate = atoi(argv[3]), q = atoi(argv[4]);
        double secs = atof(argv[5]);
        int want = argc > 6 ? atoi(argv[6]) : 0;
        char err[160]; int ch = 0;
        if (pwb_open(tg, rate, q, want, render, NULL, &ch, err, sizeof err) != 0) { printf("OPEN FAILED: %s\n", err); return 1; }
        printf("opened, channels=%d\n", ch); fflush(stdout);
        double t = 0; PwbStatus st;
        while (t < secs) {
            nap(0.5); t += 0.5;
            pwb_get_status(&st);
            printf("t=%.1f state=%d graph=%u quantum=%u frames=%u cb=%llu xruns=%llu lost=%d %s\n", t, st.state, st.graph_rate,
                   st.quantum, st.last_frames, (unsigned long long)st.callbacks, (unsigned long long)st.xruns, st.lost, st.why);
            fflush(stdout);
            if (st.lost) break;
        }
        printf("frames per callback: min %u max %u, total %llu\n", atomic_load(&g_minn), atomic_load(&g_maxn), (unsigned long long)g_k);
        pwb_close();
        printf("closed\n");
        return st.lost ? 3 : 0;
    }
    return 2;
}
