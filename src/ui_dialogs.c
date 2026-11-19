/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#include "ui_dialogs.h"
#include "ui.h"
#include "audio.h"
#include "theme.h"
#include "sdlgfx.h"
#include "export.h"
#include "separate.h"
#include "schedule.h"
#include "outx.h"
#include "mcfile.h"
#include "wav.h"
static int g_sm_src;   /* 2.0: the Speaker Map (below): 0 = the clip's file channels, 1 = the surround speakers */
#include "undo.h"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <time.h>
#include <math.h>
#include "project.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ============ common helpers ============ */

static Color lighten(Color c, int d)
{
    int r = c.r + d, g = c.g + d, b = c.b + d;
    c.r = (unsigned char)(r > 255 ? 255 : (r < 0 ? 0 : r));
    c.g = (unsigned char)(g > 255 ? 255 : (g < 0 ? 0 : g));
    c.b = (unsigned char)(b > 255 ? 255 : (b < 0 ? 0 : b));
    return c;
}

static void draw_frame(int x, int y, int w, int h, const char *title, double open_time)
{
    double t = GetTime() - open_time;
    float alpha = t < 0.12 ? (float)(t / 0.12) : 1.0f;
    if (alpha < 0.0f) alpha = 0.0f;

    Color bg = lighten(g_theme.panel_bg, 10);
    bg.a = (unsigned char)(bg.a * alpha);
    DrawRectangle(x, y, w, h, bg);
    Color border = g_theme.selection;
    border.a = (unsigned char)(border.a * alpha);
    DrawRectangleLinesEx((Rectangle){ (float)x, (float)y, (float)w, (float)h }, 2, border);

    Color tb = g_theme.selection;
    tb.a = (unsigned char)(255 * alpha);
    DrawRectangle(x, y, w, 40, tb);
    ui_text(title, x + 16, y + 10, 20, g_theme.bg);
}

static bool button(int x, int y, int w, int h, const char *label, bool primary)
{
    Vector2 m = GetMousePosition();
    Rectangle r = { (float)x, (float)y, (float)w, (float)h };
    bool hov = CheckCollisionPointRec(m, r);
    Color bg = primary ? g_theme.selection : lighten(g_theme.panel_bg2, hov ? 22 : 8);
    if (primary && hov) bg = lighten(bg, 25);
    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, 1, primary ? g_theme.selection : g_theme.text_dim);
    int tw = ui_text_w(label, 15);
    ui_text(label, x + (w - tw) / 2, y + (h - 15) / 2 - 1, 15, primary ? g_theme.bg : g_theme.text);
    return hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

/* ============ Routing ============ */

#define R_CELL   22
#define R_PITCH  26     /* maximum; with many outputs it shrinks (see hp) */
#define R_LABEL_W 190
#define R_GAP    18
#define R_ROW_H  R_PITCH
#define R_BLOCK_H (2 * R_PITCH + 8)

static bool   g_routing_open = false;
static double g_routing_t = 0.0;
static float  g_routing_scroll = 0.0f;
static int    g_routing_hcol = 0;       /* the first visible column of outputs (with many outputs - horizontal scroll) */
static bool   g_routing_hdrag = false;   /* the horizontal bar is being dragged */
static bool   g_painting = false;
static bool   g_paint_val = false;

void ui_open_routing(void)  { g_routing_open = true; g_routing_t = GetTime(); g_routing_scroll = 0.0f; g_routing_hcol = 0; g_routing_hdrag = false; g_painting = false; }

static int hw_outputs(void)
{
    int n = audio_total_outputs();                 /* 1.4.21: + the extra cards */
    if (n < 2) n = 2;
    if (n > MAX_HW_OUT) n = MAX_HW_OUT;
    return n;
}

static bool g_cards_open;                          /* 1.4.21 (below) */
/* 1.4.21: the delay of the outputs (Routing): the chosen output, dragging */
static int   g_dsel = -1;
static bool  g_ddrag = false;
static float g_ddrag_y0, g_ddrag_v0;
static void delay_set_save(int k, float ms, bool save)
{
    ms = roundf(ms * 10.0f) / 10.0f;
    outx_delay_set(k, ms);
    if (save) outx_save();
}
static void fmt_ms(char *b, size_t n, float ms)
{
    if (ms <= 0.0f) snprintf(b, n, "0");
    else if (ms < 10.0f && roundf(ms) != ms) snprintf(b, n, "%.1f", (double)ms);
    else snprintf(b, n, "%.0f", (double)ms);
}

static bool route_bit(Session *s, int row_ch, int side, int col)
{
    if (row_ch < 0) return (session_master_route_get(s, side) >> (col - 2)) & 1u;
    return (session_route_get(s, row_ch, side) >> col) & 1u;
}

static void route_set_bit(Session *s, int row_ch, int side, int col, bool on)
{
    if (row_ch < 0) {
        RouteMask m = session_master_route_get(s, side);
        if (on) m |= RBIT(col - 2); else m &= ~RBIT(col - 2);
        session_master_route_set(s, side, m);
    } else {
        RouteMask m = session_route_get(s, row_ch, side);
        if (on) m |= RBIT(col); else m &= ~RBIT(col);
        session_route_set(s, row_ch, side, m);
    }
}

static void draw_routing(Session *s, int sw, int sh)
{
    GfxDialogBackdrop(70);

    int n_hw = hw_outputs();
    int ncols = 2 + n_hw;

    /* Column width: shrinks so that all outputs fit, but not below HP_MIN (otherwise the boxes
     * become too small to click and the numbers unreadable). If they still do not fit - the outputs scroll
     * horizontally (Shift + wheel, wheel over the numbers or dragging the bar below); Master L/R and
     * the channel names stay in place. */
    #define HP_MIN 22
    int hp = R_PITCH;
    int avail_w = sw - 20 - 60 - R_LABEL_W - R_GAP;
    if (avail_w < 2 * HP_MIN + 4 * HP_MIN) avail_w = 6 * HP_MIN;
    if (ncols * hp > avail_w) hp = avail_w / ncols;
    if (hp < HP_MIN) hp = HP_MIN;
    int vis_hw = (avail_w / hp) - 2;                 /* how many outputs are visible at once */
    if (vis_hw < 4) vis_hw = 4;
    if (vis_hw > n_hw) vis_hw = n_hw;
    int max_hcol = n_hw - vis_hw;
    if (g_routing_hcol > max_hcol) g_routing_hcol = max_hcol;
    if (g_routing_hcol < 0) g_routing_hcol = 0;
    const int hcol = g_routing_hcol;
    int hc = hp - 4;                                 /* box width */
    if (hc > R_CELL) hc = R_CELL;
    int matrix_w = R_LABEL_W + (2 + vis_hw) * hp + R_GAP;
    int dlg_w = matrix_w + 60;
    if (dlg_w < 920) dlg_w = 920;                    /* 1.4.21: the delay editor at the bottom needs the room */
    if (dlg_w > sw - 20) dlg_w = sw - 20;
    int dlg_h = sh - 40;
    if (dlg_h > 780) dlg_h = 780;
    int dx = (sw - dlg_w) / 2, dy = (sh - dlg_h) / 2;

    draw_frame(dx, dy, dlg_w, dlg_h, "Routing", g_routing_t);

    int mx0 = dx + (dlg_w - matrix_w) / 2;          /* left edge of the matrix */
    int colx0 = mx0 + R_LABEL_W;                    /* x of column 0 */
    #define COL_X(c) (colx0 + ((c) >= 2 ? (c) - hcol : (c)) * hp + ((c) >= 2 ? R_GAP : 0))
    #define COL_VIS(c) ((c) < 2 || ((c) - 2 >= hcol && (c) - 2 < hcol + vis_hw))

    char info[200];
    AudioOutGroup grp[1 + OUTX_MAX];
    int ngrp = audio_output_groups(grp, 1 + OUTX_MAX);
    if (vis_hw < n_hw)
        snprintf(info, sizeof info, "Outputs: %d   (showing %d-%d - Shift + mouse wheel or the bar below to scroll)",
                 n_hw, hcol + 1, hcol + vis_hw);
    else
        snprintf(info, sizeof info, "Outputs: %d%s", n_hw, ngrp > 1 && n_hw > grp[0].count ? "   (on more than one sound card)" : "   (detected from your audio interface)");
    ui_text(info, mx0, dy + 48, 13, g_theme.text_dim);
    ui_text("Click or drag over the squares to connect.",
            mx0, dy + 66, 13, g_theme.text_dim);

    /* column headers: MASTER IN, then every sound card over its outputs (1.4.21) */
    int hy = dy + 92;
    ui_text("MASTER IN", COL_X(0), hy, 11, g_theme.text_dim);
    for (int gi = 0; gi < ngrp; ++gi) {
        int a = grp[gi].first, b = grp[gi].first + grp[gi].count - 1;   /* 0-based outputs */
        if (a < 0 || grp[gi].count <= 0) continue;
        if (b >= n_hw) b = n_hw - 1;
        int va = a < hcol ? hcol : a, vb = b > hcol + vis_hw - 1 ? hcol + vis_hw - 1 : b;
        if (va > vb) continue;
        int x0 = COL_X(2 + va), x1 = COL_X(2 + vb) + hc;
        Color gc = gi == 0 ? g_theme.text_dim : grp[gi].state == 0 ? (Color){ 110, 200, 130, 255 }
                 : grp[gi].state == 1 ? (Color){ 230, 190, 80, 255 } : (Color){ 230, 100, 90, 255 };
        char gn[170];
        snprintf(gn, sizeof gn, "%s", gi == 0 && ngrp == 1 ? "SOUND CARD OUT" : grp[gi].name);
        while (gn[0] && ui_text_w(gn, 11) > x1 - x0 && strlen(gn) > 2) gn[strlen(gn) - 1] = 0;
        ui_text(gn, x0, hy, 11, gc);
        if (ngrp > 1) DrawRectangle(x0, hy + 13, x1 - x0, 2, gi == 0 ? g_theme.grid_line : gc);
    }
    int ly = hy + 16;
    for (int c = 0; c < ncols; ++c) {
        if (!COL_VIS(c)) continue;
        char lb[8];
        if (c == 0) snprintf(lb, sizeof lb, "L");
        else if (c == 1) snprintf(lb, sizeof lb, "R");
        else snprintf(lb, sizeof lb, "%d", c - 1);
        int tw = ui_text_w(lb, hp < 20 ? 11 : 13);
        ui_text(lb, COL_X(c) + (hc - tw) / 2, ly, hp < 20 ? 11 : 13, g_theme.text);
    }

    Vector2 mouse = GetMousePosition();
    int hover_col = -1, hover_row_id = -2, hover_side = 0, hover_ch = -2;   /* hover_ch: -1 = master */
    bool did_click = false;

    /* one row of the matrix */
    #define DRAW_ROW(ry, row_ch, side, rowcol) do { \
        for (int c = 0; c < ncols; ++c) { \
            if (!COL_VIS(c)) continue; \
            int cx = COL_X(c); \
            Rectangle cr = { (float)cx, (float)(ry), (float)hc, (float)R_CELL }; \
            bool avail = !((row_ch) < 0 && c < 2); \
            bool on = avail && route_bit(s, (row_ch), (side), c); \
            bool hov = avail && CheckCollisionPointRec(mouse, cr) && mouse.y >= list_top && mouse.y < list_bot; \
            Color bgc = c < 2 ? lighten(g_theme.panel_bg2, 6) : g_theme.cell_empty; \
            if (!avail) bgc = lighten(g_theme.panel_bg, 4); \
            DrawRectangleRec(cr, bgc); \
            DrawRectangleLinesEx(cr, 1, hov ? g_theme.text : g_theme.grid_line); \
            if (on) DrawRectangle(cx + 3, (ry) + 4, hc - 6, R_CELL - 8, (rowcol)); \
            if (hov) { hover_col = c; hover_ch = (row_ch); hover_side = (side); hover_row_id = 1; } \
        } } while (0)

    /* Master rows (pinned) */
    int my = ly + 24;
    ui_text("MASTER OUT", mx0, my, 11, g_theme.text_dim);
    my += 16;
    int dly_y = my + 2 * R_ROW_H + 4;                 /* 1.4.21: the delay of every output */
    int list_top = dly_y + R_ROW_H + 14;
    int list_bot = dy + dlg_h - 104;
    {
        /* master is not in the scrolling part -> the hover check for list_top/bot must not block it */
        int saved_top = list_top, saved_bot = list_bot;
        list_top = 0; list_bot = sh;
        for (int side = 0; side < 2; ++side) {
            int ry = my + side * R_ROW_H;
            DrawRectangle(mx0, ry, 4, R_CELL, g_theme.selection);
            ui_text(side == 0 ? "Master L" : "Master R", mx0 + 12, ry + 3, 14, g_theme.text);
            DRAW_ROW(ry, -1, side, g_theme.selection);
        }
        list_top = saved_top; list_bot = saved_bot;
    }
    /* 1.4.21: the Delay row - click a box to choose the output, drag up / down (Shift = fine), right click = 0 */
    bool dly_ok = audio_outx_possible();
    {
        DrawRectangle(mx0, dly_y, 4, R_CELL, (Color){ 120, 200, 255, 255 });
        ui_text("Delay (ms)", mx0 + 12, dly_y + 3, 14, dly_ok ? g_theme.text : g_theme.text_dim);
        for (int c = 2; c < ncols; ++c) {
            if (!COL_VIS(c)) continue;
            int k = c - 2, cx = COL_X(c);
            Rectangle cr = { (float)cx, (float)dly_y, (float)hc, (float)R_CELL };
            bool hov = dly_ok && CheckCollisionPointRec(mouse, cr);
            float v = outx_delay_get(k);
            DrawRectangleRec(cr, k == g_dsel ? (Color){ 40, 80, 120, 255 } : g_theme.cell_empty);
            DrawRectangleLinesEx(cr, 1, hov || k == g_dsel ? (Color){ 120, 200, 255, 255 } : g_theme.grid_line);
            char vb[16];
            if (!dly_ok) snprintf(vb, sizeof vb, "-"); else fmt_ms(vb, sizeof vb, v);
            int fs = ui_text_w(vb, 11) > hc - 2 ? 9 : 11;
            ui_text(vb, cx + (hc - ui_text_w(vb, fs)) / 2, dly_y + (R_CELL - fs) / 2, fs, v > 0 ? RAYWHITE : g_theme.text_dim);
            if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { g_dsel = k; g_ddrag = true; g_ddrag_y0 = mouse.y; g_ddrag_v0 = v; }
            if (hov && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) { g_dsel = k; delay_set_save(k, 0.0f, true); }
            if (hov) {
                float wh = GetMouseWheelMove();
                if (wh != 0.0f) { g_dsel = k; delay_set_save(k, v + (wh > 0 ? 1.0f : -1.0f) * ((IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) ? 0.1f : 1.0f), true); }
            }
        }
        if (g_ddrag) {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && g_dsel >= 0) {
                bool fine = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
                float nv = g_ddrag_v0 + (g_ddrag_y0 - mouse.y) * (fine ? 0.1f : 1.0f);
                if (nv != outx_delay_get(g_dsel)) delay_set_save(g_dsel, nv, false);
            } else { g_ddrag = false; outx_save(); }
        }
    }
    DrawLine(mx0, list_top - 6, mx0 + matrix_w, list_top - 6, g_theme.text_dim);

    /* Channels (scrolling) */
    int nch = s->channel_count;
    float total_h = (float)(nch * R_BLOCK_H);
    float view_h = (float)(list_bot - list_top);
    float max_scroll = total_h > view_h ? total_h - view_h : 0.0f;

    Rectangle list_r = { (float)(dx), (float)list_top, (float)dlg_w, view_h };
    if (CheckCollisionPointRec(mouse, list_r) && !(max_hcol > 0 && (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)))) {
        float wh = GetMouseWheelMove();
        if (wh != 0.0f) g_routing_scroll -= wh * 60.0f;
    }
    if (g_routing_scroll > max_scroll) g_routing_scroll = max_scroll;
    if (g_routing_scroll < 0) g_routing_scroll = 0;

    BeginScissorMode(dx, list_top, dlg_w, (int)view_h);
    for (int ch = 0; ch < nch; ++ch) {
        int by = list_top + ch * R_BLOCK_H - (int)g_routing_scroll;
        if (by + R_BLOCK_H < list_top || by > list_bot) continue;
        Color cc = ui_channel_color(s->channels[ch].color_id);

        char nm[64];
        snprintf(nm, sizeof nm, "%d  %s", ch + 1, s->channels[ch].name);
        while (ui_text_w(nm, 14) > R_LABEL_W - 40 && strlen(nm) > 3) nm[strlen(nm) - 1] = 0;

        DrawRectangle(mx0, by, 4, 2 * R_ROW_H - 4, cc);
        ui_text(nm, mx0 + 12, by + 3, 14, g_theme.text);
        ui_text("L", mx0 + R_LABEL_W - 22, by + 3, 13, g_theme.text_dim);
        ui_text("R", mx0 + R_LABEL_W - 22, by + R_ROW_H + 3, 13, g_theme.text_dim);
        for (int side = 0; side < 2; ++side) {
            int ry = by + side * R_ROW_H;
            DRAW_ROW(ry, ch, side, cc);
        }
        if (ch + 1 < nch)
            DrawLine(mx0, by + R_BLOCK_H - 3, mx0 + matrix_w, by + R_BLOCK_H - 3, lighten(g_theme.panel_bg, 14));
    }
    EndScissorMode();

    /* scroll bar */
    if (max_scroll > 0) {
        int bx = dx + dlg_w - 14;
        DrawRectangle(bx, list_top, 8, (int)view_h, g_theme.scroll_bg);
        float th = view_h * view_h / total_h;
        if (th < 24) th = 24;
        float ty = list_top + (view_h - th) * (g_routing_scroll / max_scroll);
        DrawRectangle(bx, (int)ty, 8, (int)th, g_theme.scroll_fg);
    }

    /* horizontal scroll of the outputs */
    if (max_hcol > 0) {
        int hx0 = COL_X(2 + hcol), hx1 = hx0 + vis_hw * hp - (hp - hc);
        int hb_y = list_bot + 4;
        Rectangle dlg_r = { (float)dx, (float)dy, (float)dlg_w, (float)dlg_h };
        Rectangle head_r = { (float)hx0, (float)hy, (float)(hx1 - hx0), (float)(ly + 18 - hy) };
        float wh = GetMouseWheelMove();
        bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        if (wh != 0.0f && CheckCollisionPointRec(mouse, dlg_r) && (shift || CheckCollisionPointRec(mouse, head_r))) {
            g_routing_hcol -= (int)(wh > 0 ? 2 : -2);
            if (g_routing_hcol > max_hcol) g_routing_hcol = max_hcol;
            if (g_routing_hcol < 0) g_routing_hcol = 0;
        }
        int track_w = hx1 - hx0;
        float tw_ = (float)track_w * vis_hw / n_hw;
        if (tw_ < 30) tw_ = 30;
        float tx = hx0 + (track_w - tw_) * ((float)hcol / max_hcol);
        Rectangle track = { (float)hx0, (float)hb_y, (float)track_w, 10 };
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, track)) g_routing_hdrag = true;
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) g_routing_hdrag = false;
        if (g_routing_hdrag) {
            float f = (mouse.x - hx0 - tw_ / 2) / (track_w - tw_);
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            g_routing_hcol = (int)(f * max_hcol + 0.5f);
        }
        DrawRectangleRec(track, g_theme.scroll_bg);
        DrawRectangle((int)tx, hb_y, (int)tw_, 10, g_routing_hdrag ? g_theme.selection : g_theme.scroll_fg);
    }

    /* 1.4.21: the chosen output's delay, exactly */
    {
        int ey = dy + dlg_h - 96;
        int mx0 = dx + 20;                            /* (the whole width) */
        if (!dly_ok) {
            ui_text("Delays and extra sound cards need the normal mode (not the PipeWire graph mode) - see Sound Cards...",
                    mx0, ey + 8, 13, g_theme.text_dim);
        } else if (g_dsel < 0 || g_dsel >= n_hw) {
            ui_text("Delay: click a box in the Delay row, then drag it up / down (Shift = fine) or use the buttons here.",
                    mx0, ey + 8, 13, g_theme.text_dim);
        } else {
            const char *gname = "";
            int gfirst = 0, gcount = 0;
            for (int gi = 0; gi < ngrp; ++gi)
                if (g_dsel >= grp[gi].first && g_dsel < grp[gi].first + grp[gi].count) { gname = grp[gi].name; gfirst = grp[gi].first; gcount = grp[gi].count; }
            char lb[240];
            snprintf(lb, sizeof lb, "Output %d (%s): %.1f ms", g_dsel + 1, gname, (double)outx_delay_get(g_dsel));
            while (ui_text_w(lb, 14) > 330 && strlen(lb) > 10) { size_t L = strlen(lb); lb[L - 1] = 0; }
            ui_text(lb, mx0, ey + 8, 14, (Color){ 120, 200, 255, 255 });
            static const float ST[6] = { -10.0f, -1.0f, -0.1f, 0.1f, 1.0f, 10.0f };
            static const char *const SL[6] = { "-10", "-1", "-0.1", "+0.1", "+1", "+10" };
            int bx = mx0 + 340;
            for (int b = 0; b < 6; ++b, bx += 50)
                if (button(bx, ey + 2, 46, 28, SL[b], false)) delay_set_save(g_dsel, outx_delay_get(g_dsel) + ST[b], true);
            if (button(bx, ey + 2, 40, 28, "0", false)) delay_set_save(g_dsel, 0.0f, true);
            bx += 46;
            if (gcount > 1 && button(bx, ey + 2, 150, 28, "Same for the card", false)) {
                float v = outx_delay_get(g_dsel);
                for (int k = gfirst; k < gfirst + gcount; ++k) outx_delay_set(k, v);
                outx_save();
            }
        }
    }

    /* clicks / painting by dragging */
    if (!g_routing_hdrag && hover_row_id == 1 && hover_col >= 0 && (hover_ch >= 0 || hover_col >= 2)) {
        bool cur = route_bit(s, hover_ch, hover_side, hover_col);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            g_painting = true;
            g_paint_val = !cur;
            did_click = true;
        }
        if (g_painting && IsMouseButtonDown(MOUSE_BUTTON_LEFT) && cur != g_paint_val) {
            route_set_bit(s, hover_ch, hover_side, hover_col, g_paint_val);
        }
    }
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) g_painting = false;
    (void)did_click;

    /* buttons */
    int by = dy + dlg_h - 50;
    if (button(dx + 20, by, 190, 32, "Reset to defaults", false)) {
        session_routing_reset(s);
        ui_flash_message("Routing reset: channels > Master > outputs 1/2");
    }
    if (button(dx + 220, by, 160, 32, "Sound Cards...", false)) ui_open_sound_cards();   /* 1.4.21 */
    if (button(dx + dlg_w - 120, by, 100, 32, "Close", true)) g_routing_open = false;

    if (IsKeyPressed(KEY_ESCAPE) && !g_cards_open) g_routing_open = false;
    #undef COL_X
    #undef COL_VIS
    #undef HP_MIN
    #undef DRAW_ROW
}

/* ============ Shortcuts ============ */

typedef struct { const char *keys; const char *what; } Sc;
typedef struct { const char *title; const Sc *items; int n; } ScGroup;

