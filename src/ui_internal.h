/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef QLIM_UI_INTERNAL_H
#define QLIM_UI_INTERNAL_H
/*
 * 2.1: what the parts of the main window share with each other (ui.c, ui_topbar.c, ui_channels.c, ui_wave.c,
 * ui_device.c, ui_midi.c, ui_settings.c). Not for the rest of the program - that uses ui.h.
 */
#include "ui.h"
#include "theme.h"
#include "sdlgfx.h"
#include <stdbool.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define MENU_BAR_H_BASE  28          /* the menu bar is never zoomed (1.3.10) ... */
#define MENU_BAR_H       (MENU_BAR_H_BASE / GfxGetZoom())   /* ... so the rest of the window starts here */
#define TOP_BAR_H_BASE   44          /* 1.3.12: the top bar (DSP, logo, BPM) is never zoomed either ... */
#define TOP_BAR_H        (TOP_BAR_H_BASE / GfxGetZoom())     /* ... so the zoomed rest makes room for it with this */
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
#define R_CELL           (g_ui_style ? 7.0f : 4.0f)    /* cells, headers, buttons (2.0.0: rounder in Modern) */
#define R_SMALL          (g_ui_style ? 5.0f : 3.0f)    /* faders, small fields */
#define R_PANEL          (g_ui_style ? 10.0f : 6.0f)   /* panels (faders, Master) */
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
#define CTX_MENU_W      160
#define CTX_MENU_ROW_H  24
#define CTX_MENU_PAD    4
#define DOUBLE_CLICK_TIME 0.35
#define DOUBLE_CLICK_DIST 8.0f
#define FX_FAIL_COL ((Color){ 215, 95, 80, 255 })
#define FX_STRIP_H   96
#define FX_ROW_H     18
#define MASTER_W_WIDE  250        /* from this width up: the extra column */
#define FOLLOW_SEC_MAX 60.0f
/* (defines end) */

enum { EM_NONE = 0, EM_PART };     /* the waveform's menu on a selected part (ui_wave.c) */

typedef struct {
    int  open_dropdown_id;
    int  hovered_item;
    double open_time;
} DropdownState;

/* --- shared state and helpers --- */
extern bool g_about_open;
extern Theme g_appearance_backup;
extern int   g_appearance_bk_style, g_appearance_bk_time;
extern int g_appearance_bk_font;
extern bool g_appearance_open;
extern double g_appearance_open_time;
extern bool g_audio_dlg_apply_flag;
extern AudioConfig g_audio_dlg_config;
extern bool g_audio_dlg_open;
extern double g_audio_dlg_open_time;
extern AudioConfig g_audio_dlg_original;
extern bool g_can_undo;
extern int g_cell_last_ch;
extern double g_cell_last_t;
extern bool g_ch_focus;
extern bool g_chsel[MAX_CHANNELS];
extern int g_chsel_anchor;
extern int g_chsel_n;
extern bool g_ctx_open;
extern DropdownState g_dd;
extern bool g_drag_active;
extern bool g_drag_clip_active;
extern Vector2 g_drag_clip_p0;
extern int g_drag_clip_src_ch;
extern int g_drag_clip_src_scene;
extern int g_dragging_loop_marker;
extern int g_dragging_slider;
extern bool g_edit_menu_open;
extern double g_edit_menu_open_time;
extern int64_t g_end_ext;
extern FileBrowser *g_fb;
extern bool g_file_menu_open;
extern double g_file_menu_open_time;
extern float g_follow_sec;
extern Font g_font;
extern bool g_fx_open;
extern int g_fx_scroll[MAX_CHANNELS];
extern int g_fxm_ch;
extern bool g_fxm_just;
extern int g_fxm_param;
extern Vector2 g_fxm_pos;
extern Rectangle g_fxm_rect;
extern int g_fxp_ch;
extern char g_fxp_filter[64];
extern bool g_fxp_just;
extern Vector2 g_fxp_pos;
extern Rectangle g_fxp_rect;
extern int g_fxp_scroll;
extern int g_hovered_ch;
extern int g_hovered_scene;
extern int g_last_click_ch;
extern int g_last_click_scene;
extern double g_last_click_time;
extern bool g_log_open;
extern int g_log_scroll;
extern double g_loop_mv_grab;
extern bool g_loud_open;
extern bool g_collect_open;                 /* 2.0 */
extern int  g_collect_n, g_collect_choice, g_collect_sel;
extern bool g_collect_busy;
extern Rectangle g_menu_dd_rect;
extern bool g_midi_learn_mode;
extern UiActionRequest g_pending_action;
extern EditMenuAction g_pending_edit_action;
extern FileMenuAction g_pending_file_action;
extern bool g_perf_open;
extern bool g_play_menu_open;
extern double g_play_menu_open_time;
extern bool g_plugins_open;
extern int g_selected_ch;
extern int g_selected_scene;
extern bool g_ti_active;
extern int g_ti_numeric;
extern Session *g_ui_session;
extern int g_vf_id;
extern bool g_wave_follow;
extern float g_waveform_h;
extern int g_carry_hc;
extern int g_carry_n;
extern int g_em_kind;
extern Rectangle g_em_rect;
extern const char *g_tool_tip;
extern double g_clip_led[MAX_CHANNELS + 1];
extern int g_fx_click_ch;
extern double g_fx_click_t;
extern bool g_rt_open;
extern int g_rt_target;
extern bool g_dsp_open;
extern bool g_me_focus;
extern bool g_midi_settings_open;









