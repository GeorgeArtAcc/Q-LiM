/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 2.1: the channels - clip cells, the scene column, the buttons, channel headers and footers, faders, FX strip, Master. Moved out of ui.c. */
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

/* step 36: the meter scale = the fader scale (as in Bitwig): the same dB is at the same height */
static float vu_db_y(float y, float h, float db) { return y + (1.0f - session_db_to_pos(db)) * h; }

static Color mix_col(Color a, Color b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return (Color){ (unsigned char)(a.r + (b.r - a.r) * t), (unsigned char)(a.g + (b.g - a.g) * t),
                    (unsigned char)(a.b + (b.b - a.b) * t), 255 };
}

/* step 36c: a "CLIP" lamp for every meter (channel 0..MAX_CHANNELS-1, master = MAX_CHANNELS).
 * Lights up as soon as the signal goes over 0 dBFS, and stays on until clicked (the lamp or the meter). */
double g_clip_led[MAX_CHANNELS + 1];   /* 1.3.14: lit until this time (like a mixing desk: only while clipping) */
#define CLIP_LED_HOLD 0.35                       /* seconds - so that even one short peak is seen */

/* id: a channel or MAX_CHANNELS = master; clipped = the audio thread has seen > 0 dBFS (since the last frame) */
/* 1.3.16: a pan knob is dragged from where it is (like the faders): up = right, down = left,
 * the whole range over 4 knob heights; Shift = fine; it stops lightly in the centre */
static float g_pan_y0, g_pan_v0, g_pan_last;
static bool  g_pan_sh;
static float pan_drag_value(float mouse_y, float knob_d)
{
    bool sh = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (sh != g_pan_sh) { g_pan_sh = sh; g_pan_v0 = g_pan_last; g_pan_y0 = mouse_y; }   /* Shift pressed/released: no jump */
    float p = g_pan_v0 + (g_pan_y0 - mouse_y) / (knob_d * 2.0f) * (sh ? 0.2f : 1.0f);
    if (p < -1.0f) p = -1.0f;
    if (p >  1.0f) p =  1.0f;
    if (fabsf(p) < 0.03f && fabsf(g_pan_v0) >= 0.03f) p = 0.0f;   /* passing the centre: it stops there lightly */
    g_pan_last = p;
    return p;
}

static void draw_vu_meter(float x, float y, float w, float h, float lvl_l, float lvl_r, int id, bool clipped)
{
    /* 1.3.12: meters hidden (Ctrl+Shift+U) = only the dB scale, as a ruler: no box, no bars, no clip lamp -
     * nothing that moves, so nothing is drawn again */
    bool meters = perf_settings()->meters;
    if (!meters) {
        Color rl = mix_col(g_theme.panel_bg, g_theme.text_dim, 0.6f);
        DrawLine((int)(x + w), (int)y, (int)(x + w), (int)(y + h), rl);
        goto scale;
    }
    Color vu_bg = g_theme.bg;
    DrawRectangleRounded((Rectangle){ x, y, w, h }, 2, vu_bg);

    /* two bars: left and right channel, with 1 pixel between them (4 px are left on the right for the ticks) */
    float area = w - 4.0f;
    float bw = (area - 1.0f) * 0.5f;
    float lv[2] = { lvl_l, lvl_r };
    float bot = y + h;
    /* step 36c: the colours blend smoothly with height (as in DAWs): green up to -12 dB,
     * towards yellow up to -3, towards orange up to 0 dB, and ABOVE 0 dB (where the audio breaks up) - red */
    Color orange = mix_col(g_theme.vu_yellow, g_theme.vu_red, 0.5f);
    struct { float db; Color c; } st[] = {
        { -1000.0f, g_theme.vu_green }, { -12.0f, g_theme.vu_green }, { -3.0f, g_theme.vu_yellow }, { 0.0f, orange },
    };
    const int nst = (int)(sizeof st / sizeof st[0]);
    for (int k = 0; k < 2; ++k) {
        float bx = x + 1.0f + k * (bw + 1.0f);
        if (lv[k] <= 0.00001f) continue;
        float db = 20.0f * log10f(lv[k]);
        if (db > 6.0f) db = 6.0f;
        float fill_top = vu_db_y(y, h, db);
        if (fill_top >= bot - 0.5f) continue;
        for (int i = 0; i + 1 < nst; ++i) {             /* the gradient sections, cut off at the level */
            float ya = i == 0 ? bot : vu_db_y(y, h, st[i].db);      /* bottom */
            float yb = vu_db_y(y, h, st[i + 1].db);                 /* top */
            if (fill_top >= ya) break;
            float top = fill_top > yb ? fill_top : yb;
            float span = ya - yb;
            Color cb = st[i].c;
            Color ct = span > 0.5f ? mix_col(st[i].c, st[i + 1].c, (ya - top) / span) : st[i + 1].c;
            DrawRectangleGradientV(bx, top, bw, ya - top, ct, cb);
        }
        float y0 = vu_db_y(y, h, 0.0f);
        if (fill_top < y0) DrawRectangleGradientV(bx, fill_top, bw, y0 - fill_top, g_theme.vu_red, g_theme.vu_red);
    }

    /* the CLIP lamp above the meter */
    if (id >= 0 && id <= MAX_CHANNELS) {
        double now = GetTime();
        if (clipped) g_clip_led[id] = now + CLIP_LED_HOLD;
        Rectangle led = { x, y - 7, w, 5 };
        DrawRectangleRounded(led, 2, now < g_clip_led[id] ? g_theme.vu_red : mix_col(g_theme.bg, g_theme.text_dim, 0.35f));
    }
    DrawLine((int)(x + 1 + bw + 0.5f), (int)y, (int)(x + 1 + bw + 0.5f), (int)(y + h), g_theme.panel_bg);

scale:;
    /* step 36b: the scale lines - ABOVE the bars, clearer. The main ones (with a label) - across the whole meter,
     * 0 dB - the clearest; the intermediate ones - a short tick on the right. */
    /* step 36d: the scale as in Bitwig - the numbers are "how many dB below 0" (0, 6, 12, 18, 24, 36, 48, ∞), without a "minus".
     * Ticks on the right: with a label - longer (0 dB - the longest and clearest), the intermediate ones - short. Nothing crosses the column. */
    static const float minor[] = { 6, 3, -3, -9, -30, -60 };
    static const float major[] = { 0, -6, -12, -18, -24, -36, -48 };
    Color c_tick = g_theme.text_dim;
    for (size_t i = 0; i < sizeof minor / sizeof minor[0]; ++i) {
        float my = vu_db_y(y, h, minor[i]);
        DrawLine((int)(x + w - 4), (int)my, (int)(x + w), (int)my, c_tick);
    }
    for (size_t i = 0; i < sizeof major / sizeof major[0]; ++i) {
        float my = vu_db_y(y, h, major[i]);
        bool zero = major[i] == 0.0f;
        DrawLine((int)(x + w - (zero ? 11 : 8)), (int)my, (int)(x + w), (int)my, zero ? g_theme.text : c_tick);
    }

    if (meters) DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, 2, 1, g_theme.text_dim);

    /* the labels - exactly opposite their tick, right-aligned. On a short meter first 0/12/24/48 and ∞,
     * then 6/18/36 if there is room (they do not overlap). */
    static const float order[] = { 0, -12, -24, -48, -1000, -6, -18, -36 };
    float used[10]; int nused = 0;
    Color lc = { (unsigned char)((g_theme.text_dim.r * 2 + g_theme.text.r) / 3), (unsigned char)((g_theme.text_dim.g * 2 + g_theme.text.g) / 3),
                 (unsigned char)((g_theme.text_dim.b * 2 + g_theme.text.b) / 3), 255 };
    for (size_t i = 0; i < sizeof order / sizeof order[0]; ++i) {
        float db = order[i];
        float cy = db <= -999.0f ? y + h - 4 : vu_db_y(y, h, db);
        bool clash = false;
        for (int k = 0; k < nused; ++k) if (fabsf(used[k] - cy) < 12.0f) clash = true;
        if (clash) continue;
        used[nused++] = cy;
        char buf[8];
        if (db <= -999.0f) snprintf(buf, sizeof buf, "\xE2\x88\x9E");      /* ∞ */
        else snprintf(buf, sizeof buf, "%d", (int)(-db));
        int tw = ui_text_w(buf, 11);
        ui_text(buf, (int)(x - tw - 4), (int)(cy - 7), 11, db == 0.0f ? g_theme.text : lc);
    }
}


/* ===== Step 36: a fader as in Bitwig =====
 * dragged from wherever you grab it (it does not jump under the mouse), Shift = fine, double click = 0 dB,
 * it "snaps" lightly at 0 dB. A tick for 0 dB and a handle at the current position. */
static int    g_fdr_click_id = -1;
static double g_fdr_click_t = 0.0;
static float  g_fdr_y0, g_fdr_v0;
static bool   g_fdr_shift;

static void fader_draw(Rectangle sr, float pos, Color fill, Color frame)
{
    DrawRectangleRounded(sr, R_SMALL, g_theme.bg);
    float fill_h = sr.height * pos;
    if (fill_h > 0.5f)
        DrawRectangleRounded((Rectangle){ sr.x, sr.y + sr.height - fill_h, sr.width, fill_h },
                             fill_h < 2 * R_SMALL ? fill_h * 0.5f : R_SMALL, fill);
    float uy = sr.y + sr.height * (1.0f - FADER_UNITY_POS);            /* 0 dB */
    DrawLine((int)sr.x + 1, (int)uy, (int)(sr.x + sr.width) - 1, (int)uy, g_theme.text_dim);
    DrawRectangleRoundedLines(sr, R_SMALL, 1, frame);
    float hy = sr.y + sr.height * (1.0f - pos);                         /* the handle */
    DrawRectangleRounded((Rectangle){ sr.x - 3, hy - 3, sr.width + 6, 6 }, 3, g_theme.bg);
    DrawRectangleRounded((Rectangle){ sr.x - 2, hy - 2, sr.width + 4, 4 }, 2, g_theme.text);
}

/* 1.0.5: during an automatic crossfade a "ghost" handle shows where the fader effectively is right now
 * (the fader's own setting times the crossfade gain). The real fader is not touched, so your setting stays. */
static void fader_draw_ghosts(Session *s, int ch, Rectangle sr, float pos)
{
    Channel *chp = &s->channels[ch];
    float base_db = session_gain_to_db(pos);
    for (int sc = 0; sc < s->scene_count; ++sc) {
        Clip *cl = &chp->clips[sc];
        if (!atomic_load_explicit(&cl->xf_vis_on, memory_order_relaxed)) continue;
        float g = atomic_load_explicit(&cl->xf_vis, memory_order_relaxed);
        float gp = (g <= 0.0005f || base_db < -500.0f) ? 0.0f : session_db_to_pos(base_db + 20.0f * log10f(g));
        if (gp < 0.0f) gp = 0.0f;
        if (gp > pos) gp = pos;
        float hy = sr.y + sr.height * (1.0f - gp);
        Color a = g_theme.selection;
        /* the moving part between the setting and the ghost, faint */
        float top = sr.y + sr.height * (1.0f - pos);
        if (hy - top > 1.0f)
            DrawRectangle((int)sr.x + 2, (int)top, (int)sr.width - 4, (int)(hy - top), (Color){ a.r, a.g, a.b, 40 });
        DrawRectangleRounded((Rectangle){ sr.x - 5, hy - 4, sr.width + 10, 8 }, 4, (Color){ a.r, a.g, a.b, 70 });
        DrawRectangleRounded((Rectangle){ sr.x - 3, hy - 2, sr.width + 6, 4 }, 2, (Color){ a.r, a.g, a.b, 220 });
    }
}

/* true = the value changed (*pos is the new one) */
static bool fader_input(int id, Rectangle sr, float *pos)
{
    Vector2 m = GetMousePosition();
    Rectangle hit = { sr.x - 3, sr.y - 4, sr.width + 6, sr.height + 8 };
    if (CheckCollisionPointRec(m, hit) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        !g_ctx_open && !g_ti_active && !g_audio_dlg_open) {
        double now = GetTime();
        if (g_fdr_click_id == id && now - g_fdr_click_t < DOUBLE_CLICK_TIME) {
            g_fdr_click_id = -1;
            g_dragging_slider = -1;
            *pos = FADER_UNITY_POS;                                     /* double click = 0 dB */
            return true;
        }
        g_fdr_click_id = id;
        g_fdr_click_t = now;
        g_dragging_slider = id;
        g_fdr_y0 = m.y;
        g_fdr_v0 = *pos;
        g_fdr_shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        return false;
    }
    if (g_dragging_slider != id || !IsMouseButtonDown(MOUSE_BUTTON_LEFT) || sr.height < 1.0f) return false;
    bool sh = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (sh != g_fdr_shift) { g_fdr_shift = sh; g_fdr_y0 = m.y; g_fdr_v0 = *pos; }   /* switching fine/coarse - without a jump */
    float v = g_fdr_v0 + (g_fdr_y0 - m.y) / sr.height * (sh ? 0.12f : 1.0f);
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    if (!sh && fabsf(v - FADER_UNITY_POS) < 0.008f) v = FADER_UNITY_POS;   /* 0 dB is easy to hit */
    if (v == *pos) return false;
    if (fabsf(m.y - g_fdr_y0) > 2.0f) g_fdr_click_id = -1;             /* a drag - not a double click */
    *pos = v;
    return true;
}

