/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _POSIX_C_SOURCE 200809L
#include "session.h"
#include "smf.h"
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static _Atomic float g_master_tempo = 120.0f;       /* 1.1: what WARP clips follow (= s->tempo) */
#include <time.h>

// wav_convert_to_cache() is in wav.c

// ============ Internal helpers ============

// Waits for the audio thread to complete at least 2 full callbacks. After that we are sure that
// no callback that started BEFORE we turned the clip off still uses it -> it is safe to free it.
// (Replaces the old WaitTime(0.05), which only hoped.)
/* Step 35b: while the main thread waits for the audio, it services the plugins' requests (VST3 run loop etc.).
 * Some plugins (e.g. through yabridge) block the audio thread until the main thread runs one of their requests -
 * if the main thread just waits, the two wait for each other forever (the interface "freezes"). */
static void (*g_wait_pump)(void);
static pthread_t g_pump_thread;
void session_set_wait_pump(void (*fn)(void)) { g_wait_pump = fn; g_pump_thread = pthread_self(); }

static void wait_tick(void)
{
    static int depth;                                  /* no nested servicing */
    if (g_wait_pump && depth == 0 && pthread_equal(pthread_self(), g_pump_thread)) { ++depth; g_wait_pump(); --depth; }
    struct timespec ts = { 0, 500000L };
    nanosleep(&ts, NULL);
}

/* locks mix_mu without blocking the plugins (see above) */
static void mix_lock(Session *s)
{
    while (pthread_mutex_trylock(&s->mix_mu) != 0) wait_tick();
}

static void audio_barrier(Session *s)
{
    if (!s || !atomic_load(&s->audio_running)) return;
    unsigned start = atomic_load(&s->audio_cycles);
    for (int i = 0; i < 200; ++i) {
        if (atomic_load(&s->audio_cycles) - start >= 2) return;
        wait_tick();
    }
}

static void rec_abort_all(Session *s);              /* 1.4: see below */

// The disk thread asks this callback for the clip's start/end/loop.
static void clip_region_cb(void *ud, StreamRegion *out)
{
    Clip *cl = ud;
    out->start      = atomic_load(&cl->start_marker);
    out->end        = atomic_load(&cl->end_marker);
    out->loop       = atomic_load(&cl->loop);
    out->loop_start = atomic_load(&cl->loop_start);
    out->loop_end   = atomic_load(&cl->loop_end);
}

/* ===== Cutting (step 27): source wrapper =====
 * The stream reads through it: the position is on the clip's timeline, and the timeline says which part of the file to play.
 * The list is swapped at once (under streams_lock -> the disk thread is not inside a read) and the old one is freed. */
typedef struct EditSrc {
    StreamSource        inner;
    _Atomic(EditList *) list;
} EditSrc;

static size_t es_inner_read(void *ctx, size_t pos, float *out, size_t n)
{
    EditSrc *e = ctx;
    return e->inner.read(e->inner.ctx, pos, out, n);
}
static size_t es_frames(void *ctx)
{
    EditList *l = atomic_load_explicit(&((EditSrc *)ctx)->list, memory_order_acquire);
    return l ? (size_t)l->len : 0;
}
static size_t es_read(void *ctx, size_t pos, float *out, size_t n)
{
    EditSrc *e = ctx;
    EditList *l = atomic_load_explicit(&e->list, memory_order_acquire);
    if (!l) { memset(out, 0, n * 2 * sizeof(float)); return 0; }
    if (l->n == 1 && l->seg[0].t == 0 && l->seg[0].src == 0 && l->seg[0].fi1 <= l->seg[0].fi0 &&
        l->seg[0].fo1 <= l->seg[0].fo0 && l->len == l->src_len)
        return e->inner.read(e->inner.ctx, pos, out, n);      /* uncut: straight from the file */
    return edit_read(l, es_inner_read, e, (int64_t)pos, out, n);
}
static void es_close(void *ctx)
{
    EditSrc *e = ctx;
    if (e->inner.close) e->inner.close(e->inner.ctx);
    edit_free(atomic_load(&e->list));
    free(e);
}

static void clip_release_stream(Clip *cl)
{
    if (cl->bpm_job) {
        bpm_job_release(cl->bpm_job);
        cl->bpm_job = NULL;
    }
    if (cl->stretch) {
        stretch_free(cl->stretch);
        cl->stretch = NULL;
    }
    if (cl->stream) {
        stream_close(cl->stream);          /* also closes the cut wrapper */
        cl->stream = NULL;
    }
    cl->edit = NULL;
    if (cl->peaks) {
        peaks_release(cl->peaks);
        cl->peaks = NULL;
    }
    memset(&cl->wav, 0, sizeof(cl->wav));
    {   /* 1.3: the MIDI notes (the audio thread no longer sees this clip - see the callers) */
        MidiSeq *m = atomic_exchange(&cl->mseq, NULL);
        free(m);
        atomic_store(&cl->is_midi, false);
    }
}

// The Clip structs are copied on move/delete -> the streams must know the new address.
/* Moving clips in memory (inserting/deleting a channel or scene): the audio thread and the disk thread
 * must NOT read a clip while it is half copied (otherwise for a moment two slots point to the same stream,
 * or another clip's region is read -> glitches in the audio). */
static void struct_lock(Session *s)   { mix_lock(s); streams_lock(); }
static void struct_unlock(Session *s) { streams_unlock(); pthread_mutex_unlock(&s->mix_mu); }
static void rebind_all_locked(Session *s)
{
    for (int c = 0; c < s->channel_count; ++c)
        for (int sc = 0; sc < MAX_SCENES; ++sc) {
            Clip *cl = &s->channels[c].clips[sc];
            if (cl->stream) stream_rebind_nolock(cl->stream, clip_region_cb, cl);
        }
}


static void clip_start(Clip *cl, size_t from)
{
    if (atomic_load(&cl->is_midi)) {                    /* 1.3: a MIDI clip starts from its beginning */
        (void)from;
        atomic_store(&cl->xf_dir, 0);
        atomic_store(&cl->paused, false);
        atomic_store(&cl->ended, false);
        atomic_fetch_add(&cl->m_gen, 1);
        atomic_store(&cl->playing, true);
        return;
    }
    if (!cl->stream) return;
    atomic_store(&cl->xf_dir, 0);                       // manual play cancels the crossfade
    atomic_store(&cl->paused, false);
    atomic_store(&cl->ended, false);
    atomic_fetch_add(&cl->stretch_gen, 1);              // the stretch starts over from this point
    stream_start(cl->stream, from);
    atomic_store(&cl->position, stream_position(cl->stream));
    atomic_store(&cl->playing, true);
}

static Session *g_ql_session;                       /* for clip_stop: the counter of waiting launches */

static void clip_stop(Clip *cl)
{
    if (atomic_exchange(&cl->ql_state, 0) && g_ql_session) atomic_fetch_sub(&g_ql_session->ql_pending, 1);
    atomic_store(&cl->playing, false);
    atomic_store(&cl->paused, false);
    if (cl->stretch) stretch_release(cl->stretch);          // the background thread stops reading
    if (cl->stream) stream_stop(cl->stream);
}

// Turns off and frees a clip (if any) and resets it.
static void clip_discard(Session *s, Clip *cl)
{
    if (atomic_load(&cl->has_clip)) {
        atomic_store(&cl->playing, false);
        atomic_store(&cl->paused, false);
        atomic_store(&cl->has_clip, false);
        audio_barrier(s);
        clip_release_stream(cl);
    }
    if (atomic_exchange(&cl->ql_state, 0)) atomic_fetch_sub(&s->ql_pending, 1);   /* 1.1 */
    int keep_bind = atomic_load(&cl->bind);     // the pad stays bound to the cell when the clip is replaced
    mix_lock(s);                                // the audio thread does not look at the cell while it is being reset
    memset(cl, 0, sizeof(*cl));
    atomic_store(&cl->bind, keep_bind);
    pthread_mutex_unlock(&s->mix_mu);
}

// ============ Conversion helpers ============

/* Step 36: a fader like Bitwig's - +6 dB at the top, 0 dB at 80% of the travel, more and more compressed towards the bottom.
 * Points (position -> dB); between them - a straight line in dB. Below the lowest point the gain goes down to silence. */
static const float TAPER_POS[] = { 0.025f, 0.2f,   0.4f,   0.6f,   0.8f, 1.0f };
static const float TAPER_DB[]  = { -80.0f, -48.0f, -24.0f, -10.0f, 0.0f, 6.0f };
#define TAPER_N 6

float session_gain_to_db(float pos)
{
    if (pos <= 0.0001f) return -1000.0f;
    if (pos > 1.0f) pos = 1.0f;
    if (fabsf(pos - FADER_UNITY_POS) < 1e-6f) return 0.0f;
    if (pos < TAPER_POS[0])                          /* at the very bottom: from -80 dB to silence */
        return TAPER_DB[0] + 20.0f * log10f(pos / TAPER_POS[0]);
    for (int i = 1; i < TAPER_N; ++i)
        if (pos <= TAPER_POS[i]) {
            float t = (pos - TAPER_POS[i - 1]) / (TAPER_POS[i] - TAPER_POS[i - 1]);
            return TAPER_DB[i - 1] + t * (TAPER_DB[i] - TAPER_DB[i - 1]);
        }
    return TAPER_DB[TAPER_N - 1];
}

float session_pos_to_gain(float pos)
{
    if (pos <= 0.0001f) return 0.0f;
    if (fabsf(pos - FADER_UNITY_POS) < 1e-6f) return 1.0f;   /* 0 dB is EXACTLY 1.0 (no rounding) */
    return powf(10.0f, session_gain_to_db(pos) / 20.0f);
}

float session_db_to_pos(float db)
{
    if (db <= -999.0f) return 0.0f;
    if (fabsf(db) < 1e-4f) return FADER_UNITY_POS;
    if (db >= TAPER_DB[TAPER_N - 1]) return 1.0f;
    if (db < TAPER_DB[0]) {
        float p = TAPER_POS[0] * powf(10.0f, (db - TAPER_DB[0]) / 20.0f);
        return p < 0.0002f ? 0.0f : p;
    }
    for (int i = 1; i < TAPER_N; ++i)
        if (db <= TAPER_DB[i]) {
            float t = (db - TAPER_DB[i - 1]) / (TAPER_DB[i] - TAPER_DB[i - 1]);
            return TAPER_POS[i - 1] + t * (TAPER_POS[i] - TAPER_POS[i - 1]);
        }
    return 1.0f;
}

/* step 36: a value from a project older than step 36 (the old fader: linear -60..+6 dB) -> the new position */
float session_old_pos_to_pos(float old)
{
    if (old <= 0.0001f) return 0.0f;
    if (old > 1.0f) old = 1.0f;
    float db = old * 66.0f - 60.0f;
    if (fabsf(db) < 0.05f) return FADER_UNITY_POS;    /* the old "0 dB" (it was -0.006 dB) -> exactly 0 dB */
    return session_db_to_pos(db);
}

// ============ Lifecycle ============

static bool stream_eof_ud(void *ud) { return stream_finished((const ClipStream *)ud); }

void session_init(Session *s, int sample_rate)
{
    if (!s) return;
    stretch_set_eof_check(stream_eof_ud);
    memset(s, 0, sizeof(*s));
    pthread_mutex_init(&s->mix_mu, NULL);
    s->sample_rate = sample_rate;
    s->channel_count = 0;
    s->scene_count = 0;
    atomic_store(&s->master_gain, FADER_UNITY_POS);
    atomic_store(&s->master_smooth_gain, FADER_UNITY_POS);
    atomic_store(&s->master_pan, 0.0f);
    atomic_store(&s->master_smooth_pan, 0.0f);
    atomic_store(&s->master_muted, false);
    atomic_store(&s->master_peak_level, 0.0f);
    atomic_store(&s->master_midi_cc, -1);
    atomic_store(&s->master_midi_learn_armed, false);
    atomic_store(&s->master_route_l, MASTER_ROUTE_DEFAULT_L);
    atomic_store(&s->master_route_r, MASTER_ROUTE_DEFAULT_R);
    atomic_store(&s->btn_learn_kind, BTN_LEARN_NONE);
    atomic_store(&s->audio_cycles, 0);
    atomic_store(&s->audio_running, false);
    atomic_store(&s->tempo, 120.0f);
    atomic_store(&g_master_tempo, 120.0f);
    atomic_store(&s->quant_beats, 4.0f);               /* 1.1: default = the next bar */
    atomic_store(&s->tp_beats, 0.0);
    atomic_store(&s->tp_running, false);
    atomic_store(&s->ql_pending, 0);
    g_ql_session = s;
    atomic_store(&s->midi_target, -1);
    for (int i = 0; i < 4; ++i) atomic_store(&s->midi_bound[i], 0u);
    atomic_store(&s->midi_block, false);
    atomic_store(&s->midi_panic, false);
    s->queue_len = 0;
    s->last_started = -1;
    atomic_store(&s->q_anchor, -1);
    atomic_store(&s->q_next, -1);
    atomic_store(&s->q_launched, 0);
}

void session_release_all_clips(Session *s)
{
    rec_abort_all(s);                               /* 1.4: the cells move - a running recording ends here */
    if (!s) return;
    session_queue_clear(s);
    for (int c = 0; c < s->channel_count; ++c)
        for (int sc = 0; sc < MAX_SCENES; ++sc) {
            Clip *cl = &s->channels[c].clips[sc];
            atomic_store(&cl->playing, false);
            atomic_store(&cl->paused, false);
            atomic_store(&cl->has_clip, false);
            atomic_store(&cl->bind, 0);
        }
    audio_barrier(s);
    for (int c = 0; c < s->channel_count; ++c)
        for (int sc = 0; sc < MAX_SCENES; ++sc)
            clip_release_stream(&s->channels[c].clips[sc]);
}

void session_free(Session *s)
{
    if (!s) return;
    session_release_all_clips(s);
    s->channel_count = 0;
    s->scene_count = 0;
    streams_shutdown();
    peaks_shutdown();
    bpm_shutdown();
}

// ============ Channels ============

void session_channel_init(Channel *c, int idx, const char *name)
{
    memset(c, 0, sizeof(*c));

    atomic_store(&c->gain, FADER_UNITY_POS);
    atomic_store(&c->smooth_gain, FADER_UNITY_POS);
    atomic_store(&c->pan, 0.0f);
    atomic_store(&c->smooth_pan, 0.0f);
    atomic_store(&c->peak_level, 0.0f);
    atomic_store(&c->muted, false);
    atomic_store(&c->soloed, false);
    atomic_store(&c->midi_cc, -1);
    atomic_store(&c->midi_learn_armed, false);
    atomic_store(&c->pan_midi_cc, -1);
    atomic_store(&c->pan_midi_learn_armed, false);
    atomic_store(&c->mute_bind, 0);
    atomic_store(&c->solo_bind, 0);
    atomic_store(&c->route_l, ROUTE_DEFAULT_L);
    atomic_store(&c->route_r, ROUTE_DEFAULT_R);
    atomic_store(&c->rec_in_stereo, true);           /* 1.4: records inputs 1+2 by default */
    /* step 36h: the first channel is in the Q-LiM orange, then colours that look good next to it */
    static const int ORDER[8] = { 4, 1, 2, 5, 3, 7, 0, 6 };
    c->color_id = ORDER[idx % 8];
    c->width = 0;
    c->fx_count = 0;

    if (name) {
        snprintf(c->name, sizeof(c->name), "%s", name);
    } else {
        snprintf(c->name, sizeof(c->name), "Ch %d", idx + 1);
    }
}

