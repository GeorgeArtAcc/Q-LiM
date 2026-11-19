/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 2.1: the waveform panel under the clips (drawing, markers, cutting, fades, copy/paste of parts, stems, the clip's settings). Moved out of ui.c. */
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

// ============ Waveform ============
// ============ Waveform ============

// Where the waveform is on screen (for tests): valid after a clip has been drawn
static struct { float x, ruler_y, w, wave_y, wave_h; bool valid; double view0, vis; int ch, sc; } g_wave_geo;

// The fewest frames visible at maximum zoom (below that there is nothing more to see)
#define WAVE_MIN_VISIBLE 400u

// How far a clip with this many frames can be zoomed: as long as at least WAVE_MIN_VISIBLE frames are visible
static float wave_max_zoom(size_t total_frames)
{
    double z = (double)total_frames / (double)WAVE_MIN_VISIBLE;
    if (z < 1.0) z = 1.0;
    if (z > (double)WAVE_ZOOM_MAX) z = (double)WAVE_ZOOM_MAX;
    return (float)z;
}

/* The small panel in the top row of the waveform, under the Master column: [ 124 BPM ] [WARP]
 *   the number = the clip's own tempo (detected; drag = correction; double click = type a number;
 *   2.0.1: Ctrl + double click = back to the detected one)
 *   WARP on  : the clip plays at the MASTER tempo (top bar) - faster/slower WITHOUT changing the pitch (Rubber Band)
 *   WARP off : the clip plays as it is (1.1) */
static void draw_clip_bpm_panel(Session *s, int ch, int sc, float px, float py, float pw, float ph)
{
    DrawRectangle((int)px, (int)py, (int)pw, (int)ph, g_theme.panel_bg2);
    DrawLine((int)px, (int)py, (int)px, (int)(py + ph), g_theme.grid_line);

    bool warp = session_get_clip_warp(s, ch, sc);
    bool busy = session_clip_bpm_busy(s, ch, sc);
    float orig = session_clip_bpm_orig(s, ch, sc);
    float master = session_get_tempo(s);
    Vector2 m = GetMousePosition();
    bool free_ui = !g_ti_active && !g_ctx_open && !g_file_menu_open && !g_edit_menu_open && !g_play_menu_open && !g_audio_dlg_open;

    /* WARP button on the right */
    float btw = (float)ui_text_w("WARP", 12) + 16.0f;
    Rectangle wb = { px + pw - btw - 4, py + 3, btw, ph - 6 };
    bool wh = free_ui && CheckCollisionPointRec(m, wb);
    DrawRectangleRounded(wb, R_SMALL, warp ? g_theme.solo : (wh ? g_theme.cell_hover : g_theme.bg));
    DrawRectangleRoundedLines(wb, R_SMALL, 1, warp ? g_theme.solo : (wh ? g_theme.text : g_theme.text_dim));
    {
        int tw = ui_text_w("WARP", 12);
        ui_text("WARP", (int)(wb.x + (wb.width - tw) / 2), (int)(wb.y + (wb.height - 12) / 2) - 1, 12,
                warp ? g_theme.bg : g_theme.text);
    }
    if (wh) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
    if (wh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && g_vf_id == 0) session_set_clip_warp(s, ch, sc, !warp);

    /* the clip's own tempo */
    Rectangle vr = { px + 2, py, wb.x - px - 6, ph };
    bool editing = g_ti_active && g_ti_numeric == 2;
    float cur = orig;
    bool vhov = free_ui && CheckCollisionPointRec(m, vr);
    if (!editing) {
        if (vhov || g_vf_id == 2) {
            Color hc = g_theme.panel_bg;
            hc.r = (unsigned char)(hc.r + 25); hc.g = (unsigned char)(hc.g + 25); hc.b = (unsigned char)(hc.b + 25);
            DrawRectangle((int)vr.x, (int)vr.y + 2, (int)vr.width, (int)vr.height - 4, hc);
        }
        char num[32], lbl[48];
        Color col = g_theme.text;
        if (cur > 0.0f) {
            fmt_bpm(num, sizeof(num), cur);
            snprintf(lbl, sizeof(lbl), "%s BPM", num);
        } else {
            snprintf(lbl, sizeof(lbl), busy ? "... BPM" : "--- BPM");
            col = g_theme.text_dim;
        }
        int ty = (int)(py + (ph - 14) / 2);
        ui_text(lbl, (int)vr.x + 4, ty, 14, col);
        /* WARP on and another tempo: where it goes */
        if (warp && cur > 0.0f && fabsf(master - cur) > 0.004f) {
            char o[40], on[32];
            fmt_bpm(on, sizeof(on), master);
            snprintf(o, sizeof(o), "> %s", on);
            int lw = ui_text_w(lbl, 14);
            int ow = ui_text_w(o, 11);
            if (vr.x + 4 + lw + 6 + ow <= vr.x + vr.width)
                ui_text(o, (int)vr.x + 4 + lw + 6, (int)(py + (ph - 11) / 2), 11, g_theme.solo);
        }
    }
    float v = cur;
    int res = value_field_input(2, vr, &v, free_ui || g_vf_id == 2);
    if (res == 1) session_set_clip_bpm_value(s, ch, sc, v);
    else if (res == 2 && !(IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))) {   /* double click: type it */
        Rectangle er = { vr.x, py - 1, vr.width > 100 ? vr.width : 100, ph + 2 };
        num_edit_begin(2, ch, sc, cur, er);
    } else if (res == 2) {                                     /* 2.0.1: Ctrl + double click = back to its original tempo */
        float o = session_clip_bpm_orig(s, ch, sc);
        char num[32], msg[96];
        if (o > 0.0f) {
            session_clip_bpm_reset(s, ch, sc);
            fmt_bpm(num, sizeof num, o); snprintf(msg, sizeof msg, "Back to its own tempo: %s BPM", num);
            undo_poke(GetTime());
        } else snprintf(msg, sizeof msg, "This clip has no tempo of its own - double click to type one");   /* 2.1: keep what it has */
        ui_flash_message(msg);
    }
}

/* Smooth position for the "follow" mode: between two audio callbacks we compute it from the clock. */
/* Step 19: a "display clock" — runs evenly with the screen clock and only SLIGHTLY adjusts itself
 * to the position from the audio (which comes in jumps every ~5-20 ms and is a bit "noisy" with Rubber Band).
 * Before, the position was reset on every new audio block -> the waveform jittered back and forth by 1-2 pixels. */
static double smooth_position(Clip *cl, double sr)
{
    static const Clip *sp_clip = NULL;
    static size_t sp_raw = 0;
    static double sp_t = 0.0;            /* when we last saw a new value from the audio */
    static double sp_disp = 0.0;         /* the displayed position (frames) */
    static double sp_last = 0.0;         /* the previous frame */
    size_t raw = atomic_load(&cl->position);
    double now = GetTime();
    bool playing = atomic_load(&cl->playing);
    double rate = sr * session_clip_speed(cl);
    bool fresh = sp_clip != cl;
    if (fresh || raw != sp_raw) { sp_raw = raw; sp_t = now; }
    double age = now - sp_t;
    if (age > 0.1) age = 0.1;
    double target = (double)raw + (playing ? age * rate : 0.0);
    double dt = now - sp_last;
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.1) dt = 0.1;
    sp_last = now;
    if (fresh || !playing) {
        sp_disp = target;                                   /* stopped: exactly where it is */
    } else {
        sp_disp += dt * rate;                               /* evenly forward */
        double err = target - sp_disp;
        if (fabs(err) > 0.15 * sr) sp_disp = target;        /* seek / loop / new start: immediately */
        else {
            double k = dt * 5.0;                            /* smooth correction (~0.2 s) */
            if (k > 1.0) k = 1.0;
            sp_disp += err * k;
        }
    }
    sp_clip = cl;
    double est = sp_disp;
    size_t lim = atomic_load(&cl->loop) ? atomic_load(&cl->loop_end) : atomic_load(&cl->end_marker);
    if (lim > 0 && est > (double)lim && (double)raw <= (double)lim) est = (double)lim;
    return est;
}

/* Vertical band with a fractional position x0..x0+wpx: the edge pixels are partly transparent -> it moves smoothly */
static void aa_vbar(float x0, float wpx, float y, float h, Color c)
{
    float x1 = x0 + wpx;
    int a = (int)floorf(x0), b = (int)floorf(x1);
    if (a == b) {                                   /* narrower than a pixel: one pixel with a weakened colour */
        Color e = c; e.a = (unsigned char)(c.a * (x1 - x0));
        if (e.a) DrawRectangle(a, (int)y, 1, (int)h, e);
        return;
    }
    Color e = c;
    e.a = (unsigned char)(c.a * ((float)(a + 1) - x0));
    if (e.a) DrawRectangle(a, (int)y, 1, (int)h, e);
    if (b - a - 1 > 0) DrawRectangle(a + 1, (int)y, b - a - 1, (int)h, c);
    e.a = (unsigned char)(c.a * (x1 - (float)b));
    if (e.a) DrawRectangle(b, (int)y, 1, (int)h, e);
}

/* Vertical segment yt..yb in column cx with soft (partly transparent) top and bottom ends */
/* ===== 1.3.14: the waveform is drawn by Q-LiM itself into one picture (pixel by pixel, in the same order and with
 * the same transparency as before) and the screen gets that one picture: a still view costs nothing, a moving one
 * (SCROLL, follow) is one cheap pass over the pixels instead of ~10 000-30 000 tiny rectangles ===== */
static GfxImage *g_wimg;
static uint32_t *g_wpx;                               /* ARGB, not premultiplied */
static int       g_wpw, g_wph, g_wpcap;
static int       g_wox, g_woy;                       /* the picture's top left corner, in physical pixels */
static float     g_wsc;                               /* UI unit -> physical pixels */
static bool      g_wb_on;
static int wpx_d(float v) { return (int)lroundf(v * g_wsc); }
static void wb_begin(float x, float y, float w, float h)
{
    g_wsc = GfxScaleNow();
    g_wox = wpx_d(x); g_woy = wpx_d(y);
    g_wpw = wpx_d(x + w) - g_wox; g_wph = wpx_d(y + h) - g_woy;
    if (g_wpw <= 0 || g_wph <= 0) { g_wb_on = false; return; }
    int need = g_wpw * g_wph;
    if (need > g_wpcap) {
        uint32_t *nb = (uint32_t *)realloc(g_wpx, sizeof(uint32_t) * (size_t)need);
        if (!nb) { g_wb_on = false; return; }
        g_wpx = nb; g_wpcap = need;
    }
    memset(g_wpx, 0, sizeof(uint32_t) * (size_t)need);
    g_wb_on = true;
}
/* "over" into the picture: the same as drawing the rectangle on the screen (the picture is laid over the background) */
static void wb_rect(int x, int y, int w, int h, Color c)
{
    if (!g_wb_on) { DrawRectangle(x, y, w, h, c); return; }
    if (c.a == 0) return;
    int x0 = wpx_d((float)x), y0 = wpx_d((float)y), x1 = wpx_d((float)(x + w)), y1 = wpx_d((float)(y + h));
    if (x1 == x0 && w > 0) x1 = x0 + 1;
    if (y1 == y0 && h > 0) y1 = y0 + 1;
    x0 -= g_wox; x1 -= g_wox; y0 -= g_woy; y1 -= g_woy;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_wpw) x1 = g_wpw;
    if (y1 > g_wph) y1 = g_wph;
    if (x1 <= x0 || y1 <= y0) return;
    uint32_t src = ((uint32_t)c.a << 24) | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
    unsigned sa = c.a;
    for (int yy = y0; yy < y1; ++yy) {
        uint32_t *p = g_wpx + (size_t)yy * g_wpw + x0;
        for (int xx = x0; xx < x1; ++xx, ++p) {
            uint32_t d = *p;
            unsigned da = d >> 24;
            if (da == 0 || sa == 255) { *p = src; continue; }
            /* out = src + dst * (1 - src_a), not premultiplied */
            unsigned k = da * (255 - sa);                       /* dst weight * 255 */
            unsigned oa255 = sa * 255 + k;                      /* out alpha * 255 */
            unsigned r = (c.r * sa * 255 + ((d >> 16) & 255) * k + oa255 / 2) / oa255;
            unsigned g = (c.g * sa * 255 + ((d >> 8) & 255) * k + oa255 / 2) / oa255;
            unsigned b = (c.b * sa * 255 + (d & 255) * k + oa255 / 2) / oa255;
            unsigned oa = (oa255 + 127) / 255;
            *p = (oa << 24) | (r << 16) | (g << 8) | b;
        }
    }
}
static void wb_send(void) { if (g_wimg) GfxImageDraw(g_wimg, g_wox, g_woy); }
static void wb_end(void)
{
    if (!g_wb_on) return;
    g_wb_on = false;
    if (!g_wimg) g_wimg = GfxImageNew();
    GfxImageUpload(g_wimg, g_wpx, g_wpw, g_wph);
    wb_send();
}

static void wspan(int cx, float yt, float yb, Color c)
{
    if (c.a == 0 || yb <= yt) return;
    int it = (int)ceilf(yt), ib = (int)floorf(yb);
    if (ib < it) {                                   /* all of it is in one pixel */
        Color e = c; e.a = (unsigned char)(c.a * (yb - yt));
        if (e.a) wb_rect(cx, ib, 1, 1, e);
        return;
    }
    if (ib > it) wb_rect(cx, it, 1, ib - it, c);
    Color e = c;
    e.a = (unsigned char)(c.a * ((float)it - yt));
    if (e.a) wb_rect(cx, it - 1, 1, 1, e);
    e.a = (unsigned char)(c.a * (yb - (float)ib));
    if (e.a) wb_rect(cx, ib, 1, 1, e);
}
static Color wmul(Color c, float k) { c.a = (unsigned char)((float)c.a * k + 0.5f); return c; }

/* A pixel covered by two bars: A with weight wa and B with weight wb (wa + wb = 1).
 * Where they overlap — full colour; where only one is — colour by its weight.
 * This way the height of the peaks does NOT change while sliding (no "pulsing"), and their ends fade smoothly. */
static void wspan2(int cx, float tA, float bA, float wa, float tB, float bB, float wb, Color c)
{
    bool ha = wa > 0.0f && bA > tA, hb = wb > 0.0f && bB > tB;
    if (!ha && !hb) return;
    if (!hb) { wspan(cx, tA, bA, wmul(c, wa)); return; }
    if (!ha) { wspan(cx, tB, bB, wmul(c, wb)); return; }
    float t_in = tA > tB ? tA : tB, b_in = bA < bB ? bA : bB;
    if (t_in >= b_in) {                              /* they do not overlap (rare) */
        wspan(cx, tA, bA, wmul(c, wa));
        wspan(cx, tB, bB, wmul(c, wb));
        return;
    }
    wspan(cx, t_in, b_in, c);
    if (tA < tB) wspan(cx, tA, tB, wmul(c, wa)); else if (tB < tA) wspan(cx, tB, tA, wmul(c, wb));
    if (bA > bB) wspan(cx, bB, bA, wmul(c, wa)); else if (bB > bA) wspan(cx, bA, bB, wmul(c, wb));
}

/* ============ Cutting the clip (step 27b - as in Bitwig) ============
 * Two tools next to BPM:
 *   ARROW:    click = the play position (and selects the part); dragging a part = moves it (Ctrl = copy);
 *             the edges of a part: inwards cuts, outwards reveals the hidden part (stops at the start/end of the file);
 *             the top corners (the small squares) = fade in / fade out; Delete = deletes the selected part.
 *   SCISSORS: click = cuts exactly there.
 * Everything is free (down to a sample); with Shift held it snaps to the beats (the clip's BPM, from the Start marker).
 * The file is NOT touched. The change is applied when the mouse is released; while you drag, the waveform shows the new look. */
enum { TOOL_POINTER = 0, TOOL_CUT = 1 };
static int     g_tool = TOOL_POINTER;
static int     g_ed_ch = -1, g_ed_sc = -1;         /* the clip the selected part belongs to */
static int     g_part = -1;                        /* the selected part */
static int     g_hover_part = -1;                  /* the part under the mouse (for the small squares) */
static bool    g_part_own = false;                 /* g_part was set by us at the last change */
/* dragging */
enum { DR_NONE = 0, DR_PENDING, DR_MOVE, DR_TRIM_L, DR_TRIM_R, DR_FADE_IN, DR_FADE_OUT, DR_XFADE };
static int     g_drag = DR_NONE, g_drag_idx = -1;
static bool    g_drag_copy = false;
static float   g_drag_px = 0;
static double  g_drag_f0 = 0;                      /* the frame under the mouse at the press */
static EditSeg g_drag_seg;                         /* the part as it was at the press */
static EditList *g_preview = NULL;                 /* the new look while dragging */
/* right-click menu on a part */
int     g_em_kind = EM_NONE;
static int     g_em_part = -1;
static bool    g_em_just = false;
static Vector2 g_em_pos;
Rectangle g_em_rect;

static void ed_forget(void)
{
    g_part = -1;
    g_drag = DR_NONE;
    g_em_kind = EM_NONE;
    edit_free(g_preview); g_preview = NULL;
}


/* Snap grid (Shift): the step in frames (0 = no BPM) */
static double ed_grid(Session *s, int ch, int sc, double fpp, double *anchor, double *beat_out)
{
    float bpm = session_clip_bpm_orig(s, ch, sc);
    Clip *cl = &s->channels[ch].clips[sc];
    double sr = cl->wav.sample_rate > 0 ? cl->wav.sample_rate : 48000.0;
    *anchor = 0.0;      /* step 36k: the bars are anchored to the start of the clip - the Start marker only chooses from which bar it plays */
    (void)cl;
    if (bpm <= 1.0f) { if (beat_out) *beat_out = 0; return 0.0; }
    double beat = 60.0 / bpm * sr;
    if (beat_out) *beat_out = beat;
    static const double mult[] = { 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64 };
    for (int i = 0; i < 9; ++i) if (beat * mult[i] / fpp >= 14.0) return beat * mult[i];
    return beat * 64;
}

static bool ed_shift(void) { return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT); }

/* free; with Shift - to the nearest beat */
static int64_t ed_snap(double f, double step, double anchor)
{
    double r = f;
    if (step > 0 && ed_shift()) r = anchor + floor((f - anchor) / step + 0.5) * step;
    return (int64_t)floor(r + 0.5);
}

static void ed_apply(Session *s, EditList *nl, const char *msg)
{
    if (!nl) return;
    if (session_clip_apply_edits(s, g_ed_ch, g_ed_sc, nl, -1) == 0) {
        if (msg) ui_flash_message(msg);
    } else {
        ui_flash_message("Could not change the clip");
    }
}

