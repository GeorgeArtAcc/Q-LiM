/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _POSIX_C_SOURCE 200809L
#include "session.h"
#include "schedule.h"
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
    /* 2.1: the audio thread is late (an overloaded or stuck device, a slow machine): before, this simply went on after
     * 0.1 s and a clip's stream could be freed while a render was still reading it (found by the GUI monkey test under
     * ASan). A render holds mix_mu for its whole block, so taking it once = no render that saw the old state still runs.
     * (Never called with mix_mu held.) */
    mix_lock(s);
    pthread_mutex_unlock(&s->mix_mu);
}

static void rec_abort_all(Session *s);              /* 1.4: see below */

/* 1.4.19: memory the audio thread may still be reading (surround setup / points): freed two audio cycles later */
#define RET_N 64
static struct { void *p; unsigned cyc; } g_ret[RET_N];
static int g_nret = 0;
static void retire_reap(Session *s, bool all)
{
    unsigned now = atomic_load(&s->audio_cycles);
    int k = 0;
    for (int i = 0; i < g_nret; ++i) {
        if (all || !atomic_load(&s->audio_running) || now - g_ret[i].cyc >= 2) free(g_ret[i].p);
        else g_ret[k++] = g_ret[i];
    }
    g_nret = k;
}
static void retire(Session *s, void *p)
{
    if (!p) return;
    if (!s || !atomic_load(&s->audio_running)) { free(p); return; }
    retire_reap(s, false);
    if (g_nret >= RET_N) { audio_barrier(s); retire_reap(s, true); }
    g_ret[g_nret].p = p; g_ret[g_nret].cyc = atomic_load(&s->audio_cycles);
    g_nret++;
}
static void clip_sur_free(Session *s, Clip *cl) { retire(s, atomic_exchange(&cl->sur_auto, NULL)); }

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

static struct { int mode, out; bool set; char orig[1024]; } g_next_mc;    /* 1.4.24: how the next session_set_clip opens its file */

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
    if (!l) { memset(out, 0, n * (size_t)stream_src_nch(&e->inner) * sizeof(float)); return 0; }
    if (l->n == 1 && l->seg[0].t == 0 && l->seg[0].src == 0 && l->seg[0].fi1 <= l->seg[0].fi0 &&
        l->seg[0].fo1 <= l->seg[0].fo0 && l->len == l->src_len)
        return e->inner.read(e->inner.ctx, pos, out, n);      /* uncut: straight from the file */
    return edit_read_n(l, es_inner_read, e, (int64_t)pos, out, n, stream_src_nch(&e->inner));
}
static void es_close(void *ctx)
{
    EditSrc *e = ctx;
    if (e->inner.close) e->inner.close(e->inner.ctx);
    edit_free(atomic_load(&e->list));
    free(e);
}

/* ===== 1.4.14/1.4.17: the separated stems as one source: the sum of N parts, each with its level / mute / solo
 * (read by the disk thread) ===== */
#define STEM_TMP 4096
typedef struct StemSrc {
    int           n;
    StreamSource  src[STEM_MAX];
    _Atomic float db[STEM_MAX];
    atomic_bool   mute[STEM_MAX];
    atomic_bool   solo[STEM_MAX];
    _Atomic int   cc[STEM_MAX];        /* 1.4.17: MIDI CC + 1 that moves the level (0 = none) */
    atomic_bool   dirty;               /* moved by MIDI: re-mixed by session_stems_poll */
    char          path[STEM_MAX][1024];
    char          name[STEM_MAX][STEM_NAME_LEN];
    float         acc[STEM_TMP * 2], tmp[STEM_TMP * 2];
} StemSrc;

static bool stem_any_solo(StemSrc *t)
{
    for (int k = 0; k < t->n; ++k) if (atomic_load(&t->solo[k])) return true;
    return false;
}
static float stem_gain(StemSrc *t, int k)
{
    if (atomic_load(&t->mute[k])) return 0.0f;
    if (stem_any_solo(t) && !atomic_load(&t->solo[k])) return 0.0f;
    float d = atomic_load(&t->db[k]);
    return d <= CLIP_GAIN_MIN_DB ? 0.0f : d == 0.0f ? 1.0f : powf(10.0f, d / 20.0f);
}
static size_t stem_frames(void *ctx) { StemSrc *t = ctx; return t->src[0].frames(t->src[0].ctx); }
static size_t stem_read(void *ctx, size_t pos, float *out, size_t n)
{
    StemSrc *t = ctx;
    float g[STEM_MAX];
    for (int k = 0; k < t->n; ++k) g[k] = stem_gain(t, k);
    size_t real = 0;
    for (size_t done = 0; done < n;) {
        size_t m = n - done < STEM_TMP ? n - done : STEM_TMP;
        float *o = out + done * 2;
        memset(o, 0, m * 2 * sizeof(float));
        size_t best = 0;
        for (int k = 0; k < t->n; ++k) {
            size_t a = t->src[k].read(t->src[k].ctx, pos + done, t->tmp, m);  /* (read even when silent: keeps the decoders in step) */
            if (a > best) best = a;
            if (g[k] == 0.0f) continue;
            if (a < m) memset(t->tmp + a * 2, 0, (m - a) * 2 * sizeof(float));
            for (size_t j = 0; j < m * 2; ++j) o[j] += t->tmp[j] * g[k];
        }
        real = done + best;
        done += m;
        if (best < m) break;
    }
    if (real < n) memset(out + real * 2, 0, (n - real) * 2 * sizeof(float));
    return real;
}
static void stem_close(void *ctx)
{
    StemSrc *t = ctx;
    if (!t) return;
    for (int k = 0; k < t->n; ++k) if (t->src[k].close) t->src[k].close(t->src[k].ctx);
    free(t);
}
static StemSrc *stem_open_set(int sr, const StemSet *set, StreamSource *out)
{
    if (!set || set->n < 1 || set->n > STEM_MAX) return NULL;
    StemSrc *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    for (int k = 0; k < set->n; ++k) {
        int fsr = 0;
        if (stream_source_open(set->path[k], sr, &t->src[k], &fsr) != 0) {
            for (int q = 0; q < k; ++q) t->src[q].close(t->src[q].ctx);
            free(t);
            return NULL;
        }
        snprintf(t->path[k], sizeof t->path[k], "%s", set->path[k]);
        snprintf(t->name[k], sizeof t->name[k], "%s", set->name[k][0] ? set->name[k] : "Part");
    }
    t->n = set->n;
    *out = (StreamSource){ stem_frames, stem_read, stem_close, t, 0 };
    return t;
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
    cl->stems = NULL;                      /* (it was the cut wrapper's source: closed with the stream) */
    for (int k = 0; k < STEM_MAX; ++k) if (cl->stem_peaks[k]) { peaks_release(cl->stem_peaks[k]); cl->stem_peaks[k] = NULL; }
    if (cl->peaks) {
        peaks_release(cl->peaks);
        cl->peaks = NULL;
    }
    for (int k = 0; k < STREAM_MAX_CH; ++k) if (cl->mc_peaks[k]) { peaks_release(cl->mc_peaks[k]); cl->mc_peaks[k] = NULL; }
    free(cl->mc_orig);                     /* 1.4.24 */
    cl->mc_orig = NULL;
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
    atomic_store(&cl->tr_req_len, -1);                  // 1.4.17: ... and a scene transition (full level)
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
    clip_sur_free(s, cl);                       /* 1.4.19 */
    int keep_bind = atomic_load(&cl->bind);     // the pad stays bound to the cell when the clip is replaced
    mix_lock(s);                                // the audio thread does not look at the cell while it is being reset
    memset(cl, 0, sizeof(*cl));
    atomic_store(&cl->bind, keep_bind);
    pthread_mutex_unlock(&s->mix_mu);
}

