/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#include "mixer.h"
#include "stream.h"
#include "fxchain.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_BLOCK    1024
#define SMOOTH_COEF  0.0005f
#define SNAP_THRESH  0.0005f
#define PEAK_DECAY   0.92f

/* Used only by the audio thread. */
static float s_mixL[MAX_BLOCK], s_mixR[MAX_BLOCK];
static float s_rg[MAX_BLOCK], s_pl[MAX_BLOCK], s_pr[MAX_BLOCK];
static float s_blk[MAX_BLOCK * 2];
static float s_chpeak[MAX_CHANNELS];
static float s_chpk_l[MAX_CHANNELS], s_chpk_r[MAX_CHANNELS];   /* stereo: after volume + pan */
#define PAN_COMP 1.0f              /* step 36: centre pan is now 0 dB -> the meter shows exactly the output */
static float s_cl[MAX_BLOCK], s_cr[MAX_BLOCK];          /* channel sum (only with a non-standard route) */
static float s_hw[MAX_HW_OUT][MAX_BLOCK];               /* card outputs (direct and from the master) */
static RouteMask s_hw_used;                              /* which s_hw are already cleared in this block */
static float s_fxl[MAX_BLOCK], s_fxr[MAX_BLOCK];        /* channel with plugins: the sum before the plugins and the volume */

/* Adds the signal to a target: 0 = Master L, 1 = Master R, 2+k = output k+1 of the card */
static void route_add(int dest, const float *src, unsigned n)
{
    float *d;
    if (dest == 0)      d = s_mixL;
    else if (dest == 1) d = s_mixR;
    else {
        int k = dest - 2;
        if (k >= MAX_HW_OUT) return;
        if (!(s_hw_used & RBIT(k))) { memset(s_hw[k], 0, n * sizeof(float)); s_hw_used |= RBIT(k); }
        d = s_hw[k];
    }
    for (unsigned i = 0; i < n; ++i) d[i] += src[i];
}

static void route_mask_add(RouteMask mask, const float *src, unsigned n)
{
    while (mask) {                                   /* only the set bits */
        int b = __builtin_ctzll(mask);
        mask &= mask - 1;
        if (b < 2 + MAX_HW_OUT) route_add(b, src, n);
    }
}

static inline float fa(float x) { return x < 0 ? -x : x; }

/* the stretch pulls audio from the clip's stream */
static size_t pull_stream(void *ud, float *out, size_t n)
{
    return stream_read((ClipStream *)ud, out, n);
}

/* Reads n frames from the clip: directly from the stream or (with warp on) through the stretch. */
static size_t clip_read(Clip *cl, float *out, unsigned n)
{
    Stretch *st = cl->stretch;
    if (!atomic_load_explicit(&cl->warp, memory_order_acquire) || !st) {
        if (cl->stretch_live && st) {
            /* warp was just turned off: the background thread must let go of the stream before we read it ourselves */
            if (!stretch_release(st)) { memset(out, 0, (size_t)n * 2 * sizeof(float)); return n; }
            cl->stretch_live = false;
        }
        return stream_read(cl->stream, out, n);
    }
    unsigned gen = atomic_load(&cl->stretch_gen);
    if (!cl->stretch_live || gen != cl->stretch_seen) {     /* new start / seek / just turned on */
        stretch_reset(st);
        cl->stretch_seen = gen;
        cl->stretch_live = true;
    }
    return stretch_process(st, session_clip_speed(cl), out, n, pull_stream, cl->stream);
}

/* Has the clip ended: with stretch on a background thread — when the tail has been played too. */
static bool clip_finished(Clip *cl)
{
    if (cl->stretch_live && cl->stretch && stretch_threaded(cl->stretch))
        return stretch_finished(cl->stretch);
    return stream_finished(cl->stream);
}

/* Position for display: with stretch part of the audio has been pulled but not heard yet. */
static size_t clip_heard_pos(Clip *cl)
{
    size_t p = stream_position(cl->stream);
    if (cl->stretch_live && cl->stretch) {
        size_t pend = stretch_pending(cl->stretch);
        size_t lo = atomic_load(&cl->start_marker);
        if (atomic_load(&cl->loop)) {
            size_t ls = atomic_load(&cl->loop_start);
            if (ls < lo) lo = ls;
        }
        if (p >= lo + pend) p -= pend;              /* right after jumping back to the loop start we simply do not subtract */
    }
    return p;
}