static const Sc SC_PLAY[] = {
    { "Space", "Pause / resume everything" },
    { "Enter", "Play (or restart) the selected clip / scene" },
    { "Arrow keys", "Move the selection" },
    { "Delete / Backspace", "Remove the selected clip" },
    { "Click a clip's play button", "Play / restart (stops others in channel)" },
    { "Left click on a scene button", "Play the whole scene" },
};
static const Sc SC_FILE[] = {
    { "Ctrl+N", "New project" },
    { "Ctrl+O", "Open project..." },
    { "Ctrl+S", "Save" },
    { "Ctrl+Shift+S", "Save As..." },
    { "Ctrl+Shift+E", "Collect all files & save" },
};
static const Sc SC_EDIT[] = {
    { "Ctrl+Z", "Undo" },
    { "Ctrl+Shift+Z  /  Ctrl+Y", "Redo" },
    { "Ctrl+C", "Copy the selected clip" },
    { "Ctrl+X", "Cut the selected clip" },
    { "Ctrl+V", "Paste into the selected cell" },
    { "Ctrl+R", "Rename the selected clip" },
    { "Ctrl+E", "Add a scene" },
    { "Ctrl+I", "Add a channel" },
    { "Double-click empty cell", "Create a note" },
    { "Right click", "Context menu (clip, note, channel, scene)" },
    { "Right click channel > Rename", "Rename a channel (Enter = OK, Esc = cancel)" },
    { "Right click channel / note > Color...", "Pick one of 20 colors" },
    { "Drag right edge of channel name", "Change the channel width" },
    { "  (double-click the edge)", "Back to the default width" },
    { "Drag a clip", "Move it to another cell" },
    { "Drag a file in (library / file manager)", "Load it into a cell" },
};
static const Sc SC_VIEW[] = {
    { "L", "Show / hide the library" },
    { "Ctrl+W", "Show / hide the waveform" },
    { "Ctrl+F", "Show / hide the faders (mixer)" },
    { "Drag top edge of the faders", "Make them taller / hide them" },
    { "Double-click a clip", "Show the hidden waveform again" },
    { "Mouse wheel", "Scroll scenes" },
    { "Shift + mouse wheel", "Scroll channels sideways" },
    { "Ctrl+,", "Audio settings" },
    { "Esc", "Close a menu or dialog" },
};
static const Sc SC_MIDI[] = {
    { "Ctrl+M", "MIDI Learn on / off" },
    { "Learn: click a fader / knob", "then move the control to assign it" },
    { "Learn: click SOLO, MUTE or a clip", "then press a pad / button (Note or CC)" },
    { "Learn: right click", "Clear that assignment" },
    { "Click a channel name", "Its plugins + knobs below; an instrument channel gets the keys" },
    { "Ctrl+K", "Computer keys on / off (play the instrument)" },
    { "  A W S E D F T G Y H U J K", "Notes C .. C (with keys on)" },
    { "  Z / X,  C / V", "Octave down / up,  softer / harder" },
    { "Double click an empty cell", "Instrument channel: a new MIDI clip" },
    { "MIDI editor: double click", "Add a note / delete a note" },
    { "MIDI editor: drag / right edge", "Move / change the length (Shift = off the grid)" },
    { "MIDI editor: Del, arrows, Ctrl+A", "Delete, move (Shift+up/down = octave), select all" },
    { "MIDI editor: drag on the ruler", "Up/down = zoom, left/right = move" },
    { "MIDI editor: double click the ruler", "Play the clip from there" },
    { "MIDI editor: drag the triangle", "The length of the clip" },
    { "MIDI editor: Ctrl + drag a note", "Copy it (the original stays)" },
    { "MIDI editor: Ctrl+C / Ctrl+X, Ctrl+V", "Copy / cut notes, paste at the last click" },
    { "MIDI editor: Ctrl+U", "Quantize (the selected notes, or all)" },
    { "MIDI editor: B  (Esc = the arrow)", "Pencil: click = a note as long as the last one" },
    { "MIDI editor: Height - / + / Fit", "Note height (or Ctrl+Shift+wheel)" },
    { "Ctrl+Shift+M", "Hide / show the master (or drag its edge right)" },
    { "Ctrl+Shift+U", "Hide / show the level meters" },
    { "Ctrl+wheel, Ctrl+= / Ctrl+-", "Bigger / smaller interface (Ctrl+0 = 100 %)" },
    { "● on a channel header", "Arm for recording (right click: input, monitoring)" },
    { "Armed channel: red circle in a cell", "Record; click the cell again to stop (on the bar)" },
};
static const Sc SC_WAVE[] = {
    { "Drag on the time ruler", "Up/down = zoom, drag left = view moves right" },
    { "  (diagonal)", "Up-left = zoom in and move right" },
    { "Middle button drag", "Same, anywhere on the waveform" },
    { "Mouse wheel over waveform", "Zoom around the mouse" },
    { "Shift + mouse wheel", "Move the view sideways" },
    { "Drag the yellow LOOP bar", "Move the whole loop (ends: resize)" },
    { "Click on the waveform", "Move the playhead there" },
    { "Drag a marker", "Set start / end / loop points" },
    { "Right click a marker", "Reset the marker" },
    { "Double click the clip's tempo", "Type a tempo" },
    { "Ctrl + double click the tempo", "Back to the clip's own tempo" },
    { "Gain / Pan / Pitch: drag, wheel", "Change it (double click = 0)" },
    { "Surround (above the waveform)", "The Surround window: the clip's path" },
};

#define G(t, a) { t, a, (int)(sizeof(a) / sizeof((a)[0])) }
static const ScGroup SC_GROUPS_L[] = { G("Playback", SC_PLAY), G("File", SC_FILE), G("Edit", SC_EDIT) };
static const ScGroup SC_GROUPS_R[] = { G("View", SC_VIEW), G("MIDI", SC_MIDI), G("Waveform", SC_WAVE) };

static bool   g_sc_open = false;
static double g_sc_t = 0.0;
static float  g_sc_scroll = 0.0f;

void ui_open_shortcuts(void) { g_sc_open = true; g_sc_t = GetTime(); g_sc_scroll = 0.0f; }

static int draw_sc_column(const ScGroup *groups, int ng, int x, int y, int w)
{
    int cy = y;
    for (int g = 0; g < ng; ++g) {
        ui_text(groups[g].title, x, cy, 16, g_theme.selection);
        cy += 22;
        DrawLine(x, cy - 2, x + w, cy - 2, g_theme.text_dim);
        for (int i = 0; i < groups[g].n; ++i) {
            const Sc *it = &groups[g].items[i];
            int kw = 235;
            char k[64], d[96];
            snprintf(k, sizeof k, "%s", it->keys);
            while (ui_text_w(k, 13) > kw - 8 && strlen(k) > 3) k[strlen(k) - 1] = 0;
            ui_text(k, x + 4, cy, 13, g_theme.text);
            snprintf(d, sizeof d, "%s", it->what);
            while (ui_text_w(d, 13) > w - kw - 8 && strlen(d) > 3) d[strlen(d) - 1] = 0;
            ui_text(d, x + kw, cy, 13, g_theme.text_dim);
            cy += 20;
        }
        cy += 12;
    }
    return cy - y;
}

static void draw_shortcuts(int sw, int sh)
{
    GfxDialogBackdrop(70);
    int dlg_w = sw - 40 > 980 ? 980 : sw - 40;
    int dlg_h = sh - 40 > 700 ? 700 : sh - 40;
    int dx = (sw - dlg_w) / 2, dy = (sh - dlg_h) / 2;
    draw_frame(dx, dy, dlg_w, dlg_h, "Shortcuts", g_sc_t);

    int top = dy + 52, bot = dy + dlg_h - 58;
    int col_w = (dlg_w - 60) / 2;
    int vh = bot - top;

    BeginScissorMode(dx, top, dlg_w, vh);
    int h1 = draw_sc_column(SC_GROUPS_L, 3, dx + 20, top - (int)g_sc_scroll, col_w);
    int h2 = draw_sc_column(SC_GROUPS_R, 3, dx + 40 + col_w, top - (int)g_sc_scroll, col_w);
    EndScissorMode();

    int content = h1 > h2 ? h1 : h2;
    float max_scroll = content > vh ? (float)(content - vh) : 0.0f;
    Vector2 m = GetMousePosition();
    if (CheckCollisionPointRec(m, (Rectangle){ (float)dx, (float)top, (float)dlg_w, (float)vh })) {
        float wh = GetMouseWheelMove();
        if (wh != 0.0f) g_sc_scroll -= wh * 50.0f;
    }
    if (g_sc_scroll > max_scroll) g_sc_scroll = max_scroll;
    if (g_sc_scroll < 0) g_sc_scroll = 0;

    if (button(dx + dlg_w - 120, dy + dlg_h - 48, 100, 32, "Close", true)) g_sc_open = false;
    if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_ENTER)) g_sc_open = false;
}

/* ============ Save on close ============ */

static bool       g_q_open = false;
static double     g_q_t = 0.0;
static char       g_q_name[256];
static QuitChoice g_q_choice = QUIT_CHOICE_NONE;

void ui_open_quit_prompt(const char *name)
{
    g_q_open = true;
    g_q_t = GetTime();
    snprintf(g_q_name, sizeof g_q_name, "%s", name ? name : "");
    g_q_choice = QUIT_CHOICE_NONE;
}
bool ui_quit_prompt_is_open(void) { return g_q_open; }
bool ui_take_quit_choice(QuitChoice *out)
{
    if (g_q_choice == QUIT_CHOICE_NONE) return false;
    if (out) *out = g_q_choice;
    g_q_choice = QUIT_CHOICE_NONE;
    return true;
}

static void draw_quit(int sw, int sh)
{
    GfxDialogBackdrop(70);
    int w = 560, h = 210;
    int x = (sw - w) / 2, y = (sh - h) / 2;
    draw_frame(x, y, w, h, "Q-LiM", g_q_t);

    char line[400];
    if (g_q_name[0]) snprintf(line, sizeof line, "Do you want to save the changes made to \"%s\"?", g_q_name);
    else snprintf(line, sizeof line, "Do you want to save the changes made to this project?");
    while (ui_text_w(line, 16) > w - 40 && strlen(line) > 10) { line[strlen(line) - 3] = 0; }
    ui_text(line, x + 20, y + 62, 16, g_theme.text);
    ui_text("Your changes will be lost if you don't save them.", x + 20, y + 90, 14, g_theme.text_dim);

    int by = y + h - 52;
    QuitChoice c = QUIT_CHOICE_NONE;
    if (button(x + 20, by, 140, 34, "Don't Save", false)) c = QUIT_CHOICE_DONT_SAVE;
    if (button(x + w - 20 - 100 - 12 - 100, by, 100, 34, "Cancel", false)) c = QUIT_CHOICE_CANCEL;
    if (button(x + w - 20 - 100, by, 100, 34, "Save", true)) c = QUIT_CHOICE_SAVE;

    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) c = QUIT_CHOICE_SAVE;
    if (IsKeyPressed(KEY_ESCAPE)) c = QUIT_CHOICE_CANCEL;
    if (IsKeyPressed(KEY_D)) c = QUIT_CHOICE_DONT_SAVE;

    if (c != QUIT_CHOICE_NONE) { g_q_choice = c; g_q_open = false; }
}

/* ============ Colour picker (20 colours) ============ */

static bool   g_cp_open = false;
static double g_cp_t = 0.0;
static int    g_cp_ch = -1, g_cp_scene = -1;

void ui_open_color_picker(int ch, int scene)
{
    g_cp_open = true; g_cp_t = GetTime(); g_cp_ch = ch; g_cp_scene = scene;
}

static void draw_color_picker(Session *s, int sw, int sh)
{
    GfxDialogBackdrop(70);
    const int cols = 5, sz_w = 64, sz_h = 44, gap = 8;
    int n = ui_palette_count();
    int rows = (n + cols - 1) / cols;
    bool is_note = g_cp_scene >= 0;
    int w = cols * sz_w + (cols - 1) * gap + 40;
    int h = 40 + 20 + rows * sz_h + (rows - 1) * gap + (is_note ? 48 : 0) + 70;
    int x = (sw - w) / 2, y = (sh - h) / 2;
    draw_frame(x, y, w, h, is_note ? "Note color" : "Channel color", g_cp_t);

    int cur = -1;
    if (is_note) cur = session_get_note_color(s, g_cp_ch, g_cp_scene) - 1;     /* -1 = default */
    else if (g_cp_ch >= 0 && g_cp_ch < s->channel_count) cur = s->channels[g_cp_ch].color_id;

    Vector2 m = GetMousePosition();
    int gx = x + 20, gy = y + 40 + 20;
    int picked = -2;
    for (int i = 0; i < n; ++i) {
        int cx = gx + (i % cols) * (sz_w + gap);
        int cy = gy + (i / cols) * (sz_h + gap);
        Rectangle r = { (float)cx, (float)cy, (float)sz_w, (float)sz_h };
        Color c = ui_palette_color(i);
        bool hov = CheckCollisionPointRec(m, r);
        DrawRectangleRec(r, c);
        DrawRectangleLinesEx(r, (i == cur || hov) ? 3 : 1, (i == cur) ? g_theme.selection : (hov ? g_theme.text : g_theme.text_dim));
        if (i == cur) {
            const char *ok = "OK";
            ui_text(ok, cx + (sz_w - ui_text_w(ok, 15)) / 2, cy + (sz_h - 15) / 2 - 1, 15, ui_text_color_on(c));
        }
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) picked = i;
    }

    int by = gy + rows * sz_h + (rows - 1) * gap + 14;
    if (is_note) {
        if (button(x + 20, by, w - 40, 32, "Default note color", cur < 0)) picked = -1;
        by += 44;
    }
    if (button(x + w - 20 - 100, y + h - 50, 100, 32, "Cancel", false)) g_cp_open = false;

    if (picked != -2) {
        if (is_note) session_set_note_color(s, g_cp_ch, g_cp_scene, picked + 1);
        else session_set_channel_color(s, g_cp_ch, picked);
        g_cp_open = false;
    }
    if (IsKeyPressed(KEY_ESCAPE)) g_cp_open = false;
}

/* ============ 1.4.14: Export Audio (a clip, start -> stop marker, as it sounds) ============ */

static bool       g_ex_open = false;
static double     g_ex_t = 0.0;
static int        g_ex_ch = -1, g_ex_scene = -1;
static ExportOpts g_ex_o = { EXP_WAV, 24, 0, EXP_Q_HIGH };   /* remembered while the program runs */
static ExportJob *g_ex_job = NULL;
static int        g_ex_rc = -1;                    /* -1 = not exported yet, 0 = ok, else failed */
static char       g_ex_msg[400];
static char       g_ex_dir[1024];                  /* the folder of the last export */
static char       g_ex_name[NAME_LEN], g_ex_what[300];
static double     g_ex_secs = 0.0;
static int        g_ex_ssr = 48000;
static bool       g_ex_stems = false;              /* the clip is separated: what to export */
static int        g_ex_stem_n = 0;
static char       g_ex_stem_name[STEM_MAX][STEM_NAME_LEN];
static int        g_ex_what_sel = 0;               /* 0 = as heard, k + 1 = part k, n + 1 = every part (n files) */
static int        g_ex_q_n = 0, g_ex_q_i = 0;      /* "every part": the files still to export after this one */
static char       g_ex_q_path[STEM_MAX][1100];
static int        g_ex_q_stem[STEM_MAX];
static bool       g_ex_scn = false;                /* 1.4.17: Export Scene (g_ex_scene = the scene) */
static int        g_ex_scn_clips = 0;
static bool       g_ex_scn_loops = false;
static char       g_ex_scn_longest[NAME_LEN];
static int        g_ex_sur_n = 0;                  /* 1.4.23: > 0 = surround export possible (speakers / outputs) */
static bool       g_ex_mc = false;                 /* 1.4.24: the clip plays a multichannel file */
static int        g_ex_sur_sel = EXP_SUR_OFF;      /* remembered while the program runs */

void ui_open_export_scene(Session *s, int scene)
{
    if (!s || scene < 0 || scene >= s->scene_count) return;
    double secs = session_scene_length(s, scene, &g_ex_scn_clips, g_ex_scn_longest, sizeof g_ex_scn_longest, &g_ex_scn_loops);
    if (g_ex_scn_clips == 0 || !(secs > 0.0)) { ui_flash_message("This scene has no clips to export"); return; }
    g_ex_open = true; g_ex_t = GetTime(); g_ex_ch = -1; g_ex_scene = scene;
    g_ex_scn = true;
    g_ex_rc = -1; g_ex_msg[0] = 0;
    g_ex_stems = false; g_ex_stem_n = 0; g_ex_what_sel = 0;
    g_ex_q_n = g_ex_q_i = 0;
    snprintf(g_ex_name, sizeof g_ex_name, "Scene %d", scene + 1);
    g_ex_ssr = s->sample_rate > 0 ? s->sample_rate : 48000;
    g_ex_secs = secs;
    g_ex_o.stem = 0;
    g_ex_mc = false;
    {
        int no = export_scene_outputs(s);
        g_ex_sur_n = no > 2 ? no : 0;
    }
    if (!export_format_available(g_ex_o.format)) g_ex_o.format = EXP_WAV;
}

void ui_open_export(Session *s, int ch, int scene)
{
    ClipSnapshot c;
    if (!s || session_clip_snapshot(s, ch, scene, &c) != 0) { ui_flash_message("Only audio clips can be exported"); return; }
    g_ex_scn = false;
    g_ex_open = true; g_ex_t = GetTime(); g_ex_ch = ch; g_ex_scene = scene;
    g_ex_rc = -1; g_ex_msg[0] = 0;
    g_ex_stems = c.stems;
    g_ex_stem_n = c.stems ? c.stem_n : 0;
    for (int k = 0; k < g_ex_stem_n; ++k) snprintf(g_ex_stem_name[k], sizeof g_ex_stem_name[k], "%s", c.stem_name[k]);
    if (!g_ex_stems || g_ex_what_sel > g_ex_stem_n + 1) g_ex_what_sel = 0;
    g_ex_q_n = g_ex_q_i = 0;
    snprintf(g_ex_name, sizeof g_ex_name, "%s", c.name);
    g_ex_ssr = c.sample_rate;
    double sp = c.stretch && c.speed > 0.0 ? c.speed : 1.0;
    g_ex_secs = (double)(c.end - c.start) / sp / (double)c.sample_rate;
    /* what is "baked in" */
    char w[300] = "";
    size_t n = 0;
    #define ADD(...) do { if (n < sizeof w) n += (size_t)snprintf(w + n, sizeof w - n, __VA_ARGS__); } while (0)
    if (c.stretch && fabs(sp - 1.0) > 1e-6) ADD("tempo x%.3f, ", sp);
    if (c.pitch_st != 0.0f) ADD("pitch %+.2f st%s, ", c.pitch_st, c.formants ? " (formants)" : "");
    if (c.gain_db <= CLIP_GAIN_MIN_DB) ADD("gain -inf, ");
    else if (fabsf(c.gain_db) > 0.005f) ADD("gain %+.1f dB, ", c.gain_db);
    if (c.pan != 0.0f) ADD("pan %s%d, ", c.pan < 0 ? "L" : "R", (int)lrintf(fabsf(c.pan) * 100.0f));
    if (c.edits && !edit_is_identity(c.edits)) ADD("cuts/fades, ");
    #undef ADD
    if (n >= 2 && n < sizeof w) w[n - 2] = 0;
    snprintf(g_ex_what, sizeof g_ex_what, "%s", w[0] ? w : "nothing changed - the original sound");
    edit_free(c.edits);
    {
        SurSetup su;
        g_ex_sur_n = session_get_surround(s, ch, &su) && su.n > 0 ? su.n : 0;
        g_ex_mc = c.mc_nch > 2;                               /* 1.4.24: a multichannel clip: its own channels */
        if (g_ex_mc) g_ex_sur_n = c.mc_nch;
    }
    if (!export_format_available(g_ex_o.format)) g_ex_o.format = EXP_WAV;
}

static bool seg_button(int x, int y, int w, const char *label, bool on, bool enabled)
{
    if (!enabled) {
        Rectangle r = { (float)x, (float)y, (float)w, 28.0f };
        DrawRectangleLinesEx(r, 1, g_theme.text_dim);
        ui_text(label, x + (w - ui_text_w(label, 14)) / 2, y + 6, 14, g_theme.text_dim);
        return false;
    }
    Vector2 m = GetMousePosition();
    Rectangle r = { (float)x, (float)y, (float)w, 28.0f };
    bool hov = CheckCollisionPointRec(m, r);
    Color bg = on ? g_theme.selection : lighten(g_theme.panel_bg2, hov ? 22 : 8);
    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, 1, on ? g_theme.selection : g_theme.text_dim);
    ui_text(label, x + (w - ui_text_w(label, 14)) / 2, y + 6, 14, on ? g_theme.bg : g_theme.text);
    return hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_ex_job;
}

static void export_begin(Session *s)
{
    char base[NAME_LEN];
    snprintf(base, sizeof base, "%s", g_ex_name);
    char *dot = strrchr(base, '.');
    if (dot && dot != base) *dot = 0;
    if (g_ex_stems && g_ex_what_sel >= 1 && g_ex_what_sel <= g_ex_stem_n) {
        size_t bl = strlen(base);
        snprintf(base + bl, sizeof base - bl, " (%s)", g_ex_stem_name[g_ex_what_sel - 1]);
    }
    char path[1000];
    GfxExternalDialogBegin();
    int r = project_ask_export_path(g_ex_dir[0] ? g_ex_dir : NULL, base, export_format_ext(g_ex_o.format),
                                    export_format_name(g_ex_o.format), path, sizeof path);
    GfxExternalDialogEnd();
    if (r != 0) return;
    bool all = g_ex_stems && g_ex_what_sel == g_ex_stem_n + 1;
    g_ex_o.stem = g_ex_stems ? (all ? 1 : g_ex_what_sel) : 0;
    g_ex_q_n = g_ex_q_i = 0;
    if (all) {                                       /* every part: "<name> (<part>).ext", one after the other */
        char stem_[1000];
        snprintf(stem_, sizeof stem_, "%s", path);
        char *d = strrchr(stem_, '.'), *sl2 = strrchr(stem_, '/');
        const char *ext = d && (!sl2 || d > sl2) ? d : "";
        char e2[32];
        snprintf(e2, sizeof e2, "%s", ext);
        if (d && (!sl2 || d > sl2)) *d = 0;
        snprintf(path, sizeof path, "%s (%s)%s", stem_, g_ex_stem_name[0], e2);
        for (int k = 1; k < g_ex_stem_n; ++k) {
            snprintf(g_ex_q_path[g_ex_q_n], sizeof g_ex_q_path[0], "%s (%s)%s", stem_, g_ex_stem_name[k], e2);
            g_ex_q_stem[g_ex_q_n++] = k + 1;
        }
    }
    snprintf(g_ex_dir, sizeof g_ex_dir, "%s", path);
    char *sl = strrchr(g_ex_dir, '/');
    if (sl) *sl = 0; else g_ex_dir[0] = 0;
    char err[300] = "";
    if (g_ex_scn) {                                  /* 1.4.17: the scene plays and the master is recorded */
        double secs = session_scene_length(s, g_ex_scene, NULL, NULL, 0, NULL);
        if (secs > 0.0) g_ex_secs = secs;
        g_ex_job = export_scene_start(s, g_ex_scene, g_ex_secs, &g_ex_o, path, err, sizeof err);
    } else g_ex_job = export_clip_start(s, g_ex_ch, g_ex_scene, &g_ex_o, path, err, sizeof err);
    if (!g_ex_job) { g_ex_rc = 1; snprintf(g_ex_msg, sizeof g_ex_msg, "%s", err); }
}