/* step 36d: the dB under the fader - in a dark "box" (more visible, but calm) */
static void fader_db_box(const char *txt, float x, float y, float w, float h, int fs)
{
    Rectangle r = { x, y, w, h };
    DrawRectangleRounded(r, R_SMALL, g_theme.bg);
    Color fr = g_theme.text_dim; fr.a = 120;
    DrawRectangleRoundedLines(r, R_SMALL, 1, fr);
    int tw = ui_text_w(txt, fs);
    ui_text(txt, (int)(x + (w - tw) / 2), (int)(y + (h - fs) / 2 + 1), fs, g_theme.text);   /* the digits - exactly in the middle */
}

static void fader_db_text(char *buf, size_t n, float pos)
{
    float db = session_gain_to_db(pos);
    if (db < -99.0f) snprintf(buf, n, "-inf");
    else if (fabsf(db) < 0.05f) snprintf(buf, n, "0.0");
    else if (db > 0.0f) snprintf(buf, n, "+%.1f", db);
    else snprintf(buf, n, "%.1f", db);
}

// ============ Clip cell ============

static void draw_clip_cell_impl(Session *s, int ch, int scene,
                                float x, float y, float w, float h);

// MIDI Learn for buttons/pads: a small badge with the code ("N36" / "CC20" / "ARMED"); LMB = wait for the next signal, RMB = clear.
// Returns true if learn mode swallowed the click.
static bool learn_button(Session *s, int kind, int ch, int scene, Rectangle r, int bind, bool draw_badge)
{
    if (!g_midi_learn_mode) return false;
    int lk = 0, lch = -1, lsc = -1;
    bool any = session_get_button_learn(s, &lk, &lch, &lsc);
    bool armed = any && lk == kind && lch == ch && (kind != BTN_LEARN_CLIP || lsc == scene);

    if (draw_badge && (armed || bind > 0)) {
        char t[16];
        if (armed) snprintf(t, sizeof(t), "ARMED"); else session_bind_to_text(bind, t, sizeof(t));
        int tw = ui_text_w(t, 10);
        int bw = tw + 8, bh = 14;
        int bx = (int)(r.x + r.width - bw - 2), by = (int)(r.y + 2);
        DrawRectangle(bx, by, bw, bh, armed ? g_theme.mute : g_theme.selection);
        ui_text(t, bx + 4, by + 1, 10, armed ? RAYWHITE : g_theme.bg);
    }
    if (armed) DrawRectangleLinesEx(r, 2, g_theme.mute);

    Vector2 mouse = GetMousePosition();
    if (!CheckCollisionPointRec(mouse, r) || g_ctx_open || g_ti_active || g_audio_dlg_open) return false;
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        session_arm_button_learn(s, kind, ch, scene);
        return true;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        if (kind == BTN_LEARN_MUTE) session_set_mute_bind(s, ch, 0);
        else if (kind == BTN_LEARN_SOLO) session_set_solo_bind(s, ch, 0);
        else session_set_clip_bind(s, ch, scene, 0);
        if (armed) session_disarm_button_learn(s);
        ui_flash_message("MIDI button mapping cleared");
        return true;
    }
    return false;
}

/* 1.4.17: a launch at a clock time: a small clock + the time in the corner (the last minute: a countdown) */
static void draw_sched_badge(int idx, float x, float y, float w)
{
    if (idx < 0) return;
    const SchedItem *it = sched_get(idx);
    if (!it) return;
    char t[24];
    double left = sched_seconds_left(idx);
    if (it->state == SCHED_MISSED) snprintf(t, sizeof t, "missed");
    else if (left >= 0 && left < 60) snprintf(t, sizeof t, "%.0f s", ceil(left));
    else sched_format(it, t, sizeof t);
    int tw = ui_text_w(t, 10);
    float bw = (float)tw + 18, bx = x + w - bw - 3, by = y + 3;
    Color bg = it->state == SCHED_MISSED ? (Color){ 120, 60, 50, 230 } : (Color){ 30, 70, 110, 230 };
    if (left >= 0 && left < 10 && it->state == SCHED_WAITING && fmod(GetTime(), 1.0) < 0.5) bg = (Color){ 40, 110, 170, 240 };
    DrawRectangleRounded((Rectangle){ bx, by, bw, 13 }, 0.5f, bg);
    DrawCircleLines((int)bx + 7, (int)by + 6, 4.0f, RAYWHITE);                   /* the clock */
    DrawLine((int)bx + 7, (int)by + 6, (int)bx + 7, (int)by + 3, RAYWHITE);
    DrawLine((int)bx + 7, (int)by + 6, (int)bx + 9, (int)by + 6, RAYWHITE);
    ui_text(t, (int)bx + 14, (int)by + 1, 10, RAYWHITE);
}

void draw_clip_cell(Session *s, int ch, int scene,
                           float x, float y, float w, float h)
{
    Clip *cl0 = &s->channels[ch].clips[scene];
    bool real_clip = atomic_load(&cl0->has_clip) && !atomic_load(&cl0->is_note);
    if (g_midi_learn_mode && real_clip) {
        // in learn mode the cell is only shown; clicks are for binding, not for playing/selecting
        bool saved = g_ctx_open;
        g_ctx_open = true;
        draw_clip_cell_impl(s, ch, scene, x, y, w, h);
        g_ctx_open = saved;
        learn_button(s, BTN_LEARN_CLIP, ch, scene, (Rectangle){ x, y, w, h },
                     session_get_clip_bind(s, ch, scene), true);
    } else {
        draw_clip_cell_impl(s, ch, scene, x, y, w, h);
    }
    /* the frame of the selected cell is drawn ON TOP, so it also covers the Play/Stop button (otherwise it goes under it) */
    if (g_selected_ch == ch && g_selected_scene == scene && !g_ch_focus && !atomic_load(&cl0->is_note))
        DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_CELL, 2, g_theme.selection);
    {   /* 1.1: the clip waits for the next beat/bar - it blinks (stop: red) */
        int qs = session_clip_launch_state(s, ch, scene);
        if (qs) {
            float a = 0.5f + 0.5f * sinf((float)GetTime() * 14.0f);
            Color c = qs == 2 ? (Color){ 235, 80, 80, 255 } : g_theme.selection;
            Color f = c; f.a = (unsigned char)(25 + 70 * a);
            c.a = (unsigned char)(110 + 145 * a);
            DrawRectangleRounded((Rectangle){ x, y, h, h }, R_CELL, f);
            DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_CELL, 2, c);
        }
    }
    if (cmark_get(ch, scene)) {                         /* step 36: marked (Ctrl/Shift + click) */
        Color f = g_theme.selection; f.a = 70;
        DrawRectangleRounded((Rectangle){ x, y, w, h }, R_CELL, f);
        DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_CELL, 2, g_theme.selection);
    }
    if (sched_count() > 0) draw_sched_badge(sched_find(SCHED_CLIP, ch, scene), x, y, w);   /* 1.4.17 */
}

