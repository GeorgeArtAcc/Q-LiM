/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 2.0.0: plugins in a separate process. With the test plugin QTestBridge (tests/bridgeplug, built by the Makefile):
 *   - the sound through the separate process is EXACTLY the same as inside
 *   - parameters and state go there and back
 *   - the plugin crashes -> silence, Q-LiM goes on, the plugin is started again with its settings
 *   - the plugin freezes in its sound -> silence (the audio thread waits at most ~2 blocks), then a restart
 *   - the choice and the list (plugins-separate.txt) */
#define _POSIX_C_SOURCE 200809L
#include "../src/plughost.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }
static void nap(long ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
#define BLK 256

static char g_last_msg[600];

/* "--crash-inside <plugin> <list>": the plugin runs INSIDE and crashes - the crash handler must put it on the list */
#ifdef _WIN32
#include <windows.h>
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    char name[MAX_PATH * 3] = "";
    wchar_t w[MAX_PATH];
    HMODULE mod = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)ep->ExceptionRecord->ExceptionAddress, &mod) && GetModuleFileNameW(mod, w, MAX_PATH))
        WideCharToMultiByte(CP_UTF8, 0, w, -1, name, sizeof name, NULL, NULL);
    qplug_crash_note_module(name);
    TerminateProcess(GetCurrentProcess(), 9);
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
#include <signal.h>
static void crash_sig(int sig) { (void)sig; qplug_crash_note(); _exit(9); }
#endif
static int crash_inside(const char *plugin, const char *list)
{
    char id[1200], err[300];
    snprintf(id, sizeof id, "%s|qlim.test.bridge", plugin);
    qplug_separate_init(list);
    qplug_set_separate_mode(QSEP_AUTO);
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(crash_filter);
#else
    signal(SIGSEGV, crash_sig);
#endif
    QPlug *p = qplug_load("CLAP", id, 48000, 1024, err, sizeof err);
    if (!p || qplug_is_separate(p)) return 3;
    float l[256] = { 0.875f }, r[256] = { 0 };
    qplug_process(p, l, r, 256);                       /* crashes */
    return 4;
}
static void hook(const char *m) { snprintf(g_last_msg, sizeof g_last_msg, "%s", m); }

static void fill(float *l, float *r, int blk)
{
    for (int i = 0; i < BLK; ++i) { l[i] = 0.3f * sinf((float)(blk * BLK + i) * 0.031f); r[i] = 0.2f * sinf((float)(blk * BLK + i) * 0.047f); }
}

static float run(QPlug *p, int blocks, float *keep_l, float *keep_r)   /* max |out| */
{
    float mx = 0;
    for (int b = 0; b < blocks; ++b) {
        float l[BLK], r[BLK];
        fill(l, r, b);
        qplug_process(p, l, r, BLK);
        for (int i = 0; i < BLK; ++i) { if (fabsf(l[i]) > mx) mx = fabsf(l[i]); if (fabsf(r[i]) > mx) mx = fabsf(r[i]); }
        if (keep_l) { memcpy(keep_l + b * BLK, l, sizeof l); memcpy(keep_r + b * BLK, r, sizeof r); }
    }
    return mx;
}

static void settle(QPlug *p)                       /* like a playing sound card: blocks keep coming for a moment */
{
    for (int k = 0; k < 40; ++k) { run(p, 2, NULL, NULL); nap(25); qplug_idle_all(); }
}

static bool wait_alive(QPlug *p, double secs)   /* idle until the plugin makes sound again */
{
    double t0 = now();
    while (now() - t0 < secs) {
        qplug_idle_all();
        if (run(p, 2, NULL, NULL) > 0.01f) return true;
        nap(20);
    }
    return false;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "--plugin-host")) return qplug_host_child_main(argc, argv);
    if (argc >= 4 && !strcmp(argv[1], "--crash-inside")) return crash_inside(argv[2], argv[3]);
    if (argc < 2) { printf("usage: test_bridge <qtest_bridge.clap>\n"); return 2; }
    char id[1200];
    snprintf(id, sizeof id, "%s|qlim.test.bridge", argv[1]);
#ifdef _WIN32
    const char *list = "qlim_test_plugins-separate.txt";
