/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 2.1: the MIDI editor (piano roll) - in place of the waveform when a MIDI clip is selected. Moved out of ui.c. */
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

/* ===== 1.3: the MIDI editor (piano roll) - in place of the waveform when a MIDI clip is selected =====
 * Left: the keys (click = hear the note). Top: the ruler with the bars; the triangle at the end = the clip length (drag).
 * Grid: double click = add a note / delete a note; drag a note = move (time + pitch); drag its right edge = length;
 * drag on empty space = select a box; Shift = add to the selection. Bottom: the velocity of each note (drag up/down).
 * Keys (after a click in the editor): Delete, arrows (up/down = pitch, Shift = octave; left/right = one grid step),
 * Ctrl+A = all. Mouse wheel = up/down, Shift+wheel = sideways, Ctrl+wheel = zoom. */
#define ME_KW   46
#define ME_RH   16
static const float ME_GRIDS[] = { 1.0f, 0.5f, 0.25f, 0.125f, 0.0f };
static const char *ME_GRID_NAMES[] = { "1/4", "1/8", "1/16", "1/32", "Off" };
#define ME_NGRID 5
enum { MD_NONE = 0, MD_MOVE, MD_RESIZE, MD_BOX, MD_VEL, MD_LEN };
static int     g_me_ch = -1, g_me_sc = -1;
static double  g_me_b0 = 0.0;
static float   g_me_ppb = 60.0f, g_me_kh = 11.0f;
static int     g_me_top = 84, g_me_grid = 2;
bool    g_me_focus = false;
static Rectangle g_me_rect;
static int     g_md = MD_NONE;
static Vector2 g_md_p0;
static MidiSeq *g_md_orig = NULL;
static int     g_md_note = -1, g_md_lastdk = 0;
static double  g_me_click_t = 0;
static Vector2 g_me_click_p;
static float   g_me_lastlen = 0.25f;
static bool    g_me_pencil = false;        /* 2.0: the pencil: a click on empty space = a note (as long as the last one) */
static bool    g_md_copy = false;          /* 1.3.2: Ctrl + drag = copies of the notes are moved, the originals stay */
static float   g_me_pcur = -1.0f;          /* 1.3.2: where Ctrl+V pastes (beat; set by a click on empty space), <0 = none */
static MNote  *g_me_cb = NULL;             /* 1.3.2: the copied notes (starts relative to the first one) */
static int     g_me_cbn = 0;
static float   g_me_cb_span = 0.0f;
static int     g_me_pv_key = -1, g_me_pv_ch = -1;
static double  g_me_pv_until = 0;

static float me_grid(void) { return ME_GRIDS[g_me_grid]; }
static float me_snap_floor(float b) { float g = me_grid(); return g > 0 ? floorf(b / g + 1e-4f) * g : b; }
/* 1.3.2: the grid step really drawn at this zoom (coarser when zoomed out), 0 = grid off */
static float me_vis_step(void)
{
    float g = me_grid();
    if (g <= 0.0f) return 0.0f;
    while (g * g_me_ppb < 5.0f) g *= 2.0f;
    return g;
}
/* 1.3.2: a "magnet" like in other DAWs: a time near a grid line (closer than ME_MAGNET_PX on the screen) sticks
 * exactly to it; otherwise it goes freely - so zoomed out it always lands on the grid, zoomed in it moves finely
 * (to 1/960 of a beat). free_move (Shift) = no magnet. */
#define ME_MAGNET_PX 12.0f
static float me_magnet(float b, bool free_move)
{
    float g = me_vis_step();
    if (!free_move && g > 0.0f) {
        float n = roundf(b / g) * g;
        if (fabsf(n - b) * g_me_ppb <= ME_MAGNET_PX) return n;
    }
    return roundf(b * 960.0f) / 960.0f;
}
/* the grid lines: the theme's grid colour, pushed away from the background so the fine lines can be seen */
static Color me_grid_color(void)
{
    Color c = g_theme.text_dim, bg = g_theme.wave_bg;
    return (Color){ (unsigned char)((c.r + bg.r) / 2), (unsigned char)((c.g + bg.g) / 2), (unsigned char)((c.b + bg.b) / 2), 255 };
}
static bool  me_black(int k) { int n = k % 12; return n == 1 || n == 3 || n == 6 || n == 8 || n == 10; }

static void me_preview_off(void)
{
    if (g_me_pv_key >= 0) midiq_push_to(MIDIQ_UI, g_me_pv_ch, 0x80, (unsigned char)g_me_pv_key, 0);
    g_me_pv_key = -1;
}
static void me_preview(int ch, int key, int vel)
{
    me_preview_off();
    if (key < 0 || key > 127) return;
    midiq_push_to(MIDIQ_UI, ch, 0x90, (unsigned char)key, (unsigned char)(vel > 0 ? vel : 100));
    g_me_pv_key = key; g_me_pv_ch = ch; g_me_pv_until = GetTime() + 0.25;
}

static void me_publish(Session *s, MidiSeq *m) { session_midi_set(s, g_me_ch, g_me_sc, m); }

/* the selection as a new list (the old one is still read by the audio thread) */
static void me_select(Session *s, const MidiSeq *cur, int only, bool clear_others, bool on)
{
    MidiSeq *m = midiseq_dup(cur, 0);
    if (!m) return;
    for (int i = 0; i < m->n; ++i) {
        if (i == only) m->note[i].sel = on;
        else if (clear_others) m->note[i].sel = 0;
    }
    me_publish(s, m);
}

static int me_selected(const MidiSeq *m) { int n = 0; for (int i = 0; m && i < m->n; ++i) n += m->note[i].sel != 0; return n; }

static int   g_me_vzoom_req = 0;           /* 2.1: -1 / +1 = a step of the note height, 2 = fit (done with the layout) */
static float g_me_fit_len = 0.0f;          /* 2.0: > 0 = fit this many beats (a recording: what is recorded so far) */
static void me_fit(Session *s, float gw, float gh)
{
    const MidiSeq *m = session_midi_get(s, g_me_ch, g_me_sc);
    float L = g_me_fit_len > 0.0f ? g_me_fit_len : m ? m->length : 4.0f;
    g_me_b0 = 0.0;
    g_me_ppb = (gw - 8) / (L > 1.0f ? L : 1.0f);
    if (g_me_ppb > 240.0f) g_me_ppb = 240.0f;
    int lo = 127, hi = 0;
    for (int i = 0; m && i < m->n; ++i) { if (m->note[i].key < lo) lo = m->note[i].key; if (m->note[i].key > hi) hi = m->note[i].key; }
    int vis = (int)(gh / g_me_kh);
    int mid = (m && m->n) ? (lo + hi) / 2 : 60;
    g_me_top = mid + vis / 2;
    if (g_me_top > 127) g_me_top = 127;
    if (g_me_top - vis + 1 < 0) g_me_top = vis - 1 < 127 ? vis - 1 : 127;
}

