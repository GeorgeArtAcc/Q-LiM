/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 2.1: the settings windows of the Edit menu (Audio, Appearance, About, Performance, Loudness, Plugins, MIDI) - moved out of ui.c. */
#include "ui.h"
#include "perf.h"
#include "version.h"
#include "plugins.h"
#include "fxchain.h"
#include "midiq.h"
#include <ctype.h>
#include "ui_dialogs.h"
#include "mcfile.h"
#include "undo.h"
#include "separate.h"
#include "schedule.h"
#include "theme.h"
#include "midi.h"
#include "midimap.h"
#include "sdlgfx.h"
#include "stream.h"
#include "dsp_meter.h"
#include "audio.h"
#include "settings.h"
#include "project.h"
#include "applog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "ui_internal.h"
#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#endif
#include "wav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <ctype.h>

// ============ Audio Settings Dialog ============

#define MAX_HOST_APIS 16
#define MAX_DEVICES   64

static int   g_dlg_host_api_count = 0;
static char  g_dlg_host_api_names[MAX_HOST_APIS][64];
static int   g_dlg_host_api_global_idx[MAX_HOST_APIS];

static int   g_dlg_device_count = 0;
static char  g_dlg_device_names[MAX_DEVICES][128];
static int   g_dlg_device_global_idx[MAX_DEVICES];
static int   g_dlg_device_max_ch[MAX_DEVICES];
static int   g_dlg_device_default_sr[MAX_DEVICES];

static bool g_audio_dlg_refresh_flag = false;

/* (re)builds the driver and device lists from PortAudio */
static void audio_dlg_rebuild_lists(void)
{
    g_dlg_host_api_count = audio_get_host_api_count();
    if (g_dlg_host_api_count > MAX_HOST_APIS) g_dlg_host_api_count = MAX_HOST_APIS;
    for (int i = 0; i < g_dlg_host_api_count; ++i) {
        snprintf(g_dlg_host_api_names[i], sizeof(g_dlg_host_api_names[i]), "%s", audio_get_host_api_name(i));
        g_dlg_host_api_global_idx[i] = i;
    }
    if (g_audio_dlg_config.host_api_index >= g_dlg_host_api_count) {
        g_audio_dlg_config.host_api_index = -1;
        g_audio_dlg_config.backend = AUDIO_BACKEND_PORTAUDIO;      /* e.g. PipeWire is gone */
    }

    int dev_count = audio_get_device_count(g_audio_dlg_config.host_api_index);
    if (dev_count > MAX_DEVICES) dev_count = MAX_DEVICES;

    g_dlg_device_count = 0;
    for (int i = 0; i < dev_count; ++i) {
        int global_idx = -1;
        const char *name = audio_get_device_name(g_audio_dlg_config.host_api_index, i, &global_idx);
        snprintf(g_dlg_device_names[g_dlg_device_count], sizeof(g_dlg_device_names[g_dlg_device_count]),
                 "%s  (%d out)", name, audio_get_device_max_channels(global_idx));
        g_dlg_device_global_idx[g_dlg_device_count] = global_idx;
        g_dlg_device_max_ch[g_dlg_device_count] = audio_get_device_max_channels(global_idx);
        g_dlg_device_default_sr[g_dlg_device_count] = audio_get_device_default_samplerate(global_idx);
        g_dlg_device_count++;
    }
    /* the chosen device is gone -> "(default)" instead of pointing to another card */
    bool found = g_audio_dlg_config.device_index < 0;
    for (int i = 0; i < g_dlg_device_count; ++i)
        if (g_dlg_device_global_idx[i] == g_audio_dlg_config.device_index) found = true;
    if (!found) g_audio_dlg_config.device_index = -1;
}

void ui_open_audio_settings(void)
{
    g_audio_dlg_open = true;
    g_audio_dlg_open_time = GetTime();
    g_audio_dlg_apply_flag = false;
    g_audio_dlg_refresh_flag = false;

    g_audio_dlg_config = audio_get_current_config();
    g_audio_dlg_original = g_audio_dlg_config;
    audio_dlg_rebuild_lists();
}

#ifdef _WIN32
static int g_asio_panel_dev = -1;
int ui_take_asio_panel(void) { int d = g_asio_panel_dev; g_asio_panel_dev = -1; return d; }
#endif
bool ui_take_audio_refresh(void)
{
    bool r = g_audio_dlg_refresh_flag;
    g_audio_dlg_refresh_flag = false;
    return r;
}

/* main.c calls this after rescanning the cards */
void ui_audio_refresh_done(void)
{
    if (!g_audio_dlg_open) return;
    /* the audio was restarted -> the dialog shows what is REALLY running now (driver/device),
     * and the other (not yet applied) changes stay */
    AudioConfig now = audio_get_current_config();
    g_audio_dlg_config.backend = now.backend;
    g_audio_dlg_config.host_api_index = now.host_api_index;
    g_audio_dlg_config.device_index = now.device_index;
    g_audio_dlg_config.output_channels = now.output_channels;
    snprintf(g_audio_dlg_config.pw_target, sizeof g_audio_dlg_config.pw_target, "%s", now.pw_target);
    g_audio_dlg_original.backend = now.backend;
    g_audio_dlg_original.host_api_index = now.host_api_index;
    g_audio_dlg_original.device_index = now.device_index;
    g_audio_dlg_original.output_channels = now.output_channels;
    snprintf(g_audio_dlg_original.pw_target, sizeof g_audio_dlg_original.pw_target, "%s", now.pw_target);
    audio_dlg_rebuild_lists();
}

/* 1.4.21: e.g. the Sound Cards window switches the PipeWire graph mode off */
void ui_audio_apply(const AudioConfig *c)
{
    if (!c) return;
    g_audio_dlg_config = *c;
    g_audio_dlg_apply_flag = true;
    g_audio_dlg_open = false;
}



bool ui_take_audio_apply(AudioConfig *out_config)
{
    if (g_audio_dlg_apply_flag) {
        g_audio_dlg_apply_flag = false;
        if (out_config) {
            *out_config = g_audio_dlg_config;
            out_config->api_name[0] = 0;      /* chosen now in the dialog: the numbers are right, the old names are not */
            out_config->dev_name[0] = 0;
        }
        return true;
    }
    return false;
}

static const char *get_host_api_display_name(int idx)
{
    if (idx < 0 || idx >= g_dlg_host_api_count) return "(default)";
    return g_dlg_host_api_names[idx];
}

static const char *get_device_display_name(int idx)
{
    if (idx < 0 || idx >= g_dlg_device_count) return "(default)";
    return g_dlg_device_names[idx];
}

static int find_device_local_index(int global_idx)
{
    for (int i = 0; i < g_dlg_device_count; ++i) {
        if (g_dlg_device_global_idx[i] == global_idx) return i;
    }
    return 0;
}

static const char *const g_disk_strings[4] = {
    "Auto (recommended)", "Small (fast disks)", "Medium", "Large (slow / old disks)"
};
static const int g_sr_values[] = { 44100, 48000, 88200, 96000, 192000 };
#define N_SR_VALUES 5

static const int g_buf_values[] = { 64, 128, 256, 512, 1024, 2048 };
#define N_BUF_VALUES 6

static const char *g_sr_strings[] = {
    "44100", "48000", "88200", "96000", "192000"
};

static const char *g_buf_strings[] = {
    "64", "128", "256", "512", "1024", "2048"
};



static int find_sr_index(int sr)
{
    for (int i = 0; i < N_SR_VALUES; ++i) {
        if (g_sr_values[i] == sr) return i;
    }
    return 1;
}

static int find_buf_index(int buf)
{
    for (int i = 0; i < N_BUF_VALUES; ++i) {
        if (g_buf_values[i] == buf) return i;
    }
    return 2;
}


void draw_audio_settings_dialog(int screen_w, int screen_h)
{
    if (!g_audio_dlg_open) return;

    GfxDialogBackdrop(70);

    const int dlg_w = 640;
    const int dlg_h = 503;
    const int dlg_x = (screen_w - dlg_w) / 2;
    const int dlg_y = (screen_h - dlg_h) / 2;

    double t = GetTime() - g_audio_dlg_open_time;
    float alpha = 1.0f;
    if (t < 0.15) alpha = (float)(t / 0.15);
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;

    Color bg = g_theme.panel_bg;
    bg.r = (unsigned char)(bg.r + 10);
    bg.g = (unsigned char)(bg.g + 10);
    bg.b = (unsigned char)(bg.b + 10);
    bg.a = (unsigned char)(bg.a * alpha);
    DrawRectangle(dlg_x, dlg_y, dlg_w, dlg_h, bg);

    Color border = g_theme.selection;
    border.a = (unsigned char)(border.a * alpha);
    DrawRectangleLinesEx((Rectangle){ dlg_x, dlg_y, dlg_w, dlg_h },
                         2, border);

    DrawRectangle(dlg_x, dlg_y, dlg_w, 40,
                  (Color){ g_theme.selection.r,
                           g_theme.selection.g,
                           g_theme.selection.b,
                           (unsigned char)(255 * alpha) });

    Color title_col = g_theme.bg;
    title_col.a = (unsigned char)(title_col.a * alpha);
    ui_text("Audio Settings",
            dlg_x + 16, dlg_y + 10, 20, title_col);

    const int row_y_start = dlg_y + 60;
    const int row_h = 42;
    const int label_x = dlg_x + 20;
    const int field_x = dlg_x + 160;
    const int field_w = dlg_w - 180;
    const int field_h = 28;

    Vector2 mouse = GetMousePosition();

    {
        int y = row_y_start;
        ui_text("Driver:", label_x, y + 6, 15, g_theme.text);

        int dd_id = 100;
        const char *cur = get_host_api_display_name(
            g_audio_dlg_config.host_api_index);

        bool opened = draw_dropdown_button(dd_id, cur,
                                           field_x, y,
                                           field_w, field_h,
                                           g_dd.open_dropdown_id == dd_id);
        if (opened) {
            if (g_dd.open_dropdown_id == dd_id) {
                g_dd.open_dropdown_id = -1;
            } else {
                g_dd.open_dropdown_id = dd_id;
                g_dd.open_time = GetTime();
            }
        }
    }

    {
        int y = row_y_start + row_h;
        ui_text("Device:", label_x, y + 6, 15, g_theme.text);

        int dd_id = 101;
        const char *cur = get_device_display_name(
            find_device_local_index(g_audio_dlg_config.device_index));

        bool opened = draw_dropdown_button(dd_id, cur,
                                           field_x, y,
                                           field_w, field_h,
                                           g_dd.open_dropdown_id == dd_id);
        if (opened) {
            if (g_dd.open_dropdown_id == dd_id) {
                g_dd.open_dropdown_id = -1;
            } else {
                g_dd.open_dropdown_id = dd_id;
                g_dd.open_time = GetTime();
            }
        }
    }

    {
        int y = row_y_start + row_h * 2;
        ui_text("Sample Rate:", label_x, y + 6, 15, g_theme.text);

        int dd_id = 102;
        int cur_idx = find_sr_index(g_audio_dlg_config.sample_rate);
        const char *cur = g_sr_strings[cur_idx];

        bool opened = draw_dropdown_button(dd_id, cur,
                                           field_x, y,
                                           field_w, field_h,
                                           g_dd.open_dropdown_id == dd_id);
        if (opened) {
            if (g_dd.open_dropdown_id == dd_id) {
                g_dd.open_dropdown_id = -1;
            } else {
                g_dd.open_dropdown_id = dd_id;
                g_dd.open_time = GetTime();
            }
        }
    }

    {
        int y = row_y_start + row_h * 3;
        ui_text("Buffer Size:", label_x, y + 6, 15, g_theme.text);

        int dd_id = 103;
        int cur_idx = find_buf_index(g_audio_dlg_config.buffer_size);
        const char *cur = g_buf_strings[cur_idx];

        bool opened = draw_dropdown_button(dd_id, cur,
                                           field_x, y,
                                           field_w, field_h,
                                           g_dd.open_dropdown_id == dd_id);
        if (opened) {
            if (g_dd.open_dropdown_id == dd_id) {
                g_dd.open_dropdown_id = -1;
            } else {
                g_dd.open_dropdown_id = dd_id;
                g_dd.open_time = GetTime();
            }
        }
    }

    {
        int y = row_y_start + row_h * 4;
        ui_text("Sound card outputs:", label_x, y + 6, 15, g_theme.text);

        /* Not selectable: shows how many outputs the chosen device REALLY has. */
        int gdev = g_audio_dlg_config.device_index;
        int n_out = 0;
        if (gdev >= 0) n_out = audio_get_device_max_channels(gdev);
        else n_out = g_audio_dlg_original.output_channels;
        if (n_out < 2) n_out = 2;
        char tb[96];
        snprintf(tb, sizeof tb, "%d  (detected automatically)", n_out);
        DrawRectangle(field_x, y, field_w, field_h, g_theme.panel_bg2);
        ui_text(tb, field_x + 10, y + (field_h - 15) / 2, 15, g_theme.text_dim);
    }

    {
        int y = row_y_start + row_h * 5;
        ui_text("Disk buffer:", label_x, y + 6, 15, g_theme.text);

        int dd_id = 104;
        int dm = g_audio_dlg_config.disk_mode;
        if (dm < 0 || dm > 3) dm = 0;
        bool opened = draw_dropdown_button(dd_id, g_disk_strings[dm],
                                           field_x, y, field_w, field_h,
                                           g_dd.open_dropdown_id == dd_id);
        if (opened) {
            if (g_dd.open_dropdown_id == dd_id) {
                g_dd.open_dropdown_id = -1;
            } else {
                g_dd.open_dropdown_id = dd_id;
                g_dd.open_time = GetTime();
            }
        }
    }

#ifndef _WIN32                  /* no PipeWire on Windows */
    {
        // Checkbox (step 28): the channels and the master - separate nodes in the PipeWire graph (PipeWire native only)
        /* (2.0: the "Convert files with a different sample rate" row above is gone - Collect All & Save converts,
         *  into the project's folder, with the files' own names) */
        int y = row_y_start + row_h * 6;
        bool pw = g_audio_dlg_config.backend == AUDIO_BACKEND_PIPEWIRE;
        Rectangle box = { (float)label_x, (float)(y + 4), 20.0f, 20.0f };
        Rectangle hit = { (float)label_x, (float)y, (float)(dlg_w - 40), 38.0f };
        bool hov = pw && CheckCollisionPointRec(mouse, hit);
        DrawRectangleRec(box, g_theme.panel_bg2);
        DrawRectangleLinesEx(box, 1, hov ? g_theme.selection : g_theme.text_dim);
        if (g_audio_dlg_config.pw_graph)
            DrawRectangle((int)box.x + 4, (int)box.y + 4, 12, 12, pw ? g_theme.selection : g_theme.text_dim);
        ui_text("Show every channel in the PipeWire graph",
                label_x + 30, y + 5, 15, pw ? g_theme.text : g_theme.text_dim);
        ui_text(!pw ? "only with the \"PipeWire (native)\" driver"
                    : g_audio_dlg_config.pw_graph
                        ? "ON: channels -> Q-LiM Master -> card, editable in Patchance/qpwgraph"
                        : "OFF: one stereo/multichannel stream (as before)",
                label_x + 30, y + 24, 12, g_theme.text_dim);
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && g_dd.open_dropdown_id == -1)
            g_audio_dlg_config.pw_graph = g_audio_dlg_config.pw_graph ? 0 : 1;
    }