static void draw_clip_cell_impl(Session *s, int ch, int scene,
                                float x, float y, float w, float h)
{
    Clip *cl = &s->channels[ch].clips[scene];
    Color chan_col = channel_color(s->channels[ch].color_id);

    bool has    = atomic_load(&cl->has_clip);
    bool is_note = has && atomic_load(&cl->is_note);
    bool play   = atomic_load(&cl->playing);
    bool paused = atomic_load(&cl->paused);
    bool sel    = (g_selected_ch == ch && g_selected_scene == scene) && !g_ch_focus;   /* a selected channel -> the cell is not */
    bool hov    = (g_hovered_ch == ch && g_hovered_scene == scene);

    {   /* 1.4: a cell that is being recorded (or waits for the bar to start / stop) */
        double rb = 0.0;
        int rst = session_rec_state(s, ch, scene, &rb);
        if (rst != REC_IDLE) {
            Color red = { 210, 45, 45, 255 };
            bool blink = ((int)(GetTime() * 3.0)) & 1;
            Color bgc = rst == REC_WAIT ? (blink ? (Color){ 110, 30, 30, 255 } : g_theme.panel_bg2) : red;
            if (rst == REC_STOPPING && blink) bgc = (Color){ 150, 35, 35, 255 };
            DrawRectangleRounded((Rectangle){ x, y, w, h }, R_CELL, bgc);
            DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_CELL, 2, (Color){ 255, 120, 110, 255 });
            if (sel) DrawRectangleRoundedLines((Rectangle){ x - 1, y - 1, w + 2, h + 2 }, R_CELL + 1, 2, g_theme.selection);
            /* 2.0: the stop button (a square) - the rest of the cell selects it (the live view below) */
            Rectangle sb = { x + 3, y + 3, h - 6, h - 6 };
            bool sbh = CheckCollisionPointRec(GetMousePosition(), sb) && fx_ui_free();
            DrawRectangleRounded(sb, 0.25f, sbh ? (Color){ 255, 255, 255, 60 } : (Color){ 0, 0, 0, 50 });
            float sq = h * 0.34f;
            DrawRectangle((int)(x + h / 2 - sq / 2), (int)(y + h / 2 - sq / 2), (int)sq, (int)sq, RAYWHITE);
            char lb[64];
            if (rst == REC_WAIT) snprintf(lb, sizeof lb, "REC - waits for the bar");
            else { char bbx[24]; fmt_bar_beat(bbx, sizeof bbx, rb); snprintf(lb, sizeof lb, "REC  %s%s", bbx, rst == REC_STOPPING ? "  (stops on the bar)" : ""); }
            while (ui_text_w_scaled(lb, 13) > w - h - 8 && strlen(lb) > 4) utf8_chop_last(lb);
            ui_text_scaled(lb, (int)(x + h + 4), (int)(y + h / 2 - 8), 13, RAYWHITE);
            if (CheckCollisionPointRec(GetMousePosition(), (Rectangle){ x, y, w, h }) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                !g_drag_active && !g_ctx_open && !g_ti_active && !g_audio_dlg_open && fx_ui_free()) {
                g_selected_ch = ch; g_selected_scene = scene;
                g_ch_focus = false;
                if (sbh) ui_rec_click(s, ch, scene);      /* the square stops it; elsewhere only selects */
            }
            return;
        }
    }

    if (is_note) {
        Color n_bg, n_border, n_text;
        note_colors(s, ch, cl, &n_bg, &n_border, &n_text);
        DrawRectangleRounded((Rectangle){ x, y, w, h }, R_CELL, n_bg);
        DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_CELL, 2, n_border);

        if (sel) {
            DrawRectangleRoundedLines((Rectangle){ x - 1, y - 1, w + 2, h + 2 },
                                      R_CELL + 1, 2, g_theme.selection);
        }

        char nm[NAME_LEN];
        snprintf(nm, sizeof(nm), "%s", cl->name);

        int font_size = 14;
        int max_w = (int)w - 12;

        while (ui_text_w_scaled(nm, font_size) > max_w && strlen(nm) > 3) {
            utf8_chop_last(nm);
        }

        int tw = ui_text_w_scaled(nm, font_size);
        int th = (int)(font_size * ((float)g_font_size / 14.0f));

        ui_text_scaled(nm,
                (int)(x + (w - tw) / 2),
                (int)(y + (h - th) / 2),
                font_size, n_text);

        DrawRectangleRounded((Rectangle){ x + 5, y + 5, 6, 6 }, 1.5f, n_border);

        Vector2 mouse = GetMousePosition();
        Rectangle cell_rect = { x, y, w, h };

        if (CheckCollisionPointRec(mouse, cell_rect) &&
            IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            !g_drag_active && !g_ctx_open && !g_ti_active && !g_audio_dlg_open) {
            g_selected_ch = ch;
            g_selected_scene = scene;
            g_drag_clip_active = true;
            g_drag_clip_p0 = GetMousePosition();
            g_drag_clip_src_ch = ch;
            g_drag_clip_src_scene = scene;
        }

        return;
    }

    Color bg = has ? g_theme.panel_bg2 : g_theme.cell_empty;
    if (hov) bg = g_theme.cell_hover;
    if (g_ui_style && has) {                    /* 2.0.0 Modern: the clip is tinted with its channel's colour */
        float t = play ? 0.46f : 0.20f;          /* (clearer than before: the colours read at a glance) */
        if (hov) t += 0.07f;
        bg = (Color){ (unsigned char)(bg.r + (chan_col.r - bg.r) * t), (unsigned char)(bg.g + (chan_col.g - bg.g) * t),
                      (unsigned char)(bg.b + (chan_col.b - bg.b) * t), 255 };
    }
    DrawRectangleRounded((Rectangle){ x, y, w, h }, R_CELL, bg);

    if (sel) {
        DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_CELL, 2, g_theme.selection);
    } else if (hov) {
        DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_CELL, 1, g_theme.text_dim);
    } else if (g_theme.outline.a) {
        DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_CELL, 1, g_theme.outline);   /* 2.0.0: light themes */
    }

    float btn_size = h;
    float btn_x    = x;                 /* exactly in line with the left edge of the channel header */
    float btn_y    = y;
    Rectangle btn_rect = { btn_x, btn_y, btn_size, btn_size };

    Vector2 c = { btn_x + btn_size/2, btn_y + btn_size/2 };
    float tri_s = btn_size * 0.28f;

    if (!has) {
        Color empty_btn = g_theme.panel_bg;
        int eadd = empty_btn.r + empty_btn.g + empty_btn.b > 600 ? -18 : 15;   /* 2.0.0: a light theme -> a bit darker */
        empty_btn.r = (unsigned char)(empty_btn.r + eadd);
        empty_btn.g = (unsigned char)(empty_btn.g + eadd);
        empty_btn.b = (unsigned char)(empty_btn.b + eadd);
        bool sq_hov = CheckCollisionPointRec(GetMousePosition(), btn_rect) && !g_ctx_open && !g_ti_active && !g_drag_active;
        if (sq_hov && eadd < 0) {
            empty_btn.r = (unsigned char)(empty_btn.r - 20); empty_btn.g = (unsigned char)(empty_btn.g - 20); empty_btn.b = (unsigned char)(empty_btn.b - 20);
        } else if (sq_hov) {
            empty_btn.r = (unsigned char)(empty_btn.r + 20); empty_btn.g = (unsigned char)(empty_btn.g + 20); empty_btn.b = (unsigned char)(empty_btn.b + 20);
        }
        DrawRectangleRounded(btn_rect, R_CELL, empty_btn);
        DrawRectangleRoundedLines(btn_rect, R_CELL, 1, g_theme.text_dim);

        bool armed_ch = session_is_armed(s, ch);           /* 1.4: an armed channel: a record circle instead of stop */
        if (armed_ch) {
            DrawCircleV(c, btn_size * (sq_hov ? 0.26f : 0.22f), sq_hov ? (Color){ 255, 80, 70, 255 } : (Color){ 200, 50, 45, 255 });
        } else {
            float sq = btn_size * (sq_hov ? 0.25f : 0.22f);
            DrawRectangleRounded((Rectangle){ c.x - sq, c.y - sq, sq * 2, sq * 2 }, 1.5f,
                                 sq_hov ? g_theme.text : g_theme.text_dim);
        }
        if (sel) DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_CELL, 2, g_theme.selection);

        ui_text("+", (int)(x + w/2 + 10), (int)(y + h/2 - 11),
                20, g_theme.text_dim);

        Vector2 mouse = GetMousePosition();
        Rectangle cell_rect = { x, y, w, h };

        if (armed_ch && CheckCollisionPointRec(mouse, btn_rect) &&
            IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_drag_active
            && !g_ctx_open && !g_ti_active && !g_audio_dlg_open && fx_ui_free()) {
            ui_rec_click(s, ch, scene);                     /* 1.4: start recording into this cell */
            return;
        }
        if (CheckCollisionPointRec(mouse, btn_rect) &&
            IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_drag_active
            && !g_ctx_open && !g_ti_active && !g_audio_dlg_open) {
            for (int sc = 0; sc < s->scene_count; ++sc) {
                Clip *other = &s->channels[ch].clips[sc];
                if (atomic_load(&other->has_clip)) {
                    atomic_store(&other->playing, false);
                    atomic_store(&other->paused, false);
                }
            }
            return;
        }

        if (CheckCollisionPointRec(mouse, cell_rect) &&
            IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_drag_active
            && !g_ctx_open && !g_ti_active && !g_audio_dlg_open) {

            g_selected_ch = ch;
            g_selected_scene = scene;

            double now = GetTime();
            double dt = now - g_last_click_time;
            bool same_cell = (g_last_click_ch == ch &&
                              g_last_click_scene == scene);

            if (dt < DOUBLE_CLICK_TIME && same_cell) {
                g_last_click_time = 0.0;
                g_last_click_ch = -1;
                g_last_click_scene = -1;

                if (fxchain_has_instrument(s, ch)) {         /* 1.3: an instrument channel -> a new MIDI clip */
                    if (session_new_midi_clip(s, ch, scene, (float)sig_bar(), "MIDI") == 0)
                        ui_flash_message("New MIDI clip - double click in the editor below to add notes");
                    return;
                }
                g_pending_action.action = UI_ACTION_NEW_NOTE;
                g_pending_action.ch = ch;
                g_pending_action.scene = scene;
            } else {
                g_last_click_time = now;
                g_last_click_ch = ch;
                g_last_click_scene = scene;
            }
        }
        return;
    }

    bool btn_hov = CheckCollisionPointRec(GetMousePosition(), btn_rect) && !g_ctx_open && !g_ti_active && !g_drag_active;
    Color btn_bg = play ? chan_col
                        : (paused ? g_theme.text_dim
                                  : g_theme.panel_bg2);
    if (btn_hov) {                                  /* the mouse over it: a slightly lighter and bigger triangle */
        int add = play ? 30 : 22;
        btn_bg.r = (unsigned char)(btn_bg.r + add > 255 ? 255 : btn_bg.r + add);
        btn_bg.g = (unsigned char)(btn_bg.g + add > 255 ? 255 : btn_bg.g + add);
        btn_bg.b = (unsigned char)(btn_bg.b + add > 255 ? 255 : btn_bg.b + add);
        tri_s *= 1.12f;
    }
    DrawRectangleRounded(btn_rect, R_CELL, btn_bg);
    DrawRectangleRoundedLines(btn_rect, R_CELL, 1,
                              play ? g_theme.bg : chan_col);

    Color tri_color = play ? g_theme.bg : chan_col;
    if (btn_hov && !play) {
        tri_color.r = (unsigned char)(tri_color.r + (255 - tri_color.r) / 4);
        tri_color.g = (unsigned char)(tri_color.g + (255 - tri_color.g) / 4);
        tri_color.b = (unsigned char)(tri_color.b + (255 - tri_color.b) / 4);
    }
    DrawTriangle(
        (Vector2){ c.x - tri_s*0.7f, c.y - tri_s },
        (Vector2){ c.x - tri_s*0.7f, c.y + tri_s },
        (Vector2){ c.x + tri_s*0.9f, c.y    },
        tri_color
    );
    if (sel) DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_CELL, 2, g_theme.selection);   /* the frame also covers the button */

    float text_x = btn_x + btn_size + 8;
    float text_area_w = x + w - text_x - 6;

    char shortname[NAME_LEN];
    snprintf(shortname, sizeof(shortname), "%s", cl->name);
    char *dot = strrchr(shortname, '.');
    if (dot && !atomic_load(&cl->is_midi)) *dot = '\0';
    while (ui_text_w_scaled(shortname, 15) > text_area_w && strlen(shortname) > 3) {
        utf8_chop_last(shortname);
    }

    /* 1.4.12: a big text size (Appearance): when the name and the time do not both fit, only the name stays (in the
     * middle); the name always in the full text colour (easier to read) */
    float fsc = (float)g_font_size / 14.0f;
    int name_px = (int)(14.0f * fsc), time_px = (int)(11.0f * fsc);
    bool show_info = 5 + name_px + 2 + time_px + 3 <= (int)h;
    ui_text_scaled(shortname, (int)text_x, show_info ? (int)(y + 5) : (int)(y + (h - name_px) / 2 - 1),
            14, g_theme.text);

    if (g_clip_time >= 1 && (play || paused) && !atomic_load(&cl->is_midi) && cl->wav.frame_count > 0) {
        /* 2.0.0 Modern: a thin progress line at the bottom of a playing clip */
        float fr = (float)((double)cl->position / (double)cl->wav.frame_count);
        if (fr < 0.0f) fr = 0.0f;
        if (fr > 1.0f) fr = 1.0f;
        float lx = x + h + 4, lw = w - h - 10;
        DrawRectangleRounded((Rectangle){ lx, y + h - 4, lw, 2 }, 1.0f, (Color){ chan_col.r, chan_col.g, chan_col.b, 70 });
        DrawRectangleRounded((Rectangle){ lx, y + h - 4, lw * fr, 2 }, 1.0f, chan_col);
    }
    if (!show_info) {
        /* (no counter / MIDI picture: no room) */
    } else if (atomic_load(&cl->is_midi)) {                    /* 1.3: a MIDI clip - a small picture of its notes */
        const MidiSeq *mm = atomic_load(&cl->mseq);
        char pb[40];
        float Lb = mm ? mm->length : 4.0f;
        double mp_ = play ? atomic_load(&cl->mpos_vis) : -1.0;
        if (mp_ >= 0.0) { char bbx[24]; fmt_bar_beat(bbx, sizeof bbx, mp_); snprintf(pb, sizeof pb, "%s / %g", bbx, (double)Lb / sig_bar()); }
        else fmt_bars(pb, sizeof pb, Lb);
        int tw = ui_text_w_scaled(pb, 11);
        ui_text_scaled(pb, (int)(x + w - tw - 6), (int)(y + h - 14), 11, play ? chan_col : g_theme.text_dim);
        float rx = text_x, ry = y + 21, rw = x + w - tw - 12 - text_x, rh = h - 24;
        if (mm && rw > 12 && rh >= 5) {
            int lo = 127, hi = 0;
            for (int i = 0; i < mm->n; ++i) { if (mm->note[i].key < lo) lo = mm->note[i].key; if (mm->note[i].key > hi) hi = mm->note[i].key; }
            if (hi - lo < 6) { int c2 = (lo + hi) / 2; lo = c2 - 3; hi = c2 + 3; }
            float kh = rh / (float)(hi - lo + 1);
            Color nc = play ? chan_col : g_theme.text_dim;
            for (int i = 0; i < mm->n && i < 4000; ++i) {
                const MNote *q = &mm->note[i];
                if (q->start >= Lb) continue;
                float nx = rx + rw * q->start / Lb, nw = rw * q->len / Lb;
                if (nw < 1.5f) nw = 1.5f;
                float ny = ry + (hi - q->key) * kh;
                DrawRectangle((int)nx, (int)ny, (int)ceilf(nw), (int)(kh < 1.5f ? 2 : ceilf(kh)), nc);
            }
            if (mp_ >= 0.0) { float px = rx + rw * (float)(mp_ / Lb); DrawLine((int)px, (int)ry, (int)px, (int)(ry + rh), g_theme.text); }
        }
    } else if (cl->wav.sample_rate > 0 && cl->wav.frame_count > 0 && g_clip_time != 1) {   /* 2.0.0: not with "Line" */
        double pos_sec = (double)cl->position / (double)cl->wav.sample_rate;
        double total_sec = (double)cl->wav.frame_count / (double)cl->wav.sample_rate;
        char t1[16], t2[16], buf[40];
        format_time(t1, sizeof(t1), pos_sec);
        format_time(t2, sizeof(t2), total_sec);
        snprintf(buf, sizeof(buf), "%s / %s", t1, t2);
        int tw = ui_text_w_scaled(buf, 11);
        ui_text_scaled(buf,
                (int)(x + w - tw - 6),
                (int)(y + h - 14),
                11,
                play ? chan_col : g_theme.text_dim);
    }

    Vector2 mouse = GetMousePosition();

    if (CheckCollisionPointRec(mouse, btn_rect) &&
        IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_drag_active
        && !g_ctx_open && !g_ti_active && !g_audio_dlg_open) {
        // playing -> restart; not playing -> stop the others on the channel and play this one
        session_toggle_clip(s, ch, scene);
        return;
    }

    Rectangle cell_rect = { x, y, w, h };
    Rectangle btn_check_rect = { x + 2, y, h, h };
    if (CheckCollisionPointRec(mouse, cell_rect) &&
        !CheckCollisionPointRec(mouse, btn_check_rect) &&
        IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_drag_active
        && !g_ctx_open && !g_ti_active && !g_audio_dlg_open) {
        g_selected_ch = ch;
        g_selected_scene = scene;
        g_drag_clip_active = true;
        g_drag_clip_p0 = GetMousePosition();
        g_drag_clip_src_ch = ch;
        g_drag_clip_src_scene = scene;
        /* hidden waveform (Ctrl+W): a double click on a clip brings it back (then everything is as before) */
        double now = GetTime();
        if (g_waveform_h < WAVEFORM_MIN_H && g_cell_last_ch == ch && g_cell_last_sc == scene &&
            now - g_cell_last_t < DOUBLE_CLICK_TIME) {
            ui_toggle_waveform();
            g_cell_last_ch = -1;
        } else {
            g_cell_last_ch = ch; g_cell_last_sc = scene; g_cell_last_t = now;
        }
    }
}

// ============ Scene column ============

void draw_scene_button(Session *s, int scene,
                              float x, float y, float w, float h)
{
    bool sel = (g_selected_ch == -1 && g_selected_scene == scene);

    Color bg = g_theme.panel_bg2;
    if (sel) {
        bg.r = (unsigned char)(g_theme.selection.r / 4);
        bg.g = (unsigned char)(g_theme.selection.g / 4);
        bg.b = (unsigned char)(g_theme.selection.b / 4);
    }

    DrawRectangleRounded((Rectangle){ x, y, w, h }, R_CELL, bg);

    if (sel) {
        DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_CELL, 2, g_theme.selection);
    } else {
        DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_CELL, 1, g_theme.text_dim);
    }

    char label[16];
    snprintf(label, sizeof(label), "S%d", scene + 1);
    Color txt_col = sel ? g_theme.selection : g_theme.text_dim;
    ui_text(label, (int)(x + 6), (int)(y + 6), 13, txt_col);
    ui_text(">", (int)(x + 32), (int)(y + 6), 14, g_theme.text);
    if (s && session_current_scene(s) == scene) {          /* 1.4.17: the scene "next" / "back" counts from */
        float p = session_transition_progress(s);
        Color c = session_automation_held(s) ? (Color){ 240, 150, 70, 255 } : g_theme.selection;
        DrawRectangle((int)x + 3, (int)(y + h - 4), (int)((w - 6) * (p >= 0 ? p : 1.0f)), 2, c);
    }
    if (sched_count() > 0) {
        int si = sched_find(SCHED_SCENE, -1, scene);
        if (si >= 0) {                                      /* (the scene column is narrow: just the clock) */
            float bx = x + w - 13, by = y + 3;
            const SchedItem *it = sched_get(si);
            Color c = it && it->state == SCHED_MISSED ? (Color){ 230, 110, 90, 255 } : (Color){ 110, 190, 255, 255 };
            DrawCircleLines((int)bx + 5, (int)by + 5, 4.5f, c);
            DrawLine((int)bx + 5, (int)by + 5, (int)bx + 5, (int)by + 2, c);
            DrawLine((int)bx + 5, (int)by + 5, (int)bx + 8, (int)by + 5, c);
        }
    }
}