/* Delete: if a part is selected (clicked) - deletes the part, not the whole clip. main.c asks it first. */
bool ui_edit_take_delete(Session *s)
{
    if (me_take_delete(s)) return true;                /* 1.3: notes selected in the MIDI editor */
    if (g_part < 0 || g_ed_ch < 0 || g_ed_sc < 0 || g_ed_ch >= s->channel_count) return false;
    const EditList *el = session_clip_edits(s, g_ed_ch, g_ed_sc);
    if (!el || g_part >= el->n) { g_part = -1; return false; }
    ed_apply(s, edit_delete(el, g_part), "Part deleted (silence there)");
    g_part = -1;
    return true;
}

/* ===== Step 36k: copying / pasting a part of the waveform ===== */
static EditList *g_seg_clip = NULL;          /* the copied part (at 0) */
static char      g_seg_clip_path[1024];      /* which file it is from (it is pasted only into a clip with the same file) */
static int64_t   g_em_frame = 0;             /* where the right click is (frame) - "Paste" goes there */

static bool ed_copy_part(Session *s, int ch, int sc, int idx)
{
    const EditList *el = session_clip_edits(s, ch, sc);
    EditList *c = el ? edit_extract(el, idx) : NULL;
    if (!c) return false;
    edit_free(g_seg_clip);
    g_seg_clip = c;
    snprintf(g_seg_clip_path, sizeof g_seg_clip_path, "%s", s->channels[ch].clips[sc].path);
    return true;
}

/* pastes at the start of the BAR that contains f (without a tempo - exactly at f); replaces only the space it takes */
static bool ed_paste_at(Session *s, int ch, int sc, double f)
{
    if (!g_seg_clip) return false;
    if (strcmp(g_seg_clip_path, s->channels[ch].clips[sc].path) != 0) {
        ui_flash_message("Paste works in a clip with the same audio file");
        return true;
    }
    const EditList *el = session_clip_edits(s, ch, sc);
    if (!el) return false;
    double ga = 0, beat = 0;
    ed_grid(s, ch, sc, 1.0, &ga, &beat);
    int64_t t = beat > 0 ? (int64_t)llround(floor((f - ga) / (beat * sig_bar())) * beat * sig_bar() + ga) : (int64_t)llround(f);
    EditList *nl = edit_paste(el, g_seg_clip, t);
    if (!nl) { ui_flash_message("Cannot paste there"); return true; }
    g_ed_ch = ch; g_ed_sc = sc;
    ed_apply(s, nl, beat > 0 ? "Pasted (at the start of the bar)" : "Pasted");
    g_part = -1;
    return true;
}

/* Ctrl+C / Ctrl+X: a part selected in the waveform -> copies it (not the clip) */
bool ui_edit_take_copy(Session *s, bool cut)
{
    if (me_take_copy(s, cut)) return true;             /* 1.3.2: notes selected in the MIDI editor */
    if (g_part < 0 || g_ed_ch < 0 || g_ed_sc < 0 || g_ed_ch >= s->channel_count) return false;
    const EditList *el = session_clip_edits(s, g_ed_ch, g_ed_sc);
    if (!el || g_part >= el->n) { g_part = -1; return false; }
    if (!ed_copy_part(s, g_ed_ch, g_ed_sc, g_part)) return false;
    if (cut) { ed_apply(s, edit_delete(el, g_part), "Part cut"); g_part = -1; }
    else ui_flash_message("Part copied - Ctrl+V over the waveform pastes it at the bar under the mouse");
    return true;
}

/* Ctrl+V: there is a copied part and the mouse is over the waveform -> pastes it there (at the bar under the mouse) */
bool ui_edit_take_paste(Session *s)
{
    if (me_take_paste(s)) return true;                 /* 1.3.2: copied notes, the MIDI editor in use */
    if (!g_seg_clip || !g_wave_geo.valid) return false;
    Vector2 m = GetMousePosition();
    if (!CheckCollisionPointRec(m, (Rectangle){ g_wave_geo.x, g_wave_geo.ruler_y, g_wave_geo.w,
                                                g_wave_geo.wave_y + g_wave_geo.wave_h - g_wave_geo.ruler_y })) return false;
    int ch = g_wave_geo.ch, sc = g_wave_geo.sc;
    if (ch < 0 || sc < 0 || ch >= s->channel_count || sc >= s->scene_count) return false;
    double f = g_wave_geo.view0 + (double)(m.x - g_wave_geo.x) / (double)g_wave_geo.w * g_wave_geo.vis;
    return ed_paste_at(s, ch, sc, f);
}

/* icons (drawn, no font) */
void icon_pointer(float cx, float cy, Color c)
{
    DrawTriangle((Vector2){ cx - 4, cy - 7 }, (Vector2){ cx - 4, cy + 5 }, (Vector2){ cx + 5, cy + 1 }, c);
    DrawLineEx((Vector2){ cx - 1, cy + 2 }, (Vector2){ cx + 2, cy + 7 }, 2.0f, c);
}
static void icon_scissors(float cx, float cy, Color c)
{
    DrawLineEx((Vector2){ cx - 5, cy - 7 }, (Vector2){ cx + 3, cy + 3 }, 1.6f, c);
    DrawLineEx((Vector2){ cx + 5, cy - 7 }, (Vector2){ cx - 3, cy + 3 }, 1.6f, c);
    DrawCircleLines((int)(cx - 4), (int)(cy + 5), 2.6f, c);
    DrawCircleLines((int)(cx + 4), (int)(cy + 5), 2.6f, c);
}

/* 2.0: the pencil (the MIDI editor) */
void icon_pencil(float cx, float cy, Color c)
{
    DrawLineEx((Vector2){ cx - 5, cy + 6 }, (Vector2){ cx + 5, cy - 4 }, 3.2f, c);
    DrawTriangle((Vector2){ cx - 7, cy + 8 }, (Vector2){ cx - 3.5f, cy + 7.5f }, (Vector2){ cx - 6.5f, cy + 4.5f }, c);
    DrawLineEx((Vector2){ cx + 4, cy - 6 }, (Vector2){ cx + 7, cy - 3 }, 1.6f, c);
}

#define EDIT_TOOLS_W (3 * 26 + 4)                 /* 1.3.15: + "fit Start..End" */
const char *g_tool_tip;
float g_tool_tip_x, g_tool_tip_y;
/* 1.3.15: the icon of "fit": |<->| */
static void icon_fit(float cx, float cy, Color c)
{
    DrawRectangle((int)(cx - 8), (int)(cy - 6), 2, 12, c);
    DrawRectangle((int)(cx + 6), (int)(cy - 6), 2, 12, c);
    DrawRectangle((int)(cx - 4), (int)cy, 8, 1, c);
    DrawTriangle((Vector2){ cx - 6, cy + 0.5f }, (Vector2){ cx - 3, cy + 3.5f }, (Vector2){ cx - 3, cy - 2.5f }, c);
    DrawTriangle((Vector2){ cx + 6, cy + 0.5f }, (Vector2){ cx + 3, cy - 2.5f }, (Vector2){ cx + 3, cy + 3.5f }, c);
}
/* buttons in the waveform's header row (to the left of BPM): pointer, scissors, and (1.3.15) "fit Start..End".
 * Returns true when "fit" was clicked. */
static bool draw_edit_tools(float bx, float by, float bh)
{
    static const char *tips[3] = {
        "Pointer: click = play position | drag a part = move (Ctrl = copy) | drag its edges = trim | top corners = fades | Del = delete part",
        "Scissors: click to cut exactly there (hold Shift to snap to beats)",
        "Fit: show the waveform exactly from the Start to the End marker" };
    Vector2 m = GetMousePosition();
    bool free_ui = !g_ti_active && !g_ctx_open && !g_file_menu_open && !g_edit_menu_open && !g_play_menu_open && !g_audio_dlg_open;
    if (free_ui && g_em_kind == EM_NONE && !g_rt_open && IsKeyPressed(KEY_ESCAPE) && g_tool != TOOL_POINTER)
        g_tool = TOOL_POINTER;                              /* Esc: back to the arrow */
    int hov_i = -1;
    bool fit = false;
    for (int i = 0; i < 3; ++i) {
        Rectangle r = { bx + i * 26, by + 2, 24, bh - 4 };
        bool hov = free_ui && CheckCollisionPointRec(m, r);
        bool on = i < 2 && g_tool == i;
        DrawRectangleRec(r, on ? g_theme.selection : (hov ? g_theme.cell_hover : g_theme.panel_bg2));
        Color ic = on ? g_theme.bg : g_theme.text;
        float cx = r.x + r.width / 2, cy = r.y + r.height / 2;
        if (i == 0) icon_pointer(cx, cy, ic); else if (i == 1) icon_scissors(cx, cy, ic); else icon_fit(cx, cy, ic);
        if (hov) {
            hov_i = i;
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if (i < 2) { g_tool = i; g_em_kind = EM_NONE; }
                else fit = true;
            }
        }
    }
    g_tool_tip = hov_i >= 0 ? tips[hov_i] : NULL;          /* drawn last (above the ruler) */
    g_tool_tip_x = bx + 3 * 26; g_tool_tip_y = by + bh + 2;
    return fit;
}
static void draw_tool_tip(void)
{
    if (!g_tool_tip) return;
    int tw = ui_text_w(g_tool_tip, 12);
    float tx = g_tool_tip_x - tw - 12;
    if (tx < 4) tx = 4;
    DrawRectangle((int)tx - 6, (int)g_tool_tip_y, tw + 12, 18, g_theme.panel_bg);
    DrawRectangleLines((int)tx - 6, (int)g_tool_tip_y, tw + 12, 18, g_theme.grid_line);
    ui_text(g_tool_tip, (int)tx, (int)g_tool_tip_y + 2, 12, g_theme.text);
    g_tool_tip = NULL;
}

/* right-click menu on a part (drawn on top) */
void draw_edit_menu(Session *s)
{
    if (g_em_kind == EM_NONE) { g_em_rect = (Rectangle){ 0, 0, 0, 0 }; return; }
    const EditList *el = (g_ed_ch >= 0 && g_ed_ch < s->channel_count && g_ed_sc >= 0 && g_ed_sc < s->scene_count)
                         ? session_clip_edits(s, g_ed_ch, g_ed_sc) : NULL;
    if (!el) { g_em_kind = EM_NONE; return; }
    enum { A_DEL, A_DUP, A_NOFADE, A_NOXF, A_RESTORE, A_COPY, A_CUT, A_PASTE };
    struct { int a; const char *t; } it[8];
    int n = 0;
    bool edited = !edit_is_identity(el);
    bool part_ok = g_em_part >= 0 && g_em_part < el->n;
    int64_t fin = 0, fout = 0;
    if (part_ok) edit_seg_fades(&el->seg[g_em_part], &fin, &fout);
    if (part_ok && edited) { it[n].a = A_DEL; it[n++].t = "Delete this part"; }
    if (part_ok) { it[n].a = A_COPY; it[n++].t = "Copy  (Ctrl+C)"; }
    if (part_ok && edited) { it[n].a = A_CUT; it[n++].t = "Cut  (Ctrl+X)"; }
    if (g_seg_clip) { it[n].a = A_PASTE; it[n++].t = "Paste here (at this bar)"; }
    if (part_ok) { it[n].a = A_DUP; it[n++].t = "Duplicate (right after it)"; }
    if (part_ok && (fin > 0 || fout > 0)) { it[n].a = A_NOFADE; it[n++].t = "Remove fades"; }
    bool has_xf = part_ok && (el->seg[g_em_part].xl + el->seg[g_em_part].xr > 0 ||
                              (g_em_part + 1 < el->n && el->seg[g_em_part + 1].xl + el->seg[g_em_part + 1].xr > 0));
    if (has_xf) { it[n].a = A_NOXF; it[n++].t = "Remove crossfades"; }
    if (edited) { it[n].a = A_RESTORE; it[n++].t = "Restore original clip"; }
    if (n == 0) { g_em_kind = EM_NONE; return; }
    const float row = 24, w = 230, h = n * row + 8;
    float x = g_em_pos.x, y = g_em_pos.y;
    if (x + w > GetScreenWidth() - 4) x = GetScreenWidth() - 4 - w;
    if (y + h > GetScreenHeight() - 4) y = g_em_pos.y - h;
    if (y < 4) y = 4;
    Rectangle r = { x, y, w, h };
    g_em_rect = r;
    DrawRectangle((int)x + 4, (int)y + 4, (int)w, (int)h, (Color){ 0, 0, 0, 90 });
    Color bg = g_theme.panel_bg; bg.r = (unsigned char)(bg.r + 8); bg.g = (unsigned char)(bg.g + 8); bg.b = (unsigned char)(bg.b + 8);
    DrawRectangleRec(r, bg);
    GfxHoverLayer(r);                                        /* 2.0.0 */
    DrawRectangleLinesEx(r, 1, g_theme.selection);
    Vector2 m = GetMousePosition();
    for (int i = 0; i < n; ++i) {
        Rectangle ir = { x + 2, y + 4 + i * row, w - 4, row };
        bool hov = CheckCollisionPointRec(m, ir);
        if (hov) DrawRectangleRec(ir, g_theme.cell_hover);
        ui_text(it[i].t, (int)ir.x + 12, (int)ir.y + 5, 14, g_theme.text);
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_em_just) {
            switch (it[i].a) {
            case A_DEL:
                ed_apply(s, edit_delete(el, g_em_part), "Part deleted (silence there)");
                g_part = -1;
                break;
            case A_DUP: {
                const EditSeg *sg = &el->seg[g_em_part];
                EditList *nl = edit_copy(el, g_em_part, sg->t + sg->len);
                if (nl) ed_apply(s, nl, "Part duplicated"); else ui_flash_message("No room after this part (end of the clip)");
                break;
            }
            case A_COPY:
                if (ed_copy_part(s, g_ed_ch, g_ed_sc, g_em_part)) ui_flash_message("Part copied");
                break;
            case A_CUT:
                if (ed_copy_part(s, g_ed_ch, g_ed_sc, g_em_part)) { ed_apply(s, edit_delete(el, g_em_part), "Part cut"); g_part = -1; }
                break;
            case A_PASTE:
                ed_paste_at(s, g_ed_ch, g_ed_sc, (double)g_em_frame);
                break;
            case A_NOFADE: ed_apply(s, edit_set_fades(el, g_em_part, 0, 0), "Fades removed"); break;
            case A_NOXF: {
                EditList *a = edit_set_xfade(el, g_em_part, 0, 0);
                EditList *b = edit_set_xfade(a ? a : el, g_em_part + 1, 0, 0);
                if (b) { edit_free(a); a = b; }
                if (a) ed_apply(s, a, "Crossfades removed");
                break;
            }
            case A_RESTORE:
                if (session_clip_set_edits_text(s, g_ed_ch, g_ed_sc, NULL) == 0) ui_flash_message("Original clip restored");
                g_part = -1;
                break;
            }
            g_em_kind = EM_NONE;
        }
    }
    if (!g_em_just && (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) &&
        !CheckCollisionPointRec(m, r))
        g_em_kind = EM_NONE;
    if (IsKeyPressed(KEY_ESCAPE)) g_em_kind = EM_NONE;
    g_em_just = false;
}

/* The mouse over the waveform: what is under it (for the arrow) */
enum { HZ_NONE = 0, HZ_BODY, HZ_EDGE_L, HZ_EDGE_R, HZ_FADE_IN, HZ_FADE_OUT, HZ_XFADE };
#define FADE_HANDLE 8.0f
#define FADE_Y      24.0f   /* step 36i: the fade squares are BELOW the Start/End triangles (those are at the top) */
/* a seam that makes sense to crossfade: butted parts that do NOT continue the file into each other
 * (a plain cut without a move sounds as before - there the edges stay available for trimming) */
static bool ed_seam(const EditList *e, int i)
{
    return edit_joined(e, i) && (e->seg[i - 1].src + e->seg[i - 1].len != e->seg[i].src ||
                                 e->seg[i].xl + e->seg[i].xr > 0);
}

static int ed_hit(const EditList *e, double view0, double fpp, float x, float wave_y, Vector2 m, double mf,
                  bool near_marker, int *idx)
{
    *idx = -1;
    if (!e) return HZ_NONE;
    #define FX(f) (x + (float)(((double)(f) - view0) / fpp))
    bool top = m.y >= wave_y + FADE_Y - 5.0f && m.y <= wave_y + FADE_Y + FADE_HANDLE + 5.0f;
    /* 1) the fade squares (at the top, in the corners of the part) */
    if (top) {
        for (int i = 0; i < e->n; ++i) {
            const EditSeg *sg = &e->seg[i];
            int64_t fin, fout;
            edit_seg_fades(sg, &fin, &fout);
            float px0 = FX(sg->t), px1 = FX(sg->t + sg->len);
            if (px1 - px0 < 3 * FADE_HANDLE) continue;
            float hin = FX(sg->t + fin) + FADE_HANDLE * 0.5f, hout = FX(sg->t + sg->len - fout) - FADE_HANDLE * 0.5f;
            if (m.x > px0 + 2 && fabsf(m.x - hin) <= FADE_HANDLE) { *idx = i; return HZ_FADE_IN; }
            if (m.x < px1 - 2 && fabsf(m.x - hout) <= FADE_HANDLE) { *idx = i; return HZ_FADE_OUT; }
        }
    }
    if (near_marker) return HZ_NONE;                       /* the Start/End/Loop markers take priority */
    /* 2) a seam between butted parts (or its crossfade zone): dragging = crossfade (as in Bitwig) */
    for (int i = 1; i < e->n; ++i) {
        if (!ed_seam(e, i)) continue;
        const EditSeg *sg = &e->seg[i];
        float a = FX(sg->t - sg->xl), b = FX(sg->t + sg->xr);
        if (m.x >= a - 5.0f && m.x <= b + 5.0f) { *idx = i; return HZ_XFADE; }
    }
    /* 3) the edges */
    for (int i = 0; i < e->n; ++i) {
        const EditSeg *sg = &e->seg[i];
        float px0 = FX(sg->t), px1 = FX(sg->t + sg->len);
        if (fabsf(m.x - px0) <= 5.0f && m.x >= px0 - 5 && px1 - px0 > 12) { *idx = i; return HZ_EDGE_L; }
        if (fabsf(m.x - px1) <= 5.0f && px1 - px0 > 12) { *idx = i; return HZ_EDGE_R; }
    }
    #undef FX
    /* 3) the body */
    int k = edit_find(e, (int64_t)mf);
    if (k >= 0) { *idx = k; return HZ_BODY; }
    return HZ_NONE;
}

