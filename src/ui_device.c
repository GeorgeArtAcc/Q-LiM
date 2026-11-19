/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 2.1: the device panel (like Bitwig) - the plugins of the selected channel and the computer keyboard as MIDI keys. Moved out of ui.c. */
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <ctype.h>
#include "ui_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <ctype.h>

/* ===== 1.2: the device panel (like Bitwig) =====
 * The channel name is selected -> in place of the waveform: the channel's plugins one next to another, each with its
 * name, on/off, window button and knobs for its parameters (pages of 8). The first device of an instrument channel is
 * the instrument. "+" at the end adds a plugin or an instrument. */
#define DEV_CELL_W   62
#define DEV_CELL_H   54
#define DEV_HDR      20
static float g_dev_scroll[MAX_CHANNELS];
static int   g_dev_page[MAX_CHANNELS][MAX_CH_FX];
static int   g_kd_ch = -1, g_kd_k = -1, g_kd_p = -1;       /* knob being dragged */
static float g_kd_y0, g_kd_v0;

/* 1.2: playing from the computer keyboard (like Ableton): A W S E D F T G Y H U J K O L P = C .. D#,
 * Z / X = octave down / up, C / V = softer / harder */
static bool g_ckeys = false;
static int  g_ckeys_base = 60, g_ckeys_vel = 100;
static int  g_ckeys_down[16] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static void ckeys_release_all(void)
{
    for (int i = 0; i < 16; ++i)
        if (g_ckeys_down[i] >= 0) { midiq_push(MIDIQ_UI, 0x80, (unsigned char)g_ckeys_down[i], 0); g_ckeys_down[i] = -1; }
}
bool ui_computer_keys_on(void) { return g_ckeys; }
void ui_toggle_computer_keys(void)
{
    g_ckeys = !g_ckeys;
    if (!g_ckeys) ckeys_release_all();
    ui_flash_message(g_ckeys ? "Computer keys ON: A-K play notes, Z/X octave, C/V velocity" : "Computer keys OFF");
}
void computer_keys(Session *s)
{
    (void)s;
    if (!g_ckeys) return;
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool blocked = g_ti_active || g_ctx_open || g_fxp_ch >= 0 || g_audio_dlg_open || g_file_menu_open || dialogs_any_open() || ctrl;
    static const int keys[16] = { KEY_A, KEY_W, KEY_S, KEY_E, KEY_D, KEY_F, KEY_T, KEY_G, KEY_Y, KEY_H, KEY_U, KEY_J,
                                  KEY_K, KEY_O, KEY_L, KEY_P };
    for (int i = 0; i < 16; ++i) {
        if (!blocked && IsKeyPressed(keys[i]) && g_ckeys_down[i] < 0) {
            int n = g_ckeys_base + i;
            if (n >= 0 && n < 128) { midiq_push(MIDIQ_UI, 0x90, (unsigned char)n, (unsigned char)g_ckeys_vel); g_ckeys_down[i] = n; }
        }
        if (g_ckeys_down[i] >= 0 && !IsKeyDown(keys[i])) {
            midiq_push(MIDIQ_UI, 0x80, (unsigned char)g_ckeys_down[i], 0);
            g_ckeys_down[i] = -1;
        }
    }
    if (blocked) return;
    char msg[64];
    if (IsKeyPressed(KEY_Z) && g_ckeys_base >= 12) { g_ckeys_base -= 12; snprintf(msg, sizeof msg, "Octave C%d", g_ckeys_base / 12 - 1); ui_flash_message(msg); }
    if (IsKeyPressed(KEY_X) && g_ckeys_base <= 96) { g_ckeys_base += 12; snprintf(msg, sizeof msg, "Octave C%d", g_ckeys_base / 12 - 1); ui_flash_message(msg); }
    if (IsKeyPressed(KEY_C) && g_ckeys_vel > 20) { g_ckeys_vel -= 20; snprintf(msg, sizeof msg, "Velocity %d", g_ckeys_vel); ui_flash_message(msg); }
    if (IsKeyPressed(KEY_V) && g_ckeys_vel < 127) { g_ckeys_vel += 20; if (g_ckeys_vel > 127) g_ckeys_vel = 127; snprintf(msg, sizeof msg, "Velocity %d", g_ckeys_vel); ui_flash_message(msg); }
}

