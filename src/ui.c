/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#include "ui.h"
#include "version.h"
#include "plugins.h"
#include "fxchain.h"
#include "midiq.h"
#include <ctype.h>
#include "ui_dialogs.h"
#include "theme.h"
#include "midi.h"
#include "sdlgfx.h"
#include "stream.h"
#include "dsp_meter.h"
#include "audio.h"
#include "settings.h"
#include "project.h"

static bool fx_ui_free(void);                       /* 1.4: forward declarations */
static void ui_rec_click(Session *s, int ch, int sc);
#include "applog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

// Forward declarations
static void draw_midi_badge(Session *s, int ch, float x, float y);

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
static void ui_text_input_update(void);
static void ui_text_input_draw(int screen_w, int screen_h);
static void draw_audio_settings_dialog(int screen_w, int screen_h);
static void draw_appearance_dialog(int screen_w, int screen_h);

#define MENU_BAR_H       28
#define TOP_BAR_H        44
#define BOTTOM_BAR_H     28
#define SCENE_COL_W      56
#define MASTER_W         170
#define LIBRARY_W        320
#define CHANNEL_MIN_W    CH_WIDTH_MIN
#define GAP              6
#define CLIP_HEADER_H    32
#define CHANNEL_FOOTER_H 184
#define SLIDER_W         36
#define SLIDER_H         70
#define VU_W             16      /* stereo: two bars (L and R) */
#define VU_LABEL_W       28
/* step 36f: slightly rounded corners */
#define R_CELL           4.0f   /* cells, headers, buttons */
#define R_SMALL          3.0f   /* faders, small fields */
#define R_PANEL          6.0f   /* panels (faders, Master) */
#define CLIP_TOP_GAP     4      /* step 36g: gap between the headers and the first scene */
#define SCROLLBAR_H      10
#define VSCROLLBAR_W     10
#define CLIP_MIN_H       36

#define WAVEFORM_MIN_H   80
#define WAVEFORM_DEF_H   140
#define WAVEFORM_MAX_H   600
#define RESIZE_BAR_H     6
#define WAVE_HDR_H       22
#define WAVE_RULER_H     20
#define WAVE_STEREO_MIN_H 200
#define LOOP_BAR_Y        9      /* the loop bar: in the lower half of the ruler (above the start/end markers) */
#define LOOP_BAR_H        11    /* from this waveform height up: separate left and right channel */

#define ADD_SCENE_BTN_H  20
#define ADD_CH_BTN_W     30
#define LOOP_BTN_H       32
#define MIDI_BTN_H       32
#define SMALL_BTN_H      16      /* the two narrow buttons under MIDI (half as tall) */

#define FLASH_DURATION   2.0f
#define TEXT_INPUT_MAX   256

/* Removes the last CHARACTER (not byte) - Cyrillic is 2 bytes, otherwise a broken character is left. */
static void utf8_chop_last(char *s)
{
    size_t n = strlen(s);
    if (n == 0) return;
    do { n--; } while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80);
    s[n] = '\0';
}

#define CTX_MENU_W      160
#define CTX_MENU_ROW_H  24
#define CTX_MENU_PAD    4

#define DOUBLE_CLICK_TIME 0.35
#define DOUBLE_CLICK_DIST 8.0f

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
static Rectangle row_hl(Rectangle r) { return (Rectangle){ r.x + 3, r.y + 1, r.width - 6, r.height - 2 }; }

static int color_lum(Color c) { return (299 * c.r + 587 * c.g + 114 * c.b) / 1000; }

/* the real colour from the palette (for filling headers/notes) */
static Color palette_raw(int idx)
{
    if (idx < 0) idx = 0;
    return PALETTE[idx % N_PALETTE];
}

/* A colour that is VISIBLE on the dark/light panel (for faders, knobs, lines):
 * a black/dark channel is lightened, a light one on a light theme is darkened. */
static Color channel_color(int idx)
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

static bool g_can_undo = false, g_can_redo = false;
void ui_set_undo_state(bool can_undo, bool can_redo)
{
    g_can_undo = can_undo;
    g_can_redo = can_redo;
}

static Font g_font;
static bool g_font_loaded = false;

static FileBrowser *g_fb = NULL;

static int g_dragging_slider = -1;
static int g_selected_ch     = -1;
static int g_selected_scene  = -1;
static int g_hovered_ch      = -1;
static int g_hovered_scene   = -1;

static bool g_drag_active = false;
static char g_drag_path[512] = {0};
static char g_drag_name[256] = {0};

static bool g_drag_clip_active = false;
static int  g_drag_clip_src_ch = -1;
static int  g_drag_clip_src_scene = -1;

/* ===== Step 36: marking several things (Ctrl/Shift + click) and selecting/moving channels ===== */
static unsigned char g_cmark[MAX_CHANNELS][MAX_SCENES];   /* marked cells */
static int   g_cmark_n = 0;
static int   g_anchor_ch = -1, g_anchor_sc = -1;          /* where Shift + click starts from */
static bool  g_chsel[MAX_CHANNELS];                       /* marked channels */
static int   g_chsel_n = 0;
static int   g_chsel_anchor = -1;
static bool  g_ch_focus = false;                           /* a header was clicked last -> Delete deletes the channels */
static int   g_chdrag = -1;                                /* the header being dragged (reorders the channels) */
static Vector2 g_chdrag_p0;
static bool  g_chdrag_moved = false;
static bool  g_cdrag_from_mark = false;                    /* the dragged clip is marked -> all marked ones move */
static bool  g_libsel[FB_MAX_ENTRIES];                     /* marked files in the library */
static int   g_libsel_n = 0, g_libsel_anchor = -1;
static char  g_libsel_dir[FB_MAX_PATH];
static bool  g_drag_multi = false;                         /* several files are being dragged from the library */

static void cmark_clear(void) { if (g_cmark_n) memset(g_cmark, 0, sizeof g_cmark); g_cmark_n = 0; }
static bool cmark_get(int c, int sc) { return c >= 0 && c < MAX_CHANNELS && sc >= 0 && sc < MAX_SCENES && g_cmark[c][sc]; }
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
static void chsel_only(int c) { chsel_clear(); chsel_set(c, true); g_chsel_anchor = c; g_ch_focus = c >= 0; }
static void libsel_clear(void) { memset(g_libsel, 0, sizeof g_libsel); g_libsel_n = 0; }

static float g_scroll_x = 0.0f;
static float g_scroll_y = 0.0f;
static bool  g_scroll_to_sel = false;   /* the arrow keys moved the selection -> make it visible */
static int   g_scene_hl = -1, g_scene_hl_next = -1;   /* the mouse over a scene button -> the clips get a light outline */

static float g_waveform_h = WAVEFORM_DEF_H;
static bool  g_waveform_dragging = false;

static char  g_flash_msg[1024] = {0};
static float g_flash_dur = FLASH_DURATION;   /* long messages (e.g. a path) stay longer */
static double g_flash_until = 0.0;

static bool  g_ti_active = false;
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
static int   g_ti_numeric = 0;
static int   g_ti_num_ch = -1, g_ti_num_sc = -1;
static bool  g_ti_skip_click = false;          /* the click that opened the field does not close it */
static bool  g_ti_select_all = false;          /* the number is selected: the first character replaces it */
static Session *g_ui_session = NULL;           /* for applying the number on Enter */
static bool  g_queue_open = false;             /* the queue list is open */
static bool  g_q_just_opened = false;
static int   g_q_drag = -1;                    /* which row is being dragged (-1 = none) */
static float g_q_grab = 0.0f;                  /* where in the row it was grabbed */
static int   g_q_scroll = 0;                   /* the first visible row */
static Rectangle g_q_btn, g_q_rect;            /* the button and the popup (for the mouse) */
static float g_q_anchor_x = 0, g_q_anchor_bottom = 0, g_q_top_limit = 0;
static bool  g_wave_follow = false;
static Vector2 g_drag_clip_p0;                   /* where the clip was pressed (drag = only if the mouse has moved) */
static size_t g_loop_mv_s0, g_loop_mv_e0;        /* moving the whole loop: start/end when grabbed */
static double g_loop_mv_grab;                    /* the frame under the mouse when grabbed */
/* Log (indicator at the top right, as in Ardour) */
static bool  g_log_open = false, g_log_just_opened = false;
static int   g_log_scroll = 0;                 /* rows up from the newest (0 = follow the newest) */
static Rectangle g_log_rect, g_log_btn;
/* The fader panel (at the bottom): its top edge is dragged; shrink it a lot — and it hides.
 * It comes back with Ctrl+F or a double click on a clip/channel header. */