/* 2.1: the note height (vertical zoom), kept so the key under `ay` (screen y; < 0 = the middle) stays where it is */
static void me_set_kh(float kh, float gy, float gh, float ay)
{
    if (kh < 4.0f) kh = 4.0f;
    if (kh > 40.0f) kh = 40.0f;
    if (ay < gy || ay > gy + gh) ay = gy + gh * 0.5f;
    float key = (float)g_me_top - (ay - gy) / g_me_kh;          /* the (fractional) key under the anchor */
    g_me_kh = kh;
    g_me_top = (int)lroundf(key + (ay - gy) / g_me_kh);
}
/* 2.1: "Fit" - all the clip's notes fill the height (an empty clip: two octaves around C3) */
static void me_fit_keys(Session *s, float gh)
{
    const MidiSeq *m = session_midi_get(s, g_me_ch, g_me_sc);
    int lo = 127, hi = 0;
    for (int i = 0; m && i < m->n; ++i) { if (m->note[i].key < lo) lo = m->note[i].key; if (m->note[i].key > hi) hi = m->note[i].key; }
    if (!m || m->n == 0) { lo = 48; hi = 72; }
    int span = hi - lo + 1 + 4;                                     /* two keys of air above and below */
    if (span < 12) { int add = 12 - span; lo -= add / 2; hi += add - add / 2; span = 12; }
    float kh = gh / (float)span;
    g_me_kh = kh < 4.0f ? 4.0f : kh > 40.0f ? 40.0f : kh;
    int vis = (int)(gh / g_me_kh);
    g_me_top = (lo + hi) / 2 + vis / 2;
    if (g_me_top > 127) g_me_top = 127;
    if (g_me_top - vis + 1 < 0) g_me_top = vis - 1 < 127 ? vis - 1 : 127;
}

/* arrows: called by main.c before it moves the clip selection; true = the editor used them */
bool ui_midi_take_arrows(Session *s)
{
    if (!g_me_focus || g_me_ch < 0 || !session_is_midi(s, g_me_ch, g_me_sc)) return false;
    const MidiSeq *cur = session_midi_get(s, g_me_ch, g_me_sc);
    bool up = IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP), dn = IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN);
    bool lf = IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT), rt = IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT);
    if (!(up || dn || lf || rt)) return me_selected(cur) > 0;
    if (!me_selected(cur)) return false;
    bool sh = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    int dk = up ? (sh ? 12 : 1) : dn ? (sh ? -12 : -1) : 0;
    float g = me_grid() > 0 ? me_grid() : 0.25f;
    float db = lf ? -g : rt ? g : 0.0f;
    MidiSeq *m = midiseq_dup(cur, 0);
    if (!m) return true;
    int pk = -1;
    for (int i = 0; i < m->n; ++i) {
        MNote *q = &m->note[i];
        if (!q->sel) continue;
        int k = q->key + dk;
        if (k < 0 || k > 127) { free(m); return true; }
        float st = q->start + db;
        if (st < 0.0f || st >= m->length) { free(m); return true; }
        q->key = (unsigned char)k; q->start = st;
        pk = k;
    }
    me_publish(s, m);
    if (dk && pk >= 0) me_preview(g_me_ch, pk, 100);
    return true;
}

/* Delete: called through ui_edit_take_delete */
bool me_take_delete(Session *s)
{
    if (!g_me_focus || g_me_ch < 0 || !session_is_midi(s, g_me_ch, g_me_sc)) return false;
    const MidiSeq *cur = session_midi_get(s, g_me_ch, g_me_sc);
    if (!me_selected(cur)) return false;
    MidiSeq *m = midiseq_dup(cur, 0);
    if (!m) return true;
    int w = 0;
    for (int i = 0; i < m->n; ++i) if (!m->note[i].sel) m->note[w++] = m->note[i];
    m->n = w;
    me_publish(s, m);
    return true;
}

/* 1.3.2: Ctrl+C / Ctrl+X of the selected notes (main.c asks through ui_edit_take_copy) */
bool me_take_copy(Session *s, bool cut)
{
    if (!g_me_focus || g_me_ch < 0 || !session_is_midi(s, g_me_ch, g_me_sc)) return false;
    const MidiSeq *cur = session_midi_get(s, g_me_ch, g_me_sc);
    int n = me_selected(cur);
    if (!n) return false;
    MNote *cb = malloc(sizeof(MNote) * (size_t)n);
    if (!cb) return true;
    float st = 1e9f, en = 0.0f;
    int k = 0;
    for (int i = 0; i < cur->n; ++i) if (cur->note[i].sel) {
        cb[k++] = cur->note[i];
        if (cur->note[i].start < st) st = cur->note[i].start;
        if (cur->note[i].start + cur->note[i].len > en) en = cur->note[i].start + cur->note[i].len;
    }
    for (int i = 0; i < k; ++i) { cb[i].start -= st; cb[i].sel = 1; }
    free(g_me_cb); g_me_cb = cb; g_me_cbn = k;
    /* the length of the copied block, rounded up to the grid (so pasting again continues right after it) */
    float g = me_grid() > 0.0f ? me_grid() : 0.25f;
    g_me_cb_span = ceilf((en - st) / g - 1e-4f) * g;
    if (g_me_cb_span < g) g_me_cb_span = g;
    if (g_me_pcur < 0.0f) g_me_pcur = st + g_me_cb_span;    /* no place chosen yet: right after the copied notes */
    if (cut) { me_take_delete(s); ui_flash_message("Notes cut - click where to put them, Ctrl+V"); }
    else ui_flash_message("Notes copied - click where to put them, Ctrl+V");
    return true;
}

/* 1.3.2: Ctrl+V - the copied notes at the paste place (the last click on empty space), same pitches */
bool me_take_paste(Session *s)
{
    if (!g_me_focus || g_me_ch < 0 || !session_is_midi(s, g_me_ch, g_me_sc) || !g_me_cb || g_me_cbn <= 0) return false;
    const MidiSeq *cur = session_midi_get(s, g_me_ch, g_me_sc);
    if (!cur) return false;
    if (cur->n + g_me_cbn > MIDI_MAX_NOTES) { ui_flash_message("Too many notes in this clip"); return true; }
    float at = g_me_pcur >= 0.0f ? g_me_pcur : 0.0f;
    MidiSeq *m = midiseq_dup(cur, g_me_cbn);
    if (!m) return true;
    for (int i = 0; i < m->n; ++i) m->note[i].sel = 0;
    for (int i = 0; i < g_me_cbn; ++i) { m->note[m->n] = g_me_cb[i]; m->note[m->n].start += at; m->note[m->n].sel = 1; m->n++; }
    me_publish(s, m);
    g_me_pcur = at + g_me_cb_span;                          /* Ctrl+V again = the next copy right after it */
    return true;
}

