/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 2.1: the top of the window - the menu bar (File, Edit, Play), the DSP indicator, scene transitions. Moved out of ui.c. */
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <ctype.h>

// ============ Menu bar ============

void draw_menu_bar(Session *s, int screen_w)
{
    DrawRectangle(0, 0, screen_w, MENU_BAR_H_BASE, g_theme.panel_bg);

    float file_x = 8;
    float file_w = 60;
    float file_h = MENU_BAR_H_BASE;

    Vector2 mouse = GetMousePosition();
    Rectangle file_r = { file_x, 0, file_w, file_h };
    bool file_hover = CheckCollisionPointRec(mouse, file_r);

    if (file_hover || g_file_menu_open) {
        Color hc = g_theme.panel_bg;
        hc.r = (unsigned char)(hc.r + 30);
        hc.g = (unsigned char)(hc.g + 30);
        hc.b = (unsigned char)(hc.b + 30);
        DrawRectangle((int)file_x, 0, (int)file_w, (int)file_h, hc);
    }

    ui_text("File", (int)(file_x + 8), (int)(MENU_BAR_H_BASE / 2 - 8),
            15, g_file_menu_open ? g_theme.selection : g_theme.text);

    if (file_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (g_file_menu_open) {
            file_menu_close();
        } else {
            edit_menu_close();
            play_menu_close();
            file_menu_open();
        }
    }

    float edit_x = file_x + file_w + 8;
    float edit_w = 60;
    Rectangle edit_r = { edit_x, 0, edit_w, file_h };
    bool edit_hover = CheckCollisionPointRec(mouse, edit_r);

    if (edit_hover || g_edit_menu_open) {
        Color hc = g_theme.panel_bg;
        hc.r = (unsigned char)(hc.r + 30);
        hc.g = (unsigned char)(hc.g + 30);
        hc.b = (unsigned char)(hc.b + 30);
        DrawRectangle((int)edit_x, 0, (int)edit_w, (int)file_h, hc);
    }

    ui_text("Edit", (int)(edit_x + 8), (int)(MENU_BAR_H_BASE / 2 - 8),
            15, g_edit_menu_open ? g_theme.selection : g_theme.text);

    if (edit_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (g_edit_menu_open) {
            edit_menu_close();
        } else {
            file_menu_close();
            play_menu_close();
            edit_menu_open();
        }
    }

    float play_x = edit_x + edit_w + 8;          /* 2.0 */
    float play_w = 60;
    Rectangle play_r = { play_x, 0, play_w, file_h };
    bool play_hover = CheckCollisionPointRec(mouse, play_r);
    if (play_hover || g_play_menu_open) {
        Color hc = g_theme.panel_bg;
        hc.r = (unsigned char)(hc.r + 30); hc.g = (unsigned char)(hc.g + 30); hc.b = (unsigned char)(hc.b + 30);
        DrawRectangle((int)play_x, 0, (int)play_w, (int)file_h, hc);
    }
    ui_text("Play", (int)(play_x + 8), (int)(MENU_BAR_H_BASE / 2 - 8), 15, g_play_menu_open ? g_theme.selection : g_theme.text);
    if (play_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (g_play_menu_open) play_menu_close();
        else { file_menu_close(); g_edit_menu_open = false; play_menu_open(); }
    }

    // Routing and Shortcuts: open a dialog directly
    float rt_x = play_x + play_w + 8;
    float rt_w = 78;
    Rectangle rt_r = { rt_x, 0, rt_w, file_h };
    bool rt_hover = CheckCollisionPointRec(mouse, rt_r);
    if (rt_hover) {
        Color hc = g_theme.panel_bg;
        hc.r = (unsigned char)(hc.r + 30); hc.g = (unsigned char)(hc.g + 30); hc.b = (unsigned char)(hc.b + 30);
        DrawRectangle((int)rt_x, 0, (int)rt_w, (int)file_h, hc);
    }
    ui_text("Routing", (int)(rt_x + 8), (int)(MENU_BAR_H_BASE / 2 - 8), 15, g_theme.text);
    if (rt_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        file_menu_close();
        edit_menu_close();
        ui_open_routing();
    }

    float sc_x = rt_x + rt_w + 8;
    float sc_w = 92;
    Rectangle sc_r = { sc_x, 0, sc_w, file_h };
    bool sc_hover = CheckCollisionPointRec(mouse, sc_r);
    if (sc_hover) {
        Color hc = g_theme.panel_bg;
        hc.r = (unsigned char)(hc.r + 30); hc.g = (unsigned char)(hc.g + 30); hc.b = (unsigned char)(hc.b + 30);
        DrawRectangle((int)sc_x, 0, (int)sc_w, (int)file_h, hc);
    }
    ui_text("Shortcuts", (int)(sc_x + 8), (int)(MENU_BAR_H_BASE / 2 - 8), 15, g_theme.text);
    if (sc_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        file_menu_close();
        edit_menu_close();
        ui_open_shortcuts();
    }

    (void)s;                                    /* the project tempo is no longer shown (step 13) */

    /* ===== Problem indicator (top right, above DSP): click = real-time log ===== */
    {
        int ne = applog_new_errors(), nw = applog_new_warnings();
        bool lost = audio_is_lost();
        char lbl[48];
        Color dot;
        if (lost)          { snprintf(lbl, sizeof lbl, "AUDIO LOST"); dot = g_theme.vu_red; }
        else if (ne > 0)   { snprintf(lbl, sizeof lbl, ne == 1 ? "1 error" : "%d errors", ne); dot = g_theme.vu_red; }
        else if (nw > 0)   { snprintf(lbl, sizeof lbl, nw == 1 ? "1 warning" : "%d warnings", nw); dot = g_theme.vu_yellow; }
        else               { snprintf(lbl, sizeof lbl, "Log"); dot = g_theme.vu_green; }
        int lw = ui_text_w(lbl, 13);
        float bw = (float)lw + 30.0f;
        Rectangle r = { (float)screen_w - bw - 8.0f, 4.0f, bw, (float)(MENU_BAR_H_BASE - 8) };
        g_log_btn = r;
        bool hov = CheckCollisionPointRec(mouse, r);
        bool alarm = lost || ne > 0;
        Color bg = g_log_open ? g_theme.selection : g_theme.panel_bg;
        if (alarm && !g_log_open) {                      /* blinks on an error, as in Ardour */
            bool on = ((int)(GetTime() * 2.0)) % 2 == 0;
            bg = on ? (Color){ 150, 30, 30, 255 } : g_theme.panel_bg;
        }
        if (hov && !g_log_open) { bg.r = (unsigned char)(bg.r + 25); bg.g = (unsigned char)(bg.g + 25); bg.b = (unsigned char)(bg.b + 25); }
        DrawRectangleRec(r, bg);
        DrawRectangleLines((int)r.x, (int)r.y, (int)r.width, (int)r.height, alarm || nw > 0 ? dot : g_theme.text_dim);
        DrawCircleV((Vector2){ r.x + 11, r.y + r.height / 2 }, 4.5f, dot);
        ui_text(lbl, (int)r.x + 21, (int)(r.y + (r.height - 13) / 2), 13,
                g_log_open ? g_theme.bg : (alarm ? RAYWHITE : g_theme.text));
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_ti_active && !g_ctx_open) {
            g_log_open = !g_log_open;
            g_log_just_opened = g_log_open;
            g_log_scroll = 0;
            if (g_log_open) { file_menu_close(); edit_menu_close(); }
        }
    }
}