/* Step 36: the pan is a "balance" - in the middle BOTH sides are exactly 1.0 (0 dB), i.e. what is in the file
 * is what comes out (for an external mixer: -20 dB in the program = -20 dB at its input). To one side the other side fades
 * (cos), while this one stays 1.0. Before: equal power -> -3 dB in the middle. */
static inline void pan_law(float p, float *pl, float *pr)
{
    if (p > 0.0f)      { *pl = cosf(p * (float)(M_PI * 0.5)); *pr = 1.0f; }
    else if (p < 0.0f) { *pl = 1.0f; *pr = cosf(-p * (float)(M_PI * 0.5)); }
    else               { *pl = 1.0f; *pr = 1.0f; }
}

/* Volume + pan for n frames of the channel. If the knobs stay still
 * (the usual case) — compute pow/cos/sin ONCE, not for every frame. */
static void fill_gain_pan(Channel *ch, unsigned n)
{
    float tg = atomic_load(&ch->gain);
    float g  = atomic_load(&ch->smooth_gain);
    float tp = atomic_load(&ch->pan);
    float p  = atomic_load(&ch->smooth_pan);

    if (fabsf(tg - g) < SNAP_THRESH && fabsf(tp - p) < SNAP_THRESH) {
        g = tg;
        p = tp;
        float rg    = session_pos_to_gain(g);
        float pl, pr;
        pan_law(p, &pl, &pr);
        for (unsigned i = 0; i < n; ++i) { s_rg[i] = rg; s_pl[i] = pl; s_pr[i] = pr; }
    } else {
        for (unsigned i = 0; i < n; ++i) {
            float d = tg - g;
            g = fa(d) < SNAP_THRESH ? tg : g + d * SMOOTH_COEF;
            float pd = tp - p;
            p = fa(pd) < SNAP_THRESH ? tp : p + pd * SMOOTH_COEF;

            s_rg[i] = session_pos_to_gain(g);
            pan_law(p, &s_pl[i], &s_pr[i]);
        }
    }
    atomic_store(&ch->smooth_gain, g);
    atomic_store(&ch->smooth_pan, p);
}

static bool cl_threaded_live(Clip *cl)
{
    return cl->stretch_live && cl->stretch && stretch_threaded(cl->stretch);
}

/* Queue: if "now playing" ends in this block, start the prepared next one exactly from this sample. */
static Clip *q_at(Session *s, int idx)
{
    if (idx < 0) return NULL;
    int ch = idx / MAX_SCENES, sc = idx % MAX_SCENES;
    if (ch >= s->channel_count || sc >= s->scene_count) return NULL;
    Clip *cl = &s->channels[ch].clips[sc];
    if (!atomic_load(&cl->has_clip) || atomic_load(&cl->is_note) || !cl->stream) return NULL;
    return cl;
}

#define DC_TAIL   128     /* ~2.7 ms: fade-out of stopped/skipped audio */
#define DC_FADEIN 64      /* ~1.3 ms: fade-in at the start of started/skipped audio */

