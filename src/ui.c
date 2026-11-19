/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#include "ui.h"
#include "ui_internal.h"
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

// Forward declarations

static void ui_text_input_update(void);
static void ui_text_input_draw(int screen_w, int screen_h);

/* step 36f: slightly rounded corners */




/* Removes the last CHARACTER (not byte) - Cyrillic is 2 bytes, otherwise a broken character is left. */
void utf8_chop_last(char *s)
{
    size_t n = strlen(s);
    if (n == 0) return;
    do { n--; } while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80);
    s[n] = '\0';
}



/* 20 colours for channels and notes. The first 8 (the defaults for new channels) - darker, muted (step 32) */
static const Color PALETTE[N_PALETTE] = {
    { 196, 168,  92, 255 },   /*  0 yellow (ochre) */
    {  74, 142, 178, 255 },   /*  1 sky blue    */
    { 106, 160,  98, 255 },   /*  2 green       */
    { 176,  98, 150, 255 },   /*  3 pink        */
    { 214, 140,  44, 255 },   /*  4 orange (Q-LiM - like the logo, slightly muted) */
    { 128, 112, 186, 255 },   /*  5 lavender    */
    { 182, 182, 186, 255 },   /*  6 white (soft) */
    { 108, 152, 136, 255 },   /*  7 mint        */
    {  22,  22,  24, 255 },   /*  8 black       */
    {  85,  88,  95, 255 },   /*  9 dark grey   */
    {  38,  64, 150, 255 },   /* 10 navy        */
    {  98,  52, 146, 255 },   /* 11 purple      */
    {  34, 104,  62, 255 },   /* 12 dark green  */
    { 150,  34,  42, 255 },   /* 13 dark red    */
    { 232,  72,  66, 255 },   /* 14 red         */
    { 122,  76,  44, 255 },   /* 15 brown       */
    {  36, 150, 156, 255 },   /* 16 teal        */
    { 190, 232,  72, 255 },   /* 17 lime        */
    { 250, 192, 212, 255 },   /* 18 light pink  */
    { 160, 162, 168, 255 },   /* 19 grey        */
};
#define N_CHANNEL_COLORS N_PALETTE

/* step 36i: highlight of a row in a list/menu - slightly inset, so it stays inside the rounded frame */
Rectangle row_hl(Rectangle r) { return (Rectangle){ r.x + 3, r.y + 1, r.width - 6, r.height - 2 }; }

static int color_lum(Color c) { return (299 * c.r + 587 * c.g + 114 * c.b) / 1000; }

/* the real colour from the palette (for filling headers/notes) */
Color palette_raw(int idx)
{
    if (idx < 0) idx = 0;
    Color c = PALETTE[idx % N_PALETTE];
    if (g_ui_style) {                       /* 2.0.0 Modern: the channel colours clearer - more saturated, a bit brighter */
        int l = (c.r * 30 + c.g * 59 + c.b * 11) / 100;
        int v[3] = { c.r, c.g, c.b };
        for (int k = 0; k < 3; ++k) {
            int x = l + (v[k] - l) * 135 / 100;        /* saturation x1.35 */
            if (l > 40) x = x * 108 / 100;               /* (black / dark grey stay as they are) */
            v[k] = x < 0 ? 0 : x > 255 ? 255 : x;
        }
        c.r = (unsigned char)v[0]; c.g = (unsigned char)v[1]; c.b = (unsigned char)v[2];
    }
    return c;
}

/* A colour that is VISIBLE on the dark/light panel (for faders, knobs, lines):
 * a black/dark channel is lightened, a light one on a light theme is darkened. */
Color channel_color(int idx)
{
    Color c = palette_raw(idx);
    int bl = color_lum(g_theme.panel_bg);
    int cl = color_lum(c);
    if (abs(cl - bl) < 45) {
        int t = (bl < 128) ? 255 : 0;
        c.r = (unsigned char)((c.r * 65 + t * 35) / 100);
        c.g = (unsigned char)((c.g * 65 + t * 35) / 100);
        c.b = (unsigned char)((c.b * 65 + t * 35) / 100);
    }
    return c;
}

/* 1.4.2: a text note takes its own colour if one was chosen, otherwise the colour of its channel (was: always the
 * theme's yellow). Used for the cell, the big view at the bottom and the dragged ghost. */
void note_colors(Session *s, int ch, const Clip *cl, Color *bg, Color *border, Color *text)
{
    int ncol = cl->color;
    Color b = ncol > 0 ? palette_raw(ncol - 1) : channel_color(s->channels[ch].color_id);
    Color bd = { (unsigned char)(b.r / 2), (unsigned char)(b.g / 2), (unsigned char)(b.b / 2), 255 };
    if (color_lum(b) < 70) bd = (Color){ 150, 150, 150, 255 };
    if (bg) *bg = b;
    if (border) *border = bd;
    if (text) *text = ui_text_color_on(b);
}

/* The text colour on a given background: white on dark, black on light */
Color ui_text_color_on(Color bg)
{
    return color_lum(bg) < 150 ? (Color){ 245, 245, 245, 255 } : (Color){ 20, 20, 20, 255 };
}

Color ui_palette_color(int idx) { return palette_raw(idx); }
int   ui_palette_count(void)    { return N_PALETTE; }

Color ui_channel_color(int idx)
{
    return channel_color(idx < 0 ? 0 : idx);
}

/* column widths (computed every frame in ui_draw) */
static float g_chx[MAX_CHANNELS + 1];      /* left edge of the column, relative to the start of the grid */
static float g_chw[MAX_CHANNELS];          /* column width (with the gap) */
static int   g_chres_ch = -1;              /* which column is being widened right now */
static float g_chres_x0 = 0.0f, g_chres_w0 = 0.0f;
static int   g_chres_last_ch = -1;
static double g_chres_last_t = 0.0;

bool g_can_undo = false, g_can_redo = false;
void ui_set_undo_state(bool can_undo, bool can_redo)
{
    g_can_undo = can_undo;
    g_can_redo = can_redo;
}

Font g_font;
static bool g_font_loaded = false;

FileBrowser *g_fb = NULL;

int g_dragging_slider = -1;
int g_selected_ch     = -1;
int g_selected_scene  = -1;
int g_hovered_ch      = -1;
int g_hovered_scene   = -1;

bool g_drag_active = false;
static char g_drag_path[512] = {0};
static char g_drag_name[256] = {0};

bool g_drag_clip_active = false;
int  g_drag_clip_src_ch = -1;
int  g_drag_clip_src_scene = -1;

/* ===== Step 36: marking several things (Ctrl/Shift + click) and selecting/moving channels ===== */
static unsigned char g_cmark[MAX_CHANNELS][MAX_SCENES];   /* marked cells */
static int   g_cmark_n = 0;
static int   g_anchor_ch = -1, g_anchor_sc = -1;          /* where Shift + click starts from */
bool  g_chsel[MAX_CHANNELS];                       /* marked channels */
int   g_chsel_n = 0;
int   g_chsel_anchor = -1;
bool  g_ch_focus = false;                           /* a header was clicked last -> Delete deletes the channels */
static int   g_chdrag = -1;                                /* the header being dragged (reorders the channels) */
static Vector2 g_chdrag_p0;
static bool  g_chdrag_moved = false;
static bool  g_cdrag_from_mark = false;                    /* the dragged clip is marked -> all marked ones move */
static bool  g_libsel[FB_MAX_ENTRIES];                     /* marked files in the library */
static int   g_libsel_n = 0, g_libsel_anchor = -1;
static char  g_libsel_dir[FB_MAX_PATH];
static bool  g_drag_multi = false;                         /* several files are being dragged from the library */

static void cmark_clear(void) { if (g_cmark_n) memset(g_cmark, 0, sizeof g_cmark); g_cmark_n = 0; }
bool cmark_get(int c, int sc) { return c >= 0 && c < MAX_CHANNELS && sc >= 0 && sc < MAX_SCENES && g_cmark[c][sc]; }
static void cmark_set(int c, int sc, bool on)
{
    if (c < 0 || c >= MAX_CHANNELS || sc < 0 || sc >= MAX_SCENES || (g_cmark[c][sc] != 0) == on) return;
    g_cmark[c][sc] = on;
    g_cmark_n += on ? 1 : -1;
}
static void chsel_clear(void) { memset(g_chsel, 0, sizeof g_chsel); g_chsel_n = 0; }
static void chsel_set(int c, bool on)
{
    if (c < 0 || c >= MAX_CHANNELS || g_chsel[c] == on) return;
    g_chsel[c] = on;
    g_chsel_n += on ? 1 : -1;
}
void chsel_only(int c) { chsel_clear(); chsel_set(c, true); g_chsel_anchor = c; g_ch_focus = c >= 0; }
static void libsel_clear(void) { memset(g_libsel, 0, sizeof g_libsel); g_libsel_n = 0; }

static float g_scroll_x = 0.0f;
static float g_scroll_y = 0.0f;
static bool  g_scroll_to_sel = false;   /* the arrow keys moved the selection -> make it visible */
static int   g_scene_hl = -1, g_scene_hl_next = -1;   /* the mouse over a scene button -> the clips get a light outline */

float g_waveform_h = WAVEFORM_DEF_H;
static bool  g_waveform_dragging = false;

static char  g_flash_msg[1024] = {0};
static float g_flash_dur = FLASH_DURATION;   /* long messages (e.g. a path) stay longer */
static double g_flash_until = 0.0;

bool  g_ti_active = false;
static bool  g_ti_accepted_flag = false;
static char  g_ti_buffer[TEXT_INPUT_MAX] = {0};
static int   g_ti_len = 0;
static int   g_ti_cursor = 0;
static double g_ti_blink_start = 0.0;

static bool  g_ti_is_rename = false;
static int   g_ti_target_ch = -1;
static int   g_ti_target_scene = -1;

static bool  g_ti_is_rename_ch = false;      /* renaming a CHANNEL */
static int   g_ti_rename_ch = -1;

static bool  g_ti_is_new_note = false;
static int   g_ti_new_note_ch = -1;
static int   g_ti_new_note_scene = -1;

static bool  g_ti_inline = false;

/* Numeric field (BPM): 0 = plain text, 1 = project tempo, 2 = clip BPM (g_ti_num_ch/sc) */
int   g_ti_numeric = 0;
static int   g_ti_num_ch = -1, g_ti_num_sc = -1;
static bool  g_ti_skip_click = false;          /* the click that opened the field does not close it */
static bool  g_ti_select_all = false;          /* the number is selected: the first character replaces it */
Session *g_ui_session = NULL;           /* for applying the number on Enter */
static bool  g_queue_open = false;             /* the queue list is open */
static bool  g_q_just_opened = false;
static int   g_q_drag = -1;                    /* which row is being dragged (-1 = none) */
static float g_q_grab = 0.0f;                  /* where in the row it was grabbed */
static int   g_q_scroll = 0;                   /* the first visible row */
static Rectangle g_q_btn, g_q_rect;            /* the button and the popup (for the mouse) */
static float g_q_anchor_x = 0, g_q_anchor_bottom = 0, g_q_top_limit = 0;
bool  g_wave_follow = false;
Vector2 g_drag_clip_p0;                   /* where the clip was pressed (drag = only if the mouse has moved) */
size_t g_loop_mv_s0, g_loop_mv_e0;        /* moving the whole loop: start/end when grabbed */
double g_loop_mv_grab;                    /* the frame under the mouse when grabbed */
/* Log (indicator at the top right, as in Ardour) */
bool  g_log_open = false, g_log_just_opened = false;
int   g_log_scroll = 0;                 /* rows up from the newest (0 = follow the newest) */
Rectangle g_log_rect, g_log_btn;
/* The fader panel (at the bottom): its top edge is dragged; shrink it a lot — and it hides.
 * It comes back with Ctrl+F or a double click on a clip/channel header. */
#define FOOTER_MIN      CHANNEL_FOOTER_H   /* it cannot be lower (the buttons on the left must fit) */
#define FOOTER_HIDE_AT  90.0f              /* drag below this -> it hides */
static float g_footer_h = CHANNEL_FOOTER_H;
static bool  g_footer_hidden = false;
/* Plugins (step 33): an FX strip above the faders of every channel */
bool      g_fx_open = false;
static bool      g_fx_toggle_req = false;     /* the FX button was pressed - applied before the frame's layout */
int       g_fxp_ch = -1;                 /* open plugin list for this channel */
Vector2   g_fxp_pos;
Rectangle g_fxp_rect;
char      g_fxp_filter[64];
int       g_fxp_scroll = 0;
bool      g_fxp_just = false;
int       g_fxm_ch = -1, g_fxm_idx = -1;  /* menu on a slot (right click) */
int       g_fxm_param = -1;               /* 1.4.14: >= 0 = the menu of this knob (MIDI Learn) */
Vector2   g_fxm_pos;
Rectangle g_fxm_rect;
bool      g_fxm_just = false;
int       g_fx_scroll[MAX_CHANNELS];
/* Queue crossfade (step 17): whether it is on and how many seconds. Remembered in layout.conf. */
#define XF_SEC_MIN      0.1f
#define XF_SEC_DEFAULT  6.0f
static bool  g_xf_on  = false;
static float g_xf_sec = XF_SEC_DEFAULT;
static bool  g_footer_drag = false;
/* Master width (step 24): its left edge is dragged; with a wide master the balance and MUTE are shown */
#define MASTER_W_MAX   460
static float g_master_w = MASTER_W;
static bool  g_mw_drag = false;
static bool  g_master_hidden = false;          /* 1.3.10: the master column is hidden (drag right / Ctrl+Shift+M) */
static float g_mw_x0 = 0, g_mw_w0 = 0;
static double g_mw_last_t = 0;
static float g_fd_y0 = 0, g_fd_h0 = 0;
static double g_fd_last_t = 0;
double g_cell_last_t = 0;
int   g_cell_last_ch = -1, g_cell_last_sc = -1;

/* the panel's height and hidden state are remembered in ~/.config/qlim/layout.conf */
static void layout_path(char *out, size_t n)
{
    const char *h = getenv("HOME");
    snprintf(out, n, "%s/.config/qlim/layout.conf", h ? h : ".");
}
static void layout_save(void)
{
    char p[1024];
    layout_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "footer_h=%d\nfooter_hidden=%d\nxfade_on=%d\nxfade_sec=%.2f\nmaster_w=%d\nfx_open=%d\nmaster_hidden=%d\n",
            (int)g_footer_h, g_footer_hidden ? 1 : 0, g_xf_on ? 1 : 0, g_xf_sec, (int)g_master_w, g_fx_open ? 1 : 0,
            g_master_hidden ? 1 : 0);
    fclose(f);
}
static void layout_load(void)
{
    char p[1024], line[128];
    layout_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        int v;
        float fv;
        if (sscanf(line, "footer_h=%d", &v) == 1 && v >= FOOTER_MIN && v <= 2000) g_footer_h = (float)v;
        else if (sscanf(line, "footer_hidden=%d", &v) == 1) g_footer_hidden = v != 0;
        else if (sscanf(line, "fx_open=%d", &v) == 1) g_fx_open = v != 0;
        else if (sscanf(line, "master_hidden=%d", &v) == 1) g_master_hidden = v != 0;
        else if (sscanf(line, "xfade_on=%d", &v) == 1) g_xf_on = v != 0;
        else if (sscanf(line, "master_w=%d", &v) == 1 && v >= MASTER_W && v <= MASTER_W_MAX) g_master_w = (float)v;
        else if (sscanf(line, "xfade_sec=%f", &fv) == 1 && fv >= XF_SEC_MIN && fv <= XFADE_MAX_SEC) g_xf_sec = fv;
    }
    fclose(f);
}

void ui_toggle_mixer(void)
{
    g_footer_hidden = !g_footer_hidden;
    layout_save();
}            /* the waveform follows the position (as in Mixxx): centre = now */
float g_follow_sec  = FOLLOW_SEC_MAX;   /* how many seconds are visible in this mode (when turned on: the farthest out) */

/* 1.4.8: the time signature of the project: quarter beats in a bar / in one counted beat */
double sig_bar(void) { return g_ui_session ? session_bar_beats(g_ui_session) : 4.0; }
double sig_unit(void) { return g_ui_session ? session_sig_unit(g_ui_session) : 1.0; }
/* "bar.beat" (both from 1) of a position in quarter beats */
void fmt_bar_beat(char *out, size_t n, double b)
{
    double bar = sig_bar(), unit = sig_unit();
    if (b < 0) b = 0;
    long long br = (long long)floor(b / bar + 1e-9);
    double in = b - (double)br * bar;
    snprintf(out, n, "%lld.%d", br + 1, (int)floor(in / unit + 1e-9) + 1);
}
/* a length in quarter beats -> "2 bars", "1 bar", "3.5 beats" */
void fmt_bars(char *out, size_t n, double L)
{
    double bar = sig_bar(), nb = L / bar;
    if (fabs(nb - floor(nb + 0.5)) < 1e-4 && nb >= 0.999) snprintf(out, n, "%d bar%s", (int)floor(nb + 0.5), (int)floor(nb + 0.5) == 1 ? "" : "s");
    else snprintf(out, n, "%g beats", L / sig_unit());
}

/* "128", "127.5", "127.53" */
void fmt_bpm(char *out, size_t n, float v)
{
    snprintf(out, n, "%.2f", v);
    char *dot = strchr(out, '.');
    if (dot) {
        char *e = out + strlen(out) - 1;
        while (e > dot && *e == '0') *e-- = '\0';
        if (e == dot) *e = '\0';
    }
}

/* A number field dragged with the mouse (as in Bitwig): up = more, down = less.
 * 1 pixel = 0.1 BPM; with Shift held = 0.01 BPM (fine).
 * Returns 1 if the value changed (*val), 2 on a double click (-> keyboard editing), otherwise 0. */
int   g_vf_id = 0;          /* which field is grabbed */
static float g_vf_v0, g_vf_acc;    /* value when grabbed; accumulated upward movement (px) */
static bool  g_vf_moved;
static int   g_vf_last_id = 0;
static double g_vf_last_t = 0.0;

int value_field_input(int id, Rectangle r, float *val, bool enabled)
{
    return value_field_input_ex(id, r, val, enabled, BPM_MIN, BPM_MAX, 0.1f, 0.01f);
}
/* General: vmin..vmax, step per pixel (fine with Shift); the value is rounded to 0.01. */
int value_field_input_ex(int id, Rectangle r, float *val, bool enabled,
                                float vmin, float vmax, float step_px, float fine_px)
{
    Vector2 m = GetMousePosition();
    bool hov = CheckCollisionPointRec(m, r);
    if (enabled && hov && g_vf_id == 0) SetMouseCursor(MOUSE_CURSOR_RESIZE_NS);
    if (enabled && hov && g_vf_id == 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        g_vf_id = id;
        g_vf_v0 = *val;
        g_vf_acc = 0.0f;
        g_vf_moved = false;
        return 0;
    }
    if (g_vf_id != id) return 0;
    SetMouseCursor(MOUSE_CURSOR_RESIZE_NS);
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        bool sh = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        float dy = -GetMouseDelta().y;               /* up = positive */
        if (dy > 200.0f) dy = 200.0f;                /* protection against a cursor "jump" (e.g. when the system moves it) */
        if (dy < -200.0f) dy = -200.0f;
        if (!g_vf_moved) {
            g_vf_acc += dy;
            if (fabsf(g_vf_acc) < 3.0f) return 0;    /* a little jitter on a click is not a drag */
            g_vf_moved = true;
            g_vf_acc = 0.0f;
            g_vf_v0 = *val;                          /* exact (unrounded) value during the drag */
            GfxMouseRelative(true);                  /* from here: the cursor is hidden and does not stop at the screen edge */
            return 0;
        }
        if (*val <= 0.0f) return 0;
        float step = sh ? fine_px : step_px;          /* Shift = fine */
        g_vf_v0 += dy * step;
        if (g_vf_v0 < vmin) g_vf_v0 = vmin;
        if (g_vf_v0 > vmax) g_vf_v0 = vmax;
        float nv = floorf(g_vf_v0 * 100.0f + 0.5f) / 100.0f;
        if (nv != *val) { *val = nv; return 1; }
        return 0;
    }
    /* button released */
    g_vf_id = 0;
    if (g_vf_moved) { GfxMouseRelative(false); return 0; }
    double now = GetTime();
    if (g_vf_last_id == id && now - g_vf_last_t < DOUBLE_CLICK_TIME) {
        g_vf_last_id = 0;
        return 2;
    }
    g_vf_last_id = id;
    g_vf_last_t = now;
    return 0;
}
static int   g_ti_rect_x = 0;
static int   g_ti_rect_y = 0;
static int   g_ti_rect_w = 0;
static int   g_ti_rect_h = 0;

double g_last_click_time = 0.0;
int    g_last_click_ch = -1;
int    g_last_click_scene = -1;

int g_dragging_loop_marker = 0;
int64_t g_end_ext = 0;      /* step 36k: the End marker is dragged past the end of the clip (0 = no) */

typedef enum {
    CTX_NONE = 0,
    CTX_CHANNEL,
    CTX_CLIP,
    CTX_EMPTY,
    CTX_SCENE,
} CtxType;

bool    g_ctx_open = false;
static CtxType g_ctx_type = CTX_NONE;
static int     g_ctx_x = 0;
static int     g_ctx_y = 0;
static int     g_ctx_ch = -1;
static int     g_ctx_scene = -1;
static double  g_ctx_open_time = 0.0;

UiActionRequest g_pending_action = { UI_ACTION_NONE, -1, -1 };

bool g_file_menu_open = false;
double g_file_menu_open_time = 0.0;
FileMenuAction g_pending_file_action = FILE_MENU_NONE;

bool g_edit_menu_open = false;
bool   g_play_menu_open = false;        /* 2.0 */
double g_play_menu_open_time = 0.0;
Rectangle g_menu_dd_rect;             /* the open drop-down menu (File/Edit): a click in it does not reach the cells below */
double g_edit_menu_open_time = 0.0;
EditMenuAction g_pending_edit_action = EDIT_MENU_NONE;

bool        g_audio_dlg_open = false;
double      g_audio_dlg_open_time = 0.0;
AudioConfig g_audio_dlg_config;
AudioConfig g_audio_dlg_original;
bool        g_audio_dlg_apply_flag = false;

bool g_midi_learn_mode = false;

