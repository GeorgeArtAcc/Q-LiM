/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _POSIX_C_SOURCE 200809L
#include "audio.h"
#include "mixer.h"
#include "stream.h"
#include "dsp_meter.h"
#include "devwatch.h"
#include "audio_pw.h"
#include "outx.h"
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
#include <portaudio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEFAULT_SAMPLE_RATE       48000
#define DEFAULT_FRAMES_PER_BUFFER 256
#define MAX_OUTPUT_CHANNELS       MAX_HW_OUT

static Session  *g_session = NULL;
static PaStream *g_stream  = NULL;

/* --- lost card detection --- */
static atomic_uint g_cb_count;
static DevWatch    g_watch;
static Heartbeat   g_hb;
static atomic_bool g_lost;
static char        g_lost_name[160];
static char        g_dev_name[160];
static double      g_last_poll;

static AudioConfig g_config = {
    .host_api_index = -1,
    .device_index = -1,
    .sample_rate = DEFAULT_SAMPLE_RATE,
    .buffer_size = DEFAULT_FRAMES_PER_BUFFER,
    .output_channels = 2,
    .convert_files = 0,
    .disk_mode = 0,
    .backend = AUDIO_BACKEND_PORTAUDIO,
    .pw_graph = 1,
};

/* ---- direct PipeWire ---- */
static bool      g_use_pw = false;            /* we are currently playing through PipeWire (not PortAudio) */
static bool      g_use_graph = false;         /* ... and in graph mode at that (step 28) */
static PwbSink   g_pw_sinks[PWB_MAX_SINKS];   /* the last list shown in the dialog */
static int       g_pw_nsinks = 0;
static bool      g_pw_list_valid = false;      /* the list was taken in this session (the numbers in it are valid) */
static uint64_t  g_pw_xruns_seen = 0;
static double    g_pw_paused_since = 0;

/* The whole mixer is in mixer.c (no dependency on PortAudio -> it can be tested without a sound card).
 * Only the thin callback that PortAudio calls is here. */
/* How many channels the buffer PortAudio passes to the callback has. Set BEFORE the stream is opened:
 * it used to be set only after the start, and the first callbacks could write with the wrong stride (when switching
 * from a card with many outputs to one with fewer — outside the buffer). */
static atomic_int g_cb_ch = 2;
/* 1.4: the input (recording): how many input channels the stream has, and the round trip delay in frames */
static atomic_int g_in_ch = 0;
static unsigned   g_rt_frames = 0;
static double     g_out_lat = 0.0;     /* 1.3.1: the output delay the driver reports (s) */
static char       g_in_name[256];

static int pa_callback(const void *input, void *output,
                       unsigned long frame_count,
                       const PaStreamCallbackTimeInfo *time_info,
                       PaStreamCallbackFlags status_flags,
                       void *user_data)
{
    (void)time_info; (void)user_data;
    atomic_fetch_add_explicit(&g_cb_count, 1u, memory_order_relaxed);
    if (status_flags & (paOutputUnderflow | paOutputOverflow)) dsp_meter_xrun();
    uint64_t t0 = dsp_meter_now_ns();
    int ich = atomic_load_explicit(&g_in_ch, memory_order_acquire);
    if (input && ich > 0) mixer_set_input((const float *)input, ich, frame_count);   /* 1.4 */
    outx_render(g_session, (float *)output, (unsigned)frame_count, atomic_load_explicit(&g_cb_ch, memory_order_acquire));   /* 1.4.21 */
    dsp_meter_end(t0, frame_count, g_config.sample_rate);
    return paContinue;
}

#ifdef _WIN32
/* Windows: WASAPI first (low latency, the real device rate), then DirectSound, then MME (high latency) */
static int win_api_rank(const PaDeviceInfo *info)
{
    const PaHostApiInfo *h = info ? Pa_GetHostApiInfo(info->hostApi) : NULL;
    if (!h) return 9;
    switch (h->type) {
    case paWASAPI:      return 0;
    case paDirectSound: return 1;
    case paMME:         return 2;
    default:            return 3;          /* WDM-KS, ASIO: only when chosen by hand in Edit -> Audio */
    }
}
#endif

static bool api_unusable_here(PaHostApiIndex api);
static PaDeviceIndex find_device_by_name(const char *needle)
{
    int n = Pa_GetDeviceCount();
#ifdef _WIN32
    PaDeviceIndex best = paNoDevice;
    int best_rank = 99;
    for (int i = 0; i < n; ++i) {
        const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
        if (info && info->maxOutputChannels > 0 && strstr(info->name, needle) && !api_unusable_here(info->hostApi)) {
            int r = win_api_rank(info);
            if (r < best_rank) { best_rank = r; best = i; }
        }
    }
    return best;
#else
    for (int i = 0; i < n; ++i) {
        const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
        if (info && info->maxOutputChannels > 0 &&
            strstr(info->name, needle)) {
            return i;
        }
    }
    return paNoDevice;
#endif
}

/* the system's default output; on Windows the WASAPI one (if there is WASAPI) */
/* 1.4.1: crash guard. A marker file is written before the sound card is opened and removed once the sound runs
 * (or at a normal exit). If Q-LiM finds it at start, the last start crashed inside the driver: the saved audio
 * settings are set aside (audio.conf -> audio.conf.crashed) and the default device is used, so Q-LiM can start. */
#include "settings.h"
static bool g_start_mark;
static void start_mark_path(char *out, size_t n)
{
    char p[1024];
    settings_get_config_path(p, sizeof p);
    char *slash = strrchr(p, '/');
    if (slash) slash[1] = 0;
    snprintf(out, n, "%saudio-starting", p);
}
static bool start_mark_found(void)
{
    char m[1100]; start_mark_path(m, sizeof m);
    FILE *f = fopen(m, "r");
    if (!f) return false;
    fclose(f);
    return true;
}
static void start_mark_set(bool on)
{
    char m[1100]; start_mark_path(m, sizeof m);
    if (on) { FILE *f = fopen(m, "w"); if (f) { fputs("Q-LiM is opening the sound card\n", f); fclose(f); } }
    else remove(m);
    g_start_mark = on;
}
#ifdef _WIN32
/* WASAPI exists only since Vista; on XP PortAudio may still list it, and opening it crashes */
#include <windows.h>
static bool api_unusable_here(PaHostApiIndex api)
{
    const PaHostApiInfo *h = Pa_GetHostApiInfo(api);
    if (!h || h->type != paWASAPI) return false;
    OSVERSIONINFOA v; memset(&v, 0, sizeof v); v.dwOSVersionInfoSize = sizeof v;
    return GetVersionExA(&v) && v.dwMajorVersion < 6;
}
#else
static bool api_unusable_here(PaHostApiIndex api) { (void)api; return false; }
#endif

static PaDeviceIndex default_output_device(void)
{
#ifdef _WIN32
    PaHostApiIndex w = Pa_HostApiTypeIdToHostApiIndex(paWASAPI);
    if (w >= 0 && !api_unusable_here(w)) {            /* (on XP: DirectSound) */
        const PaHostApiInfo *h = Pa_GetHostApiInfo(w);
        if (h && h->defaultOutputDevice != paNoDevice) return h->defaultOutputDevice;
    }
    PaHostApiIndex d = Pa_HostApiTypeIdToHostApiIndex(paDirectSound);
    if (d >= 0) {
        const PaHostApiInfo *h = Pa_GetHostApiInfo(d);
        if (h && h->defaultOutputDevice != paNoDevice) return h->defaultOutputDevice;
    }
    PaHostApiIndex m = Pa_HostApiTypeIdToHostApiIndex(paMME);
    if (m >= 0) {
        const PaHostApiInfo *h = Pa_GetHostApiInfo(m);
        if (h && h->defaultOutputDevice != paNoDevice) return h->defaultOutputDevice;
    }
#endif
    PaDeviceIndex def = Pa_GetDefaultOutputDevice();
    const PaDeviceInfo *di = def != paNoDevice ? Pa_GetDeviceInfo(def) : NULL;
    return di && api_unusable_here(di->hostApi) ? paNoDevice : def;
}


/* How many outputs the device REALLY has.
 * Virtual ALSA devices (default, pulse, pipewire, dmix ...) claim to have 32 or 128 channels
 * without that being true -> for them we count only stereo. Real cards (hw:..., JACK, ...) are trusted.
 * Upper limit: MAX_HW_OUT (as many as the mixer can handle). */
