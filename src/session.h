/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef SESSION_H
#define SESSION_H

#include "wav.h"
#include "stream.h"
#include "peaks.h"
#include "bpm.h"
#include "stretch.h"
#include "edit.h"
#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_CHANNELS 50
#define MAX_SCENES   250
// Waveform zoom: 1x = the whole clip; the upper limit is large because the real limit is computed from the clip length (UI)
#define WAVE_ZOOM_MIN (1.0f / 3.0f)   /* step 36j: the view reaches one length before and after the clip */
#define WAVE_ZOOM_MAX 200000.0f
#define N_PALETTE    20            // colours for channels and notes (ui.c: PALETTE[])
#define CH_WIDTH_DEFAULT 220       // default channel width (pixels; it was 190)
#define NEW_PROJECT_CHANNELS 4     // new project / program start
#define NEW_PROJECT_SCENES   16
#define CH_WIDTH_MIN     172       // step 36h: any narrower and the faders and buttons get cramped
#define CH_WIDTH_MAX     700
#define MAX_HW_OUT   62            // the most sound card outputs the mixer can fill (2 + 62 = 64 bits in the mask)
#define QUEUE_MAX    128           // the most clips in the queue

// --- Routing (like the matrix in Ardour) ---
// Every channel has two output ports (L and R). Every port has a mask of "where it goes":
//   bit 0 = Master L, bit 1 = Master R, bit 2+k = directly to output k+1 of the sound card.
// The master also has two ports (L and R) with a mask only towards the card outputs: bit k = output k+1.
typedef uint64_t RouteMask;                      /* 64 bits: enough for 2 + MAX_HW_OUT */
#define RBIT(b)          ((RouteMask)1 << (b))
#define ROUTE_MASTER_L   RBIT(0)
#define ROUTE_MASTER_R   RBIT(1)
#define ROUTE_HW(k)      RBIT(2 + (k))            /* k = 0 .. MAX_HW_OUT-1 */
#define MASTER_ROUTE_HW(k) RBIT(k)                /* master port mask */
#define ROUTE_DEFAULT_L  ROUTE_MASTER_L           /* channel L -> Master L */
#define ROUTE_DEFAULT_R  ROUTE_MASTER_R           /* channel R -> Master R */
#define MASTER_ROUTE_DEFAULT_L MASTER_ROUTE_HW(0) /* Master L -> output 1 */
#define MASTER_ROUTE_DEFAULT_R MASTER_ROUTE_HW(1) /* Master R -> output 2 */

// --- MIDI buttons (Note or CC) for Mute / Solo / clips ---
#define MIDI_BIND_NOTE   1
#define MIDI_BIND_CC     2
#define MIDI_BIND_MAKE(kind, num) (((kind) << 8) | ((num) & 0x7F))
#define MIDI_BIND_KIND(b) ((b) >> 8)
#define MIDI_BIND_NUM(b)  ((b) & 0xFF)

typedef enum { BTN_LEARN_NONE = 0, BTN_LEARN_MUTE, BTN_LEARN_SOLO, BTN_LEARN_CLIP } BtnLearnKind;

/* Maximum length of a clip/note/channel name IN BYTES (Cyrillic is 2 bytes per letter). */
#define NAME_LEN 256