extern size_t g_loop_mv_s0, g_loop_mv_e0;
extern Rectangle g_log_rect, g_log_btn;
extern char g_appearance_bk_theme[32], g_appearance_bk_accent[32];
extern Rectangle g_rt_btn, g_rt_rect;
extern float g_tool_tip_x, g_tool_tip_y;
extern bool g_can_redo, g_log_just_opened, g_rt_just_opened;
extern int g_fxm_idx, g_cell_last_sc, g_fx_click_k, g_carry_hs;
void appearance_restore(void);
Color channel_color(int idx);
void chsel_only(int c);
bool cmark_get(int c, int sc);
bool draw_dropdown_button(int id, const char *label, float x, float y, float w, float h, bool is_open);
int draw_dropdown_list(int id, const char *const *items, int count, int current, float x, float y, float w);
void edit_menu_close(void);
void edit_menu_open(void);
void file_menu_close(void);
void file_menu_open(void);
void fmt_bar_beat(char *out, size_t n, double b);
void fmt_bars(char *out, size_t n, double L);
void fmt_bpm(char *out, size_t n, float v);
void note_colors(Session *s, int ch, const Clip *cl, Color *bg, Color *border, Color *text);
void num_edit_begin(int kind, int ch, int sc, float value, Rectangle r);
Color palette_raw(int idx);
void play_menu_close(void);
void play_menu_open(void);
Rectangle row_hl(Rectangle r);
double sig_bar(void);
void sig_edit_begin(const char *init, Rectangle r);
double sig_unit(void);
void utf8_chop_last(char *s);
int value_field_input(int id, Rectangle r, float *val, bool enabled);
int value_field_input_ex(int id, Rectangle r, float *val, bool enabled, float vmin, float vmax, float step_px, float fine_px);
void carry_draw_ghost(Session *s);
void carry_input(Session *s);
void carry_mark_cell(Session *s, int c, int i, Rectangle r);
void draw_edit_menu(Session *s);
void draw_waveform_zone(Session *s, float x, float y, float w, float h);
void icon_pencil(float cx, float cy, Color c);
void icon_pointer(float cx, float cy, Color c);
void computer_keys(Session *s);
int device_panel_channel(Session *s);
void draw_device_panel(Session *s, int ch, float x, float y, float w, float h);
void draw_about_dialog(int screen_w, int screen_h);
void draw_loud_dialog(int screen_w, int screen_h);
void draw_collect_dialog(int screen_w, int screen_h);   /* 2.0 */
void draw_collect_progress(int screen_w, int screen_h);
void draw_midi_settings_dialog(int screen_w, int screen_h);
void draw_perf_dialog(int screen_w, int screen_h);
void draw_plugins_dialog(int screen_w, int screen_h);
bool draw_add_scene_button(float x, float y, float w, float h);
void draw_channel_footer(Session *s, int ch, float x, float y, float w, float h);
void draw_channel_header(Session *s, int ch,  float x, float y, float w, float h);
void draw_clip_cell(Session *s, int ch, int scene, float x, float y, float w, float h);
void draw_fx_picker(Session *s);
void draw_fx_slot_menu(Session *s);
bool draw_loop_button(Session *s, float x, float y, float w, float h);
void draw_master(Session *s, float x, float y, float w, float h);
bool draw_midi_button(float x, float y, float w, float h);
void draw_rec_input_menu(Session *s);
void draw_scene_button(Session *s, int scene, float x, float y, float w, float h);
void draw_scene_header(float x, float y, float w, float h);
void draw_stop_button(Session *s, float x, float y, float w, float h);
void fx_open_window(Session *s, int ch, int k);
int rt_hw_outputs(void);
void draw_bottom_bar(int screen_w, int screen_h);
void draw_dsp_popup(int screen_w);
void draw_edit_dropdown(int screen_w, int screen_h);
void draw_file_dropdown(int screen_w, int screen_h);
void draw_menu_bar(Session *s, int screen_w);
void draw_play_dropdown(void);
void draw_top_bar(int screen_w);
Rectangle dsp_popup_rect(int screen_w);
void format_time(char *buf, size_t bufsize, double seconds);
double ruler_interval(double total_sec);
void ui_transition_to(Session *s, int scene);
void draw_midi_editor(Session *s, int ch, int sc, float x, float y, float w, float h);
void draw_midi_badge(Session *s, int ch, float x, float y);
bool fx_ui_free(void);
void ui_rec_click(Session *s, int ch, int sc);
void draw_audio_settings_dialog(int screen_w, int screen_h);
void draw_appearance_dialog(int screen_w, int screen_h);
bool me_take_delete(Session *s);
bool me_take_copy(Session *s, bool cut);
bool me_take_paste(Session *s);
/* (end of declarations) */

#endif
