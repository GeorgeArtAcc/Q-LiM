/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.2: controller buttons for the whole program (any MIDI controller): move the selection with buttons or a jog
 * wheel, launch / stop what is selected, stop everything, mute the master. Learned in MIDI Settings (or with MIDI
 * learn on the big stop button / the master MUTE) and kept in ~/.config/qlim/controls.conf (not in the project). */
#ifndef QLIM_MIDIMAP_H
#define QLIM_MIDIMAP_H
#include <stdbool.h>

typedef enum {
    MM_UP = 0, MM_DOWN, MM_LEFT, MM_RIGHT,     /* buttons: move the selection one step */
    MM_WHEEL_V,                                 /* a jog wheel / endless knob: up and down (CC, relative) */
    MM_WHEEL_H,                                 /* a second one: left and right */
    MM_LAUNCH,                                  /* play the selected clip (a scene: the whole scene) - like Enter */
    MM_STOP,                                    /* stop the selected clip / scene */
    MM_STOP_ALL,                                /* the big stop button */
    MM_MASTER_MUTE,                             /* the master MUTE */
    MM_SCENE_NEXT,                              /* 1.4.17: the next scene with the transition (fade out / fade in) */
    MM_SCENE_BACK,                              /* 1.4.17: back to the scene before, with the transition */
    MM_AUTO_STOP,                               /* 1.4.17: stop the automations (press again: they go on) */
    MM_FADE_TIME,                               /* 1.4.17: a knob / wheel: the transition time */
    MM_COUNT
} MidiMapAction;

/* 1.4.8: pads -> a box of clips (like the "session ring" in Ableton): MPAD_W channels x MPAD_H scenes from the
 * selected cell; pad i launches the clip in row i / w, column i % w of the box. The handler gets MM_PAD, delta = i. */
#define MM_PAD      100
#define MM_PAD_MAX  8          /* at most 8 x 8 */

/* what happens: action + direction (for the moves: -1 / +1; the wheels: steps, sign = direction) */
typedef void (*MidiMapHandler)(int action, int delta, void *user);

void        midimap_set_handler(MidiMapHandler fn, void *user);
const char *midimap_label(int action);            /* "Select up" ... */
bool        midimap_is_wheel(int action);
int         midimap_get(int action);              /* MIDI_BIND_MAKE code, 0 = none */
void        midimap_set(int action, int code);    /* 0 = clear; saved at once */
void        midimap_describe(int action, char *out, int n);   /* "CC 46", "Note 36", "-" */

int         midimap_wheel_ticks(void);            /* jog wheel clicks per step (1..32) */
void        midimap_set_wheel_ticks(int n);
void        midimap_learn(int action);            /* the next MIDI message is bound to it (-1 = cancel) */
int         midimap_learning(void);               /* the action waiting, or -1 */

/* the pad box: size (1..8 each way), the learned pads (in box order), learning them one after the other */
void        midimap_pad_box(int *w, int *h);
void        midimap_set_pad_box(int w, int h);
int         midimap_pad_count(void);              /* pads learned (0 = no box) */
bool        midimap_pads_active(void);            /* a box of more than one cell with its pads learned */
void        midimap_pads_learn(bool on);          /* on: the next w*h pads pressed become pad 1, 2, ... */
int         midimap_pads_learning(void);          /* -1, or the index of the pad waiting */
void        midimap_pads_clear(void);
/* the notes bound to controller buttons / pads (they must not play the instrument); notes 0..127 as 4 x 32 bits */
void        midimap_note_mask(unsigned out[4]);

void        midimap_load(void);
/* kind = MIDI_BIND_NOTE / MIDI_BIND_CC; true = it was for a controller button (do not use it for anything else) */
bool        midimap_handle(int kind, int number, int value);

/* relative CC of a jog wheel -> steps (both the "1 / 127" and the "65 / 63" styles; 0 = no move) */
int         midimap_wheel_delta(int value);
#endif