static bool g_bpm_pending = true;              /* 2.0: a tempo detection may be running (UI thread) */

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
    atomic_store(&s->tr_sec, 4.0f);                    /* 1.4.17 */
    s->tr_cur = -1;
    atomic_store(&s->sig_num, 4);                      /* 1.4.8 */
    atomic_store(&s->sig_den, 4);
    s->quant_mode = QUANT_BAR;
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
    for (int c = 0; c < s->channel_count; ++c) {
        for (int sc = 0; sc < MAX_SCENES; ++sc) {
            clip_release_stream(&s->channels[c].clips[sc]);
            clip_sur_free(s, &s->channels[c].clips[sc]);          /* 2.1: the surround points (found by fuzz_project) */
        }
        retire(s, atomic_exchange(&s->channels[c].sur, NULL));     /* 2.1: and the channel's surround setup */
    }
    audio_barrier(s);                                                /* nothing waits to be freed after this */
    retire_reap(s, true);
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
    /* step 36h: the first channel is in the Q-LiM orange, then colours that look good next to it;
     * 1.4.12: orange, navy, dark green, dark red (as the author uses them), then the others */
    static const int ORDER[8] = { 4, 10, 12, 13, 11, 16, 0, 7 };
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
    StreamSource wrapped = { es_frames, es_read, es_close, es, src->nch };

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
        char what[32];
        int kind = audio_sniff(path, what, sizeof what);   /* 1.4.14: by content, not by name */
        if (kind == SNIFF_MISSING) { printf("session_set_clip: '%s' does not exist or cannot be read\n", path); return 1; }
        printf("session_set_clip: '%s' is %s - read through ffmpeg (once, cached)\n", path, what[0] ? what : "a format libsndfile cannot read");
        if (wav_convert_with_ffmpeg(path, sr, cache, cache_size) != 0) return 1;
        *open_path = cache;
        return stream_source_open(cache, sr, src, &fsr);
    }

    if (fsr == sr) {                    // already at the needed rate
        *src = nat;
        return 0;
    }

    if (atomic_load(&s->convert_files)) {
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

/* 1.4.14: a fresh, private source for an offline render (the same decoding and caching as when the clip was loaded) */
int session_open_clip_source(Session *s, const char *path, StreamSource *out)
{
    if (!s || !path || !out) return 1;
    int sr = s->sample_rate > 0 ? s->sample_rate : 48000;
    char cache[1024], mcache[1024];
    const char *open_path = path;
    const char *rp = wav_readable_all_channels(path, sr, mcache, sizeof mcache);   /* 2.1: the same copy as the clip */
    return open_clip_source(s, rp, sr, out, cache, sizeof cache, &open_path);
}

int session_clip_snapshot(Session *s, int ch, int scene, ClipSnapshot *o)
{
    if (!s || !o || ch < 0 || ch >= s->channel_count || scene < 0 || scene >= MAX_SCENES) return 1;
    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip) || atomic_load(&cl->is_note) || atomic_load(&cl->is_midi) || !cl->stream || !cl->edit) return 1;
    memset(o, 0, sizeof *o);
    snprintf(o->path, sizeof o->path, "%s", cl->path);
    snprintf(o->name, sizeof o->name, "%s", cl->name);
    EditList *el = atomic_load(&cl->edit->list);
    o->edits = el ? edit_clone(el) : NULL;
    if (!o->edits) return 1;
    size_t len = (size_t)o->edits->len;
    o->start = atomic_load(&cl->start_marker);
    o->end   = atomic_load(&cl->end_marker);
    if (o->end == 0 || o->end > len) o->end = len;
    if (o->start >= o->end) o->start = 0;
    o->stretch  = atomic_load(&cl->stretch_on);
    o->speed    = o->stretch ? session_clip_speed(cl) : 1.0;
    o->pitch_st = atomic_load(&cl->pitch_st);
    o->formants = atomic_load(&cl->pitch_fmt);
    float g = atomic_load(&cl->cgain_db);
    o->gain_db  = g <= CLIP_GAIN_MIN_DB ? g : g + atomic_load(&cl->norm_db);
    o->pan      = atomic_load(&cl->cpan);
    o->sample_rate = s->sample_rate > 0 ? s->sample_rate : 48000;
    if (cl->mc_mode == CLIP_MC_MULTI && stream_channels(cl->stream) > 2) o->mc_nch = stream_channels(cl->stream);   /* 1.4.24 */
    if (cl->stems) {                                   /* 1.4.14 */
        o->stems = true;
        o->stem_n = cl->stems->n;
        for (int k = 0; k < cl->stems->n; ++k) {
            snprintf(o->stem_path[k], sizeof o->stem_path[k], "%s", cl->stems->path[k]);
            snprintf(o->stem_name[k], sizeof o->stem_name[k], "%s", cl->stems->name[k]);
            o->stem_gain[k] = stem_gain(cl->stems, k);
        }
    }
    return 0;
}

int session_open_snapshot_source(Session *s, const ClipSnapshot *c, StreamSource *out)
{
    if (!s || !c || !out) return 1;
    if (!c->stems) return session_open_clip_source(s, c->path, out);
    StemSet set;
    memset(&set, 0, sizeof set);
    set.n = c->stem_n;
    for (int k = 0; k < set.n; ++k) snprintf(set.path[k], sizeof set.path[k], "%s", c->stem_path[k]);
    StemSrc *t = stem_open_set(s->sample_rate > 0 ? s->sample_rate : 48000, &set, out);
    if (!t) return 1;
    for (int k = 0; k < t->n; ++k) {           /* the snapshot's gains are final (mute / solo already in them) */
        float g = c->stem_gain[k];
        if (g <= 0.0f) atomic_store(&t->mute[k], true);
        else atomic_store(&t->db[k], 20.0f * log10f(g));
    }
    return 0;
}

static Clip *stem_clip(Session *s, int ch, int scene)
{
    if (!s || ch < 0 || ch >= s->channel_count || scene < 0 || scene >= MAX_SCENES) return NULL;
    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip) || atomic_load(&cl->is_note) || atomic_load(&cl->is_midi) || !cl->stream || !cl->edit) return NULL;
    return cl;
}

int session_clip_set_stem_set(Session *s, int ch, int scene, const StemSet *set)
{
    Clip *cl = stem_clip(s, ch, scene);
    if (!cl) return 1;
    int sr = s->sample_rate > 0 ? s->sample_rate : 48000;
    StreamSource ns;
    StemSrc *t = NULL;
    if (set && set->n > 0) {
        t = stem_open_set(sr, set, &ns);
        if (!t) { printf("session_clip_set_stem_set: cannot open the %d parts\n", set->n); return 1; }
        /* the same parts again (e.g. re-separated): keep their levels */
        if (cl->stems)
            for (int k = 0; k < t->n; ++k)
                for (int q = 0; q < cl->stems->n; ++q)
                    if (!strcmp(t->name[k], cl->stems->name[q])) {
                        atomic_store(&t->db[k], atomic_load(&cl->stems->db[q]));
                        atomic_store(&t->mute[k], atomic_load(&cl->stems->mute[q]));
                        atomic_store(&t->solo[k], atomic_load(&cl->stems->solo[q]));
                        atomic_store(&t->cc[k], atomic_load(&cl->stems->cc[q]));
                    }
    } else {
        if (!cl->stems) return 0;
        char cache[1024];
        const char *op = cl->path;
        if (open_clip_source(s, cl->path, sr, &ns, cache, sizeof cache, &op) != 0) return 1;
    }
    /* swap the source under the cuts: the disk thread is not inside a read while locked */
    streams_lock();
    StreamSource old = cl->edit->inner;
    cl->edit->inner = ns;
    cl->stems = t;
    streams_unlock();
    if (old.close) old.close(old.ctx);
    stream_invalidate_preroll(cl->stream);
    for (int k = 0; k < STEM_MAX; ++k) if (cl->stem_peaks[k]) { peaks_release(cl->stem_peaks[k]); cl->stem_peaks[k] = NULL; }
    if (t) for (int k = 0; k < t->n; ++k) cl->stem_peaks[k] = peaks_start(t->path[k], sr, cl->wav.frame_count);
    session_clip_stems_apply(s, ch, scene);
    return 0;
}

int session_clip_set_stems(Session *s, int ch, int scene, const char *vocals, const char *inst)
{
    if (!vocals || !inst) return session_clip_set_stem_set(s, ch, scene, NULL);
    StemSet set;
    memset(&set, 0, sizeof set);
    set.n = 2;
    snprintf(set.path[0], sizeof set.path[0], "%s", vocals);
    snprintf(set.path[1], sizeof set.path[1], "%s", inst);
    snprintf(set.name[0], sizeof set.name[0], "Vocals");
    snprintf(set.name[1], sizeof set.name[1], "Instrumental");
    return session_clip_set_stem_set(s, ch, scene, &set);
}

bool session_clip_has_stems(Session *s, int ch, int scene) { Clip *cl = stem_clip(s, ch, scene); return cl && cl->stems; }
int  session_clip_stem_count(Session *s, int ch, int scene) { Clip *cl = stem_clip(s, ch, scene); return cl && cl->stems ? cl->stems->n : 0; }