void draw_edit_dropdown(int screen_w, int screen_h)
{
    (void)screen_w;
    (void)screen_h;
    if (!g_edit_menu_open) return;

    const int item_h = 24;
    const int menu_w = 220;
    const int pad = 4;

    /* 2.0: Loudness -> Play; Separation (from File) and Speaker Map here */
    enum { EM_UNDO, EM_REDO, EM_AUDIO, EM_CARDS, EM_SPKMAP, EM_MIDI, EM_PLUGINS, EM_SEP, EM_APPEAR, EM_PERF, EM_ABOUT, EM_SEPLINE };
    const char *items[16], *shortcuts[16];
    int ids[16];
    bool enabled[16];
    int n_items = 0;
    #define EM_ADD(id_, txt_, sc_, en_) do { ids[n_items] = (id_); items[n_items] = (txt_); shortcuts[n_items] = (sc_); enabled[n_items] = (en_); n_items++; } while (0)
    EM_ADD(EM_UNDO, "Undo", "Ctrl+Z", g_can_undo);
    EM_ADD(EM_REDO, "Redo", "Ctrl+Shift+Z", g_can_redo);
    EM_ADD(EM_SEPLINE, NULL, NULL, true);
    EM_ADD(EM_AUDIO, "Audio...", "Ctrl+,", true);
    EM_ADD(EM_CARDS, "Sound Cards...", NULL, true);
    EM_ADD(EM_SPKMAP, "Speaker Map...", NULL, true);
    EM_ADD(EM_MIDI, "MIDI...", "Ctrl+M", true);
    EM_ADD(EM_PLUGINS, "Plugins...", NULL, true);
    if (sep_supported()) EM_ADD(EM_SEP, "Separation...", NULL, true);
    EM_ADD(EM_APPEAR, "Appearance...", NULL, true);
    EM_ADD(EM_PERF, "Performance...", NULL, true);
    EM_ADD(EM_SEPLINE, NULL, NULL, true);
    EM_ADD(EM_ABOUT, "About Q-LiM...", NULL, true);
    #undef EM_ADD

    int menu_h = pad * 2;
    for (int i = 0; i < n_items; ++i) menu_h += items[i] ? item_h : 6;

    int mx = 8 + 60 + 8;
    int my = MENU_BAR_H_BASE;

    double t = GetTime() - g_edit_menu_open_time;
    float alpha = 1.0f;
    if (t < 0.1) alpha = (float)(t / 0.1);
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;

    Color bg = g_theme.panel_bg;
    bg.r = (unsigned char)(bg.r + 10);
    bg.g = (unsigned char)(bg.g + 10);
    bg.b = (unsigned char)(bg.b + 10);
    bg.a = (unsigned char)(bg.a * alpha);
    DrawRectangle(mx, my, menu_w, menu_h, bg);
    g_menu_dd_rect = (Rectangle){ (float)mx, (float)my, (float)menu_w, (float)menu_h };   /* for the mouse shield */
    GfxHoverLayer(g_menu_dd_rect);                                                         /* 1.3.12 */

    Color border = g_theme.text_dim;
    border.a = (unsigned char)(border.a * alpha);
    DrawRectangleLines(mx, my, menu_w, menu_h, border);

    Vector2 mouse = GetMousePosition();
    bool inside = (mouse.x >= mx && mouse.x <= mx + menu_w &&
                   mouse.y >= my && mouse.y <= my + menu_h);

    int cur_y = my + pad;
    int clicked = -1;

    for (int i = 0; i < n_items; ++i) {
        if (!items[i]) {
            Color sep = g_theme.text_dim;
            sep.a = (unsigned char)(sep.a * 0.5f * alpha);
            DrawLine(mx + 6, cur_y + 3, mx + menu_w - 6, cur_y + 3, sep);
            cur_y += 6;
            continue;
        }

        Rectangle row = { (float)mx, (float)cur_y, (float)menu_w, (float)item_h };
        bool hov = inside && enabled[i] && CheckCollisionPointRec(mouse, row);

        if (hov) {
            Color hc = g_theme.selection;
            hc.a = (unsigned char)(hc.a * 0.5f * alpha);
            DrawRectangleRec(row_hl(row), hc);
        }

        Color txt = enabled[i] ? g_theme.text : g_theme.text_dim;
        if (!enabled[i]) txt.a = (unsigned char)(txt.a * 0.5f);
        txt.a = (unsigned char)(txt.a * alpha);
        ui_text(items[i], mx + 10, cur_y + 5, 14, txt);

        if (shortcuts[i]) {
            Color sc = g_theme.text_dim;
            sc.a = (unsigned char)(sc.a * (enabled[i] ? 1.0f : 0.5f) * alpha);
            int sw = ui_text_w(shortcuts[i], 12);
            ui_text(shortcuts[i], mx + menu_w - sw - 10, cur_y + 6, 12, sc);
        }

        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) clicked = i;
        cur_y += item_h;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !inside) {
        Rectangle edit_r = { 8 + 60 + 8, 0, 60, MENU_BAR_H_BASE };
        if (!CheckCollisionPointRec(mouse, edit_r)) {
            edit_menu_close();
        }
        return;
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        edit_menu_close();
        return;
    }

    int id = clicked >= 0 ? ids[clicked] : -1;
    if (id >= 0) edit_menu_close();
    switch (id) {
    case EM_ABOUT:   g_about_open = true; break;
    case EM_CARDS:   ui_open_sound_cards(); break;
    case EM_SPKMAP:  if (g_ui_session) ui_open_speaker_map(g_ui_session, g_selected_ch, g_selected_scene); break;
    case EM_PERF:    g_perf_open = true; break;
    case EM_PLUGINS: g_plugins_open = true; plugins_start(); break;
    case EM_SEP:     ui_open_sep_settings(); break;
    case EM_UNDO:    g_pending_edit_action = EDIT_MENU_UNDO; break;
    case EM_REDO:    g_pending_edit_action = EDIT_MENU_REDO; break;
    case EM_AUDIO:   g_pending_edit_action = EDIT_MENU_AUDIO; break;
    case EM_MIDI:    g_pending_edit_action = EDIT_MENU_MIDI; break;
    case EM_APPEAR:  g_pending_edit_action = EDIT_MENU_APPEARANCE; break;
    default: break;
    }
}