static void draw_export(Session *s, int sw, int sh)
{
    GfxDialogBackdrop(70);
    int w = 760, h = g_ex_stems ? 462 : 420;
    if (g_ex_scn) h = 440;
    bool graph = g_ex_scn && audio_graph_active();
    if (g_ex_sur_n > 0) h += 62;
    int x = (sw - w) / 2, y = (sh - h) / 2;
    g_ex_o.sur = g_ex_sur_n > 0 && !graph ? g_ex_sur_sel : EXP_SUR_OFF;
    draw_frame(x, y, w, h, g_ex_scn ? "Export Scene" : "Export Audio", g_ex_t);
    int lx = x + 20, vx = x + 140, cy = y + 56;

    char line[400];
    snprintf(line, sizeof line, "%s", g_ex_name);
    while (ui_text_w(line, 16) > w - 40 && strlen(line) > 10) line[strlen(line) - 4] = 0;
    ui_text(line, lx, cy, 16, g_theme.text);
    cy += 24;
    int mm = (int)(g_ex_secs / 60.0);
    if (g_ex_scn) {                                  /* 1.4.17 */
        snprintf(line, sizeof line, "%d clip%s, %d:%04.1f - %s \"%s\"", g_ex_scn_clips, g_ex_scn_clips == 1 ? "" : "s", mm, g_ex_secs - mm * 60.0,
                 g_ex_scn_loops ? "all loop: one pass of the longest," : "until the longest clip without loop ends:", g_ex_scn_longest);
        while (ui_text_w(line, 13) > w - 40 && strlen(line) > 10) line[strlen(line) - 4] = 0;
        ui_text(line, lx, cy, 13, g_theme.text_dim);
        ui_text("Exactly as heard: faders, pan, mute / solo, plugins and the master (no metronome).", lx, cy + 18, 13, g_theme.text_dim);
        ui_text("In real time: the scene plays (you hear it) while it is recorded.", lx, cy + 36, 13, g_theme.text_dim);
        cy += 34 + 36;
    } else {
        snprintf(line, sizeof line, "From the start to the stop marker: %d:%04.1f   Includes: %s", mm, g_ex_secs - mm * 60.0, g_ex_what);
        while (ui_text_w(line, 13) > w - 40 && strlen(line) > 10) line[strlen(line) - 4] = 0;
        ui_text(line, lx, cy, 13, g_theme.text_dim);
        cy += 34;
    }

    if (g_ex_stems) {                                /* a separated clip: what to export */
        ui_text("Export", lx, cy + 6, 15, g_theme.text);
        int bx = vx;
        for (int i = 0; i <= g_ex_stem_n + 1; ++i) {
            char lb[48];
            if (i == 0) snprintf(lb, sizeof lb, "As heard");
            else if (i <= g_ex_stem_n) snprintf(lb, sizeof lb, "%s", g_ex_stem_name[i - 1]);
            else snprintf(lb, sizeof lb, "All (%d files)", g_ex_stem_n);
            int bw = ui_text_w(lb, 14) + 18;
            if (bx + bw > x + w - 16) { bx = vx; cy += 32; }
            if (seg_button(bx, cy, bw, lb, g_ex_what_sel == i, true)) g_ex_what_sel = i;
            bx += bw + 5;
        }
        cy += 42;
    }
    /* 1.4.23: surround - the clip through its path / the scene on every output */
    if (g_ex_sur_n > 0) {
        ui_text("Surround", lx, cy + 6, 15, g_theme.text);
        char l1[48], l2[48];
        snprintf(l1, sizeof l1, "One file (%d ch)", g_ex_sur_n);
        snprintf(l2, sizeof l2, g_ex_scn ? "A file per output" : g_ex_mc ? "A file per channel" : "A file per speaker");
        const char *lb[3] = { g_ex_scn ? "Stereo (master)" : g_ex_mc ? "Stereo (first 2)" : "Stereo", l1, l2 };
        int bx = vx;
        for (int i = 0; i < 3; ++i) {
            int bw = ui_text_w(lb[i], 14) + 22;
            if (seg_button(bx, cy, bw, lb[i], g_ex_o.sur == i, i == 0 || !graph)) { g_ex_sur_sel = i; g_ex_o.sur = i; }
            bx += bw + 6;
        }
        const char *hint = graph ? "Not in graph mode (Audio Settings): the outputs are links there"
                         : g_ex_o.sur == EXP_SUR_OFF ? (g_ex_scn ? "Only the master, as before" : "The clip in stereo, without its path")
                         : g_ex_scn ? "Every output as it plays: the surround channels, the routes and the master"
                         : g_ex_mc ? "Every channel of the file, with the clip's cuts, markers and gain (session rate)"
                                    : "The clip moves through its path, one channel per speaker (session rate)";
        ui_text(hint, vx, cy + 34, 13, g_theme.text_dim);
        cy += 62;
    }
    int fch_ = g_ex_o.sur == EXP_SUR_ONE ? g_ex_sur_n : g_ex_o.sur == EXP_SUR_SPLIT ? 1 : 2;
    if (g_ex_o.sur != EXP_SUR_OFF) {
        g_ex_o.sample_rate = 0;                      /* surround: at the session rate */
        if (!export_format_available_ch(g_ex_o.format, fch_)) g_ex_o.format = EXP_WAV;
    }
    /* format */
    ui_text("Format", lx, cy + 6, 15, g_theme.text);
    {
        int bx = vx, bw = 78;
        for (int f = 0; f < EXP_NFMT; ++f) {
            bool av = export_format_available(f) && (g_ex_o.sur == EXP_SUR_OFF || export_format_available_ch(f, fch_));
            const char *nm = f == EXP_OGG ? "Ogg" : export_format_name(f);
            if (seg_button(bx, cy, bw, nm, g_ex_o.format == f, av)) g_ex_o.format = f;
            bx += bw + 6;
        }
    }
    cy += 42;
    int fmt = g_ex_o.format;
    /* bits or quality */
    if (export_format_has_bits(fmt)) {
        ui_text("Bits", lx, cy + 6, 15, g_theme.text);
        const int bits[3] = { 16, 24, 32 };
        const char *bl[3] = { "16 bit", "24 bit", "32 bit float" };
        int cur = export_fix_bits(fmt, g_ex_o.bits);
        int bx = vx;
        for (int i = 0; i < 3; ++i) {
            int bw = i == 2 ? 130 : 90;
            if (seg_button(bx, cy, bw, bl[i], cur == bits[i], !(fmt == EXP_FLAC && i == 2))) g_ex_o.bits = bits[i];
            bx += bw + 6;
        }
        if (cur == 16) ui_text("with dither", bx + 6, cy + 7, 13, g_theme.text_dim);
    } else {
        ui_text("Quality", lx, cy + 6, 15, g_theme.text);
        const char *ql[3] = { "High", "Medium", "Small" };
        int bx = vx;
        for (int i = 0; i < 3; ++i) {
            if (seg_button(bx, cy, 90, ql[i], g_ex_o.quality == i, true)) g_ex_o.quality = i;
            bx += 96;
        }
        const char *hint = "";
        if (fmt == EXP_MP3) hint = g_ex_o.quality == 0 ? "320 kbit/s" : g_ex_o.quality == 1 ? "VBR ~175 kbit/s" : "VBR ~130 kbit/s";
        else if (fmt == EXP_OPUS) hint = g_ex_o.quality == 0 ? "~290 kbit/s" : g_ex_o.quality == 1 ? "~160 kbit/s" : "~96 kbit/s";
        else hint = g_ex_o.quality == 0 ? "q9 ~320 kbit/s" : g_ex_o.quality == 1 ? "q6 ~190 kbit/s" : "q4 ~130 kbit/s";
        ui_text(hint, bx + 6, cy + 7, 13, g_theme.text_dim);
    }
    cy += 42;
    /* sample rate */
    ui_text("Sample rate", lx, cy + 6, 15, g_theme.text);
    {
        const int rates[6] = { 0, 44100, 48000, 88200, 96000, 192000 };
        int bx = vx;
        int eff = export_fix_rate(fmt, g_ex_o.sample_rate > 0 ? g_ex_o.sample_rate : g_ex_ssr);
        for (int i = 0; i < 6; ++i) {
            char lb[40];
            if (i == 0) snprintf(lb, sizeof lb, "Session %.1fk", g_ex_ssr / 1000.0);
            else snprintf(lb, sizeof lb, "%gk", rates[i] / 1000.0);
            int r = rates[i] ? rates[i] : g_ex_ssr;
            bool ok = export_fix_rate(fmt, r) == r && (g_ex_o.sur == EXP_SUR_OFF || i == 0);
            bool on = ok && g_ex_o.sample_rate == rates[i];
            int bw = i == 0 ? 116 : 64;
            if (seg_button(bx, cy, bw, lb, on, ok)) g_ex_o.sample_rate = rates[i];
            bx += bw + 6;
        }
        if (export_fix_rate(fmt, g_ex_o.sample_rate > 0 ? g_ex_o.sample_rate : g_ex_ssr) != (g_ex_o.sample_rate > 0 ? g_ex_o.sample_rate : g_ex_ssr)) {
            snprintf(line, sizeof line, "%s is written at %.1f kHz", export_format_name(fmt), eff / 1000.0);
            ui_text(line, vx, cy + 34, 13, g_theme.text_dim);
        }
    }
    cy += 70;

    /* progress / result */
    if (g_ex_job) {
        float p = export_progress(g_ex_job);
        Rectangle bar = { (float)lx, (float)cy, (float)(w - 40), 22.0f };
        DrawRectangleRec(bar, lighten(g_theme.panel_bg2, 6));
        DrawRectangle(lx, cy, (int)((w - 40) * p), 22, g_theme.selection);
        DrawRectangleLinesEx(bar, 1, g_theme.text_dim);
        if (g_ex_q_n > 0) snprintf(line, sizeof line, "Exporting part %d of %d... %d%%", g_ex_q_i + 1, g_ex_q_n + 1, (int)(p * 100.0f));
        else if (export_phase(g_ex_job) == 1) {
            double t = p * g_ex_secs;
            snprintf(line, sizeof line, "Recording the scene... %d:%02d of %d:%02d", (int)t / 60, (int)t % 60, (int)g_ex_secs / 60, (int)g_ex_secs % 60);
        } else if (export_phase(g_ex_job) == 2) snprintf(line, sizeof line, "Saving... %d%%", (int)(p * 100.0f));
        else snprintf(line, sizeof line, "Exporting... %d%%", (int)(p * 100.0f));
        ui_text(line, lx + 8, cy + 3, 14, g_theme.text);
        if (export_finished(g_ex_job)) {
            g_ex_rc = export_result(g_ex_job, g_ex_msg, sizeof g_ex_msg);
            export_free(g_ex_job);
            g_ex_job = NULL;
            if (g_ex_scn) session_stop_all(s);          /* the recorded scene stops */
            if (g_ex_rc == 0 && g_ex_q_i < g_ex_q_n) {   /* "all": the next part */
                char err[300] = "";
                g_ex_o.stem = g_ex_q_stem[g_ex_q_i];
                g_ex_job = export_clip_start(s, g_ex_ch, g_ex_scene, &g_ex_o, g_ex_q_path[g_ex_q_i], err, sizeof err);
                g_ex_q_i++;
                if (!g_ex_job) { g_ex_rc = 1; snprintf(g_ex_msg, sizeof g_ex_msg, "%s", err); g_ex_q_n = 0; }
                else g_ex_rc = -1;
            } else if (g_ex_rc == 0 && g_ex_q_n > 0) {
                snprintf(g_ex_msg, sizeof g_ex_msg, "Exported all %d parts", g_ex_q_n + 1);
                g_ex_q_n = g_ex_q_i = 0;
            }
        }
    } else if (g_ex_rc >= 0) {
        snprintf(line, sizeof line, "%s", g_ex_msg);
        while (ui_text_w(line, 14) > w - 40 && strlen(line) > 10) line[strlen(line) - 4] = 0;
        ui_text(line, lx, cy + 3, 14, g_ex_rc == 0 && !strstr(g_ex_msg, "CLIPS") ? g_theme.text : (Color){ 230, 90, 80, 255 });
    }

    int by = y + h - 52;
    if (g_ex_job) {
        if (button(x + w - 20 - 110, by, 110, 34, "Cancel", false) || IsKeyPressed(KEY_ESCAPE)) export_cancel(g_ex_job);
        return;
    }
    if (button(x + w - 20 - 110 - 12 - 110, by, 110, 34, g_ex_rc == 0 ? "Close" : "Cancel", false) || IsKeyPressed(KEY_ESCAPE)) {
        g_ex_open = false;
        return;
    }
    if (button(x + w - 20 - 110, by, 110, 34, "Export...", true) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        g_ex_rc = -1;
        export_begin(s);
    }
}

/* ============ 1.4.14 / 1.4.17: Separate (choose the parts, download on first use, progress) ============ */

static bool    g_sp_open = false;            /* the window is shown (the work may go on while it is hidden) */
static double  g_sp_t = 0.0;
static int     g_sp_ch = -1, g_sp_scene = -1;
static bool    g_sp_choose = false;          /* 1.4.17: choosing what to separate */
static bool    g_sp_ask = false;             /* asking whether to download */
static bool    g_sp_ask_dir = false;         /* 1.4.15: the first time: where to keep the separated tracks */
static SepRequest g_sp_req;
static Session *g_sp_s = NULL;
static SepJob *g_sp_job = NULL;
static char    g_sp_msg[400];                /* the result (shown until closed) */
static int     g_sp_rc = -1;

bool ui_separation_busy(void) { return g_sp_job != NULL; }

static void sep_begin(Session *s)
{
    char err[300] = "";
    g_sp_job = sep_start(s, g_sp_ch, g_sp_scene, &g_sp_req, err, sizeof err);
    if (!g_sp_job) { g_sp_rc = 1; snprintf(g_sp_msg, sizeof g_sp_msg, "%s", err); }
}

static void sep_done_line(const StemSet *set, char *out, size_t n)
{
    char l[300] = "";
    for (int k = 0; k < set->n; ++k) {
        size_t ln = strlen(l);
        snprintf(l + ln, sizeof l - ln, "%s%s", k ? (k == set->n - 1 ? " and " : ", ") : "", set->name[k]);
    }
    snprintf(out, n, "%s", l);
}

/* after the choice: at once if it was separated so before, else (the folder, the download) and start */
static void sep_go(Session *s)
{
    SepPrefs pr;
    sep_prefs_get(&pr);
    pr.last = g_sp_req;
    sep_prefs_set(&pr);
    g_sp_choose = false;
    ClipSnapshot c;
    if (session_clip_snapshot(s, g_sp_ch, g_sp_scene, &c) == 0) {
        static StemSet set;
        int hit = sep_cached(c.path, &g_sp_req, &set);
        edit_free(c.edits);
        if (hit == 0 && session_clip_set_stem_set(s, g_sp_ch, g_sp_scene, &set) == 0) {
            char l[300], m[400];
            sep_done_line(&set, l, sizeof l);
            snprintf(m, sizeof m, "Separated before - at once: %s", l);
            ui_flash_message(m);
            g_sp_open = false;
            return;
        }
    }
    g_sp_ask_dir = !pr.asked;
    g_sp_ask = sep_need_mb(&g_sp_req) > 0.0;
    if (!g_sp_ask && !g_sp_ask_dir) sep_begin(s);
}

void ui_open_separate(Session *s, int ch, int scene)
{
    if (g_sp_job) { g_sp_open = true; return; }      /* one at a time: show the running one */
    ClipSnapshot c;
    if (!s || session_clip_snapshot(s, ch, scene, &c) != 0) { ui_flash_message("Only audio clips can be separated"); return; }
    edit_free(c.edits);
    g_sp_open = true; g_sp_t = GetTime(); g_sp_ch = ch; g_sp_scene = scene;
    g_sp_rc = -1; g_sp_msg[0] = 0;
    g_sp_s = s;
    SepPrefs pr;
    sep_prefs_get(&pr);
    g_sp_req = pr.last;                              /* the last choice */
    if (!g_sp_req.parts) g_sp_req.parts = SEP_P_VOCALS;
    g_sp_choose = true;
    g_sp_ask = g_sp_ask_dir = false;
}

void ui_separation_tick(Session *s)
{
    if (!g_sp_job) return;
    int st = sep_state(g_sp_job, NULL, NULL, 0);
    if (st != SEP_DONE && st != SEP_FAILED && st != SEP_CANCELLED) return;
    char text[400] = "";
    sep_state(g_sp_job, NULL, text, sizeof text);
    if (st == SEP_DONE) {
        static StemSet set;
        char path[1024];
        int ch, sc;
        sep_result(g_sp_job, &set);
        sep_target(g_sp_job, &ch, &sc, path, sizeof path);
        /* the clip may have been moved / replaced meanwhile: only if it is still the same file */
        ClipSnapshot c;
        bool same = session_clip_snapshot(s, ch, sc, &c) == 0;
        if (same) { same = strcmp(c.path, path) == 0; edit_free(c.edits); }
        char l[300];
        sep_done_line(&set, l, sizeof l);
        g_sp_rc = 0;
        if (same && session_clip_set_stem_set(s, ch, sc, &set) == 0) {
            snprintf(g_sp_msg, sizeof g_sp_msg, "Done: %s. Each part has its colour, level, M and S under the waveform.", l);
            ui_flash_message("Separated");
        } else
            snprintf(g_sp_msg, sizeof g_sp_msg, "Done (the clip has moved - separate it again with the same choice: it is instant now)");
    } else if (st == SEP_FAILED) {
        g_sp_rc = 1;
        snprintf(g_sp_msg, sizeof g_sp_msg, "%s", text);
        if (!g_sp_open) ui_flash_message(text);
    } else {
        g_sp_rc = 2;
        snprintf(g_sp_msg, sizeof g_sp_msg, "Cancelled");
    }
    sep_free(g_sp_job);
    g_sp_job = NULL;
}

/* the small progress line under the waveform of the clip being separated (when the window is hidden) */
bool ui_separation_progress(int ch, int scene, float *p, char *text, size_t n)
{
    if (!g_sp_job || ch != g_sp_ch || scene != g_sp_scene) return false;
    sep_state(g_sp_job, p, text, n);
    return true;
}
void ui_separation_show(void) { if (g_sp_job) g_sp_open = true; }

/* the parts' colours (the same as under the waveform) */
static Color part_color(int k)
{
    static const Color C[SEP_NPART] = { { 80, 210, 255, 255 }, { 240, 200, 60, 255 }, { 170, 110, 250, 255 },
                                        { 90, 210, 120, 255 }, { 250, 120, 190, 255 } };
    return k >= 0 && k < SEP_NPART ? C[k] : g_theme.text_dim;
}

/* a check box: true = clicked */
static bool check_box(int x, int y, const char *label, bool on, Color mark)
{
    Vector2 m = GetMousePosition();
    int tw = ui_text_w(label, 14);
    Rectangle hit = { (float)x, (float)y, (float)(24 + tw + 6), 22.0f };
    bool hov = CheckCollisionPointRec(m, hit);
    Rectangle bx = { (float)x, (float)y + 2, 18.0f, 18.0f };
    DrawRectangleRec(bx, lighten(g_theme.panel_bg2, hov ? 22 : 8));
    DrawRectangleLinesEx(bx, 1, on ? mark : g_theme.text_dim);
    if (on) DrawRectangle(x + 4, y + 6, 10, 10, mark);
    ui_text(label, x + 26, y + 3, 14, g_theme.text);
    return hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

/* 1.4.17: what to separate */
static void draw_sep_choose(Session *s, int x, int y, int w, int h)
{
    int lx = x + 20, cy = y + 58, by = y + h - 52;
    char line[600];
    ui_text("What should be separated from the song?", lx, cy, 15, g_theme.text);
    {   /* quick choices */
        int qx = x + w - 20 - 100 - 8 - 130;
        if (seg_button(qx, cy - 6, 130, "Vocals only", g_sp_req.parts == SEP_P_VOCALS, true)) g_sp_req.parts = SEP_P_VOCALS;
        if (seg_button(qx + 138, cy - 6, 100, "Everything", g_sp_req.parts == SEP_P_ALL, true)) g_sp_req.parts = SEP_P_ALL;
    }
    cy += 34;
    /* the parts: one big toggle each */
    {
        int bw = (w - 40 - 4 * 8) / SEP_NPART, bx = lx;
        for (int k = 0; k < SEP_NPART; ++k) {
            unsigned bit = 1u << k;
            bool on = (g_sp_req.parts & bit) != 0;
            Rectangle r = { (float)bx, (float)cy, (float)bw, 46.0f };
            Vector2 m = GetMousePosition();
            bool hov = CheckCollisionPointRec(m, r);
            Color c = part_color(k);
            Color bg = on ? (Color){ (unsigned char)(c.r / 3), (unsigned char)(c.g / 3), (unsigned char)(c.b / 3), 255 } : lighten(g_theme.panel_bg2, hov ? 22 : 8);
            DrawRectangleRec(r, bg);
            DrawRectangleLinesEx(r, on ? 2 : 1, on ? c : g_theme.text_dim);
            Rectangle bx2 = { r.x + 10, r.y + 14, 18, 18 };
            DrawRectangleLinesEx(bx2, 1, on ? c : g_theme.text_dim);
            if (on) DrawRectangle((int)bx2.x + 4, (int)bx2.y + 4, 10, 10, c);
            ui_text(sep_part_name(k), (int)r.x + 36, (int)r.y + 15, 15, on ? c : g_theme.text);
            if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_sp_req.parts ^= bit;
            bx += bw + 8;
        }
    }
    cy += 56;
    unsigned p = g_sp_req.parts;
    char names[STEM_MAX][STEM_NAME_LEN];
    int nn = sep_request_parts(&g_sp_req, names);
    if (nn > 0) {
        snprintf(line, sizeof line, "Makes %d track%s: ", nn, nn == 1 ? "" : "s");
        for (int k = 0; k < nn; ++k) {
            size_t ln = strlen(line);
            snprintf(line + ln, sizeof line - ln, "%s%s", k ? (k == nn - 1 ? " + " : ", ") : "", names[k]);
        }
        ui_text(line, lx, cy, 14, g_theme.text);
        ui_text(p == SEP_P_VOCALS ? "The instrumental is everything else - together they are exactly the song."
                                  : "\"Other\" is everything not chosen - all the tracks together are exactly the song.",
                lx, cy + 20, 13, g_theme.text_dim);
    } else ui_text("Choose at least one part.", lx, cy, 14, (Color){ 230, 90, 80, 255 });
    cy += 50;
    /* the 6-part model and the vocals */
    bool multi = p && p != SEP_P_VOCALS;
    if (multi) {
        ui_text("Instruments by", lx, cy + 6, 14, g_theme.text);
        int mx = lx + 120;
        if (seg_button(mx, cy, 200, "BS-RoFormer SW (best)", g_sp_req.multi != SEP_DEMUCS, true)) g_sp_req.multi = SEP_BSRSW;
        if (seg_button(mx + 208, cy, 200, "HTDemucs 6s (faster)", g_sp_req.multi == SEP_DEMUCS, true)) g_sp_req.multi = SEP_DEMUCS;
        ui_text(sep_model_info(g_sp_req.multi == SEP_DEMUCS ? SEP_DEMUCS : SEP_BSRSW), lx + 120, cy + 34, 12, g_theme.text_dim);
        cy += 58;
        if (p & SEP_P_VOCALS) {
            if (check_box(lx, cy, "Vocals by Mel-Band RoFormer first (cleaner vocals, takes longer)", g_sp_req.best_vocals, part_color(0)))
                g_sp_req.best_vocals = !g_sp_req.best_vocals;
            cy += 30;
        }
    } else cy += 30;
    /* what it costs */
    if (nn > 0) {
        double mb = sep_need_mb(&g_sp_req);
        int need = sep_ram_need_mb(&g_sp_req), tot = sep_ram_total_mb();
        int mods[2], nm = sep_request_models(&g_sp_req, mods);
        if (mb > 0) snprintf(line, sizeof line, "First it downloads about %.0f MB (once - remove it any time in File -> Separation...).", mb);
        else snprintf(line, sizeof line, "The model%s %s here - it works offline.", nm > 1 ? "s" : "", nm > 1 ? "are" : "is");
        ui_text(line, lx, cy, 13, g_theme.text_dim);
        bool slow = nm > 1 || mods[nm - 1] == SEP_BSRSW;
        snprintf(line, sizeof line, "Needs about %.1f GB of RAM; %s. Q-LiM keeps playing.", need / 1024.0,
                 slow ? "takes a while (a few times the song's length on a slow PC)" : "a few minutes per song");
        while (ui_text_w(line, 13) > w - 40 && strlen(line) > 10) line[strlen(line) - 4] = 0;
        ui_text(line, lx, cy + 20, 13, g_theme.text_dim);
        if (tot > 0 && tot < need + 2500) {                  /* (the system and Q-LiM need their part too) */
            snprintf(line, sizeof line, "This computer has %.1f GB of RAM - it may be very slow or fail. Close other programs%s.",
                     tot / 1024.0, multi && g_sp_req.multi != SEP_DEMUCS ? ", or use HTDemucs" : "");
            while (ui_text_w(line, 13) > w - 40 && strlen(line) > 10) line[strlen(line) - 4] = 0;
            ui_text(line, lx, cy + 40, 13, (Color){ 240, 150, 70, 255 });
        }
    }
    if (button(x + w - 20 - 110 - 12 - 160, by, 110, 34, "Cancel", false) || IsKeyPressed(KEY_ESCAPE)) { g_sp_open = false; g_sp_choose = false; return; }
    if (nn > 0 && (button(x + w - 20 - 160, by, 160, 34, "Separate", true) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))) sep_go(s);
}