#define STEM_OK(k) (cl && cl->stems && (k) >= 0 && (k) < cl->stems->n)
const char *session_clip_stem_path(Session *s, int ch, int scene, int k)
{
    Clip *cl = stem_clip(s, ch, scene);
    return STEM_OK(k) ? cl->stems->path[k] : NULL;
}
const char *session_clip_stem_name(Session *s, int ch, int scene, int k)
{
    Clip *cl = stem_clip(s, ch, scene);
    return STEM_OK(k) ? cl->stems->name[k] : "";
}
void session_clip_set_stem_level(Session *s, int ch, int scene, int k, float db)
{
    Clip *cl = stem_clip(s, ch, scene);
    if (!STEM_OK(k) || !(db == db)) return;
    if (db < CLIP_GAIN_MIN_DB) db = CLIP_GAIN_MIN_DB;
    if (db > CLIP_GAIN_MAX_DB) db = CLIP_GAIN_MAX_DB;
    db = roundf(db * 100.0f) / 100.0f;
    atomic_store(&cl->stems->db[k], db);
}
float session_clip_get_stem_level(Session *s, int ch, int scene, int k)
{
    Clip *cl = stem_clip(s, ch, scene);
    return STEM_OK(k) ? atomic_load(&cl->stems->db[k]) : 0.0f;
}
void session_clip_set_stem_mute(Session *s, int ch, int scene, int k, bool mute)
{
    Clip *cl = stem_clip(s, ch, scene);
    if (STEM_OK(k)) atomic_store(&cl->stems->mute[k], mute);
}
bool session_clip_get_stem_mute(Session *s, int ch, int scene, int k)
{
    Clip *cl = stem_clip(s, ch, scene);
    return STEM_OK(k) && atomic_load(&cl->stems->mute[k]);
}
void session_clip_set_stem_solo(Session *s, int ch, int scene, int k, bool solo)
{
    Clip *cl = stem_clip(s, ch, scene);
    if (STEM_OK(k)) atomic_store(&cl->stems->solo[k], solo);
}
bool session_clip_get_stem_solo(Session *s, int ch, int scene, int k)
{
    Clip *cl = stem_clip(s, ch, scene);
    return STEM_OK(k) && atomic_load(&cl->stems->solo[k]);
}
bool session_clip_stem_audible(Session *s, int ch, int scene, int k)
{
    Clip *cl = stem_clip(s, ch, scene);
    return STEM_OK(k) && stem_gain(cl->stems, k) > 0.0f;
}
float session_clip_stem_gain(Session *s, int ch, int scene, int k)
{
    Clip *cl = stem_clip(s, ch, scene);
    return STEM_OK(k) ? stem_gain(cl->stems, k) : 0.0f;
}
/* 1.4.17: MIDI Learn of a part's level */
void session_clip_set_stem_cc(Session *s, int ch, int scene, int k, int cc)
{
    Clip *cl = stem_clip(s, ch, scene);
    if (STEM_OK(k)) atomic_store(&cl->stems->cc[k], cc >= 0 && cc < 128 ? cc + 1 : 0);
}
int session_clip_get_stem_cc(Session *s, int ch, int scene, int k)
{
    Clip *cl = stem_clip(s, ch, scene);
    return STEM_OK(k) ? atomic_load(&cl->stems->cc[k]) - 1 : -1;
}
static struct { int ch, sc, k; } g_stem_learn = { -1, -1, -1 };
void session_stem_learn_arm(int ch, int scene, int k) { g_stem_learn.ch = ch; g_stem_learn.sc = scene; g_stem_learn.k = k; }
bool session_stem_learn_armed(int *ch, int *scene, int *k)
{
    if (g_stem_learn.ch < 0) return false;
    if (ch) *ch = g_stem_learn.ch;
    if (scene) *scene = g_stem_learn.sc;
    if (k) *k = g_stem_learn.k;
    return true;
}
float session_stem_cc_db(int value)
{
    if (value <= 0) return CLIP_GAIN_MIN_DB;
    if (value == 100) return 0.0f;
    float db = 40.0f * log10f((float)value / 100.0f);        /* a fader law: 100 = 0 dB, 50 = -12 dB, 127 = +4.2 dB */
    return db < CLIP_GAIN_MIN_DB ? CLIP_GAIN_MIN_DB : db;
}
int session_stems_midi_cc(Session *s, int cc, int value)
{
    if (!s) return 0;
    int n = 0;
    for (int c = 0; c < s->channel_count; ++c)
        for (int sc = 0; sc < s->scene_count; ++sc) {
            Clip *cl = &s->channels[c].clips[sc];
            if (!atomic_load(&cl->has_clip) || !cl->stems) continue;
            for (int k = 0; k < cl->stems->n; ++k)
                if (atomic_load(&cl->stems->cc[k]) == cc + 1) {
                    atomic_store(&cl->stems->db[k], roundf(session_stem_cc_db(value) * 100.0f) / 100.0f);
                    atomic_store(&cl->stems->dirty, true);
                    ++n;
                }
        }
    return n;
}
void session_stems_poll(Session *s, double now)
{
    static double last = -1.0;
    if (!s || now - last < 0.1) return;                       /* a moving fader: re-mixed ~10 times a second */
    last = now;
    for (int c = 0; c < s->channel_count; ++c)
        for (int sc = 0; sc < s->scene_count; ++sc) {
            Clip *cl = &s->channels[c].clips[sc];
            if (atomic_load(&cl->has_clip) && cl->stems && atomic_exchange(&cl->stems->dirty, false))
                session_clip_stems_apply(s, c, sc);
        }
}


/* 1.4.17: what "Split to clips" carries: one stem as a clip with everything of the original clip */
int session_clip_capture_part(Session *s, int ch, int scene, int k, ClipPart *p)
{
    Clip *cl = stem_clip(s, ch, scene);
    if (!STEM_OK(k) || !p) return 1;
    memset(p, 0, sizeof *p);
    snprintf(p->path, sizeof p->path, "%s", cl->stems->path[k]);
    char nm[NAME_LEN];
    snprintf(nm, sizeof nm, "%s", cl->name);
    char *dot = strrchr(nm, '.');
    if (dot && dot != nm) *dot = 0;
    snprintf(p->name, sizeof p->name, "%s (%s)", nm, cl->stems->name[k]);
    p->edits = session_clip_edits_text(s, ch, scene);
    p->start = atomic_load(&cl->start_marker); p->end = atomic_load(&cl->end_marker);
    p->loop = atomic_load(&cl->loop);
    p->loop_start = atomic_load(&cl->loop_start); p->loop_end = atomic_load(&cl->loop_end);
    p->bpm_d = atomic_load(&cl->bpm_detected); p->bpm_f = atomic_load(&cl->bpm_fix); p->bpm_p = atomic_load(&cl->bpm_play);
    p->warp = atomic_load(&cl->warp); p->fmt = atomic_load(&cl->pitch_fmt);
    p->pan = atomic_load(&cl->cpan); p->pitch = atomic_load(&cl->pitch_st);
    float g = atomic_load(&cl->cgain_db), sd = atomic_load(&cl->stems->db[k]);
    bool off = atomic_load(&cl->stems->mute[k]) || sd <= CLIP_GAIN_MIN_DB || g <= CLIP_GAIN_MIN_DB;
    p->gain = off ? CLIP_GAIN_MIN_DB : g + sd;
    return 0;
}

int session_place_part(Session *s, int ch, int scene, const ClipPart *p)
{
    if (!s || !p || ch < 0 || ch >= s->channel_count || scene < 0 || scene >= MAX_SCENES) return 1;
    while (scene >= s->scene_count && s->scene_count < MAX_SCENES) session_add_scene(s);
    if (atomic_load(&s->channels[ch].clips[scene].has_clip)) return 2;        /* (only empty cells) */
    if (session_set_clip(s, ch, scene, p->path) != 0) return 1;
    session_rename_clip(s, ch, scene, p->name);
    if (p->edits) session_clip_set_edits_text(s, ch, scene, p->edits);
    Clip *d = &s->channels[ch].clips[scene];
    atomic_store(&d->start_marker, p->start);
    atomic_store(&d->end_marker, p->end);
    atomic_store(&d->loop_start, p->loop_start);
    atomic_store(&d->loop_end, p->loop_end);
    atomic_store(&d->loop, p->loop);
    if (d->stream) stream_invalidate_preroll(d->stream);
    session_set_clip_bpm_fields(s, ch, scene, p->bpm_d, p->bpm_f, p->bpm_p);
    session_set_clip_warp(s, ch, scene, p->warp);
    session_set_clip_gain(s, ch, scene, p->gain);
    session_set_clip_pan(s, ch, scene, p->pan);
    session_set_clip_formants(s, ch, scene, p->fmt);
    session_set_clip_pitch(s, ch, scene, p->pitch);
    return 0;
}

void clip_part_free(ClipPart *p) { if (p) { free(p->edits); p->edits = NULL; } }