int device_panel_channel(Session *s)
{
    if (!g_ch_focus || g_chsel_n != 1 || g_chsel_anchor < 0 || g_chsel_anchor >= s->channel_count) return -1;
    return g_chsel_anchor;
}

static void draw_arc(Vector2 c, float r, float a0, float a1, float th, Color col)
{
    int seg = (int)(fabsf(a1 - a0) / 0.12f) + 1;
    for (int i = 0; i < seg; ++i) {
        float u0 = a0 + (a1 - a0) * i / seg, u1 = a0 + (a1 - a0) * (i + 1) / seg;
        DrawLineEx((Vector2){ c.x + cosf(u0) * r, c.y + sinf(u0) * r }, (Vector2){ c.x + cosf(u1) * r, c.y + sinf(u1) * r }, th, col);
    }
}

/* one knob: drag up/down (Shift = fine), mouse wheel */
static void dev_knob(Session *s, int ch, int k, QPlug *qp, int pi, float x, float y, Color col, bool active)
{
    Vector2 m = GetMousePosition();
    Rectangle cell = { x, y, DEV_CELL_W, DEV_CELL_H };
    bool free = fx_ui_free();
    bool hov = free && CheckCollisionPointRec(m, cell);
    bool drag = g_kd_ch == ch && g_kd_k == k && g_kd_p == pi;
    float v = qplug_param_get(qp, pi);
    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        g_kd_ch = ch; g_kd_k = k; g_kd_p = pi; g_kd_y0 = m.y; g_kd_v0 = v; drag = true;
    }
    if (drag) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            bool fine = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            float nv = g_kd_v0 + (g_kd_y0 - m.y) * (fine ? 0.001f : 0.006f);
            if (nv < 0) nv = 0;
            if (nv > 1) nv = 1;
            if (fabsf(nv - v) > 1e-5f) { qplug_param_set(qp, pi, nv); v = nv; }
            SetMouseCursor(MOUSE_CURSOR_RESIZE_NS);
        } else { g_kd_ch = g_kd_k = g_kd_p = -1; drag = false; }
    }
    if (hov && !drag) {
        float wh = GetMouseWheelMove();
        if (wh != 0.0f) {
            float nv = v + wh * ((IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) ? 0.002f : 0.02f);
            if (nv < 0) nv = 0;
            if (nv > 1) nv = 1;
            qplug_param_set(qp, pi, nv);
            v = nv;
        }
    }
    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {       /* 1.4.14: MIDI Learn / Forget */
        g_fxm_ch = ch; g_fxm_idx = k; g_fxm_param = pi; g_fxm_pos = m; g_fxm_just = true;
    }
    if (hov || drag) DrawRectangleRounded(cell, R_SMALL, g_theme.cell_hover);
    {
        int lch, lk, lp;
        if (fxchain_learning(&lch, &lk, &lp) && lch == ch && lk == k && lp == pi) {   /* the learn target blinks */
            Color bc = g_theme.selection;
            bc.a = (unsigned char)(140 + 115 * sinf((float)GetTime() * 8.0f));
            DrawRectangleRoundedLines(cell, R_SMALL, 2, bc);
        }
        int cc = session_fx_midi_get(s, ch, k, pi);
        if (cc >= 0) {
            char cl[12];
            snprintf(cl, sizeof cl, "CC%d", cc);
            ui_text(cl, (int)(x + DEV_CELL_W - ui_text_w(cl, 9) - 2), (int)y + 1, 9, g_theme.selection);
        }
    }
    Vector2 c = { x + DEV_CELL_W / 2.0f, y + 15 };
    const float A0 = 2.356f, A1 = 7.069f, R = 11;               /* 135 deg .. 405 deg (a 270 deg arc) */
    Color off = g_theme.grid_line;
    draw_arc(c, R, A0, A1, 3, off);
    Color on = active ? col : g_theme.text_dim;
    if (v > 0.001f) draw_arc(c, R, A0, A0 + (A1 - A0) * v, 3, on);
    float a = A0 + (A1 - A0) * v;
    DrawLineEx(c, (Vector2){ c.x + cosf(a) * (R - 3), c.y + sinf(a) * (R - 3) }, 2, (hov || drag) ? g_theme.text : on);
    char nm[64];
    qplug_param_name(qp, pi, nm, sizeof nm);
    while (ui_text_w(nm, 10) > DEV_CELL_W - 4 && strlen(nm) > 2) utf8_chop_last(nm);
    int nw = ui_text_w(nm, 10);
    ui_text(nm, (int)(x + (DEV_CELL_W - nw) / 2), (int)(y + 29), 10, active ? g_theme.text : g_theme.text_dim);
    char val[64];
    qplug_param_text(qp, pi, val, sizeof val);
    while (ui_text_w(val, 10) > DEV_CELL_W - 4 && strlen(val) > 2) utf8_chop_last(val);
    int vw = ui_text_w(val, 10);
    ui_text(val, (int)(x + (DEV_CELL_W - vw) / 2), (int)(y + 41), 10, (hov || drag) ? g_theme.text : g_theme.text_dim);
}