typedef struct {
    WavFile        wav;            // metadata only (frame_count, sample_rate); samples == NULL
    ClipStream    *stream;         // streaming from disk
    Peaks         *peaks;          // waveform "sketch" (for drawing)
    _Atomic size_t position;       // the next frame to play (updated by the audio thread)
    atomic_bool    has_clip;
    atomic_bool    is_note;
    atomic_bool    playing;
    atomic_bool    paused;
    atomic_bool    loop;
    _Atomic size_t loop_start;
    _Atomic size_t loop_end;

    // Start / End markers
    _Atomic size_t start_marker;
    _Atomic size_t end_marker;

    // Waveform zoom / scroll (visual only)
    _Atomic float  wave_zoom;          // WAVE_ZOOM_MIN .. WAVE_ZOOM_MAX
    _Atomic size_t wave_scroll_frame;  // start of the visible range

    // MIDI pad/button that plays the clip (0 = none). The code is MIDI_BIND_MAKE(kind, number).
    _Atomic int    bind;

    // --- Clip tempo (BPM) ---
    // Detected in the background on load; can be corrected by hand. "warp" (the tick) = plays at
    // its own BPM field: if you change it, the speed changes WITHOUT changing the pitch (stretch).
    _Atomic float  bpm_detected;       // 0 = not detected yet / failed
    _Atomic float  bpm_fix;            // manually corrected "real" tempo of the file (0 = none)
    _Atomic float  bpm_play;           // the tempo to play at with warp on (0 = like the original)
    atomic_bool    warp;               // the warp tick
    atomic_bool    bpm_busy;           // detection is still running
    atomic_uint    stretch_gen;        // +1 on every start/seek -> the audio thread resets the stretch
    BpmJob        *bpm_job;            // UI thread only
    Stretch       *stretch;            // created when warp is turned on for the first time
    struct EditSrc *edit;              // step 27: the cutting (belongs to the stream's source; NULL = no stream)
    atomic_bool    ended;              // audio thread: it reached the end by itself (not stopped by hand) — for the queue
    unsigned       start_offset;       // audio thread only: the queue starts it in the middle of a block (silence before)
    // --- no clicks on start/stop/seek (step 23), audio thread only ---
    bool           dc_was_playing;     // it was playing in the previous block
    unsigned       dc_gen;             // the stretch_gen seen last (a change = seek/new start)
    unsigned       dc_tail;            // remaining frames of the smooth fade-out of the old audio
    unsigned       dc_fadein;          // remaining frames of the smooth fade-in of the new audio
    float          dc_last_l, dc_last_r;
    bool           q_launch;           // started by the queue (seamlessly) -> no fade-in at the start
    // --- queue crossfade ---
    atomic_int     xf_dir;             // 0 = none, +1 = coming in (fade in), -1 = going out (fade out)
    long           xf_len, xf_pos;     // audio thread only: length and position of the transition (frames; pos<0 = not started yet)
    unsigned       xf_gen;             // stretch_gen at the start (a manual start/seek cancels it)
    atomic_size_t  aud_end;            // how far it really sounds (without the silence at the end); 0 = unknown
    unsigned       stretch_seen;       // audio thread only: the last stretch_gen seen
    bool           stretch_live;       // audio thread only: the stretch is in use (otherwise -> reset)

    int            color;              // notes only: 0 = default colour, 1..N_PALETTE = palette colour + 1

    char           name[NAME_LEN];
    char           path[1024];
} Clip;

/* Plugin on the channel (step 33; since step 34 it processes the audio - see fxchain.h) */
#define MAX_CH_FX 8
struct FxChain;
typedef struct {
    char fmt[8];          /* "LV2", "VST3", "VST", "CLAP" */
    char name[96];
    char path[512];
    bool bypass;
    int  state_ref;       /* step 35: the settings saved from the project (fxchain.h: fxstate_*), 0 = none */
} ChFx;

typedef struct {
    Clip           clips[MAX_SCENES];

    _Atomic float  gain;
    _Atomic float  smooth_gain;

    _Atomic float  pan;
    _Atomic float  smooth_pan;

    _Atomic float  peak_level;
    _Atomic float  peak_l, peak_r;     // stereo metering (after volume and pan)
    atomic_bool    clipped;            // step 36c: it went above 0 dBFS (the UI takes it and resets it)
    atomic_bool    muted;
    atomic_bool    soloed;
    int            color_id;           // 0 .. N_PALETTE-1
    int            width;              // column width in pixels; 0 = default (view only)
    char           name[NAME_LEN];

    _Atomic int    midi_cc;
    atomic_bool    midi_learn_armed;

    _Atomic int    pan_midi_cc;
    atomic_bool    pan_midi_learn_armed;

    _Atomic int    mute_bind;          // MIDI button for Mute (0 = none)
    _Atomic int    solo_bind;          // MIDI button for Solo (0 = none)

    _Atomic RouteMask route_l;            // where the channel's L port goes (see ROUTE_*)
    _Atomic RouteMask route_r;

    int            fx_count;           // the channel's plugins (main thread only)
    ChFx           fx[MAX_CH_FX];
    _Atomic(struct FxChain *) fxchain; // the loaded plugins (step 34): written by fxchain_sync, read by the mixer
} Channel;

