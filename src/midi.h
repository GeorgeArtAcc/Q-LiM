/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef MIDI_H
#define MIDI_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Callback called when an MIDI CC message is received.
// cc = CC number (0-127), value = value (0-127).
// channel = MIDI channel (0-15).
typedef void (*MidiCallback)(int cc, int value, int channel, void *user_data);

// Callback for Note On / Note Off (pads, keys).
// note = 0-127, velocity = 0-127, on = true for Note On with velocity > 0, channel = MIDI channel (0-15).
// Called from midi_poll() (i.e. from the main thread, together with MidiCallback).
typedef void (*MidiNoteCallback)(int note, int velocity, bool on, int channel, void *user_data);

// === Lifecycle ===

int midi_init(void);
void midi_shutdown(void);

// === Port list ===

// Returns the number of available MIDI inputs.
int midi_get_port_count(void);

// Returns the name of a MIDI input by index (or "?").
const char *midi_get_port_name(int index);

// === Connection ===
// 1.4.6: several MIDI inputs at the same time (a keyboard, a controller, the sound card's MIDI in ...), up to
// MIDI_MAX_OPEN. Every open input has its uses: PLAY = its notes / CCs go to the instruments, CONTROL = they drive
// the program (MIDI learn, faders, pads, controller buttons). Default: both.
#define MIDI_MAX_OPEN    32
#define MIDI_USE_PLAY    1
#define MIDI_USE_CONTROL 2

// Opens (adds) a MIDI input by index; the ones already open stay open. Returns 0 on success (also if it was open).
int midi_open_port(int index);

// Opens a MIDI input by name (the same device, without the ALSA client numbers; or a part of the name).
int midi_open_port_by_name(const char *name);

// Closes one input / every input.
void midi_close_port_index(int index);
void midi_close_port(void);

bool midi_port_is_open(int index);
int  midi_port_uses(int index);                  // MIDI_USE_* of an open input (0 = not open)
void midi_set_port_uses(int index, int uses);    // saved at once

int  midi_open_count(void);

// Inputs remembered in midi.conf that are not plugged in now (shown in the dialog; they connect by themselves).
int         midi_missing_count(void);
const char *midi_missing_name(int i);
void        midi_forget_missing(int i);

// Looks at the ports again: connects remembered devices that were plugged in, reconnects the ones that came back.
// Called by midi_poll() every 2 seconds too.
void midi_refresh(void);

// Old (one input) API - the first open input.
const char *midi_get_current_port_name(void);
int midi_get_current_port_index(void);
bool midi_is_open(void);                         // at least one input is open

// === Callback ===

void midi_set_callback(MidiCallback cb, void *user_data);
void midi_set_note_callback(MidiNoteCallback cb, void *user_data);

// === Learn mode (for the UI) ===

void midi_set_learn_mode(bool on);
bool midi_get_learn_mode(void);

// === Config save/load ===

// Path: ~/.config/qlim/midi.conf
void midi_get_config_path(char *out, size_t size);

// Saves the open inputs (and their uses) + the remembered ones that are not plugged in.
void midi_save_config(void);

// Loads the saved inputs and opens the ones that are there.
// Returns 0 if at least one was opened, != 0 if none was saved or found.
int midi_load_config(void);

// === Poll ===

void midi_poll(void);

#ifdef __cplusplus
}
#endif

#endif