static void queue_launch(Session *s, unsigned n)
{
    int nx = atomic_load(&s->q_next);
    if (nx < 0) return;
    int an = atomic_load(&s->q_anchor);
    if (an == nx) return;
    Clip *a = q_at(s, an), *b = q_at(s, nx);
    if (!a || !b || !atomic_load(&a->playing) || atomic_load(&a->loop)) return;
    size_t end = atomic_load(&a->end_marker);
    size_t a_len = __atomic_load_n(&a->wav.frame_count, __ATOMIC_RELAXED);   /* the UI may change it (cutting) */
    if (end == 0 || end > a_len) end = a_len;

    long xf = atomic_load(&s->xfade_frames);
    if (xf > 0) {
        /* ===== Crossfade: b starts xf frames before a goes silent; a fades out, b fades in (equal power) ===== */
        size_t ae = atomic_load(&a->aud_end);
        if (ae > 0 && ae < end) end = ae;                  /* the silence at the end does not count */
        size_t heard = clip_heard_pos(a);
        double sp_a = a->stretch_live ? session_clip_speed(a) : 1.0;
        double rem = end > heard ? (double)(end - heard) / sp_a : 0.0;   /* output frames to the end */
        if (rem - (double)xf >= (double)n) return;         /* still too early */
        if (!atomic_compare_exchange_strong(&s->q_next, &nx, -1)) return;
        double off = rem - (double)xf;
        if (off < 0.0) off = 0.0;
        long len = (long)(rem < (double)xf ? rem : (double)xf);
        /* not longer than half of b (otherwise the next transition would start before this one ends) */
        size_t be = atomic_load(&b->end_marker), bp = stream_position(b->stream);
        size_t b_len = __atomic_load_n(&b->wav.frame_count, __ATOMIC_RELAXED);
        if (be == 0 || be > b_len) be = b_len;
        double sp_b = atomic_load(&b->warp) ? session_clip_speed(b) : 1.0;
        double blen = be > bp ? (double)(be - bp) / sp_b : 0.0;
        if ((double)len > blen * 0.5) len = (long)(blen * 0.5);
        if (len < 1) len = 1;
        b->start_offset = (unsigned)off;
        b->xf_len = len;  b->xf_pos = -(long)off;  b->xf_gen = atomic_load(&b->stretch_gen);
        atomic_store(&b->xf_dir, +1);
        a->xf_len = len;  a->xf_pos = -(long)off;  a->xf_gen = atomic_load(&a->stretch_gen);
        atomic_store(&a->xf_dir, -1);
        atomic_store(&b->ended, false);
        b->q_launch = true;
        atomic_store(&b->playing, true);
        atomic_store(&s->q_launched, nx + 1);
        return;
    }

    /* how many output frames remain until the end of a's music */
    double rem;
    if (cl_threaded_live(a)) {
        long fe = stretch_frames_to_end(a->stretch);    /* exact: known once the background thread reaches the end */
        if (fe < 0) return;                             /* the end has not been read yet -> there are at least ~100 ms */
        rem = (double)fe;
    } else {
        size_t pos = stream_position(a->stream);
        rem = end > pos ? (double)(end - pos) : 0.0;
        if (a->stretch_live) rem /= session_clip_speed(a);
    }
    if (rem >= (double)n) return;
    if (!atomic_compare_exchange_strong(&s->q_next, &nx, -1)) return;
    b->start_offset = (unsigned)rem;
    atomic_store(&b->ended, false);
    b->q_launch = true;                         /* seamless continuation -> no fade-in at the start */
    atomic_store(&b->playing, true);
    atomic_store(&s->q_launched, nx + 1);
}

/* Master: volume (smoothed), balance, metering. Input L/R -> output oL/oR (no clipping). May write into the same buffers. */
static void master_block(Session *s, const float *inL, const float *inR, float *oL, float *oR, unsigned n,
                         float *master_peak, float *mpk_l, float *mpk_r)
{
    float mt = atomic_load(&s->master_muted) ? 0.0f : atomic_load(&s->master_gain);   /* MUTE = smoothly to 0 */
    float mc = atomic_load(&s->master_smooth_gain);
    float mpt = atomic_load(&s->master_pan), mp = atomic_load(&s->master_smooth_pan);
    if (!(mpt == mpt)) mpt = 0.0f;
    bool  settled = fa(mt - mc) < SNAP_THRESH;
    float mg_const = 0.0f;
    if (settled) { mc = mt; mg_const = session_pos_to_gain(mc); }
    bool clip = false;

    for (unsigned i = 0; i < n; ++i) {
        float mg;
        if (settled) {
            mg = mg_const;
        } else {
            float d = mt - mc;
            mc = fa(d) < SNAP_THRESH ? mt : mc + d * SMOOTH_COEF;
            mg = session_pos_to_gain(mc);
        }
        float L = inL[i] * mg;
        float R = inR[i] * mg;
        if (mp != mpt || mp != 0.0f) {          /* balance: the centre is 1:1, to one side the other side decreases */
            float d = mpt - mp;
            mp = fa(d) < SNAP_THRESH ? mpt : mp + d * SMOOTH_COEF;
            if (mp > 0.0f) L *= 1.0f - mp; else R *= 1.0f + mp;
        }
        if (!(L == L)) L = 0.0f;                /* last line of defence: NaN never reaches the speakers */
        if (!(R == R)) R = 0.0f;

        float a = fa(L) > fa(R) ? fa(L) : fa(R);
        if (a > *master_peak) *master_peak = a;
        if (a > 1.0f) clip = true;                  /* step 36c: this gets clipped at the output */
        if (fa(L) > *mpk_l) *mpk_l = fa(L);
        if (fa(R) > *mpk_r) *mpk_r = fa(R);
        oL[i] = L;
        oR[i] = R;
    }
    atomic_store(&s->master_smooth_gain, mc);
    atomic_store(&s->master_smooth_pan, mp);
    if (clip) atomic_store(&s->master_clipped, true);
}