void draw_scene_header(float x, float y, float w, float h)
{
    DrawRectangleRounded((Rectangle){ x, y, w, h }, R_CELL, g_theme.panel_bg);
    ui_text("Scenes", (int)(x + 6), (int)(y + 8), 13, g_theme.text);
}

bool draw_add_scene_button(float x, float y, float w, float h)
{
    Rectangle r = { x, y, w, h };
    Vector2 m = GetMousePosition();
    bool hov = CheckCollisionPointRec(m, r);

    Color bg = hov ? g_theme.btn_add_hover : g_theme.btn_add;
    DrawRectangleRounded(r, R_CELL, bg);
    DrawRectangleRoundedLines(r, R_CELL, 1, g_theme.text_dim);

    const char *plus = "+";
    int tw = ui_text_w(plus, 16);
    ui_text(plus,
            (int)(x + (w - tw) / 2),
            (int)(y + (h - 16) / 2 + 1),
            16, g_theme.text_dim);

    return hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

// ============ Loop button ============

bool draw_loop_button(Session *s,
                             float x, float y, float w, float h)
{
    bool has_selection = false;
    bool loop_on = false;
    int sel_ch = g_selected_ch;
    int sel_sc = g_selected_scene;

    if (sel_ch >= 0 && sel_sc >= 0 &&
        sel_ch < s->channel_count &&
        sel_sc < s->scene_count) {
        Clip *cl = &s->channels[sel_ch].clips[sel_sc];
        if (atomic_load(&cl->has_clip) &&
            !atomic_load(&cl->is_note)) {
            has_selection = true;
            loop_on = atomic_load(&cl->loop);
        }
    }

    Color bg;
    Color txt_col;

    if (!has_selection) {
        bg = g_theme.panel_bg2;
        txt_col = g_theme.text_dim;
    } else if (loop_on) {
        bg = g_theme.solo;
        txt_col = g_theme.bg;
    } else {
        bg = g_theme.panel_bg2;
        txt_col = g_theme.text;
    }

    DrawRectangleRounded((Rectangle){ x, y, w, h }, R_CELL, bg);
    DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_CELL, 1, g_theme.text_dim);

    const char *label = "LOOP";
    int font_size = 13;
    int tw = ui_text_w(label, font_size);
    ui_text(label,
            (int)(x + (w - tw) / 2),
            (int)(y + (h - font_size) / 2),
            font_size, txt_col);

    Vector2 mouse = GetMousePosition();
    Rectangle r = { x, y, w, h };

    if (has_selection &&
        CheckCollisionPointRec(mouse, r) &&
        IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        !g_ctx_open && !g_ti_active && !g_audio_dlg_open) {
        return true;
    }

    return false;
}

// ============ MIDI button ============

bool draw_midi_button(float x, float y, float w, float h)
{
    Color bg = g_midi_learn_mode ? g_theme.vu_red : g_theme.panel_bg2;
    Color txt_col = g_midi_learn_mode ? RAYWHITE : g_theme.text;

    DrawRectangleRounded((Rectangle){ x, y, w, h }, R_CELL, bg);
    DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_CELL, 1, g_theme.text_dim);

    const char *label = "MIDI";
    int font_size = 13;
    int tw = ui_text_w(label, font_size);
    ui_text(label,
            (int)(x + (w - tw) / 2),
            (int)(y + (h - font_size) / 2),
            font_size, txt_col);

    Vector2 mouse = GetMousePosition();
    Rectangle r = { x, y, w, h };

    if (CheckCollisionPointRec(mouse, r) &&
        IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        !g_ctx_open && !g_ti_active && !g_audio_dlg_open) {
        return true;
    }

    return false;
}

// ============ Stop button ============

void draw_stop_button(Session *s, float x, float y, float w, float h)
{
    DrawRectangleRounded((Rectangle){ x, y, w, h }, R_CELL, g_theme.stop_btn);
    DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_CELL, 1, g_theme.text_dim);   /* a frame like Loop/MIDI's */

    float sq = h * 0.32f;
    float cxx = x + w/2;
    float cyy = y + h/2;
    DrawRectangle((int)(cxx - sq/2), (int)(cyy - sq/2),
                  (int)sq, (int)sq,
                  (Color){ 245, 245, 245, 255 });
    if (g_midi_learn_mode) {                               /* 1.4.2: its MIDI binding, like the faders show theirs */
        char b[32]; midimap_describe(MM_STOP_ALL, b, sizeof b);
        bool wait = midimap_learning() == MM_STOP_ALL;
        const char *t = wait ? "press..." : b;
        ui_text(t, (int)(x + 4), (int)(y + 2), 10, wait ? g_theme.selection : RAYWHITE);
    }

    Vector2 mouse = GetMousePosition();
    Rectangle r = { x, y, w, h };
    if (CheckCollisionPointRec(mouse, r) &&
        IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        !g_ctx_open && !g_ti_active && !g_audio_dlg_open) {
        if (g_midi_learn_mode) {                           /* 1.4.2: MIDI learn -> bind a controller button to it */
            midimap_learn(MM_STOP_ALL);
            ui_flash_message("Stop all: press a button on the controller");
        } else {
            session_stop_all(s);
        }
    }
}
// ============ Channel header ============

// ============ Channel header ============

/* ===== 1.4: recording - the record button of a channel and the click on a cell ===== */
static int       g_rin_ch = -1;                  /* the input menu (right click on the record button) */
static Vector2   g_rin_pos;
static bool      g_rin_just;
static Rectangle g_rin_rect;

/* where a new recording goes: the project's "Recordings" folder, or ~/Q-LiM Recordings for a new project */
static void rec_path(Session *s, int ch, char *out, size_t sz)
{
    char dir[1024];
    if (project_has_current()) {
        snprintf(dir, sizeof dir, "%s", project_get_current_path());
        char *sl = strrchr(dir, '/');
#ifdef _WIN32
        char *bs = strrchr(dir, '\\'); if (bs && (!sl || bs > sl)) sl = bs;
#endif
        if (sl) *sl = 0; else snprintf(dir, sizeof dir, ".");
        strncat(dir, "/Recordings", sizeof dir - strlen(dir) - 1);
    } else {
        const char *home = getenv("HOME");
#ifdef _WIN32
        if (!home || !home[0]) home = getenv("USERPROFILE");
#endif
        snprintf(dir, sizeof dir, "%s/Q-LiM Recordings", home && home[0] ? home : ".");
    }
    settings_ensure_dir(dir);
    char nm[NAME_LEN];
    snprintf(nm, sizeof nm, "%s", s->channels[ch].name);
    for (char *q = nm; *q; ++q) if (*q == '/' || *q == '\\' || *q == ':' || *q == '*' || *q == '?' || *q == '"' || *q == '<' || *q == '>' || *q == '|') *q = '_';
    time_t now = time(NULL);
    struct tm tmv;
    struct tm *lt = localtime(&now);                 /* UI thread only */
    if (lt) tmv = *lt; else memset(&tmv, 0, sizeof tmv);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H-%M-%S", &tmv);
    for (int k = 0; k < 100; ++k) {
        if (k == 0) snprintf(out, sz, "%s/%s %s.wav", dir, nm, ts);
        else snprintf(out, sz, "%s/%s %s (%d).wav", dir, nm, ts, k + 1);
        FILE *f = fopen(out, "rb");
        if (!f) return;
        fclose(f);
    }
}

/* a click on the record circle of an empty cell / on a recording cell */
void ui_rec_click(Session *s, int ch, int sc)
{
    char err[200] = "";
    bool midi = fxchain_has_instrument(s, ch);
    int st = session_rec_state(s, ch, sc, NULL);
    if (st == REC_IDLE && !midi && audio_input_channels() <= 0) {
        ui_flash_message("No audio input: choose a sound card with inputs in Edit > Audio (or arm an instrument channel to record MIDI)");
        return;
    }
    char path[1100] = "";
    if (st == REC_IDLE && !midi) rec_path(s, ch, path, sizeof path);
    if (session_rec_cell(s, ch, sc, midi, path, audio_roundtrip_frames(), err, sizeof err) != 0) {
        char m[300]; snprintf(m, sizeof m, "Cannot record: %s", err); ui_flash_message(m);
        return;
    }
    if (st == REC_IDLE) {
        g_selected_ch = ch; g_selected_scene = sc;
        ui_flash_message(midi ? "Recording MIDI - the square in the cell stops it" : "Recording - the square in the cell stops it");
    }
}

/* the record button: a circle (filled red = armed). Click = arm, right click = the inputs / monitoring */
static void rec_arm_button(Session *s, int ch, Rectangle r, Color on_col)
{
    bool armed = session_is_armed(s, ch);
    Vector2 m = GetMousePosition();
    bool hov = CheckCollisionPointRec(m, r) && fx_ui_free() && !g_midi_learn_mode;
    Vector2 c = { r.x + r.width / 2, r.y + r.height / 2 };
    float rad = r.height * 0.36f;
    Color red = { 230, 60, 55, 255 };
    if (armed) {
        DrawCircleV(c, rad + 1.5f, red);
        if (atomic_load(&s->channels[ch].monitor)) DrawCircleLines((int)c.x, (int)c.y, rad + 3.5f, red);
    } else {
        DrawCircleLines((int)c.x, (int)c.y, rad, hov ? red : (Color){ on_col.r, on_col.g, on_col.b, 170 });
        if (hov) DrawCircleV(c, rad * 0.5f, red);
    }
    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) session_toggle_arm(s, ch);
    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) { g_rin_ch = ch; g_rin_pos = m; g_rin_just = true; }
}

/* the menu of the record button: which input(s) and monitoring */
void draw_rec_input_menu(Session *s)
{
    if (g_rin_ch < 0) { g_rin_rect = (Rectangle){ 0, 0, 0, 0 }; return; }
    if (g_rin_ch >= s->channel_count) { g_rin_ch = -1; return; }
    Channel *chp = &s->channels[g_rin_ch];
    int nin = audio_input_channels();
    bool midi = fxchain_has_instrument(s, g_rin_ch);
    char items[40][48]; int kind[40], val[40]; int n = 0;
    if (midi) { snprintf(items[n], 48, "Records MIDI (instrument channel)"); kind[n] = -1; val[n++] = 0; }
    else if (nin <= 0) { snprintf(items[n], 48, "No audio input (Edit > Audio)"); kind[n] = -1; val[n++] = 0; }
    else {
        for (int i = 0; i + 1 < nin && n < 16; i += 2) { snprintf(items[n], 48, "Inputs %d + %d (stereo)", i + 1, i + 2); kind[n] = 1; val[n++] = i; }
        for (int i = 0; i < nin && n < 34; ++i) { snprintf(items[n], 48, "Input %d (mono)", i + 1); kind[n] = 0; val[n++] = i; }
    }
    snprintf(items[n], 48, "Monitor: %s", atomic_load(&chp->monitor) ? "ON (hear the input)" : "off"); kind[n] = 2; val[n++] = 0;
    snprintf(items[n], 48, "%s", session_is_armed(s, g_rin_ch) ? "Disarm" : "Arm for recording"); kind[n] = 3; val[n++] = 0;
    const float W = 250, RH = 22;
    float H = n * RH + 8;
    float px = g_rin_pos.x, py = g_rin_pos.y + 6;
    if (px + W > GetScreenWidth() - 4) px = GetScreenWidth() - 4 - W;
    if (py + H > GetScreenHeight() - 4) py = GetScreenHeight() - 4 - H;
    Rectangle r = { px, py, W, H };
    g_rin_rect = r;
    DrawRectangle((int)px + 4, (int)py + 4, (int)W, (int)H, (Color){ 0, 0, 0, 90 });
    Color bg = g_theme.panel_bg; bg.r = (unsigned char)(bg.r + 8); bg.g = (unsigned char)(bg.g + 8); bg.b = (unsigned char)(bg.b + 8);
    DrawRectangleRec(r, bg);
    GfxHoverLayer(r);                                        /* 2.0.0 */
    DrawRectangleLinesEx(r, 1, g_theme.selection);
    Vector2 m = GetMousePosition();
    int cf = atomic_load(&chp->rec_in_first);
    bool cst = atomic_load(&chp->rec_in_stereo);
    for (int i = 0; i < n; ++i) {
        Rectangle ir = { px + 2, py + 4 + i * RH, W - 4, RH };
        bool cur = (kind[i] == 1 && cst && val[i] == cf) || (kind[i] == 0 && !cst && val[i] == cf);
        bool hov = kind[i] >= 0 && CheckCollisionPointRec(m, ir);
        if (hov) DrawRectangleRec(ir, g_theme.cell_hover);
        if (cur) DrawCircleV((Vector2){ ir.x + 9, ir.y + RH / 2 }, 3, g_theme.selection);
        ui_text(items[i], (int)ir.x + 18, (int)ir.y + 4, 13, kind[i] < 0 ? g_theme.text_dim : g_theme.text);
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_rin_just) {
            if (kind[i] == 1 || kind[i] == 0) session_set_rec_input(s, g_rin_ch, val[i], kind[i] == 1);
            else if (kind[i] == 2) session_set_monitor(s, g_rin_ch, !atomic_load(&chp->monitor));
            else if (kind[i] == 3) session_toggle_arm(s, g_rin_ch);
            g_rin_ch = -1;
            return;
        }
    }
    if (!g_rin_just && (IsKeyPressed(KEY_ESCAPE) ||
        ((IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) && !CheckCollisionPointRec(m, r))))
        g_rin_ch = -1;
    g_rin_just = false;
}