static void draw_separate(Session *s, int sw, int sh)
{
    GfxDialogBackdrop(70);
    int w = 700, h = g_sp_choose ? 470 : 320;
    int x = (sw - w) / 2, y = (sh - h) / 2;
    draw_frame(x, y, w, h, "Separate", g_sp_t);
    if (g_sp_choose) { draw_sep_choose(s, x, y, w, h); return; }
    int lx = x + 20, cy = y + 58;
    char line[600];
    int by = y + h - 52;
    if (g_sp_ask_dir) {                              /* 1.4.15: once: where to keep them */
        SepPrefs pr;
        sep_prefs_get(&pr);
        char cur[1024];
        sep_stems_dir(cur, sizeof cur);
        ui_text("Where should the separated tracks be kept?", lx, cy, 15, g_theme.text);
        snprintf(line, sizeof line, "%s", cur);
        while (ui_text_w(line, 14) > w - 40 && strlen(line) > 10) memmove(line, line + 4, strlen(line + 4) + 1);
        ui_text(line, lx, cy + 30, 14, g_theme.selection);
        ui_text("Every song gets its own folder there, e.g. \"Song/Song (Vocals).flac\".", lx, cy + 62, 13, g_theme.text_dim);
        snprintf(line, sizeof line, "Saved as %s%s. Folder, format and sample rate: File -> Separation...",
                 export_format_name(pr.format), export_format_has_bits(pr.format) ? (pr.bits == 16 ? " 16-bit" : pr.bits == 24 ? " 24-bit" : " 32-bit float") : "");
        while (ui_text_w(line, 13) > w - 40 && strlen(line) > 10) line[strlen(line) - 4] = 0;
        ui_text(line, lx, cy + 82, 13, g_theme.text_dim);
        if (button(x + 20, by, 110, 34, "Cancel", false) || IsKeyPressed(KEY_ESCAPE)) { g_sp_open = false; return; }
        bool pick = button(x + w - 20 - 170 - 12 - 160, by, 160, 34, "Choose folder...", false);
        bool use = button(x + w - 20 - 170, by, 170, 34, "Use this folder", true) || IsKeyPressed(KEY_ENTER);
        if (pick) {
            char chosen[1024];
            GfxExternalDialogBegin();
            int r = project_ask_folder("Q-LiM - Where to keep the separated tracks", cur, chosen, sizeof chosen);
            GfxExternalDialogEnd();
            if (r == 0 && chosen[0]) { snprintf(pr.dir, sizeof pr.dir, "%s", chosen); use = true; }
        }
        if (use) {
            pr.asked = true;
            sep_prefs_set(&pr);
            g_sp_ask_dir = false;
            if (!g_sp_ask) sep_begin(s);
        }
        return;
    }
    if (g_sp_ask) {
        char dd[1024];
        sep_data_dir(dd, sizeof dd);
        int mods[2], nm = sep_request_models(&g_sp_req, mods);
        char ml[200] = "";
        for (int k = 0; k < nm; ++k) if (!sep_model_installed(mods[k])) {
            size_t ln = strlen(ml);
            snprintf(ml + ln, sizeof ml - ln, "%s%s", ml[0] ? " + " : "", sep_model_title(mods[k]));
        }
        ui_text("The first time, these are downloaded once:", lx, cy, 15, g_theme.text);
        snprintf(line, sizeof line, "%s%s - about %.0f MB", ml[0] ? ml : "", sep_runtime_installed() ? "" : (ml[0] ? " + ONNX Runtime" : "ONNX Runtime"),
                 sep_need_mb(&g_sp_req));
        ui_text(line, lx, cy + 24, 15, g_theme.selection);
        snprintf(line, sizeof line, "They are kept in %s", dd);
        while (ui_text_w(line, 13) > w - 40 && strlen(line) > 10) memmove(line + 14, line + 18, strlen(line + 18) + 1);
        ui_text(line, lx, cy + 52, 13, g_theme.text_dim);
        ui_text("and can be removed in File -> Separation... (then downloaded again only if needed).", lx, cy + 70, 13, g_theme.text_dim);
        ui_text("After that everything works offline; the result of each song is kept.", lx, cy + 100, 14, g_theme.text_dim);
        if (button(x + w - 20 - 110 - 12 - 200, by, 110, 34, "Cancel", false) || IsKeyPressed(KEY_ESCAPE)) { g_sp_open = false; return; }
        if (button(x + w - 20 - 200, by, 200, 34, "Download & Separate", true) || IsKeyPressed(KEY_ENTER)) {
            g_sp_ask = false;
            sep_begin(s);
        }
        return;
    }
    if (g_sp_job) {
        float p = 0.0f;
        char text[300] = "";
        int st = sep_state(g_sp_job, &p, text, sizeof text);
        while (ui_text_w(text, 15) > w - 40 && strlen(text) > 10) text[strlen(text) - 4] = 0;
        ui_text(text, lx, cy, 15, g_theme.text);
        Rectangle bar = { (float)lx, (float)(cy + 36), (float)(w - 40), 22.0f };
        DrawRectangleRec(bar, lighten(g_theme.panel_bg2, 6));
        DrawRectangle(lx, cy + 36, (int)((w - 40) * p), 22, g_theme.selection);
        DrawRectangleLinesEx(bar, 1, g_theme.text_dim);
        int ns = 1, stp = sep_step(g_sp_job, &ns);
        if (st == SEP_DOWNLOADING) snprintf(line, sizeof line, "Downloading (only the first time)");
        else if (st == SEP_PREPARING) snprintf(line, sizeof line, "Preparing");
        else if (ns > 1) snprintf(line, sizeof line, "Separating - model %d of %d", stp, ns);
        else snprintf(line, sizeof line, "Separating");
        ui_text(line, lx, cy + 70, 13, g_theme.text_dim);
        ui_text("You can hide this window: it goes on, the clip shows the progress.", lx, cy + 100, 13, g_theme.text_dim);
        if (button(x + w - 20 - 110 - 12 - 110, by, 110, 34, "Cancel", false)) sep_cancel(g_sp_job);
        if (button(x + w - 20 - 110, by, 110, 34, "Hide", true) || IsKeyPressed(KEY_ESCAPE)) g_sp_open = false;
        return;
    }
    snprintf(line, sizeof line, "%s", g_sp_msg[0] ? g_sp_msg : "...");
    /* (up to three lines if long) */
    Color mc = g_sp_rc == 1 ? (Color){ 230, 90, 80, 255 } : g_theme.text;
    char *rest = line;
    for (int row = 0; row < 3 && *rest; ++row) {
        char t[600];
        snprintf(t, sizeof t, "%s", rest);
        size_t cut = strlen(t);
        if (ui_text_w(t, 15) > w - 40) {
            size_t best = 0;
            for (size_t i = 1; t[i]; ++i) {
                if (t[i] != ' ') continue;
                char u[600];
                memcpy(u, t, i); u[i] = 0;
                if (ui_text_w(u, 15) <= w - 40) best = i; else break;
            }
            if (best) cut = best;
        }
        t[cut] = 0;
        ui_text(t, lx, cy + row * 22, 15, mc);
        rest += cut;
        while (*rest == ' ') ++rest;
    }
    if (button(x + w - 20 - 110, by, 110, 34, "OK", true) || IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_ENTER)) g_sp_open = false;
}

/* ============ 1.4.15: File -> Separation... (where and how the separated tracks are saved, 1.4.17: the models) ============ */

static bool   g_ss_open = false;
static double g_ss_t = 0.0;
static int    g_ss_del = 0;                  /* "Delete all": 1 = asked once (click again) */
static int    g_ss_rm = -1;                  /* 1.4.17: a model to remove: asked once (click again); SEP_NMODEL = ONNX Runtime */
static double g_ss_use_t = -10.0;            /* the space in use is counted at most once a second */
static double g_ss_bytes = 0.0;
static int    g_ss_songs = 0;

void ui_open_sep_settings(void) { g_ss_open = true; g_ss_t = GetTime(); g_ss_del = 0; g_ss_rm = -1; g_ss_use_t = -10.0; }

static void draw_sep_settings(int sw, int sh)
{
    GfxDialogBackdrop(70);
    int w = 720, h = 640;
    int x = (sw - w) / 2, y = (sh - h) / 2;
    draw_frame(x, y, w, h, "Separation", g_ss_t);
    int lx = x + 20, vx = x + 140, cy = y + 58;
    SepPrefs pr;
    sep_prefs_get(&pr);
    bool changed = false;
    char line[1200], cur[1024];
    sep_stems_dir(cur, sizeof cur);

    /* the folder */
    ui_text("Folder", lx, cy + 6, 15, g_theme.text);
    snprintf(line, sizeof line, "%s", cur);
    while (ui_text_w(line, 13) > w - 160 - 220 && strlen(line) > 10) memmove(line, line + 4, strlen(line + 4) + 1);
    ui_text(line, vx, cy + 7, 13, g_theme.selection);
    if (seg_button(x + w - 20 - 200, cy, 110, "Change...", false, true)) {
        char chosen[1024];
        GfxExternalDialogBegin();
        int r = project_ask_folder("Q-LiM - Where to keep the separated tracks", cur, chosen, sizeof chosen);
        GfxExternalDialogEnd();
        if (r == 0 && chosen[0]) { snprintf(pr.dir, sizeof pr.dir, "%s", chosen); pr.asked = true; changed = true; g_ss_use_t = -10.0; }
    }
    if (seg_button(x + w - 20 - 84, cy, 84, "Default", !pr.dir[0], true)) { pr.dir[0] = 0; pr.asked = true; changed = true; g_ss_use_t = -10.0; }
    cy += 46;

    /* the format (like Export Audio) */
    ui_text("Format", lx, cy + 6, 15, g_theme.text);
    {
        int bx = vx, bw = 78;
        for (int f = 0; f < EXP_NFMT; ++f) {
            const char *nm = f == EXP_OGG ? "Ogg" : export_format_name(f);
            if (seg_button(bx, cy, bw, nm, pr.format == f, export_format_available(f)) && pr.format != f) { pr.format = f; changed = true; }
            bx += bw + 6;
        }
    }
    cy += 42;
    if (export_format_has_bits(pr.format)) {
        ui_text("Bits", lx, cy + 6, 15, g_theme.text);
        const int bits[3] = { 16, 24, 32 };
        const char *bl[3] = { "16 bit", "24 bit", "32 bit float" };
        int curb = export_fix_bits(pr.format, pr.bits), bx = vx;
        for (int i = 0; i < 3; ++i) {
            int bw = i == 2 ? 130 : 90;
            if (seg_button(bx, cy, bw, bl[i], curb == bits[i], !(pr.format == EXP_FLAC && i == 2)) && pr.bits != bits[i]) { pr.bits = bits[i]; changed = true; }
            bx += bw + 6;
        }
    } else {
        ui_text("Quality", lx, cy + 6, 15, g_theme.text);
        const char *ql[3] = { "High", "Medium", "Small" };
        int bx = vx;
        for (int i = 0; i < 3; ++i) {
            if (seg_button(bx, cy, 90, ql[i], pr.quality == i, true) && pr.quality != i) { pr.quality = i; changed = true; }
            bx += 96;
        }
    }
    cy += 42;
    ui_text("Sample rate", lx, cy + 6, 15, g_theme.text);
    {
        const int rates[4] = { 0, 48000, 88200, 96000 };
        const char *rl[4] = { "44.1k (as made)", "48k", "88.2k", "96k" };
        int bx = vx;
        for (int i = 0; i < 4; ++i) {
            int r = rates[i] ? rates[i] : 44100;
            bool ok = export_fix_rate(pr.format, r) == r;
            int bw = i == 0 ? 140 : 70;
            if (seg_button(bx, cy, bw, rl[i], pr.rate == rates[i] && ok, ok) && pr.rate != rates[i]) { pr.rate = rates[i]; changed = true; }
            bx += bw + 6;
        }
    }
    cy += 42;
    /* what it means */
    {
        int r = export_fix_rate(pr.format, pr.rate > 0 ? pr.rate : 44100);
        double mb;                                    /* one track, one minute */
        if (export_format_lossy(pr.format)) {
            static const double KB[3][3] = { { 320, 192, 128 }, { 290, 160, 96 }, { 320, 175, 130 } };   /* Ogg, Opus, MP3 kbit/s */
            int row = pr.format == EXP_OGG ? 0 : pr.format == EXP_OPUS ? 1 : 2;
            mb = KB[row][pr.quality] * 1000.0 / 8.0 * 60.0 / 1048576.0;
        } else {
            int b = export_fix_bits(pr.format, pr.bits);
            mb = (double)r * 2 * (b / 8) * 60.0 / 1048576.0;
            if (pr.format == EXP_FLAC) mb *= 0.6;
        }
        snprintf(line, sizeof line, "About %.0f MB per minute of song for each track (vocals + instrumental: twice that).", mb);
        ui_text(line, lx, cy, 13, g_theme.text);
        ui_text(export_format_lossy(pr.format) ? "Smaller files; a little of the quality is lost (the tracks together are no longer exactly the original)."
                                               : "Lossless: all the tracks together are exactly the original.", lx, cy + 20, 13, g_theme.text_dim);
        ui_text("The settings are for new separations; songs already separated stay as they are.", lx, cy + 40, 13, g_theme.text_dim);
    }
    cy += 70;
    /* the space they take + delete */
    double now = GetTime();
    if (now - g_ss_use_t > 1.0) { sep_usage(&g_ss_bytes, &g_ss_songs); g_ss_use_t = now; }
    if (g_ss_bytes >= 1073741824.0) snprintf(line, sizeof line, "In this folder: %d separated song%s, %.2f GB", g_ss_songs, g_ss_songs == 1 ? "" : "s", g_ss_bytes / 1073741824.0);
    else snprintf(line, sizeof line, "In this folder: %d separated song%s, %.0f MB", g_ss_songs, g_ss_songs == 1 ? "" : "s", g_ss_bytes / 1048576.0);
    ui_text(line, lx, cy + 7, 15, g_theme.text);
    if (g_ss_songs > 0) {
        const char *dl = g_ss_del ? "Click again: delete them all" : "Delete all...";
        int dw = ui_text_w(dl, 14) + 24;
        if (button(x + w - 20 - dw, cy, dw, 30, dl, g_ss_del != 0)) {
            if (g_ss_del) {
                int n = sep_delete_all();
                char m[120];
                snprintf(m, sizeof m, "Deleted the separated tracks of %d song%s (clips play their original files)", n, n == 1 ? "" : "s");
                ui_flash_message(m);
                g_ss_del = 0;
                g_ss_use_t = -10.0;
            } else g_ss_del = 1;
        }
        ui_text("Deleted songs can be separated again at any time (offline).", lx, cy + 34, 12, g_theme.text_dim);
    }
    cy += 62;
    /* 1.4.17: the downloaded models: their size, remove */
    DrawLine(lx, cy - 6, x + w - 20, cy - 6, g_theme.grid_line);
    ui_text("AI models (downloaded once, when first needed)", lx, cy + 2, 15, g_theme.text);
    cy += 28;
    bool busy = sep_busy_with_models();
    double total = 0;
    for (int m = 0; m <= SEP_NMODEL; ++m) {
        bool rt = m == SEP_NMODEL;
        double mb = rt ? sep_runtime_disk_mb() : sep_model_disk_mb(m);
        total += mb;
        ui_text(rt ? "ONNX Runtime (runs the models)" : sep_model_title(m), lx, cy + 4, 14, mb > 0 ? g_theme.text : g_theme.text_dim);
        if (mb > 0) snprintf(line, sizeof line, "%.0f MB", mb);
        else snprintf(line, sizeof line, rt ? "not downloaded" : "not downloaded (%.0f MB when needed)", rt ? 0.0 : sep_model_download_mb(m));
        ui_text(line, lx + 290, cy + 5, 13, mb > 0 ? g_theme.selection : g_theme.text_dim);
        if (mb > 0) {
            bool ask = g_ss_rm == m;
            const char *bl = busy ? "In use" : ask ? "Click again: remove" : "Remove";
            int bw = ui_text_w(bl, 14) + 24;
            if (busy) seg_button(x + w - 20 - bw, cy, bw, bl, false, false);
            else if (button(x + w - 20 - bw, cy, bw, 28, bl, ask)) {
                if (ask) {
                    int r = rt ? sep_runtime_remove() : sep_model_remove(m);
                    char msg[200];
                    snprintf(msg, sizeof msg, r == 0 ? "Removed %s (%.0f MB freed)" : "Could not remove %s", rt ? "ONNX Runtime" : sep_model_title(m), mb);
                    ui_flash_message(msg);
                    g_ss_rm = -1;
                } else g_ss_rm = m;
            }
        }
        cy += 32;
    }
    snprintf(line, sizeof line, "Together %.0f MB. A removed model is simply downloaded again the next time a separation needs it.", total);
    ui_text(line, lx, cy + 2, 12, g_theme.text_dim);
    if (changed) sep_prefs_set(&pr);
    if (button(x + w - 20 - 110, y + h - 52, 110, 34, "Close", true) || IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_ENTER)) g_ss_open = false;
}

/* ============ 1.4.17: Launch at a Time (a clip or a scene at an exact clock time) ============ */

static bool    g_sch_open = false;
static double  g_sch_t = 0.0;
static int     g_sch_kind = -1, g_sch_ch = -1, g_sch_sc = -1;   /* kind -1 = only the list */
static int     g_sch_hh, g_sch_mm, g_sch_ss;
static bool    g_sch_fade, g_sch_daily;
static int     g_sch_scroll = 0;

static void sch_from_now(double add) { sched_clock(sched_now() + add, &g_sch_hh, &g_sch_mm, &g_sch_ss); }

void ui_open_schedule(Session *s, int kind, int ch, int scene)
{
    (void)s;
    g_sch_open = true; g_sch_t = GetTime(); g_sch_scroll = 0;
    g_sch_kind = kind; g_sch_ch = kind == SCHED_SCENE ? -1 : ch; g_sch_sc = scene;
    int i = sched_find(kind, g_sch_ch, scene);
    const SchedItem *it = sched_get(i);
    if (it) { g_sch_hh = it->hh; g_sch_mm = it->mm; g_sch_ss = it->ss; g_sch_fade = it->fade; g_sch_daily = it->daily; }
    else {
        sch_from_now(60.0);                          /* in a minute, on the full minute */
        g_sch_ss = 0;
        g_sch_fade = kind == SCHED_SCENE;
        g_sch_daily = false;
    }
}
void ui_open_schedule_list(Session *s) { ui_open_schedule(s, -1, -1, -1); }

/* "Scene 3" / "Clip 'Intro' (Drums, scene 2)" */
static void sch_what(Session *s, int kind, int ch, int sc, char *out, size_t n)
{
    if (kind == SCHED_SCENE) { snprintf(out, n, "Scene %d", sc + 1); return; }
    const char *cn = s && ch >= 0 && ch < s->channel_count ? s->channels[ch].name : "?";
    const char *nm = s && ch >= 0 && ch < s->channel_count && sc >= 0 && sc < MAX_SCENES && atomic_load(&s->channels[ch].clips[sc].has_clip)
                     ? s->channels[ch].clips[sc].name : "(empty cell)";
    snprintf(out, n, "%s  (%s, scene %d)", nm, cn, sc + 1);
}
static void sch_left_text(double left, char *out, size_t n)
{
    if (left < 0) { snprintf(out, n, "passed"); return; }
    long l = (long)ceil(left);
    if (l < 60) snprintf(out, n, "in %ld s", l);
    else if (l < 3600) snprintf(out, n, "in %ld min %ld s", l / 60, l % 60);
    else snprintf(out, n, "in %ld h %ld min", l / 3600, (l % 3600) / 60);
}

/* one number of the time: [ - ] 21 [ + ], the wheel over it too */
static void sch_spin(int x, int y, int *v, int max, const char *lbl)
{
    Vector2 m = GetMousePosition();
    ui_text(lbl, x + 30, y - 18, 12, g_theme.text_dim);
    Rectangle r = { (float)(x + 26), (float)y, 56.0f, 40.0f };
    bool hov = CheckCollisionPointRec(m, r);
    DrawRectangleRec(r, lighten(g_theme.panel_bg2, hov ? 18 : 6));
    DrawRectangleLinesEx(r, 1, g_theme.selection);
    char t[8];
    snprintf(t, sizeof t, "%02d", *v);
    ui_text(t, (int)r.x + (56 - ui_text_w(t, 26)) / 2, y + 7, 26, g_theme.text);
    if (hov) {
        float wh = GetMouseWheelMove();
        if (wh > 0) *v = (*v + 1) % (max + 1);
        if (wh < 0) *v = (*v + max) % (max + 1);
    }
    if (button(x, y + 6, 24, 28, "-", false)) *v = (*v + max) % (max + 1);
    if (button(x + 84, y + 6, 24, 28, "+", false)) *v = (*v + 1) % (max + 1);
}

static void draw_schedule(Session *s, int sw, int sh)
{
    GfxDialogBackdrop(70);
    bool target = g_sch_kind >= 0;
    int w = 660, h = target ? 600 : 420;
    int x = (sw - w) / 2, y = (sh - h) / 2;
    draw_frame(x, y, w, h, "Launch at a Time", g_sch_t);
    int lx = x + 20, cy = y + 56;
    char line[400], what[300];
    int ch_, cm_, cs_;
    sched_clock(sched_now(), &ch_, &cm_, &cs_);
    snprintf(line, sizeof line, "The computer's clock: %02d:%02d:%02d", ch_, cm_, cs_);
    ui_text(line, x + w - 20 - ui_text_w(line, 13), cy + 2, 13, g_theme.text_dim);
    int idx = target ? sched_find(g_sch_kind, g_sch_ch, g_sch_sc) : -1;
    if (target) {
        sch_what(s, g_sch_kind, g_sch_ch, g_sch_sc, what, sizeof what);
        snprintf(line, sizeof line, "%s", what);
        while (ui_text_w(line, 15) > w - 260 && strlen(line) > 8) line[strlen(line) - 4] = 0;
        ui_text(line, lx, cy, 15, g_theme.selection);
        cy += 46;
        sch_spin(lx, cy, &g_sch_hh, 23, "hour");
        ui_text(":", lx + 114, cy + 6, 26, g_theme.text);
        sch_spin(lx + 130, cy, &g_sch_mm, 59, "minute");
        ui_text(":", lx + 244, cy + 6, 26, g_theme.text);
        sch_spin(lx + 260, cy, &g_sch_ss, 59, "second");
        {   /* quick: from now */
            int qx = lx + 400;
            if (seg_button(qx, cy - 4, 96, "now + 1 min", false, true)) sch_from_now(60.0);
            if (seg_button(qx + 102, cy - 4, 96, "+ 5 min", false, true)) sch_from_now(300.0);
            if (seg_button(qx, cy + 28, 96, "+ 15 min", false, true)) sch_from_now(900.0);
            if (seg_button(qx + 102, cy + 28, 96, "full hour", false, true)) { sch_from_now(3600.0); g_sch_mm = 0; g_sch_ss = 0; }
        }
        cy += 62;
        {   /* when that is */
            double at = sched_next_time(g_sch_hh, g_sch_mm, g_sch_ss), now = sched_now();
            int nh, nm, ns;
            sched_clock(now, &nh, &nm, &ns);
            bool tomorrow = (g_sch_hh * 3600 + g_sch_mm * 60 + g_sch_ss) <= (nh * 3600 + nm * 60 + ns);
            char lft[64];
            sch_left_text(at - now, lft, sizeof lft);
            snprintf(line, sizeof line, "Starts %s at %02d:%02d:%02d - %s.", tomorrow ? "tomorrow" : "today", g_sch_hh, g_sch_mm, g_sch_ss, lft);
            ui_text(line, lx, cy, 14, g_theme.text);
        }
        cy += 30;
        if (g_sch_kind == SCHED_SCENE) {
            snprintf(line, sizeof line, "With the scene transition (what plays fades out, the scene fades in: %.1f s)", (double)session_get_transition_sec(s));
            if (check_box(lx, cy, line, g_sch_fade, g_theme.selection)) g_sch_fade = !g_sch_fade;
            cy += 28;
        }
        if (check_box(lx, cy, "Every day at this time", g_sch_daily, g_theme.selection)) g_sch_daily = !g_sch_daily;
        cy += 30;
        ui_text("It is not quantized: it starts at that second. \"Stop automations\" (AUTO in the top bar) holds it.", lx, cy, 12, g_theme.text_dim);
        cy += 26;
        int by = cy;
        if (idx >= 0 && button(lx, by, 110, 32, "Remove", false)) { sched_remove(idx); ui_flash_message("The launch at a time is removed"); idx = -1; }
        if (button(x + w - 20 - 150, by, 150, 32, idx >= 0 ? "Change" : "Set", true) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
            if (idx >= 0) sched_set(idx, g_sch_hh, g_sch_mm, g_sch_ss, g_sch_fade, g_sch_daily);
            else idx = sched_add(g_sch_kind, g_sch_ch, g_sch_sc, g_sch_hh, g_sch_mm, g_sch_ss, g_sch_fade, g_sch_daily);
            if (idx < 0) ui_flash_message("Too many launches at a time (64)");
            else {
                char m[200];
                snprintf(m, sizeof m, "%s starts at %02d:%02d:%02d", what, g_sch_hh, g_sch_mm, g_sch_ss);
                ui_flash_message(m);
                g_sch_open = false;
                return;
            }
        }
        cy += 48;
        DrawLine(lx, cy - 8, x + w - 20, cy - 8, g_theme.grid_line);
    }
    /* all of them */
    int n = sched_count();
    snprintf(line, sizeof line, n ? "Planned launches (%d)" : "No launches at a time yet (right click a clip or a scene: Launch at Time...)", n);
    ui_text(line, lx, cy, 14, g_theme.text);
    cy += 24;
    int rows = (y + h - 64 - cy) / 28;
    if (rows < 1) rows = 1;
    if (g_sch_scroll > n - rows) g_sch_scroll = n - rows;
    if (g_sch_scroll < 0) g_sch_scroll = 0;
    {
        Rectangle lr = { (float)lx, (float)cy, (float)(w - 40), (float)(rows * 28) };
        if (CheckCollisionPointRec(GetMousePosition(), lr)) {
            float wh = GetMouseWheelMove();
            if (wh < 0) g_sch_scroll++;
            if (wh > 0) g_sch_scroll--;
        }
    }
    for (int r = 0; r < rows && g_sch_scroll + r < n; ++r) {
        int i = g_sch_scroll + r;
        const SchedItem *it = sched_get(i);
        if (!it) break;
        int ry = cy + r * 28;
        char tm[16], lft[64];
        sched_format(it, tm, sizeof tm);
        sch_what(s, it->kind, it->ch, it->sc, what, sizeof what);
        if (it->state == SCHED_MISSED) snprintf(lft, sizeof lft, "missed (held)");
        else sch_left_text(sched_seconds_left(i), lft, sizeof lft);
        bool me = target && it->kind == g_sch_kind && it->sc == g_sch_sc && (it->kind == SCHED_SCENE || it->ch == g_sch_ch);
        if (me) DrawRectangle(lx - 4, ry - 2, w - 32, 26, lighten(g_theme.panel_bg2, 10));
        ui_text(tm, lx, ry + 3, 15, it->state == SCHED_MISSED ? (Color){ 230, 110, 90, 255 } : g_theme.selection);
        snprintf(line, sizeof line, "%s%s%s", what, it->fade && it->kind == SCHED_SCENE ? "  - fade" : "", it->daily ? "  - every day" : "");
        while (ui_text_w(line, 14) > w - 330 && strlen(line) > 8) line[strlen(line) - 4] = 0;
        ui_text(line, lx + 80, ry + 4, 14, g_theme.text);
        ui_text(lft, x + w - 20 - 36 - 10 - ui_text_w(lft, 13), ry + 5, 13, g_theme.text_dim);
        if (button(x + w - 20 - 36, ry, 36, 24, "x", false)) { sched_remove(i); break; }
    }
    if (button(x + w - 20 - 110, y + h - 52, 110, 34, target ? "Cancel" : "Close", !target) || IsKeyPressed(KEY_ESCAPE)) g_sch_open = false;
}

