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

/* 1.3: a MIDI clip = notes in beats. A published MidiSeq is never changed: an edit makes a new one and swaps the
 * pointer (the audio thread may be reading the old one; it is freed a few audio blocks later). */
typedef struct { float start, len; unsigned char key, vel, sel; } MNote;   /* sel: the editor's selection (not saved) */
typedef struct MidiSeq { float length; int n; MNote note[]; } MidiSeq;   /* length in beats; notes sorted by start */
#define MIDI_MAX_NOTES 20000

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
    _Atomic float  xf_vis;             // 1.0.5: the crossfade gain right now (for the "ghost" fader), valid when xf_vis_on
    atomic_bool    xf_vis_on;          // 1.0.5: true only while the crossfade is really moving
    // --- 1.1: quantized launch ---
    atomic_int     ql_state;           // 0 = nothing, 1 = launch, 2 = stop, 3 = restart - at the beat ql_beat
    _Atomic double ql_beat;
    atomic_bool    ql_restart_now;     // audio thread -> UI: restart this playing clip now (from the start)
    atomic_bool    ql_stopped;         // the audio thread stopped it -> the UI finishes the stop (stream)
    atomic_size_t  aud_end;            // how far it really sounds (without the silence at the end); 0 = unknown
    unsigned       stretch_seen;       // audio thread only: the last stretch_gen seen
    bool           stretch_live;       // audio thread only: the stretch is in use (otherwise -> reset)
    // --- warp switched on/off while playing (1.0.5): fade the old sound out, the new one in (no click) ---
    bool           wsw_init;           // audio thread only: wsw_warp is valid
    bool           wsw_warp;           // audio thread only: which path was heard in the previous block
    bool           wsw_wait;           // audio thread only: waiting for the stretch's first sound, then fade it in
    unsigned       wsw_fadein;         // audio thread only: remaining frames of that fade-in
    unsigned       wsw_blocks;         // audio thread only: blocks waited for the first sound (safety limit)

    int            color;              // notes only: 0 = default colour, 1..N_PALETTE = palette colour + 1

    // --- 1.3: MIDI clip (has_clip + is_midi, no stream) ---
    atomic_bool    is_midi;
    _Atomic(MidiSeq *) mseq;           // the notes (see MidiSeq)
    _Atomic double mpos_vis;           // the playhead in beats (for the editor); written by the audio thread
    atomic_uint    m_gen;              // +1 = start from the beginning (the audio thread resets its position)
    _Atomic double m_seek;             // 1.3.1: the beat a start with generation m_seek_gen begins at (play from here)
    atomic_uint    m_seek_gen;
    double         mpos;               // audio thread only: the position in beats
    unsigned       m_gen_seen;         // audio thread only
    bool           m_live;             // audio thread only: it was playing in the previous block
    float          m_off[128];         // audio thread only: when a sounding key ends (beats of the clip); <0 = silent

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

    // --- 1.4: recording ---
    atomic_bool    armed;              // the record button of the channel (an empty cell then records)
    atomic_int     rec_in_first;       // the first input of the sound card it records (0 = input 1)
    atomic_bool    rec_in_stereo;      // two inputs (first, first+1) or one (mono, copied to L and R)
    atomic_bool    monitor;            // the input is heard through the channel (and its plugins) while armed

    _Atomic int    mute_bind;          // MIDI button for Mute (0 = none)
    _Atomic int    solo_bind;          // MIDI button for Solo (0 = none)

    _Atomic RouteMask route_l;            // where the channel's L port goes (see ROUTE_*)
    _Atomic RouteMask route_r;

    int            fx_count;           // the channel's plugins (main thread only)
    ChFx           fx[MAX_CH_FX];
    _Atomic(struct FxChain *) fxchain; // the loaded plugins (step 34): written by fxchain_sync, read by the mixer
} Channel;

/* 1.4: one recording into a cell (audio from the sound card's input, or the MIDI notes played on an instrument channel).
 * The UI starts and stops it; the audio thread does it exactly on the beat (launch quantization) and passes the audio /
 * the notes through lock-free rings; the UI writes the file / builds the notes and, at the end, makes it a clip that
 * goes on playing (looped) in time. */
