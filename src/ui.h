/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef UI_H
#define UI_H

#include "session.h"
#include "filebrowser.h"
#include "audio.h"
#include "sdlgfx.h"
#include <stdbool.h>
#include <stddef.h>

// ============ Text ============

int ui_text_w(const char *text, int size);
void ui_text(const char *text, int x, int y, int size, Color color);

// Scaled versions (use g_font_size from theme.h)
int ui_text_w_scaled(const char *text, int size);
void ui_text_scaled(const char *text, int x, int y, int size, Color color);

// ============ Lifecycle ============

void ui_init(FileBrowser *fb);
void ui_draw(Session *s, int *out_sel_ch, int *out_sel_scene);
void ui_move_selection(Session *s, int dch, int dscene);
void ui_get_selection(int *ch, int *scene);
/* 1.4.8: the pad box (midimap.h) from the selected cell: first channel, first scene, size (w x h) */
void ui_pad_box(Session *s, int *c0, int *r0, int *w, int *h);
// The cell under the cursor at the last draw (-1/-1 if none) — for drag & drop from outside
void ui_get_hovered_cell(int *ch, int *scene);
void ui_toggle_library(void);
void ui_toggle_waveform(void);
void ui_toggle_master(void);      /* 1.3.10: Ctrl+Shift+M */
void ui_toggle_meters(void);      /* 1.3.10: Ctrl+Shift+U */
void ui_zoom_step(int dir);       /* 1.3.10: +1 bigger, -1 smaller, 0 = 100 % */
void ui_toggle_mixer(void);        /* Ctrl+F: shows/hides the fader panel */
void ui_flash_message(const char *msg);

// ============ Text Input ============

void ui_text_input_begin(const char *initial_text);
void ui_text_input_begin_at(const char *initial_text,
                            int x, int y, int w, int h);
void ui_text_input_end(bool accepted);
bool ui_text_input_is_active(void);
bool ui_text_input_was_accepted(void);
void ui_text_input_get(char *out, size_t out_size);

void ui_request_rename(void);
bool ui_text_input_is_rename(void);
int  ui_text_input_get_rename_ch(void);
int  ui_text_input_get_rename_scene(void);

// Renaming a channel (inline in its header)
void ui_request_rename_channel(int ch);
bool ui_text_input_is_rename_channel(void);
int  ui_text_input_get_rename_channel(void);
bool ui_get_channel_header_rect(int ch, int *x, int *y, int *w, int *h);
// Scrollbar from the last frame (vertical: 0 = horizontal, 1 = vertical); false if it is not shown (for tests)

// Colours: 20-colour palette; ui_text_color_on() gives white/black text depending on the background
Color ui_palette_color(int idx);
int   ui_palette_count(void);
Color ui_text_color_on(Color bg);

void ui_request_new_note(int ch, int scene);
bool ui_text_input_is_new_note(void);
int  ui_text_input_get_new_note_ch(void);
int  ui_text_input_get_new_note_scene(void);

// Waveform geometry (for tests). false if not drawn yet.

bool ui_get_clip_rect(int ch, int scene,
                      int *x, int *y, int *w, int *h);

// ============ Context Menu ============

typedef enum {
    UI_ACTION_NONE = 0,
    UI_ACTION_RENAME,
    UI_ACTION_DELETE_CLIP,
    UI_ACTION_DELETE_CHANNEL,
    UI_ACTION_DELETE_SCENE,
    UI_ACTION_CUT_CLIP,
    UI_ACTION_COPY_CLIP,
    UI_ACTION_PASTE,
    UI_ACTION_NEW_NOTE,
    UI_ACTION_COLOR,
    UI_ACTION_ADD_QUEUE,        /* handled directly in ui.c */
    UI_ACTION_NEW_MIDI,         /* 1.3: handled directly in ui.c */
    UI_ACTION_EXPORT_AUDIO,     /* 1.4.14: handled directly in ui.c */
    UI_ACTION_SEPARATE,         /* 1.4.14 */
    UI_ACTION_UNSEPARATE,       /* 1.4.14 */
    UI_ACTION_SCHEDULE,         /* 1.4.17: launch at a clock time (a clip or a scene) */
    UI_ACTION_TRANSITION,       /* 1.4.17: fade to this scene */
    UI_ACTION_EXPORT_SCENE,     /* 1.4.17 */
    UI_ACTION_SURROUND,         /* 1.4.19 */
    UI_ACTION_MC_FILE,          /* 1.4.24: a surround file - how it plays */
} UiAction;

typedef struct {
    UiAction action;
    int      ch;
    int      scene;
} UiActionRequest;

bool ui_take_action(UiActionRequest *out_req);
bool ui_context_menu_is_open(void);

// ============ Clipboard ============

void ui_clipboard_set_mc(int mode, int out, const char *orig);   /* 1.4.24 */
void ui_clipboard_mc_next(void);
void ui_clipboard_copy_clip(const char *path, const char *name);
bool ui_edit_take_delete(Session *s);   /* step 27b: Delete deletes the selected part (if any), not the clip */
bool ui_edit_take_copy(Session *s, bool cut);   /* step 36k: Ctrl+C/X with a part selected in the waveform */
bool ui_edit_take_paste(Session *s);            /* Ctrl+V with the mouse over the waveform -> at the bar under it */
/* step 36: selecting channels and marking (Ctrl/Shift + click) */
bool ui_channel_focus(void);                        /* a channel (header) was selected last */
bool ui_delete_selection(Session *s);               /* Delete: the selected channels or the marked clips; false = there are none */
bool ui_ctx_delete_channels(Session *s, int ch);    /* the context menu: ch is among several selected -> deletes all of them */
bool ui_ctx_delete_clips(Session *s, int ch, int sc);
void ui_clipboard_set_edits(const char *text);   /* step 27: the parts go with the copy */
const char *ui_clipboard_edits(void);
void ui_clipboard_clear(void);
bool ui_clipboard_has_clip(void);
void ui_clipboard_get(char *out_path, size_t path_size,
                      char *out_name, size_t name_size);

