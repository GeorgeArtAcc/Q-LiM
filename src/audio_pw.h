/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef AUDIO_PW_H
#define AUDIO_PW_H
/*
 * Direct (native) PipeWire output - without PortAudio's ALSA/JACK layers.
 *
 * libpipewire-0.3.so.0 is loaded DYNAMICALLY (dlopen) when needed:
 *   - compiling does NOT need libpipewire-0.3-dev (the headers are in third_party/pipewire);
 *   - if the machine has no PipeWire, the program works as before (PortAudio only).
 * The module knows nothing about the mixer: it is given a function that fills the buffer (tested separately).
 */
#include <stdbool.h>
#include <stdint.h>

#define PWB_MAX_CH    64          /* the most outputs we ask PipeWire for */
#define PWB_MAX_SINKS 48

typedef struct {
    uint32_t id;                  /* the object's id in PipeWire (changes every time it is plugged in) */
    char     name[256];           /* node.name - the permanent name (saved in audio.conf) */
    char     desc[160];           /* node.description - for humans */
    int      channels;            /* number of outputs (0 = not known yet) */
    uint32_t pos[PWB_MAX_CH];     /* SPA_AUDIO_CHANNEL_* for every output */
    int      rate;                /* 0 = unknown */
    bool     is_default;          /* this is the system's default output */
} PwbSink;

/* Fills out with `frames` frames x ch channels (interleaved float32). Called on PipeWire's real-time thread. */
typedef void (*PwbRenderFn)(float *out, uint32_t frames, int ch, void *user);

typedef struct {
    int      state;               /* 0 = closed, 1 = connecting, 2 = paused (not connected to an output), 3 = playing, -1 = error */
    uint32_t graph_rate;          /* the rate the PipeWire graph runs at (0 = unknown) */
    uint32_t quantum;             /* how many frames per cycle the graph wants */
    uint32_t last_frames;         /* how many frames it last asked us for */
    uint64_t callbacks;
    uint64_t xruns;               /* discontinuities in the graph position / missing buffer */
    bool     lost;                /* the output disappeared / PipeWire stopped */
    char     why[128];
    char     node_desc[160];      /* what we are connected to (if known) */
} PwbStatus;

/* true = libpipewire loads and the PipeWire server answers (the result is remembered for a few seconds). */
bool pwb_available(void);
/* Forces a new check (Refresh button). */
void pwb_forget_availability(void);

/* List of the outputs (Audio/Sink). Returns the count. Connects briefly if no stream is open. */
int  pwb_list_sinks(PwbSink *out, int max);

/* Opens the output. target = node.name or "" for the system default.
 * channels <= 0 -> as many as the output has. Returns 0 on success. err receives the reason on error. */
int  pwb_open(const char *target, int rate, int quantum, int channels,
              PwbRenderFn fn, void *user, int *out_channels, char *err, int err_size);
void pwb_close(void);
bool pwb_is_open(void);
void pwb_get_status(PwbStatus *st);

/* ================= Graph mode (step 28) =================
 * Instead of one stream: two nodes in the PipeWire graph, visible in Patchance / qpwgraph / Helvum:
 *   "Q-LiM"         - 2 outputs per channel ("Drums L", "Drums R" ...), after volume/pan/mute;
 *   "Q-LiM Master"  - inputs "In L/R" (PipeWire itself sums everything connected to them) and outputs "Out L/R".
 * The routing (Routing in the program) = the links in the graph, in both directions:
 *   a change in the program -> links are made/removed; a link made/removed in Patchance -> the routing changes.
 * Channel mask: bit 0/1 = Master In L/R, bit 2+k = input k of the card. Master mask: bit k = input k of the card. */
typedef void (*PwbChannelsFn)(float *const *outs, int nouts, uint32_t frames, void *user);   /* outs[k] may be NULL */
typedef void (*PwbMasterFn)(const float *in_l, const float *in_r, float *out_l, float *out_r, uint32_t frames, void *user);
typedef struct { uint64_t l, r; } PwbRoute;

/* true = this libpipewire supports graph mode (pw_filter) */
bool pwb_graph_supported(void);
/* Opens graph mode towards output target ("" = the system one). nch channels with names names[i].
 * out_hw_ports = how many inputs the card has (for the routing menu). 0 = success. */
int  pwb_graph_open(const char *target, int rate, int quantum, int nch, const char *const *names,
                    PwbChannelsFn chfn, PwbMasterFn mfn, void *user, int *out_hw_ports, char *err, int err_size);
bool pwb_graph_is_open(void);
/* Called often from the main thread (every frame). Passes the current channels/names/routes; if someone in the graph
 * connected/disconnected one of our ports, routes/master change and it returns 1. 0 = nothing from outside; -1 = the graph is not open. */
/* quiet = nothing is playing (then the channels' node may be recreated because of a new name) */
int  pwb_graph_sync(int nch, const char *const *names, PwbRoute *routes, PwbRoute *master, bool quiet);
/* how many inputs the card has right now (may change if the card switches profile); 0 = unknown */
int  pwb_graph_hw_ports(void);

/* version of the loaded library ("" if not loaded) */
const char *pwb_library_version(void);

#endif
