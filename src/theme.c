/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _POSIX_C_SOURCE 200809L   /* fileno, fsync (needed on musl/Alpine) */
#include "theme.h"
#include "safefile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

Theme g_theme;
int   g_font_size = 14;
char  g_theme_name[32]  = "dark";
char  g_accent_name[32] = "yellow";

// ============ Preset themes ============

static void apply_dark(void)
{
    g_theme.bg           = (Color){ 22, 22, 26, 255 };
    g_theme.panel_bg     = (Color){ 34, 34, 40, 255 };
    g_theme.panel_bg2    = (Color){ 28, 28, 34, 255 };
    g_theme.text         = (Color){ 220, 220, 228, 255 };
    g_theme.text_dim     = (Color){ 130, 130, 145, 255 };
    g_theme.selection    = (Color){ 255, 230, 90, 255 };

    g_theme.cell_empty   = (Color){ 42, 42, 50, 255 };
    g_theme.cell_hover   = (Color){ 58, 58, 70, 255 };
    g_theme.note_bg      = (Color){ 245, 225, 110, 255 };
    g_theme.note_border  = (Color){ 180, 160, 60, 255 };
    g_theme.note_text    = (Color){ 40, 35, 10, 255 };

    g_theme.mute         = (Color){ 210, 70, 70, 255 };
    g_theme.solo         = (Color){ 240, 210, 80, 255 };
    g_theme.stop_btn     = (Color){ 200, 70, 70, 255 };
    g_theme.btn_add      = (Color){ 55, 65, 80, 255 };
    g_theme.btn_add_hover = (Color){ 85, 105, 130, 255 };

    g_theme.scroll_bg    = (Color){ 18, 18, 22, 255 };
    g_theme.scroll_fg    = (Color){ 100, 100, 115, 255 };

    g_theme.vu_green     = (Color){ 70, 200, 100, 255 };
    g_theme.vu_yellow    = (Color){ 230, 200, 70, 255 };
    g_theme.vu_red       = (Color){ 230, 70, 70, 255 };

    g_theme.grid_line    = (Color){ 60, 60, 75, 255 };
    g_theme.ruler_bg     = (Color){ 22, 22, 28, 255 };
    g_theme.ruler_line   = (Color){ 60, 60, 75, 255 };
    g_theme.ruler_tick   = (Color){ 50, 50, 62, 255 };

    g_theme.wave_bg      = (Color){ 16, 16, 20, 255 };
    g_theme.wave_fg      = (Color){ 100, 200, 245, 255 };

    strcpy(g_theme_name, "dark");
}

static void apply_light(void)
{
    g_theme.bg           = (Color){ 240, 240, 245, 255 };
    g_theme.panel_bg     = (Color){ 220, 220, 228, 255 };
    g_theme.panel_bg2    = (Color){ 230, 230, 236, 255 };
    g_theme.text         = (Color){ 30, 30, 40, 255 };
    g_theme.text_dim     = (Color){ 120, 120, 130, 255 };
    g_theme.selection    = (Color){ 200, 100, 30, 255 };

    g_theme.cell_empty   = (Color){ 210, 210, 218, 255 };
    g_theme.cell_hover   = (Color){ 190, 190, 205, 255 };
    g_theme.note_bg      = (Color){ 255, 230, 100, 255 };
    g_theme.note_border  = (Color){ 200, 170, 40, 255 };
    g_theme.note_text    = (Color){ 60, 40, 10, 255 };

    g_theme.mute         = (Color){ 200, 60, 60, 255 };
    g_theme.solo         = (Color){ 230, 180, 50, 255 };
    g_theme.stop_btn     = (Color){ 200, 60, 60, 255 };
    g_theme.btn_add      = (Color){ 180, 190, 210, 255 };
    g_theme.btn_add_hover = (Color){ 140, 170, 200, 255 };

    g_theme.scroll_bg    = (Color){ 210, 210, 220, 255 };
    g_theme.scroll_fg    = (Color){ 130, 130, 145, 255 };

    g_theme.vu_green     = (Color){ 40, 180, 80, 255 };
    g_theme.vu_yellow    = (Color){ 210, 180, 40, 255 };
    g_theme.vu_red       = (Color){ 210, 50, 50, 255 };

    g_theme.grid_line    = (Color){ 180, 180, 190, 255 };
    g_theme.ruler_bg     = (Color){ 210, 210, 218, 255 };
    g_theme.ruler_line   = (Color){ 150, 150, 160, 255 };
    g_theme.ruler_tick   = (Color){ 170, 170, 180, 255 };

    g_theme.wave_bg      = (Color){ 245, 245, 250, 255 };
    g_theme.wave_fg      = (Color){ 50, 100, 180, 255 };

    strcpy(g_theme_name, "light");
}

