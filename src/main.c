/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _GNU_SOURCE
#include <stdio.h>
#include <math.h>
#include <limits.h>
#include "version.h"
#include <stdlib.h>
#include <stdatomic.h>
#include "sdlgfx.h"
#include "audio.h"
#include "stream.h"
#include "session.h"
#include "stretch.h"
#include "applog.h"
#include "dsp_meter.h"
#include "ui.h"
#include "filebrowser.h"
#include "project.h"
#include "settings.h"
#include "midi.h"
#include "theme.h"
#include "dropfiles.h"
#include "undo.h"
#include "plugins.h"
#include "fxchain.h"
#include "perf.h"
#include <string.h>
#include <signal.h>
#include <unistd.h>
#if defined(__GLIBC__)                 /* musl (Alpine) has no backtrace() */
#include <execinfo.h>
#define QLIM_HAVE_BACKTRACE 1
#endif
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>

static bool g_recovered_unsaved = false;      /* the work was recovered from an autosave and has not been saved by the user yet */

// ============ MIDI ============
// Everything MIDI is called from midi_poll() (the main thread), so it is safe to touch the session.

/* 1.3: the clipboard "path" of a copied MIDI clip (its notes are in the clipboard edits text) */
#define MIDI_CLIP_MARK "<midi clip>"

static void on_midi_cc(int cc, int value, int channel, void *user_data)
{
    (void)channel;
    Session *s = (Session *)user_data;
    if (!s) return;
    undo_poke(GetTime());

    int pan_armed = session_get_armed_pan_channel(s);
    if (pan_armed != -2) {
        if (pan_armed >= 0 && pan_armed < s->channel_count) {
            session_set_pan_midi_cc(s, pan_armed, cc);
        }
        session_disarm_all_pan_midi_learn(s);
        return;
    }

    int armed = session_get_armed_channel(s);
    if (armed != -2) {
        if (armed == -1) {
            session_set_master_midi_cc(s, cc);
        } else {
            session_set_midi_cc(s, armed, cc);
        }
        session_disarm_all_midi_learn(s);
        return;
    }

    // waiting for a button/pad (Solo, Mute, clip): this CC gets bound to it
    if (session_get_button_learn(s, NULL, NULL, NULL)) {
        session_handle_midi_button(s, MIDI_BIND_CC, cc, value);
        return;
    }

    for (int c = 0; c < s->channel_count; ++c) {
        int mapped_cc = session_get_midi_cc(s, c);
        if (mapped_cc == cc) {
            float g = (float)value / 127.0f;
            if (fabsf(g - FADER_UNITY_POS) < 0.5f / 127.0f) g = FADER_UNITY_POS;   /* step 36: CC 102 = exactly 0 dB */
            session_set_channel_gain(s, c, g);
        }
        int pan_cc = session_get_pan_midi_cc(s, c);
        if (pan_cc == cc) {
            float p = ((float)value / 127.0f) * 2.0f - 1.0f;
            if (value == 64) p = 0.0f;                                           /* the middle = exactly centre */
            session_set_channel_pan(s, c, p);
        }
    }

    int master_cc = session_get_master_midi_cc(s);
    if (master_cc == cc) {
        float g = (float)value / 127.0f;
        if (fabsf(g - FADER_UNITY_POS) < 0.5f / 127.0f) g = FADER_UNITY_POS;
        atomic_store(&s->master_gain, g);
    }

    // buttons/pads bound to this CC
    session_handle_midi_button(s, MIDI_BIND_CC, cc, value);
}

static void on_midi_note(int note, int velocity, bool on, int channel, void *user_data)
{
    (void)channel;
    Session *s = (Session *)user_data;
    if (!s) return;
    undo_poke(GetTime());
    // Note Off does nothing (the pads are "press = play/toggle")
    if (!on) return;
    session_handle_midi_button(s, MIDI_BIND_NOTE, note, velocity);
}

// ============ Project: save, new, open, close ============

typedef enum { PENDING_NONE = 0, PENDING_QUIT, PENDING_NEW, PENDING_OPEN, PENDING_OPEN_PATH } Pending;
static char g_pending_path[PATH_MAX];      /* step 36l: a project opened from the file manager */
static Pending g_pending = PENDING_NONE;
static bool    g_quit_now = false;

static void project_display_name(char *out, size_t size)
{
    const char *p = project_has_current() ? project_get_current_path() : "";
    const char *slash = strrchr(p, '/');
    snprintf(out, size, "%s", slash ? slash + 1 : p);
}


/* External windows (zenity) are launched from the main thread. Before that we release the mouse from SDL,
 * otherwise the mouse does not work in them (only the keyboard). */
static int ext_project_ask_save_path(char *o, size_t n) { GfxExternalDialogBegin(); int r = project_ask_save_path(o, n); GfxExternalDialogEnd(); return r; }
static int ext_project_ask_name(char *o, size_t n)      { GfxExternalDialogBegin(); int r = project_ask_name(o, n);      GfxExternalDialogEnd(); return r; }
static int ext_project_ask_open_path(char *o, size_t n) { GfxExternalDialogBegin(); int r = project_ask_open_path(o, n); GfxExternalDialogEnd(); return r; }
static int ext_project_ask_directory(char *o, size_t n) { GfxExternalDialogBegin(); int r = project_ask_directory(o, n); GfxExternalDialogEnd(); return r; }

static bool do_save(Session *session)
{
    char path[1024];
    if (project_has_current()) {
        snprintf(path, sizeof(path), "%s", project_get_current_path());
    } else if (ext_project_ask_save_path(path, sizeof(path)) != 0) {
        return false;                       // the dialog was cancelled
    }
    if (project_save(session, path) == 0) {
        project_set_current_path(path);
        undo_mark_saved(session);
        g_recovered_unsaved = false;
        ui_flash_message("Project saved");
        return true;
    }
    ui_flash_message("Save failed");
    return false;
}

static bool do_save_as(Session *session)
{
    char path[1024];
    if (ext_project_ask_save_path(path, sizeof(path)) != 0) return false;
    if (project_save(session, path) == 0) {
        project_set_current_path(path);
        undo_mark_saved(session);
        g_recovered_unsaved = false;
        ui_flash_message("Project saved");
        return true;
    }
    ui_flash_message("Save failed");
    return false;
}