#else
    const char *list = "/tmp/qlim_test_audio/plugins-separate.txt";
    if (system("mkdir -p /tmp/qlim_test_audio") != 0) return 2;
#endif
    remove(list);
    qplug_separate_init(list);
    qplug_set_report_hook(hook);
    char err[300] = "";

    /* 1) inside vs separate: the same sound */
    qplug_set_separate_mode(QSEP_NEVER);
    QPlug *in = qplug_load("CLAP", id, 48000, 1024, err, sizeof err);
    CHECK(in && !qplug_is_separate(in), "BRIDGE loads inside Q-LiM (%s)", err);
    qplug_set_separate_mode(QSEP_ALL);
    QPlug *sp = qplug_load("CLAP", id, 48000, 1024, err, sizeof err);
    CHECK(sp && qplug_is_separate(sp), "BRIDGE loads in a separate process (%s)", err);
    if (!in || !sp) { printf("BRIDGE: %d failed\n", fails); return 1; }
    static float al[40 * BLK], ar[40 * BLK], bl[40 * BLK], br[40 * BLK];
    run(in, 40, al, ar);
    run(sp, 40, bl, br);
    float d = 0;
    for (int i = 0; i < 40 * BLK; ++i) { d = fmaxf(d, fabsf(al[i] - bl[i])); d = fmaxf(d, fabsf(ar[i] - br[i])); }
    CHECK(d == 0.0f && fabsf(bl[1000] - 0.5f * ar[1000] / 0.5f * 0.5f) < 1.0f, "BRIDGE the sound is exactly the same (max diff %g)", d);
    CHECK(fabsf(bl[777] - 0.5f * (0.2f * sinf(777 * 0.047f))) < 1e-6f, "BRIDGE the plugin really works there (L = 0.5 * R)");

    /* 1b) MIDI goes there, at the right frame */
    {
        QMidiEv ev = { 100, { 0x90, 60, 100 } };
        float l1[BLK], r1[BLK], l2[BLK], r2[BLK];
        memset(l1, 0, sizeof l1); memset(r1, 0, sizeof r1); memset(l2, 0, sizeof l2); memset(r2, 0, sizeof r2);
        CHECK(qplug_wants_midi(sp), "BRIDGE MIDI: the plugin takes MIDI there too");
        qplug_midi(in, &ev, 1); qplug_process(in, l1, r1, BLK);
        qplug_midi(sp, &ev, 1); qplug_process(sp, l2, r2, BLK);
        CHECK(l2[99] == 0.0f && fabsf(l2[100] - 0.25f) < 1e-6f && !memcmp(l1, l2, sizeof l1) && !memcmp(r1, r2, sizeof r1),
              "BRIDGE MIDI: the note is heard from frame 100, the same as inside (%g %g)", l2[99], l2[100]);
    }

#ifdef _WIN32
    /* 1c) Windows: the plugin's window opens in the other process (it is that process's own window) and closes */
    {
        int rc = qplug_editor_open(sp, "QTestBridge test window", err, sizeof err);
        HWND w = NULL;
        for (int k = 0; k < 150 && !w; ++k) { qplug_idle_all(); nap(20); w = FindWindowW(NULL, L"QTestBridge test window"); }
        DWORD wpid = 0;
        if (w) GetWindowThreadProcessId(w, &wpid);
        CHECK(rc == 0 && w && wpid != GetCurrentProcessId() && IsWindowVisible(w), "BRIDGE window: opened in the separate process (%s)", err);
        HWND child = w ? FindWindowExW(w, NULL, L"STATIC", NULL) : NULL;
        CHECK(child != NULL, "BRIDGE window: the plugin's own content is inside it");
        PostMessageW(w, WM_CLOSE, 0, 0);                 /* the user presses X */
        for (int k = 0; k < 100 && qplug_editor_is_open(sp); ++k) { qplug_idle_all(); nap(20); }
        CHECK(!qplug_editor_is_open(sp) && !IsWindow(w), "BRIDGE window: X closes it, Q-LiM knows");
    }