static void apply_midnight(void)
{
    g_theme.bg           = (Color){ 10, 12, 20, 255 };
    g_theme.panel_bg     = (Color){ 20, 24, 38, 255 };
    g_theme.panel_bg2    = (Color){ 15, 18, 30, 255 };
    g_theme.text         = (Color){ 200, 210, 230, 255 };
    g_theme.text_dim     = (Color){ 110, 120, 150, 255 };
    g_theme.selection    = (Color){ 80, 180, 255, 255 };

    g_theme.cell_empty   = (Color){ 25, 30, 45, 255 };
    g_theme.cell_hover   = (Color){ 40, 50, 75, 255 };
    g_theme.note_bg      = (Color){ 180, 200, 255, 255 };
    g_theme.note_border  = (Color){ 100, 130, 200, 255 };
    g_theme.note_text    = (Color){ 20, 30, 60, 255 };

    g_theme.mute         = (Color){ 230, 80, 100, 255 };
    g_theme.solo         = (Color){ 230, 200, 90, 255 };
    g_theme.stop_btn     = (Color){ 200, 60, 70, 255 };
    g_theme.btn_add      = (Color){ 40, 55, 80, 255 };
    g_theme.btn_add_hover = (Color){ 70, 100, 140, 255 };

    g_theme.scroll_bg    = (Color){ 8, 10, 16, 255 };
    g_theme.scroll_fg    = (Color){ 80, 90, 120, 255 };

    g_theme.vu_green     = (Color){ 80, 220, 130, 255 };
    g_theme.vu_yellow    = (Color){ 240, 210, 90, 255 };
    g_theme.vu_red       = (Color){ 240, 80, 80, 255 };

    g_theme.grid_line    = (Color){ 40, 50, 75, 255 };
    g_theme.ruler_bg     = (Color){ 15, 18, 30, 255 };
    g_theme.ruler_line   = (Color){ 50, 60, 90, 255 };
    g_theme.ruler_tick   = (Color){ 40, 50, 75, 255 };

    g_theme.wave_bg      = (Color){ 5, 8, 15, 255 };
    g_theme.wave_fg      = (Color){ 120, 200, 255, 255 };

    strcpy(g_theme_name, "midnight");
}

static void apply_solarized(void)
{
    g_theme.bg           = (Color){ 0, 43, 54, 255 };
    g_theme.panel_bg     = (Color){ 7, 54, 66, 255 };
    g_theme.panel_bg2    = (Color){ 0, 55, 70, 255 };
    g_theme.text         = (Color){ 147, 161, 161, 255 };
    g_theme.text_dim     = (Color){ 88, 110, 117, 255 };
    g_theme.selection    = (Color){ 181, 137, 0, 255 };

    g_theme.cell_empty   = (Color){ 0, 55, 70, 255 };
    g_theme.cell_hover   = (Color){ 7, 70, 85, 255 };
    g_theme.note_bg      = (Color){ 181, 137, 0, 255 };
    g_theme.note_border  = (Color){ 120, 90, 0, 255 };
    g_theme.note_text    = (Color){ 253, 246, 227, 255 };

    g_theme.mute         = (Color){ 220, 50, 47, 255 };
    g_theme.solo         = (Color){ 181, 137, 0, 255 };
    g_theme.stop_btn     = (Color){ 220, 50, 47, 255 };
    g_theme.btn_add      = (Color){ 20, 70, 85, 255 };
    g_theme.btn_add_hover = (Color){ 40, 100, 120, 255 };

    g_theme.scroll_bg    = (Color){ 0, 30, 40, 255 };
    g_theme.scroll_fg    = (Color){ 88, 110, 117, 255 };

    g_theme.vu_green     = (Color){ 133, 153, 0, 255 };
    g_theme.vu_yellow    = (Color){ 181, 137, 0, 255 };
    g_theme.vu_red       = (Color){ 220, 50, 47, 255 };

    g_theme.grid_line    = (Color){ 7, 54, 66, 255 };
    g_theme.ruler_bg     = (Color){ 0, 43, 54, 255 };
    g_theme.ruler_line   = (Color){ 88, 110, 117, 255 };
    g_theme.ruler_tick   = (Color){ 88, 110, 117, 255 };

    g_theme.wave_bg      = (Color){ 0, 30, 40, 255 };
    g_theme.wave_fg      = (Color){ 42, 161, 152, 255 };

    strcpy(g_theme_name, "solarized");
}

