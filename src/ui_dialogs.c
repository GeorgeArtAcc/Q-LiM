/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#include "ui_dialogs.h"
#include "ui.h"
#include "audio.h"
#include "theme.h"
#include "sdlgfx.h"
#include <stdio.h>
#include <string.h>

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
bool ui_routing_is_open(void) { return g_routing_open; }

static int hw_outputs(void)
{
    int n = audio_get_current_config().output_channels;
    if (n < 2) n = 2;
    if (n > MAX_HW_OUT) n = MAX_HW_OUT;
    return n;
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
    if (dlg_w < 640) dlg_w = 640;
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
    if (vis_hw < n_hw)
        snprintf(info, sizeof info, "Sound card outputs: %d   (showing %d-%d - Shift + mouse wheel or the bar below to scroll)",
                 n_hw, hcol + 1, hcol + vis_hw);
    else
        snprintf(info, sizeof info, "Sound card outputs: %d   (detected from your audio interface)", n_hw);
    ui_text(info, mx0, dy + 48, 13, g_theme.text_dim);
    ui_text("Click or drag over the squares to connect.",
            mx0, dy + 66, 13, g_theme.text_dim);

    /* column headers */
    int hy = dy + 92;
    ui_text("MASTER IN", COL_X(0), hy, 11, g_theme.text_dim);
    ui_text("SOUND CARD OUT", COL_X(2 + hcol), hy, 11, g_theme.text_dim);
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
    int list_top = my + 2 * R_ROW_H + 14;
    int list_bot = dy + dlg_h - 64;
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
    if (button(dx + dlg_w - 120, by, 100, 32, "Close", true)) g_routing_open = false;

    if (IsKeyPressed(KEY_ESCAPE)) g_routing_open = false;
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
};

#define G(t, a) { t, a, (int)(sizeof(a) / sizeof((a)[0])) }
static const ScGroup SC_GROUPS_L[] = { G("Playback", SC_PLAY), G("File", SC_FILE), G("Edit", SC_EDIT) };
static const ScGroup SC_GROUPS_R[] = { G("View", SC_VIEW), G("MIDI", SC_MIDI), G("Waveform", SC_WAVE) };

static bool   g_sc_open = false;
static double g_sc_t = 0.0;
static float  g_sc_scroll = 0.0f;

void ui_open_shortcuts(void) { g_sc_open = true; g_sc_t = GetTime(); g_sc_scroll = 0.0f; }
bool ui_shortcuts_is_open(void) { return g_sc_open; }

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
bool ui_color_picker_is_open(void) { return g_cp_open; }

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

/* ============ entry point from ui_draw ============ */

bool dialogs_any_open(void) { return g_routing_open || g_sc_open || g_q_open || g_cp_open; }

void dialogs_draw(Session *s, int sw, int sh)
{
    /* priority: the close question is on top */
    if (g_q_open) draw_quit(sw, sh);
    else if (g_cp_open) draw_color_picker(s, sw, sh);
    else if (g_routing_open) draw_routing(s, sw, sh);
    else if (g_sc_open) draw_shortcuts(sw, sh);
}