void draw_play_dropdown(void)
{
    if (!g_play_menu_open) return;
    const int item_h = 24, menu_w = 230, pad = 4;
    Session *s = g_ui_session;
    const char *items[2] = { "Launches at a Time...", "Loudness..." };
    int n = 2;
    int mx = 8 + 60 + 8 + 60 + 8, my = MENU_BAR_H_BASE;
    int menu_h = pad * 2 + n * item_h;
    double t = GetTime() - g_play_menu_open_time;
    float alpha = t < 0.1 ? (float)(t / 0.1) : 1.0f;
    if (alpha < 0.0f) alpha = 0.0f;
    Color bg = g_theme.panel_bg;
    bg.r = (unsigned char)(bg.r + 10); bg.g = (unsigned char)(bg.g + 10); bg.b = (unsigned char)(bg.b + 10);
    bg.a = (unsigned char)(bg.a * alpha);
    DrawRectangle(mx, my, menu_w, menu_h, bg);
    g_menu_dd_rect = (Rectangle){ (float)mx, (float)my, (float)menu_w, (float)menu_h };
    GfxHoverLayer(g_menu_dd_rect);
    Color border = g_theme.text_dim;
    border.a = (unsigned char)(border.a * alpha);
    DrawRectangleLines(mx, my, menu_w, menu_h, border);
    Vector2 mouse = GetMousePosition();
    bool inside = CheckCollisionPointRec(mouse, g_menu_dd_rect);
    int clicked = -1;
    for (int i = 0; i < n; ++i) {
        Rectangle row = { (float)mx, (float)(my + pad + i * item_h), (float)menu_w, (float)item_h };
        bool hov = inside && CheckCollisionPointRec(mouse, row);
        if (hov) { Color hc = g_theme.selection; hc.a = (unsigned char)(hc.a * 0.5f * alpha); DrawRectangleRec(row_hl(row), hc); }
        Color txt = g_theme.text;
        txt.a = (unsigned char)(txt.a * alpha);
        ui_text(items[i], mx + 10, (int)row.y + 5, 14, txt);
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) clicked = i;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !inside) {
        Rectangle pr = { (float)(8 + 60 + 8 + 60 + 8), 0, 60, MENU_BAR_H_BASE };
        if (!CheckCollisionPointRec(mouse, pr)) play_menu_close();
        return;
    }
    if (IsKeyPressed(KEY_ESCAPE)) { play_menu_close(); return; }
    if (clicked == 0) { play_menu_close(); if (s) ui_open_schedule_list(s); }
    else if (clicked == 1) { play_menu_close(); g_loud_open = true; }
}

void draw_file_dropdown(int screen_w, int screen_h)
{
    (void)screen_w;
    (void)screen_h;
    if (!g_file_menu_open) return;

    const int item_h = 24;
    const int menu_w = 220;
    const int pad = 4;

    const char *items[] = {
        "New",
        "Save",
        "Save As...",
        "Collect All & Save",
        NULL,
        "Load...",
    };
    const char *shortcuts[] = {
        "Ctrl+N",
        "Ctrl+S",
        "Ctrl+Shift+S",
        "Ctrl+Shift+E",
        NULL,
        "Ctrl+O",
    };
    FileMenuAction actions[] = {
        FILE_MENU_NEW,
        FILE_MENU_SAVE,
        FILE_MENU_SAVE_AS,
        FILE_MENU_COLLECT_AND_SAVE,
        FILE_MENU_NONE,
        FILE_MENU_LOAD,
    };
    int n_items = 6;                     /* 2.0: Launches at a Time -> Play, Separation -> Edit */

    int menu_h = pad * 2;
    for (int i = 0; i < n_items; ++i) {
        if (items[i] == NULL) menu_h += 6;
        else menu_h += item_h;
    }

    int mx = 8;
    int my = MENU_BAR_H_BASE;

    double t = GetTime() - g_file_menu_open_time;
    float alpha = 1.0f;
    if (t < 0.1) alpha = (float)(t / 0.1);
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;

    Color bg = g_theme.panel_bg;
    bg.r = (unsigned char)(bg.r + 10);
    bg.g = (unsigned char)(bg.g + 10);
    bg.b = (unsigned char)(bg.b + 10);
    bg.a = (unsigned char)(bg.a * alpha);
    DrawRectangle(mx, my, menu_w, menu_h, bg);
    g_menu_dd_rect = (Rectangle){ (float)mx, (float)my, (float)menu_w, (float)menu_h };   /* for the mouse shield */
    GfxHoverLayer(g_menu_dd_rect);                                                         /* 1.3.12 */

    Color border = g_theme.text_dim;
    border.a = (unsigned char)(border.a * alpha);
    DrawRectangleLines(mx, my, menu_w, menu_h, border);

    Vector2 mouse = GetMousePosition();
    bool inside = (mouse.x >= mx && mouse.x <= mx + menu_w &&
                   mouse.y >= my && mouse.y <= my + menu_h);

    int cur_y = my + pad;
    int clicked = -1;

    for (int i = 0; i < n_items; ++i) {
        if (items[i] == NULL) {
            Color sep = g_theme.text_dim;
            sep.a = (unsigned char)(sep.a * 0.5f * alpha);
            DrawLine(mx + 6, cur_y + 3, mx + menu_w - 6, cur_y + 3, sep);
            cur_y += 6;
            continue;
        }

        Rectangle row = { (float)mx, (float)cur_y,
                          (float)menu_w, (float)item_h };

        bool hov = inside && CheckCollisionPointRec(mouse, row);

        if (hov) {
            Color hc = g_theme.selection;
            hc.a = (unsigned char)(hc.a * 0.5f * alpha);
            DrawRectangleRec(row_hl(row), hc);
        }

        Color txt = g_theme.text;
        txt.a = (unsigned char)(txt.a * alpha);
        ui_text(items[i], mx + 10, cur_y + 5, 14, txt);

        if (shortcuts[i]) {
            Color sc = g_theme.text_dim;
            sc.a = (unsigned char)(sc.a * alpha);
            int sw = ui_text_w(shortcuts[i], 12);
            ui_text(shortcuts[i],
                    mx + menu_w - sw - 10,
                    cur_y + 6,
                    12, sc);
        }

        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            clicked = i;
        }

        cur_y += item_h;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !inside) {
        Rectangle file_r = { 8, 0, 60, MENU_BAR_H_BASE };
        if (!CheckCollisionPointRec(mouse, file_r)) {
            file_menu_close();
        }
        return;
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        file_menu_close();
        return;
    }

    if (clicked >= 0 && actions[clicked] == FILE_MENU_SCHEDULE) {     /* 1.4.17: handled here */
        file_menu_close();
        if (g_ui_session) ui_open_schedule_list(g_ui_session);
        return;
    }
    if (clicked >= 0 && actions[clicked] == FILE_MENU_SEPARATION) {   /* 1.4.15: handled here */
        file_menu_close();
        ui_open_sep_settings();
        return;
    }
    if (clicked >= 0 && actions[clicked] != FILE_MENU_NONE) {
        g_pending_file_action = actions[clicked];
        file_menu_close();
    }
}


/* ============ DSP indicator (as in Bitwig) ============ */

#define DSP_HIST_N      240          /* 240 x 0.25 s = 60 s of history */
#define DSP_BUCKET_S    0.25

static float  g_dsp_hist_avg[DSP_HIST_N];
static float  g_dsp_hist_peak[DSP_HIST_N];
static int    g_dsp_hist_count = 0;          /* how many are valid (up to DSP_HIST_N) */
static int    g_dsp_hist_head  = 0;          /* where the next value is written */
static double g_dsp_bucket_t0  = 0.0;
static double g_dsp_bucket_sum = 0.0;
static unsigned g_dsp_bucket_blocks = 0;
static float  g_dsp_bucket_peak = 0.0f;
static float  g_dsp_now  = 0.0f;             /* the displayed (smoothed) load */
bool   g_dsp_open = false;
static bool   g_dsp_just_opened = false;   /* click on the indicator: in the same frame the popup is not closed by the same click */

static Color dsp_level_color(float v)
{
    if (v < 0.60f) return (Color){ 90, 205, 120, 255 };
    if (v < 0.85f) return (Color){ 235, 200, 60, 255 };
    return (Color){ 235, 80, 70, 255 };
}