/* ---- step 18: three new themes ---- */
/* OLED: the background, the waveform and the empty areas are PURE black (on an OLED screen those pixels are off); the panels almost black */
static void apply_oled(void)
{
    g_theme.bg           = (Color){ 0, 0, 0, 255 };
    g_theme.panel_bg     = (Color){ 13, 13, 15, 255 };
    g_theme.panel_bg2    = (Color){ 0, 0, 0, 255 };
    g_theme.text         = (Color){ 236, 236, 240, 255 };
    g_theme.text_dim     = (Color){ 125, 125, 135, 255 };
    g_theme.selection    = (Color){ 0, 224, 200, 255 };
    g_theme.cell_empty   = (Color){ 17, 17, 20, 255 };
    g_theme.cell_hover   = (Color){ 36, 36, 42, 255 };
    g_theme.note_bg      = (Color){ 232, 204, 92, 255 };
    g_theme.note_border  = (Color){ 140, 120, 40, 255 };
    g_theme.note_text    = (Color){ 20, 18, 5, 255 };
    g_theme.mute         = (Color){ 235, 70, 80, 255 };
    g_theme.solo         = (Color){ 240, 205, 70, 255 };
    g_theme.stop_btn     = (Color){ 215, 55, 65, 255 };
    g_theme.btn_add      = (Color){ 28, 28, 34, 255 };
    g_theme.btn_add_hover= (Color){ 50, 60, 72, 255 };
    g_theme.scroll_bg    = (Color){ 0, 0, 0, 255 };
    g_theme.scroll_fg    = (Color){ 70, 70, 80, 255 };
    g_theme.vu_green     = (Color){ 40, 225, 120, 255 };
    g_theme.vu_yellow    = (Color){ 245, 205, 60, 255 };
    g_theme.vu_red       = (Color){ 245, 60, 70, 255 };
    g_theme.grid_line    = (Color){ 38, 38, 44, 255 };
    g_theme.ruler_bg     = (Color){ 0, 0, 0, 255 };
    g_theme.ruler_line   = (Color){ 48, 48, 56, 255 };
    g_theme.ruler_tick   = (Color){ 36, 36, 42, 255 };
    g_theme.wave_bg      = (Color){ 0, 0, 0, 255 };
    g_theme.wave_fg      = (Color){ 0, 205, 255, 255 };

    strcpy(g_theme_name, "oled");
}