void draw_channel_header(Session *s, int ch,

                                float x, float y, float w, float h)
{
    Channel *chp = &s->channels[ch];
    Color c = palette_raw(chp->color_id);

    /* 2.0.0 Modern: a calm panel with the channel colour as a strip on top (instead of the whole header in colour) */
    Color hc = c;
    if (g_ui_style) {
        hc = g_theme.panel_bg;
        /* the strip follows the rounded corners exactly (like a rounded top border): the whole shape in the channel
         * colour, then the panel over it, 3 px lower - nothing sticks out of the outline */
        float ch = 2.0f * R_CELL + 4.0f < h ? 2.0f * R_CELL + 4.0f : h;   /* only the top: no colour at the bottom edge */
        DrawRectangleRounded((Rectangle){ x, y, w, ch }, R_CELL, c);
        DrawRectangleRounded((Rectangle){ x, y + 3, w, h - 3 }, R_CELL, hc);
        DrawRectangleRounded((Rectangle){ x + 8, y + h / 2 - 4, 8, 8 }, 4.0f, c);
    } else DrawRectangleRounded((Rectangle){ x, y, w, h }, R_CELL, c);
    if (g_theme.outline.a) DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_CELL, 1, g_theme.outline);   /* 2.0.0 */
    if (ch >= 0 && ch < MAX_CHANNELS && g_chsel[ch]) {   /* step 36: selected channel */
        Color tc = ui_text_color_on(hc);
        DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_CELL, 3, g_theme.selection);
        DrawRectangleRounded((Rectangle){ x + 6, y + h - 6, w - 12, 2 }, 1, tc);
    }

    int name_x = (int)(x + (g_ui_style ? 22 : 8));
    int name_w = (int)(w - (g_ui_style ? 30 : 16));
    /* 1.4: the record button (right end of the header) */
    if (w > 60) {
        rec_arm_button(s, ch, (Rectangle){ x + w - 24, y + (h - 18) / 2, 18, 18 }, ui_text_color_on(hc));
        name_w -= 22;
        w -= 22;                                   /* the keyboard icon goes left of it */
    }
    /* 1.2: an instrument channel - a small keyboard on the right (filled = the MIDI keyboard plays it; click = play it) */
    if (fxchain_has_instrument(s, ch) && w > 60) {
        bool armed = session_get_midi_target(s) == ch;
        Color tc = ui_text_color_on(hc);
        Rectangle kb = { x + w - 26, y + (h - 14) / 2, 20, 14 };
        Vector2 mm = GetMousePosition();
        bool kh = CheckCollisionPointRec(mm, kb) && fx_ui_free();
        if (armed) DrawRectangleRec(kb, tc);
        else DrawRectangleLinesEx(kb, 1, kh ? tc : (Color){ tc.r, tc.g, tc.b, 150 });
        Color key = armed ? hc : (Color){ tc.r, tc.g, tc.b, 150 };
        for (int i = 1; i < 4; ++i) DrawLine((int)(kb.x + i * 5), (int)kb.y + 1, (int)(kb.x + i * 5), (int)(kb.y + kb.height - 1), key);
        for (int i = 0; i < 3; ++i) if (i != 1) DrawRectangle((int)(kb.x + 3 + i * 5 + (i > 1 ? 0 : 0)), (int)kb.y + 1, 3, 7, key);
        if (kh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) session_set_midi_target(s, ch);
        name_w -= 26;
    }
    if (name_w > 20) {
        char nm[NAME_LEN];
        snprintf(nm, sizeof(nm), "%s", chp->name);
        while (ui_text_w(nm, 16) > name_w && strlen(nm) > 3) {
            utf8_chop_last(nm);
        }
        ui_text(nm, name_x, (int)(y + 8), 16, ui_text_color_on(hc));
    }
}

// ============ Channel footer ============

/* ===== Where the audio goes (step 24): a button by the faders + a list of the outputs ===== */
bool      g_rt_open = false, g_rt_just_opened = false;
int       g_rt_target = 0;            /* a channel or -1 = master */
Rectangle g_rt_btn, g_rt_rect;

int rt_hw_outputs(void)
{
    int n = audio_total_outputs();                       /* 1.4.21: + the extra cards */
    if (n < 2) n = 2;
    if (n > MAX_HW_OUT) n = MAX_HW_OUT;
    return n;
}
/* masks -> a short label: "Master", "3-4", "5" (mono), "None", "Custom" */
static void rt_label(Session *s, int target, char *out, size_t n)
{
    RouteMask l, r;
    if (target < 0) { l = session_master_route_get(s, 0); r = session_master_route_get(s, 1); }
    else            { l = session_route_get(s, target, 0); r = session_route_get(s, target, 1); }
    if (l == 0 && r == 0) { snprintf(out, n, "None"); return; }
    if (target >= 0 && l == ROUTE_MASTER_L && r == ROUTE_MASTER_R) { snprintf(out, n, "Master"); return; }
    RouteMask hl = target < 0 ? l : (l >> 2), hr = target < 0 ? r : (r >> 2);
    if (target >= 0 && ((l | r) & (ROUTE_MASTER_L | ROUTE_MASTER_R))) { snprintf(out, n, "Custom"); return; }
    for (int k = 0; k < MAX_HW_OUT; ++k) {
        if (hl == RBIT(k) && k + 1 < MAX_HW_OUT && hr == RBIT(k + 1)) { snprintf(out, n, "%d-%d", k + 1, k + 2); return; }
        if (hl == RBIT(k) && hr == RBIT(k)) { snprintf(out, n, "%d", k + 1); return; }
    }
    snprintf(out, n, "Custom");
}
/* the button: shows where it is sent; click -> list */
static void rt_button(Session *s, int target, Rectangle r, int fs)
{
    char lbl[24];
    rt_label(s, target, lbl, sizeof lbl);
    Vector2 m = GetMousePosition();
    bool hov = CheckCollisionPointRec(m, r) && !g_midi_learn_mode;
    bool open = g_rt_open && g_rt_target == target;
    Color bg = open ? g_theme.selection : g_theme.bg;
    if (hov && !open) { bg.r = (unsigned char)(bg.r + 25); bg.g = (unsigned char)(bg.g + 25); bg.b = (unsigned char)(bg.b + 25); }
    DrawRectangleRounded(r, R_SMALL, bg);
    DrawRectangleRoundedLines(r, R_SMALL, 1, open ? g_theme.selection : g_theme.text_dim);
    /* a small arrow "->" on the left, then the label */
    Color tc = open ? g_theme.bg : (strcmp(lbl, "None") == 0 ? g_theme.mute : g_theme.text);
    /* no BeginScissorMode: the button is drawn inside the channel's clipped area and EndScissorMode would break it */
    int tw = ui_text_w(lbl, fs);
    if (tw > r.width - 14 && fs > 9) { fs = 9; tw = ui_text_w(lbl, fs); }
    float ax = r.x + 4, ay = r.y + r.height / 2;
    DrawTriangle((Vector2){ ax, ay - 3 }, (Vector2){ ax, ay + 3 }, (Vector2){ ax + 5, ay }, tc);
    float tx = r.x + 10 + (r.width - 10 - tw) / 2;
    if (tx < r.x + 11) tx = r.x + 11;
    ui_text(lbl, (int)tx, (int)(r.y + (r.height - fs) / 2), fs, tc);
    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_ctx_open && !g_ti_active && !g_audio_dlg_open) {
        if (open) g_rt_open = false;
        else { g_rt_open = true; g_rt_just_opened = true; g_rt_target = target; g_rt_btn = r; }
    }
}

static void draw_fx_strip(Session *s, int ch, float x, float y, float w, float h);


/* step 35: the plugin window (double click or the menu) */
void fx_open_window(Session *s, int ch, int k)
{
    if (ch < 0 || ch >= s->channel_count || k < 0 || k >= s->channels[ch].fx_count) return;
    const ChFx *fx = &s->channels[ch].fx[k];
    QPlug *p = fxchain_find(s, ch, k);
    if (!p) { ui_flash_message(fxchain_slot_failed(s, ch, k) ? "This plugin could not start - see the log" : "The plugin is still loading..."); return; }
    char title[256], err[300] = "";
    snprintf(title, sizeof title, "%s - %s", fx->name, s->channels[ch].name);
    GfxGLRelease();
    int r = qplug_editor_open(p, title, err, (int)sizeof err);
    GfxGLRestore();
    if (r != 0) {
        char msg[400];
        snprintf(msg, sizeof msg, "%s: %s", fx->name, err);
        ui_flash_message(msg);
    }
}

double g_fx_click_t = -1;
int    g_fx_click_ch = -1, g_fx_click_k = -1;

bool fx_ui_free(void)
{
    return !g_ctx_open && !g_ti_active && !g_audio_dlg_open && g_fxp_ch < 0 && g_fxm_ch < 0 && g_rin_ch < 0;
}

/* ===== The FX strip of one channel: the slots (name, format, on/off) and "+" ===== */
static void draw_fx_strip(Session *s, int ch, float x, float y, float w, float h)
{
    Channel *chp = &s->channels[ch];
    Color col = channel_color(chp->color_id);
    Color bg = g_theme.panel_bg;
    float dk = bg.r + bg.g + bg.b > 600 ? 0.95f : 0.8f;      /* 2.0.0: a light theme - only a little darker */
    bg.r = (unsigned char)(bg.r * dk); bg.g = (unsigned char)(bg.g * dk); bg.b = (unsigned char)(bg.b * dk);
    DrawRectangleRounded((Rectangle){ x, y, w, h }, R_PANEL, bg);
    DrawRectangle((int)x, (int)(y + h - R_PANEL), (int)w, (int)R_PANEL, bg);   /* bottom - straight edge (next to the faders) */
    DrawLine((int)x, (int)(y + h - 1), (int)(x + w), (int)(y + h - 1), g_theme.grid_line);
    if (g_theme.outline.a) DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_PANEL, 1, g_theme.outline);   /* 2.0.0 */
    ui_text("FX", (int)x + 6, (int)y + 3, 10, g_theme.text_dim);

    Vector2 m = GetMousePosition();
    bool free = fx_ui_free();
    float list_y = y + 16, plus_h = 18;
    int rows = (int)((h - 16 - plus_h - 6) / FX_ROW_H);
    if (rows < 1) rows = 1;
    int n = chp->fx_count;
    int maxs = n - rows; if (maxs < 0) maxs = 0;
    if (g_fx_scroll[ch] > maxs) g_fx_scroll[ch] = maxs;
    if (g_fx_scroll[ch] < 0) g_fx_scroll[ch] = 0;
    if (free && CheckCollisionPointRec(m, (Rectangle){ x, y, w, h })) {
        float wh = GetMouseWheelMove();
        if (wh != 0.0f && n > rows) g_fx_scroll[ch] -= wh > 0 ? 1 : -1;
    }
    if (n > rows) {                                       /* there are more: a small number at the top right */
        char more[16]; snprintf(more, sizeof more, "%d/%d", n < rows ? n : rows + g_fx_scroll[ch], n);
        int tw = ui_text_w(more, 9);
        ui_text(more, (int)(x + w - tw - 6), (int)y + 4, 9, g_theme.text_dim);
    }
    for (int r = 0; r < rows; ++r) {
        int k = g_fx_scroll[ch] + r;
        if (k >= n) break;
        const ChFx *fx = &chp->fx[k];
        Rectangle rr = { x + 6, list_y + r * FX_ROW_H, w - 12, FX_ROW_H - 2 };
        bool hov = free && CheckCollisionPointRec(m, rr);
        Color sb = g_theme.panel_bg2;
        if (hov) { sb.r = (unsigned char)(sb.r + 14); sb.g = (unsigned char)(sb.g + 14); sb.b = (unsigned char)(sb.b + 14); }
        DrawRectangleRounded(rr, R_SMALL, sb);
        bool failed = !fx->bypass && fxchain_slot_failed(s, ch, k);
        Color bar = fx->bypass ? g_theme.text_dim : (failed ? FX_FAIL_COL : col);
        DrawRectangleRounded((Rectangle){ rr.x + 1, rr.y + 2, 3, rr.height - 4 }, 1.5f, bar);
        /* on/off - a small circle on the right */
        Vector2 pc = { rr.x + rr.width - 8, rr.y + rr.height / 2 };
        Rectangle pw = { pc.x - 7, rr.y, 14, rr.height };
        bool pw_hov = hov && CheckCollisionPointRec(m, pw);
        if (fx->bypass) DrawCircleLines((int)pc.x, (int)pc.y, 4, pw_hov ? g_theme.text : g_theme.text_dim);
        else DrawCircleV(pc, 4, pw_hov ? g_theme.text : col);
        bool loading = !failed && fxchain_slot_loading(s, ch, k);
        char tag[12]; snprintf(tag, sizeof tag, "%s", failed ? "ERROR" : loading ? "starting..." : plugins_fmt_name(plugins_fmt_index(fx->fmt)));
        int tgw = ui_text_w(tag, 9);
        ui_text(tag, (int)(pc.x - 10 - tgw), (int)rr.y + 4, 9, failed ? FX_FAIL_COL : g_theme.text_dim);
        char nm[96]; snprintf(nm, sizeof nm, "%s", fx->name);
        float maxw = (pc.x - 14 - tgw) - (rr.x + 8);
        while (ui_text_w(nm, 12) > maxw && strlen(nm) > 2) utf8_chop_last(nm);
        ui_text(nm, (int)rr.x + 8, (int)rr.y + 2, 12, fx->bypass ? g_theme.text_dim : g_theme.text);
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && pw_hov) session_fx_toggle_bypass(s, ch, k);
        else if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {            /* double click -> the window */
            double t = GetTime();
            if (g_fx_click_ch == ch && g_fx_click_k == k && t - g_fx_click_t < 0.4) { fx_open_window(s, ch, k); g_fx_click_t = -1; }
            else { g_fx_click_t = t; g_fx_click_ch = ch; g_fx_click_k = k; }
        }
        if (qplug_editor_is_open(fxchain_find(s, ch, k)))                     /* open window: a frame */
            DrawRectangleRoundedLines(rr, R_SMALL, 1, col);
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            g_fxm_ch = ch; g_fxm_idx = k; g_fxm_pos = m; g_fxm_just = true;
        }
    }
    /* "+" - opens the plugin list */
    Rectangle pr = { x + 6, y + h - plus_h - 4, w - 12, plus_h };
    bool full = n >= MAX_CH_FX;
    bool phov = free && !full && CheckCollisionPointRec(m, pr);
    Color pb = g_theme.panel_bg2;
    if (phov) { pb.r = (unsigned char)(pb.r + 18); pb.g = (unsigned char)(pb.g + 18); pb.b = (unsigned char)(pb.b + 18); }
    DrawRectangleRounded(pr, R_SMALL, pb);
    DrawRectangleRoundedLines(pr, R_SMALL, 1, phov ? col : g_theme.grid_line);
    const char *pl = full ? "full" : "+";
    int pw2 = ui_text_w(pl, full ? 10 : 16);
    ui_text(pl, (int)(pr.x + (pr.width - pw2) / 2), (int)pr.y + (full ? 4 : 0), full ? 10 : 16, phov ? g_theme.text : g_theme.text_dim);
    if (phov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        g_fxp_ch = ch; g_fxp_pos = (Vector2){ pr.x, pr.y }; g_fxp_filter[0] = 0; g_fxp_scroll = 0; g_fxp_just = true;
        plugins_count();                                 /* the first time - it scans */
    }
}