int session_add_channel(Session *s, const char *name)
{
    if (!s || s->channel_count >= MAX_CHANNELS) return -1;
    int idx = s->channel_count;
    Channel *c = &s->channels[idx];
    session_channel_init(c, idx, name);

    s->channel_count++;
    printf("session_add_channel: [%d] '%s'\n", idx, c->name);
    return idx;
}

void session_set_channel_color(Session *s, int ch, int idx)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    if (idx < 0 || idx >= N_PALETTE) return;
    s->channels[ch].color_id = idx;
}

void session_rename_channel(Session *s, int ch, const char *new_name)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    if (!new_name || new_name[0] == '\0') return;
    snprintf(s->channels[ch].name, sizeof(s->channels[ch].name), "%s", new_name);
}

void session_set_note_color(Session *s, int ch, int scene, int color)
{
    if (!s || ch < 0 || ch >= s->channel_count || scene < 0 || scene >= MAX_SCENES) return;
    if (color < 0 || color > N_PALETTE) return;
    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip) || !atomic_load(&cl->is_note)) return;
    cl->color = color;
}

int session_get_note_color(Session *s, int ch, int scene)
{
    if (!s || ch < 0 || ch >= s->channel_count || scene < 0 || scene >= MAX_SCENES) return 0;
    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip) || !atomic_load(&cl->is_note)) return 0;
    return cl->color;
}

void session_set_channel_width(Session *s, int ch, int width)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    if (width != 0) {
        if (width < CH_WIDTH_MIN) width = CH_WIDTH_MIN;
        if (width > CH_WIDTH_MAX) width = CH_WIDTH_MAX;
    }
    s->channels[ch].width = width;
}

int session_get_channel_width(Session *s, int ch)
{
    if (!s || ch < 0 || ch >= s->channel_count) return CH_WIDTH_DEFAULT;
    int w = s->channels[ch].width;
    if (w <= 0) w = CH_WIDTH_DEFAULT;
    if (w < CH_WIDTH_MIN) w = CH_WIDTH_MIN;
    if (w > CH_WIDTH_MAX) w = CH_WIDTH_MAX;
    return w;
}

void session_set_channel_gain(Session *s, int ch, float g)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    if (g < 0.0f) g = 0.0f;
    if (g > 1.0f) g = 1.0f;
    atomic_store(&s->channels[ch].gain, g);
}

void session_set_channel_pan(Session *s, int ch, float p)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    if (p < -1.0f) p = -1.0f;
    if (p >  1.0f) p =  1.0f;
    atomic_store(&s->channels[ch].pan, p);
}

void session_toggle_mute(Session *s, int ch)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    bool was = atomic_load(&s->channels[ch].muted);
    atomic_store(&s->channels[ch].muted, !was);
}

void session_toggle_solo(Session *s, int ch)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    bool was = atomic_load(&s->channels[ch].soloed);
    atomic_store(&s->channels[ch].soloed, !was);
}

bool session_any_soloed(Session *s)
{
    if (!s) return false;
    for (int c = 0; c < s->channel_count; ++c) {
        if (atomic_load(&s->channels[c].soloed)) return true;
    }
    return false;
}

bool session_channel_audible(Session *s, int ch)
{
    if (!s || ch < 0 || ch >= s->channel_count) return false;

    bool muted = atomic_load(&s->channels[ch].muted);
    bool soloed = atomic_load(&s->channels[ch].soloed);
    bool any_solo = session_any_soloed(s);

    if (muted) return false;
    if (any_solo && !soloed) return false;
    return true;
}

// ============ Clips ============

int session_attach_stream(Session *s, int ch, int scene,
                          const StreamSource *src, const char *display_path)
{
    if (!s || !src || ch < 0 || ch >= s->channel_count ||
        scene < 0 || scene >= MAX_SCENES) {
        if (src && src->close) src->close(src->ctx);
        return 1;
    }

    Clip *cl = &s->channels[ch].clips[scene];
    clip_discard(s, cl);

    /* every clip goes through the cut wrapper (uncut = straight from the file, bit-exact) */
    EditSrc *es = calloc(1, sizeof *es);
    EditList *el = es ? edit_new((int64_t)src->frames(src->ctx)) : NULL;
    if (!es || !el) {
        free(es); edit_free(el);
        if (src->close) src->close(src->ctx);
        return 1;
    }
    es->inner = *src;
    atomic_store(&es->list, el);
    StreamSource wrapped = { es_frames, es_read, es_close, es };

    cl->stream = stream_open_path(&wrapped, clip_region_cb, cl, display_path);
    if (!cl->stream) {
        es_close(es);
        return 1;
    }
    cl->edit = es;

    int sr = s->sample_rate > 0 ? s->sample_rate : 48000;
    size_t frames = stream_frames(cl->stream);

    cl->wav.samples     = NULL;
    cl->wav.frame_count = frames;
    cl->wav.sample_rate = sr;
    cl->wav.channels    = 2;
    snprintf(cl->wav.path, sizeof(cl->wav.path), "%s", display_path ? display_path : "");

    atomic_store(&cl->position, 0);
    atomic_store(&cl->playing, false);
    atomic_store(&cl->paused, false);
    atomic_store(&cl->loop, false);
    atomic_store(&cl->loop_start, 0);
    atomic_store(&cl->loop_end, frames);
    atomic_store(&cl->start_marker, 0);
    atomic_store(&cl->end_marker, frames);
    atomic_store(&cl->wave_zoom, 1.0f);
    atomic_store(&cl->wave_scroll_frame, 0);
    atomic_store(&cl->is_note, false);

    const char *p = display_path ? display_path : "";
    const char *slash = strrchr(p, '/');
    snprintf(cl->name, sizeof(cl->name), "%s", slash ? slash + 1 : p);
    snprintf(cl->path, sizeof(cl->path), "%s", p);

    atomic_store(&cl->has_clip, true);
    printf("session_set_clip: [%d][%d] = '%s' (%zu frames, streamed)\n",
           ch, scene, cl->name, frames);
    return 0;
}

void session_set_convert_files(Session *s, bool on)
{
    if (s) atomic_store(&s->convert_files, on);
}

// Opens a source for the clip. "As is" (the default): a file with a different sample rate is resampled live.
// "Convert": a copy is made in the cache. A format libsndfile cannot read -> ffmpeg (if present).
// Returns 0 on success; *open_path = the file the waveform sketch must be computed from.
static int open_clip_source(Session *s, const char *path, int sr,
                            StreamSource *src, char *cache, size_t cache_size,
                            const char **open_path)
{
    *open_path = path;
    int fsr = 0;
    StreamSource nat;

    if (stream_source_sndfile_native(path, &nat, &fsr) != 0) {
        // libsndfile cannot read it
        printf("session_set_clip: '%s' is not readable directly, trying ffmpeg\n", path);
        if (wav_convert_with_ffmpeg(path, sr, cache, cache_size) != 0) return 1;
        *open_path = cache;
        return stream_source_open(cache, sr, src, &fsr);
    }

    int must_cache = stream_file_needs_cache(path);    // MP3: no exact seek
    if (fsr == sr && !must_cache) {                    // already at the needed rate
        *src = nat;
        return 0;
    }

    if (atomic_load(&s->convert_files) || must_cache) {
        printf("session_set_clip: '%s' is %d Hz, converting to %d Hz (cached copy)\n", path, fsr, sr);
        if (wav_convert_to_cache(path, sr, cache, cache_size) == 0) {
            nat.close(nat.ctx);
            *open_path = cache;
            return stream_source_open(cache, sr, src, &fsr);
        }
        printf("session_set_clip: conversion failed, playing as is\n");
    }

    printf("session_set_clip: '%s' is %d Hz, playing as is (live resampling to %d Hz)\n", path, fsr, sr);
    if (stream_source_resample(&nat, fsr, sr, src) != 0) {
        nat.close(nat.ctx);
        return 1;
    }
    return 0;
}

int session_set_clip(Session *s, int ch, int scene, const char *wav_path)
{
    if (smf_is_midi_path(wav_path)) {                /* 1.3: a .mid file -> a MIDI clip with its notes */
        char err[160];
        MidiSeq *m = smf_load(wav_path, err, sizeof err);
        if (!m) { printf("session_set_clip: '%s': %s\n", wav_path, err); return 1; }
        const char *b = strrchr(wav_path, '/');
        char nm[NAME_LEN];
        snprintf(nm, sizeof nm, "%s", b ? b + 1 : wav_path);
        char *dot = strrchr(nm, '.');
        if (dot) *dot = 0;
        if (session_new_midi_clip(s, ch, scene, m->length, nm) != 0) { free(m); return 1; }
        session_midi_set(s, ch, scene, m);
        return 0;
    }
    if (!s || ch < 0 || ch >= s->channel_count) return 1;
    if (scene < 0 || scene >= MAX_SCENES) return 1;

    clip_discard(s, &s->channels[ch].clips[scene]);

    int sr = s->sample_rate > 0 ? s->sample_rate : 48000;

    StreamSource src;
    char cache[1024];
    const char *open_path = wav_path;
    if (open_clip_source(s, wav_path, sr, &src, cache, sizeof(cache), &open_path) != 0) return 1;

    int arc = session_attach_stream(s, ch, scene, &src, wav_path);
    if (arc == 0) {
        Clip *cl = &s->channels[ch].clips[scene];
        cl->peaks = peaks_start(open_path, sr, cl->wav.frame_count);   // NULL is OK: there is simply no waveform
        cl->bpm_job = bpm_job_start(open_path);                         // the tempo is detected in the background
        atomic_store(&cl->bpm_busy, cl->bpm_job != NULL);
    }
    return arc;
}

typedef struct {
    int      ch, sc;
    char     path[1024], name[NAME_LEN];
    size_t   frames, loop_start, loop_end, start, end, scroll;
    bool     loop, warp;
    float    zoom, bpm_det, bpm_fix, bpm_play;
    char    *edits;              /* step 27: the parts (malloc, NULL = not cut) */
} SavedClip;

int session_set_sample_rate(Session *s, int sr)
{
    if (!s || sr <= 0) return 0;
    int old_sr = s->sample_rate > 0 ? s->sample_rate : 48000;
    if (old_sr == sr) { s->sample_rate = sr; return 0; }

    // 1) remember everything, 2) stop, 3) change the rate, 4) reload with scaled positions
    int cap = 0;
    SavedClip *saved = NULL;
    for (int c = 0; c < s->channel_count; ++c)
        for (int sc = 0; sc < MAX_SCENES; ++sc) {
            Clip *cl = &s->channels[c].clips[sc];
            if (!atomic_load(&cl->has_clip) || atomic_load(&cl->is_note) || !cl->path[0]) continue;
            SavedClip *n = realloc(saved, (size_t)(cap + 1) * sizeof(*saved));
            if (!n) { for (int i = 0; i < cap; ++i) free(saved[i].edits); free(saved); return -1; }
            saved = n;
            SavedClip *sv = &saved[cap++];
            sv->ch = c; sv->sc = sc;
            snprintf(sv->path, sizeof(sv->path), "%s", cl->path);
            snprintf(sv->name, sizeof(sv->name), "%s", cl->name);
            sv->frames     = cl->wav.frame_count;
            sv->loop       = atomic_load(&cl->loop);
            sv->loop_start = atomic_load(&cl->loop_start);
            sv->loop_end   = atomic_load(&cl->loop_end);
            sv->start      = atomic_load(&cl->start_marker);
            sv->end        = atomic_load(&cl->end_marker);
            sv->zoom       = atomic_load(&cl->wave_zoom);
            sv->scroll     = atomic_load(&cl->wave_scroll_frame);
            sv->warp       = atomic_load(&cl->warp);
            sv->bpm_det    = atomic_load(&cl->bpm_detected);
            sv->bpm_fix    = atomic_load(&cl->bpm_fix);
            sv->bpm_play   = atomic_load(&cl->bpm_play);
            sv->edits      = session_clip_edits_text(s, c, sc);
        }

    session_stop_all(s);
    s->sample_rate = sr;

    double k = (double)sr / (double)old_sr;
    int failed = 0;
    for (int i = 0; i < cap; ++i) {
        SavedClip *sv = &saved[i];
        if (session_set_clip(s, sv->ch, sv->sc, sv->path) != 0) { ++failed; continue; }
        Clip *cl = &s->channels[sv->ch].clips[sv->sc];
        if (sv->edits) {                                   /* the parts - scaled to the new rate */
            const EditList *now = session_clip_edits(s, sv->ch, sv->sc);
            EditList *old = edit_from_text(sv->edits, INT64_MAX);
            EditList *sc_ = (old && now) ? edit_scale(old, k, now->src_len) : NULL;
            edit_free(old);
            if (sc_) session_clip_apply_edits(s, sv->ch, sv->sc, sc_, -1);
        }
        size_t nf = cl->wav.frame_count;
        #define SCALE(v) ((size_t)((double)(v) * k + 0.5))
        size_t ls = SCALE(sv->loop_start), le = (sv->loop_end >= sv->frames) ? nf : SCALE(sv->loop_end);
        size_t st = SCALE(sv->start),      en = (sv->end >= sv->frames)      ? nf : SCALE(sv->end);
        #undef SCALE
        if (le > nf) le = nf;
        if (ls > le) ls = le;
        if (en > nf) en = nf;
        if (st > en) st = en;
        atomic_store(&cl->loop_start, ls);
        atomic_store(&cl->loop_end, le);
        atomic_store(&cl->loop, sv->loop);
        atomic_store(&cl->start_marker, st);
        atomic_store(&cl->end_marker, en);
        atomic_store(&cl->wave_zoom, sv->zoom);
        atomic_store(&cl->wave_scroll_frame, (size_t)(int64_t)llround((double)(int64_t)sv->scroll * k));
        snprintf(cl->name, sizeof(cl->name), "%s", sv->name);
        session_set_clip_bpm_fields(s, sv->ch, sv->sc, sv->bpm_det, sv->bpm_fix, sv->bpm_play);
        if (sv->warp) session_set_clip_warp(s, sv->ch, sv->sc, true);   // a new stretch for the new rate
        stream_invalidate_preroll(cl->stream);
    }
    for (int i = 0; i < cap; ++i) free(saved[i].edits);
    free(saved);
    printf("session_set_sample_rate: %d -> %d Hz (%d clips reloaded, %d failed)\n", old_sr, sr, cap - failed, failed);
    return failed;
}

int session_set_note(Session *s, int ch, int scene, const char *text)
{
    if (!s || ch < 0 || ch >= s->channel_count) return 1;
    if (scene < 0 || scene >= MAX_SCENES) return 1;

    Clip *cl = &s->channels[ch].clips[scene];
    clip_discard(s, cl);

    atomic_store(&cl->has_clip, true);
    atomic_store(&cl->is_note, true);
    atomic_store(&cl->playing, false);
    atomic_store(&cl->paused, false);
    atomic_store(&cl->loop, false);
    atomic_store(&cl->loop_start, 0);
    atomic_store(&cl->loop_end, 0);
    atomic_store(&cl->wave_zoom, 1.0f);
    atomic_store(&cl->wave_scroll_frame, 0);

    if (text && text[0] != '\0') {
        snprintf(cl->name, sizeof(cl->name), "%s", text);
    } else {
        snprintf(cl->name, sizeof(cl->name), "Note");
    }
    cl->path[0] = '\0';

    printf("session_set_note: [%d][%d] = '%s'\n", ch, scene, cl->name);
    return 0;
}