/* ============ 1.4.19: Surround (a channel through 4 ... 8 outputs around the room) ============ */

static bool   g_sd_open = false;
static double g_sd_t = 0.0;
static int    g_sd_ch = -1;
static int    g_sd_drag = -1;                /* a speaker being moved (its angle), 100 = the channel's position */
static SurSetup g_sd_last = { .n = 0 };      /* the last setup made (offered when switching on) */
/* the editing state of the Surround window (both modes; 2.1: the circle has a path too) */
enum { RT_SPK = 0, RT_POS = 1, RT_PATH = 2 };
static int    g_rm_tool = RT_PATH;
static int    g_rm_spk = -1;                 /* the selected speaker */
static int    g_rm_pt = -1;                  /* the selected point of the path */
static int    g_rm_drag = -1;                /* 0..15 speaker, 100 position, 200+i point i, 300 the cursor, 400+i a point's time */
static float  g_rm_cursor = 0.0f;            /* the time a new point gets (s) */
static SurPt  g_rm_pts[SUR_MAX_PTS];
static int    g_rm_n = 0;

void ui_open_surround(Session *s, int ch)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    g_sd_open = true; g_sd_t = GetTime(); g_sd_ch = ch; g_sd_drag = -1;
    g_rm_drag = -1; g_rm_pt = -1; g_rm_spk = -1;
    int sc = -1, sch = -1;
    g_rm_tool = ui_get_selected_clip(&sch, &sc) && sch == ch ? RT_PATH : RT_POS;   /* 2.1 */
}

static float circ_az(float cx, float cy, Vector2 m) { return sur_wrap(atan2f(m.x - cx, -(m.y - cy)) * 180.0f / (float)M_PI); }

/* 2.1: the timeline of the selected clip under the Circle and the Room windows: its path's points (marks to drag),
 * the cursor (blue - the time a new point gets), the playhead (yellow), Play from cursor / Stop, Clear path. */
static void sur_timeline(Session *s, int TX, int TY, int TW, int cch, int csc, const Clip *cl, float dur, bool playing,
                         float ph_t, double sr, bool fine, bool *pts_changed)
{
    Vector2 m = GetMousePosition();
    char line[96];
    {
        char nm[200];
        snprintf(nm, sizeof nm, "Path of the clip \"%s\" (%.1f s) - %d point%s", cl->name, (double)dur, g_rm_n, g_rm_n == 1 ? "" : "s");
        ui_text(nm, TX, TY - 20, 13, g_theme.text);
        Rectangle tr = { (float)TX, (float)TY, (float)TW, 26 };
        DrawRectangleRec(tr, g_theme.panel_bg2);
        DrawRectangleLinesEx(tr, 1, g_theme.grid_line);
        for (int sec = 0; sec <= (int)dur; sec += dur > 60 ? 10 : dur > 20 ? 5 : 1) {
            float px = TX + TW * sec / dur;
            DrawRectangle((int)px, TY + 18, 1, 8, g_theme.text_dim);
        }
        int hov_tick = -1;
        for (int i = 0; i < g_rm_n; ++i) {
            float px = TX + TW * g_rm_pts[i].t / dur;
            if (fabsf(m.x - px) < 6 && m.y >= TY && m.y <= TY + 26) hov_tick = i;
            DrawRectangle((int)px - 3, TY + 3, 6, 20, i == g_rm_pt ? RAYWHITE : (Color){ 120, 200, 255, 255 });
        }
        float cxp = TX + TW * g_rm_cursor / dur;
        DrawRectangle((int)cxp - 1, TY - 4, 3, 34, (Color){ 80, 160, 255, 255 });
        if (playing) DrawRectangle((int)(TX + TW * ph_t / dur), TY - 4, 2, 34, (Color){ 255, 230, 100, 255 });
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(m, tr)) {
            if (hov_tick >= 0) { g_rm_pt = hov_tick; g_rm_drag = 400 + hov_tick; g_rm_tool = RT_PATH; }
            else g_rm_drag = 300;
        }
        if (g_rm_drag >= 300 && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            float t = (m.x - TX) / TW * dur;
            if (t < 0) t = 0;
            if (t > dur) t = dur;
            if (!fine) t = roundf(t * 10.0f) / 10.0f;
            if (g_rm_drag == 300) g_rm_cursor = t;
            else if (g_rm_drag - 400 < g_rm_n && g_rm_pts[g_rm_drag - 400].t != t) {
                g_rm_pts[g_rm_drag - 400].t = t;
                SurPt keep = g_rm_pts[g_rm_drag - 400];
                sur_sort(g_rm_pts, g_rm_n);
                for (int i = 0; i < g_rm_n; ++i) if (g_rm_pts[i].t == keep.t && g_rm_pts[i].x == keep.x && g_rm_pts[i].y == keep.y) { g_rm_drag = 400 + i; g_rm_pt = i; }
                *pts_changed = true;
            }
        } else if (g_rm_drag >= 300 && !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) g_rm_drag = -1;
        snprintf(line, sizeof line, "cursor %.2f s", (double)g_rm_cursor);
        ui_text(line, TX, TY + 32, 12, (Color){ 80, 160, 255, 255 });
        if (button(TX + 120, TY + 32, 150, 28, playing ? "Stop" : "Play from cursor", false)) {
            if (playing) session_stop_clip(s, cch, csc);
            else session_play_clip_from(s, cch, csc, (size_t)((double)g_rm_cursor * sr));
        }
        if (g_rm_n > 0 && button(TX + 280, TY + 32, 120, 28, "Clear path", false)) { g_rm_n = 0; g_rm_pt = -1; *pts_changed = true; }
        ui_text("Drag the cursor / a point's mark here. While it plays, a click on the plan puts a point where it is.", TX + 420, TY + 38, 12, g_theme.text_dim);
    }
}

/* ---- 1.4.22: the room mode - the hall from above, the speakers anywhere, the path of the selected clip ---- */

static void draw_surround_room(Session *s, int sw, int sh, SurSetup *su, bool *changed)
{
    int w = sw - 20 < 1220 ? sw - 20 : 1220, h = sh - 20 < 820 ? sh - 20 : 820;
    int x = (sw - w) / 2, y = (sh - h) / 2;
    draw_frame(x, y, w, h, "Surround - Room", g_sd_t);
    Vector2 m = GetMousePosition();
    char line[320];
    int cy0 = y + 56;
    snprintf(line, sizeof line, "Channel \"%s\" - the hall from above (the stage at the top)", s->channels[g_sd_ch].name);
    ui_text(line, x + 20, cy0, 15, g_theme.text);
    if (seg_button(x + w - 20 - 340, cy0 - 6, 80, "Circle", false, true)) { su->mode = SUR_MODE_CIRCLE; *changed = true; }
    seg_button(x + w - 20 - 256, cy0 - 6, 80, "Room", true, true);
    if (seg_button(x + w - 20 - 170, cy0 - 6, 80, "Off", false, true)) { g_sd_last = *su; session_set_surround(s, g_sd_ch, NULL); return; }
    seg_button(x + w - 20 - 84, cy0 - 6, 84, "On", true, true);

    /* the clip whose path is drawn */
    int cch = -1, csc = -1;
    bool have_clip = ui_get_selected_clip(&cch, &csc) && cch == g_sd_ch && csc >= 0 && csc < MAX_SCENES &&
                     atomic_load(&s->channels[cch].clips[csc].has_clip) && !session_is_midi(s, cch, csc) && !session_is_note(s, cch, csc);
    Clip *cl = have_clip ? &s->channels[cch].clips[csc] : NULL;
    double sr = s->sample_rate > 0 ? s->sample_rate : 48000;
    float dur = cl ? (float)((double)__atomic_load_n(&cl->wav.frame_count, __ATOMIC_RELAXED) / sr) : 10.0f;
    if (!(dur > 0.05f)) dur = 10.0f;
    bool playing = cl && atomic_load(&cl->playing);
    float ph_t = cl ? (float)((double)atomic_load(&cl->position) / sr) : 0.0f;
    if (have_clip && g_rm_drag < 200) g_rm_n = session_clip_get_sur_points(s, cch, csc, g_rm_pts, SUR_MAX_PTS);
    if (!have_clip) { g_rm_n = 0; if (g_rm_tool == RT_PATH) g_rm_tool = RT_POS; }
    if (g_rm_pt >= g_rm_n) g_rm_pt = -1;
    if (g_rm_spk >= su->n) g_rm_spk = -1;
    bool pts_changed = false;

    /* the plan */
    int PX = x + 20, PY = y + 120, PH = h - 120 - 182;
    float RW = su->room_w > 1 ? su->room_w : 16, RD = su->room_d > 1 ? su->room_d : 26;
    int PWmax = (int)(w * 0.52f);
    float k = (float)PH / RD;
    if (RW * k > PWmax) k = PWmax / RW;
    float hw = RW * k, hh = RD * k;
    float hx0 = (float)PX, hy0 = (float)PY;
    #define SX(xx) (hx0 + ((xx) + RW * 0.5f) * k)
    #define SY(yy) (hy0 + (RD - (yy)) * k)
    #define MX(px) (((px) - hx0) / k - RW * 0.5f)
    #define MY(py) (RD - ((py) - hy0) / k)
    DrawRectangle((int)hx0, (int)hy0, (int)hw, (int)hh, lighten(g_theme.panel_bg2, 2));
    float stg = su->stage_d > 0 && su->stage_d < RD ? su->stage_d : 0;
    if (stg > 0) {
        DrawRectangle((int)hx0, (int)hy0, (int)hw, (int)(stg * k), (Color){ 60, 50, 30, 255 });
        ui_text("STAGE", (int)(hx0 + hw / 2 - ui_text_w("STAGE", 12) / 2), (int)hy0 + 4, 12, (Color){ 220, 190, 120, 255 });
        DrawRectangle((int)hx0, (int)(hy0 + stg * k), (int)hw, 2, (Color){ 220, 190, 120, 200 });
    }
    for (float gx = 2; gx < RW; gx += 2) DrawRectangle((int)(hx0 + gx * k), (int)hy0, 1, (int)hh, (Color){ g_theme.grid_line.r, g_theme.grid_line.g, g_theme.grid_line.b, 70 });
    for (float gy = 2; gy < RD; gy += 2) DrawRectangle((int)hx0, (int)(hy0 + gy * k), (int)hw, 1, (Color){ g_theme.grid_line.r, g_theme.grid_line.g, g_theme.grid_line.b, 70 });
    DrawRectangleLinesEx((Rectangle){ hx0, hy0, hw, hh }, 2, g_theme.grid_line);
    ui_text("AUDIENCE", (int)(hx0 + hw / 2 - ui_text_w("AUDIENCE", 12) / 2), (int)(hy0 + hh - 18), 12, g_theme.text_dim);
    snprintf(line, sizeof line, "%.0f x %.0f m", (double)RW, (double)RD);
    ui_text(line, (int)hx0, (int)(hy0 + hh + 4), 11, g_theme.text_dim);
    Rectangle plan_r = { hx0, hy0, hw, hh };
    bool in_plan = CheckCollisionPointRec(m, plan_r);

    /* where the sound is now (playing) or would be (the chosen point / the channel's position) */
    float cx_, cy_, cw_, csp;
    bool now = playing && session_surround_now_room(s, g_sd_ch, &cx_, &cy_, &cw_);
    if (!now) {
        if (g_rm_tool == RT_PATH && g_rm_pt >= 0) { cx_ = g_rm_pts[g_rm_pt].x; cy_ = g_rm_pts[g_rm_pt].y; cw_ = g_rm_pts[g_rm_pt].w; csp = g_rm_pts[g_rm_pt].spread; }
        else if (g_rm_n > 0) sur_eval_room(g_rm_pts, g_rm_n, g_rm_cursor, su, &cx_, &cy_, &cw_, &csp);
        else { cx_ = su->x0; cy_ = su->y0; cw_ = su->w0; csp = su->spread0; }
    } else {
        float t1, t2, t3;
        sur_eval_room(g_rm_pts, g_rm_n, ph_t, su, &t1, &t2, &t3, &csp);
    }
    float gl[SUR_MAX_SPK], gr[SUR_MAX_SPK], gsum[SUR_MAX_SPK];
    if (cw_ >= 0.05f) { sur_gains_room(su, cx_ - cw_ * 0.5f, cy_, csp, gl); sur_gains_room(su, cx_ + cw_ * 0.5f, cy_, csp, gr); }
    else { sur_gains_room(su, cx_, cy_, csp, gl); memcpy(gr, gl, sizeof gl); }
    for (int i = 0; i < su->n; ++i) gsum[i] = sqrtf(0.5f * (gl[i] * gl[i] + gr[i] * gr[i]));

    int hov_pt = -1, hov_spk = -1;
    /* the speakers */
    for (int i = 0; i < su->n; ++i) {
        float px = SX(su->x[i]), py = SY(su->y[i]);
        Rectangle br = { px - 28, py - 15, 56, 30 };
        if (g_rm_tool == RT_SPK && CheckCollisionPointRec(m, br)) hov_spk = i;
        bool bad = su->out[i] >= hw_outputs();
        Color bg = bad ? (Color){ 150, 50, 45, 255 } : i == g_rm_spk ? (Color){ 70, 130, 190, 255 } : lighten(g_theme.panel_bg, 14);
        DrawRectangleRounded(br, 0.3f, bg);
        float lv = gsum[i] > 1 ? 1 : gsum[i];                  /* how much of the sound it gets now */
        if (lv > 0.01f) DrawRectangle((int)br.x + 2, (int)(br.y + br.height - 4), (int)((br.width - 4) * lv), 3, (Color){ 255, 200, 80, 255 });
        DrawRectangleRoundedLines(br, 0.3f, 1, i == hov_spk ? RAYWHITE : (Color){ 120, 200, 255, 255 });
        const char *nm = su->layout >= 0 && su->layout < SUR_R_N ? sur_room_speaker_name(su->layout, i) : "";
        char t1[24];
        if (nm[0]) snprintf(t1, sizeof t1, "%s", nm); else snprintf(t1, sizeof t1, "%d", i + 1);
        ui_text(t1, (int)(px - ui_text_w(t1, 10) / 2), (int)py - 13, 10, RAYWHITE);
        snprintf(t1, sizeof t1, "out %d", su->out[i] + 1);
        ui_text(t1, (int)(px - ui_text_w(t1, 10) / 2), (int)py - 1, 10, g_theme.text);
    }
    /* the path */
    if (g_rm_n > 0) {
        for (int i = 0; i + 1 < g_rm_n; ++i)
            DrawLineEx((Vector2){ SX(g_rm_pts[i].x), SY(g_rm_pts[i].y) }, (Vector2){ SX(g_rm_pts[i + 1].x), SY(g_rm_pts[i + 1].y) }, 2.0f, (Color){ 120, 200, 255, 200 });
    }
    if (g_rm_tool == RT_PATH)
        for (int i = 0; i < g_rm_n; ++i) {
            float px = SX(g_rm_pts[i].x), py = SY(g_rm_pts[i].y);
            if ((m.x - px) * (m.x - px) + (m.y - py) * (m.y - py) < 81) hov_pt = i;
        }
    for (int i = 0; i < g_rm_n; ++i) {
        float px = SX(g_rm_pts[i].x), py = SY(g_rm_pts[i].y), ww = g_rm_pts[i].w * 0.5f * k;
        Color pc = i == g_rm_pt ? RAYWHITE : (Color){ 120, 200, 255, 255 };
        if (ww >= 2) { DrawRectangle((int)(px - ww), (int)py - 1, (int)(2 * ww), 2, (Color){ 120, 200, 255, 120 });
                       DrawCircleV((Vector2){ px - ww, py }, 3.0f, pc); DrawCircleV((Vector2){ px + ww, py }, 3.0f, pc); }
        DrawCircleV((Vector2){ px, py }, i == hov_pt || i == g_rm_pt ? 7.0f : 5.5f, pc);
        snprintf(line, sizeof line, "%.1fs", (double)g_rm_pts[i].t);
        ui_text(line, (int)px + 8, (int)py - 16, 11, pc);
    }
    /* the channel's own position (no points) */
    {
        float px = SX(su->x0), py = SY(su->y0), ww = su->w0 * 0.5f * k;
        Color oc = g_rm_tool == RT_POS ? (Color){ 255, 160, 60, 255 } : (Color){ 255, 160, 60, 110 };
        if (ww >= 2) { DrawCircleV((Vector2){ px - ww, py }, 4.0f, oc); DrawCircleV((Vector2){ px + ww, py }, 4.0f, oc); }
        DrawCircleV((Vector2){ px, py }, 8.0f, oc);
    }
    if (now) {                                                   /* playing: the yellow dot follows */
        float px = SX(cx_), py = SY(cy_), ww = cw_ * 0.5f * k;
        if (ww >= 2) { DrawCircleV((Vector2){ px - ww, py }, 4.0f, (Color){ 255, 230, 100, 255 }); DrawCircleV((Vector2){ px + ww, py }, 4.0f, (Color){ 255, 230, 100, 255 }); }
        DrawCircleV((Vector2){ px, py }, 7.0f, (Color){ 255, 230, 100, 255 });
    }

    /* the mouse on the plan */
    float mx_ = MX(m.x), my_ = MY(m.y);
    if (mx_ < -RW * 0.5f) mx_ = -RW * 0.5f;
    if (mx_ > RW * 0.5f) mx_ = RW * 0.5f;
    if (my_ < 0) my_ = 0;
    if (my_ > RD) my_ = RD;
    bool fine = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (!fine) { mx_ = roundf(mx_ * 4.0f) / 4.0f; my_ = roundf(my_ * 4.0f) / 4.0f; }   /* 25 cm steps (Shift: free) */
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && in_plan) {
        if (g_rm_tool == RT_SPK) { g_rm_spk = hov_spk; if (hov_spk >= 0) g_rm_drag = hov_spk; }
        else if (g_rm_tool == RT_POS) g_rm_drag = 100;
        else if (g_rm_tool == RT_PATH && have_clip) {
            if (hov_pt >= 0) { g_rm_pt = hov_pt; g_rm_drag = 200 + hov_pt; }
            else {                                           /* a new point at the cursor (playing: at the playhead) */
                float t = playing ? ph_t : g_rm_cursor;
                int same = -1;
                for (int i = 0; i < g_rm_n; ++i) if (fabsf(g_rm_pts[i].t - t) < 0.02f) same = i;
                if (same >= 0) { g_rm_pts[same].x = mx_; g_rm_pts[same].y = my_; g_rm_pt = same; }
                else if (g_rm_n < SUR_MAX_PTS) {
                    float ex, ey, ew, es;
                    sur_eval_room(g_rm_pts, g_rm_n, t, su, &ex, &ey, &ew, &es);
                    SurPt np = { t, 0, es, mx_, my_, ew };
                    g_rm_pts[g_rm_n++] = np;
                    sur_sort(g_rm_pts, g_rm_n);
                    for (int i = 0; i < g_rm_n; ++i) if (g_rm_pts[i].t == t) g_rm_pt = i;
                }
                g_rm_drag = 200 + g_rm_pt;
                pts_changed = true;
            }
        }
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && g_rm_tool == RT_PATH && hov_pt >= 0) {
        memmove(&g_rm_pts[hov_pt], &g_rm_pts[hov_pt + 1], (size_t)(g_rm_n - hov_pt - 1) * sizeof(SurPt));
        g_rm_n--; g_rm_pt = -1; pts_changed = true;
    }
    if (g_rm_drag >= 0 && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        if (g_rm_drag < SUR_MAX_SPK && g_rm_drag < su->n) {
            if (su->x[g_rm_drag] != mx_ || su->y[g_rm_drag] != my_) { su->x[g_rm_drag] = mx_; su->y[g_rm_drag] = my_; su->layout = su->layout >= 0 ? su->layout : -1; *changed = true; }
        } else if (g_rm_drag == 100) {
            if (su->x0 != mx_ || su->y0 != my_) { su->x0 = mx_; su->y0 = my_; *changed = true; }
        } else if (g_rm_drag >= 200 && g_rm_drag < 200 + SUR_MAX_PTS && g_rm_drag - 200 < g_rm_n) {
            SurPt *p = &g_rm_pts[g_rm_drag - 200];
            if (p->x != mx_ || p->y != my_) { p->x = mx_; p->y = my_; pts_changed = true; }
        }
    } else if (g_rm_drag >= 0 && g_rm_drag < 300 && !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) g_rm_drag = -1;
    #undef SX
    #undef SY
    #undef MX
    #undef MY

    /* the controls (right) */
    int rx = (int)(hx0 + hw) + 30, ry = y + 100, rw = x + w - 20 - rx;
    ui_text("Edit", rx, ry + 6, 14, g_theme.text);
    if (seg_button(rx + 70, ry, 110, "Speakers", g_rm_tool == RT_SPK, true)) g_rm_tool = RT_SPK;
    if (seg_button(rx + 184, ry, 110, "Position", g_rm_tool == RT_POS, true)) g_rm_tool = RT_POS;
    if (seg_button(rx + 298, ry, 110, "Path", g_rm_tool == RT_PATH, have_clip)) g_rm_tool = RT_PATH;
    ry += 34;
    ui_text(g_rm_tool == RT_SPK ? "Drag a speaker to where it stands in the hall (Shift: free, else 25 cm steps)."
            : g_rm_tool == RT_POS ? "Drag the orange dot: where the channel sounds when its clip has no path."
            : !have_clip ? "Path: first select a clip of this channel (click it), then open this window."
            : playing ? "Playing: click on the plan = a point at the playhead. Drag a point, right click = delete."
            : "Click on the plan = a point at the cursor's time (below). Drag a point, right click = delete.",
            rx, ry, 12, g_theme.text_dim);
    ry += 30;
    ui_text("Layout", rx, ry + 6, 14, g_theme.text);
    for (int l = 0; l < SUR_R_N; ++l)
        if (seg_button(rx + 70 + l * 114, ry, 110, sur_room_layout_name(l), su->layout == l, true)) {
            int first = su->n > 0 ? su->out[0] : 0;
            float x0 = su->x0, y0 = su->y0, w0 = su->w0, sp0 = su->spread0;
            sur_room_layout(su, l, first);
            su->x0 = x0; su->y0 = y0; su->w0 = w0; su->spread0 = sp0;
            g_rm_spk = -1; *changed = true;
        }
    ry += 36;
    {   /* the outputs (all together) */
        int lo = 99, hi = -1;
        for (int i = 0; i < su->n; ++i) { if (su->out[i] < lo) lo = su->out[i]; if (su->out[i] > hi) hi = su->out[i]; }
        ui_text("Outputs", rx, ry + 6, 14, g_theme.text);
        snprintf(line, sizeof line, "%d - %d", lo + 1, hi + 1);
        if (button(rx + 70, ry, 30, 28, "-", false) && lo > 0) { for (int i = 0; i < su->n; ++i) su->out[i]--; *changed = true; }
        ui_text(line, rx + 110, ry + 6, 14, g_theme.selection);
        if (button(rx + 180, ry, 30, 28, "+", false) && hi + 1 < MAX_HW_OUT) { for (int i = 0; i < su->n; ++i) su->out[i]++; *changed = true; }
        snprintf(line, sizeof line, "(%d outputs)", hw_outputs());
        ui_text(line, rx + 220, ry + 7, 12, hi >= hw_outputs() ? (Color){ 230, 90, 80, 255 } : g_theme.text_dim);
    }
    ry += 36;
    if (g_rm_tool == RT_SPK) {                                  /* the chosen speaker */
        if (g_rm_spk >= 0) {
            const char *nm = su->layout >= 0 && su->layout < SUR_R_N ? sur_room_speaker_name(su->layout, g_rm_spk) : "";
            snprintf(line, sizeof line, "Speaker %d%s%s%s: output %d", g_rm_spk + 1, nm[0] ? " (" : "", nm, nm[0] ? ")" : "", su->out[g_rm_spk] + 1);
            ui_text(line, rx, ry + 6, 14, (Color){ 120, 200, 255, 255 });
            int bx = rx + ui_text_w(line, 14) + 12;
            if (button(bx, ry, 30, 28, "-", false) && su->out[g_rm_spk] > 0) { su->out[g_rm_spk]--; *changed = true; }
            if (button(bx + 34, ry, 30, 28, "+", false) && su->out[g_rm_spk] + 1 < MAX_HW_OUT) { su->out[g_rm_spk]++; *changed = true; }
            if (su->n > 1 && button(bx + 74, ry, 80, 28, "Delete", false)) {
                for (int i = g_rm_spk; i + 1 < su->n; ++i) { su->out[i] = su->out[i + 1]; su->x[i] = su->x[i + 1]; su->y[i] = su->y[i + 1]; su->az[i] = su->az[i + 1]; }
                su->n--; su->layout = -1; g_rm_spk = -1; *changed = true;
            }
        } else ui_text("Click a speaker to choose it.", rx, ry + 6, 13, g_theme.text_dim);
        ry += 36;
        if (su->n < SUR_MAX_SPK && button(rx, ry, 150, 28, "+ Speaker", false)) {
            int mo = -1;
            for (int i = 0; i < su->n; ++i) if (su->out[i] > mo) mo = su->out[i];
            su->x[su->n] = 0; su->y[su->n] = RD * 0.5f; su->out[su->n] = mo + 1 < MAX_HW_OUT ? mo + 1 : mo; su->az[su->n] = 0;
            g_rm_spk = su->n; su->n++; su->layout = -1; *changed = true;
        }
        ui_text("Hall", rx + 170, ry + 6, 14, g_theme.text);
        if (button(rx + 210, ry, 40, 28, "W-", false) && su->room_w > 4) { su->room_w -= 2; *changed = true; }
        if (button(rx + 254, ry, 40, 28, "W+", false) && su->room_w < 200) { su->room_w += 2; *changed = true; }
        if (button(rx + 304, ry, 40, 28, "D-", false) && su->room_d > 4) { su->room_d -= 2; if (su->stage_d > su->room_d - 2) su->stage_d = su->room_d - 2; *changed = true; }
        if (button(rx + 348, ry, 40, 28, "D+", false) && su->room_d < 200) { su->room_d += 2; *changed = true; }
        if (button(rx + 398, ry, 60, 28, "Stage-", false) && su->stage_d >= 1) { su->stage_d -= 1; *changed = true; }
        if (button(rx + 462, ry, 60, 28, "Stage+", false) && su->stage_d + 2 < su->room_d) { su->stage_d += 1; *changed = true; }
        ry += 36;
    } else {
        /* the width and the spread: of the chosen point, or of the channel's own position */
        bool on_pt = g_rm_tool == RT_PATH && g_rm_pt >= 0;
        float *pw_ = on_pt ? &g_rm_pts[g_rm_pt].w : &su->w0;
        float *ps_ = on_pt ? &g_rm_pts[g_rm_pt].spread : &su->spread0;
        if (on_pt) {
            snprintf(line, sizeof line, "Point %d: %.2f s", g_rm_pt + 1, (double)g_rm_pts[g_rm_pt].t);
            ui_text(line, rx, ry + 6, 14, (Color){ 120, 200, 255, 255 });
            static const float TS[4] = { -1.0f, -0.1f, 0.1f, 1.0f };
            static const char *const TL[4] = { "-1 s", "-0.1", "+0.1", "+1 s" };
            int bx = rx + 170;
            for (int b = 0; b < 4; ++b, bx += 54)
                if (button(bx, ry, 50, 28, TL[b], false)) {
                    float t = g_rm_pts[g_rm_pt].t + TS[b];
                    if (t < 0) t = 0;
                    if (t > dur) t = dur;
                    g_rm_pts[g_rm_pt].t = t;
                    SurPt keep = g_rm_pts[g_rm_pt];
                    sur_sort(g_rm_pts, g_rm_n);
                    for (int i = 0; i < g_rm_n; ++i) if (g_rm_pts[i].t == keep.t && g_rm_pts[i].x == keep.x) g_rm_pt = i;
                    pts_changed = true;
                }
        } else ui_text(g_rm_tool == RT_PATH ? "Width / spread below: of the chosen point (click one)." : "Width / spread below: of the channel's own position.",
                       rx, ry + 6, 13, g_theme.text_dim);
        ry += 36;
        ui_text("Stereo", rx, ry + 6, 14, g_theme.text);
        {
            float WV[5] = { 0, 4, 8, 12, RW };
            static const char *const WL[5] = { "Mono", "4 m", "8 m", "12 m", "Wall-wall" };
            for (int i = 0; i < 5; ++i)
                if (seg_button(rx + 70 + i * 84, ry, 80, WL[i], fabsf(*pw_ - WV[i]) < 0.05f, true)) {
                    *pw_ = WV[i];
                    if (on_pt) pts_changed = true; else *changed = true;
                }
            ui_text("left to the left wall, right to the right one (the distance between them)", rx + 70, ry + 32, 11, g_theme.text_dim);
        }
        ry += 54;
        ui_text("Spread", rx, ry + 6, 14, g_theme.text);
        {
            static const float SP[5] = { 0, 0.25f, 0.5f, 0.75f, 1.0f };
            static const char *const SL[5] = { "0", "25%", "50%", "75%", "All" };
            for (int i = 0; i < 5; ++i)
                if (seg_button(rx + 70 + i * 84, ry, 80, SL[i], fabsf(*ps_ - SP[i]) < 0.02f, true)) {
                    *ps_ = SP[i];
                    if (on_pt) pts_changed = true; else *changed = true;
                }
            ui_text("0 = a sharp point, All = every speaker the same", rx + 70, ry + 32, 11, g_theme.text_dim);
        }
        ry += 54;
    }
    (void)rw;

    /* the timeline of the clip: the points, the cursor (blue), the playhead (yellow) */
    int TX = x + 20, TW = w - 40, TY = y + h - 128;
    if (have_clip) sur_timeline(s, TX, TY, TW, cch, csc, cl, dur, playing, ph_t, sr, fine, &pts_changed);
    else ui_text("To draw a path: close this window, click a clip of this channel, then open this window again (or \"Surround\" above its waveform).", TX, TY + 4, 13, g_theme.text_dim);
    if (pts_changed && have_clip) session_clip_set_sur_points(s, cch, csc, g_rm_pts, g_rm_n);
    if (audio_graph_active())
        ui_text("PipeWire graph mode: surround works only in the normal mode (Audio Settings).", x + 20, y + h - 70, 12, (Color){ 240, 150, 70, 255 });
    if (button(x + w - 20 - 110 - 12 - 150, y + h - 52, 150, 34, "Speaker Map...", false)) {   /* 2.0 */
        int cch = -1, csc = -1;
        ui_get_selected_clip(&cch, &csc);
        int ch_ = g_sd_ch;
        g_sd_open = false;
        ui_open_speaker_map(s, ch_, cch == ch_ ? csc : -1);
        g_sm_src = 1;
        return;
    }
    if (button(x + w - 20 - 110, y + h - 52, 110, 34, "Close", true) || IsKeyPressed(KEY_ESCAPE)) g_sd_open = false;
}