static int is_virtual_device(const PaDeviceInfo *info)
{
    static const char *const names[] = {
        "default", "sysdefault", "pulse", "pipewire", "dmix", "dsnoop", "samplerate", "speexrate",
        "lavrate", "upmix", "vdownmix", "oss", "null", "front", "plughw", "surround", "iec958",
        "spdif", "hdmi", "usbstream", NULL
    };
    const PaHostApiInfo *api = Pa_GetHostApiInfo(info->hostApi);
    if (!api || api->type != paALSA) return 0;          /* only ALSA has these "fake" devices */
    if (strncmp(info->name, "surround", 8) == 0) return 1;   /* surround21/40/41/50/51/71 */
    for (int i = 0; names[i]; ++i) {
        size_t n = strlen(names[i]);
        if (strncmp(info->name, names[i], n) == 0 &&
            (info->name[n] == 0 || info->name[n] == ':' || info->name[n] == ' ' || info->name[n] == ',')) return 1;
    }
    return 0;
}

static int usable_channels(const PaDeviceInfo *info)
{
    if (!info) return 0;
    int n = info->maxOutputChannels;
    if (n > 2 && is_virtual_device(info)) n = 2;
    if (n > MAX_OUTPUT_CHANNELS) n = MAX_OUTPUT_CHANNELS;
    return n;
}

/* PipeWire calls this on its real-time thread - the same as pa_callback */
static float g_pw_in[8192 * 2];                  /* 1.4: the input of this cycle */
static void pw_input(uint32_t frames)
{
    if (frames <= 8192 && pwb_input_take(g_pw_in, frames) > 0) mixer_set_input(g_pw_in, 2, frames);
}

static void pw_render(float *out, uint32_t frames, int ch, void *user)
{
    (void)user;
    atomic_fetch_add_explicit(&g_cb_count, 1u, memory_order_relaxed);
    uint64_t t0 = dsp_meter_now_ns();
    pw_input(frames);
    outx_render(g_session, out, frames, ch);              /* 1.4.21: + the extra cards, the delays */
    dsp_meter_end(t0, frames, g_config.sample_rate);
}

/* graph mode: the channels (PipeWire's RT thread) */
static void pwg_channels(float *const *outs, int nouts, uint32_t frames, void *user)
{
    (void)user;
    atomic_fetch_add_explicit(&g_cb_count, 1u, memory_order_relaxed);
    uint64_t t0 = dsp_meter_now_ns();
    pw_input(frames);
    mixer_render_channels(g_session, outs, nouts, frames);
    dsp_meter_end(t0, frames, g_config.sample_rate);
}

/* graph mode: the master (after the channels in the same cycle) */
static void pwg_master(const float *il, const float *ir, float *ol, float *orr, uint32_t frames, void *user)
{
    (void)user;
    mixer_render_master(g_session, il, ir, ol, orr, frames);
}

/* the channel names (main thread) */
static const char *g_names[MAX_CHANNELS];
static int graph_names(void)
{
    Session *s = g_session;
    int n = s ? s->channel_count : 0;
    if (n < 0) n = 0;
    if (n > MAX_CHANNELS) n = MAX_CHANNELS;
    for (int i = 0; i < n; ++i) g_names[i] = s->channels[i].name;
    return n;
}

static void pw_refresh_list(void)
{
    g_pw_nsinks = pwb_list_sinks(g_pw_sinks, PWB_MAX_SINKS);
    g_pw_list_valid = true;
}

/* the output a list number points to (NULL = "default" or an invalid number) */
static const PwbSink *pw_sink_for_index(int global_index)
{
    if (!g_pw_list_valid) return NULL;
    int slot = global_index - AUDIO_PW_DEV_BASE - 1;
    return (slot >= 0 && slot < g_pw_nsinks) ? &g_pw_sinks[slot] : NULL;
}

static const PwbSink *pw_sink_by_name(const char *name)
{
    if (name && *name)
        for (int i = 0; i < g_pw_nsinks; ++i) if (strcmp(g_pw_sinks[i].name, name) == 0) return &g_pw_sinks[i];
    for (int i = 0; i < g_pw_nsinks; ++i) if (g_pw_sinks[i].is_default) return &g_pw_sinks[i];
    return NULL;
}

static double mono_s(void);

/* ===== 1.4.9: the sample rate in PipeWire graph mode =====
 * In graph mode the channels are DSP nodes: they run at the rate of the PipeWire graph and PipeWire does NOT convert
 * them. If the graph stays at 48000 while Q-LiM computes at 88200, everything plays slower and lower (transposed).
 * So: Q-LiM asks PipeWire to allow the chosen rate (clock.allowed-rates - only for this login, until PipeWire restarts;
 * most systems allow only 48000), and if the graph (or the sound card) still does not follow, Q-LiM works at the
 * graph's rate: the files are converted with Q-LiM's resampler and the pitch is right. */
static char g_rate_note[200];
static int  g_rate_wanted;                     /* the rate chosen in the dialog (0 = the one in use) */
static double g_rate_bad_since = -1.0;

const char *audio_rate_note(void) { return g_rate_note; }
int audio_rate_wanted(void) { return g_rate_wanted > 0 ? g_rate_wanted : g_config.sample_rate; }

static void sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
#endif
}

/* the rate the graph runs at, once our node has run (0 = not known within max_ms) */
static unsigned pw_wait_graph_rate(int max_ms)
{
    unsigned r = 0;
    for (int t = 0; t < max_ms; t += 20) {
        PwbStatus st;
        memset(&st, 0, sizeof st);
        pwb_get_status(&st);
        if (st.state == 3 && st.graph_rate > 0 && st.quantum > 0) {
            r = st.graph_rate;
            if (t >= 200) break;                         /* give a rate switch a moment to happen */
        }
        sleep_ms(20);
    }
    return r;
}