static void dsp_update(void)
{
    double now = GetTime();
    DspSample sm;
    dsp_meter_take(&sm);
    if (g_dsp_bucket_t0 == 0.0) g_dsp_bucket_t0 = now;
    if (sm.blocks) {
        g_dsp_bucket_sum += (double)sm.avg * sm.blocks;
        g_dsp_bucket_blocks += sm.blocks;
        if (sm.peak > g_dsp_bucket_peak) g_dsp_bucket_peak = sm.peak;
        /* fast up, slower down */
        float k = sm.avg > g_dsp_now ? 0.5f : 0.08f;
        g_dsp_now += (sm.avg - g_dsp_now) * k;
    } else {
        g_dsp_now *= 0.92f;                  /* no audio -> smoothly towards 0 */
    }
    if (now - g_dsp_bucket_t0 >= DSP_BUCKET_S) {
        float avg = g_dsp_bucket_blocks ? (float)(g_dsp_bucket_sum / g_dsp_bucket_blocks) : 0.0f;
        g_dsp_hist_avg[g_dsp_hist_head]  = avg;
        g_dsp_hist_peak[g_dsp_hist_head] = g_dsp_bucket_peak;
        g_dsp_hist_head = (g_dsp_hist_head + 1) % DSP_HIST_N;
        if (g_dsp_hist_count < DSP_HIST_N) g_dsp_hist_count++;
        g_dsp_bucket_t0 = now;
        g_dsp_bucket_sum = 0.0; g_dsp_bucket_blocks = 0; g_dsp_bucket_peak = 0.0f;
    }
}

/* Below 10% - with one decimal, so you can see it is measuring (a light program = small numbers) */
static void dsp_pct_text(char *out, size_t n, float v)
{
    float p = v * 100.0f;
    if (p < 10.0f) snprintf(out, n, "%.1f%%", p);
    else snprintf(out, n, "%d%%", (int)(p + 0.5f));
}

static Rectangle dsp_widget_rect(int screen_w)
{
    return (Rectangle){ (float)(screen_w - MASTER_W - GAP), (float)(MENU_BAR_H_BASE + 7),
                        (float)MASTER_W, (float)(TOP_BAR_H_BASE - 14) };
}

Rectangle dsp_popup_rect(int screen_w)
{
    Rectangle w = dsp_widget_rect(screen_w);
    const float pw = 380, ph = 270;
    float x = w.x + w.width - pw;
    if (x < 6) x = 6;
    return (Rectangle){ x, w.y + w.height + 6, pw, ph };
}

/* The history from the oldest to the newest: i = 0 is the oldest valid one */
static void dsp_hist_at(int i, float *avg, float *peak)
{
    int idx = (g_dsp_hist_head - g_dsp_hist_count + i + DSP_HIST_N * 2) % DSP_HIST_N;
    *avg = g_dsp_hist_avg[idx];
    *peak = g_dsp_hist_peak[idx];
}


/* The small indicator above Master: a mini graph in the background, a bar and a percentage. */
static void draw_dsp_widget(int screen_w)
{
    dsp_update();
    Rectangle r = dsp_widget_rect(screen_w);
    Vector2 m = GetMousePosition();
    bool hov = CheckCollisionPointRec(m, r);

    Color bg = g_theme.panel_bg2;
    DrawRectangleRounded(r, R_CELL, hov ? (Color){ (unsigned char)(bg.r + 14), (unsigned char)(bg.g + 14), (unsigned char)(bg.b + 14), 255 } : bg);

    /* mini graph: the last ~ 30 seconds, one column per 2 pixels. 2.0.0: kept away from the rounded corners
     * (it went out of the outline at the bottom corners) */
    float ins = ceilf(R_CELL);
    int cols = (int)(r.width - 2 * ins) / 2;
    if (cols > g_dsp_hist_count) cols = g_dsp_hist_count;
    int inner_h = (int)r.height - 4;
    for (int c = 0; c < cols; ++c) {
        float a, p;
        dsp_hist_at(g_dsp_hist_count - cols + c, &a, &p);
        if (a > 1.0f) a = 1.0f;
        int h = (int)(a * inner_h + 0.5f);
        Color col = dsp_level_color(a);
        col.a = 120;
        int x = (int)(r.x + r.width - ins - (cols - c) * 2);
        if (h > 0) DrawRectangle(x, (int)(r.y + r.height - 2 - h), 2, h, col);
    }

    float v = g_dsp_now > 1.0f ? 1.0f : g_dsp_now;
    if (v > 0.0f)                                    /* the "now" bar: inside the rounded outline, rounded ends */
        DrawRectangleRounded((Rectangle){ r.x + ins, r.y + r.height - 5, (r.width - 2 * ins) * v, 3 }, 1.5f, dsp_level_color(g_dsp_now));
    DrawRectangleRoundedLines(r, R_CELL, 1, (g_dsp_open || hov) ? g_theme.selection : g_theme.text_dim);

    char pc[16];
    dsp_pct_text(pc, sizeof pc, g_dsp_now);
    ui_text("DSP", (int)r.x + 8, (int)(r.y + (r.height - 14) / 2) - 1, 13, g_theme.text_dim);
    int tw = ui_text_w(pc, 15);
    ui_text(pc, (int)(r.x + r.width - tw - 8), (int)(r.y + (r.height - 15) / 2) - 1, 15, g_theme.text);

    if (hov) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { g_dsp_open = !g_dsp_open; g_dsp_just_opened = g_dsp_open; }
}