/* ===== 1.3: MIDI clips ===== */
MidiSeq *midiseq_new(int n, float length)
{
    if (n < 0) n = 0;
    MidiSeq *m = malloc(sizeof(MidiSeq) + sizeof(MNote) * (size_t)(n > 0 ? n : 1));
    if (!m) return NULL;
    m->length = length > 0.0f ? length : 4.0f;
    m->n = n;
    return m;
}

MidiSeq *midiseq_dup(const MidiSeq *src, int extra)
{
    int n = src ? src->n : 0;
    MidiSeq *m = midiseq_new(n + (extra > 0 ? extra : 0), src ? src->length : 4.0f);
    if (!m) return NULL;
    m->n = n;
    if (n) memcpy(m->note, src->note, sizeof(MNote) * (size_t)n);
    return m;
}

static int mnote_cmp(const void *a, const void *b)
{
    const MNote *x = a, *y = b;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return (int)x->key - (int)y->key;
}
void midiseq_sort(MidiSeq *m) { if (m && m->n > 1) qsort(m->note, (size_t)m->n, sizeof(MNote), mnote_cmp); }

/* old note lists wait here until the audio thread has certainly finished with them */
#define MRET_N 256
static struct { MidiSeq *m; Session *s; unsigned cyc; } g_mret[MRET_N];
static int g_nmret;

void session_midi_reap(Session *s)
{
    for (int i = 0; i < g_nmret; ) {
        bool done = g_mret[i].s != s ? false
                  : (!atomic_load(&s->audio_running) || atomic_load(&s->audio_cycles) - g_mret[i].cyc >= 2);
        if (done) { free(g_mret[i].m); g_mret[i] = g_mret[--g_nmret]; }
        else ++i;
    }
}

static void midi_retire(Session *s, MidiSeq *old)
{
    if (!old) return;
    if (!atomic_load(&s->audio_running)) { free(old); return; }
    if (g_nmret >= MRET_N) { audio_barrier(s); session_midi_reap(s); }
    if (g_nmret >= MRET_N) { audio_barrier(s); free(old); return; }
    g_mret[g_nmret].m = old; g_mret[g_nmret].s = s; g_mret[g_nmret].cyc = atomic_load(&s->audio_cycles);
    g_nmret++;
}

int session_new_midi_clip(Session *s, int ch, int scene, float len_beats, const char *name)
{
    if (!s || ch < 0 || ch >= s->channel_count || scene < 0 || scene >= MAX_SCENES) return 1;
    MidiSeq *m = midiseq_new(0, len_beats);
    if (!m) return 1;
    Clip *cl = &s->channels[ch].clips[scene];
    clip_discard(s, cl);
    for (int k = 0; k < 128; ++k) cl->m_off[k] = -1.0f;
    atomic_store(&cl->mseq, m);
    atomic_store(&cl->is_midi, true);
    atomic_store(&cl->is_note, false);
    atomic_store(&cl->playing, false);
    atomic_store(&cl->paused, false);
    atomic_store(&cl->loop, true);                   /* a MIDI clip loops by default (like in Ableton/Bitwig) */
    atomic_store(&cl->mpos_vis, -1.0);
    snprintf(cl->name, sizeof cl->name, "%s", name && name[0] ? name : "MIDI");
    cl->path[0] = 0;
    atomic_store(&cl->has_clip, true);
    printf("session_new_midi_clip: [%d][%d] '%s' (%.0f beats)\n", ch, scene, cl->name, (double)m->length);
    return 0;
}

bool session_is_midi(Session *s, int ch, int scene)
{
    if (!s || ch < 0 || ch >= s->channel_count || scene < 0 || scene >= MAX_SCENES) return false;
    Clip *cl = &s->channels[ch].clips[scene];
    return atomic_load(&cl->has_clip) && atomic_load(&cl->is_midi);
}

const MidiSeq *session_midi_get(Session *s, int ch, int scene)
{
    if (!session_is_midi(s, ch, scene)) return NULL;
    return atomic_load(&s->channels[ch].clips[scene].mseq);
}

void session_midi_set(Session *s, int ch, int scene, MidiSeq *seq)
{
    if (!seq) return;
    if (!session_is_midi(s, ch, scene)) { free(seq); return; }
    if (seq->length < 0.25f) seq->length = 0.25f;
    if (seq->length > 4096.0f) seq->length = 4096.0f;
    if (seq->n > MIDI_MAX_NOTES) seq->n = MIDI_MAX_NOTES;
    midiseq_sort(seq);
    MidiSeq *old = atomic_exchange(&s->channels[ch].clips[scene].mseq, seq);
    if (old != seq) midi_retire(s, old);
}

double session_midi_playhead(Session *s, int ch, int scene)
{
    if (!session_is_midi(s, ch, scene)) return -1.0;
    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->playing)) return -1.0;
    return atomic_load(&cl->mpos_vis);
}

/* text form (project file and undo): "midi_len=16\nmn=0 0.5 60 100\n..." (start length key velocity) */
char *session_midi_text(Session *s, int ch, int scene)
{
    const MidiSeq *m = session_midi_get(s, ch, scene);
    if (!m) return NULL;
    size_t cap = 64 + (size_t)m->n * 40, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    len += (size_t)snprintf(out + len, cap - len, "midi_len=%g\n", (double)m->length);
    for (int i = 0; i < m->n; ++i)
        len += (size_t)snprintf(out + len, cap - len, "mn=%g %g %d %d\n", (double)m->note[i].start, (double)m->note[i].len,
                                m->note[i].key, m->note[i].vel);
    return out;
}

MidiSeq *midiseq_from_text(const char *text)
{
    if (!text) return NULL;
    float length = -1.0f;
    int cnt = 0;
    for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
        if (!strncmp(p, "midi_len=", 9)) length = (float)atof(p + 9);
        else if (!strncmp(p, "mn=", 3)) ++cnt;
    }
    if (length <= 0.0f) return NULL;
    if (cnt > MIDI_MAX_NOTES) cnt = MIDI_MAX_NOTES;
    MidiSeq *m = midiseq_new(cnt, length);
    if (!m) return NULL;
    m->n = 0;
    for (const char *p = text; p && *p && m->n < cnt; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
        if (strncmp(p, "mn=", 3)) continue;
        double a, b; int k, v;
        if (sscanf(p + 3, "%lf %lf %d %d", &a, &b, &k, &v) != 4) continue;
        if (k < 0 || k > 127 || b <= 0.0 || a < 0.0) continue;
        if (v < 1) v = 1;
        if (v > 127) v = 127;
        MNote *q = &m->note[m->n++];
        q->start = (float)a; q->len = (float)b; q->key = (unsigned char)k; q->vel = (unsigned char)v; q->sel = 0;
    }
    midiseq_sort(m);
    return m;
}

int session_midi_set_text(Session *s, int ch, int scene, const char *text)
{
    MidiSeq *m = midiseq_from_text(text);
    if (!m) return 1;
    if (!session_is_midi(s, ch, scene)) {
        char nm[NAME_LEN] = "MIDI";
        if (s && ch >= 0 && ch < s->channel_count && scene >= 0 && scene < MAX_SCENES &&
            atomic_load(&s->channels[ch].clips[scene].has_clip))
            snprintf(nm, sizeof nm, "%s", s->channels[ch].clips[scene].name);
        if (session_new_midi_clip(s, ch, scene, m->length, nm) != 0) { free(m); return 1; }
    }
    session_midi_set(s, ch, scene, m);
    return 0;
}

bool session_is_note(Session *s, int ch, int scene)
{
    if (!s || ch < 0 || ch >= s->channel_count) return false;
    if (scene < 0 || scene >= MAX_SCENES) return false;

    Clip *cl = &s->channels[ch].clips[scene];
    return atomic_load(&cl->has_clip) && atomic_load(&cl->is_note);
}

void session_clear_clip(Session *s, int ch, int scene)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    if (scene < 0 || scene >= MAX_SCENES) return;

    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip)) return;

    clip_discard(s, cl);
    atomic_store(&cl->bind, 0);                 // explicit delete -> remove the pad too
    printf("session_clear_clip: [%d][%d]\n", ch, scene);
}

// Pressing a clip: if it is playing -> restart; otherwise stop the others on the channel and play this one.
/* ===== 1.1: quantized launch =====
 * While the transport runs (something plays) and quantization is on, a launch does not happen at once: the clip is
 * prepared (stream at the start, Rubber Band computing ahead) and the AUDIO thread starts it exactly at the next
 * multiple of quant_beats (sample accurate, see mixer.c ql_process). The beat grid comes from the master tempo. */
static size_t ql_pull(void *ud, float *out, size_t n) { return stream_read((ClipStream *)ud, out, n); }

/* the beat of the next boundary, or -1 = launch at once (no quantization or nothing plays) */
static double ql_target(Session *s)
{
    float q = atomic_load(&s->quant_beats);
    if (q <= 0.0f || !atomic_load(&s->tp_running) || !session_any_playing(s)) {
        if (!session_any_playing(s)) atomic_store(&s->tp_reset, true);   /* nothing plays: this launch is beat 1 */
        return -1.0;
    }
    double tp = atomic_load(&s->tp_beats);
    double b = ceil(tp / (double)q - 1e-9) * (double)q;
    if (b < tp) b = tp;
    return b;
}

static void ql_schedule(Session *s, Clip *cl, int state, double beat)
{
    atomic_store(&cl->ql_beat, beat);
    if (atomic_exchange(&cl->ql_state, state) == 0) atomic_fetch_add(&s->ql_pending, 1);
}

/* the clip waits silently, ready to sound at once (as the queue prepares its next clip) */
static void ql_prepare(Clip *cl)
{
    atomic_store(&cl->paused, false);
    atomic_store(&cl->ended, false);
    atomic_store(&cl->xf_dir, 0);
    cl->start_offset = 0;
    if (atomic_load(&cl->is_midi)) { atomic_fetch_add(&cl->m_gen, 1); return; }   /* 1.3: nothing to read ahead */
    unsigned g = atomic_fetch_add(&cl->stretch_gen, 1) + 1;
    stream_start(cl->stream, STREAM_FROM_START);
    atomic_store(&cl->position, stream_position(cl->stream));
    if (atomic_load(&cl->warp) && cl->stretch && stretch_threaded(cl->stretch)) {
        cl->stretch_seen = g;
        cl->stretch_live = true;
        stretch_prepare(cl->stretch, session_clip_speed(cl), ql_pull, cl->stream);
    }
}

/* a newer press in the same channel wins: the older waiting launches of that channel are cancelled */
static void ql_cancel_channel(Session *s, int ch, const Clip *keep)
{
    for (int sc = 0; sc < MAX_SCENES; ++sc) {
        Clip *o = &s->channels[ch].clips[sc];
        if (o == keep) continue;
        int st = atomic_load(&o->ql_state);
        if (!st) continue;
        if (atomic_exchange(&o->ql_state, 0)) atomic_fetch_sub(&s->ql_pending, 1);
        if (st == 1 && !atomic_load(&o->playing)) {
            if (o->stretch) stretch_release(o->stretch);
            if (o->stream) stream_stop(o->stream);
        }
    }
}

/* launch (or restart) one clip at the beat qb */
static void ql_launch_clip(Session *s, int ch, Clip *cl, double qb)
{
    ql_cancel_channel(s, ch, cl);
    if (atomic_load(&cl->playing)) ql_schedule(s, cl, 3, qb);
    else { ql_prepare(cl); ql_schedule(s, cl, 1, qb); }
}

void session_quant_tick(Session *s)
{
    if (!s) return;
    for (int c = 0; c < s->channel_count; ++c)
        for (int sc = 0; sc < s->scene_count; ++sc) {
            Clip *cl = &s->channels[c].clips[sc];
            if (atomic_exchange(&cl->ql_restart_now, false) && atomic_load(&cl->has_clip)) clip_start(cl, STREAM_FROM_START);
            if (atomic_exchange(&cl->ql_stopped, false) && !atomic_load(&cl->playing) && !atomic_load(&cl->ql_state))
                if (cl->stream) stream_stop(cl->stream);
        }
}

void session_toggle_clip(Session *s, int ch, int scene)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    if (scene < 0 || scene >= MAX_SCENES) return;

    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip)) return;
    if (atomic_load(&cl->is_note)) return;

    session_queue_clear(s);                         // manual play -> the queue is cleared
    s->last_started = ch * MAX_SCENES + scene;

    double qb = ql_target(s);
    if (qb >= 0.0) { ql_launch_clip(s, ch, cl, qb); return; }   /* 1.1: waits for the next beat/bar */

    if (!atomic_load(&cl->playing)) {
        for (int sc = 0; sc < s->scene_count; ++sc) {
            if (sc == scene) continue;
            Clip *other = &s->channels[ch].clips[sc];
            if (atomic_load(&other->has_clip) && !atomic_load(&other->is_note))
                clip_stop(other);
        }
    }
    clip_start(cl, STREAM_FROM_START);
}

void session_play_clip_from(Session *s, int ch, int scene, size_t frame)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    if (scene < 0 || scene >= MAX_SCENES) return;
    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip) || atomic_load(&cl->is_note) || !cl->stream) return;
    if (frame > cl->wav.frame_count) frame = cl->wav.frame_count;
    if (atomic_load(&cl->playing)) { session_seek_clip(s, ch, scene, frame); return; }
    session_queue_clear(s);                         // manual play -> the queue is cleared
    s->last_started = ch * MAX_SCENES + scene;
    for (int sc = 0; sc < s->scene_count; ++sc) {
        if (sc == scene) continue;
        Clip *other = &s->channels[ch].clips[sc];
        if (atomic_load(&other->has_clip) && !atomic_load(&other->is_note)) clip_stop(other);
    }
    clip_start(cl, frame);
}

void session_midi_play_from(Session *s, int ch, int scene, double beat)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    if (scene < 0 || scene >= MAX_SCENES) return;
    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip) || !atomic_load(&cl->is_midi)) return;
    if (beat < 0.0) beat = 0.0;
    bool was = atomic_load(&cl->playing);
    if (!was) {
        session_queue_clear(s);
        s->last_started = ch * MAX_SCENES + scene;
        for (int sc = 0; sc < s->scene_count; ++sc) {
            if (sc == scene) continue;
            Clip *other = &s->channels[ch].clips[sc];
            if (atomic_load(&other->has_clip) && !atomic_load(&other->is_note)) clip_stop(other);
        }
    }
    atomic_store(&cl->m_seek, beat);
    atomic_store(&cl->m_seek_gen, atomic_load(&cl->m_gen) + 1);      /* the start that follows begins at "beat" */
    if (!atomic_load(&s->tp_running)) atomic_store(&s->tp_start, beat);   /* the beat count (and metronome) follow */
    if (was) atomic_fetch_add(&cl->m_gen, 1);
    else clip_start(cl, 0);
}

void session_seek_clip(Session *s, int ch, int scene, size_t frame)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    if (scene < 0 || scene >= MAX_SCENES) return;

    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip)) return;
    if (atomic_load(&cl->is_note)) return;
    if (frame > cl->wav.frame_count) frame = cl->wav.frame_count;

    if (cl->stream && (atomic_load(&cl->playing) || atomic_load(&cl->paused))) {
        atomic_fetch_add(&cl->stretch_gen, 1);
        stream_start(cl->stream, frame);
    }
    atomic_store(&cl->position, frame);
}

