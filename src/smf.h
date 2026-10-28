/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef SMF_H
#define SMF_H
/* 1.3: a Standard MIDI File (.mid) -> the notes of a MIDI clip (all tracks merged, times in beats). */
#include "session.h"
#include <stdbool.h>

MidiSeq *smf_load(const char *path, char *err, int esz);   /* malloc; NULL + err on error */
bool     smf_is_midi_path(const char *path);              /* .mid / .midi / .smf / .kar */

#endif
