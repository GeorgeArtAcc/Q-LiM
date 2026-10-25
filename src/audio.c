/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _POSIX_C_SOURCE 200809L
#include "audio.h"
#include "mixer.h"
#include "stream.h"
#include "dsp_meter.h"
#include "devwatch.h"
#include "audio_pw.h"
#include <stdatomic.h>
#include <time.h>
#include <portaudio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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

static int pa_callback(const void *input, void *output,
                       unsigned long frame_count,
                       const PaStreamCallbackTimeInfo *time_info,
                       PaStreamCallbackFlags status_flags,
                       void *user_data)
{
    (void)input; (void)time_info; (void)user_data;
    atomic_fetch_add_explicit(&g_cb_count, 1u, memory_order_relaxed);
    if (status_flags & (paOutputUnderflow | paOutputOverflow)) dsp_meter_xrun();
    uint64_t t0 = dsp_meter_now_ns();
    mixer_render(g_session, (float *)output, frame_count, atomic_load_explicit(&g_cb_ch, memory_order_acquire));
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

static PaDeviceIndex find_device_by_name(const char *needle)
{
    int n = Pa_GetDeviceCount();
#ifdef _WIN32
    PaDeviceIndex best = paNoDevice;
    int best_rank = 99;
    for (int i = 0; i < n; ++i) {
        const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
        if (info && info->maxOutputChannels > 0 && strstr(info->name, needle)) {
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
static PaDeviceIndex default_output_device(void)
{
#ifdef _WIN32
    PaHostApiIndex w = Pa_HostApiTypeIdToHostApiIndex(paWASAPI);
    if (w >= 0) {
        const PaHostApiInfo *h = Pa_GetHostApiInfo(w);
        if (h && h->defaultOutputDevice != paNoDevice) return h->defaultOutputDevice;
    }
    PaHostApiIndex d = Pa_HostApiTypeIdToHostApiIndex(paDirectSound);
    if (d >= 0) {
        const PaHostApiInfo *h = Pa_GetHostApiInfo(d);
        if (h && h->defaultOutputDevice != paNoDevice) return h->defaultOutputDevice;
    }
#endif
    return Pa_GetDefaultOutputDevice();
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
static void pw_render(float *out, uint32_t frames, int ch, void *user)
{
    (void)user;
    atomic_fetch_add_explicit(&g_cb_count, 1u, memory_order_relaxed);
    uint64_t t0 = dsp_meter_now_ns();
    mixer_render(g_session, out, frames, ch);
    dsp_meter_end(t0, frames, g_config.sample_rate);
}

/* graph mode: the channels (PipeWire's RT thread) */
static void pwg_channels(float *const *outs, int nouts, uint32_t frames, void *user)
{
    (void)user;
    atomic_fetch_add_explicit(&g_cb_count, 1u, memory_order_relaxed);
    uint64_t t0 = dsp_meter_now_ns();
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
                out_params.suggestedLatency = dev->defaultLowOutputLatency;
                out_params.hostApiSpecificStreamInfo = NULL;

                atomic_store(&g_cb_ch, use_ch);
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
        return 1;
    }

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
    if (g_use_pw) {
        pwb_close();
        g_use_pw = false;
        g_use_graph = false;
    }
    pwb_forget_availability();               /* the next "is there PipeWire" query - from scratch */
    if (g_stream) {
        /* with a lost card Stop may hang -> Abort is safe */
        if (atomic_load(&g_lost)) Pa_AbortStream(g_stream); else Pa_StopStream(g_stream);
        Pa_CloseStream(g_stream);
        g_stream = NULL;
    }
    if (g_session) atomic_store(&g_session->audio_running, false);
    atomic_store(&g_lost, false);
    Pa_Terminate();
    printf("Audio shut down.\n");
}

#ifdef _WIN32
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
const char *audio_lost_name(void) { return g_lost_name; }

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