/* Nord: the well-known cool "arctic" palette (Arctic Ice Studio, free) */
static void apply_nord(void)
{
    g_theme.bg           = (Color){ 36, 41, 51, 255 };
    g_theme.panel_bg     = (Color){ 46, 52, 64, 255 };
    g_theme.panel_bg2    = (Color){ 52, 59, 74, 255 };
    g_theme.text         = (Color){ 216, 222, 233, 255 };
    g_theme.text_dim     = (Color){ 132, 143, 163, 255 };
    g_theme.selection    = (Color){ 136, 192, 208, 255 };
    g_theme.cell_empty   = (Color){ 59, 66, 82, 255 };
    g_theme.cell_hover   = (Color){ 76, 86, 106, 255 };
    g_theme.note_bg      = (Color){ 235, 203, 139, 255 };
    g_theme.note_border  = (Color){ 176, 146, 90, 255 };
    g_theme.note_text    = (Color){ 46, 52, 64, 255 };
    g_theme.mute         = (Color){ 191, 97, 106, 255 };
    g_theme.solo         = (Color){ 235, 203, 139, 255 };
    g_theme.stop_btn     = (Color){ 191, 97, 106, 255 };
    g_theme.btn_add      = (Color){ 67, 76, 94, 255 };
    g_theme.btn_add_hover= (Color){ 94, 129, 172, 255 };
    g_theme.scroll_bg    = (Color){ 36, 41, 51, 255 };
    g_theme.scroll_fg    = (Color){ 76, 86, 106, 255 };
    g_theme.vu_green     = (Color){ 163, 190, 140, 255 };
    g_theme.vu_yellow    = (Color){ 235, 203, 139, 255 };
    g_theme.vu_red       = (Color){ 191, 97, 106, 255 };
    g_theme.grid_line    = (Color){ 67, 76, 94, 255 };
    g_theme.ruler_bg     = (Color){ 40, 45, 56, 255 };
    g_theme.ruler_line   = (Color){ 76, 86, 106, 255 };
    g_theme.ruler_tick   = (Color){ 67, 76, 94, 255 };
    g_theme.wave_bg      = (Color){ 32, 36, 45, 255 };
    g_theme.wave_fg      = (Color){ 136, 192, 208, 255 };

    strcpy(g_theme_name, "nord");
}

/* Gruvbox: the well-known warm retro palette (Pavel Pertsev, free) */
static void apply_gruvbox(void)
{
    g_theme.bg           = (Color){ 29, 32, 33, 255 };
    g_theme.panel_bg     = (Color){ 40, 40, 40, 255 };
    g_theme.panel_bg2    = (Color){ 40, 40, 40, 255 };
    g_theme.text         = (Color){ 235, 219, 178, 255 };
    g_theme.text_dim     = (Color){ 146, 131, 116, 255 };
    g_theme.selection    = (Color){ 254, 128, 25, 255 };
    g_theme.cell_empty   = (Color){ 50, 48, 47, 255 };
    g_theme.cell_hover   = (Color){ 80, 73, 69, 255 };
    g_theme.note_bg      = (Color){ 250, 189, 47, 255 };
    g_theme.note_border  = (Color){ 181, 118, 20, 255 };
    g_theme.note_text    = (Color){ 40, 40, 40, 255 };
    g_theme.mute         = (Color){ 251, 73, 52, 255 };
    g_theme.solo         = (Color){ 250, 189, 47, 255 };
    g_theme.stop_btn     = (Color){ 204, 36, 29, 255 };
    g_theme.btn_add      = (Color){ 80, 73, 69, 255 };
    g_theme.btn_add_hover= (Color){ 102, 92, 84, 255 };
    g_theme.scroll_bg    = (Color){ 29, 32, 33, 255 };
    g_theme.scroll_fg    = (Color){ 102, 92, 84, 255 };
    g_theme.vu_green     = (Color){ 184, 187, 38, 255 };
    g_theme.vu_yellow    = (Color){ 250, 189, 47, 255 };
    g_theme.vu_red       = (Color){ 251, 73, 52, 255 };
    g_theme.grid_line    = (Color){ 60, 56, 54, 255 };
    g_theme.ruler_bg     = (Color){ 29, 32, 33, 255 };
    g_theme.ruler_line   = (Color){ 102, 92, 84, 255 };
    g_theme.ruler_tick   = (Color){ 80, 73, 69, 255 };
    g_theme.wave_bg      = (Color){ 24, 26, 27, 255 };
    g_theme.wave_fg      = (Color){ 142, 192, 124, 255 };

    strcpy(g_theme_name, "gruvbox");
}

// ============ Presets API ============