void session_clip_stems_apply(Session *s, int ch, int scene)
{
    Clip *cl = stem_clip(s, ch, scene);
    if (!cl) return;
    stream_invalidate_preroll(cl->stream);
    if (atomic_load(&cl->playing) || atomic_load(&cl->paused))
        session_seek_clip(s, ch, scene, atomic_load(&cl->position));   /* what is buffered ahead was mixed the old way */
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
    /* 1.4.24: how this file is to be played (set once by session_load_mc_next: the project / a mode change) */
    int mode = g_next_mc.mode, mout = g_next_mc.out;
    bool asked = g_next_mc.set;
    char morig[1024];
    snprintf(morig, sizeof morig, "%s", g_next_mc.orig);
    memset(&g_next_mc, 0, sizeof g_next_mc);

    StreamSource src;
    char cache[1024], mcache[1024];
    const char *open_path = wav_path;
    /* 2.1: a file only ffmpeg reads (a surround M4A ...): its copy with all the channels stands in for it */
    const char *mc_path = wav_readable_all_channels(wav_path, sr, mcache, sizeof mcache);
    int fch = stream_file_channels(mc_path);
    int mnch = 0;
    if (mode == CLIP_MC_MULTI && fch > 2 && fch <= STREAM_MAX_CH &&
        stream_source_open_mc(mc_path, sr, &src, NULL, &mnch) == 0 && mnch > 2) {
        /* every channel of the file goes through the clip (no warp / pitch for it) */
    } else {
        if (mode == CLIP_MC_MULTI) mode = CLIP_MC_STEREO;
        if (open_clip_source(s, mc_path, sr, &src, cache, sizeof(cache), &open_path) != 0) return 1;
    }

    int arc = session_attach_stream(s, ch, scene, &src, wav_path);
    if (arc == 0) {
        Clip *cl = &s->channels[ch].clips[scene];
        cl->mc_mode = mode;
        cl->mc_asked = asked;
        cl->mc_nch = mode == CLIP_MC_PATH ? 0 : fch;
        atomic_store(&cl->mc_out, mout);
        free(cl->mc_orig);
        cl->mc_orig = mode == CLIP_MC_PATH && morig[0] ? strdup(morig) : NULL;
        if (mode == CLIP_MC_PATH) {                                      /* the original's channels (for the menu) */
            char oc_buf[1024];
            int oc = stream_file_channels(wav_readable_all_channels(morig, sr, oc_buf, sizeof oc_buf));
            cl->mc_nch = oc > 2 ? oc : 0;
        }
        cl->peaks = peaks_start(open_path, sr, cl->wav.frame_count);   // NULL is OK: there is simply no waveform
        if (mode == CLIP_MC_MULTI)
            for (int k = 0; k < mnch && k < STREAM_MAX_CH; ++k) cl->mc_peaks[k] = peaks_start_ch(mc_path, sr, cl->wav.frame_count, k);
        cl->bpm_job = bpm_job_start(open_path);                         // the tempo is detected in the background
        if (cl->bpm_job) g_bpm_pending = true;                          // 2.0: session_poll_bpm looks again
        atomic_store(&cl->bpm_busy, cl->bpm_job != NULL);
    }
    return arc;
}

/* ===== 1.4.24: surround (multichannel) files ===== */
void session_load_mc_next(int mode, int first_out, const char *orig)
{
    g_next_mc.mode = mode;
    g_next_mc.set = orig != NULL || mode != 0 || first_out != 0;   /* (0, 0, NULL) = reset */
    g_next_mc.out = first_out < 0 ? 0 : first_out;
    snprintf(g_next_mc.orig, sizeof g_next_mc.orig, "%s", orig ? orig : "");
}

static Clip *mc_clip(Session *s, int ch, int sc)
{
    if (!s || ch < 0 || ch >= s->channel_count || sc < 0 || sc >= MAX_SCENES) return NULL;
    Clip *cl = &s->channels[ch].clips[sc];
    if (!atomic_load(&cl->has_clip) || atomic_load(&cl->is_note) || atomic_load(&cl->is_midi) || !cl->stream) return NULL;
    return cl;
}
int  session_clip_mc_mode(Session *s, int ch, int sc) { Clip *cl = mc_clip(s, ch, sc); return cl ? cl->mc_mode : 0; }
int  session_clip_mc_channels(Session *s, int ch, int sc) { Clip *cl = mc_clip(s, ch, sc); return cl ? cl->mc_nch : 0; }
int  session_clip_mc_out(Session *s, int ch, int sc) { Clip *cl = mc_clip(s, ch, sc); return cl ? atomic_load(&cl->mc_out) : 0; }
void session_clip_set_mc_out(Session *s, int ch, int sc, int out)
{
    Clip *cl = mc_clip(s, ch, sc);
    if (!cl) return;
    if (out < 0) out = 0;
    if (out > MAX_HW_OUT - 1) out = MAX_HW_OUT - 1;
    atomic_store(&cl->mc_out, out);
}
const char *session_clip_mc_orig(Session *s, int ch, int sc) { Clip *cl = mc_clip(s, ch, sc); return cl && cl->mc_orig && cl->mc_orig[0] ? cl->mc_orig : NULL; }
bool session_mc_find_new(Session *s, int *ch, int *sc)
{
    if (!s) return false;
    for (int c = 0; c < s->channel_count; ++c)
        for (int q = 0; q < s->scene_count; ++q) {
            Clip *cl = &s->channels[c].clips[q];
            if (cl->mc_nch > 2 && !cl->mc_asked && cl->mc_mode == CLIP_MC_STEREO && atomic_load(&cl->has_clip) && cl->stream) {
                if (ch) *ch = c;
                if (sc) *sc = q;
                return true;
            }
        }
    return false;
}
uint64_t session_clip_mc_map(Session *s, int ch, int sc, int k)
{
    Clip *cl = mc_clip(s, ch, sc);
    return cl && k >= 0 && k < STREAM_MAX_CH ? atomic_load(&cl->mc_map[k]) : 0;
}
void session_clip_set_mc_map(Session *s, int ch, int sc, int k, uint64_t mask)
{
    Clip *cl = mc_clip(s, ch, sc);
    if (cl && k >= 0 && k < STREAM_MAX_CH) atomic_store(&cl->mc_map[k], mask);
}
void session_clip_mc_set_asked(Session *s, int ch, int sc) { Clip *cl = mc_clip(s, ch, sc); if (cl) cl->mc_asked = true; }


typedef struct {
    int      ch, sc;
    int      mc_mode, mc_out;            /* 1.4.24 */
    uint64_t mc_map[STREAM_MAX_CH];      /* 2.0 */
    char     mc_orig[1024];
    char     path[1024], name[NAME_LEN];
    size_t   frames, loop_start, loop_end, start, end, scroll;
    bool     loop, warp;
    float    zoom, bpm_det, bpm_fix, bpm_play;
    float    cgain, cpan;                    /* 1.4.9 */
    float    pitch;  bool fmt;               /* 1.4.13 */
    int      bind;
    char    *edits;              /* step 27: the parts (malloc, NULL = not cut) */
    SurPt   *sur;                /* 1.4.19 points (malloc) */
    int      nsur;
} SavedClip;

/* Remembers everything of a clip (to load its file again: another sample rate, 1.4.24 another way of playing it) */
static bool clip_save(Session *s, int c, int sc, SavedClip *sv)
{
    Clip *cl = &s->channels[c].clips[sc];
    memset(sv, 0, sizeof *sv);
    sv->ch = c; sv->sc = sc;
    snprintf(sv->path, sizeof(sv->path), "%s", cl->path);
    snprintf(sv->name, sizeof(sv->name), "%s", cl->name);
    sv->mc_mode    = cl->mc_mode;
    sv->mc_out     = atomic_load(&cl->mc_out);
    for (int k = 0; k < STREAM_MAX_CH; ++k) sv->mc_map[k] = atomic_load(&cl->mc_map[k]);
    snprintf(sv->mc_orig, sizeof sv->mc_orig, "%s", cl->mc_orig ? cl->mc_orig : "");
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
    sv->cgain      = atomic_load(&cl->cgain_db);
    sv->cpan       = atomic_load(&cl->cpan);
    sv->pitch      = atomic_load(&cl->pitch_st);
    sv->fmt        = atomic_load(&cl->pitch_fmt);
    sv->bind       = atomic_load(&cl->bind);
    sv->edits      = session_clip_edits_text(s, c, sc);
    SurAuto *sa = atomic_load(&cl->sur_auto);
    if (sa && sa->n > 0) {
        sv->sur = malloc(sizeof(SurPt) * (size_t)sa->n);
        if (sv->sur) { memcpy(sv->sur, sa->p, sizeof(SurPt) * (size_t)sa->n); sv->nsur = sa->n; }
    }
    return true;
}

static void clip_saved_free(SavedClip *sv) { free(sv->edits); free(sv->sur); sv->edits = NULL; sv->sur = NULL; }

