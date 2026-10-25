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

// Opens a MIDI input by index. Returns 0 on success.
int midi_open_port(int index);

// Opens a MIDI input by name (substring search). Returns 0 on success.
int midi_open_port_by_name(const char *name);

// Closes the current port (without stopping the midi system).
void midi_close_port(void);

// Returns the name of the currently open port (or "" if none).
const char *midi_get_current_port_name(void);

// Returns the index of the currently open port (-1 if none).
int midi_get_current_port_index(void);

// Checks whether a port is open.
bool midi_is_open(void);

// === Callback ===

void midi_set_callback(MidiCallback cb, void *user_data);
void midi_set_note_callback(MidiNoteCallback cb, void *user_data);

// === Learn mode (for the UI) ===

void midi_set_learn_mode(bool on);
bool midi_get_learn_mode(void);

// === Config save/load ===

// Path: ~/.config/qlim/midi.conf
void midi_get_config_path(char *out, size_t size);

// Saves the name of the currently open port (or nothing if none).
void midi_save_config(void);

// Loads the last saved port and tries to open it.
// Returns 0 on success, != 0 if there is no saved port or it was not found.
int midi_load_config(void);

// === Poll ===

void midi_poll(void);

#ifdef __cplusplus
}
#endif

#endif