static void draw_surround(Session *s, int sw, int sh)
{
    if (g_sd_ch >= 0 && g_sd_ch < s->channel_count) {               /* 1.4.22: the room mode has its own window */
        SurSetup rs;
        if (session_get_surround(s, g_sd_ch, &rs) && rs.mode == SUR_MODE_ROOM) {
            GfxDialogBackdrop(70);
            bool ch_ = false;
            draw_surround_room(s, sw, sh, &rs, &ch_);
            if (ch_ && session_get_surround(s, g_sd_ch, NULL)) {
                if (rs.mode == SUR_MODE_CIRCLE) {                    /* back to the circle: the angles from the plan */
                    float cyy = (rs.room_d - rs.stage_d) * 0.5f;
                    for (int k2 = 0; k2 < rs.n; ++k2) rs.az[k2] = sur_wrap(atan2f(rs.x[k2], rs.y[k2] - cyy) * 180.0f / (float)M_PI);
                    rs.layout = -1;
                }
                session_set_surround(s, g_sd_ch, &rs); g_sd_last = rs;
            }
            return;
        }
    }
    GfxDialogBackdrop(70);
    /* 2.1: the circle window has the path of the selected clip too (it was a lane over the waveform before) */
    int w = sw - 20 < 1120 ? sw - 20 : 1120, h = sh - 20 < 780 ? sh - 20 : 780;
    int x = (sw - w) / 2, y = (sh - h) / 2;
    draw_frame(x, y, w, h, "Surround", g_sd_t);
    if (g_sd_ch < 0 || g_sd_ch >= s->channel_count) { g_sd_open = false; return; }
    SurSetup su;
    bool on = session_get_surround(s, g_sd_ch, &su);
    bool changed = false, pts_changed = false;
    int nhw = hw_outputs();
    char line[300];
    int lx = x + 20, cy0 = y + 56;
    snprintf(line, sizeof line, "Channel \"%s\" plays through speakers around the room", s->channels[g_sd_ch].name);
    ui_text(line, lx, cy0, 15, g_theme.text);
    seg_button(x + w - 20 - 340, cy0 - 6, 80, "Circle", on, on);
    if (seg_button(x + w - 20 - 256, cy0 - 6, 80, "Room", false, true)) {      /* 1.4.22: the hall from above */
        if (!on) {
            su = g_sd_last;
            if (su.n <= 0 || su.mode != SUR_MODE_ROOM) { memset(&su, 0, sizeof su); sur_room_layout(&su, SUR_R_THEATER9, 0); }
        } else if (su.layout >= 0 && su.layout < SUR_L_N) {
            int first = su.out[0];
            sur_room_layout(&su, SUR_R_THEATER9, first);       /* a ready theatre from the same first output */
        }
        su.mode = SUR_MODE_ROOM;
        session_set_surround(s, g_sd_ch, &su); g_sd_last = su;
        g_rm_tool = RT_PATH; g_rm_pt = -1; g_rm_spk = -1;
        return;
    }
    if (seg_button(x + w - 20 - 170, cy0 - 6, 80, "Off", !on, true) && on) { g_sd_last = su; session_set_surround(s, g_sd_ch, NULL); on = false; }
    if (seg_button(x + w - 20 - 84, cy0 - 6, 84, "On", on, true) && !on) {
        su = g_sd_last;
        if (su.n <= 0) { memset(&su, 0, sizeof su); sur_layout(&su, nhw >= 8 ? SUR_L_RING8 : nhw >= 6 ? SUR_L_51 : SUR_L_QUAD, 0); }
        su.mode = SUR_MODE_CIRCLE;
        on = true; changed = true;
    }

    /* the clip whose path is edited (as in the room window) */
    int cch = -1, csc = -1;
    bool have_clip = on && ui_get_selected_clip(&cch, &csc) && cch == g_sd_ch && csc >= 0 && csc < MAX_SCENES &&
                     atomic_load(&s->channels[cch].clips[csc].has_clip) && !session_is_midi(s, cch, csc) && !session_is_note(s, cch, csc) &&
                     session_clip_mc_mode(s, cch, csc) != CLIP_MC_MULTI;   /* 1.4.24: a multichannel clip has its own channels */
    Clip *cl = have_clip ? &s->channels[cch].clips[csc] : NULL;
    double sr = s->sample_rate > 0 ? s->sample_rate : 48000;
    float dur = cl ? (float)((double)__atomic_load_n(&cl->wav.frame_count, __ATOMIC_RELAXED) / sr) : 10.0f;
    if (!(dur > 0.05f)) dur = 10.0f;
    bool playing = cl && atomic_load(&cl->playing);
    float ph_t = cl ? (float)((double)atomic_load(&cl->position) / sr) : 0.0f;
    if (have_clip && !(g_rm_drag >= 200 && g_rm_drag < 300) && g_rm_drag < 400) g_rm_n = session_clip_get_sur_points(s, cch, csc, g_rm_pts, SUR_MAX_PTS);
    if (!have_clip) { g_rm_n = 0; if (g_rm_tool == RT_PATH) g_rm_tool = RT_POS; }
    if (g_rm_pt >= g_rm_n) g_rm_pt = -1;
    bool fine = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    /* the circle (left) */
    int TY = y + h - 128;                                          /* the timeline (bottom) */
    float r = (float)(TY - 40 - (y + 100)) * 0.5f - 14.0f;
    if (r > 170.0f) r = 170.0f;
    if (r < 80.0f) r = 80.0f;
    float cx = (float)(x + 40) + r + 24.0f, cyy = (float)(y + 100) + 18.0f + r;
    DrawCircleLines((int)cx, (int)cyy, r, g_theme.grid_line);
    DrawCircleLines((int)cx, (int)cyy, r * 0.5f, g_theme.grid_line);
    DrawRectangle((int)cx - 1, (int)(cyy - r), 2, (int)(2 * r), (Color){ g_theme.grid_line.r, g_theme.grid_line.g, g_theme.grid_line.b, 90 });
    DrawRectangle((int)(cx - r), (int)cyy - 1, (int)(2 * r), 2, (Color){ g_theme.grid_line.r, g_theme.grid_line.g, g_theme.grid_line.b, 90 });
    ui_text("FRONT", (int)cx - ui_text_w("FRONT", 11) / 2, (int)(cyy - r - 16), 11, g_theme.text_dim);
    ui_text("BEHIND", (int)cx - ui_text_w("BEHIND", 11) / 2, (int)(cyy + r + 4), 11, g_theme.text_dim);
    ui_text("L", (int)(cx - r - 14), (int)cyy - 6, 11, g_theme.text_dim);
    ui_text("R", (int)(cx + r + 6), (int)cyy - 6, 11, g_theme.text_dim);
    Vector2 m = GetMousePosition();
    bool in_circ = (m.x - cx) * (m.x - cx) + (m.y - cyy) * (m.y - cyy) < (r + 24) * (r + 24);
    #define CPT(az_, sp_) ((Vector2){ cx + sinf((az_) * (float)M_PI / 180.0f) * r * (1.0f - (sp_) * 0.85f), \
                                      cyy - cosf((az_) * (float)M_PI / 180.0f) * r * (1.0f - (sp_) * 0.85f) })
    if (!on) {
        ui_text("Off: the channel uses its normal outputs (Routing).", (int)(cx - 160), (int)cyy - 8, 13, g_theme.text_dim);
    } else {
        int hov = -1, hov_pt = -1;
        for (int k = 0; k < su.n; ++k) {                     /* the speakers */
            float a = su.az[k] * (float)M_PI / 180.0f;
            float sx = cx + sinf(a) * r, sy = cyy - cosf(a) * r;
            Rectangle br = { sx - 22, sy - 13, 44, 26 };
            bool hv = g_rm_tool == RT_SPK && CheckCollisionPointRec(m, br) && g_sd_drag < 0;
            if (hv) hov = k;
            bool bad = su.out[k] >= nhw;
            DrawRectangleRounded(br, 0.3f, bad ? (Color){ 150, 50, 45, 255 } : hv || g_sd_drag == k ? (Color){ 70, 130, 190, 255 } : g_theme.panel_bg2);
            DrawRectangleRoundedLines(br, 0.3f, 1, (Color){ 120, 200, 255, 255 });
            char t[24];
            const char *nm = sur_speaker_name(su.layout, k);
            snprintf(t, sizeof t, "%s", nm[0] ? nm : "");
            ui_text(t, (int)(sx - ui_text_w(t, 11) / 2), (int)sy - 12, 11, RAYWHITE);
            snprintf(t, sizeof t, "out %d", su.out[k] + 1);
            ui_text(t, (int)(sx - ui_text_w(t, 10) / 2), (int)sy + 1, 10, g_theme.text);
        }
        /* the path of the clip: the real curve (sampled), then its points */
        if (g_rm_n > 0) {
            Vector2 prev = { 0, 0 };
            for (int i = 0; i <= 160; ++i) {
                float t = dur * (float)i / 160.0f, az, sp;
                sur_eval(g_rm_pts, g_rm_n, t, su.az0, su.spread0, &az, &sp);
                Vector2 q = CPT(az, sp);
                if (i > 0) DrawLineEx(prev, q, 2.0f, (Color){ 120, 200, 255, 170 });
                prev = q;
            }
        }
        if (g_rm_tool == RT_PATH)
            for (int i = 0; i < g_rm_n; ++i) {
                Vector2 q = CPT(g_rm_pts[i].az, g_rm_pts[i].spread);
                if ((m.x - q.x) * (m.x - q.x) + (m.y - q.y) * (m.y - q.y) < 81) hov_pt = i;
            }
        for (int i = 0; i < g_rm_n; ++i) {
            Vector2 q = CPT(g_rm_pts[i].az, g_rm_pts[i].spread);
            Color pc = i == g_rm_pt ? RAYWHITE : (Color){ 120, 200, 255, 255 };
            DrawCircleV(q, i == hov_pt || i == g_rm_pt ? 7.0f : 5.5f, pc);
            snprintf(line, sizeof line, "%.1fs", (double)g_rm_pts[i].t);
            ui_text(line, (int)q.x + 8, (int)q.y - 16, 11, pc);
        }
        /* the channel's own position (a clip without a path) */
        Vector2 pp = CPT(su.az0, su.spread0);
        Color oc = g_rm_tool == RT_POS ? (Color){ 255, 160, 60, 255 } : (Color){ 255, 160, 60, 110 };
        DrawCircleV(pp, 9.0f, oc);
        if (su.width >= 1.0f)
            for (int sd = -1; sd <= 1; sd += 2) DrawCircleV(CPT(su.az0 + sd * su.width * 0.5f, su.spread0), 4.0f, (Color){ oc.r, oc.g, oc.b, (unsigned char)(oc.a * 0.6f) });
        float naz, nsp;
        if (session_surround_now(s, g_sd_ch, &naz, &nsp)) DrawCircleV(CPT(naz, nsp), 6.0f, (Color){ 255, 230, 100, 255 });   /* now (playing) */
        else if (g_rm_n > 0 && g_rm_pt < 0) {                  /* where the cursor's time is */
            float az, sp;
            sur_eval(g_rm_pts, g_rm_n, g_rm_cursor, su.az0, su.spread0, &az, &sp);
            Vector2 q = CPT(az, sp);
            DrawCircleLines((int)q.x, (int)q.y, 8.0f, (Color){ 80, 160, 255, 255 });
        }

        /* the mouse in the circle: an angle (5 degree steps, Shift = free) and a spread (nearer the middle = wider) */
        float maz = circ_az(cx, cyy, m);
        if (!fine) maz = roundf(maz / 5.0f) * 5.0f;
        float md = sqrtf((m.x - cx) * (m.x - cx) + (m.y - cyy) * (m.y - cyy)) / r;
        float msp = (1.0f - (md > 1 ? 1 : md)) / 0.85f;
        msp = msp < 0 ? 0 : msp > 1 ? 1 : msp;
        if (!fine) msp = roundf(msp * 20.0f) / 20.0f;
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && in_circ) {
            if (g_rm_tool == RT_SPK) { if (hov >= 0) g_sd_drag = hov; }
            else if (g_rm_tool == RT_POS) g_sd_drag = 100;
            else if (g_rm_tool == RT_PATH && have_clip) {
                if (hov_pt >= 0) { g_rm_pt = hov_pt; g_rm_drag = 200 + hov_pt; }
                else {                                           /* a new point at the cursor (playing: at the playhead) */
                    float t = playing ? ph_t : g_rm_cursor;
                    int same = -1;
                    for (int i = 0; i < g_rm_n; ++i) if (fabsf(g_rm_pts[i].t - t) < 0.02f) same = i;
                    if (same >= 0) { g_rm_pts[same].az = maz; g_rm_pts[same].spread = msp; g_rm_pt = same; }
                    else if (g_rm_n < SUR_MAX_PTS) {
                        SurPt np = { t, maz, msp, 0, 0, 0 };
                        g_rm_pts[g_rm_n++] = np;
                        sur_sort(g_rm_pts, g_rm_n);
                        for (int i = 0; i < g_rm_n; ++i) if (g_rm_pts[i].t == t) g_rm_pt = i;
                    }
                    g_rm_drag = g_rm_pt >= 0 ? 200 + g_rm_pt : -1;
                    pts_changed = true;
                }
            }
        }
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && g_rm_tool == RT_PATH && hov_pt >= 0) {
            memmove(&g_rm_pts[hov_pt], &g_rm_pts[hov_pt + 1], (size_t)(g_rm_n - hov_pt - 1) * sizeof(SurPt));
            g_rm_n--; g_rm_pt = -1; pts_changed = true;
        }
        if (g_sd_drag >= 0 && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            if (g_sd_drag == 100) {
                if (su.az0 != maz || su.spread0 != msp) { su.az0 = sur_wrap(maz); su.spread0 = msp; changed = true; }
            } else if (g_sd_drag < su.n && su.az[g_sd_drag] != sur_wrap(maz)) { su.az[g_sd_drag] = sur_wrap(maz); su.layout = -1; changed = true; }
        } else if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) g_sd_drag = -1;
        if (g_rm_drag >= 200 && g_rm_drag < 300 && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            SurPt *p = g_rm_drag - 200 < g_rm_n ? &g_rm_pts[g_rm_drag - 200] : NULL;
            if (p && (p->az != maz || p->spread != msp)) { p->az = maz; p->spread = msp; pts_changed = true; }
        } else if (g_rm_drag >= 200 && g_rm_drag < 300) g_rm_drag = -1;
        if (hov >= 0) {                                       /* the wheel on a speaker: its output */
            float wh = GetMouseWheelMove();
            if (wh != 0.0f) {
                int o = su.out[hov] + (wh > 0 ? 1 : -1);
                if (o >= 0 && o < MAX_HW_OUT) { su.out[hov] = o; changed = true; }
            }
        }
    }
    #undef CPT

    /* the controls (right) */
    int rx = (int)(cx + r + 60), ry = y + 100;
    if (on) {
        ui_text("Edit", rx, ry + 6, 14, g_theme.text);
        if (seg_button(rx + 70, ry, 110, "Speakers", g_rm_tool == RT_SPK, true)) g_rm_tool = RT_SPK;
        if (seg_button(rx + 184, ry, 110, "Position", g_rm_tool == RT_POS, true)) g_rm_tool = RT_POS;
        if (seg_button(rx + 298, ry, 110, "Path", g_rm_tool == RT_PATH, have_clip)) g_rm_tool = RT_PATH;
        ry += 34;
        ui_text(g_rm_tool == RT_SPK ? "Drag a speaker around the circle (5 degree steps, Shift: free). Wheel on it: its output."
                : g_rm_tool == RT_POS ? "Drag the orange dot: where the channel sounds when its clip has no path."
                : !have_clip ? "Path: first select a clip of this channel (click it), then open this window."
                : playing ? "Playing: click in the circle = a point at the playhead. Drag a point, right click = delete."
                : "Click in the circle = a point at the cursor's time (below). Drag a point, right click = delete.",
                rx, ry, 12, g_theme.text_dim);
        ui_text("Nearer the middle = more spread (more speakers).", rx, ry + 16, 12, g_theme.text_dim);
        ry += 42;
    }
    ui_text("Layout", rx, ry + 6, 14, g_theme.text);
    for (int l = 0; l < SUR_L_N; ++l) {
        int bx = rx + 70 + (l % 4) * 82, by = ry + (l / 4) * 34;
        if (seg_button(bx, by, 76, sur_layout_name(l), on && su.layout == l, on)) {
            int first = su.n > 0 ? su.out[0] : 0;
            sur_layout(&su, l, first);
            changed = true;
        }
    }
    ry += 74;
    ui_text("Outputs", rx, ry + 6, 14, g_theme.text);
    if (on) {
        int lo = 99, hi = -1;
        for (int k = 0; k < su.n; ++k) { if (su.out[k] < lo) lo = su.out[k]; if (su.out[k] > hi) hi = su.out[k]; }
        snprintf(line, sizeof line, "%d - %d", lo + 1, hi + 1);
        if (button(rx + 70, ry, 30, 28, "-", false) && lo > 0) { for (int k = 0; k < su.n; ++k) su.out[k]--; changed = true; }
        ui_text(line, rx + 110, ry + 6, 14, g_theme.selection);
        if (button(rx + 180, ry, 30, 28, "+", false) && hi + 1 < MAX_HW_OUT) { for (int k = 0; k < su.n; ++k) su.out[k]++; changed = true; }
        snprintf(line, sizeof line, "(%d outputs)", nhw);
        ui_text(line, rx + 220, ry + 7, 12, hi >= nhw ? (Color){ 230, 90, 80, 255 } : g_theme.text_dim);
        if (su.layout == SUR_L_51 || su.layout == SUR_L_71)
            ui_text("(the 4th output, LFE, stays free)", rx + 70, ry + 32, 11, g_theme.text_dim);
    }
    ry += 52;
    ui_text("Width", rx, ry + 6, 14, g_theme.text);
    {
        static const float WD[4] = { 0, 60, 90, 120 };
        static const char *const WL[4] = { "Mono", "60", "90", "120" };
        for (int i = 0; i < 4; ++i)
            if (seg_button(rx + 70 + i * 62, ry, 56, WL[i], on && fabsf(su.width - WD[i]) < 0.5f, on)) { su.width = WD[i]; changed = true; }
        ui_text("degrees between the channel's left and right", rx + 70, ry + 32, 11, g_theme.text_dim);
    }
    ry += 52;
    /* the spread: of the chosen point of the path, or of the channel's own position */
    bool on_pt = on && g_rm_tool == RT_PATH && g_rm_pt >= 0 && g_rm_pt < g_rm_n;
    if (on_pt) {
        snprintf(line, sizeof line, "Point %d: %.2f s, %+.0f deg", g_rm_pt + 1, (double)g_rm_pts[g_rm_pt].t, (double)g_rm_pts[g_rm_pt].az);
        ui_text(line, rx, ry + 6, 14, (Color){ 120, 200, 255, 255 });
        static const float TS[4] = { -1.0f, -0.1f, 0.1f, 1.0f };
        static const char *const TL[4] = { "-1 s", "-0.1", "+0.1", "+1 s" };
        int bx = rx + 250;
        for (int b2 = 0; b2 < 4; ++b2, bx += 54)
            if (button(bx, ry, 50, 28, TL[b2], false)) {
                float t = g_rm_pts[g_rm_pt].t + TS[b2];
                if (t < 0) t = 0;
                if (t > dur) t = dur;
                g_rm_pts[g_rm_pt].t = t;
                SurPt keep = g_rm_pts[g_rm_pt];
                sur_sort(g_rm_pts, g_rm_n);
                for (int i = 0; i < g_rm_n; ++i) if (g_rm_pts[i].t == keep.t && g_rm_pts[i].az == keep.az) g_rm_pt = i;
                pts_changed = true;
            }
        ry += 36;
    }
    ui_text("Spread", rx, ry + 6, 14, g_theme.text);
    {
        float *ps_ = on_pt ? &g_rm_pts[g_rm_pt].spread : &su.spread0;
        static const float SP[5] = { 0, 0.25f, 0.5f, 0.75f, 1.0f };
        static const char *const SL[5] = { "0", "25%", "50%", "75%", "All" };
        for (int i = 0; i < 5; ++i)
            if (seg_button(rx + 70 + i * 50, ry, 46, SL[i], on && fabsf(*ps_ - SP[i]) < 0.02f, on)) {
                *ps_ = SP[i];
                if (on_pt) pts_changed = true; else changed = true;
            }
        ui_text(on_pt ? "of the chosen point: 0 = a point, All = every speaker the same"
                      : "0 = a point, All = every speaker the same", rx + 70, ry + 32, 11, g_theme.text_dim);
    }
    ry += 52;
    if (on && !on_pt) {
        snprintf(line, sizeof line, "Position: %+.0f deg (%s)", (double)su.az0,
                 fabsf(su.az0) < 10 ? "front" : fabsf(su.az0) > 170 ? "behind" : su.az0 < 0 ? "left" : "right");
        ui_text(line, rx, ry, 13, g_theme.text);
    }

    /* the timeline of the clip (bottom) */
    int TX = x + 20, TW = w - 40;
    if (have_clip) sur_timeline(s, TX, TY, TW, cch, csc, cl, dur, playing, ph_t, sr, fine, &pts_changed);
    else if (on) ui_text("To give a clip a path (the sound moves while it plays): click a clip of this channel, then open this window again.", TX, TY + 4, 13, g_theme.text_dim);
    if (pts_changed && have_clip) session_clip_set_sur_points(s, cch, csc, g_rm_pts, g_rm_n);
    if (audio_graph_active())
        ui_text("PipeWire graph mode: surround works only in the normal mode (Audio Settings).", x + 20, y + h - 70, 12, (Color){ 240, 150, 70, 255 });
    if (changed && on) { session_set_surround(s, g_sd_ch, &su); g_sd_last = su; }
    if (button(x + w - 20 - 110 - 12 - 150, y + h - 52, 150, 34, "Speaker Map...", false)) {   /* 2.0 */
        int ch_ = g_sd_ch;
        g_sd_open = false;
        ui_open_speaker_map(s, ch_, cch == ch_ ? csc : -1);
        g_sm_src = 1;
        return;
    }
    if (button(x + w - 20 - 110, y + h - 52, 110, 34, "Close", true) || IsKeyPressed(KEY_ESCAPE)) g_sd_open = false;
}