/* Loads the clip's file again and gives it back what clip_save remembered (k = new rate / old rate) */
static int clip_reload(Session *s, const SavedClip *sv, double k)
{
    session_load_mc_next(sv->mc_mode, sv->mc_out, sv->mc_orig);
    if (session_set_clip(s, sv->ch, sv->sc, sv->path) != 0) return 1;
    Clip *cl = &s->channels[sv->ch].clips[sv->sc];
    if (sv->edits) {                                   /* the parts - scaled to the new rate */
        const EditList *now = session_clip_edits(s, sv->ch, sv->sc);
        EditList *old = edit_from_text(sv->edits, INT64_MAX);
        EditList *sc_ = (old && now) ? (k == 1.0 && old->src_len == now->src_len ? edit_clone(old) : edit_scale(old, k, now->src_len)) : NULL;
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
    atomic_store(&cl->wave_scroll_frame, (size_t)(int64_t)llround((double)(intptr_t)sv->scroll * k));
    atomic_store(&cl->bind, sv->bind);
    snprintf(cl->name, sizeof(cl->name), "%s", sv->name);
    session_set_clip_bpm_fields(s, sv->ch, sv->sc, sv->bpm_det, sv->bpm_fix, sv->bpm_play);
    session_set_clip_gain(s, sv->ch, sv->sc, sv->cgain);
    session_set_clip_pan(s, sv->ch, sv->sc, sv->cpan);
    if (sv->warp) session_set_clip_warp(s, sv->ch, sv->sc, true);   // a new stretch for the new rate
    session_set_clip_formants(s, sv->ch, sv->sc, sv->fmt);
    session_set_clip_pitch(s, sv->ch, sv->sc, sv->pitch);
    if (sv->nsur > 0) session_clip_set_sur_points(s, sv->ch, sv->sc, sv->sur, sv->nsur);
    for (int q = 0; q < STREAM_MAX_CH; ++q) atomic_store(&cl->mc_map[q], sv->mc_map[q]);   /* 2.0 */
    stream_invalidate_preroll(cl->stream);
    return 0;
}

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
            if (!n) { for (int i = 0; i < cap; ++i) clip_saved_free(&saved[i]); free(saved); return -1; }
            saved = n;
            clip_save(s, c, sc, &saved[cap++]);
        }

    session_stop_all(s);
    s->sample_rate = sr;

    double k = (double)sr / (double)old_sr;
    int failed = 0;
    for (int i = 0; i < cap; ++i) if (clip_reload(s, &saved[i], k) != 0) ++failed;
    for (int i = 0; i < cap; ++i) clip_saved_free(&saved[i]);
    free(saved);
    if (failed) printf("session_set_sample_rate: %d -> %d Hz (%d clips reloaded, %d failed)\n", old_sr, sr, cap - failed, failed);
    else printf("session_set_sample_rate: %d -> %d Hz (%d clips reloaded)\n", old_sr, sr, cap);
    return failed;
}

/* 1.4.24: plays the clip's file another way. MULTI: every channel on its output; STEREO: as always (the first two);
 * PATH: path = the stereo mix (made by mc_job), orig = the multichannel file. Everything else of the clip stays. */