static void do_collect(Session *session)
{
    /* 1) the name of the project folder, 2) WHERE it should be (it used to always go into ~/qlim/Projects without asking) */
    char name[128];
    if (ext_project_ask_name(name, sizeof(name)) != 0) return;

    char base[1024];
    if (ext_project_ask_directory(base, sizeof(base)) != 0) return;

    char project_dir[1200];
    snprintf(project_dir, sizeof(project_dir), "%s/%s", base, name);

    if (project_collect_and_save(session, project_dir, name) == 0) {
        char proj_file[1400];
        snprintf(proj_file, sizeof(proj_file), "%s/%s.limproject", project_dir, name);
        project_set_current_path(proj_file);
        undo_mark_saved(session);
        g_recovered_unsaved = false;
        char msg[1500];
        snprintf(msg, sizeof msg, "Collected to: %s", project_dir);
        ui_flash_message(msg);
    } else {
        ui_flash_message("Collect failed");
    }
}

static void do_new(Session *session)
{
    project_new(session);
    undo_reset(session);
    g_recovered_unsaved = false;
    ui_flash_message("New project");
}

static void do_open(Session *session)
{
    char path[1024];
    if (ext_project_ask_open_path(path, sizeof(path)) != 0) return;
    if (project_load(session, path) == 0) {
        project_set_current_path(path);
        undo_reset(session);
        g_recovered_unsaved = false;
        ui_flash_message("Project loaded");
    } else {
        ui_flash_message("Load failed");
    }
}

/* step 36l: opens exactly this project (double click on a .limproject) */
static void do_open_path(Session *session, const char *path)
{
    if (project_load(session, path) == 0) {
        project_set_current_path(path);
        undo_reset(session);
        g_recovered_unsaved = false;
        ui_flash_message("Project loaded");
    } else {
        char m[1200];
        snprintf(m, sizeof m, "Could not open '%s'", path);
        ui_flash_message(m);
    }
}

static void perform_pending(Session *session, Pending what)
{
    switch (what) {
    case PENDING_QUIT: g_quit_now = true; break;
    case PENDING_NEW:  do_new(session); break;
    case PENDING_OPEN: do_open(session); break;
    case PENDING_OPEN_PATH: do_open_path(session, g_pending_path); break;
    default: break;
    }
}

// Something that would lose unsaved work (close, new, open): ask first.
static void request_action(Session *session, Pending what)
{
    if (ui_quit_prompt_is_open()) return;
    undo_commit_now(session);
    if (undo_is_dirty() || g_recovered_unsaved) {
        char name[256];
        project_display_name(name, sizeof(name));
        g_pending = what;
        ui_open_quit_prompt(name);
    } else {
        perform_pending(session, what);
    }
}

/* ---- Autosave and recovery after a crash ----
 * Every minute, if there are unsaved changes, the project is saved to ~/.config/qlim/autosave.limproject.
 * At startup we create a marker file "running"; on a normal exit we remove it. If the marker is still there at startup
 * (the program crashed / the power went out), the autosave is loaded automatically. It is NOT your project: you save it with Save As. */
static double g_autosave_every = 60.0;          /* QLIM_AUTOSAVE_S - for tests only */

static void qlim_cfg_path(char *out, size_t n, const char *name)
{
    const char *h = getenv("HOME");
    snprintf(out, n, "%s/.config/qlim/%s", h ? h : ".", name);
}

static void autosave_write_marker(void)
{
    char p[1024]; qlim_cfg_path(p, sizeof p, "running");
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
}

/* true = the previous run did not end normally AND there is an autosave */
static bool autosave_crash_detected(char *auto_path, size_t n)
{
    char m[1024]; qlim_cfg_path(m, sizeof m, "running");
    qlim_cfg_path(auto_path, n, "autosave.limproject");
    FILE *f = fopen(m, "r");
    if (!f) return false;
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1) pid = 0;
    fclose(f);
#ifdef _WIN32
    if (pid > 0 && pid != (int)getpid() && qw_process_alive(pid)) return false;   /* another instance is still running */
#else
    if (pid > 0 && pid != (int)getpid() && kill(pid, 0) == 0) return false;     /* another instance is still running */
#endif
    struct stat st;
    return stat(auto_path, &st) == 0 && st.st_size > 20;
}

static void autosave_clean_exit(void)
{
    char m[1024]; qlim_cfg_path(m, sizeof m, "running");
    unlink(m);
    /* we got here = normal exit (if there was unsaved recovered work, the user has been asked) */
    char a[1024]; qlim_cfg_path(a, sizeof a, "autosave.limproject");
    unlink(a);
    strncat(a, ".bak", sizeof a - strlen(a) - 1);
    unlink(a);
}

static void autosave_tick(Session *session, double now, double *next)
{
    if (now < *next) return;
    *next = now + g_autosave_every;
    if (!undo_is_dirty()) return;
    char a[1024]; qlim_cfg_path(a, sizeof a, "autosave.limproject");
    if (project_save(session, a) == 0) printf("autosave: OK\n");
}

/* ===== Step 36l: opening a project from the file manager (double click on a .limproject) =====
 * "qlim path.limproject" -> opens it. If Q-LiM is already running, the new instance only leaves a note
 * (~/.config/qlim/open_request) and exits - the running one sees it, comes to the front and opens the project. */
static int other_instance_pid(void)
{
    char m[1024]; qlim_cfg_path(m, sizeof m, "running");
    FILE *f = fopen(m, "r");
    if (!f) return 0;
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1) pid = 0;
    fclose(f);
#ifdef _WIN32
    if (pid <= 0 || pid == (int)getpid() || !qw_process_alive(pid)) return 0;
    return qw_process_is_qlim(pid) ? pid : 0;
#else
    if (pid <= 0 || pid == (int)getpid() || kill(pid, 0) != 0) return 0;
    char exe[64], target[1024];                        /* is it really Q-LiM (process ids get reused) */
    snprintf(exe, sizeof exe, "/proc/%d/exe", pid);
    ssize_t n = readlink(exe, target, sizeof target - 1);
    if (n <= 0) return 0;
    target[n] = 0;
    const char *b = strrchr(target, '/');
    b = b ? b + 1 : target;
    return strncmp(b, "qlim", 4) == 0 ? pid : 0;
#endif
}

static bool forward_open_request(const char *path)
{
    char p[1024], tmp[1100];
    qlim_cfg_path(p, sizeof p, "open_request");
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    fprintf(f, "%s\n", path);
    fclose(f);
    return rename(tmp, p) == 0;
}

