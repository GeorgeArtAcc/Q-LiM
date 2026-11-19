/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef QLIM_OUTX_H
#define QLIM_OUTX_H
/*
 * 1.4.21: more than one sound card at the same time + a delay for every output.
 *
 * The main card (Edit -> Audio) keeps outputs 1..N. Every extra card adds its outputs after them (N+1 ...), so the
 * Routing matrix, the master, the surround - everything simply sees more outputs. The mixer renders all of them in the
 * main card's callback; the outputs of an extra card go into a ring, and the extra card's own callback takes them out.
 *
 * Two cards never run on exactly the same clock (and Bluetooth even less): the extra card reads the ring through a
 * resampler whose ratio a slow controller nudges (+-0.5 % at most) so the ring stays at its target fill - no drift,
 * no clicks, no growing delay. The same on every system (PipeWire, ALSA, JACK, Windows): nothing is left to the driver.
 * Resampler: windowed sinc (Kaiser), 32 taps, 256 phases with linear interpolation (~ -80 dB images), also converts
 * between different rates (e.g. 48000 -> 44100).
 *
 * Delay: every output (main or extra) can be delayed by hand, 0 ... 2000 ms (e.g. the main speakers wait for a
 * Bluetooth speaker). A change glides over 10 ms (no click).
 *
 * Light: with no extra card and no delay the audio path is exactly as before (one test of a flag per callback).
 * The settings are the computer's (they belong to its sound cards, not to a project): ~/.config/qlim/outputs.conf.
 */
#include "session.h"
#include <stdbool.h>
#include <stdint.h>

#define OUTX_MAX          8           /* extra cards */
#define OUTX_DELAY_MAX_MS 2000.0f

enum { OUTX_KIND_PA = 0, OUTX_KIND_PW = 1 };   /* PortAudio device (ALSA / JACK / Windows) or a PipeWire output */

typedef struct {
    int  kind;
    int  channels;                  /* how many of its outputs are used (they get numbers after the main card's) */
    char api[64];                   /* PortAudio: the driver's name ("ALSA", "Windows WASAPI" ...) */
    char name[256];                 /* PortAudio: the device's name; PipeWire: node.name */
    char desc[160];                 /* what the user sees */
} OutxDevCfg;

/* ---- the configuration (main thread) ---- */
void  outx_load(void);                           /* reads outputs.conf (once at startup) */
void  outx_save(void);
int   outx_cfg_count(void);
const OutxDevCfg *outx_cfg_get(int i);
int   outx_cfg_add(const OutxDevCfg *c);         /* -> its index, -1 = full */
void  outx_cfg_remove(int i);
float outx_delay_get(int out);                   /* ms */
void  outx_delay_set(int out, float ms);         /* live (also while playing) */
bool  outx_any_delay(void);
/* the numbering: the extra card i starts at output `first` (0 = output 1); -1 = past the 62 outputs */
int   outx_first(int i, int main_ch);
int   outx_total(int main_ch);                   /* main + extra outputs (at most MAX_HW_OUT) */

/* ---- the engine (main thread) ---- */
void  outx_engine_start(int main_rate, int main_ch, bool enabled);   /* after the main card started */
void  outx_engine_stop(void);                    /* after the main card stopped */
/* an extra card was opened: dev_rate = the rate its stream runs at, dev_ch = its stream's channels */
void  outx_slot_attach(int i, int dev_rate, int dev_ch);
/* before (wait = false) and after (wait = true) its stream is closed: stops it being fed, then frees its ring */
void  outx_slot_detach(int i);
void  outx_slot_free(int i);
typedef struct {
    bool     attached;
    bool     running;               /* audio is flowing (the ring filled, the card takes it) */
    double   latency_ms;            /* what the ring adds (the card's own delay comes on top) */
    double   ppm;                   /* the clock difference the controller found */
    unsigned underruns, overruns;
    uint64_t pulls;                 /* the card's callbacks (to see if it stopped) */
} OutxStat;
void  outx_slot_stat(int i, OutxStat *st);

/* ---- the real-time side ---- */
/* the main card's callback: instead of mixer_render (renders the extra outputs too, applies the delays) */
void  outx_render(Session *s, float *out, unsigned frames, int main_ch);
/* an extra card's callback: fills `frames` interleaved frames of dev_ch channels */
void  outx_pull(int i, float *out, unsigned frames, int dev_ch);

/* tests: render through a function instead of mixer_render */
typedef void (*OutxRenderFn)(float *out, unsigned frames, int nch, void *user);
void  outx_test_set_render(OutxRenderFn fn, void *user);
void  outx_test_set_clock(double (*fn)(void));
#endif