// Appearance dialog state
bool   g_about_open = false;
bool   g_perf_open = false;            /* 1.3.7: Edit > Performance... */
bool   g_loud_open = false;            /* 1.4.11: Edit > Loudness... */
bool   g_collect_open = false;         /* 2.0: Collect All & Save - what the compressed files become */
int    g_collect_n = 0, g_collect_choice = -2;   /* -2 = no answer yet */
int    g_collect_sel = 1;              /* the format chosen in the window (WAV_OUT_*), confirmed with Collect */
bool   g_collect_busy = false;         /* the collect thread runs: the progress window */
void ui_open_collect_choice(int n) { g_collect_n = n; g_collect_choice = -2; g_collect_sel = 1; g_collect_open = true; }
void ui_collect_busy(bool on) { g_collect_busy = on; }
bool ui_take_collect_choice(int *fmt)
{
    if (g_collect_open || g_collect_choice == -2) return false;
    *fmt = g_collect_choice;
    g_collect_choice = -2;
    return true;
}
bool   g_plugins_open = false;
bool   g_appearance_open = false;
double g_appearance_open_time = 0.0;
Theme  g_appearance_backup;   // for cancel
char   g_appearance_bk_theme[32], g_appearance_bk_accent[32];
int    g_appearance_bk_font = 14;
int    g_appearance_bk_style = 0, g_appearance_bk_time = 0;
void appearance_restore(void)
{
    g_clip_time = g_appearance_bk_time;
    g_ui_style = g_appearance_bk_style;
    g_theme = g_appearance_backup;
    snprintf(g_theme_name, sizeof g_theme_name, "%s", g_appearance_bk_theme);
    snprintf(g_accent_name, sizeof g_accent_name, "%s", g_appearance_bk_accent);
    g_font_size = g_appearance_bk_font;
}

#define CLIPBOARD_PATH_MAX 512
#define CLIPBOARD_NAME_MAX NAME_LEN

static bool g_clip_has = false;
static char g_clip_path[CLIPBOARD_PATH_MAX] = {0};
static char g_clip_name[CLIPBOARD_NAME_MAX] = {0};

typedef struct {
    float grid_x;
    float grid_y;
    float grid_w;
    float grid_h;
    float scene_col_x;
    float master_x;
    float channel_w;
    float hdr_y;
    float row_h;
    float footer_y;
    float footer_h;
} Layout;

static Layout L;

int ui_text_w(const char *text, int size)
{
    if (!text) return 0;
    if (!g_font_loaded) return MeasureText(text, size);
    Vector2 v = MeasureTextEx(g_font, text, (float)size, 0);
    return (int)v.x;
}

void ui_text(const char *text, int x, int y, int size, Color color)
{
    if (!text) return;
    if (!g_font_loaded) {
        DrawText(text, x, y, size, color);
        return;
    }
    DrawTextEx(g_font, text,
               (Vector2){ (float)x, (float)y },
               (float)size, 0, color);
}

// New functions for clips/notes – scaled by g_font_size
int ui_text_w_scaled(const char *text, int size)
{
    if (!text) return 0;
    int scaled = (int)((float)size * ((float)g_font_size / 14.0f));
    if (scaled < 6) scaled = 6;
    if (!g_font_loaded) return MeasureText(text, scaled);
    Vector2 v = MeasureTextEx(g_font, text, (float)scaled, 0);
    return (int)v.x;
}

void ui_text_scaled(const char *text, int x, int y, int size, Color color)
{
    if (!text) return;
    int scaled = (int)((float)size * ((float)g_font_size / 14.0f));
    if (scaled < 6) scaled = 6;
    if (!g_font_loaded) {
        DrawText(text, x, y, scaled, color);
        return;
    }
    DrawTextEx(g_font, text,
               (Vector2){ (float)x, (float)y },
               (float)scaled, 0, color);
}

static char *g_clip_edits;          /* step 27: the cut parts of the copied clip */
void ui_clipboard_set_edits(const char *text)
{
    free(g_clip_edits);
    g_clip_edits = NULL;
    if (text) {
        size_t n = strlen(text) + 1;
        g_clip_edits = malloc(n);
        if (g_clip_edits) memcpy(g_clip_edits, text, n);
    }
}
const char *ui_clipboard_edits(void) { return g_clip_edits; }

static int  g_clip_mc, g_clip_mc_out;          /* 1.4.24: how the copied surround file plays */
static char g_clip_mc_orig[1024];
void ui_clipboard_set_mc(int mode, int out, const char *orig)
{
    g_clip_mc = mode; g_clip_mc_out = out;
    snprintf(g_clip_mc_orig, sizeof g_clip_mc_orig, "%s", orig ? orig : "");
}
void ui_clipboard_mc_next(void) { session_load_mc_next(g_clip_mc, g_clip_mc_out, g_clip_mc_orig); }

void ui_clipboard_copy_clip(const char *path, const char *name)
{
    if (!path) return;
    ui_clipboard_set_edits(NULL);
    ui_clipboard_set_mc(0, 0, NULL);
    snprintf(g_clip_path, sizeof(g_clip_path), "%s", path);
    snprintf(g_clip_name, sizeof(g_clip_name), "%s", name ? name : "");
    g_clip_has = true;
}

void ui_clipboard_clear(void)
{
    ui_clipboard_set_edits(NULL);
    g_clip_has = false;
    g_clip_path[0] = '\0';
    g_clip_name[0] = '\0';
}

bool ui_clipboard_has_clip(void)
{
    return g_clip_has;
}

void ui_clipboard_get(char *out_path, size_t path_size,
                      char *out_name, size_t name_size)
{
    if (out_path && path_size > 0) {
        snprintf(out_path, path_size, "%s", g_clip_path);
    }
    if (out_name && name_size > 0) {
        snprintf(out_name, name_size, "%s", g_clip_name);
    }
}

bool ui_take_action(UiActionRequest *out_req)
{
    if (g_pending_action.action == UI_ACTION_NONE) return false;
    if (out_req) *out_req = g_pending_action;
    g_pending_action.action = UI_ACTION_NONE;
    g_pending_action.ch = -1;
    g_pending_action.scene = -1;
    return true;
}

bool ui_context_menu_is_open(void)
{
    return g_ctx_open;
}

bool ui_take_file_action(FileMenuAction *out)
{
    if (g_pending_file_action == FILE_MENU_NONE) return false;
    if (out) *out = g_pending_file_action;
    g_pending_file_action = FILE_MENU_NONE;
    return true;
}

void file_menu_open(void)
{
    g_file_menu_open = true;
    g_file_menu_open_time = GetTime();
}

void file_menu_close(void)
{
    g_file_menu_open = false;
}

void edit_menu_open(void)
{
    g_edit_menu_open = true;
    g_edit_menu_open_time = GetTime();
}

void edit_menu_close(void)
{
    g_edit_menu_open = false;
    g_play_menu_open = false;                   /* 2.0: (every place that closes the menus) */
}

/* 2.0: the Play menu (Launches at a Time, Loudness) */
void play_menu_open(void) { g_play_menu_open = true; g_play_menu_open_time = GetTime(); }
void play_menu_close(void) { g_play_menu_open = false; }

bool ui_take_edit_action(EditMenuAction *out)
{
    if (g_pending_edit_action == EDIT_MENU_NONE) return false;
    if (out) *out = g_pending_edit_action;
    g_pending_edit_action = EDIT_MENU_NONE;
    return true;
}

void ui_init(FileBrowser *fb)
{
    layout_load();
    g_fb = fb;

    int codepoints[1024];
    int cp_count = 0;
    for (int cp = 0x20; cp <= 0x7F; ++cp) codepoints[cp_count++] = cp;
    for (int cp = 0x400; cp <= 0x4FF; ++cp) codepoints[cp_count++] = cp;
    codepoints[cp_count++] = 0x2013;
    codepoints[cp_count++] = 0x2014;
    codepoints[cp_count++] = 0x2018;
    codepoints[cp_count++] = 0x2019;
    codepoints[cp_count++] = 0x201C;
    codepoints[cp_count++] = 0x201D;
    codepoints[cp_count++] = 0x2026;
    codepoints[cp_count++] = 0x00AB;
    codepoints[cp_count++] = 0x00BB;

    g_font = LoadFontEx("assets/DejaVuSans.ttf", 64, codepoints, cp_count);
    if (g_font.texture.id == 0) {
        TraceLog(LOG_WARNING, "Cannot load DejaVuSans, using default font");
        g_font = GetFontDefault();
        g_font_loaded = false;
    } else {
        SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR);
        g_font_loaded = true;
    }

    g_dragging_slider = -1;
    g_selected_ch = -1;
    g_selected_scene = -1;
    g_drag_active = false;
    g_drag_clip_active = false;
    g_drag_clip_src_ch = -1;
    g_drag_clip_src_scene = -1;
    g_scroll_x = 0.0f;
    g_scroll_y = 0.0f;
    g_waveform_h = WAVEFORM_DEF_H;
    g_waveform_dragging = false;
    g_flash_msg[0] = '\0';
    g_flash_until = 0.0;

    g_ti_active = false;
    g_ti_accepted_flag = false;
    g_ti_buffer[0] = '\0';
    g_ti_len = 0;
    g_ti_cursor = 0;
    g_ti_blink_start = 0.0;

    g_ti_is_rename = false;
    g_ti_target_ch = -1;
    g_ti_target_scene = -1;

    g_ti_is_new_note = false;
    g_ti_new_note_ch = -1;
    g_ti_new_note_scene = -1;

    g_ti_is_rename_ch = false;
    g_ti_rename_ch = -1;

    g_ti_inline = false;
    g_ti_rect_x = 0;
    g_ti_rect_y = 0;
    g_ti_rect_w = 0;
    g_ti_rect_h = 0;

    g_last_click_time = 0.0;
    g_last_click_ch = -1;
    g_last_click_scene = -1;

    g_dragging_loop_marker = 0;

    g_ctx_open = false;
    g_ctx_type = CTX_NONE;
    g_ctx_ch = -1;
    g_ctx_scene = -1;

    g_pending_action.action = UI_ACTION_NONE;
    g_pending_action.ch = -1;
    g_pending_action.scene = -1;

    g_file_menu_open = false;
    g_file_menu_open_time = 0.0;
    g_pending_file_action = FILE_MENU_NONE;

    g_edit_menu_open = false;
    g_edit_menu_open_time = 0.0;
    g_pending_edit_action = EDIT_MENU_NONE;
    g_play_menu_open = false;

    g_audio_dlg_open = false;
    g_audio_dlg_open_time = 0.0;
    g_audio_dlg_apply_flag = false;

    g_midi_learn_mode = false;

    g_appearance_open = false;
    g_appearance_open_time = 0.0;

    ui_clipboard_clear();
}

void ui_toggle_library(void)
{
    if (g_fb) fb_toggle(g_fb);
}

/* 1.3.10 */
void ui_toggle_master(void)
{
    g_master_hidden = !g_master_hidden;
    layout_save();
    ui_flash_message(g_master_hidden ? "Master hidden (Ctrl+Shift+M shows it)" : "Master shown");
}
void ui_toggle_meters(void)
{
    PerfSettings *ps = perf_settings();
    ps->meters = !ps->meters;
    perf_save();
    ui_flash_message(ps->meters ? "Level meters shown" : "Level meters hidden (Ctrl+Shift+U shows them)");
}
void ui_zoom_step(int dir)
{
    PerfSettings *ps = perf_settings();
    float z = dir == 0 ? 1.0f : ps->zoom * (dir > 0 ? 1.1f : 1.0f / 1.1f);
    if (fabsf(z - 1.0f) < 0.04f) z = 1.0f;
    if (z < 0.5f) z = 0.5f;
    if (z > 2.5f) z = 2.5f;
    ps->zoom = z;
    perf_save();
    char m[64];
    snprintf(m, sizeof m, "Interface size %.0f %%  (Ctrl+0 = 100 %%)", z * 100.0f);
    ui_flash_message(m);
}

void ui_toggle_waveform(void)
{
    static float prev = WAVEFORM_DEF_H;               /* comes back at the same height */
    if (g_waveform_h < WAVEFORM_MIN_H) {
        g_waveform_h = prev >= WAVEFORM_MIN_H ? prev : WAVEFORM_DEF_H;
    } else {
        prev = g_waveform_h;
        g_waveform_h = 0.0f;
    }
}

void ui_get_hovered_cell(int *ch, int *scene)
{
    if (ch) *ch = g_hovered_ch;
    if (scene) *scene = g_hovered_scene;
}

void ui_flash_message(const char *msg)
{
    if (!msg) return;
    snprintf(g_flash_msg, sizeof(g_flash_msg), "%s", msg);
    g_flash_dur = strlen(g_flash_msg) > 40 ? 6.0f : FLASH_DURATION;
    g_flash_until = GetTime() + g_flash_dur;
}

void ui_get_selection(int *ch, int *scene)
{
    if (ch)    *ch = g_selected_ch;
    if (scene) *scene = g_selected_scene;
}

void ui_pad_box(Session *s, int *c0, int *r0, int *w, int *h)
{
    int bw = 1, bh = 1;
    midimap_pad_box(&bw, &bh);
    int nch = s ? s->channel_count : 0, nsc = s ? s->scene_count : 0;
    int c = g_selected_ch < 0 ? 0 : g_selected_ch, r = g_selected_scene < 0 ? 0 : g_selected_scene;
    if (c > nch - bw) c = nch - bw;                  /* the box stays inside the grid (if it fits) */
    if (r > nsc - bh) r = nsc - bh;
    if (c < 0) c = 0;
    if (r < 0) r = 0;
    if (c0) *c0 = c;
    if (r0) *r0 = r;
    if (w) *w = bw;
    if (h) *h = bh;
}

void ui_move_selection(Session *s, int dch, int dscene)
{
    if (!s) return;
    cmark_clear();
    if (g_ch_focus && dscene == 0 && s->channel_count > 0) {   /* step 36: a selected channel -> the arrow keys move the selection across the channels */
        int c = g_chsel_anchor < 0 ? 0 : g_chsel_anchor + dch;
        if (c < 0) c = 0;
        if (c >= s->channel_count) c = s->channel_count - 1;
        chsel_only(c);
        if (g_selected_scene >= 0) g_selected_ch = c;
        g_scroll_to_sel = true;
        return;
    }
    if (g_ch_focus) { g_ch_focus = false; chsel_clear(); }

    if (g_selected_ch == -1 && g_selected_scene == -1) {
        g_selected_ch = 0;
        g_selected_scene = 0;
        return;
    }

    int ch = g_selected_ch + dch;
    int sc = g_selected_scene + dscene;

    if (sc < 0) sc = 0;
    if (sc >= s->scene_count) sc = s->scene_count - 1;
    if (ch < -1) ch = -1;
    if (ch >= s->channel_count) ch = s->channel_count - 1;

    g_selected_ch = ch;
    g_selected_scene = sc;
    g_scroll_to_sel = true;          /* the next frame scrolls so the selection is visible */
}

int  ui_selected_scene_row(void) { return (g_selected_ch == -1 && g_selected_scene >= 0) ? g_selected_scene : -1; }
void ui_select_scene_row(int sc) { g_selected_ch = -1; g_selected_scene = sc; g_scroll_to_sel = true; }
// ============ Library ============

static void draw_library(FileBrowser *fb, float x, float y, float w, float h)
{
    if (!fb || !fb->visible) return;

    DrawRectangle((int)x, (int)y, (int)w, (int)h, g_theme.panel_bg2);
    DrawRectangle((int)x, (int)y, (int)w, CLIP_HEADER_H, g_theme.panel_bg);

    const char *path = fb->current_dir;
    /* 1.4.12: two buttons on the right: go to the start folder (house), make this folder the start folder (star) */
    float bs = CLIP_HEADER_H - 8;
    Rectangle home_r = { x + w - 2 * (bs + 4) - 2, y + 4, bs, bs }, star_r = { x + w - (bs + 4) - 2, y + 4, bs, bs };
    {
        Vector2 mm = GetMousePosition();
        bool is_start = strcmp(fb->current_dir, fb->start_dir) == 0;
        for (int k = 0; k < 2; ++k) {
            Rectangle r = k ? star_r : home_r;
            bool hv = GfxPointIn(mm, r) && !g_ctx_open && !g_ti_active;
            DrawRectangleRounded(r, R_CELL, hv ? g_theme.cell_hover : g_theme.panel_bg2);
            Color ic = (k == 1 && is_start) ? g_theme.selection : (hv ? g_theme.text : g_theme.text_dim);
            float cx = r.x + r.width / 2, cy = r.y + r.height / 2;
            if (k == 0) {                                 /* a house */
                DrawTriangle((Vector2){ cx, cy - 8 }, (Vector2){ cx - 8, cy - 1 }, (Vector2){ cx + 8, cy - 1 }, ic);
                DrawRectangle((int)(cx - 5), (int)(cy - 1), 10, 8, ic);
                DrawRectangle((int)(cx - 1.5f), (int)(cy + 2), 3, 5, hv ? g_theme.cell_hover : g_theme.panel_bg2);
            } else {                                      /* a star */
                Vector2 pt[10];
                for (int i = 0; i < 10; ++i) {
                    float a = -1.5707963f + (float)i * 0.6283185f, rr = (i & 1) ? 3.6f : 8.5f;
                    pt[i] = (Vector2){ cx + rr * cosf(a), cy + 1 + rr * sinf(a) };
                }
                for (int i = 0; i < 10; ++i)
                    DrawTriangle((Vector2){ cx, cy + 1 }, pt[(i + 1) % 10], pt[i], ic);
            }
            if (hv && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if (k == 0) {
                    if (fb->start_dir[0]) {
                        snprintf(fb->current_dir, sizeof fb->current_dir, "%s", fb->start_dir);
                        fb->scroll_offset = 0;
                        fb_scan(fb);
                    }
                } else {
                    snprintf(fb->start_dir, sizeof fb->start_dir, "%s", fb->current_dir);
                    settings_save_library_dir(fb->start_dir);
                    ui_flash_message("The library will open in this folder from now on");
                }
            }
            if (hv) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
        }
    }
    char display[300];
    snprintf(display, sizeof display, "%s", path);
    {   /* the end of the path, as much as fits before the buttons */
        int maxw = (int)(home_r.x - x - 14);
        const char *tail = path;
        while (*tail && ui_text_w(display, 14) > maxw) {
            ++tail;
            while ((*tail & 0xC0) == 0x80) ++tail;      /* (UTF-8) */
            snprintf(display, sizeof display, "...%s", tail);
        }
    }
    ui_text(display, (int)(x + 8), (int)(y + 8), 14, g_theme.text);

    float row_h = 22;
    int max_rows = (int)((h - CLIP_HEADER_H - 8) / row_h);
    if (max_rows < 1) max_rows = 1;

    Vector2 mouse = GetMousePosition();
    if (strcmp(g_libsel_dir, fb->current_dir) != 0) {          /* another folder -> the marked ones are cleared */
        libsel_clear();
        g_libsel_anchor = -1;
        snprintf(g_libsel_dir, sizeof g_libsel_dir, "%s", fb->current_dir);
    }
    bool lctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool lshift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    for (int i = 0; i < max_rows; ++i) {
        int idx = i + fb->scroll_offset;
        if (idx >= fb->entry_count) break;

        FbEntry *e = &fb->entries[idx];
        float ry = y + CLIP_HEADER_H + 4 + i * row_h;
        Rectangle rr = { x + 4, ry, w - 8, row_h };

        bool hov = CheckCollisionPointRec(mouse, rr);
        bool marked = idx < FB_MAX_ENTRIES && g_libsel[idx];

        if (marked) {                                            /* step 36: marked file */
            Color mc = g_theme.selection;
            mc.a = 150;
            DrawRectangleRec(rr, mc);
        }
        if (hov) {
            Color hc = g_theme.selection;
            hc.a = 80;
            DrawRectangleRec(rr, hc);

            float wheel = GetMouseWheelMove();
            if (wheel != 0.0f) {
                fb_scroll(fb, (int)(-wheel));
            }
        }

        Color txt_col = g_theme.text;
        const char *icon = "o";
        if (e->type == FB_ENTRY_PARENT) {
            icon = "..";
            txt_col = g_theme.text_dim;
        } else if (e->type == FB_ENTRY_DIR) {
            icon = "/";
            txt_col = g_theme.selection;
        } else if (e->type == FB_ENTRY_AUDIO) {
            icon = "*";
            txt_col = g_theme.text;
        }

        ui_text(icon, (int)(x + 8), (int)(ry + 3), 16, txt_col);

        char name_disp[40];
        snprintf(name_disp, sizeof(name_disp), "%s", e->name);
        while (ui_text_w(name_disp, 14) > (w - 40) && strlen(name_disp) > 3) {
            name_disp[strlen(name_disp) - 1] = '\0';
        }
        ui_text(name_disp, (int)(x + 32), (int)(ry + 5), 14, txt_col);

        bool lfree = !g_drag_active && !g_ctx_open && !g_ti_active && !g_audio_dlg_open;
        if (hov && lfree && (lctrl || lshift) && e->type == FB_ENTRY_AUDIO && idx < FB_MAX_ENTRIES &&
            (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))) {
            /* step 36: Ctrl + click = mark/unmark; Shift + click = all from the previous one to this one */
            if (lshift && g_libsel_anchor >= 0 && g_libsel_anchor < fb->entry_count) {
                if (!lctrl) libsel_clear();
                int a0 = g_libsel_anchor < idx ? g_libsel_anchor : idx, a1 = g_libsel_anchor < idx ? idx : g_libsel_anchor;
                for (int k = a0; k <= a1 && k < FB_MAX_ENTRIES; ++k)
                    if (fb->entries[k].type == FB_ENTRY_AUDIO && !g_libsel[k]) { g_libsel[k] = true; ++g_libsel_n; }
            } else {
                g_libsel[idx] = !g_libsel[idx];
                g_libsel_n += g_libsel[idx] ? 1 : -1;
                g_libsel_anchor = idx;
            }
            GfxMouseShield(true);
        } else if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && lfree) {
            if (e->type == FB_ENTRY_AUDIO) {
                g_drag_active = true;
                g_drag_multi = marked && g_libsel_n > 1;            /* a marked one was grabbed -> all marked ones are dragged */
                if (!g_drag_multi) { libsel_clear(); g_libsel_anchor = idx; }
                snprintf(g_drag_path, sizeof(g_drag_path), "%s", e->full_path);
                if (g_drag_multi) snprintf(g_drag_name, sizeof(g_drag_name), "%d files", g_libsel_n);
                else snprintf(g_drag_name, sizeof(g_drag_name), "%s", e->name);
            } else {
                libsel_clear();
                fb_enter(fb, idx);
            }
        }
    }
}

// ============ Scroll bars ============

/* Scrollbars: drawn and DRAGGED. The thumb is dragged; a click in the track jumps there and keeps dragging. */
static int   g_sb_drag = 0;          /* 0 = nothing, 1 = the horizontal one, 2 = the vertical one */
static float g_sb_grab = 0.0f;       /* where in the thumb it was grabbed (px from its start) */

typedef struct { float bar, travel, pos, max_off; } SbGeom;

/* scrollbar geometry from the last frame (for tests) */
static Rectangle g_sb_rect_h, g_sb_rect_v;
static bool      g_sb_have_h = false, g_sb_have_v = false;

static SbGeom sb_geom(float len, float total, float visible, float offset)
{
    SbGeom g = { 0, 0, 0, 0 };
    if (total <= visible || len <= 0) return g;
    g.bar = len * (visible / total);
    if (g.bar < 30) g.bar = 30;
    if (g.bar > len) g.bar = len;
    g.travel = len - g.bar;
    g.max_off = total - visible;
    g.pos = g.max_off > 0 ? (offset / g.max_off) * g.travel : 0;
    if (g.pos < 0) g.pos = 0;
    if (g.pos > g.travel) g.pos = g.travel;
    return g;
}