/* The big popup with the graph (opened by clicking the indicator). */
void draw_dsp_popup(int screen_w)
{
    if (!g_dsp_open) return;
    Rectangle r = dsp_popup_rect(screen_w);
    Vector2 m = GetMousePosition();

    /* closing: a click outside the popup (and outside the indicator - it toggles it itself), Esc */
    if (!g_dsp_just_opened &&
        (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) &&
        !CheckCollisionPointRec(m, r)) g_dsp_open = false;
    g_dsp_just_opened = false;
    if (IsKeyPressed(KEY_ESCAPE)) g_dsp_open = false;
    if (!g_dsp_open) return;

    Color bg = g_theme.panel_bg;
    bg.r = (unsigned char)(bg.r + 10); bg.g = (unsigned char)(bg.g + 10); bg.b = (unsigned char)(bg.b + 10);
    DrawRectangle((int)r.x + 3, (int)r.y + 3, (int)r.width, (int)r.height, (Color){ 0, 0, 0, 90 });   /* shadow */
    DrawRectangleRec(r, bg);
    GfxHoverLayer(r);                                        /* 2.0.0 */
    DrawRectangleLinesEx(r, 2, g_theme.selection);

    AudioConfig ac = audio_get_current_config();
    float lat_ms = ac.sample_rate > 0 ? 1000.0f * ac.buffer_size / ac.sample_rate : 0.0f;
    char buf[160];

    ui_text("DSP load", (int)r.x + 14, (int)r.y + 10, 17, g_theme.text);
    {   /* 1.3.1: what really runs (e.g. PipeWire may run another buffer than the one asked for) */
        int af; double ams;
        if (audio_actual_buffer(&af, &ams) && ams > 0.0) {
            if (af != ac.buffer_size)
                snprintf(buf, sizeof buf, "%d Hz  |  %d frames (asked %d)  |  %.1f ms", ac.sample_rate, af, ac.buffer_size, ams);
            else
                snprintf(buf, sizeof buf, "%d Hz  |  %d frames  |  %.1f ms", ac.sample_rate, af, ams);
        } else
            snprintf(buf, sizeof buf, "%d Hz  |  %d frames  |  %.1f ms", ac.sample_rate, ac.buffer_size, lat_ms);
    }
    int tw = ui_text_w(buf, 12);
    ui_text(buf, (int)(r.x + r.width - tw - 14), (int)r.y + 13, 12, g_theme.text_dim);

    /* Now / peak for the last minute */
    float peak60 = 0.0f;
    for (int i = 0; i < g_dsp_hist_count; ++i) { float a, p; dsp_hist_at(i, &a, &p); if (p > peak60) peak60 = p; }
    dsp_pct_text(buf, sizeof buf, g_dsp_now);
    ui_text(buf, (int)r.x + 14, (int)r.y + 36, 30, dsp_level_color(g_dsp_now));
    ui_text("now", (int)r.x + 14 + ui_text_w(buf, 30) + 8, (int)r.y + 52, 12, g_theme.text_dim);
    { char pk[16]; dsp_pct_text(pk, sizeof pk, peak60); snprintf(buf, sizeof buf, "peak (last minute): %s", pk); }
    tw = ui_text_w(buf, 13);
    ui_text(buf, (int)(r.x + r.width - tw - 14), (int)r.y + 48, 13, g_theme.text);

    /* Graph */
    const int gx = (int)r.x + 44, gy = (int)r.y + 82, gw = (int)r.width - 44 - 14, gh = 110;
    DrawRectangle(gx, gy, gw, gh, g_theme.bg);
    const float marks[3] = { 0.0f, 0.5f, 1.0f };
    const char *mlab[3] = { "0%", "50%", "100%" };
    for (int i = 0; i < 3; ++i) {
        int y = gy + gh - (int)(marks[i] * gh);
        if (y >= gy + gh) y = gy + gh - 1;
        Color gl = g_theme.text_dim; gl.a = i == 2 ? 140 : 70;
        DrawLine(gx, y, gx + gw, y, gl);
        int lw = ui_text_w(mlab[i], 11);
        ui_text(mlab[i], gx - lw - 6, y - 6, 11, g_theme.text_dim);
    }

    int n = g_dsp_hist_count;
    for (int c = 0; c < gw; ++c) {
        /* column c -> a sample from the history (the newest is on the right, the history is 60 s, the columns are gw) */
        int back = (int)((float)(gw - 1 - c) * DSP_HIST_N / gw);   /* how many samples back */
        if (back >= n) continue;
        float a, p;
        dsp_hist_at(n - 1 - back, &a, &p);
        if (a > 1.0f) a = 1.0f;
        if (p > 1.0f) p = 1.0f;
        int ph = (int)(p * gh), ah = (int)(a * gh);
        if (ph > ah) { Color pc = dsp_level_color(p); pc.a = 70; DrawRectangle(gx + c, gy + gh - ph, 1, ph - ah, pc); }
        if (ah > 0)   DrawRectangle(gx + c, gy + gh - ah, 1, ah, dsp_level_color(a));
    }
    DrawRectangleLines(gx, gy, gw, gh, g_theme.text_dim);
    ui_text("-60 s", gx, gy + gh + 4, 11, g_theme.text_dim);
    int nw = ui_text_w("now", 11);
    ui_text("now", gx + gw - nw, gy + gh + 4, 11, g_theme.text_dim);

    /* Dropouts */
    unsigned xr = dsp_meter_xruns();
    snprintf(buf, sizeof buf, "Dropouts reported by the sound card: %u", xr);
    ui_text(buf, (int)r.x + 14, (int)r.y + 214, 13, xr ? (Color){ 235, 80, 70, 255 } : g_theme.text);
    snprintf(buf, sizeof buf, "Audio blocks over 100%%: %u    |    Disk buffer dry-outs: %lu",
             dsp_meter_overloads(), streams_starved_total());
    ui_text(buf, (int)r.x + 14, (int)r.y + 234, 12, g_theme.text_dim);
}

/* 1.2.1: the Q-LiM wordmark at the top centre: "Q" + the sign (the waveform with the orange "X" from the icon,
 * standing in for the dash) + "LiM". The sign is drawn from the same data as assets/icons/qlim.svg. */
static const float LOGO_BARS[][4] = {   /* x, y, height, opacity - as in assets/icons/qlim.svg */
    {92,236,40,1},{108,216,80,1},{124,196,120,1},{140,221,70,1},{156,181,150,1},{172,206,100,1},{188,226,60,1},
    {204,191,130,.8f},{220,211,90,.6f},{236,171,170,.5f},{252,201,110,.5f},{268,176,160,.5f},{284,216,80,.6f},
    {300,186,140,.8f},{316,226,60,1},{332,196,120,1},{348,206,100,1},{364,221,70,1},{380,201,110,1},
    {396,231,50,1},{412,241,30,1} };

/* the sign: its height h (the "X" from top to bottom), left edge x0, centre y; returns its width */
static float draw_logo_sign(float x0, float cy, float h, bool draw)
{
    float fy = h / 196.0f;                           /* the "X" spans 158..354 in the svg */
    float step = roundf(16.0f * fy * 1.25f);         /* whole pixels between the bars: all equally sharp */
    if (step < 3.0f) step = 3.0f;
    float fx = step / 16.0f;
    float bw = roundf(step * 0.55f);
    if (bw < 2.0f) bw = 2.0f;
    float width = 320.0f * fx + bw;                  /* bars from x 92 to 412 */
    if (!draw) return width;
    float ox = roundf(x0 - 92.0f * fx), oy = cy - 256.0f * fy;
    for (size_t i = 0; i < sizeof LOGO_BARS / sizeof LOGO_BARS[0]; ++i) {
        Color c = g_theme.text;
        c.a = (unsigned char)(170 * LOGO_BARS[i][3]);
        DrawRectangleRounded((Rectangle){ roundf(ox + LOGO_BARS[i][0] * fx), oy + LOGO_BARS[i][1] * fy, bw, LOGO_BARS[i][2] * fy },
                             0.5f, c);
    }
    Color acc = { 255, 163, 26, 235 };
    for (int k = 0; k < 2; ++k) {                    /* the two orange curves ("X") */
        float p[4][2] = { {208,158},{248,158},{268,354},{304,354} };
        if (k) { p[0][1] = 354; p[1][0] = 244; p[1][1] = 354; p[2][0] = 264; p[2][1] = 158; p[3][1] = 158; }
        Vector2 prev = { 0, 0 };
        for (int i = 0; i <= 32; ++i) {
            float tt = i / 32.0f, u = 1 - tt;
            float x = u*u*u*p[0][0] + 3*u*u*tt*p[1][0] + 3*u*tt*tt*p[2][0] + tt*tt*tt*p[3][0];
            float y = u*u*u*p[0][1] + 3*u*u*tt*p[1][1] + 3*u*tt*tt*p[2][1] + tt*tt*tt*p[3][1];
            Vector2 v = { ox + x * fx, oy + y * fy };
            if (i) DrawLineEx(prev, v, 2.0f, acc);
            prev = v;
        }
    }
    return width;
}

/* letters a little bolder and wider apart than the normal text */
static float wm_text(const char *s, float x, float y, int size, Color c, bool draw)
{
    float sp = size * 0.06f, w = 0;
    char ch[2] = { 0, 0 };
    for (const char *q = s; *q; ++q) {
        ch[0] = *q;
        if (draw) { ui_text(ch, (int)roundf(x + w), (int)y, size, c); DrawTextEx(g_font, ch, (Vector2){ roundf(x + w) + 0.6f, y }, (float)size, 0, c); }
        w += (float)ui_text_w(ch, size) + sp;
    }
    return w - sp;
}