static bool ci_contains(const char *hay, const char *needle)
{
    if (!needle[0]) return true;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; ++p) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) ++i;
        if (i == n) return true;
    }
    return false;
}

/* ===== The plugin list (after "+") ===== */
void draw_fx_picker(Session *s)
{
    if (g_fxp_ch < 0) { g_fxp_rect = (Rectangle){ 0, 0, 0, 0 }; return; }
    if (g_fxp_ch >= s->channel_count) { g_fxp_ch = -1; return; }
    /* typing = search */
    int cp;
    while ((cp = GetCharPressed()) > 0) {
        size_t l = strlen(g_fxp_filter);
        char u[5] = { 0 };
        if (cp < 0x80) u[0] = (char)cp;
        else if (cp < 0x800) { u[0] = (char)(0xC0 | (cp >> 6)); u[1] = (char)(0x80 | (cp & 0x3F)); }
        else { u[0] = (char)(0xE0 | (cp >> 12)); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[2] = (char)(0x80 | (cp & 0x3F)); }
        if (l + strlen(u) < sizeof g_fxp_filter) { strcat(g_fxp_filter, u); g_fxp_scroll = 0; }
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) { utf8_chop_last(g_fxp_filter); g_fxp_scroll = 0; }

    static int idx[4000];
    int nf = 0, total = plugins_count();
    for (int i = 0; i < total && nf < 4000; ++i) {
        const PluginInfo *pi = plugins_get(i);
        if (ci_contains(pi->name, g_fxp_filter) || ci_contains(plugins_fmt_name(plugins_fmt_index(pi->fmt)), g_fxp_filter)) idx[nf++] = i;
    }
    const float W = 340, RH = 22, HDR = 50;
    int vis = nf < 3 ? 3 : (nf > 14 ? 14 : nf);         /* as many as there are (3..14 rows) */
    float hgt = HDR + vis * RH + 8;
    float px = g_fxp_pos.x, py = g_fxp_pos.y - hgt - 4;          /* above "+" (the panel is at the bottom) */
    if (py < MENU_BAR_H + 4) py = MENU_BAR_H + 4;
    if (px + W > GetScreenWidth() - 4) px = GetScreenWidth() - 4 - W;
    Rectangle r = { px, py, W, hgt };
    g_fxp_rect = r;
    DrawRectangle((int)px + 4, (int)py + 4, (int)W, (int)hgt, (Color){ 0, 0, 0, 90 });
    Color bg = g_theme.panel_bg; bg.r = (unsigned char)(bg.r + 8); bg.g = (unsigned char)(bg.g + 8); bg.b = (unsigned char)(bg.b + 8);
    DrawRectangleRec(r, bg);
    GfxHoverLayer(r);                                        /* 2.0.0 */
    DrawRectangleLinesEx(r, 1, g_theme.selection);

    char title[160];
    snprintf(title, sizeof title, "Add plugin to \"%s\"", s->channels[g_fxp_ch].name);
    ui_text(title, (int)px + 10, (int)py + 7, 13, g_theme.text);
    Rectangle rs = { px + W - 74, py + 5, 64, 18 };               /* Rescan */
    Vector2 m = GetMousePosition();
    bool rs_hov = CheckCollisionPointRec(m, rs);
    DrawRectangleLinesEx(rs, 1, rs_hov ? g_theme.selection : g_theme.grid_line);
    int rtw = ui_text_w("Rescan", 11);
    ui_text("Rescan", (int)(rs.x + (rs.width - rtw) / 2), (int)rs.y + 3, 11, rs_hov ? g_theme.text : g_theme.text_dim);
    Rectangle sf = { px + 10, py + 27, W - 20, 18 };              /* search */
    DrawRectangleRec(sf, g_theme.bg);
    char sline[96];
    snprintf(sline, sizeof sline, "%s%s", g_fxp_filter[0] ? g_fxp_filter : "type to search...", ((int)(GetTime() * 2) & 1) && g_fxp_filter[0] ? "|" : "");
    ui_text(sline, (int)sf.x + 6, (int)sf.y + 3, 12, g_fxp_filter[0] ? g_theme.text : g_theme.text_dim);
    char cnt[32]; snprintf(cnt, sizeof cnt, "%d", nf);
    int cw = ui_text_w(cnt, 10);
    ui_text(cnt, (int)(sf.x + sf.width - cw - 6), (int)sf.y + 4, 10, g_theme.text_dim);

    if (CheckCollisionPointRec(m, r)) {
        float wh = GetMouseWheelMove();
        if (wh != 0.0f) g_fxp_scroll -= (int)(wh * 3);
    }
    if (g_fxp_scroll > nf - vis) g_fxp_scroll = nf - vis;
    if (g_fxp_scroll < 0) g_fxp_scroll = 0;

    int picked = -1;
    int sd = 0, stot = 0;
    bool scanning = plugins_scanning(&sd, &stot, NULL, 0);
    if (total == 0) {
        char ln[80];
        if (scanning) snprintf(ln, sizeof ln, "Checking plugins... %d / %d", sd, stot);
        else snprintf(ln, sizeof ln, "No usable plugins found.");
        ui_text(ln, (int)px + 12, (int)(py + HDR + 6), 13, g_theme.text);
        ui_text("Folders and problems: Edit > Plugins...", (int)px + 12, (int)(py + HDR + 26), 11, g_theme.text_dim);
    } else if (scanning) {
        char ln[64]; snprintf(ln, sizeof ln, "checking %d/%d", sd, stot);
        int lw = ui_text_w(ln, 9);
        ui_text(ln, (int)(px + W - 80 - lw - 6), (int)py + 9, 9, g_theme.text_dim);
    }
    for (int r2 = 0; r2 < vis && g_fxp_scroll + r2 < nf; ++r2) {
        const PluginInfo *pi = plugins_get(idx[g_fxp_scroll + r2]);
        Rectangle row = { px + 4, py + HDR + r2 * RH, W - 8, RH };
        bool hov = CheckCollisionPointRec(m, row);
        if (hov) DrawRectangleRec(row_hl(row), g_theme.cell_hover);
        char nm[PLUGIN_NAME_LEN]; snprintf(nm, sizeof nm, "%s", pi->name);
        const char *fmtn = plugins_fmt_name(plugins_fmt_index(pi->fmt));
        int tgw = ui_text_w(fmtn, 10);
        int itw = pi->inst ? ui_text_w("INST", 9) + 10 : 0;    /* 1.2: instruments are marked */
        while (ui_text_w(nm, 13) > W - 40 - tgw - itw && strlen(nm) > 2) utf8_chop_last(nm);
        ui_text(nm, (int)row.x + 8, (int)row.y + 4, 13, g_theme.text);
        ui_text(fmtn, (int)(row.x + row.width - tgw - 8), (int)row.y + 6, 10, g_theme.text_dim);
        if (pi->inst) {
            float fcol = (float)ui_text_w("VST3", 10);          /* the same column for every format */
            Rectangle ib = { row.x + row.width - fcol - 8 - itw, row.y + 4, (float)itw - 4, RH - 8 };
            DrawRectangleRounded(ib, R_SMALL, g_theme.selection);
            ui_text("INST", (int)ib.x + 3, (int)ib.y + 2, 9, g_theme.bg);
        }
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_fxp_just) picked = idx[g_fxp_scroll + r2];
    }
    if (IsKeyPressed(KEY_ENTER) && nf > 0) picked = idx[g_fxp_scroll];
    if (picked >= 0) {
        const PluginInfo *pi = plugins_get(picked);
        /* 1.2: an instrument goes before the effects (they then process its sound) */
        int k = pi->inst ? session_fx_insert(s, g_fxp_ch, 0, pi->fmt, pi->name, pi->path)
                         : session_fx_add(s, g_fxp_ch, pi->fmt, pi->name, pi->path);
        if (k >= 0 && pi->inst) { session_set_midi_target(s, g_fxp_ch); chsel_only(g_fxp_ch); }
        char msg[200];
        if (k >= 0) snprintf(msg, sizeof msg, "%s added to %s", pi->name, s->channels[g_fxp_ch].name);
        else snprintf(msg, sizeof msg, "This channel already has %d plugins", MAX_CH_FX);
        ui_flash_message(msg);
        g_fxp_ch = -1;
    } else if (rs_hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_fxp_just) {
        plugins_rescan(false);
        ui_flash_message("Checking plugins again...");
    } else if (!g_fxp_just && (IsKeyPressed(KEY_ESCAPE) ||
               ((IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) && !CheckCollisionPointRec(m, r)))) {
        g_fxp_ch = -1;
    }
    g_fxp_just = false;
}