/* The scrollbar's input. Called BEFORE the grid is drawn, so there is no one-frame lag. vertical: 0 = horizontal */
static void scrollbar_input(int vertical, float x, float y, float w, float h,
                            float total, float visible, float *offset, bool allowed)
{
    const int me = vertical ? 2 : 1;
    float len = vertical ? h : w;
    SbGeom g = sb_geom(len, total, visible, *offset);
    if (g.bar <= 0) { if (g_sb_drag == me) g_sb_drag = 0; return; }

    Vector2 m = GetMousePosition();
    float mpos = vertical ? (m.y - y) : (m.x - x);

    if (g_sb_drag == me) {
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) { g_sb_drag = 0; return; }
        float np = mpos - g_sb_grab;
        if (np < 0) np = 0;
        if (np > g.travel) np = g.travel;
        *offset = g.travel > 0 ? (np / g.travel) * g.max_off : 0;
        return;
    }
    if (!allowed || g_sb_drag != 0 || !IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;

    Rectangle hit = vertical ? (Rectangle){ x - 2, y, w + 4, h } : (Rectangle){ x, y - 2, w, h + 2 };
    if (!CheckCollisionPointRec(m, hit)) return;
    g_sb_drag = me;
    if (mpos >= g.pos && mpos <= g.pos + g.bar) g_sb_grab = mpos - g.pos;        /* grabbed thumb */
    else {                                                                          /* click in the track: centre the thumb there */
        g_sb_grab = g.bar * 0.5f;
        float np = mpos - g_sb_grab;
        if (np < 0) np = 0;
        if (np > g.travel) np = g.travel;
        *offset = g.travel > 0 ? (np / g.travel) * g.max_off : 0;
    }
}

static void draw_scrollbar(int vertical, float x, float y, float w, float h,
                           float total, float visible, float offset)
{
    float len = vertical ? h : w;
    SbGeom g = sb_geom(len, total, visible, offset);
    if (g.bar <= 0) return;

    DrawRectangle((int)x, (int)y, (int)w, (int)h, g_theme.scroll_bg);

    Rectangle th = vertical ? (Rectangle){ x, y + g.pos, w, g.bar } : (Rectangle){ x + g.pos, y, g.bar, h };
    Color c = g_theme.scroll_fg;
    Vector2 m = GetMousePosition();
    if (g_sb_drag == (vertical ? 2 : 1) || CheckCollisionPointRec(m, th)) {      /* slightly lighter when hovered/dragged */
        c.r = (unsigned char)(c.r + (255 - c.r) / 3);
        c.g = (unsigned char)(c.g + (255 - c.g) / 3);
        c.b = (unsigned char)(c.b + (255 - c.b) / 3);
    }
    DrawRectangle((int)th.x, (int)th.y, (int)th.width, (int)th.height, c);
}

// ============ Flash ============

static void draw_audio_lost_banner(int screen_w)
{
    if (!audio_is_lost()) return;
    char msg[260];
    snprintf(msg, sizeof msg, "AUDIO DEVICE LOST: %s  -  Edit > Audio... > Refresh", audio_lost_name());
    int tw = ui_text_w(msg, 18);
    int px = (screen_w - tw) / 2, py = MENU_BAR_H + (TOP_BAR_H - 32) / 2 + 6;
    DrawRectangle(px - 20, py - 6, tw + 40, 32, (Color){170, 30, 30, 235});
    DrawRectangleLines(px - 20, py - 6, tw + 40, 32, (Color){255, 140, 140, 255});
    ui_text(msg, px, py, 18, RAYWHITE);
}

static void draw_flash(int screen_w)
{
    double now = GetTime();
    if (now > g_flash_until) return;
    if (g_flash_msg[0] == '\0') return;

    float elapsed = g_flash_dur - (float)(g_flash_until - now);
    float alpha = 1.0f;
    if (elapsed > g_flash_dur - 0.5f) {
        alpha = (g_flash_dur - elapsed) / 0.5f;
        if (alpha < 0) alpha = 0;
    }
    if (alpha > 1) alpha = 1;

    int fs = 20;
    int tw = ui_text_w(g_flash_msg, fs);
    if (tw > screen_w - 80) { fs = 14; tw = ui_text_w(g_flash_msg, fs); }   /* long path: a smaller font */
    float px = (screen_w - tw) / 2;
    float py = MENU_BAR_H + TOP_BAR_H + 20;

    Color bg = g_theme.selection;
    bg.a = (unsigned char)(200 * alpha);

    DrawRectangle((int)(px - 20), (int)py - 8,
                  tw + 40, 36, bg);

    Color fg = RAYWHITE;
    fg.a = (unsigned char)(255 * alpha);
    ui_text(g_flash_msg, (int)px, (int)py + (20 - fs) / 2, fs, fg);
}

// ============ UTF-8 helpers ============

static int utf8_prev_pos(const char *buf, int pos)
{
    if (pos <= 0) return 0;
    int i = pos - 1;
    while (i > 0 && ((unsigned char)buf[i] & 0xC0) == 0x80) i--;
    return i;
}

static int utf8_next_pos(const char *buf, int len, int pos)
{
    if (pos >= len) return len;
    int i = pos + 1;
    while (i < len && ((unsigned char)buf[i] & 0xC0) == 0x80) i++;
    return i;
}

static int utf8_insert_at(char *buf, int *len, int max, int pos, int codepoint)
{
    if (pos < 0) pos = 0;
    if (pos > *len) pos = *len;

    unsigned char out[4];
    int bytes = 0;

    if (codepoint < 0x80) {
        out[0] = (unsigned char)codepoint;
        bytes = 1;
    } else if (codepoint < 0x800) {
        out[0] = 0xC0 | (codepoint >> 6);
        out[1] = 0x80 | (codepoint & 0x3F);
        bytes = 2;
    } else if (codepoint < 0x10000) {
        out[0] = 0xE0 | (codepoint >> 12);
        out[1] = 0x80 | ((codepoint >> 6) & 0x3F);
        out[2] = 0x80 | (codepoint & 0x3F);
        bytes = 3;
    } else if (codepoint < 0x110000) {
        out[0] = 0xF0 | (codepoint >> 18);
        out[1] = 0x80 | ((codepoint >> 12) & 0x3F);
        out[2] = 0x80 | ((codepoint >> 6) & 0x3F);
        out[3] = 0x80 | (codepoint & 0x3F);
        bytes = 4;
    } else {
        return pos;
    }

    if (*len + bytes >= max) return pos;

    for (int i = *len; i >= pos; --i) {
        buf[i + bytes] = buf[i];
    }

    for (int i = 0; i < bytes; ++i) {
        buf[pos + i] = (char)out[i];
    }

    *len += bytes;
    buf[*len] = '\0';

    return pos + bytes;
}

static int utf8_delete_at(char *buf, int *len, int pos)
{
    if (pos < 0 || pos >= *len) return pos;

    int end = pos + 1;
    while (end < *len && ((unsigned char)buf[end] & 0xC0) == 0x80) end++;

    int bytes = end - pos;

    for (int i = end; i <= *len; ++i) {
        buf[i - bytes] = buf[i];
    }

    *len -= bytes;
    buf[*len] = '\0';

    return pos;
}

void ui_text_input_begin(const char *initial_text)
{
    g_ti_active = true;
    g_ti_accepted_flag = false;
    g_ti_blink_start = GetTime();

    g_ti_inline = false;
    g_ti_numeric = 0;
    g_ti_skip_click = false;
    g_ti_select_all = false;
    g_ti_is_rename = false;
    g_ti_is_new_note = false;
    g_ti_is_rename_ch = false;
    g_ti_rename_ch = -1;

    g_ti_buffer[0] = '\0';
    g_ti_len = 0;

    if (initial_text) {
        int n = (int)strlen(initial_text);
        if (n >= TEXT_INPUT_MAX) n = TEXT_INPUT_MAX - 1;
        memcpy(g_ti_buffer, initial_text, n);
        g_ti_buffer[n] = '\0';
        g_ti_len = n;
    }

    g_ti_cursor = g_ti_len;
}

void ui_text_input_begin_at(const char *initial_text,
                            int x, int y, int w, int h)
{
    ui_text_input_begin(initial_text);

    g_ti_inline = true;
    g_ti_rect_x = x;
    g_ti_rect_y = y;
    g_ti_rect_w = w;
    g_ti_rect_h = h;
}

void ui_text_input_end(bool accepted)
{
    g_ti_active = false;
    if (g_ti_numeric) {                        /* BPM: applied here, main.c is not involved */
        int kind = g_ti_numeric;
        g_ti_numeric = 0;
        g_ti_accepted_flag = false;
        if (!accepted || !g_ui_session) return;
        char tmp[TEXT_INPUT_MAX];
        snprintf(tmp, sizeof(tmp), "%s", g_ti_buffer);
        if (kind == 7) {                                     /* 1.4.8: "7/8" (just "7" = 7/4) */
            int sn = 0, sd = 4;
            int got = sscanf(tmp, "%d/%d", &sn, &sd);
            if (got < 1 || session_set_time_sig(g_ui_session, sn, sd) != 0)
                ui_flash_message("Time signature: like 4/4, 7/8, 5/4, 12/8 (top 1-32, bottom 1, 2, 4, 8, 16 or 32)");
            return;
        }
        for (char *c = tmp; *c; ++c) if (*c == ',') *c = '.';
        char *endp = NULL;
        float v = strtof(tmp, &endp);
        if (endp == tmp || !(v > 0.0f)) return;            /* empty/invalid -> nothing changes */
        if (kind == 1) session_set_tempo(g_ui_session, v);
        else if (kind == 3) {                                /* queue crossfade (seconds) */
            if (v < XF_SEC_MIN) v = XF_SEC_MIN;
            if (v > XFADE_MAX_SEC) v = XFADE_MAX_SEC;
            g_xf_sec = floorf(v * 10.0f + 0.5f) / 10.0f;
            if (g_xf_sec < XF_SEC_MIN) g_xf_sec = XF_SEC_MIN;
            session_set_crossfade(g_ui_session, g_xf_on ? g_xf_sec : 0.0f);
            layout_save();
        }
        else           session_set_clip_bpm_value(g_ui_session, g_ti_num_ch, g_ti_num_sc, v);
        return;
    }
    if (accepted) {
        g_ti_accepted_flag = true;
    } else {
        g_ti_is_rename = false;
        g_ti_target_ch = -1;
        g_ti_target_scene = -1;
        g_ti_is_new_note = false;
        g_ti_new_note_ch = -1;
        g_ti_new_note_scene = -1;
        g_ti_is_rename_ch = false;
        g_ti_rename_ch = -1;
    }
}

bool ui_text_input_is_active(void)
{
    return g_ti_active;
}

bool ui_text_input_was_accepted(void)
{
    if (g_ti_accepted_flag) {
        g_ti_accepted_flag = false;
        return true;
    }
    return false;
}

void ui_text_input_get(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    snprintf(out, out_size, "%s", g_ti_buffer);
}

bool ui_text_input_is_rename(void)
{
    return g_ti_is_rename;
}

int ui_text_input_get_rename_ch(void)
{
    return g_ti_target_ch;
}

int ui_text_input_get_rename_scene(void)
{
    return g_ti_target_scene;
}

bool ui_text_input_is_new_note(void)
{
    return g_ti_is_new_note;
}

int ui_text_input_get_new_note_ch(void)
{
    return g_ti_new_note_ch;
}

int ui_text_input_get_new_note_scene(void)
{
    return g_ti_new_note_scene;
}

void ui_request_rename(void)
{
    if (g_selected_ch < 0 || g_selected_scene < 0) {
        ui_flash_message("No clip selected");
        return;
    }

    g_ti_is_rename = true;
    g_ti_target_ch = g_selected_ch;
    g_ti_target_scene = g_selected_scene;
}

void ui_request_rename_channel(int ch)
{
    g_ti_is_rename_ch = true;
    g_ti_rename_ch = ch;
}

bool ui_text_input_is_rename_channel(void) { return g_ti_is_rename_ch; }
int  ui_text_input_get_rename_channel(void) { return g_ti_rename_ch; }

void ui_request_new_note(int ch, int scene)
{
    g_ti_is_new_note = true;
    g_ti_new_note_ch = ch;
    g_ti_new_note_scene = scene;
}

static void ui_text_input_update(void)
{
    if (!g_ti_active) return;

    int cp;
    while ((cp = GetCharPressed()) != 0) {
        if (g_ti_numeric == 7) {               /* 1.4.8: the time signature: digits and one "/" */
            bool digit = cp >= '0' && cp <= '9';
            if (cp == ':' || cp == '\\' || cp == '.') cp = '/';
            if (!digit && cp != '/') continue;
            if (g_ti_select_all) { g_ti_buffer[0] = '\0'; g_ti_len = 0; g_ti_cursor = 0; g_ti_select_all = false; }
            if (cp == '/' && strchr(g_ti_buffer, '/')) continue;
            if (g_ti_len >= 5) continue;
        } else if (g_ti_numeric) {             /* only digits and one dot (a comma becomes a dot) */
            if (cp == ',') cp = '.';
            bool digit = cp >= '0' && cp <= '9';
            if (!digit && cp != '.') continue;
            if (g_ti_select_all) {             /* the selected number is replaced by the new one */
                g_ti_buffer[0] = '\0'; g_ti_len = 0; g_ti_cursor = 0;
                g_ti_select_all = false;
            }
            if (cp == '.' && strchr(g_ti_buffer, '.')) continue;
            if (g_ti_len >= 7) continue;
        }
        if (cp >= 32) {
            g_ti_cursor = utf8_insert_at(g_ti_buffer, &g_ti_len,
                                         TEXT_INPUT_MAX - 1,
                                         g_ti_cursor, cp);
        }
    }

    if (g_ti_select_all && (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressed(KEY_DELETE))) {
        g_ti_buffer[0] = '\0'; g_ti_len = 0; g_ti_cursor = 0;     /* deletes the selection */
        g_ti_select_all = false;
    } else if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (g_ti_cursor > 0) {
            int prev = utf8_prev_pos(g_ti_buffer, g_ti_cursor);
            utf8_delete_at(g_ti_buffer, &g_ti_len, prev);
            g_ti_cursor = prev;
        }
    }

    if (IsKeyPressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE)) {
        if (g_ti_cursor < g_ti_len) {
            utf8_delete_at(g_ti_buffer, &g_ti_len, g_ti_cursor);
        }
    }

    if (g_ti_select_all && (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_RIGHT) ||
                            IsKeyPressed(KEY_HOME) || IsKeyPressed(KEY_END))) {
        g_ti_select_all = false;                                     /* arrow key = remove the selection */
    } else if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) {
        g_ti_cursor = utf8_prev_pos(g_ti_buffer, g_ti_cursor);
    }
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) {
        g_ti_cursor = utf8_next_pos(g_ti_buffer, g_ti_len, g_ti_cursor);
    }

    if (IsKeyPressed(KEY_HOME)) g_ti_cursor = 0;
    if (IsKeyPressed(KEY_END))  g_ti_cursor = g_ti_len;

    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        ui_text_input_end(true);
        return;
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        ui_text_input_end(false);
        return;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (g_ti_skip_click) { g_ti_skip_click = false; return; }
        ui_text_input_end(true);
        return;
    }
    g_ti_skip_click = false;
}

/* Starts editing a BPM in rectangle r. kind: 1 = project, 2 = clip. */
void num_edit_begin(int kind, int ch, int sc, float value, Rectangle r)
{
    char init[32] = "";
    if (value > 0.0f) {
        snprintf(init, sizeof(init), "%.2f", value);
        char *dot = strchr(init, '.');                  /* 128.00 -> 128, 127.50 -> 127.5 */
        if (dot) {
            char *e = init + strlen(init) - 1;
            while (e > dot && *e == '0') *e-- = '\0';
            if (e == dot) *e = '\0';
        }
    }
    ui_text_input_begin_at(init, (int)r.x, (int)r.y, (int)r.width, (int)r.height);
    g_ti_numeric = kind;
    g_ti_num_ch = ch;
    g_ti_num_sc = sc;
    g_ti_skip_click = true;
    g_ti_select_all = init[0] != '\0';
}

/* 1.4.8: the time signature typed in ("7/8") - g_ti_numeric 7 */
void sig_edit_begin(const char *init, Rectangle r)
{
    ui_text_input_begin_at(init, (int)r.x, (int)r.y, (int)r.width, (int)r.height);
    g_ti_numeric = 7;
    g_ti_num_ch = -1;
    g_ti_num_sc = -1;
    g_ti_skip_click = true;
    g_ti_select_all = true;
}

static void ui_text_input_draw(int screen_w, int screen_h)
{
    if (!g_ti_active) return;

    int box_w, box_h, box_x, box_y;

    if (g_ti_inline) {
        box_x = g_ti_rect_x;
        box_y = g_ti_rect_y;
        box_w = g_ti_rect_w;
        box_h = g_ti_rect_h;
        if (box_w < 100) box_w = 100;
        if (box_h < 24) box_h = 24;
    } else {
        box_w = 500;
        box_h = 60;
        box_x = (screen_w - box_w) / 2;
        box_y = (screen_h - box_h) / 2;
    }

    if (!g_ti_inline) {
        GfxDialogBackdrop(70);
    }

    DrawRectangle(box_x, box_y, box_w, box_h, g_theme.panel_bg);
    DrawRectangleLines(box_x, box_y, box_w, box_h, g_theme.selection);
    DrawRectangleLines(box_x + 1, box_y + 1, box_w - 2, box_h - 2,
                       g_theme.text_dim);

    if (!g_ti_inline) {
        ui_text("Enter name (Enter = OK, ESC = Cancel)",
                box_x + 12, box_y - 20, 13, g_theme.text_dim);
    }

    const int font_size = g_ti_inline ? 14 : 22;
    const int text_x = box_x + (g_ti_inline ? 6 : 14);
    const int text_y = g_ti_inline
                       ? box_y + (box_h - font_size) / 2
                       : box_y + 18;

    if (g_ti_select_all && g_ti_len > 0) {
        Color sel = g_theme.selection;
        sel.a = 110;
        DrawRectangle(text_x - 1, text_y - 1, ui_text_w(g_ti_buffer, font_size) + 3, font_size + 3, sel);
    }
    ui_text(g_ti_buffer, text_x, text_y, font_size, g_theme.text);

    double t = GetTime() - g_ti_blink_start;
    bool cursor_on = (((int)(t / 0.5)) % 2) == 0;

    if (cursor_on) {
        char before[TEXT_INPUT_MAX];
        int blen = g_ti_cursor;
        if (blen > TEXT_INPUT_MAX - 1) blen = TEXT_INPUT_MAX - 1;
        memcpy(before, g_ti_buffer, blen);
        before[blen] = '\0';

        int tw_before = ui_text_w(before, font_size);

        DrawRectangle(text_x + tw_before + 1, text_y + 2,
                      2, font_size - 4,
                      g_theme.text);
    }
}

bool ui_get_clip_rect(int ch, int scene,
                      int *x, int *y, int *w, int *h)
{
    if (!x || !y || !w || !h) return false;
    if (L.channel_w <= 0 || L.row_h <= 0) return false;

    if (ch < 0 || ch >= MAX_CHANNELS) return false;
    float cx = L.grid_x + g_chx[ch] - g_scroll_x;
    float cy = L.grid_y + scene * L.row_h - g_scroll_y;
    float cw = g_chw[ch] - GAP;
    float chh = L.row_h - 2;

    *x = (int)cx;
    *y = (int)cy;
    *w = (int)cw;
    *h = (int)chh;

    return true;
}
bool ui_get_channel_header_rect(int ch, int *x, int *y, int *w, int *h)
{
    if (!x || !y || !w || !h || ch < 0 || ch >= MAX_CHANNELS) return false;
    if (L.row_h <= 0) return false;
    *x = (int)(L.grid_x + g_chx[ch] - g_scroll_x);
    *y = (int)L.hdr_y;
    *w = (int)(g_chw[ch] - GAP);
    *h = CLIP_HEADER_H;
    return true;
}

// ============ Context Menu ============

typedef struct {
    const char *label;
    UiAction    action;
    bool        enabled;
    bool        is_separator;
} CtxItem;

static void ctx_menu_open(CtxType type, int ch, int scene, int mx, int my)
{
    g_ctx_open = true;
    g_ctx_type = type;
    g_ctx_ch = ch;
    g_ctx_scene = scene;
    g_ctx_x = mx;
    g_ctx_y = my;
    g_ctx_open_time = GetTime();
}

static void ctx_menu_close(void)
{
    g_ctx_open = false;
    g_ctx_type = CTX_NONE;
}