void session_move_clip(Session *s,
                       int src_ch, int src_scene,
                       int dst_ch, int dst_scene)
{
    if (!s) return;
    if (src_ch < 0 || src_ch >= s->channel_count) return;
    if (dst_ch < 0 || dst_ch >= s->channel_count) return;
    if (src_scene < 0 || src_scene >= MAX_SCENES) return;
    if (dst_scene < 0 || dst_scene >= MAX_SCENES) return;
    if (src_ch == dst_ch && src_scene == dst_scene) return;

    Clip *src = &s->channels[src_ch].clips[src_scene];
    Clip *dst = &s->channels[dst_ch].clips[dst_scene];

    if (!atomic_load(&src->has_clip)) return;

    session_queue_clear(s);                         // moving = a manual action; stops both clips

    clip_stop(src);
    clip_stop(dst);

    if (atomic_load(&dst->has_clip)) {
        atomic_store(&dst->has_clip, false);
        audio_barrier(s);
        clip_release_stream(dst);
    }

    bool src_is_note = atomic_load(&src->is_note);

    atomic_store(&src->has_clip, false);
    audio_barrier(s);

    int dst_bind = atomic_load(&dst->bind);       // the MIDI pad belongs to the cell, not to the clip
    int src_bind = atomic_load(&src->bind);
    struct_lock(s);                                // the copy must not be seen half done
    *dst = *src;
    atomic_store(&dst->bind, dst_bind);
    if (dst->stream) stream_rebind_nolock(dst->stream, clip_region_cb, dst);
    memset(src, 0, sizeof(*src));
    atomic_store(&src->bind, src_bind);
    struct_unlock(s);

    atomic_store(&dst->has_clip, true);
    atomic_store(&dst->is_note, src_is_note);
    atomic_store(&dst->playing, false);
    atomic_store(&dst->paused, false);
    atomic_store(&dst->position, 0);

    printf("session_move_clip: [%d][%d] -> [%d][%d] ('%s')\n",
           src_ch, src_scene, dst_ch, dst_scene, dst->name);
}

void session_rename_clip(Session *s, int ch, int scene, const char *new_name)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    if (scene < 0 || scene >= MAX_SCENES) return;
    if (!new_name || new_name[0] == '\0') return;

    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip)) return;

    snprintf(cl->name, sizeof(cl->name), "%s", new_name);
    printf("session_rename_clip: [%d][%d] = '%s'\n", ch, scene, cl->name);
}

// ============ Delete ============

void session_delete_channel(Session *s, int ch)
{
    rec_abort_all(s);                               /* 1.4: the cells move - a running recording ends here */
    if (!s || ch < 0 || ch >= s->channel_count) return;
    session_queue_clear(s);                         // the cell numbers shift

    for (int sc = 0; sc < MAX_SCENES; ++sc) {
        Clip *cl = &s->channels[ch].clips[sc];
        atomic_store(&cl->playing, false);
        atomic_store(&cl->paused, false);
        atomic_store(&cl->has_clip, false);
    }
    audio_barrier(s);
    for (int sc = 0; sc < MAX_SCENES; ++sc)
        clip_release_stream(&s->channels[ch].clips[sc]);

    struct_lock(s);
    for (int c = ch; c < s->channel_count - 1; ++c) {
        s->channels[c] = s->channels[c + 1];
    }
    int last = s->channel_count - 1;
    s->channel_count--;

    rebind_all_locked(s);                            // first the streams point to the new addresses...
    memset(&s->channels[last], 0, sizeof(Channel));  // ...and only then do we clear the freed slot
    struct_unlock(s);

    printf("session_delete_channel: [%d] (count now %d)\n",
           ch, s->channel_count);
}

void session_delete_scene(Session *s, int sc)
{
    rec_abort_all(s);                               /* 1.4: the cells move - a running recording ends here */
    if (!s || sc < 0 || sc >= s->scene_count) return;
    session_queue_clear(s);                         // the cell numbers shift

    for (int c = 0; c < s->channel_count; ++c) {
        Clip *cl = &s->channels[c].clips[sc];
        atomic_store(&cl->playing, false);
        atomic_store(&cl->paused, false);
        atomic_store(&cl->has_clip, false);
    }
    audio_barrier(s);

    for (int c = 0; c < s->channel_count; ++c)
        clip_release_stream(&s->channels[c].clips[sc]);

    struct_lock(s);
    for (int c = 0; c < s->channel_count; ++c) {
        Channel *chp = &s->channels[c];
        for (int i = sc; i < s->scene_count - 1; ++i) {
            chp->clips[i] = chp->clips[i + 1];
        }
    }
    int last_sc = s->scene_count - 1;
    s->scene_count--;

    rebind_all_locked(s);                            // first the streams to the new addresses...
    for (int c = 0; c < s->channel_count; ++c)
        memset(&s->channels[c].clips[last_sc], 0, sizeof(Clip));   // ...then clear the freed cell
    struct_unlock(s);

    printf("session_delete_scene: [%d] (count now %d)\n",
           sc, s->scene_count);
}

// Inserts an EMPTY channel at position idx (the later ones shift right). The reverse of session_delete_channel.
// Used by undo to bring back a deleted channel without stopping the ones playing around it.
int session_insert_channel(Session *s, int idx)
{
    rec_abort_all(s);                               /* 1.4: the cells move - a running recording ends here */
    if (!s || idx < 0 || idx > s->channel_count || s->channel_count >= MAX_CHANNELS) return -1;
    session_queue_clear(s);
    struct_lock(s);
    for (int c = s->channel_count; c > idx; --c)
        s->channels[c] = s->channels[c - 1];
    session_channel_init(&s->channels[idx], idx, NULL);   // the slot has already been copied further on
    s->channel_count++;
    rebind_all_locked(s);                            // the streams of the shifted channels -> the new addresses
    struct_unlock(s);
    printf("session_insert_channel: [%d] (count now %d)\n", idx, s->channel_count);
    return idx;
}

static void q_disarm(Session *s);
/* Step 36: moves channel from to position to (the others shift). Playing clips continue. */
static int move_index(int i, int from, int to)
{
    if (i == from) return to;
    if (from < to && i > from && i <= to) return i - 1;
    if (from > to && i >= to && i < from) return i + 1;
    return i;
}
static int move_cell(int cell, int from, int to)
{
    if (cell < 0) return cell;
    return move_index(cell / MAX_SCENES, from, to) * MAX_SCENES + cell % MAX_SCENES;
}

bool session_move_channel(Session *s, int from, int to)
{
    if (from != to) rec_abort_all(s);              /* 1.4 */
    if (!s) return false;
    int n = s->channel_count;
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return false;
    Channel *tmp = malloc(sizeof(Channel));
    if (!tmp) return false;
    q_disarm(s);                                      /* the prepared next clip is recomputed */
    struct_lock(s);
    memcpy(tmp, &s->channels[from], sizeof(Channel));
    if (from < to) memmove(&s->channels[from], &s->channels[from + 1], sizeof(Channel) * (size_t)(to - from));
    else           memmove(&s->channels[to + 1], &s->channels[to], sizeof(Channel) * (size_t)(from - to));
    memcpy(&s->channels[to], tmp, sizeof(Channel));
    rebind_all_locked(s);
    for (int i = 0; i < s->queue_len; ++i) s->queue_ch[i] = move_index(s->queue_ch[i], from, to);
    atomic_store(&s->q_anchor, move_cell(atomic_load(&s->q_anchor), from, to));
    s->last_started = move_cell(s->last_started, from, to);
    struct_unlock(s);
    free(tmp);
    printf("session_move_channel: [%d] -> [%d]\n", from, to);
    return true;
}

// Inserts an EMPTY scene at position idx (the later ones shift down).
int session_insert_scene(Session *s, int idx)
{
    rec_abort_all(s);                               /* 1.4: the cells move - a running recording ends here */
    if (!s || idx < 0 || idx > s->scene_count || s->scene_count >= MAX_SCENES) return -1;
    session_queue_clear(s);
    /* the cell right below the last scene will be overwritten -> if something was left there, free it */
    for (int c = 0; c < s->channel_count; ++c)
        clip_discard(s, &s->channels[c].clips[s->scene_count]);
    struct_lock(s);
    for (int c = 0; c < s->channel_count; ++c) {
        Channel *chp = &s->channels[c];
        for (int i = s->scene_count; i > idx; --i)
            chp->clips[i] = chp->clips[i - 1];
    }
    s->scene_count++;
    rebind_all_locked(s);
    for (int c = 0; c < s->channel_count; ++c)
        memset(&s->channels[c].clips[idx], 0, sizeof(Clip));
    struct_unlock(s);
    printf("session_insert_scene: [%d] (count now %d)\n", idx, s->scene_count);
    return idx;
}

// ============ Scenes ============

int session_add_scene(Session *s)
{
    if (!s || s->scene_count >= MAX_SCENES) return -1;
    int idx = s->scene_count;
    s->scene_count++;
    printf("session_add_scene: [%d]\n", idx);
    return idx;
}

void session_play_scene(Session *s, int scene)
{
    if (!s || scene < 0 || scene >= s->scene_count) return;
    session_queue_clear(s);                         // manual play -> the queue is cleared

    double qb = ql_target(s);
    if (qb >= 0.0) {                                /* 1.1: the whole scene starts together on the next beat/bar */
        for (int c = 0; c < s->channel_count; ++c) {
            Clip *target = &s->channels[c].clips[scene];
            if (atomic_load(&target->has_clip) && !atomic_load(&target->is_note) && (target->stream || atomic_load(&target->is_midi))) {
                ql_launch_clip(s, c, target, qb);
            } else {                                /* no clip in this scene: what plays in this channel stops then */
                ql_cancel_channel(s, c, NULL);
                for (int sc = 0; sc < s->scene_count; ++sc) {
                    Clip *o = &s->channels[c].clips[sc];
                    if (atomic_load(&o->has_clip) && atomic_load(&o->playing)) ql_schedule(s, o, 2, qb);
                }
            }
        }
        return;
    }

    for (int c = 0; c < s->channel_count; ++c) {
        for (int sc = 0; sc < s->scene_count; ++sc) {
            Clip *cl = &s->channels[c].clips[sc];
            if (atomic_load(&cl->has_clip)) clip_stop(cl);
        }
        Clip *target = &s->channels[c].clips[scene];
        if (atomic_load(&target->has_clip) && !atomic_load(&target->is_note))
            clip_start(target, STREAM_FROM_START);
    }
}

void session_stop_scene(Session *s, int scene)
{
    if (!s || scene < 0 || scene >= s->scene_count) return;
    for (int c = 0; c < s->channel_count; ++c) {
        Clip *cl = &s->channels[c].clips[scene];
        if (atomic_load(&cl->has_clip)) clip_stop(cl);
    }
}

// ============ Global ============

void session_play_all(Session *s)
{
    if (!s) return;
    session_queue_clear(s);
    for (int c = 0; c < s->channel_count; ++c) {
        for (int sc = 0; sc < MAX_SCENES; ++sc) {
            Clip *cl = &s->channels[c].clips[sc];
            if (!atomic_load(&cl->has_clip) || atomic_load(&cl->is_note)) continue;
            if (atomic_load(&cl->playing)) continue;              // already playing
            if (atomic_load(&cl->paused)) {                       // paused -> continues seamlessly
                atomic_store(&cl->paused, false);
                atomic_store(&cl->playing, true);
            } else {
                clip_start(cl, atomic_load(&cl->position));       // from the last position
            }
        }
    }
}

/* ===== 1.2: playing instruments ===== */
void session_midi_tick(Session *s)
{
    if (!s) return;
    session_midi_reap(s);                            /* 1.3: old note lists */
    unsigned b[4] = { 0, 0, 0, 0 };
#define MARK(v) do { int bb_ = (v); if (MIDI_BIND_KIND(bb_) == MIDI_BIND_NOTE) { int nn_ = MIDI_BIND_NUM(bb_) & 0x7F; b[nn_ >> 5] |= 1u << (nn_ & 31); } } while (0)
    for (int c = 0; c < s->channel_count; ++c) {
        Channel *ch = &s->channels[c];
        MARK(atomic_load(&ch->mute_bind));
        MARK(atomic_load(&ch->solo_bind));
        for (int sc = 0; sc < s->scene_count; ++sc) {
            int v = atomic_load_explicit(&ch->clips[sc].bind, memory_order_relaxed);
            if (v) MARK(v);
        }
    }
#undef MARK
    for (int i = 0; i < 4; ++i) atomic_store(&s->midi_bound[i], b[i]);
    atomic_store(&s->midi_block, atomic_load(&s->btn_learn_kind) != BTN_LEARN_NONE);
    int t = atomic_load(&s->midi_target);
    if (t >= s->channel_count) atomic_store(&s->midi_target, -1);
}

void session_set_midi_target(Session *s, int ch)
{
    if (!s) return;
    if (ch < -1 || ch >= s->channel_count) ch = -1;
    atomic_store(&s->midi_target, ch);
}
int session_get_midi_target(Session *s) { return s ? atomic_load(&s->midi_target) : -1; }
void session_midi_panic(Session *s) { if (s) atomic_store(&s->midi_panic, true); }

/* ===== 1.4: recording ===== */
void session_toggle_arm(Session *s, int ch)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    atomic_store(&s->channels[ch].armed, !atomic_load(&s->channels[ch].armed));
}
bool session_is_armed(Session *s, int ch) { return s && ch >= 0 && ch < s->channel_count && atomic_load(&s->channels[ch].armed); }
void session_set_rec_input(Session *s, int ch, int first, bool stereo)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    if (first < 0) first = 0;
    if (first > 62) first = 62;
    atomic_store(&s->channels[ch].rec_in_first, first);
    atomic_store(&s->channels[ch].rec_in_stereo, stereo);
}
void session_set_monitor(Session *s, int ch, bool on) { if (s && ch >= 0 && ch < s->channel_count) atomic_store(&s->channels[ch].monitor, on); }

/* what the UI keeps for one recording */
typedef struct {
    SNDFILE *sf;
    char     path[1024];
    float   *buf;                       /* for writing out of the ring */
    /* MIDI: the keys being held (start, velocity) and the finished notes */
    double   on_beat[16][128];
    unsigned char on_vel[16][128];
    bool     on[16][128];
    MNote   *notes; int n, cap;
    bool     dirty;
    double   last_pub;
} RecUi;

static RecSlot *rec_find(Session *s, int ch, int sc)
{
    for (int i = 0; i < MAX_REC; ++i) {
        RecSlot *r = &s->rec[i];
        if (atomic_load(&r->state) != REC_IDLE && r->ch == ch && r->sc == sc) return r;
    }
    return NULL;
}

static void rec_free(Session *s, RecSlot *r)
{
    RecUi *u = r->ui;
    if (u) { if (u->sf) sf_close(u->sf); free(u->buf); free(u->notes); free(u); }
    r->ui = NULL;
    free(r->ring); r->ring = NULL;
    atomic_store(&r->state, REC_IDLE);
    atomic_fetch_sub(&s->rec_active, 1);
}

int session_rec_state(Session *s, int ch, int scene, double *beats)
{
    if (!s) return REC_IDLE;
    RecSlot *r = rec_find(s, ch, scene);
    if (!r) return REC_IDLE;
    if (beats) *beats = atomic_load(&r->rec_beats);
    return atomic_load(&r->state);
}