/* ===== Menu on a slot: bypass on/off, remove ===== */
void draw_fx_slot_menu(Session *s)
{
    if (g_fxm_ch < 0) { g_fxm_rect = (Rectangle){ 0, 0, 0, 0 }; return; }
    if (g_fxm_ch >= s->channel_count || g_fxm_idx >= s->channels[g_fxm_ch].fx_count) { g_fxm_ch = -1; return; }
    const ChFx *fx = &s->channels[g_fxm_ch].fx[g_fxm_idx];
    QPlug *qp = fxchain_find(s, g_fxm_ch, g_fxm_idx);
    bool open = qplug_editor_is_open(qp);
    int nfx = s->channels[g_fxm_ch].fx_count;
    int lch = -1, lk = -1, lp = -1;
    bool learning = fxchain_learning(&lch, &lk, &lp);
    if (g_fxm_param >= 0) {                              /* 1.4.14: the menu of one knob */
        int cc = session_fx_midi_get(s, g_fxm_ch, g_fxm_idx, g_fxm_param);
        char forget[48];
        if (cc >= 0) snprintf(forget, sizeof forget, "Forget MIDI (CC %d)", cc); else snprintf(forget, sizeof forget, "Forget MIDI");
        bool me = learning && lch == g_fxm_ch && lk == g_fxm_idx && lp == g_fxm_param;
        const char *ki[2] = { me ? "Cancel MIDI Learn" : "MIDI Learn", forget };
        bool ken[2] = { qp != NULL, cc >= 0 };
        const float KW = 210, KRH = 24, KH = 2 * KRH + 8;
        float kx = g_fxm_pos.x, ky = g_fxm_pos.y - KH;
        if (kx + KW > GetScreenWidth() - 4) kx = GetScreenWidth() - 4 - KW;
        if (ky < 4) ky = 4;
        Rectangle kr = { kx, ky, KW, KH };
        g_fxm_rect = kr;
        DrawRectangle((int)kx + 4, (int)ky + 4, (int)KW, (int)KH, (Color){ 0, 0, 0, 90 });
        Color kbg = g_theme.panel_bg; kbg.r = (unsigned char)(kbg.r + 8); kbg.g = (unsigned char)(kbg.g + 8); kbg.b = (unsigned char)(kbg.b + 8);
        DrawRectangleRec(kr, kbg);
        GfxHoverLayer(kr);                                   /* 2.0.0: Eco - no hover frame from below over it */
        DrawRectangleLinesEx(kr, 1, g_theme.selection);
        Vector2 km = GetMousePosition();
        for (int i = 0; i < 2; ++i) {
            Rectangle ir = { kx + 2, ky + 4 + i * KRH, KW - 4, KRH };
            bool hov = ken[i] && CheckCollisionPointRec(km, ir);
            if (hov) DrawRectangleRec(ir, g_theme.cell_hover);
            ui_text(ki[i], (int)ir.x + 10, (int)ir.y + 5, 14, ken[i] ? g_theme.text : g_theme.text_dim);
            if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_fxm_just) {
                if (i == 0) {
                    if (me) fxchain_learn_cancel();
                    else { fxchain_learn(s, g_fxm_ch, g_fxm_idx, g_fxm_param); ui_flash_message("MIDI Learn: now move a knob / fader on your MIDI controller (Esc = cancel)"); }
                } else {
                    session_fx_midi_set(s, g_fxm_ch, g_fxm_idx, g_fxm_param, -1);
                    ui_flash_message("MIDI forgotten");
                }
                g_fxm_ch = -1; g_fxm_param = -1; g_fxm_just = false;
                return;
            }
        }
        if (!g_fxm_just && (IsKeyPressed(KEY_ESCAPE) ||
            ((IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) && !CheckCollisionPointRec(km, kr))))
            { g_fxm_ch = -1; g_fxm_param = -1; }
        g_fxm_just = false;
        return;
    }
    /* 1.2: the order can be changed (the first device of an instrument channel is the instrument) */
    int nmidi = session_fx_midi_count(s, g_fxm_ch, g_fxm_idx);
    char forget_all[48];
    snprintf(forget_all, sizeof forget_all, nmidi ? "Forget all MIDI (%d)" : "Forget all MIDI", nmidi);
    bool me_slot = learning && lch == g_fxm_ch && lk == g_fxm_idx;
    /* 2.0.0: this plugin in a separate process (safe from crashes) or inside Q-LiM */
    char sep_id[520] = "";
    bool have_id = plugins_resolve(fx->fmt, fx->path, fx->name, sep_id, sizeof sep_id);
    int smode = qplug_separate_mode();
    bool sep_now = qp ? qplug_is_separate(qp) : (have_id && qplug_wants_separate(fx->fmt, sep_id));
    const char *sep_lbl =
        smode == QSEP_ALL ? "Separate process: all plugins (Edit > Plugins)" :
        smode == QSEP_NEVER ? "Separate process: off (Edit > Plugins)" :
        sep_now ? "Run inside Q-LiM again" : "Run in a separate process (safer)";
    const char *items[8] = { open ? "Close window" : "Open window", fx->bypass ? "Turn on" : "Turn off (bypass)",
                             "Move left (earlier)", "Move right (later)", "Remove",
                             me_slot ? "Cancel MIDI Learn" : "MIDI Learn (move a parameter)", forget_all, sep_lbl };
    bool en[8] = { qp && qplug_has_editor(qp), true, g_fxm_idx > 0, g_fxm_idx < nfx - 1, true, qp != NULL, nmidi > 0,
                   have_id && smode == QSEP_AUTO };
    const int NI = 8;
    const float W = 330, RH = 24, H = NI * RH + 8;
    float px = g_fxm_pos.x, py = g_fxm_pos.y - H;
    if (px + W > GetScreenWidth() - 4) px = GetScreenWidth() - 4 - W;
    if (py < 4) py = 4;
    Rectangle r = { px, py, W, H };
    g_fxm_rect = r;
    DrawRectangle((int)px + 4, (int)py + 4, (int)W, (int)H, (Color){ 0, 0, 0, 90 });
    Color bg = g_theme.panel_bg; bg.r = (unsigned char)(bg.r + 8); bg.g = (unsigned char)(bg.g + 8); bg.b = (unsigned char)(bg.b + 8);
    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, 1, g_theme.selection);
    Vector2 m = GetMousePosition();
    for (int i = 0; i < NI; ++i) {
        Rectangle ir = { px + 2, py + 4 + i * RH, W - 4, RH };
        bool hov = en[i] && CheckCollisionPointRec(m, ir);
        if (hov) DrawRectangleRec(ir, g_theme.cell_hover);
        ui_text(items[i], (int)ir.x + 10, (int)ir.y + 5, 14, en[i] ? g_theme.text : g_theme.text_dim);
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_fxm_just) {
            if (i == 0) { if (open) { GfxGLRelease(); qplug_editor_close(qp); GfxGLRestore(); } else fx_open_window(s, g_fxm_ch, g_fxm_idx); }
            else if (i == 1) session_fx_toggle_bypass(s, g_fxm_ch, g_fxm_idx);
            else if (i == 2) session_fx_move(s, g_fxm_ch, g_fxm_idx, g_fxm_idx - 1);
            else if (i == 3) session_fx_move(s, g_fxm_ch, g_fxm_idx, g_fxm_idx + 1);
            else if (i == 4) session_fx_remove(s, g_fxm_ch, g_fxm_idx);
            else if (i == 5) {                                       /* 1.4.14 */
                if (me_slot) fxchain_learn_cancel();
                else {
                    fxchain_learn(s, g_fxm_ch, g_fxm_idx, -1);
                    if (qp && qplug_has_editor(qp) && !open) fx_open_window(s, g_fxm_ch, g_fxm_idx);
                    ui_flash_message("MIDI Learn: move a parameter (plugin window or knob), then a control on your MIDI controller");
                }
            } else if (i == 6) { session_fx_midi_clear(s, g_fxm_ch, g_fxm_idx); ui_flash_message("All MIDI of this plugin forgotten"); }
            else {                                                   /* 2.0.0: the plugin moves (its settings go with it) */
                if (open) { GfxGLRelease(); qplug_editor_close(qp); GfxGLRestore(); }
                fxchain_keep_states(s);
                qplug_separate_set(fx->fmt, sep_id, !sep_now, "chosen by you");
                ui_flash_message(!sep_now ? "The plugin now runs in a separate process - if it crashes, Q-LiM keeps going"
                                          : "The plugin runs inside Q-LiM again");
            }
            g_fxm_ch = -1;
            g_fxm_just = false;
            return;
        }
    }
    if (!g_fxm_just && (IsKeyPressed(KEY_ESCAPE) ||
        ((IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) && !CheckCollisionPointRec(m, r))))
        g_fxm_ch = -1;
    g_fxm_just = false;
}

void draw_channel_footer(Session *s, int ch,
                                float x, float y, float w, float h)
{
    Channel *chp = &s->channels[ch];
    Color col = channel_color(chp->color_id);
    Rectangle whole = { x, y, w, h };                     /* step 36g: the whole channel panel (FX + faders) */
    bool with_fx = g_fx_open && h > FX_STRIP_H + 40;
    if (with_fx) {                                        /* the plugin strip - at the top, the faders below it */
        draw_fx_strip(s, ch, x, y, w, FX_STRIP_H);
        y += FX_STRIP_H;
        h -= FX_STRIP_H;
    }

    DrawRectangleRounded((Rectangle){ x, y, w, h }, R_PANEL, g_theme.panel_bg);
    if (with_fx) DrawRectangle((int)x, (int)y, (int)w, (int)R_PANEL, g_theme.panel_bg);   /* top - straight edge (next to FX) */
    /* a light frame around the whole panel - separates the channel from the rest */
    DrawRectangleRoundedLines(whole, R_PANEL, 1, (g_theme.outline.a ? g_theme.outline : mix_col(g_theme.panel_bg, g_theme.text_dim, 0.45f)));

    if (g_midi_learn_mode) {
        bool armed = atomic_load(&chp->midi_learn_armed);
        if (armed) {
            DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_PANEL, 3, g_theme.mute);
        }
    }

    // ===== PAN knob – top right =====
    /* step 36h: right column (pan, Solo, Mute, output) - everything centred in it, with air up to the frame */
    const float FPAD = 12.0f;
    float knob_d = 38.0f;
    float col_w = knob_d + 14.0f;                         /* the column width = the output button */
    float col_x = x + w - FPAD - col_w;
    float knob_x = col_x + (col_w - knob_d) * 0.5f;
    float knob_y = y + 14;

    float pan = atomic_load(&chp->pan);
    bool pan_armed = g_midi_learn_mode && atomic_load(&chp->pan_midi_learn_armed);

    Vector2 kc = { knob_x + knob_d/2, knob_y + knob_d/2 };
    float krad = knob_d * 0.5f - 2;

    DrawCircleV(kc, krad, g_theme.bg);
    DrawCircleLines((int)kc.x, (int)kc.y, krad, col);

    if (pan_armed) {
        DrawCircleLines((int)kc.x, (int)kc.y, krad + 2, g_theme.mute);
    }

    /* the centre (pan = 0) is UP; left = counter-clockwise, right = clockwise (up to +-135 degrees) */
    float ang = -(float)M_PI / 2.0f + pan * (3.0f * (float)M_PI / 4.0f);

    float lx = kc.x + cosf(ang) * (krad - 4);
    float ly = kc.y + sinf(ang) * (krad - 4);
    DrawLineEx(kc, (Vector2){ lx, ly }, 2.5f, col);

    DrawCircleV(kc, 3, col);

    if (g_midi_learn_mode) {
        int pcc = session_get_pan_midi_cc(s, ch);
        bool p_armed = atomic_load(&chp->pan_midi_learn_armed);

        if (p_armed || pcc >= 0) {
            char text[32];
            if (p_armed) snprintf(text, sizeof(text), "ARMED");
            else snprintf(text, sizeof(text), "P%d", pcc);

            int tw = ui_text_w(text, 10);
            int pad = 3;
            int bw = tw + pad * 2;
            int bh = 14;
            Color bg = p_armed ? g_theme.mute : g_theme.selection;
            DrawRectangle((int)(kc.x - bw/2),
                          (int)(knob_y - bh - 2), bw, bh, bg);
            ui_text(text, (int)(kc.x - tw/2),
                    (int)(knob_y - bh), 10, RAYWHITE);
        }
    }

    Vector2 mouse = GetMousePosition();
    Rectangle knob_hit = { knob_x - 4, knob_y - 4,
                           knob_d + 8, knob_d + 8 };
    bool knob_hover = CheckCollisionPointRec(mouse, knob_hit);

    if (g_midi_learn_mode) {
        if (knob_hover) {
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                session_arm_pan_midi_learn(s, ch);
            }
            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                atomic_store(&chp->pan_midi_cc, -1);
                atomic_store(&chp->pan_midi_learn_armed, false);
                ui_flash_message("PAN MIDI mapping cleared");
            }
        }
    } else {
        static double pan_click_t = 0.0;
        static int    pan_click_ch = -1;
        if (knob_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            double now = GetTime();
            if ((pan_click_ch == ch && now - pan_click_t < DOUBLE_CLICK_TIME) || GetMouseClicks(MOUSE_BUTTON_LEFT) >= 2) {   /* double click = centre */
                session_set_channel_pan(s, ch, 0.0f);
                pan_click_ch = -1;
            } else {
                g_dragging_slider = 2000 + ch;
                pan_click_ch = ch;
                pan_click_t = now;
                g_pan_y0 = mouse.y; g_pan_v0 = g_pan_last = pan; g_pan_sh = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);   /* 1.3.16: grabbed where it is - no jump */
            }
        }
        if (g_dragging_slider == 2000 + ch &&
            IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            session_set_channel_pan(s, ch, pan_drag_value(mouse.y, knob_d));
        }
    }

    // ===== SOLO button =====
    float btn_w = knob_d + 6;
    float btn_h = 22.0f;
    float btn_x = col_x + (col_w - btn_w) * 0.5f;

    float solo_y = knob_y + knob_d + 8;
    Rectangle solo_r = { btn_x, solo_y, btn_w, btn_h };

    bool soloed = atomic_load(&chp->soloed);
    Color solo_bg = soloed ? g_theme.solo : g_theme.panel_bg2;
    DrawRectangleRounded(solo_r, R_CELL, solo_bg);
    DrawRectangleRoundedLines(solo_r, R_CELL, 1, g_theme.text_dim);

    {
        const char *ss = "SOLO";
        int tw = ui_text_w(ss, 11);
        ui_text(ss,
                (int)(solo_r.x + (btn_w - tw) / 2),
                (int)(solo_r.y + (btn_h - 11) / 2),
                11, soloed ? g_theme.bg : g_theme.text_dim);
    }

    if (g_midi_learn_mode) {
        learn_button(s, BTN_LEARN_SOLO, ch, -1, solo_r, session_get_solo_bind(s, ch), true);
    } else if (CheckCollisionPointRec(mouse, solo_r) &&
        IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        !g_ctx_open && !g_ti_active && !g_audio_dlg_open) {
        session_toggle_solo(s, ch);
    }

    // ===== MUTE button =====
    float mute_y = solo_y + btn_h + 4;
    Rectangle mute_r = { btn_x, mute_y, btn_w, btn_h };

    bool muted = atomic_load(&chp->muted);
    Color mute_bg = muted ? g_theme.mute : g_theme.panel_bg2;
    DrawRectangleRounded(mute_r, R_CELL, mute_bg);
    DrawRectangleRoundedLines(mute_r, R_CELL, 1, g_theme.text_dim);

    {
        const char *mm = "MUTE";
        int tw = ui_text_w(mm, 11);
        ui_text(mm,
                (int)(mute_r.x + (btn_w - tw) / 2),
                (int)(mute_r.y + (btn_h - 11) / 2),
                11, muted ? RAYWHITE : g_theme.text_dim);
    }

    if (g_midi_learn_mode) {
        learn_button(s, BTN_LEARN_MUTE, ch, -1, mute_r, session_get_mute_bind(s, ch), true);
    } else if (CheckCollisionPointRec(mouse, mute_r) &&
        IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        !g_ctx_open && !g_ti_active && !g_audio_dlg_open) {
        session_toggle_mute(s, ch);
    }

    // ===== Output (where the channel goes) =====
    {
        float ry = mute_y + btn_h + 6;
        if (ry + 20 <= y + h - 2)
            rt_button(s, ch, (Rectangle){ col_x, ry, col_w, 20 }, 11);
    }

    // ===== Fader =====
    float total_w = VU_LABEL_W + VU_W + 4 + SLIDER_W;
    /* step 36h: the fader block is in the middle of the space to the left of the column (equal air on both sides) */
    float start_x = x + ((col_x - x) - total_w) * 0.5f;
    if (start_x < x + 6) start_x = x + 6;

    /* top: the CLIP lamp is 13 px from the edge (like the pan); bottom: the dB box and 10 px of air */
    float slider_y = y + 20;
    float slider_h = h - 50;
    if (slider_h < 30) slider_h = 30;

    float vu_x = start_x + VU_LABEL_W;
    float lvl_l = atomic_load(&chp->peak_l), lvl_r = atomic_load(&chp->peak_r);
    if (!session_channel_audible(s, ch)) lvl_l = lvl_r = 0.0f;
    draw_vu_meter(vu_x, slider_y, VU_W, slider_h, lvl_l, lvl_r, ch, atomic_exchange(&chp->clipped, false));

    float slider_x = vu_x + VU_W + 4;
    float gain = atomic_load(&chp->gain);
    Color slider_col = col;
    if (!session_channel_audible(s, ch)) {
        slider_col = (Color){ col.r/3, col.g/3, col.b/3, 255 };
    }
    Rectangle sr = { slider_x, slider_y, SLIDER_W, slider_h };
    fader_draw(sr, gain, slider_col, col);
    fader_draw_ghosts(s, ch, sr, gain);

    if (g_midi_learn_mode) {
        Rectangle footer_r = { x, y, w, h };
        if (CheckCollisionPointRec(mouse, footer_r) && !knob_hover &&
            !CheckCollisionPointRec(mouse, solo_r) && !CheckCollisionPointRec(mouse, mute_r)) {
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                g_dragging_slider != ch) {
                session_arm_midi_learn(s, ch);
            }
            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                atomic_store(&chp->midi_cc, -1);
                atomic_store(&chp->midi_learn_armed, false);
                ui_flash_message("MIDI mapping cleared");
            }
        }

        float badge_x = slider_x + SLIDER_W / 2;
        float badge_y = slider_y - 2;
        draw_midi_badge(s, ch, badge_x, badge_y);
    } else {
        float g = gain;
        if (fader_input(ch, sr, &g)) session_set_channel_gain(s, ch, g);
    }

    char db_buf[16];
    fader_db_text(db_buf, sizeof db_buf, atomic_load(&chp->gain));
    fader_db_box(db_buf, slider_x - 2, slider_y + slider_h + 5, SLIDER_W + 4, 15, 12);
}