typedef struct {
    Channel        channels[MAX_CHANNELS];
    _Atomic int    channel_count;      // atomic: the audio thread reads them while the UI adds/deletes
    _Atomic int    scene_count;
    int            sample_rate;

    _Atomic float  master_gain;
    _Atomic float  master_smooth_gain;
    _Atomic float  master_pan;         // step 24: master balance -1 (left) .. 0 .. +1 (right)
    _Atomic float  master_smooth_pan;  // audio thread only
    atomic_bool    master_muted;       // step 24: main MUTE (smooth, no clicks)

    _Atomic float  master_peak_level;
    _Atomic float  master_peak_l, master_peak_r;
    atomic_bool    master_clipped;     // step 36c: the master went above 0 dBFS (it is clipped at the output)

    _Atomic int    master_midi_cc;
    atomic_bool    master_midi_learn_armed;

    // synchronization UI <-> audio thread (see session.c: audio_barrier)
    atomic_uint    audio_cycles;
    pthread_mutex_t mix_mu;            // The UI holds it while it moves clips in memory (inserting/deleting a channel/scene);
                                       // the audio thread only TRIES (trylock) — if it is taken, this block is silence
    atomic_bool    audio_running;

    // routing of the master to the card outputs
    _Atomic RouteMask master_route_l;
    _Atomic RouteMask master_route_r;

    // "learn" for buttons: which button/clip is waiting for the next MIDI signal
    atomic_int     btn_learn_kind;     // BtnLearnKind
    atomic_int     btn_learn_ch;
    atomic_int     btn_learn_scene;

    _Atomic float  tempo;              // project BPM (kept in the project; no longer shown)

    // --- Queue: clips that start one after another ---
    // Owned by the UI thread; the audio thread sees only q_anchor/q_next and starts q_next exactly when q_anchor ends.
    int            queue_ch[QUEUE_MAX], queue_sc[QUEUE_MAX];
    int            queue_len;
    int            last_started;       // ch*MAX_SCENES+sc of the last clip started (-1 = none)
    atomic_int     q_anchor;           // the clip after which the queue starts ("now playing"), -1 = none
    atomic_int     q_next;             // the prepared next one (the audio thread may start it), -1 = none
    atomic_int     q_launched;         // the audio thread started it: index + 1 (0 = no)
    atomic_long    xfade_frames;       // crossfade between the clips in the queue (0 = none, then it is seamless)

    // true = files with a different sample rate are converted into the cache; false (default) = they play as they are
    atomic_bool    convert_files;
} Session;

// --- Lifecycle ---
void session_init(Session *s, int sample_rate);
/* step 35b: what the main thread does while it waits for the audio (services the plugins) */
void session_set_wait_pump(void (*fn)(void));
void session_free(Session *s);

// --- Channels ---
int  session_add_channel(Session *s, const char *name);
void session_set_channel_gain(Session *s, int ch, float pos);
void session_set_channel_pan(Session *s, int ch, float pos);
void session_toggle_mute(Session *s, int ch);
void session_toggle_solo(Session *s, int ch);
bool session_any_soloed(Session *s);
bool session_channel_audible(Session *s, int ch);

// Brings a channel to its initial state (volume, pan, default routing...).
// Used by session_add_channel and by project loading.
void session_channel_init(Channel *c, int idx, const char *name);

// --- Routing ---
// side: 0 = L port, 1 = R port. dest: 0 = Master L, 1 = Master R, 2+k = output k+1 of the card.
RouteMask session_route_get(Session *s, int ch, int side);
void     session_route_set(Session *s, int ch, int side, RouteMask mask);
void     session_route_toggle(Session *s, int ch, int side, int dest);
// Master port -> card outputs. dest: k = output k+1.
RouteMask session_master_route_get(Session *s, int side);
void     session_master_route_set(Session *s, int side, RouteMask mask);
void     session_master_route_toggle(Session *s, int side, int dest);
// Everything default: channels -> Master, Master -> outputs 1/2.
void     session_routing_reset(Session *s);

// --- Conversion helpers ---
/* Fader (step 36, like Bitwig): position 0..1 -> gain. 1.0 = +6 dB, FADER_UNITY_POS = exactly 0 dB (gain 1.0). */
#define FADER_UNITY_POS 0.8f
float session_pos_to_gain(float pos);
float session_gain_to_db(float pos);            /* position -> dB (-1000 = silence) */
float session_db_to_pos(float db);
float session_old_pos_to_pos(float old);        /* a project from before step 36 -> the new position */

// --- Clips ---
int  session_set_clip(Session *s, int ch, int scene, const char *wav_path);
// Like session_set_clip, but with a ready source (also used by tests). Takes ownership of src.
int  session_attach_stream(Session *s, int ch, int scene,
                           const StreamSource *src, const char *display_path);