#endif

    {
        int y = row_y_start + row_h * 7 + 6;
        char info[256];
        snprintf(info, sizeof(info),
                 "Current: %s / %s / %d Hz / %d / %d ch",
                 get_host_api_display_name(g_audio_dlg_config.host_api_index),
                 get_device_display_name(
                     find_device_local_index(g_audio_dlg_config.device_index)),
                 g_audio_dlg_config.sample_rate,
                 g_audio_dlg_config.buffer_size,
                 g_audio_dlg_config.output_channels);

        ui_text(info, dlg_x + 20, y, 13, g_theme.text_dim);
        {
            char st[96], ds[220];
            streams_status_text(st, sizeof st);
            snprintf(ds, sizeof ds, "Disk buffers: %s   |   dry-outs this session: %lu",
                     st, streams_starved_total());
            ui_text(ds, dlg_x + 20, y + 18, 13, g_theme.text_dim);
        }

        {
            char es[200];
            if (audio_engine_status_text(es, sizeof es))
                ui_text(es, dlg_x + 20, y + 36, 13, g_theme.text_dim);
        }
        bool changed =
            (g_audio_dlg_config.backend != g_audio_dlg_original.backend) ||
            (g_audio_dlg_config.host_api_index != g_audio_dlg_original.host_api_index) ||
            (g_audio_dlg_config.device_index != g_audio_dlg_original.device_index) ||
            (g_audio_dlg_config.sample_rate != g_audio_dlg_original.sample_rate) ||
            (g_audio_dlg_config.buffer_size != g_audio_dlg_original.buffer_size) ||
            (g_audio_dlg_config.disk_mode != g_audio_dlg_original.disk_mode) ||
            (g_audio_dlg_config.backend == AUDIO_BACKEND_PIPEWIRE &&
             g_audio_dlg_config.pw_graph != g_audio_dlg_original.pw_graph);

        if (changed) {
            ui_text("(changes pending)", dlg_x + 20, y + 54, 13, g_theme.selection);
        }
    }

    const int btn_w = 120;
    const int btn_h = 36;
    const int btn_y = dlg_y + dlg_h - 56;

    Rectangle apply_r = { dlg_x + dlg_w - 2 * btn_w - 30, btn_y,
                          btn_w, btn_h };
    Rectangle cancel_r = { dlg_x + dlg_w - btn_w - 20, btn_y,
                           btn_w, btn_h };

    {
        Rectangle refresh_r = { dlg_x + 20, btn_y, btn_w, btn_h };
        bool hov = CheckCollisionPointRec(mouse, refresh_r);
        DrawRectangleRec(refresh_r, hov ? g_theme.panel_bg : g_theme.panel_bg2);
        DrawRectangleLinesEx(refresh_r, 1, g_theme.text_dim);
        int tw = ui_text_w("Refresh", 16);
        ui_text("Refresh", (int)(refresh_r.x + (btn_w - tw) / 2), (int)(refresh_r.y + (btn_h - 16) / 2), 16, g_theme.text);
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && g_dd.open_dropdown_id == -1)
            g_audio_dlg_refresh_flag = true;
        int lost_x = (int)refresh_r.x + btn_w + 14;
#ifdef _WIN32
        if (audio_device_is_asio(g_audio_dlg_config.device_index)) {      /* the driver's own settings (buffer etc.) */
            Rectangle ar = { refresh_r.x + btn_w + 10, btn_y, 130, btn_h };
            bool ah = CheckCollisionPointRec(mouse, ar);
            DrawRectangleRec(ar, ah ? g_theme.panel_bg : g_theme.panel_bg2);
            DrawRectangleLinesEx(ar, 1, g_theme.text_dim);
            int aw = ui_text_w("ASIO Panel", 16);
            ui_text("ASIO Panel", (int)(ar.x + (ar.width - aw) / 2), (int)(ar.y + (btn_h - 16) / 2), 16, g_theme.text);
            if (ah && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && g_dd.open_dropdown_id == -1)
                g_asio_panel_dev = g_audio_dlg_config.device_index;
            lost_x += 140;
        }
#endif
        {   /* 1.4.21: more than one sound card */
            Rectangle cr2 = { (float)lost_x, btn_y, 150, btn_h };
            bool ch2 = CheckCollisionPointRec(mouse, cr2);
            DrawRectangleRec(cr2, ch2 ? g_theme.panel_bg : g_theme.panel_bg2);
            DrawRectangleLinesEx(cr2, 1, g_theme.text_dim);
            int cw2 = ui_text_w("Sound Cards...", 16);
            ui_text("Sound Cards...", (int)(cr2.x + (cr2.width - cw2) / 2), (int)(cr2.y + (btn_h - 16) / 2), 16, g_theme.text);
            if (ch2 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && g_dd.open_dropdown_id == -1) {
                g_audio_dlg_open = false;
                ui_open_sound_cards();
            }
            lost_x += 160;
        }
        if (audio_is_lost()) {
            char lm[200]; snprintf(lm, sizeof lm, "Device lost: %s", audio_lost_name());
            ui_text(lm, lost_x, (int)(refresh_r.y + (btn_h - 14) / 2), 14, (Color){235, 70, 70, 255});
        }
    }

    {
        bool hov = CheckCollisionPointRec(mouse, apply_r);
        Color b = hov ? g_theme.selection : g_theme.panel_bg2;
        DrawRectangleRec(apply_r, b);
        DrawRectangleLinesEx(apply_r, 1, g_theme.text_dim);

        Color tc = hov ? g_theme.bg : g_theme.text;
        int tw = ui_text_w("Apply", 16);
        ui_text("Apply",
                (int)(apply_r.x + (btn_w - tw) / 2),
                (int)(apply_r.y + (btn_h - 16) / 2),
                16, tc);

        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            g_dd.open_dropdown_id == -1) {
            g_audio_dlg_apply_flag = true;
            g_audio_dlg_open = false;
            g_dd.open_dropdown_id = -1;
        }
    }

    {
        bool hov = CheckCollisionPointRec(mouse, cancel_r);
        Color b = hov ? g_theme.panel_bg : g_theme.panel_bg2;
        DrawRectangleRec(cancel_r, b);
        DrawRectangleLinesEx(cancel_r, 1, g_theme.text_dim);

        Color tc = g_theme.text;
        int tw = ui_text_w("Cancel", 16);
        ui_text("Cancel",
                (int)(cancel_r.x + (btn_w - tw) / 2),
                (int)(cancel_r.y + (btn_h - 16) / 2),
                16, tc);

        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            g_dd.open_dropdown_id == -1) {
            g_audio_dlg_open = false;
            g_dd.open_dropdown_id = -1;
        }
    }

    if (g_dd.open_dropdown_id == 100) {
        const char *items[MAX_HOST_APIS + 1];
        items[0] = "(default)";
        for (int i = 0; i < g_dlg_host_api_count; ++i) {
            items[i + 1] = g_dlg_host_api_names[i];
        }
        int total = g_dlg_host_api_count + 1;
        int current = g_audio_dlg_config.host_api_index + 1;

        int sel = draw_dropdown_list(100, items, total, current,
                                     field_x,
                                     row_y_start + field_h + 2,
                                     field_w);
        if (sel >= 0) {
            int new_api = sel - 1;
            if (new_api != g_audio_dlg_config.host_api_index) {
                g_audio_dlg_config.host_api_index = new_api;
                g_audio_dlg_config.backend = audio_host_api_is_pipewire(new_api) ? AUDIO_BACKEND_PIPEWIRE
                                                                                 : AUDIO_BACKEND_PORTAUDIO;
                /* with PipeWire the first entry is the system "default" */
                g_audio_dlg_config.device_index = g_audio_dlg_config.backend == AUDIO_BACKEND_PIPEWIRE ? AUDIO_PW_DEV_BASE : -1;

                audio_dlg_rebuild_lists();
            }
        }
    } else if (g_dd.open_dropdown_id == 101) {
        const char *items[MAX_DEVICES];
        for (int i = 0; i < g_dlg_device_count; ++i) {
            items[i] = g_dlg_device_names[i];
        }
        int current = find_device_local_index(
            g_audio_dlg_config.device_index);

        int sel = draw_dropdown_list(101, items, g_dlg_device_count,
                                     current,
                                     field_x,
                                     row_y_start + row_h + field_h + 2,
                                     field_w);
        if (sel >= 0) {
            g_audio_dlg_config.device_index =
                g_dlg_device_global_idx[sel];
        }
    } else if (g_dd.open_dropdown_id == 102) {
        int cur_idx = find_sr_index(g_audio_dlg_config.sample_rate);
        int sel = draw_dropdown_list(102, g_sr_strings, N_SR_VALUES,
                                     cur_idx,
                                     field_x,
                                     row_y_start + row_h * 2 + field_h + 2,
                                     field_w);
        if (sel >= 0) {
            g_audio_dlg_config.sample_rate = g_sr_values[sel];
        }
    } else if (g_dd.open_dropdown_id == 103) {
        int cur_idx = find_buf_index(g_audio_dlg_config.buffer_size);
        int sel = draw_dropdown_list(103, g_buf_strings, N_BUF_VALUES,
                                     cur_idx,
                                     field_x,
                                     row_y_start + row_h * 3 + field_h + 2,
                                     field_w);
        if (sel >= 0) {
            g_audio_dlg_config.buffer_size = g_buf_values[sel];
        }
    } else if (g_dd.open_dropdown_id == 104) {
        int dm = g_audio_dlg_config.disk_mode;
        if (dm < 0 || dm > 3) dm = 0;
        int sel = draw_dropdown_list(104, g_disk_strings, 4, dm,
                                     field_x,
                                     row_y_start + row_h * 5 + field_h + 2,
                                     field_w);
        if (sel >= 0) g_audio_dlg_config.disk_mode = sel;
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        if (g_dd.open_dropdown_id != -1) {
            g_dd.open_dropdown_id = -1;
        } else {
            g_audio_dlg_open = false;
        }
    }
}
// ============ Appearance Dialog ============