/* fingerprint of the list (for the detailed waveform cache; the pointer may be reused after free) */
static uint64_t el_hash(const EditList *e)
{
    if (!e) return 0;
    uint64_t h = 1469598103934665603ull ^ (uint64_t)e->len;
    const unsigned char *p = (const unsigned char *)e->seg;
    for (size_t i = 0; i < (size_t)e->n * sizeof(EditSeg); ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h ^ (uint64_t)e->n;
}

/* Detailed waveform (deep zoom) for a cut clip: every bar reads from its own part of the file */
static bool ed_detail(const Clip *cl, const EditList *e, double view0, double fpp, int ncol, PeakVal *buf)
{
    int i = 0;
    bool any = false;
    while (i < ncol) {
        double f = view0 + i * fpp;
        int k = edit_find(e, (int64_t)floor(f));
        if (k < 0) { memset(&buf[i], 0, sizeof buf[i]); ++i; continue; }
        const EditSeg *sg = &e->seg[k];
        int j = i;                                           /* the columns that are in the same part */
        while (j < ncol && view0 + j * fpp < (double)(sg->t + sg->len)) ++j;
        if (j == i) j = i + 1;
        int64_t s0i = sg->src + (int64_t)floor(f - (double)sg->t);
        size_t src0 = (size_t)(s0i < 0 ? 0 : s0i);
        if (!peaks_detail_val(cl->peaks, src0, fpp, j - i, buf + i)) return false;
        for (int q = i; q < j; ++q) {
            int64_t tq = (int64_t)(view0 + (q + 0.5) * fpp);
            float g = edit_gain(e, k, tq - sg->t) * edit_xfade_base_gain(e, tq);
            if (g < 0.999f) { buf[q].lmin *= g; buf[q].lmax *= g; buf[q].rmin *= g; buf[q].rmax *= g; buf[q].lrms *= g; buf[q].rrms *= g; }
            int64_t xs; float xg; PeakVal xv;                 /* crossfade: the other audio too */
            if (edit_xfade_extra(e, tq, &xs, &xg) && xs >= 0 && peaks_detail_val(cl->peaks, (size_t)xs, fpp, 1, &xv)) {
                buf[q].lmin += xv.lmin * xg; buf[q].lmax += xv.lmax * xg; buf[q].rmin += xv.rmin * xg; buf[q].rmax += xv.rmax * xg;
                buf[q].lrms = sqrtf(buf[q].lrms * buf[q].lrms + xv.lrms * xg * xv.lrms * xg);
                buf[q].rrms = sqrtf(buf[q].rrms * buf[q].rrms + xv.rrms * xg * xv.rrms * xg);
            }
        }
        any = true;
        i = j;
    }
    return any || ncol == 0;
}

/* ===== 1.4.14 / 1.4.17: the separated parts of a clip: a strip at the bottom of the waveform =====
 * [■ VOCALS [level] M S] [■ DRUMS [level] M S] ...  [Split to clips] [Export...]   - or the separation's progress.
 * The level field: drag / wheel / double click = 0 dB; right click = MIDI Learn (again = forget the CC). */
/* the vocals' colour: one that stands out from the channel's (warm channel -> cyan, cool channel -> orange) */
static Color vocal_color(Color ch)
{
    return ch.r >= ch.b ? (Color){ 80, 210, 255, 255 } : (Color){ 255, 120, 70, 255 };
}
/* each kind of part has its own colour (the same in the strip, the waveform and the "carried" clips) */
static Color stem_color(const char *name, Color chan)
{
    if (!name) return g_theme.text;
    if (!strcmp(name, "Vocals")) return vocal_color(chan);
    if (!strcmp(name, "Drums"))  return (Color){ 240, 200, 60, 255 };
    if (!strcmp(name, "Bass"))   return (Color){ 170, 110, 250, 255 };
    if (!strcmp(name, "Guitar")) return (Color){ 90, 210, 120, 255 };
    if (!strcmp(name, "Piano"))  return (Color){ 250, 120, 190, 255 };
    Color c = g_theme.text_dim; c.a = 255;                      /* Instrumental / Other: neutral */
    return c;
}
static void stem_label(const char *name, int n, char *out, size_t sz)
{
    if (n == 2 && !strcmp(name, "Instrumental")) { snprintf(out, sz, "INSTR."); return; }
    size_t i = 0;
    for (; name[i] && i + 1 < sz && i < 8; ++i) out[i] = (char)toupper((unsigned char)name[i]);
    out[i] = 0;
}
static double g_stem_apply_t = -1.0;              /* a level drag: applied (re-buffered) at most ~8 times a second */
static bool   g_stem_apply_pending = false;

/* the layout of the strip (the same for its size and for drawing it) */
#define STEM_ROW_H 26.0f
typedef struct { Rectangle lab, lev, m, so; } StemCtl;
typedef struct { int n; StemCtl c[STEM_MAX]; Rectangle btn[2]; float w, h; } StemLayout;
static void stem_layout(Session *s, int ch, int sc, float x0, float y0, float maxw, StemLayout *L)
{
    memset(L, 0, sizeof *L);
    L->n = session_clip_stem_count(s, ch, sc);
    float cx = x0 + 8, cy = y0 + 3, rowr = x0 + maxw - 6, wmax = 0;
    float fh = STEM_ROW_H - 6;
    for (int k = 0; k < L->n; ++k) {
        char lb[16];
        stem_label(session_clip_stem_name(s, ch, sc, k), L->n, lb, sizeof lb);
        float lw = 10 + (float)ui_text_w(lb, 11) + 6;
        float ew = lw + 80 + 3 + 22 + 3 + 22;
        if (cx > x0 + 8 && cx + ew > rowr) { cx = x0 + 8; cy += STEM_ROW_H; }
        L->c[k].lab = (Rectangle){ cx, cy, lw, fh };
        L->c[k].lev = (Rectangle){ cx + lw, cy, 80, fh };
        L->c[k].m   = (Rectangle){ cx + lw + 83, cy, 22, fh };
        L->c[k].so  = (Rectangle){ cx + lw + 108, cy, 22, fh };
        cx += ew + 12;
        if (cx - x0 > wmax) wmax = cx - x0;
    }
    static const char *const BL[2] = { "Split to clips", "Export..." };
    for (int b = 0; b < 2; ++b) {
        float bw = (float)ui_text_w(BL[b], 12) + 16;
        if (cx > x0 + 8 && cx + bw > rowr) { cx = x0 + 8; cy += STEM_ROW_H; }
        L->btn[b] = (Rectangle){ cx, cy, bw, fh };
        cx += bw + 6;
        if (cx - x0 > wmax) wmax = cx - x0;
    }
    L->w = wmax + 2;
    L->h = cy - y0 + STEM_ROW_H;
}
static Rectangle stem_strip_rect(Session *s, float x, float wave_y, float w, float wave_h)
{
    bool st = session_clip_has_stems(s, g_selected_ch, g_selected_scene);
    float p; char t[8];
    bool busy = !st && ui_separation_progress(g_selected_ch, g_selected_scene, &p, t, sizeof t);
    if (!st && !busy) return (Rectangle){ 0, 0, 0, 0 };
    if (!st) {
        float sw = 430.0f;
        if (sw > w - 16) sw = w - 16;
        return (Rectangle){ x + 8, wave_y + wave_h - 32, sw, 28 };
    }
    StemLayout L;
    stem_layout(s, g_selected_ch, g_selected_scene, 0, 0, w - 16, &L);
    float h = L.h + 2;
    if (h > wave_h - 4) h = wave_h - 4;
    return (Rectangle){ x + 8, wave_y + wave_h - h - 4, L.w < w - 16 ? L.w : w - 16, h };
}

/* ===== 1.4.17: "Split to clips" - the parts hang on the mouse until they are dropped (one under another) ===== */
int      g_carry_n = 0;
static ClipPart g_carry[STEM_MAX];
static Color    g_carry_col[STEM_MAX];
static char     g_carry_lab[STEM_MAX][STEM_NAME_LEN];
int      g_carry_hc = -1, g_carry_hs = -1;          /* the cell under the mouse (this frame) */
static bool     g_carry_eat = false;                       /* the click that dropped them: nothing else sees it */
static void carry_cancel(void)
{
    for (int k = 0; k < g_carry_n; ++k) clip_part_free(&g_carry[k]);
    g_carry_n = 0;
}
static void carry_start(Session *s, int ch, int sc)
{
    carry_cancel();
    int n = session_clip_stem_count(s, ch, sc);
    bool any = false;
    for (int k = 0; k < n; ++k) if (session_clip_stem_audible(s, ch, sc, k)) any = true;
    Color cc = channel_color(s->channels[ch].color_id);
    for (int k = 0; k < n; ++k) {
        if (any && !session_clip_stem_audible(s, ch, sc, k)) continue;   /* muted / not soloed parts stay behind */
        if (session_clip_capture_part(s, ch, sc, k, &g_carry[g_carry_n]) != 0) continue;
        g_carry_col[g_carry_n] = stem_color(session_clip_stem_name(s, ch, sc, k), cc);
        snprintf(g_carry_lab[g_carry_n], sizeof g_carry_lab[0], "%s", session_clip_stem_name(s, ch, sc, k));
        ++g_carry_n;
    }
    if (g_carry_n == 0) { ui_flash_message("Could not split"); return; }
    char m[200];
    snprintf(m, sizeof m, "%d part%s on the mouse: click an empty cell - they go one under another. Right click / Esc: cancel",
             g_carry_n, g_carry_n == 1 ? "" : "s");
    ui_flash_message(m);
}
/* can the parts go to channel ch from scene sc down? (only into empty cells; missing scenes are added) */
static bool carry_fits(Session *s, int ch, int sc)
{
    if (ch < 0 || ch >= s->channel_count || sc < 0 || sc + g_carry_n > MAX_SCENES) return false;
    for (int k = 0; k < g_carry_n; ++k)
        if (sc + k < s->scene_count && atomic_load(&s->channels[ch].clips[sc + k].has_clip)) return false;
    return true;
}
/* the start of a frame: a click drops (or cancels) them */
void carry_input(Session *s)
{
    if (g_carry_eat) {
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) g_carry_eat = false;
        else { GfxMouseShield(true); return; }
    }
    if (g_carry_n == 0) return;
    if (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        carry_cancel();
        ui_flash_message("Split cancelled");
        g_carry_eat = true;
        GfxMouseShield(true);
        return;
    }
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || g_carry_hc < 0) return;   /* (a click outside the cells works as usual) */
    g_carry_eat = true;
    GfxMouseShield(true);
    if (!carry_fits(s, g_carry_hc, g_carry_hs)) {
        ui_flash_message("Not enough empty cells there - pick a free place (the parts go one under another)");
        return;
    }
    int ok = 0;
    for (int k = 0; k < g_carry_n; ++k) if (session_place_part(s, g_carry_hc, g_carry_hs + k, &g_carry[k]) == 0) ++ok;
    char m[160];
    snprintf(m, sizeof m, "Split: %d clip%s placed", ok, ok == 1 ? "" : "s");
    ui_flash_message(m);
    g_selected_ch = g_carry_hc;
    g_selected_scene = g_carry_hs;
    carry_cancel();
}
/* over the cells: where they would go (green = free, red = not) */
void carry_mark_cell(Session *s, int c, int i, Rectangle r)
{
    if (g_carry_n == 0 || g_carry_hc != c || i < g_carry_hs || i >= g_carry_hs + g_carry_n) return;
    bool ok = carry_fits(s, g_carry_hc, g_carry_hs);
    Color col = ok ? (Color){ 90, 220, 120, 255 } : (Color){ 235, 80, 70, 255 };
    Color f = col; f.a = 50;
    DrawRectangleRec(r, f);
    DrawRectangleLinesEx(r, 2, col);
}
/* the "carried" clips at the mouse */
void carry_draw_ghost(Session *s)
{
    if (g_carry_n == 0) return;
    Vector2 m = GfxMouseTrue();
    bool ok = g_carry_hc < 0 || carry_fits(s, g_carry_hc, g_carry_hs);
    float gx = m.x + 12, gy = m.y + 10, gw = 190, gh = 20;
    for (int k = 0; k < g_carry_n; ++k) {
        Rectangle r = { gx + k * 3.0f, gy + k * (gh + 2), gw, gh };
        Color bg = g_carry_col[k]; bg.a = 215;
        DrawRectangleRounded(r, R_CELL, bg);
        DrawRectangleRoundedLines(r, R_CELL, 1, ok ? g_theme.text : (Color){ 235, 80, 70, 255 });
        char d[NAME_LEN];
        snprintf(d, sizeof d, "%s", g_carry[k].name);
        while (ui_text_w(d, 12) > gw - 12 && strlen(d) > 3) utf8_chop_last(d);
        ui_text(d, (int)r.x + 6, (int)r.y + 3, 12, (Color){ 20, 20, 24, 255 });
    }
    const char *hint = g_carry_hc < 0 ? "Click an empty cell  -  Esc: cancel"
                     : ok ? "Click: place here" : "Not free here";
    ui_text(hint, (int)gx, (int)(gy + g_carry_n * (gh + 2) + 2), 11, ok ? g_theme.text : (Color){ 235, 80, 70, 255 });
}

static void draw_stem_strip(Session *s, Rectangle r)
{
    if (r.width <= 0) return;
    int ch = g_selected_ch, sc = g_selected_scene;
    Color bg = g_theme.panel_bg; bg.a = 235;
    DrawRectangleRounded(r, R_CELL * 0.4f, bg);
    DrawRectangleRoundedLines(r, R_CELL * 0.4f, 1, g_theme.grid_line);
    Vector2 m = GetMousePosition();
    bool en = !g_ti_active && !g_ctx_open && !g_audio_dlg_open && !g_file_menu_open && !g_edit_menu_open && !g_play_menu_open && !dialogs_any_open()
              && g_carry_n == 0;
    if (!session_clip_has_stems(s, ch, sc)) {                  /* still separating */
        float p = 0; char t[200] = "";
        ui_separation_progress(ch, sc, &p, t, sizeof t);
        Rectangle bar = { r.x + 6, r.y + 6, 120, r.height - 12 };
        DrawRectangleRec(bar, g_theme.panel_bg2);
        DrawRectangle((int)bar.x, (int)bar.y, (int)(bar.width * p), (int)bar.height, g_theme.selection);
        while (ui_text_w(t, 12) > r.width - 140 && strlen(t) > 4) t[strlen(t) - 1] = 0;
        ui_text(t, (int)(bar.x + bar.width + 8), (int)r.y + 7, 12, g_theme.text);
        if (en && CheckCollisionPointRec(m, r) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ui_separation_show();
        return;
    }
    StemLayout L;
    stem_layout(s, ch, sc, r.x - 8, r.y, r.width + 16, &L);
    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);
    Color chc = channel_color(s->channels[ch].color_id);
    int lch = -1, lsc = -1, lk = -1;
    bool armed = session_stem_learn_armed(&lch, &lsc, &lk);
    if (armed && IsKeyPressed(KEY_ESCAPE)) {                    /* Esc: no MIDI Learn */
        session_stem_learn_arm(-1, -1, -1);
        armed = false;
        ui_flash_message("MIDI Learn cancelled");
    }
    double now = GetTime();
    for (int k = 0; k < L.n; ++k) {
        const StemCtl *C = &L.c[k];
        const char *nm = session_clip_stem_name(s, ch, sc, k);
        Color col = stem_color(nm, chc);
        bool audible = session_clip_stem_audible(s, ch, sc, k);
        char lb[16];
        stem_label(nm, L.n, lb, sizeof lb);
        Color sq = col;
        if (!audible) sq.a = 70;
        DrawRectangleRounded((Rectangle){ C->lab.x, C->lab.y + 5, 7, 10 }, 0.5f, sq);
        ui_text(lb, (int)C->lab.x + 10, (int)C->lab.y + 4, 11, audible ? col : g_theme.text_dim);

        Rectangle fr = C->lev;
        int fid = 300 + k;
        float cur = session_clip_get_stem_level(s, ch, sc, k);
        bool muted = session_clip_get_stem_mute(s, ch, sc, k);
        bool solo = session_clip_get_stem_solo(s, ch, sc, k);
        int cc = session_clip_get_stem_cc(s, ch, sc, k);
        bool me_armed = armed && lch == ch && lsc == sc && lk == k;
        bool hv = en && CheckCollisionPointRec(m, fr);
        Color fbg = (hv || g_vf_id == fid) ? g_theme.cell_hover : g_theme.panel_bg2;
        if (me_armed && fmod(now, 0.8) < 0.4) fbg = (Color){ 150, 50, 50, 255 };
        DrawRectangleRounded(fr, R_CELL, fbg);
        DrawRectangleRoundedLines(fr, R_CELL, 1, me_armed ? (Color){ 235, 80, 70, 255 } : cur != 0.0f ? g_theme.selection : g_theme.text_dim);
        char lv[24];
        if (me_armed) snprintf(lv, sizeof lv, "move a knob");
        else if (cur <= CLIP_GAIN_MIN_DB) snprintf(lv, sizeof lv, "-inf");
        else snprintf(lv, sizeof lv, "%+.1f dB", (double)cur);
        bool badge = (cc >= 0 || g_midi_learn_mode) && !me_armed;
        float tx = badge ? fr.x + 5 : fr.x + (fr.width - ui_text_w(lv, 12)) / 2;
        ui_text(lv, (int)tx, (int)fr.y + 4, 12, audible ? g_theme.text : g_theme.text_dim);
        if (badge) {                                               /* its MIDI binding: a small badge on the right */
            char bd[12];
            if (cc >= 0) snprintf(bd, sizeof bd, "%d", cc); else snprintf(bd, sizeof bd, "--");
            int bw = ui_text_w(bd, 9) + 6;
            Rectangle br = { fr.x + fr.width - bw - 3, fr.y + 4, (float)bw, 12 };
            DrawRectangleRounded(br, 0.4f, g_midi_learn_mode ? g_theme.vu_red : (Color){ 40, 90, 60, 255 });
            ui_text(bd, (int)br.x + 3, (int)br.y + 1, 9, RAYWHITE);
        }
        if (g_midi_learn_mode) {                                   /* the MIDI Learn mode: click = bind, right click = forget */
            if (hv && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                session_stem_learn_arm(ch, sc, k);
                ui_flash_message("MIDI Learn: move a knob or fader on the controller");
            }
            if (hv && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                session_clip_set_stem_cc(s, ch, sc, k, -1);
                if (me_armed) session_stem_learn_arm(-1, -1, -1);
                ui_flash_message("MIDI mapping cleared");
            }
        } else {
            float v = cur + 100.0f;
            int res = value_field_input_ex(fid, fr, &v, en || g_vf_id == fid, CLIP_GAIN_MIN_DB + 100.0f, CLIP_GAIN_MAX_DB + 100.0f, 0.1f, 0.01f);
            if (res == 1) { session_clip_set_stem_level(s, ch, sc, k, v - 100.0f); g_stem_apply_pending = true; }
            else if (res == 2) { session_clip_set_stem_level(s, ch, sc, k, 0.0f); session_clip_stems_apply(s, ch, sc); }
            if (hv && g_vf_id == 0) {
                float wh = GetMouseWheelMove();
                if (wh != 0.0f) { session_clip_set_stem_level(s, ch, sc, k, cur + (wh > 0 ? 1.0f : -1.0f)); g_stem_apply_pending = true; }
                if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {        /* right click: MIDI Learn / forget */
                    if (me_armed) { session_stem_learn_arm(-1, -1, -1); ui_flash_message("MIDI Learn cancelled"); }
                    else if (cc >= 0) {
                        session_clip_set_stem_cc(s, ch, sc, k, -1);
                        char t[80]; snprintf(t, sizeof t, "MIDI: CC %d no longer moves %s", cc, nm); ui_flash_message(t);
                    } else {
                        session_stem_learn_arm(ch, sc, k);
                        char t[120]; snprintf(t, sizeof t, "MIDI Learn: move a knob or fader for %s (right click again: cancel)", nm);
                        ui_flash_message(t);
                    }
                }
            }
        }

        /* M and S */
        for (int b = 0; b < 2; ++b) {
            Rectangle br = b == 0 ? C->m : C->so;
            bool on = b == 0 ? muted : solo;
            Color onc = b == 0 ? (Color){ 230, 90, 80, 255 } : (Color){ 240, 200, 60, 255 };
            bool bh = en && CheckCollisionPointRec(m, br);
            DrawRectangleRounded(br, R_CELL, on ? onc : (bh ? g_theme.cell_hover : g_theme.panel_bg2));
            const char *t = b == 0 ? "M" : "S";
            ui_text(t, (int)(br.x + (br.width - ui_text_w(t, 12)) / 2), (int)br.y + 4, 12, on ? g_theme.bg : g_theme.text);
            if (bh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                bool ex = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
                if (b == 0) session_clip_set_stem_mute(s, ch, sc, k, !muted);
                else {
                    if (ex && !solo)                               /* Ctrl + S: only this one (the other solos off) */
                        for (int q = 0; q < L.n; ++q) session_clip_set_stem_solo(s, ch, sc, q, false);
                    session_clip_set_stem_solo(s, ch, sc, k, !solo);
                }
                session_clip_stems_apply(s, ch, sc);
            }
        }
    }
    /* a level drag: re-buffer now and then, and once more when it is let go */
    if (g_stem_apply_pending && (g_vf_id == 0 || now - g_stem_apply_t > 0.12)) {
        session_clip_stems_apply(s, ch, sc);
        g_stem_apply_t = now;
        g_stem_apply_pending = false;
    }
    static const char *const BL[2] = { "Split to clips", "Export..." };
    for (int b = 0; b < 2; ++b) {
        Rectangle br = L.btn[b];
        bool bh = en && CheckCollisionPointRec(m, br);
        DrawRectangleRounded(br, R_CELL, bh ? g_theme.cell_hover : g_theme.panel_bg2);
        DrawRectangleRoundedLines(br, R_CELL, 1, g_theme.text_dim);
        ui_text(BL[b], (int)br.x + 8, (int)br.y + 4, 12, g_theme.text);
        if (bh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (b == 0) { carry_start(s, ch, sc); g_carry_eat = true; }   /* (this click does not drop them) */
            else ui_open_export(s, ch, sc);
        }
    }
    EndScissorMode();
}