#define FOOTER_MIN      CHANNEL_FOOTER_H   /* it cannot be lower (the buttons on the left must fit) */
#define FOOTER_HIDE_AT  90.0f              /* drag below this -> it hides */
static float g_footer_h = CHANNEL_FOOTER_H;
static bool  g_footer_hidden = false;
/* Plugins (step 33): an FX strip above the faders of every channel */
#define FX_STRIP_H   96
#define FX_ROW_H     18
static bool      g_fx_open = false;
static bool      g_fx_toggle_req = false;     /* the FX button was pressed - applied before the frame's layout */
static int       g_fxp_ch = -1;                 /* open plugin list for this channel */
static Vector2   g_fxp_pos;
static Rectangle g_fxp_rect;
static char      g_fxp_filter[64];
static int       g_fxp_scroll = 0;
static bool      g_fxp_just = false;
static int       g_fxm_ch = -1, g_fxm_idx = -1;  /* menu on a slot (right click) */
static Vector2   g_fxm_pos;
static Rectangle g_fxm_rect;
static bool      g_fxm_just = false;
static int       g_fx_scroll[MAX_CHANNELS];
/* Queue crossfade (step 17): whether it is on and how many seconds. Remembered in layout.conf. */
#define XF_SEC_MIN      0.1f
#define XF_SEC_DEFAULT  6.0f
static bool  g_xf_on  = false;
static float g_xf_sec = XF_SEC_DEFAULT;
static bool  g_footer_drag = false;
/* Master width (step 24): its left edge is dragged; with a wide master the balance and MUTE are shown */
#define MASTER_W_MAX   460
#define MASTER_W_WIDE  250        /* from this width up: the extra column */
static float g_master_w = MASTER_W;
static bool  g_mw_drag = false;
static float g_mw_x0 = 0, g_mw_w0 = 0;
static double g_mw_last_t = 0;
static float g_fd_y0 = 0, g_fd_h0 = 0;
static double g_fd_last_t = 0;
static double g_cell_last_t = 0;
static int   g_cell_last_ch = -1, g_cell_last_sc = -1;

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
    fprintf(f, "footer_h=%d\nfooter_hidden=%d\nxfade_on=%d\nxfade_sec=%.2f\nmaster_w=%d\nfx_open=%d\n",
            (int)g_footer_h, g_footer_hidden ? 1 : 0, g_xf_on ? 1 : 0, g_xf_sec, (int)g_master_w, g_fx_open ? 1 : 0);
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
#define FOLLOW_SEC_MAX 60.0f
static float g_follow_sec  = FOLLOW_SEC_MAX;   /* how many seconds are visible in this mode (when turned on: the farthest out) */
static void num_edit_begin(int kind, int ch, int sc, float value, Rectangle r);

/* "128", "127.5", "127.53" */
static void fmt_bpm(char *out, size_t n, float v)
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
static int   g_vf_id = 0;          /* which field is grabbed */
static float g_vf_v0, g_vf_acc;    /* value when grabbed; accumulated upward movement (px) */
static bool  g_vf_moved;
static int   g_vf_last_id = 0;
static double g_vf_last_t = 0.0;

static int value_field_input_ex(int id, Rectangle r, float *val, bool enabled,
                                float vmin, float vmax, float step, float fine);