void theme_apply_preset(ThemePreset preset)
{
    switch (preset) {
    case THEME_DARK:      apply_dark();      break;
    case THEME_LIGHT:     apply_light();     break;
    case THEME_MIDNIGHT:  apply_midnight();  break;
    case THEME_SOLARIZED: apply_solarized(); break;
    case THEME_OLED:      apply_oled();      break;
    case THEME_NORD:      apply_nord();      break;
    case THEME_GRUVBOX:   apply_gruvbox();   break;
    case THEME_CUSTOM:    strcpy(g_theme_name, "custom"); break;
    default: break;
    }
}

void theme_apply_accent(AccentPreset accent)
{
    Color sel;
    switch (accent) {
    case ACCENT_YELLOW: sel = (Color){ 255, 230, 90, 255 };  strcpy(g_accent_name, "yellow"); break;
    case ACCENT_BLUE:   sel = (Color){ 90, 170, 255, 255 };  strcpy(g_accent_name, "blue");   break;
    case ACCENT_GREEN:  sel = (Color){ 100, 220, 130, 255 }; strcpy(g_accent_name, "green");  break;
    case ACCENT_RED:    sel = (Color){ 240, 90, 90, 255 };   strcpy(g_accent_name, "red");    break;
    case ACCENT_PURPLE: sel = (Color){ 190, 130, 240, 255 }; strcpy(g_accent_name, "purple"); break;
    case ACCENT_ORANGE: sel = (Color){ 255, 163, 26, 255 };  strcpy(g_accent_name, "orange"); break;   /* the orange from the logo */
    default: return;
    }
    g_theme.selection = sel;
}

const char *theme_preset_name(ThemePreset preset)
{
    switch (preset) {
    case THEME_DARK:      return "Dark";
    case THEME_LIGHT:     return "Light";
    case THEME_MIDNIGHT:  return "Midnight";
    case THEME_SOLARIZED: return "Solarized";
    case THEME_OLED:      return "OLED Black";
    case THEME_NORD:      return "Nord";
    case THEME_GRUVBOX:   return "Gruvbox";
    case THEME_CUSTOM:    return "Custom";
    default:              return "?";
    }
}

const char *theme_accent_name(AccentPreset accent)
{
    switch (accent) {
    case ACCENT_YELLOW: return "Yellow";
    case ACCENT_BLUE:   return "Blue";
    case ACCENT_GREEN:  return "Green";
    case ACCENT_RED:    return "Red";
    case ACCENT_PURPLE: return "Purple";
    case ACCENT_ORANGE: return "Orange";
    default:            return "?";
    }
}

ThemePreset theme_preset_from_name(const char *name)
{
    if (!name) return THEME_DARK;
    if (strcmp(name, "light") == 0)     return THEME_LIGHT;
    if (strcmp(name, "midnight") == 0)  return THEME_MIDNIGHT;
    if (strcmp(name, "solarized") == 0) return THEME_SOLARIZED;
    if (strcmp(name, "oled") == 0)      return THEME_OLED;
    if (strcmp(name, "nord") == 0)      return THEME_NORD;
    if (strcmp(name, "gruvbox") == 0)   return THEME_GRUVBOX;
    if (strcmp(name, "custom") == 0)    return THEME_CUSTOM;
    return THEME_DARK;
}

AccentPreset theme_accent_from_name(const char *name)
{
    if (!name) return ACCENT_YELLOW;
    if (strcmp(name, "blue") == 0)   return ACCENT_BLUE;
    if (strcmp(name, "green") == 0)  return ACCENT_GREEN;
    if (strcmp(name, "red") == 0)    return ACCENT_RED;
    if (strcmp(name, "purple") == 0) return ACCENT_PURPLE;
    if (strcmp(name, "orange") == 0) return ACCENT_ORANGE;
    return ACCENT_YELLOW;
}

// ============ Save / Load ============

void theme_get_config_path(char *out, size_t size)
{
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(out, size, "%s/.config/qlim/theme.conf", home);
}