static inline float clip1(float v)
{
    if (!(v == v)) return 0.0f;
    return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}

/* gouts != NULL -> graph mode (PipeWire, step 28): every channel (after volume/pan/mute) goes into its
 * 2 buffers gouts[2c], gouts[2c+1] (from position goff); the routing and the master are links in the graph -> not done here. */
static void render_sub(Session *s, float *out, unsigned n, int out_ch,
                       bool any_solo, float *master_peak, float *mpk_l, float *mpk_r,
                       float *const *gouts, int ngo, size_t goff)
{
    queue_launch(s, n);

    memset(s_mixL, 0, n * sizeof(float));
    memset(s_mixR, 0, n * sizeof(float));
    s_hw_used = 0;

    for (int c = 0; c < s->channel_count; ++c) {
        Channel *ch = &s->channels[c];

        /* which clips of the channel are playing */
        int idx[MAX_SCENES];
        bool tail_only[MAX_SCENES];
        int na = 0;
        for (int sc = 0; sc < s->scene_count; ++sc) {
            Clip *cl = &ch->clips[sc];
            if (!atomic_load(&cl->has_clip) || atomic_load(&cl->is_note) || !cl->stream) {
                cl->dc_was_playing = false; cl->dc_tail = 0;
                continue;
            }
            bool pl = atomic_load(&cl->playing);
            if (!pl) {
                /* just stopped (stop/pause/another clip in the channel): the old audio fades out smoothly over ~2.7 ms instead of abruptly */
                if (cl->dc_was_playing) { cl->dc_was_playing = false; cl->dc_tail = DC_TAIL; }
                if (cl->dc_tail == 0) continue;
            }
            tail_only[na] = !pl;
            idx[na++] = sc;
        }

        /* channel with plugins (step 34): the clips -> sum -> the plugins -> volume/pan.
         * The plugins also run when there is no clip (echo/reverb tails). Without plugins - the old path, as before. */
        FxChain *fxc = atomic_load_explicit(&ch->fxchain, memory_order_acquire);
        bool fxm = fxc && fxc->nactive > 0;

        if (na == 0 && !fxm) {
            /* silent channel: do not spend CPU; the knobs simply "jump" to the target */
            atomic_store(&ch->smooth_gain, atomic_load(&ch->gain));
            atomic_store(&ch->smooth_pan,  atomic_load(&ch->pan));
            continue;
        }

        fill_gain_pan(ch, n);
        bool ch_clip = false;                         /* step 36c: above 0 dBFS in this block */

        bool muted  = atomic_load(&ch->muted);
        bool soloed = atomic_load(&ch->soloed);
        bool audible = !muted && !(any_solo && !soloed);

        RouteMask rl = atomic_load(&ch->route_l), rr = atomic_load(&ch->route_r);
        bool custom = gouts || !(rl == ROUTE_DEFAULT_L && rr == ROUTE_DEFAULT_R);
        if (custom) { memset(s_cl, 0, n * sizeof(float)); memset(s_cr, 0, n * sizeof(float)); }
        if (fxm) { memset(s_fxl, 0, n * sizeof(float)); memset(s_fxr, 0, n * sizeof(float)); }

        for (int k = 0; k < na; ++k) {
            Clip *cl = &ch->clips[idx[k]];
            size_t got;
            bool tail_alone = tail_only[k];
            if (!tail_alone) {
                /* new start or seek (stretch_gen changed): the old audio fades out, the new one fades in */
                unsigned gen = atomic_load(&cl->stretch_gen);
                bool restarted = cl->dc_was_playing && gen != cl->dc_gen;
                if (!cl->dc_was_playing || restarted) {
                    if (restarted) cl->dc_tail = DC_TAIL;
                    cl->dc_fadein = (cl->q_launch && !restarted) ? 0 : DC_FADEIN;   /* the seamless queue - without */
                }
                cl->q_launch = false;
                cl->dc_gen = gen;
                cl->dc_was_playing = true;
            }
            if (tail_alone) {
                memset(s_blk, 0, (size_t)n * 2 * sizeof(float));
                got = 0;
            } else if (cl->start_offset) {               /* started by the queue in the middle of the block */
                unsigned off = cl->start_offset < n ? cl->start_offset : n;
                memset(s_blk, 0, (size_t)off * 2 * sizeof(float));
                got = off + clip_read(cl, s_blk + (size_t)off * 2, n - off);
                cl->start_offset = 0;
            } else {
                got = clip_read(cl, s_blk, n);
            }

            /* no clicks: fade-in of the new audio + fade-out of the old one (from its last value) */
            if (got < n) memset(s_blk + got * 2, 0, (size_t)(n - got) * 2 * sizeof(float));
            if (cl->dc_fadein && got > 0) {
                unsigned st = DC_FADEIN - cl->dc_fadein;
                size_t lim = got < cl->dc_fadein ? got : cl->dc_fadein;
                for (size_t i = 0; i < lim; ++i) {
                    float t = ((float)(st + i) + 0.5f) / (float)DC_FADEIN;
                    float g = 0.5f - 0.5f * cosf(t * (float)M_PI);
                    s_blk[i * 2 + 0] *= g;
                    s_blk[i * 2 + 1] *= g;
                }
                cl->dc_fadein -= (unsigned)lim;
            }
            if (cl->dc_tail) {
                unsigned st = DC_TAIL - cl->dc_tail;
                size_t lim = n < cl->dc_tail ? n : cl->dc_tail;
                for (size_t i = 0; i < lim; ++i) {
                    float t = ((float)(st + i) + 0.5f) / (float)DC_TAIL;
                    float g = 0.5f + 0.5f * cosf(t * (float)M_PI);
                    s_blk[i * 2 + 0] += cl->dc_last_l * g;
                    s_blk[i * 2 + 1] += cl->dc_last_r * g;
                }
                cl->dc_tail -= (unsigned)lim;
                if (got < lim) got = lim;
            }
            if (tail_alone) {
                for (size_t i = 0; i < got; ++i) {   /* only the fade-out of the stopped clip */
                    float l = s_blk[i * 2 + 0], r = s_blk[i * 2 + 1];
                    if (fxm) { s_fxl[i] += l; s_fxr[i] += r; continue; }
                    if (audible) {
                        if (custom) { s_cl[i] += l * s_rg[i] * s_pl[i]; s_cr[i] += r * s_rg[i] * s_pr[i]; }
                        else        { s_mixL[i] += l * s_rg[i] * s_pl[i]; s_mixR[i] += r * s_rg[i] * s_pr[i]; }
                    }
                }
                continue;
            }

            /* crossfade: smooth fade-in/fade-out, sample by sample (sin/cos = equal power, no "dip") */
            bool fade_done = false;
            int xdir = atomic_load(&cl->xf_dir);
            if (xdir && atomic_load(&cl->stretch_gen) != cl->xf_gen) {      /* manually restarted/skipped */
                atomic_store(&cl->xf_dir, 0);
                xdir = 0;
            }
            if (xdir) {
                double inv = 1.0 / (double)cl->xf_len;
                for (size_t i = 0; i < got; ++i) {
                    long p = cl->xf_pos + (long)i;
                    float g;
                    if (p <= 0)                g = xdir > 0 ? 0.0f : 1.0f;
                    else if (p >= cl->xf_len)  g = xdir > 0 ? 1.0f : 0.0f;
                    else {
                        float t = (float)((double)p * inv) * (float)(M_PI * 0.5);
                        g = xdir > 0 ? sinf(t) : cosf(t);
                    }
                    s_blk[i * 2 + 0] *= g;
                    s_blk[i * 2 + 1] *= g;
                }
                cl->xf_pos += (long)n;
                if (cl->xf_pos >= cl->xf_len) {
                    atomic_store(&cl->xf_dir, 0);
                    if (xdir < 0) fade_done = true;            /* fully faded out -> stops (the silence after it is not needed) */
                }
            }

            for (size_t i = 0; i < got; ++i) {
                float l = s_blk[i * 2 + 0];
                float r = s_blk[i * 2 + 1];
                if (fxm) { s_fxl[i] += l; s_fxr[i] += r; continue; }
                if (audible) {
                    if (custom) {
                        s_cl[i] += l * s_rg[i] * s_pl[i];
                        s_cr[i] += r * s_rg[i] * s_pr[i];
                    } else {
                        s_mixL[i] += l * s_rg[i] * s_pl[i];
                        s_mixR[i] += r * s_rg[i] * s_pr[i];
                    }
                }
                float al = fa(l), ar = fa(r);
                float a = (al > ar ? al : ar) * s_rg[i];
                if (a > s_chpeak[c]) s_chpeak[c] = a;
                float pl = al * s_rg[i] * s_pl[i] * PAN_COMP, pr = ar * s_rg[i] * s_pr[i] * PAN_COMP;
                if (pl > s_chpk_l[c]) s_chpk_l[c] = pl;
                if (pr > s_chpk_r[c]) s_chpk_r[c] = pr;
                if (pl > 1.0f || pr > 1.0f) ch_clip = true;
            }

            if (got > 0) { cl->dc_last_l = s_blk[(got - 1) * 2 + 0]; cl->dc_last_r = s_blk[(got - 1) * 2 + 1]; }
            if (fade_done || clip_finished(cl)) {
                cl->dc_was_playing = false;           /* ended by itself (it is already silent) -> no fade-out */
                cl->dc_tail = 0;
                size_t sm = atomic_load(&cl->start_marker);
                size_t cl_len = __atomic_load_n(&cl->wav.frame_count, __ATOMIC_RELAXED);
                if (sm > cl_len) sm = cl_len;
                atomic_store(&cl->ended, true);       /* ended by itself (for the queue) */
                atomic_store(&cl->playing, false);
                stream_stop(cl->stream);
                if (cl->stretch) stretch_release(cl->stretch);
                cl->stretch_live = false;
                atomic_store(&cl->position, sm);
            } else {
                atomic_store(&cl->position, clip_heard_pos(cl));
            }
        }

        if (fxm) {
            for (int k = 0; k < fxc->n; ++k) if (!fxc->byp[k]) qplug_process(fxc->p[k], s_fxl, s_fxr, n);
            for (unsigned i = 0; i < n; ++i) {
                float l = s_fxl[i], r = s_fxr[i];
                if (audible) {
                    if (custom) { s_cl[i] += l * s_rg[i] * s_pl[i];   s_cr[i] += r * s_rg[i] * s_pr[i]; }
                    else        { s_mixL[i] += l * s_rg[i] * s_pl[i]; s_mixR[i] += r * s_rg[i] * s_pr[i]; }
                }
                float al = fa(l), ar = fa(r);
                float a = (al > ar ? al : ar) * s_rg[i];
                if (a > s_chpeak[c]) s_chpeak[c] = a;
                float pl = al * s_rg[i] * s_pl[i] * PAN_COMP, pr = ar * s_rg[i] * s_pr[i] * PAN_COMP;
                if (pl > s_chpk_l[c]) s_chpk_l[c] = pl;
                if (pr > s_chpk_r[c]) s_chpk_r[c] = pr;
                if (pl > 1.0f || pr > 1.0f) ch_clip = true;
            }
        }

        if (ch_clip && audible) atomic_store(&ch->clipped, true);
        if (gouts) {
            if (2 * c + 1 < ngo) {
                if (gouts[2 * c])     memcpy(gouts[2 * c] + goff, s_cl, n * sizeof(float));
                if (gouts[2 * c + 1]) memcpy(gouts[2 * c + 1] + goff, s_cr, n * sizeof(float));
            }
        } else if (custom) {
            route_mask_add(rl, s_cl, n);
            route_mask_add(rr, s_cr, n);
        }
    }
    if (gouts) return;          /* the master is a separate node in the graph */

    /* master */
    RouteMask ml = atomic_load(&s->master_route_l), mr = atomic_load(&s->master_route_r);
    bool simple = (ml == MASTER_ROUTE_DEFAULT_L && mr == MASTER_ROUTE_DEFAULT_R && s_hw_used == 0);
    master_block(s, s_mixL, s_mixR, s_cl, s_cr, n, master_peak, mpk_l, mpk_r);   /* s_cl/s_cr: the master after the volume */
    if (simple) {
        for (unsigned i = 0; i < n; ++i) {
            out[(size_t)i * out_ch + 0] = clip1(s_cl[i]);
            out[(size_t)i * out_ch + 1] = clip1(s_cr[i]);
        }
    }

    if (!simple) {
        route_mask_add(ml << 2, s_cl, n);       /* master L -> outputs */
        route_mask_add(mr << 2, s_cr, n);
        int lim = out_ch < MAX_HW_OUT ? out_ch : MAX_HW_OUT;
        for (int k = 0; k < lim; ++k) {
            if (!(s_hw_used & RBIT(k))) continue;
            for (unsigned i = 0; i < n; ++i) {
                float v = s_hw[k][i];
                if (!(v == v)) v = 0.0f;
                v = v >  1.0f ?  1.0f : (v < -1.0f ? -1.0f : v);
                out[(size_t)i * out_ch + k] = v;
            }
        }
    }
}