static int ctx_menu_build(Session *s, CtxItem *items, int max_items)
{
    int n = 0;

    switch (g_ctx_type) {
    case CTX_CHANNEL:
        items[n++] = (CtxItem){ "Rename", UI_ACTION_RENAME, true, false };
        items[n++] = (CtxItem){ "Color...", UI_ACTION_COLOR, true, false };
        items[n++] = (CtxItem){ s && session_get_surround(s, g_ctx_ch, NULL) ? "Surround (on)..." : "Surround...", UI_ACTION_SURROUND, true, false };
        items[n++] = (CtxItem){ "Delete", UI_ACTION_DELETE_CHANNEL, true, false };
        items[n++] = (CtxItem){ NULL, UI_ACTION_NONE, false, true };
        items[n++] = (CtxItem){ "Copy", UI_ACTION_COPY_CLIP, false, false };
        items[n++] = (CtxItem){ "Paste", UI_ACTION_PASTE, g_clip_has, false };
        break;

    case CTX_CLIP:
        if (s && g_ctx_ch >= 0 && g_ctx_scene >= 0 && !session_is_note(s, g_ctx_ch, g_ctx_scene) &&
            !session_is_midi(s, g_ctx_ch, g_ctx_scene)) {          /* (1.3: the queue is for audio clips) */
            /* queue: makes sense only while something is playing */
            items[n++] = (CtxItem){ session_any_playing(s) ? "Add to queue" : "Add to queue (nothing playing)",
                                    UI_ACTION_ADD_QUEUE, session_any_playing(s), false };
            items[n++] = (CtxItem){ NULL, UI_ACTION_NONE, false, true };
        }
        items[n++] = (CtxItem){ "Rename", UI_ACTION_RENAME, true, false };
        if (s && g_ctx_ch >= 0 && g_ctx_scene >= 0 && session_is_note(s, g_ctx_ch, g_ctx_scene))
            items[n++] = (CtxItem){ "Color...", UI_ACTION_COLOR, true, false };
        items[n++] = (CtxItem){ "Delete", UI_ACTION_DELETE_CLIP, true, false };
        if (s && g_ctx_ch >= 0 && g_ctx_scene >= 0 && !session_is_note(s, g_ctx_ch, g_ctx_scene) &&
            !session_is_midi(s, g_ctx_ch, g_ctx_scene))
            items[n++] = (CtxItem){ "Export Audio...", UI_ACTION_EXPORT_AUDIO, true, false };   /* 1.4.14 */
        if (s && g_ctx_ch >= 0 && g_ctx_scene >= 0 && session_clip_mc_channels(s, g_ctx_ch, g_ctx_scene) > 2)
            items[n++] = (CtxItem){ "Surround File...", UI_ACTION_MC_FILE, true, false };        /* 1.4.24 */
        if (s && g_ctx_ch >= 0 && g_ctx_scene >= 0 && !session_is_note(s, g_ctx_ch, g_ctx_scene) &&
            !session_is_midi(s, g_ctx_ch, g_ctx_scene) && sep_supported()) {                    /* 1.4.14 (64-bit only) */
            bool hs = session_clip_has_stems(s, g_ctx_ch, g_ctx_scene);   /* 1.4.17: other parts can be chosen again */
            items[n++] = (CtxItem){ ui_separation_busy() ? "Separate (busy...)" : hs ? "Separate Again..." : "Separate...",
                                    UI_ACTION_SEPARATE, !ui_separation_busy(), false };
            if (hs) items[n++] = (CtxItem){ "Remove Separation", UI_ACTION_UNSEPARATE, true, false };
        }
        if (s && g_ctx_ch >= 0 && g_ctx_scene >= 0 && !session_is_note(s, g_ctx_ch, g_ctx_scene))   /* 1.4.17 */
            items[n++] = (CtxItem){ sched_find(SCHED_CLIP, g_ctx_ch, g_ctx_scene) >= 0 ? "Launch at Time (set)..." : "Launch at Time...",
                                    UI_ACTION_SCHEDULE, true, false };
        items[n++] = (CtxItem){ NULL, UI_ACTION_NONE, false, true };
        items[n++] = (CtxItem){ "Cut", UI_ACTION_CUT_CLIP, true, false };
        items[n++] = (CtxItem){ "Copy", UI_ACTION_COPY_CLIP, true, false };
        items[n++] = (CtxItem){ "Paste", UI_ACTION_PASTE, g_clip_has, false };
        break;

    case CTX_EMPTY: {
        bool inst = s && g_ctx_ch >= 0 && fxchain_has_instrument(s, g_ctx_ch);   /* 1.3 */
        items[n++] = (CtxItem){ inst ? "New MIDI clip" : "New MIDI clip (add an instrument first)", UI_ACTION_NEW_MIDI, inst, false };
        items[n++] = (CtxItem){ "New Note", UI_ACTION_NEW_NOTE, true, false };
        items[n++] = (CtxItem){ NULL, UI_ACTION_NONE, false, true };
        items[n++] = (CtxItem){ "Paste", UI_ACTION_PASTE, g_clip_has, false };
        break;
    }

    case CTX_SCENE:
        items[n++] = (CtxItem){ "Fade to This Scene", UI_ACTION_TRANSITION, true, false };     /* 1.4.17 */
        items[n++] = (CtxItem){ s && sched_find(SCHED_SCENE, -1, g_ctx_scene) >= 0 ? "Launch at Time (set)..." : "Launch at Time...",
                                UI_ACTION_SCHEDULE, true, false };
        items[n++] = (CtxItem){ "Export Scene Audio...", UI_ACTION_EXPORT_SCENE, true, false };
        items[n++] = (CtxItem){ NULL, UI_ACTION_NONE, false, true };
        items[n++] = (CtxItem){ "Delete Scene", UI_ACTION_DELETE_SCENE, true, false };
        items[n++] = (CtxItem){ NULL, UI_ACTION_NONE, false, true };
        items[n++] = (CtxItem){ "Copy", UI_ACTION_COPY_CLIP, false, false };
        items[n++] = (CtxItem){ "Paste", UI_ACTION_PASTE, g_clip_has, false };
        break;

    default:
        break;
    }

    (void)max_items;
    return n;
}

static int ctx_menu_height(CtxItem *items, int n)
{
    int h = CTX_MENU_PAD * 2;
    for (int i = 0; i < n; ++i) {
        if (items[i].is_separator) {
            h += 5;
        } else {
            h += CTX_MENU_ROW_H;
        }
    }
    return h;
}

static void ctx_menu_draw(Session *s, int screen_w, int screen_h)
{
    if (!g_ctx_open) return;

    CtxItem items[16];
    int n = ctx_menu_build(s, items, 16);
    if (n == 0) {
        ctx_menu_close();
        return;
    }

    int menu_h = ctx_menu_height(items, n);
    /* 2.0.0: as wide as the longest line (e.g. "Add to queue (nothing playing)" went out of the menu) */
    int menu_w = CTX_MENU_W;
    for (int i = 0; i < n; ++i)
        if (!items[i].is_separator && items[i].label) {
            int lw = ui_text_w(items[i].label, 14) + 24;
            if (lw > menu_w) menu_w = lw;
        }

    int mx = g_ctx_x;
    int my = g_ctx_y;

    if (mx + menu_w > screen_w) mx = screen_w - menu_w - 4;
    if (my + menu_h > screen_h) my = screen_h - menu_h - 4;
    if (mx < 4) mx = 4;
    if (my < MENU_BAR_H + 4) my = MENU_BAR_H + 4;

    double t = GetTime() - g_ctx_open_time;
    float alpha = 1.0f;
    if (t < 0.15) alpha = (float)(t / 0.15);
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;

    Color bg = g_theme.panel_bg;
    bg.r = (unsigned char)(bg.r + 10);
    bg.g = (unsigned char)(bg.g + 10);
    bg.b = (unsigned char)(bg.b + 10);
    bg.a = (unsigned char)(bg.a * alpha);
    DrawRectangle(mx, my, menu_w, menu_h, bg);
    GfxHoverLayer((Rectangle){ (float)mx, (float)my, (float)menu_w, (float)menu_h });   /* 1.3.12 */

    Color border = g_theme.text_dim;
    border.a = (unsigned char)(border.a * alpha);
    DrawRectangleLines(mx, my, menu_w, menu_h, border);

    Vector2 mouse = GetMousePosition();
    bool mouse_inside_menu = (mouse.x >= mx && mouse.x <= mx + menu_w &&
                              mouse.y >= my && mouse.y <= my + menu_h);

    int cur_y = my + CTX_MENU_PAD;
    int clicked_index = -1;

    for (int i = 0; i < n; ++i) {
        CtxItem *it = &items[i];

        if (it->is_separator) {
            Color sep = g_theme.text_dim;
            sep.a = (unsigned char)(sep.a * 0.5f * alpha);
            DrawLine(mx + 6, cur_y + 2, mx + menu_w - 6, cur_y + 2, sep);
            cur_y += 5;
            continue;
        }

        Rectangle row = { (float)mx, (float)cur_y,
                          (float)menu_w, (float)CTX_MENU_ROW_H };

        bool hovered = it->enabled && mouse_inside_menu &&
                       CheckCollisionPointRec(mouse, row);

        if (hovered) {
            Color hov = g_theme.selection;
            hov.a = (unsigned char)(hov.a * 0.5f * alpha);
            DrawRectangleRec(row_hl(row), hov);
        }

        Color txt = it->enabled ? g_theme.text : g_theme.text_dim;
        txt.a = (unsigned char)(txt.a * alpha);

        ui_text(it->label, mx + 10, cur_y + 5, 14, txt);

        if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            clicked_index = i;
        }

        cur_y += CTX_MENU_ROW_H;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !mouse_inside_menu) {
        ctx_menu_close();
        return;
    }

    if (clicked_index >= 0) {
        CtxItem *it = &items[clicked_index];

        if (it->action == UI_ACTION_NEW_MIDI) {               /* 1.3: an empty MIDI clip of 1 bar, opened in the editor */
            if (s && session_new_midi_clip(s, g_ctx_ch, g_ctx_scene, (float)sig_bar(), "MIDI") == 0) {
                g_selected_ch = g_ctx_ch; g_selected_scene = g_ctx_scene;
                g_ch_focus = false; chsel_clear();
                ui_flash_message("New MIDI clip - double click in the editor below to add notes");
            }
            ctx_menu_close();
            return;
        }
        if (it->action == UI_ACTION_SCHEDULE) {               /* 1.4.17 */
            if (s) ui_open_schedule(s, g_ctx_type == CTX_SCENE ? SCHED_SCENE : SCHED_CLIP, g_ctx_type == CTX_SCENE ? -1 : g_ctx_ch, g_ctx_scene);
            ctx_menu_close();
            return;
        }
        if (it->action == UI_ACTION_SURROUND) {               /* 1.4.19 */
            if (s) ui_open_surround(s, g_ctx_ch);
            ctx_menu_close();
            return;
        }
        if (it->action == UI_ACTION_EXPORT_SCENE) {           /* 1.4.17 */
            if (s) ui_open_export_scene(s, g_ctx_scene);
            ctx_menu_close();
            return;
        }
        if (it->action == UI_ACTION_TRANSITION) {
            if (s) ui_transition_to(s, g_ctx_scene);
            ctx_menu_close();
            return;
        }
        if (it->action == UI_ACTION_SEPARATE) {               /* 1.4.14 */
            if (s) ui_open_separate(s, g_ctx_ch, g_ctx_scene);
            ctx_menu_close();
            return;
        }
        if (it->action == UI_ACTION_UNSEPARATE) {
            if (s && session_clip_set_stems(s, g_ctx_ch, g_ctx_scene, NULL, NULL) == 0) ui_flash_message("The clip plays its original file again");
            ctx_menu_close();
            return;
        }
        if (it->action == UI_ACTION_MC_FILE) {                /* 1.4.24 */
            if (s) ui_open_mc(s, g_ctx_ch, g_ctx_scene, false);
            ctx_menu_close();
            return;
        }
        if (it->action == UI_ACTION_EXPORT_AUDIO) {           /* 1.4.14 */
            if (s) ui_open_export(s, g_ctx_ch, g_ctx_scene);
            ctx_menu_close();
            return;
        }
        if (it->action == UI_ACTION_ADD_QUEUE) {
            if (s && session_queue_add(s, g_ctx_ch, g_ctx_scene)) {
                char m[64];
                snprintf(m, sizeof m, "Added to queue (%d)", session_queue_len(s));
                ui_flash_message(m);
            } else {
                ui_flash_message("Nothing is playing - queue not started");
            }
            ctx_menu_close();
            return;
        }

        g_pending_action.action = it->action;
        g_pending_action.ch = g_ctx_ch;
        g_pending_action.scene = g_ctx_scene;

        ctx_menu_close();
        return;
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        ctx_menu_close();
        return;
    }
}

// ============ Dropdown Widget ============

DropdownState g_dd = { -1, -1, 0.0 };

bool draw_dropdown_button(int id,
                                 const char *label,
                                 float x, float y, float w, float h,
                                 bool is_open)
{
    (void)is_open;

    Rectangle r = { x, y, w, h };
    Vector2 mouse = GetMousePosition();
    bool hov = CheckCollisionPointRec(mouse, r);

    Color bg = g_theme.panel_bg2;
    if (hov) {
        bg.r = (unsigned char)(bg.r + 20);
        bg.g = (unsigned char)(bg.g + 20);
        bg.b = (unsigned char)(bg.b + 20);
    }
    if (g_dd.open_dropdown_id == id) {
        bg = g_theme.selection;
    }

    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, 1, g_theme.text_dim);

    int font_size = 15;
    Color txt_col = (g_dd.open_dropdown_id == id) ? g_theme.bg : g_theme.text;
    ui_text(label, (int)(x + 8), (int)(y + (h - font_size) / 2), font_size, txt_col);

    float ar_x = x + w - 20;
    float ar_y = y + h / 2;
    DrawTriangle(
        (Vector2){ ar_x, ar_y - 3 },
        (Vector2){ ar_x + 8, ar_y - 3 },
        (Vector2){ ar_x + 4, ar_y + 3 },
        txt_col
    );

    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (g_dd.open_dropdown_id == -1 ||
            g_dd.open_dropdown_id == id) {
            return true;
        }
    }

    return false;
}

int draw_dropdown_list(int id,
                              const char *const *items, int count, int current,
                              float x, float y, float w)
{
    if (g_dd.open_dropdown_id != id) return -1;

    double time_since_open = GetTime() - g_dd.open_time;
    bool ignore_clicks = (time_since_open < 0.2);

    const int item_h = 22;
    const int pad = 4;
    int list_h = pad * 2 + count * item_h;

    int screen_h = GetScreenHeight();
    int max_h = screen_h - (int)y - 40;
    if (list_h > max_h) {
        list_h = max_h;
    }
    if (list_h < item_h + pad * 2) {
        list_h = item_h + pad * 2;
    }

    DrawRectangle((int)x, (int)y, (int)w, list_h, g_theme.panel_bg);
    GfxHoverLayer((Rectangle){ x, y, w, (float)list_h });   /* 1.3.12 */
    DrawRectangleLines((int)x, (int)y, (int)w, list_h, g_theme.text_dim);

    Vector2 mouse = GetMousePosition();
    bool inside = (mouse.x >= x && mouse.x <= x + w &&
                   mouse.y >= y && mouse.y <= y + list_h);

    static int scroll_offset = 0;
    static int last_id = -1;
    if (last_id != id) {
        scroll_offset = 0;
        last_id = id;
    }

    int visible_items = (list_h - pad * 2) / item_h;
    if (visible_items < 1) visible_items = 1;

    if (current >= 0) {
        if (current < scroll_offset) scroll_offset = current;
        if (current >= scroll_offset + visible_items)
            scroll_offset = current - visible_items + 1;
    }

    if (inside) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            scroll_offset -= (int)wheel;
            if (scroll_offset < 0) scroll_offset = 0;
            if (scroll_offset > count - visible_items)
                scroll_offset = count - visible_items;
            if (scroll_offset < 0) scroll_offset = 0;
        }
    }

    int cur_y = (int)y + pad;
    int clicked = -1;

    for (int i = 0; i < visible_items && (i + scroll_offset) < count; ++i) {
        int idx = i + scroll_offset;
        Rectangle row = { x, (float)cur_y, w, (float)item_h };

        bool hov = inside && CheckCollisionPointRec(mouse, row);
        bool is_current = (idx == current);

        if (hov) {
            Color hc = g_theme.selection;
            hc.a = 120;
            DrawRectangleRec(row_hl(row), hc);
        } else if (is_current) {
            Color cur_col = g_theme.selection;
            cur_col.a = 60;
            DrawRectangleRec(row_hl(row), cur_col);
        }

        Color txt = g_theme.text;
        if (is_current) txt = g_theme.selection;

        const char *display = items[idx];
        char buf[128];
        snprintf(buf, sizeof(buf), "%s", display);
        while (ui_text_w(buf, 13) > (int)(w - 16) && strlen(buf) > 3) {
            buf[strlen(buf) - 1] = '\0';
        }
        ui_text(buf, (int)(x + 8), cur_y + 4, 13, txt);

        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !ignore_clicks) {
            clicked = idx;
        }

        cur_y += item_h;
    }

    if (count > visible_items) {
        float sb_x = x + w - 4;
        float sb_h = (float)list_h - pad * 2;
        float bar_h = sb_h * ((float)visible_items / (float)count);
        if (bar_h < 20) bar_h = 20;
        float bar_y = y + pad + (sb_h - bar_h) *
                      ((float)scroll_offset / (float)(count - visible_items));

        DrawRectangle((int)sb_x, (int)(y + pad),
                      4, (int)sb_h, g_theme.scroll_bg);
        DrawRectangle((int)sb_x, (int)bar_y,
                      4, (int)bar_h, g_theme.scroll_fg);
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !inside && !ignore_clicks) {
        g_dd.open_dropdown_id = -1;
        g_dd.hovered_item = -1;
        return -1;
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        g_dd.open_dropdown_id = -1;
        g_dd.hovered_item = -1;
        return -1;
    }

    if (clicked >= 0) {
        g_dd.open_dropdown_id = -1;
        g_dd.hovered_item = -1;
        return clicked;
    }

    return -1;
}

// ============ Main function ============