int session_rec_cell(Session *s, int ch, int sc, bool midi, const char *wav_path, unsigned latency, char *err, int esz)
{
    char dummy[8];
    if (!err) { err = dummy; esz = sizeof dummy; }
    err[0] = 0;
    if (!s || ch < 0 || ch >= s->channel_count || sc < 0 || sc >= s->scene_count) { snprintf(err, (size_t)esz, "no such cell"); return 1; }
    RecSlot *r = rec_find(s, ch, sc);
    if (r) {                                         /* a recording is here: stop it */
        int st = atomic_load(&r->state);
        if (st == REC_WAIT) {                        /* has not started yet: just cancel */
            if (atomic_compare_exchange_strong(&r->state, &st, REC_IDLE + 100)) {   /* the audio thread ignores this state */
                audio_barrier(s);
                if (r->midi) session_clear_clip(s, ch, sc);
                if (!r->midi && r->ui && ((RecUi *)r->ui)->path[0]) { RecUi *u = r->ui; sf_close(u->sf); u->sf = NULL; remove(u->path); }
                rec_free(s, r);
            }
            return 0;
        }
        if (st == REC_ON) {                          /* at the next beat/bar (like a launch), or at once */
            double qb = -1.0;
            float q = atomic_load(&s->quant_beats);
            if (q > 0.0f && atomic_load(&s->tp_running)) {
                double tp = atomic_load(&s->tp_beats);
                qb = ceil(tp / (double)q - 1e-9) * (double)q;
                if (qb <= r->beat0 + 1e-9) qb += q;  /* at least one step long */
            }
            atomic_store(&r->stop_beat, qb);
            atomic_store(&r->state, REC_STOPPING);
        } else if (st == REC_STOPPING) {
            atomic_store(&r->stop_beat, -1.0);       /* a second click: now */
        }
        return 0;
    }
    if (!atomic_load(&s->channels[ch].armed)) { snprintf(err, (size_t)esz, "the channel is not armed for recording"); return 1; }
    if (atomic_load(&s->channels[ch].clips[sc].has_clip)) { snprintf(err, (size_t)esz, "the cell is not empty"); return 1; }
    r = NULL;
    for (int i = 0; i < MAX_REC && !r; ++i) if (atomic_load(&s->rec[i].state) == REC_IDLE) r = &s->rec[i];
    if (!r) { snprintf(err, (size_t)esz, "already %d recordings at once", MAX_REC); return 1; }
    RecUi *u = calloc(1, sizeof *u);
    if (!u) { snprintf(err, (size_t)esz, "out of memory"); return 1; }
    r->ch = ch; r->sc = sc; r->midi = midi;
    r->in_first = atomic_load(&s->channels[ch].rec_in_first);
    r->in_count = atomic_load(&s->channels[ch].rec_in_stereo) ? 2 : 1;
    r->latency = midi ? 0 : latency;
    r->stopped_at = false;
    atomic_store(&r->rec_beats, 0.0);
    atomic_store(&r->frames, 0ull); atomic_store(&r->after, 0ull);
    atomic_store(&r->overflow, false);
    atomic_store(&r->final_beats, 0.0);
    atomic_store(&r->rw, 0u); atomic_store(&r->rr, 0u);
    atomic_store(&r->mw, 0u); atomic_store(&r->mr, 0u);
    atomic_store(&r->stop_beat, -1.0);
    if (midi) {
        if (session_new_midi_clip(s, ch, sc, 1024.0f, "Rec") != 0) { free(u); snprintf(err, (size_t)esz, "cannot make the clip"); return 1; }
        atomic_store(&s->channels[ch].clips[sc].loop, true);
    } else {
        if (!wav_path || !wav_path[0]) { free(u); snprintf(err, (size_t)esz, "no file to record into"); return 1; }
        r->ring = malloc(sizeof(float) * 2 * REC_RING_FRAMES);
        u->buf = malloc(sizeof(float) * 2 * 8192);
        SF_INFO info;
        memset(&info, 0, sizeof info);
        info.samplerate = s->sample_rate > 0 ? s->sample_rate : 48000;
        info.channels = 2;
        info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_24;
        snprintf(u->path, sizeof u->path, "%s", wav_path);
        u->sf = r->ring && u->buf ? sf_open(wav_path, SFM_WRITE, &info) : NULL;
        if (!u->sf) {
            snprintf(err, (size_t)esz, "cannot write '%s'", wav_path);
            free(r->ring); r->ring = NULL; free(u->buf); free(u);
            return 1;
        }
    }
    r->ui = u;
    /* the start: on the next beat/bar while something plays (like a launch), otherwise at once */
    double qb = -1.0;
    float q = atomic_load(&s->quant_beats);
    bool any = session_any_playing(s);
    if (!any) atomic_store(&s->tp_reset, true);      /* nothing plays: the recording is beat 1 */
    else if (q > 0.0f && atomic_load(&s->tp_running)) {
        double tp = atomic_load(&s->tp_beats);
        qb = ceil(tp / (double)q - 1e-9) * (double)q;
    }
    atomic_store(&r->start_beat, qb);
    atomic_fetch_add(&s->rec_active, 1);
    atomic_store(&r->state, REC_WAIT);
    printf("rec: [%d][%d] %s %s\n", ch, sc, midi ? "MIDI" : "audio", qb >= 0 ? "waits for the beat" : "starts now");
    return 0;
}

static void rec_publish_midi(Session *s, RecSlot *r, double len_beats, bool final)
{
    RecUi *u = r->ui;
    MidiSeq *m = midiseq_new(u->n, final ? (float)len_beats : 1024.0f);
    if (!m) return;
    if (u->n) memcpy(m->note, u->notes, sizeof(MNote) * (size_t)u->n);
    m->n = u->n;
    if (final) {                                     /* notes that start after the end are dropped */
        int w = 0;
        for (int i = 0; i < m->n; ++i) if (m->note[i].start < m->length) m->note[w++] = m->note[i];
        m->n = w;
    }
    session_midi_set(s, r->ch, r->sc, m);
    u->dirty = false;
}

static void rec_add_note(RecUi *u, double start, double end, int key, int vel)
{
    if (end <= start) end = start + 1.0 / 64.0;
    if (u->n == u->cap) {
        int nc = u->cap ? u->cap * 2 : 256;
        MNote *nn = realloc(u->notes, sizeof(MNote) * (size_t)nc);
        if (!nn) return;
        u->notes = nn; u->cap = nc;
    }
    MNote *q = &u->notes[u->n++];
    q->start = (float)(start < 0 ? 0 : start); q->len = (float)(end - (start < 0 ? 0 : start)); q->key = (unsigned char)key;
    q->vel = (unsigned char)(vel < 1 ? 1 : vel); q->sel = 0;
    u->dirty = true;
}

void session_rec_tick(Session *s)
{
    if (!s || atomic_load(&s->rec_active) <= 0) return;
    for (int i = 0; i < MAX_REC; ++i) {
        RecSlot *r = &s->rec[i];
        int st = atomic_load(&r->state);
        if (st == REC_IDLE || !r->ui) continue;
        RecUi *u = r->ui;
        if (!r->midi) {                              /* the ring -> the file */
            unsigned rr = atomic_load_explicit(&r->rr, memory_order_relaxed), rw = atomic_load_explicit(&r->rw, memory_order_acquire);
            while (rr != rw && u->sf) {
                unsigned k = rw - rr, at = rr % REC_RING_FRAMES;
                if (k > REC_RING_FRAMES - at) k = REC_RING_FRAMES - at;
                if (k > 8192) k = 8192;
                memcpy(u->buf, r->ring + (size_t)at * 2, sizeof(float) * 2 * k);
                sf_writef_float(u->sf, u->buf, k);
                rr += k;
                atomic_store_explicit(&r->rr, rr, memory_order_release);
            }
        } else {                                     /* the played messages -> notes */
            unsigned mr = atomic_load_explicit(&r->mr, memory_order_relaxed), mw = atomic_load_explicit(&r->mw, memory_order_acquire);
            for (; mr != mw; ++mr) {
                const RecEv *e = &r->mev[mr % REC_MEV];
                unsigned stt = e->m[0] & 0xF0, mc = e->m[0] & 0x0F, k = e->m[1] & 0x7F;
                if (stt == 0x90 && e->m[2] > 0) {
                    if (u->on[mc][k]) rec_add_note(u, u->on_beat[mc][k], e->beat, (int)k, u->on_vel[mc][k]);
                    u->on[mc][k] = true; u->on_beat[mc][k] = e->beat; u->on_vel[mc][k] = e->m[2];
                } else if ((stt == 0x80 || stt == 0x90) && u->on[mc][k]) {
                    rec_add_note(u, u->on_beat[mc][k], e->beat, (int)k, u->on_vel[mc][k]);
                    u->on[mc][k] = false;
                }
            }
            atomic_store_explicit(&r->mr, mr, memory_order_release);
            if (u->dirty && st != REC_DONE) rec_publish_midi(s, r, 0.0, false);   /* the notes appear while you play */
        }
        if (st != REC_DONE) continue;
        /* ---- finished: it becomes a clip that plays on in time ---- */
        double len = atomic_load(&r->final_beats);
        Clip *cl = &s->channels[r->ch].clips[r->sc];
        char nm[NAME_LEN];
        int nrec = 1;
        for (int sc = 0; sc < s->scene_count; ++sc) {
            const Clip *o = &s->channels[r->ch].clips[sc];
            if (atomic_load(&o->has_clip) && !strncmp(o->name, "Rec ", 4)) ++nrec;
        }
        snprintf(nm, sizeof nm, "Rec %d", nrec);
        if (r->midi) {
            for (int c = 0; c < 16; ++c)             /* keys still held: they end at the end */
                for (int k = 0; k < 128; ++k)
                    if (u->on[c][k]) { rec_add_note(u, u->on_beat[c][k], len, k, u->on_vel[c][k]); u->on[c][k] = false; }
            if (len < 0.25) len = 0.25;
            rec_publish_midi(s, r, len, true);
            session_rename_clip(s, r->ch, r->sc, nm);
            printf("rec: [%d][%d] MIDI done, %.2f beats, %d notes\n", r->ch, r->sc, len, u->n);
        } else {
            sf_close(u->sf); u->sf = NULL;
            bool ovf = atomic_load(&r->overflow);
            char path[1024];
            snprintf(path, sizeof path, "%s", u->path);
            unsigned long long after = atomic_load(&r->after);
            unsigned long long frames = atomic_load(&r->frames);
            if (frames > 0 && session_set_clip(s, r->ch, r->sc, path) == 0) {
                session_rename_clip(s, r->ch, r->sc, nm);
                atomic_store(&cl->loop, true);
                float T = atomic_load(&s->tempo);
                session_set_clip_bpm_fields(s, r->ch, r->sc, 0.0f, T > 0 ? T : 0.0f, 0.0f);   /* recorded at this tempo */
                size_t len_f = cl->wav.frame_count;
                if (len_f > 0) clip_start(cl, (size_t)(after % len_f));   /* goes on as if it had started at the stop */
                printf("rec: [%d][%d] audio done, %llu frames (%.2f beats)%s -> %s\n", r->ch, r->sc, frames, len,
                       ovf ? " - SOME AUDIO WAS LOST (the disk was too slow)" : "", path);
            } else {
                remove(path);
                printf("rec: [%d][%d] nothing recorded\n", r->ch, r->sc);
            }
        }
        rec_free(s, r);
    }
}

/* the cells are about to move (a channel/scene is inserted, deleted, moved; a project is closed): every recording
 * ends at once. What was recorded stays in its file (audio); no clip is made from it. */
static void rec_abort_all(Session *s)
{
    if (!s || atomic_load(&s->rec_active) <= 0) return;
    bool any = false;
    for (int i = 0; i < MAX_REC; ++i) {
        RecSlot *r = &s->rec[i];
        int st = atomic_load(&r->state);
        if (st == REC_IDLE) continue;
        atomic_store(&r->state, REC_IDLE + 100);     /* the audio thread ignores it from now on */
        any = true;
    }
    if (!any) return;
    audio_barrier(s);
    for (int i = 0; i < MAX_REC; ++i) {
        RecSlot *r = &s->rec[i];
        if (atomic_load(&r->state) != REC_IDLE + 100) continue;
        RecUi *u = r->ui;
        if (u && u->sf) {                            /* the audio recorded so far still goes into the file */
            unsigned rr = atomic_load(&r->rr), rw = atomic_load(&r->rw);
            for (; rr != rw; ++rr) sf_writef_float(u->sf, r->ring + (size_t)(rr % REC_RING_FRAMES) * 2, 1);
            printf("rec: stopped by a change of the grid - the audio stays in %s\n", u->path);
        }
        rec_free(s, r);
    }
}

void session_rec_stop_all(Session *s)
{
    if (!s || atomic_load(&s->rec_active) <= 0) return;
    for (int i = 0; i < MAX_REC; ++i) {
        RecSlot *r = &s->rec[i];
        int st = atomic_load(&r->state);
        if (st == REC_WAIT) session_rec_cell(s, r->ch, r->sc, r->midi, NULL, 0, NULL, 0);    /* cancel */
        else if (st == REC_ON || st == REC_STOPPING) { atomic_store(&r->stop_beat, -1.0); atomic_store(&r->state, REC_STOPPING); }
    }
}

void session_stop_all(Session *s)
{
    if (!s) return;
    session_rec_stop_all(s);                         /* 1.4: Stop all also ends the recordings (now) */
    for (int c = 0; c < s->channel_count; ++c) {
        for (int sc = 0; sc < MAX_SCENES; ++sc) {
            Clip *cl = &s->channels[c].clips[sc];
            if (atomic_load(&cl->has_clip)) clip_stop(cl);
        }
    }
}

// ============ Pause / Resume ============
// (Pause does NOT stop the stream: the buffer stays full and resume has no break.)

void session_pause_all(Session *s)
{
    if (!s) return;
    for (int c = 0; c < s->channel_count; ++c) {
        for (int sc = 0; sc < MAX_SCENES; ++sc) {
            Clip *cl = &s->channels[c].clips[sc];
            if (atomic_load(&cl->has_clip) &&
                !atomic_load(&cl->is_note) &&
                atomic_load(&cl->playing)) {
                atomic_store(&cl->playing, false);
                atomic_store(&cl->paused, true);
                if (cl->stretch) stretch_release(cl->stretch);   // pause: the background thread stops (the ring stays)
            }
        }
    }
}

void session_resume_all(Session *s)
{
    if (!s) return;
    for (int c = 0; c < s->channel_count; ++c) {
        for (int sc = 0; sc < MAX_SCENES; ++sc) {
            Clip *cl = &s->channels[c].clips[sc];
            if (atomic_load(&cl->has_clip) &&
                !atomic_load(&cl->is_note) &&
                atomic_load(&cl->paused)) {
                atomic_store(&cl->paused, false);
                atomic_store(&cl->playing, true);
            }
        }
    }
}

bool session_any_playing(Session *s)
{
    if (!s) return false;
    for (int c = 0; c < s->channel_count; ++c) {
        for (int sc = 0; sc < MAX_SCENES; ++sc) {
            Clip *cl = &s->channels[c].clips[sc];
            if (atomic_load(&cl->has_clip) &&
                !atomic_load(&cl->is_note) &&
                atomic_load(&cl->playing)) {
                return true;
            }
        }
    }
    return false;
}