static bool take_open_request(char *out, size_t n)
{
    char p[1024]; qlim_cfg_path(p, sizeof p, "open_request");
    FILE *f = fopen(p, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    unlink(p);
    if (!ok) return false;
    size_t l = strlen(out);
    while (l && (out[l - 1] == '\n' || out[l - 1] == '\r')) out[--l] = 0;
    return l > 0;
}

#ifndef _WIN32
/* On a crash (segfault) shows where it happened - helps to find the bug. */
static char g_crash_file[1024];       /* 1.3.13: ~/.config/qlim/crash-report.txt (made ready at the start) */
static void crash_handler(int sig)
{
#ifdef QLIM_HAVE_BACKTRACE
    void *bt[40];
    int n = backtrace(bt, 40);
#endif
    static const char msg[] = "\n*** Q-LiM CRASH (fatal signal). Where it happened:\n";
    int fd = applog_orig_stderr() >= 0 ? applog_orig_stderr() : 2;   /* straight to the console (the log may not keep up) */
    if (write(fd, msg, sizeof(msg) - 1) < 0) {}
#ifdef QLIM_HAVE_BACKTRACE
    backtrace_symbols_fd(bt, n, fd);
#endif
    /* 1.3.13: the same into a file, with what the program wrote just before - so it can be sent later */
    if (g_crash_file[0]) {
        int cf = open(g_crash_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (cf >= 0) {
            static const char h[] = "Q-LiM " QLIM_VERSION " crash report\n\n*** Where it happened:\n";
            if (write(cf, h, sizeof(h) - 1) < 0) {}
            char sg[48]; int k = snprintf(sg, sizeof sg, "signal %d\n", sig);
            if (k > 0 && write(cf, sg, (size_t)k) < 0) {}
#ifdef QLIM_HAVE_BACKTRACE
            backtrace_symbols_fd(bt, n, cf);
#endif
            static const char h2[] = "\n*** The last lines of the log:\n";
            if (write(cf, h2, sizeof(h2) - 1) < 0) {}
            applog_dump_fd(cf, 150);
            close(cf);
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

/* ===== Step 35b: "freezing" =====
 * A separate thread watches whether the main loop and the audio are running. If the main loop stops for 10 seconds
 * (or the audio - for 4), the console/log shows WHERE exactly it stopped (to find the bug). */
static atomic_long g_main_beat;                       /* frame counter */
static pthread_t   g_main_tid;
static Session    *g_watch_session;

#ifdef _WIN32
static void freeze_report(void)
{
    fprintf(stderr, "\n*** Q-LiM: the interface has not responded for 10 s\n");
}
#else
static void freeze_dump(int sig)
{
    (void)sig;
#ifdef QLIM_HAVE_BACKTRACE
    void *bt[48];
    int n = backtrace(bt, 48);
#endif
    int fd = applog_orig_stderr() >= 0 ? applog_orig_stderr() : 2;
    static const char m1[] = "\n*** Q-LiM: the interface has not responded for 10 s. Main thread is here:\n";
    if (write(fd, m1, sizeof(m1) - 1) < 0) {}
#ifdef QLIM_HAVE_BACKTRACE
    backtrace_symbols_fd(bt, n, fd);
#endif
}
#endif

static void *freeze_watch(void *arg)
{
    (void)arg;
    long last = -1; int still = 0; bool told = false;
    unsigned last_ac = 0; int a_still = 0; bool a_told = false;
    for (;;) {
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, NULL);
        long b = atomic_load(&g_main_beat);
        if (GfxExternalDialogActive()) still = 0;       /* step 36l: the file chooser window is open - we are waiting for the human */
        if (b == last) { if (++still >= 10 && !told) {
            told = true;
#ifdef _WIN32
            freeze_report();
#else
            pthread_kill(g_main_tid, SIGUSR2);
#endif
        } }
        else { if (told) fprintf(stderr, ">>> Q-LiM: the interface is responding again\n"); still = 0; told = false; }
        last = b;
        Session *s = g_watch_session;
        if (s && atomic_load(&s->audio_running)) {
            unsigned ac = atomic_load(&s->audio_cycles);
            if (ac == last_ac) {
                if (++a_still >= 4 && !a_told) {
                    a_told = true;
                    fprintf(stderr, ">>> Q-LiM: audio is stuck for 4 s (a plugin is not answering?)\n");
                }
            } else { a_still = 0; a_told = false; }
            last_ac = ac;
        }
    }
    return NULL;
}

/* Ctrl+C in the terminal: the first two times - a normal close (asks to save); the third - exit immediately */
static void (*g_old_sigint)(int);
static void sigint_handler(int sig)
{
    static volatile sig_atomic_t n;
    if (++n >= 3) {
        static const char m[] = "\n>>> Q-LiM: Ctrl+C x3 - exiting now (your work is in the autosave)\n";
        if (write(2, m, sizeof m - 1) < 0) {}
        _exit(130);
    }
    if (g_old_sigint && g_old_sigint != SIG_IGN && g_old_sigint != SIG_DFL) g_old_sigint(sig);
}

static void pump_plugins(void) { GfxGLRelease(); qplug_idle_all(); GfxGLRestore(); }

static void freeze_watch_start(void)
{
    g_main_tid = pthread_self();
#ifndef _WIN32
    signal(SIGUSR2, freeze_dump);
#endif
    pthread_t t;
    if (pthread_create(&t, NULL, freeze_watch, NULL) == 0) pthread_detach(t);
}

/* Watches for dangers and writes them to the log (the indicator at the top right lights up). Called every frame, works every 0.5 s. */
static void health_watch(Session *s)
{
    static double next = 0;
    static unsigned last_xr = 0, last_over = 0, last_st = 0;
    double now = GetTime();
    if (now < next) return;
    next = now + 0.5;
    unsigned xr = dsp_meter_xruns(), ov = dsp_meter_overloads();
    if (xr > last_xr) printf("WARNING: audio dropout (xrun) x%u (total %u) - try a bigger buffer in Audio Settings\n", xr - last_xr, xr);
    if (ov > last_over) printf("WARNING: audio processing overload x%u (total %u)\n", ov - last_over, ov);
    last_xr = xr; last_over = ov;
    unsigned st = 0;
    for (int c = 0; c < s->channel_count; ++c)
        for (int sc = 0; sc < s->scene_count; ++sc)
            if (s->channels[c].clips[sc].stretch) st += stretch_underruns(s->channels[c].clips[sc].stretch);
    if (st > last_st) printf("WARNING: time-stretch could not keep up x%u (CPU too busy?)\n", st - last_st);
    last_st = st;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
        printf("Q-LiM %s\n", QLIM_VERSION);
        return 0;
    }
    /* a separate process for checking one plugin (started by the program itself) - before anything else */
#ifdef _WIN32
    qw_init(&argc, &argv);                          /* UTF-8 arguments, HOME, the folder of qlim.exe */
#endif
    if (argc >= 4 && strcmp(argv[1], "--plugin-scan") == 0) return plugins_scan_child_main(argc, argv);
    applog_init();                                  // everything written also goes into the log (the indicator at the top right)
#ifdef _WIN32
    setvbuf(stdout, NULL, _IONBF, 0);              /* msvcrt has no line buffering */
#else
    setvbuf(stdout, NULL, _IOLBF, 0);
#endif
#ifdef _WIN32
    qw_install_crash_handler();
#else
    qlim_cfg_path(g_crash_file, sizeof g_crash_file, "crash-report.txt");
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGBUS, crash_handler);
    signal(SIGFPE, crash_handler);
    signal(SIGILL, crash_handler);
#endif
    printf("Q-LiM %s build: %s %s  [%s]\n", QLIM_VERSION, __DATE__, __TIME__, QLIM_STEP);
    /* step 36l: a project from the command line / the file manager */
    char open_abs[PATH_MAX] = "";
    if (argc >= 2 && argv[1][0] != '-') {
        if (!realpath(argv[1], open_abs)) snprintf(open_abs, sizeof open_abs, "%s", argv[1]);
        if (other_instance_pid() && forward_open_request(open_abs)) {
#ifdef _WIN32
            qw_raise_other_instance();
#endif
            printf(">>> Q-LiM is already running - it will open '%s'\n", open_abs);
            return 0;
        }
    }
    printf("%s. License: %s. This program comes with ABSOLUTELY NO WARRANTY.\n", QLIM_COPYRIGHT, QLIM_LICENSE);
    printf("Time-stretch engine: %s\n", stretch_engine_name());
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    perf_load();                                      /* 1.3.7: Edit > Performance (the drawing is chosen at start) */
    GfxSetRendererPref(perf_settings()->draw);
    InitWindow(1600, 950, "Q-LiM");
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);
    MaximizeWindow();

    const char *home = getenv("HOME");
    char music_dir[512];
    snprintf(music_dir, sizeof(music_dir), "%s/Music", home ? home : "");

    FileBrowser fb;
    fb_init(&fb, music_dir);

    ui_init(&fb);
    theme_load();

    Session *session = malloc(sizeof(Session));
    if (!session) {
        TraceLog(LOG_ERROR, "Cannot allocate Session");
        CloseWindow();
        return 1;
    }
    session_init(session, 48000);

    for (int i = 0; i < NEW_PROJECT_CHANNELS; ++i) session_add_channel(session, NULL);
    for (int i = 0; i < NEW_PROJECT_SCENES; ++i) session_add_scene(session);

    AudioConfig cfg;
    settings_load_audio(&cfg);

    if (audio_init(session, &cfg) != 0) {
        TraceLog(LOG_ERROR, "Audio init failed");
        session_free(session);
        free(session);
        CloseWindow();
        return 1;
    }

    if (cfg.backend == AUDIO_BACKEND_PIPEWIRE && audio_get_current_config().backend != AUDIO_BACKEND_PIPEWIRE)
        ui_flash_message("PipeWire is not available - using PortAudio (see log)");

    // The session rate = the real rate of the audio stream (not always 48000), and the file mode
    session->sample_rate = audio_get_current_config().sample_rate;
    session_set_convert_files(session, cfg.convert_files != 0);
    streams_set_disk_mode(cfg.disk_mode);

    if (midi_init() != 0) {
        TraceLog(LOG_WARNING, "MIDI init failed (continuing anyway)");
    } else {
        midi_set_callback(on_midi_cc, session);
        midi_set_note_callback(on_midi_note, session);
        if (midi_load_config() != 0) {
            printf(">>> MIDI: no saved port, waiting for user to select\n");
        }
    }

    undo_reset(session);
    {
        char ap[1024];
        if (autosave_crash_detected(ap, sizeof ap)) {
            printf(">>> Previous session did not end normally - restoring autosave '%s'\n", ap);
            /* If loading itself breaks, we do not want a "crash on every start" loop: move the file aside and put it back after success */
            char aside[1100]; snprintf(aside, sizeof aside, "%s.recovering", ap);
            rename(ap, aside);
            int lr = project_load(session, aside);
            rename(aside, ap);
            if (lr == 0) {
                project_set_current_path("");          // it is not "your" file: Save will ask where to save
                undo_reset(session);
                g_recovered_unsaved = true;
                ui_flash_message("Recovered your work from the last session - use Save As to keep it");
            }
        }
        autosave_write_marker();
    }
    if (open_abs[0]) {                                /* step 36l: opened by double clicking a project */
        snprintf(g_pending_path, sizeof g_pending_path, "%s", open_abs);
        if (g_recovered_unsaved) request_action(session, PENDING_OPEN_PATH);   /* first ask about the recovered work */
        else do_open_path(session, open_abs);
    }
    if (getenv("QLIM_AUTOSAVE_S")) g_autosave_every = atof(getenv("QLIM_AUTOSAVE_S"));
    double next_autosave = GetTime() + g_autosave_every;
    char last_title[320] = "";

    plugins_start();                                  // the plugin list is checked in the background
    session_set_wait_pump(pump_plugins);              // while waiting for the audio, the main thread services the plugins
    freeze_watch_start();                             // if the interface "freezes" - the log says where
    g_old_sigint = signal(SIGINT, sigint_handler);

    for (;;)
    {
        atomic_fetch_add(&g_main_beat, 1);
        {   /* step 36l: another instance (double click on a project) wants us to open a project */
            static int poll_n;
            char req[1024];
            if (++poll_n >= 20 && (poll_n = 0, take_open_request(req, sizeof req))) {
                printf(">>> Open request from the file manager: '%s'\n", req);
                GfxRaiseWindow();
                snprintf(g_pending_path, sizeof g_pending_path, "%s", req);
                request_action(session, PENDING_OPEN_PATH);
            }
        }
        g_watch_session = session;
        GfxGLRelease();                               // plugins draw with their own OpenGL - ours must not get in the way
        fxchain_sync(session);                        // the channels' plugins (loading/removing)
        qplug_idle_all();                             // the plugin windows, their timers
        GfxGLRestore();
        {
            static unsigned seen_edits;               // touched in a plugin window -> "there are unsaved changes"
            unsigned e = qplug_edit_count();
            if (e != seen_edits) { seen_edits = e; undo_mark_plugin_changed(); }
        }
        if (WindowShouldClose()) {
            ClearWindowShouldClose();                 // do not close right away: there may be unsaved work
            request_action(session, PENDING_QUIT);
        }
        if (g_quit_now) break;
        autosave_tick(session, GetTime(), &next_autosave);

        bool ti_active = ui_text_input_is_active();
        bool ctx_open = ui_context_menu_is_open();
        bool mod_ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        bool mod_shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        bool any_modal = ui_any_modal_open();

        // Mouse / wheel / keys = "something may have changed" (undo takes a snapshot when the mouse is released)
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonReleased(MOUSE_BUTTON_LEFT) ||
            IsMouseButtonReleased(MOUSE_BUTTON_RIGHT) || IsMouseButtonReleased(MOUSE_BUTTON_MIDDLE) ||
            IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE) || IsKeyPressed(KEY_ENTER) ||
            (mod_ctrl && (IsKeyPressed(KEY_V) || IsKeyPressed(KEY_X) || IsKeyPressed(KEY_E) || IsKeyPressed(KEY_I) || IsKeyPressed(KEY_U)))) {
            undo_poke(GetTime());
        }

        if (!ti_active && !ctx_open && !any_modal) {
            if (IsKeyPressed(KEY_SPACE)) {
                if (session_any_playing(session)) {
                    session_pause_all(session);
                } else if (session_any_paused(session)) {
                    session_resume_all(session);
                }
            }

            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
                int sel_ch = -1, sel_sc = -1;
                ui_get_selection(&sel_ch, &sel_sc);

                if (sel_sc >= 0) {
                    if (sel_ch == -1) {
                        session_play_scene(session, sel_sc);
                    } else {
                        Clip *cl = &session->channels[sel_ch].clips[sel_sc];
                        if (atomic_load(&cl->has_clip) &&
                            !atomic_load(&cl->is_note)) {
                            // playing -> restart; not playing -> play (toggle does both)
                            session_toggle_clip(session, sel_ch, sel_sc);
                        } else if (atomic_load(&cl->has_clip) &&
                                   atomic_load(&cl->is_note)) {
                            ui_flash_message("Note (not playable)");
                        } else {
                            for (int sc = 0; sc < session->scene_count; ++sc) {
                                Clip *other = &session->channels[sel_ch].clips[sc];
                                if (atomic_load(&other->has_clip)) {
                                    atomic_store(&other->playing, false);
                                    atomic_store(&other->paused, false);
                                }
                            }
                        }
                    }
                }
            }

            if (IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE)) {
                /* step 36: a selected channel -> deletes the channel(s); marked clips -> deletes all of them */
                if (ui_channel_focus()) {
                    ui_delete_selection(session);
                } else if (!ui_edit_take_delete(session) && !ui_delete_selection(session)) {
                    int sel_ch = -1, sel_sc = -1;
                    ui_get_selection(&sel_ch, &sel_sc);
                    if (sel_ch >= 0 && sel_sc >= 0) {
                        session_clear_clip(session, sel_ch, sel_sc);
                    }
                }
            }

            if (!ui_midi_take_arrows(session)) {          /* 1.3: notes selected in the MIDI editor -> the arrows move them */
                if (IsKeyPressed(KEY_UP))    ui_move_selection(session,  0, -1);
                if (IsKeyPressed(KEY_DOWN))  ui_move_selection(session,  0, +1);
                if (IsKeyPressed(KEY_LEFT))  ui_move_selection(session, -1,  0);
                if (IsKeyPressed(KEY_RIGHT)) ui_move_selection(session, +1,  0);
            }

            if (IsKeyPressed(KEY_L) && !mod_ctrl && !ui_computer_keys_on()) ui_toggle_library();
            if (mod_ctrl && IsKeyPressed(KEY_K) && !mod_shift) ui_toggle_computer_keys();   /* 1.2 */

            // Ctrl+S
            if (mod_ctrl && IsKeyPressed(KEY_S) && !mod_shift) {
                do_save(session);
            }

            // Ctrl+Shift+S
            if (mod_ctrl && mod_shift && IsKeyPressed(KEY_S)) {
                do_save_as(session);
            }

            // Ctrl+Shift+E
            if (mod_ctrl && mod_shift && IsKeyPressed(KEY_E)) {
                do_collect(session);
            }

            // Ctrl+O
            if (mod_ctrl && IsKeyPressed(KEY_O) && !mod_shift) {
                request_action(session, PENDING_OPEN);
            }

            // Ctrl+N
            if (mod_ctrl && IsKeyPressed(KEY_N) && !mod_shift) {
                request_action(session, PENDING_NEW);
            }

            // Ctrl+Z = Undo ; Ctrl+Shift+Z / Ctrl+Y = Redo
            if (mod_ctrl && IsKeyPressed(KEY_Z) && !mod_shift) {
                ui_flash_message(undo_undo(session) ? "Undo" : "Nothing to undo");
            }
            if ((mod_ctrl && mod_shift && IsKeyPressed(KEY_Z)) ||
                (mod_ctrl && !mod_shift && IsKeyPressed(KEY_Y))) {
                ui_flash_message(undo_redo(session) ? "Redo" : "Nothing to redo");
            }

            // Ctrl+,
            if (mod_ctrl && IsKeyPressed(KEY_COMMA)) {
                ui_open_audio_settings();
            }

            // Ctrl+M
            if (mod_ctrl && IsKeyPressed(KEY_M) && !mod_shift) {
                ui_toggle_midi_learn();
            }

            // Ctrl+E
            if (mod_ctrl && IsKeyPressed(KEY_E) && !mod_shift) {
                int sel_row = ui_selected_scene_row();
                if (sel_row >= 0) {                              /* a selected scene -> the new one goes right below it */
                    if (session_insert_scene(session, sel_row + 1) < 0) ui_flash_message("Max scenes reached");
                    else ui_select_scene_row(sel_row + 1);
                } else if (session_add_scene(session) < 0) {
                    ui_flash_message("Max scenes reached");
                }
            }

            // Ctrl+I
            if (mod_ctrl && IsKeyPressed(KEY_I) && !mod_shift) {
                int idx = session_add_channel(session, NULL);
                if (idx < 0) {
                    ui_flash_message("Max channels reached");
                }
            }

            // Ctrl+W
            if (mod_ctrl && IsKeyPressed(KEY_W) && !mod_shift) {
                ui_toggle_waveform();
            }

            // Ctrl+F: the fader panel
            if (mod_ctrl && IsKeyPressed(KEY_F) && !mod_shift) {
                ui_toggle_mixer();
            }

            // 1.3.10: Ctrl+Shift+M = the master column, Ctrl+Shift+U = the level meters,
            //         Ctrl+= / Ctrl+- / Ctrl+0 = the size of the interface (also Ctrl+wheel over the clips)
            if (mod_ctrl && mod_shift && IsKeyPressed(KEY_M)) ui_toggle_master();
            if (mod_ctrl && mod_shift && IsKeyPressed(KEY_U)) ui_toggle_meters();
            if (mod_ctrl && !mod_shift && IsKeyPressed(KEY_EQUAL)) ui_zoom_step(1);
            if (mod_ctrl && !mod_shift && IsKeyPressed(KEY_MINUS)) ui_zoom_step(-1);
            if (mod_ctrl && !mod_shift && IsKeyPressed(KEY_ZERO)) ui_zoom_step(0);

            // Ctrl+R
            if (mod_ctrl && IsKeyPressed(KEY_R) && !mod_shift) {
                int sel_ch = -1, sel_sc = -1;
                ui_get_selection(&sel_ch, &sel_sc);
                if (sel_ch >= 0 && sel_sc >= 0) {
                    Clip *cl = &session->channels[sel_ch].clips[sel_sc];
                    if (atomic_load(&cl->has_clip)) {
                        int rx, ry, rw, rh;
                        if (ui_get_clip_rect(sel_ch, sel_sc,
                                             &rx, &ry, &rw, &rh)) {
                            ui_text_input_begin_at(cl->name,
                                                   rx, ry, rw, rh);
                            ui_request_rename();
                        } else {
                            ui_flash_message("Clip is offscreen");
                        }
                    } else {
                        ui_flash_message("Cannot rename empty cell");
                    }
                } else {
                    ui_flash_message("No clip selected");
                }
            }

            // Ctrl+C (step 36k: a part selected in the waveform -> copies the part)
            if (mod_ctrl && IsKeyPressed(KEY_C) && !mod_shift && !ui_edit_take_copy(session, false)) {
                int sel_ch = -1, sel_sc = -1;
                ui_get_selection(&sel_ch, &sel_sc);
                if (sel_ch >= 0 && sel_sc >= 0) {
                    Clip *cl = &session->channels[sel_ch].clips[sel_sc];
                    if (atomic_load(&cl->has_clip)) {
                        if (atomic_load(&cl->is_midi)) {                    /* 1.3: a MIDI clip: its notes go with it */
                            ui_clipboard_copy_clip(MIDI_CLIP_MARK, cl->name);
                            { char *mt_ = session_midi_text(session, sel_ch, sel_sc); ui_clipboard_set_edits(mt_); free(mt_); }
                        } else if (!atomic_load(&cl->is_note)) {
                            ui_clipboard_copy_clip(cl->path, cl->name);
                            { char *et_ = session_clip_edits_text(session, sel_ch, sel_sc); ui_clipboard_set_edits(et_); free(et_); }
                        } else {
                            ui_clipboard_copy_clip("", cl->name);
                        }
                        ui_flash_message("Copied");
                    }
                }
            }

            // Ctrl+X
            if (mod_ctrl && IsKeyPressed(KEY_X) && !mod_shift && !ui_edit_take_copy(session, true)) {
                int sel_ch = -1, sel_sc = -1;
                ui_get_selection(&sel_ch, &sel_sc);
                if (sel_ch >= 0 && sel_sc >= 0) {
                    Clip *cl = &session->channels[sel_ch].clips[sel_sc];
                    if (atomic_load(&cl->has_clip)) {
                        if (atomic_load(&cl->is_midi)) {                    /* 1.3: a MIDI clip: its notes go with it */
                            ui_clipboard_copy_clip(MIDI_CLIP_MARK, cl->name);
                            { char *mt_ = session_midi_text(session, sel_ch, sel_sc); ui_clipboard_set_edits(mt_); free(mt_); }
                        } else if (!atomic_load(&cl->is_note)) {
                            ui_clipboard_copy_clip(cl->path, cl->name);
                            { char *et_ = session_clip_edits_text(session, sel_ch, sel_sc); ui_clipboard_set_edits(et_); free(et_); }
                        } else {
                            ui_clipboard_copy_clip("", cl->name);
                        }
                        session_clear_clip(session, sel_ch, sel_sc);
                        ui_flash_message("Cut");
                    }
                }
            }

            // Ctrl+V (step 36k: a copied part + the mouse over the waveform -> pastes it there)
            if (mod_ctrl && IsKeyPressed(KEY_V) && !mod_shift && !ui_edit_take_paste(session)) {
                int sel_ch = -1, sel_sc = -1;
                ui_get_selection(&sel_ch, &sel_sc);
                if (sel_ch >= 0 && sel_sc >= 0 && ui_clipboard_has_clip()) {
                    char path[512];
                    char name[NAME_LEN];
                    ui_clipboard_get(path, sizeof(path), name, sizeof(name));

                    if (strcmp(path, MIDI_CLIP_MARK) == 0) {                /* 1.3: a MIDI clip */
                        if (session_midi_set_text(session, sel_ch, sel_sc, ui_clipboard_edits()) == 0 && name[0]) session_rename_clip(session, sel_ch, sel_sc, name);
                    } else if (path[0] == '\0' && name[0] != '\0') {
                        session_set_note(session, sel_ch, sel_sc, name);
                        ui_flash_message("Pasted note");
                    } else if (path[0] != '\0') {
                        if (session_set_clip(session, sel_ch, sel_sc, path) == 0) {
                            session_clip_set_edits_text(session, sel_ch, sel_sc, ui_clipboard_edits());
                            if (name[0] != '\0') {
                                session_rename_clip(session, sel_ch, sel_sc, name);
                            }
                            ui_flash_message("Pasted");
                        } else {
                            ui_flash_message("Cannot paste here");
                        }
                    }
                }
            }
        }

        if (ui_text_input_was_accepted()) {
            char buf[256];
            ui_text_input_get(buf, sizeof(buf));

            if (ui_text_input_is_rename_channel()) {
                int ch = ui_text_input_get_rename_channel();
                if (ch >= 0 && buf[0] != '\0') session_rename_channel(session, ch, buf);
            } else if (ui_text_input_is_rename()) {
                int ch = ui_text_input_get_rename_ch();
                int sc = ui_text_input_get_rename_scene();
                if (ch >= 0 && sc >= 0 && buf[0] != '\0') {
                    session_rename_clip(session, ch, sc, buf);
                }
            } else if (ui_text_input_is_new_note()) {
                int ch = ui_text_input_get_new_note_ch();
                int sc = ui_text_input_get_new_note_scene();
                if (ch >= 0 && sc >= 0) {
                    session_set_note(session, ch, sc, buf);
                }
            }
        }

        EditMenuAction em_act;
        if (ui_take_edit_action(&em_act)) {
            switch (em_act) {
            case EDIT_MENU_AUDIO:
                ui_open_audio_settings();
                break;
            case EDIT_MENU_MIDI:
                ui_open_midi_settings();
                break;
            case EDIT_MENU_APPEARANCE:
                ui_open_appearance();
                break;
            case EDIT_MENU_UNDO:
                ui_flash_message(undo_undo(session) ? "Undo" : "Nothing to undo");
                break;
            case EDIT_MENU_REDO:
                ui_flash_message(undo_redo(session) ? "Redo" : "Nothing to redo");
                break;
            default:
                break;
            }
        }

        audio_health_poll();
        audio_graph_poll();                        // PipeWire graph: port names, Routing <-> links
        {   /* once per second: the buffers of clips not used for a long time are freed */
            static double trim_t = 0;
            double tn = GetTime();
            if (tn - trim_t > 1.0) { trim_t = tn; streams_trim_idle(30.0); }
        }
        session_poll_bpm(session);                 // the finished tempo detections from the background thread
        session_queue_tick(session);               // the queue: prepares the next one / cleans up
        session_quant_tick(session);               // 1.1: quantized launch - finishes what the audio thread did
        session_midi_tick(session);                // 1.2: notes bound to pads do not play the instruments
        session_rec_tick(session);                 // 1.4: recordings -> files / notes; finished ones become clips
        health_watch(session);                     // xrun, overload... -> log
        if (ui_take_audio_refresh()) {
            /* PortAudio remembers the list from startup -> close it and open it again */
            bool was_lost = audio_is_lost();
            AudioConfig cur = audio_get_current_config();
            audio_shutdown();
            int rc = audio_init(session, &cur);
            ui_audio_refresh_done();
            if (rc == 0) {
                if (audio_get_current_config().sample_rate != cur.sample_rate)
                    session_set_sample_rate(session, audio_get_current_config().sample_rate);
                ui_flash_message(was_lost ? "Audio restarted" : "Device list refreshed");
            } else {
                ui_flash_message("No working audio device found");
            }
        }