void mixer_render(Session *s, float *out, unsigned long frame_count, int out_ch)
{
    if (out_ch < 2) out_ch = 2;
    memset(out, 0, frame_count * (unsigned long)out_ch * sizeof(float));
    if (!s) return;
    /* the UI is moving clips in memory (inserting/deleting a channel/scene) -> this block is silence, without waiting */
    if (pthread_mutex_trylock(&s->mix_mu) != 0) {
        atomic_fetch_add(&s->audio_cycles, 1);
        return;
    }

    if (s->channel_count >= 0 && s->channel_count <= MAX_CHANNELS &&
        s->scene_count   >= 0 && s->scene_count   <= MAX_SCENES) {

        bool any_solo = false;
        for (int c = 0; c < s->channel_count; ++c)
            if (atomic_load(&s->channels[c].soloed)) { any_solo = true; break; }

        for (int c = 0; c < s->channel_count; ++c) s_chpeak[c] = s_chpk_l[c] = s_chpk_r[c] = 0.0f;
        float master_peak = 0.0f, mpk_l = 0.0f, mpk_r = 0.0f;

        unsigned long done = 0;
        while (done < frame_count) {
            unsigned n = (unsigned)(frame_count - done);
            if (n > MAX_BLOCK) n = MAX_BLOCK;
            render_sub(s, out + done * (unsigned long)out_ch, n, out_ch,
                       any_solo, &master_peak, &mpk_l, &mpk_r, NULL, 0, 0);
            done += n;
        }

        for (int c = 0; c < s->channel_count; ++c) {
            float old = atomic_load(&s->channels[c].peak_level);
            float nv  = s_chpeak[c] > old * PEAK_DECAY ? s_chpeak[c] : old * PEAK_DECAY;
            atomic_store(&s->channels[c].peak_level, nv);
        }
        #define DECAY_STORE(atom, val) do { float o_ = atomic_load(&(atom)); \
            atomic_store(&(atom), (val) > o_ * PEAK_DECAY ? (val) : o_ * PEAK_DECAY); } while (0)
        for (int c = 0; c < s->channel_count; ++c) {
            DECAY_STORE(s->channels[c].peak_l, s_chpk_l[c]);
            DECAY_STORE(s->channels[c].peak_r, s_chpk_r[c]);
        }
        DECAY_STORE(s->master_peak_l, mpk_l);
        DECAY_STORE(s->master_peak_r, mpk_r);
        #undef DECAY_STORE
        float old_m = atomic_load(&s->master_peak_level);
        float new_m = master_peak > old_m * PEAK_DECAY ? master_peak : old_m * PEAK_DECAY;
        atomic_store(&s->master_peak_level, new_m);
    }

    pthread_mutex_unlock(&s->mix_mu);
    /* signal to the UI thread that one full callback has passed (see audio_barrier in session.c) */
    atomic_fetch_add(&s->audio_cycles, 1);
}