/* ============ 1.4.21: Sound Cards (more than one at the same time) ============ */

static bool       g_cards_open = false;
static double     g_cards_t = 0.0;
static OutxDevCfg g_cand[48];
static int        g_cand_max[48];
static int        g_ncand = 0;
static float      g_cards_scroll = 0.0f;

void ui_open_sound_cards(void)
{
    g_cards_open = true;
    g_cards_t = GetTime();
    g_cards_scroll = 0.0f;
    g_ncand = audio_outx_candidates(g_cand, g_cand_max, 48);
}

static void draw_sound_cards(int sw, int sh)
{
    GfxDialogBackdrop(70);
    int w = 820, h = 600;
    if (h > sh - 20) h = sh - 20;
    int x = (sw - w) / 2, y = (sh - h) / 2;
    draw_frame(x, y, w, h, "Sound Cards", g_cards_t);
    int lx = x + 20, cy = y + 52;
    ui_text("Use more than one sound card at the same time (a second card, the headphone output, Bluetooth speakers).",
            lx, cy, 13, g_theme.text_dim);
    ui_text("Their outputs get the numbers after the main card's - send channels or the master to them in Routing,",
            lx, cy + 18, 13, g_theme.text_dim);
    ui_text("where every output also has its own delay (to line up the speakers, e.g. with a Bluetooth one).",
            lx, cy + 36, 13, g_theme.text_dim);
    cy += 66;
    bool possible = audio_outx_possible();
    AudioOutGroup grp[1 + OUTX_MAX];
    int ngrp = audio_output_groups(grp, 1 + OUTX_MAX);
    Vector2 m = GetMousePosition();
    int remove_i = -1;
    for (int gi = 0; gi < ngrp; ++gi) {
        AudioOutGroup *g = &grp[gi];
        DrawRectangle(lx, cy, w - 40, 44, lighten(g_theme.panel_bg2, gi == 0 ? 4 : 0));
        Color sc = gi == 0 ? g_theme.selection : g->state == 0 ? (Color){ 110, 200, 130, 255 } : g->state == 1 ? (Color){ 230, 190, 80, 255 } : (Color){ 230, 100, 90, 255 };
        DrawRectangle(lx, cy, 4, 44, sc);
        char nm[200];
        snprintf(nm, sizeof nm, "%s%s", gi == 0 ? "Main: " : "", g->name);
        while (ui_text_w(nm, 15) > 380 && strlen(nm) > 8) nm[strlen(nm) - 1] = 0;
        ui_text(nm, lx + 14, cy + 5, 15, g_theme.text);
        char ol[64];
        if (g->first < 0) snprintf(ol, sizeof ol, "no numbers left");
        else if (g->count == 1) snprintf(ol, sizeof ol, "output %d", g->first + 1);
        else snprintf(ol, sizeof ol, "outputs %d-%d", g->first + 1, g->first + g->count);
        ui_text(ol, lx + 410, cy + 5, 15, (Color){ 120, 200, 255, 255 });
        char st[200];
        if (gi == 0) snprintf(st, sizeof st, "change it in Edit -> Audio Settings%s", g->lat_ms > 0 ? "" : "");
        else if (g->state == 0) {
            char dr[48] = "";
            if (g->dropouts) snprintf(dr, sizeof dr, ", %u dropout%s", g->dropouts, g->dropouts == 1 ? "" : "s");
            snprintf(st, sizeof st, "playing - about %.0f ms behind the main card (line them up with the delays in Routing), clock %+.0f ppm%s",
                     g->lat_ms, g->ppm, dr);
        }
        else if (g->state == 1) snprintf(st, sizeof st, "starting...%s%s", g->why[0] ? " " : "", g->why);
        else if (g->state == 3) snprintf(st, sizeof st, "off: %s", g->why);
        else snprintf(st, sizeof st, "not playing: %s - tried again by itself", g->why);
        ui_text(st, lx + 14, cy + 25, 12, gi == 0 ? g_theme.text_dim : sc);
        if (gi > 0 && button(x + w - 20 - 100, cy + 8, 90, 28, "Remove", false)) remove_i = gi - 1;
        cy += 50;
    }
    if (remove_i >= 0) {
        bool last = remove_i == outx_cfg_count() - 1;
        audio_outx_remove(remove_i);
        ui_flash_message(last ? "Sound card removed" : "Sound card removed - the ones after it got new output numbers");
    }
    cy += 6;
    if (!possible) {
        ui_text("PipeWire graph mode is on: there every channel is its own node - connect them to as many cards as you like",
                lx, cy, 13, (Color){ 240, 150, 70, 255 });
        ui_text("in qpwgraph / Patchance. The extra cards here and the output delays work in the normal mode.",
                lx, cy + 18, 13, (Color){ 240, 150, 70, 255 });
        if (button(lx, cy + 42, 240, 32, "Switch to the normal mode", true)) {
            AudioConfig c = audio_get_current_config();
            c.pw_graph = 0;
            ui_audio_apply(&c);
            g_cards_open = false;
        }
    } else if (outx_cfg_count() >= OUTX_MAX) {
        ui_text("8 extra cards - that is the most.", lx, cy, 14, g_theme.text_dim);
    } else {
        ui_text("Add a sound card:", lx, cy, 15, g_theme.text);
        if (button(x + w - 20 - 130, cy - 6, 130, 28, "Refresh list", false)) g_ncand = audio_outx_candidates(g_cand, g_cand_max, 48);
        cy += 26;
        int list_y = cy, list_h = y + h - 64 - list_y;
        int rows = 0;
        BeginScissorMode(lx, list_y, w - 40, list_h);
        for (int i = 0; i < g_ncand; ++i) {
            bool have = false;
            for (int k = 0; k < outx_cfg_count(); ++k) if (!strcmp(outx_cfg_get(k)->name, g_cand[i].name)) have = true;
            if (have) continue;
            int ry = list_y + rows * 34 - (int)g_cards_scroll;
            rows++;
            if (ry + 34 < list_y || ry > list_y + list_h) continue;
            Rectangle rr = { (float)lx, (float)ry, (float)(w - 40), 30 };
            bool hov = CheckCollisionPointRec(m, rr) && m.y >= list_y && m.y < list_y + list_h;
            DrawRectangleRec(rr, hov ? lighten(g_theme.panel_bg2, 18) : g_theme.panel_bg2);
            char nm[220];
            snprintf(nm, sizeof nm, "%s", g_cand[i].desc);
            while (ui_text_w(nm, 14) > w - 260 && strlen(nm) > 8) nm[strlen(nm) - 1] = 0;
            ui_text(nm, lx + 12, ry + 7, 14, g_theme.text);
            char oc[32];
            snprintf(oc, sizeof oc, "%d out", g_cand_max[i]);
            ui_text(oc, lx + w - 200, ry + 7, 13, g_theme.text_dim);
            ui_text("+ Add", lx + w - 110, ry + 7, 14, hov ? g_theme.selection : g_theme.text_dim);
            if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                OutxDevCfg c = g_cand[i];
                if (audio_outx_add(&c) == 0) {
                    char fm[240];
                    snprintf(fm, sizeof fm, "Added: %s", c.desc);
                    ui_flash_message(fm);
                }
            }
        }
        EndScissorMode();
        if (rows == 0) ui_text("(no other sound card found - plug one in / connect the speaker, then Refresh list)", lx + 12, list_y + 6, 13, g_theme.text_dim);
        float max_sc = (float)(rows * 34 - list_h);
        if (max_sc < 0) max_sc = 0;
        if (CheckCollisionPointRec(m, (Rectangle){ (float)lx, (float)list_y, (float)(w - 40), (float)list_h })) {
            float wh = GetMouseWheelMove();
            if (wh != 0.0f) g_cards_scroll -= wh * 40.0f;
        }
        if (g_cards_scroll > max_sc) g_cards_scroll = max_sc;
        if (g_cards_scroll < 0) g_cards_scroll = 0;
    }
    if (button(x + w - 20 - 110, y + h - 50, 110, 34, "Close", true) || IsKeyPressed(KEY_ESCAPE)) g_cards_open = false;
}

/* ============ 1.4.24: a surround (multichannel) file - how to play it ============ */

static bool    g_mc_open = false;
static double  g_mc_t = 0.0;
static int     g_mc_ch = -1, g_mc_sc = -1;
static int     g_mc_sel = CLIP_MC_MULTI;          /* the choice in the window */
static int     g_mc_out = 0;                      /* MULTI: the first output */
static bool    g_mc_import = false;               /* opened because the file was just added */
static bool    g_mc_all = false;                  /* the same for the other new surround files */
static McJob  *g_mc_job = NULL;                   /* PATH: the analysis runs */
static char    g_mc_mix[1100], g_mc_orig[1024];
static char    g_mc_msg[300];
static int     g_mc_rc = -1;
static int     g_mc_nch = 0, g_mc_layout = -1;
static char    g_mc_name[NAME_LEN];
static int     g_mc_last_choice = -1;             /* "the same for the others" */
static int     g_mc_last_out = 0;

static const char *mc_layout_text(int nch, int layout)
{
    static char t[32];
    if (nch == 6 && layout == SUR_L_51) return "5.1";
    if (nch == 8 && layout == SUR_L_71) return "7.1";
    if (nch == 5) return "5.0";
    if (nch == 7) return "7.0";
    if (nch == 4) return "Quad";
    snprintf(t, sizeof t, "%d channels", nch);
    return t;
}

void ui_open_mc(Session *s, int ch, int sc, bool import)
{
    if (!s || session_clip_mc_channels(s, ch, sc) <= 2) { ui_flash_message("This clip is not a surround file"); return; }
    g_mc_open = true; g_mc_t = GetTime(); g_mc_ch = ch; g_mc_sc = sc;
    g_mc_import = import;
    g_mc_all = false;
    g_mc_rc = -1; g_mc_msg[0] = 0;
    g_mc_nch = session_clip_mc_channels(s, ch, sc);
    int mode = session_clip_mc_mode(s, ch, sc);
    g_mc_sel = import ? CLIP_MC_MULTI : mode;
    g_mc_out = session_clip_mc_out(s, ch, sc);
    const char *orig = session_clip_mc_orig(s, ch, sc);
    snprintf(g_mc_orig, sizeof g_mc_orig, "%s", orig ? orig : s->channels[ch].clips[sc].path);
    float az[SUR_MAX_SPK];
    g_mc_layout = -1;
    char rbuf[1024];                                              /* 2.1: an M4A ...: its copy with all the channels */
    mc_file_directions(wav_readable_all_channels(g_mc_orig, s->sample_rate > 0 ? s->sample_rate : 48000, rbuf, sizeof rbuf), az, SUR_MAX_SPK, &g_mc_layout);
    snprintf(g_mc_name, sizeof g_mc_name, "%s", s->channels[ch].clips[sc].name);
    session_clip_mc_set_asked(s, ch, sc);
}

/* MULTI / STEREO at once; PATH: the analysis starts (applied when it is done) */
static void mc_apply(Session *s, int ch, int sc, int mode, int out)
{
    if (mode == CLIP_MC_PATH) {
        if (session_clip_mc_mode(s, ch, sc) == CLIP_MC_PATH) { g_mc_rc = 0; snprintf(g_mc_msg, sizeof g_mc_msg, "It already plays as a surround path"); return; }
        mc_mix_path(g_mc_orig, g_mc_mix, sizeof g_mc_mix);
        char rbuf[1024];
        g_mc_job = mc_job_start(wav_readable_all_channels(g_mc_orig, s->sample_rate > 0 ? s->sample_rate : 48000, rbuf, sizeof rbuf), g_mc_mix);
        if (!g_mc_job) { g_mc_rc = 1; snprintf(g_mc_msg, sizeof g_mc_msg, "Cannot start the analysis"); }
        return;
    }
    if (session_clip_set_mc(s, ch, sc, mode, NULL, NULL) != 0) {
        g_mc_rc = 1;
        snprintf(g_mc_msg, sizeof g_mc_msg, mode == CLIP_MC_MULTI ? "Cannot play this file in multichannel" : "Cannot load the file again");
        return;
    }
    if (mode == CLIP_MC_MULTI) session_clip_set_mc_out(s, ch, sc, out);
    undo_poke(GetTime());
    g_mc_rc = 0;
}

/* the analysis is done: the clip plays the mix through the path; the channel gets the file's speakers if it has none */
static void mc_path_done(Session *s)
{
    McResult *r = malloc(sizeof *r);
    int rc = r ? mc_job_result(g_mc_job, r) : 1;
    if (rc != 0) {
        g_mc_rc = 1;
        snprintf(g_mc_msg, sizeof g_mc_msg, "%s", r ? mc_job_error(g_mc_job) : "Out of memory");
    } else if (session_clip_set_mc(s, g_mc_ch, g_mc_sc, CLIP_MC_PATH, g_mc_mix, g_mc_orig) != 0) {
        g_mc_rc = 1;
        snprintf(g_mc_msg, sizeof g_mc_msg, "Cannot load the stereo mix");
    } else {
        SurSetup su;
        bool had = session_get_surround(s, g_mc_ch, &su);
        if (!had || su.mode != SUR_MODE_CIRCLE) {
            memset(&su, 0, sizeof su);
            sur_layout(&su, r->layout, 0);
            su.width = 0;
            session_set_surround(s, g_mc_ch, &su);
        }
        session_clip_set_sur_points(s, g_mc_ch, g_mc_sc, r->pts, r->npts);
        undo_poke(GetTime());
        g_mc_rc = 0;
        snprintf(g_mc_msg, sizeof g_mc_msg, "Done: %d points%s. Select the clip and press Surround to see or change them.", r->npts,
                 had ? "" : " (the channel got the file's speakers)");
    }
    free(r);
    mc_job_free(g_mc_job);
    g_mc_job = NULL;
}

static void draw_mc(Session *s, int sw, int sh)
{
    GfxDialogBackdrop(70);
    int w = 720, h = 470;
    if (g_mc_import && session_mc_find_new(s, NULL, NULL)) h += 34;
    int x = (sw - w) / 2, y = (sh - h) / 2;
    draw_frame(x, y, w, h, "Surround File", g_mc_t);
    int lx = x + 22, cy = y + 56;
    char line[400];
    snprintf(line, sizeof line, "\"%s\" has %s.", g_mc_name, mc_layout_text(g_mc_nch, g_mc_layout));
    while (ui_text_w(line, 16) > w - 44 && strlen(line) > 10) line[strlen(line) - 4] = 0;
    ui_text(line, lx, cy, 16, g_theme.text);
    cy += 26;
    ui_text(g_mc_import ? "How should it play?" : "How it plays:", lx, cy, 14, g_theme.text_dim);
    cy += 30;
    bool busy = g_mc_job != NULL;
    int total = audio_total_outputs();
    const char *title[3] = { "Multichannel", "As a surround path", "Stereo" };
    const int mode_of[3] = { CLIP_MC_MULTI, CLIP_MC_PATH, CLIP_MC_STEREO };
    for (int i = 0; i < 3; ++i) {
        int m = mode_of[i];
        Rectangle r = { (float)lx, (float)cy, (float)(w - 44), 70.0f };
        bool on = g_mc_sel == m;
        bool hov = !busy && CheckCollisionPointRec(GetMousePosition(), r);
        DrawRectangleRec(r, on ? lighten(g_theme.panel_bg2, 26) : lighten(g_theme.panel_bg2, hov ? 14 : 4));
        DrawRectangleLinesEx(r, on ? 2 : 1, on ? g_theme.selection : g_theme.text_dim);
        DrawCircleLines(lx + 18, cy + 18, 7, on ? g_theme.selection : g_theme.text_dim);
        if (on) DrawCircleV((Vector2){ (float)(lx + 18), (float)(cy + 18) }, 4, g_theme.selection);
        ui_text(title[i], lx + 34, cy + 9, 16, g_theme.text);
        if (m == CLIP_MC_MULTI) {
            snprintf(line, sizeof line, "Every channel on its own output: outputs %d-%d%s", g_mc_out + 1, g_mc_out + g_mc_nch,
                     g_mc_out + g_mc_nch > total ? " (the card has fewer)" : "");
            ui_text(line, lx + 34, cy + 32, 13, g_mc_out + g_mc_nch > total ? (Color){ 230, 150, 80, 255 } : g_theme.text_dim);
            ui_text("No warp / pitch. The channel's fader still works.", lx + 34, cy + 49, 13, g_theme.text_dim);
            if (on && !busy) {                                    /* the first output */
                int bx = x + w - 22 - 190;
                ui_text("First output", bx, cy + 9, 13, g_theme.text_dim);
                if (seg_button(bx + 86, cy + 4, 30, "-", false, g_mc_out > 0)) g_mc_out--;
                snprintf(line, sizeof line, "%d", g_mc_out + 1);
                ui_text(line, bx + 122 + (30 - ui_text_w(line, 15)) / 2, cy + 10, 15, g_theme.text);
                if (seg_button(bx + 156, cy + 4, 30, "+", false, g_mc_out + 1 < MAX_HW_OUT)) g_mc_out++;
            }
        } else if (m == CLIP_MC_PATH) {
            ui_text("Mixed to stereo; Q-LiM finds where the sound moves and makes it the clip's", lx + 34, cy + 32, 13, g_theme.text_dim);
            ui_text("surround path - edit it like any other. An estimate. The original is kept.", lx + 34, cy + 49, 13, g_theme.text_dim);
        } else {
            ui_text("Only the first two channels, as before.", lx + 34, cy + 32, 13, g_theme.text_dim);
        }
        Rectangle click = r;
        if (m == CLIP_MC_MULTI && on) click.width -= 200;          /* (the output buttons) */
        if (hov && CheckCollisionPointRec(GetMousePosition(), click) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { g_mc_sel = m; g_mc_rc = -1; }
        cy += 80;
    }
    if (g_mc_import && session_mc_find_new(s, NULL, NULL)) {
        Rectangle cb = { (float)lx, (float)cy + 2, 18, 18 };
        DrawRectangleLinesEx(cb, 1, g_theme.text_dim);
        if (g_mc_all) DrawRectangle(lx + 4, cy + 6, 10, 10, g_theme.selection);
        ui_text("The same for the other surround files just added", lx + 28, cy + 3, 14, g_theme.text);
        if (!busy && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), (Rectangle){ (float)lx, (float)cy, 420, 24 })) g_mc_all = !g_mc_all;
        cy += 34;
    }
    /* progress / result */
    if (busy) {
        float p = mc_job_progress(g_mc_job);
        Rectangle bar = { (float)lx, (float)cy, (float)(w - 44), 22.0f };
        DrawRectangleRec(bar, lighten(g_theme.panel_bg2, 6));
        DrawRectangle(lx, cy, (int)((w - 44) * p), 22, g_theme.selection);
        DrawRectangleLinesEx(bar, 1, g_theme.text_dim);
        snprintf(line, sizeof line, "Finding where the sound moves... %d%%", (int)(p * 100.0f));
        ui_text(line, lx + 8, cy + 3, 14, g_theme.text);
        if (mc_job_done(g_mc_job)) mc_path_done(s);
    } else if (g_mc_rc >= 0 && g_mc_msg[0]) {
        snprintf(line, sizeof line, "%s", g_mc_msg);
        while (ui_text_w(line, 14) > w - 44 && strlen(line) > 10) line[strlen(line) - 4] = 0;
        ui_text(line, lx, cy + 3, 14, g_mc_rc == 0 ? g_theme.text : (Color){ 230, 90, 80, 255 });
    }
    int by = y + h - 52;
    if (busy) {
        if (button(x + w - 22 - 110, by, 110, 34, "Cancel", false) || IsKeyPressed(KEY_ESCAPE)) mc_job_cancel(g_mc_job);
        return;
    }
    bool done = g_mc_rc == 0;
    if (session_clip_mc_mode(s, g_mc_ch, g_mc_sc) == CLIP_MC_MULTI && button(lx, by, 150, 34, "Speaker Map...", false)) {   /* 2.0 */
        g_mc_open = false;
        ui_open_speaker_map(s, g_mc_ch, g_mc_sc);
        return;
    }
    if (button(x + w - 22 - 110 - 12 - 110, by, 110, 34, done ? "Close" : "Cancel", false) || IsKeyPressed(KEY_ESCAPE)) {
        g_mc_open = false;
        return;
    }
    bool apply = !done && (button(x + w - 22 - 110, by, 110, 34, "OK", true) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER));
    if (apply) {
        g_mc_rc = -1; g_mc_msg[0] = 0;
        g_mc_last_choice = g_mc_all ? g_mc_sel : -1;
        g_mc_last_out = g_mc_out;
        bool same = g_mc_sel == session_clip_mc_mode(s, g_mc_ch, g_mc_sc) &&
                    (g_mc_sel != CLIP_MC_MULTI || g_mc_out == session_clip_mc_out(s, g_mc_ch, g_mc_sc));
        if (same) { g_mc_open = false; return; }
        if (g_mc_sel == CLIP_MC_MULTI && session_clip_mc_mode(s, g_mc_ch, g_mc_sc) == CLIP_MC_MULTI) {
            session_clip_set_mc_out(s, g_mc_ch, g_mc_sc, g_mc_out);    /* only the outputs */
            undo_poke(GetTime());
            g_mc_open = false;
            return;
        }
        mc_apply(s, g_mc_ch, g_mc_sc, g_mc_sel, g_mc_out);
        if (g_mc_rc == 0 && g_mc_sel != CLIP_MC_PATH) g_mc_open = false;   /* (a path shows its result) */
    }
}