/* 1.3.2: quantize - the starts of the selected notes (all, if none is selected) to the grid (1/16 if it is off) */
static void me_quantize(Session *s)
{
    const MidiSeq *cur = session_midi_get(s, g_me_ch, g_me_sc);
    if (!cur || cur->n == 0) return;
    float g = me_grid() > 0.0f ? me_grid() : 0.25f;
    bool only_sel = me_selected(cur) > 0;
    MidiSeq *m = midiseq_dup(cur, 0);
    if (!m) return;
    int moved = 0;
    for (int i = 0; i < m->n; ++i) {
        MNote *q = &m->note[i];
        if (only_sel && !q->sel) continue;
        float ns = roundf(q->start / g) * g;
        if (ns < 0.0f) ns = 0.0f;
        if (fabsf(ns - q->start) > 1e-5f) ++moved;
        q->start = ns;
    }
    me_publish(s, m);
    char msg[96];
    snprintf(msg, sizeof msg, "Quantized %s to %s (%d moved)", only_sel ? "the selected notes" : "all notes",
             ME_GRID_NAMES[me_grid() > 0.0f ? g_me_grid : 2], moved);
    ui_flash_message(msg);
}

static bool me_hdr_button(Rectangle r, const char *label, bool on, bool enabled)
{
    Vector2 m = GetMousePosition();
    bool hov = enabled && fx_ui_free() && CheckCollisionPointRec(m, r);
    if (on) DrawRectangleRounded(r, R_SMALL, g_theme.selection);
    else DrawRectangleRoundedLines(r, R_SMALL, 1, hov ? g_theme.text : g_theme.grid_line);
    int tw = ui_text_w(label, 12);
    ui_text(label, (int)(r.x + (r.width - tw) / 2), (int)(r.y + (r.height - 12) / 2), 12,
            on ? g_theme.bg : (!enabled ? g_theme.grid_line : hov ? g_theme.text : g_theme.text_dim));
    return hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

void draw_midi_editor(Session *s, int ch, int sc, float x, float y, float w, float h)
{
    Clip *cl = &s->channels[ch].clips[sc];
    Color col = channel_color(s->channels[ch].color_id);
    Vector2 mp = GetMousePosition();
    bool uf = fx_ui_free();
    if (g_me_pv_key >= 0 && GetTime() > g_me_pv_until && !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) me_preview_off();

    float by = y + WAVE_HDR_H, bh = h - WAVE_HDR_H;
    float vh = bh > 150 ? 36.0f : 0.0f;
    float gx = x + ME_KW, gy = by + ME_RH, gw = w - ME_KW, gh = bh - ME_RH - vh;
    g_me_rect = (Rectangle){ x, y, w, h };
    if (ch != g_me_ch || sc != g_me_sc) {                  /* another clip: fit it into view */
        if (g_waveform_h < 260.0f) {                       /* the piano roll needs room: the panel grows (once per clip) */
            g_waveform_h = 260.0f;
            return;                                         /* the layout of this frame used the old height */
        }
        g_me_ch = ch; g_me_sc = sc;
        g_me_pcur = -1.0f;
        g_md = MD_NONE; free(g_md_orig); g_md_orig = NULL;
        me_fit(s, gw, gh);
    }
    const MidiSeq *m = session_midi_get(s, ch, sc);
    if (!m) return;
    float L = m->length;
    /* 2.0: being recorded: the view shows what is recorded so far (16 beats more at a time), a red line where it is */
    double rec_b = 0.0;
    int rec_st = session_rec_state(s, ch, sc, &rec_b);
    {
        static float rec_view = 0.0f;
        if (rec_st != REC_IDLE) {
            float want = 16.0f * ceilf(((float)rec_b + 2.0f) / 16.0f);
            if (want < 16.0f) want = 16.0f;
            if (fabsf(want - rec_view) > 0.01f) { rec_view = want; g_me_fit_len = want; me_fit(s, gw, gh); g_me_fit_len = 0.0f; }
        } else rec_view = 0.0f;
    }
    {   /* the length changed from outside (a recording ended, undo): fit the view again */
        static float lastL = -1.0f;
        if (lastL >= 0.0f && fabsf(L - lastL) > 1e-3f && g_md != MD_LEN &&
            ((L - g_me_b0) * g_me_ppb > gw - 8 || L * g_me_ppb < (gw - 8) * 0.5f)) me_fit(s, gw, gh);
        lastL = L;
    }

    /* ---- the header ---- */
    DrawRectangle((int)x, (int)y, (int)w, WAVE_HDR_H, g_theme.panel_bg);
    char hdr[400];
    snprintf(hdr, sizeof hdr, "%s  |  MIDI  |  %d note%s  |  Ch %d / S%d", cl->name, m->n, m->n == 1 ? "" : "s", ch + 1, sc + 1);
    ui_text(hdr, (int)x + 10, (int)y + 4, 14, g_theme.text);
    float hx = x + 10 + ui_text_w(hdr, 14) + 18;
    if (rec_st != REC_IDLE) {                              /* 2.0: recording: REC and Stop */
        Color red = { 220, 55, 50, 255 };
        Rectangle sbr = { hx, y + 2, 92, WAVE_HDR_H - 4 };
        bool hv = uf && CheckCollisionPointRec(mp, sbr);
        DrawRectangleRec(sbr, hv ? (Color){ 240, 80, 70, 255 } : red);
        DrawRectangle((int)sbr.x + 9, (int)(sbr.y + sbr.height / 2 - 4), 8, 8, RAYWHITE);
        ui_text("REC  Stop", (int)sbr.x + 24, (int)sbr.y + 3, 13, RAYWHITE);
        if (hv && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ui_rec_click(s, ch, sc);
        hx += sbr.width + 10;
    }
    {   /* 2.0: the tools - the arrow and the pencil (B; Esc = the arrow) */
        static const char *tips[2] = { "Pointer: select, move, resize notes (double click = add / delete)",
                                       "Pencil (B): click on empty space = a note as long as the last one, drag = its length" };
        for (int i = 0; i < 2; ++i) {
            Rectangle r = { hx + i * 26, y + 2, 24, WAVE_HDR_H - 4 };
            bool hov = uf && CheckCollisionPointRec(mp, r);
            bool on = (i == 1) == g_me_pencil;
            DrawRectangleRec(r, on ? g_theme.selection : (hov ? g_theme.cell_hover : g_theme.panel_bg2));
            Color ic = on ? g_theme.bg : g_theme.text;
            if (i == 0) icon_pointer(r.x + 12, r.y + r.height / 2, ic); else icon_pencil(r.x + 12, r.y + r.height / 2, ic);
            if (hov) {
                g_tool_tip = tips[i]; g_tool_tip_x = x + w - 8; g_tool_tip_y = y + WAVE_HDR_H + 2;
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_me_pencil = i == 1;
            }
        }
        hx += 2 * 26 + 10;
    }
    {   /* 2.1: the note height (vertical zoom): - / + / Fit; also Ctrl+Shift+wheel, or Ctrl+wheel on the keys */
        static const char *lab[3] = { "-", "+", "Fit" };
        static const char *tips[3] = { "Lower notes (see more keys)  -  Ctrl+Shift+wheel, or Ctrl+wheel on the keys",
                                       "Taller notes  -  Ctrl+Shift+wheel, or Ctrl+wheel on the keys",
                                       "Fit: all the notes of the clip fill the height" };
        ui_text("Height", (int)hx, (int)y + 5, 11, g_theme.text_dim);
        hx += ui_text_w("Height", 11) + 6;
        for (int i = 0; i < 3; ++i) {
            Rectangle r = { hx, y + 2, i == 2 ? 34.0f : 22.0f, WAVE_HDR_H - 4 };
            bool hov = uf && CheckCollisionPointRec(mp, r);
            DrawRectangleRec(r, hov ? g_theme.cell_hover : g_theme.panel_bg2);
            DrawRectangleLinesEx(r, 1, g_theme.text_dim);
            ui_text(lab[i], (int)(r.x + (r.width - ui_text_w(lab[i], 12)) / 2), (int)r.y + 3, 12, g_theme.text);
            if (hov) {
                g_tool_tip = tips[i]; g_tool_tip_x = x + w - 8; g_tool_tip_y = y + WAVE_HDR_H + 2;
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_me_vzoom_req = i == 0 ? -1 : i == 1 ? 1 : 2;
            }
            hx += r.width + 3;
        }
        hx += 8;
    }
    char gl[32]; snprintf(gl, sizeof gl, "Grid %s", ME_GRID_NAMES[g_me_grid]);
    Rectangle gr = { hx, y + 2, (float)ui_text_w("Grid 1/32", 12) + 16, WAVE_HDR_H - 4 };
    if (me_hdr_button(gr, gl, false, true)) g_me_grid = (g_me_grid + 1) % ME_NGRID;
    if (uf && CheckCollisionPointRec(mp, gr) && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) g_me_grid = (g_me_grid + ME_NGRID - 1) % ME_NGRID;
    hx += gr.width + 10;
    char ll[48];
    fmt_bars(ll, sizeof ll, L);
    /* 1.3.1: the length is only shown here - it is changed by dragging the triangle in the ruler */
    int llw = ui_text_w(ll, 12);
    ui_text(ll, (int)hx, (int)y + 5, 12, g_theme.text_dim);
    hx += llw + 10;
    Rectangle dbl = { hx, y + 2, 30, WAVE_HDR_H - 4 };
    bool dup = me_hdr_button(dbl, "x2", false, L <= 512.0f);
    hx += dbl.width + 8;
    bool loop_on = atomic_load(&cl->loop);
    Rectangle lr = { hx, y + 2, 46, WAVE_HDR_H - 4 };
    if (me_hdr_button(lr, "Loop", loop_on, true)) atomic_store(&cl->loop, !loop_on);
    hx += lr.width + 8;
    Rectangle qzr = { hx, y + 2, (float)ui_text_w("Quantize", 12) + 16, WAVE_HDR_H - 4 };
    if (me_hdr_button(qzr, "Quantize", false, m->n > 0)) { g_me_focus = true; me_quantize(s); m = session_midi_get(s, ch, sc); }
    hx += qzr.width + 14;
    if (dup) {                                             /* x2: twice as long, the notes repeated */
        MidiSeq *n2 = midiseq_dup(m, m->n);
        if (n2) {
            for (int i = 0; i < m->n; ++i) { n2->note[m->n + i] = m->note[i]; n2->note[m->n + i].start += L; n2->note[m->n + i].sel = 0; }
            n2->n = m->n * 2;
            n2->length = L * 2.0f;
            me_publish(s, n2);
            m = session_midi_get(s, ch, sc); L = m->length;
            me_fit(s, gw, gh);
        }
    }
    if (!fxchain_has_instrument(s, ch)) {
        const char *warn = "No instrument on this channel: click the channel name and press + to add one";
        ui_text(warn, (int)hx, (int)y + 5, 12, FX_FAIL_COL);
    } else if (w - (hx - x) > 420) {
        const char *hint = g_me_pencil ? "Pencil: click = a note, drag = its length   Shift = no magnet   Esc / B: the arrow"
                                       : "Double click: add / delete   Ctrl+drag: copy   Ruler: drag = zoom/move, double click = play";
        int hw = ui_text_w(hint, 11);
        ui_text(hint, (int)(x + w - hw - 10), (int)y + 6, 11, g_theme.text_dim);
    }

    /* ---- focus (the keys go to the editor after a click in it) ---- */
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
        g_me_focus = uf && CheckCollisionPointRec(mp, (Rectangle){ x, by, w, bh });

    /* ---- the view: wheel ---- */
    Rectangle grid_r = { gx, gy, gw, gh }, keys_r = { x, gy, ME_KW, gh };
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (uf && (CheckCollisionPointRec(mp, grid_r) || CheckCollisionPointRec(mp, keys_r) ||
                 CheckCollisionPointRec(mp, (Rectangle){ gx, by, gw, ME_RH }))) {
        float wh = GetMouseWheelMove();
        if (wh != 0.0f) {
            if (ctrl && (shift || CheckCollisionPointRec(mp, keys_r))) {   /* the note height (2.1: + Ctrl+Shift anywhere) */
                me_set_kh(g_me_kh * (wh > 0 ? 1.15f : 1.0f / 1.15f), gy, gh, mp.y);
            } else if (ctrl) {
                double bm = g_me_b0 + (mp.x - gx) / g_me_ppb;
                g_me_ppb *= wh > 0 ? 1.2f : 1.0f / 1.2f;
                if (g_me_ppb < 4) g_me_ppb = 4;
                if (g_me_ppb > 800) g_me_ppb = 800;
                g_me_b0 = bm - (mp.x - gx) / g_me_ppb;
            } else if (shift) {
                g_me_b0 -= wh * 40.0 / g_me_ppb;
            } else {
                g_me_top += wh > 0 ? 3 : -3;
            }
        }
    }
    if (g_me_vzoom_req) {                                         /* 2.1: the header buttons */
        if (g_me_vzoom_req == 2) me_fit_keys(s, gh);
        else me_set_kh(g_me_kh * (g_me_vzoom_req > 0 ? 1.25f : 1.0f / 1.25f), gy, gh, -1.0f);
        g_me_vzoom_req = 0;
    }
    int vis = (int)(gh / g_me_kh) + 1;
    if (g_me_top > 127) g_me_top = 127;
    if (g_me_top - vis + 2 < 0) g_me_top = vis - 2 > 127 ? 127 : vis - 2;
    double maxb0 = (double)L + 4.0 - (gw - 10) / g_me_ppb;
    if (g_me_b0 > maxb0) g_me_b0 = maxb0;
    if (g_me_b0 < 0) g_me_b0 = 0;
#define BX(b) (gx + (float)(((double)(b) - g_me_b0) * g_me_ppb))
#define KY(k) (gy + (float)(g_me_top - (k)) * g_me_kh)

    /* ---- the grid ---- */
    DrawRectangle((int)x, (int)by, (int)w, (int)bh, g_theme.wave_bg);
    BeginScissorMode((int)gx, (int)gy, (int)gw, (int)gh);
    for (int k = g_me_top; k >= 0 && KY(k) < gy + gh; --k) {
        Color rc = g_theme.wave_bg;
        if (me_black(k)) {                                 /* the rows of the black keys: a little darker/lighter */
            int lum = (rc.r + rc.g + rc.b) / 3, d = lum > 128 ? -14 : 9;
            rc.r = (unsigned char)(rc.r + d); rc.g = (unsigned char)(rc.g + d); rc.b = (unsigned char)(rc.b + d);
        }
        DrawRectangle((int)gx, (int)KY(k), (int)gw, (int)ceilf(g_me_kh), rc);
        if (k % 12 == 0) DrawLine((int)gx, (int)(KY(k) + g_me_kh), (int)(gx + gw), (int)(KY(k) + g_me_kh), g_theme.grid_line);
    }
    /* 1.3.1: every second bar a little darker, and the lines of the chosen grid clearly visible (before, the 1/8 ..
     * 1/32 lines were so faint that changing the grid looked like it did nothing). "Off" = beats only. */
    const double SB = sig_bar();                       /* 1.4.8: the bars of the time signature */
    for (double b = floor(g_me_b0 / (2.0 * SB)) * 2.0 * SB + SB; BX(b) < gx + gw; b += 2.0 * SB) {
        if (b < 0) continue;
        float x0 = BX(b), x1 = BX(b + SB);
        if (x0 < gx) x0 = gx;
        if (x1 > gx + gw) x1 = gx + gw;
        if (x1 > x0) DrawRectangle((int)x0, (int)gy, (int)(x1 - x0), (int)gh, (Color){ 0, 0, 0, 22 });
    }
    float gstep = me_grid() > 0 ? me_grid() : 1.0f;
    while (gstep * g_me_ppb < 5.0f) gstep *= 2.0f;
    Color glc = me_grid_color();
    double first = floor(g_me_b0 / gstep) * gstep;
    for (double b = first; BX(b) < gx + gw; b += gstep) {
        if (b < 0) continue;
        bool bar = fmod(b + 1e-6, sig_bar()) < 2e-6, beat = fmod(b + 1e-6, sig_unit()) < 2e-6;
        Color lc = glc;
        lc.a = bar ? 255 : beat ? 170 : 110;
        DrawLine((int)BX(b), (int)gy, (int)BX(b), (int)(gy + gh), lc);
        if (bar) DrawLine((int)BX(b) + 1, (int)gy, (int)BX(b) + 1, (int)(gy + gh), lc);
    }
    if (BX(L) < gx + gw) DrawRectangle((int)BX(L), (int)gy, (int)(gx + gw - BX(L)) + 1, (int)gh, (Color){ 0, 0, 0, 110 });
    if (g_me_pcur >= 0.0f && BX(g_me_pcur) >= gx && BX(g_me_pcur) < gx + gw) {   /* 1.3.2: where Ctrl+V pastes */
        Color pc = g_theme.selection; pc.a = 170;
        DrawLine((int)BX(g_me_pcur), (int)gy, (int)BX(g_me_pcur), (int)(gy + gh), pc);
    }
    /* the notes */
    int hover_note = -1; bool hover_edge = false;
    for (int i = 0; i < m->n; ++i) {
        const MNote *q = &m->note[i];
        float nx = BX(q->start), nw = q->len * g_me_ppb, ny = KY(q->key);
        if (nx > gx + gw || nx + nw < gx || ny > gy + gh || ny + g_me_kh < gy) continue;
        if (nw < 3) nw = 3;
        Rectangle r = { nx, ny + 1, nw - 1, g_me_kh - 2 };
        float a = 0.45f + 0.55f * q->vel / 127.0f;
        Color c = col;
        c.r = (unsigned char)(c.r * a); c.g = (unsigned char)(c.g * a); c.b = (unsigned char)(c.b * a);
        if (q->start >= L) c.a = 90;
        DrawRectangleRec(r, c);
        DrawRectangleLinesEx(r, 1, q->sel ? g_theme.text : (Color){ (unsigned char)(col.r / 2), (unsigned char)(col.g / 2), (unsigned char)(col.b / 2), 255 });
        if (q->sel) DrawRectangleLinesEx((Rectangle){ r.x - 1, r.y - 1, r.width + 2, r.height + 2 }, 1, g_theme.text);
        if (uf && g_md == MD_NONE && CheckCollisionPointRec(mp, r)) {
            hover_note = i;
            hover_edge = r.width > 10 && mp.x > r.x + r.width - 6;
        }
    }
    /* the playhead */
    double ph = session_midi_playhead(s, ch, sc);
    if (ph >= 0.0 && BX(ph) >= gx && BX(ph) <= gx + gw) DrawLine((int)BX(ph), (int)gy, (int)BX(ph), (int)(gy + gh), g_theme.text);
    if (g_md == MD_BOX) {
        Rectangle br = { fminf(g_md_p0.x, mp.x), fminf(g_md_p0.y, mp.y), fabsf(mp.x - g_md_p0.x), fabsf(mp.y - g_md_p0.y) };
        Color bc = g_theme.selection; bc.a = 50;
        DrawRectangleRec(br, bc);
        DrawRectangleLinesEx(br, 1, g_theme.selection);
    }
    EndScissorMode();
    if (hover_edge) SetMouseCursor(MOUSE_CURSOR_RESIZE_EW);

    /* ---- the ruler ---- */
    DrawRectangle((int)gx, (int)by, (int)gw, ME_RH, g_theme.panel_bg);
    BeginScissorMode((int)gx, (int)by, (int)gw, ME_RH);
    /* the numbers: bars ("3"), and when zoomed in also beats ("3.2") and sixteenths ("3.2.3") */
    /* 1.4.8: the step: quarters of a beat .. a bar (what fits), then whole bars; it starts again at every bar */
    const double RB = sig_bar();
    double bstep = sig_unit() / 4.0;
    while (bstep * g_me_ppb < (bstep < RB ? 40.0 : 34.0)) {
        double nx = bstep * 2.0;
        if (bstep < RB - 1e-9 && nx > RB + 1e-9) nx = RB;
        bstep = nx;
    }
    double rb0 = floor(g_me_b0 / RB) * RB;
    if (bstep >= RB - 1e-9) rb0 = floor(g_me_b0 / bstep) * bstep;
    for (double b = rb0, bar0 = rb0; BX(b) < gx + gw; ) {
        double here = b;
        if (bstep >= RB - 1e-9) b += bstep;
        else { b += bstep; if (b > bar0 + RB - 1e-9) { bar0 += RB; b = bar0; } }   /* the next bar starts on its 1 */
        if (here < 0) continue;
        double bsave = b; b = here;
        /* 1.4.8: in the bars / beats of the time signature; the 3rd number = quarters of a beat */
        double SBr = sig_bar(), SU = sig_unit(), qu = SU / 4.0;
        long bi = (long)floor(b / SBr + 1e-9);
        double in = b - (double)bi * SBr;
        long qi_ = lround(in / qu);
        int bar = (int)bi + 1, bt = (int)(qi_ / 4) + 1, sx = (int)(qi_ % 4) + 1;
        bool on_bar = fabs(in) < 1e-6 || fabs(in - SBr) < 1e-6, on_beat = fabs(in / SU - floor(in / SU + 0.5)) < 1e-6;
        bool on_q = fabs(in / qu - floor(in / qu + 0.5)) < 1e-6;
        if (!on_bar && !on_beat && !on_q) { b = bsave; continue; }   /* (should not happen any more) */
        char bn[24];
        if (on_bar) snprintf(bn, sizeof bn, "%d", fabs(in - SBr) < 1e-6 ? bar + 1 : bar);
        else if (on_beat) snprintf(bn, sizeof bn, "%d.%d", bar, bt);
        else snprintf(bn, sizeof bn, "%d.%d.%d", bar, bt, sx);
        bool major = on_bar;
        DrawLine((int)BX(b), (int)by + (major ? 8 : 11), (int)BX(b), (int)by + ME_RH, g_theme.text_dim);
        ui_text(bn, (int)BX(b) + 3, (int)by + 2, 10, major ? g_theme.text : g_theme.text_dim);
        b = bsave;
    }
    {   /* the end of the clip: a triangle to drag */
        float ex = BX(L);
        bool eh = uf && fabsf(mp.x - ex) < 7 && mp.y >= by && mp.y < by + ME_RH;
        Color ec = (eh || g_md == MD_LEN) ? g_theme.text : g_theme.selection;
        DrawTriangle((Vector2){ ex - 7, by + 2 }, (Vector2){ ex, by + ME_RH }, (Vector2){ ex, by + 2 }, ec);
        if (eh) SetMouseCursor(MOUSE_CURSOR_RESIZE_EW);
        if (eh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { g_md = MD_LEN; g_md_p0 = mp; }
        /* 1.3.1: like the waveform - drag on the ruler (or the middle button anywhere in the notes): up/down = zoom
         * around the place grabbed, left/right = move; double click on the ruler = play the clip from there */
        static int    mz_btn = -1;
        static float  mz_y0, mz_lastx, mz_ppb0, mz_rel;
        static Vector2 mz_vm, mz_p0;                             /* 1.3.3: virtual mouse (the cursor is hidden) */
        static bool   mz_fresh = false;
        static double mz_anchor, mz_pan, rc_t = -10.0;
        static float  rc_x = -1000.0f;
        bool on_ruler = uf && !eh && g_md == MD_NONE && mp.x >= gx && mp.x < gx + gw && mp.y >= by && mp.y < by + ME_RH;
        bool lp = on_ruler && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        bool mpress = uf && g_md == MD_NONE && CheckCollisionPointRec(mp, (Rectangle){ gx, by, gw, bh }) && IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE);
        if (on_ruler) SetMouseCursor(MOUSE_CURSOR_RESIZE_EW);
        if (lp) {
            double now = GetTime();
            if (now - rc_t < DOUBLE_CLICK_TIME && fabsf(mp.x - rc_x) < 6.0f) {
                double b = g_me_b0 + (mp.x - gx) / g_me_ppb;
                if (b >= 0.0 && b < L) session_midi_play_from(s, ch, sc, b);
                rc_t = -10.0;
            } else { rc_t = now; rc_x = mp.x; }
        }
        if (mz_btn < 0 && (lp || mpress)) {
            mz_btn = lp ? MOUSE_BUTTON_LEFT : MOUSE_BUTTON_MIDDLE;
            mz_vm = mp; mz_p0 = mp; mz_fresh = true;
            mz_y0 = mp.y; mz_lastx = mp.x; mz_ppb0 = g_me_ppb;
            mz_rel = mp.x - gx;
            mz_anchor = g_me_b0 + mz_rel / g_me_ppb;
            mz_pan = 0.0;
        }
        if (mz_btn >= 0) {
            Vector2 md = GetMouseDelta();
            if (!mz_fresh && IsMouseButtonDown(mz_btn)) {
                mz_vm.x += md.x; mz_vm.y += md.y;
                if (fabsf(mz_vm.x - mz_p0.x) + fabsf(mz_vm.y - mz_p0.y) > 3.0f) GfxMouseRelative(true);
            }
            mz_fresh = false;
            Vector2 mp = mz_vm;                                  /* below: the virtual mouse */
            if (!IsMouseButtonDown(mz_btn)) { mz_btn = -1; GfxMouseRelative(false); }
            else {
                float np = mz_ppb0 * expf((mz_y0 - mp.y) / 90.0f);      /* up = zoom in */
                if (np < 4.0f) np = 4.0f;
                if (np > 800.0f) np = 800.0f;
                g_me_ppb = np;
                mz_pan -= (double)(mp.x - mz_lastx) / g_me_ppb;          /* left = the view moves right */
                mz_lastx = mp.x;
                double want = mz_anchor - mz_rel / g_me_ppb + mz_pan;
                double maxb = (double)L + 4.0 - (gw - 10) / g_me_ppb;
                double cl2 = want > maxb ? maxb : want;
                if (cl2 < 0.0) cl2 = 0.0;
                mz_pan += cl2 - want;
                g_me_b0 = cl2;
                SetMouseCursor(MOUSE_CURSOR_RESIZE_EW);
            }
        }
    }
    if (ph >= 0.0) DrawRectangle((int)BX(ph) - 1, (int)by, 3, ME_RH, g_theme.text);
    EndScissorMode();

    /* ---- the keys ---- */
    BeginScissorMode((int)x, (int)gy, ME_KW, (int)gh);
    for (int k = g_me_top; k >= 0 && KY(k) < gy + gh; --k) {
        bool blk = me_black(k);
        Rectangle kr = { x, KY(k), blk ? ME_KW * 0.62f : (float)ME_KW, g_me_kh };
        Color kc = blk ? (Color){ 30, 30, 34, 255 } : (Color){ 225, 225, 228, 255 };
        if (g_me_pv_key == k) kc = col;
        DrawRectangleRec(kr, kc);
        DrawLine((int)x, (int)(KY(k) + g_me_kh), (int)(x + ME_KW), (int)(KY(k) + g_me_kh), (Color){ 120, 120, 120, 255 });
        if (k % 12 == 0 && g_me_kh >= 8) {
            char kn[8]; snprintf(kn, sizeof kn, "C%d", k / 12 - 2);   /* 60 = C3, as in Ableton and Bitwig */
            ui_text(kn, (int)(x + ME_KW - ui_text_w(kn, 9) - 3), (int)(KY(k) + (g_me_kh - 9) / 2), 9, (Color){ 60, 60, 60, 255 });
        }
    }
    EndScissorMode();
    if (uf && CheckCollisionPointRec(mp, keys_r) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        int k = g_me_top - (int)((mp.y - gy) / g_me_kh);
        me_preview(ch, k, 100);
    }

    /* ---- the velocity lane ---- */
    if (vh > 0) {
        float vy = gy + gh;
        DrawRectangle((int)x, (int)vy, (int)w, (int)vh, g_theme.panel_bg);
        ui_text("Vel", (int)x + 6, (int)(vy + vh / 2 - 6), 11, g_theme.text_dim);
        BeginScissorMode((int)gx, (int)vy, (int)gw, (int)vh);
        for (int i = 0; i < m->n; ++i) {
            const MNote *q = &m->note[i];
            float nx = BX(q->start);
            if (nx < gx - 4 || nx > gx + gw) continue;
            float hh = (vh - 6) * q->vel / 127.0f;
            Color c = q->sel ? g_theme.text : col;
            DrawRectangle((int)nx, (int)(vy + vh - 3 - hh), 3, (int)hh, c);
            DrawRectangle((int)nx - 2, (int)(vy + vh - 3 - hh), 7, 2, c);
        }
        EndScissorMode();
        if (uf && g_md == MD_NONE && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            CheckCollisionPointRec(mp, (Rectangle){ gx, vy, gw, vh })) {
            int best = -1; float bd = 6;
            for (int i = 0; i < m->n; ++i) { float d = fabsf(BX(m->note[i].start) + 1 - mp.x); if (d < bd) { bd = d; best = i; } }
            if (best >= 0) {
                if (!m->note[best].sel) { me_select(s, m, best, !shift, true); m = session_midi_get(s, ch, sc); }
                free(g_md_orig); g_md_orig = midiseq_dup(m, 0);
                g_md = MD_VEL; g_md_p0 = mp; g_md_note = best;
            }
        }
    }

    /* ---- the mouse in the grid ---- */
    bool in_grid = uf && CheckCollisionPointRec(mp, grid_r);
    float mb = (float)(g_me_b0 + (mp.x - gx) / g_me_ppb);
    int mk = g_me_top - (int)floorf((mp.y - gy) / g_me_kh);
    if (rec_st == REC_ON || rec_st == REC_STOPPING) {      /* 2.0: where the recording is */
        float rx = BX(rec_b);
        if (rx >= gx && rx <= gx + gw) DrawRectangle((int)rx, (int)gy, 2, (int)gh, (Color){ 220, 55, 50, 255 });
    }
    /* 2.0: the pencil: Esc / B; a pale note where a click would put one (as long as the last placed) */
    if (g_me_focus && !g_ti_active && IsKeyPressed(KEY_B) && !ctrl) g_me_pencil = !g_me_pencil;
    if (g_me_pencil && g_me_focus && !g_ti_active && IsKeyPressed(KEY_ESCAPE)) g_me_pencil = false;
    if (g_me_pencil && in_grid && g_md == MD_NONE && hover_note < 0 && mk >= 0 && mk <= 127 && mb >= 0 && mb < L) {
        float st = shift ? roundf(mb * 960.0f) / 960.0f : me_magnet(me_snap_floor(mb), false);
        float ln = g_me_lastlen;
        if (st + ln > L) ln = L - st;
        if (ln < 1.0f / 64.0f) ln = 1.0f / 64.0f;
        BeginScissorMode((int)gx, (int)gy, (int)gw, (int)gh);
        Rectangle gn = { BX(st) + 1, KY(mk) + 1, (float)(ln * g_me_ppb) - 1, g_me_kh - 1 };
        if (gn.width < 3) gn.width = 3;
        DrawRectangleRec(gn, (Color){ col.r, col.g, col.b, 70 });
        DrawRectangleLinesEx(gn, 1, (Color){ col.r, col.g, col.b, 150 });
        EndScissorMode();
        SetMouseCursor(MOUSE_CURSOR_CROSSHAIR);
    }
    if (in_grid && g_md == MD_NONE && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        double tnow = GetTime();
        bool dbl = tnow - g_me_click_t < DOUBLE_CLICK_TIME && fabsf(mp.x - g_me_click_p.x) < 5 && fabsf(mp.y - g_me_click_p.y) < 5;
        g_me_click_t = dbl ? 0 : tnow; g_me_click_p = mp;
        if (dbl && hover_note >= 0) {                        /* double click on a note: delete it */
            MidiSeq *n2 = midiseq_dup(m, 0);
            if (n2) { memmove(&n2->note[hover_note], &n2->note[hover_note + 1], sizeof(MNote) * (size_t)(n2->n - hover_note - 1)); n2->n--; me_publish(s, n2); }
        } else if (dbl && mk >= 0 && mk <= 127 && mb >= 0 && mb < L) {   /* double click on empty space: a new note */
            MidiSeq *n2 = midiseq_dup(m, 1);
            if (n2) {
                for (int i = 0; i < n2->n; ++i) n2->note[i].sel = 0;
                MNote *q = &n2->note[n2->n++];
                q->start = me_snap_floor(mb); q->len = g_me_lastlen; q->key = (unsigned char)mk; q->vel = 100; q->sel = 1;
                if (q->start + q->len > L) q->len = L - q->start;
                if (q->len <= 0.0f) q->len = 0.0625f;
                me_publish(s, n2);
                me_preview(ch, mk, 100);
            }
        } else if (g_me_pencil && hover_note < 0 && mk >= 0 && mk <= 127 && mb >= 0 && mb < L) {   /* 2.0: the pencil */
            MidiSeq *n2 = midiseq_dup(m, 1);
            if (n2) {
                for (int i = 0; i < n2->n; ++i) n2->note[i].sel = 0;
                MNote *q = &n2->note[n2->n++];
                q->start = me_magnet(me_snap_floor(mb), shift); q->len = g_me_lastlen; q->key = (unsigned char)mk; q->vel = 100; q->sel = 1;
                if (shift) q->start = roundf(mb * 960.0f) / 960.0f;
                if (q->start + q->len > L) q->len = L - q->start;
                if (q->len <= 0.0f) q->len = 0.0625f;
                me_publish(s, n2);
                me_preview(ch, mk, 100);
                m = session_midi_get(s, ch, sc);
                free(g_md_orig); g_md_orig = m ? midiseq_dup(m, 0) : NULL;   /* dragging on = its length */
                g_md_note = -1;
                for (int i = 0; g_md_orig && i < g_md_orig->n; ++i) if (g_md_orig->note[i].sel) { g_md_note = i; break; }
                if (g_md_orig && g_md_note >= 0) {
                    g_md = MD_RESIZE; g_md_copy = false; g_md_lastdk = 0;
                    g_md_p0 = mp;
                }
                g_me_click_t = 0;
            }
        } else if (hover_note >= 0) {                        /* grab a note */
            bool was = m->note[hover_note].sel != 0;
            g_me_lastlen = m->note[hover_note].len;          /* 2.0: the pencil goes on with this length */
            if (shift && was) { me_select(s, m, hover_note, false, false); }
            else {
                if (!was) { me_select(s, m, hover_note, !shift, true); m = session_midi_get(s, ch, sc); }
                free(g_md_orig); g_md_orig = midiseq_dup(m, 0);
                g_md = hover_edge ? MD_RESIZE : MD_MOVE;
                g_md_copy = ctrl && g_md == MD_MOVE;             /* 1.3.2: Ctrl + drag = copy */
                g_md_p0 = mp; g_md_lastdk = 0;
                /* find the grabbed note again in the list (after the sort the index may differ) */
                g_md_note = -1;
                for (int i = 0; i < g_md_orig->n; ++i)
                    if (g_md_orig->note[i].sel && fabsf(BX(g_md_orig->note[i].start) - BX(m->note[hover_note < m->n ? hover_note : 0].start)) < 0.5f &&
                        g_md_orig->note[i].key == m->note[hover_note < m->n ? hover_note : 0].key) { g_md_note = i; break; }
                if (g_md_note >= 0 && g_md == MD_MOVE) me_preview(ch, g_md_orig->note[g_md_note].key, g_md_orig->note[g_md_note].vel);
            }
        } else {                                             /* empty space: a selection box */
            if (!shift && me_selected(m)) { me_select(s, m, -1, true, false); m = session_midi_get(s, ch, sc); }
            g_md = MD_BOX; g_md_p0 = mp;
            if (mb >= 0.0f) g_me_pcur = me_snap_floor(mb);      /* 1.3.2: Ctrl+V pastes here */
        }
    }

    /* ---- dragging ---- */
    if (g_md != MD_NONE) {
        bool down = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        if (g_md == MD_MOVE && g_md_orig) {
            /* 1.3.2: the grabbed note follows the mouse finely and sticks to a grid line near it (magnet) */
            float a0 = g_md_note >= 0 ? g_md_orig->note[g_md_note].start : 0.0f;
            float db = me_magnet(a0 + (mp.x - g_md_p0.x) / g_me_ppb, shift) - a0;
            int dk = (int)roundf((g_md_p0.y - mp.y) / g_me_kh);
            float minst = 1e9f; int mink = 127, maxk = 0;
            for (int i = 0; i < g_md_orig->n; ++i) if (g_md_orig->note[i].sel) {
                const MNote *q = &g_md_orig->note[i];
                if (q->start < minst) minst = q->start;
                if (q->key < mink) mink = q->key;
                if (q->key > maxk) maxk = q->key;
            }
            if (minst + db < 0) db = -minst;
            if (mink + dk < 0) dk = -mink;
            if (maxk + dk > 127) dk = 127 - maxk;
            int nsel = me_selected(g_md_orig);
            bool cp = g_md_copy && (fabsf(db) > 1e-6f || dk != 0);   /* Ctrl: the copies move, the originals stay */
            MidiSeq *n2 = midiseq_dup(g_md_orig, cp ? nsel : 0);
            if (n2) {
                int on = n2->n;
                for (int i = 0; i < on; ++i) if (n2->note[i].sel) {
                    MNote *q = &n2->note[i];
                    if (cp) { if (n2->n < MIDI_MAX_NOTES) { n2->note[n2->n] = *q; q = &n2->note[n2->n++]; n2->note[i].sel = 0; } else continue; }
                    q->start += db; q->key = (unsigned char)(q->key + dk);
                }
                me_publish(s, n2);
            }
            if (dk != g_md_lastdk && g_md_note >= 0) { g_md_lastdk = dk; me_preview(ch, g_md_orig->note[g_md_note].key + dk, g_md_orig->note[g_md_note].vel); }
        } else if (g_md == MD_RESIZE && g_md_orig) {
            float dl = (mp.x - g_md_p0.x) / g_me_ppb;
            if (g_md_note >= 0) {                              /* 1.3.2: the end of the grabbed note: fine + magnet */
                float e0 = g_md_orig->note[g_md_note].start + g_md_orig->note[g_md_note].len;
                dl = me_magnet(e0 + dl, shift) - e0;
            }
            MidiSeq *n2 = midiseq_dup(g_md_orig, 0);
            if (n2) {
                float minlen = 1.0f / 64.0f;
                for (int i = 0; i < n2->n; ++i) if (n2->note[i].sel) {
                    MNote *q = &n2->note[i];
                    float e = q->start + q->len + dl;
                    q->len = e - q->start;
                    if (q->len < minlen) q->len = minlen;
                    if (i == g_md_note) g_me_lastlen = q->len;
                }
                me_publish(s, n2);
            }
        } else if (g_md == MD_VEL && g_md_orig) {
            int dv = (int)roundf((g_md_p0.y - mp.y) * 127.0f / (vh > 0 ? vh : 36.0f));
            MidiSeq *n2 = midiseq_dup(g_md_orig, 0);
            if (n2) {
                for (int i = 0; i < n2->n; ++i) if (n2->note[i].sel) {
                    int v = n2->note[i].vel + dv;
                    n2->note[i].vel = (unsigned char)(v < 1 ? 1 : v > 127 ? 127 : v);
                }
                me_publish(s, n2);
            }
        } else if (g_md == MD_LEN) {
            float nl = roundf((float)(g_me_b0 + (mp.x - gx) / g_me_ppb));
            if (nl < 1.0f) nl = 1.0f;
            if (nl > 1024.0f) nl = 1024.0f;
            if (nl != L) { MidiSeq *n2 = midiseq_dup(m, 0); if (n2) { n2->length = nl; me_publish(s, n2); } }
        }
        if (!down) {
            if (g_md == MD_BOX) {
                float b0 = (float)(g_me_b0 + (fminf(g_md_p0.x, mp.x) - gx) / g_me_ppb), b1 = (float)(g_me_b0 + (fmaxf(g_md_p0.x, mp.x) - gx) / g_me_ppb);
                int k1 = g_me_top - (int)floorf((fminf(g_md_p0.y, mp.y) - gy) / g_me_kh);
                int k0 = g_me_top - (int)floorf((fmaxf(g_md_p0.y, mp.y) - gy) / g_me_kh);
                const MidiSeq *cur = session_midi_get(s, ch, sc);
                MidiSeq *n2 = cur ? midiseq_dup(cur, 0) : NULL;
                if (n2) {
                    for (int i = 0; i < n2->n; ++i) {
                        const MNote *q = &n2->note[i];
                        if (q->start < b1 && q->start + q->len > b0 && q->key >= k0 && q->key <= k1) n2->note[i].sel = 1;
                    }
                    me_publish(s, n2);
                }
            }
            g_md = MD_NONE; g_md_copy = false;
            free(g_md_orig); g_md_orig = NULL;
            me_preview_off();
        }
    }

    /* ---- keys ---- */
    if (g_me_focus && !g_ti_active) {
        if (ctrl && IsKeyPressed(KEY_A)) {
            MidiSeq *n2 = midiseq_dup(m, 0);
            if (n2) { for (int i = 0; i < n2->n; ++i) n2->note[i].sel = 1; me_publish(s, n2); }
        }
        if (IsKeyPressed(KEY_ESCAPE) && me_selected(m)) me_select(s, m, -1, true, false);
        if (ctrl && IsKeyPressed(KEY_U)) me_quantize(s);                  /* 1.3.2: like Ableton */
    }
#undef BX
#undef KY
}