// Converting files with a different sample rate: on/off (applies to files loaded from now on).
void session_set_convert_files(Session *s, bool on);
// Changes the session rate (e.g. when the sound card's sample rate changes):
// reloads all clips for the new rate and scales the markers/loop. Returns the number of clips it could not load.
int  session_set_sample_rate(Session *s, int sample_rate);
// Stops everything and frees all streams (for "new project" / "load project").
void session_release_all_clips(Session *s);
void session_clear_clip(Session *s, int ch, int scene);
void session_toggle_clip(Session *s, int ch, int scene);
// Plays the clip from a given frame (the Start marker is not touched). If it is already playing - only seeks there.
void session_play_clip_from(Session *s, int ch, int scene, size_t frame);
void session_seek_clip(Session *s, int ch, int scene, size_t frame);
void session_move_clip(Session *s,
                       int src_ch, int src_scene,
                       int dst_ch, int dst_scene);
void session_rename_clip(Session *s, int ch, int scene, const char *new_name);

// --- Notes ---
int  session_set_note(Session *s, int ch, int scene, const char *text);

// --- Colours, names and width (looks only) ---
void session_set_channel_color(Session *s, int ch, int palette_idx);        // 0 .. N_PALETTE-1
void session_rename_channel(Session *s, int ch, const char *new_name);
void session_set_note_color(Session *s, int ch, int scene, int color);      // 0 = default, 1..N_PALETTE
int  session_get_note_color(Session *s, int ch, int scene);
void session_set_channel_width(Session *s, int ch, int width);              // 0 = default
int  session_get_channel_width(Session *s, int ch);                         // always CH_WIDTH_MIN..CH_WIDTH_MAX
bool session_is_note(Session *s, int ch, int scene);

// --- Delete ---
void session_delete_channel(Session *s, int ch);
void session_delete_scene(Session *s, int sc);

// --- Scenes ---
int  session_add_scene(Session *s);
// Inserting an empty channel / scene at a given position (for undo).
int  session_insert_channel(Session *s, int idx);
int  session_insert_scene(Session *s, int idx);
// Step 36: moves channel from to position to (playback continues). false = nothing was done.
bool session_move_channel(Session *s, int from, int to);
void session_play_scene(Session *s, int scene);
void session_stop_scene(Session *s, int scene);

// --- Global ---
void session_play_all(Session *s);
void session_stop_all(Session *s);

// --- Pause / Resume ---
void session_pause_all(Session *s);
void session_resume_all(Session *s);
bool session_any_playing(Session *s);
bool session_any_paused(Session *s);

// --- Loop ---
void session_toggle_loop(Session *s, int ch, int scene);
void session_set_loop_region(Session *s, int ch, int scene,
                              size_t start, size_t end);
bool session_get_loop_state(Session *s, int ch, int scene,
                             bool *out_on, size_t *out_start, size_t *out_end);

// --- Start / End markers ---
void session_set_start_marker(Session *s, int ch, int scene, size_t pos);
void session_set_end_marker(Session *s, int ch, int scene, size_t pos);
void session_reset_start_marker(Session *s, int ch, int scene);
void session_reset_end_marker(Session *s, int ch, int scene);
bool session_get_markers(Session *s, int ch, int scene,
                          size_t *out_start, size_t *out_end);

// --- MIDI: fader ---
void session_set_midi_cc(Session *s, int ch, int cc);
int  session_get_midi_cc(Session *s, int ch);
void session_set_master_midi_cc(Session *s, int cc);
int  session_get_master_midi_cc(Session *s);
void session_arm_midi_learn(Session *s, int ch);
void session_disarm_all_midi_learn(Session *s);
int  session_get_armed_channel(Session *s);

// --- MIDI: pan ---
void session_set_pan_midi_cc(Session *s, int ch, int cc);
int  session_get_pan_midi_cc(Session *s, int ch);
void session_arm_pan_midi_learn(Session *s, int ch);
void session_disarm_all_pan_midi_learn(Session *s);
int  session_get_armed_pan_channel(Session *s);

// --- MIDI: buttons (Mute / Solo / clips — pads) ---
void session_arm_button_learn(Session *s, int kind, int ch, int scene);
void session_disarm_button_learn(Session *s);
bool session_get_button_learn(Session *s, int *kind, int *ch, int *scene);
int  session_get_mute_bind(Session *s, int ch);
int  session_get_solo_bind(Session *s, int ch);
int  session_get_clip_bind(Session *s, int ch, int scene);
void session_set_mute_bind(Session *s, int ch, int bind);
void session_set_solo_bind(Session *s, int ch, int bind);