/* every frame: a surround file was just added -> ask (or do what was chosen "for the others") */
static void mc_poll(Session *s)
{
    if (g_mc_open || dialogs_any_open()) return;
    int ch, sc;
    if (!session_mc_find_new(s, &ch, &sc)) { g_mc_last_choice = -1; return; }
    if (g_mc_last_choice >= 0 && g_mc_last_choice != CLIP_MC_PATH) {
        session_clip_mc_set_asked(s, ch, sc);
        if (g_mc_last_choice == CLIP_MC_MULTI) {
            if (session_clip_set_mc(s, ch, sc, CLIP_MC_MULTI, NULL, NULL) == 0) session_clip_set_mc_out(s, ch, sc, g_mc_last_out);
        }
        return;
    }
    ui_open_mc(s, ch, sc, true);
    if (g_mc_last_choice == CLIP_MC_PATH) {                  /* the analysis, one file after the other */
        g_mc_sel = CLIP_MC_PATH;
        mc_apply(s, ch, sc, CLIP_MC_PATH, 0);
    }
}

void dialogs_poll(Session *s) { if (s) mc_poll(s); }

/* ============ 2.0: the Speaker Map - every surround signal wired to any output (as the PipeWire graph) ============
 * Left: the signals of a channel - the channels of its multichannel clip (L R C LFE Ls Rs ...) or its surround
 * speakers. Right: every output, by sound card (the main card, the extra ones - Bluetooth, the headphones ...).
 * Drag from a signal to an output (or back) = a wire; again on the same pair = it is removed; right click a wire =
 * removed. A signal can go to several outputs; several signals to one output add up. */

static bool   g_sm_open = false;
static double g_sm_t = 0.0;
static int    g_sm_ch = -1, g_sm_sc = -1;
/* g_sm_src: */                  /* 0 = the clip's file channels, 1 = the channel's surround speakers */
static int    g_sm_drag_side = -1, g_sm_drag_i = -1;   /* dragging a new wire from (0 = a signal, 1 = an output) */
static float  g_sm_scroll = 0.0f;            /* the outputs list */

static bool sm_has_clip(Session *s) { return s && g_sm_sc >= 0 && session_clip_mc_mode(s, g_sm_ch, g_sm_sc) == CLIP_MC_MULTI; }
static bool sm_has_sur(Session *s) { SurSetup su; return s && session_get_surround(s, g_sm_ch, &su) && su.n > 0; }

void ui_open_speaker_map(Session *s, int ch, int sc)
{
    if (!s) return;
    if (ch < 0 || ch >= s->channel_count) {                    /* nothing selected: the first channel that has one */
        ch = -1;
        for (int c = 0; c < s->channel_count && ch < 0; ++c) {
            SurSetup su;
            if (session_get_surround(s, c, &su) && su.n > 0) ch = c;
            for (int q = 0; q < s->scene_count && ch < 0; ++q) if (session_clip_mc_mode(s, c, q) == CLIP_MC_MULTI) { ch = c; sc = q; }
        }
        if (ch < 0) { ui_flash_message("No channel plays in surround: open Surround on a channel or load a surround file"); return; }
    }
    g_sm_open = true; g_sm_t = GetTime();
    g_sm_ch = ch; g_sm_sc = sc;
    g_sm_drag_side = -1; g_sm_scroll = 0.0f;
    g_sm_src = sm_has_clip(s) ? 0 : 1;
}

/* the signals of the current source: names, masks, default outputs */
static int sm_signals(Session *s, char names[][24], uint64_t *mask, int *def)
{
    if (g_sm_src == 0 && sm_has_clip(s)) {
        int n = session_clip_mc_channels(s, g_sm_ch, g_sm_sc);
        if (n > STREAM_MAX_CH) n = STREAM_MAX_CH;
        int first = session_clip_mc_out(s, g_sm_ch, g_sm_sc);
        for (int k = 0; k < n; ++k) {
            snprintf(names[k], 24, "%s", mc_channel_name(n, k));
            mask[k] = session_clip_mc_map(s, g_sm_ch, g_sm_sc, k);
            def[k] = first + k;
        }
        return n;
    }
    SurSetup su;
    if (!session_get_surround(s, g_sm_ch, &su)) return 0;
    for (int k = 0; k < su.n; ++k) {
        sur_speaker_label(&su, k, names[k], 24);
        mask[k] = su.omask[k];
        def[k] = su.out[k];
    }
    return su.n;
}

static bool sm_connected(uint64_t mask, int def, int o)
{
    if (mask == 0) return o == def;
    if (mask & SUR_MUTED) return false;
    return o >= 0 && o < 62 && (mask & (1ull << o));
}

/* a wire on / off */
static void sm_toggle(Session *s, int k, int o)
{
    char names[SUR_MAX_SPK][24];
    uint64_t mask[SUR_MAX_SPK];
    int def[SUR_MAX_SPK];
    int n = sm_signals(s, names, mask, def);
    if (k < 0 || k >= n || o < 0 || o >= 62) return;
    uint64_t set = 0;                                          /* the outputs it plays on now */
    if (mask[k] == 0) set = def[k] >= 0 && def[k] < 62 ? 1ull << def[k] : 0;
    else if (!(mask[k] & SUR_MUTED)) set = mask[k];
    set ^= 1ull << o;
    uint64_t nm = set == 0 ? SUR_MUTED : (def[k] >= 0 && def[k] < 62 && set == (1ull << def[k])) ? 0 : set;
    if (g_sm_src == 0 && sm_has_clip(s)) session_clip_set_mc_map(s, g_sm_ch, g_sm_sc, k, nm);
    else {
        SurSetup su;
        if (!session_get_surround(s, g_sm_ch, &su)) return;
        su.omask[k] = nm;
        session_set_surround(s, g_sm_ch, &su);
    }
    undo_poke(GetTime());
}

static void sm_reset(Session *s)
{
    if (g_sm_src == 0 && sm_has_clip(s)) { for (int k = 0; k < STREAM_MAX_CH; ++k) session_clip_set_mc_map(s, g_sm_ch, g_sm_sc, k, 0); }
    else {
        SurSetup su;
        if (!session_get_surround(s, g_sm_ch, &su)) return;
        memset(su.omask, 0, sizeof su.omask);
        session_set_surround(s, g_sm_ch, &su);
    }
    undo_poke(GetTime());
}

static Vector2 sm_bez(Vector2 a, Vector2 b, float t)
{
    float dx = (b.x - a.x) * 0.5f;
    Vector2 c1 = { a.x + dx, a.y }, c2 = { b.x - dx, b.y };
    float u = 1 - t;
    return (Vector2){ u * u * u * a.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * b.x,
                      u * u * u * a.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * b.y };
}
static float sm_wire(Vector2 a, Vector2 b, Color c, float th, Vector2 m)
{
    float best = 1e9f;
    Vector2 p = a;
    for (int i = 1; i <= 24; ++i) {
        Vector2 q = sm_bez(a, b, (float)i / 24.0f);
        DrawLineEx(p, q, th, c);
        float dx = m.x - q.x, dy = m.y - q.y, d = dx * dx + dy * dy;
        if (d < best) best = d;
        p = q;
    }
    return sqrtf(best);
}

static void draw_speaker_map(Session *s, int sw, int sh)
{
    GfxDialogBackdrop(70);
    int w = 920, h = 660;
    if (h > sh - 20) h = sh - 20;
    if (w > sw - 20) w = sw - 20;
    int x = (sw - w) / 2, y = (sh - h) / 2;
    draw_frame(x, y, w, h, "Speaker Map", g_sm_t);
    Vector2 m = GetMousePosition();
    if (g_sm_ch < 0 || g_sm_ch >= s->channel_count) { g_sm_open = false; return; }
    bool hc = sm_has_clip(s), hs = sm_has_sur(s);
    if (g_sm_src == 0 && !hc) g_sm_src = 1;
    if (g_sm_src == 1 && !hs && hc) g_sm_src = 0;
    int lx = x + 22, cy = y + 52;
    /* the channel (< >) and the source */
    char line[300];
    if (button(lx, cy, 30, 28, "<", false)) {
        for (int c = g_sm_ch - 1; c >= 0; --c) {
            SurSetup su; bool ok = session_get_surround(s, c, &su) && su.n > 0;
            int q2 = -1;
            for (int q = 0; q < s->scene_count && q2 < 0; ++q) if (session_clip_mc_mode(s, c, q) == CLIP_MC_MULTI) q2 = q;
            if (ok || q2 >= 0) { g_sm_ch = c; g_sm_sc = q2; g_sm_src = q2 >= 0 ? 0 : 1; break; }
        }
    }
    snprintf(line, sizeof line, "%s", s->channels[g_sm_ch].name);
    while (ui_text_w(line, 16) > 230 && strlen(line) > 4) line[strlen(line) - 1] = 0;
    ui_text(line, lx + 40, cy + 5, 16, g_theme.text);
    if (button(lx + 280, cy, 30, 28, ">", false)) {
        for (int c = g_sm_ch + 1; c < s->channel_count; ++c) {
            SurSetup su; bool ok = session_get_surround(s, c, &su) && su.n > 0;
            int q2 = -1;
            for (int q = 0; q < s->scene_count && q2 < 0; ++q) if (session_clip_mc_mode(s, c, q) == CLIP_MC_MULTI) q2 = q;
            if (ok || q2 >= 0) { g_sm_ch = c; g_sm_sc = q2; g_sm_src = q2 >= 0 ? 0 : 1; break; }
        }
    }
    int bx = lx + 330;
    if (hc) {
        snprintf(line, sizeof line, "Surround file \"%s\"", s->channels[g_sm_ch].clips[g_sm_sc].name);
        while (ui_text_w(line, 13) > 240 && strlen(line) > 8) line[strlen(line) - 1] = 0;
        int bw = ui_text_w(line, 13) + 22;
        if (seg_button(bx, cy, bw, line, g_sm_src == 0, true)) g_sm_src = 0;
        bx += bw + 6;
    }
    if (hs) {
        const char *t = "Surround speakers";
        int bw = ui_text_w(t, 13) + 22;
        if (seg_button(bx, cy, bw, t, g_sm_src == 1, true)) g_sm_src = 1;
    }
    cy += 38;
    ui_text("Drag from a signal to an output to connect them (again = disconnect). One signal can go to several outputs.",
            lx, cy, 13, g_theme.text_dim);
    ui_text("Right click a wire to remove it. Sound cards: Edit > Sound Cards. Their delays: Routing.", lx, cy + 17, 13, g_theme.text_dim);
    cy += 44;
    if (!hc && !hs) {
        ui_text("This channel does not play in surround.", lx, cy + 10, 15, g_theme.text);
        if (button(x + w - 22 - 110, y + h - 52, 110, 34, "Close", true) || IsKeyPressed(KEY_ESCAPE)) g_sm_open = false;
        return;
    }
    char names[SUR_MAX_SPK][24];
    uint64_t mask[SUR_MAX_SPK];
    int def[SUR_MAX_SPK];
    int n = sm_signals(s, names, mask, def);
    /* the two columns */
    int top = cy, bot = y + h - 64;
    int sx0 = lx, sx1 = lx + 200;                  /* the signals */
    int ox0 = x + w - 22 - 280, ox1 = x + w - 22;  /* the outputs */
    DrawRectangle(sx0, top, sx1 - sx0, bot - top, lighten(g_theme.panel_bg2, 2));
    DrawRectangle(ox0, top, ox1 - ox0, bot - top, lighten(g_theme.panel_bg2, 2));
    ui_text(g_sm_src == 0 ? "File channels" : "Speakers", sx0 + 10, top + 6, 13, g_theme.text_dim);
    ui_text("Outputs", ox0 + 26, top + 6, 13, g_theme.text_dim);
    int rh = 26;
    float sig_y[SUR_MAX_SPK];
    int sig_hov = -1;
    for (int k = 0; k < n; ++k) {
        float yy = (float)(top + 30 + k * rh);
        sig_y[k] = yy + rh / 2.0f;
        Rectangle r = { (float)sx0 + 6, yy + 2, (float)(sx1 - sx0 - 12), (float)rh - 4 };
        bool hv = CheckCollisionPointRec(m, (Rectangle){ r.x, yy, (float)(sx1 - sx0) + 4, (float)rh });   /* (the dot too) */
        if (hv) sig_hov = k;
        DrawRectangleRec(r, hv || (g_sm_drag_side == 0 && g_sm_drag_i == k) ? lighten(g_theme.panel_bg2, 30) : lighten(g_theme.panel_bg2, 12));
        bool muted = mask[k] & SUR_MUTED;
        ui_text(names[k], (int)r.x + 8, (int)r.y + 4, 14, muted ? g_theme.text_dim : g_theme.text);
        if (muted) ui_text("(silent)", (int)r.x + 70, (int)r.y + 5, 12, g_theme.text_dim);
        DrawCircleV((Vector2){ (float)sx1 - 4, sig_y[k] }, 5, g_theme.selection);
    }
    /* the outputs, by card (scrolls) */
    AudioOutGroup grp[1 + OUTX_MAX];
    int ngrp = audio_output_groups(grp, 1 + OUTX_MAX);
    int total = audio_total_outputs();
    if (total < 2) total = 2;
    int out_hov = -1;
    float oy_of[62];
    for (int o = 0; o < 62; ++o) oy_of[o] = -1;
    int list_h = 0;
    for (int gi = 0; gi < ngrp; ++gi) if (grp[gi].first >= 0) list_h += 22 + grp[gi].count * rh;
    /* 2.1: wired outputs no sound card has (e.g. a 5.1 file on a stereo card): their own rows at the end, so the wires
     * end somewhere you can see (before, they piled up in the corner) */
    int miss[62], nmiss = 0;
    for (int o = total; o < 62; ++o) {
        bool used = false;
        for (int k = 0; k < n && !used; ++k) used = sm_connected(mask[k], def[k], o);
        if (used) miss[nmiss++] = o;
    }
    if (nmiss) list_h += 22 + nmiss * rh;
    int view_h = bot - top - 30;
    if (CheckCollisionPointRec(m, (Rectangle){ (float)ox0, (float)top, (float)(ox1 - ox0), (float)(bot - top) })) g_sm_scroll -= GetMouseWheelMove() * 40.0f;
    if (g_sm_scroll > list_h - view_h) g_sm_scroll = (float)(list_h - view_h);
    if (g_sm_scroll < 0) g_sm_scroll = 0;
    BeginScissorMode(ox0, top + 26, ox1 - ox0, view_h + 4);
    float yy = (float)(top + 30) - g_sm_scroll;
    for (int gi = 0; gi < ngrp; ++gi) {
        AudioOutGroup *g = &grp[gi];
        if (g->first < 0) continue;
        char gn[200];
        snprintf(gn, sizeof gn, "%s%s", gi == 0 ? "Main: " : "", g->name);
        while (ui_text_w(gn, 13) > ox1 - ox0 - 30 && strlen(gn) > 6) gn[strlen(gn) - 1] = 0;
        Color sc = gi == 0 ? g_theme.selection : g->state == 0 ? (Color){ 110, 200, 130, 255 } : (Color){ 230, 150, 80, 255 };
        DrawRectangle(ox0 + 4, (int)yy + 4, 3, 14, sc);
        ui_text(gn, ox0 + 12, (int)yy + 3, 13, g_theme.text);
        yy += 22;
        for (int q = 0; q < g->count; ++q) {
            int o = g->first + q;
            if (o >= 62) break;
            Rectangle r = { (float)ox0 + 6, yy + 2, (float)(ox1 - ox0 - 12), (float)rh - 4 };
            bool vis = r.y + r.height > top + 26 && r.y < bot;
            bool hv = vis && CheckCollisionPointRec(m, (Rectangle){ (float)ox0 - 6, yy, (float)(ox1 - ox0) + 6, (float)rh });
            if (hv) out_hov = o;
            DrawRectangleRec(r, hv || (g_sm_drag_side == 1 && g_sm_drag_i == o) ? lighten(g_theme.panel_bg2, 30) : lighten(g_theme.panel_bg2, 12));
            char ol[48];
            if (g->count == 2) snprintf(ol, sizeof ol, "Output %d (%s)", o + 1, q == 0 ? "L" : "R");
            else snprintf(ol, sizeof ol, "Output %d", o + 1);
            ui_text(ol, (int)r.x + 24, (int)r.y + 4, 14, g_theme.text);
            oy_of[o] = yy + rh / 2.0f;
            DrawCircleV((Vector2){ (float)ox0 + 4, oy_of[o] }, 5, g_theme.selection);
            yy += rh;
        }
    }
    if (nmiss) {
        DrawRectangle(ox0 + 4, (int)yy + 4, 3, 14, (Color){ 230, 150, 80, 255 });
        ui_text("Not on any sound card (silent)", ox0 + 12, (int)yy + 3, 13, (Color){ 230, 150, 80, 255 });
        yy += 22;
        for (int q = 0; q < nmiss; ++q) {
            int o = miss[q];
            Rectangle r = { (float)ox0 + 6, yy + 2, (float)(ox1 - ox0 - 12), (float)rh - 4 };
            DrawRectangleRec(r, lighten(g_theme.panel_bg2, 6));
            char ol[48];
            snprintf(ol, sizeof ol, "Output %d", o + 1);
            ui_text(ol, (int)r.x + 24, (int)r.y + 4, 14, g_theme.text_dim);
            oy_of[o] = yy + rh / 2.0f;
            DrawCircleV((Vector2){ (float)ox0 + 4, oy_of[o] }, 5, (Color){ 230, 150, 80, 255 });
            yy += rh;
        }
    }
    EndScissorMode();
    /* the wires */
    int wire_hov_k = -1, wire_hov_o = -1;
    float wbest = 6.0f;
    for (int pass = 0; pass < 2; ++pass)
        for (int k = 0; k < n; ++k)
            for (int o = 0; o < 62; ++o) {
                if (!sm_connected(mask[k], def[k], o)) continue;
                Vector2 a = { (float)sx1 - 4, sig_y[k] };
                float oy = oy_of[o] >= 0 ? oy_of[o] : -1.0f;
                bool off = oy < top + 26 || oy > bot;                 /* scrolled away / no such output */
                if (oy < 0) oy = (float)bot - 6;
                if (off) oy = oy < top + 26 ? (float)top + 28 : (float)bot - 4;
                Vector2 b = { (float)ox0 + 4, oy };
                bool hl = (k == wire_hov_k && o == wire_hov_o);
                if (pass == 0) {
                    Color c = ui_channel_color(s->channels[g_sm_ch].color_id);
                    if (off || oy_of[o] < 0) c.a = 110;
                    float d = sm_wire(a, b, c, 2.5f, m);
                    if (d < wbest && sig_hov < 0 && out_hov < 0 && g_sm_drag_side < 0) { wbest = d; wire_hov_k = k; wire_hov_o = o; }
                } else if (hl) sm_wire(a, b, RAYWHITE, 3.5f, m);
            }
    if (wire_hov_k >= 0) {
        snprintf(line, sizeof line, "%s -> Output %d   (right click: remove)", names[wire_hov_k], wire_hov_o + 1);
        int tw = ui_text_w(line, 12);
        DrawRectangle((int)m.x + 12, (int)m.y + 10, tw + 12, 18, g_theme.panel_bg);
        ui_text(line, (int)m.x + 18, (int)m.y + 12, 12, g_theme.text);
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) sm_toggle(s, wire_hov_k, wire_hov_o);
    }
    /* a new wire */
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (sig_hov >= 0) { g_sm_drag_side = 0; g_sm_drag_i = sig_hov; }
        else if (out_hov >= 0) { g_sm_drag_side = 1; g_sm_drag_i = out_hov; }
    }
    if (g_sm_drag_side >= 0) {
        Vector2 a = g_sm_drag_side == 0 ? (Vector2){ (float)sx1 - 4, sig_y[g_sm_drag_i < n ? g_sm_drag_i : 0] }
                                        : (Vector2){ (float)ox0 + 4, oy_of[g_sm_drag_i] >= 0 ? oy_of[g_sm_drag_i] : m.y };
        Color c = g_theme.selection;
        if (g_sm_drag_side == 0) sm_wire(a, m, c, 2.0f, m); else sm_wire(m, a, c, 2.0f, m);
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            if (g_sm_drag_side == 0 && out_hov >= 0) sm_toggle(s, g_sm_drag_i, out_hov);
            else if (g_sm_drag_side == 1 && sig_hov >= 0) sm_toggle(s, sig_hov, g_sm_drag_i);
            g_sm_drag_side = -1;
        }
    }
    /* the buttons */
    int by = y + h - 52;
    if (button(lx, by, 150, 34, "Default", false)) sm_reset(s);
    ui_text(g_sm_src == 0 ? "Default: the file's channels on the outputs in order (Surround File... sets the first)"
                          : "Default: every speaker on the output set in the Surround window", lx + 164, by + 9, 13, g_theme.text_dim);
    if (button(x + w - 22 - 110, by, 110, 34, "Close", true) || (IsKeyPressed(KEY_ESCAPE) && g_sm_drag_side < 0)) g_sm_open = false;
}

/* ============ entry point from ui_draw ============ */

bool dialogs_any_open(void) { return g_sm_open || g_mc_open || g_routing_open || g_sc_open || g_q_open || g_cp_open || g_ex_open || g_sp_open || g_ss_open || g_sch_open || g_sd_open || g_cards_open; }

void dialogs_draw(Session *s, int sw, int sh)
{
    /* priority: the close question is on top */
    if (g_q_open) draw_quit(sw, sh);
    else if (g_mc_open) draw_mc(s, sw, sh);                       /* 1.4.24 */
    else if (g_sm_open) draw_speaker_map(s, sw, sh);              /* 2.0 */
    else if (g_cp_open) draw_color_picker(s, sw, sh);
    else if (g_ex_open) draw_export(s, sw, sh);
    else if (g_sp_open) draw_separate(s, sw, sh);
    else if (g_ss_open) draw_sep_settings(sw, sh);
    else if (g_sch_open) draw_schedule(s, sw, sh);
    else if (g_sd_open) draw_surround(s, sw, sh);
    else if (g_cards_open) draw_sound_cards(sw, sh);             /* 1.4.21 (over Routing) */
    else if (g_routing_open) draw_routing(s, sw, sh);
    else if (g_sc_open) draw_shortcuts(sw, sh);
}