#ifdef _WIN32
        {   /* ASIO: the driver's settings window can only open while we do not use the driver */
            int adev = ui_take_asio_panel();
            if (adev >= 0) {
                AudioConfig cur = audio_get_current_config();
                audio_shutdown();
                int pr = audio_asio_panel(adev);
                int rc = audio_init(session, &cur);
                if (rc == 0 && audio_get_current_config().sample_rate != cur.sample_rate)
                    session_set_sample_rate(session, audio_get_current_config().sample_rate);
                ui_flash_message(pr != 0 ? "The ASIO settings window could not be opened (see log)"
                                         : rc == 0 ? "ASIO settings opened - audio restarted"
                                                   : "No working audio device found");
            }
        }
#endif
        AudioConfig new_cfg;
        if (ui_take_audio_apply(&new_cfg)) {
            AudioConfig cur = audio_get_current_config();
            bool audio_changed =
                new_cfg.backend         != cur.backend         ||
                new_cfg.host_api_index  != cur.host_api_index  ||
                new_cfg.device_index    != cur.device_index    ||
                new_cfg.sample_rate     != cur.sample_rate     ||
                new_cfg.buffer_size     != cur.buffer_size     ||
                (new_cfg.backend == AUDIO_BACKEND_PIPEWIRE && new_cfg.pw_graph != cur.pw_graph);
            int want_convert = new_cfg.convert_files ? 1 : 0;

            session_set_convert_files(session, want_convert != 0);
            streams_set_disk_mode(new_cfg.disk_mode);

            if (!audio_changed) {
                // only the file mode: the audio is not restarted
                settings_save_audio(&new_cfg);
                ui_flash_message(want_convert
                    ? "Convert ON (applies to files loaded from now on)"
                    : "Convert OFF - files play as they are");
            } else {
                printf(">>> Applying new audio config...\n");
                audio_shutdown();

                if (audio_init(session, &new_cfg) == 0) {
                    AudioConfig applied = audio_get_current_config();
                    applied.convert_files = want_convert;
                    applied.disk_mode = new_cfg.disk_mode;
                    settings_save_audio(&applied);
                    // new rate -> the clips are reloaded for it (the markers are scaled)
                    int failed = session_set_sample_rate(session, applied.sample_rate);
                    if (new_cfg.backend == AUDIO_BACKEND_PIPEWIRE && applied.backend != AUDIO_BACKEND_PIPEWIRE)
                        ui_flash_message("PipeWire could not start - using PortAudio instead (see log)");
                    else
                        ui_flash_message(failed > 0 ? "Audio applied (some clips could not be reloaded)"
                                                    : "Audio settings applied");
                } else {
                    ui_flash_message("Audio init failed");
                }
            }
        }

        FileMenuAction fm_act;
        if (ui_take_file_action(&fm_act)) {
            switch (fm_act) {
            case FILE_MENU_NEW:              request_action(session, PENDING_NEW); break;
            case FILE_MENU_SAVE:             do_save(session); break;
            case FILE_MENU_SAVE_AS:          do_save_as(session); break;
            case FILE_MENU_COLLECT_AND_SAVE: do_collect(session); break;
            case FILE_MENU_LOAD:             request_action(session, PENDING_OPEN); break;
            default: break;
            }
        }

        // Answer to "Save changes?"
        QuitChoice qc;
        if (ui_take_quit_choice(&qc)) {
            Pending what = g_pending;
            g_pending = PENDING_NONE;
            if (qc == QUIT_CHOICE_SAVE) {
                if (do_save(session)) perform_pending(session, what);   // the save dialog was cancelled = do not continue
            } else if (qc == QUIT_CHOICE_DONT_SAVE) {
                perform_pending(session, what);
            }                                                            // Cancel: nothing
        }

        UiActionRequest act;
        if (ui_take_action(&act)) {
            switch (act.action) {
            case UI_ACTION_RENAME: {
                if (act.ch >= 0 && act.scene < 0) {
                    /* right click on a channel header */
                    int rx, ry, rw, rh;
                    if (ui_get_channel_header_rect(act.ch, &rx, &ry, &rw, &rh)) {
                        ui_text_input_begin_at(session->channels[act.ch].name, rx, ry, rw, rh);
                        ui_request_rename_channel(act.ch);
                    }
                } else if (act.ch >= 0 && act.scene >= 0) {
                    Clip *cl = &session->channels[act.ch].clips[act.scene];
                    if (atomic_load(&cl->has_clip)) {
                        int rx, ry, rw, rh;
                        if (ui_get_clip_rect(act.ch, act.scene,
                                             &rx, &ry, &rw, &rh)) {
                            ui_text_input_begin_at(cl->name,
                                                   rx, ry, rw, rh);
                            ui_request_rename();
                        }
                    }
                }
                break;
            }

            case UI_ACTION_COLOR:
                if (act.ch >= 0) ui_open_color_picker(act.ch, act.scene);
                break;

            case UI_ACTION_DELETE_CLIP: {
                if (act.ch >= 0 && act.scene >= 0 && !ui_ctx_delete_clips(session, act.ch, act.scene)) {
                    session_clear_clip(session, act.ch, act.scene);
                }
                break;
            }

            case UI_ACTION_DELETE_CHANNEL: {
                if (act.ch >= 0 && !ui_ctx_delete_channels(session, act.ch)) {
                    session_delete_channel(session, act.ch);
                }
                break;
            }

            case UI_ACTION_DELETE_SCENE: {
                if (act.scene >= 0) {
                    session_delete_scene(session, act.scene);
                }
                break;
            }

            case UI_ACTION_CUT_CLIP: {
                if (act.ch >= 0 && act.scene >= 0) {
                    Clip *cl = &session->channels[act.ch].clips[act.scene];
                    if (atomic_load(&cl->has_clip)) {
                        if (atomic_load(&cl->is_midi)) {                    /* 1.3: a MIDI clip: its notes go with it */
                            ui_clipboard_copy_clip(MIDI_CLIP_MARK, cl->name);
                            { char *mt_ = session_midi_text(session, act.ch, act.scene); ui_clipboard_set_edits(mt_); free(mt_); }
                        } else if (!atomic_load(&cl->is_note)) {
                            ui_clipboard_copy_clip(cl->path, cl->name);
                            { char *et_ = session_clip_edits_text(session, act.ch, act.scene); ui_clipboard_set_edits(et_); free(et_); }
                        } else {
                            ui_clipboard_copy_clip("", cl->name);
                        }
                        session_clear_clip(session, act.ch, act.scene);
                    }
                }
                break;
            }

            case UI_ACTION_COPY_CLIP: {
                if (act.ch >= 0 && act.scene >= 0) {
                    Clip *cl = &session->channels[act.ch].clips[act.scene];
                    if (atomic_load(&cl->has_clip)) {
                        if (atomic_load(&cl->is_midi)) {                    /* 1.3: a MIDI clip: its notes go with it */
                            ui_clipboard_copy_clip(MIDI_CLIP_MARK, cl->name);
                            { char *mt_ = session_midi_text(session, act.ch, act.scene); ui_clipboard_set_edits(mt_); free(mt_); }
                        } else if (!atomic_load(&cl->is_note)) {
                            ui_clipboard_copy_clip(cl->path, cl->name);
                            { char *et_ = session_clip_edits_text(session, act.ch, act.scene); ui_clipboard_set_edits(et_); free(et_); }
                        } else {
                            ui_clipboard_copy_clip("", cl->name);
                        }
                    }
                }
                break;
            }

            case UI_ACTION_PASTE: {
                if (act.ch >= 0 && act.scene >= 0 && ui_clipboard_has_clip()) {
                    char path[512];
                    char name[NAME_LEN];
                    ui_clipboard_get(path, sizeof(path), name, sizeof(name));

                    if (strcmp(path, MIDI_CLIP_MARK) == 0) {                /* 1.3: a MIDI clip */
                        if (session_midi_set_text(session, act.ch, act.scene, ui_clipboard_edits()) == 0 && name[0]) session_rename_clip(session, act.ch, act.scene, name);
                    } else if (path[0] == '\0' && name[0] != '\0') {
                        session_set_note(session, act.ch, act.scene, name);
                    } else if (path[0] != '\0') {
                        if (session_set_clip(session, act.ch, act.scene, path) == 0) {
                            session_clip_set_edits_text(session, act.ch, act.scene, ui_clipboard_edits());
                            if (name[0] != '\0') {
                                session_rename_clip(session, act.ch, act.scene, name);
                            }
                        } else {
                            ui_flash_message("Cannot paste here");
                        }
                    }
                }
                break;
            }

            case UI_ACTION_NEW_NOTE: {
                if (act.ch >= 0 && act.scene >= 0) {
                    int rx, ry, rw, rh;
                    if (ui_get_clip_rect(act.ch, act.scene,
                                         &rx, &ry, &rw, &rh)) {
                        ui_text_input_begin_at("", rx, ry, rw, rh);
                        ui_request_new_note(act.ch, act.scene);
                    }
                }
                break;
            }

            case UI_ACTION_NONE:
            default:
                break;
            }
        }

        midi_poll();

        // We left MIDI Learn -> nothing must stay "waiting"
        if (!ui_midi_learn_is_active()) {
            session_disarm_button_learn(session);
        }

        // Undo/redo: takes a snapshot when something has changed and the mouse is released
        undo_update(session, GetTime(),
                    IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT) ||
                    IsMouseButtonDown(MOUSE_BUTTON_MIDDLE));
        ui_set_undo_state(undo_can_undo(), undo_can_redo());

        {
            char name[256], title[320];
            project_display_name(name, sizeof(name));
            snprintf(title, sizeof(title), "%s%s%s%s", "Q-LiM", name[0] ? " - " : "", name, undo_is_dirty() ? " *" : "");
            if (strcmp(title, last_title) != 0) {
                SetWindowTitle(title);
                snprintf(last_title, sizeof(last_title), "%s", title);
            }
        }

        BeginDrawing();
        ui_draw(session, NULL, NULL);

        // Files dropped onto the window from outside (from the file manager).
        // ui_draw has just computed the cell under the cursor (the mouse is at the drop position).
        if (IsFileDropped()) {
            FilePathList dropped = LoadDroppedFiles();
            int hov_ch = -1, hov_sc = -1, sel_ch = -1, sel_sc = -1;
            ui_get_hovered_cell(&hov_ch, &hov_sc);
            ui_get_selection(&sel_ch, &sel_sc);
            char dmsg[128];
            dropfiles_load(session, (const char *const *)dropped.paths, dropped.count,
                           hov_ch, hov_sc, sel_ch, sel_sc, dmsg, sizeof(dmsg));
            if (dmsg[0]) ui_flash_message(dmsg);
            UnloadDroppedFiles(dropped);
            undo_poke(GetTime());
        }

        {   /* 1.3.4/1.3.7: the frame rate (Edit > Performance): automatic or fixed, fewer while nothing moves,
             * fewer for a while after a sound dropout. Moving the mouse or pressing a key = full speed at once. */
            bool busy = session_any_playing(session) || atomic_load(&session->rec_active) > 0 ||
                        atomic_load(&session->ql_pending) > 0;
            perf_frame(busy, qplug_editors_open() > 0);
        }
        EndDrawing();
    }

    autosave_clean_exit();
    undo_shutdown();
    midi_shutdown();
    audio_shutdown();
    GfxGLRelease();
    fxchain_shutdown(session);
    GfxGLRestore();
    session_free(session);
    free(session);
    CloseWindow();
    return 0;
}