#endif

    /* 2) parameters and state */
    char nm[64] = "";
    qplug_param_name(sp, 0, nm, sizeof nm);
    CHECK(qplug_param_count(sp) == 1 && !strcmp(nm, "Gain"), "BRIDGE parameter name over there: '%s'", nm);
    qplug_param_set(sp, 0, 0.25f);
    settle(sp);
    CHECK(fabsf(qplug_param_get(sp, 0) - 0.25f) < 1e-6f, "BRIDGE parameter set and read back (%g)", qplug_param_get(sp, 0));
    char tx[64] = "";
    qplug_param_text(sp, 0, tx, sizeof tx);
    CHECK(!strcmp(tx, "gain 0.25"), "BRIDGE the value as the plugin shows it: '%s'", tx);
    run(sp, 2, bl, br);
    fill(al, ar, 1);
    CHECK(fabsf(bl[BLK + 10] - 0.25f * ar[10]) < 1e-6f, "BRIDGE the new gain is heard");
    void *st = NULL; size_t sl = 0;
    CHECK(qplug_get_state(sp, &st, &sl) == 0 && sl > 0 && atof((char *)st) == 0.25, "BRIDGE state out (%zu bytes)", sl);
    qplug_param_set(sp, 0, 0.9f);
    CHECK(qplug_set_state(sp, "0.4", 3) == 0, "BRIDGE state in");
    for (int w_ = 0; w_ < 10; ++w_) { nap(100); qplug_idle_all(); }
    CHECK(fabsf(qplug_param_get(sp, 0) - 0.4f) < 1e-5f, "BRIDGE state took effect (%g)", qplug_param_get(sp, 0));
    free(st);

    /* 3) a crash: silence, Q-LiM goes on, the plugin comes back with the gain it had */
    settle(sp);
    float before = qplug_param_get(sp, 0);
    float l[BLK], r[BLK];
    fill(l, r, 0); l[0] = 0.875f;
    qplug_process(sp, l, r, BLK);
    float mx = 0;
    for (int i = 0; i < BLK; ++i) mx = fmaxf(mx, fmaxf(fabsf(l[i]), fabsf(r[i])));
    CHECK(mx == 0.0f, "BRIDGE crash: this block is silent");
    g_last_msg[0] = 0;
    CHECK(wait_alive(sp, 5.0), "BRIDGE crash: the plugin was started again by itself");
    CHECK(strstr(g_last_msg, "crashed") && strstr(g_last_msg, "started again"), "BRIDGE crash: the message says so ('%s')", g_last_msg);
    settle(sp);
    CHECK(fabsf(qplug_param_get(sp, 0) - before) < 1e-5f, "BRIDGE crash: its settings are back (gain %g, was %g)", qplug_param_get(sp, 0), before);

    /* 4) a freeze in the sound: the audio thread does not wait for it */
    fill(l, r, 0); l[0] = 0.625f;
    double t0 = now();
    qplug_process(sp, l, r, BLK);
    double waited = now() - t0;
    t0 = now();
    for (int k = 0; k < 20; ++k) { fill(l, r, k); qplug_process(sp, l, r, BLK); }
    double later = now() - t0;
    CHECK(waited < 0.05 && later < 0.01, "BRIDGE freeze: the sound waits %.1f ms, then not at all (%.2f ms for 20 blocks)", waited * 1000, later * 1000);
    g_last_msg[0] = 0;
    CHECK(wait_alive(sp, 15.0), "BRIDGE freeze: found and started again");
    CHECK(strstr(g_last_msg, "froze") != NULL, "BRIDGE freeze: the message says so ('%s')", g_last_msg);

    /* 5) the list and the choice */
    qplug_set_separate_mode(QSEP_AUTO);
    CHECK(!qplug_wants_separate("CLAP", id), "BRIDGE automatic: not on the list -> inside");
    qplug_separate_set("CLAP", id, true, "chosen by you");
    CHECK(qplug_wants_separate("CLAP", id), "BRIDGE automatic: on the list -> separate");
    qplug_separate_init(list);                          /* read back from the file */
    char f[8], pid[1300], why[80];
    CHECK(qplug_separate_count() == 1 && qplug_separate_get(0, f, sizeof f, pid, sizeof pid, why, sizeof why) &&
          !strcmp(pid, id) && !strcmp(why, "chosen by you") && qplug_separate_mode() == QSEP_AUTO, "BRIDGE the list is saved and read back");
    qplug_separate_set("CLAP", id, false, NULL);
    CHECK(qplug_separate_count() == 0 && !qplug_wants_separate("CLAP", id), "BRIDGE removed from the list");
    qplug_set_separate_mode(QSEP_NEVER);
    CHECK(!qplug_wants_separate("CLAP", id), "BRIDGE never: inside");

    /* 5b) a plugin crashes Q-LiM (inside): it goes on the list by itself -> next time separate */
    {
        qplug_set_separate_mode(QSEP_AUTO);              /* the file: mode=1, empty list */
        char cmd[3000];
#ifdef _WIN32
        snprintf(cmd, sizeof cmd, "\"\"%s\" --crash-inside \"%s\" \"%s\"\"", argv[0], argv[1], list);
#else
        snprintf(cmd, sizeof cmd, "'%s' --crash-inside '%s' '%s'", argv[0], argv[1], list);
#endif
        int rc = system(cmd);
        qplug_separate_init(list);
        char why[80] = "";
        bool listed = qplug_separate_listed("CLAP", id);
        if (qplug_separate_count() > 0) qplug_separate_get(0, NULL, 0, NULL, 0, why, sizeof why);
        CHECK(rc != 0 && listed && !strcmp(why, "crashed Q-LiM") && qplug_wants_separate("CLAP", id),
              "BRIDGE a plugin that crashed Q-LiM is on the list by itself ('%s')", why);
        qplug_separate_set("CLAP", id, false, NULL);
        qplug_set_separate_mode(QSEP_NEVER);
    }

    /* 6) an LV2 plugin whose only window is Gtk 3: automatic -> separate process, the window opens and closes there.
     *    Needs a display and Gtk 3 (else skipped). */