static void ensure_config_dir(void)
{
    const char *home = getenv("HOME");
    if (!home) return;

    char path[512];
    snprintf(path, sizeof(path), "%s/.config", home);
    mkdir(path, 0755);

    snprintf(path, sizeof(path), "%s/.config/qlim", home);
    mkdir(path, 0755);
}

static void write_color(FILE *f, const char *key, Color c)
{
    fprintf(f, "%s=%d,%d,%d,%d\n", key, c.r, c.g, c.b, c.a);
}

static bool read_color(const char *line, const char *key, Color *out)
{
    size_t klen = strlen(key);
    if (strncmp(line, key, klen) != 0) return false;
    if (line[klen] != '=') return false;

    const char *val = line + klen + 1;
    int r, g, b, a;
    if (sscanf(val, "%d,%d,%d,%d", &r, &g, &b, &a) != 4) return false;

    out->r = (unsigned char)r;
    out->g = (unsigned char)g;
    out->b = (unsigned char)b;
    out->a = (unsigned char)a;
    return true;
}

static bool read_int(const char *line, const char *key, int *out)
{
    size_t klen = strlen(key);
    if (strncmp(line, key, klen) != 0) return false;
    if (line[klen] != '=') return false;

    *out = atoi(line + klen + 1);
    return true;
}

static bool read_str(const char *line, const char *key, char *out, size_t out_size)
{
    size_t klen = strlen(key);
    if (strncmp(line, key, klen) != 0) return false;
    if (line[klen] != '=') return false;

    const char *val = line + klen + 1;
    size_t vlen = strlen(val);
    while (vlen > 0 && (val[vlen-1] == '\n' || val[vlen-1] == '\r')) vlen--;
    if (vlen >= out_size) vlen = out_size - 1;
    memcpy(out, val, vlen);
    out[vlen] = '\0';
    return true;
}

void theme_load(void)
{
    // First apply Dark as the default
    apply_dark();
    theme_apply_accent(ACCENT_ORANGE);       /* step 36g: the main colour of Q-LiM (as in the logo) */
    g_font_size = 14;

    char path[512];
    theme_get_config_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        printf("theme_load: no config, using Dark defaults\n");
        return;
    }

    char theme_name_buf[32] = "dark";
    char accent_name_buf[32] = "orange";
    int  font_size = 14;
    int  accent_rev = 0;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        read_str(line, "theme",  theme_name_buf, sizeof(theme_name_buf));
        read_str(line, "accent", accent_name_buf, sizeof(accent_name_buf));
        read_int(line, "font_size", &font_size);
        read_int(line, "accent_rev", &accent_rev);
    }
    if (accent_rev < 2) strcpy(accent_name_buf, "orange");   /* step 36g: once - the Q-LiM orange */

    // Close, apply the basics, then if it is custom, read the colours
    fclose(f);

    ThemePreset preset = theme_preset_from_name(theme_name_buf);
    if (preset == THEME_CUSTOM) preset = THEME_DARK;   /* The Custom theme was removed from the menu -> an old file falls back to Dark */
    theme_apply_preset(preset);
    if (strcmp(accent_name_buf, "theme") == 0) strcpy(g_accent_name, "theme");   /* the colour of the theme itself */
    else theme_apply_accent(theme_accent_from_name(accent_name_buf));

    if (font_size < 8) font_size = 8;
    if (font_size > 32) font_size = 32;
    g_font_size = font_size;

    // If custom – read all the colours
    if (preset == THEME_CUSTOM) {
        f = fopen(path, "r");
        if (!f) return;

        while (fgets(line, sizeof(line), f)) {
            read_color(line, "bg",            &g_theme.bg);
            read_color(line, "panel_bg",      &g_theme.panel_bg);
            read_color(line, "panel_bg2",     &g_theme.panel_bg2);
            read_color(line, "text",          &g_theme.text);
            read_color(line, "text_dim",      &g_theme.text_dim);
            read_color(line, "selection",     &g_theme.selection);

            read_color(line, "cell_empty",    &g_theme.cell_empty);
            read_color(line, "cell_hover",    &g_theme.cell_hover);
            read_color(line, "note_bg",       &g_theme.note_bg);
            read_color(line, "note_border",   &g_theme.note_border);
            read_color(line, "note_text",     &g_theme.note_text);

            read_color(line, "mute",          &g_theme.mute);
            read_color(line, "solo",          &g_theme.solo);
            read_color(line, "stop_btn",      &g_theme.stop_btn);
            read_color(line, "btn_add",       &g_theme.btn_add);
            read_color(line, "btn_add_hover", &g_theme.btn_add_hover);

            read_color(line, "scroll_bg",     &g_theme.scroll_bg);
            read_color(line, "scroll_fg",     &g_theme.scroll_fg);

            read_color(line, "vu_green",      &g_theme.vu_green);
            read_color(line, "vu_yellow",     &g_theme.vu_yellow);
            read_color(line, "vu_red",        &g_theme.vu_red);

            read_color(line, "grid_line",     &g_theme.grid_line);
            read_color(line, "ruler_bg",      &g_theme.ruler_bg);
            read_color(line, "ruler_line",    &g_theme.ruler_line);
            read_color(line, "ruler_tick",    &g_theme.ruler_tick);

            read_color(line, "wave_bg",       &g_theme.wave_bg);
            read_color(line, "wave_fg",       &g_theme.wave_fg);
        }
        fclose(f);
    }

    printf("theme_load: OK (theme=%s, accent=%s, font=%d)\n",
           g_theme_name, g_accent_name, g_font_size);
}