/* ============ Real-time log (popup under the indicator at the top right) ============ */
#define LOG_W      700
#define LOG_ROW_H  15
static void draw_log_popup(int screen_w)
{
    if (!g_log_open) { g_log_rect = (Rectangle){ 0, 0, 0, 0 }; return; }
    applog_mark_seen();                               /* open = seen (the indicator goes back to "Log") */
    float w = LOG_W;
    if (w > screen_w - 16) w = (float)screen_w - 16;
    float h = 380;
    Rectangle r = { (float)screen_w - w - 8, (float)MENU_BAR_H + 2, w, h };
    g_log_rect = r;
    Vector2 m = GetMousePosition();
    bool inside = CheckCollisionPointRec(m, r);

    DrawRectangle((int)r.x + 4, (int)r.y + 4, (int)r.width, (int)r.height, (Color){ 0, 0, 0, 100 });
    DrawRectangleRec(r, g_theme.bg);
    GfxHoverLayer(r);                                        /* 2.0.0 */
    DrawRectangleLines((int)r.x, (int)r.y, (int)r.width, (int)r.height, g_theme.selection);
    ui_text("Log", (int)r.x + 10, (int)r.y + 7, 15, g_theme.text);
    {
        char st[96];
        snprintf(st, sizeof st, "%s   |   %d lines   |   xruns: %u", audio_is_lost() ? "AUDIO DEVICE LOST" : "audio OK",
                 applog_count(), dsp_meter_xruns());
        ui_text(st, (int)r.x + 50, (int)r.y + 9, 12, audio_is_lost() ? g_theme.vu_red : g_theme.text_dim);
    }
    /* Clear */
    Rectangle cr = { r.x + r.width - 62, r.y + 6, 54, 20 };
    bool chov = CheckCollisionPointRec(m, cr);
    DrawRectangleRec(cr, chov ? g_theme.vu_red : g_theme.panel_bg2);
    DrawRectangleLines((int)cr.x, (int)cr.y, (int)cr.width, (int)cr.height, g_theme.text_dim);
    int tw = ui_text_w("Clear", 12);
    ui_text("Clear", (int)(cr.x + (cr.width - tw) / 2), (int)cr.y + 3, 12, chov ? RAYWHITE : g_theme.text);
    if (chov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { applog_clear(); g_log_scroll = 0; }

    float ly = r.y + 32, lh = r.height - 38;
    DrawLine((int)r.x + 6, (int)ly - 3, (int)(r.x + r.width - 6), (int)ly - 3, g_theme.grid_line);
    int vis = (int)(lh / LOG_ROW_H);
    int n = applog_count();
    if (inside) {
        float wh = GetMouseWheelMove();
        if (wh != 0.0f) g_log_scroll += (int)(wh * 3);
    }
    int maxs = n - vis; if (maxs < 0) maxs = 0;
    if (g_log_scroll > maxs) g_log_scroll = maxs;
    if (g_log_scroll < 0) g_log_scroll = 0;
    int first = n - vis - g_log_scroll; if (first < 0) first = 0;

    BeginScissorMode((int)r.x + 2, (int)ly, (int)r.width - 4, (int)lh);
    char line[320];
    for (int k = 0; k < vis; ++k) {
        int lv; double t;
        if (!applog_get(first + k, line, sizeof line, &lv, &t)) break;
        float yy = ly + k * LOG_ROW_H;
        Color c = lv == APPLOG_ERR ? g_theme.vu_red : (lv == APPLOG_WARN ? g_theme.vu_yellow : g_theme.text_dim);
        if (lv != APPLOG_INFO) {
            Color b = c; b.a = 35;
            DrawRectangle((int)r.x + 2, (int)yy, (int)r.width - 4, LOG_ROW_H, b);
        }
        char ts[24];
        int mm = (int)(t / 60), ss = (int)t % 60;
        snprintf(ts, sizeof ts, "%02d:%02d", mm, ss);
        ui_text(ts, (int)r.x + 8, (int)yy + 1, 11, g_theme.text_dim);
        ui_text(line, (int)r.x + 50, (int)yy + 1, 11, lv == APPLOG_INFO ? g_theme.text : c);
    }
    EndScissorMode();
    if (g_log_scroll > 0) {                           /* we are not at the bottom: a small hint */
        const char *hint = "scrolled up - wheel down for live";
        int hw = ui_text_w(hint, 11);
        DrawRectangle((int)(r.x + r.width - hw - 16), (int)(r.y + r.height - 20), hw + 10, 16, g_theme.panel_bg2);
        ui_text(hint, (int)(r.x + r.width - hw - 11), (int)(r.y + r.height - 19), 11, g_theme.selection);
    }

    if (!g_log_just_opened && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !inside && !CheckCollisionPointRec(m, g_log_btn))
        g_log_open = false;
    if (IsKeyPressed(KEY_ESCAPE) && !g_ti_active && !g_ctx_open) g_log_open = false;
    g_log_just_opened = false;
}

/* ============ Queue: list ============
 * The row: number, clip name, channel, × (remove), ≡ (grab and drag up/down — as in streaming apps).
 * A click on the name selects the clip. A click outside the list or Esc closes it. */
#define Q_ROW_H   26
#define Q_HDR_H   44
#define Q_W       380

static void q_trunc(char *buf, size_t n, const char *src, int max_w, int size)
{
    snprintf(buf, n, "%s", src);
    if (ui_text_w(buf, size) <= max_w) return;
    while (buf[0] && ui_text_w(buf, size) > max_w - ui_text_w("...", size)) utf8_chop_last(buf);
    strncat(buf, "...", n - strlen(buf) - 1);
}

/* The crossfade icon: two curves (one fading out, the other fading in) that cross */
static void draw_xfade_icon(Rectangle b, Color c)
{
    float x0 = b.x + 4, x1 = b.x + b.width - 4, y0 = b.y + 4, y1 = b.y + b.height - 4;
    const int N = 10;
    for (int k = 0; k < N; ++k) {
        float t0 = (float)k / N, t1 = (float)(k + 1) / N;
        float xa = x0 + (x1 - x0) * t0, xb = x0 + (x1 - x0) * t1;
        /* fade out: cos, fade in: sin (as it sounds) */
        float o0 = cosf(t0 * (float)M_PI * 0.5f), o1 = cosf(t1 * (float)M_PI * 0.5f);
        float i0 = sinf(t0 * (float)M_PI * 0.5f), i1 = sinf(t1 * (float)M_PI * 0.5f);
        DrawLineEx((Vector2){ xa, y1 - (y1 - y0) * o0 }, (Vector2){ xb, y1 - (y1 - y0) * o1 }, 1.6f, c);
        DrawLineEx((Vector2){ xa, y1 - (y1 - y0) * i0 }, (Vector2){ xb, y1 - (y1 - y0) * i1 }, 1.6f, c);
    }
}

/* a small hint label under a point (x = centre) */
static void draw_hint(const char *txt, float cx, float y)
{
    int tw = ui_text_w(txt, 11);
    int x = (int)(cx - tw / 2 - 5);
    int sw = GetScreenWidth();
    if (x + tw + 10 > sw - 2) x = sw - 2 - tw - 10;
    if (x < 2) x = 2;
    DrawRectangle(x, (int)y, tw + 10, 18, (Color){ 20, 20, 24, 235 });
    DrawRectangleLines(x, (int)y, tw + 10, 18, g_theme.text_dim);
    ui_text(txt, x + 5, (int)y + 3, 11, RAYWHITE);
}

static bool g_xf_save_pending = false;
static void draw_queue_panel(Session *s)
{
    session_set_crossfade(s, g_xf_on ? g_xf_sec : 0.0f);   /* the setting -> the audio (cheap, every frame) */
    if (!g_queue_open) { g_q_rect = (Rectangle){ 0, 0, 0, 0 }; return; }
    int qn = session_queue_len(s);
    float avail = g_q_anchor_bottom - g_q_top_limit - Q_HDR_H - 10;
    int vis = (int)(avail / Q_ROW_H);
    if (vis < 3) vis = 3;
    int rows = qn < vis ? qn : vis;
    if (rows < 1) rows = 1;
    if (g_q_scroll > qn - rows) g_q_scroll = qn - rows;
    if (g_q_scroll < 0) g_q_scroll = 0;
    float h = Q_HDR_H + rows * Q_ROW_H + 8;
    Rectangle r = { g_q_anchor_x, g_q_anchor_bottom - h, Q_W, h };
    g_q_rect = r;

    Vector2 m = GetMousePosition();
    bool inside = CheckCollisionPointRec(m, r);

    /* shadow + background */
    DrawRectangle((int)r.x + 4, (int)r.y + 4, (int)r.width, (int)r.height, (Color){ 0, 0, 0, 90 });
    Color bg = g_theme.panel_bg;
    bg.r = (unsigned char)(bg.r + 8); bg.g = (unsigned char)(bg.g + 8); bg.b = (unsigned char)(bg.b + 8);
    DrawRectangleRec(r, bg);
    GfxHoverLayer(r);                                        /* 2.0.0 */
    DrawRectangleLines((int)r.x, (int)r.y, (int)r.width, (int)r.height, g_theme.selection);

    /* title + "now playing" + Clear */
    ui_text("Queue", (int)r.x + 10, (int)r.y + 6, 15, g_theme.text);
    int ach, asc;
    char now[NAME_LEN + 16], tmp[NAME_LEN];
    if (session_queue_anchor(s, &ach, &asc) && ach < s->channel_count) {
        q_trunc(tmp, sizeof tmp, s->channels[ach].clips[asc].name, Q_W - 70, 11);
        snprintf(now, sizeof now, "Now: %s", tmp);
    } else {
        snprintf(now, sizeof now, "Nothing playing");
    }
    ui_text(now, (int)r.x + 10, (int)r.y + 26, 11, g_theme.text_dim);
    const char *hint = NULL;
    float hint_x = 0;
    bool qact = qn > 0;                     /* Clear works only when there is something in the queue */
    bool free_m = g_q_drag < 0 && (g_vf_id == 0 || g_vf_id == 3) && !g_q_just_opened;
    {   /* Clear: always in its place; grey and inactive with an empty queue */
        Rectangle cr = { r.x + r.width - 58, r.y + 6, 50, 18 };
        bool ch = qact && free_m && g_vf_id == 0 && CheckCollisionPointRec(m, cr);
        DrawRectangleRec(cr, ch ? g_theme.vu_red : g_theme.panel_bg2);
        DrawRectangleLines((int)cr.x, (int)cr.y, (int)cr.width, (int)cr.height,
                           qact ? g_theme.text_dim : g_theme.grid_line);
        int tw = ui_text_w("Clear", 11);
        ui_text("Clear", (int)(cr.x + (cr.width - tw) / 2), (int)cr.y + 3, 11,
                ch ? RAYWHITE : (qact ? g_theme.text : g_theme.grid_line));
        if (ch && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { session_queue_clear(s); qn = 0; }
    }
    {   /* Crossfade: a square with an icon (no label, so it is not confused with Clear) */
        Rectangle xb = { r.x + r.width - 58 - 10 - 30, r.y + 4, 30, 22 };
        bool xh = free_m && g_vf_id == 0 && CheckCollisionPointRec(m, xb);   /* always works (even with an empty queue) */
        Color fill = g_xf_on ? g_theme.selection : g_theme.panel_bg2;
        if (xh) { fill.r = (unsigned char)(fill.r + 25 > 255 ? 255 : fill.r + 25);
                  fill.g = (unsigned char)(fill.g + 25 > 255 ? 255 : fill.g + 25);
                  fill.b = (unsigned char)(fill.b + 25 > 255 ? 255 : fill.b + 25); }
        DrawRectangleRec(xb, fill);
        Color ic = g_xf_on ? (Color){ 25, 25, 30, 255 } : g_theme.text;   /* dark icon on the light background */
        DrawRectangleLines((int)xb.x, (int)xb.y, (int)xb.width, (int)xb.height,
                           g_xf_on ? g_theme.selection : g_theme.text_dim);
        draw_xfade_icon(xb, ic);
        if (CheckCollisionPointRec(m, xb) && g_q_drag < 0 && g_vf_id == 0) {
            hint = g_xf_on ? "Auto crossfade: ON"
                           : "Auto crossfade: OFF";
            hint_x = xb.x + xb.width / 2;
        }
        if (xh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            g_xf_on = !g_xf_on;
            session_set_crossfade(s, g_xf_on ? g_xf_sec : 0.0f);
            layout_save();
            printf("queue: auto crossfade %s (%.1f s)\n", g_xf_on ? "on" : "off", g_xf_sec);
        }

        /* the seconds field — next to the icon, only when it is on */
        if (g_xf_on) {
            Rectangle vr = { xb.x - 6 - 56, r.y + 4, 56, 22 };
            bool editing = g_ti_active && g_ti_numeric == 3;
            bool vh = free_m && CheckCollisionPointRec(m, vr);
            if (!editing) {
                Color vb = g_theme.panel_bg2;
                if (vh || g_vf_id == 3) { vb.r = (unsigned char)(vb.r + 25); vb.g = (unsigned char)(vb.g + 25); vb.b = (unsigned char)(vb.b + 25); }
                DrawRectangleRec(vr, vb);
                DrawRectangleLines((int)vr.x, (int)vr.y, (int)vr.width, (int)vr.height, g_theme.text_dim);
                char lbl[24];
                snprintf(lbl, sizeof lbl, "%.1f s", g_xf_sec);
                int tw = ui_text_w(lbl, 13);
                ui_text(lbl, (int)(vr.x + (vr.width - tw) / 2), (int)vr.y + 4, 13, g_theme.solo);
                if (vh && g_vf_id == 0) {
                    hint = "Drag up/down, double-click to type";
                    hint_x = vr.x + vr.width / 2;
                }
            }
            float v = g_xf_sec;
            int res = value_field_input_ex(3, vr, &v, (free_m && !editing) || g_vf_id == 3,
                                           XF_SEC_MIN, XFADE_MAX_SEC, 0.05f, 0.01f);
            if (res == 1) {
                g_xf_sec = floorf(v * 10.0f + 0.5f) / 10.0f;      /* shown/used in 0.1 s steps */
                if (g_xf_sec < XF_SEC_MIN) g_xf_sec = XF_SEC_MIN;
                session_set_crossfade(s, g_xf_sec);
            } else if (res == 2) {
                num_edit_begin(3, -1, -1, g_xf_sec, (Rectangle){ vr.x - 14, vr.y, vr.width + 14, vr.height });
            }
            if (g_vf_id == 0 && g_xf_save_pending) { g_xf_save_pending = false; layout_save(); }
            if (res == 1) g_xf_save_pending = true;          /* saved when you release the mouse */
        }
    }
    DrawLine((int)r.x + 6, (int)(r.y + Q_HDR_H - 4), (int)(r.x + r.width - 6), (int)(r.y + Q_HDR_H - 4), g_theme.grid_line);

    float list_y = r.y + Q_HDR_H;
    if (qn == 0) {
        ui_text("Empty - right-click a clip > Add to queue", (int)r.x + 10, (int)list_y + 5, 12, g_theme.text_dim);
    }

    /* wheel: scrolling a long list */
    if (inside && qn > rows) {
        float wh = GetMouseWheelMove();
        if (wh != 0.0f) g_q_scroll -= (int)(wh > 0 ? 1 : -1);
        if (g_q_scroll > qn - rows) g_q_scroll = qn - rows;
        if (g_q_scroll < 0) g_q_scroll = 0;
    }

    /* dragging: where the grabbed row would go */
    int target = -1;
    if (g_q_drag >= 0 && g_q_drag < qn) {
        float top = m.y - g_q_grab;
        target = g_q_scroll + (int)floorf((top - list_y) / Q_ROW_H + 0.5f);
        if (target < 0) target = 0;
        if (target > qn - 1) target = qn - 1;
    } else {
        g_q_drag = -1;
    }

    BeginScissorMode((int)r.x, (int)list_y, (int)r.width, rows * Q_ROW_H);
    int remove_i = -1;
    for (int slot = 0; slot < rows; ++slot) {
        int pos = g_q_scroll + slot;               /* the place in the list we are drawing */
        if (pos >= qn) break;
        /* which item is at this place if the dragged one is "dropped" at target */
        int i = pos;
        if (g_q_drag >= 0) {
            if (pos == target) continue;           /* the dragged one will be here (drawn separately) */
            i = pos;
            if (g_q_drag < target && pos >= g_q_drag && pos < target) i = pos + 1;
            else if (g_q_drag > target && pos > target && pos <= g_q_drag) i = pos - 1;
        }
        float ry = list_y + slot * Q_ROW_H;
        Rectangle row = { r.x + 4, ry, r.width - 8, Q_ROW_H - 2 };
        int ch, sc;
        if (!session_queue_get(s, i, &ch, &sc) || ch >= s->channel_count) continue;
        bool rh = g_q_drag < 0 && CheckCollisionPointRec(m, row);
        Color rb = (i % 2) ? g_theme.panel_bg2 : g_theme.panel_bg;
        if (rh) { rb.r = (unsigned char)(rb.r + 18); rb.g = (unsigned char)(rb.g + 18); rb.b = (unsigned char)(rb.b + 18); }
        DrawRectangleRec(row_hl(row), rb);
        DrawRectangle((int)row.x, (int)row.y, 3, (int)row.height, channel_color(s->channels[ch].color_id));

        char num[16];
        snprintf(num, sizeof num, "%d", pos + 1);            /* the number = the place (also while dragging) */
        ui_text(num, (int)row.x + 10, (int)row.y + 6, 12, pos == 0 ? g_theme.selection : g_theme.text_dim);
        q_trunc(tmp, sizeof tmp, s->channels[ch].clips[sc].name, (int)row.width - 150, 13);
        ui_text(tmp, (int)row.x + 34, (int)row.y + 5, 13, g_theme.text);
        char chn[NAME_LEN];
        q_trunc(chn, sizeof chn, s->channels[ch].name, 64, 10);
        int cw = ui_text_w(chn, 10);
        ui_text(chn, (int)(row.x + row.width - 52 - cw), (int)row.y + 8, 10, g_theme.text_dim);

        /* × */
        Rectangle xr = { row.x + row.width - 46, row.y + 3, 18, row.height - 6 };
        bool xh = rh && CheckCollisionPointRec(m, xr);
        int xw = ui_text_w("x", 13);
        ui_text("x", (int)(xr.x + (xr.width - xw) / 2), (int)xr.y + 2, 13, xh ? g_theme.vu_red : g_theme.text_dim);
        /* ≡ handle */
        Rectangle hr = { row.x + row.width - 24, row.y, 22, row.height };
        bool hh = rh && CheckCollisionPointRec(m, hr);
        Color hc = hh ? g_theme.text : g_theme.text_dim;
        for (int k = 0; k < 3; ++k)
            DrawRectangle((int)hr.x + 5, (int)(hr.y + 8 + k * 4), 12, 2, hc);
        if (hh) SetMouseCursor(MOUSE_CURSOR_RESIZE_NS);

        if (rh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_q_just_opened) {
            if (hh) { g_q_drag = i; g_q_grab = m.y - ry; }
            else if (xh) remove_i = i;
            else { g_selected_ch = ch; g_selected_scene = sc; }    /* click on the name: selects the clip */
        }
    }
    EndScissorMode();

    /* the dragged row — follows the mouse */
    if (g_q_drag >= 0) {
        int ch, sc;
        SetMouseCursor(MOUSE_CURSOR_RESIZE_NS);
        if (session_queue_get(s, g_q_drag, &ch, &sc) && ch < s->channel_count) {
            float ry = m.y - g_q_grab;
            float lo = list_y, hi = list_y + (rows - 1) * Q_ROW_H;
            if (ry < lo) ry = lo;
            if (ry > hi) ry = hi;
            Rectangle row = { r.x + 4, ry, r.width - 8, Q_ROW_H - 2 };
            DrawRectangle((int)row.x + 2, (int)row.y + 3, (int)row.width, (int)row.height, (Color){ 0, 0, 0, 100 });
            Color sel = g_theme.selection; sel.a = 70;
            DrawRectangleRec(row_hl(row), g_theme.panel_bg2);
            DrawRectangleRec(row_hl(row), sel);
            DrawRectangleLines((int)row.x, (int)row.y, (int)row.width, (int)row.height, g_theme.selection);
            DrawRectangle((int)row.x, (int)row.y, 3, (int)row.height, channel_color(s->channels[ch].color_id));
            char num[16];
            snprintf(num, sizeof num, "%d", target + 1);
            ui_text(num, (int)row.x + 10, (int)row.y + 6, 12, g_theme.selection);
            q_trunc(tmp, sizeof tmp, s->channels[ch].clips[sc].name, (int)row.width - 150, 13);
            ui_text(tmp, (int)row.x + 34, (int)row.y + 5, 13, g_theme.text);
            for (int k = 0; k < 3; ++k)
                DrawRectangle((int)(row.x + row.width - 19), (int)(row.y + 8 + k * 4), 12, 2, g_theme.text);
        }
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            if (target >= 0) session_queue_move(s, g_q_drag, target);
            g_q_drag = -1;
        }
    }
    if (remove_i >= 0) session_queue_remove(s, remove_i);

    /* closing: a click outside the list (and outside the button) or Esc */
    if (!g_q_just_opened && g_q_drag < 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        !inside && !CheckCollisionPointRec(m, g_q_btn))
        g_queue_open = false;
    if (IsKeyPressed(KEY_ESCAPE) && !g_ti_active && !g_ctx_open) g_queue_open = false;
    if (hint) draw_hint(hint, hint_x, r.y + 28);
    g_q_just_opened = false;
}

/* The output list (opened from the button by the fader or in the master header) */
static float g_rt_scroll = 0;     /* scrolling the list (the wheel) when it is taller than the screen */

static void draw_route_popup(Session *s)
{
    if (!g_rt_open) { g_rt_rect = (Rectangle){ 0, 0, 0, 0 }; return; }
    if (g_rt_target >= s->channel_count) { g_rt_open = false; return; }
    if (g_rt_just_opened) g_rt_scroll = 0;
    int nhw = rt_hw_outputs();
    /* Items (in content coordinates). With many outputs the pairs and the mono outputs are in a GRID
     * (4 / 8 per row) so they fit; if the height is still not enough - it scrolls with the wheel. */
    enum { K_MASTER, K_PAIR, K_MONO, K_NONE, K_MATRIX, K_LABEL, K_SEP };
    typedef struct { int kind, k; float x, y, w, h; char txt[40]; } RtItem;
    static RtItem it[8 + MAX_HW_OUT + MAX_HW_OUT / 2 + 8];
    int ni = 0;
    const bool grid = nhw > 4;
    const float w = grid ? 270.0f : 190.0f, row = 22, pad = 6, sep = 7;
    float cy = 26;                                          /* below the title */
    #define RT_ADD(K, KK, X, Y, W, H, ...) do { RtItem *q_ = &it[ni++]; q_->kind = (K); q_->k = (KK); \
        q_->x = (X); q_->y = (Y); q_->w = (W); q_->h = (H); snprintf(q_->txt, sizeof q_->txt, __VA_ARGS__); } while (0)
    if (g_rt_target >= 0) { RT_ADD(K_MASTER, 0, 2, cy, w - 4, row, "Master"); cy += row; }
    if (!grid) {
        for (int k = 0; k + 1 < nhw; k += 2) { RT_ADD(K_PAIR, k, 2, cy, w - 4, row, "Outputs %d-%d", k + 1, k + 2); cy += row; }
        if (nhw > 2) {
            RT_ADD(K_SEP, 0, 0, cy, w, sep, " "); cy += sep;
            for (int k = 0; k < nhw; ++k) { RT_ADD(K_MONO, k, 2, cy, w - 4, row, "Output %d (mono)", k + 1); cy += row; }
        }
    } else {
        const int pc = 4, mc = 8;
        const float pw = (w - 2 * pad) / pc, mw = (w - 2 * pad) / mc;
        RT_ADD(K_LABEL, 0, pad, cy, w, 18, "Stereo pairs (L-R)"); cy += 18;
        int np = nhw / 2;
        for (int i = 0; i < np; ++i)
            RT_ADD(K_PAIR, 2 * i, pad + (i % pc) * pw, cy + (i / pc) * row, pw - 2, row - 2, "%d-%d", 2 * i + 1, 2 * i + 2);
        cy += ((np + pc - 1) / pc) * row + 4;
        RT_ADD(K_LABEL, 0, pad, cy, w, 18, "Single outputs (mono)"); cy += 18;
        for (int k = 0; k < nhw; ++k)
            RT_ADD(K_MONO, k, pad + (k % mc) * mw, cy + (k / mc) * row, mw - 2, row - 2, "%d", k + 1);
        cy += ((nhw + mc - 1) / mc) * row + 2;
    }
    RT_ADD(K_SEP, 0, 0, cy, w, sep, " "); cy += sep;
    RT_ADD(K_NONE, 0, 2, cy, w - 4, row, "None (silent)"); cy += row;
    RT_ADD(K_MATRIX, 0, 2, cy, w - 4, row, "Routing matrix..."); cy += row;
    #undef RT_ADD
    const float content_h = cy + 4;

    int sw = GetScreenWidth(), sh = GetScreenHeight();
    float h = content_h;
    if (h > sh - 8) h = (float)(sh - 8);                   /* taller than the screen -> it scrolls */
    float x = g_rt_btn.x + g_rt_btn.width - w;            /* right-aligned to the button */
    if (x < 4) x = 4;
    if (x + w > sw - 4) x = sw - 4 - w;
    float y = g_rt_btn.y - h - 4;                           /* above the button; if there is no room — below it */
    if (y < 4) y = g_rt_btn.y + g_rt_btn.height + 4;
    if (y + h > sh - 4) y = sh - 4 - h;
    if (y < 4) y = 4;
    Rectangle r = { x, y, w, h };
    g_rt_rect = r;

    Vector2 m = GetMousePosition();
    const float max_scroll = content_h - h > 0 ? content_h - h : 0;
    if (max_scroll > 0 && CheckCollisionPointRec(m, r)) {
        float wh = GetMouseWheelMove();
        if (wh != 0) g_rt_scroll -= wh * 3 * row;
    }
    if (g_rt_scroll > max_scroll) g_rt_scroll = max_scroll;
    if (g_rt_scroll < 0) g_rt_scroll = 0;
    const float oy = y - g_rt_scroll;                       /* start of the content on screen */

    DrawRectangle((int)x + 4, (int)y + 4, (int)w, (int)h, (Color){ 0, 0, 0, 90 });
    Color bg = g_theme.panel_bg; bg.r = (unsigned char)(bg.r + 8); bg.g = (unsigned char)(bg.g + 8); bg.b = (unsigned char)(bg.b + 8);
    DrawRectangleRec(r, bg);
    BeginScissorMode((int)x, (int)y, (int)w, (int)h);
    char title[64];
    if (g_rt_target < 0) snprintf(title, sizeof title, "Master output");
    else snprintf(title, sizeof title, "%.40s output", s->channels[g_rt_target].name);
    ui_text(title, (int)x + 8, (int)(oy + 6), 12, g_theme.text_dim);

    RouteMask l, rr;
    if (g_rt_target < 0) { l = session_master_route_get(s, 0); rr = session_master_route_get(s, 1); }
    else { l = session_route_get(s, g_rt_target, 0); rr = session_route_get(s, g_rt_target, 1); }

    const bool in_view = CheckCollisionPointRec(m, r);
    for (int i = 0; i < ni; ++i) {
        RtItem *q = &it[i];
        float iy = oy + q->y;
        if (iy + q->h < y || iy > y + h) continue;          /* outside the visible area */
        if (q->kind == K_SEP) {
            DrawLine((int)x + 6, (int)(iy + sep / 2), (int)(x + w - 6), (int)(iy + sep / 2), g_theme.grid_line);
            continue;
        }
        if (q->kind == K_LABEL) { ui_text(q->txt, (int)(x + q->x), (int)iy + 3, 11, g_theme.text_dim); continue; }
        Rectangle cr = { x + q->x, iy, q->w, q->h };
        bool hov = in_view && CheckCollisionPointRec(m, cr);
        RouteMask wl = 0, wr = 0;
        int k = q->k;
        if (q->kind == K_MASTER) { wl = ROUTE_MASTER_L; wr = ROUTE_MASTER_R; }
        else if (q->kind == K_PAIR) { wl = g_rt_target < 0 ? MASTER_ROUTE_HW(k) : ROUTE_HW(k); wr = g_rt_target < 0 ? MASTER_ROUTE_HW(k + 1) : ROUTE_HW(k + 1); }
        else if (q->kind == K_MONO) { wl = wr = g_rt_target < 0 ? MASTER_ROUTE_HW(k) : ROUTE_HW(k); }
        bool cur = q->kind != K_MATRIX && l == wl && rr == wr;
        bool cell = grid && (q->kind == K_PAIR || q->kind == K_MONO);
        if (cell) {
            DrawRectangleRec(cr, cur ? g_theme.selection : (hov ? g_theme.cell_hover : g_theme.panel_bg2));
            int tw = ui_text_w(q->txt, 12);
            ui_text(q->txt, (int)(cr.x + (cr.width - tw) / 2), (int)cr.y + 4, 12, cur ? g_theme.bg : g_theme.text);
        } else {
            if (hov) DrawRectangleRec(cr, g_theme.cell_hover);
            if (cur) ui_text(">", (int)cr.x + 6, (int)iy + 4, 13, g_theme.selection);
            ui_text(q->txt, (int)cr.x + 20, (int)iy + 4, 13, cur ? g_theme.selection : g_theme.text);
        }
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_rt_just_opened) {
            if (q->kind == K_MATRIX) {
                ui_open_routing();
            } else if (g_rt_target < 0) {
                session_master_route_set(s, 0, wl);
                session_master_route_set(s, 1, wr);
            } else {
                session_route_set(s, g_rt_target, 0, wl);
                session_route_set(s, g_rt_target, 1, wr);
            }
            g_rt_open = false;
        }
    }
    EndScissorMode();
    DrawRectangleLinesEx(r, 1, g_theme.selection);
    if (max_scroll > 0) {                                   /* a thin indicator of how far it is scrolled */
        float th = h * h / content_h, ty = y + (h - th) * (g_rt_scroll / max_scroll);
        DrawRectangle((int)(x + w - 4), (int)ty, 3, (int)th, g_theme.text_dim);
    }
    if (!g_rt_just_opened && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        !CheckCollisionPointRec(m, r) && !CheckCollisionPointRec(m, g_rt_btn))
        g_rt_open = false;
    if (IsKeyPressed(KEY_ESCAPE)) g_rt_open = false;
    g_rt_just_opened = false;
}