int session_clip_set_mc(Session *s, int ch, int sc, int mode, const char *path, const char *orig)
{
    Clip *cl = mc_clip(s, ch, sc);
    if (!cl) return 1;
    SavedClip sv;
    clip_save(s, ch, sc, &sv);
    if (mode == CLIP_MC_PATH) {
        if (!path || !orig) { clip_saved_free(&sv); return 1; }
        snprintf(sv.path, sizeof sv.path, "%s", path);
        snprintf(sv.mc_orig, sizeof sv.mc_orig, "%s", orig);
    } else if (sv.mc_mode == CLIP_MC_PATH && sv.mc_orig[0]) {          /* back to the original */
        snprintf(sv.path, sizeof sv.path, "%s", sv.mc_orig);
        sv.mc_orig[0] = 0;
    }
    if (mode == CLIP_MC_MULTI) { sv.warp = false; sv.pitch = 0.0f; }
    sv.mc_mode = mode;
    bool was = atomic_load(&cl->playing);
    if (was) session_stop_clip(s, ch, sc);
    int rc = clip_reload(s, &sv, 1.0);
    clip_saved_free(&sv);
    return rc;
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
    sched_cell_moved(ch, scene, -1, -1);            /* 1.4.17: what was planned for it goes too */

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
    if (atomic_load(&cl->stretch_on) && cl->stretch && stretch_threaded(cl->stretch)) {
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

    double qb = s->play_now ? -1.0 : ql_target(s);
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
    sched_cell_moved(dst_ch, dst_scene, -1, -1);    /* 1.4.17: a launch at a time follows its clip */
    sched_cell_moved(src_ch, src_scene, dst_ch, dst_scene);

    clip_stop(src);
    clip_stop(dst);

    if (atomic_load(&dst->has_clip)) {
        atomic_store(&dst->has_clip, false);
        audio_barrier(s);
        clip_release_stream(dst);
    }
    clip_sur_free(s, dst);                          /* 1.4.19 (the source's points go with it) */

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
    sched_channel_removed(ch);                      /* 1.4.17 */

    for (int sc = 0; sc < MAX_SCENES; ++sc) {
        Clip *cl = &s->channels[ch].clips[sc];
        atomic_store(&cl->playing, false);
        atomic_store(&cl->paused, false);
        atomic_store(&cl->has_clip, false);
    }
    audio_barrier(s);
    for (int sc = 0; sc < MAX_SCENES; ++sc) {
        clip_release_stream(&s->channels[ch].clips[sc]);
        clip_sur_free(s, &s->channels[ch].clips[sc]);          /* 1.4.19 */
    }
    retire(s, atomic_exchange(&s->channels[ch].sur, NULL));

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
    sched_scene_removed(sc);                        /* 1.4.17 */

    for (int c = 0; c < s->channel_count; ++c) {
        Clip *cl = &s->channels[c].clips[sc];
        atomic_store(&cl->playing, false);
        atomic_store(&cl->paused, false);
        atomic_store(&cl->has_clip, false);
    }
    audio_barrier(s);

    for (int c = 0; c < s->channel_count; ++c) {
        clip_release_stream(&s->channels[c].clips[sc]);
        clip_sur_free(s, &s->channels[c].clips[sc]);           /* 1.4.19 */
    }

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
    sched_scene_inserted(idx);                      /* 1.4.17 */
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

static void tr_set_current(Session *s, int scene, bool push)
{
    if (push && s->tr_cur >= 0 && s->tr_cur != scene) {
        if (s->tr_hist_n == (int)(sizeof s->tr_hist / sizeof s->tr_hist[0])) {
            memmove(s->tr_hist, s->tr_hist + 1, sizeof s->tr_hist - sizeof s->tr_hist[0]);
            s->tr_hist_n--;
        }
        s->tr_hist[s->tr_hist_n++] = s->tr_cur;
    }
    s->tr_cur = scene;
}

void session_play_scene(Session *s, int scene)
{
    if (!s || scene < 0 || scene >= s->scene_count) return;
    session_queue_clear(s);                         // manual play -> the queue is cleared
    tr_set_current(s, scene, true);                 /* 1.4.17: for "next" / "back" */

    double qb = s->play_now ? -1.0 : ql_target(s);   /* (1.4.17: a launch at a clock time is not quantized) */
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

void session_play_scene_now(Session *s, int scene)
{
    if (!s) return;
    s->play_now = true;
    session_play_scene(s, scene);
    s->play_now = false;
}

/* 1.4.17: how long a scene plays (Export Scene): the longest clip that does not loop; if every clip loops, one
 * pass of the longest loop. The clips' markers, warp speed; MIDI clips by their length in beats. */
double session_scene_length(Session *s, int scene, int *nclips, char *longest, size_t ln, bool *all_loop)
{
    if (nclips) *nclips = 0;
    if (longest && ln) longest[0] = 0;
    if (all_loop) *all_loop = false;
    if (!s || scene < 0 || scene >= s->scene_count) return 0.0;
    double best_once = 0.0, best_loop = 0.0;
    char n_once[NAME_LEN] = "", n_loop[NAME_LEN] = "";
    int sr = s->sample_rate > 0 ? s->sample_rate : 48000, cnt = 0;
    float T = session_get_tempo(s);
    if (!(T > 0.0f)) T = 120.0f;
    for (int c = 0; c < s->channel_count; ++c) {
        Clip *cl = &s->channels[c].clips[scene];
        if (!atomic_load(&cl->has_clip) || atomic_load(&cl->is_note)) continue;
        double secs;
        if (atomic_load(&cl->is_midi)) {
            const MidiSeq *m = session_midi_get(s, c, scene);
            secs = m ? (double)m->length * 60.0 / (double)T : 0.0;
        } else {
            size_t a = atomic_load(&cl->start_marker), b = atomic_load(&cl->end_marker);
            double sp = session_clip_speed(cl);
            if (!(sp > 0.0)) sp = 1.0;
            secs = b > a ? (double)(b - a) / sp / (double)sr : 0.0;
        }
        ++cnt;
        if (atomic_load(&cl->loop)) { if (secs > best_loop) { best_loop = secs; snprintf(n_loop, sizeof n_loop, "%s", cl->name); } }
        else if (secs > best_once) { best_once = secs; snprintf(n_once, sizeof n_once, "%s", cl->name); }
    }
    if (nclips) *nclips = cnt;
    if (best_once > 0.0) { if (longest) snprintf(longest, ln, "%s", n_once); return best_once; }
    if (all_loop) *all_loop = cnt > 0;
    if (longest) snprintf(longest, ln, "%s", n_loop);
    return best_loop;
}

/* ===== 1.4.19: surround ===== */
int session_set_surround(Session *s, int ch, const SurSetup *setup)
{
    if (!s || ch < 0 || ch >= s->channel_count) return 1;
    SurSetup *n = NULL;
    if (setup && setup->n > 0) {
        n = malloc(sizeof *n);
        if (!n) return 1;
        *n = *setup;
        if (n->n > SUR_MAX_SPK) n->n = SUR_MAX_SPK;
        for (int k = 0; k < n->n; ++k) { if (n->out[k] < 0) n->out[k] = 0; if (n->out[k] >= MAX_HW_OUT) n->out[k] = MAX_HW_OUT - 1; }
    }
    retire(s, atomic_exchange(&s->channels[ch].sur, n));
    return 0;
}
bool session_get_surround(Session *s, int ch, SurSetup *out)
{
    if (!s || ch < 0 || ch >= s->channel_count) return false;
    SurSetup *c = atomic_load(&s->channels[ch].sur);
    if (!c) { if (out) memset(out, 0, sizeof *out); return false; }
    if (out) *out = *c;
    return true;
}
void session_clip_set_sur_points(Session *s, int ch, int scene, const SurPt *p, int n)
{
    if (!s || ch < 0 || ch >= s->channel_count || scene < 0 || scene >= MAX_SCENES) return;
    if (n > SUR_MAX_PTS) n = SUR_MAX_PTS;
    SurAuto *a = NULL;
    if (p && n > 0) {
        a = malloc(sizeof *a + (size_t)n * sizeof(SurPt));
        if (!a) return;
        a->n = n;
        memcpy(a->p, p, (size_t)n * sizeof(SurPt));
        sur_sort(a->p, n);
    }
    retire(s, atomic_exchange(&s->channels[ch].clips[scene].sur_auto, a));
}
int session_clip_get_sur_points(Session *s, int ch, int scene, SurPt *out, int max)
{
    if (!s || ch < 0 || ch >= s->channel_count || scene < 0 || scene >= MAX_SCENES) return 0;
    SurAuto *a = atomic_load(&s->channels[ch].clips[scene].sur_auto);
    if (!a) return 0;
    int n = a->n < max ? a->n : max;
    if (out) memcpy(out, a->p, (size_t)n * sizeof(SurPt));
    return n;
}
bool session_surround_now(Session *s, int ch, float *az, float *spread)
{
    if (!s || ch < 0 || ch >= s->channel_count) return false;
    Channel *c = &s->channels[ch];
    if (!atomic_load(&c->sur) || !atomic_load(&c->sur_vis_on)) return false;
    if (az) *az = atomic_load(&c->sur_vis_az);
    if (spread) *spread = atomic_load(&c->sur_vis_spread);
    return true;
}

bool session_surround_now_room(Session *s, int ch, float *x, float *y, float *w)
{
    if (!s || ch < 0 || ch >= s->channel_count) return false;
    Channel *c = &s->channels[ch];
    if (!atomic_load(&c->sur) || !atomic_load(&c->sur_vis_on)) return false;
    if (x) *x = atomic_load(&c->sur_vis_x);
    if (y) *y = atomic_load(&c->sur_vis_y);
    if (w) *w = atomic_load(&c->sur_vis_w);
    return true;
}

/* ===== 1.4.17: scene transitions ===== */
static double mono_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
void session_set_transition_sec(Session *s, float sec)
{
    if (!s || !(sec == sec)) return;
    if (sec < TR_SEC_MIN) sec = TR_SEC_MIN;
    if (sec > TR_SEC_MAX) sec = TR_SEC_MAX;
    atomic_store(&s->tr_sec, roundf(sec * 10.0f) / 10.0f);
}
float session_get_transition_sec(Session *s) { return s ? atomic_load(&s->tr_sec) : 4.0f; }
int session_current_scene(Session *s) { return s ? s->tr_cur : -1; }
void session_automation_hold(Session *s, bool hold)
{
    if (!s) return;
    if (!hold && atomic_load(&s->auto_hold)) {
        /* the time that stood still does not count: the rest of the transition goes on from where it was */
        double p = s->tr_len_s > 0 ? (s->tr_hold_t - s->tr_t0) : 0;
        s->tr_t0 = mono_s() - p;
    } else if (hold && !atomic_load(&s->auto_hold)) s->tr_hold_t = mono_s();
    atomic_store(&s->auto_hold, hold);
}
bool session_automation_held(Session *s) { return s && atomic_load(&s->auto_hold); }
float session_transition_progress(Session *s)
{
    if (!s || s->tr_len_s <= 0) return -1.0f;
    double now = atomic_load(&s->auto_hold) ? s->tr_hold_t : mono_s();
    double p = (now - s->tr_t0) / s->tr_len_s;
    if (p >= 1.0) { s->tr_len_s = 0; return -1.0f; }
    return p < 0 ? 0.0f : (float)p;
}

/* a ramp to `to` (0..1) over len frames; from < 0 = from where it is */
static void tr_request(Clip *cl, float from, float to, long len)
{
    atomic_store(&cl->tr_req_from, from);
    atomic_store(&cl->tr_req_to, to);
    atomic_store(&cl->tr_req_len, len > 0 ? len : 1);
}

int session_transition_to(Session *s, int scene)
{
    if (!s || scene < 0 || scene >= s->scene_count) return 1;
    session_queue_clear(s);
    atomic_store(&s->auto_hold, false);             /* a new transition = the automations run again */
    float sec = session_get_transition_sec(s);
    long len = (long)(sec * (float)(s->sample_rate > 0 ? s->sample_rate : 48000) + 0.5f);
    for (int c = 0; c < s->channel_count; ++c) {
        Clip *b = &s->channels[c].clips[scene];
        bool has_b = atomic_load(&b->has_clip) && !atomic_load(&b->is_note) && (b->stream || atomic_load(&b->is_midi));
        ql_cancel_channel(s, c, NULL);
        bool midi_b = has_b && atomic_load(&b->is_midi);
        for (int sc = 0; sc < s->scene_count; ++sc) {
            Clip *o = &s->channels[c].clips[sc];
            if (sc == scene || !atomic_load(&o->has_clip) || !atomic_load(&o->playing)) continue;
            if (atomic_load(&o->is_midi) || midi_b) clip_stop(o);      /* (an instrument plays one clip: it switches) */
            else tr_request(o, -1.0f, 0.0f, len);                     /* fades out, then stops by itself */
        }
        if (!has_b) continue;
        if (midi_b) { if (!atomic_load(&b->playing)) clip_start(b, STREAM_FROM_START); continue; }
        if (atomic_load(&b->playing)) { tr_request(b, -1.0f, 1.0f, len); continue; }   /* (already there: back up to full) */
        /* starts silent and comes in: the request is there before it plays (no loud first block) */
        tr_request(b, 0.0f, 1.0f, len);
        atomic_store(&b->xf_dir, 0);
        atomic_store(&b->paused, false);
        atomic_store(&b->ended, false);
        atomic_fetch_add(&b->stretch_gen, 1);
        stream_start(b->stream, STREAM_FROM_START);
        atomic_store(&b->position, stream_position(b->stream));
        atomic_store(&b->playing, true);
    }
    s->tr_t0 = mono_s();
    s->tr_len_s = sec;
    tr_set_current(s, scene, true);
    printf("session_transition_to: scene %d over %.1f s\n", scene, (double)sec);
    return 0;
}

static bool scene_has_clips(Session *s, int sc)
{
    for (int c = 0; c < s->channel_count; ++c) {
        Clip *cl = &s->channels[c].clips[sc];
        if (atomic_load(&cl->has_clip) && !atomic_load(&cl->is_note)) return true;
    }
    return false;
}
int session_transition_next(Session *s)
{
    if (!s || s->scene_count <= 0) return 1;
    int cur = s->tr_cur;
    if (cur < 0)                                    /* nothing launched as a scene: the first scene that plays */
        for (int sc = 0; sc < s->scene_count && cur < 0; ++sc)
            for (int c = 0; c < s->channel_count; ++c) {
                Clip *cl = &s->channels[c].clips[sc];
                if (atomic_load(&cl->has_clip) && atomic_load(&cl->playing)) { cur = sc; break; }
            }
    for (int sc = cur + 1; sc < s->scene_count; ++sc)
        if (scene_has_clips(s, sc)) return session_transition_to(s, sc);
    return 2;                                       /* (no scene after it) */
}
int session_transition_back(Session *s)
{
    if (!s) return 1;
    while (s->tr_hist_n > 0) {
        int sc = s->tr_hist[--s->tr_hist_n];
        if (sc >= 0 && sc < s->scene_count) {
            int hn = s->tr_hist_n;
            int r = session_transition_to(s, sc);
            s->tr_hist_n = hn;                      /* (going back is not a new step) */
            return r;
        }
    }
    return 2;                                       /* nothing before */
}

/* 1.4.2: stop one clip (a controller's Stop button) */
void session_stop_clip(Session *s, int ch, int scene)
{
    if (!s || ch < 0 || ch >= s->channel_count || scene < 0 || scene >= s->scene_count) return;
    Clip *cl = &s->channels[ch].clips[scene];
    if (atomic_load(&cl->has_clip)) clip_stop(cl);
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
    for (int i = 0; i < 4; ++i) atomic_store(&s->midi_bound[i], b[i] | s->extra_bound[i]);
    atomic_store(&s->midi_block, atomic_load(&s->btn_learn_kind) != BTN_LEARN_NONE || s->extra_block);
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
    /* 2.0: audio: the sketch of what was recorded so far (min L, max L, min R, max R per REC_PK_BLOCK frames) */
    float   *pk; size_t npk, cappk;
    float    acc[4]; unsigned accn;
    unsigned long long written;
} RecUi;
#define REC_PK_BLOCK 256u

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
    if (u) { if (u->sf) sf_close(u->sf); free(u->buf); free(u->notes); free(u->pk); free(u); }
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

/* 2.0: the live waveform of a recording */
static void rec_sketch(RecUi *u, const float *b, unsigned k)
{
    for (unsigned i = 0; i < k; ++i) {
        float l = b[i * 2], r = b[i * 2 + 1];
        if (u->accn == 0) { u->acc[0] = u->acc[1] = l; u->acc[2] = u->acc[3] = r; }
        else {
            if (l < u->acc[0]) u->acc[0] = l;
            if (l > u->acc[1]) u->acc[1] = l;
            if (r < u->acc[2]) u->acc[2] = r;
            if (r > u->acc[3]) u->acc[3] = r;
        }
        if (++u->accn == REC_PK_BLOCK) {
            if (u->npk == u->cappk) {
                size_t nc = u->cappk ? u->cappk * 2 : 4096;
                float *n = realloc(u->pk, sizeof(float) * 4 * nc);
                if (!n) { u->accn = 0; continue; }
                u->pk = n; u->cappk = nc;
            }
            memcpy(u->pk + u->npk * 4, u->acc, sizeof u->acc);
            u->npk++;
            u->accn = 0;
        }
    }
    u->written += k;
}

int session_rec_sketch(Session *s, int ch, int sc, const float **pk, size_t *nblocks, unsigned *block, unsigned long long *frames)
{
    RecSlot *r = s ? rec_find(s, ch, sc) : NULL;
    if (!r || r->midi || !r->ui) return 1;
    RecUi *u = r->ui;
    if (pk) *pk = u->pk;
    if (nblocks) *nblocks = u->npk;
    if (block) *block = REC_PK_BLOCK;
    if (frames) *frames = u->written;
    return 0;
}

bool session_rec_is_midi(Session *s, int ch, int sc)
{
    RecSlot *r = s ? rec_find(s, ch, sc) : NULL;
    return r && r->midi;
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
                rec_sketch(u, u->buf, k);                /* 2.0: for the live waveform */
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
                session_set_clip_bpm_fields(s, r->ch, r->sc, T > 0 ? T : 0.0f, 0.0f, 0.0f);   /* recorded at this tempo (2.0: = its own) */
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


void session_set_wave_scroll(Session *s, int ch, int scene, size_t frame)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    if (scene < 0 || scene >= MAX_SCENES) return;

    Clip *cl = &s->channels[ch].clips[scene];
    if (!atomic_load(&cl->has_clip)) return;
    if (atomic_load(&cl->is_note)) return;

    atomic_store(&cl->wave_scroll_frame, frame);
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

static Clip *clip_at(Session *s, int ch, int scene)
{
    if (!s || ch < 0 || ch >= s->channel_count || scene < 0 || scene >= MAX_SCENES) return NULL;
    return &s->channels[ch].clips[scene];
}

void session_set_clip_gain(Session *s, int ch, int scene, float db)
{
    Clip *cl = clip_at(s, ch, scene);
    if (!cl || !(db == db)) return;
    if (db < CLIP_GAIN_MIN_DB) db = CLIP_GAIN_MIN_DB;
    if (db > CLIP_GAIN_MAX_DB) db = CLIP_GAIN_MAX_DB;
    if (fabsf(db) < 0.005f) db = 0.0f;                  /* 0.0 dB = exactly untouched */
    atomic_store(&cl->cgain_db, db);
}
float session_get_clip_gain(Session *s, int ch, int scene) { Clip *cl = clip_at(s, ch, scene); return cl ? atomic_load(&cl->cgain_db) : 0.0f; }

void session_set_clip_pan(Session *s, int ch, int scene, float pan)
{
    Clip *cl = clip_at(s, ch, scene);
    if (!cl || !(pan == pan)) return;
    if (pan < -1.0f) pan = -1.0f;
    if (pan > 1.0f) pan = 1.0f;
    if (fabsf(pan) < 0.005f) pan = 0.0f;
    atomic_store(&cl->cpan, pan);
}
float session_get_clip_pan(Session *s, int ch, int scene) { Clip *cl = clip_at(s, ch, scene); return cl ? atomic_load(&cl->cpan) : 0.0f; }

void session_set_loudness(Session *s, int target, bool down_only)
{
    if (!s) return;
    if (target > -5 || target < -40) target = 0;
    s->loud_target = target;
    s->loud_down_only = down_only;
    session_update_loudness(s);
}

static float loud_gain(const Session *s, float lufs, float peak_db)
{
    if (!s->loud_target || !isfinite(lufs) || lufs < -70.0f) return 0.0f;
    float g = (float)s->loud_target - lufs;
    if (g > 0.0f) {
        if (s->loud_down_only) return 0.0f;
        float room = -1.0f - peak_db;                     /* the highest sample stays at -1 dBFS */
        if (g > room) g = room;
        if (g < 0.0f) g = 0.0f;
    }
    if (g < -30.0f) g = -30.0f;
    if (g > 24.0f) g = 24.0f;
    if (fabsf(g) < 0.01f) g = 0.0f;
    return g;
}

void session_update_loudness(Session *s)
{
    if (!s) return;
    for (int c = 0; c < s->channel_count; ++c)
        for (int k = 0; k < s->scene_count && k < MAX_SCENES; ++k) {
            Clip *cl = &s->channels[c].clips[k];
            float want = 0.0f, lufs, pk;
            if (s->loud_target && atomic_load(&cl->has_clip) && !cl->is_midi && cl->peaks && peaks_loudness(cl->peaks, &lufs, &pk))
                want = loud_gain(s, lufs, pk);
            if (atomic_load_explicit(&cl->norm_db, memory_order_relaxed) != want) atomic_store(&cl->norm_db, want);
        }
}

bool session_clip_loudness(Session *s, int ch, int scene, float *lufs, float *peak_db, float *norm_db)
{
    Clip *cl = clip_at(s, ch, scene);
    float l, p;
    if (!cl || !atomic_load(&cl->has_clip) || cl->is_midi || !cl->peaks || !peaks_loudness(cl->peaks, &l, &p)) return false;
    if (lufs) *lufs = l;
    if (peak_db) *peak_db = p;
    if (norm_db) *norm_db = atomic_load(&cl->norm_db);
    return true;
}

void session_set_extra_bound(Session *s, const unsigned mask[4], bool block)
{
    if (!s) return;
    for (int i = 0; i < 4; ++i) s->extra_bound[i] = mask ? mask[i] : 0u;
    s->extra_block = block;
}

static double quant_mode_beats(const Session *s, int mode)
{
    double bar = session_bar_beats(s);
    switch (mode) {
    case QUANT_NOW:      return 0.0;
    case QUANT_BEAT:     return session_sig_unit(s);
    case QUANT_HALF_BAR: return bar * 0.5;
    case QUANT_BAR:      return bar;
    case QUANT_2BARS:    return bar * 2.0;
    case QUANT_4BARS:    return bar * 4.0;
    default:             return -1.0;
    }
}

void session_set_quantize(Session *s, float beats)
{
    if (!s) return;
    if (!(beats >= 0.0f)) beats = 0.0f;
    if (beats > 256.0f) beats = 256.0f;
    atomic_store(&s->quant_beats, beats);
    s->quant_mode = -1;                                 /* a number of beats - one of the modes if it is one */
    for (int m = 0; m < QUANT_N_MODES; ++m)
        if (fabs(quant_mode_beats(s, m) - (double)beats) < 1e-4) { s->quant_mode = m; break; }
}

void session_set_quant_mode(Session *s, int mode)
{
    if (!s || mode < 0 || mode >= QUANT_N_MODES) return;
    s->quant_mode = mode;
    atomic_store(&s->quant_beats, (float)quant_mode_beats(s, mode));
}

int session_get_quant_mode(const Session *s) { return s ? s->quant_mode : QUANT_BAR; }

int session_set_time_sig(Session *s, int num, int den)
{
    if (!s || num < 1 || num > SIG_NUM_MAX) return -1;
    if (den != 1 && den != 2 && den != 4 && den != 8 && den != 16 && den != 32) return -1;
    atomic_store(&s->sig_num, num);
    atomic_store(&s->sig_den, den);
    if (s->quant_mode >= 0) atomic_store(&s->quant_beats, (float)quant_mode_beats(s, s->quant_mode));   /* "1 Bar" stays 1 bar */
    return 0;
}

void session_get_time_sig(const Session *s, int *num, int *den)
{
    int n = s ? atomic_load(&((Session *)s)->sig_num) : 4, d = s ? atomic_load(&((Session *)s)->sig_den) : 4;
    if (n < 1) n = 4;
    if (d < 1) d = 4;
    if (num) *num = n;
    if (den) *den = d;
}

double session_sig_unit(const Session *s) { int n, d; session_get_time_sig(s, &n, &d); return 4.0 / (double)d; }
double session_bar_beats(const Session *s) { int n, d; session_get_time_sig(s, &n, &d); return (double)n * 4.0 / (double)d; }

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

/* 1.4.13: the clip plays through its Stretch when warp is on OR it has a pitch shift (stretch_on).
 * restart: the path stays on but the engine changes (pitch 0 <-> not 0 with warp on) -> start the Stretch again
 * from the heard position (the mixer fades the old sound out and the new one in). */
static void clip_update_path(Session *s, Clip *cl, bool restart)
{
    bool want = atomic_load(&cl->warp) || atomic_load(&cl->pitch_st) != 0.0f;
    if (want && !cl->stretch) {
        Stretch *st = stretch_create(s->sample_rate > 0 ? s->sample_rate : 48000);
        if (!st) { printf("clip_update_path: out of memory\n"); return; }
        mix_lock(s);                                // the audio thread reads the pointer -> change it between blocks
        cl->stretch = st;
        pthread_mutex_unlock(&s->mix_mu);
    }
    if (cl->stretch) stretch_set_pitch(cl->stretch, atomic_load(&cl->pitch_st), atomic_load(&cl->pitch_fmt));
    bool was = atomic_load(&cl->stretch_on);
    atomic_store(&cl->stretch_on, want);            // the audio thread sees the stretch after this flag
    if (was && (!want || restart) && cl->stream && (atomic_load(&cl->playing) || atomic_load(&cl->paused)) &&
        stretch_threaded(cl->stretch)) {
        // the background thread has pulled audio ahead: continue from the heard position, so there is no jump
        size_t pos = atomic_load(&cl->position);
        atomic_fetch_add(&cl->stretch_gen, 1);
        stream_start(cl->stream, pos);
    }
}

void session_set_clip_warp(Session *s, int ch, int scene, bool on)
{
    Clip *cl = audio_clip(s, ch, scene);
    if (!cl) return;
    if (cl->mc_mode == CLIP_MC_MULTI) on = false;      /* 1.4.24: a multichannel clip plays straight from its file */
    atomic_store(&cl->warp, on);
    clip_update_path(s, cl, false);
}

void session_set_clip_pitch(Session *s, int ch, int scene, float semis)
{
    Clip *cl = audio_clip(s, ch, scene);
    if (!cl || !(semis == semis)) return;
    if (cl->mc_mode == CLIP_MC_MULTI) semis = 0.0f;    /* 1.4.24 */
    if (semis < -CLIP_PITCH_MAX) semis = -CLIP_PITCH_MAX;
    if (semis > CLIP_PITCH_MAX) semis = CLIP_PITCH_MAX;
    semis = roundf(semis * 100.0f) / 100.0f;        /* 1 cent steps; 0.00 = exactly untouched */
    if (semis == 0.0f) semis = 0.0f;                /* (no -0) */
    float old = atomic_load(&cl->pitch_st);
    if (old == semis) return;
    atomic_store(&cl->pitch_st, semis);
    /* 0 <-> not 0 with warp on: Rubber Band (tempo only) <-> the pitch engine -> a fresh start of the Stretch */
    bool engine_change = (old == 0.0f) != (semis == 0.0f) && atomic_load(&cl->warp);
    clip_update_path(s, cl, engine_change);
}
float session_get_clip_pitch(Session *s, int ch, int scene) { Clip *cl = clip_at(s, ch, scene); return cl ? atomic_load(&cl->pitch_st) : 0.0f; }

void session_set_clip_formants(Session *s, int ch, int scene, bool on)
{
    Clip *cl = audio_clip(s, ch, scene);
    if (!cl) return;
    atomic_store(&cl->pitch_fmt, on);
    if (cl->stretch) stretch_set_pitch(cl->stretch, atomic_load(&cl->pitch_st), on);
}
bool session_get_clip_formants(Session *s, int ch, int scene) { Clip *cl = clip_at(s, ch, scene); return cl && atomic_load(&cl->pitch_fmt); }

void session_set_clip_bpm_value(Session *s, int ch, int scene, float bpm)
{
    Clip *cl = audio_clip(s, ch, scene);
    if (!cl) return;
    bpm = clamp_bpm(bpm);
    atomic_store(&cl->bpm_fix, bpm);                /* 1.1: the field is always the clip's own tempo (a correction) */
}

/* 2.0: back to the tempo the clip had (the detected one, without the correction) */
void session_clip_bpm_reset(Session *s, int ch, int scene)
{
    Clip *cl = audio_clip(s, ch, scene);
    if (cl) atomic_store(&cl->bpm_fix, 0.0f);
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
    if (!s || !g_bpm_pending) return;                  /* 2.0: no detection running -> no walk over all cells */
    bool left = false;
    for (int c = 0; c < s->channel_count; ++c)
        for (int sc = 0; sc < MAX_SCENES; ++sc) {
            Clip *cl = &s->channels[c].clips[sc];
            if (!cl->bpm_job) continue;
            float r = 0.0f;
            if (!bpm_job_poll(cl->bpm_job, &r)) { left = true; continue; }
            atomic_store(&cl->bpm_detected, bpm_round(r));
            bpm_job_release(cl->bpm_job);
            cl->bpm_job = NULL;
            atomic_store(&cl->bpm_busy, false);
        }
    g_bpm_pending = left;
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
                if (atomic_load(&b->stretch_on) && b->stretch && stretch_threaded(b->stretch)) {
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
    int64_t ws = (int64_t)(intptr_t)atomic_load(&cl->wave_scroll_frame);      /* may be < 0 (step 36j) */
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
    return session_peaks_list(cl, cl ? cl->peaks : NULL, e, f0, f1, out);
}

/* the same for any sketch of the clip's timeline (1.4.14: also the stems) */
bool session_peaks_list(const Clip *cl, Peaks *pk, const EditList *e, int64_t f0, int64_t f1, PeakVal *out)
{
    if (!cl || !pk || f1 <= f0) return false;
    if (!e || edit_is_identity(e)) {                     /* step 36j: the view may also be outside the clip */
        int64_t L = e ? e->len : (int64_t)cl->wav.frame_count;
        if (f0 < 0) f0 = 0;
        if (f1 > L) f1 = L;
        if (f1 <= f0) return false;
        return peaks_range(pk, (size_t)f0 / PEAKS_BLOCK, (size_t)(f1 - 1) / PEAKS_BLOCK, out);
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
        if (!peaks_range(pk, (size_t)s0 / PEAKS_BLOCK, (size_t)(s1 - 1) / PEAKS_BLOCK, &v)) continue;
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
        if (xs1 > xs0 && peaks_range(pk, (size_t)xs0 / PEAKS_BLOCK, (size_t)(xs1 - 1) / PEAKS_BLOCK, &v)) {
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

static ChFx *fx_at(Session *s, int ch, int k)
{
    if (!s || ch < 0 || ch >= s->channel_count) return NULL;
    Channel *c = &s->channels[ch];
    return k >= 0 && k < c->fx_count ? &c->fx[k] : NULL;
}

int session_fx_midi_get(Session *s, int ch, int k, int param)
{
    ChFx *f = fx_at(s, ch, k);
    if (!f) return -1;
    for (int i = 0; i < f->midi_n; ++i) if (f->midi[i].param == param) return f->midi[i].cc;
    return -1;
}

bool session_fx_midi_set(Session *s, int ch, int k, int param, int cc)
{
    ChFx *f = fx_at(s, ch, k);
    if (!f || param < 0 || param > 32767) return false;
    for (int i = 0; i < f->midi_n; ++i) {
        if (f->midi[i].param != param) continue;
        if (cc < 0) { memmove(&f->midi[i], &f->midi[i + 1], sizeof f->midi[0] * (size_t)(f->midi_n - i - 1)); f->midi_n--; }
        else f->midi[i].cc = (short)cc;
        return true;
    }
    if (cc < 0) return true;
    if (cc > 127 || f->midi_n >= FX_MIDI_MAX) return false;
    f->midi[f->midi_n].param = (short)param;
    f->midi[f->midi_n].cc = (short)cc;
    f->midi_n++;
    return true;
}

int  session_fx_midi_count(Session *s, int ch, int k) { ChFx *f = fx_at(s, ch, k); return f ? f->midi_n : 0; }
void session_fx_midi_clear(Session *s, int ch, int k) { ChFx *f = fx_at(s, ch, k); if (f) f->midi_n = 0; }

void session_fx_toggle_bypass(Session *s, int ch, int idx)
{
    if (!s || ch < 0 || ch >= s->channel_count) return;
    Channel *c = &s->channels[ch];
    if (idx < 0 || idx >= c->fx_count) return;
    c->fx[idx].bypass = !c->fx[idx].bypass;
}