/* 2.0.0: the name at the top - plain letters in the Inter Display font (simple and calm, Apple-like):
 * "Q", a small orange dash, "LiM". Falls back to the old sign if the font cannot be used. */
static bool draw_wordmark2(float cx, float cy, float lmin, float rmax)
{
    Color tc = g_theme.text, acc = { 255, 163, 26, 255 };
    const float fs = 23.0f;
    float wq = GfxLogoText(1, "Q", 0, 0, fs, 0, tc, false), wl = GfxLogoText(1, "LiM", 0, 0, fs, 0, tc, false);
    if (wq <= 0.0f || wl <= 0.0f) return false;
    float asc = GfxLogoAscent(1, fs), top = roundf(cy - asc + fs * 0.727f * 0.5f);   /* capitals centred on cy */
    float dash = 9.0f, g1 = 3.0f, g2 = 3.5f;
    float total = wq + g1 + dash + g2 + wl, x = roundf(cx - total * 0.5f);
    if (x + total > rmax) x = rmax - total;
    if (x < lmin) x = lmin;
    if (x + total > rmax) return true;               /* no room (a very narrow window): nothing rather than overlap */
    GfxLogoText(1, "Q", x, top, fs, 0, tc, true);
    DrawRectangleRounded((Rectangle){ x + wq + g1, roundf(cy - 1.5f), dash, 3.0f }, 1.5f, acc);
    GfxLogoText(1, "LiM", x + wq + g1 + dash + g2, top, fs, 0, tc, true);
    return true;
}

/* the whole wordmark centred on cx (and kept between lmin and rmax) */
static void draw_wordmark(float cx, float cy, float lmin, float rmax)
{
    if (draw_wordmark2(cx, cy, lmin, rmax)) return;   /* 2.0.0: the new name */
    const int fs = 24;
    float sh = 32.0f;                                /* the sign is a bit taller than the letters */
    float gap = 6.0f;
    float wq = wm_text("Q", 0, 0, fs, g_theme.text, false);
    float ws = draw_logo_sign(0, 0, sh, false);
    float wl = wm_text("LiM", 0, 0, fs, g_theme.text, false);
    float total = wq + gap + ws + gap - 2.0f + wl;
    float x = roundf(cx - total * 0.5f);
    if (x + total > rmax) x = rmax - total;
    if (x < lmin) x = lmin;
    if (x + total > rmax) return;                    /* no room (a very narrow window): nothing rather than overlap */
    float ty = roundf(cy - fs * 0.51f);              /* DejaVu: puts the middle of the capitals on the middle of the sign */
    wm_text("Q", x, ty, fs, g_theme.text, true);
    draw_logo_sign(x + wq + gap, cy, sh, true);
    wm_text("LiM", x + wq + gap + ws + gap - 2.0f, ty, fs, g_theme.text, true);   /* "L" has a wider left side bearing */
}

/* 1.1: launch quantization (as "Global Quantization" in Ableton/Bitwig) + the master BPM + the beat in the bar.
 * Click on "Launch" = the next value, right click = the previous one. BPM: drag up/down, double click = type it. */
static const char  *QUANT_NAMES[]  = { "Now", "1 Beat", "1/2 Bar", "1 Bar", "2 Bars", "4 Bars" };   /* QUANT_* (session.h) */
#define QUANT_N QUANT_N_MODES

static float draw_transport_group(float x, float y0)
{
    Session *s = g_ui_session;
    if (!s) return x;
    Vector2 m = GetMousePosition();
    bool free_ui = !g_ti_active && !g_ctx_open && !g_file_menu_open && !g_edit_menu_open && !g_play_menu_open && !g_audio_dlg_open;
    float h = 28.0f, y = y0 + (TOP_BAR_H_BASE - h) * 0.5f;

    /* launch quantization */
    int qi = session_get_quant_mode(s);
    char ql[48];
    if (qi >= 0) snprintf(ql, sizeof ql, "Launch: %s", QUANT_NAMES[qi]);
    else snprintf(ql, sizeof ql, "Launch: %g b", (double)session_get_quantize(s));
    if (qi < 0) qi = QUANT_BAR;
    float qw = (float)ui_text_w("Launch: 1/2 Bar", 14) + 22.0f;
    Rectangle qr = { x, y, qw, h };
    bool qh = free_ui && CheckCollisionPointRec(m, qr);
    DrawRectangleRounded(qr, R_CELL, qh ? g_theme.cell_hover : g_theme.panel_bg2);
    DrawRectangleRoundedLines(qr, R_CELL, 1, qi ? g_theme.selection : g_theme.text_dim);
    ui_text(ql, (int)(x + 11), (int)(y + (h - 14) / 2) - 1, 14, qi ? g_theme.text : g_theme.text_dim);
    if (qh) {
        SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))  session_set_quant_mode(s, (qi + 1) % QUANT_N);
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) session_set_quant_mode(s, (qi + QUANT_N - 1) % QUANT_N);
    }
    x += qw + 8.0f;

    /* master BPM */
    float bw = (float)ui_text_w("000.00 BPM", 15) + 18.0f;
    Rectangle br = { x, y, bw, h };
    bool editing = g_ti_active && g_ti_numeric == 1;
    bool bh = free_ui && CheckCollisionPointRec(m, br);
    DrawRectangleRounded(br, R_CELL, (bh || g_vf_id == 4) ? g_theme.cell_hover : g_theme.panel_bg2);
    DrawRectangleRoundedLines(br, R_CELL, 1, g_theme.text_dim);
    float tempo = session_get_tempo(s);
    if (!editing) {
        char num[32], lbl[48];
        fmt_bpm(num, sizeof num, tempo);
        snprintf(lbl, sizeof lbl, "%s BPM", num);
        int tw = ui_text_w(lbl, 15);
        ui_text(lbl, (int)(x + (bw - tw) / 2), (int)(y + (h - 15) / 2) - 1, 15, g_theme.text);
    }
    float v = tempo;
    int res = value_field_input(4, br, &v, free_ui || g_vf_id == 4);
    if (res == 1) session_set_tempo(s, v);
    else if (res == 2) {           /* (the top bar is not zoomed; the text box is drawn with the zoomed rest) */
        float z = GfxGetZoom();
        num_edit_begin(1, -1, -1, tempo, (Rectangle){ x / z, (y - 1) / z, (bw > 100 ? bw : 100) / z, (h + 2) / z });
    }
    x += bw + 8.0f;

    {   /* 1.4.8: the time signature - double click = type it (7/8, 5/4, 12/8 ...) */
        int sn, sd;
        session_get_time_sig(s, &sn, &sd);
        char sl[16];
        snprintf(sl, sizeof sl, "%d/%d", sn, sd);
        float sw = (float)ui_text_w("00/00", 15) + 16.0f;
        Rectangle sr = { x, y, sw, h };
        bool sedit = g_ti_active && g_ti_numeric == 7;
        bool shv = free_ui && CheckCollisionPointRec(m, sr);
        DrawRectangleRounded(sr, R_CELL, shv ? g_theme.cell_hover : g_theme.panel_bg2);
        DrawRectangleRoundedLines(sr, R_CELL, 1, (sn != 4 || sd != 4) ? g_theme.selection : g_theme.text_dim);
        if (!sedit) ui_text(sl, (int)(x + (sw - ui_text_w(sl, 15)) / 2), (int)(y + (h - 15) / 2) - 1, 15, g_theme.text);
        if (shv) {
            static double s_sig_click;
            SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                double now = GetTime();
                if (now - s_sig_click < 0.35) {
                    float z = GfxGetZoom();
                    sig_edit_begin(sl, (Rectangle){ x / z, (y - 1) / z, (sw > 70 ? sw : 70) / z, (h + 2) / z });
                    s_sig_click = 0;
                } else s_sig_click = now;
            }
        }
        x += sw + 8.0f;
    }

    {   /* 1.3.1: the metronome (on/off) - ticks on every beat while clips play */
        bool on = atomic_load(&s->metronome);
        Rectangle mr = { x, y, 32.0f, h };
        bool mh = free_ui && CheckCollisionPointRec(m, mr);
        DrawRectangleRounded(mr, R_CELL, on ? g_theme.selection : mh ? g_theme.cell_hover : g_theme.panel_bg2);
        DrawRectangleRoundedLines(mr, R_CELL, 1, on ? g_theme.selection : g_theme.text_dim);
        Color ic = on ? g_theme.bg : g_theme.text;
        float cx = mr.x + mr.width * 0.5f, top = mr.y + 5.0f, bot = mr.y + h - 5.0f;
        /* the body: a trapezoid, the base, the pendulum leaning to one side */
        Vector2 a = { cx - 3.0f, top }, b = { cx + 3.0f, top }, c = { cx + 8.0f, bot }, d = { cx - 8.0f, bot };
        DrawLineEx(a, b, 1.6f, ic); DrawLineEx(b, c, 1.6f, ic); DrawLineEx(c, d, 1.6f, ic); DrawLineEx(d, a, 1.6f, ic);
        DrawLineEx((Vector2){ cx, bot - 3.0f }, (Vector2){ cx + 7.0f, top + 1.0f }, 1.6f, ic);
        DrawCircleV((Vector2){ cx + 3.6f, bot - 9.0f }, 1.8f, ic);
        if (mh) {
            SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) atomic_store(&s->metronome, !on);
        }
        x += mr.width + 10.0f;
    }

    /* the beat in the bar: one dot per beat of the time signature (the 1st one brighter); 1.4.8: any signature */
    double beats = 0.0;
    bool run = session_transport(s, &beats);
    int nb = 4, dn = 4;
    session_get_time_sig(s, &nb, &dn);
    double unit = 4.0 / dn, bar = nb * unit;
    int beat = run ? (int)floor((beats - floor(beats / bar + 1e-9) * bar) / unit + 1e-9) : -1;
    int nd = nb > 12 ? 12 : nb;                        /* more than 12: the dots stop, the number says it */
    float dstep = nb > 8 ? 10.0f : 14.0f, d = nb > 8 ? 7.0f : 9.0f;
    for (int i = 0; i < nd; ++i) {
        Rectangle r = { x + i * dstep, y + (h - d) / 2, d, d };
        Color c = g_theme.panel_bg2;
        if (i == beat) c = i == 0 ? g_theme.selection : g_theme.text;
        DrawRectangleRounded(r, 0.4f, c);
        DrawRectangleRoundedLines(r, 0.4f, 1, g_theme.text_dim);
    }
    x += nd * dstep;
    if (run) {
        char bb[24];
        fmt_bar_beat(bb, sizeof bb, beats);
        ui_text(bb, (int)(x + 4), (int)(y + (h - 13) / 2), 13, g_theme.text_dim);
    }
    return x + 48.0f;
}