bool session_any_paused(Session *s)
{
    if (!s) return false;
    for (int c = 0; c < s->channel_count; ++c) {
        for (int sc = 0; sc < MAX_SCENES; ++sc) {
            Clip *cl = &s->channels[c].clips[sc];
            if (atomic_load(&cl->has_clip) &&
                !atomic_load(&cl->is_note) &&
                atomic_load(&cl->paused)) {
                return true;
            }
        }
    }
    return false;
}

// ============ Loop ============

void session_toggle_loop(Session *s, int ch, int scene)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    if (scene < 0 || scene >= MAX_SCENES) return;

    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip)) return;
    if (atomic_load(&cl->is_note)) return;

    bool was = atomic_load(&cl->loop);
    atomic_store(&cl->loop, !was);
    printf("session_toggle_loop: [%d][%d] loop=%d\n", ch, scene, !was);
}

void session_set_loop_region(Session *s, int ch, int scene,
                              size_t start, size_t end)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    if (scene < 0 || scene >= MAX_SCENES) return;

    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip)) return;
    if (atomic_load(&cl->is_note)) return;

    if (end > cl->wav.frame_count) end = cl->wav.frame_count;
    if (start > end) start = end;

    atomic_store(&cl->loop_start, start);
    atomic_store(&cl->loop_end, end);
}

bool session_get_loop_state(Session *s, int ch, int scene,
                             bool *out_on, size_t *out_start, size_t *out_end)
{
    if (!s || ch < 0 || ch >= s->channel_count) return false;
    if (scene < 0 || scene >= MAX_SCENES) return false;

    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip)) return false;
    if (atomic_load(&cl->is_note)) return false;

    if (out_on)    *out_on    = atomic_load(&cl->loop);
    if (out_start) *out_start = atomic_load(&cl->loop_start);
    if (out_end)   *out_end   = atomic_load(&cl->loop_end);

    return true;
}

// ============ Start / End markers ============

void session_set_start_marker(Session *s, int ch, int scene, size_t pos)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    if (scene < 0 || scene >= MAX_SCENES) return;

    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip)) return;
    if (atomic_load(&cl->is_note)) return;

    size_t end = atomic_load(&cl->end_marker);
    if (end == 0 || end > cl->wav.frame_count) {
        end = cl->wav.frame_count;
    }

    if (pos > end) pos = end;

    atomic_store(&cl->start_marker, pos);
    stream_invalidate_preroll(cl->stream);
}

void session_set_end_marker(Session *s, int ch, int scene, size_t pos)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    if (scene < 0 || scene >= MAX_SCENES) return;

    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip)) return;
    if (atomic_load(&cl->is_note)) return;

    size_t start = atomic_load(&cl->start_marker);
    if (pos < start) pos = start;

    if (pos > cl->wav.frame_count) pos = cl->wav.frame_count;

    atomic_store(&cl->end_marker, pos);
}

void session_reset_start_marker(Session *s, int ch, int scene)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    if (scene < 0 || scene >= MAX_SCENES) return;

    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip)) return;
    if (atomic_load(&cl->is_note)) return;

    atomic_store(&cl->start_marker, 0);
    stream_invalidate_preroll(cl->stream);
}

void session_reset_end_marker(Session *s, int ch, int scene)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    if (scene < 0 || scene >= MAX_SCENES) return;

    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip)) return;
    if (atomic_load(&cl->is_note)) return;

    atomic_store(&cl->end_marker, cl->wav.frame_count);
}

bool session_get_markers(Session *s, int ch, int scene,
                          size_t *out_start, size_t *out_end)
{
    if (!s || ch < 0 || ch >= s->channel_count) return false;
    if (scene < 0 || scene >= MAX_SCENES) return false;

    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip)) return false;
    if (atomic_load(&cl->is_note)) return false;

    if (out_start) *out_start = atomic_load(&cl->start_marker);
    if (out_end)   *out_end   = atomic_load(&cl->end_marker);

    return true;
}

// ============ Waveform zoom (visual) ============

void session_set_wave_zoom(Session *s, int ch, int scene, float zoom)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    if (scene < 0 || scene >= MAX_SCENES) return;

    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip)) return;
    if (atomic_load(&cl->is_note)) return;

    if (zoom < WAVE_ZOOM_MIN) zoom = WAVE_ZOOM_MIN;
    if (zoom > WAVE_ZOOM_MAX) zoom = WAVE_ZOOM_MAX;

    atomic_store(&cl->wave_zoom, zoom);
}

float session_get_wave_zoom(Session *s, int ch, int scene)
{
    if (!s || ch < 0 || ch >= s->channel_count) return 1.0f;
    if (scene < 0 || scene >= MAX_SCENES) return 1.0f;

    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip)) return 1.0f;
    if (atomic_load(&cl->is_note)) return 1.0f;

    return atomic_load(&cl->wave_zoom);
}

void session_set_wave_scroll(Session *s, int ch, int scene, size_t frame)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    if (scene < 0 || scene >= MAX_SCENES) return;

    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip)) return;
    if (atomic_load(&cl->is_note)) return;

    atomic_store(&cl->wave_scroll_frame, frame);
}

size_t session_get_wave_scroll(Session *s, int ch, int scene)
{
    if (!s || ch < 0 || ch >= s->channel_count) return 0;
    if (scene < 0 || scene >= MAX_SCENES) return 0;

    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip)) return 0;
    if (atomic_load(&cl->is_note)) return 0;

    return atomic_load(&cl->wave_scroll_frame);
}

void session_reset_wave_zoom(Session *s, int ch, int scene)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    if (scene < 0 || scene >= MAX_SCENES) return;

    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip)) return;
    if (atomic_load(&cl->is_note)) return;

    atomic_store(&cl->wave_zoom, 1.0f);
    atomic_store(&cl->wave_scroll_frame, 0);
}

// ============ MIDI: fader ============

void session_set_midi_cc(Session *s, int ch, int cc)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    atomic_store(&s->channels[ch].midi_cc, cc);
}

int session_get_midi_cc(Session *s, int ch)
{
    if (!s || ch < 0 || ch >= s->channel_count) return -1;
    return atomic_load(&s->channels[ch].midi_cc);
}

void session_set_master_midi_cc(Session *s, int cc)
{
    if (!s) return;
    atomic_store(&s->master_midi_cc, cc);
}

int session_get_master_midi_cc(Session *s)
{
    if (!s) return -1;
    return atomic_load(&s->master_midi_cc);
}

void session_arm_midi_learn(Session *s, int ch)
{
    if (!s) return;
    atomic_store(&s->btn_learn_kind, BTN_LEARN_NONE);

    for (int c = 0; c < s->channel_count; ++c) {
        atomic_store(&s->channels[c].midi_learn_armed, false);
        atomic_store(&s->channels[c].pan_midi_learn_armed, false);
    }
    atomic_store(&s->master_midi_learn_armed, false);

    if (ch == -1) {
        atomic_store(&s->master_midi_learn_armed, true);
    } else if (ch >= 0 && ch < s->channel_count) {
        atomic_store(&s->channels[ch].midi_learn_armed, true);
    }
}

void session_disarm_all_midi_learn(Session *s)
{
    if (!s) return;
    for (int c = 0; c < s->channel_count; ++c) {
        atomic_store(&s->channels[c].midi_learn_armed, false);
    }
    atomic_store(&s->master_midi_learn_armed, false);
}

int session_get_armed_channel(Session *s)
{
    if (!s) return -2;

    if (atomic_load(&s->master_midi_learn_armed)) return -1;

    for (int c = 0; c < s->channel_count; ++c) {
        if (atomic_load(&s->channels[c].midi_learn_armed)) return c;
    }

    return -2;
}

// ============ MIDI: pan ============

void session_set_pan_midi_cc(Session *s, int ch, int cc)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    atomic_store(&s->channels[ch].pan_midi_cc, cc);
}

int session_get_pan_midi_cc(Session *s, int ch)
{
    if (!s || ch < 0 || ch >= s->channel_count) return -1;
    return atomic_load(&s->channels[ch].pan_midi_cc);
}

void session_arm_pan_midi_learn(Session *s, int ch)
{
    if (!s) return;
    atomic_store(&s->btn_learn_kind, BTN_LEARN_NONE);

    for (int c = 0; c < s->channel_count; ++c) {
        atomic_store(&s->channels[c].pan_midi_learn_armed, false);
        atomic_store(&s->channels[c].midi_learn_armed, false);
    }
    atomic_store(&s->master_midi_learn_armed, false);

    if (ch >= 0 && ch < s->channel_count) {
        atomic_store(&s->channels[ch].pan_midi_learn_armed, true);
    }
}

void session_disarm_all_pan_midi_learn(Session *s)
{
    if (!s) return;
    for (int c = 0; c < s->channel_count; ++c) {
        atomic_store(&s->channels[c].pan_midi_learn_armed, false);
    }
}

int session_get_armed_pan_channel(Session *s)
{
    if (!s) return -2;

    for (int c = 0; c < s->channel_count; ++c) {
        if (atomic_load(&s->channels[c].pan_midi_learn_armed)) return c;
    }

    return -2;
}


// ============ Routing ============

static _Atomic RouteMask *route_slot(Session *s, int ch, int side)
{
    if (!s || ch < 0 || ch >= s->channel_count || side < 0 || side > 1) return NULL;
    return side == 0 ? &s->channels[ch].route_l : &s->channels[ch].route_r;
}

RouteMask session_route_get(Session *s, int ch, int side)
{
    _Atomic RouteMask *p = route_slot(s, ch, side);
    return p ? atomic_load(p) : 0;
}

void session_route_set(Session *s, int ch, int side, RouteMask mask)
{
    _Atomic RouteMask *p = route_slot(s, ch, side);
    if (!p) return;
    mask &= (ROUTE_MASTER_L | ROUTE_MASTER_R | ((RBIT(MAX_HW_OUT) - 1u) << 2));
    atomic_store(p, mask);
}

void session_route_toggle(Session *s, int ch, int side, int dest)
{
    _Atomic RouteMask *p = route_slot(s, ch, side);
    if (!p || dest < 0 || dest >= 2 + MAX_HW_OUT) return;
    atomic_fetch_xor(p, RBIT(dest));
}

RouteMask session_master_route_get(Session *s, int side)
{
    if (!s || side < 0 || side > 1) return 0;
    return atomic_load(side == 0 ? &s->master_route_l : &s->master_route_r);
}

void session_master_route_set(Session *s, int side, RouteMask mask)
{
    if (!s || side < 0 || side > 1) return;
    mask &= RBIT(MAX_HW_OUT) - 1u;
    atomic_store(side == 0 ? &s->master_route_l : &s->master_route_r, mask);
}

void session_master_route_toggle(Session *s, int side, int dest)
{
    if (!s || side < 0 || side > 1 || dest < 0 || dest >= MAX_HW_OUT) return;
    atomic_fetch_xor(side == 0 ? &s->master_route_l : &s->master_route_r, RBIT(dest));
}

void session_routing_reset(Session *s)
{
    if (!s) return;
    for (int c = 0; c < s->channel_count; ++c) {
        atomic_store(&s->channels[c].route_l, ROUTE_DEFAULT_L);
        atomic_store(&s->channels[c].route_r, ROUTE_DEFAULT_R);
    }
    atomic_store(&s->master_route_l, MASTER_ROUTE_DEFAULT_L);
    atomic_store(&s->master_route_r, MASTER_ROUTE_DEFAULT_R);
}

// ============ MIDI: buttons (Mute / Solo / clips) ============

void session_arm_button_learn(Session *s, int kind, int ch, int scene)
{
    if (!s) return;
    session_disarm_all_midi_learn(s);           // one pending "learn" at a time
    session_disarm_all_pan_midi_learn(s);
    atomic_store(&s->btn_learn_ch, ch);
    atomic_store(&s->btn_learn_scene, scene);
    atomic_store(&s->btn_learn_kind, kind);
}

void session_disarm_button_learn(Session *s)
{
    if (s) atomic_store(&s->btn_learn_kind, BTN_LEARN_NONE);
}

bool session_get_button_learn(Session *s, int *kind, int *ch, int *scene)
{
    if (!s) return false;
    int k = atomic_load(&s->btn_learn_kind);
    if (kind)  *kind  = k;
    if (ch)    *ch    = atomic_load(&s->btn_learn_ch);
    if (scene) *scene = atomic_load(&s->btn_learn_scene);
    return k != BTN_LEARN_NONE;
}

int session_get_mute_bind(Session *s, int ch)
{
    if (!s || ch < 0 || ch >= s->channel_count) return 0;
    return atomic_load(&s->channels[ch].mute_bind);
}

int session_get_solo_bind(Session *s, int ch)
{
    if (!s || ch < 0 || ch >= s->channel_count) return 0;
    return atomic_load(&s->channels[ch].solo_bind);
}

int session_get_clip_bind(Session *s, int ch, int scene)
{
    if (!s || ch < 0 || ch >= s->channel_count || scene < 0 || scene >= MAX_SCENES) return 0;
    return atomic_load(&s->channels[ch].clips[scene].bind);
}

void session_set_mute_bind(Session *s, int ch, int bind)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    atomic_store(&s->channels[ch].mute_bind, bind);
}

void session_set_solo_bind(Session *s, int ch, int bind)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    atomic_store(&s->channels[ch].solo_bind, bind);
}

void session_set_clip_bind(Session *s, int ch, int scene, int bind)
{
    if (!s || ch < 0 || ch >= s->channel_count || scene < 0 || scene >= MAX_SCENES) return;
    atomic_store(&s->channels[ch].clips[scene].bind, bind);
}

void session_bind_to_text(int bind, char *out, size_t size)
{
    if (!out || size == 0) return;
    if (bind <= 0) { snprintf(out, size, "-"); return; }
    snprintf(out, size, "%s%d", MIDI_BIND_KIND(bind) == MIDI_BIND_NOTE ? "N" : "CC", MIDI_BIND_NUM(bind));
}

bool session_handle_midi_button(Session *s, int kind, int number, int value)
{
    if (!s || number < 0 || number > 127) return false;

    // CC: buttons send 127 on press and 0 on release. We react only to the "upward" crossing of 64.
    static bool cc_down[128];
    bool pressed;
    if (kind == MIDI_BIND_CC) {
        bool was = cc_down[number];
        bool now = value >= 64;
        cc_down[number] = now;
        pressed = now && !was;
    } else {
        pressed = value > 0;                    // Note On with velocity > 0
    }
    if (!pressed) return false;

    int code = MIDI_BIND_MAKE(kind, number);

    int lk = atomic_load(&s->btn_learn_kind);
    if (lk != BTN_LEARN_NONE) {
        int ch = atomic_load(&s->btn_learn_ch);
        int sc = atomic_load(&s->btn_learn_scene);
        if (lk == BTN_LEARN_MUTE)      session_set_mute_bind(s, ch, code);
        else if (lk == BTN_LEARN_SOLO) session_set_solo_bind(s, ch, code);
        else if (lk == BTN_LEARN_CLIP) session_set_clip_bind(s, ch, sc, code);
        atomic_store(&s->btn_learn_kind, BTN_LEARN_NONE);
        return true;
    }

    bool did = false;
    for (int c = 0; c < s->channel_count; ++c) {
        Channel *chp = &s->channels[c];
        if (atomic_load(&chp->mute_bind) == code) { session_toggle_mute(s, c); did = true; }
        if (atomic_load(&chp->solo_bind) == code) { session_toggle_solo(s, c); did = true; }
        for (int sc = 0; sc < s->scene_count; ++sc) {
            Clip *cl = &chp->clips[sc];
            if (atomic_load(&cl->bind) != code) continue;
            if (!atomic_load(&cl->has_clip) || atomic_load(&cl->is_note)) continue;
            session_toggle_clip(s, c, sc);
            did = true;
        }
    }
    return did;
}