/* ===================== Graph mode (PipeWire, step 28) ===================== */

static void decay_store(_Atomic float *atom, float val)
{
    float o = atomic_load(atom);
    atomic_store(atom, val > o * PEAK_DECAY ? val : o * PEAK_DECAY);
}

void mixer_render_channels(Session *s, float *const *outs, int nouts, unsigned long frame_count)
{
    for (int k = 0; k < nouts; ++k) if (outs[k]) memset(outs[k], 0, frame_count * sizeof(float));
    if (!s) return;
    if (pthread_mutex_trylock(&s->mix_mu) != 0) {
        atomic_fetch_add(&s->audio_cycles, 1);
        return;
    }
    if (s->channel_count >= 0 && s->channel_count <= MAX_CHANNELS &&
        s->scene_count   >= 0 && s->scene_count   <= MAX_SCENES) {
        bool any_solo = false;
        for (int c = 0; c < s->channel_count; ++c)
            if (atomic_load(&s->channels[c].soloed)) { any_solo = true; break; }
        for (int c = 0; c < s->channel_count; ++c) s_chpeak[c] = s_chpk_l[c] = s_chpk_r[c] = 0.0f;
        float dummy_m = 0, dummy_l = 0, dummy_r = 0;
        unsigned long done = 0;
        while (done < frame_count) {
            unsigned n = (unsigned)(frame_count - done);
            if (n > MAX_BLOCK) n = MAX_BLOCK;
            render_sub(s, NULL, n, 2, any_solo, &dummy_m, &dummy_l, &dummy_r, outs, nouts, done);
            done += n;
        }
        for (int c = 0; c < s->channel_count; ++c) {
            decay_store(&s->channels[c].peak_level, s_chpeak[c]);
            decay_store(&s->channels[c].peak_l, s_chpk_l[c]);
            decay_store(&s->channels[c].peak_r, s_chpk_r[c]);
        }
    }
    pthread_mutex_unlock(&s->mix_mu);
    atomic_fetch_add(&s->audio_cycles, 1);
}