/* ===== 1.4.17: scene transitions - [<] [Next >] [4.0 s] [AUTO] in the top bar (MIDI learn: click = bind) ===== */
void ui_scene_next(Session *s)
{
    int r = session_transition_next(s);
    char m[120];
    if (r == 0) {
        snprintf(m, sizeof m, "Fading to scene %d (%.1f s)", session_current_scene(s) + 1, (double)session_get_transition_sec(s));
    } else snprintf(m, sizeof m, "No scene with clips after this one");
    ui_flash_message(m);
}
void ui_scene_back(Session *s)
{
    int r = session_transition_back(s);
    char m[120];
    if (r == 0) {
        snprintf(m, sizeof m, "Back to scene %d (%.1f s)", session_current_scene(s) + 1, (double)session_get_transition_sec(s));
    } else snprintf(m, sizeof m, "Nothing to go back to");
    ui_flash_message(m);
}
void ui_transition_to(Session *s, int scene)
{
    if (session_transition_to(s, scene) == 0) {
        char m[120];
        snprintf(m, sizeof m, "Fading to scene %d (%.1f s)", scene + 1, (double)session_get_transition_sec(s));
        ui_flash_message(m);
    }
}
void ui_automation_toggle(Session *s)
{
    bool h = !session_automation_held(s);
    session_automation_hold(s, h);
    ui_flash_message(h ? "Automations stopped: the fades stand still, nothing starts at a time - you have the control"
                       : "Automations go on");
}
void ui_fade_time_midi(Session *s, int delta)
{
    float sec = session_get_transition_sec(s);
    if (delta >= 1000) {                                    /* an ordinary knob: 0.1 s ... 30 s, finer at the bottom */
        int v = delta - 1000;
        sec = 0.1f * powf(300.0f, (float)v / 127.0f);
    } else {
        float st = sec < 2.0f ? 0.1f : sec < 10.0f ? 0.5f : 1.0f;
        sec += st * (float)delta;
    }
    session_set_transition_sec(s, sec);
    char m[64];
    snprintf(m, sizeof m, "Fade time: %.1f s", (double)session_get_transition_sec(s));
    ui_flash_message(m);
}