// ============ Tempo ============

static float clamp_bpm(float v)
{
    if (!(v == v)) return 120.0f;                  // NaN
    if (v < BPM_MIN) v = BPM_MIN;
    if (v > BPM_MAX) v = BPM_MAX;
    return bpm_round(v);
}

void session_set_tempo(Session *s, float bpm)
{
    if (!s) return;
    atomic_store(&s->tempo, clamp_bpm(bpm));
    atomic_store(&g_master_tempo, atomic_load(&s->tempo));
}

void session_set_quantize(Session *s, float beats)
{
    if (!s) return;
    if (!(beats >= 0.0f)) beats = 0.0f;
    if (beats > 64.0f) beats = 64.0f;
    atomic_store(&s->quant_beats, beats);
}

float session_get_quantize(Session *s) { return s ? atomic_load(&s->quant_beats) : 0.0f; }

bool session_transport(Session *s, double *beats)
{
    if (!s) return false;
    if (beats) *beats = atomic_load(&s->tp_beats);
    return atomic_load(&s->tp_running);
}

int session_clip_launch_state(Session *s, int ch, int scene)
{
    if (!s || ch < 0 || ch >= s->channel_count || scene < 0 || scene >= MAX_SCENES) return 0;
    return atomic_load(&s->channels[ch].clips[scene].ql_state);
}

float session_get_tempo(Session *s)
{
    return s ? atomic_load(&s->tempo) : 120.0f;
}

static Clip *audio_clip(Session *s, int ch, int scene)
{
    if (!s || ch < 0 || ch >= s->channel_count || scene < 0 || scene >= MAX_SCENES) return NULL;
    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip) || atomic_load(&cl->is_note)) return NULL;
    return cl;
}

static float clip_orig(const Clip *cl)
{
    float f = atomic_load(&cl->bpm_fix);
    return f > 0.0f ? f : atomic_load(&cl->bpm_detected);
}

float session_clip_bpm_orig(Session *s, int ch, int scene)
{
    Clip *cl = audio_clip(s, ch, scene);
    return cl ? clip_orig(cl) : 0.0f;
}

float session_clip_bpm_play(Session *s, int ch, int scene)
{
    Clip *cl = audio_clip(s, ch, scene);
    if (!cl) return 0.0f;
    float o = clip_orig(cl);
    if (!atomic_load(&cl->warp) || o <= 0.0f) return o;
    return atomic_load(&g_master_tempo);           /* 1.1: WARP = the master tempo */
}

bool session_clip_bpm_busy(Session *s, int ch, int scene)
{
    Clip *cl = audio_clip(s, ch, scene);
    return cl && atomic_load(&cl->bpm_busy);
}

bool session_get_clip_warp(Session *s, int ch, int scene)
{
    Clip *cl = audio_clip(s, ch, scene);
    return cl && atomic_load(&cl->warp);
}

void session_set_clip_warp(Session *s, int ch, int scene, bool on)
{
    Clip *cl = audio_clip(s, ch, scene);
    if (!cl) return;
    if (on && !cl->stretch) {
        Stretch *st = stretch_create(s->sample_rate > 0 ? s->sample_rate : 48000);
        if (!st) { printf("session_set_clip_warp: out of memory\n"); return; }
        mix_lock(s);                                // the audio thread reads the pointer -> change it between blocks
        cl->stretch = st;
        pthread_mutex_unlock(&s->mix_mu);
    }
    bool was = atomic_load(&cl->warp);
    atomic_store(&cl->warp, on);                    // the audio thread sees the stretch after this flag
    if (was && !on && cl->stream && (atomic_load(&cl->playing) || atomic_load(&cl->paused)) &&
        stretch_threaded(cl->stretch)) {
        // the background thread has pulled audio ahead: continue from the heard position, so there is no jump
        size_t pos = atomic_load(&cl->position);
        atomic_fetch_add(&cl->stretch_gen, 1);
        stream_start(cl->stream, pos);
    }
}

void session_set_clip_bpm_value(Session *s, int ch, int scene, float bpm)
{
    Clip *cl = audio_clip(s, ch, scene);
    if (!cl) return;
    bpm = clamp_bpm(bpm);
    atomic_store(&cl->bpm_fix, bpm);                /* 1.1: the field is always the clip's own tempo (a correction) */
}

void session_set_clip_bpm_fields(Session *s, int ch, int scene, float detected, float fix, float play)
{
    Clip *cl = audio_clip(s, ch, scene);
    if (!cl) return;
    if (detected > 0.0f) {                          // already known -> do not detect again
        atomic_store(&cl->bpm_detected, bpm_round(detected));
        if (cl->bpm_job) { bpm_job_release(cl->bpm_job); cl->bpm_job = NULL; }
        atomic_store(&cl->bpm_busy, false);
    }
    atomic_store(&cl->bpm_fix,  fix  > 0.0f ? clamp_bpm(fix)  : 0.0f);
    atomic_store(&cl->bpm_play, play > 0.0f ? clamp_bpm(play) : 0.0f);
}

double session_clip_speed(const Clip *cl)
{
    if (!cl || !atomic_load(&cl->warp)) return 1.0;
    return session_clip_speed_warp(cl);
}

double session_clip_speed_warp(const Clip *cl)
{
    if (!cl) return 1.0;
    float o = clip_orig(cl);
    if (o <= 0.0f) return 1.0;
    float p = atomic_load(&g_master_tempo);         /* 1.1: WARP clips follow the master tempo */
    if (p <= 0.0f) return 1.0;
    double sp = (double)p / (double)o;
    if (sp < STRETCH_SPEED_MIN) sp = STRETCH_SPEED_MIN;
    if (sp > STRETCH_SPEED_MAX) sp = STRETCH_SPEED_MAX;
    return sp;
}

void session_poll_bpm(Session *s)
{
    if (!s) return;
    for (int c = 0; c < s->channel_count; ++c)
        for (int sc = 0; sc < MAX_SCENES; ++sc) {
            Clip *cl = &s->channels[c].clips[sc];
            if (!cl->bpm_job) continue;
            float r = 0.0f;
            if (!bpm_job_poll(cl->bpm_job, &r)) continue;
            atomic_store(&cl->bpm_detected, bpm_round(r));
            bpm_job_release(cl->bpm_job);
            cl->bpm_job = NULL;
            atomic_store(&cl->bpm_busy, false);
        }
}

// ============ Queue ============

static Clip *qclip(Session *s, int idx)
{
    if (idx < 0) return NULL;
    int ch = idx / MAX_SCENES, sc = idx % MAX_SCENES;
    if (ch >= s->channel_count || sc >= s->scene_count) return NULL;
    Clip *cl = &s->channels[ch].clips[sc];
    if (!atomic_load(&cl->has_clip) || atomic_load(&cl->is_note) || !cl->stream) return NULL;
    return cl;
}

static size_t q_pull(void *ud, float *out, size_t n) { return stream_read((ClipStream *)ud, out, n); }

/* undoes the preparation of the next one (if the audio thread has not started it yet) */
static void q_disarm(Session *s)
{
    int nx = atomic_load(&s->q_next);
    if (nx < 0) return;
    if (atomic_compare_exchange_strong(&s->q_next, &nx, -1)) {
        Clip *b = qclip(s, nx);
        if (b && !atomic_load(&b->playing) && b->stretch) stretch_release(b->stretch);
    }
}

void session_queue_clear(Session *s)
{
    if (!s) return;
    q_disarm(s);
    s->queue_len = 0;
    atomic_store(&s->q_anchor, -1);
    atomic_store(&s->q_launched, 0);
}

int session_queue_len(Session *s) { return s ? s->queue_len : 0; }

bool session_queue_get(Session *s, int i, int *ch, int *scene)
{
    if (!s || i < 0 || i >= s->queue_len) return false;
    if (ch) *ch = s->queue_ch[i];
    if (scene) *scene = s->queue_sc[i];
    return true;
}

bool session_queue_anchor(Session *s, int *ch, int *scene)
{
    int a = s ? atomic_load(&s->q_anchor) : -1;
    if (a < 0) return false;
    if (ch) *ch = a / MAX_SCENES;
    if (scene) *scene = a % MAX_SCENES;
    return true;
}

bool session_queue_add(Session *s, int ch, int scene)
{
    if (!s || !qclip(s, ch * MAX_SCENES + scene)) return false;
    if (s->queue_len >= QUEUE_MAX) return false;
    if (s->queue_len == 0 || atomic_load(&s->q_anchor) < 0) {
        /* "now playing": the last one started, if it is still playing; otherwise the first one playing */
        int anchor = -1;
        Clip *ls = qclip(s, s->last_started);
        if (ls && atomic_load(&ls->playing)) anchor = s->last_started;
        for (int c = 0; c < s->channel_count && anchor < 0; ++c)
            for (int sc = 0; sc < s->scene_count && anchor < 0; ++sc) {
                Clip *cl = &s->channels[c].clips[sc];
                if (atomic_load(&cl->has_clip) && !atomic_load(&cl->is_note) && cl->stream && atomic_load(&cl->playing))
                    anchor = c * MAX_SCENES + sc;
            }
        if (anchor < 0) return false;                  // nothing is playing -> no queue
        s->queue_len = 0;
        atomic_store(&s->q_launched, 0);
        atomic_store(&s->q_anchor, anchor);
    }
    s->queue_ch[s->queue_len] = ch;
    s->queue_sc[s->queue_len] = scene;
    s->queue_len++;
    printf("queue: + [%d][%d] (%d in queue)\n", ch, scene, s->queue_len);
    return true;
}

void session_queue_remove(Session *s, int i)
{
    if (!s || i < 0 || i >= s->queue_len) return;
    if (i == 0) q_disarm(s);                           // remove the prepared one
    for (int k = i; k < s->queue_len - 1; ++k) {
        s->queue_ch[k] = s->queue_ch[k + 1];
        s->queue_sc[k] = s->queue_sc[k + 1];
    }
    s->queue_len--;
}

void session_queue_move(Session *s, int from, int to)
{
    if (!s || from < 0 || from >= s->queue_len || to < 0 || to >= s->queue_len || from == to) return;
    if (from == 0 || to == 0) q_disarm(s);             // the first one changes -> prepare again
    int c = s->queue_ch[from], sc = s->queue_sc[from];
    if (from < to) {
        for (int k = from; k < to; ++k) { s->queue_ch[k] = s->queue_ch[k + 1]; s->queue_sc[k] = s->queue_sc[k + 1]; }
    } else {
        for (int k = from; k > to; --k) { s->queue_ch[k] = s->queue_ch[k - 1]; s->queue_sc[k] = s->queue_sc[k - 1]; }
    }
    s->queue_ch[to] = c;
    s->queue_sc[to] = sc;
}

/* stops the other clips in the channel of the one just started (as with a normal start);
 * a clip that is currently fading out in a crossfade is left to finish the transition */
static void q_stop_others(Session *s, int idx)
{
    int ch = idx / MAX_SCENES, sc = idx % MAX_SCENES;
    if (ch >= s->channel_count) return;
    for (int k = 0; k < s->scene_count; ++k) {
        if (k == sc) continue;
        Clip *o = &s->channels[ch].clips[k];
        if (atomic_load(&o->xf_dir) < 0) continue;
        if (atomic_load(&o->has_clip) && !atomic_load(&o->is_note) && atomic_load(&o->playing)) clip_stop(o);
    }
}

void session_set_crossfade(Session *s, float sec)
{
    if (!s) return;
    if (!(sec > 0.0f)) sec = 0.0f;
    if (sec > XFADE_MAX_SEC) sec = XFADE_MAX_SEC;
    int sr = s->sample_rate > 0 ? s->sample_rate : 48000;
    atomic_store(&s->xfade_frames, (long)(sec * (float)sr + 0.5f));
}

float session_get_crossfade(Session *s)
{
    if (!s) return 0.0f;
    int sr = s->sample_rate > 0 ? s->sample_rate : 48000;
    return (float)atomic_load(&s->xfade_frames) / (float)sr;
}

/* Where the music really starts/ends (for the crossfade): skip DIGITAL silence at the start/end
 * (below -50 dBFS), but at most 15 s. Uses the waveform "sketch"; if it is not ready yet — no skipping. */
#define AUD_THR      0.0031623f            /* -50 dBFS */
#define AUD_MAX_SKIP 15.0
static float blk_level(const PeakVal *v)
{
    float m = -v->lmin;
    if (v->lmax > m) m = v->lmax;
    if (-v->rmin > m) m = -v->rmin;
    if (v->rmax > m) m = v->rmax;
    return m;
}
static size_t audible_end(Session *s, Clip *c, size_t from, size_t end)
{
    if (!c->peaks || !peaks_done(c->peaks) || end <= from) return end;
    size_t lim = (size_t)(AUD_MAX_SKIP * (s->sample_rate > 0 ? s->sample_rate : 48000));
    size_t lo = end > from + lim ? end - lim : from;
    for (size_t b = (end - 1) / PEAKS_BLOCK + 1; b-- > lo / PEAKS_BLOCK; ) {
        PeakVal v;
        if (!session_clip_peaks(c, b * PEAKS_BLOCK, (b + 1) * PEAKS_BLOCK, &v)) continue;   /* empty (cut out) = silence */
        if (blk_level(&v) > AUD_THR) {
            size_t e = (b + 1) * PEAKS_BLOCK;
            return e < end ? e : end;
        }
    }
    return lo;
}
static size_t audible_start(Session *s, Clip *c, size_t from, size_t end)
{
    if (!c->peaks || !peaks_done(c->peaks) || end <= from) return from;
    size_t lim = (size_t)(AUD_MAX_SKIP * (s->sample_rate > 0 ? s->sample_rate : 48000));
    size_t hi = end < from + lim ? end : from + lim;
    for (size_t b = from / PEAKS_BLOCK; b * PEAKS_BLOCK < hi; ++b) {
        PeakVal v;
        if (!session_clip_peaks(c, b * PEAKS_BLOCK, (b + 1) * PEAKS_BLOCK, &v)) continue;
        if (blk_level(&v) > AUD_THR) {
            size_t st = b * PEAKS_BLOCK;
            return st > from ? st : from;
        }
    }
    return hi;
}

/* seconds until the end of the clip (while playing); <0 = never (loop) */
static double q_remaining_sec(Session *s, Clip *a)
{
    if (atomic_load(&a->loop)) return -1.0;
    size_t end = atomic_load(&a->end_marker);
    if (end == 0 || end > a->wav.frame_count) end = a->wav.frame_count;
    size_t pos = atomic_load(&a->position);
    double rem = end > pos ? (double)(end - pos) : 0.0;
    int sr = s->sample_rate > 0 ? s->sample_rate : 48000;
    return rem / session_clip_speed(a) / (double)sr;
}

