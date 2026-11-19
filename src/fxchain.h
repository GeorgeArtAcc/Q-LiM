/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef FXCHAIN_H
#define FXCHAIN_H
/*
 * The plugin chain of one channel (step 34).
 * The main thread (fxchain_sync, every frame) compares the list ch->fx[] with the loaded chain and, if they differ,
 * loads a new one (plugins already loaded are reused - they keep their state) and swaps it in atomically.
 * The old one is freed only once the audio thread is certainly not using it.
 * The mixer (audio thread) only reads ch->fxchain and calls qplug_process for every plugin.
 */
#include "session.h"
#include "plughost.h"

#define FXC_SIG_LEN 4800

struct FxChain {
    int    n;                         /* how many plugins are in the chain (bypassed ones too - they stay loaded) */
    int    nactive;                   /* how many of them are running */
    QPlug *p[MAX_CH_FX];
    bool   byp[MAX_CH_FX];            /* bypassed: loaded, but the audio goes around it */
    bool   inst[MAX_CH_FX];           /* 1.2: an instrument (takes MIDI; no audio input or the scan says "instrument") */
    /* main thread only: */
    int    slot[MAX_CH_FX];           /* which row of ch->fx[] p[i] belongs to */
    char   id[MAX_CH_FX][520];
    char   fmt[MAX_CH_FX][8];
    bool   moved[MAX_CH_FX];          /* p[i] was moved into a new chain (p[]/n are NOT touched - the audio may be reading) */
    unsigned fail_mask;
    unsigned loading_mask;            /* the rows still starting up in the background (Windows plugins) */               /* the rows of ch->fx[] that could not be loaded */
    char   sig[FXC_SIG_LEN];
    bool   claimed;
};
typedef struct FxChain FxChain;

void fxchain_sync(Session *s);                    /* main thread, every frame */
bool fxchain_slot_failed(Session *s, int ch, int k);
bool fxchain_slot_loading(Session *s, int ch, int k);   /* step 35d: still starting up (Windows plugin) */
void fxchain_shutdown(Session *s);
bool fxchain_has_instrument(Session *s, int ch);
bool fxchain_slot_is_instrument(Session *s, int ch, int k);   /* 1.2: row k of the channel is a loaded instrument */  /* 1.2: a loaded plugin of the channel is an instrument (no audio input, takes MIDI) */                /* on exit (after the audio is stopped) */

/* step 35 */
QPlug *fxchain_find(Session *s, int ch, int k);   /* the loaded plugin on row k of the channel (or NULL) */
/* the settings of row k for saving into the project: from the live plugin, otherwise the ones saved from the project. malloc; 0 = present */
int   fxchain_state_for_save(Session *s, int ch, int k, void **data, size_t *len);
void  fxchain_keep_states(Session *s);   /* 2.0.0: before plugins move into / out of a separate process */

/* 1.4.14: MIDI Learn of plugin parameters (main thread; the bindings themselves are in ChFx, see session.h).
 * learn: param -1 = the next parameter that is moved (in the plugin's window or on the panel) becomes the target. */
void fxchain_learn(Session *s, int ch, int k, int param);
void fxchain_learn_cancel(void);
bool fxchain_learning(int *ch, int *k, int *param);
void fxchain_learn_poll(Session *s);                       /* every frame (fxchain_sync does it) */
bool fxchain_learn_cc(Session *s, int cc, char *msg, int msz);   /* a CC while learning: bound (true = taken) */
void fxchain_midi_cc(Session *s, int cc, int value);        /* a CC: moves every parameter bound to it */

/* store for settings from the project (the number is kept in ChFx.state_ref) */
int         fxstate_new(void);
void        fxstate_append(int ref, const void *data, size_t len);
const void *fxstate_get(int ref, size_t *len);

#endif
