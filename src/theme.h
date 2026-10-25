/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef THEME_H
#define THEME_H

#include "sdlgfx.h"
#include <stdbool.h>
#include <stddef.h>

// ============ Theme struct ============

typedef struct {
    // Basics
    Color bg;
    Color panel_bg;
    Color panel_bg2;
    Color text;
    Color text_dim;
    Color selection;

    // Clips
    Color cell_empty;
    Color cell_hover;
    Color note_bg;
    Color note_border;
    Color note_text;

    // Controls
    Color mute;
    Color solo;
    Color stop_btn;
    Color btn_add;
    Color btn_add_hover;

    // Scroll
    Color scroll_bg;
    Color scroll_fg;

    // VU meters
    Color vu_green;
    Color vu_yellow;
    Color vu_red;

    // Grid / ruler
    Color grid_line;
    Color ruler_bg;
    Color ruler_line;
    Color ruler_tick;

    // Waveform
    Color wave_bg;
    Color wave_fg;
} Theme;

// ============ Global ============

extern Theme g_theme;

// Font size (8 – 32)
extern int   g_font_size;

// Name of the active theme
extern char  g_theme_name[32];

// Name of the accent
extern char  g_accent_name[32];

// ============ Presets ============

typedef enum {
    THEME_DARK = 0,
    THEME_LIGHT,
    THEME_MIDNIGHT,
    THEME_SOLARIZED,
    THEME_OLED,         /* step 18: true black (for OLED screens) */
    THEME_NORD,         /* step 18: cool grey-blue (the Nord palette) */
    THEME_GRUVBOX,      /* step 18: warm retro (the Gruvbox palette) */
    THEME_CUSTOM,
    THEME_COUNT
} ThemePreset;

typedef enum {
    ACCENT_YELLOW = 0,
    ACCENT_BLUE,
    ACCENT_GREEN,
    ACCENT_RED,
    ACCENT_PURPLE,
    ACCENT_ORANGE,
    ACCENT_COUNT
} AccentPreset;

// Applies a preset to g_theme
void theme_apply_preset(ThemePreset preset);

// Applies an accent colour
void theme_apply_accent(AccentPreset accent);

// ============ Save / Load ============

// Path: ~/.config/qlim/theme.conf
void theme_get_config_path(char *out, size_t size);

// Loads the theme from the config (if it does not exist → Dark)
void theme_load(void);

// Saves the theme to the config
void theme_save(void);

// ============ Helpers ============

// Returns the preset's name
const char *theme_preset_name(ThemePreset preset);

// Returns the accent's name
const char *theme_accent_name(AccentPreset accent);

// Returns the preset's index by name
ThemePreset theme_preset_from_name(const char *name);

// Returns the accent's index by name
AccentPreset theme_accent_from_name(const char *name);

#endif