/* ===== Step 36: marking, moving and deleting several things ===== */
static void rotate_items(void *arr, size_t elem, int from, int to)
{
    unsigned char tmp[64], *b = arr;
    if (elem > sizeof tmp || from == to) return;
    memcpy(tmp, b + elem * (size_t)from, elem);
    if (from < to) memmove(b + elem * (size_t)from, b + elem * (size_t)(from + 1), elem * (size_t)(to - from));
    else           memmove(b + elem * (size_t)(to + 1), b + elem * (size_t)to, elem * (size_t)(from - to));
    memcpy(b + elem * (size_t)to, tmp, elem);
}
static int moved_index(int i, int from, int to)
{
    if (i < 0) return i;
    if (i == from) return to;
    if (from < to && i > from && i <= to) return i - 1;
    if (from > to && i >= to && i < from) return i + 1;
    return i;
}

/* channel from goes to the place of to (everything in the interface that points to channels is reordered) */
static bool ui_move_channel(Session *s, int from, int to)
{
    if (!session_move_channel(s, from, to)) return false;
    rotate_items(g_chsel, sizeof g_chsel[0], from, to);
    rotate_items(g_fx_scroll, sizeof g_fx_scroll[0], from, to);
    rotate_items(g_clip_led, sizeof g_clip_led[0], from, to);
    g_chsel_anchor = moved_index(g_chsel_anchor, from, to);
    g_selected_ch = moved_index(g_selected_ch, from, to);
    g_fxp_ch = -1; g_fxm_ch = -1; g_rt_open = false;
    cmark_clear();
    g_anchor_ch = -1;
    return true;
}

/* Ctrl/Shift + click on a channel header */
static void chsel_mod_click(int c, bool ctrl, bool shift)
{
    cmark_clear();
    if (shift && g_chsel_anchor >= 0) {
        if (!ctrl) chsel_clear();
        int a = g_chsel_anchor < c ? g_chsel_anchor : c, b = g_chsel_anchor < c ? c : g_chsel_anchor;
        for (int i = a; i <= b; ++i) chsel_set(i, true);
    } else {
        chsel_set(c, !g_chsel[c]);
        g_chsel_anchor = c;
    }
    g_ch_focus = g_chsel_n > 0;
}

/* Ctrl/Shift + click on a cell */
static void cell_mod_click(Session *s, int c, int sc, bool ctrl, bool shift)
{
    g_ch_focus = false;
    chsel_clear();
    if (g_anchor_ch < 0 && g_selected_ch >= 0 && g_selected_scene >= 0) { g_anchor_ch = g_selected_ch; g_anchor_sc = g_selected_scene; }
    if (shift && g_anchor_ch >= 0 && g_anchor_sc >= 0) {
        if (!ctrl) cmark_clear();
        int c0 = g_anchor_ch < c ? g_anchor_ch : c, c1 = g_anchor_ch < c ? c : g_anchor_ch;
        int s0 = g_anchor_sc < sc ? g_anchor_sc : sc, s1 = g_anchor_sc < sc ? sc : g_anchor_sc;
        if (c1 >= s->channel_count) c1 = s->channel_count - 1;
        if (s1 >= s->scene_count) s1 = s->scene_count - 1;
        for (int i = c0; i <= c1; ++i)
            for (int j = s0; j <= s1; ++j) cmark_set(i, j, true);
    } else {
        /* the first Ctrl + click: the previously selected cell also becomes marked */
        if (g_cmark_n == 0 && g_selected_ch >= 0 && g_selected_scene >= 0 &&
            g_selected_ch < s->channel_count && g_selected_scene < s->scene_count &&
            !(g_selected_ch == c && g_selected_scene == sc))
            cmark_set(g_selected_ch, g_selected_scene, true);
        cmark_set(c, sc, !cmark_get(c, sc));
        g_anchor_ch = c; g_anchor_sc = sc;
    }
    g_selected_ch = c;
    g_selected_scene = sc;
}

/* the marked clips move by (dc, ds); false = one of them would leave the grid */
static int g_mv_dc, g_mv_ds;
static int mv_cmp(const void *a, const void *b)
{
    const int *x = a, *y = b;
    int kx = x[0] * g_mv_dc + x[1] * g_mv_ds, ky = y[0] * g_mv_dc + y[1] * g_mv_ds;
    return ky - kx;                                       /* the farthest one in the direction - first */
}
static bool move_marked(Session *s, int dc, int ds)
{
    static int list[MAX_CHANNELS * MAX_SCENES][2];
    int n = 0;
    for (int c = 0; c < s->channel_count; ++c)
        for (int sc = 0; sc < s->scene_count; ++sc) {
            if (!g_cmark[c][sc]) continue;
            int tc = c + dc, ts = sc + ds;
            if (tc < 0 || tc >= s->channel_count || ts < 0 || ts >= s->scene_count) {
                ui_flash_message("The marked clips do not fit there");
                return false;
            }
            list[n][0] = c; list[n][1] = sc; ++n;
        }
    g_mv_dc = dc; g_mv_ds = ds;
    qsort(list, (size_t)n, sizeof list[0], mv_cmp);
    for (int i = 0; i < n; ++i)
        if (atomic_load(&s->channels[list[i][0]].clips[list[i][1]].has_clip))
            session_move_clip(s, list[i][0], list[i][1], list[i][0] + dc, list[i][1] + ds);
    cmark_clear();
    for (int i = 0; i < n; ++i) cmark_set(list[i][0] + dc, list[i][1] + ds, true);
    if (g_selected_ch >= 0) { g_selected_ch += dc; g_selected_scene += ds; }
    g_anchor_ch = g_selected_ch; g_anchor_sc = g_selected_scene;
    return true;
}

static int delete_marked_clips(Session *s)
{
    int n = 0;
    for (int c = 0; c < s->channel_count; ++c)
        for (int sc = 0; sc < s->scene_count; ++sc)
            if (g_cmark[c][sc] && atomic_load(&s->channels[c].clips[sc].has_clip)) { session_clear_clip(s, c, sc); ++n; }
    cmark_clear();
    return n;
}

/* deletes the selected channels; then the next one is selected (or the previous one, if there is no next) */
static int delete_selected_channels(Session *s)
{
    int lowest = -1, n = 0;
    for (int c = s->channel_count - 1; c >= 0; --c)
        if (c < MAX_CHANNELS && g_chsel[c]) { session_delete_channel(s, c); lowest = c; ++n; }
    chsel_clear();
    cmark_clear();
    g_fxp_ch = -1; g_fxm_ch = -1; g_rt_open = false;
    int cnt = s->channel_count;
    if (g_selected_ch >= cnt) g_selected_ch = cnt - 1;
    g_anchor_ch = -1;
    if (lowest >= 0 && cnt > 0) {
        chsel_only(lowest < cnt ? lowest : cnt - 1);
        g_scroll_to_sel = true;
    } else {
        g_ch_focus = false;
    }
    return n;
}

bool ui_channel_focus(void) { return g_ch_focus && g_chsel_n > 0; }

bool ui_delete_selection(Session *s)
{
    if (!s) return false;
    if (ui_channel_focus()) { delete_selected_channels(s); return true; }
    if (g_cmark_n > 0) { delete_marked_clips(s); return true; }
    return false;
}

bool ui_ctx_delete_channels(Session *s, int ch)
{
    if (!s || ch < 0 || ch >= MAX_CHANNELS || !g_chsel[ch] || g_chsel_n < 2) return false;
    delete_selected_channels(s);
    return true;
}

bool ui_ctx_delete_clips(Session *s, int ch, int sc)
{
    if (!s || !cmark_get(ch, sc) || g_cmark_n < 2) return false;
    delete_marked_clips(s);
    return true;
}

/* several files from the library: one below the other, downwards from the cell (scenes are added if needed) */
static void drop_lib_multi(Session *s, int ch, int sc)
{
    if (!g_fb) return;
    int put = 0, fail = 0;
    for (int i = 0; i < g_fb->entry_count && i < FB_MAX_ENTRIES; ++i) {
        if (!g_libsel[i] || g_fb->entries[i].type != FB_ENTRY_AUDIO) continue;
        while (sc >= s->scene_count) {
            if (session_add_scene(s) < 0) { ui_flash_message("Max scenes reached"); goto done; }
        }
        if (session_set_clip(s, ch, sc, g_fb->entries[i].full_path) == 0) ++put; else ++fail;
        ++sc;
    }
done:
    {
        char msg[96];
        if (fail) snprintf(msg, sizeof msg, "Added %d clips (%d could not be loaded)", put, fail);
        else snprintf(msg, sizeof msg, "Added %d clips", put);
        ui_flash_message(msg);
    }
}