void theme_save(void)
{
    ensure_config_dir();

    char path[512];
    theme_get_config_path(path, sizeof(path));

    char tmp[1100];
    FILE *f = safe_open(path, tmp, sizeof tmp);
    if (!f) {
        fprintf(stderr, "theme_save: cannot open '%s'\n", path);
        return;
    }

    fprintf(f, "# Q-LiM theme config\n\n");
    fprintf(f, "theme=%s\n",  g_theme_name);
    fprintf(f, "accent=%s\n", g_accent_name);
    fprintf(f, "accent_rev=2\n");
    fprintf(f, "font_size=%d\n\n", g_font_size);

    // If custom – save all the colours
    if (strcmp(g_theme_name, "custom") == 0) {
        fprintf(f, "# Custom colors\n");
        write_color(f, "bg",            g_theme.bg);
        write_color(f, "panel_bg",      g_theme.panel_bg);
        write_color(f, "panel_bg2",     g_theme.panel_bg2);
        write_color(f, "text",          g_theme.text);
        write_color(f, "text_dim",      g_theme.text_dim);
        write_color(f, "selection",     g_theme.selection);

        write_color(f, "cell_empty",    g_theme.cell_empty);
        write_color(f, "cell_hover",    g_theme.cell_hover);
        write_color(f, "note_bg",       g_theme.note_bg);
        write_color(f, "note_border",   g_theme.note_border);
        write_color(f, "note_text",     g_theme.note_text);

        write_color(f, "mute",          g_theme.mute);
        write_color(f, "solo",          g_theme.solo);
        write_color(f, "stop_btn",      g_theme.stop_btn);
        write_color(f, "btn_add",       g_theme.btn_add);
        write_color(f, "btn_add_hover", g_theme.btn_add_hover);

        write_color(f, "scroll_bg",     g_theme.scroll_bg);
        write_color(f, "scroll_fg",     g_theme.scroll_fg);

        write_color(f, "vu_green",      g_theme.vu_green);
        write_color(f, "vu_yellow",     g_theme.vu_yellow);
        write_color(f, "vu_red",        g_theme.vu_red);

        write_color(f, "grid_line",     g_theme.grid_line);
        write_color(f, "ruler_bg",      g_theme.ruler_bg);
        write_color(f, "ruler_line",    g_theme.ruler_line);
        write_color(f, "ruler_tick",    g_theme.ruler_tick);

        write_color(f, "wave_bg",       g_theme.wave_bg);
        write_color(f, "wave_fg",       g_theme.wave_fg);
    }

    if (safe_commit(f, tmp, path, 0) != 0) return;
    printf("theme_save: OK '%s'\n", path);
}