#ifndef _WIN32
    if (argc >= 3 && getenv("DISPLAY") && plugwin_available()) {
        char gid[1200];
        snprintf(gid, sizeof gid, "%s|urn:qlim:test:gtk", argv[2]);
        qplug_set_separate_mode(QSEP_AUTO);
        CHECK(qplug_wants_separate("LV2", gid), "BRIDGE Gtk: an LV2 plugin with only a Gtk window goes to a separate process");
        QPlug *g = qplug_load("LV2", gid, 48000, 1024, err, sizeof err);
        CHECK(g && qplug_is_separate(g) && qplug_has_editor(g), "BRIDGE Gtk: loaded there, it has a window (%s)", err);
        if (g) {
            g_last_msg[0] = 0;
            int rc = qplug_editor_open(g, "QTestGtk", err, sizeof err);
            for (int k = 0; k < 100; ++k) { qplug_idle_all(); nap(20); }
            CHECK(rc == 0 && qplug_editor_is_open(g) && !g_last_msg[0], "BRIDGE Gtk: the Gtk window opened (%s%s)", err, g_last_msg);
            qplug_editor_close(g);
            CHECK(!qplug_editor_is_open(g), "BRIDGE Gtk: closed");
            float l[BLK], r[BLK];
            for (int i = 0; i < BLK; ++i) { l[i] = 0.4f; r[i] = -0.2f; }
            qplug_process(g, l, r, BLK);
            CHECK(fabsf(l[50] - 0.2f) < 1e-6f && fabsf(r[50] + 0.1f) < 1e-6f, "BRIDGE Gtk: its sound works (gain 0.5)");
            qplug_free(g);
        }
        qplug_set_separate_mode(QSEP_NEVER);
        QPlug *g2 = qplug_load("LV2", gid, 48000, 1024, err, sizeof err);
        if (g2) {
            int rc = qplug_editor_open(g2, "QTestGtk", err, sizeof err);
            CHECK(rc != 0 && strstr(err, "Safety"), "BRIDGE Gtk: 'Never' -> no window, the message says what to do ('%s')", err);
            qplug_free(g2);
        }
    } else printf("BRIDGE Gtk: skipped (no display)\n");
#endif

    qplug_free(sp);
    qplug_free(in);
    printf("BRIDGE: %s (%d failed)\n", fails ? "FAILED" : "all ok", fails);
    return fails ? 1 : 0;
}