static void ui_draw_backdrop_world(Session *s);
static void ui_draw_world(Session *s)
{
    int screen_w = GetScreenWidth();
    int screen_h = GetScreenHeight();

    if (g_fx_toggle_req) {                 /* step 36b: the FX button - before the layout is computed */
        g_fx_toggle_req = false;
        g_fx_open = !g_fx_open;
        if (g_fx_open && g_footer_hidden) g_footer_hidden = false;   /* the strip is part of the fader panel */
        if (!g_fx_open) { g_fxp_ch = -1; g_fxm_ch = -1; }
        layout_save();
    }

    if (g_log_open && GfxPointIn(GfxMouseTrue(), g_log_rect))
        GfxMouseShield(true);

    /* File/Edit drop-down menu: a click on it must not select/play the cell below */
    Rectangle ddz = { g_menu_dd_rect.x / GfxGetZoom(), g_menu_dd_rect.y / GfxGetZoom(),      /* (the menu: normal size) */
                      g_menu_dd_rect.width / GfxGetZoom(), g_menu_dd_rect.height / GfxGetZoom() };
    if ((g_file_menu_open || g_edit_menu_open || g_play_menu_open) && GfxPointIn(GfxMouseTrue(), ddz))
        GfxMouseShield(true);

    /* While the queue is open, the elements below it do not see the mouse */
    if (g_rt_open && GfxPointIn(GfxMouseTrue(), g_rt_rect))
        GfxMouseShield(true);
    if (g_em_kind != EM_NONE && GfxPointIn(GfxMouseTrue(), g_em_rect))
        GfxMouseShield(true);
    if ((g_fxp_ch >= 0 && GfxPointIn(GfxMouseTrue(), g_fxp_rect)) ||
        (g_fxm_ch >= 0 && GfxPointIn(GfxMouseTrue(), g_fxm_rect)))
        GfxMouseShield(true);
    if (g_queue_open && (GfxPointIn(GfxMouseTrue(), g_q_rect) || g_q_drag >= 0 || g_vf_id == 3))
        GfxMouseShield(true);

    /* While the DSP popup is open, the elements below it do not see the mouse when it is clicked/hovered over it */
    if (g_dsp_open) {
        Vector2 mm = GfxMouseTrue();   /* 1.3.12: the real mouse (Eco) */
        float z = GfxGetZoom();
        Rectangle pr = dsp_popup_rect((int)lroundf(screen_w * z));          /* (in the top bar's normal size) */
        pr = (Rectangle){ pr.x / z, pr.y / z, pr.width / z, pr.height / z };
        if (GfxPointIn(mm, pr) ||
            IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
            GfxMouseShield(true);
    }

    carry_input(s);                        /* 1.4.17: "Split to clips" - a click drops the carried parts */
    {   /* 1.4.17: leaving the MIDI Learn mode: a part's level no longer waits for a CC */
        static bool was_learn = false;
        if (was_learn && !g_midi_learn_mode) session_stem_learn_arm(-1, -1, -1);
        was_learn = g_midi_learn_mode;
    }

    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        g_dragging_slider = -1;

        if (g_drag_active) {
            if (g_hovered_ch >= 0 && g_hovered_scene >= 0) {
                if (g_drag_multi) drop_lib_multi(s, g_hovered_ch, g_hovered_scene);   /* step 36 */
                else session_set_clip(s, g_hovered_ch, g_hovered_scene, g_drag_path);
            }
            g_drag_multi = false;
            g_drag_active = false;
            g_drag_path[0] = '\0';
            g_drag_name[0] = '\0';
        }

        if (g_drag_clip_active) {
            Vector2 mr = GetMousePosition();
            float mdx = mr.x - g_drag_clip_p0.x, mdy = mr.y - g_drag_clip_p0.y;
            bool really_moved = mdx * mdx + mdy * mdy > 6.0f * 6.0f;   /* a click in place never moves a clip */
            if (really_moved && g_hovered_ch >= 0 && g_hovered_scene >= 0 &&
                (g_hovered_ch != g_drag_clip_src_ch ||
                 g_hovered_scene != g_drag_clip_src_scene)) {
                if (g_cdrag_from_mark && cmark_get(g_drag_clip_src_ch, g_drag_clip_src_scene))   /* step 36: all marked ones */
                    move_marked(s, g_hovered_ch - g_drag_clip_src_ch, g_hovered_scene - g_drag_clip_src_scene);
                else
                    session_move_clip(s,
                                      g_drag_clip_src_ch, g_drag_clip_src_scene,
                                      g_hovered_ch, g_hovered_scene);
            } else if (!really_moved && g_cdrag_from_mark) {
                cmark_clear();                                  /* plain click on a marked one -> only it stays */
            }
            g_cdrag_from_mark = false;
            g_drag_clip_active = false;
            g_drag_clip_src_ch = -1;
            g_drag_clip_src_scene = -1;
        }

        if (g_waveform_dragging) g_waveform_dragging = false;
        if (g_dragging_loop_marker != 0) g_dragging_loop_marker = 0;
    }

    float lib_w = 0;
    if (g_fb && g_fb->visible) lib_w = LIBRARY_W;

    float menu_offset = MENU_BAR_H;
    float grid_top = menu_offset + TOP_BAR_H + GAP;
    float grid_bottom = screen_h - BOTTOM_BAR_H - GAP;

    float wave_h = 0;
    if (g_waveform_h >= WAVEFORM_MIN_H) {
        wave_h = g_waveform_h;
        grid_bottom -= wave_h + RESIZE_BAR_H + GAP;
        if (grid_bottom < grid_top + 100) {
            wave_h = 0;
            grid_bottom = screen_h - BOTTOM_BAR_H - GAP;
        }
    }

    {   /* the master must not eat the grid */
        float mx_max = (float)screen_w * 0.45f;
        if (mx_max > MASTER_W_MAX) mx_max = MASTER_W_MAX;
        if (mx_max < MASTER_W) mx_max = MASTER_W;
        if (g_master_w > mx_max) g_master_w = mx_max;
        if (g_master_w < MASTER_W) g_master_w = MASTER_W;
    }
    float master_x = screen_w - (g_master_hidden ? 14.0f : g_master_w) - GAP;   /* 1.3.10: hidden = a thin handle */
    float scene_col_x = GAP + lib_w;
    float channels_x = scene_col_x + SCENE_COL_W + GAP;
    float channels_w = master_x - channels_x - GAP;
    if (channels_w < CHANNEL_MIN_W) channels_w = CHANNEL_MIN_W;

    int nc = s->channel_count;
    if (nc < 1) nc = 1;

    /* step 36: dragged header -> the channel moves right away while dragging (by the widths from the previous frame) */
    if (g_chdrag >= 0) {
        Vector2 m = GetMousePosition();
        if (g_chdrag >= s->channel_count || !IsMouseButtonDown(MOUSE_BUTTON_LEFT) || g_ctx_open || g_ti_active) {
            g_chdrag = -1;
        } else {
            if (!g_chdrag_moved && fabsf(m.x - g_chdrag_p0.x) > 6.0f) g_chdrag_moved = true;
            if (g_chdrag_moved) {
                SetMouseCursor(MOUSE_CURSOR_RESIZE_EW);
                if (m.x < channels_x + 24) g_scroll_x -= 12.0f;          /* at the edge -> it scrolls */
                else if (m.x > master_x - 24) g_scroll_x += 12.0f;
                if (g_scroll_x < 0) g_scroll_x = 0;
                float mx = m.x - channels_x + g_scroll_x;
                int t = 0;                                               /* how many other channels are to the left of the mouse */
                for (int j = 0; j < s->channel_count; ++j)
                    if (j != g_chdrag && g_chx[j] + g_chw[j] * 0.5f < mx) ++t;
                if (t != g_chdrag && ui_move_channel(s, g_chdrag, t)) g_chdrag = t;
            }
        }
    }

    /* Column widths: each has its own (the right edge of the header is dragged), the free space is NOT distributed automatically */
    g_chx[0] = 0.0f;
    for (int c = 0; c < s->channel_count; ++c) {
        g_chw[c] = (float)session_get_channel_width(s, c);
        g_chx[c + 1] = g_chx[c] + g_chw[c];
    }
    float total_channels_w = g_chx[s->channel_count];
    (void)nc;

    /* Horizontally: if the columns + the "+" button do not fit, there is a scroll. At the end there is a margin of one channel,
     * so the "+" is visible and can be pressed. */
    bool need_h_scroll = (total_channels_w + 4 + ADD_CH_BTN_W + 4 + VSCROLLBAR_W + 4 > channels_w);
    float h_scroll_h = need_h_scroll ? SCROLLBAR_H + 4 : 0;
    float content_w = total_channels_w + (need_h_scroll ? CH_WIDTH_DEFAULT : 0);

    /* height of the fader panel (0 = hidden); it cannot eat the whole grid */
    float footer_max = grid_bottom - grid_top - CLIP_HEADER_H - 90.0f;
    if (footer_max < FOOTER_MIN) footer_max = FOOTER_MIN;
    if (g_footer_h > footer_max) g_footer_h = footer_max;
    if (g_footer_h < FOOTER_MIN) g_footer_h = FOOTER_MIN;
    float fh = g_footer_hidden ? 0.0f : g_footer_h;
    if (!g_footer_hidden && g_fx_open) fh += FX_STRIP_H;         /* the FX strip is above the faders */

    float clip_area_h = grid_bottom - grid_top - CLIP_HEADER_H - CLIP_TOP_GAP
                        - h_scroll_h - fh;
    if (clip_area_h < 30) clip_area_h = 30;

    /* Vertically: the rows + the "+" button for a new scene; if they do not fit - a scroll with a margin of one scene. */
    int nsc = s->scene_count;
    if (nsc < 1) nsc = 1;
    /* The scene height is ALWAYS the same (the narrowest one): otherwise when hiding/showing the waveform or
     * the faders the rows move under the mouse and the second click of a double click becomes a "drag" of the clip. */
    float row_h = CLIP_MIN_H;
    bool need_v_scroll = (nsc * row_h + ADD_SCENE_BTN_H > clip_area_h + 0.5f);
    float v_scroll_w = need_v_scroll ? VSCROLLBAR_W + 4 : 0;
    float content_h = nsc * row_h + ADD_SCENE_BTN_H + (need_v_scroll ? row_h : 0);

    float avail_ch_w = channels_w - v_scroll_w;
    if (avail_ch_w < CHANNEL_MIN_W) avail_ch_w = CHANNEL_MIN_W;

    L.grid_x = channels_x;
    L.grid_y = grid_top + CLIP_HEADER_H + CLIP_TOP_GAP;
    L.grid_w = channels_w;
    L.grid_h = clip_area_h;
    L.scene_col_x = scene_col_x;
    L.master_x = master_x;
    L.channel_w = 1;
    L.hdr_y = grid_top;
    L.row_h = row_h;
    L.footer_y = grid_bottom - fh;
    L.footer_h = fh;

    float max_scroll_x = content_w - avail_ch_w;
    if (max_scroll_x < 0) max_scroll_x = 0;
    float max_scroll_y = content_h - clip_area_h;
    if (max_scroll_y < 0) max_scroll_y = 0;

    /* Scrollbars: geometry (shared by input and drawing) and dragging - BEFORE the grid is drawn */
    const float sb_clip_y0   = grid_top + CLIP_HEADER_H + CLIP_TOP_GAP;
    const float sb_footer_y  = grid_bottom - fh;
    const float hsb_x = channels_x, hsb_y = sb_footer_y - SCROLLBAR_H - 2, hsb_w = avail_ch_w;
    /* the vertical scrollbar starts at the top edge of the headers (not below them) — the headers hide behind it */
    /* ...and reaches the bottom (past the faders) — they also hide behind it */
    const float vsb_x = master_x - VSCROLLBAR_W - 2, vsb_y = sb_clip_y0 - CLIP_HEADER_H - CLIP_TOP_GAP, vsb_h = grid_bottom - vsb_y;
    g_sb_rect_h = (Rectangle){ hsb_x, hsb_y, hsb_w, SCROLLBAR_H }; g_sb_have_h = need_h_scroll;
    g_sb_rect_v = (Rectangle){ vsb_x, vsb_y, VSCROLLBAR_W, vsb_h }; g_sb_have_v = need_v_scroll;
    {
        bool sb_ok = !g_ti_active && !g_ctx_open && !g_file_menu_open && !g_audio_dlg_open && !g_appearance_open &&
                     !g_midi_settings_open && !dialogs_any_open() && !g_drag_active && g_chres_ch < 0;
        if (need_h_scroll) scrollbar_input(0, hsb_x, hsb_y, hsb_w, SCROLLBAR_H, content_w, avail_ch_w, &g_scroll_x, sb_ok);
        if (need_v_scroll) scrollbar_input(1, vsb_x, vsb_y, VSCROLLBAR_W, vsb_h, content_h, clip_area_h, &g_scroll_y, sb_ok);
        if (!need_h_scroll && g_sb_drag == 1) g_sb_drag = 0;
        if (!need_v_scroll && g_sb_drag == 2) g_sb_drag = 0;
    }

    Vector2 mouse = GetMousePosition();
    bool in_library = (g_fb && g_fb->visible && mouse.x < GAP + lib_w);

    bool mouse_in_wave = (wave_h >= WAVEFORM_MIN_H) &&
                         mouse.y >= screen_h - BOTTOM_BAR_H - GAP - wave_h - RESIZE_BAR_H;

    if (!in_library && !g_ti_active && !g_ctx_open && !g_file_menu_open
        && !g_audio_dlg_open && !mouse_in_wave) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f && (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))) {
            ui_zoom_step(wheel > 0 ? 1 : -1);        /* 1.3.10: Ctrl+wheel = the size of the whole interface */
        } else if (wheel != 0.0f) {
            if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
                g_scroll_x -= wheel * 60;
            } else {
                g_scroll_y -= wheel * 60;
            }
        }
    }

    /* what is selected with the keyboard is always visible (step 32) */
    if (g_scroll_to_sel) {
        g_scroll_to_sel = false;
        if (g_selected_scene >= 0 && g_selected_scene < s->scene_count) {
            float y0 = (float)g_selected_scene * row_h, y1 = y0 + row_h;
            if (y0 < g_scroll_y) g_scroll_y = y0;
            else if (y1 > g_scroll_y + clip_area_h) g_scroll_y = y1 - clip_area_h;
        }
        int sx_ch = (g_ch_focus && g_chsel_anchor >= 0) ? g_chsel_anchor : g_selected_ch;   /* step 36 */
        if (sx_ch >= 0 && sx_ch < s->channel_count) {
            float x0 = g_chx[sx_ch], x1 = x0 + g_chw[sx_ch];
            if (x0 < g_scroll_x) g_scroll_x = x0;
            else if (x1 > g_scroll_x + avail_ch_w) g_scroll_x = x1 - avail_ch_w;
        }
    }

    /* Clamp AFTER the wheel (otherwise the frame is drawn with an out-of-range value and the interface jitters at the end) */
    if (g_scroll_x > max_scroll_x) g_scroll_x = max_scroll_x;
    if (g_scroll_x < 0) g_scroll_x = 0;
    if (g_scroll_y > max_scroll_y) g_scroll_y = max_scroll_y;
    if (g_scroll_y < 0) g_scroll_y = 0;

    g_hovered_ch = -1;
    g_hovered_scene = -1;
    g_carry_hc = g_carry_hs = -1;

    /* the visible scenes (the rows are exactly row_h apart; here it used to divide by row_h+2 and with a long scroll
     * the last rows above the faders were not drawn -> an empty black area) */
    int first_vis_sc = (int)(g_scroll_y / row_h) - 1;
    if (first_vis_sc < 0) first_vis_sc = 0;
    int last_vis_sc = first_vis_sc + (int)(clip_area_h / row_h) + 3;
    if (last_vis_sc >= s->scene_count) last_vis_sc = s->scene_count - 1;

    int first_vis_ch = 0;
    while (first_vis_ch < s->channel_count - 1 && g_chx[first_vis_ch + 1] <= g_scroll_x) first_vis_ch++;
    int last_vis_ch = first_vis_ch;
    while (last_vis_ch < s->channel_count - 1 && g_chx[last_vis_ch + 1] < g_scroll_x + channels_w) last_vis_ch++;
    if (s->channel_count == 0) { first_vis_ch = 0; last_vis_ch = -1; }

    /* step 36: Ctrl/Shift + click (left or right) marks clips and channels. The click is "swallowed" (it does not play a clip,
     * does not open a menu). A plain click in the grid clears the marked ones (unless it is on a marked one - then it drags them). */
    {
        static int last_nch = -1, last_nsc = -1;
        if (last_nch != s->channel_count || last_nsc != s->scene_count) {   /* the channels/scenes were changed from outside */
            if (last_nch >= 0) {
                cmark_clear();
                memset(g_clip_led, 0, sizeof g_clip_led);
                for (int c = s->channel_count; c < MAX_CHANNELS; ++c) chsel_set(c, false);
                if (g_chsel_n == 0) g_ch_focus = false;
                if (g_chsel_anchor >= s->channel_count) g_chsel_anchor = -1;
            }
            last_nch = s->channel_count; last_nsc = s->scene_count;
        }
        bool popups = g_ti_active || g_ctx_open || g_file_menu_open || g_edit_menu_open || g_play_menu_open || g_audio_dlg_open ||
                      g_appearance_open || g_midi_settings_open || dialogs_any_open();
        if (IsKeyPressed(KEY_ESCAPE) && !popups) { cmark_clear(); libsel_clear(); chsel_clear(); g_ch_focus = false; }
        bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        bool lp = IsMouseButtonPressed(MOUSE_BUTTON_LEFT), rp = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
        if ((lp || rp) && !popups && !g_midi_learn_mode && g_sb_drag == 0 && !g_drag_active && g_chres_ch < 0) {
            float hr = need_v_scroll ? channels_x + avail_ch_w : master_x;
            int hc = -1;
            if (mouse.x >= channels_x && mouse.x < hr) {
                float mx = mouse.x - channels_x + g_scroll_x;
                for (int c = 0; c < s->channel_count; ++c)
                    if (mx >= g_chx[c] && mx < g_chx[c + 1] - GAP) { hc = c; break; }
            }
            float cy0 = grid_top + CLIP_HEADER_H + CLIP_TOP_GAP;
            bool in_hdr = hc >= 0 && mouse.y >= grid_top && mouse.y < cy0 &&
                          mouse.x < channels_x + g_chx[hc] + g_chw[hc] - GAP - 6 - g_scroll_x;   /* not on the width handle */
            bool in_grid = hc >= 0 && mouse.y >= cy0 && mouse.y < cy0 + clip_area_h && mouse.x < channels_x + avail_ch_w;
            int hs = in_grid ? (int)floorf((mouse.y - cy0 + g_scroll_y) / row_h) : -1;
            if (hs < 0 || hs >= s->scene_count) in_grid = false;
            if ((ctrl || shift) && in_hdr) {
                chsel_mod_click(hc, ctrl, shift);
                GfxMouseShield(true);
            } else if ((ctrl || shift) && in_grid) {
                cell_mod_click(s, hc, hs, ctrl, shift);
                GfxMouseShield(true);
            } else if (lp && in_grid && !ctrl && !shift) {
                g_ch_focus = false;
                chsel_clear();
                if (!cmark_get(hc, hs)) cmark_clear();
                g_cdrag_from_mark = cmark_get(hc, hs) && g_cmark_n > 1;
                g_anchor_ch = hc; g_anchor_sc = hs;
            }
        }
    }

    float footer_y = L.footer_y;
    /* the buttons on the left (Stop, Loop, MIDI, SCROLL, QUEUE) always have room: with a hidden panel they sit at the very bottom */
    float side_y = g_footer_hidden ? grid_bottom - CHANNEL_FOOTER_H : footer_y + (g_fx_open ? FX_STRIP_H : 0.0f);   /* the FX strip does not move the buttons */

    GfxUnzoomBegin();                        /* 1.3.10: the menu bar keeps its size */
    draw_menu_bar(s, GetScreenWidth());
    GfxUnzoomEnd();
    GfxUnzoomBegin();                        /* 1.3.12: the top bar (DSP, logo, BPM) keeps its size too */
    draw_top_bar(GetScreenWidth());
    GfxUnzoomEnd();

    draw_stop_button(s, scene_col_x, side_y, SCENE_COL_W, CLIP_HEADER_H);

    {
        float loop_y = side_y + CLIP_HEADER_H + 2;
        if (draw_loop_button(s, scene_col_x, loop_y,
                             SCENE_COL_W, LOOP_BTN_H)) {
            int sel_ch = g_selected_ch;
            int sel_sc = g_selected_scene;
            if (sel_ch >= 0 && sel_sc >= 0) {
                session_toggle_loop(s, sel_ch, sel_sc);
            }
        }
    }

    {
        float midi_y = side_y + CLIP_HEADER_H + 2 + LOOP_BTN_H + 4;
        if (draw_midi_button(scene_col_x, midi_y,
                             SCENE_COL_W, MIDI_BTN_H)) {
            ui_toggle_midi_learn();
        }
    }

    /* two narrow buttons under MIDI: 1) a waveform that follows the position (Mixxx); 2) the queue */
    {
        float b1_y = side_y + CLIP_HEADER_H + 2 + LOOP_BTN_H + 4 + MIDI_BTN_H + 4;
        float b2_y = b1_y + SMALL_BTN_H + 4;
        Rectangle r1 = { scene_col_x, b1_y, SCENE_COL_W, SMALL_BTN_H };
        bool hov1 = CheckCollisionPointRec(mouse, r1);
        Color bg1 = g_wave_follow ? g_theme.selection : g_theme.panel_bg2;
        if (hov1 && !g_wave_follow) {
            bg1.r = (unsigned char)(bg1.r + 15); bg1.g = (unsigned char)(bg1.g + 15); bg1.b = (unsigned char)(bg1.b + 15);
        }
        DrawRectangleRounded(r1, R_CELL, bg1);
        DrawRectangleRoundedLines(r1, R_CELL, 1, g_theme.text_dim);
        const char *l1 = "SCROLL";
        int t1 = ui_text_w(l1, 10);
        ui_text(l1, (int)(r1.x + (r1.width - t1) / 2), (int)(r1.y + (SMALL_BTN_H - 10) / 2 + 1), 10,
                g_wave_follow ? g_theme.bg : g_theme.text);
        if (hov1 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_ctx_open && !g_ti_active && !g_audio_dlg_open) {
            g_wave_follow = !g_wave_follow;
            if (g_wave_follow) g_follow_sec = FOLLOW_SEC_MAX;     /* always starts at the farthest zoom */
        }

        /* second button: the queue (the list opens above it) */
        Rectangle r2 = { scene_col_x, b2_y, SCENE_COL_W, SMALL_BTN_H };
        g_q_btn = r2;
        int qn = session_queue_len(s);
        bool hov2 = CheckCollisionPointRec(mouse, r2);
        Color bg2 = g_queue_open ? g_theme.selection : g_theme.panel_bg2;
        if (hov2 && !g_queue_open) {
            bg2.r = (unsigned char)(bg2.r + 15); bg2.g = (unsigned char)(bg2.g + 15); bg2.b = (unsigned char)(bg2.b + 15);
        }
        DrawRectangleRounded(r2, R_CELL, bg2);
        DrawRectangleRoundedLines(r2, R_CELL, 1, (qn > 0 && !g_queue_open) ? g_theme.selection : g_theme.text_dim);
        char l2[24];
        if (qn > 0) snprintf(l2, sizeof l2, "QUEUE %d", qn); else snprintf(l2, sizeof l2, "QUEUE");
        int t2 = ui_text_w(l2, 10);
        ui_text(l2, (int)(r2.x + (r2.width - t2) / 2), (int)(r2.y + (SMALL_BTN_H - 10) / 2 + 1), 10,
                g_queue_open ? g_theme.bg : (qn > 0 ? g_theme.selection : g_theme.text));
        if (hov2 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_ctx_open && !g_ti_active && !g_audio_dlg_open) {
            g_queue_open = !g_queue_open;
            g_q_just_opened = g_queue_open;
            g_q_drag = -1;
        }
        g_q_anchor_x = scene_col_x + SCENE_COL_W + 8;
        g_q_anchor_bottom = b2_y + SMALL_BTN_H;
        g_q_top_limit = grid_top;

        /* FX (step 33): shows/hides the plugin strip above the faders */
        float b3_y = b2_y + SMALL_BTN_H + 4;
        Rectangle r3 = { scene_col_x, b3_y, SCENE_COL_W, MIDI_BTN_H };
        bool hov3 = CheckCollisionPointRec(mouse, r3);
        Color bg3 = g_fx_open ? g_theme.selection : g_theme.panel_bg2;
        if (hov3 && !g_fx_open) { bg3.r = (unsigned char)(bg3.r + 15); bg3.g = (unsigned char)(bg3.g + 15); bg3.b = (unsigned char)(bg3.b + 15); }
        DrawRectangleRounded(r3, R_CELL, bg3);
        DrawRectangleRoundedLines(r3, R_CELL, 1, g_theme.text_dim);
        int t3 = ui_text_w("FX", 13);
        ui_text("FX", (int)(r3.x + (r3.width - t3) / 2), (int)(r3.y + (MIDI_BTN_H - 13) / 2), 13, g_fx_open ? g_theme.bg : g_theme.text);
        if (hov3 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_ctx_open && !g_ti_active && !g_audio_dlg_open) {
            g_fx_toggle_req = true;   /* step 36b: changed at the start of the next frame (otherwise 1 frame is misplaced) */
        }
    }

    draw_scene_header(scene_col_x, grid_top, SCENE_COL_W, CLIP_HEADER_H);

    /* The headers and the bottom panels are clipped at the left boundary of the channels: with horizontal scrolling
     * they hide BEHIND the static column (Scenes, Stop, Loop, MIDI), and do not cover it. */
    /* ...and on the right at the scrollbar (if there is one), so headers do not stick out next to it */
    float hdr_right = need_v_scroll ? channels_x + avail_ch_w : master_x;
    BeginScissorMode((int)channels_x, 0, (int)(hdr_right - channels_x), screen_h);
    for (int c = first_vis_ch; c <= last_vis_ch; ++c) {
        float cx = channels_x + g_chx[c] - g_scroll_x;
        float cwid = g_chw[c];
        if (cx + cwid < channels_x) continue;
        if (cx > master_x) continue;

        draw_channel_header(s, c, cx, grid_top,
                            cwid - GAP, CLIP_HEADER_H);

        Rectangle hdr_rect = { cx, grid_top, cwid - GAP, CLIP_HEADER_H };
        bool free_ui = !g_ti_active && !g_ctx_open && !g_file_menu_open && !g_audio_dlg_open && mouse.x >= channels_x &&
                       mouse.x < hdr_right;
        /* width handle: the right edge of the header (and a bit of the gap) */
        Rectangle grip = { cx + cwid - GAP - 6, grid_top, 6 + GAP, CLIP_HEADER_H };
        bool over_grip = free_ui && g_chres_ch < 0 && CheckCollisionPointRec(mouse, grip) && mouse.x < hdr_right;
        float btns = (cwid - GAP > 60 ? 24.0f : 0.0f) + (fxchain_has_instrument(s, c) && cwid - GAP > 82 ? 26.0f : 0.0f);   /* 1.4 */
        bool on_btns = mouse.x > hdr_rect.x + hdr_rect.width - btns;
        if (free_ui && !on_btns &&
            CheckCollisionPointRec(mouse, hdr_rect) &&
            IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && !g_midi_learn_mode) {
            if (!g_chsel[c]) chsel_only(c);                  /* step 36: right click selects the channel (several selected ones stay) */
            else g_ch_focus = true;
            cmark_clear();
            ctx_menu_open(CTX_CHANNEL, c, -1,
                          (int)mouse.x, (int)mouse.y);
        }
        /* step 36: click on the header = selects the channel; drag = move */
        if (free_ui && !over_grip && !on_btns && g_chres_ch < 0 && CheckCollisionPointRec(mouse, hdr_rect) &&
            IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_midi_learn_mode && !g_drag_active) {
            chsel_only(c);
            cmark_clear();
            g_chdrag = c;
            g_chdrag_p0 = mouse;
            g_chdrag_moved = false;
        }
        if (over_grip || g_chres_ch == c) SetMouseCursor(MOUSE_CURSOR_RESIZE_EW);
        if (over_grip && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            double now = GetTime();
            if (g_chres_last_ch == c && now - g_chres_last_t < DOUBLE_CLICK_TIME) {
                session_set_channel_width(s, c, 0);          /* double click = default width */
                g_chres_last_ch = -1;
            } else {
                g_chres_ch = c;
                g_chres_x0 = mouse.x;
                g_chres_w0 = (float)session_get_channel_width(s, c);
                g_chres_last_ch = -1;
            }
        }
        if (g_chres_ch == c) {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                int nw = (int)(g_chres_w0 + (mouse.x - g_chres_x0) + 0.5f);
                session_set_channel_width(s, c, nw);
            } else {
                if (fabsf(mouse.x - g_chres_x0) < 3.0f) {      /* simple click (no drag) -> may become a double click */
                    g_chres_last_ch = c;
                    g_chres_last_t = GetTime();
                }
                g_chres_ch = -1;
            }
        }
    }

    EndScissorMode();

    if (g_chres_ch >= 0 && !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) g_chres_ch = -1;

    {
        float plus_x = channels_x + total_channels_w - g_scroll_x;
        if (plus_x + 4 + ADD_CH_BTN_W <= channels_x + avail_ch_w && plus_x + 4 + ADD_CH_BTN_W > channels_x) {
            Rectangle plus_r = { plus_x + 4, grid_top + (CLIP_HEADER_H - 24) / 2,
                                 ADD_CH_BTN_W, 24 };
            Vector2 m2 = GetMousePosition();
            bool hov = CheckCollisionPointRec(m2, plus_r);
            Color bg = hov ? g_theme.btn_add_hover : g_theme.btn_add;
            DrawRectangleRec(plus_r, bg);
            DrawRectangleLinesEx(plus_r, 1, g_theme.text_dim);

            const char *plus = "+";
            int tw = ui_text_w(plus, 18);
            ui_text(plus,
                    (int)(plus_r.x + (ADD_CH_BTN_W - tw) / 2),
                    (int)(plus_r.y + 2),
                    18, g_theme.text_dim);

            if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)
                && !g_ti_active && !g_ctx_open && !g_audio_dlg_open) {
                if (session_add_channel(s, NULL) < 0) {
                    ui_flash_message("Max channels reached");
                }
            }
        }
    }

    float clip_y0 = grid_top + CLIP_HEADER_H + CLIP_TOP_GAP;   /* step 36g: a little space below the headers */
    float clip_h_avail = footer_y - clip_y0;
    if (clip_h_avail < 10) clip_h_avail = 10;

    /* Visible (and clickable) area of the cells: it does not go under the scrollbars and under the bottom panels */
    g_scene_hl = g_scene_hl_next;          /* from the previous frame (the scenes are drawn after the clips) */
    g_scene_hl_next = -1;
    Rectangle grid_vis = { channels_x, clip_y0, avail_ch_w, clip_area_h };
    bool mouse_in_grid = CheckCollisionPointRec(mouse, grid_vis) && g_sb_drag == 0;
    BeginScissorMode((int)grid_vis.x, (int)grid_vis.y, (int)grid_vis.width, (int)grid_vis.height);

    for (int c = first_vis_ch; c <= last_vis_ch; ++c) {
        float cx = channels_x + g_chx[c] - g_scroll_x;
        if (cx + g_chw[c] < channels_x) continue;
        if (cx > master_x) continue;

        for (int i = first_vis_sc; i <= last_vis_sc; ++i) {
            float cy = clip_y0 + i * row_h - g_scroll_y;
            if (cy + row_h < clip_y0) continue;
            if (cy > clip_y0 + clip_h_avail) continue;

            float cw = g_chw[c] - GAP;

            Rectangle r = { cx, cy, cw, row_h - 2 };
            bool hov = mouse_in_grid && CheckCollisionPointRec(mouse, r);
            if (hov && !g_ti_active && !g_ctx_open && !g_file_menu_open
                && !g_audio_dlg_open) {
                g_hovered_ch = c;
                g_hovered_scene = i;
            }
            if (g_carry_n > 0 && GfxPointIn(GfxMouseTrue(), grid_vis) && GfxPointIn(GfxMouseTrue(), r)) { g_carry_hc = c; g_carry_hs = i; }

            {   /* the mouse is outside the visible area (over a scrollbar/panel) -> the cell does not react */
                bool saved_ctx = g_ctx_open;
                if (!mouse_in_grid) g_ctx_open = true;
                draw_clip_cell(s, c, i, cx, cy, cw, row_h - 2);
                g_ctx_open = saved_ctx;
            }
            carry_mark_cell(s, c, i, r);                   /* 1.4.17 */
            if (i == g_scene_hl && !(g_selected_ch == c && g_selected_scene == i)) {   /* a very light outline */
                Color hl = g_theme.selection; hl.a = 70;
                DrawRectangleLinesEx(r, 1, hl);
            }

            if (!g_ti_active && !g_ctx_open && !g_file_menu_open
                && !g_audio_dlg_open && hov && !g_midi_learn_mode &&
                IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                Clip *cl = &s->channels[c].clips[i];
                bool has = atomic_load(&cl->has_clip);
                if (has) {
                    ctx_menu_open(CTX_CLIP, c, i,
                                  (int)mouse.x, (int)mouse.y);
                } else {
                    ctx_menu_open(CTX_EMPTY, c, i,
                                  (int)mouse.x, (int)mouse.y);
                }
            }
        }
    }
    /* 1.4.8: the pad box (the clips the pads of the controller play), from the selected cell */
    if (midimap_pads_active() && g_selected_ch >= 0 && g_selected_scene >= 0 && s->channel_count > 0 && s->scene_count > 0) {
        int b0, r0, bw, bh;
        ui_pad_box(s, &b0, &r0, &bw, &bh);
        int b1 = b0 + bw - 1, r1 = r0 + bh - 1;
        if (b1 >= s->channel_count) b1 = s->channel_count - 1;
        if (r1 >= s->scene_count) r1 = s->scene_count - 1;
        float bx0 = channels_x + g_chx[b0] - g_scroll_x - 3, bx1 = channels_x + g_chx[b1] + g_chw[b1] - GAP - g_scroll_x + 3;
        float by0 = clip_y0 + r0 * row_h - g_scroll_y - 3, by1 = clip_y0 + (r1 + 1) * row_h - 2 - g_scroll_y + 3;
        Color bc = { 255, 96, 64, 255 };
        DrawRectangleRoundedLines((Rectangle){ bx0, by0, bx1 - bx0, by1 - by0 }, 6.0f, 2.5f, bc);
    }
    EndScissorMode();

    float scene_h_avail = (side_y - 2) - clip_y0;          /* the Scenes column ends above the buttons on the left */
    if (scene_h_avail < 10) scene_h_avail = 10;
    BeginScissorMode((int)scene_col_x, (int)clip_y0,
                     SCENE_COL_W, (int)scene_h_avail);
    for (int i = first_vis_sc; i <= last_vis_sc; ++i) {
        float ry = clip_y0 + i * row_h - g_scroll_y;
        if (ry + row_h < clip_y0) continue;
        if (ry > clip_y0 + clip_h_avail) continue;

        draw_scene_button(s, i, scene_col_x, ry, SCENE_COL_W, row_h - 2);   /* step 36j: a gap like between the clips */

        Rectangle r = { scene_col_x, ry, SCENE_COL_W, row_h };
        if (CheckCollisionPointRec(mouse, r) && ry + row_h > clip_y0 && !g_ctx_open && !g_ti_active)
            g_scene_hl_next = i;

        bool scene_vis = mouse.y < clip_y0 + scene_h_avail;     /* not under the buttons on the left */
        if (!g_ti_active && !g_ctx_open && !g_file_menu_open &&
            !g_audio_dlg_open && scene_vis &&
            CheckCollisionPointRec(mouse, r) &&
            IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_drag_active) {
            session_play_scene(s, i);
            g_selected_ch = -1;
            g_selected_scene = i;
            cmark_clear(); chsel_clear(); g_ch_focus = false;
        }
        if (!g_ti_active && !g_ctx_open && !g_file_menu_open &&
            !g_audio_dlg_open && scene_vis &&
            CheckCollisionPointRec(mouse, r) &&
            IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && !g_midi_learn_mode) {
            ctx_menu_open(CTX_SCENE, -1, i,
                          (int)mouse.x, (int)mouse.y);
        }
    }

    {
        int last_scene_idx = s->scene_count;
        float plus_y = clip_y0 + last_scene_idx * row_h - g_scroll_y;
        float plus_h = ADD_SCENE_BTN_H;

        if (plus_y + plus_h >= clip_y0 && plus_y <= clip_y0 + scene_h_avail &&
            (mouse.y < clip_y0 + scene_h_avail || !CheckCollisionPointRec(mouse, (Rectangle){ scene_col_x, plus_y, SCENE_COL_W, plus_h }))) {
            float plus_x = scene_col_x;
            if (!g_ti_active && !g_ctx_open && !g_file_menu_open &&
                !g_audio_dlg_open &&
                draw_add_scene_button(plus_x, plus_y, SCENE_COL_W, plus_h)) {
                if (session_add_scene(s) < 0) {
                    ui_flash_message("Max scenes reached");
                }
            }
        }
    }
    EndScissorMode();

    /* the scrollbars are drawn BEFORE the bottom panels, so they do not cover the MIDI badges (those stick out above the panel) */
    if (need_h_scroll) draw_scrollbar(0, hsb_x, hsb_y, hsb_w, SCROLLBAR_H, content_w, avail_ch_w, g_scroll_x);
    if (need_v_scroll) draw_scrollbar(1, vsb_x, vsb_y, VSCROLLBAR_W, vsb_h, content_h, clip_area_h, g_scroll_y);

    BeginScissorMode((int)channels_x, 0, (int)(hdr_right - channels_x), screen_h);   /* next to the scrollbar, like the headers */
    bool shield_was = GfxMouseShieldGet();
    { float tx = GfxMouseTrue().x;   /* 1.3.12: the real mouse (in Eco "mouse" is far away) */
      if (tx < channels_x || tx >= hdr_right) GfxMouseShield(true); }    /* the hidden part does not react to the mouse */
    for (int c = first_vis_ch; c <= last_vis_ch; ++c) {
        float cx = channels_x + g_chx[c] - g_scroll_x;
        if (cx + g_chw[c] < channels_x) continue;
        if (cx > master_x) continue;

        if (!g_footer_hidden)
            draw_channel_footer(s, c, cx, footer_y,
                                g_chw[c] - GAP, fh);
    }
    GfxMouseShield(shield_was);
    EndScissorMode();

    /* ===== The top edge of the fader panel: it is dragged (up = taller; far down = hidden) ===== */
    {
        float hy = g_footer_hidden ? grid_bottom : footer_y;
        Rectangle band = { channels_x, hy - 2, hdr_right - channels_x - GAP, 5 };
        bool free_ui = !g_ti_active && !g_ctx_open && !g_file_menu_open && !g_audio_dlg_open &&
                       g_sb_drag == 0 && !g_drag_active && !g_drag_clip_active && g_dragging_slider < 0 && g_chres_ch < 0;
        bool hov = free_ui && CheckCollisionPointRec(mouse, band);
        if (hov || g_footer_drag) {
            SetMouseCursor(MOUSE_CURSOR_RESIZE_NS);
            DrawRectangle((int)band.x, (int)hy - 1, (int)band.width, 2, g_theme.text_dim);
        }
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            double now = GetTime();
            if (now - g_fd_last_t < DOUBLE_CLICK_TIME) {     /* double click = the normal height */
                g_footer_h = CHANNEL_FOOTER_H;
                g_footer_hidden = false;
                g_fd_last_t = 0;
                layout_save();
            } else {
                g_footer_drag = true;
                g_fd_y0 = mouse.y;
                g_fd_h0 = g_footer_hidden ? 0.0f : g_footer_h;
                g_fd_last_t = now;
            }
        }
        if (g_footer_drag) {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                float nh = g_fd_h0 + (g_fd_y0 - mouse.y);
                if (nh < FOOTER_HIDE_AT) {
                    g_footer_hidden = true;
                } else {
                    g_footer_hidden = false;
                    g_footer_h = nh < FOOTER_MIN ? FOOTER_MIN : (nh > footer_max ? footer_max : nh);
                }
            } else {
                g_footer_drag = false;
                if (g_footer_hidden && g_fd_h0 >= FOOTER_MIN) g_footer_h = g_fd_h0;   /* when it comes back: the old height */
                layout_save();
            }
        }
    }

    if (g_master_hidden) {                   /* 1.3.10: a thin handle - click or drag left = the master again */
        Rectangle hr = { master_x, grid_top, 14.0f, grid_bottom - grid_top };
        bool free_ui = !g_ti_active && !g_ctx_open && !g_file_menu_open && !g_audio_dlg_open && !g_drag_active &&
                       !g_drag_clip_active && g_dragging_slider < 0;
        bool hov = free_ui && CheckCollisionPointRec(mouse, hr);
        DrawRectangleRec(hr, hov ? g_theme.cell_hover : g_theme.panel_bg);
        DrawRectangleLinesEx(hr, 1, hov ? g_theme.selection : g_theme.grid_line);
        float cy = grid_top + (grid_bottom - grid_top) * 0.5f;
        DrawTriangle((Vector2){ master_x + 10, cy - 7 }, (Vector2){ master_x + 4, cy }, (Vector2){ master_x + 10, cy + 7 },
                     hov ? g_theme.text : g_theme.text_dim);
        if (hov) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { g_master_hidden = false; layout_save(); }
    } else draw_master(s, master_x, grid_top, g_master_w,
                grid_bottom - grid_top);

    /* the left edge of the master: it is dragged (wider = balance and MUTE); double click = the usual width;
     * 1.3.10: dragged far to the right = hidden */
    if (!g_master_hidden) {
        Rectangle grip = { master_x - 2, grid_top, 6, grid_bottom - grid_top };   /* does not overlap the scrollbar on the left */
        bool free_ui = !g_ti_active && !g_ctx_open && !g_file_menu_open && !g_audio_dlg_open &&
                       g_sb_drag == 0 && !g_drag_active && !g_drag_clip_active && g_dragging_slider < 0 &&
                       g_chres_ch < 0 && !g_footer_drag;
        bool hov = free_ui && CheckCollisionPointRec(mouse, grip);
        if (hov || g_mw_drag) {
            SetMouseCursor(MOUSE_CURSOR_RESIZE_EW);
            DrawRectangle((int)(master_x - 3), (int)grid_top, 2, (int)(grid_bottom - grid_top), g_theme.text_dim);
        }
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            double now = GetTime();
            if (now - g_mw_last_t < DOUBLE_CLICK_TIME) {
                g_master_w = MASTER_W; g_mw_last_t = 0; layout_save();
            } else {
                g_mw_drag = true; g_mw_x0 = mouse.x; g_mw_w0 = g_master_w; g_mw_last_t = now;
            }
        }
        if (g_mw_drag) {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                g_master_w = g_mw_w0 + (g_mw_x0 - mouse.x);       /* left = wider */
                if (g_master_w < MASTER_W - 70.0f) {             /* 1.3.10: far to the right = hide it */
                    g_master_w = g_mw_w0 < MASTER_W ? MASTER_W : g_mw_w0;
                    g_master_hidden = true;
                    g_mw_drag = false;
                    layout_save();
                    ui_flash_message("Master hidden - click the handle on the right or press Ctrl+Shift+M to show it");
                }
                if (g_master_w < MASTER_W) g_master_w = MASTER_W;
                if (g_master_w > MASTER_W_MAX) g_master_w = MASTER_W_MAX;
            } else {
                g_mw_drag = false;
                layout_save();
            }
        }
    }

    if (g_fb && g_fb->visible) {
        draw_library(g_fb, GAP, grid_top, LIBRARY_W,
                     grid_bottom - grid_top);
    }

    if (wave_h >= WAVEFORM_MIN_H) {
        float wave_y = screen_h - BOTTOM_BAR_H - GAP - wave_h;
        float wave_x = GAP;
        float wave_w = screen_w - GAP * 2;

        float rb_y = wave_y - RESIZE_BAR_H - 2;
        Rectangle rb_rect = { wave_x, rb_y, wave_w, RESIZE_BAR_H };

        bool rb_hovered = CheckCollisionPointRec(mouse, rb_rect)
                          && !g_ti_active && !g_ctx_open && !g_file_menu_open
                          && !g_audio_dlg_open;
        Color rb_col = rb_hovered ? g_theme.text_dim : g_theme.panel_bg;
        DrawRectangle((int)wave_x, (int)rb_y,
                      (int)wave_w, RESIZE_BAR_H, rb_col);

        if (rb_hovered) SetMouseCursor(MOUSE_CURSOR_RESIZE_NS);

        if (rb_hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            g_waveform_dragging = true;
        }
        if (g_waveform_dragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            float new_h = screen_h - BOTTOM_BAR_H - GAP - mouse.y;
            if (new_h < WAVEFORM_MIN_H) new_h = 0;
            else if (new_h > WAVEFORM_MAX_H) new_h = WAVEFORM_MAX_H;
            g_waveform_h = new_h;
        }

        draw_waveform_zone(s, wave_x, wave_y, wave_w, wave_h);
    }

    if (g_drag_active) {
        Vector2 m = GetMousePosition();
        Color ghost = g_theme.selection;
        ghost.a = 200;
        DrawRectangle((int)m.x + 8, (int)m.y + 8, 200, 24, ghost);
        DrawRectangleLines((int)m.x + 8, (int)m.y + 8,
                           200, 24, g_theme.text);
        char disp[NAME_LEN];
        snprintf(disp, sizeof(disp), "%s", g_drag_name);
        while (ui_text_w(disp, 14) > 180 && strlen(disp) > 3) {
            utf8_chop_last(disp);
        }
        ui_text(disp, (int)m.x + 16, (int)m.y + 12, 14, g_theme.bg);
    }

    if (g_drag_clip_active && g_drag_clip_src_ch >= 0) {
        Vector2 m = GetMousePosition();
        Clip *src = &s->channels[g_drag_clip_src_ch]
                            .clips[g_drag_clip_src_scene];

        bool is_note_drag = atomic_load(&src->is_note);
        Color ghost_bg = g_theme.selection, ghost_text = g_theme.bg;
        if (is_note_drag) note_colors(s, g_drag_clip_src_ch, src, &ghost_bg, NULL, &ghost_text);
        ghost_bg.a = 220;

        char disp[NAME_LEN];
        snprintf(disp, sizeof(disp), "%s", src->name);
        while (ui_text_w(disp, 14) > 180 && strlen(disp) > 3) {
            utf8_chop_last(disp);
        }

        DrawRectangle((int)m.x + 8, (int)m.y + 8, 200, 24, ghost_bg);
        DrawRectangleLines((int)m.x + 8, (int)m.y + 8, 200, 24, g_theme.text);
        ui_text(disp, (int)m.x + 16, (int)m.y + 12, 14, ghost_text);
    }

    carry_draw_ghost(s);                                   /* 1.4.17 */

    draw_bottom_bar(screen_w, screen_h);
    draw_audio_lost_banner(screen_w);
    draw_flash(screen_w);

    bool xf_edit = g_ti_numeric == 3;          /* the crossfade field is in the queue -> drawn above it (further down) */
    if (g_ti_active && g_ti_inline && !xf_edit) {
        ui_text_input_update();
        ui_text_input_draw(screen_w, screen_h);
    }

    /* the field that was being dragged has disappeared (e.g. the clip changed) -> let it go.
     * (the button is checked without the "shield" — otherwise a field in the queue was dropped right away) */
    GfxMouseShield(false);
    GfxRoundAll(R_CELL);                     /* step 36g: the drop-down menus and popups - rounded */
    draw_queue_panel(s);
    draw_route_popup(s);
    draw_edit_menu(s);
    draw_fx_slot_menu(s);
    draw_rec_input_menu(s);                       /* 1.4 */
    draw_fx_picker(s);
    if (g_ti_active && g_ti_inline && xf_edit) {
        ui_text_input_update();
        ui_text_input_draw(screen_w, screen_h);
    }
    /* 1.3.3: no button held -> the cursor is always visible again (whatever started the hidden dragging) */
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) GfxMouseRelative(false);
    if (g_vf_id != 0 && !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        if (g_vf_moved) GfxMouseRelative(false);
        g_vf_id = 0;
    }
    GfxUnzoomBegin();                        /* 1.3.12: opened from the top bar - its size too */
    draw_dsp_popup(GetScreenWidth());
    GfxUnzoomEnd();
    draw_log_popup(screen_w);

    if (g_ctx_open) {
        ctx_menu_draw(s, screen_w, screen_h);
    }

    GfxUnzoomBegin();                        /* 1.3.10: its menus too */
    if (g_file_menu_open) {
        draw_file_dropdown(GetScreenWidth(), GetScreenHeight());
    } else if (g_edit_menu_open) {
        draw_edit_dropdown(GetScreenWidth(), GetScreenHeight());
    } else if (g_play_menu_open) {
        draw_play_dropdown();
    }
    GfxUnzoomEnd();
    GfxRoundAll(0.0f);
}

