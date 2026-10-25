/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef DROPFILES_H
#define DROPFILES_H

#include "session.h"
#include <stddef.h>

/*
 * Files/folders dropped onto the window from outside (drag & drop).
 *   - file with an audio extension -> clip; folder -> the audio files in it (alphabetically, no subfolders)
 *   - the first file goes into the cell under the cursor (overwrites it),
 *     the following ones into the next EMPTY scenes of the same channel (scenes are added if there are not enough)
 *   - if the cursor is not over a cell: the selected cell, otherwise the first empty one of the first channel
 * Returns how many clips were loaded; msg receives a short message for the user.
 */
int dropfiles_load(Session *s, const char *const *paths, unsigned count,
                   int hov_ch, int hov_scene, int sel_ch, int sel_scene,
                   char *msg, size_t msg_size);

/* 1 if the extension is an audio format the program can read */
int dropfiles_is_audio(const char *path);

#endif