// ============ MIDI badge ============

void draw_midi_badge(Session *s, int ch, float x, float y)
{
    if (!g_midi_learn_mode) return;

    int cc = -1;
    bool armed = false;

    if (ch == -1) {
        cc = session_get_master_midi_cc(s);
        armed = atomic_load(&s->master_midi_learn_armed);
    } else if (ch >= 0 && ch < s->channel_count) {
        cc = session_get_midi_cc(s, ch);
        armed = atomic_load(&s->channels[ch].midi_learn_armed);
    }

    if (!armed && cc < 0) return;

    char text[32];
    if (armed) {
        snprintf(text, sizeof(text), "ARMED");
    } else {
        snprintf(text, sizeof(text), "CC %d", cc);
    }

    int tw = ui_text_w(text, 11);
    int pad = 4;
    int bw = tw + pad * 2;
    int bh = 16;

    Color bg = armed ? g_theme.mute : g_theme.selection;

    DrawRectangle((int)(x - bw / 2), (int)(y - bh - 4), bw, bh, bg);
    DrawRectangleLines((int)(x - bw / 2), (int)(y - bh - 4), bw, bh,
                       g_theme.bg);

    ui_text(text, (int)(x - tw / 2), (int)(y - bh), 11, RAYWHITE);
}

// ============ Channel footer ============


// ============ Master ============

void draw_master(Session *s, float x, float y, float w, float h)
{
    Color col = g_theme.text;
    if (g_theme.outline.a) col = (Color){ 58, 62, 70, 255 };     /* 2.0.0: light themes - a dark, clear fader */

    DrawRectangleRounded((Rectangle){ x, y, w, h }, R_PANEL, g_theme.panel_bg);

    if (g_midi_learn_mode) {
        bool armed = atomic_load(&s->master_midi_learn_armed);
        if (armed) {
            DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_PANEL, 3, g_theme.mute);
        }
    }

    DrawRectangleRounded((Rectangle){ x, y, w, CLIP_HEADER_H }, R_PANEL, col);
    DrawRectangle((int)x, (int)(y + CLIP_HEADER_H / 2), (int)w, CLIP_HEADER_H - CLIP_HEADER_H / 2, col);   /* bottom - straight edge */
    DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_PANEL, 1, (g_theme.outline.a ? g_theme.outline : mix_col(g_theme.panel_bg, g_theme.text_dim, 0.45f)));   /* step 36g */
    ui_text("Master", (int)(x + 10), (int)(y + 7),
            18, g_theme.bg);
    /* where the master goes — on the right in the header */
    rt_button(s, -1, (Rectangle){ x + w - 64 - 6, y + 5, 64, CLIP_HEADER_H - 10 }, 11);

    bool wide = w >= MASTER_W_WIDE;
    bool mmuted = atomic_load(&s->master_muted);
    float area_w = wide ? w - 74 : w;                    /* with a wide master there is a column on the right */
    float total_w = VU_LABEL_W + VU_W + 4 + SLIDER_W;
    float start_x = x + (area_w - total_w) / 2;
    if (start_x < x + 4) start_x = x + 4;

    if (wide) {
        Vector2 mm = GetMousePosition();
        float cx0 = x + w - 70, cw = 62;
        DrawLine((int)(cx0 - 4), (int)(y + CLIP_HEADER_H + 10), (int)(cx0 - 4), (int)(y + h - 10), g_theme.grid_line);
        /* balance (pan) */
        float knob_d = 40.0f;
        float ky = y + CLIP_HEADER_H + 16;
        Vector2 kc = { cx0 + cw / 2, ky + knob_d / 2 };
        float krad = knob_d * 0.5f - 2;
        float mpan = atomic_load(&s->master_pan);
        DrawCircleV(kc, krad, g_theme.bg);
        DrawCircleLines((int)kc.x, (int)kc.y, krad, col);
        float ang = -(float)M_PI / 2.0f + mpan * (3.0f * (float)M_PI / 4.0f);
        DrawLineEx(kc, (Vector2){ kc.x + cosf(ang) * (krad - 4), kc.y + sinf(ang) * (krad - 4) }, 2.5f, col);
        DrawCircleV(kc, 3, col);
        char pb[16];
        if (fabsf(mpan) < 0.005f) snprintf(pb, sizeof pb, "C");
        else snprintf(pb, sizeof pb, "%s%d", mpan < 0 ? "L" : "R", (int)lroundf(fabsf(mpan) * 100.0f));
        int pw0 = ui_text_w(pb, 11);
        ui_text(pb, (int)(kc.x - pw0 / 2), (int)(ky + knob_d + 2), 11, g_theme.text_dim);
        Rectangle khit = { kc.x - knob_d / 2, ky, knob_d, knob_d };
        const int MPAN_ID = 3000;
        static double mpan_last = 0;
        if (!g_midi_learn_mode && CheckCollisionPointRec(mm, khit) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            !g_ctx_open && !g_ti_active && !g_audio_dlg_open) {
            double now = GetTime();
            if (now - mpan_last < DOUBLE_CLICK_TIME || GetMouseClicks(MOUSE_BUTTON_LEFT) >= 2) { atomic_store(&s->master_pan, 0.0f); mpan_last = 0; }   /* double click = centre */
            else { g_dragging_slider = MPAN_ID; mpan_last = now; g_pan_y0 = mm.y; g_pan_v0 = g_pan_last = mpan; g_pan_sh = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT); }   /* 1.3.16: no jump */
        }
        if (g_dragging_slider == MPAN_ID && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            atomic_store(&s->master_pan, pan_drag_value(mm.y, knob_d));
        }
        /* main MUTE */
        Rectangle mr = { cx0 + 4, ky + knob_d + 20, cw - 8, 26 };
        DrawRectangleRounded(mr, R_CELL, mmuted ? g_theme.mute : g_theme.panel_bg2);
        DrawRectangleRoundedLines(mr, R_CELL, 1, g_theme.text_dim);
        int mw0 = ui_text_w("MUTE", 12);
        ui_text("MUTE", (int)(mr.x + (mr.width - mw0) / 2), (int)(mr.y + 7), 12, mmuted ? RAYWHITE : g_theme.text_dim);
        if (g_midi_learn_mode) {                           /* 1.4.2: its MIDI binding */
            char b[32]; midimap_describe(MM_MASTER_MUTE, b, sizeof b);
            bool wait = midimap_learning() == MM_MASTER_MUTE;
            ui_text(wait ? "press..." : b, (int)(mr.x + 4), (int)(mr.y + 1), 10, wait ? g_theme.selection : g_theme.vu_green);
        }
        if (!g_midi_learn_mode && CheckCollisionPointRec(mm, mr) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            !g_ctx_open && !g_ti_active && !g_audio_dlg_open)
            atomic_store(&s->master_muted, !mmuted);
        else if (g_midi_learn_mode && CheckCollisionPointRec(mm, mr) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                 !g_ctx_open && !g_ti_active && !g_audio_dlg_open) {
            midimap_learn(MM_MASTER_MUTE);                 /* 1.4.2 */
            ui_flash_message("Master MUTE: press a button on the controller");
        }
    }

    float slider_y = y + CLIP_HEADER_H + 20;
    float slider_h = h - CLIP_HEADER_H - 60;
    if (slider_h < 60) slider_h = 60;

    float vu_x = start_x + VU_LABEL_W;
    draw_vu_meter(vu_x, slider_y, VU_W, slider_h,
                  atomic_load(&s->master_peak_l), atomic_load(&s->master_peak_r),
                  MAX_CHANNELS, atomic_exchange(&s->master_clipped, false));

    float slider_x = vu_x + VU_W + 4;
    float gain = atomic_load(&s->master_gain);
    Vector2 mouse = GetMousePosition();
    Rectangle sr = { slider_x, slider_y, SLIDER_W, slider_h };
    const int MASTER_ID = 1000;
    fader_draw(sr, gain, col, col);

    if (g_midi_learn_mode) {
        Rectangle master_r = { x, y, w, h };

        if (CheckCollisionPointRec(mouse, master_r)) {
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                g_dragging_slider != MASTER_ID) {
                session_arm_midi_learn(s, -1);
            }
            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                atomic_store(&s->master_midi_cc, -1);
                atomic_store(&s->master_midi_learn_armed, false);
                ui_flash_message("Master MIDI mapping cleared");
            }
        }

        float badge_x = slider_x + SLIDER_W / 2;
        float badge_y = slider_y - 2;
        draw_midi_badge(s, -1, badge_x, badge_y);
    } else {
        float g = gain;
        if (fader_input(MASTER_ID, sr, &g)) atomic_store(&s->master_gain, g);
    }

    // dB display
    char db_buf[16];
    fader_db_text(db_buf, sizeof db_buf, atomic_load(&s->master_gain));
    fader_db_box(db_buf, slider_x - 6, slider_y + slider_h + 7, SLIDER_W + 12, 19, 14);
    if (atomic_load(&s->master_muted)) {             /* visible even with a narrow master */
        int mw1 = ui_text_w("MUTED", 12);
        float bx = slider_x + SLIDER_W / 2 - mw1 / 2 - 4;
        DrawRectangleRounded((Rectangle){ bx, slider_y - 18, (float)(mw1 + 8), 16 }, R_SMALL, g_theme.mute);
        ui_text("MUTED", (int)bx + 4, (int)(slider_y - 17), 12, RAYWHITE);
    }
}