#ifndef _WIN32
/* runs pw-metadata with these arguments (output thrown away); true = it worked. Never waits more than 3 s. */
static bool pw_meta_run(const char *key, const char *value)
{
    pid_t pid = fork();
    if (pid == 0) {
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        execlp("pw-metadata", "pw-metadata", "-n", "settings", "0", key, value, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return false;
    int stt = 0;
    for (int t = 0; t < 3000; t += 20) {
        pid_t r = waitpid(pid, &stt, WNOHANG);
        if (r == pid) return WIFEXITED(stt) && WEXITSTATUS(stt) == 0;
        if (r < 0) return false;
        sleep_ms(20);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &stt, 0);
    return false;
}

/* the value of a key of the PipeWire "settings" metadata ("" = not there / no pw-metadata) */
static void pw_meta_get(const char *key, char *out, size_t n)
{
    out[0] = 0;
    char cmd[160];
    snprintf(cmd, sizeof cmd, "pw-metadata -n settings 0 %s 2>/dev/null", key);
    FILE *p = popen(cmd, "r");
    if (!p) return;
    char line[512], want[96];
    snprintf(want, sizeof want, "key:'%s'", key);
    while (fgets(line, sizeof line, p)) {
        if (!strstr(line, want)) continue;
        char *v = strstr(line, "value:'");
        if (!v) continue;
        v += 7;
        char *e = strchr(v, '\'');
        if (e) *e = 0;
        snprintf(out, n, "%s", v);
    }
    pclose(p);
}
#endif

/* adds 'rate' to PipeWire's clock.allowed-rates (keeps the ones there, + 44100 and 48000). true = it was run */
static bool pw_allow_rate(int rate)
{
#ifdef _WIN32
    (void)rate;
    return false;
#else
    int rates[16], n = 0;
    char cur[256];
    pw_meta_get("clock.allowed-rates", cur, sizeof cur);    /* "[ 48000 ]" */
    for (char *q = cur; *q && n < 12; ) {
        if (*q >= '0' && *q <= '9') { int x = (int)strtol(q, &q, 10); if (x >= 8000 && x <= 768000) rates[n++] = x; }
        else ++q;
    }
    int add[3] = { 44100, 48000, rate };
    for (int k = 0; k < 3; ++k) {
        bool have = false;
        for (int i = 0; i < n; ++i) if (rates[i] == add[k]) have = true;
        if (!have && n < 16) rates[n++] = add[k];
    }
    for (int i = 0; i < n; ++i)                          /* sorted */
        for (int j = i + 1; j < n; ++j) if (rates[j] < rates[i]) { int t = rates[i]; rates[i] = rates[j]; rates[j] = t; }
    char list[200] = "[";
    for (int i = 0; i < n; ++i) { char b[16]; snprintf(b, sizeof b, " %d", rates[i]); strncat(list, b, sizeof list - strlen(list) - 1); }
    strncat(list, " ]", sizeof list - strlen(list) - 1);
    printf(">>> PipeWire: allowing %d Hz: clock.allowed-rates '%s'\n", rate, list);
    return pw_meta_run("clock.allowed-rates", list);
#endif
}

/* 1.4.10: PipeWire did not switch by itself (some systems never do) -> hold it at our rate while Q-LiM runs
 * (clock.force-rate, like "pw-metadata -n settings 0 clock.force-rate 88200"). The old value comes back when the
 * audio is closed (Q-LiM ends, another rate / card is chosen). Other programs still play right: PipeWire converts them. */
static bool g_rate_forced;
static char g_rate_force_prev[32];

static bool pw_force_rate(int rate)
{
#ifdef _WIN32
    (void)rate;
    return false;
#else
    if (!g_rate_forced) pw_meta_get("clock.force-rate", g_rate_force_prev, sizeof g_rate_force_prev);
    char v[16];
    snprintf(v, sizeof v, "%d", rate);
    printf(">>> PipeWire: holding the graph at %d Hz while Q-LiM runs (clock.force-rate, was '%s')\n", rate, g_rate_force_prev);
    if (!pw_meta_run("clock.force-rate", v)) return false;
    g_rate_forced = true;
    return true;
#endif
}

static void pw_force_rate_release(void)
{
#ifndef _WIN32
    if (!g_rate_forced) return;
    const char *prev = g_rate_force_prev[0] ? g_rate_force_prev : "0";
    printf(">>> PipeWire: clock.force-rate back to %s\n", prev);
    pw_meta_run("clock.force-rate", prev);
    g_rate_forced = false;
    g_rate_force_prev[0] = 0;
#endif
}

/* main loop: in graph mode the graph changed its rate under us (another program, a new card ...) -> the rate it runs at
 * now (once, after it stayed so for 1.5 s), otherwise 0. The caller opens the audio again. */
int audio_rate_drift(void)
{
    if (!g_use_pw || !g_use_graph) { g_rate_bad_since = -1.0; return 0; }
    PwbStatus st;
    memset(&st, 0, sizeof st);
    pwb_get_status(&st);
    if (st.state != 3 || !st.graph_rate || st.graph_rate == (unsigned)g_config.sample_rate) { g_rate_bad_since = -1.0; return 0; }
    double now = mono_s();
    if (g_rate_bad_since < 0) { g_rate_bad_since = now; return 0; }
    if (now - g_rate_bad_since < 1.5) return 0;
    g_rate_bad_since = -1.0;
    return (int)st.graph_rate;
}

/* 0 = playing through PipeWire; otherwise - it did not work (the caller falls back to PortAudio) */
static int pw_start(void)
{
    /* which output: the one chosen in the dialog (the numbers are valid only in this session) or the saved name */
    if (g_config.device_index == AUDIO_PW_DEV_BASE && g_pw_list_valid) g_config.pw_target[0] = 0;
    else {
        const PwbSink *sel = pw_sink_for_index(g_config.device_index);
        if (sel) snprintf(g_config.pw_target, sizeof g_config.pw_target, "%s", sel->name);
    }
    pw_refresh_list();
    printf(">>> PipeWire outputs (libpipewire %s):\n", pwb_library_version());
    for (int i = 0; i < g_pw_nsinks; ++i)
        printf("      %-44s %-50s outputs: %d%s\n", g_pw_sinks[i].desc, g_pw_sinks[i].name, g_pw_sinks[i].channels,
               g_pw_sinks[i].is_default ? "  (system default)" : "");

    const PwbSink *sk = pw_sink_by_name(g_config.pw_target);
    int want = sk ? sk->channels : 2;
    if (want < 2) want = 2;
    if (want > MAX_OUTPUT_CHANNELS) want = MAX_OUTPUT_CHANNELS;
    printf(">>> PipeWire: output '%s', SR=%d, buffer=%d, channels=%d\n",
           g_config.pw_target[0] ? g_config.pw_target : "(system default)", g_config.sample_rate, g_config.buffer_size, want);

    char err[160] = "";
    int ch = 0;
    g_use_graph = false;
    if (g_config.pw_graph && pwb_graph_supported()) {
        int nch = graph_names();
        if (pwb_graph_open(g_config.pw_target, g_config.sample_rate, g_config.buffer_size, nch, g_names,
                           pwg_channels, pwg_master, NULL, &ch, err, (int)sizeof err) == 0) {
            g_use_graph = true;
            /* 1.4.9: does the graph run at our rate? (otherwise everything would be transposed) */
            int want_sr = g_config.sample_rate;
            g_rate_wanted = want_sr;
            g_rate_note[0] = 0;
            g_rate_bad_since = -1.0;
            unsigned gr = pw_wait_graph_rate(1500);
            if (gr && gr != (unsigned)want_sr) {
                printf(">>> PipeWire graph runs at %u Hz, Q-LiM wants %d Hz\n", gr, want_sr);
                if (pw_allow_rate(want_sr)) {                /* allowed now -> open again, the graph may switch */
                    pwb_close();
                    if (pwb_graph_open(g_config.pw_target, want_sr, g_config.buffer_size, nch, g_names,
                                       pwg_channels, pwg_master, NULL, &ch, err, (int)sizeof err) != 0) { gr = 0; g_use_graph = false; }
                    else gr = pw_wait_graph_rate(2000);
                }
                /* 1.4.10: still not (PipeWire 1.0 often never switches by itself) -> hold it at our rate */
                if (g_use_graph && gr && gr != (unsigned)want_sr && pw_force_rate(want_sr)) {
                    gr = pw_wait_graph_rate(2500);
                    if (gr == (unsigned)want_sr)
                        snprintf(g_rate_note, sizeof g_rate_note, "PipeWire held at %d Hz while Q-LiM runs (given back when it closes)", want_sr);
                    else pw_force_rate_release();             /* the card cannot: nothing stays changed */
                }
                if (g_use_graph && gr && gr != (unsigned)want_sr) {   /* still not: Q-LiM works at the graph's rate */
                    pwb_close();
                    g_config.sample_rate = (int)gr;
                    if (pwb_graph_open(g_config.pw_target, (int)gr, g_config.buffer_size, nch, g_names,
                                       pwg_channels, pwg_master, NULL, &ch, err, (int)sizeof err) != 0) {
                        g_use_graph = false;
                        g_config.sample_rate = want_sr;     /* the single stream converts by itself */
                    } else {
                        snprintf(g_rate_note, sizeof g_rate_note,
                                 "PipeWire / the sound card runs at %u Hz - Q-LiM uses %u Hz (%d Hz is not possible there)",
                                 gr, gr, want_sr);
                    }
                } else if (gr == (unsigned)want_sr && !g_rate_note[0]) {
                    snprintf(g_rate_note, sizeof g_rate_note, "PipeWire switched to %d Hz", want_sr);
                }
                if (g_rate_note[0]) printf(">>> %s\n", g_rate_note);
            } else if (gr == 0 && g_use_graph) printf(">>> PipeWire: the graph rate is not known yet\n");
            if (ch > MAX_OUTPUT_CHANNELS) ch = MAX_OUTPUT_CHANNELS;
            if (ch < 2) ch = 2;
            printf(">>> PipeWire: graph mode - every channel and the master are visible in Patchance/qpwgraph\n");
        } else {
            fprintf(stderr, ">>> PipeWire graph mode failed (%s) -> single stream\n", err);
        }
    }
    if (!g_use_graph &&
        pwb_open(g_config.pw_target, g_config.sample_rate, g_config.buffer_size, want, pw_render, NULL, &ch, err,
                 (int)sizeof err) != 0) {
        fprintf(stderr, ">>> PipeWire native output failed: %s\n", err);
        return 1;
    }
    if (!g_use_graph) {
        /* 1.4.10: a single stream plays at the right pitch anyway (PipeWire converts it), but for the full quality the
         * graph should run at our rate: if it does not, hold it there while Q-LiM runs */
        int want_sr = g_config.sample_rate;
        g_rate_wanted = want_sr;
        g_rate_note[0] = 0;
        unsigned gr = pw_wait_graph_rate(1500);
        if (gr && gr != (unsigned)want_sr) {
            printf(">>> PipeWire graph runs at %u Hz, Q-LiM plays %d Hz\n", gr, want_sr);
            pw_allow_rate(want_sr);
            if (pw_force_rate(want_sr)) {
                gr = pw_wait_graph_rate(2500);
                if (gr == (unsigned)want_sr)
                    snprintf(g_rate_note, sizeof g_rate_note, "PipeWire held at %d Hz while Q-LiM runs (given back when it closes)", want_sr);
                else pw_force_rate_release();
            }
            if (gr && gr != (unsigned)want_sr)
                snprintf(g_rate_note, sizeof g_rate_note, "PipeWire / the sound card runs at %u Hz - it converts Q-LiM's %d Hz", gr, want_sr);
            printf(">>> %s\n", g_rate_note);
        }
    }
    g_use_pw = true;
    g_config.output_channels = ch;
    g_config.backend = AUDIO_BACKEND_PIPEWIRE;
    g_config.host_api_index = Pa_GetHostApiCount();          /* the virtual entry "PipeWire (native)" */
    /* the number in the list (for the dialog) */
    g_config.device_index = AUDIO_PW_DEV_BASE;
    if (g_config.pw_target[0])
        for (int i = 0; i < g_pw_nsinks; ++i)
            if (strcmp(g_pw_sinks[i].name, g_config.pw_target) == 0) g_config.device_index = AUDIO_PW_DEV_BASE + 1 + i;
    snprintf(g_dev_name, sizeof g_dev_name, "%s", sk ? sk->desc : "PipeWire");
    g_pw_xruns_seen = 0;
    g_pw_paused_since = 0;
    return 0;
}

/* ===================== 1.4.21: extra sound cards (outx.c does the mixing side) =====================
 * Each extra card has its own stream (PortAudio or PipeWire), opened after the main card. If it is not there
 * (a Bluetooth speaker switched off, a USB card unplugged) it is tried again every few seconds; meanwhile its outputs
 * keep their numbers and are silent. */
typedef struct {
    bool      open;
    PaStream *pa;
    int       pwh;
    int       dev_ch, dev_rate;
    double    dev_lat;              /* the card's own output delay (s), as its driver reports it */
    double    next_try;
    uint64_t  last_pulls;
    double    last_move;
    char      why[96];
} XDev;
static XDev g_xd[OUTX_MAX];

static int pa_x_cb(const void *in, void *out, unsigned long n, const PaStreamCallbackTimeInfo *ti,
                   PaStreamCallbackFlags fl, void *u)
{
    (void)in; (void)ti; (void)fl;
    int i = (int)(intptr_t)u;
    outx_pull(i, (float *)out, (unsigned)n, g_xd[i].dev_ch);
    return paContinue;
}
static void pw_x_cb(float *out, uint32_t n, int ch, void *u) { outx_pull((int)(intptr_t)u, out, n, ch); }

static double xnow(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static bool xdev_open(int i)
{
    const OutxDevCfg *c = outx_cfg_get(i);
    XDev *x = &g_xd[i];
    if (!c || x->open) return x->open;
    x->pa = NULL; x->pwh = -1; x->dev_lat = 0;
    char err[160] = "";
    if (c->kind == OUTX_KIND_PW) {
#ifdef _WIN32
        snprintf(x->why, sizeof x->why, "PipeWire is Linux only");
        return false;
#else
        int ch = 0;
        int h = pwb_extra_open(c->name, g_config.sample_rate, c->channels, pw_x_cb, (void *)(intptr_t)i, &ch, err, (int)sizeof err);
        if (h < 0) { snprintf(x->why, sizeof x->why, "%s", err[0] ? err : "cannot open"); return false; }
        x->pwh = h; x->dev_ch = ch; x->dev_rate = g_config.sample_rate;   /* PipeWire converts to the card's rate itself */
        outx_slot_attach(i, x->dev_rate, ch);
#endif
    } else {
        PaDeviceIndex dev = paNoDevice;
        for (int d = 0; d < Pa_GetDeviceCount(); ++d) {
            const PaDeviceInfo *di = Pa_GetDeviceInfo(d);
            const PaHostApiInfo *ha = di ? Pa_GetHostApiInfo(di->hostApi) : NULL;
            if (di && ha && di->maxOutputChannels > 0 && !strcmp(di->name, c->name) && (!c->api[0] || !strcmp(ha->name, c->api))) { dev = d; break; }
        }
        if (dev == paNoDevice) { snprintf(x->why, sizeof x->why, "not connected now"); return false; }
        if (g_stream && dev == g_config.device_index) { snprintf(x->why, sizeof x->why, "this is the main card"); return false; }
        const PaDeviceInfo *di = Pa_GetDeviceInfo(dev);
        int ch = c->channels < di->maxOutputChannels ? c->channels : di->maxOutputChannels;
        if (ch < 1) { snprintf(x->why, sizeof x->why, "no outputs"); return false; }
        int rates[2] = { g_config.sample_rate, (int)di->defaultSampleRate };
#ifdef _WIN32
        rates[0] = (int)di->defaultSampleRate; rates[1] = g_config.sample_rate;   /* WASAPI shared: only the Windows mixer's rate */
#endif
        PaError e = paInvalidDevice;
        for (int ri = 0; ri < 2 && !x->pa; ++ri) {
            if (rates[ri] <= 0 || (ri == 1 && rates[1] == rates[0])) continue;
            PaStreamParameters op;
            memset(&op, 0, sizeof op);
            op.device = dev; op.channelCount = ch; op.sampleFormat = paFloat32;
            op.suggestedLatency = di->defaultLowOutputLatency > 0.02 ? di->defaultLowOutputLatency : 0.02;
            x->dev_ch = ch;
            e = Pa_OpenStream(&x->pa, NULL, &op, rates[ri], paFramesPerBufferUnspecified, paClipOff, pa_x_cb, (void *)(intptr_t)i);
            if (e == paNoError) x->dev_rate = rates[ri]; else x->pa = NULL;
        }
        if (!x->pa) { snprintf(x->why, sizeof x->why, "%s", Pa_GetErrorText(e)); return false; }
        outx_slot_attach(i, x->dev_rate, ch);
        e = Pa_StartStream(x->pa);
        if (e != paNoError) {
            outx_slot_detach(i);
            Pa_CloseStream(x->pa); x->pa = NULL;
            outx_slot_free(i);
            snprintf(x->why, sizeof x->why, "%s", Pa_GetErrorText(e));
            return false;
        }
        const PaStreamInfo *si = Pa_GetStreamInfo(x->pa);
        x->dev_lat = si ? si->outputLatency : 0.0;
    }
    x->open = true;
    x->why[0] = 0;
    x->last_pulls = 0;
    x->last_move = xnow();
    printf(">>> Extra output %d: %s - outputs %d-%d, %d Hz\n", i + 1, c->desc,
           outx_first(i, g_config.output_channels) + 1, outx_first(i, g_config.output_channels) + c->channels, x->dev_rate);
    return true;
}

static void xdev_close(int i)
{
    XDev *x = &g_xd[i];
    outx_slot_detach(i);                                  /* the main callback stops feeding it */
    if (x->pa) {
        if (Pa_IsStreamActive(x->pa) == 1) Pa_AbortStream(x->pa);
        Pa_CloseStream(x->pa);
        x->pa = NULL;
    }
#ifndef _WIN32
    if (x->pwh >= 0) { pwb_extra_close(x->pwh); x->pwh = -1; }
#endif
    /* the main callback may still be inside the push: one of its cycles must pass */
    unsigned c0 = atomic_load(&g_cb_count);
    for (int t = 0; t < 50 && atomic_load(&g_cb_count) == c0 && (g_stream || g_use_pw); ++t) sleep_ms(2);
    outx_slot_free(i);
    x->open = false;
}

static void xdev_start_all(void)
{
    outx_engine_start(g_config.sample_rate, g_config.output_channels, !g_use_graph);
    if (g_use_graph) return;                              /* graph mode: the outputs are links in the PipeWire graph */
    double now = xnow();
    for (int i = 0; i < outx_cfg_count(); ++i) {
        memset(&g_xd[i], 0, sizeof g_xd[i]);
        g_xd[i].pwh = -1;
        if (!xdev_open(i)) {
            g_xd[i].next_try = now + 3.0;
            printf(">>> Extra output %d (%s): %s - will try again\n", i + 1, outx_cfg_get(i)->desc, g_xd[i].why);
        }
    }
}

static void xdev_close_all(void)
{
    for (int i = 0; i < OUTX_MAX; ++i) if (g_xd[i].open) xdev_close(i);
}

/* main thread (~2.5 times a second): a card that stopped taking audio is closed and tried again later */
static void xdev_poll(void)
{
    if (g_use_graph) return;
    double now = xnow();
    for (int i = 0; i < outx_cfg_count(); ++i) {
        XDev *x = &g_xd[i];
        const OutxDevCfg *c = outx_cfg_get(i);
        if (x->open) {
            OutxStat st;
            outx_slot_stat(i, &st);
            bool gone = false, waiting = false;
#ifndef _WIN32
            if (c->kind == OUTX_KIND_PW) {
                int ps = pwb_extra_state(x->pwh);
                if (!pwb_sink_present(c->name) || ps <= 0) gone = true;
                else if (ps == 2) waiting = true;          /* paused: not linked to its output (yet) - not a loss */
            }
#endif
            if (!gone && st.pulls != x->last_pulls) { x->last_pulls = st.pulls; x->last_move = now; x->why[0] = 0; continue; }
            if (waiting) { x->last_move = now; snprintf(x->why, sizeof x->why, "waiting for PipeWire to connect it"); continue; }
            if (gone || now - x->last_move > 3.0) {
                printf(">>> Extra output %d (%s) stopped - %s\n", i + 1, c->desc, gone ? "disconnected" : "not responding");
                xdev_close(i);
                snprintf(x->why, sizeof x->why, "%s", gone ? "disconnected" : "stopped responding");
                x->next_try = now + 3.0;
            }
        } else if (now >= x->next_try) {
            bool may = true;
#ifndef _WIN32
            if (c->kind == OUTX_KIND_PW) may = pwb_available() && pwb_sink_present(c->name);   /* (no waiting while it is off) */
#endif
            if (c->kind == OUTX_KIND_PW && !may) { x->next_try = now + 2.0; if (!x->why[0]) snprintf(x->why, sizeof x->why, "not connected now"); continue; }
            if (!xdev_open(i)) x->next_try = now + 5.0;
        }
    }
}

int audio_total_outputs(void)
{
    int m = g_config.output_channels < 2 ? 2 : g_config.output_channels;
    if (g_use_graph) return m;
    return outx_total(m);
}

bool audio_outx_possible(void) { return !g_use_graph && (g_stream || g_use_pw); }

int audio_output_groups(AudioOutGroup *g, int max)
{
    if (max < 1) return 0;
    int m = g_config.output_channels < 2 ? 2 : g_config.output_channels;
    memset(&g[0], 0, sizeof g[0]);
    snprintf(g[0].name, sizeof g[0].name, "%s", g_dev_name[0] ? g_dev_name : "Main card");
    g[0].first = 0; g[0].count = m; g[0].state = 0;
    g[0].lat_ms = g_out_lat * 1000.0;
    int n = 1;
    for (int i = 0; i < outx_cfg_count() && n < max; ++i, ++n) {
        const OutxDevCfg *c = outx_cfg_get(i);
        AudioOutGroup *o = &g[n];
        memset(o, 0, sizeof *o);
        snprintf(o->name, sizeof o->name, "%s", c->desc[0] ? c->desc : c->name);
        o->first = outx_first(i, m);
        o->count = c->channels;
        if (o->first < 0) { o->state = 2; snprintf(o->why, sizeof o->why, "past output 62"); continue; }
        if (o->first + o->count > MAX_HW_OUT) o->count = MAX_HW_OUT - o->first;
        if (g_use_graph) { o->state = 3; snprintf(o->why, sizeof o->why, "not in PipeWire graph mode"); continue; }
        if (!g_xd[i].open) { o->state = 2; snprintf(o->why, sizeof o->why, "%s", g_xd[i].why[0] ? g_xd[i].why : "not open"); continue; }
        OutxStat st;
        outx_slot_stat(i, &st);
        o->state = st.running ? 0 : 1;
        if (!st.running && g_xd[i].why[0]) snprintf(o->why, sizeof o->why, "%s", g_xd[i].why);
        o->lat_ms = st.latency_ms + g_xd[i].dev_lat * 1000.0;
        o->ppm = st.ppm;
        o->rate = g_xd[i].dev_rate;
        o->dropouts = st.underruns;
    }
    return n;
}

int audio_outx_add(const OutxDevCfg *c)
{
    int i = outx_cfg_add(c);
    if (i < 0) return -1;
    outx_save();
    if (audio_outx_possible()) {
        memset(&g_xd[i], 0, sizeof g_xd[i]);
        g_xd[i].pwh = -1;
        if (!xdev_open(i)) g_xd[i].next_try = xnow() + 3.0;
    }
    return 0;
}

void audio_outx_remove(int i)
{
    if (i < 0 || i >= outx_cfg_count()) return;
    xdev_close_all();                                     /* the ones after it get new numbers */
    outx_cfg_remove(i);
    outx_save();
    if (audio_outx_possible()) {
        double now = xnow();
        for (int k = 0; k < outx_cfg_count(); ++k) {
            memset(&g_xd[k], 0, sizeof g_xd[k]);
            g_xd[k].pwh = -1;
            if (!xdev_open(k)) g_xd[k].next_try = now + 3.0;
        }
    }
}

/* what can be added: the PipeWire outputs (Linux with PipeWire - every card and Bluetooth is there), otherwise the
 * PortAudio devices of the main card's driver (JACK -> the ALSA ones); not the main card itself */
int audio_outx_candidates(OutxDevCfg *out, int *maxch, int max)
{
    int n = 0;
#ifndef _WIN32
    if (pwb_available()) {
        static PwbSink sk[PWB_MAX_SINKS];
        int ns = pwb_list_sinks(sk, PWB_MAX_SINKS);
        const char *main_name = NULL;
        if (g_use_pw) {
            main_name = g_config.pw_target;
            if (!main_name[0]) for (int k = 0; k < ns; ++k) if (sk[k].is_default) main_name = sk[k].name;
        }
        for (int k = 0; k < ns && n < max; ++k) {
            if (main_name && main_name[0] && !strcmp(sk[k].name, main_name)) continue;
            memset(&out[n], 0, sizeof out[n]);
            out[n].kind = OUTX_KIND_PW;
            out[n].channels = sk[k].channels > 0 ? sk[k].channels : 2;
            snprintf(out[n].name, sizeof out[n].name, "%s", sk[k].name);
            snprintf(out[n].desc, sizeof out[n].desc, "%s", sk[k].desc);
            if (maxch) maxch[n] = out[n].channels;
            n++;
        }
        return n;
    }
#endif
    int main_api = -1;
    if (g_stream && g_config.device_index >= 0) {
        const PaDeviceInfo *md = Pa_GetDeviceInfo(g_config.device_index);
        if (md) main_api = md->hostApi;
    }
    const PaHostApiInfo *mha = main_api >= 0 ? Pa_GetHostApiInfo(main_api) : NULL;
    bool main_jack = mha && mha->type == paJACK;
    for (int d = 0; d < Pa_GetDeviceCount() && n < max; ++d) {
        const PaDeviceInfo *di = Pa_GetDeviceInfo(d);
        if (!di || di->maxOutputChannels <= 0 || d == g_config.device_index) continue;
        if (api_unusable_here(di->hostApi)) continue;
        const PaHostApiInfo *ha = Pa_GetHostApiInfo(di->hostApi);
        if (!ha) continue;
        if (main_jack) { if (ha->type != paALSA) continue; }
        else if (main_api >= 0 && di->hostApi != main_api) continue;
        if (is_virtual_device(di)) continue;
        memset(&out[n], 0, sizeof out[n]);
        out[n].kind = OUTX_KIND_PA;
        out[n].channels = usable_channels(di);
        if (out[n].channels < 1) out[n].channels = di->maxOutputChannels;
        snprintf(out[n].api, sizeof out[n].api, "%s", ha->name);
        snprintf(out[n].name, sizeof out[n].name, "%s", di->name);
        snprintf(out[n].desc, sizeof out[n].desc, "%s", di->name);
        if (maxch) maxch[n] = out[n].channels;
        n++;
    }
    return n;
}

int audio_init(Session *session, const AudioConfig *config)
{
    g_session = session;

    if (config) {
        g_config = *config;
    }

    if (streams_init() != 0) {
        fprintf(stderr, "streams_init failed\n");
        return 1;
    }

    PaError err = Pa_Initialize();
    if (err != paNoError) {
        fprintf(stderr, "PortAudio init failed: %s\n", Pa_GetErrorText(err));
        return 1;
    }

    /* list of everything PortAudio sees (helps with problems: PipeWire, JACK, USB cards) */
    printf(">>> Audio devices seen by PortAudio:\n");
    for (int i = 0; i < Pa_GetDeviceCount(); ++i) {
        const PaDeviceInfo *di = Pa_GetDeviceInfo(i);
        if (!di || di->maxOutputChannels <= 0) continue;
        const PaHostApiInfo *ha = Pa_GetHostApiInfo(di->hostApi);
        printf("      [%d] %-6s %-40s outputs: reported %d, used %d%s\n", i, ha ? ha->name : "?", di->name,
               di->maxOutputChannels, usable_channels(di), is_virtual_device(di) ? "  (virtual device -> stereo only)" : "");
    }

    /* The names (if saved) win over the numbers: a new card or driver (e.g. ASIO) shifts the numbers */
    if (g_config.backend != AUDIO_BACKEND_PIPEWIRE && g_config.api_name[0]) {
        int napi = Pa_GetHostApiCount();
        int api = -1;
        for (int i = 0; i < napi; ++i) { const PaHostApiInfo *h = Pa_GetHostApiInfo(i); if (h && !strcmp(h->name, g_config.api_name)) api = i; }
        if (api >= 0 && api != g_config.host_api_index) {
            printf(">>> Audio driver '%s' is now number %d (was %d)\n", g_config.api_name, api, g_config.host_api_index);
            g_config.host_api_index = api;
        }
        if (api >= 0 && g_config.dev_name[0]) {
            const PaDeviceInfo *cur = g_config.device_index >= 0 ? Pa_GetDeviceInfo(g_config.device_index) : NULL;
            if (!cur || cur->hostApi != api || strcmp(cur->name, g_config.dev_name) != 0) {
                int found = -1;
                for (int i = 0; i < Pa_GetDeviceCount(); ++i) {
                    const PaDeviceInfo *di = Pa_GetDeviceInfo(i);
                    if (di && di->hostApi == api && di->maxOutputChannels > 0 && !strcmp(di->name, g_config.dev_name)) { found = i; break; }
                }
                printf(">>> Audio device '%s': %s\n", g_config.dev_name, found >= 0 ? "found (new number)" : "not found now - using the saved number");
                if (found >= 0) g_config.device_index = found;
            }
        }
    }

    if (g_config.backend != AUDIO_BACKEND_PIPEWIRE) {
        if (start_mark_found()) {
            char cp[1024], bad[1100];
            settings_get_config_path(cp, sizeof cp);
            snprintf(bad, sizeof bad, "%s.crashed", cp);
            remove(bad);
            rename(cp, bad);
            fprintf(stderr, ">>> The last start of the sound card ('%s' / '%s') crashed Q-LiM - starting with the default "
                    "device. Those settings are kept in audio.conf.crashed.\n", g_config.api_name, g_config.dev_name);
            g_config.host_api_index = -1; g_config.device_index = -1;
            g_config.api_name[0] = 0; g_config.dev_name[0] = 0;
            if (g_config.buffer_size < 2048) g_config.buffer_size = 2048;
        }
        if (g_config.host_api_index >= 0 && api_unusable_here(g_config.host_api_index)) {
            fprintf(stderr, ">>> WASAPI does not work on this Windows (it needs Vista or newer) - using the default device\n");
            g_config.host_api_index = -1; g_config.device_index = -1;
        }
        if (g_config.device_index >= 0) {
            const PaDeviceInfo *di = Pa_GetDeviceInfo(g_config.device_index);
            if (di && api_unusable_here(di->hostApi)) g_config.device_index = -1;
        }
        start_mark_set(true);
    }

    /* The numbers saved in audio.conf may be stale (after a restart/USB change the numbers shift).
     * We check them, otherwise the pointer is NULL and the program crashes. */
    if (g_config.device_index >= 0 && g_config.backend != AUDIO_BACKEND_PIPEWIRE) {
        const PaDeviceInfo *di = Pa_GetDeviceInfo(g_config.device_index);
        if (!di || di->maxOutputChannels <= 0) {
            printf(">>> Saved audio device [%d] no longer exists -> choosing automatically\n", g_config.device_index);
            g_config.device_index = -1;
        }
    }
    if (g_config.host_api_index >= 0 && g_config.backend != AUDIO_BACKEND_PIPEWIRE &&
        !Pa_GetHostApiInfo(g_config.host_api_index)) {
        printf(">>> Saved audio driver [%d] no longer exists -> choosing automatically\n", g_config.host_api_index);
        g_config.host_api_index = -1;
    }

    PaDeviceIndex device = paNoDevice;

    if (g_config.backend == AUDIO_BACKEND_PIPEWIRE) {
        /* the device is chosen in pw_start() */
    } else if (g_config.device_index >= 0) {
        device = g_config.device_index;
    } else if (g_config.host_api_index >= 0) {
        const PaHostApiInfo *api_info =
            Pa_GetHostApiInfo(g_config.host_api_index);
        if (api_info && api_info->defaultOutputDevice != paNoDevice) {
            device = api_info->defaultOutputDevice;
        } else if (api_info) {
            int n = Pa_GetDeviceCount();
            for (int i = 0; i < n; ++i) {
                const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
                if (info && info->hostApi == g_config.host_api_index &&
                    info->maxOutputChannels > 0) {
                    device = i;
                    break;
                }
            }
        }
    } else {
        device = find_device_by_name("Scarlett");
        if (device == paNoDevice) device = find_device_by_name("UMC");
        if (device == paNoDevice) device = find_device_by_name("USB");
        if (device == paNoDevice) device = default_output_device();
    }

    if (g_config.sample_rate <= 0) g_config.sample_rate = DEFAULT_SAMPLE_RATE;
    if (g_config.buffer_size <= 0) g_config.buffer_size = DEFAULT_FRAMES_PER_BUFFER;

    /* margin for fast loop switching: at least 2 audio buffers + headroom */
    streams_set_trim_margin((size_t)g_config.buffer_size * 2 + 2048);

    g_use_pw = false;
    if (g_config.backend == AUDIO_BACKEND_PIPEWIRE) {
        if (pw_start() == 0) {
            xdev_start_all();                             /* 1.4.21 */
            atomic_store(&session->audio_running, true);
            atomic_store(&g_lost, false);
            heartbeat_reset(&g_hb);
            g_last_poll = 0;
            printf(">>> Audio STARTED (PipeWire native)\n");
            return 0;
        }
        /* PipeWire did not work -> so the program is not left without sound: the ordinary path through PortAudio */
        printf(">>> Falling back to PortAudio\n");
        g_config.backend = AUDIO_BACKEND_PORTAUDIO;
        g_config.host_api_index = -1;
        g_config.device_index = -1;
        device = find_device_by_name("Scarlett");
        if (device == paNoDevice) device = find_device_by_name("UMC");
        if (device == paNoDevice) device = find_device_by_name("USB");
        if (device == paNoDevice) device = default_output_device();
    }

    /* Try the chosen device; if it is busy/broken — once more with the system default. */
    bool tried_default = false;
#ifdef _WIN32
    bool tried_rate = false;
#endif
    for (;;) {
        const PaDeviceInfo *dev = device != paNoDevice ? Pa_GetDeviceInfo(device) : NULL;
        const char *why = NULL;
        int use_ch = 0;
        PaStreamParameters out_params;
        memset(&out_params, 0, sizeof(out_params));

        if (!dev) {
            why = "no output device";
        } else {
            printf(">>> Audio device [%d]: %s\n", device, dev->name);
            int max_ch = usable_channels(dev);
            if (max_ch < 2) {
                why = "no usable stereo output (maybe busy)";
            } else {
                use_ch = max_ch;            /* the number of outputs is determined by the card itself */
                printf(">>> Config: SR=%d, buffer=%d, channels=%d\n", g_config.sample_rate, g_config.buffer_size, use_ch);
                out_params.device = device;
                out_params.channelCount = use_ch;
                out_params.sampleFormat = paFloat32;
                /* 1.3.1: the delay follows the buffer chosen in Edit -> Audio (before, the driver's "low" default was
                 * used, which on ALSA/PulseAudio devices is often 20-40 ms whatever the buffer was) */
                double want_lat = (double)g_config.buffer_size / (double)g_config.sample_rate;
#ifdef _WIN32
                /* MME and DirectSound (the drivers of Windows XP) cannot run with a few-ms device buffer: the sound
                 * breaks up. There the buffer size only sets the block size; the driver keeps its own safe minimum. */
                {
                    const PaHostApiInfo *hai = Pa_GetHostApiInfo(dev->hostApi);
                    if (hai && (hai->type == paMME || hai->type == paDirectSound) && want_lat < dev->defaultLowOutputLatency)
                        want_lat = dev->defaultLowOutputLatency;
                }
#endif
                out_params.suggestedLatency = want_lat;
                out_params.hostApiSpecificStreamInfo = NULL;

                atomic_store(&g_cb_ch, use_ch);
                /* 1.4: with the input too (recording) - the same device, or the default input of the same driver */
                PaStreamParameters in_params;
                memset(&in_params, 0, sizeof in_params);
                PaDeviceIndex in_dev = paNoDevice;
                if (dev->maxInputChannels > 0) in_dev = device;
                else {
                    const PaHostApiInfo *ha = Pa_GetHostApiInfo(dev->hostApi);
                    if (ha && ha->defaultInputDevice != paNoDevice) in_dev = ha->defaultInputDevice;
                }
                const PaDeviceInfo *idev = in_dev != paNoDevice ? Pa_GetDeviceInfo(in_dev) : NULL;
                atomic_store(&g_in_ch, 0);
                err = paInvalidDevice;
                if (idev && idev->maxInputChannels > 0) {
                    int ich = idev->maxInputChannels > 16 ? 16 : idev->maxInputChannels;
                    if (is_virtual_device(idev) && ich > 2) ich = 2;
                    in_params.device = in_dev;
                    in_params.channelCount = ich;
                    in_params.sampleFormat = paFloat32;
                    in_params.suggestedLatency = want_lat;
                    err = Pa_OpenStream(&g_stream, &in_params, &out_params, g_config.sample_rate, g_config.buffer_size,
                                        paClipOff, pa_callback, NULL);
                    if (err == paNoError) {
                        atomic_store(&g_in_ch, ich);
                        snprintf(g_in_name, sizeof g_in_name, "%s", idev->name);
                        printf(">>> Audio input: %s (%d channels)\n", idev->name, ich);
                    } else {
                        printf(">>> Audio input could not be opened with the output (%s) - playing without recording\n", Pa_GetErrorText(err));
                    }
                }
                if (err != paNoError)
                    err = Pa_OpenStream(&g_stream, NULL, &out_params, g_config.sample_rate, g_config.buffer_size,
                                        paClipOff, pa_callback, NULL);
                if (err != paNoError && use_ch > 2) {
                    /* the card did not open all of its outputs (busy/limited) -> try stereo */
                    fprintf(stderr, "Opening %d channels failed (%s), trying 2\n", use_ch, Pa_GetErrorText(err));
                    use_ch = 2;
                    out_params.channelCount = 2;
                    atomic_store(&g_cb_ch, 2);
                    err = Pa_OpenStream(&g_stream, NULL, &out_params, g_config.sample_rate, g_config.buffer_size,
                                        paClipOff, pa_callback, NULL);
                }
                if (err == paNoError) {
                    err = Pa_StartStream(g_stream);
                    if (err != paNoError) { Pa_CloseStream(g_stream); g_stream = NULL; }
                }
                if (err == paNoError) {
                    const PaStreamInfo *si = Pa_GetStreamInfo(g_stream);
                    g_rt_frames = 0;
                    g_out_lat = si ? si->outputLatency : 0.0;
                    printf(">>> Audio: buffer %d frames, output delay %.1f ms (driver)\n", g_config.buffer_size, g_out_lat * 1000.0);
                    if (si && atomic_load(&g_in_ch) > 0)
                        g_rt_frames = (unsigned)((si->inputLatency + si->outputLatency) * (double)g_config.sample_rate + 0.5);
                    if (g_rt_frames > (unsigned)g_config.sample_rate) g_rt_frames = 0;   /* nonsense from the driver */
                    g_config.output_channels = use_ch;
                    g_config.device_index = device;
                    {   /* remember the names too (see AudioConfig) */
                        const PaHostApiInfo *ha = Pa_GetHostApiInfo(dev->hostApi);
                        snprintf(g_config.api_name, sizeof g_config.api_name, "%s", ha ? ha->name : "");
                        snprintf(g_config.dev_name, sizeof g_config.dev_name, "%s", dev->name);
                    }
                    snprintf(g_dev_name, sizeof g_dev_name, "%s", dev ? dev->name : "");
                    break;                  /* success */
                }
                g_stream = NULL;
                why = Pa_GetErrorText(err);
            }
        }

        fprintf(stderr, "Audio device %s failed: %s\n", dev ? dev->name : "(none)", why ? why : "?");
#ifdef _WIN32
        /* WASAPI (shared) opens only at the rate set in the Windows sound settings -> try that rate once */
        if (dev && !tried_rate && (int)dev->defaultSampleRate > 0 && (int)dev->defaultSampleRate != g_config.sample_rate) {
            tried_rate = true;
            printf(">>> Trying the device's own rate %d Hz\n", (int)dev->defaultSampleRate);
            g_config.sample_rate = (int)dev->defaultSampleRate;
            continue;
        }
#endif
        PaDeviceIndex def = default_output_device();
        if (!tried_default && def != paNoDevice && def != device) {
            tried_default = true;
            printf(">>> Trying the system default output device instead\n");
            device = def;
            continue;
        }
        Pa_Terminate();
        if (g_start_mark) start_mark_set(false);      /* no device is not a crash */
        return 1;
    }

    xdev_start_all();                                     /* 1.4.21 */
    atomic_store(&session->audio_running, true);
    atomic_store(&g_lost, false);
    heartbeat_reset(&g_hb);
    devwatch_init(&g_watch, g_dev_name, NULL);
    g_last_poll = 0;
    printf(">>> Audio STARTED\n");
    return 0;
}

void audio_shutdown(void)
{
    xdev_close_all();                        /* 1.4.21: the extra cards first (they use the main card's ring) */
    if (g_use_pw) {
        pwb_close();
        g_use_pw = false;
        g_use_graph = false;
    }
    pw_force_rate_release();                 /* 1.4.10: PipeWire's rate as it was */
    pwb_forget_availability();               /* the next "is there PipeWire" query - from scratch */
    if (g_stream) {
        /* with a lost card Stop may hang -> Abort is safe */
        if (atomic_load(&g_lost)) Pa_AbortStream(g_stream); else Pa_StopStream(g_stream);
        Pa_CloseStream(g_stream);
        g_stream = NULL;
        atomic_store(&g_in_ch, 0);
    }
    if (g_session) atomic_store(&g_session->audio_running, false);
    atomic_store(&g_lost, false);
    outx_engine_stop();
#ifndef _WIN32
    pwb_close();                             /* the extra PipeWire outputs may have connected even without PipeWire as the main */
#endif
    Pa_Terminate();
    if (g_start_mark) start_mark_set(false);
    printf("Audio shut down.\n");
}

#if defined(_WIN32) && defined(QLIM_NO_ASIO)
/* 32-bit build without the ASIO SDK: MME / DirectSound / WASAPI / WDM-KS only */
bool audio_device_is_asio(int dev) { return false; }
int audio_asio_panel(int dev) { return -1; }
#elif defined(_WIN32)
#include "pa_asio.h"
bool audio_device_is_asio(int dev)
{
    if (dev < 0 || dev >= Pa_GetDeviceCount()) return false;
    const PaDeviceInfo *info = Pa_GetDeviceInfo(dev);
    const PaHostApiInfo *h = info ? Pa_GetHostApiInfo(info->hostApi) : NULL;
    return h && h->type == paASIO;
}
int audio_asio_panel(int dev)
{
    bool own = false;
    if (Pa_GetHostApiCount() < 0) { if (Pa_Initialize() != paNoError) return -1; own = true; }
    PaError e = audio_device_is_asio(dev) ? PaAsio_ShowControlPanel(dev, NULL) : paInvalidDevice;
    if (e != paNoError) fprintf(stderr, "ASIO control panel: %s\n", Pa_GetErrorText(e));
    if (own) Pa_Terminate();
    return e == paNoError ? 0 : -1;
}
#endif

int audio_get_host_api_count(void)
{
    if (Pa_GetHostApiCount() < 0) {
        if (Pa_Initialize() != paNoError) return 0;
    }
    int n = Pa_GetHostApiCount();
    if (n < 0) n = 0;
    return n + (pwb_available() ? 1 : 0);    /* the last entry = direct PipeWire */
}

bool audio_host_api_is_pipewire(int index)
{
    int n = Pa_GetHostApiCount();
    return n >= 0 && index == n && pwb_available();
}

const char *audio_get_host_api_name(int index)
{
    if (audio_host_api_is_pipewire(index)) return "PipeWire (native)";
    if (index < 0 || index >= Pa_GetHostApiCount()) return "?";
    const PaHostApiInfo *info = Pa_GetHostApiInfo(index);
    if (info && api_unusable_here(index)) return "Windows WASAPI (needs Vista or newer)";
    return info ? info->name : "?";
}

int audio_get_device_count(int host_api_index)
{
    if (audio_host_api_is_pipewire(host_api_index)) {
        pw_refresh_list();
        return 1 + g_pw_nsinks;               /* + "default" */
    }
    int count = 0;
    int n = Pa_GetDeviceCount();
    for (int i = 0; i < n; ++i) {
        const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
        if (!info || info->maxOutputChannels <= 0) continue;
        if (host_api_index >= 0 && info->hostApi != host_api_index) continue;
        if (api_unusable_here(info->hostApi)) continue;           /* WASAPI on XP: nothing to choose */
        count++;
    }
    return count;
}

const char *audio_get_device_name(int host_api_index,
                                  int device_local_index,
                                  int *out_global_index)
{
    if (audio_host_api_is_pipewire(host_api_index)) {
        static char nm[200];
        if (device_local_index == 0) {
            const PwbSink *d = pw_sink_by_name("");
            if (out_global_index) *out_global_index = AUDIO_PW_DEV_BASE;
            snprintf(nm, sizeof nm, "System default%s%s", d ? ": " : "", d ? d->desc : "");
            return nm;
        }
        int slot = device_local_index - 1;
        if (slot >= 0 && slot < g_pw_nsinks) {
            if (out_global_index) *out_global_index = AUDIO_PW_DEV_BASE + 1 + slot;
            return g_pw_sinks[slot].desc;
        }
        if (out_global_index) *out_global_index = -1;
        return "?";
    }
    int idx = 0;
    int n = Pa_GetDeviceCount();
    for (int i = 0; i < n; ++i) {
        const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
        if (!info || info->maxOutputChannels <= 0) continue;
        if (host_api_index >= 0 && info->hostApi != host_api_index) continue;
        if (api_unusable_here(info->hostApi)) continue;

        if (idx == device_local_index) {
            if (out_global_index) *out_global_index = i;
            return info->name;
        }
        idx++;
    }
    if (out_global_index) *out_global_index = -1;
    return "?";
}

int audio_get_device_max_channels(int global_device_index)
{
    if (global_device_index < 0) return 0;
    if (global_device_index >= AUDIO_PW_DEV_BASE) {
        const PwbSink *sk = global_device_index == AUDIO_PW_DEV_BASE ? pw_sink_by_name("") : pw_sink_for_index(global_device_index);
        int n = sk ? sk->channels : 2;
        return n > MAX_OUTPUT_CHANNELS ? MAX_OUTPUT_CHANNELS : n;
    }
    return usable_channels(Pa_GetDeviceInfo(global_device_index));
}

int audio_get_device_default_samplerate(int global_device_index)
{
    if (global_device_index < 0) return DEFAULT_SAMPLE_RATE;
    if (global_device_index >= AUDIO_PW_DEV_BASE) {
        const PwbSink *sk = global_device_index == AUDIO_PW_DEV_BASE ? pw_sink_by_name("") : pw_sink_for_index(global_device_index);
        return (sk && sk->rate > 0) ? sk->rate : DEFAULT_SAMPLE_RATE;
    }
    const PaDeviceInfo *info = Pa_GetDeviceInfo(global_device_index);
    return info ? (int)info->defaultSampleRate : DEFAULT_SAMPLE_RATE;
}

AudioConfig audio_get_current_config(void)
{
    AudioConfig c = g_config;
    c.disk_mode = streams_get_disk_mode();
    if (g_session) c.convert_files = atomic_load(&g_session->convert_files) ? 1 : 0;   // the live value
    return c;
}


/* ============ Lost sound card ============ */

static double mono_s(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

void audio_health_poll(void)
{
    if ((!g_stream && !g_use_pw) || atomic_load(&g_lost)) return;
    xdev_poll();                                          /* 1.4.21 */
    double now = mono_s();
    if (now - g_last_poll < 0.4) return;                 /* not more often than ~2.5 times per second */
    g_last_poll = now;

    const char *why = NULL;
    if (g_use_pw) {
        static char pw_why[160];
        PwbStatus st;
        pwb_get_status(&st);
        while (g_pw_xruns_seen < st.xruns) { dsp_meter_xrun(); g_pw_xruns_seen++; }   /* into the total dropout counter */
        if (st.lost) { snprintf(pw_why, sizeof pw_why, "PipeWire: %s", st.why); why = pw_why; }
        else if (st.state == 3) {
            /* "paused" (no output connected - e.g. disconnected by hand in qpwgraph/Helvum) is NOT a loss:
             * PipeWire will connect us again; the dialog shows "not connected". We only track a real stop. */
            if (g_pw_paused_since > 0) { heartbeat_reset(&g_hb); g_pw_paused_since = 0; }
            if (heartbeat_stalled(&g_hb, atomic_load(&g_cb_count), now, 2.0))
                why = "PipeWire: audio stopped responding";
        } else if (g_pw_paused_since == 0) {
            g_pw_paused_since = now;
            printf(">>> PipeWire: not connected to any output right now\n");
        }
        if (!why) return;
        snprintf(g_lost_name, sizeof g_lost_name, "%s", g_dev_name[0] ? g_dev_name : "PipeWire output");
        atomic_store(&g_lost, true);
        if (g_session) atomic_store(&g_session->audio_running, false);
        fprintf(stderr, ">>> AUDIO DEVICE LOST: %s (%s)\n", g_lost_name, why);
        return;
    }

    if (g_start_mark && atomic_load(&g_cb_count) > 50) start_mark_set(false);     /* the sound runs: not a crash */
    if (Pa_IsStreamActive(g_stream) != 1) why = "stream stopped";
    else if (devwatch_present(&g_watch) == 0) why = "sound card disappeared";
    else if (heartbeat_stalled(&g_hb, atomic_load(&g_cb_count), now, 2.0)) why = "audio stopped responding";
    if (!why) return;

    snprintf(g_lost_name, sizeof g_lost_name, "%s", g_dev_name[0] ? g_dev_name : "audio device");
    atomic_store(&g_lost, true);
    if (g_session) atomic_store(&g_session->audio_running, false);
    fprintf(stderr, ">>> AUDIO DEVICE LOST: %s (%s)\n", g_lost_name, why);
}

bool audio_is_lost(void) { return atomic_load(&g_lost); }

/* 1.4: recording */
int audio_input_channels(void)
{
    if (g_use_pw) return pwb_input_channels();
    return g_stream ? atomic_load(&g_in_ch) : 0;
}
unsigned audio_roundtrip_frames(void)
{
    if (g_use_pw) return pwb_input_latency_frames();
    return g_rt_frames;
}
void audio_input_want(bool on) { if (g_use_pw) pwb_input_enable(on); }
const char *audio_lost_name(void) { return g_lost_name; }

bool audio_actual_buffer(int *frames, double *out_ms)
{
    *frames = 0; *out_ms = 0.0;
    if (g_use_pw) {
        PwbStatus st;
        pwb_get_status(&st);
        if (st.state != 3 || st.graph_rate == 0 || st.quantum == 0) return false;
        *frames = (int)st.quantum;
        *out_ms = st.quantum * 1000.0 / st.graph_rate;
        return true;
    }
    if (!g_stream) return false;
    *frames = g_config.buffer_size;
    *out_ms = g_out_lat * 1000.0;
    return true;
}

bool audio_engine_status_text(char *out, int size)
{
    if (!out || size <= 0) return false;
    out[0] = 0;
    if (!g_use_pw) return false;
    PwbStatus st;
    pwb_get_status(&st);
    if (st.state == 3 && st.graph_rate > 0) {
        double ms = st.quantum * 1000.0 / st.graph_rate;
        bool resample = st.graph_rate != (uint32_t)g_config.sample_rate;
        snprintf(out, (size_t)size, "PipeWire: %u Hz, buffer %u (%.1f ms)%s  |  xruns: %llu", st.graph_rate, st.quantum, ms,
                 resample ? "  - RESAMPLED (graph rate differs)" : "", (unsigned long long)st.xruns);
    } else if (st.state == 2) {
        snprintf(out, (size_t)size, "PipeWire: not connected to any output (check routing / session manager)");
    } else if (st.lost) {
        snprintf(out, (size_t)size, "PipeWire: %s", st.why);
    } else {
        snprintf(out, (size_t)size, "PipeWire: connecting...");
    }
    return true;
}

/* ============ PipeWire graph (step 28) ============ */

bool audio_graph_active(void) { return g_use_pw && g_use_graph; }

bool audio_graph_poll(void)
{
    if (!g_use_pw || !g_use_graph || !g_session || atomic_load(&g_lost)) return false;
    Session *s = g_session;
    static PwbRoute routes[MAX_CHANNELS];
    int n = graph_names();
    for (int i = 0; i < n; ++i) {
        routes[i].l = session_route_get(s, i, 0);
        routes[i].r = session_route_get(s, i, 1);
    }
    PwbRoute m = { session_master_route_get(s, 0), session_master_route_get(s, 1) };
    PwbRoute m0 = m;
    /* is anything playing (for renaming: the node is replaced only in silence) - checked rarely */
    static double t_q = 0;
    static bool quiet = true;
    if (mono_s() - t_q > 0.25) {
        t_q = mono_s();
        quiet = true;
        for (int c = 0; c < n && quiet; ++c)
            for (int sc = 0; sc < s->scene_count; ++sc)
                if (atomic_load(&s->channels[c].clips[sc].playing)) { quiet = false; break; }
    }
    int r = pwb_graph_sync(n, g_names, routes, &m, quiet);
    if (r == 1) {
        for (int i = 0; i < n; ++i) {
            if (routes[i].l != session_route_get(s, i, 0)) session_route_set(s, i, 0, routes[i].l);
            if (routes[i].r != session_route_get(s, i, 1)) session_route_set(s, i, 1, routes[i].r);
        }
        if (m.l != m0.l) session_master_route_set(s, 0, m.l);
        if (m.r != m0.r) session_master_route_set(s, 1, m.r);
    }
    /* the card may change its number of inputs (another profile) -> the routing menu must know */
    static double t_hw = 0;
    double now = mono_s();
    if (now - t_hw > 1.0) {
        t_hw = now;
        int hw = pwb_graph_hw_ports();
        if (hw > MAX_OUTPUT_CHANNELS) hw = MAX_OUTPUT_CHANNELS;
        if (hw >= 2 && hw != g_config.output_channels) {
            printf(">>> PipeWire graph: output now has %d inputs\n", hw);
            g_config.output_channels = hw;
        }
    }
    return r == 1;
}