// Plugins on the channel (step 33). add returns the index or -1 (no room).
int  session_fx_add(Session *s, int ch, const char *fmt, const char *name, const char *path);
void session_fx_remove(Session *s, int ch, int idx);
void session_fx_toggle_bypass(Session *s, int ch, int idx);
void session_set_clip_bind(Session *s, int ch, int scene, int bind);
// A new MIDI signal (kind = MIDI_BIND_NOTE / MIDI_BIND_CC; value = velocity / CC value).
// If there is an armed button -> binds it; otherwise triggers everything bound to this signal.
// Called from the UI thread. Returns true if something was bound or triggered.
bool session_handle_midi_button(Session *s, int kind, int number, int value);
// Human-readable code: "N36" or "CC20"
void session_bind_to_text(int bind, char *out, size_t size);

// --- Tempo ---
void   session_set_tempo(Session *s, float bpm);           // clamps to BPM_MIN..BPM_MAX, rounds to 0.01
float  session_get_tempo(Session *s);
// The original tempo of the file (the manually corrected one if any; otherwise the detected one). 0 = unknown.
float  session_clip_bpm_orig(Session *s, int ch, int scene);
// The tempo the clip is playing at right now (with warp off = the original).
float  session_clip_bpm_play(Session *s, int ch, int scene);
bool   session_clip_bpm_busy(Session *s, int ch, int scene);
bool   session_get_clip_warp(Session *s, int ch, int scene);
void   session_set_clip_warp(Session *s, int ch, int scene, bool on);
// Manual value in the field: with warp on = the tempo to play at; with warp off = a correction of the original.
void   session_set_clip_bpm_value(Session *s, int ch, int scene, float bpm);
// Directly (for loading a project/undo): 0 = no value.
void   session_set_clip_bpm_fields(Session *s, int ch, int scene, float detected, float fix, float play);
// Playback speed (1 = normal) — used by the mixer.
double session_clip_speed(const Clip *cl);

// --- Cutting without changing the file (step 27) ---
// The clip's current parts (read only; valid until the next session_clip_apply_edits). NULL = no audio.
const EditList *session_clip_edits(Session *s, int ch, int scene);
// Replaces the parts (takes ownership of nl). crop_a >= 0: "keep only this" from crop_a (positions/markers shift).
// If the clip is playing, it continues from the same place with the new parts. 0 = OK.
int    session_clip_apply_edits(Session *s, int ch, int scene, EditList *nl, long long crop_a);
bool   session_clip_is_edited(Session *s, int ch, int scene);
// Text for project/undo/copying (malloc; NULL = not cut) and back (NULL/"" = restores the original).
char  *session_clip_edits_text(Session *s, int ch, int scene);
int    session_clip_set_edits_text(Session *s, int ch, int scene, const char *text);
// Waveform sketch for a section of the clip's TIMELINE (taking the cuts and fades into account). false = no audio there.
bool   session_clip_peaks(const Clip *cl, size_t f0, size_t f1, PeakVal *out);
// The same, but with a given list of parts (UI: preview while dragging)
bool   session_clip_peaks_list(const Clip *cl, const EditList *e, int64_t f0, int64_t f1, PeakVal *out);   /* f0 may be < 0 (step 36j) */
// Called every frame: collects the finished BPM detections from the background thread.
void   session_poll_bpm(Session *s);

// --- Queue ---
// Adds a clip to the end of the queue. false = nothing is playing (then a queue makes no sense) or it is full.
bool   session_queue_add(Session *s, int ch, int scene);
int    session_queue_len(Session *s);
bool   session_queue_get(Session *s, int i, int *ch, int *scene);
void   session_queue_move(Session *s, int from, int to);   // reordering (dragging in the list)
void   session_queue_remove(Session *s, int i);
void   session_queue_clear(Session *s);
bool   session_queue_anchor(Session *s, int *ch, int *scene);  // which one is playing "now"
// Called every frame from main: prepares the next one, finishes/cleans up the queue.
void   session_queue_tick(Session *s);
// Crossfade between the clips in the queue in seconds (0 = off -> seamless, as before). Limits 0..XFADE_MAX_SEC.
#define XFADE_MAX_SEC 30.0f
void   session_set_crossfade(Session *s, float sec);
float  session_get_crossfade(Session *s);

// --- Waveform zoom (visual) ---
void   session_set_wave_zoom(Session *s, int ch, int scene, float zoom);
float  session_get_wave_zoom(Session *s, int ch, int scene);
void   session_set_wave_scroll(Session *s, int ch, int scene, size_t frame);
size_t session_get_wave_scroll(Session *s, int ch, int scene);
void   session_reset_wave_zoom(Session *s, int ch, int scene);

#endif