/* Background behind a dialog: draw the real interface without input, so it can be captured and blurred. */
static void ui_draw_backdrop_world(Session *s)
{
    if (!GfxBackdropNeeded()) return;
    GfxInputSuppress(true);
    ui_draw_world(s);
    GfxInputSuppress(false);
}

/* 1.2: the MIDI keyboard follows the selection (like "auto-arm" in Ableton): selecting an instrument channel
 * (its name or one of its clips) makes the keyboard play it. Selecting a channel without an instrument keeps the
 * last one, so you can launch clips while you play. */
static void midi_target_follow(Session *s)
{
    static int last = -2;
    int cur = (g_ch_focus && g_chsel_anchor >= 0) ? g_chsel_anchor : g_selected_ch;
    if (cur >= s->channel_count) cur = -1;
    int tgt = session_get_midi_target(s);
    if (tgt >= 0 && !fxchain_has_instrument(s, tgt) && !fxchain_slot_loading(s, tgt, 0)) {
        bool any_row = tgt < s->channel_count && s->channels[tgt].fx_count > 0;
        if (!any_row) { session_set_midi_target(s, -1); tgt = -1; }
    }
    bool has = cur >= 0 && fxchain_has_instrument(s, cur);
    if ((cur != last && has) || (tgt < 0 && has)) session_set_midi_target(s, cur);
    last = cur;
}

void ui_draw(Session *s, int *out_sel_ch, int *out_sel_scene)
{
    g_ui_session = s;
    midi_target_follow(s);
    {   /* 1.4: the input of the card is needed only while a channel is armed (PipeWire opens it then) */
        static int last = -1;
        int any = 0;
        for (int c = 0; c < s->channel_count && !any; ++c) any = session_is_armed(s, c);
        if (any != last) { audio_input_want(any != 0); last = any; }
    }
    computer_keys(s);
    ui_separation_tick(s);                                 /* 1.4.14 (nothing to do unless a separation runs) */
    if (IsKeyPressed(KEY_ESCAPE) && fxchain_learning(NULL, NULL, NULL)) fxchain_learn_cancel();   /* 1.4.14 */
    if (out_sel_ch)    *out_sel_ch = g_selected_ch;
    if (out_sel_scene) *out_sel_scene = g_selected_scene;

    ClearBackground(g_theme.bg);

    if (g_ti_active && !g_ti_inline) {
        ui_text_input_update();
        ClearBackground(g_theme.bg);
        ui_draw_backdrop_world(s);
        GfxRoundAll(R_CELL);
        GfxUnzoomBegin(); GfxHoverLayer((Rectangle){ 0, 0, 1e6f, 1e6f }); ui_text_input_draw(GetScreenWidth(), GetScreenHeight()); GfxUnzoomEnd();   /* 1.3.11: dialogs keep their size */
        GfxRoundAll(0.0f);
        return;
    }

    dialogs_poll(s);                         /* 1.4.24: a surround file was just added -> ask how to play it */
    if (dialogs_any_open()) {
        ClearBackground(g_theme.bg);
        g_dragging_slider = -1;
        g_drag_active = false;
        g_drag_clip_active = false;
        g_drag_clip_src_ch = -1;
        g_drag_clip_src_scene = -1;
        g_waveform_dragging = false;
        g_dragging_loop_marker = 0;
        file_menu_close();
        edit_menu_close();
        ui_draw_backdrop_world(s);
        GfxRoundAll(R_CELL);                 /* step 36g: the dialogs - with rounded corners */
        GfxUnzoomBegin(); GfxHoverLayer((Rectangle){ 0, 0, 1e6f, 1e6f }); dialogs_draw(s, GetScreenWidth(), GetScreenHeight()); GfxUnzoomEnd();   /* 1.3.11: dialogs keep their size */
        GfxRoundAll(0.0f);
        return;
    }

    if (g_audio_dlg_open) {
        ClearBackground(g_theme.bg);
        g_dragging_slider = -1;
        g_drag_active = false;
        g_drag_clip_active = false;
        g_drag_clip_src_ch = -1;
        g_drag_clip_src_scene = -1;
        g_waveform_dragging = false;
        g_dragging_loop_marker = 0;
        ui_draw_backdrop_world(s);
        GfxRoundAll(R_CELL);                 /* step 36g: the dialogs - with rounded corners */
        GfxUnzoomBegin(); GfxHoverLayer((Rectangle){ 0, 0, 1e6f, 1e6f }); draw_audio_settings_dialog(GetScreenWidth(), GetScreenHeight()); GfxUnzoomEnd();   /* 1.3.11: dialogs keep their size */
        GfxRoundAll(0.0f);
        return;
    }

    if (g_about_open) {
        ClearBackground(g_theme.bg);
        g_dragging_slider = -1;
        g_drag_active = false;
        g_drag_clip_active = false;
        g_drag_clip_src_ch = -1;
        g_drag_clip_src_scene = -1;
        g_waveform_dragging = false;
        g_dragging_loop_marker = 0;
        ui_draw_backdrop_world(s);
        GfxRoundAll(R_CELL);                 /* step 36g: the dialogs - with rounded corners */
        GfxUnzoomBegin(); GfxHoverLayer((Rectangle){ 0, 0, 1e6f, 1e6f }); draw_about_dialog(GetScreenWidth(), GetScreenHeight()); GfxUnzoomEnd();   /* 1.3.11: dialogs keep their size */
        GfxRoundAll(0.0f);
        return;
    }

    if (g_perf_open) {                       /* 1.3.7 */
        ClearBackground(g_theme.bg);
        g_dragging_slider = -1;
        g_drag_active = false;
        g_drag_clip_active = false;
        g_drag_clip_src_ch = -1;
        g_drag_clip_src_scene = -1;
        g_waveform_dragging = false;
        g_dragging_loop_marker = 0;
        ui_draw_backdrop_world(s);
        GfxRoundAll(R_CELL);
        GfxUnzoomBegin(); GfxHoverLayer((Rectangle){ 0, 0, 1e6f, 1e6f }); draw_perf_dialog(GetScreenWidth(), GetScreenHeight()); GfxUnzoomEnd();   /* 1.3.11: dialogs keep their size */
        GfxRoundAll(0.0f);
        return;
    }

    if (g_collect_busy) {                    /* 2.0: collect running */
        ClearBackground(g_theme.bg);
        g_dragging_slider = -1;
        g_drag_active = false;
        g_drag_clip_active = false;
        g_waveform_dragging = false;
        ui_draw_backdrop_world(s);
        GfxRoundAll(R_CELL);
        GfxUnzoomBegin(); GfxHoverLayer((Rectangle){ 0, 0, 1e6f, 1e6f }); draw_collect_progress(GetScreenWidth(), GetScreenHeight()); GfxUnzoomEnd();
        GfxRoundAll(0.0f);
        return;
    }

    if (g_collect_open) {                    /* 2.0 */
        ClearBackground(g_theme.bg);
        g_dragging_slider = -1;
        g_drag_active = false;
        g_drag_clip_active = false;
        g_drag_clip_src_ch = -1;
        g_drag_clip_src_scene = -1;
        g_waveform_dragging = false;
        g_dragging_loop_marker = 0;
        ui_draw_backdrop_world(s);
        GfxRoundAll(R_CELL);
        GfxUnzoomBegin(); GfxHoverLayer((Rectangle){ 0, 0, 1e6f, 1e6f }); draw_collect_dialog(GetScreenWidth(), GetScreenHeight()); GfxUnzoomEnd();
        GfxRoundAll(0.0f);
        return;
    }

    if (g_loud_open) {                       /* 1.4.11 */
        ClearBackground(g_theme.bg);
        g_dragging_slider = -1;
        g_drag_active = false;
        g_drag_clip_active = false;
        g_drag_clip_src_ch = -1;
        g_drag_clip_src_scene = -1;
        g_waveform_dragging = false;
        g_dragging_loop_marker = 0;
        ui_draw_backdrop_world(s);
        GfxRoundAll(R_CELL);
        GfxUnzoomBegin(); GfxHoverLayer((Rectangle){ 0, 0, 1e6f, 1e6f }); draw_loud_dialog(GetScreenWidth(), GetScreenHeight()); GfxUnzoomEnd();
        GfxRoundAll(0.0f);
        return;
    }

    if (g_plugins_open) {
        ClearBackground(g_theme.bg);
        g_dragging_slider = -1;
        g_drag_active = false;
        g_drag_clip_active = false;
        g_drag_clip_src_ch = -1;
        g_drag_clip_src_scene = -1;
        g_waveform_dragging = false;
        g_dragging_loop_marker = 0;
        ui_draw_backdrop_world(s);
        GfxRoundAll(R_CELL);                 /* step 36g: the dialogs - with rounded corners */
        GfxUnzoomBegin(); GfxHoverLayer((Rectangle){ 0, 0, 1e6f, 1e6f }); draw_plugins_dialog(GetScreenWidth(), GetScreenHeight()); GfxUnzoomEnd();   /* 1.3.11: dialogs keep their size */
        GfxRoundAll(0.0f);
        return;
    }

    if (g_appearance_open) {
        ClearBackground(g_theme.bg);
        // Clear the drag/selection state
        g_dragging_slider = -1;
        g_drag_active = false;
        g_drag_clip_active = false;
        g_drag_clip_src_ch = -1;
        g_drag_clip_src_scene = -1;
        g_waveform_dragging = false;
        g_dragging_loop_marker = 0;
        ui_draw_backdrop_world(s);
        GfxRoundAll(R_CELL);                 /* step 36g: the dialogs - with rounded corners */
        GfxUnzoomBegin(); GfxHoverLayer((Rectangle){ 0, 0, 1e6f, 1e6f }); draw_appearance_dialog(GetScreenWidth(), GetScreenHeight()); GfxUnzoomEnd();   /* 1.3.11: dialogs keep their size */
        GfxRoundAll(0.0f);
        return;
    }

    if (g_midi_settings_open) {
        ClearBackground(g_theme.bg);
        g_dragging_slider = -1;
        g_drag_active = false;
        g_drag_clip_active = false;
        g_drag_clip_src_ch = -1;
        g_drag_clip_src_scene = -1;
        g_waveform_dragging = false;
        g_dragging_loop_marker = 0;
        ui_draw_backdrop_world(s);
        GfxRoundAll(R_CELL);                 /* step 36g: the dialogs - with rounded corners */
        GfxUnzoomBegin(); GfxHoverLayer((Rectangle){ 0, 0, 1e6f, 1e6f }); draw_midi_settings_dialog(GetScreenWidth(), GetScreenHeight()); GfxUnzoomEnd();   /* 1.3.11: dialogs keep their size */
        GfxRoundAll(0.0f);
        return;
    }

    ui_draw_world(s);
}

// ============ MIDI Learn ============

void ui_toggle_midi_learn(void)
{
    g_midi_learn_mode = !g_midi_learn_mode;

    if (g_midi_learn_mode) {
        ui_flash_message("MIDI Learn ON");
    } else {
        ui_flash_message("MIDI Learn: OFF");
    }
}

bool ui_midi_learn_is_active(void)
{
    return g_midi_learn_mode;
}

// ============ General ============

bool ui_any_modal_open(void)
{
    return g_ti_active || g_audio_dlg_open || g_appearance_open || g_about_open || g_perf_open || g_loud_open || g_collect_open || g_collect_busy || g_plugins_open || g_midi_settings_open || dialogs_any_open() ||
           g_fxp_ch >= 0;                       /* the plugin list: typing is search, not keyboard shortcuts */
}