void draw_device_panel(Session *s, int ch, float x, float y, float w, float h)
{
    Channel *chp = &s->channels[ch];
    Color col = channel_color(chp->color_id);
    Vector2 m = GetMousePosition();
    bool free = fx_ui_free();
    /* header line: the channel, what it is, and who gets the MIDI keyboard */
    DrawRectangle((int)x, (int)y, (int)w, WAVE_HDR_H, g_theme.panel_bg);
    bool inst = fxchain_has_instrument(s, ch);
    bool armed = inst && session_get_midi_target(s) == ch;
    char hdr[320];
    snprintf(hdr, sizeof hdr, "%s  |  %s  |  %d plugin%s", chp->name, inst ? "Instrument channel" : "Audio channel",
             chp->fx_count, chp->fx_count == 1 ? "" : "s");
    ui_text(hdr, (int)(x + 10), (int)(y + 4), 14, g_theme.text);
    {   /* 1.4.14: MIDI Learn in progress on this channel */
        int lch, lk, lp;
        if (fxchain_learning(&lch, &lk, &lp) && lch == ch) {
            QPlug *lq = fxchain_find(s, lch, lk);
            char lt[200], pn[64] = "";
            if (lq && lp >= 0) qplug_param_name(lq, lp, pn, sizeof pn);
            if (lp < 0) snprintf(lt, sizeof lt, "MIDI LEARN: move a parameter of %s  (Esc = cancel)", chp->fx[lk].name);
            else snprintf(lt, sizeof lt, "MIDI LEARN: %s - now move a control on your MIDI controller  (Esc)", pn);
            int tw = ui_text_w(hdr, 14);
            Color lc = g_theme.selection;
            lc.a = (unsigned char)(170 + 85 * sinf((float)GetTime() * 6.0f));
            ui_text(lt, (int)(x + 24 + tw), (int)(y + 5), 12, lc);
            if (IsKeyPressed(KEY_ESCAPE) && g_fxm_ch < 0) fxchain_learn_cancel();
        }
    }
    if (inst) {
        const char *ml = armed ? "MIDI keyboard plays this channel" : "Click to play this channel from the MIDI keyboard";
        int mw = ui_text_w(ml, 12);
        Rectangle mr = { x + w - mw - 22, y + 2, (float)mw + 14, WAVE_HDR_H - 4 };
        bool mh = free && !armed && CheckCollisionPointRec(m, mr);
        if (armed) DrawRectangleRounded(mr, R_SMALL, col);
        else DrawRectangleRoundedLines(mr, R_SMALL, 1, mh ? g_theme.text : g_theme.grid_line);
        ui_text(ml, (int)(mr.x + 7), (int)(mr.y + 3), 12, armed ? ui_text_color_on(col) : (mh ? g_theme.text : g_theme.text_dim));
        if (mh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) session_set_midi_target(s, ch);
        /* the computer keyboard switch */
        const char *kl = g_ckeys ? "Computer keys: ON  (A-K, Z/X octave)" : "Computer keys: OFF";
        int kw = ui_text_w(kl, 12);
        Rectangle kr = { mr.x - kw - 22, y + 2, (float)kw + 14, WAVE_HDR_H - 4 };
        bool kh = free && CheckCollisionPointRec(m, kr);
        if (g_ckeys) DrawRectangleRounded(kr, R_SMALL, g_theme.selection);
        else DrawRectangleRoundedLines(kr, R_SMALL, 1, kh ? g_theme.text : g_theme.grid_line);
        ui_text(kl, (int)(kr.x + 7), (int)(kr.y + 3), 12, g_ckeys ? g_theme.bg : (kh ? g_theme.text : g_theme.text_dim));
        if (kh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ui_toggle_computer_keys();
    }
    float by = y + WAVE_HDR_H, bh = h - WAVE_HDR_H;
    DrawRectangle((int)x, (int)by, (int)w, (int)bh, g_theme.wave_bg);
    int rows = (int)((bh - DEV_HDR - 8) / DEV_CELL_H);
    if (rows < 1) rows = 1;
    if (rows > 4) rows = 4;
    int cols = rows == 1 ? 8 : 4;
    int per_page = rows * cols;
    float gap = 8;
    float dwid[MAX_CH_FX];                            /* each device is as wide as its knobs (at least 170) */
    float total = 120;
    for (int k = 0; k < chp->fx_count; ++k) {
        QPlug *q = fxchain_find(s, ch, k);
        int np = q ? qplug_param_count(q) : 0;
        int used = np < cols ? np : cols;
        dwid[k] = used * DEV_CELL_W + 12;
        if (dwid[k] < 170) dwid[k] = 170;
        total += dwid[k] + gap;
    }
    float maxs = total - w; if (maxs < 0) maxs = 0;
    if (free && CheckCollisionPointRec(m, (Rectangle){ x, by, w, bh }) && g_kd_ch < 0) {
        float wh = GetMouseWheelMove();
        bool over_knob = false;                     /* the wheel over a knob changes the knob, elsewhere it scrolls */
        float fx0 = x + 6 - g_dev_scroll[ch];
        for (int k = 0; k < chp->fx_count; ++k) {
            Rectangle body = { fx0 + 6, by + 6 + DEV_HDR, dwid[k] - 12, rows * DEV_CELL_H };
            if (CheckCollisionPointRec(m, body)) over_knob = true;
            fx0 += dwid[k] + gap;
        }
        if (wh != 0.0f && !over_knob) g_dev_scroll[ch] -= wh * 60;
    }
    if (g_dev_scroll[ch] > maxs) g_dev_scroll[ch] = maxs;
    if (g_dev_scroll[ch] < 0) g_dev_scroll[ch] = 0;
    BeginScissorMode((int)x, (int)by, (int)w, (int)bh);
    float dx = x + 6 - g_dev_scroll[ch];
    for (int k = 0; k < chp->fx_count; dx += dwid[k] + gap, ++k) {
        const ChFx *fx = &chp->fx[k];
        float dev_w = dwid[k];
        Rectangle box = { dx, by + 6, dev_w, bh - 12 };
        if (box.x > x + w || box.x + box.width < x) continue;
        QPlug *qp = fxchain_find(s, ch, k);
        bool failed = !fx->bypass && fxchain_slot_failed(s, ch, k);
        bool loading = !failed && fxchain_slot_loading(s, ch, k);
        bool active = !fx->bypass && qp;
        bool is_inst = qp && fxchain_slot_is_instrument(s, ch, k);
        Color bg = g_theme.panel_bg;
        DrawRectangleRounded(box, R_SMALL, bg);
        DrawRectangleRoundedLines(box, R_SMALL, 1, qplug_editor_is_open(qp) ? col : g_theme.grid_line);
        /* the device header: power | name | INST/format | page | window */
        Rectangle hr = { box.x, box.y, box.width, DEV_HDR };
        Color hb = active ? col : g_theme.panel_bg2;
        hb.a = active ? 70 : 255;
        DrawRectangleRounded(hr, R_SMALL, hb);
        Vector2 pc = { hr.x + 11, hr.y + DEV_HDR / 2.0f };
        Rectangle pwr = { hr.x + 2, hr.y, 18, DEV_HDR };
        bool pwh = free && CheckCollisionPointRec(m, pwr);
        if (fx->bypass) DrawCircleLines((int)pc.x, (int)pc.y, 5, pwh ? g_theme.text : g_theme.text_dim);
        else DrawCircleV(pc, 5, pwh ? g_theme.text : col);
        if (pwh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) session_fx_toggle_bypass(s, ch, k);
        /* window button */
        Rectangle wb = { hr.x + hr.width - 22, hr.y + 3, 18, DEV_HDR - 6 };
        bool has_ed = qp && qplug_has_editor(qp);
        bool wbh = free && has_ed && CheckCollisionPointRec(m, wb);
        if (has_ed) {
            DrawRectangleLinesEx(wb, 1, wbh ? g_theme.text : g_theme.text_dim);
            DrawRectangle((int)wb.x, (int)wb.y, (int)wb.width, 3, wbh ? g_theme.text : g_theme.text_dim);
            if (wbh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) fx_open_window(s, ch, k);
        }
        int np = qp ? qplug_param_count(qp) : 0;
        int pages = np > 0 ? (np + per_page - 1) / per_page : 1;
        int *pg = &g_dev_page[ch][k];
        if (*pg >= pages) *pg = pages - 1;
        if (*pg < 0) *pg = 0;
        float right = wb.x - 4;
        if (pages > 1) {                                  /* < 2/5 > */
            char pl[24]; snprintf(pl, sizeof pl, "%d/%d", *pg + 1, pages);
            int plw = ui_text_w(pl, 10);
            Rectangle nx = { right - 12, hr.y + 2, 12, DEV_HDR - 4 };
            Rectangle pv = { right - 12 - plw - 16, hr.y + 2, 12, DEV_HDR - 4 };
            bool nh = free && CheckCollisionPointRec(m, nx), ph = free && CheckCollisionPointRec(m, pv);
            float cy = hr.y + DEV_HDR / 2.0f;
            DrawTriangle((Vector2){ nx.x + 3, cy - 4 }, (Vector2){ nx.x + 3, cy + 4 }, (Vector2){ nx.x + 9, cy }, nh ? g_theme.text : g_theme.text_dim);
            DrawTriangle((Vector2){ pv.x + 9, cy - 4 }, (Vector2){ pv.x + 3, cy }, (Vector2){ pv.x + 9, cy + 4 }, ph ? g_theme.text : g_theme.text_dim);
            ui_text(pl, (int)(pv.x + 14), (int)(hr.y + 5), 10, g_theme.text_dim);
            if (nh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) *pg = (*pg + 1) % pages;
            if (ph && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) *pg = (*pg + pages - 1) % pages;
            right = pv.x - 4;
        }
        const char *tag = failed ? "ERROR" : loading ? "starting..." : is_inst ? "INST" : plugins_fmt_name(plugins_fmt_index(fx->fmt));
        int tgw = ui_text_w(tag, 9);
        ui_text(tag, (int)(right - tgw), (int)hr.y + 6, 9, failed ? FX_FAIL_COL : (is_inst ? g_theme.text : g_theme.text_dim));
        char nm[96]; snprintf(nm, sizeof nm, "%s", fx->name);
        float maxw = right - tgw - 6 - (hr.x + 22);
        while (ui_text_w(nm, 12) > maxw && strlen(nm) > 2) utf8_chop_last(nm);
        ui_text(nm, (int)hr.x + 22, (int)hr.y + 3, 12, active ? g_theme.text : g_theme.text_dim);
        /* right click on the header: the slot menu (open window / bypass / remove) */
        if (free && CheckCollisionPointRec(m, hr) && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            g_fxm_ch = ch; g_fxm_idx = k; g_fxm_param = -1; g_fxm_pos = m; g_fxm_just = true;
        }
        /* double click on the name: the plugin's own window */
        if (free && CheckCollisionPointRec(m, (Rectangle){ hr.x + 22, hr.y, maxw, DEV_HDR }) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            double t = GetTime();
            if (g_fx_click_ch == ch && g_fx_click_k == k && t - g_fx_click_t < 0.4) { fx_open_window(s, ch, k); g_fx_click_t = -1; }
            else { g_fx_click_t = t; g_fx_click_ch = ch; g_fx_click_k = k; }
        }
        /* the knobs */
        float ky = box.y + DEV_HDR + 4;
        if (!qp) {
            const char *msg = failed ? "Could not load this plugin" : loading ? "Starting..." : fx->bypass ? "Off" : "Loading...";
            ui_text(msg, (int)box.x + 10, (int)ky + 10, 12, failed ? FX_FAIL_COL : g_theme.text_dim);
        } else if (np == 0) {
            ui_text("No parameters", (int)box.x + 10, (int)ky + 10, 12, g_theme.text_dim);
            if (has_ed) ui_text("open its window", (int)box.x + 10, (int)ky + 26, 11, g_theme.text_dim);
        } else {
            for (int i = 0; i < per_page; ++i) {
                int pi = *pg * per_page + i;
                if (pi >= np) break;
                float kx = box.x + 6 + (i % cols) * DEV_CELL_W;
                float kyy = ky + (i / cols) * DEV_CELL_H;
                dev_knob(s, ch, k, qp, pi, kx, kyy, col, active);
            }
        }
    }
    /* "+": add a plugin / an instrument */
    if (chp->fx_count < MAX_CH_FX) {
        Rectangle pr = { dx, by + 6, 100, bh - 12 };
        bool ph = free && CheckCollisionPointRec(m, pr);
        DrawRectangleRounded(pr, R_SMALL, ph ? g_theme.cell_hover : g_theme.panel_bg);
        DrawRectangleRoundedLines(pr, R_SMALL, 1, ph ? col : g_theme.grid_line);
        int pw = ui_text_w("+", 24);
        ui_text("+", (int)(pr.x + (pr.width - pw) / 2), (int)(pr.y + pr.height / 2 - 20), 24, ph ? g_theme.text : g_theme.text_dim);
        const char *l1 = chp->fx_count == 0 ? "Add instrument" : "Add plugin";
        int l1w = ui_text_w(l1, 10);
        ui_text(l1, (int)(pr.x + (pr.width - l1w) / 2), (int)(pr.y + pr.height / 2 + 8), 10, g_theme.text_dim);
        if (chp->fx_count == 0) {
            int l2w = ui_text_w("or effect", 10);
            ui_text("or effect", (int)(pr.x + (pr.width - l2w) / 2), (int)(pr.y + pr.height / 2 + 20), 10, g_theme.text_dim);
        }
        if (ph && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            g_fxp_ch = ch; g_fxp_pos = (Vector2){ pr.x, pr.y + 10 }; g_fxp_filter[0] = 0; g_fxp_scroll = 0; g_fxp_just = true;
            plugins_count();
        }
    }
    EndScissorMode();
    if (total > w) {                                      /* a thin scroll bar */
        float tw = w * w / total, tx = x + (w - tw) * (g_dev_scroll[ch] / (maxs > 0 ? maxs : 1));
        DrawRectangle((int)tx, (int)(y + h - 3), (int)tw, 3, g_theme.text_dim);
    }
}