void ui_open_appearance(void)
{
    g_appearance_open = true;
    g_appearance_open_time = GetTime();
    g_appearance_backup = g_theme;
    snprintf(g_appearance_bk_theme, sizeof g_appearance_bk_theme, "%s", g_theme_name);
    snprintf(g_appearance_bk_accent, sizeof g_appearance_bk_accent, "%s", g_accent_name);
    g_appearance_bk_font = g_font_size;
    g_appearance_bk_style = g_ui_style;
    g_appearance_bk_time = g_clip_time;
}



/* ============ About ============ */
/* 2.0.0: support for the project (optional) - Revolut. The QR code below was made from the link with libqrencode
 * (error correction M) and checked by decoding it. */
#define QLIM_SUPPORT_URL "https://revolut.me/georgiefgj"
static const char *const SUPPORT_QR[] = {
    "#######.###.#..##..##.#######",
    "#.....#.#..##.##......#.....#",
    "#.###.#...#.####....#.#.###.#",
    "#.###.#.#..#....##.#..#.###.#",
    "#.###.#..#.###...#.#..#.###.#",
    "#.....#...#...#..####.#.....#",
    "#######.#.#.#.#.#.#.#.#######",
    "........#.##.##..##..........",
    "#.##.###...####.####..#..#.##",
    "...##....#..#..##########...#",
    "#.#..##....##.##.#...##.#.##.",
    "#####..#.##..###..#.#.###...#",
    ".#...#######....##.......##..",
    "####...#....##...#.#.##...###",
    "#....##.......#...#####.#.###",
    ".#.#.#...#.###.#...##..##..#.",
    "#.##.###....#.....####..##.#.",
    ".#........#.##.......#.#.###.",
    "#.##.##..##.#....#..#..##.#..",
    "..##...#.#.....###........#..",
    ".##..####.####.#.##########..",
    "........########..#.#...#####",
    "#######.###.##..#.###.#.##.#.",
    "#.....#.#.###...#.#.#...##.##",
    "#.###.#...##..##.#..#####.#.#",
    "#.###.#.####..#.#..##..###.#.",
    "#.###.#.#..#######..#..#..#.#",
    "#.....#.........#.#.#.#..#.#.",
    "#######.###.##.#.#.##.##.#.#.",
};
static void open_url(const char *url)
{
#ifdef _WIN32
    qw_open_url(url);
#else
    pid_t p = fork();                                /* two forks: the browser is not our child (no zombie) */
    if (p == 0) {
        setsid();
        if (fork() == 0) {
            execlp("xdg-open", "xdg-open", url, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (p > 0) {
        int st;
        (void)waitpid(p, &st, 0);                    /* returns at once: the middle process ends right away */
    }
#endif
}

void draw_about_dialog(int screen_w, int screen_h)
{
    GfxDialogBackdrop(70);
    const int w = 600, h = 620;
    const int x = (screen_w - w) / 2, y = (screen_h - h) / 2;
    Color bg = g_theme.panel_bg;
    bg.r = (unsigned char)(bg.r + 10); bg.g = (unsigned char)(bg.g + 10); bg.b = (unsigned char)(bg.b + 10);
    DrawRectangle(x, y, w, h, bg);
    DrawRectangleLinesEx((Rectangle){ (float)x, (float)y, (float)w, (float)h }, 2, g_theme.selection);
    DrawRectangle(x, y, w, 40, g_theme.selection);
    ui_text("About Q-LiM", x + 16, y + 10, 20, g_theme.bg);

    int ty = y + 58;
    ui_text("Q-LiM", x + 24, ty, 30, g_theme.text);
    char vb[160];
    snprintf(vb, sizeof vb, "Version %s   (built %s)", QLIM_VERSION, __DATE__);
    ui_text(vb, x + 24, ty + 40, 14, g_theme.text_dim);
    ui_text("Author: " QLIM_AUTHOR " (" QLIM_GITHUB ")", x + 24, ty + 66, 16, g_theme.text);
    ui_text(QLIM_COPYRIGHT, x + 24, ty + 90, 14, g_theme.text);
    ui_text("Free software: " QLIM_LICENSE ".", x + 24, ty + 112, 12, g_theme.text_dim);
    ui_text("Comes with ABSOLUTELY NO WARRANTY. Full text: the LICENSE file next to the program.", x + 24, ty + 128, 12, g_theme.text_dim);

    ty += 160;
    ui_text("Uses these libraries (each under its own license):", x + 24, ty, 13, g_theme.text);
    static const char *const libs[] = {
        "SDL2, SDL2_ttf - zlib license",
        "PortAudio - MIT license",
        "libsndfile (with mpg123, FLAC, Vorbis, Opus) - LGPL-2.1",
        "RtMidi - MIT-style license",
        "Rubber Band Library - GPL-2.0-or-later (commercial license available)",
        "PipeWire headers - MIT license",
        "Plugin hosting: VST3 interfaces, CLAP headers - MIT; LV2 headers, lilv - ISC",
        "DejaVu Sans font - Bitstream Vera / free license",
        "Inter Display font (the name at the top) - SIL Open Font License 1.1",
    };
    int nlibs = (int)(sizeof libs / sizeof libs[0]);
    for (int i = 0; i < nlibs; ++i)
        ui_text(libs[i], x + 36, ty + 22 + i * 18, 12, g_theme.text_dim);

    {   /* 2.0.0: support the project - entirely optional */
        int sy = ty + 22 + nlibs * 18 + 16;
        DrawLine(x + 24, sy - 8, x + w - 24, sy - 8, g_theme.grid_line);
        int nq = (int)(sizeof SUPPORT_QR / sizeof SUPPORT_QR[0]);
        int mod = 4, qz = 8, qs = nq * mod + 2 * qz;             /* modules of 4 px + a white quiet zone */
        int qx = x + w - 24 - qs, qy = sy;
        DrawRectangle(qx, qy, qs, qs, (Color){ 255, 255, 255, 255 });
        for (int r = 0; r < nq; ++r)
            for (int c = 0; SUPPORT_QR[r][c]; ++c)
                if (SUPPORT_QR[r][c] == '#') DrawRectangle(qx + qz + c * mod, qy + qz + r * mod, mod, mod, (Color){ 0, 0, 0, 255 });
        ui_text("Support Q-LiM", x + 24, sy, 16, g_theme.text);
        ui_text("Q-LiM is free and will stay free. If it is useful to you,", x + 24, sy + 26, 12, g_theme.text_dim);
        ui_text("you can support its development - only if you want to.", x + 24, sy + 42, 12, g_theme.text_dim);
        ui_text("Revolut (scan the code with your phone or click):", x + 24, sy + 66, 12, g_theme.text_dim);
        const char *lt = "revolut.me/georgiefgj";
        int lw = ui_text_w(lt, 15);
        Rectangle lr = { (float)(x + 24), (float)(sy + 84), (float)lw, 20 };
        bool lh = CheckCollisionPointRec(GetMousePosition(), lr);
        ui_text(lt, x + 24, sy + 84, 15, g_theme.selection);
        if (lh) {
            DrawLine(x + 24, sy + 103, x + 24 + lw, sy + 103, g_theme.selection);
            SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) open_url(QLIM_SUPPORT_URL);
        }
        ui_text("Thank you!", x + 24, sy + 112, 12, g_theme.text_dim);
    }

    Rectangle close_r = { (float)(x + w - 140), (float)(y + h - 52), 120, 36 };
    Vector2 m = GetMousePosition();
    bool hov = CheckCollisionPointRec(m, close_r);
    DrawRectangleRec(close_r, hov ? g_theme.selection : g_theme.panel_bg2);
    DrawRectangleLinesEx(close_r, 1, g_theme.text_dim);
    int tw = ui_text_w("Close", 16);
    ui_text("Close", (int)(close_r.x + (close_r.width - tw) / 2), (int)close_r.y + 9, 16, hov ? g_theme.bg : g_theme.text);
    bool outside = !CheckCollisionPointRec(m, (Rectangle){ (float)x, (float)y, (float)w, (float)h });
    if ((hov || outside) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_about_open = false;
    if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_ENTER)) g_about_open = false;
}

/* ============ 1.3.7: Edit -> Performance... ============
 * How the window is drawn and how often, so the sound always has room (see perf.h). Every change applies at once
 * and is saved (the drawing method: at the next start). */
static bool pl_button(Rectangle r, const char *label, int fs, bool active);
static void perf_check(int x, int y, bool *v, const char *label, const char *sub)
{
    Vector2 m = GetMousePosition();
    Rectangle box = { (float)x, (float)y, 20, 20 };
    Rectangle hit = { (float)x, (float)y, 560, 34 };
    bool hov = CheckCollisionPointRec(m, hit);
    DrawRectangleRec(box, *v ? g_theme.selection : g_theme.panel_bg2);
    DrawRectangleLinesEx(box, 1, hov ? g_theme.selection : g_theme.text_dim);
    if (*v) { DrawLineEx((Vector2){ x + 4.0f, y + 10.0f }, (Vector2){ x + 8.5f, y + 15.0f }, 2.2f, g_theme.bg);
              DrawLineEx((Vector2){ x + 8.5f, y + 15.0f }, (Vector2){ x + 16.0f, y + 5.0f }, 2.2f, g_theme.bg); }
    ui_text(label, x + 30, y + 2, 14, g_theme.text);
    if (sub) ui_text(sub, x + 30, y + 20, 11, g_theme.text_dim);
    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { *v = !*v; perf_save(); }
}

void draw_perf_dialog(int screen_w, int screen_h)
{
    GfxDialogBackdrop(70);
    PerfSettings *ps = perf_settings();
    const int w = 720, h = 700;
    const int x = (screen_w - w) / 2, y = (screen_h - h) / 2;
    Color bg = g_theme.panel_bg;
    bg.r = (unsigned char)(bg.r + 10); bg.g = (unsigned char)(bg.g + 10); bg.b = (unsigned char)(bg.b + 10);
    DrawRectangle(x, y, w, h, bg);
    DrawRectangleLinesEx((Rectangle){ (float)x, (float)y, (float)w, (float)h }, 2, g_theme.selection);
    DrawRectangle(x, y, w, 40, g_theme.selection);
    ui_text("Performance", x + 16, y + 10, 20, g_theme.bg);

    int lx = x + 24, cx = x + 190, ty = y + 62;
    /* 1.3.8: the interface */
    ui_text("Interface:", lx, ty + 7, 14, g_theme.text);
    static const char *in[] = { "Auto", "Normal", "Eco" };
    for (int i = 0; i < 3; ++i)
        if (pl_button((Rectangle){ (float)(cx + i * 150), (float)ty, 142, 30 }, in[i], 13, ps->eco == i) && ps->eco != i) {
            ps->eco = i; perf_save();
        }
    ui_text(GfxEco() ? "Now: Eco" : "Now: Normal", cx, ty + 36, 11, g_theme.text_dim);
    ui_text("Always: only the parts of the window that changed are drawn again. Eco saves even more: no highlights",
            lx, ty + 54, 11, g_theme.text_dim);
    ui_text("under the mouse, no soft edges, no blur, at most 30 frames/s. Auto = Eco when there is no graphics card.",
            lx, ty + 69, 11, g_theme.text_dim);
    ty += 104;
    /* drawing */
    ui_text("Drawing:", lx, ty + 7, 14, g_theme.text);
    static const char *dn[] = { "Auto", "Graphics card", "Light (CPU)" };
    for (int i = 0; i < 3; ++i) {
        Rectangle r = { (float)(cx + i * 150), (float)ty, 142, 30 };
        if (pl_button(r, dn[i], 13, ps->draw == i)) {
            if (ps->draw != i) { ps->draw = i; perf_save(); ui_flash_message("Drawing: applies the next time Q-LiM starts"); }
        }
    }
    char nb[220];
    snprintf(nb, sizeof nb, "Now: %s", GfxRendererDesc());
    ui_text(nb, cx, ty + 36, 11, g_theme.text_dim);
    ui_text("Auto = the graphics card, or the light drawing when there is no acceleration. Applies at the next start.",
            lx, ty + 54, 11, g_theme.text_dim);

    /* frames per second */
    ty += 86;
    ui_text("Frames per second:", lx, ty + 7, 14, g_theme.text);
    static const int fv[] = { 0, 60, 30, 20, 12 };
    static const char *fn[] = { "Auto", "60", "30", "20", "12" };
    for (int i = 0; i < 5; ++i) {
        Rectangle r = { (float)(cx + i * 90), (float)ty, 82, 30 };
        if (pl_button(r, fn[i], 13, ps->fps == fv[i]) && ps->fps != fv[i]) { ps->fps = fv[i]; perf_save(); }
    }
    ui_text("Auto = as many as fit in about 40 % of one CPU core (measured all the time); a number = at most that many.",
            lx, ty + 38, 11, g_theme.text_dim);

    /* 1.3.14: the waveform */
    ty += 66;
    ui_text("Waveform:", lx, ty + 7, 14, g_theme.text);
    static const char *wvn[] = { "Auto", "Smooth", "Light" };
    for (int i = 0; i < 3; ++i)
        if (pl_button((Rectangle){ (float)(cx + i * 150), (float)ty, 142, 30 }, wvn[i], 13, ps->wave == i) && ps->wave != i) {
            ps->wave = i; perf_save();
        }
    ui_text(perf_wave_light() ? "Now: light" : "Now: smooth", cx + 460, ty + 9, 11, g_theme.text_dim);
    ui_text("Light: in SCROLL the waveform moves 15 times per second, in whole pixels (less CPU). Auto = light with Eco.",
            lx, ty + 38, 11, g_theme.text_dim);

    /* switches */
    ty += 66;
    perf_check(lx, ty, &ps->idle_save, "Fewer frames while nothing moves (10 per second)",
               "The mouse or a key brings full speed back at once.");
    ty += 44;
    perf_check(lx, ty, &ps->sound_first, "Sound first",
               "When the sound breaks up, draw fewer frames for 30 seconds and say why.");
    ty += 44;
    {   bool was = ps->meters;
        perf_check(lx, ty, &ps->meters, "Level meters (Ctrl+Shift+U)",
                   "Off: only the dB scale stays next to the faders - nothing moves there.");
        (void)was; }
    ty += 44;
    perf_check(lx, ty, &ps->gentle, "Gentle on the graphics card (old laptops with USB sound)",
               "Changed pictures go to the graphics card in small pieces. Try it if USB sound clicks when the view changes.");
    ty += 44;
    ty -= 60;                               /* (1.3.12: the interface size moved to Edit > Appearance) */
    /* the state now */
    ty += 56;
    DrawLine(lx, ty, x + w - 24, ty, g_theme.grid_line);
    ty += 10;
    int cap = perf_max_fps_now();
    char sb[300];
    if (cap > 0) snprintf(sb, sizeof sb, "Right now: at most %d frames per second%s   |   drawing uses about %.0f %% of one CPU core",
                          cap, perf_slowed_for_sound() ? " (sound first)" : "", 100.0f * perf_ui_load());
    else snprintf(sb, sizeof sb, "Right now: full frame rate   |   drawing uses about %.0f %% of one CPU core", 100.0f * perf_ui_load());
    ui_text(sb, lx, ty, 12, g_theme.text);
    unsigned c, d, wp, ov;
    perf_problem_counts(&c, &d, &wp, &ov);
    snprintf(sb, sizeof sb, "Sound dropouts since the start:  sound card %u   |   disk %u   |   WARP %u   |   processing over 100 %% %u",
             c, d, wp, ov);
    ui_text(sb, lx, ty + 20, 12, (c || d || wp || ov) ? (Color){ 235, 120, 90, 255 } : g_theme.text_dim);

    Rectangle close_r = { (float)(x + w - 140), (float)(y + h - 52), 120, 36 };
    bool close = pl_button(close_r, "Close", 16, false);
    Vector2 m = GetMousePosition();
    bool outside = !CheckCollisionPointRec(m, (Rectangle){ (float)x, (float)y, (float)w, (float)h });
    if (close || (outside && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) || IsKeyPressed(KEY_ESCAPE)) g_perf_open = false;
}

/* ============ 2.0: Collect All & Save - what the compressed files become ============
 * Compressed files (MP3, FLAC, Ogg, M4A ...) are decoded while they play - on an old computer that costs CPU. The
 * collected copy can be a file that is only read (WAV), at the project's sample rate (no live conversion either). */
void draw_collect_dialog(int screen_w, int screen_h)
{
    GfxDialogBackdrop(70);
    const int w = 840, h = 470;
    const int x = (screen_w - w) / 2, y = (screen_h - h) / 2;
    Color bg = g_theme.panel_bg;
    bg.r = (unsigned char)(bg.r + 10); bg.g = (unsigned char)(bg.g + 10); bg.b = (unsigned char)(bg.b + 10);
    DrawRectangle(x, y, w, h, bg);
    DrawRectangleLinesEx((Rectangle){ (float)x, (float)y, (float)w, (float)h }, 2, g_theme.selection);
    DrawRectangle(x, y, w, 40, g_theme.selection);
    ui_text("Collect All & Save", x + 16, y + 10, 20, g_theme.bg);
    int lx = x + 24, ty = y + 58;
    char t[200];
    snprintf(t, sizeof t, "%d clip%s use compressed files (MP3, FLAC, Ogg, M4A ...).", g_collect_n, g_collect_n == 1 ? "" : "s");
    ui_text(t, lx, ty, 15, g_theme.text);
    ui_text("They are decoded while they play. The collected copies can be files that are only read - less CPU.",
            lx, ty + 22, 12, g_theme.text_dim);
    ui_text("The copies are made at the project's sample rate, so nothing is converted while playing either.",
            lx, ty + 38, 12, g_theme.text_dim);
    ty += 70;
    static const char *nm[4] = { "Keep them as they are", "WAV, 32-bit float", "WAV, 24-bit", "FLAC, 24-bit" };
    static const char *sub[4] = {
        "Copied unchanged - the smallest folder, decoded while playing (as now).",
        "Not one bit lost, no decoding at all - the lightest to play. About 23 MB per minute.",
        "No audible difference (144 dB), no decoding. About 17 MB per minute. Overs above 0 dB are cut.",
        "Lossless, about half the size of WAV; decoding is much lighter than MP3's. Overs above 0 dB are cut.",
    };
    static const int val[4] = { WAV_OUT_KEEP, WAV_OUT_FLOAT, WAV_OUT_PCM24, WAV_OUT_FLAC24 };
    for (int i = 0; i < 4; ++i) {                     /* choose one, then Collect */
        Rectangle r = { (float)lx, (float)ty, 230, 40 };
        if (pl_button(r, nm[i], 15, g_collect_sel == val[i])) g_collect_sel = val[i];
        ui_text(sub[i], lx + 244, ty + 13, 12, g_collect_sel == val[i] ? g_theme.text : g_theme.text_dim);
        ty += 52;
    }
    ty += 8;
    ui_text("Tip: WAV, 32-bit float for an old computer; the folder can be big (a 30-minute track ~ 700 MB).",
            lx, ty, 12, g_theme.text);
    ui_text("Files that are already WAV / AIFF are copied as they are.", lx, ty + 18, 12, g_theme.text_dim);
    Rectangle ok_r = { (float)(x + w - 280), (float)(y + h - 52), 130, 36 };
    Rectangle cancel_r = { (float)(x + w - 140), (float)(y + h - 52), 120, 36 };
    bool ok = pl_button(ok_r, "Collect", 16, true);
    bool cancel = pl_button(cancel_r, "Cancel", 16, false);
    if (ok || IsKeyPressed(KEY_ENTER)) { g_collect_choice = g_collect_sel; g_collect_open = false; }
    else if (cancel || IsKeyPressed(KEY_ESCAPE)) { g_collect_choice = -1; g_collect_open = false; }
}

/* 2.0: while Collect All & Save runs: which clip, how far, a bar */
void draw_collect_progress(int screen_w, int screen_h)
{
    GfxDialogBackdrop(70);
    const int w = 620, h = 170;
    const int x = (screen_w - w) / 2, y = (screen_h - h) / 2;
    Color bg = g_theme.panel_bg;
    bg.r = (unsigned char)(bg.r + 10); bg.g = (unsigned char)(bg.g + 10); bg.b = (unsigned char)(bg.b + 10);
    DrawRectangle(x, y, w, h, bg);
    DrawRectangleLinesEx((Rectangle){ (float)x, (float)y, (float)w, (float)h }, 2, g_theme.selection);
    DrawRectangle(x, y, w, 40, g_theme.selection);
    ui_text("Collect All & Save", x + 16, y + 10, 20, g_theme.bg);
    int done = 0, total = 0, pm = 0;
    char name[256];
    project_collect_progress(&done, &total, &pm, name, sizeof name);
    if (total < 1) total = 1;
    if (done > total) done = total;
    double f = ((double)done + (done < total ? pm / 1000.0 : 0.0)) / (double)total;
    char t[400];
    snprintf(t, sizeof t, "Clip %d of %d:  %s", done < total ? done + 1 : total, total, name);
    ui_text(t, x + 24, y + 58, 14, g_theme.text);
    Rectangle bar = { (float)(x + 24), (float)(y + 88), (float)(w - 48), 22 };
    DrawRectangleRec(bar, g_theme.panel_bg2);
    DrawRectangle((int)bar.x, (int)bar.y, (int)(bar.width * (float)f + 0.5f), (int)bar.height, g_theme.selection);
    DrawRectangleLinesEx(bar, 1, g_theme.text_dim);
    snprintf(t, sizeof t, "%d %%   -   the sound keeps playing; this window closes by itself", (int)(f * 100.0 + 0.5));
    ui_text(t, x + 24, y + 124, 12, g_theme.text_dim);
}

/* ============ 1.4.11: Edit -> Loudness... ============
 * Loudness normalization like the streaming services: every audio clip plays at the same loudness (LUFS). */
void draw_loud_dialog(int screen_w, int screen_h)
{
    GfxDialogBackdrop(70);
    Session *s = g_ui_session;
    const int w = 700, h = 430;
    const int x = (screen_w - w) / 2, y = (screen_h - h) / 2;
    Color bg = g_theme.panel_bg;
    bg.r = (unsigned char)(bg.r + 10); bg.g = (unsigned char)(bg.g + 10); bg.b = (unsigned char)(bg.b + 10);
    DrawRectangle(x, y, w, h, bg);
    DrawRectangleLinesEx((Rectangle){ (float)x, (float)y, (float)w, (float)h }, 2, g_theme.selection);
    DrawRectangle(x, y, w, 40, g_theme.selection);
    ui_text("Loudness", x + 16, y + 10, 20, g_theme.bg);

    int lx = x + 24, ty = y + 60;
    ui_text("Normalize every clip to the same loudness, like Spotify, YouTube or Apple Music do.", lx, ty, 14, g_theme.text);
    ty += 34;
    static const int tv[] = { 0, -14, -16, -23 };
    static const char *tn[] = { "Off", "-14 LUFS", "-16 LUFS", "-23 LUFS" };
    static const char *tsub[] = { "as in the files", "Spotify, YouTube, Tidal", "Apple Music", "TV / radio (EBU R128)" };
    int cur = s ? s->loud_target : 0;
    for (int i = 0; i < 4; ++i) {
        Rectangle r = { (float)(lx + i * 164), (float)ty, 156, 34 };
        if (pl_button(r, tn[i], 15, cur == tv[i]) && cur != tv[i] && s) {
            session_set_loudness(s, tv[i], s->loud_down_only);
            settings_save_loudness(s->loud_target, s->loud_down_only);
        }
        ui_text(tsub[i], (int)(r.x + (r.width - ui_text_w(tsub[i], 11)) / 2), ty + 40, 11, g_theme.text_dim);
    }
    ty += 72;
    if (s) {
        bool d = s->loud_down_only;
        perf_check(lx, ty, &d, "Only turn loud clips down (like YouTube)",
                   "off: quiet clips are turned up too (like Spotify) - but never so far that the highest sample passes -1 dBFS");
        if (d != s->loud_down_only) { session_set_loudness(s, s->loud_target, d); settings_save_loudness(s->loud_target, d); }
    }
    ty += 52;
    ui_text("The loudness of every clip is measured once in the background (ITU-R BS.1770 / EBU R128, like the", lx, ty, 12, g_theme.text_dim);
    ui_text("streaming services). The clip's own Gain is added on top. Off = the sound is not touched at all.", lx, ty + 16, 12, g_theme.text_dim);
    ty += 48;
    /* the selected clip */
    float lu, pk, nd;
    char info[200];
    if (s && g_selected_ch >= 0 && g_selected_scene >= 0 && session_clip_loudness(s, g_selected_ch, g_selected_scene, &lu, &pk, &nd)) {
        if (isfinite(lu))
            snprintf(info, sizeof info, "Selected clip: %.1f LUFS, highest sample %.1f dBFS  ->  %s%+.1f dB", (double)lu, (double)pk,
                     s->loud_target ? "normalized " : "(off) ", (double)nd);
        else snprintf(info, sizeof info, "Selected clip: silence");
    } else snprintf(info, sizeof info, "Selected clip: - (select an audio clip; it is measured when its waveform is ready)");
    ui_text(info, lx, ty, 13, g_theme.text);

    Rectangle close_r = { (float)(x + w - 140), (float)(y + h - 52), 120, 36 };
    bool close = pl_button(close_r, "Close", 16, false);
    Vector2 m = GetMousePosition();
    bool outside = !CheckCollisionPointRec(m, (Rectangle){ (float)x, (float)y, (float)w, (float)h });
    if (close || (outside && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) || IsKeyPressed(KEY_ESCAPE)) g_loud_open = false;
}

/* ============ Edit -> Plugins... (step 34) ============ */
static int   g_pl_tab = 0;                 /* 0 = available, 1 = problems, 2 = search locations, 3 = safety (2.0.0) */
static float g_pl_scroll[4];

static bool pl_button(Rectangle r, const char *label, int fs, bool active)
{
    Vector2 m = GetMousePosition();
    bool hov = CheckCollisionPointRec(m, r);
    DrawRectangleRec(r, active ? g_theme.selection : (hov ? g_theme.cell_hover : g_theme.panel_bg2));
    DrawRectangleLinesEx(r, 1, active || hov ? g_theme.selection : g_theme.grid_line);
    int tw = ui_text_w(label, fs);
    ui_text(label, (int)(r.x + (r.width - tw) / 2), (int)(r.y + (r.height - fs) / 2), fs, active ? g_theme.bg : g_theme.text);
    return hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

static void pl_fit(char *buf, size_t sz, const char *src, int fs, float maxw)
{
    snprintf(buf, sz, "%s", src);
    if (ui_text_w(buf, fs) <= maxw) return;
    /* long path: keep the end ("...folder/file") */
    const char *p = src;
    char t[700];
    while (*p) {
        ++p;
        snprintf(t, sizeof t, "...%s", p);
        if (ui_text_w(t, fs) <= maxw) break;
    }
    snprintf(buf, sz, "%s", t);
}

void draw_plugins_dialog(int screen_w, int screen_h)
{
    GfxDialogBackdrop(70);
    int w = 820, h = 600;
    if (w > screen_w - 40) w = screen_w - 40;
    if (h > screen_h - 40) h = screen_h - 40;
    const int x = (screen_w - w) / 2, y = (screen_h - h) / 2;
    Color bg = g_theme.panel_bg;
    bg.r = (unsigned char)(bg.r + 10); bg.g = (unsigned char)(bg.g + 10); bg.b = (unsigned char)(bg.b + 10);
    DrawRectangle(x, y, w, h, bg);
    DrawRectangleLinesEx((Rectangle){ (float)x, (float)y, (float)w, (float)h }, 2, g_theme.selection);
    DrawRectangle(x, y, w, 40, g_theme.selection);
    ui_text("Plugins", x + 16, y + 10, 20, g_theme.bg);
    Vector2 m = GetMousePosition();

    int npl = plugins_count(), npr = plugins_problem_count();
    char t0[48], t1[48];
    snprintf(t0, sizeof t0, "Available (%d)", npl);
    snprintf(t1, sizeof t1, "Problems (%d)", npr);
    const char *tabs[4] = { t0, t1, "Scan locations", "Safety" };
    float tx = (float)x + 16;
    for (int i = 0; i < 4; ++i) {
        float tw = (float)ui_text_w(tabs[i], 14) + 28;
        if (pl_button((Rectangle){ tx, (float)y + 52, tw, 28 }, tabs[i], 14, g_pl_tab == i)) g_pl_tab = i;
        tx += tw + 8;
    }
    if (npr > 0) {                                   /* red dot on "Problems" */
        float px = (float)x + 16 + (float)ui_text_w(t0, 14) + 28 + 8 + (float)ui_text_w(t1, 14) + 28 - 6;
        DrawCircleV((Vector2){ px, (float)y + 56 }, 4, FX_FAIL_COL);
    }

    Rectangle area = { (float)x + 16, (float)y + 92, (float)w - 32, (float)h - 92 - 64 };
    DrawRectangleRec(area, g_theme.bg);
    DrawRectangleLinesEx(area, 1, g_theme.grid_line);
    float *scroll = &g_pl_scroll[g_pl_tab];
    if (CheckCollisionPointRec(m, area)) {
        float wh = GetMouseWheelMove();
        if (wh != 0.0f) *scroll -= wh * 60.0f;
    }
    float content_h = 0;
    float cy = area.y + 6 - *scroll;
    BeginScissorMode((int)area.x + 1, (int)area.y + 1, (int)area.width - 2, (int)area.height - 2);
    if (g_pl_tab == 0) {
        if (npl == 0) {
            ui_text(plugins_scanning(NULL, NULL, NULL, 0) ? "Checking plugins..." : "No usable plugins found.",
                    (int)area.x + 12, (int)cy + 4, 14, g_theme.text);
            ui_text("Check 'Scan locations' (the folders Q-LiM looks in) and 'Problems'.", (int)area.x + 12, (int)cy + 26, 12, g_theme.text_dim);
            cy += 50;
        }
        const char *last = "";
        for (int i = 0; i < npl; ++i) {
            const PluginInfo *pi = plugins_get(i);
            if (strcmp(pi->fmt, last) != 0) {
                last = pi->fmt;
                int cnt = 0;
                for (int k = i; k < npl && !strcmp(plugins_get(k)->fmt, last); ++k) ++cnt;
                char hd[48];
                snprintf(hd, sizeof hd, "%s  (%d)", plugins_fmt_name(plugins_fmt_index(last)), cnt);
                if (i > 0) cy += 6;
                ui_text(hd, (int)area.x + 10, (int)cy + 4, 14, g_theme.selection);
                cy += 24;
            }
            if (cy > area.y - 24 && cy < area.y + area.height) {
                Rectangle row = { area.x + 4, cy, area.width - 8, 22 };
                if (CheckCollisionPointRec(m, row) && CheckCollisionPointRec(m, area)) DrawRectangleRec(row_hl(row), g_theme.cell_hover);
                char nm[PLUGIN_NAME_LEN];
                pl_fit(nm, sizeof nm, pi->name, 13, area.width * 0.38f);
                ui_text(nm, (int)area.x + 24, (int)cy + 4, 13, g_theme.text);
                char pth[PLUGIN_PATH_LEN];
                char only[PLUGIN_PATH_LEN];
                snprintf(only, sizeof only, "%s", pi->path);
                char *bar = strchr(only, '|');
                if (bar) *bar = 0;
                pl_fit(pth, sizeof pth, only, 11, area.width * 0.55f);
                ui_text(pth, (int)(area.x + area.width * 0.42f), (int)cy + 6, 11, g_theme.text_dim);
            }
            cy += 22;
        }
    } else if (g_pl_tab == 1) {
        if (npr == 0) { ui_text("No problems - every plugin that was found works.", (int)area.x + 12, (int)cy + 4, 14, g_theme.text); cy += 30; }
        for (int i = 0; i < npr; ++i) {
            const PluginProblem *pp = plugins_problem_get(i);
            if (cy > area.y - 60 && cy < area.y + area.height) {
                char nm[PLUGIN_NAME_LEN + 16];
                snprintf(nm, sizeof nm, "%s", pp->name);
                ui_text(nm, (int)area.x + 12, (int)cy + 4, 13, g_theme.text);
                const char *fn = plugins_fmt_name(plugins_fmt_index(pp->fmt));
                int fw = ui_text_w(fn, 11);
                ui_text(fn, (int)(area.x + area.width - fw - 12), (int)cy + 6, 11, g_theme.text_dim);
                bool inst = strstr(pp->reason, "instrument") != NULL;
                ui_text(pp->reason, (int)area.x + 24, (int)cy + 22, 12, inst ? g_theme.text_dim : FX_FAIL_COL);
                char only[PLUGIN_PATH_LEN], pth[PLUGIN_PATH_LEN];
                snprintf(only, sizeof only, "%s", pp->path);
                char *bar = strchr(only, '|');
                if (bar) *bar = 0;
                pl_fit(pth, sizeof pth, only, 11, area.width - 40);
                ui_text(pth, (int)area.x + 24, (int)cy + 38, 11, g_theme.text_dim);
            }
            cy += 58;
        }
    } else if (g_pl_tab == 3) {
        /* 2.0.0: plugins in a separate process - a plugin that crashes or freezes cannot take Q-LiM with it */
        int tx0 = (int)area.x + 14;
        ui_text("Plugins in a separate process", tx0, (int)cy + 6, 16, g_theme.selection);
        cy += 30;
        ui_text("A plugin in a separate process cannot crash or freeze Q-LiM: if it fails, only that plugin goes silent.", tx0, (int)cy, 12, g_theme.text);
        cy += 18;
        ui_text("It costs a little more CPU per plugin, so by default only the plugins that need it run like that.", tx0, (int)cy, 12, g_theme.text_dim);
        cy += 30;
        static const char *mn[3] = { "Never", "Automatic (recommended)", "All plugins" };
        static const char *md[3] = {
#ifdef _WIN32
            "Every plugin runs inside Q-LiM (the lightest).",
            "Plugins that crashed or froze Q-LiM before, and the ones you choose.",
#else
            "Every plugin runs inside Q-LiM (the lightest; LV2 plugins with Gtk windows cannot open them).",
            "Plugins that crashed or froze Q-LiM before, the ones you choose, and LV2 plugins with Gtk windows.",
#endif
            "Every plugin in its own process (the safest; a bit more CPU - for a big live show with many plugins)." };
        int mode = qplug_separate_mode();
        float bx = area.x + 14;
        for (int i = 0; i < 3; ++i) {
            float bw = (float)ui_text_w(mn[i], 14) + 30;
            if (pl_button((Rectangle){ bx, cy, bw, 30 }, mn[i], 14, mode == i) && CheckCollisionPointRec(m, area) && mode != i) {
                if (g_ui_session) fxchain_keep_states(g_ui_session);
                qplug_set_separate_mode(i);
            }
            bx += bw + 8;
        }
        cy += 38;
        ui_text(md[mode], tx0, (int)cy, 12, g_theme.text);
        cy += 30;
        int ns = qplug_separate_count();
        char hd[80];
        snprintf(hd, sizeof hd, "Plugins that always run in a separate process (%d)", ns);
        ui_text(hd, tx0, (int)cy, 14, g_theme.selection);
        cy += 24;
        if (ns == 0) { ui_text("None yet. Q-LiM adds a plugin here by itself if it ever crashes or freezes Q-LiM;", tx0 + 10, (int)cy, 12, g_theme.text_dim);
                       cy += 18;
                       ui_text("you can also add one: right click on the plugin -> 'Run in a separate process'.", tx0 + 10, (int)cy, 12, g_theme.text_dim);
                       cy += 22; }
        for (int i = 0; i < ns; ++i) {
            char f[8], id[1300], why[80];
            if (!qplug_separate_get(i, f, sizeof f, id, sizeof id, why, sizeof why)) continue;
            Rectangle row = { area.x + 4, cy, area.width - 8, 38 };
            bool hov = CheckCollisionPointRec(m, row) && CheckCollisionPointRec(m, area);
            if (hov) DrawRectangleRec(row_hl(row), g_theme.cell_hover);
            char only[1300], base[300], nm[400], pth[400];
            snprintf(only, sizeof only, "%s", id);
            char *bar = strchr(only, '|');
            if (bar) *bar = 0;
            size_t ol = strlen(only);
            while (ol > 1 && only[ol - 1] == '/') only[--ol] = 0;
            const char *sl = strrchr(only, '/');
            snprintf(base, sizeof base, "%s", sl ? sl + 1 : only);
            char *dot = strrchr(base, '.');
            if (dot && dot != base) *dot = 0;                       /* "vitalium.lv2" -> "vitalium" */
            snprintf(nm, sizeof nm, "%s  (%s)  -  %s", base, plugins_fmt_name(plugins_fmt_index(f)), why);
            pl_fit(pth, sizeof pth, only, 11, area.width - 80);
            ui_text(nm, tx0 + 10, (int)cy + 3, 13, g_theme.text);
            ui_text(pth, tx0 + 10, (int)cy + 21, 11, g_theme.text_dim);
            Rectangle xr = { area.x + area.width - 36, cy + 8, 24, 20 };
            bool xh = hov && CheckCollisionPointRec(m, xr);
            if (hov) {
                DrawRectangleLinesEx(xr, 1, xh ? FX_FAIL_COL : g_theme.grid_line);
                int xw = ui_text_w("x", 13);
                ui_text("x", (int)(xr.x + (xr.width - xw) / 2), (int)xr.y + 2, 13, xh ? FX_FAIL_COL : g_theme.text_dim);
            }
            if (xh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if (g_ui_session) fxchain_keep_states(g_ui_session);
                qplug_separate_set(f, id, false, NULL);
                break;
            }
            cy += 40;
        }
    } else {
        for (int pf = 0; pf < PF_COUNT; ++pf) {
#ifdef _WIN32
            if (pf == PF_LV2) continue;                          /* no LV2 on Windows */
#endif
            if (pf > 0) cy += 8;
            ui_text(plugins_fmt_name(pf), (int)area.x + 10, (int)cy + 6, 15, g_theme.selection);
            Rectangle add = { area.x + area.width - 228, cy + 2, 120, 24 };
            Rectangle def = { area.x + area.width - 100, cy + 2, 88, 24 };
            if (pl_button(add, "+ Add folder", 12, false) && CheckCollisionPointRec(m, area)) {
                char dir[PLUGIN_PATH_LEN];
                GfxExternalDialogBegin();
                int r = plugins_ask_folder(dir, (int)sizeof dir);
                GfxExternalDialogEnd();
                if (r == 0) { plugins_loc_add(pf, dir); plugins_rescan(false); }
            }
            if (pl_button(def, "Defaults", 12, false) && CheckCollisionPointRec(m, area)) { plugins_loc_reset(pf); plugins_rescan(false); }
            cy += 30;
            int nl = plugins_loc_count(pf);
            if (nl == 0) { ui_text("(no folders - this format is not scanned)", (int)area.x + 24, (int)cy + 3, 12, g_theme.text_dim); cy += 22; }
            for (int i = 0; i < nl; ++i) {
                Rectangle row = { area.x + 4, cy, area.width - 8, 22 };
                bool hov = CheckCollisionPointRec(m, row) && CheckCollisionPointRec(m, area);
                if (hov) DrawRectangleRec(row_hl(row), g_theme.cell_hover);
                bool ex = plugins_loc_exists(pf, i);
                char lb[PLUGIN_PATH_LEN + 20];
                snprintf(lb, sizeof lb, "%s%s", plugins_loc_get(pf, i), ex ? "" : "   (folder does not exist)");
                ui_text(lb, (int)area.x + 24, (int)cy + 4, 13, ex ? g_theme.text : g_theme.text_dim);
                Rectangle xr = { area.x + area.width - 36, cy + 1, 24, 20 };
                bool xh = hov && CheckCollisionPointRec(m, xr);
                if (hov) {
                    DrawRectangleLinesEx(xr, 1, xh ? FX_FAIL_COL : g_theme.grid_line);
                    int xw = ui_text_w("x", 13);
                    ui_text("x", (int)(xr.x + (xr.width - xw) / 2), (int)xr.y + 2, 13, xh ? FX_FAIL_COL : g_theme.text_dim);
                }
                if (xh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { plugins_loc_remove(pf, i); plugins_rescan(false); break; }
                cy += 22;
            }
        }
        cy += 8;
#ifdef _WIN32
        ui_text("Also used: the folders in the VST3_PATH, VST_PATH and CLAP_PATH environment variables (separated by ;).",
#else
        ui_text("Also used: the folders in the LV2_PATH, VST3_PATH, VST_PATH and CLAP_PATH environment variables.",
#endif
                (int)area.x + 10, (int)cy + 4, 11, g_theme.text_dim);
        cy += 24;
    }
    EndScissorMode();
    content_h = cy + *scroll - (area.y + 6);
    float maxs = content_h - area.height + 12;
    if (maxs < 0) maxs = 0;
    if (*scroll > maxs) *scroll = maxs;
    if (*scroll < 0) *scroll = 0;

    /* bottom: status, Rescan, Close */
    int done = 0, total = 0;
    char cur[200] = "";
    bool scanning = plugins_scanning(&done, &total, cur, sizeof cur);
    float by = (float)(y + h - 50);
    char st[300];
    if (scanning) snprintf(st, sizeof st, "Checking %d / %d   %s", done, total, cur);
    else snprintf(st, sizeof st, "%d usable, %d with problems. Each plugin is tested in a separate process.", npl, npr);
    char stf[300];
    pl_fit(stf, sizeof stf, st, 12, (float)w - 32 - 380);
    ui_text(stf, x + 18, (int)by + 12, 12, g_theme.text_dim);
    if (pl_button((Rectangle){ (float)(x + w - 372), by, 100, 36 }, scanning ? "Scanning..." : "Rescan", 14, false) && !scanning)
        plugins_rescan(false);
    if (pl_button((Rectangle){ (float)(x + w - 264), by, 120, 36 }, "Full rescan", 14, false) && !scanning)
        plugins_rescan(true);
    bool close = pl_button((Rectangle){ (float)(x + w - 136), by, 120, 36 }, "Close", 16, false);
    bool outside = !CheckCollisionPointRec(m, (Rectangle){ (float)x, (float)y, (float)w, (float)h });
    if (close || (outside && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) || IsKeyPressed(KEY_ESCAPE)) g_plugins_open = false;
}

void draw_appearance_dialog(int screen_w, int screen_h)
{
    if (!g_appearance_open) return;

    GfxDialogBackdrop(70);

    const int dlg_w = 700;
    const int dlg_h = 345 + 34 + 34 + 40;                  /* +40: the themes are on two rows; +70: the interface size (1.3.12) */
    const int dlg_x = (screen_w - dlg_w) / 2;
    const int dlg_y = (screen_h - dlg_h) / 2;

    double t = GetTime() - g_appearance_open_time;
    float alpha = 1.0f;
    if (t < 0.15) alpha = (float)(t / 0.15);
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;

    Color bg = g_theme.panel_bg;
    bg.r = (unsigned char)(bg.r + 10);
    bg.g = (unsigned char)(bg.g + 10);
    bg.b = (unsigned char)(bg.b + 10);
    bg.a = (unsigned char)(bg.a * alpha);
    DrawRectangle(dlg_x, dlg_y, dlg_w, dlg_h, bg);

    Color border = g_theme.selection;
    border.a = (unsigned char)(border.a * alpha);
    DrawRectangleLinesEx((Rectangle){ dlg_x, dlg_y, dlg_w, dlg_h }, 2, border);

    DrawRectangle(dlg_x, dlg_y, dlg_w, 40,
                  (Color){ g_theme.selection.r,
                           g_theme.selection.g,
                           g_theme.selection.b,
                           (unsigned char)(255 * alpha) });

    Color title_col = g_theme.bg;
    title_col.a = (unsigned char)(title_col.a * alpha);
    ui_text("Appearance", dlg_x + 16, dlg_y + 10, 20, title_col);

    int y = dlg_y + 60;
    int x = dlg_x + 20;

    Vector2 mouse = GetMousePosition();

    // === Theme ===
    ui_text("Theme:", x, y + 6, 14, g_theme.text);

    const char *theme_names[] = {
        "Dark", "Light", "Midnight", "Solarized", "OLED Black", "Nord", "Gruvbox",
        "Graphite", "Slate", "Aurora", "Paper", "Ember"
    };
    const char *theme_ids[] = {
        "dark", "light", "midnight", "solarized", "oled", "nord", "gruvbox",
        "graphite", "slate", "aurora", "paper", "ember"
    };
    int n_themes = 12;

    float btn_w = 100;
    float btn_h = 28;
    float btn_gap = 6;

    for (int i = 0; i < n_themes; ++i) {
        float bx = x + 80 + (i % 4) * (btn_w + btn_gap);       /* 4 per row */
        float by = y + (i / 4) * (btn_h + 6);
        Rectangle r = { bx, by, btn_w, btn_h };

        bool is_active = (strcmp(g_theme_name, theme_ids[i]) == 0);

        Color b = is_active ? g_theme.selection : g_theme.panel_bg2;
        DrawRectangleRec(r, b);
        DrawRectangleLinesEx(r, 1, g_theme.text_dim);

        Color tc = is_active ? g_theme.bg : g_theme.text;
        int tw = ui_text_w(theme_names[i], 13);
        ui_text(theme_names[i],
                (int)(bx + (btn_w - tw) / 2),
                (int)(by + (btn_h - 13) / 2),
                13, tc);

        if (CheckCollisionPointRec(mouse, r) &&
            IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            theme_apply_preset((ThemePreset)i);
            if (strcmp(g_accent_name, "theme") != 0)              /* step 36g: the chosen accent stays */
                theme_apply_accent(theme_accent_from_name(g_accent_name));
        }
    }

    y += 3 * (btn_h + 6) + 6;

    {   /* 2.0.0: the look of cells and headers */
        ui_text("Style:", x, y + 6, 14, g_theme.text);
        const char *sn[2] = { "Classic", "Modern" };
        for (int i = 0; i < 2; ++i) {
            if (pl_button((Rectangle){ x + 80 + i * 106.0f, (float)y, 100, btn_h }, sn[i], 13, g_ui_style == i)) g_ui_style = i;
        }
        ui_text("rounder, colour strips, tinted clips",
                (int)(x + 300), y + 7, 12, g_theme.text_dim);
        y += (int)btn_h + 12;
        ui_text("Clip time:", x, y + 6, 14, g_theme.text);
        const char *tn[3] = { "Counter", "Line", "Both" };
        for (int i = 0; i < 3; ++i) {
            if (pl_button((Rectangle){ x + 80 + i * 106.0f, (float)y, 100, btn_h }, tn[i], 13, g_clip_time == i)) g_clip_time = i;
        }
        ui_text("how a playing clip shows where it is", (int)(x + 406), y + 7, 12, g_theme.text_dim);
        y += (int)btn_h + 12;
    }

    // === Accent ===
    ui_text("Accent:", x, y + 6, 14, g_theme.text);

    const char *accent_names[] = {
        "Yellow", "Blue", "Green", "Red", "Purple", "Orange"
    };
    Color accent_preview[] = {
        { 255, 230, 90, 255 },
        { 90, 170, 255, 255 },
        { 100, 220, 130, 255 },
        { 240, 90, 90, 255 },
        { 190, 130, 240, 255 },
        { 255, 163, 26, 255 },
    };
    int n_accents = 6;

    btn_w = 80;
    for (int i = 0; i < n_accents; ++i) {
        float bx = x + 80 + i * (btn_w + btn_gap);
        Rectangle r = { bx, y, btn_w, btn_h };

        bool is_active = (strcmp(g_accent_name,
            (i==0?"yellow":i==1?"blue":i==2?"green":
             i==3?"red":i==4?"purple":"orange")) == 0);

        Color b = accent_preview[i];
        DrawRectangleRec(r, b);
        DrawRectangleLinesEx(r, is_active ? 3 : 1,
                             is_active ? g_theme.text : g_theme.text_dim);

        Color tc = g_theme.bg;
        int tw = ui_text_w(accent_names[i], 12);
        ui_text(accent_names[i],
                (int)(bx + (btn_w - tw) / 2),
                (int)(y + (btn_h - 12) / 2),
                12, tc);

        if (CheckCollisionPointRec(mouse, r) &&
            IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            theme_apply_accent((AccentPreset)i);
        }
    }

    y += btn_h + 12;

    // === Font size ===
    ui_text("Font size:", x, y + 6, 14, g_theme.text);

    char fsize_buf[16];
    snprintf(fsize_buf, sizeof(fsize_buf), "%d", g_font_size);
    ui_text(fsize_buf, x + 200, y + 6, 14, g_theme.selection);

    float fs_slider_x = x + 80;
    float fs_slider_w = 400;
    Rectangle fs_track = { fs_slider_x, y + 6, fs_slider_w, 18 };
    DrawRectangleRec(fs_track, g_theme.bg);
    DrawRectangleLinesEx(fs_track, 1, g_theme.text_dim);

    float fs_fill = (float)(g_font_size - 8) / (32.0f - 8.0f);
    Rectangle fs_fill_r = { fs_slider_x, y + 6, fs_slider_w * fs_fill, 18 };
    DrawRectangleRec(fs_fill_r, g_theme.selection);

    // - and + buttons
    Rectangle minus_r = { fs_slider_x + fs_slider_w + 10, y + 4, 26, 22 };
    Rectangle plus_r  = { fs_slider_x + fs_slider_w + 40, y + 4, 26, 22 };

    DrawRectangleRec(minus_r, g_theme.panel_bg2);
    DrawRectangleLinesEx(minus_r, 1, g_theme.text_dim);
    ui_text("-", (int)(minus_r.x + 9), (int)(minus_r.y + 3), 16, g_theme.text);

    DrawRectangleRec(plus_r, g_theme.panel_bg2);
    DrawRectangleLinesEx(plus_r, 1, g_theme.text_dim);
    ui_text("+", (int)(plus_r.x + 7), (int)(plus_r.y + 3), 16, g_theme.text);

    if (CheckCollisionPointRec(mouse, fs_track) &&
        IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        float rel = (mouse.x - fs_slider_x) / fs_slider_w;
        if (rel < 0) rel = 0;
        if (rel > 1) rel = 1;
        g_font_size = 8 + (int)(rel * 24);
        if (g_font_size < 8) g_font_size = 8;
        if (g_font_size > 32) g_font_size = 32;
    }
    if (CheckCollisionPointRec(mouse, minus_r) &&
        IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        g_font_size--;
        if (g_font_size < 8) g_font_size = 8;
    }
    if (CheckCollisionPointRec(mouse, plus_r) &&
        IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        g_font_size++;
        if (g_font_size > 32) g_font_size = 32;
    }

    y += 40;

    {   /* 1.3.12 (from Performance): the size of the interface */
        float z = perf_settings()->zoom;
        ui_text("Interface size:", x, y + 7, 14, g_theme.text);
        float zx = x + 130;
        if (pl_button((Rectangle){ zx, (float)y, 40, 30 }, "-", 16, false)) ui_zoom_step(-1);
        char zb[32]; snprintf(zb, sizeof zb, "%.0f %%", z * 100.0f);
        int zw = ui_text_w(zb, 14);
        ui_text(zb, (int)zx + 48 + (70 - zw) / 2, y + 7, 14, g_theme.selection);
        if (pl_button((Rectangle){ zx + 126, (float)y, 40, 30 }, "+", 16, false)) ui_zoom_step(1);
        if (pl_button((Rectangle){ zx + 176, (float)y, 70, 30 }, "100 %", 13, false)) ui_zoom_step(0);
        ui_text("Also Ctrl+wheel over the clips, Ctrl+= / Ctrl+- / Ctrl+0. The menus, the top bar and the dialogs keep their size.",
                x, y + 38, 11, g_theme.text_dim);
        y += 70;
    }

    // === Apply / Cancel / Save buttons ===
    const int btn2_w = 120;
    const int btn2_h = 36;
    const int btn2_y = dlg_y + dlg_h - 56;

    Rectangle apply_r = { dlg_x + dlg_w - 3 * btn2_w - 40, btn2_y,
                          btn2_w, btn2_h };
    Rectangle save_r  = { dlg_x + dlg_w - 2 * btn2_w - 30, btn2_y,
                          btn2_w, btn2_h };
    Rectangle cancel_r = { dlg_x + dlg_w - btn2_w - 20, btn2_y,
                           btn2_w, btn2_h };

    {
        bool hov = CheckCollisionPointRec(mouse, apply_r);
        Color b = hov ? g_theme.selection : g_theme.panel_bg2;
        DrawRectangleRec(apply_r, b);
        DrawRectangleLinesEx(apply_r, 1, g_theme.text_dim);
        Color tc = hov ? g_theme.bg : g_theme.text;
        int tw = ui_text_w("Apply", 15);
        ui_text("Apply", (int)(apply_r.x + (btn2_w - tw) / 2),
                (int)(apply_r.y + (btn2_h - 15) / 2), 15, tc);

        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            theme_save();
            ui_flash_message("Theme applied");
            g_appearance_open = false;
        }
    }

    {
        bool hov = CheckCollisionPointRec(mouse, save_r);
        Color b = hov ? g_theme.selection : g_theme.panel_bg2;
        DrawRectangleRec(save_r, b);
        DrawRectangleLinesEx(save_r, 1, g_theme.text_dim);
        Color tc = hov ? g_theme.bg : g_theme.text;
        int tw = ui_text_w("Save As...", 15);
        ui_text("Save As...", (int)(save_r.x + (btn2_w - tw) / 2),
                (int)(save_r.y + (btn2_h - 15) / 2), 15, tc);

        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            theme_save();
            ui_flash_message("Theme saved");
        }
    }

    {
        bool hov = CheckCollisionPointRec(mouse, cancel_r);
        Color b = hov ? g_theme.panel_bg : g_theme.panel_bg2;
        DrawRectangleRec(cancel_r, b);
        DrawRectangleLinesEx(cancel_r, 1, g_theme.text_dim);
        Color tc = g_theme.text;
        int tw = ui_text_w("Cancel", 15);
        ui_text("Cancel", (int)(cancel_r.x + (btn2_w - tw) / 2),
                (int)(cancel_r.y + (btn2_h - 15) / 2), 15, tc);

        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            appearance_restore();
            g_appearance_open = false;
        }
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        appearance_restore();
        g_appearance_open = false;
    }
}

// ============ MIDI Settings Dialog ============

bool   g_midi_settings_open = false;
static double g_midi_settings_open_time = 0.0;
static int    g_midi_settings_scroll = 0;        /* 1.4.6: the first row of the input list */

void ui_open_midi_settings(void)
{
    g_midi_settings_open = true;
    g_midi_settings_open_time = GetTime();
    g_midi_settings_scroll = 0;
    midi_refresh();
}


bool ui_midi_settings_is_open(void)
{
    return g_midi_settings_open;
}

void draw_midi_settings_dialog(int screen_w, int screen_h)
{
    if (!g_midi_settings_open) return;

    GfxDialogBackdrop(70);

    const int dlg_w = 600;
    const int dlg_h = 600;          /* 1.4.2: + the controller buttons; 1.4.8: + the pad box */
    const int dlg_x = (screen_w - dlg_w) / 2;
    const int dlg_y = (screen_h - dlg_h) / 2;

    double t = GetTime() - g_midi_settings_open_time;
    float alpha = 1.0f;
    if (t < 0.15) alpha = (float)(t / 0.15);
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;

    Color bg = g_theme.panel_bg;
    bg.r = (unsigned char)(bg.r + 10);
    bg.g = (unsigned char)(bg.g + 10);
    bg.b = (unsigned char)(bg.b + 10);
    DrawRectangle(dlg_x, dlg_y, dlg_w, dlg_h, bg);

    Color border = g_theme.selection;
    DrawRectangleLinesEx((Rectangle){ dlg_x, dlg_y, dlg_w, dlg_h }, 2, border);

    DrawRectangle(dlg_x, dlg_y, dlg_w, 40,
                  (Color){ g_theme.selection.r,
                           g_theme.selection.g,
                           g_theme.selection.b, 255 });

    ui_text("MIDI Settings", dlg_x + 16, dlg_y + 10, 20, g_theme.bg);

    /* 1.4.6: several inputs at once - click a device to connect / disconnect it; Play / Control = what it is for */
    char cur_buf[160];
    int nopen = midi_open_count();
    if (nopen > 0) snprintf(cur_buf, sizeof(cur_buf), "Connected: %d input%s  (click a device to connect / disconnect it)", nopen, nopen == 1 ? "" : "s");
    else snprintf(cur_buf, sizeof(cur_buf), "Connected: (none)  - click a device to connect it, as many as you like");
    ui_text(cur_buf, dlg_x + 16, dlg_y + 50, 13, nopen > 0 ? g_theme.vu_green : g_theme.text_dim);

    int port_count = midi_get_port_count();
    int miss_count = midi_missing_count();
    int rows = port_count + miss_count;

    const int list_y = dlg_y + 80;
    const int list_h = 168;
    const int row_h = 28;
    int visible_rows = list_h / row_h;

    DrawRectangle(dlg_x + 16, list_y, dlg_w - 32, list_h, g_theme.panel_bg2);
    DrawRectangleLines(dlg_x + 16, list_y, dlg_w - 32, list_h, g_theme.text_dim);

    Vector2 mouse = GetMousePosition();
    Rectangle list_r = { (float)(dlg_x + 16), (float)list_y, (float)(dlg_w - 32), (float)list_h };
    if (CheckCollisionPointRec(mouse, list_r)) {
        float wh = GetMouseWheelMove();
        if (wh > 0.0f) g_midi_settings_scroll--;
        if (wh < 0.0f) g_midi_settings_scroll++;
    }
    if (g_midi_settings_scroll > rows - visible_rows) g_midi_settings_scroll = rows - visible_rows;
    if (g_midi_settings_scroll < 0) g_midi_settings_scroll = 0;

    if (rows == 0) {
        ui_text("No MIDI ports available", dlg_x + 30, list_y + 20, 14, g_theme.text_dim);
    } else {
        for (int vr = 0; vr < visible_rows; ++vr) {
            int i = g_midi_settings_scroll + vr;
            if (i >= rows) break;
            Rectangle row = { (float)(dlg_x + 16), (float)(list_y + vr * row_h), (float)(dlg_w - 32 - (rows > visible_rows ? 8 : 0)), (float)row_h };
            bool hov = CheckCollisionPointRec(mouse, row);
            int ty = (int)row.y + 6;
            if (i >= port_count) {                       /* remembered, not plugged in now */
                int k = i - port_count;
                Rectangle fr = { row.x + row.width - 74, row.y + 4, 66, row_h - 8 };
                bool fh = CheckCollisionPointRec(mouse, fr);
                if (hov) { Color hc = g_theme.selection; hc.a = 40; DrawRectangleRec(row_hl(row), hc); }
                char src_[256], buf[400];
                snprintf(src_, sizeof src_, "%s", midi_missing_name(k));
                snprintf(buf, sizeof buf, "%s  (not plugged in - connects by itself)", src_);
                for (size_t L = strlen(src_); L > 4 && ui_text_w(buf, 14) > (int)(fr.x - 8) - (dlg_x + 26 + 22); ) {
                    src_[--L] = 0;
                    snprintf(buf, sizeof buf, "%s...  (not plugged in - connects by itself)", src_);
                }
                ui_text(buf, dlg_x + 26 + 22, ty, 14, g_theme.text_dim);
                DrawRectangleRec(fr, fh ? g_theme.mute : g_theme.panel_bg);
                DrawRectangleLinesEx(fr, 1, g_theme.text_dim);
                ui_text("Forget", (int)(fr.x + (fr.width - ui_text_w("Forget", 12)) / 2), (int)fr.y + 4, 12, fh ? RAYWHITE : g_theme.text);
                if (fh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { midi_forget_missing(k); break; }
                continue;
            }
            bool on = midi_port_is_open(i);
            int uses = on ? midi_port_uses(i) : 0;
            Rectangle tg[2] = { { row.x + row.width - 150, row.y + 4, 68, row_h - 8 },
                                { row.x + row.width - 76, row.y + 4, 68, row_h - 8 } };
            bool on_toggle = on && (CheckCollisionPointRec(mouse, tg[0]) || CheckCollisionPointRec(mouse, tg[1]));
            if (hov && !on_toggle) { Color hc = g_theme.selection; hc.a = 50; DrawRectangleRec(row_hl(row), hc); }
            /* the check box */
            Rectangle cb = { (float)(dlg_x + 26), row.y + 7, 14, 14 };
            DrawRectangleRec(cb, on ? g_theme.vu_green : g_theme.panel_bg);
            DrawRectangleLinesEx(cb, 1, g_theme.text_dim);
            char name_src[256], name_copy[260];
            snprintf(name_src, sizeof(name_src), "%s", midi_get_port_name(i));
            snprintf(name_copy, sizeof(name_copy), "%s", name_src);
            int max_w = (int)(on ? tg[0].x - 8 : row.x + row.width - 8) - (dlg_x + 26 + 22);
            for (size_t L = strlen(name_src); L > 4 && ui_text_w(name_copy, 14) > max_w; ) {   /* long names: cut with "..." */
                name_src[--L] = 0;
                snprintf(name_copy, sizeof(name_copy), "%s...", name_src);
            }
            ui_text(name_copy, dlg_x + 26 + 22, ty, 14, on ? g_theme.vu_green : g_theme.text);
            if (on) {
                static const char *const TN[2] = { "Play", "Control" };
                for (int t = 0; t < 2; ++t) {
                    int bit = t ? MIDI_USE_CONTROL : MIDI_USE_PLAY;
                    bool set = (uses & bit) != 0, th = CheckCollisionPointRec(mouse, tg[t]);
                    DrawRectangleRec(tg[t], set ? g_theme.selection : (th ? g_theme.panel_bg2 : g_theme.panel_bg));
                    DrawRectangleLinesEx(tg[t], 1, g_theme.text_dim);
                    ui_text(TN[t], (int)(tg[t].x + (tg[t].width - ui_text_w(TN[t], 12)) / 2), (int)tg[t].y + 4, 12,
                            set ? g_theme.bg : g_theme.text_dim);
                    if (th && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        int nu = uses ^ bit;
                        if (nu == 0) ui_flash_message("An input with neither Play nor Control does nothing - disconnect it instead");
                        else midi_set_port_uses(i, nu);
                    }
                }
            }
            if (hov && !on_toggle && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if (on) { midi_close_port_index(i); midi_save_config(); ui_flash_message("MIDI input disconnected"); }
                else if (midi_open_port(i) == 0) { midi_save_config(); ui_flash_message("MIDI input connected"); }
                else ui_flash_message(midi_open_count() >= MIDI_MAX_OPEN ? "Too many MIDI inputs open" : "MIDI connect failed");
            }
        }
        if (rows > visible_rows) {                       /* scroll bar */
            float th = (float)list_h * (float)visible_rows / (float)rows;
            float ty = (float)list_y + ((float)list_h - th) * (float)g_midi_settings_scroll / (float)(rows - visible_rows);
            DrawRectangle(dlg_x + dlg_w - 16 - 7, (int)ty, 6, (int)th, g_theme.text_dim);
        }
    }
    ui_text("Play = notes go to the instruments    Control = MIDI learn, faders, pads, controller buttons",
            dlg_x + 16, list_y + list_h + 3, 11, g_theme.text_dim);

    /* 1.4.2: controller buttons (any controller): click = learn (then press / turn it), right click = clear */
    {
        int sy = list_y + list_h + 22;
        ui_text("Controller buttons", dlg_x + 16, sy, 15, g_theme.text);
        ui_text("click, then press or turn the control  |  right click = clear", dlg_x + 190, sy + 2, 12, g_theme.text_dim);
        int cw = (dlg_w - 32 - 10) / 2, rh = 30;
        /* (Stop all and the master MUTE: with MIDI learn on the buttons themselves, like the faders) */
        for (int a = 0; a < MM_STOP_ALL; ++a) {
            int col = a < 4 ? 0 : 1, row = a % 4;
            Rectangle r = { (float)(dlg_x + 16 + col * (cw + 10)), (float)(sy + 26 + row * (rh + 6)), (float)cw, (float)rh };
            bool hov = CheckCollisionPointRec(mouse, r);
            bool learning = midimap_learning() == a;
            Color b = learning ? g_theme.selection : (hov ? g_theme.panel_bg : g_theme.panel_bg2);
            DrawRectangleRec(r, b);
            DrawRectangleLinesEx(r, 1, g_theme.text_dim);
            char bind[32], line[160];
            midimap_describe(a, bind, sizeof bind);
            ui_text(midimap_label(a), (int)r.x + 8, (int)r.y + 8, 13, learning ? g_theme.bg : g_theme.text);
            snprintf(line, sizeof line, "%s", learning ? (midimap_is_wheel(a) ? "turn it..." : "press it...") : bind);
            int lw = ui_text_w(line, 13);
            ui_text(line, (int)(r.x + r.width - lw - 8), (int)r.y + 8, 13,
                    learning ? g_theme.bg : (midimap_get(a) ? g_theme.vu_green : g_theme.text_dim));
            if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) midimap_learn(learning ? -1 : a);
            if (hov && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) { midimap_set(a, 0); if (learning) midimap_learn(-1); }
        }
        /* how many clicks of a jog wheel make one step (fine = more) */
        int wy = sy + 26 + 4 * (rh + 6) + 4;
        ui_text("Jog wheel: clicks per step", dlg_x + 16, wy + 7, 13, g_theme.text);
        Rectangle mn = { (float)(dlg_x + 230), (float)wy, 30, 28 }, pl = { (float)(dlg_x + 310), (float)wy, 30, 28 };
        char tv[16]; snprintf(tv, sizeof tv, "%d", midimap_wheel_ticks());
        int tvw = ui_text_w(tv, 15);
        ui_text(tv, dlg_x + 285 - tvw / 2, wy + 6, 15, g_theme.text);
        for (int k = 0; k < 2; ++k) {
            Rectangle r = k ? pl : mn;
            bool hov = CheckCollisionPointRec(mouse, r);
            DrawRectangleRec(r, hov ? g_theme.selection : g_theme.panel_bg2);
            DrawRectangleLinesEx(r, 1, g_theme.text_dim);
            const char *t = k ? "+" : "-";
            ui_text(t, (int)(r.x + (r.width - ui_text_w(t, 16)) / 2), (int)r.y + 5, 16, hov ? g_theme.bg : g_theme.text);
            if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) midimap_set_wheel_ticks(midimap_wheel_ticks() + (k ? 1 : -1));
        }
        ui_text("(more = finer)", dlg_x + 352, wy + 7, 12, g_theme.text_dim);

        /* 1.4.8: the pad box - pads play the clips of a box (channels x scenes) that moves with the selection */
        int py = wy + 36;
        int bw, bh;
        midimap_pad_box(&bw, &bh);
        ui_text("Pads: a box of", dlg_x + 16, py + 7, 13, g_theme.text);
        int bx = dlg_x + 120;
        for (int d = 0; d < 2; ++d) {                    /* channels (wide) x scenes (high) */
            int v = d ? bh : bw;
            for (int k = 0; k < 2; ++k) {
                Rectangle r = { (float)(bx + (k ? 54 : 0)), (float)py, 26, 28 };
                bool hov = CheckCollisionPointRec(mouse, r);
                DrawRectangleRec(r, hov ? g_theme.selection : g_theme.panel_bg2);
                DrawRectangleLinesEx(r, 1, g_theme.text_dim);
                const char *t = k ? "+" : "-";
                ui_text(t, (int)(r.x + (r.width - ui_text_w(t, 16)) / 2), (int)r.y + 5, 16, hov ? g_theme.bg : g_theme.text);
                if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    int nv = v + (k ? 1 : -1);
                    if (d) midimap_set_pad_box(bw, nv); else midimap_set_pad_box(nv, bh);
                }
            }
            char vs[8]; snprintf(vs, sizeof vs, "%d", v);
            ui_text(vs, bx + 40 - ui_text_w(vs, 15) / 2, py + 6, 15, g_theme.text);
            ui_text(d ? "scenes" : "channels  x", bx + 86, py + 7, 13, g_theme.text_dim);
            bx += d ? 0 : 172;
        }
        midimap_pad_box(&bw, &bh);
        Rectangle lr = { (float)(dlg_x + dlg_w - 16 - 150), (float)py, 150, 28 };
        bool lh = CheckCollisionPointRec(mouse, lr);
        int pdl = midimap_pads_learning();
        DrawRectangleRec(lr, pdl >= 0 ? g_theme.selection : (lh ? g_theme.panel_bg : g_theme.panel_bg2));
        DrawRectangleLinesEx(lr, 1, g_theme.text_dim);
        char lt[48];
        if (pdl >= 0) snprintf(lt, sizeof lt, "press pad %d of %d", pdl + 1, bw * bh);
        else if (midimap_pad_count() >= bw * bh) snprintf(lt, sizeof lt, "Pads learned (%d)", bw * bh);
        else snprintf(lt, sizeof lt, "Learn the pads");
        ui_text(lt, (int)(lr.x + (lr.width - ui_text_w(lt, 13)) / 2), (int)lr.y + 7, 13,
                pdl >= 0 ? g_theme.bg : (midimap_pad_count() >= bw * bh ? g_theme.vu_green : g_theme.text));
        if (lh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) midimap_pads_learn(pdl < 0);
        if (lh && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) midimap_pads_clear();
        ui_text(pdl >= 0 ? "top row first, left to right, then the next row  (click = cancel)"
                        : "learn: press the pads top row first, left to right  |  right click = clear  |  Play button = the row's scene",
                dlg_x + 16, py + 32, 11, g_theme.text_dim);
    }

    const int btn_w = 120;
    const int btn_h = 36;
    const int btn_y = dlg_y + dlg_h - 56;

    Rectangle disconnect_r = { dlg_x + 16, btn_y, btn_w + 20, btn_h };
    Rectangle refresh_r   = { dlg_x + dlg_w - 2 * btn_w - 30, btn_y, btn_w, btn_h };
    Rectangle close_r     = { dlg_x + dlg_w - btn_w - 20, btn_y, btn_w, btn_h };

    {
        bool hov = CheckCollisionPointRec(mouse, disconnect_r);
        bool can_disconnect = midi_is_open();
        Color b = hov && can_disconnect ? g_theme.mute : g_theme.panel_bg2;
        DrawRectangleRec(disconnect_r, b);
        DrawRectangleLinesEx(disconnect_r, 1, g_theme.text_dim);
        Color tc = (hov && can_disconnect) ? RAYWHITE : (can_disconnect ? g_theme.text : g_theme.text_dim);
        int tw = ui_text_w("Disconnect all", 15);
        ui_text("Disconnect all", (int)(disconnect_r.x + (disconnect_r.width - tw) / 2),
                (int)(disconnect_r.y + (btn_h - 15) / 2), 15, tc);
        if (hov && can_disconnect && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            midi_close_port();
            midi_save_config();
            ui_flash_message("MIDI disconnected");
        }
    }

    {
        bool hov = CheckCollisionPointRec(mouse, refresh_r);
        Color b = hov ? g_theme.selection : g_theme.panel_bg2;
        DrawRectangleRec(refresh_r, b);
        DrawRectangleLinesEx(refresh_r, 1, g_theme.text_dim);

        Color tc = hov ? g_theme.bg : g_theme.text;
        int tw = ui_text_w("Refresh", 15);
        ui_text("Refresh", (int)(refresh_r.x + (btn_w - tw) / 2),
                (int)(refresh_r.y + (btn_h - 15) / 2), 15, tc);

        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            midi_refresh();                             /* 1.4.6: the open inputs stay open */
            ui_flash_message("MIDI ports refreshed");
        }
    }

    {
        bool hov = CheckCollisionPointRec(mouse, close_r);
        DrawRectangleRec(close_r, hov ? g_theme.selection : g_theme.panel_bg2);
        DrawRectangleLinesEx(close_r, 1, g_theme.text_dim);
        int tw = ui_text_w("Close", 15);
        ui_text("Close", (int)(close_r.x + (btn_w - tw) / 2),
                (int)(close_r.y + (btn_h - 15) / 2), 15, hov ? g_theme.bg : g_theme.text);
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { g_midi_settings_open = false; midimap_learn(-1); }
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        g_midi_settings_open = false;
    }
}