static float s_zero[MAX_BLOCK];
static float s_gl[MAX_BLOCK], s_gr[MAX_BLOCK];

void mixer_render_master(Session *s, const float *in_l, const float *in_r, float *out_l, float *out_r,
                         unsigned long frame_count)
{
    if (!s) {
        if (out_l) memset(out_l, 0, frame_count * sizeof(float));
        if (out_r) memset(out_r, 0, frame_count * sizeof(float));
        return;
    }
    float master_peak = 0.0f, mpk_l = 0.0f, mpk_r = 0.0f;
    unsigned long done = 0;
    while (done < frame_count) {
        unsigned n = (unsigned)(frame_count - done);
        if (n > MAX_BLOCK) n = MAX_BLOCK;
        master_block(s, in_l ? in_l + done : s_zero, in_r ? in_r + done : s_zero, s_gl, s_gr, n,
                     &master_peak, &mpk_l, &mpk_r);
        for (unsigned i = 0; i < n; ++i) {
            if (out_l) out_l[done + i] = clip1(s_gl[i]);
            if (out_r) out_r[done + i] = clip1(s_gr[i]);
        }
        done += n;
    }
    decay_store(&s->master_peak_l, mpk_l);
    decay_store(&s->master_peak_r, mpk_r);
    decay_store(&s->master_peak_level, master_peak);
}