static void tri(float cx, float cy, float sz, bool right, Color c)
{
    if (right) DrawTriangle((Vector2){ cx - sz * 0.5f, cy - sz * 0.6f }, (Vector2){ cx - sz * 0.5f, cy + sz * 0.6f }, (Vector2){ cx + sz * 0.6f, cy }, c);
    else       DrawTriangle((Vector2){ cx + sz * 0.5f, cy - sz * 0.6f }, (Vector2){ cx - sz * 0.6f, cy }, (Vector2){ cx + sz * 0.5f, cy + sz * 0.6f }, c);
}
/* 2.0.0: the width of the scene-transition buttons [<] [Next >] [4.0 s] [AUTO] (they sit on the right) */
static float transition_group_w(void)
{
    return 30.0f + ((float)ui_text_w("Next", 14) + 34.0f) + ((float)ui_text_w("00.0 s", 14) + 16.0f) + ((float)ui_text_w("AUTO", 13) + 18.0f) + 3 * 4.0f;
}
static float draw_transition_group(float x, float y0)
{
    Session *s = g_ui_session;
    if (!s) return x;
    Vector2 m = GetMousePosition();
    bool free_ui = !g_ti_active && !g_ctx_open && !g_file_menu_open && !g_edit_menu_open && !g_play_menu_open && !g_audio_dlg_open;
    float h = 28.0f, y = y0 + (TOP_BAR_H_BASE - h) * 0.5f;
    float prog = session_transition_progress(s);
    bool held = session_automation_held(s);
    static const int ACT[4] = { MM_SCENE_BACK, MM_SCENE_NEXT, MM_FADE_TIME, MM_AUTO_STOP };
    float wd[4] = { 30.0f, (float)ui_text_w("Next", 14) + 34.0f, (float)ui_text_w("00.0 s", 14) + 16.0f, (float)ui_text_w("AUTO", 13) + 18.0f };
    for (int b = 0; b < 4; ++b) {
        Rectangle r = { x, y, wd[b], h };
        bool hv = free_ui && CheckCollisionPointRec(m, r);
        Color bg = hv ? g_theme.cell_hover : g_theme.panel_bg2;
        Color ln = g_theme.text_dim;
        if (b == 1 && prog >= 0) ln = held ? (Color){ 240, 150, 70, 255 } : g_theme.selection;
        if (b == 3 && held) bg = fmod(GetTime(), 1.0) < 0.6 ? (Color){ 170, 95, 30, 255 } : (Color){ 120, 70, 25, 255 };
        if (g_midi_learn_mode && midimap_learning() == ACT[b]) bg = g_theme.selection;
        DrawRectangleRounded(r, R_CELL, bg);
        if (b == 1 && prog >= 0) {                          /* the transition running: how far */
            Color f = held ? (Color){ 240, 150, 70, 90 } : g_theme.selection;
            f.a = 90;
            DrawRectangleRounded((Rectangle){ x + 1, y + 1, (wd[b] - 2) * prog, h - 2 }, R_CELL, f);
        }
        DrawRectangleRoundedLines(r, R_CELL, 1, ln);
        float cy = y + h * 0.5f;
        if (b == 0) tri(x + wd[b] * 0.5f, cy, 9.0f, false, g_theme.text);
        else if (b == 1) {
            ui_text("Next", (int)(x + 9), (int)(y + (h - 14) / 2) - 1, 14, g_theme.text);
            tri(x + wd[b] - 13, cy, 9.0f, true, g_theme.text);
        } else if (b == 2) {
            char t[16];
            snprintf(t, sizeof t, "%.1f s", (double)session_get_transition_sec(s));
            ui_text(t, (int)(x + (wd[b] - ui_text_w(t, 14)) / 2), (int)(y + (h - 14) / 2) - 1, 14, g_theme.text);
        } else {
            const char *t = held ? "HELD" : "AUTO";
            ui_text(t, (int)(x + (wd[b] - ui_text_w(t, 13)) / 2), (int)(y + (h - 13) / 2) - 1, 13, held ? RAYWHITE : g_theme.text);
        }
        if (g_midi_learn_mode) {                            /* MIDI learn: click = bind a controller button / knob */
            char bd[32];
            midimap_describe(ACT[b], bd, sizeof bd);
            if (midimap_get(ACT[b])) ui_text(bd, (int)x + 2, (int)y - 1, 9, g_theme.vu_green);
            if (hv && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                midimap_learn(midimap_learning() == ACT[b] ? -1 : ACT[b]);
                ui_flash_message(b == 2 ? "Fade time: turn a knob on the controller" : "Press a button on the controller");
            }
            if (hv && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) { midimap_set(ACT[b], 0); ui_flash_message("MIDI mapping cleared"); }
        } else if (b == 2) {
            float v = session_get_transition_sec(s);
            int res = value_field_input_ex(1701, r, &v, free_ui || g_vf_id == 1701, TR_SEC_MIN, TR_SEC_MAX, 0.05f, 0.01f);
            if (res == 1) session_set_transition_sec(s, v);
            else if (res == 2) session_set_transition_sec(s, 4.0f);
            if (hv && g_vf_id == 0) {
                float wh = GetMouseWheelMove();
                if (wh != 0.0f) ui_fade_time_midi(s, wh > 0 ? 1 : -1);
            }
        } else if (hv && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (b == 0) ui_scene_back(s);
            else if (b == 1) ui_scene_next(s);
            else ui_automation_toggle(s);
        }
        x += wd[b] + (b == 3 ? 0.0f : 4.0f);
    }
    return x;
}

void draw_top_bar(int screen_w)
{
    int y0 = MENU_BAR_H_BASE;   /* 1.3.12: drawn at the normal size */

    DrawRectangle(0, y0, screen_w, TOP_BAR_H_BASE, g_theme.panel_bg);
    float group_r = draw_transport_group(12.0f, (float)y0);
    /* 2.0.0: the scene transitions [<] [Next >] [4.0 s] [AUTO] on the right, next to the library folder */
    float right = (float)(screen_w - MASTER_W - GAP - 36 - 12) - 10.0f;
    float group_l = right - transition_group_w();
    draw_transition_group(group_l, (float)y0);
    {   /* the name in the middle of the FREE space between the two groups - where the eye sees the centre
         * (the middle of the window looked too far right: the left group is longer) */
        float vis_r = group_r - 48.0f;               /* (the left group keeps 48 px for the bar counter while playing) */
        draw_wordmark((vis_r + group_l) * 0.5f, y0 + TOP_BAR_H_BASE * 0.5f, group_r + 8.0f, group_l - 16.0f);
    }

    float icon_w = 36;
    float icon_h = 28;
    float icon_x = screen_w - MASTER_W - GAP - icon_w - 12;   /* to the left of the DSP indicator */
    float icon_y = y0 + (TOP_BAR_H_BASE - icon_h) / 2;

    bool lib_open = (g_fb && g_fb->visible);
    Color icon_bg;
    if (lib_open) {
        icon_bg = (Color){ 90, 130, 200, 255 };
    } else {
        icon_bg = g_theme.panel_bg;
        icon_bg.r = (unsigned char)(icon_bg.r + 30);
        icon_bg.g = (unsigned char)(icon_bg.g + 30);
        icon_bg.b = (unsigned char)(icon_bg.b + 30);
    }
    DrawRectangleRounded((Rectangle){ icon_x, icon_y, icon_w, icon_h }, R_CELL, icon_bg);
    DrawRectangleRoundedLines((Rectangle){ icon_x, icon_y, icon_w, icon_h }, R_CELL, 1, g_theme.text_dim);

    DrawRectangleRounded((Rectangle){ icon_x + 8, icon_y + 9, 9, 5 }, 1.5f, g_theme.selection);    /* folder */
    DrawRectangleRounded((Rectangle){ icon_x + 6, icon_y + 12, 24, 10 }, 2, g_theme.selection);

    Vector2 m = GetMousePosition();
    Rectangle ir = { icon_x, icon_y, icon_w, icon_h };
    if (CheckCollisionPointRec(m, ir) &&
        IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        ui_toggle_library();
    }

    draw_dsp_widget(screen_w);
}

void draw_bottom_bar(int screen_w, int screen_h)
{
    DrawRectangle(0, screen_h - BOTTOM_BAR_H,
                  screen_w, BOTTOM_BAR_H, g_theme.panel_bg);

    const char *hint =
        "SPACE: pause | ENTER: play | DEL: clear | Ctrl+R: rename | "
        "RMB: menu | L: library | Ctrl+W: waveform | Ctrl+M: MIDI learn";
    ui_text(hint, 12, screen_h - BOTTOM_BAR_H + 8, 14, g_theme.text_dim);
}

void format_time(char *buf, size_t bufsize, double seconds)
{
    if (seconds < 0) seconds = 0;
    int total = (int)(seconds + 0.5);
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int s = total % 60;

    if (h > 0) {
        snprintf(buf, bufsize, "%02d:%02d:%02d", h, m, s);
    } else {
        snprintf(buf, bufsize, "%02d:%02d", m, s);
    }
}

double ruler_interval(double total_sec)
{
    if (total_sec < 12)    return 1;        /* close view (and SCROLL mode): a tick every second */
    if (total_sec < 30)    return 5;
    if (total_sec < 120)   return 10;
    if (total_sec < 300)   return 30;
    if (total_sec < 900)   return 60;
    if (total_sec < 1800)  return 120;
    if (total_sec < 3600)  return 300;
    return 900;
}