static int value_field_input(int id, Rectangle r, float *val, bool enabled)
{
    return value_field_input_ex(id, r, val, enabled, BPM_MIN, BPM_MAX, 0.1f, 0.01f);
}
/* General: vmin..vmax, step per pixel (fine with Shift); the value is rounded to 0.01. */
static int value_field_input_ex(int id, Rectangle r, float *val, bool enabled,
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

static double g_last_click_time = 0.0;
static int    g_last_click_ch = -1;
static int    g_last_click_scene = -1;

static int g_dragging_loop_marker = 0;
static int64_t g_end_ext = 0;      /* step 36k: the End marker is dragged past the end of the clip (0 = no) */

typedef enum {
    CTX_NONE = 0,
    CTX_CHANNEL,
    CTX_CLIP,
    CTX_EMPTY,
    CTX_SCENE,
} CtxType;

static bool    g_ctx_open = false;
static CtxType g_ctx_type = CTX_NONE;
static int     g_ctx_x = 0;
static int     g_ctx_y = 0;
static int     g_ctx_ch = -1;
static int     g_ctx_scene = -1;
static double  g_ctx_open_time = 0.0;

static UiActionRequest g_pending_action = { UI_ACTION_NONE, -1, -1 };

static bool g_file_menu_open = false;
static double g_file_menu_open_time = 0.0;
static FileMenuAction g_pending_file_action = FILE_MENU_NONE;

static bool g_edit_menu_open = false;
static Rectangle g_menu_dd_rect;             /* the open drop-down menu (File/Edit): a click in it does not reach the cells below */
static double g_edit_menu_open_time = 0.0;
static EditMenuAction g_pending_edit_action = EDIT_MENU_NONE;

static bool        g_audio_dlg_open = false;
static double      g_audio_dlg_open_time = 0.0;
static AudioConfig g_audio_dlg_config;
static AudioConfig g_audio_dlg_original;
static bool        g_audio_dlg_apply_flag = false;

static bool g_midi_learn_mode = false;

// Appearance dialog state
static bool   g_about_open = false;
static bool   g_plugins_open = false;
static bool   g_appearance_open = false;
static double g_appearance_open_time = 0.0;
static Theme  g_appearance_backup;   // for cancel
static char   g_appearance_bk_theme[32], g_appearance_bk_accent[32];
static int    g_appearance_bk_font = 14;
static void appearance_restore(void)
{
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

void ui_clipboard_copy_clip(const char *path, const char *name)
{
    if (!path) return;
    ui_clipboard_set_edits(NULL);
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

static void file_menu_open(void)
{
    g_file_menu_open = true;
    g_file_menu_open_time = GetTime();
}

static void file_menu_close(void)
{
    g_file_menu_open = false;
}

static void edit_menu_open(void)
{
    g_edit_menu_open = true;
    g_edit_menu_open_time = GetTime();
}

static void edit_menu_close(void)
{
    g_edit_menu_open = false;
}

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
// ============ Menu bar ============

static void draw_menu_bar(Session *s, int screen_w)
{
    DrawRectangle(0, 0, screen_w, MENU_BAR_H, g_theme.panel_bg);

    float file_x = 8;
    float file_w = 60;
    float file_h = MENU_BAR_H;

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

    ui_text("File", (int)(file_x + 8), (int)(MENU_BAR_H / 2 - 8),
            15, g_file_menu_open ? g_theme.selection : g_theme.text);

    if (file_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (g_file_menu_open) {
            file_menu_close();
        } else {
            edit_menu_close();
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

    ui_text("Edit", (int)(edit_x + 8), (int)(MENU_BAR_H / 2 - 8),
            15, g_edit_menu_open ? g_theme.selection : g_theme.text);

    if (edit_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (g_edit_menu_open) {
            edit_menu_close();
        } else {
            file_menu_close();
            edit_menu_open();
        }
    }

    // Routing and Shortcuts: open a dialog directly
    float rt_x = edit_x + edit_w + 8;
    float rt_w = 78;
    Rectangle rt_r = { rt_x, 0, rt_w, file_h };
    bool rt_hover = CheckCollisionPointRec(mouse, rt_r);
    if (rt_hover) {
        Color hc = g_theme.panel_bg;
        hc.r = (unsigned char)(hc.r + 30); hc.g = (unsigned char)(hc.g + 30); hc.b = (unsigned char)(hc.b + 30);
        DrawRectangle((int)rt_x, 0, (int)rt_w, (int)file_h, hc);
    }
    ui_text("Routing", (int)(rt_x + 8), (int)(MENU_BAR_H / 2 - 8), 15, g_theme.text);
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
    ui_text("Shortcuts", (int)(sc_x + 8), (int)(MENU_BAR_H / 2 - 8), 15, g_theme.text);
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
        Rectangle r = { (float)screen_w - bw - 8.0f, 4.0f, bw, (float)(MENU_BAR_H - 8) };
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

static void draw_edit_dropdown(int screen_w, int screen_h)
{
    (void)screen_w;
    (void)screen_h;
    if (!g_edit_menu_open) return;

    const int item_h = 24;
    const int menu_w = 220;
    const int pad = 4;

    const char *items[] = { "Undo", "Redo", NULL, "Audio...", "MIDI...", "Plugins...", "Appearance...", NULL, "About Q-LiM..." };
    const char *shortcuts[] = { "Ctrl+Z", "Ctrl+Shift+Z", NULL, "Ctrl+,", "Ctrl+M", NULL, NULL, NULL, NULL };
    EditMenuAction actions[] = { EDIT_MENU_UNDO, EDIT_MENU_REDO, EDIT_MENU_NONE,
                                 EDIT_MENU_AUDIO, EDIT_MENU_MIDI, EDIT_MENU_NONE, EDIT_MENU_APPEARANCE, EDIT_MENU_NONE, EDIT_MENU_NONE };
    bool enabled[] = { g_can_undo, g_can_redo, true, true, true, true, true, true, true };
    int n_items = 9;

    int menu_h = pad * 2;
    for (int i = 0; i < n_items; ++i) menu_h += items[i] ? item_h : 6;

    int mx = 8 + 60 + 8;
    int my = MENU_BAR_H;

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
        Rectangle edit_r = { 8 + 60 + 8, 0, 60, MENU_BAR_H };
        if (!CheckCollisionPointRec(mouse, edit_r)) {
            edit_menu_close();
        }
        return;
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        edit_menu_close();
        return;
    }

    if (clicked == 8) {                      /* About - opens here, without main.c */
        g_about_open = true;
        edit_menu_close();
    } else if (clicked == 5) {               /* Plugins... */
        g_plugins_open = true;
        plugins_start();
        edit_menu_close();
    } else if (clicked >= 0 && actions[clicked] != EDIT_MENU_NONE) {
        g_pending_edit_action = actions[clicked];
        edit_menu_close();
    }
}

static void draw_file_dropdown(int screen_w, int screen_h)
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
    int n_items = 6;

    int menu_h = pad * 2;
    for (int i = 0; i < n_items; ++i) {
        if (items[i] == NULL) menu_h += 6;
        else menu_h += item_h;
    }

    int mx = 8;
    int my = MENU_BAR_H;

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
        Rectangle file_r = { 8, 0, 60, MENU_BAR_H };
        if (!CheckCollisionPointRec(mouse, file_r)) {
            file_menu_close();
        }
        return;
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        file_menu_close();
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
static bool   g_dsp_open = false;
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
    return (Rectangle){ (float)(screen_w - MASTER_W - GAP), (float)(MENU_BAR_H + 7),
                        (float)MASTER_W, (float)(TOP_BAR_H - 14) };
}

static Rectangle dsp_popup_rect(int screen_w)
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

bool ui_dsp_panel_is_open(void) { return g_dsp_open; }
void ui_toggle_dsp_panel(void) { g_dsp_open = !g_dsp_open; g_dsp_just_opened = g_dsp_open; }

/* The small indicator above Master: a mini graph in the background, a bar and a percentage. */
static void draw_dsp_widget(int screen_w)
{
    dsp_update();
    Rectangle r = dsp_widget_rect(screen_w);
    Vector2 m = GetMousePosition();
    bool hov = CheckCollisionPointRec(m, r);

    Color bg = g_theme.panel_bg2;
    DrawRectangleRounded(r, R_CELL, hov ? (Color){ (unsigned char)(bg.r + 14), (unsigned char)(bg.g + 14), (unsigned char)(bg.b + 14), 255 } : bg);

    /* mini graph: the last ~ 30 seconds, one column per 2 pixels */
    int cols = (int)(r.width - 2) / 2;
    if (cols > g_dsp_hist_count) cols = g_dsp_hist_count;
    int inner_h = (int)r.height - 4;
    for (int c = 0; c < cols; ++c) {
        float a, p;
        dsp_hist_at(g_dsp_hist_count - cols + c, &a, &p);
        if (a > 1.0f) a = 1.0f;
        int h = (int)(a * inner_h + 0.5f);
        Color col = dsp_level_color(a);
        col.a = 120;
        int x = (int)(r.x + r.width - 1 - (cols - c) * 2);
        if (h > 0) DrawRectangle(x, (int)(r.y + r.height - 2 - h), 2, h, col);
    }

    float v = g_dsp_now > 1.0f ? 1.0f : g_dsp_now;
    DrawRectangle((int)r.x + 1, (int)(r.y + r.height - 4), (int)((r.width - 2) * v), 3, dsp_level_color(g_dsp_now));
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
static void draw_dsp_popup(int screen_w)
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

/* the whole wordmark centred on cx (and kept between lmin and rmax) */
static void draw_wordmark(float cx, float cy, float lmin, float rmax)
{
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
static const float  QUANT_VALS[]   = { 0.0f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f };
static const char  *QUANT_NAMES[]  = { "Now", "1 Beat", "1/2 Bar", "1 Bar", "2 Bars", "4 Bars" };
#define QUANT_N 6

static float draw_transport_group(float x, float y0)
{
    Session *s = g_ui_session;
    if (!s) return x;
    Vector2 m = GetMousePosition();
    bool free_ui = !g_ti_active && !g_ctx_open && !g_file_menu_open && !g_edit_menu_open && !g_audio_dlg_open;
    float h = 28.0f, y = y0 + (TOP_BAR_H - h) * 0.5f;

    /* launch quantization */
    float q = session_get_quantize(s);
    int qi = 0;
    for (int i = 0; i < QUANT_N; ++i) if (fabsf(QUANT_VALS[i] - q) < 0.01f) qi = i;
    char ql[48];
    snprintf(ql, sizeof ql, "Launch: %s", QUANT_NAMES[qi]);
    float qw = (float)ui_text_w("Launch: 1/2 Bar", 14) + 22.0f;
    Rectangle qr = { x, y, qw, h };
    bool qh = free_ui && CheckCollisionPointRec(m, qr);
    DrawRectangleRounded(qr, R_CELL, qh ? g_theme.cell_hover : g_theme.panel_bg2);
    DrawRectangleRoundedLines(qr, R_CELL, 1, qi ? g_theme.selection : g_theme.text_dim);
    ui_text(ql, (int)(x + 11), (int)(y + (h - 14) / 2) - 1, 14, qi ? g_theme.text : g_theme.text_dim);
    if (qh) {
        SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))  session_set_quantize(s, QUANT_VALS[(qi + 1) % QUANT_N]);
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) session_set_quantize(s, QUANT_VALS[(qi + QUANT_N - 1) % QUANT_N]);
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
    else if (res == 2) num_edit_begin(1, -1, -1, tempo, (Rectangle){ x, y - 1, bw > 100 ? bw : 100, h + 2 });
    x += bw + 8.0f;

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

    /* the beat in the bar: 4 dots (the 1st one brighter) */
    double beats = 0.0;
    bool run = session_transport(s, &beats);
    int beat = run ? (int)floor(beats) % 4 : -1;
    for (int i = 0; i < 4; ++i) {
        float d = 9.0f;
        Rectangle r = { x + i * (d + 5.0f), y + (h - d) / 2, d, d };
        Color c = g_theme.panel_bg2;
        if (i == beat) c = i == 0 ? g_theme.selection : g_theme.text;
        DrawRectangleRounded(r, 0.4f, c);
        DrawRectangleRoundedLines(r, 0.4f, 1, g_theme.text_dim);
    }
    x += 4 * 14.0f;
    if (run) {
        char bb[24];
        snprintf(bb, sizeof bb, "%d.%d", (int)(beats / 4.0) + 1, beat + 1);
        ui_text(bb, (int)(x + 4), (int)(y + (h - 13) / 2), 13, g_theme.text_dim);
    }
    return x + 40.0f;
}

static void draw_top_bar(int screen_w)
{
    int y0 = MENU_BAR_H;

    DrawRectangle(0, y0, screen_w, TOP_BAR_H, g_theme.panel_bg);
    float group_r = draw_transport_group(12.0f, (float)y0);
    {   /* 1.2.1: the wordmark in the middle of the WINDOW (where the eye looks for the centre), never over the buttons */
        float r = (float)(screen_w - MASTER_W - GAP - 36 - 12) - 16.0f;
        draw_wordmark(screen_w * 0.5f, y0 + TOP_BAR_H * 0.5f, group_r + 16.0f, r);
    }

    float icon_w = 36;
    float icon_h = 28;
    float icon_x = screen_w - MASTER_W - GAP - icon_w - 12;   /* to the left of the DSP indicator */
    float icon_y = y0 + (TOP_BAR_H - icon_h) / 2;

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

static void draw_bottom_bar(int screen_w, int screen_h)
{
    DrawRectangle(0, screen_h - BOTTOM_BAR_H,
                  screen_w, BOTTOM_BAR_H, g_theme.panel_bg);

    const char *hint =
        "SPACE: pause | ENTER: play | DEL: clear | Ctrl+R: rename | "
        "RMB: menu | L: library | Ctrl+W: waveform | Ctrl+M: MIDI learn";
    ui_text(hint, 12, screen_h - BOTTOM_BAR_H + 8, 14, g_theme.text_dim);
}

static void format_time(char *buf, size_t bufsize, double seconds)
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

static double ruler_interval(double total_sec)
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
static bool g_clip_led[MAX_CHANNELS + 1];

/* id: a channel or MAX_CHANNELS = master; clipped = the audio thread has seen > 0 dBFS (since the last frame) */
static void draw_vu_meter(float x, float y, float w, float h, float lvl_l, float lvl_r, int id, bool clipped)
{
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
        if (clipped) g_clip_led[id] = true;
        Rectangle led = { x, y - 7, w, 5 };
        Rectangle hit = { x - 2, y - 10, w + 4, h + 12 };
        DrawRectangleRounded(led, 2, g_clip_led[id] ? g_theme.vu_red : mix_col(g_theme.bg, g_theme.text_dim, 0.35f));
        if (g_clip_led[id] && CheckCollisionPointRec(GetMousePosition(), hit) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            !g_ctx_open && !g_ti_active)
            g_clip_led[id] = false;                     /* click on the meter/lamp = reset */
    }
    DrawLine((int)(x + 1 + bw + 0.5f), (int)y, (int)(x + 1 + bw + 0.5f), (int)(y + h), g_theme.panel_bg);

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

    DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, 2, 1, g_theme.text_dim);

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

static void draw_clip_cell(Session *s, int ch, int scene,
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
            Vector2 c = { x + h / 2, y + h / 2 };
            DrawCircleV(c, h * 0.2f, RAYWHITE);
            char lb[64];
            if (rst == REC_WAIT) snprintf(lb, sizeof lb, "REC - waits for the bar");
            else snprintf(lb, sizeof lb, "REC  %d.%d%s", (int)(rb / 4.0) + 1, (int)fmod(rb, 4.0) + 1, rst == REC_STOPPING ? "  (stops on the bar)" : "");
            while (ui_text_w_scaled(lb, 13) > w - h - 8 && strlen(lb) > 4) utf8_chop_last(lb);
            ui_text_scaled(lb, (int)(x + h + 4), (int)(y + h / 2 - 8), 13, RAYWHITE);
            if (CheckCollisionPointRec(GetMousePosition(), (Rectangle){ x, y, w, h }) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                !g_drag_active && !g_ctx_open && !g_ti_active && !g_audio_dlg_open && fx_ui_free()) {
                g_selected_ch = ch; g_selected_scene = scene;
                ui_rec_click(s, ch, scene);
            }
            return;
        }
    }

    if (is_note) {
        int ncol = cl->color;                                   /* 0 = the theme's default colour */
        Color n_bg     = ncol > 0 ? palette_raw(ncol - 1) : g_theme.note_bg;
        Color n_border = ncol > 0 ? (Color){ (unsigned char)(n_bg.r / 2), (unsigned char)(n_bg.g / 2), (unsigned char)(n_bg.b / 2), 255 }
                                  : g_theme.note_border;
        if (ncol > 0 && color_lum(n_bg) < 70) n_border = (Color){ 150, 150, 150, 255 };
        Color n_text   = ncol > 0 ? ui_text_color_on(n_bg) : g_theme.note_text;
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
    DrawRectangleRounded((Rectangle){ x, y, w, h }, R_CELL, bg);

    if (sel) {
        DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_CELL, 2, g_theme.selection);
    } else if (hov) {
        DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_CELL, 1, g_theme.text_dim);
    }

    float btn_size = h;
    float btn_x    = x;                 /* exactly in line with the left edge of the channel header */
    float btn_y    = y;
    Rectangle btn_rect = { btn_x, btn_y, btn_size, btn_size };

    Vector2 c = { btn_x + btn_size/2, btn_y + btn_size/2 };
    float tri_s = btn_size * 0.28f;

    if (!has) {
        Color empty_btn = g_theme.panel_bg;
        empty_btn.r = (unsigned char)(empty_btn.r + 15);
        empty_btn.g = (unsigned char)(empty_btn.g + 15);
        empty_btn.b = (unsigned char)(empty_btn.b + 15);
        bool sq_hov = CheckCollisionPointRec(GetMousePosition(), btn_rect) && !g_ctx_open && !g_ti_active && !g_drag_active;
        if (sq_hov) {
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
                    if (session_new_midi_clip(s, ch, scene, 4.0f, "MIDI") == 0)
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

    ui_text_scaled(shortname, (int)text_x, (int)(y + 5),
            14, play ? g_theme.text : g_theme.text_dim);

    if (atomic_load(&cl->is_midi)) {                    /* 1.3: a MIDI clip - a small picture of its notes */
        const MidiSeq *mm = atomic_load(&cl->mseq);
        char pb[40];
        float Lb = mm ? mm->length : 4.0f;
        double mp_ = play ? atomic_load(&cl->mpos_vis) : -1.0;
        if (mp_ >= 0.0) snprintf(pb, sizeof pb, "%d.%d / %g", (int)(mp_ / 4.0) + 1, (int)fmod(mp_, 4.0) + 1, (double)(Lb / 4.0f));
        else snprintf(pb, sizeof pb, "%g bar%s", (double)(Lb / 4.0f), Lb == 4.0f ? "" : "s");
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
    } else if (cl->wav.sample_rate > 0 && cl->wav.frame_count > 0) {
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

static void draw_scene_button(Session *s, int scene,
                              float x, float y, float w, float h)
{
    (void)s;
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
}

static void draw_scene_header(float x, float y, float w, float h)
{
    DrawRectangleRounded((Rectangle){ x, y, w, h }, R_CELL, g_theme.panel_bg);
    ui_text("Scenes", (int)(x + 6), (int)(y + 8), 13, g_theme.text);
}

static bool draw_add_scene_button(float x, float y, float w, float h)
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

static bool draw_loop_button(Session *s,
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

static bool draw_midi_button(float x, float y, float w, float h)
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

static void draw_stop_button(Session *s, float x, float y, float w, float h)
{
    DrawRectangleRounded((Rectangle){ x, y, w, h }, R_CELL, g_theme.stop_btn);
    DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_CELL, 1, g_theme.text_dim);   /* a frame like Loop/MIDI's */

    float sq = h * 0.32f;
    float cxx = x + w/2;
    float cyy = y + h/2;
    DrawRectangle((int)(cxx - sq/2), (int)(cyy - sq/2),
                  (int)sq, (int)sq,
                  (Color){ 245, 245, 245, 255 });

    Vector2 mouse = GetMousePosition();
    Rectangle r = { x, y, w, h };
    if (CheckCollisionPointRec(mouse, r) &&
        IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        !g_ctx_open && !g_ti_active && !g_audio_dlg_open) {
        session_stop_all(s);
    }
}
// ============ Channel header ============

// ============ Channel header ============

/* ===== 1.4: recording - the record button of a channel and the click on a cell ===== */
static bool fx_ui_free(void);
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
static void ui_rec_click(Session *s, int ch, int sc)
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
        ui_flash_message(midi ? "Recording MIDI - click the cell again to stop" : "Recording - click the cell again to stop");
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
static void draw_rec_input_menu(Session *s)
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

static bool fx_ui_free(void);
static void draw_channel_header(Session *s, int ch,

                                float x, float y, float w, float h)
{
    Channel *chp = &s->channels[ch];
    Color c = palette_raw(chp->color_id);

    DrawRectangleRounded((Rectangle){ x, y, w, h }, R_CELL, c);
    if (ch >= 0 && ch < MAX_CHANNELS && g_chsel[ch]) {   /* step 36: selected channel */
        Color tc = ui_text_color_on(c);
        DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_CELL, 3, g_theme.selection);
        DrawRectangleRounded((Rectangle){ x + 6, y + h - 6, w - 12, 2 }, 1, tc);
    }

    int name_x = (int)(x + 8);
    int name_w = (int)(w - 16);
    /* 1.4: the record button (right end of the header) */
    if (w > 60) {
        rec_arm_button(s, ch, (Rectangle){ x + w - 24, y + (h - 18) / 2, 18, 18 }, ui_text_color_on(c));
        name_w -= 22;
        w -= 22;                                   /* the keyboard icon goes left of it */
    }
    /* 1.2: an instrument channel - a small keyboard on the right (filled = the MIDI keyboard plays it; click = play it) */
    if (fxchain_has_instrument(s, ch) && w > 60) {
        bool armed = session_get_midi_target(s) == ch;
        Color tc = ui_text_color_on(c);
        Rectangle kb = { x + w - 26, y + (h - 14) / 2, 20, 14 };
        Vector2 mm = GetMousePosition();
        bool kh = CheckCollisionPointRec(mm, kb) && fx_ui_free();
        if (armed) DrawRectangleRec(kb, tc);
        else DrawRectangleLinesEx(kb, 1, kh ? tc : (Color){ tc.r, tc.g, tc.b, 150 });
        Color key = armed ? c : (Color){ tc.r, tc.g, tc.b, 150 };
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
        ui_text(nm, name_x, (int)(y + 8), 16, ui_text_color_on(c));
    }
}

// ============ Channel footer ============

/* ===== Where the audio goes (step 24): a button by the faders + a list of the outputs ===== */
static bool      g_rt_open = false, g_rt_just_opened = false;
static int       g_rt_target = 0;            /* a channel or -1 = master */
static Rectangle g_rt_btn, g_rt_rect;

static int rt_hw_outputs(void)
{
    int n = audio_get_current_config().output_channels;
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

#define FX_FAIL_COL ((Color){ 215, 95, 80, 255 })

/* step 35: the plugin window (double click or the menu) */
static void fx_open_window(Session *s, int ch, int k)
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

static double g_fx_click_t = -1;
static int    g_fx_click_ch = -1, g_fx_click_k = -1;

static bool fx_ui_free(void)
{
    return !g_ctx_open && !g_ti_active && !g_audio_dlg_open && g_fxp_ch < 0 && g_fxm_ch < 0 && g_rin_ch < 0;
}

/* ===== The FX strip of one channel: the slots (name, format, on/off) and "+" ===== */
static void draw_fx_strip(Session *s, int ch, float x, float y, float w, float h)
{
    Channel *chp = &s->channels[ch];
    Color col = channel_color(chp->color_id);
    Color bg = g_theme.panel_bg;
    bg.r = (unsigned char)(bg.r * 0.8f); bg.g = (unsigned char)(bg.g * 0.8f); bg.b = (unsigned char)(bg.b * 0.8f);
    DrawRectangleRounded((Rectangle){ x, y, w, h }, R_PANEL, bg);
    DrawRectangle((int)x, (int)(y + h - R_PANEL), (int)w, (int)R_PANEL, bg);   /* bottom - straight edge (next to the faders) */
    DrawLine((int)x, (int)(y + h - 1), (int)(x + w), (int)(y + h - 1), g_theme.grid_line);
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
static void draw_fx_picker(Session *s)
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
static void draw_rec_input_menu(Session *s);
static void draw_fx_slot_menu(Session *s)
{
    if (g_fxm_ch < 0) { g_fxm_rect = (Rectangle){ 0, 0, 0, 0 }; return; }
    if (g_fxm_ch >= s->channel_count || g_fxm_idx >= s->channels[g_fxm_ch].fx_count) { g_fxm_ch = -1; return; }
    const ChFx *fx = &s->channels[g_fxm_ch].fx[g_fxm_idx];
    QPlug *qp = fxchain_find(s, g_fxm_ch, g_fxm_idx);
    bool open = qplug_editor_is_open(qp);
    int nfx = s->channels[g_fxm_ch].fx_count;
    /* 1.2: the order can be changed (the first device of an instrument channel is the instrument) */
    const char *items[5] = { open ? "Close window" : "Open window", fx->bypass ? "Turn on" : "Turn off (bypass)",
                             "Move left (earlier)", "Move right (later)", "Remove" };
    bool en[5] = { qp && qplug_has_editor(qp), true, g_fxm_idx > 0, g_fxm_idx < nfx - 1, true };
    const int NI = 5;
    const float W = 190, RH = 24, H = NI * RH + 8;
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
            else session_fx_remove(s, g_fxm_ch, g_fxm_idx);
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

static void draw_channel_footer(Session *s, int ch,
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
    DrawRectangleRoundedLines(whole, R_PANEL, 1, mix_col(g_theme.panel_bg, g_theme.text_dim, 0.45f));

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
            if (pan_click_ch == ch && now - pan_click_t < DOUBLE_CLICK_TIME) {   /* step 36: double click = centre */
                session_set_channel_pan(s, ch, 0.0f);
                pan_click_ch = -1;
            } else {
                g_dragging_slider = 2000 + ch;
                pan_click_ch = ch;
                pan_click_t = now;
            }
        }
        if (g_dragging_slider == 2000 + ch &&
            IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            float rel = (knob_y + knob_d/2) - mouse.y;
            float p = rel / (knob_d * 2.0f);
            if (p < -1.0f) p = -1.0f;
            if (p >  1.0f) p =  1.0f;
            if (fabsf(p) < 0.03f) p = 0.0f;                  /* the centre is easy to hit (it is exactly 0 dB there) */
            session_set_channel_pan(s, ch, p);
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

static void draw_midi_badge(Session *s, int ch, float x, float y)
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

static void draw_master(Session *s, float x, float y, float w, float h)
{
    Color col = g_theme.text;

    DrawRectangleRounded((Rectangle){ x, y, w, h }, R_PANEL, g_theme.panel_bg);

    if (g_midi_learn_mode) {
        bool armed = atomic_load(&s->master_midi_learn_armed);
        if (armed) {
            DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_PANEL, 3, g_theme.mute);
        }
    }

    DrawRectangleRounded((Rectangle){ x, y, w, CLIP_HEADER_H }, R_PANEL, col);
    DrawRectangle((int)x, (int)(y + CLIP_HEADER_H / 2), (int)w, CLIP_HEADER_H - CLIP_HEADER_H / 2, col);   /* bottom - straight edge */
    DrawRectangleRoundedLines((Rectangle){ x, y, w, h }, R_PANEL, 1, mix_col(g_theme.panel_bg, g_theme.text_dim, 0.45f));   /* step 36g */
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
            if (now - mpan_last < DOUBLE_CLICK_TIME) { atomic_store(&s->master_pan, 0.0f); mpan_last = 0; }   /* double click = centre */
            else { g_dragging_slider = MPAN_ID; mpan_last = now; }
        }
        if (g_dragging_slider == MPAN_ID && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            float p = ((ky + knob_d / 2) - mm.y) / (knob_d * 2.0f);
            if (p < -1.0f) p = -1.0f;
            if (p >  1.0f) p =  1.0f;
            if (fabsf(p) < 0.03f) p = 0.0f;                 /* the centre is easy to hit */
            atomic_store(&s->master_pan, p);
        }
        /* main MUTE */
        Rectangle mr = { cx0 + 4, ky + knob_d + 20, cw - 8, 26 };
        DrawRectangleRounded(mr, R_CELL, mmuted ? g_theme.mute : g_theme.panel_bg2);
        DrawRectangleRoundedLines(mr, R_CELL, 1, g_theme.text_dim);
        int mw0 = ui_text_w("MUTE", 12);
        ui_text("MUTE", (int)(mr.x + (mr.width - mw0) / 2), (int)(mr.y + 7), 12, mmuted ? RAYWHITE : g_theme.text_dim);
        if (!g_midi_learn_mode && CheckCollisionPointRec(mm, mr) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            !g_ctx_open && !g_ti_active && !g_audio_dlg_open)
            atomic_store(&s->master_muted, !mmuted);
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

// ============ Library ============

static void draw_library(FileBrowser *fb, float x, float y, float w, float h)
{
    if (!fb || !fb->visible) return;

    DrawRectangle((int)x, (int)y, (int)w, (int)h, g_theme.panel_bg2);
    DrawRectangle((int)x, (int)y, (int)w, CLIP_HEADER_H, g_theme.panel_bg);

    const char *path = fb->current_dir;
    char display[64];
    if (strlen(path) > 42) {
        snprintf(display, sizeof(display), "...%s", path + strlen(path) - 38);
    } else {
        snprintf(display, sizeof(display), "%s", path);
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
bool ui_get_scrollbar_rect(int vertical, int *x, int *y, int *w, int *h)
{
    Rectangle r = vertical ? g_sb_rect_v : g_sb_rect_h;
    if (!(vertical ? g_sb_have_v : g_sb_have_h) || !x || !y || !w || !h) return false;
    *x = (int)r.x; *y = (int)r.y; *w = (int)r.width; *h = (int)r.height;
    return true;
}

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

// ============ Waveform ============
// ============ Waveform ============

// Where the waveform is on screen (for tests): valid after a clip has been drawn
static struct { float x, ruler_y, w, wave_y, wave_h; bool valid; double view0, vis; int ch, sc; } g_wave_geo;
bool ui_get_wave_geometry(float *x, float *ruler_y, float *w, float *wave_y, float *wave_h)
{
    if (!g_wave_geo.valid) return false;
    if (x) *x = g_wave_geo.x;
    if (ruler_y) *ruler_y = g_wave_geo.ruler_y;
    if (w) *w = g_wave_geo.w;
    if (wave_y) *wave_y = g_wave_geo.wave_y;
    if (wave_h) *wave_h = g_wave_geo.wave_h;
    return true;
}

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
 *   the number = the clip's own tempo (detected; drag / double click = correction)
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
    bool free_ui = !g_ti_active && !g_ctx_open && !g_file_menu_open && !g_edit_menu_open && !g_audio_dlg_open;

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
    else if (res == 2) {
        Rectangle er = { vr.x, py - 1, vr.width > 100 ? vr.width : 100, ph + 2 };
        num_edit_begin(2, ch, sc, cur, er);
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
static void wspan(int cx, float yt, float yb, Color c)
{
    if (c.a == 0 || yb <= yt) return;
    int it = (int)ceilf(yt), ib = (int)floorf(yb);
    if (ib < it) {                                   /* all of it is in one pixel */
        Color e = c; e.a = (unsigned char)(c.a * (yb - yt));
        if (e.a) DrawRectangle(cx, ib, 1, 1, e);
        return;
    }
    if (ib > it) DrawRectangle(cx, it, 1, ib - it, c);
    Color e = c;
    e.a = (unsigned char)(c.a * ((float)it - yt));
    if (e.a) DrawRectangle(cx, it - 1, 1, 1, e);
    e.a = (unsigned char)(c.a * (yb - (float)ib));
    if (e.a) DrawRectangle(cx, ib, 1, 1, e);
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
enum { EM_NONE = 0, EM_PART };
static int     g_em_kind = EM_NONE;
static int     g_em_part = -1;
static bool    g_em_just = false;
static Vector2 g_em_pos;
static Rectangle g_em_rect;

static void ed_forget(void)
{
    g_part = -1;
    g_drag = DR_NONE;
    g_em_kind = EM_NONE;
    edit_free(g_preview); g_preview = NULL;
}

bool ui_edit_menu_open(void) { return g_em_kind != EM_NONE; }

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
static bool me_take_delete(Session *s);
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
    int64_t t = beat > 0 ? (int64_t)llround(floor((f - ga) / (beat * 4.0)) * beat * 4.0 + ga) : (int64_t)llround(f);
    EditList *nl = edit_paste(el, g_seg_clip, t);
    if (!nl) { ui_flash_message("Cannot paste there"); return true; }
    g_ed_ch = ch; g_ed_sc = sc;
    ed_apply(s, nl, beat > 0 ? "Pasted (at the start of the bar)" : "Pasted");
    g_part = -1;
    return true;
}

/* Ctrl+C / Ctrl+X: a part selected in the waveform -> copies it (not the clip) */
static bool me_take_copy(Session *s, bool cut);
static bool me_take_paste(Session *s);
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
static void icon_pointer(float cx, float cy, Color c)
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

#define EDIT_TOOLS_W (2 * 26 + 4)
static const char *g_tool_tip;
static float g_tool_tip_x, g_tool_tip_y;
/* two buttons in the waveform's header row (to the left of BPM) */
static void draw_edit_tools(float bx, float by, float bh)
{
    static const char *tips[2] = {
        "Pointer: click = play position | drag a part = move (Ctrl = copy) | drag its edges = trim | top corners = fades | Del = delete part",
        "Scissors: click to cut exactly there (hold Shift to snap to beats)" };
    Vector2 m = GetMousePosition();
    bool free_ui = !g_ti_active && !g_ctx_open && !g_file_menu_open && !g_edit_menu_open && !g_audio_dlg_open;
    if (free_ui && g_em_kind == EM_NONE && !g_rt_open && IsKeyPressed(KEY_ESCAPE) && g_tool != TOOL_POINTER)
        g_tool = TOOL_POINTER;                              /* Esc: back to the arrow */
    int hov_i = -1;
    for (int i = 0; i < 2; ++i) {
        Rectangle r = { bx + i * 26, by + 2, 24, bh - 4 };
        bool hov = free_ui && CheckCollisionPointRec(m, r);
        bool on = g_tool == i;
        DrawRectangleRec(r, on ? g_theme.selection : (hov ? g_theme.cell_hover : g_theme.panel_bg2));
        Color ic = on ? g_theme.bg : g_theme.text;
        float cx = r.x + r.width / 2, cy = r.y + r.height / 2;
        if (i == 0) icon_pointer(cx, cy, ic); else icon_scissors(cx, cy, ic);
        if (hov) {
            hov_i = i;
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { g_tool = i; g_em_kind = EM_NONE; }
        }
    }
    g_tool_tip = hov_i >= 0 ? tips[hov_i] : NULL;          /* drawn last (above the ruler) */
    g_tool_tip_x = bx + 2 * 26; g_tool_tip_y = by + bh + 2;
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
static void draw_edit_menu(Session *s)
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
static bool    g_me_focus = false;
static Rectangle g_me_rect;
static int     g_md = MD_NONE;
static Vector2 g_md_p0;
static MidiSeq *g_md_orig = NULL;
static int     g_md_note = -1, g_md_lastdk = 0;
static double  g_me_click_t = 0;
static Vector2 g_me_click_p;
static float   g_me_lastlen = 0.25f;
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

static void me_fit(Session *s, float gw, float gh)
{
    const MidiSeq *m = session_midi_get(s, g_me_ch, g_me_sc);
    float L = m ? m->length : 4.0f;
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
static bool me_take_delete(Session *s)
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
static bool me_take_copy(Session *s, bool cut)
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
static bool me_take_paste(Session *s)
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

static void draw_midi_editor(Session *s, int ch, int sc, float x, float y, float w, float h)
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
    char gl[32]; snprintf(gl, sizeof gl, "Grid %s", ME_GRID_NAMES[g_me_grid]);
    Rectangle gr = { hx, y + 2, (float)ui_text_w("Grid 1/32", 12) + 16, WAVE_HDR_H - 4 };
    if (me_hdr_button(gr, gl, false, true)) g_me_grid = (g_me_grid + 1) % ME_NGRID;
    if (uf && CheckCollisionPointRec(mp, gr) && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) g_me_grid = (g_me_grid + ME_NGRID - 1) % ME_NGRID;
    hx += gr.width + 10;
    char ll[48];
    if (fmodf(L, 4.0f) < 1e-3f) snprintf(ll, sizeof ll, "%d bar%s", (int)(L / 4.0f), (int)(L / 4.0f) == 1 ? "" : "s");
    else snprintf(ll, sizeof ll, "%g beats", (double)L);
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
        const char *hint = "Double click: add / delete   Ctrl+drag: copy   Ruler: drag = zoom/move, double click = play";
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
            if (ctrl && CheckCollisionPointRec(mp, keys_r)) {
                g_me_kh *= wh > 0 ? 1.15f : 1.0f / 1.15f;
                if (g_me_kh < 5) g_me_kh = 5;
                if (g_me_kh > 30) g_me_kh = 30;
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
    for (double b = floor(g_me_b0 / 8.0) * 8.0 + 4.0; BX(b) < gx + gw; b += 8.0) {
        if (b < 0) continue;
        float x0 = BX(b), x1 = BX(b + 4.0);
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
        bool bar = fmod(b + 1e-6, 4.0) < 2e-6, beat = fmod(b + 1e-6, 1.0) < 2e-6;
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
    float bstep = 0.25f;
    while (bstep * g_me_ppb < (bstep < 4.0f ? 40.0f : 34.0f)) bstep *= 2.0f;
    for (double b = floor(g_me_b0 / bstep) * bstep; BX(b) < gx + gw; b += bstep) {
        if (b < 0) continue;
        long q16 = lround(b * 4.0);
        int bar = (int)(q16 / 16) + 1, bt = (int)(q16 / 4 % 4) + 1, sx = (int)(q16 % 4) + 1;
        char bn[24];
        if (q16 % 16 == 0) snprintf(bn, sizeof bn, "%d", bar);
        else if (q16 % 4 == 0) snprintf(bn, sizeof bn, "%d.%d", bar, bt);
        else snprintf(bn, sizeof bn, "%d.%d.%d", bar, bt, sx);
        bool major = q16 % 16 == 0;
        DrawLine((int)BX(b), (int)by + (major ? 8 : 11), (int)BX(b), (int)by + ME_RH, g_theme.text_dim);
        ui_text(bn, (int)BX(b) + 3, (int)by + 2, 10, major ? g_theme.text : g_theme.text_dim);
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
        } else if (hover_note >= 0) {                        /* grab a note */
            bool was = m->note[hover_note].sel != 0;
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
static void computer_keys(Session *s)
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

static int device_panel_channel(Session *s)
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
    (void)s;
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
    if (hov || drag) DrawRectangleRounded(cell, R_SMALL, g_theme.cell_hover);
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

static void draw_device_panel(Session *s, int ch, float x, float y, float w, float h)
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
            g_fxm_ch = ch; g_fxm_idx = k; g_fxm_pos = m; g_fxm_just = true;
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

static void draw_waveform_zone(Session *s,
                               float x, float y, float w, float h)
{
    if (h < WAVEFORM_MIN_H) return;
    {
        int dch = device_panel_channel(s);                 /* 1.2: a selected channel -> its devices */
        if (dch >= 0) { draw_device_panel(s, dch, x, y, w, h); return; }
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

        DrawRectangle((int)x, (int)(y + WAVE_HDR_H),
                      (int)w, (int)(h - WAVE_HDR_H), g_theme.note_bg);

        int tw = ui_text_w(cl->name, 20);
        ui_text(cl->name,
                (int)(x + (w - tw) / 2),
                (int)(y + WAVE_HDR_H + (h - WAVE_HDR_H) / 2 - 10),
                20, g_theme.note_text);
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
    int64_t scroll_frame = (int64_t)atomic_load(&cl->wave_scroll_frame);
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
    }

    const char *state = atomic_load(&cl->playing) ? "PLAY"
                      : (atomic_load(&cl->paused) ? "PAUSE" : "STOP");
    const char *loop_str = loop_on ? "  [LOOP]" : "";
    char hdr[256];
    if (g_wave_follow) {
        snprintf(hdr, sizeof(hdr), "%s  |  %s%s  |  Ch %d / S%d  |  scroll: %.0f s",
                 cl->name, state, loop_str,
                 g_selected_ch + 1, g_selected_scene + 1, g_follow_sec);
    } else if (zoom > 1.01f) {
        snprintf(hdr, sizeof(hdr), zoom >= 100.0f ? "%s  |  %s%s  |  Ch %d / S%d  |  zoom: %.0fx"
                                                  : "%s  |  %s%s  |  Ch %d / S%d  |  zoom: %.1fx",
                 cl->name, state, loop_str,
                 g_selected_ch + 1, g_selected_scene + 1, zoom);
    } else {
        snprintf(hdr, sizeof(hdr), "%s  |  %s%s  |  Ch %d / S%d",
                 cl->name, state, loop_str,
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
        float text_w = tools_x - x - 14;
        BeginScissorMode((int)x, (int)y, (int)text_w + 10, WAVE_HDR_H);
        ui_text(hdr, (int)(x + 10), (int)(y + 4), 14, g_theme.text);
        EndScissorMode();
        if (has_panel)
            draw_clip_bpm_panel(s, g_selected_ch, g_selected_scene, panel_x, y, MASTER_W, WAVE_HDR_H);
        if (g_ed_ch != g_selected_ch || g_ed_sc != g_selected_scene) {   /* another clip -> the selection does not apply */
            ed_forget();
            g_ed_ch = g_selected_ch; g_ed_sc = g_selected_scene;
        }
        draw_edit_tools(tools_x, y, WAVE_HDR_H);
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
        double bar = bar_beat * 4.0, fppx = (double)visible_frames / (double)w;
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
    double fpp = (double)visible_frames / (double)ncol;             /* frames per pixel */

    /* lanes: mono = one; stereo = L on top, R below */
    float lane_h = stereo ? (wave_h - 4) * 0.5f : wave_h;
    float lane_y[2] = { wave_y, wave_y + lane_h + 4 };
    int   nlanes = stereo ? 2 : 1;
    for (int ln = 0; ln < nlanes; ++ln) {
        float my = lane_y[ln] + lane_h * 0.5f;
        DrawLine((int)x, (int)my, (int)(x + w), (int)my, g_theme.grid_line);
    }
    if (stereo) {
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
    bool subpix = !detail_mode && ncol + 2 <= WCOL_MAX && off_px > 0.001f;
    if (subpix) {
        for (int i = 0; i <= ncol; ++i) {
            double f0 = (q0 + i) * fpp, f1 = f0 + fpp;
            wok[i] = false;
            if (!cl->peaks) continue;
            int64_t st = (int64_t)floor(f0), en = (int64_t)floor(f1);   /* may also be outside the clip (step 36j) */
            if (en <= st) en = st + 1;
            wok[i] = session_clip_peaks_list(cl, draw_edits, st, en, &wcol[i]);
        }
    }

    BeginScissorMode((int)x, (int)wave_y, (int)w, (int)wave_h);
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
            double f0 = (q0 + i) * fpp, f1 = f0 + fpp;
            int64_t st = (int64_t)floor(f0), en = (int64_t)floor(f1);   /* may also be outside the clip (step 36j) */
            if (en <= st) en = st + 1;
            if (!session_clip_peaks_list(cl, draw_edits, st, en, &v)) continue;
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
            if (ib > it) DrawRectangle(cx, it, 1, ib - it, c_out);
            Color e = c_out;
            e.a = (unsigned char)(c_out.a * ((float)it - yt));
            if (e.a) DrawRectangle(cx, it - 1, 1, 1, e);
            e.a = (unsigned char)(c_out.a * (yb - (float)ib));
            if (e.a) DrawRectangle(cx, ib, 1, 1, e);
            /* core: +-RMS around the middle, but not outside the outline */
            float rt = my - rms * amp, rb = my + rms * amp;
            if (rt < yt) rt = yt;
            if (rb > yb) rb = yb;
            if (rb - rt >= 1.0f) DrawRectangle(cx, (int)(rt + 0.5f), 1, (int)(rb - rt + 0.5f), c_rms);
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
    if (stereo) {
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
            bool bar = grid_beat > 0 && fabs(fmod(f - grid_anchor, grid_beat * 4.0)) < 0.5;
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
                    if (gb > 0 && !ed_shift()) g_end_ext = (int64_t)llround(ga + floor((nfe - ga) / (gb * 4.0) + 0.5) * gb * 4.0);
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
    draw_tool_tip();
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
        if (g_ti_numeric) {                    /* only digits and one dot (a comma becomes a dot) */
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
static void num_edit_begin(int kind, int ch, int sc, float value, Rectangle r)
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

    int mx = g_ctx_x;
    int my = g_ctx_y;

    if (mx + CTX_MENU_W > screen_w) mx = screen_w - CTX_MENU_W - 4;
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
    DrawRectangle(mx, my, CTX_MENU_W, menu_h, bg);

    Color border = g_theme.text_dim;
    border.a = (unsigned char)(border.a * alpha);
    DrawRectangleLines(mx, my, CTX_MENU_W, menu_h, border);

    Vector2 mouse = GetMousePosition();
    bool mouse_inside_menu = (mouse.x >= mx && mouse.x <= mx + CTX_MENU_W &&
                              mouse.y >= my && mouse.y <= my + menu_h);

    int cur_y = my + CTX_MENU_PAD;
    int clicked_index = -1;

    for (int i = 0; i < n; ++i) {
        CtxItem *it = &items[i];

        if (it->is_separator) {
            Color sep = g_theme.text_dim;
            sep.a = (unsigned char)(sep.a * 0.5f * alpha);
            DrawLine(mx + 6, cur_y + 2, mx + CTX_MENU_W - 6, cur_y + 2, sep);
            cur_y += 5;
            continue;
        }

        Rectangle row = { (float)mx, (float)cur_y,
                          (float)CTX_MENU_W, (float)CTX_MENU_ROW_H };

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
            if (s && session_new_midi_clip(s, g_ctx_ch, g_ctx_scene, 4.0f, "MIDI") == 0) {
                g_selected_ch = g_ctx_ch; g_selected_scene = g_ctx_scene;
                g_ch_focus = false; chsel_clear();
                ui_flash_message("New MIDI clip - double click in the editor below to add notes");
            }
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

typedef struct {
    int  open_dropdown_id;
    int  hovered_item;
    double open_time;
} DropdownState;

static DropdownState g_dd = { -1, -1, 0.0 };

static bool draw_dropdown_button(int id,
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

static int draw_dropdown_list(int id,
                              const char **items, int count, int current,
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

void ui_close_audio_settings(void)
{
    g_audio_dlg_open = false;
    g_dd.open_dropdown_id = -1;
}

bool ui_audio_settings_is_open(void)
{
    return g_audio_dlg_open;
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


static void draw_audio_settings_dialog(int screen_w, int screen_h)
{
    if (!g_audio_dlg_open) return;

    GfxDialogBackdrop(70);

    const int dlg_w = 640;
    const int dlg_h = 545;
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

    {
        // Checkbox: converting files with a different rate (default OFF = they play as they are)
        int y = row_y_start + row_h * 6;
        Rectangle box = { (float)label_x, (float)(y + 4), 20.0f, 20.0f };
        Rectangle hit = { (float)label_x, (float)y, (float)(dlg_w - 40), 38.0f };
        bool hov = CheckCollisionPointRec(mouse, hit);

        DrawRectangleRec(box, g_theme.panel_bg2);
        DrawRectangleLinesEx(box, 1, hov ? g_theme.selection : g_theme.text_dim);
        if (g_audio_dlg_config.convert_files) {
            DrawRectangle((int)box.x + 4, (int)box.y + 4, 12, 12, g_theme.selection);
        }
        ui_text("Convert files with a different sample rate",
                label_x + 30, y + 5, 15, g_theme.text);
        ui_text(g_audio_dlg_config.convert_files
                    ? "ON: converted copy goes to ~/.cache/qlim (new files only)"
                    : "OFF: files play as they are (live resampling, nothing written)",
                label_x + 30, y + 24, 12, g_theme.text_dim);

        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            g_dd.open_dropdown_id == -1) {
            g_audio_dlg_config.convert_files = g_audio_dlg_config.convert_files ? 0 : 1;
        }
    }

#ifndef _WIN32                  /* no PipeWire on Windows */
    {
        // Checkbox (step 28): the channels and the master - separate nodes in the PipeWire graph (PipeWire native only)
        int y = row_y_start + row_h * 7;
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
        int y = row_y_start + row_h * 8 + 6;
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
            (g_audio_dlg_config.convert_files != g_audio_dlg_original.convert_files) ||
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
}

void ui_close_appearance(void)
{
    g_appearance_open = false;
}

bool ui_appearance_is_open(void)
{
    return g_appearance_open;
}

/* ============ About ============ */
static void draw_about_dialog(int screen_w, int screen_h)
{
    GfxDialogBackdrop(70);
    const int w = 560, h = 470;
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
    };
    for (int i = 0; i < (int)(sizeof libs / sizeof libs[0]); ++i)
        ui_text(libs[i], x + 36, ty + 22 + i * 18, 12, g_theme.text_dim);

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

/* ============ Edit -> Plugins... (step 34) ============ */
static int   g_pl_tab = 0;                 /* 0 = available, 1 = problems, 2 = search locations */
static float g_pl_scroll[3];

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

static void draw_plugins_dialog(int screen_w, int screen_h)
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
    const char *tabs[3] = { t0, t1, "Scan locations" };
    float tx = (float)x + 16;
    for (int i = 0; i < 3; ++i) {
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

static void draw_appearance_dialog(int screen_w, int screen_h)
{
    if (!g_appearance_open) return;

    GfxDialogBackdrop(70);

    const int dlg_w = 700;
    const int dlg_h = 300;                  /* +40: the themes are on two rows */
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
        "Dark", "Light", "Midnight", "Solarized", "OLED Black", "Nord", "Gruvbox"
    };
    const char *theme_ids[] = {
        "dark", "light", "midnight", "solarized", "oled", "nord", "gruvbox"
    };
    int n_themes = 7;

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

    y += 2 * btn_h + 6 + 12;

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

static bool   g_midi_settings_open = false;
static double g_midi_settings_open_time = 0.0;
static int    g_midi_settings_selected_port = -1;

void ui_open_midi_settings(void)
{
    g_midi_settings_open = true;
    g_midi_settings_open_time = GetTime();
    g_midi_settings_selected_port = midi_get_current_port_index();
}

void ui_close_midi_settings(void)
{
    g_midi_settings_open = false;
}

bool ui_midi_settings_is_open(void)
{
    return g_midi_settings_open;
}

static void draw_midi_settings_dialog(int screen_w, int screen_h)
{
    if (!g_midi_settings_open) return;

    GfxDialogBackdrop(70);

    const int dlg_w = 600;
    const int dlg_h = 500;
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

    int cur_idx = midi_get_current_port_index();
    const char *cur_name = midi_get_current_port_name();

    char cur_buf[512];
    if (midi_is_open()) {
        snprintf(cur_buf, sizeof(cur_buf), "Connected: [%d] %s",
                 cur_idx, cur_name);
    } else {
        snprintf(cur_buf, sizeof(cur_buf), "Connected: (none)");
    }
    ui_text(cur_buf, dlg_x + 16, dlg_y + 50, 13,
            midi_is_open() ? g_theme.vu_green : g_theme.text_dim);

    int port_count = midi_get_port_count();

    const int list_y = dlg_y + 80;
    const int list_h = 320;
    const int row_h = 28;

    DrawRectangle(dlg_x + 16, list_y, dlg_w - 32, list_h, g_theme.panel_bg2);
    DrawRectangleLines(dlg_x + 16, list_y, dlg_w - 32, list_h, g_theme.text_dim);

    Vector2 mouse = GetMousePosition();

    if (port_count == 0) {
        ui_text("No MIDI ports available",
                dlg_x + 30, list_y + 20, 14, g_theme.text_dim);
    } else {
        int visible_rows = list_h / row_h;

        for (int i = 0; i < port_count && i < visible_rows; ++i) {
            Rectangle row = { (float)(dlg_x + 16),
                              (float)(list_y + i * row_h),
                              (float)(dlg_w - 32),
                              (float)row_h };

            bool hov = CheckCollisionPointRec(mouse, row);
            bool is_selected = (i == g_midi_settings_selected_port);
            bool is_current = (i == cur_idx && midi_is_open());

            if (is_selected) {
                Color hc = g_theme.selection;
                hc.a = 100;
                DrawRectangleRec(row_hl(row), hc);
            } else if (hov) {
                Color hc = g_theme.selection;
                hc.a = 50;
                DrawRectangleRec(row_hl(row), hc);
            }

            Color txt_col = is_current ? g_theme.vu_green : g_theme.text;
            if (is_selected) txt_col = g_theme.selection;

            char name_copy[256];
            snprintf(name_copy, sizeof(name_copy), "%s",
                     midi_get_port_name(i));

            char port_buf[600];
            snprintf(port_buf, sizeof(port_buf), "[%d] %s%s",
                     i, name_copy,
                     is_current ? "  (connected)" : "");

            ui_text(port_buf, dlg_x + 26, list_y + i * row_h + 6,
                    14, txt_col);

            if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                g_midi_settings_selected_port = i;
            }
        }
    }

    const int btn_w = 120;
    const int btn_h = 36;
    const int btn_y = dlg_y + dlg_h - 56;

    Rectangle connect_r   = { dlg_x + 16, btn_y, btn_w, btn_h };
    Rectangle disconnect_r = { dlg_x + 16 + btn_w + 8, btn_y, btn_w, btn_h };
    Rectangle refresh_r   = { dlg_x + dlg_w - 2 * btn_w - 30, btn_y, btn_w, btn_h };
    Rectangle close_r     = { dlg_x + dlg_w - btn_w - 20, btn_y, btn_w, btn_h };

    {
        bool hov = CheckCollisionPointRec(mouse, connect_r);
        bool can_connect = (g_midi_settings_selected_port >= 0 &&
                            g_midi_settings_selected_port < port_count);

        Color b = hov && can_connect ? g_theme.selection : g_theme.panel_bg2;
        DrawRectangleRec(connect_r, b);
        DrawRectangleLinesEx(connect_r, 1, g_theme.text_dim);

        Color tc = (hov && can_connect) ? g_theme.bg :
                   (can_connect ? g_theme.text : g_theme.text_dim);
        int tw = ui_text_w("Connect", 15);
        ui_text("Connect", (int)(connect_r.x + (btn_w - tw) / 2),
                (int)(connect_r.y + (btn_h - 15) / 2), 15, tc);

        if (hov && can_connect && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (midi_open_port(g_midi_settings_selected_port) == 0) {
                midi_save_config();
                ui_flash_message("MIDI connected");
            } else {
                ui_flash_message("MIDI connect failed");
            }
        }
    }

    {
        bool hov = CheckCollisionPointRec(mouse, disconnect_r);
        bool can_disconnect = midi_is_open();

        Color b = hov && can_disconnect ? g_theme.mute : g_theme.panel_bg2;
        DrawRectangleRec(disconnect_r, b);
        DrawRectangleLinesEx(disconnect_r, 1, g_theme.text_dim);

        Color tc = (hov && can_disconnect) ? RAYWHITE :
                   (can_disconnect ? g_theme.text : g_theme.text_dim);
        int tw = ui_text_w("Disconnect", 15);
        ui_text("Disconnect", (int)(disconnect_r.x + (btn_w - tw) / 2),
                (int)(disconnect_r.y + (btn_h - 15) / 2), 15, tc);

        if (hov && can_disconnect && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            midi_close_port();
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
            midi_shutdown();
            midi_init();
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
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_midi_settings_open = false;
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        g_midi_settings_open = false;
    }
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

    if (g_log_open && CheckCollisionPointRec(GetMousePosition(), g_log_rect))
        GfxMouseShield(true);

    /* File/Edit drop-down menu: a click on it must not select/play the cell below */
    if ((g_file_menu_open || g_edit_menu_open) && CheckCollisionPointRec(GetMousePosition(), g_menu_dd_rect))
        GfxMouseShield(true);

    /* While the queue is open, the elements below it do not see the mouse */
    if (g_rt_open && CheckCollisionPointRec(GetMousePosition(), g_rt_rect))
        GfxMouseShield(true);
    if (g_em_kind != EM_NONE && CheckCollisionPointRec(GetMousePosition(), g_em_rect))
        GfxMouseShield(true);
    if ((g_fxp_ch >= 0 && CheckCollisionPointRec(GetMousePosition(), g_fxp_rect)) ||
        (g_fxm_ch >= 0 && CheckCollisionPointRec(GetMousePosition(), g_fxm_rect)))
        GfxMouseShield(true);
    if (g_queue_open && (CheckCollisionPointRec(GetMousePosition(), g_q_rect) || g_q_drag >= 0 || g_vf_id == 3))
        GfxMouseShield(true);

    /* While the DSP popup is open, the elements below it do not see the mouse when it is clicked/hovered over it */
    if (g_dsp_open) {
        Vector2 mm = GetMousePosition();
        if (CheckCollisionPointRec(mm, dsp_popup_rect(screen_w)) ||
            IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
            GfxMouseShield(true);
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
    float master_x = screen_w - g_master_w - GAP;
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
        if (wheel != 0.0f) {
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
        bool popups = g_ti_active || g_ctx_open || g_file_menu_open || g_edit_menu_open || g_audio_dlg_open ||
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

    draw_menu_bar(s, screen_w);
    draw_top_bar(screen_w);

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

            {   /* the mouse is outside the visible area (over a scrollbar/panel) -> the cell does not react */
                bool saved_ctx = g_ctx_open;
                if (!mouse_in_grid) g_ctx_open = true;
                draw_clip_cell(s, c, i, cx, cy, cw, row_h - 2);
                g_ctx_open = saved_ctx;
            }
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
    if (mouse.x < channels_x || mouse.x >= hdr_right) GfxMouseShield(true);    /* the hidden part does not react to the mouse */
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

    draw_master(s, master_x, grid_top, g_master_w,
                grid_bottom - grid_top);

    /* the left edge of the master: it is dragged (wider = balance and MUTE); double click = the usual width */
    {
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
        Color ghost_bg = is_note_drag ? g_theme.note_bg : g_theme.selection;
        ghost_bg.a = 220;
        Color ghost_text = is_note_drag ? g_theme.note_text : g_theme.bg;

        char disp[NAME_LEN];
        snprintf(disp, sizeof(disp), "%s", src->name);
        while (ui_text_w(disp, 14) > 180 && strlen(disp) > 3) {
            utf8_chop_last(disp);
        }

        DrawRectangle((int)m.x + 8, (int)m.y + 8, 200, 24, ghost_bg);
        DrawRectangleLines((int)m.x + 8, (int)m.y + 8, 200, 24, g_theme.text);
        ui_text(disp, (int)m.x + 16, (int)m.y + 12, 14, ghost_text);
    }

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
    draw_dsp_popup(screen_w);
    draw_log_popup(screen_w);

    if (g_ctx_open) {
        ctx_menu_draw(s, screen_w, screen_h);
    }

    if (g_file_menu_open) {
        draw_file_dropdown(screen_w, screen_h);
    } else if (g_edit_menu_open) {
        draw_edit_dropdown(screen_w, screen_h);
    }
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
    if (out_sel_ch)    *out_sel_ch = g_selected_ch;
    if (out_sel_scene) *out_sel_scene = g_selected_scene;

    int screen_w = GetScreenWidth();
    int screen_h = GetScreenHeight();

    ClearBackground(g_theme.bg);

    if (g_ti_active && !g_ti_inline) {
        ui_text_input_update();
        ClearBackground(g_theme.bg);
        ui_draw_backdrop_world(s);
        GfxRoundAll(R_CELL);
        ui_text_input_draw(screen_w, screen_h);
        GfxRoundAll(0.0f);
        return;
    }

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
        dialogs_draw(s, screen_w, screen_h);
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
        draw_audio_settings_dialog(screen_w, screen_h);
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
        draw_about_dialog(screen_w, screen_h);
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
        draw_plugins_dialog(screen_w, screen_h);
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
        draw_appearance_dialog(screen_w, screen_h);
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
        draw_midi_settings_dialog(screen_w, screen_h);
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
    return g_ti_active || g_audio_dlg_open || g_appearance_open || g_about_open || g_plugins_open || g_midi_settings_open || dialogs_any_open() ||
           g_fxp_ch >= 0;                       /* the plugin list: typing is search, not keyboard shortcuts */
}
