/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef SETTINGS_H
#define SETTINGS_H

#include "audio.h"
#include <stdbool.h>

// ============ Audio Settings (persist) ============

// Returns the path to the config file: ~/.config/qlim/audio.conf
void settings_get_config_path(char *out_path, size_t size);

// Loads the audio settings from the config file.
// If the file does not exist → fills config with defaults.
// Returns 0 on success (or if the file is missing — defaults),
// != 0 on a read error.
int settings_load_audio(AudioConfig *config);

// Saves the audio settings to the config file.
// Returns 0 on success.
int settings_save_audio(const AudioConfig *config);

int  settings_ensure_dir(const char *path);   /* mkdir -p (0 = ok) */
/* 1.4.11: loudness normalization (Edit -> Loudness...): ~/.config/qlim/loudness.conf. target 0 = off */
void settings_load_loudness(int *target_lufs, bool *down_only);
void settings_save_loudness(int target_lufs, bool down_only);
/* 1.4.12: the folder the library (L) opens in: ~/.config/qlim/library.conf. false = none saved */
bool settings_load_library_dir(char *out, size_t n);
void settings_save_library_dir(const char *dir);

#endif
