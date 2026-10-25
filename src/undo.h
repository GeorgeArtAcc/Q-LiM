/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef UNDO_H
#define UNDO_H

#include "session.h"
#include <stdbool.h>

/*
 * Undo / Redo and detecting "there are unsaved changes".
 *
 * How it works (simply): every few moments, when something was touched and the mouse is released,
 * a "snapshot" of the project is taken (text in memory). If the snapshot differs from the previous one,
 * the previous one goes onto the Undo stack. Undo brings back the old snapshot, touching ONLY the cells
 * that changed — so clips that are playing and not affected do not stop.
 *
 * The live things are NOT in the "undo snapshot": volume, pan, Mute, Solo and the Master volume.
 * (Otherwise every nudge of a fader from a MIDI controller would fill the history.)
 * They are only part of the "are there unsaved changes" check — so you are asked on close.
 */

void undo_reset(Session *s);                   // new/loaded project: new history, "no changes"
void undo_mark_saved(Session *s);              // just saved
void undo_poke(double now);                    // "something was touched" (key, mouse, MIDI, drop...)
void undo_update(Session *s, double now, bool mouse_down);   // called every frame
void undo_commit_now(Session *s);              // force a snapshot (before undo/close)

bool undo_can_undo(void);
bool undo_can_redo(void);
bool undo_undo(Session *s);
bool undo_redo(Session *s);
bool undo_is_dirty(void);
void undo_mark_plugin_changed(void);          // step 35: touched in a plugin window                      // valid after undo_update/undo_commit_now

void undo_shutdown(void);

// For tests: returns whether the pointers point to things on the stack
int  undo_depth(void);
int  undo_redo_depth(void);

#endif