/* the selected clip (the Surround window edits its path - 2.1: in both modes; the lane over the waveform is gone) */
bool ui_get_selected_clip(int *ch, int *scene)
{
    if (ch) *ch = g_selected_ch;
    if (scene) *scene = g_selected_scene;
    return g_selected_ch >= 0 && g_selected_scene >= 0;
}

/* 2.0: an audio recording, live: the whole recording so far (it grows; at least 10 s wide), L and R */
static void draw_rec_view(Session *s, int ch, int sc, float x, float y, float w, float h)
{
    const float *pk = NULL;
    size_t nb = 0;
    unsigned blk = 256;
    unsigned long long frames = 0;
    double beats = 0.0;
    int st = session_rec_state(s, ch, sc, &beats);
    session_rec_sketch(s, ch, sc, &pk, &nb, &blk, &frames);
    double sr = s->sample_rate > 0 ? s->sample_rate : 48000;
    Color red = { 220, 55, 50, 255 };
    DrawRectangle((int)x, (int)y, (int)w, (int)h, g_theme.wave_bg);
    DrawRectangle((int)x, (int)y, (int)w, WAVE_HDR_H, g_theme.panel_bg);
    char hdr[200], tb[32], bb[24];
    double sec = (double)frames / sr;
    snprintf(tb, sizeof tb, "%d:%04.1f", (int)(sec / 60.0), fmod(sec, 60.0));
    fmt_bar_beat(bb, sizeof bb, beats);
    snprintf(hdr, sizeof hdr, "REC  |  %s  |  %s  |  Ch %d / S%d%s", tb, bb, ch + 1, sc + 1,
             st == REC_WAIT ? "  |  waits for the bar" : st == REC_STOPPING ? "  |  stops on the bar" : "");
    bool blink = ((int)(GetTime() * 2.0)) & 1;
    DrawCircleV((Vector2){ x + 14, y + WAVE_HDR_H / 2.0f }, 5, blink || st != REC_ON ? red : (Color){ 120, 30, 30, 255 });
    ui_text(hdr, (int)x + 26, (int)y + 4, 14, g_theme.text);
    {   /* Stop in the header too */
        Rectangle sbr = { x + 26 + ui_text_w(hdr, 14) + 16, y + 2, 64, WAVE_HDR_H - 4 };
        bool hv = fx_ui_free() && CheckCollisionPointRec(GetMousePosition(), sbr);
        DrawRectangleRec(sbr, hv ? (Color){ 240, 80, 70, 255 } : red);
        DrawRectangle((int)sbr.x + 9, (int)(sbr.y + sbr.height / 2 - 4), 8, 8, RAYWHITE);
        ui_text("Stop", (int)sbr.x + 24, (int)sbr.y + 3, 13, RAYWHITE);
        if (hv && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ui_rec_click(s, ch, sc);
    }
    float wy = y + WAVE_HDR_H + 4, wh = h - WAVE_HDR_H - 8;
    if (wh < 20) return;
    float lh = wh / 2.0f;
    double span = sec > 10.0 ? sec * 1.08 : 10.0;            /* the visible seconds: it grows with the recording */
    double fpp = span * sr / (double)w;                      /* frames per pixel */
    DrawLine((int)x, (int)(wy + lh), (int)(x + w), (int)(wy + lh), g_theme.grid_line);
    for (int ln = 0; ln < 2; ++ln) {
        float my = wy + lh * ln + lh / 2;
        DrawLine((int)x, (int)my, (int)(x + w), (int)my, (Color){ g_theme.grid_line.r, g_theme.grid_line.g, g_theme.grid_line.b, 90 });
    }
    {   /* seconds */
        double step = span > 120 ? 30 : span > 40 ? 10 : span > 15 ? 5 : 1;
        for (double t = step; t < span; t += step) {
            float tx = x + (float)(t * sr / fpp);
            DrawLine((int)tx, (int)wy, (int)tx, (int)(wy + wh), (Color){ g_theme.grid_line.r, g_theme.grid_line.g, g_theme.grid_line.b, 70 });
            char lb[16]; snprintf(lb, sizeof lb, "%ds", (int)t);
            ui_text(lb, (int)tx + 3, (int)wy + 1, 10, g_theme.text_dim);
        }
    }
    Color wc = channel_color(s->channels[ch].color_id);
    int ncol = (int)w;
    for (int i = 0; i < ncol && pk; ++i) {
        size_t b0 = (size_t)((double)i * fpp / blk), b1 = (size_t)((double)(i + 1) * fpp / blk);
        if (b1 <= b0) b1 = b0 + 1;
        if (b0 >= nb) break;
        if (b1 > nb) b1 = nb;
        float v[4] = { 1, -1, 1, -1 };
        for (size_t b = b0; b < b1; ++b) {
            const float *q = pk + b * 4;
            if (q[0] < v[0]) v[0] = q[0];
            if (q[1] > v[1]) v[1] = q[1];
            if (q[2] < v[2]) v[2] = q[2];
            if (q[3] > v[3]) v[3] = q[3];
        }
        for (int ln = 0; ln < 2; ++ln) {
            float my = wy + lh * ln + lh / 2, amp = lh / 2 - 2;
            float lo = v[ln * 2], hi = v[ln * 2 + 1];
            if (lo < -1) lo = -1;
            if (hi > 1) hi = 1;
            float yt = my - hi * amp, yb = my - lo * amp;
            if (yb - yt < 1) yb = yt + 1;
            bool clip = hi >= 0.999f || lo <= -0.999f;
            DrawRectangle((int)x + i, (int)yt, 1, (int)(yb - yt + 0.5f), clip ? red : wc);
        }
    }
    float ex = x + (float)((double)frames / fpp);              /* where it is now */
    DrawRectangle((int)ex, (int)wy, 2, (int)wh, red);
    ui_text("L", (int)(x + w) - 12, (int)wy + 2, 10, g_theme.text_dim);
    ui_text("R", (int)(x + w) - 12, (int)(wy + lh) + 2, 10, g_theme.text_dim);
}

void draw_waveform_zone(Session *s,
                               float x, float y, float w, float h)
{
    if (h < WAVEFORM_MIN_H) return;
    {
        int dch = device_panel_channel(s);                 /* 1.2: a selected channel -> its devices */
        if (dch >= 0) { draw_device_panel(s, dch, x, y, w, h); return; }
        if (g_selected_ch >= 0 && g_selected_scene >= 0 && session_rec_state(s, g_selected_ch, g_selected_scene, NULL) != REC_IDLE &&
            !session_rec_is_midi(s, g_selected_ch, g_selected_scene)) {
            draw_rec_view(s, g_selected_ch, g_selected_scene, x, y, w, h);      /* 2.0: the audio being recorded, live */
            return;
        }
        if (g_selected_ch >= 0 && g_selected_scene >= 0 && session_is_midi(s, g_selected_ch, g_selected_scene)) {
            draw_midi_editor(s, g_selected_ch, g_selected_scene, x, y, w, h);   /* 1.3: a MIDI clip -> the piano roll */
            return;
        }
        g_me_focus = false;
    }

    DrawRectangle((int)x, (int)y, (int)w, WAVE_HDR_H, g_theme.panel_bg);

    if (g_selected_ch < 0 || g_selected_scene < 0 ||
        g_selected_ch >= s->channel_count ||
        g_selected_scene >= s->scene_count) {
        ui_text("No clip selected",
                (int)(x + 10), (int)(y + 4), 14, g_theme.text_dim);
        DrawRectangle((int)x, (int)(y + WAVE_HDR_H),
                      (int)w, (int)(h - WAVE_HDR_H), g_theme.wave_bg);
        return;
    }

    Clip *cl = &s->channels[g_selected_ch].clips[g_selected_scene];
    Color chan_col = channel_color(s->channels[g_selected_ch].color_id);

    if (atomic_load(&cl->has_clip) && atomic_load(&cl->is_note)) {
        char hdr[256];
        snprintf(hdr, sizeof(hdr), "Note  |  %s  |  Ch %d / S%d",
                 cl->name, g_selected_ch + 1, g_selected_scene + 1);
        ui_text(hdr, (int)(x + 10), (int)(y + 4), 14, g_theme.text);

        Color nb, nt;
        note_colors(s, g_selected_ch, cl, &nb, NULL, &nt);
        DrawRectangle((int)x, (int)(y + WAVE_HDR_H),
                      (int)w, (int)(h - WAVE_HDR_H), nb);

        int tw = ui_text_w(cl->name, 20);
        ui_text(cl->name,
                (int)(x + (w - tw) / 2),
                (int)(y + WAVE_HDR_H + (h - WAVE_HDR_H) / 2 - 10),
                20, nt);
        return;
    }

    if (!atomic_load(&cl->has_clip)) {
        ui_text("Empty cell",
                (int)(x + 10), (int)(y + 4), 14, g_theme.text_dim);
        DrawRectangle((int)x, (int)(y + WAVE_HDR_H),
                      (int)w, (int)(h - WAVE_HDR_H), g_theme.wave_bg);
        return;
    }

    bool loop_on = atomic_load(&cl->loop);
    size_t loop_start_f = atomic_load(&cl->loop_start);
    size_t loop_end_f   = atomic_load(&cl->loop_end);
    if (loop_end_f == 0 || loop_end_f > cl->wav.frame_count) {
        loop_end_f = cl->wav.frame_count;
    }

    size_t start_marker_f = atomic_load(&cl->start_marker);
    size_t end_marker_f   = atomic_load(&cl->end_marker);
    if (end_marker_f == 0 || end_marker_f > cl->wav.frame_count) {
        end_marker_f = cl->wav.frame_count;
    }

    // ===== Zoom / Scroll =====
    size_t total_frames = cl->wav.frame_count;
    float max_zoom = wave_max_zoom(total_frames);
    float zoom = atomic_load(&cl->wave_zoom);
    if (zoom < WAVE_ZOOM_MIN) zoom = WAVE_ZOOM_MIN;
    if (zoom > max_zoom) zoom = max_zoom;

    size_t visible_frames = (size_t)((double)total_frames / (double)zoom);
    if (visible_frames < WAVE_MIN_VISIBLE) visible_frames = WAVE_MIN_VISIBLE;
    if (visible_frames > 3 * total_frames) visible_frames = 3 * total_frames;

    /* step 36j (as in Bitwig): the view reaches one clip length BEFORE the start and AFTER the end -
     * shifted parts may lie there (they do not play). scroll_frame may be < 0. */
    int64_t view_lo = -(int64_t)total_frames, view_hi = 2 * (int64_t)total_frames;
    int64_t scroll_frame = (int64_t)(intptr_t)atomic_load(&cl->wave_scroll_frame);
    if (scroll_frame + (int64_t)visible_frames > view_hi) scroll_frame = view_hi - (int64_t)visible_frames;
    if (scroll_frame < view_lo) scroll_frame = view_lo;

    /* view0 = the first visible frame (when following it may be negative: empty space before the start) */
    double view0 = (double)scroll_frame;
    double pos_disp = (double)cl->position;
    double clip_sr = cl->wav.sample_rate > 0 ? (double)cl->wav.sample_rate : 48000.0;
    if (g_wave_follow && total_frames > 0) {
        visible_frames = (size_t)((double)g_follow_sec * clip_sr);
        if (visible_frames < WAVE_MIN_VISIBLE) visible_frames = WAVE_MIN_VISIBLE;
        pos_disp = smooth_position(cl, clip_sr);
        view0 = pos_disp - (double)visible_frames * 0.5;
        if (perf_wave_light()) {               /* 1.3.14: light - the view moves 15 times per second */
            static double hv0, ht; static const Clip *hcl; static size_t hvis;
            double now = GetTime();
            if (hcl != cl || hvis != visible_frames || now - ht >= 1.0 / 15.0 || now < ht ||
                fabs(view0 - hv0) > (double)visible_frames * 0.25) {
                hv0 = view0; ht = now; hcl = cl; hvis = visible_frames;
            } else view0 = hv0;
        }
    }

    const char *state = atomic_load(&cl->playing) ? "PLAY"
                      : (atomic_load(&cl->paused) ? "PAUSE" : "STOP");
    const char *loop_str = loop_on ? "  [LOOP]" : "";
    /* 1.4.8: the name + what the file is (sample rate, bits); "-> 48000 Hz" when it is converted for the sound card */
    char name_info[300];
    {
        static char last_path[1024];
        static int  fi_sr, fi_bits, fi_ok;
        if (strcmp(last_path, cl->wav.path) != 0) {
            snprintf(last_path, sizeof last_path, "%s", cl->wav.path);
            fi_ok = wav_file_info(cl->wav.path, &fi_sr, &fi_bits, NULL) == 0;
        }
        char inf[180] = "";
        if (fi_ok && fi_sr > 0) {
            char bt[16] = "";
            if (fi_bits > 0) snprintf(bt, sizeof bt, " %d-bit", fi_bits);
            else if (fi_bits < 0) snprintf(bt, sizeof bt, " %d-bit float", -fi_bits);
            int dev = s->sample_rate;
            if (dev > 0 && dev != fi_sr) snprintf(inf, sizeof inf, "  (%d Hz%s -> %d Hz)", fi_sr, bt, dev);
            else snprintf(inf, sizeof inf, "  (%d Hz%s)", fi_sr, bt);
        }
        {   /* 1.4.11: loudness of the clip (+ what the normalization does to it) */
            float lu, pk, nd;
            if (session_clip_loudness(s, g_selected_ch, g_selected_scene, &lu, &pk, &nd) && isfinite(lu)) {
                size_t L = strlen(inf);
                if (s->loud_target && nd != 0.0f) snprintf(inf + L, sizeof inf - L, "  %.1f LUFS -> %+.1f dB", (double)lu, (double)nd);
                else snprintf(inf + L, sizeof inf - L, "  %.1f LUFS", (double)lu);
            }
        }
        snprintf(name_info, sizeof name_info, "%s%s", cl->name, inf);
    }
    char hdr[400];
    if (g_wave_follow) {
        snprintf(hdr, sizeof(hdr), "%s  |  %s%s  |  Ch %d / S%d  |  scroll: %.0f s",
                 name_info, state, loop_str,
                 g_selected_ch + 1, g_selected_scene + 1, g_follow_sec);
    } else if (zoom > 1.01f) {
        snprintf(hdr, sizeof(hdr), zoom >= 100.0f ? "%s  |  %s%s  |  Ch %d / S%d  |  zoom: %.0fx"
                                                  : "%s  |  %s%s  |  Ch %d / S%d  |  zoom: %.1fx",
                 name_info, state, loop_str,
                 g_selected_ch + 1, g_selected_scene + 1, zoom);
    } else {
        snprintf(hdr, sizeof(hdr), "%s  |  %s%s  |  Ch %d / S%d",
                 name_info, state, loop_str,
                 g_selected_ch + 1, g_selected_scene + 1);
    }
    if (cl->peaks && !peaks_done(cl->peaks)) {
        size_t tot = peaks_total(cl->peaks);
        char tmp[48];
        snprintf(tmp, sizeof(tmp), "  |  analyzing %d%%",
                 tot ? (int)(100 * peaks_ready(cl->peaks) / tot) : 0);
        strncat(hdr, tmp, sizeof(hdr) - strlen(hdr) - 1);
    }
    {
        float panel_x = (float)GetScreenWidth() - MASTER_W - GAP;     /* always on the right, however wide the master is */
        bool has_panel = panel_x > x + 220 && panel_x + MASTER_W <= x + w + GAP + 1;
        float tools_x = (has_panel ? panel_x : x + w) - EDIT_TOOLS_W - 4;
        /* 1.4.9: the clip's own gain and pan (like Bitwig): drag up/down (Shift = fine), double click = back to 0.
         * 1.4.13: + the clip's pitch (semitones; drag = whole semitones, Shift = cents; wheel = 1 semitone,
         *   Shift + wheel = 10 cents) and "Formants" (keep the voice's natural colour when the pitch moves). */
        const float fw_[3] = { 96.0f, 96.0f, 104.0f };
        float fmt_w = (float)ui_text_w("Formants", 11) + 14.0f;
        bool has_sur = session_get_surround(s, g_selected_ch, NULL) && !session_is_midi(s, g_selected_ch, g_selected_scene) &&
                       session_clip_mc_mode(s, g_selected_ch, g_selected_scene) != CLIP_MC_MULTI;   /* 1.4.24 */
        float sur_w = has_sur ? (float)ui_text_w("Surround", 11) + 14.0f + 4.0f : 0.0f;   /* 1.4.19 */
        float all_w = fw_[0] + fw_[1] + fw_[2] + fmt_w + 3 * 4.0f + sur_w;
        float cgx = tools_x - all_w - 10, text_end = tools_x;
        if (cgx > x + 160) {
            bool en = !g_ti_active && !g_ctx_open && !g_audio_dlg_open && !g_file_menu_open && !g_edit_menu_open && !g_play_menu_open;
            bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            float fx_ = cgx;
            for (int k = 0; k < 3; ++k) {
                Rectangle fr = { fx_, y + 2, fw_[k], WAVE_HDR_H - 4 };
                fx_ += fw_[k] + 4;
                int fid = 40 + k;
                float cur = k == 2 ? session_get_clip_pitch(s, g_selected_ch, g_selected_scene)
                          : k == 1 ? session_get_clip_pan(s, g_selected_ch, g_selected_scene)
                                   : session_get_clip_gain(s, g_selected_ch, g_selected_scene);
                bool hv = en && CheckCollisionPointRec(GetMousePosition(), fr);
                bool set = cur != 0.0f;
                DrawRectangleRounded(fr, R_CELL, (hv || g_vf_id == fid) ? g_theme.cell_hover : g_theme.panel_bg2);
                DrawRectangleRoundedLines(fr, R_CELL, 1, set ? g_theme.selection : g_theme.text_dim);
                char lb[32];
                if (k == 0) {
                    if (cur <= CLIP_GAIN_MIN_DB) snprintf(lb, sizeof lb, "Gain -inf");
                    else snprintf(lb, sizeof lb, "Gain %+.1f dB", (double)cur);
                } else if (k == 1) {
                    int pc = (int)lroundf(cur * 100.0f);
                    if (pc == 0) snprintf(lb, sizeof lb, "Pan C");
                    else snprintf(lb, sizeof lb, "Pan %s%d", pc < 0 ? "L" : "R", pc < 0 ? -pc : pc);
                } else {
                    if (cur == 0.0f) snprintf(lb, sizeof lb, "Pitch 0");
                    else if (fabsf(cur - roundf(cur)) < 0.005f) snprintf(lb, sizeof lb, "Pitch %+d st", (int)lroundf(cur));
                    else snprintf(lb, sizeof lb, "Pitch %+.2f", (double)cur);
                }
                ui_text(lb, (int)(fr.x + (fr.width - ui_text_w(lb, 12)) / 2), (int)fr.y + 3, 12, set ? g_theme.text : g_theme.text_dim);
                /* (the field helper only moves positive values -> shifted) */
                float off = k == 1 ? 2.0f : 100.0f;
                float v = cur + off;
                float vmin = k == 2 ? -CLIP_PITCH_MAX : k == 1 ? -1.0f : CLIP_GAIN_MIN_DB;
                float vmax = k == 2 ?  CLIP_PITCH_MAX : k == 1 ?  1.0f : CLIP_GAIN_MAX_DB;
                float step = k == 2 ? 0.05f : k == 1 ? 0.005f : 0.1f;
                float fine = k == 2 ? 0.01f : k == 1 ? 0.001f : 0.01f;
                int res = value_field_input_ex(fid, fr, &v, en || g_vf_id == fid, vmin + off, vmax + off, step, fine);
                if (res == 1) {
                    float nv = v - off;
                    if (k == 2 && !shift) nv = roundf(nv);          /* pitch: whole semitones unless Shift */
                    if (k == 2)      session_set_clip_pitch(s, g_selected_ch, g_selected_scene, nv);
                    else if (k == 1) session_set_clip_pan(s, g_selected_ch, g_selected_scene, nv);
                    else             session_set_clip_gain(s, g_selected_ch, g_selected_scene, nv);
                } else if (res == 2) {
                    if (k == 2)      session_set_clip_pitch(s, g_selected_ch, g_selected_scene, 0.0f);
                    else if (k == 1) session_set_clip_pan(s, g_selected_ch, g_selected_scene, 0.0f);
                    else             session_set_clip_gain(s, g_selected_ch, g_selected_scene, 0.0f);
                }
                if (en && g_vf_id == 0 && GfxPointIn(GetMousePosition(), fr)) {
                    float wh = GetMouseWheelMove();       /* the mouse wheel: 0.5 dB / 5 % / 1 semitone (Shift: 10 cents) per click */
                    if (wh != 0.0f) {
                        float d = wh > 0 ? 1.0f : -1.0f;
                        if (k == 2)      session_set_clip_pitch(s, g_selected_ch, g_selected_scene, cur + d * (shift ? 0.1f : 1.0f));
                        else if (k == 1) session_set_clip_pan(s, g_selected_ch, g_selected_scene, cur + d * 0.05f);
                        else             session_set_clip_gain(s, g_selected_ch, g_selected_scene, cur + d * 0.5f);
                    }
                }
            }
            {   /* 1.4.13: Formants on/off */
                bool on = session_get_clip_formants(s, g_selected_ch, g_selected_scene);
                bool pitched = session_get_clip_pitch(s, g_selected_ch, g_selected_scene) != 0.0f;
                Rectangle br = { fx_, y + 2, fmt_w, WAVE_HDR_H - 4 };
                bool hv = en && g_vf_id == 0 && CheckCollisionPointRec(GetMousePosition(), br);
                Color bg = on ? g_theme.selection : (hv ? g_theme.cell_hover : g_theme.panel_bg2);
                DrawRectangleRounded(br, R_CELL, bg);
                DrawRectangleRoundedLines(br, R_CELL, 1, on ? g_theme.selection : g_theme.text_dim);
                Color tc = on ? g_theme.bg : (pitched ? g_theme.text : g_theme.text_dim);
                ui_text("Formants", (int)(br.x + (br.width - ui_text_w("Formants", 11)) / 2), (int)br.y + 4, 11, tc);
                if (hv) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
                if (hv && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                    session_set_clip_formants(s, g_selected_ch, g_selected_scene, !on);
            }
            if (has_sur) {   /* 1.4.19: the surround - 2.1: opens the Surround window (its path is edited there) */
                Rectangle br = { fx_ + fmt_w + 4, y + 2, sur_w - 4, WAVE_HDR_H - 4 };
                bool hv = en && g_vf_id == 0 && CheckCollisionPointRec(GetMousePosition(), br);
                int np = session_clip_get_sur_points(s, g_selected_ch, g_selected_scene, NULL, SUR_MAX_PTS);
                Color bg = hv ? g_theme.cell_hover : g_theme.panel_bg2;
                DrawRectangleRounded(br, R_CELL, bg);
                DrawRectangleRoundedLines(br, R_CELL, 1, np ? (Color){ 120, 200, 255, 255 } : g_theme.text_dim);
                ui_text("Surround", (int)(br.x + (br.width - ui_text_w("Surround", 11)) / 2), (int)br.y + 4, 11, g_theme.text);
                if (hv) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
                if (hv && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ui_open_surround(s, g_selected_ch);   /* 2.1: the path is edited in the window */
            }
            text_end = cgx;
        }
        float text_w = text_end - x - 14;
        BeginScissorMode((int)x, (int)y, (int)text_w + 10, WAVE_HDR_H);
        ui_text(hdr, (int)(x + 10), (int)(y + 4), 14, g_theme.text);
        EndScissorMode();
        if (has_panel)
            draw_clip_bpm_panel(s, g_selected_ch, g_selected_scene, panel_x, y, MASTER_W, WAVE_HDR_H);
        if (g_ed_ch != g_selected_ch || g_ed_sc != g_selected_scene) {   /* another clip -> the selection does not apply */
            ed_forget();
            g_ed_ch = g_selected_ch; g_ed_sc = g_selected_scene;
        }
        if (draw_edit_tools(tools_x, y, WAVE_HDR_H) && atomic_load(&cl->has_clip) && cl->wav.frame_count > 0) {
            /* 1.3.15: fit - the view exactly from the Start to the End marker (a little air so they can be grabbed) */
            size_t tf = cl->wav.frame_count;
            size_t a = atomic_load(&cl->start_marker), b = atomic_load(&cl->end_marker);
            if (b == 0 || b > tf) b = tf;
            if (a >= b) { a = 0; b = tf; }
            double span = (double)(b - a), vis = span * 1.03;
            if (vis < (double)WAVE_MIN_VISIBLE) vis = (double)WAVE_MIN_VISIBLE;
            float z = (float)((double)tf / vis);
            if (z < WAVE_ZOOM_MIN) z = WAVE_ZOOM_MIN;
            g_wave_follow = false;                          /* SCROLL off: the view stays where it is put */
            session_set_wave_zoom(s, g_selected_ch, g_selected_scene, z);
            double v = (double)tf / (double)atomic_load(&cl->wave_zoom);    /* (after the limits) */
            int64_t sf = (int64_t)llround(((double)a + (double)b) * 0.5 - v * 0.5);
            session_set_wave_scroll(s, g_selected_ch, g_selected_scene, (size_t)sf);
        }
    }

    float ruler_y = y + WAVE_HDR_H;
    float wave_y  = ruler_y + WAVE_RULER_H;
    float wave_h  = h - WAVE_HDR_H - WAVE_RULER_H;
    if (wave_h < 10) wave_h = 10;
    g_wave_geo.x = x; g_wave_geo.ruler_y = ruler_y; g_wave_geo.w = w; g_wave_geo.wave_y = wave_y; g_wave_geo.wave_h = wave_h; g_wave_geo.valid = true;
    g_wave_geo.view0 = view0; g_wave_geo.vis = (double)visible_frames; g_wave_geo.ch = g_selected_ch; g_wave_geo.sc = g_selected_scene;

    if (total_frames == 0) {
        DrawRectangle((int)x, (int)ruler_y, (int)w, (int)(h - WAVE_HDR_H),
                      g_theme.wave_bg);
        return;
    }

    double total_sec = (double)total_frames / (double)cl->wav.sample_rate;

    double vis_start_sec = view0 / (double)cl->wav.sample_rate;
    double vis_end_sec = (view0 + (double)visible_frames) /
                         (double)cl->wav.sample_rate;

    DrawRectangle((int)x, (int)ruler_y, (int)w, WAVE_RULER_H, g_theme.ruler_bg);
    DrawLine((int)x, (int)(ruler_y + WAVE_RULER_H),
             (int)(x + w), (int)(ruler_y + WAVE_RULER_H),
             g_theme.ruler_line);

    /* step 36j: the ruler continues both before the start and after the end of the clip (darkened).
     * If the clip has a tempo - it marks the BARS (1 = the Start marker; before it 0, -1, ...), otherwise the seconds. */
    {
        double sx0 = x + (0.0 - view0) / (double)visible_frames * w, sx1 = x + ((double)total_frames - view0) / (double)visible_frames * w;
        Color dimr = { 0, 0, 0, 90 };
        if (sx0 > x) DrawRectangle((int)x, (int)ruler_y, (int)((sx0 < x + w ? sx0 : x + w) - x), WAVE_RULER_H, dimr);
        if (sx1 < x + w) { float a = sx1 > x ? (float)sx1 : x; DrawRectangle((int)a, (int)ruler_y, (int)(x + w - a), WAVE_RULER_H, dimr); }
    }
    double bar_anchor = 0, bar_beat = 0;
    ed_grid(s, g_selected_ch, g_selected_scene, 1.0, &bar_anchor, &bar_beat);
    if (bar_beat > 0) {
        double bar = bar_beat * sig_bar(), fppx = (double)visible_frames / (double)w;
        double step = bar;
        while (step / fppx < 56.0 && step < bar * 4096) step *= 2.0;
        double k0 = floor((view0 - bar_anchor) / step);
        for (double k = k0; ; k += 1.0) {
            double f = bar_anchor + k * step;
            float rx = x + (float)((f - view0) / fppx);
            if (rx > x + w + 1) break;
            if (rx >= x - 1) {
                aa_vbar(rx - 0.5f, 1.0f, ruler_y + 6, WAVE_RULER_H - 7, g_theme.ruler_tick);
                char lbl[16];
                snprintf(lbl, sizeof lbl, "%lld", (long long)llround((f - bar_anchor) / bar) + 1);
                bool inside = f >= -0.5 && f <= (double)total_frames + 0.5;
                ui_text(lbl, (int)rx + 3, (int)(ruler_y + 2), 10, inside ? g_theme.text_dim : g_theme.grid_line);
                aa_vbar(rx - 0.5f, 1.0f, wave_y, wave_h, g_theme.grid_line);
            }
            if (k - k0 > 2000) break;
        }
    }
    double interval = ruler_interval(vis_end_sec - vis_start_sec);
    if (interval < 0.5) interval = 0.5;

    double t = floor(vis_start_sec / interval) * interval;   /* may be negative too (before the start) */
    while (bar_beat <= 0) {
        if (t > vis_end_sec + interval) break;

        float ratio = (float)((t - vis_start_sec) /
                              (vis_end_sec - vis_start_sec));
        float rx = x + ratio * w;

        if (rx < x || rx > x + w) { t += interval; continue; }

        aa_vbar(rx - 0.5f, 1.0f, ruler_y + 6, WAVE_RULER_H - 7, g_theme.ruler_tick);

        char lbl[16];
        if (t < -0.0005) { lbl[0] = '-'; format_time(lbl + 1, sizeof(lbl) - 1, -t); }
        else format_time(lbl, sizeof(lbl), t);
        int lw = ui_text_w(lbl, 10);
        bool inside_t = t >= -0.0005 && t <= total_sec + 0.0005;
        ui_text(lbl, (int)(rx - lw / 2), (int)(ruler_y + 2),
                10, inside_t ? g_theme.text_dim : g_theme.grid_line);

        aa_vbar(rx - 0.5f, 1.0f, wave_y, wave_h, g_theme.grid_line);

        t += interval;
    }

    DrawRectangle((int)x, (int)wave_y, (int)w, (int)wave_h, g_theme.wave_bg);

    /* ===== The waveform: 1 pixel each, min/max + a solid core (RMS); with a tall panel — left and right channel ===== */
    bool stereo = wave_h >= WAVE_STEREO_MIN_H;
    int ncol = (int)w;
    if (ncol < 1) return;
    /* 1.4.14: the stem strip is on top: the waveform below does not see the mouse over it */
    Rectangle stem_r = stem_strip_rect(s, x, wave_y, w, wave_h);
    bool stem_shield = stem_r.width > 0 && !GfxMouseShieldGet() && GfxPointIn(GfxMouseTrue(), stem_r);
    if (stem_shield) GfxMouseShield(true);
    double fpp = (double)visible_frames / (double)ncol;             /* frames per pixel */

    /* lanes: mono = one; stereo = L on top, R below */
    float lane_h = stereo ? (wave_h - 4) * 0.5f : wave_h;
    float lane_y[2] = { wave_y, wave_y + lane_h + 4 };
    int   nlanes = stereo ? 2 : 1;
    bool  mc_multi = cl->mc_mode == CLIP_MC_MULTI && cl->mc_nch > 2;     /* 1.4.24: a lane for every channel */
    for (int ln = 0; ln < nlanes && !mc_multi; ++ln) {
        float my = lane_y[ln] + lane_h * 0.5f;
        DrawLine((int)x, (int)my, (int)(x + w), (int)my, g_theme.grid_line);
    }
    if (stereo && !mc_multi) {
        Color sep = g_theme.grid_line;
        DrawRectangle((int)x, (int)(wave_y + lane_h + 1), (int)w, 2, sep);
    }

    /* Deep zoom: one pixel is less than one block of the sketch -> read the real samples from the file.
     * The result is remembered until the view changes (otherwise it would be read from disk every frame). */
    static PeakVal detail_buf[4096];
    static const Peaks *detail_peaks = NULL;
    static int64_t detail_start = INT64_MIN;
    static size_t  detail_vis = 0;
    static int     detail_n = 0;
    static bool    detail_ok = false;
    static uint64_t detail_edits = 0;
    const EditList *cur_edits = session_clip_edits(s, g_selected_ch, g_selected_scene);
    bool cut_clip = cur_edits && !edit_is_identity(cur_edits);
    bool tool_owns = false;
    double grid_anchor = 0, grid_beat = 0;
    double grid_step = ed_grid(s, g_selected_ch, g_selected_scene, fpp, &grid_anchor, &grid_beat);
    {   /* ===== Cutting: the mouse (before drawing, so the new look shows while dragging) ===== */
        Vector2 m = GetMousePosition();
        double mf = view0 + (double)(m.x - x) / (double)w * (double)visible_frames;
        Rectangle wr = { x, wave_y, w, wave_h };
        bool in_wave = CheckCollisionPointRec(m, wr);
        bool ui_free = !g_ctx_open && !g_ti_active && !g_audio_dlg_open && g_em_kind == EM_NONE &&
                       g_dragging_loop_marker == 0 && !g_rt_open;
        /* the markers (in pixels) - take priority over the edges */
        #define FX_(f) (x + (float)(((double)(f) - view0) / (double)visible_frames) * w)
        bool near_mk = fabsf(m.x - FX_(start_marker_f)) < 10.0f || fabsf(m.x - FX_(end_marker_f)) < 10.0f ||
                       (loop_on && (fabsf(m.x - FX_(loop_start_f)) < 8.0f || fabsf(m.x - FX_(loop_end_f)) < 8.0f));
        #undef FX_
        /* click outside the waveform -> the part is no longer selected (Delete deletes the clip again) */
        if ((IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) &&
            !CheckCollisionPointRec(m, (Rectangle){ x, y, w, h }) &&
            !(g_em_kind != EM_NONE && CheckCollisionPointRec(m, g_em_rect)))
            g_part = -1;
        {   /* the parts were changed from outside (undo, loading...) -> the selected one is no longer the same */
            static const EditList *seen = NULL;
            if (cur_edits != seen && g_drag == DR_NONE) { if (!g_part_own) g_part = -1; seen = cur_edits; }
            g_part_own = false;
        }
        if (g_tool == TOOL_CUT) {
            if (in_wave && ui_free && cur_edits) {
                tool_owns = true;
                SetMouseCursor(MOUSE_CURSOR_CROSSHAIR);
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    int64_t cf = ed_snap(mf, grid_step, grid_anchor);
                    EditList *nl = edit_split(cur_edits, cf);
                    if (nl) ed_apply(s, nl, NULL);
                    else ui_flash_message(edit_find(cur_edits, cf) < 0 ? "Nothing to cut here (silence)" : "Already cut here");
                }
            }
        } else if (cur_edits) {
            double fpp_now = (double)visible_frames / (double)w;
            if (g_drag != DR_NONE) {
                tool_owns = true;
                if (g_drag == DR_PENDING && fabsf(m.x - g_drag_px) > 5.0f) g_drag = DR_MOVE;
                edit_free(g_preview); g_preview = NULL;
                const EditSeg *o = &g_drag_seg;
                double d = mf - g_drag_f0;
                if (g_drag == DR_MOVE) {
                    int64_t nt = ed_snap((double)o->t + d, grid_step, grid_anchor);
                    /* at least a little of the part must stay in the clip (otherwise you cannot grab it again) */
                    int64_t keep = o->len / 8 < 2000 ? (o->len < 2000 ? o->len / 2 : 2000) : o->len / 8;
                    /* step 36j: it may also be outside the clip (it does not play there), up to the end of the visible range (one length out) */
                    if (nt > 2 * cur_edits->len - keep) nt = 2 * cur_edits->len - keep;
                    if (nt < keep - o->len - cur_edits->len) nt = keep - o->len - cur_edits->len;
                    g_preview = g_drag_copy ? edit_copy(cur_edits, g_drag_idx, nt) : edit_move(cur_edits, g_drag_idx, nt);
                    SetMouseCursor(MOUSE_CURSOR_RESIZE_EW);
                } else if (g_drag == DR_TRIM_L) {
                    g_preview = edit_trim(cur_edits, g_drag_idx, 0, ed_snap((double)o->t + d, grid_step, grid_anchor));
                    SetMouseCursor(MOUSE_CURSOR_RESIZE_EW);
                } else if (g_drag == DR_TRIM_R) {
                    g_preview = edit_trim(cur_edits, g_drag_idx, 1, ed_snap((double)(o->t + o->len) + d, grid_step, grid_anchor));
                    SetMouseCursor(MOUSE_CURSOR_RESIZE_EW);
                } else if (g_drag == DR_XFADE) {
                    /* the seam stays in place; mouse to the left = the right one's earlier audio mixes with the left one,
                     * to the right = the left one continues and mixes with the right one. Close to the seam = no crossfade. */
                    int64_t mp = ed_snap(mf, grid_step, grid_anchor);
                    int64_t off = mp - o->t;
                    if (fabs((double)off / fpp_now) < 3.0) off = 0;
                    g_preview = edit_set_xfade(cur_edits, g_drag_idx, off < 0 ? -off : 0, off > 0 ? off : 0);
                    SetMouseCursor(MOUSE_CURSOR_RESIZE_EW);
                } else if (g_drag == DR_FADE_IN || g_drag == DR_FADE_OUT) {
                    int64_t fin, fout;
                    edit_seg_fades(o, &fin, &fout);
                    if (g_drag == DR_FADE_IN) fin = (int64_t)((double)fin + d); else fout = (int64_t)((double)fout - d);
                    if (fin < 0) fin = 0;
                    if (fout < 0) fout = 0;
                    if (fin > o->len - fout) fin = o->len - fout;
                    if (fout > o->len - fin) fout = o->len - fin;
                    g_preview = edit_set_fades(cur_edits, g_drag_idx, fin, fout);
                    SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
                }
                if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {         /* released: apply */
                    if (g_drag == DR_PENDING) {
                        /* plain click: only selects the part (the play position is a double click on the seconds) */
                    } else if (g_preview) {
                        EditList *nl = g_preview; g_preview = NULL;
                        int64_t mid = 0;
                        int k = -1;
                        for (int i = 0; i < nl->n; ++i)            /* the new position of the part */
                            if (nl->seg[i].src <= o->src + o->len / 2 && nl->seg[i].src + nl->seg[i].len > o->src + o->len / 2 &&
                                (k < 0 || llabs(nl->seg[i].t - (int64_t)(o->t + d)) < llabs(nl->seg[k].t - (int64_t)(o->t + d)))) k = i;
                        mid = k >= 0 ? nl->seg[k].t + nl->seg[k].len / 2 : -1;
                        ed_apply(s, nl, NULL);
                        const EditList *now = session_clip_edits(s, g_selected_ch, g_selected_scene);
                        g_part = (mid >= 0 && now && !edit_is_identity(now)) ? edit_find(now, mid) : -1;
                        g_part_own = true;
                    }
                    edit_free(g_preview); g_preview = NULL;
                    g_drag = DR_NONE;
                }
            } else if (in_wave && ui_free) {
                int k;
                int hz = ed_hit(cur_edits, view0, fpp_now, x, wave_y, m, mf, near_mk, &k);
                if (hz == HZ_EDGE_L || hz == HZ_EDGE_R || hz == HZ_XFADE) SetMouseCursor(MOUSE_CURSOR_RESIZE_EW);
                else if (hz == HZ_FADE_IN || hz == HZ_FADE_OUT) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
                if (hz != HZ_NONE) g_hover_part = k;
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && hz != HZ_NONE) {
                    tool_owns = true;
                    g_drag_idx = k;
                    g_drag_seg = cur_edits->seg[k];
                    g_drag_px = m.x;
                    g_drag_f0 = mf;
                    g_drag_copy = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
                    g_drag = hz == HZ_BODY ? DR_PENDING : hz == HZ_EDGE_L ? DR_TRIM_L : hz == HZ_EDGE_R ? DR_TRIM_R
                           : hz == HZ_XFADE ? DR_XFADE : hz == HZ_FADE_IN ? DR_FADE_IN : DR_FADE_OUT;
                    g_part = cut_clip ? k : -1;
                } else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !near_mk) {
                    g_part = -1;                                   /* empty space: only the play position */
                }
                if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && !near_mk && (k >= 0 || cut_clip || g_seg_clip)) {
                    tool_owns = true;
                    if (k >= 0 && cut_clip) g_part = k;
                    g_em_kind = EM_PART; g_em_part = k; g_em_pos = m; g_em_just = true;
                    g_em_frame = (int64_t)llround(mf);
                    g_ed_ch = g_selected_ch; g_ed_sc = g_selected_scene;
                }
            }
        }
    }
    cur_edits = session_clip_edits(s, g_selected_ch, g_selected_scene);   /* it may have been replaced just now (the old one is freed) */
    cut_clip = cur_edits && !edit_is_identity(cur_edits);
    total_frames = cl->wav.frame_count;
    const EditList *draw_edits = g_preview ? g_preview : cur_edits;      /* what gets drawn */
    bool draw_cut = draw_edits && !edit_is_identity(draw_edits);
    bool detail_mode = !g_wave_follow && (fpp < (double)PEAKS_BLOCK) && cl->peaks && ncol <= 4096 &&
                       (draw_edits || scroll_frame >= 0);
    if (detail_mode) {
        if (detail_peaks != cl->peaks || detail_start != scroll_frame ||
            detail_vis != visible_frames || detail_n != ncol || detail_edits != el_hash(draw_edits)) {
            /* through the parts (also with an uncut clip, if the view goes outside it) */
            bool via_edits = draw_edits && (draw_cut || scroll_frame < 0 || scroll_frame + (int64_t)visible_frames > (int64_t)total_frames);
            detail_ok = via_edits ? ed_detail(cl, draw_edits, (double)scroll_frame, fpp, ncol, detail_buf)
                                  : peaks_detail_val(cl->peaks, (size_t)scroll_frame, fpp, ncol, detail_buf);
            detail_peaks = cl->peaks; detail_start = scroll_frame;
            detail_vis = visible_frames; detail_n = ncol; detail_edits = el_hash(draw_edits);
        }
        if (!detail_ok) detail_mode = false;
    }

    /* colours: the outer outline (min/max) is more transparent, the core (RMS) — solid and slightly lighter */
    Color c_out = chan_col; c_out.a = 150;
    Color c_rms = chan_col;
    c_rms.r = (unsigned char)(c_rms.r + (255 - c_rms.r) / 5);
    c_rms.g = (unsigned char)(c_rms.g + (255 - c_rms.g) / 5);
    c_rms.b = (unsigned char)(c_rms.b + (255 - c_rms.b) / 5);

    /* The bars are anchored to a grid of absolute frames: when the view moves (follow/scroll)
     * every pixel shows the same section and only slides -> no flicker. */
    double q0 = detail_mode ? 0.0 : floor(view0 / fpp);
    float off_px = detail_mode ? 0.0f : (float)((view0 - q0 * fpp) / fpp);

    /* Smooth sliding (step 19): the bars are anchored to the grid, and every screen pixel blends the two
     * neighbouring bars according to the fractional offset -> the waveform moves in fractions of a pixel, not in 1-pixel jumps. */
    #define WCOL_MAX 8192
    static PeakVal wcol[WCOL_MAX + 2];
    static bool    wok[WCOL_MAX + 2];
    bool subpix = !detail_mode && ncol + 2 <= WCOL_MAX && off_px > 0.001f && !perf_wave_light();

    /* 1.3.14: the values of the columns are kept (by their absolute number q0 + i): a moving view (SCROLL, follow)
     * computes only the columns that are new; a still view computes nothing */
    static PeakVal cv[WCOL_MAX + 2], cv2[WCOL_MAX + 2];
    static bool    cok[WCOL_MAX + 2], cok2[WCOL_MAX + 2];
    static struct { const Peaks *pk; size_t ready; uint64_t eh; double fpp; } ck;
    static double  cq0 = 0.0; static int cn = 0;
    uint64_t ehash = el_hash(draw_edits);
    size_t pready = cl->peaks ? peaks_ready(cl->peaks) : 0;
    bool use_cols = !detail_mode && ncol + 2 <= WCOL_MAX;
    if (use_cols) {
        if (ck.pk != cl->peaks || ck.ready != pready || ck.eh != ehash || ck.fpp != fpp) {
            ck.pk = cl->peaks; ck.ready = pready; ck.eh = ehash; ck.fpp = fpp; cn = 0;
        }
        int need = ncol + 1;
        for (int i = 0; i < need; ++i) {
            double j = q0 + i - cq0;
            if (cn > 0 && j >= 0.0 && j < (double)cn && j == floor(j)) { cv2[i] = cv[(int)j]; cok2[i] = cok[(int)j]; continue; }
            double f0 = (q0 + i) * fpp, f1 = f0 + fpp;
            int64_t st = (int64_t)floor(f0), en = (int64_t)floor(f1);   /* may also be outside the clip (step 36j) */
            if (en <= st) en = st + 1;
            cok2[i] = cl->peaks && session_clip_peaks_list(cl, draw_edits, st, en, &cv2[i]);
        }
        memcpy(cv, cv2, sizeof(PeakVal) * (size_t)need);
        memcpy(cok, cok2, sizeof(bool) * (size_t)need);
        cq0 = q0; cn = need;
        memcpy(wcol, cv, sizeof(PeakVal) * (size_t)need);
        memcpy(wok, cok, sizeof(bool) * (size_t)need);
    }

    /* 1.3.14: a still view (nothing moved, nothing changed) -> the same batches again */
    static struct { const Peaks *pk; size_t ready; uint64_t eh; double q0, fpp; float off, x, w, wy, wh; int nl, st, dm, ncol;
                    int64_t dstart; Color co; float sc; int lt; } gk, nk;
    memset(&nk, 0, sizeof nk);
    nk.pk = cl->peaks; nk.ready = pready; nk.eh = ehash; nk.q0 = q0; nk.fpp = fpp; nk.off = off_px; nk.x = x; nk.w = w;
    nk.wy = wave_y; nk.wh = wave_h; nk.nl = nlanes; nk.st = stereo; nk.dm = detail_mode; nk.ncol = ncol;
    nk.dstart = detail_mode ? detail_start : 0; nk.co = chan_col; nk.sc = GfxScaleNow(); nk.lt = perf_wave_light();
    static bool g_have = false;
    bool same = g_have && memcmp(&gk, &nk, sizeof nk) == 0;

    BeginScissorMode((int)x, (int)wave_y, (int)w, (int)wave_h);
    if (mc_multi) {                                  /* 1.4.24: every channel of the file in its own lane */
        int nc = cl->mc_nch > STREAM_MAX_CH ? STREAM_MAX_CH : cl->mc_nch;
        float lh = wave_h / (float)nc;
        int first = atomic_load(&cl->mc_out);
        for (int k = 0; k < nc; ++k) {
            float ly = wave_y + lh * (float)k, my = ly + lh * 0.5f, amp = lh * 0.5f - 1.5f;
            if (amp < 1.0f) amp = 1.0f;
            if (k > 0) DrawRectangle((int)x, (int)ly, (int)w, 1, g_theme.grid_line);
            DrawLine((int)x, (int)my, (int)(x + w), (int)my, (Color){ g_theme.grid_line.r, g_theme.grid_line.g, g_theme.grid_line.b, 90 });
            Peaks *pk = cl->mc_peaks[k];
            for (int i = 0; i < ncol && pk; ++i) {
                double f0 = (q0 + i) * fpp, f1 = f0 + fpp;
                int64_t st = (int64_t)floor(f0), en = (int64_t)floor(f1);
                if (en <= st) en = st + 1;
                PeakVal v;
                if (!session_peaks_list(cl, pk, draw_edits, st, en, &v)) continue;
                int cx = (int)floorf(x + (float)i - off_px + 0.5f);
                float yt = my - v.lmax * amp, yb = my - v.lmin * amp;
                if (yb - yt < 1.0f) { float cc = (yt + yb) * 0.5f; yt = cc - 0.5f; yb = cc + 0.5f; }
                DrawRectangle(cx, (int)(yt + 0.5f), 1, (int)(yb - yt + 0.5f) > 0 ? (int)(yb - yt + 0.5f) : 1, c_out);
                float rt = my - v.lrms * amp, rb = my + v.lrms * amp;
                if (rt < yt) rt = yt;
                if (rb > yb) rb = yb;
                if (rb - rt >= 1.0f) DrawRectangle(cx, (int)(rt + 0.5f), 1, (int)(rb - rt + 0.5f), c_rms);
            }
            if (lh >= 11.0f) {                       /* the channel and where it plays (2.1: as wired in the Speaker Map) */
                char lb[64];
                int oo[62];
                int no = sur_mask_outputs(atomic_load(&cl->mc_map[k]), first + k, oo);
                int ln = snprintf(lb, sizeof lb, "%s", mc_channel_name(cl->mc_nch, k));
                if (no <= 0) snprintf(lb + ln, sizeof lb - (size_t)ln, "  silent");
                else {
                    ln += snprintf(lb + ln, sizeof lb - (size_t)ln, "  > out %d", oo[0] + 1);
                    for (int q = 1; q < no && q < 4 && ln < (int)sizeof lb - 8; ++q) ln += snprintf(lb + ln, sizeof lb - (size_t)ln, ", %d", oo[q] + 1);
                    if (no > 4) snprintf(lb + ln, sizeof lb - (size_t)ln, " ...");
                }
                int fs = lh >= 16.0f ? 12 : 10;
                int tw = ui_text_w(lb, fs);
                Rectangle br = { x + 6, ly + 1, (float)tw + 8, (float)fs + 3 };
                DrawRectangleRec(br, (Color){ 0, 0, 0, 150 });
                ui_text(lb, (int)br.x + 4, (int)br.y + 1, fs, no <= 0 ? g_theme.text_dim : g_theme.text);
            }
        }
        g_have = false;
        goto wave_done;
    }
    if (same) { wb_send(); goto wave_done; }
    memcpy(&gk, &nk, sizeof nk); g_have = true;
    wb_begin(x, wave_y, w, wave_h);
    for (int i = 0; i <= ncol; ++i) {
        PeakVal v;
        if (subpix) {
            if (i >= ncol) break;
            /* pixel i is covered by bar i (weight 1-off) and bar i+1 (weight off): we draw both at their real
             * height (no blending of the values -> the peaks do not "pulse"), and the coverage gives the smoothness */
            bool a = wok[i], b = wok[i + 1];
            if (!a && !b) continue;
            float wa = a ? 1.0f - off_px : 0.0f, wb = b ? off_px : 0.0f;
            int pcx = (int)x + i;
            for (int ln = 0; ln < nlanes; ++ln) {
                float my = lane_y[ln] + lane_h * 0.5f;
                float amp = lane_h * 0.5f - 2.0f;
                if (amp < 1.0f) amp = 1.0f;
                float t2[2], b2[2], rt2[2], rb2[2];
                for (int k = 0; k < 2; ++k) {
                    const PeakVal *pv = &wcol[i + k];
                    float mn, mx, rms;
                    if (!stereo) {
                        mn = pv->lmin < pv->rmin ? pv->lmin : pv->rmin;
                        mx = pv->lmax > pv->rmax ? pv->lmax : pv->rmax;
                        rms = sqrtf((pv->lrms * pv->lrms + pv->rrms * pv->rrms) * 0.5f);
                    } else if (ln == 0) { mn = pv->lmin; mx = pv->lmax; rms = pv->lrms; }
                    else                { mn = pv->rmin; mx = pv->rmax; rms = pv->rrms; }
                    float yt = my - mx * amp, yb = my - mn * amp;
                    if (yb - yt < 1.0f) { float cc = (yt + yb) * 0.5f; yt = cc - 0.5f; yb = cc + 0.5f; }
                    float rt = my - rms * amp, rb = my + rms * amp;
                    if (rt < yt) rt = yt;
                    if (rb > yb) rb = yb;
                    t2[k] = yt; b2[k] = yb; rt2[k] = rt; rb2[k] = rb;
                }
                wspan2(pcx, t2[0], b2[0], wa, t2[1], b2[1], wb, c_out);
                float ra = (rb2[0] - rt2[0] >= 1.0f) ? wa : 0.0f, rbw = (rb2[1] - rt2[1] >= 1.0f) ? wb : 0.0f;
                wspan2(pcx, rt2[0], rb2[0], ra, rt2[1], rb2[1], rbw, c_rms);
            }
            continue;
        } else if (detail_mode) {
            if (i >= ncol) break;
            v = detail_buf[i];
        } else {
            if (!use_cols) {
                double f0 = (q0 + i) * fpp, f1 = f0 + fpp;
                int64_t st = (int64_t)floor(f0), en = (int64_t)floor(f1);   /* may also be outside the clip (step 36j) */
                if (en <= st) en = st + 1;
                if (!session_clip_peaks_list(cl, draw_edits, st, en, &v)) continue;
            } else {
                if (!wok[i]) continue;
                v = wcol[i];
            }
        }
        int cx = subpix ? (int)x + i : (int)floorf(x + (float)i - off_px + 0.5f);
        for (int ln = 0; ln < nlanes; ++ln) {
            float mn, mx, rms;
            if (!stereo) {
                mn = v.lmin < v.rmin ? v.lmin : v.rmin;
                mx = v.lmax > v.rmax ? v.lmax : v.rmax;
                rms = sqrtf((v.lrms * v.lrms + v.rrms * v.rrms) * 0.5f);
            } else if (ln == 0) { mn = v.lmin; mx = v.lmax; rms = v.lrms; }
            else                { mn = v.rmin; mx = v.rmax; rms = v.rrms; }
            float my = lane_y[ln] + lane_h * 0.5f;
            float amp = lane_h * 0.5f - 2.0f;
            if (amp < 1.0f) amp = 1.0f;
            float yt = my - mx * amp, yb = my - mn * amp;
            if (yb - yt < 1.0f) { float c = (yt + yb) * 0.5f; yt = c - 0.5f; yb = c + 0.5f; }
            /* outline with soft ends: whole pixels + partly transparent end pixels */
            int it = (int)ceilf(yt), ib = (int)floorf(yb);
            if (ib > it) wb_rect(cx, it, 1, ib - it, c_out);
            Color e = c_out;
            e.a = (unsigned char)(c_out.a * ((float)it - yt));
            if (e.a) wb_rect(cx, it - 1, 1, 1, e);
            e.a = (unsigned char)(c_out.a * (yb - (float)ib));
            if (e.a) wb_rect(cx, ib, 1, 1, e);
            /* core: +-RMS around the middle, but not outside the outline */
            float rt = my - rms * amp, rb = my + rms * amp;
            if (rt < yt) rt = yt;
            if (rb > yb) rb = yb;
            if (rb - rt >= 1.0f) wb_rect(cx, (int)(rt + 0.5f), 1, (int)(rb - rt + 0.5f), c_rms);
        }
    }
    wb_end();
    wave_done:;
    {   /* 1.4.14 / 1.4.17: the separated parts in their colours */
        int sn = session_clip_stem_count(s, g_selected_ch, g_selected_scene);
        if (sn == 2 && cl->stem_peaks[0]) {
            /* vocals + instrumental: the voice over the whole picture */
            Peaks *vp = cl->stem_peaks[0];
            float vg = session_clip_stem_gain(s, g_selected_ch, g_selected_scene, 0);
            Color vc = stem_color(session_clip_stem_name(s, g_selected_ch, g_selected_scene, 0), chan_col);
            vc.a = 210;
            for (int i = 0; i < ncol && vg > 0.0f; ++i) {
                double f0 = (q0 + i) * fpp, f1 = f0 + fpp;
                int64_t st = (int64_t)floor(f0), en = (int64_t)floor(f1);
                if (en <= st) en = st + 1;
                PeakVal v;
                if (!session_peaks_list(cl, vp, draw_edits, st, en, &v)) continue;
                int cx = (int)floorf(x + (float)i - off_px + 0.5f);
                for (int ln = 0; ln < nlanes; ++ln) {
                    float mn, mx;
                    if (!stereo) { mn = v.lmin < v.rmin ? v.lmin : v.rmin; mx = v.lmax > v.rmax ? v.lmax : v.rmax; }
                    else if (ln == 0) { mn = v.lmin; mx = v.lmax; }
                    else { mn = v.rmin; mx = v.rmax; }
                    float my = lane_y[ln] + lane_h * 0.5f, amp = lane_h * 0.5f - 2.0f;
                    if (amp < 1.0f) amp = 1.0f;
                    float yt = my - mx * vg * amp, yb = my - mn * vg * amp;
                    if (yb - yt >= 1.0f) DrawRectangle(cx, (int)(yt + 0.5f), 1, (int)(yb - yt + 0.5f), vc);
                }
            }
        } else if (sn > 2) {
            /* more parts: stacked - each column is split among the parts by how loud each one is there,
             * from the middle outwards (the order of the strip), so the eye sees where the drums, the bass ... are */
            float pg[STEM_MAX];
            Color pc[STEM_MAX];
            bool any = false;
            for (int k = 0; k < sn; ++k) {
                pg[k] = cl->stem_peaks[k] ? session_clip_stem_gain(s, g_selected_ch, g_selected_scene, k) : 0.0f;
                pc[k] = stem_color(session_clip_stem_name(s, g_selected_ch, g_selected_scene, k), chan_col);
                pc[k].a = 225;
                if (pg[k] > 0.0f) any = true;
            }
            for (int i = 0; i < ncol && any; ++i) {
                double f0 = (q0 + i) * fpp, f1 = f0 + fpp;
                int64_t st = (int64_t)floor(f0), en = (int64_t)floor(f1);
                if (en <= st) en = st + 1;
                float a[STEM_MAX][2];
                float sum[2] = { 0, 0 };
                for (int k = 0; k < sn; ++k) {
                    a[k][0] = a[k][1] = 0.0f;
                    PeakVal v;
                    if (pg[k] <= 0.0f || !session_peaks_list(cl, cl->stem_peaks[k], draw_edits, st, en, &v)) continue;
                    float l = fmaxf(fabsf(v.lmin), fabsf(v.lmax)), rr = fmaxf(fabsf(v.rmin), fabsf(v.rmax));
                    if (!stereo) l = rr = fmaxf(l, rr);
                    a[k][0] = l * pg[k]; a[k][1] = rr * pg[k];
                    sum[0] += a[k][0]; sum[1] += a[k][1];
                }
                int cx = (int)floorf(x + (float)i - off_px + 0.5f);
                for (int ln = 0; ln < nlanes; ++ln) {
                    int q = stereo ? ln : 0;
                    if (sum[q] <= 1e-6f) continue;
                    float my = lane_y[ln] + lane_h * 0.5f, amp = lane_h * 0.5f - 2.0f;
                    if (amp < 1.0f) amp = 1.0f;
                    /* the height: the loudest part's peak .. the sum (what the mix can reach), never past the lane */
                    float tot = sum[q] > 1.0f ? 1.0f : sum[q];
                    float h = tot * amp, acc = 0.0f;
                    for (int k = 0; k < sn; ++k) {
                        if (a[k][q] <= 0.0f) continue;
                        float seg = h * a[k][q] / sum[q];
                        float y0 = acc, y1 = acc + seg;
                        acc = y1;
                        int t0 = (int)(my - y1 + 0.5f), t1 = (int)(my - y0 + 0.5f);
                        if (t1 > t0) DrawRectangle(cx, t0, 1, t1 - t0, pc[k]);              /* the upper half */
                        int b0 = (int)(my + y0 + 0.5f), b1 = (int)(my + y1 + 0.5f);
                        if (b1 > b0) DrawRectangle(cx, b0, 1, b1 - b0, pc[k]);              /* mirrored below */
                    }
                }
            }
        }
    }
    {   /* step 36j: outside the clip - darkened (the parts there do not play) + the clip's boundaries */
        double sx0 = x + (0.0 - view0) / (double)visible_frames * w, sx1 = x + ((double)total_frames - view0) / (double)visible_frames * w;
        Color dim = { 0, 0, 0, 130 };
        if (sx0 > x) DrawRectangle((int)x, (int)wave_y, (int)((sx0 < x + w ? sx0 : x + w) - x), (int)wave_h, dim);
        if (sx1 < x + w) { float a = sx1 > x ? (float)sx1 : x; DrawRectangle((int)a, (int)wave_y, (int)(x + w - a), (int)wave_h, dim); }
        Color bl = g_theme.text_dim; bl.a = 160;
        if (sx0 > x && sx0 < x + w) aa_vbar((float)sx0 - 0.5f, 1.0f, ruler_y, wave_y + wave_h - ruler_y, bl);
        if (sx1 > x && sx1 < x + w) aa_vbar((float)sx1 - 0.5f, 1.0f, ruler_y, wave_y + wave_h - ruler_y, bl);
    }
    EndScissorMode();
    if (stereo && !mc_multi) {
        ui_text("L", (int)(x + w) - 12, (int)lane_y[0] + 2, 10, g_theme.text_dim);    /* on the right: the start marker is on the left */
        ui_text("R", (int)(x + w) - 12, (int)lane_y[1] + 2, 10, g_theme.text_dim);
    }

    // Helper: frame → screen X
    #define FRAME_TO_X(f) (x + (float)(((double)(f) - view0) / \
                                     (double)visible_frames) * w)

    // ===== Cutting: grid (Shift), parts, fades, scissors =====
    Vector2 emouse = GetMousePosition();
    double mouse_f = view0 + (double)(emouse.x - x) / (double)w * (double)visible_frames;
    bool in_wave_rect = CheckCollisionPointRec(emouse, (Rectangle){ x, wave_y, w, wave_h });
    BeginScissorMode((int)x, (int)wave_y, (int)w, (int)wave_h);
    if (grid_step > 0 && ed_shift() && (g_tool == TOOL_CUT || g_drag >= DR_MOVE)) {
        double k0 = ceil((view0 - grid_anchor) / grid_step);
        for (double k = k0; ; k += 1.0) {
            double f = grid_anchor + k * grid_step;
            float gx = FRAME_TO_X(f);
            if (gx > x + w) break;
            bool bar = grid_beat > 0 && fabs(fmod(f - grid_anchor, grid_beat * sig_bar())) < 0.5;
            Color gc = g_theme.text; gc.a = bar ? 70 : 28;
            aa_vbar(gx - 0.5f, 1.0f, wave_y, wave_h, gc);
            if (k - k0 > 4000) break;
        }
    }
    if (draw_edits) {
        int hover = (in_wave_rect && g_drag == DR_NONE) ? g_hover_part : -1;
        for (int i = 0; i < draw_edits->n; ++i) {
            const EditSeg *sg = &draw_edits->seg[i];
            float px0 = FRAME_TO_X(sg->t), px1 = FRAME_TO_X(sg->t + sg->len);
            if (px1 < x || px0 > x + w) continue;
            bool sel = (i == g_part && !g_preview) ;
            int64_t fin, fout;
            edit_seg_fades(sg, &fin, &fout);
            if (draw_cut) {                                     /* top band + edges */
                Color band = sel ? g_theme.selection : chan_col;
                band.a = sel ? 230 : 150;
                DrawRectangle((int)px0, (int)wave_y, (int)(px1 - px0 + 0.5f), 4, band);
                if (sel) DrawRectangleLinesEx((Rectangle){ px0, wave_y, px1 - px0, wave_h }, 1.5f, g_theme.selection);
                Color edge = g_theme.text; edge.a = 150;
                aa_vbar(px0 - 0.5f, 1.0f, wave_y, wave_h, edge);
                aa_vbar(px1 - 0.5f, 1.0f, wave_y, wave_h, edge);
            }
            if ((sg->fi1 > sg->fi0) || (sg->fo1 > sg->fo0)) {  /* level curve */
                Color fc = g_theme.text; fc.a = 210;
                float prevx = 0, prevy = 0; bool have = false;
                for (float gx = px0 < x ? x : px0; gx <= (px1 > x + w ? x + w : px1); gx += 2.0f) {
                    double f = view0 + (double)(gx - x) / (double)w * (double)visible_frames;
                    float g = edit_gain(draw_edits, i, (int64_t)f - sg->t);
                    float gy = wave_y + 3 + (1.0f - g) * (wave_h - 5);
                    if (have) DrawLineEx((Vector2){ prevx, prevy }, (Vector2){ gx, gy }, 1.5f, fc);
                    prevx = gx; prevy = gy; have = true;
                }
                Color shade = { 0, 0, 0, 70 };                  /* darkening above the fade curve */
                if (fin > 0) { float a1 = FRAME_TO_X(sg->t + fin); DrawTriangle((Vector2){ px0, wave_y }, (Vector2){ px0, wave_y + wave_h }, (Vector2){ a1, wave_y }, shade); }
                if (fout > 0) { float a0 = FRAME_TO_X(sg->t + sg->len - fout); DrawTriangle((Vector2){ a0, wave_y }, (Vector2){ px1, wave_y + wave_h }, (Vector2){ px1, wave_y }, shade); }
            }
            /* the fade squares: with a hovered/selected part, or if there already is a fade */
            if (px1 - px0 >= 3 * FADE_HANDLE && (i == hover || sel || fin > 0 || fout > 0 || (g_drag >= DR_FADE_IN && i == g_drag_idx))) {
                float hin = FRAME_TO_X(sg->t + fin), hout = FRAME_TO_X(sg->t + sg->len - fout);
                Color hc = g_theme.text; hc.a = (i == hover || sel) ? 255 : 150;
                DrawRectangleRounded((Rectangle){ hin, wave_y + FADE_Y, FADE_HANDLE, FADE_HANDLE }, 2, hc);
                DrawRectangleRounded((Rectangle){ hout - FADE_HANDLE, wave_y + FADE_Y, FADE_HANDLE, FADE_HANDLE }, 2, hc);
            }
        }
    }
    if (draw_edits) {                                       /* the crossfades: zone + the two curves (X) */
        for (int i = 1; i < draw_edits->n; ++i) {
            const EditSeg *sg = &draw_edits->seg[i];
            if (!ed_seam(draw_edits, i)) continue;
            float jx = FRAME_TO_X(sg->t);
            if (sg->xl + sg->xr <= 0) {                      /* seam only: a small handle at the bottom */
                if (jx < x || jx > x + w) continue;
                Color hc = g_theme.text; hc.a = 170;
                DrawTriangle((Vector2){ jx, wave_y + wave_h - 9 }, (Vector2){ jx - 5, wave_y + wave_h - 1 },
                             (Vector2){ jx + 5, wave_y + wave_h - 1 }, hc);
                continue;
            }
            float a = FRAME_TO_X(sg->t - sg->xl), b = FRAME_TO_X(sg->t + sg->xr);
            if (b < x || a > x + w) continue;
            Color zc = g_theme.selection; zc.a = 40;
            DrawRectangle((int)a, (int)wave_y, (int)(b - a + 0.5f), (int)wave_h, zc);
            Color co = g_theme.text; co.a = 220;
            Color ci = g_theme.selection; ci.a = 240;
            float px = 0, py_o = 0, py_i = 0; bool have = false;
            float gx0 = a < x ? x : a, gx1 = b > x + w ? x + w : b;
            for (float gx = gx0; gx <= gx1 + 0.01f; gx += 2.0f) {
                float u = b > a ? (gx - a) / (b - a) : 0.0f;
                float go = cosf(u * (float)M_PI * 0.5f), gi = sinf(u * (float)M_PI * 0.5f);
                float yo = wave_y + 3 + (1.0f - go) * (wave_h - 5), yi = wave_y + 3 + (1.0f - gi) * (wave_h - 5);
                if (have) {
                    DrawLineEx((Vector2){ px, py_o }, (Vector2){ gx, yo }, 1.5f, co);
                    DrawLineEx((Vector2){ px, py_i }, (Vector2){ gx, yi }, 1.5f, ci);
                }
                px = gx; py_o = yo; py_i = yi; have = true;
            }
            Color hc = g_theme.selection;
            DrawTriangle((Vector2){ jx, wave_y + wave_h - 9 }, (Vector2){ jx - 5, wave_y + wave_h - 1 },
                         (Vector2){ jx + 5, wave_y + wave_h - 1 }, hc);
        }
    }
    g_hover_part = -1;
    if (g_tool == TOOL_CUT && in_wave_rect && !g_ctx_open && g_em_kind == EM_NONE) {
        int64_t cf = ed_snap(mouse_f, grid_step, grid_anchor);
        float cx = FRAME_TO_X(cf);
        aa_vbar(cx - 0.5f, 1.5f, wave_y, wave_h, g_theme.selection);
        icon_scissors(cx + 12, wave_y + 14, g_theme.selection);
    }
    EndScissorMode();

    // ===== Loop markers =====
    float ls_x = FRAME_TO_X(loop_start_f);
    float le_x = FRAME_TO_X(loop_end_f);

    /* Darkening: everything outside the Start..End markers, and with LOOP on — also outside the loop.
     * LOOP off -> the waveform is as usual (only Start/End are visible). */
    {
        Color dim = { 0, 0, 0, 120 };
        float a0 = FRAME_TO_X(start_marker_f), a1 = FRAME_TO_X(end_marker_f);
        if (loop_on) {
            if (ls_x > a0) a0 = ls_x;
            if (le_x < a1) a1 = le_x;
        }
        if (a1 < a0) a1 = a0;                         /* nothing plays: everything is dark */
        float l1 = a0 > x + w ? x + w : a0;
        if (l1 > x) aa_vbar(x, l1 - x, wave_y, wave_h, dim);
        float r0 = a1 < x ? x : a1;
        if (r0 < x + w) aa_vbar(r0, x + w - r0, wave_y, wave_h, dim);
    }

    if (loop_on) {

        Color marker_col = loop_on ? g_theme.solo : g_theme.text_dim;

        if (loop_on) {
            /* LOOP on: the markers stick up ABOVE the start/end markers (in the lower half of the ruler)
             * and are joined by a band at the top. The band is dragged: in the middle = moves the whole loop, at the ends = only that end. */
            float by = ruler_y + LOOP_BAR_Y, bh = LOOP_BAR_H;
            float bx0 = ls_x < x ? x : ls_x, bx1 = le_x > x + w ? x + w : le_x;
            Color bar = g_theme.solo;
            bar.a = 210;
            if (bx1 > bx0) DrawRectangle((int)bx0, (int)by, (int)(bx1 - bx0 + 0.5f), (int)bh, bar);
            if (bx1 - bx0 > 44) {
                int tw = ui_text_w("LOOP", 10);
                float cx = (bx0 + bx1) * 0.5f;
                ui_text("LOOP", (int)(cx - tw / 2), (int)by, 10, g_theme.bg);
            }
            for (int k = 0; k < 2; ++k) {
                float mx = k == 0 ? ls_x : le_x;
                if (mx < x - 1 || mx > x + w + 1) continue;
                aa_vbar(mx - 1.5f, 3.0f, by, wave_y + wave_h - by, g_theme.solo);
                /* "tab" for grabbing the end */
                DrawRectangle((int)mx - (k == 0 ? 1 : 5), (int)by, 6, (int)bh, g_theme.solo);
            }
        } else if (ls_x >= x && ls_x <= x + w) {
            DrawRectangle((int)ls_x - 1, (int)wave_y, 3, (int)wave_h, marker_col);
            DrawTriangle(
                (Vector2){ ls_x - 5, wave_y },
                (Vector2){ ls_x + 5, wave_y },
                (Vector2){ ls_x,     wave_y + 8 },
                marker_col);
        }

        if (!loop_on && le_x >= x && le_x <= x + w) {
            DrawRectangle((int)le_x - 1, (int)wave_y, 3, (int)wave_h, marker_col);
            DrawTriangle(
                (Vector2){ le_x - 5, wave_y },
                (Vector2){ le_x + 5, wave_y },
                (Vector2){ le_x,     wave_y + 8 },
                marker_col);
        }
    }

    // ===== Start / End markers =====
    float sm_x = FRAME_TO_X(start_marker_f);
    /* step 36k: the End marker dragged PAST the end of the clip - the clip will get longer (silence / shifted parts at the back).
     * While dragging - it is only shown; on release the clip is extended. */
    if (g_end_ext > 0 && g_dragging_loop_marker != 4) {
        const EditList *ce = session_clip_edits(s, g_selected_ch, g_selected_scene);
        if (ce && g_end_ext > (int64_t)total_frames) {
            float z0 = atomic_load(&cl->wave_zoom);
            EditList *nl = edit_set_len(ce, g_end_ext);
            if (nl && session_clip_apply_edits(s, g_selected_ch, g_selected_scene, nl, -1) == 0) {
                session_set_end_marker(s, g_selected_ch, g_selected_scene, (size_t)g_end_ext);
                /* so the view does not "jump": the same frames per pixel */
                session_set_wave_zoom(s, g_selected_ch, g_selected_scene, z0 * (float)((double)g_end_ext / (double)total_frames));
                char msg[64];
                snprintf(msg, sizeof msg, "Clip is longer now (%.1f s)", (double)g_end_ext / (cl->wav.sample_rate > 0 ? cl->wav.sample_rate : 48000));
                ui_flash_message(msg);
            }
        }
        g_end_ext = 0;
    }
    float em_x = FRAME_TO_X(g_end_ext > 0 ? (double)g_end_ext : (double)end_marker_f);

    Color sm_col = (Color){ 80, 240, 100, 255 };
    Color em_col = (Color){ 240, 80, 80, 255 };

    if (sm_x >= x && sm_x <= x + w) {
        aa_vbar(sm_x - 1.5f, 3.0f, wave_y, wave_h, sm_col);
        DrawTriangle(                                    /* step 36i: bigger - easier to grab */
            (Vector2){ sm_x - 10, wave_y },
            (Vector2){ sm_x + 10, wave_y },
            (Vector2){ sm_x,     wave_y + 18 },
            sm_col);
    }

    if (em_x >= x && em_x <= x + w) {
        aa_vbar(em_x - 1.5f, 3.0f, wave_y, wave_h, em_col);
        DrawTriangle(
            (Vector2){ em_x - 10, wave_y },
            (Vector2){ em_x + 10, wave_y },
            (Vector2){ em_x,     wave_y + 18 },
            em_col);
    }

    // ===== Dragging the markers =====
    Vector2 mouse = GetMousePosition();
    float drag_zone = 10.0f;                             /* step 36i: wider */

    if (g_dragging_loop_marker != 0) {
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            g_dragging_loop_marker = 0;
        } else if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            float rel_x = mouse.x - x;
            if (rel_x < 0) rel_x = 0;
            if (rel_x > w) rel_x = w;

            double nf = view0 + (double)rel_x / (double)w * (double)visible_frames;
            if (nf < 0.0) nf = 0.0;
            size_t new_frame = (size_t)nf;
            if (new_frame > total_frames) new_frame = total_frames;

            if (g_dragging_loop_marker == 1) {
                if (new_frame > loop_end_f) new_frame = loop_end_f;
                session_set_loop_region(s, g_selected_ch, g_selected_scene,
                                        new_frame, loop_end_f);
            } else if (g_dragging_loop_marker == 2) {
                if (new_frame < loop_start_f) new_frame = loop_start_f;
                session_set_loop_region(s, g_selected_ch, g_selected_scene,
                                        loop_start_f, new_frame);
            } else if (g_dragging_loop_marker == 3) {
                if (new_frame > end_marker_f) new_frame = end_marker_f;
                session_set_start_marker(s, g_selected_ch, g_selected_scene,
                                         new_frame);
            } else if (g_dragging_loop_marker == 4) {
                double nfe = view0 + (double)(mouse.x - x) / (double)w * (double)visible_frames;   /* and past the end */
                double lim = 2.0 * (double)total_frames;
                if (nfe > lim) nfe = lim;
                if (nfe > (double)total_frames + 0.5) {
                    double ga, gb;          /* past the end: snaps to the BARS (if there is a tempo); Shift = free */
                    ed_grid(s, g_selected_ch, g_selected_scene, (double)visible_frames / (double)w, &ga, &gb);
                    if (gb > 0 && !ed_shift()) g_end_ext = (int64_t)llround(ga + floor((nfe - ga) / (gb * sig_bar()) + 0.5) * gb * sig_bar());
                    else g_end_ext = (int64_t)llround(nfe);
                    if (g_end_ext <= (int64_t)total_frames) g_end_ext = 0;
                    else if (end_marker_f != total_frames)
                        session_set_end_marker(s, g_selected_ch, g_selected_scene, total_frames);
                } else {
                    g_end_ext = 0;
                    if (new_frame < start_marker_f) new_frame = start_marker_f;
                    session_set_end_marker(s, g_selected_ch, g_selected_scene,
                                           new_frame);
                }
            } else if (g_dragging_loop_marker == 5) {
                /* the whole loop moves: the length stays, it does not leave the file */
                double fm = view0 + (double)(mouse.x - x) / (double)w * (double)visible_frames;
                double len = (double)g_loop_mv_e0 - (double)g_loop_mv_s0;
                double ns = (double)g_loop_mv_s0 + (fm - g_loop_mv_grab);
                if (ns < 0.0) ns = 0.0;
                if (ns + len > (double)total_frames) ns = (double)total_frames - len;
                if (ns < 0.0) ns = 0.0;
                session_set_loop_region(s, g_selected_ch, g_selected_scene,
                                        (size_t)(ns + 0.5), (size_t)(ns + 0.5) + (size_t)len);
            }
            SetMouseCursor(g_dragging_loop_marker == 5 ? MOUSE_CURSOR_POINTING_HAND : MOUSE_CURSOR_RESIZE_EW);
        }
    }
    else if (tool_owns) {
        /* the cutting tool has taken the mouse */
    }
    else if (loop_on && CheckCollisionPointRec(mouse, (Rectangle){ x, ruler_y + LOOP_BAR_Y - 2, w, LOOP_BAR_H + 2 }) &&
             mouse.x >= (ls_x < le_x ? ls_x : le_x) - 7 && mouse.x <= (ls_x > le_x ? ls_x : le_x) + 7 &&
             !g_ctx_open && !g_ti_active && !g_audio_dlg_open) {
        /* the loop band: an end -> only that marker; the middle -> the whole loop */
        float d_ls = fabsf(mouse.x - ls_x), d_le = fabsf(mouse.x - le_x);
        int mode = (d_ls <= 7.0f && d_ls <= d_le) ? 1 : (d_le <= 7.0f ? 2 : 5);
        SetMouseCursor(mode == 5 ? MOUSE_CURSOR_POINTING_HAND : MOUSE_CURSOR_RESIZE_EW);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            g_dragging_loop_marker = mode;
            g_loop_mv_s0 = loop_start_f;
            g_loop_mv_e0 = loop_end_f;
            g_loop_mv_grab = view0 + (double)(mouse.x - x) / (double)w * (double)visible_frames;
        }
    }
    else if (CheckCollisionPointRec(mouse,
                 (Rectangle){ x, wave_y, w, wave_h })) {

        float d_ls = mouse.x - ls_x;
        float d_le = mouse.x - le_x;
        float d_sm = mouse.x - sm_x;
        float d_em = mouse.x - em_x;
        if (d_ls < 0) d_ls = -d_ls;
        if (d_le < 0) d_le = -d_le;
        if (d_sm < 0) d_sm = -d_sm;
        if (d_em < 0) d_em = -d_em;

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (d_sm < drag_zone && d_sm <= d_ls && d_sm <= d_em) {
                g_dragging_loop_marker = 3;
            } else if (d_em < drag_zone && d_em <= d_ls && d_em <= d_le) {
                g_dragging_loop_marker = 4;
            } else if (d_ls < drag_zone && loop_on) {
                g_dragging_loop_marker = 1;
            } else if (d_le < drag_zone && loop_on) {
                g_dragging_loop_marker = 2;
            }
        }
    }

    // ===== Right click to reset =====
    if (!tool_owns && g_em_kind == EM_NONE && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) &&
        CheckCollisionPointRec(mouse, (Rectangle){ x, wave_y, w, wave_h })) {

        float d_sm = mouse.x - sm_x;
        float d_em = mouse.x - em_x;
        if (d_sm < 0) d_sm = -d_sm;
        if (d_em < 0) d_em = -d_em;

        if (d_sm < 12.0f && d_sm <= d_em) {
            session_reset_start_marker(s, g_selected_ch, g_selected_scene);
        } else if (d_em < 12.0f) {
            session_reset_end_marker(s, g_selected_ch, g_selected_scene);
        }
    }

    // ===== Playhead =====
    float playhead_x = FRAME_TO_X(pos_disp);
    if (g_wave_follow) {                       /* follow: a fixed line in the middle (as in Mixxx) */
        float cxp = x + w * 0.5f;
        DrawRectangle((int)cxp - 1, (int)ruler_y, 3, (int)(wave_y + wave_h - ruler_y), g_theme.selection);
        DrawTriangle((Vector2){ cxp - 6, ruler_y }, (Vector2){ cxp, ruler_y + 7 }, (Vector2){ cxp + 6, ruler_y }, g_theme.selection);
    } else if (playhead_x >= x && playhead_x <= x + w) {
        DrawLine((int)playhead_x, (int)ruler_y,
                 (int)playhead_x, (int)(wave_y + wave_h),
                 g_theme.selection);
    }

    // ===== Zoom + sideways movement =====
    // Mouse (left button on the ruler OR middle button anywhere in the waveform):
    //   up/down = zoom (up zooms in), left/right = moves the view in the same direction.
    //   Diagonally up-left = zooms in and at the same time moves the view to the left.
    // Wheel over the waveform: Ctrl+wheel = zoom around the mouse, wheel alone = sideways movement.
    Rectangle ruler_rect = { x, ruler_y, w, WAVE_RULER_H };
    Rectangle wave_area  = { x, ruler_y, w, h - WAVE_HDR_H };
    bool click_ruler = CheckCollisionPointRec(mouse, ruler_rect);
    bool in_area = CheckCollisionPointRec(mouse, wave_area);

    static bool   zoom_dragging = false;
    static int    zoom_btn = 0;
    static float  zd_start_y = 0.0f, zd_last_x = 0.0f, zd_start_zoom = 1.0f, zd_ratio = 0.0f;
    static double zd_anchor = 0.0, zd_pan = 0.0;
    static float  zd_start_sec = 8.0f;
    static Vector2 zd_p0;
    static bool   zd_fresh = false;
    static Vector2 zd_vm;          /* 1.3.3: a "virtual" mouse made of the movements: the cursor is hidden while
                                    * dragging and does not stop at the screen edge; it comes back where it was */

    bool can_start = !g_ctx_open && !g_ti_active && !g_audio_dlg_open && g_dragging_loop_marker == 0;
    bool start_left   = click_ruler && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    /* double click on the seconds bar = play from there (the Start marker does not move) */
    {
        static double rc_t = -10.0;
        static float  rc_x = -1000.0f;
        if (start_left && can_start && g_em_kind == EM_NONE && !tool_owns) {
            double now = GetTime();
            if (now - rc_t < 0.35 && fabsf(mouse.x - rc_x) < 6.0f) {
                float ratio = (mouse.x - x) / w;
                if (ratio < 0) ratio = 0;
                if (ratio > 1) ratio = 1;
                double np = view0 + (double)ratio * (double)visible_frames;
                if (np < 0.0) np = 0.0;
                if (np > (double)total_frames) np = (double)total_frames;
                session_play_clip_from(s, g_selected_ch, g_selected_scene, (size_t)np);
                rc_t = -10.0;
            } else {
                rc_t = now; rc_x = mouse.x;
            }
        }
    }
    bool start_middle = in_area && IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE);
    if (can_start && !zoom_dragging && (start_left || start_middle)) {
        zoom_dragging = true;
        zoom_btn = start_left ? MOUSE_BUTTON_LEFT : MOUSE_BUTTON_MIDDLE;
        zd_vm = mouse; zd_p0 = mouse; zd_fresh = true;
        zd_start_y = mouse.y;
        zd_last_x = mouse.x;
        zd_start_zoom = zoom;
        zd_start_sec = g_follow_sec;
        float rel_x = mouse.x - x;
        if (rel_x < 0) rel_x = 0;
        if (rel_x > w) rel_x = w;
        zd_ratio = rel_x / w;
        zd_anchor = (double)scroll_frame + (double)zd_ratio * (double)visible_frames;
        zd_pan = 0.0;
    }

    if (zoom_dragging) {
        Vector2 md = GetMouseDelta();
        if (!zd_fresh && !(IsMouseButtonReleased(zoom_btn) || !IsMouseButtonDown(zoom_btn))) {
            zd_vm.x += md.x; zd_vm.y += md.y;
            if (fabsf(zd_vm.x - zd_p0.x) + fabsf(zd_vm.y - zd_p0.y) > 3.0f) GfxMouseRelative(true);
        }
        zd_fresh = false;
    }
    if (zoom_dragging) {
        Vector2 mouse = zd_vm;                       /* 1.3.3: below, "mouse" = the virtual mouse */
        if (IsMouseButtonReleased(zoom_btn) || !IsMouseButtonDown(zoom_btn)) {
            zoom_dragging = false;
            GfxMouseRelative(false);                 /* the cursor appears again where the drag started */
        } else if (g_wave_follow) {                  /* follow: up/down = fewer/more seconds */
            float dy = zd_start_y - mouse.y;
            float sec = zd_start_sec * expf(-dy / 90.0f);
            g_follow_sec = sec < 2.0f ? 2.0f : (sec > FOLLOW_SEC_MAX ? FOLLOW_SEC_MAX : sec);
        } else {
            float dy = zd_start_y - mouse.y;                 // up = positive
            float new_zoom = zd_start_zoom * expf(dy / 90.0f);
            if (new_zoom < WAVE_ZOOM_MIN) new_zoom = WAVE_ZOOM_MIN;
            if (new_zoom > max_zoom) new_zoom = max_zoom;

            size_t new_visible = (size_t)((double)total_frames / (double)new_zoom);
            if (new_visible < WAVE_MIN_VISIBLE) new_visible = WAVE_MIN_VISIBLE;
            if (new_visible > 3 * total_frames) new_visible = 3 * total_frames;

            // the sideways movement is in frames per pixel at the CURRENT zoom
            zd_pan -= (double)(mouse.x - zd_last_x) * (double)new_visible / (double)w;   // left = the view moves right
            zd_last_x = mouse.x;

            double want = zd_anchor - (double)zd_ratio * (double)new_visible + zd_pan;
            double mins = (double)view_lo, maxs = (double)(view_hi - (int64_t)new_visible);
            double clamped = want < mins ? mins : (want > maxs ? maxs : want);
            zd_pan += clamped - want;                        // at the end we do not "accumulate" movement

            session_set_wave_zoom(s, g_selected_ch, g_selected_scene, new_zoom);
            session_set_wave_scroll(s, g_selected_ch, g_selected_scene, (size_t)(int64_t)llround(clamped));
        }
    } else if (in_area && can_start && g_wave_follow) {
        float wheel = GetMouseWheelMove();          /* follow: the wheel zooms in/out */
        if (wheel != 0.0f) {
            float sec = g_follow_sec / powf(1.4f, wheel);
            g_follow_sec = sec < 2.0f ? 2.0f : (sec > FOLLOW_SEC_MAX ? FOLLOW_SEC_MAX : sec);
        }
    } else if (in_area && can_start) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            /* wheel = zoom around the mouse (also with Ctrl); Shift + wheel = sideways movement */
            bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            if (!shift) {
                float rel_x = mouse.x - x;
                if (rel_x < 0) rel_x = 0;
                if (rel_x > w) rel_x = w;
                float ratio = rel_x / w;
                double anchor = (double)scroll_frame + (double)ratio * (double)visible_frames;
                float new_zoom = zoom * powf(1.4f, wheel);
                if (new_zoom < WAVE_ZOOM_MIN) new_zoom = WAVE_ZOOM_MIN;
                if (new_zoom > max_zoom) new_zoom = max_zoom;
                size_t nv = (size_t)((double)total_frames / (double)new_zoom);
                if (nv < WAVE_MIN_VISIBLE) nv = WAVE_MIN_VISIBLE;
                if (nv > 3 * total_frames) nv = 3 * total_frames;
                double ns = anchor - (double)ratio * (double)nv;
                double maxs = (double)(view_hi - (int64_t)nv);
                if (ns < (double)view_lo) ns = (double)view_lo;
                if (ns > maxs) ns = maxs;
                session_set_wave_zoom(s, g_selected_ch, g_selected_scene, new_zoom);
                session_set_wave_scroll(s, g_selected_ch, g_selected_scene, (size_t)(int64_t)llround(ns));
            } else {
                double ns = (double)scroll_frame - (double)wheel * 0.1 * (double)visible_frames;
                double maxs = (double)(view_hi - (int64_t)visible_frames);
                if (ns < (double)view_lo) ns = (double)view_lo;
                if (ns > maxs) ns = maxs;
                session_set_wave_scroll(s, g_selected_ch, g_selected_scene, (size_t)(int64_t)llround(ns));
            }
        }
    }
    (void)zoom_dragging;

    // ===== Click on the waveform → seek =====
    Rectangle wave_rect = { x, wave_y, w, wave_h };
    bool click_wave = CheckCollisionPointRec(mouse, wave_rect);

    bool near_marker = false;
    {
        float d_sm = mouse.x - sm_x;
        float d_em = mouse.x - em_x;
        float d_ls = mouse.x - ls_x;
        float d_le = mouse.x - le_x;
        if (d_sm < 0) d_sm = -d_sm;
        if (d_em < 0) d_em = -d_em;
        if (d_ls < 0) d_ls = -d_ls;
        if (d_le < 0) d_le = -d_le;
        if (d_sm < 8.0f || d_em < 8.0f || (loop_on && (d_ls < 8.0f || d_le < 8.0f))) {
            near_marker = true;
        }
    }

    /* (step 28b) a click on the waveform NO LONGER moves the play position - only a double click on the seconds does */
    (void)click_wave; (void)near_marker;

    #undef FRAME_TO_X
    if (stem_shield) GfxMouseShield(false);
    draw_stem_strip(s, stem_r);                                    /* 1.4.14 */
    draw_tool_tip();
}