// ============ File Menu ============

typedef enum {
    FILE_MENU_NONE = 0,
    FILE_MENU_NEW,
    FILE_MENU_SAVE,
    FILE_MENU_SAVE_AS,
    FILE_MENU_COLLECT_AND_SAVE,
    FILE_MENU_LOAD,
    FILE_MENU_SEPARATION,       /* 1.4.15: handled directly in ui.c */
    FILE_MENU_SCHEDULE,         /* 1.4.17: handled directly in ui.c */
} FileMenuAction;

bool ui_take_file_action(FileMenuAction *out);

// ============ Edit Menu ============

typedef enum {
    EDIT_MENU_NONE = 0,
    EDIT_MENU_AUDIO,
    EDIT_MENU_MIDI,
    EDIT_MENU_APPEARANCE,
    EDIT_MENU_UNDO,
    EDIT_MENU_REDO,
} EditMenuAction;

bool ui_take_edit_action(EditMenuAction *out);
// Tells the Edit menu whether Undo/Redo are available (called every frame from main)
void ui_set_undo_state(bool can_undo, bool can_redo);
// Window title: true = there are unsaved changes (shows "*")
Color ui_channel_color(int idx);

// ============ Routing / Shortcuts ============

void ui_open_routing(void);
void ui_open_export(Session *s, int ch, int scene);   /* 1.4.14: the Export Audio dialog for a clip */
void ui_open_separate(Session *s, int ch, int scene); /* 1.4.14: vocal separation (download on first use, progress) */
bool ui_separation_busy(void);
void ui_open_sep_settings(void);
/* 1.4.17: launch at a clock time (kind SCHED_CLIP / SCHED_SCENE; ch -1 for a scene); the list of all */
void ui_open_schedule(Session *s, int kind, int ch, int scene);
void ui_open_schedule_list(Session *s);
void ui_open_export_scene(Session *s, int scene);
void ui_open_surround(Session *s, int ch);         /* 1.4.19 */
void ui_open_sound_cards(void);
bool ui_get_selected_clip(int *ch, int *scene);    /* 1.4.22 */                    /* 1.4.21: more than one sound card */
void ui_audio_apply(const AudioConfig *c);         /* 1.4.21: apply this audio configuration (as the Audio Settings' Apply) */
/* 1.4.17: scene transitions (the top bar, MIDI buttons) */
void ui_scene_next(Session *s);
void ui_scene_back(Session *s);
void ui_automation_toggle(Session *s);
void ui_fade_time_midi(Session *s, int delta);     /* steps (+/-) or 1000 + an absolute knob 0..127 */                      /* 1.4.15: File -> Separation... */
void ui_separation_tick(Session *s);                  /* every frame: a finished separation is attached to its clip */
bool ui_separation_progress(int ch, int scene, float *p, char *text, size_t n);   /* this clip is being separated */
void ui_separation_show(void);
void ui_open_color_picker(int ch, int scene);   /* scene < 0 = colour of a channel, otherwise of a note */
void ui_open_shortcuts(void);

// ============ Save on close ============

typedef enum { QUIT_CHOICE_NONE = 0, QUIT_CHOICE_SAVE, QUIT_CHOICE_DONT_SAVE, QUIT_CHOICE_CANCEL } QuitChoice;

void ui_open_quit_prompt(const char *project_name);   // project_name may be "" (unnamed)
bool ui_quit_prompt_is_open(void);
bool ui_take_quit_choice(QuitChoice *out);

// ============ Audio Settings ============

void ui_open_audio_settings(void);
bool ui_take_audio_refresh(void);   // true = "Refresh" was clicked in Audio Settings
#ifdef _WIN32
int  ui_take_asio_panel(void);        /* Windows: "ASIO Panel" pressed -> the device index, otherwise -1 */
#endif
void ui_audio_refresh_done(void);   // main.c: the cards were rescanned -> refresh the lists
bool ui_take_audio_apply(AudioConfig *out_config);

// ============ Appearance ============

void ui_open_appearance(void);
/* 2.0: Collect All & Save - asks what the n compressed files (MP3, FLAC, Ogg ...) become; the answer comes once from
 * ui_take_collect_choice: -1 = cancel, else WAV_OUT_* (wav.h) */
void ui_open_collect_choice(int n);
bool ui_take_collect_choice(int *fmt);
void ui_collect_busy(bool on);   /* 2.0: collect running -> a progress window (modal) */

// ============ MIDI Settings ============

void ui_open_midi_settings(void);
bool ui_midi_settings_is_open(void);

// ============ MIDI Learn ============

void ui_toggle_midi_learn(void);
bool ui_midi_learn_is_active(void);

// ============ General ============

bool ui_any_modal_open(void);

// step 32: selected scene (its button on the left) -> index, otherwise -1; selecting a scene row
int  ui_selected_scene_row(void);
void ui_select_scene_row(int sc);

/* 1.3: the MIDI editor uses the arrow keys (main.c asks before it moves the clip selection) */
bool ui_midi_take_arrows(Session *s);
/* 1.2: playing an instrument from the computer keyboard */
bool ui_computer_keys_on(void);
void ui_toggle_computer_keys(void);

#endif
