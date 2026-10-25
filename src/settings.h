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

#endif