#define MAX_REC 8
#define REC_RING_FRAMES (1u << 20)          /* ~21 s at 48 kHz between two UI frames - plenty */
#define REC_MEV 2048
enum { REC_IDLE = 0, REC_WAIT, REC_ON, REC_STOPPING, REC_DONE };
typedef struct { double beat; unsigned char m[3]; } RecEv;
typedef struct {
    atomic_int     state;
    int            ch, sc;
    bool           midi;               // MIDI notes (instrument channel) or audio
    _Atomic double start_beat;         // the beat it starts at (-1 = at once)
    _Atomic double stop_beat;          // the beat it stops at (-1 = at once), valid in REC_STOPPING
    int            in_first, in_count; // audio: which inputs
    unsigned       latency;            // audio: frames to skip at the start / record after the stop (the delay of the card)
    /* audio thread only */
    double         beat0;              // the transport beat of the first frame
    unsigned       skip, tail;
    bool           stopped_at;         // the stop boundary has passed (now only the tail)
    /* audio thread -> UI */
    _Atomic double rec_beats;          // how long it has recorded (beats) - for the cell
    atomic_ullong  frames;             // audio: frames put into the ring so far
    atomic_ullong  after;              // frames since the stop boundary (the clip then starts in time)
    atomic_bool    overflow;           // the ring was full (the UI did not keep up) - some audio is lost
    _Atomic double final_beats;        // the length (beats) once it is DONE
    float         *ring;               // audio: REC_RING_FRAMES * 2 floats (UI allocates)
    atomic_uint    rw, rr;
    RecEv          mev[REC_MEV];       // MIDI: the played messages with their beat (from the start)
    atomic_uint    mw, mr;
    void          *ui;                 // UI thread: the writer (file) / the notes being built
} RecSlot;

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

    _Atomic float  tempo;              // 1.1: the master BPM (top bar); clips with WARP on play at this tempo
    // --- 1.1: launch quantization (like "Global Quantization" in Ableton/Bitwig) ---
    _Atomic float  quant_beats;        // a clip launched while something plays waits for the next multiple of this many beats (0 = at once)
    _Atomic double tp_beats;           // transport position in beats (from the first launched clip); written by the audio thread
    atomic_bool    tp_running;         // the transport runs (from the first launched clip until everything stops)
    atomic_bool    metronome;          // 1.3.1: clicks on every beat while the transport runs (accent on the 1st beat of the bar)
    _Atomic double tp_start;           // 1.3.1: when the transport starts next, it starts at this beat (play from a beat)
    atomic_bool    tp_reset;           // UI -> audio thread: start counting from 0 (a launch with nothing playing)
    atomic_int     ql_pending;         // how many clips wait for a quantized launch/stop/restart (0 = the audio thread skips the scan)

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

    // --- 1.2: playing instruments (see midiq.h) ---
    atomic_int     midi_target;        // the channel the MIDI keyboard plays (the selected instrument channel), -1 = none
    atomic_uint    midi_bound[4];      // notes 0..127 bound to buttons/clips (they do not play the instrument)
    atomic_bool    midi_block;         // a "learn" is waiting: notes are not played
    atomic_bool    midi_panic;         // UI -> audio: all notes off on every instrument

    // --- 1.4: recording ---
    RecSlot        rec[MAX_REC];
    atomic_int     rec_active;         // how many slots are not IDLE (0 = the audio thread skips them)
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
/* 1.3.1: a MIDI clip plays (at once) from this beat - the double click on the ruler of the MIDI editor */
void session_midi_play_from(Session *s, int ch, int scene, double beat);
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
/* 1.3: MIDI clips */
int  session_new_midi_clip(Session *s, int ch, int scene, float len_beats, const char *name);  /* 0 = ok */
bool session_is_midi(Session *s, int ch, int scene);
const MidiSeq *session_midi_get(Session *s, int ch, int scene);       /* main thread: read the current notes */
void session_midi_set(Session *s, int ch, int scene, MidiSeq *seq);   /* main thread: takes ownership, publishes */
MidiSeq *midiseq_new(int n, float length);                            /* n notes (uninitialized), malloc */
MidiSeq *midiseq_dup(const MidiSeq *m, int extra);                    /* a copy with room for 'extra' more notes */
void     midiseq_sort(MidiSeq *m);                                    /* by start, then key */
char *session_midi_text(Session *s, int ch, int scene);               /* malloc: "midi_len=..\nmn=..\n" (project/undo) */
int   session_midi_set_text(Session *s, int ch, int scene, const char *text);   /* makes the cell a MIDI clip */
MidiSeq *midiseq_from_text(const char *text);                         /* NULL = no midi_len line */
double session_midi_playhead(Session *s, int ch, int scene);          /* beats, -1 = not playing */
void session_midi_reap(Session *s);                                   /* main thread: frees old MidiSeqs (every frame) */
/* 1.4: recording (main thread) */
void session_toggle_arm(Session *s, int ch);
bool session_is_armed(Session *s, int ch);
void session_set_rec_input(Session *s, int ch, int first, bool stereo);
void session_set_monitor(Session *s, int ch, bool on);
/* a click on a cell of an armed channel: starts recording into it (empty cell) or stops the recording there.
 * wav_path: the file to record into (audio); latency: frames of the card's round trip. 0 = ok */
int  session_rec_cell(Session *s, int ch, int scene, bool midi, const char *wav_path, unsigned latency, char *err, int esz);
int  session_rec_state(Session *s, int ch, int scene, double *beats);   /* REC_* of that cell (REC_IDLE = none) */
void session_rec_tick(Session *s);                       /* every frame: writes the file / the notes, finishes */
void session_rec_stop_all(Session *s);                   /* stop every recording at once (e.g. Stop all) */

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
int  session_fx_insert(Session *s, int ch, int pos, const char *fmt, const char *name, const char *path);   /* 1.2 */
void session_fx_move(Session *s, int ch, int from, int to);                                                 /* 1.2 */
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
// 1.1: launch quantization in beats (0 = launch at once, 1 = next beat, 4 = next bar, 8 = 2 bars ...)
void   session_set_quantize(Session *s, float beats);
float  session_get_quantize(Session *s);
// the transport position in beats (bar = 4 beats); false = not running
bool   session_transport(Session *s, double *beats);
// 0 = nothing waits, 1 = waits to start, 2 = waits to stop, 3 = waits to restart
int    session_clip_launch_state(Session *s, int ch, int scene);
// UI thread, every frame: finishes what the audio thread did at the quantized moment
void   session_quant_tick(Session *s);
/* 1.2: instruments */
void session_midi_tick(Session *s);                      /* main thread, every frame: which notes are bound to buttons */
void session_set_midi_target(Session *s, int ch);        /* -1 = none */
int  session_get_midi_target(Session *s);
void session_midi_panic(Session *s);                     /* all notes off */
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
/* The stretch speed from the tempo fields, even when warp is already switched off (for fading out the old sound). */
double session_clip_speed_warp(const Clip *cl);

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