void session_queue_tick(Session *s)
{
    if (!s) return;

    /* the audio thread started the next one exactly at the end -> it becomes "now playing" */
    int L = atomic_exchange(&s->q_launched, 0);
    if (L > 0) {
        int idx = L - 1;
        if (s->queue_len > 0 && s->queue_ch[0] * MAX_SCENES + s->queue_sc[0] == idx) session_queue_remove(s, 0);
        atomic_store(&s->q_anchor, idx);
        s->last_started = idx;
        q_stop_others(s, idx);
        printf("queue: next started [%d][%d] (%d left)\n", idx / MAX_SCENES, idx % MAX_SCENES, s->queue_len);
    }

    if (s->queue_len == 0) {
        if (atomic_load(&s->q_anchor) >= 0) session_queue_clear(s);
        return;
    }

    /* remove cells that are no longer clips (deleted etc.) */
    for (int i = 0; i < s->queue_len; ) {
        if (!qclip(s, s->queue_ch[i] * MAX_SCENES + s->queue_sc[i])) session_queue_remove(s, i);
        else ++i;
    }
    if (s->queue_len == 0) { session_queue_clear(s); return; }

    int an = atomic_load(&s->q_anchor);
    Clip *a = qclip(s, an);
    if (!a) { session_queue_clear(s); return; }
    int head = s->queue_ch[0] * MAX_SCENES + s->queue_sc[0];

    if (atomic_load(&a->playing)) {
        /* prepare the next one ~3 s before the end, so it starts without a single gap */
        int nx = atomic_load(&s->q_next);
        if (nx >= 0 && nx != head) { q_disarm(s); nx = -1; }
        double rem = q_remaining_sec(s, a);
        double xf_sec = (double)session_get_crossfade(s);
        if (nx < 0 && head != an && rem >= 0.0 && rem < 3.0 + xf_sec + (xf_sec > 0 ? AUD_MAX_SKIP : 0.0)) {
            Clip *b = qclip(s, head);
            if (b && !atomic_load(&b->playing)) {
                atomic_store(&b->paused, false);
                atomic_store(&b->ended, false);
                atomic_store(&b->xf_dir, 0);
                b->start_offset = 0;
                size_t from = STREAM_FROM_START;
                atomic_store(&a->aud_end, 0);
                if (xf_sec > 0.0) {
                    /* crossfade: measure where a really ends and where b really starts (without the silence) */
                    size_t ae = atomic_load(&a->end_marker);
                    if (ae == 0 || ae > a->wav.frame_count) ae = a->wav.frame_count;
                    atomic_store(&a->aud_end, audible_end(s, a, atomic_load(&a->start_marker), ae));
                    size_t bs = atomic_load(&b->start_marker), be = atomic_load(&b->end_marker);
                    if (be == 0 || be > b->wav.frame_count) be = b->wav.frame_count;
                    size_t as = audible_start(s, b, bs, be);
                    if (as > bs) from = as;
                    printf("queue: crossfade %.1f s prepared (skips %.2f s of silence at the end, %.2f s at the start)\n",
                           xf_sec, (double)(ae - atomic_load(&a->aud_end)) / s->sample_rate,
                           (double)(from == STREAM_FROM_START ? 0 : from - bs) / s->sample_rate);
                }
                unsigned g = atomic_fetch_add(&b->stretch_gen, 1) + 1;
                stream_start(b->stream, from);
                atomic_store(&b->position, stream_position(b->stream));
                if (atomic_load(&b->warp) && b->stretch && stretch_threaded(b->stretch)) {
                    /* Rubber Band: compute the start in advance, so there is no delay on start */
                    b->stretch_seen = g;
                    b->stretch_live = true;
                    stretch_prepare(b->stretch, session_clip_speed(b), q_pull, b->stream);
                }
                atomic_store(&s->q_next, head);
            }
        }
        return;
    }
    if (atomic_load(&a->paused)) return;               // pause: wait

    /* "now playing" has stopped */
    if (atomic_load(&a->ended)) {
        /* it ended by itself, but the next one was not started by the audio thread (e.g. the same clip) -> start it now */
        q_disarm(s);
        Clip *b = qclip(s, head);
        session_queue_remove(s, 0);
        if (b) {
            q_stop_others(s, head);
            clip_start(b, STREAM_FROM_START);
            atomic_store(&s->q_anchor, head);
            s->last_started = head;
        } else {
            session_queue_clear(s);
        }
    } else {
        session_queue_clear(s);                        // stopped by hand / nothing playing -> the queue is cleared
    }
}

/* ============ Cutting without changing the file (step 27) ============ */

static Clip *edit_clip(Session *s, int ch, int scene)
{
    if (!s || ch < 0 || ch >= s->channel_count || scene < 0 || scene >= MAX_SCENES) return NULL;
    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip) || atomic_load(&cl->is_note) || !cl->edit || !cl->stream) return NULL;
    return cl;
}

const EditList *session_clip_edits(Session *s, int ch, int scene)
{
    Clip *cl = edit_clip(s, ch, scene);
    return cl ? atomic_load(&cl->edit->list) : NULL;
}

bool session_clip_is_edited(Session *s, int ch, int scene)
{
    const EditList *e = session_clip_edits(s, ch, scene);
    return e && !edit_is_identity(e);
}

int session_clip_apply_edits(Session *s, int ch, int scene, EditList *nl, long long crop_a)
{
    Clip *cl = edit_clip(s, ch, scene);
    if (!cl || !nl || !edit_valid(nl) || nl->len <= 0) { edit_free(nl); return 1; }
    EditList *cur = atomic_load(&cl->edit->list);
    nl->src_len = cur ? cur->src_len : nl->src_len;

    bool active = atomic_load(&cl->playing) || atomic_load(&cl->paused);
    size_t pos = atomic_load(&cl->position);
    size_t old_len = cl->wav.frame_count;
    size_t L = (size_t)nl->len;

    /* swap: the disk thread is stopped (streams_lock), so nobody is inside a read with the old list */
    streams_lock();
    EditList *old = atomic_exchange(&cl->edit->list, nl);
    stream_set_total_nolock(cl->stream, L);
    streams_unlock();
    edit_free(old);
    stream_invalidate_preroll(cl->stream);
    __atomic_store_n(&cl->wav.frame_count, L, __ATOMIC_RELAXED);

    /* markers and loop: on crop - the whole new clip; otherwise they stay, but within bounds
     * (a marker that was exactly at the end follows the end if the clip grows) */
    size_t sm = atomic_load(&cl->start_marker), em = atomic_load(&cl->end_marker);
    size_t ls = atomic_load(&cl->loop_start), le = atomic_load(&cl->loop_end);
    if (crop_a >= 0) {
        sm = 0; em = L; ls = 0; le = L;
        pos = pos >= (size_t)crop_a ? pos - (size_t)crop_a : 0;
    } else {
        if (em == 0 || em >= old_len || em > L) em = L;
        if (le == 0 || le >= old_len || le > L) le = L;
        if (sm >= em) sm = 0;
        if (ls >= le) ls = 0;
    }
    if (pos > L) pos = sm;
    atomic_store(&cl->start_marker, sm);
    atomic_store(&cl->end_marker, em);
    atomic_store(&cl->loop_start, ls);
    atomic_store(&cl->loop_end, le);
    int64_t ws = (int64_t)atomic_load(&cl->wave_scroll_frame);      /* may be < 0 (step 36j) */
    if (ws > 2 * (int64_t)L || ws < -(int64_t)L) atomic_store(&cl->wave_scroll_frame, 0);
    atomic_store(&cl->aud_end, 0);

    /* playing -> continues from the same place, now with the new parts (smoothly, like a click in the waveform) */
    if (active) session_seek_clip(s, ch, scene, pos);
    else atomic_store(&cl->position, pos);
    return 0;
}

char *session_clip_edits_text(Session *s, int ch, int scene)
{
    const EditList *e = session_clip_edits(s, ch, scene);
    if (!e || edit_is_identity(e)) return NULL;
    return edit_to_text(e);
}

int session_clip_set_edits_text(Session *s, int ch, int scene, const char *text)
{
    Clip *cl = edit_clip(s, ch, scene);
    if (!cl) return 1;
    EditList *cur = atomic_load(&cl->edit->list);
    int64_t src_len = cur ? cur->src_len : (int64_t)cl->wav.frame_count;
    EditList *nl = (text && text[0]) ? edit_from_text(text, src_len) : edit_new(src_len);
    if (!nl) return 1;
    if (cur && cur->n == nl->n && cur->len == nl->len &&
        memcmp(cur->seg, nl->seg, (size_t)nl->n * sizeof(EditSeg)) == 0) { edit_free(nl); return 0; }   /* the same */
    return session_clip_apply_edits(s, ch, scene, nl, -1);
}

/* merges b into a (min/max; RMS - the larger one, good enough for drawing) */
static void pv_merge(PeakVal *a, const PeakVal *b, bool *have)
{
    if (!*have) { *a = *b; *have = true; return; }
    if (b->lmin < a->lmin) a->lmin = b->lmin;
    if (b->lmax > a->lmax) a->lmax = b->lmax;
    if (b->rmin < a->rmin) a->rmin = b->rmin;
    if (b->rmax > a->rmax) a->rmax = b->rmax;
    if (b->lrms > a->lrms) a->lrms = b->lrms;
    if (b->rrms > a->rrms) a->rrms = b->rrms;
}

bool session_clip_peaks(const Clip *cl, size_t f0, size_t f1, PeakVal *out)
{
    return session_clip_peaks_list(cl, cl && cl->edit ? atomic_load(&cl->edit->list) : NULL, f0, f1, out);
}

bool session_clip_peaks_list(const Clip *cl, const EditList *e, int64_t f0, int64_t f1, PeakVal *out)
{
    if (!cl || !cl->peaks || f1 <= f0) return false;
    if (!e || edit_is_identity(e)) {                     /* step 36j: the view may also be outside the clip */
        int64_t L = e ? e->len : (int64_t)cl->wav.frame_count;
        if (f0 < 0) f0 = 0;
        if (f1 > L) f1 = L;
        if (f1 <= f0) return false;
        return peaks_range(cl->peaks, (size_t)f0 / PEAKS_BLOCK, (size_t)(f1 - 1) / PEAKS_BLOCK, out);
    }
    bool have = false;
    int i = edit_find(e, (int64_t)f0);
    if (i < 0) {                                         /* f0 is in empty space: the next part */
        i = 0;
        while (i < e->n && e->seg[i].t + e->seg[i].len <= (int64_t)f0) ++i;
    }
    for (; i < e->n && e->seg[i].t < (int64_t)f1; ++i) {
        const EditSeg *sg = &e->seg[i];
        int64_t a = (int64_t)f0 > sg->t ? (int64_t)f0 : sg->t;
        int64_t b = (int64_t)f1 < sg->t + sg->len ? (int64_t)f1 : sg->t + sg->len;
        if (b <= a) continue;
        int64_t s0 = sg->src + (a - sg->t), s1 = sg->src + (b - sg->t);
        PeakVal v;
        if (!peaks_range(cl->peaks, (size_t)s0 / PEAKS_BLOCK, (size_t)(s1 - 1) / PEAKS_BLOCK, &v)) continue;
        /* fades: the level in the middle of the section (good enough for the picture) */
        float g = edit_gain(e, i, (a + b) / 2 - sg->t) * edit_xfade_base_gain(e, (a + b) / 2);
        if (g < 0.999f) {
            v.lmin *= g; v.lmax *= g; v.rmin *= g; v.rmax *= g; v.lrms *= g; v.rrms *= g;
        }
        pv_merge(out, &v, &have);
    }
    /* the crossfades: the other audio too (in the middle of the section) */
    int64_t mid = ((int64_t)f0 + (int64_t)f1) / 2, xs;
    float xg;
    if (edit_xfade_extra(e, mid, &xs, &xg) && xs >= 0) {
        int64_t xs0 = xs - (mid - (int64_t)f0), xs1 = xs + ((int64_t)f1 - mid);
        if (xs0 < 0) xs0 = 0;
        PeakVal v;
        if (xs1 > xs0 && peaks_range(cl->peaks, (size_t)xs0 / PEAKS_BLOCK, (size_t)(xs1 - 1) / PEAKS_BLOCK, &v)) {
            v.lmin *= xg; v.lmax *= xg; v.rmin *= xg; v.rmax *= xg; v.lrms *= xg; v.rrms *= xg;
            if (have) {   /* the sum of the two sounds: approximate (for the picture) */
                out->lmin += v.lmin; out->lmax += v.lmax; out->rmin += v.rmin; out->rmax += v.rmax;
                out->lrms = sqrtf(out->lrms * out->lrms + v.lrms * v.lrms);
                out->rrms = sqrtf(out->rrms * out->rrms + v.rrms * v.rrms);
            } else { *out = v; have = true; }
        }
    }
    return have;
}

// ============ Plugins on the channel (step 33) ============

int session_fx_add(Session *s, int ch, const char *fmt, const char *name, const char *path)
{
    if (!s || ch < 0 || ch >= s->channel_count) return -1;
    Channel *c = &s->channels[ch];
    if (c->fx_count >= MAX_CH_FX) return -1;
    ChFx *f = &c->fx[c->fx_count];
    memset(f, 0, sizeof *f);
    snprintf(f->fmt, sizeof f->fmt, "%s", fmt ? fmt : "");
    snprintf(f->name, sizeof f->name, "%s", name ? name : "");
    snprintf(f->path, sizeof f->path, "%s", path ? path : "");
    return c->fx_count++;
}

/* 1.2: an instrument goes to the front of the chain (the effects after it process its sound) */
int session_fx_insert(Session *s, int ch, int pos, const char *fmt, const char *name, const char *path)
{
    if (!s || ch < 0 || ch >= s->channel_count) return -1;
    Channel *c = &s->channels[ch];
    if (c->fx_count >= MAX_CH_FX) return -1;
    if (pos < 0) pos = 0;
    if (pos > c->fx_count) pos = c->fx_count;
    memmove(&c->fx[pos + 1], &c->fx[pos], sizeof(ChFx) * (size_t)(c->fx_count - pos));
    ChFx *f = &c->fx[pos];
    memset(f, 0, sizeof *f);
    snprintf(f->fmt, sizeof f->fmt, "%s", fmt ? fmt : "");
    snprintf(f->name, sizeof f->name, "%s", name ? name : "");
    snprintf(f->path, sizeof f->path, "%s", path ? path : "");
    c->fx_count++;
    return pos;
}

/* 1.2: move a plugin in the chain (the device panel) */
void session_fx_move(Session *s, int ch, int from, int to)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    Channel *c = &s->channels[ch];
    if (from < 0 || from >= c->fx_count || to < 0 || to >= c->fx_count || from == to) return;
    ChFx tmp = c->fx[from];
    if (from < to) memmove(&c->fx[from], &c->fx[from + 1], sizeof(ChFx) * (size_t)(to - from));
    else memmove(&c->fx[to + 1], &c->fx[to], sizeof(ChFx) * (size_t)(from - to));
    c->fx[to] = tmp;
}

void session_fx_remove(Session *s, int ch, int idx)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    Channel *c = &s->channels[ch];
    if (idx < 0 || idx >= c->fx_count) return;
    memmove(&c->fx[idx], &c->fx[idx + 1], sizeof(ChFx) * (size_t)(c->fx_count - idx - 1));
    c->fx_count--;
}

void session_fx_toggle_bypass(Session *s, int ch, int idx)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    Channel *c = &s->channels[ch];
    if (idx < 0 || idx >= c->fx_count) return;
    c->fx[idx].bypass = !c->fx[idx].bypass;
}
