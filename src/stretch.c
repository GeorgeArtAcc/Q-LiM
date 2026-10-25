/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _POSIX_C_SOURCE 200809L
#include "stretch.h"

typedef struct Wsola Wsola;
static void ws_reset(Wsola *st);

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEC 4                       /* the coarse search is done on every 4th sample */

struct Wsola {
    int        N, Hs, delta;        /* piece length, output step, search range */
    float     *win;                 /* N (Hann) */

    float     *in;                  /* stereo; in[0] = absolute frame in_base */
    size_t     in_cap, in_len;
    long long  in_base;

    float     *acc;                 /* stereo, N: overlap */
    float     *outq;                /* stereo, Hs: ready frames */
    int        outq_pos, outq_len;

    long long  prev_start;
    double     tau;                 /* nominal position of the last piece (may be fractional) */
    int        stage;               /* 0 = start, 1 = first real piece, 2 = normal */

    float     *nd, *cd;             /* mono, decimated (for the coarse search) */
};

static void free_all(Wsola *st)
{
    free(st->win); free(st->in); free(st->acc); free(st->outq); free(st->nd); free(st->cd);
    free(st);
}

static Wsola *ws_create(int sample_rate)
{
    if (sample_rate <= 0) sample_rate = 48000;
    Wsola *st = calloc(1, sizeof(*st));
    if (!st) return NULL;
    int hs = (int)lround(sample_rate * 0.021 / DEC) * DEC;  /* ~21 ms step, ~42 ms piece */
    if (hs < 64) hs = 64;
    st->Hs = hs;
    st->N = 2 * hs;
    st->delta = (int)lround(sample_rate * 0.012);           /* ±12 ms search */
    st->in_cap = (size_t)(6 * st->N + 4 * st->delta + 64);
    st->win  = malloc((size_t)st->N * sizeof(float));
    st->in   = malloc(st->in_cap * 2 * sizeof(float));
    st->acc  = malloc((size_t)st->N * 2 * sizeof(float));
    st->outq = malloc((size_t)st->Hs * 2 * sizeof(float));
    st->nd   = malloc((size_t)(st->Hs / DEC + 1) * sizeof(float));
    st->cd   = malloc((size_t)((2 * st->delta + st->Hs) / DEC + 4) * sizeof(float));
    if (!st->win || !st->in || !st->acc || !st->outq || !st->nd || !st->cd) { free_all(st); return NULL; }
    for (int i = 0; i < st->N; ++i)
        st->win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i / (float)st->N);   /* the sum at 50% overlap is exactly 1 */
    ws_reset(st);
    return st;
}

static void ws_free(Wsola *st)
{
    if (st) free_all(st);
}

static void ws_reset(Wsola *st)
{
    if (!st) return;
    st->in_len = 0;
    st->in_base = 0;
    memset(st->acc, 0, (size_t)st->N * 2 * sizeof(float));
    st->outq_pos = st->outq_len = 0;
    st->prev_start = 0;
    st->tau = 0.0;
    st->stage = 0;
}

static size_t ws_pending(const Wsola *st)
{
    if (!st || st->stage == 0) return 0;
    long long end = st->in_base + (long long)st->in_len;
    long long heard = st->prev_start + st->Hs - (st->outq_len - st->outq_pos);
    long long d = end - heard;
    return d > 0 ? (size_t)d : 0;
}

/* discards frames before keep (absolute) */
static void compact(Wsola *st, long long keep)
{
    if (keep <= st->in_base) return;
    long long drop = keep - st->in_base;
    if ((size_t)drop >= st->in_len) { st->in_base += (long long)st->in_len; st->in_len = 0; return; }
    memmove(st->in, st->in + (size_t)drop * 2, (st->in_len - (size_t)drop) * 2 * sizeof(float));
    st->in_len -= (size_t)drop;
    st->in_base = keep;
}

/* brings in data until we have everything up to absolute frame end (exclusive) */
static void ensure(Wsola *st, long long end, long long keep_from, StretchPull pull, void *ud)
{
    long long have = st->in_base + (long long)st->in_len;
    if (end <= have) return;
    if ((size_t)(end - st->in_base) > st->in_cap) compact(st, keep_from);
    have = st->in_base + (long long)st->in_len;
    while (have < end) {
        size_t room = st->in_cap - st->in_len;
        if (room == 0) break;                                  /* should not happen */
        size_t want = (size_t)(end - have);
        if (want > room) want = room;
        float *dst = st->in + st->in_len * 2;
        size_t got = pull ? pull(ud, dst, want) : 0;
        if (got < want) memset(dst + got * 2, 0, (want - got) * 2 * sizeof(float));   /* end of file = silence */
        st->in_len += want;
        have += (long long)want;
    }
}

static inline float mono_at(const Wsola *st, long long abs)
{
    long long i = abs - st->in_base;
    if (i < 0 || (size_t)i >= st->in_len) return 0.0f;
    const float *p = st->in + (size_t)i * 2;
    return (p[0] + p[1]) * 0.5f;
}

/* normalized similarity of in[a .. a+len) with in[nat .. nat+len) (full precision) */
static double sim_full(const Wsola *st, long long a, long long nat, int len)
{
    double c = 0.0, e = 0.0;
    for (int i = 0; i < len; ++i) {
        float x = mono_at(st, a + i);
        c += (double)x * mono_at(st, nat + i);
        e += (double)x * x;
    }
    return e > 1e-12 ? c / sqrt(e) : 0.0;
}

/* finds the best start in [lo, hi] that continues the natural position nat */
static long long best_start(Wsola *st, long long lo, long long hi, long long center, long long nat)
{
    const int Hs = st->Hs;
    const int nl = Hs / DEC;

    /* decimated mono of the "natural continuation" and of the whole search window */
    double ne = 0.0;
    for (int j = 0; j < nl; ++j) {
        float a = 0.0f;
        for (int k = 0; k < DEC; ++k) a += mono_at(st, nat + j * DEC + k);
        st->nd[j] = a;
        ne += (double)a * a;
    }
    if (ne < 1e-12) return center;                  /* silence — nothing to search for */

    int span = (int)((hi - lo) / DEC);
    int cl = span + nl;
    for (int j = 0; j < cl; ++j) {
        float a = 0.0f;
        for (int k = 0; k < DEC; ++k) a += mono_at(st, lo + j * DEC + k);
        st->cd[j] = a;
    }

    /* coarse: every DEC-th sample, with a sliding energy */
    double en = 0.0;
    for (int j = 0; j < nl; ++j) en += (double)st->cd[j] * st->cd[j];
    double best = -1e300;
    int bo = (int)((center - lo) / DEC);
    for (int o = 0; o <= span; ++o) {
        if (o > 0) {
            double a = st->cd[o - 1], b = st->cd[o + nl - 1];
            en += b * b - a * a;
        }
        double c = 0.0;
        const float *cp = st->cd + o;
        for (int j = 0; j < nl; ++j) c += (double)st->nd[j] * cp[j];
        double sc = en > 1e-12 ? c / sqrt(en) : 0.0;
        if (sc > best) { best = sc; bo = o; }
    }

    /* fine: ±DEC around the coarse result, at full precision (half a piece is enough) */
    long long cs = lo + (long long)bo * DEC;
    long long fbest = cs;
    double fs = -1e300;
    for (long long s = cs - DEC; s <= cs + DEC; ++s) {
        if (s < lo || s > hi) continue;
        double sc = sim_full(st, s, nat, Hs / 2);
        if (sc > fs) { fs = sc; fbest = s; }
    }
    return fbest;
}

static void do_hop(Wsola *st, double speed, StretchPull pull, void *ud)
{
    const int N = st->N, Hs = st->Hs;
    long long start;
    bool discard = false;

    if (st->stage == 0) {
        /* first piece: virtually starts Hs before the file (with zeros), so there is no "fade-in" at the start */
        st->in_base = -(long long)Hs;
        st->in_len = (size_t)Hs;
        memset(st->in, 0, (size_t)Hs * 2 * sizeof(float));
        start = -(long long)Hs;
        ensure(st, start + N, start, pull, ud);
        st->tau = (double)start;
        st->stage = 1;
        discard = true;                             /* the first half is only zeros */
    } else if (st->stage == 1) {
        start = 0;                                  /* exact continuation of the first one */
        ensure(st, start + N, st->in_base, pull, ud);
        st->tau = 0.0;
        st->stage = 2;
    } else {
        if (speed < STRETCH_SPEED_MIN) speed = STRETCH_SPEED_MIN;
        if (speed > STRETCH_SPEED_MAX) speed = STRETCH_SPEED_MAX;
        st->tau += speed * Hs;
        long long nat = st->prev_start + Hs;
        long long center = llround(st->tau);
        long long lo = center - st->delta, hi = center + st->delta;
        long long keep = nat < lo ? nat : lo;
        if (keep < st->in_base) keep = st->in_base;
        if (lo < st->in_base) lo = st->in_base;
        if (hi < lo) hi = lo;
        if (center < lo) center = lo;
        if (center > hi) center = hi;
        long long need = hi + N;
        if (nat + N > need) need = nat + N;
        ensure(st, need, keep, pull, ud);
        start = best_start(st, lo, hi, center, nat);
    }

    /* overlap-add of the piece */
    const float *src = st->in + (size_t)(start - st->in_base) * 2;
    for (int i = 0; i < N; ++i) {
        st->acc[i * 2 + 0] += src[i * 2 + 0] * st->win[i];
        st->acc[i * 2 + 1] += src[i * 2 + 1] * st->win[i];
    }
    if (!discard) {
        memcpy(st->outq, st->acc, (size_t)Hs * 2 * sizeof(float));
        st->outq_pos = 0;
        st->outq_len = Hs;
    }
    memmove(st->acc, st->acc + (size_t)Hs * 2, (size_t)(N - Hs) * 2 * sizeof(float));
    memset(st->acc + (size_t)(N - Hs) * 2, 0, (size_t)Hs * 2 * sizeof(float));
    st->prev_start = start;

    /* free the space at the front: the next search starts no earlier than min(the natural one, tau - delta) */
    long long keep2 = start + Hs;
    long long lo2 = (long long)floor(st->tau) - st->delta;
    if (lo2 < keep2) keep2 = lo2;
    if ((size_t)(keep2 - st->in_base) > st->in_cap / 2) compact(st, keep2);
}

static size_t ws_process(Wsola *st, double speed, float *out, size_t n,
                         StretchPull pull, void *ud)
{
    if (!st) { memset(out, 0, n * 2 * sizeof(float)); return n; }
    size_t done = 0;
    int guard = 0;
    while (done < n) {
        if (st->outq_pos < st->outq_len) {
            size_t k = (size_t)(st->outq_len - st->outq_pos);
            if (k > n - done) k = n - done;
            memcpy(out + done * 2, st->outq + (size_t)st->outq_pos * 2, k * 2 * sizeof(float));
            st->outq_pos += (int)k;
            done += k;
            continue;
        }
        if (++guard > 64) { memset(out + done * 2, 0, (n - done) * 2 * sizeof(float)); break; }
        do_hop(st, speed, pull, ud);
    }
    return n;
}

/* ======================= Rubber Band (R3 "finer") =======================
 * The best quality: a multi-resolution phase vocoder (the same library as the "Rubberband" keylock in Mixxx,
 * Ardour, Audacity). It is bundled in the project (third_party/rubberband) — nothing has to be installed.
 *
 * Rubber Band computes "in bursts" (sometimes 5-10 ms at once), which with a small card buffer would cause
 * dropouts. So every such clip has ITS OWN background thread that computes ~85 ms ahead and fills a ring;
 * the audio thread only reads what is ready -> no risk to the audio with any buffer size.
 *
 * Who reads the clip's stream: while the clip is "active" — the background thread; otherwise — the mixer. The switch is safe:
 *   the thread: busy=1; if !active -> busy=0, waits;  the mixer: active=0; if busy -> silence this block (it will try again).
 * If for some reason Rubber Band cannot be created, the WSOLA above is used (directly on the audio thread). */

#ifdef HAVE_RUBBERBAND
#include <rubberband/rubberband-c.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#define RB_CHUNK   1024u                /* at most this many frames per call into Rubber Band */
#define RING_CAP   16384u               /* power of 2 */
#define RING_AHEAD 4096u                /* how much ready audio we keep ahead (~85 ms @ 48 kHz) */
#endif

struct Stretch {
    Wsola  *ws;                         /* fallback (no thread) */
#ifdef HAVE_RUBBERBAND
    RubberBandState rb;
    pthread_t  thr;
    bool       thr_ok;
    atomic_bool quit;
    /* shared with the audio thread */
    atomic_bool active;                 /* the audio thread wants audio (the clip plays with warp) */
    atomic_bool busy;                   /* the background thread is inside work with the stream */
    atomic_uint gen_req, gen_done;      /* reset request / reset done */
    _Atomic float speed;
    _Atomic(StretchPull) pull;
    _Atomic(void *) ud;
    float     *ring;                    /* stereo, RING_CAP */
    atomic_size_t wr, rd;
    atomic_size_t inner_pending;        /* source frames inside Rubber Band (not in the ring yet) */
    atomic_bool eof_done;               /* the stream has ended and the tail has been played into the ring */
    atomic_bool primed;
    atomic_bool end_known;              /* we know at which frame in the ring the music ends */
    atomic_size_t end_wr;                 /* after a reset the ring has already been full once (otherwise empty just means start) */
    atomic_uint underruns;
    /* background thread only */
    float  *inL, *inR, *outL, *outR, *tmp;
    double  ratio, pulled, produced_src;
    unsigned drop;
    unsigned fade_in;                   /* after a reset: the first output frames ramp in smoothly (no "tick" on start) */
    size_t  tail_left;                  /* after the end of the file: how many more zeros to feed so the tail comes out */
    bool    input_ended;
#endif
};

static bool (*g_eof_fn)(void *ud) = NULL;
void stretch_set_eof_check(bool (*is_eof)(void *ud)) { g_eof_fn = is_eof; }

#ifdef HAVE_RUBBERBAND
#define RB_FADE_IN 128
static void sleep_us(long us)
{
    struct timespec ts = { 0, us * 1000L };
    nanosleep(&ts, NULL);
}

static void rb_do_reset(Stretch *st)
{
    rubberband_reset(st->rb);
    /* zeros in front, so it starts exactly at the beginning (no fade-in) */
    unsigned pad = rubberband_get_preferred_start_pad(st->rb);
    memset(st->inL, 0, RB_CHUNK * sizeof(float));
    memset(st->inR, 0, RB_CHUNK * sizeof(float));
    while (pad > 0) {
        unsigned k = pad < RB_CHUNK ? pad : RB_CHUNK;
        const float *ins[2] = { st->inL, st->inR };
        rubberband_process(st->rb, ins, k, 0);
        pad -= k;
    }
    st->drop = rubberband_get_start_delay(st->rb);
    st->fade_in = RB_FADE_IN;
    st->pulled = st->produced_src = 0.0;
    st->input_ended = false;
    st->tail_left = 0;
    atomic_store(&st->inner_pending, 0);
    atomic_store(&st->eof_done, false);
    atomic_store(&st->primed, false);
    atomic_store(&st->end_known, false);
    /* the audio thread does not read while gen_done != gen_req -> rd stays put; empty the ring */
    atomic_store(&st->wr, atomic_load(&st->rd));
}

/* Fills the ring while there are fewer than RING_AHEAD ready frames. Called only from the background thread, with busy=1. */
static void rb_fill(Stretch *st)
{
    StretchPull pull = atomic_load(&st->pull);
    void *ud = atomic_load(&st->ud);
    float sp = atomic_load(&st->speed);
    if (sp < (float)STRETCH_SPEED_MIN) sp = (float)STRETCH_SPEED_MIN;
    if (sp > (float)STRETCH_SPEED_MAX) sp = (float)STRETCH_SPEED_MAX;
    double ratio = 1.0 / sp;
    if (fabs(ratio - st->ratio) > 1e-9) { rubberband_set_time_ratio(st->rb, ratio); st->ratio = ratio; }

    for (int guard = 0; guard < 64; ++guard) {
        size_t wr = atomic_load(&st->wr), rd = atomic_load(&st->rd);
        size_t fill = wr - rd;
        if (fill >= RING_AHEAD || atomic_load(&st->eof_done)) { atomic_store(&st->primed, true); break; }
        if (atomic_load(&st->gen_done) != atomic_load(&st->gen_req)) break;   /* new reset request */

        int av = rubberband_available(st->rb);
        if (av > 0) {
            unsigned k = (unsigned)av;
            if (k > RB_CHUNK) k = RB_CHUNK;
            if (k > RING_CAP - fill) k = (unsigned)(RING_CAP - fill);
            float *outs[2] = { st->outL, st->outR };
            unsigned got = (unsigned)rubberband_retrieve(st->rb, outs, k);
            unsigned skip = 0;
            if (st->drop) { skip = got < st->drop ? got : st->drop; st->drop -= skip; }
            for (unsigned i = skip; i < got; ++i) {
                size_t w = (wr++) & (RING_CAP - 1);
                float g = 1.0f;
                if (st->fade_in) {
                    float t = (float)(RB_FADE_IN - st->fade_in) / (float)RB_FADE_IN;
                    g = 0.5f - 0.5f * cosf(t * 3.14159265f);
                    st->fade_in--;
                }
                st->ring[w * 2 + 0] = st->outL[i] * g;
                st->ring[w * 2 + 1] = st->outR[i] * g;
            }
            st->produced_src += (double)(got - skip) * sp;
            atomic_store(&st->wr, wr);
            double ip = st->pulled - st->produced_src;
            atomic_store(&st->inner_pending, ip > 0.0 ? (size_t)ip : 0);
            continue;
        }
        if (st->input_ended && st->tail_left == 0) { atomic_store(&st->eof_done, true); break; }
        unsigned req = rubberband_get_samples_required(st->rb);
        if (req == 0) req = 256;
        if (req > RB_CHUNK) req = RB_CHUNK;
        size_t got = 0;
        if (!st->input_ended) {
            got = pull ? pull(ud, st->tmp, req) : 0;
            if (got < req && g_eof_fn && ud && !g_eof_fn(ud)) {
                /* the disk has not delivered the audio yet (it is not the end): feed what there is and try again shortly */
                if (got > 0) {
                    st->pulled += (double)got;
                    for (unsigned i = 0; i < got; ++i) { st->inL[i] = st->tmp[i * 2]; st->inR[i] = st->tmp[i * 2 + 1]; }
                    const float *ins[2] = { st->inL, st->inR };
                    rubberband_process(st->rb, ins, (unsigned)got, 0);
                }
                break;
            }
            if (got < req) {                            /* end of the file: feed a bit more silence for the tail */
                st->input_ended = true;
                /* the music ends where the output gets to once the last fed frame comes out too */
                st->pulled += (double)got;
                double lat_out = (st->pulled - st->produced_src) / sp;
                if (lat_out < 0.0) lat_out = 0.0;
                atomic_store(&st->end_wr, atomic_load(&st->wr) + (size_t)(lat_out + 0.5));
                atomic_store(&st->end_known, true);
                st->pulled -= (double)got;              /* added from below, as always */
                st->tail_left = (size_t)rubberband_get_start_delay(st->rb) + 2 * RB_CHUNK;
            }
            st->pulled += (double)got;
        } else {
            req = (unsigned)(st->tail_left < req ? st->tail_left : req);
            st->tail_left -= req;
        }
        for (unsigned i = 0; i < req; ++i) {
            if (i < got) { st->inL[i] = st->tmp[i * 2]; st->inR[i] = st->tmp[i * 2 + 1]; }
            else         { st->inL[i] = 0.0f; st->inR[i] = 0.0f; }
        }
        const float *ins[2] = { st->inL, st->inR };
        rubberband_process(st->rb, ins, req, 0);
    }
}

static void *rb_thread(void *arg)
{
    Stretch *st = arg;
    while (!atomic_load(&st->quit)) {
        atomic_store(&st->busy, true);
        if (!atomic_load(&st->active)) {               /* the clip is not playing with warp -> do not touch the stream */
            atomic_store(&st->busy, false);
            sleep_us(4000);
            continue;
        }
        unsigned g = atomic_load(&st->gen_req);
        if (g != atomic_load(&st->gen_done)) {
            rb_do_reset(st);
            atomic_store(&st->gen_done, g);
        }
        rb_fill(st);
        atomic_store(&st->busy, false);
        sleep_us(1500);
    }
    return NULL;
}

static void rb_destroy(Stretch *st)
{
    if (st->thr_ok) {
        atomic_store(&st->quit, true);
        pthread_join(st->thr, NULL);
    }
    if (st->rb) rubberband_delete(st->rb);
    free(st->ring); free(st->inL); free(st->inR); free(st->outL); free(st->outR); free(st->tmp);
    st->rb = NULL;
}
#endif

Stretch *stretch_create(int sample_rate)
{
    Stretch *st = calloc(1, sizeof(*st));
    if (!st) return NULL;
#ifdef HAVE_RUBBERBAND
    RubberBandOptions o = RubberBandOptionProcessRealTime | RubberBandOptionEngineFiner |
                          RubberBandOptionChannelsTogether;
    st->rb = rubberband_new((unsigned)(sample_rate > 0 ? sample_rate : 48000), 2, o, 1.0, 1.0);
    st->ring = calloc((size_t)RING_CAP * 2, sizeof(float));
    st->inL = malloc(RB_CHUNK * sizeof(float));  st->inR = malloc(RB_CHUNK * sizeof(float));
    st->outL = malloc(RB_CHUNK * sizeof(float)); st->outR = malloc(RB_CHUNK * sizeof(float));
    st->tmp = malloc(RB_CHUNK * 2 * sizeof(float));
    if (st->rb && st->ring && st->inL && st->inR && st->outL && st->outR && st->tmp) {
        rubberband_set_max_process_size(st->rb, RB_CHUNK);
        st->ratio = 1.0;
        atomic_store(&st->speed, 1.0f);
        atomic_store(&st->gen_req, 1);                 /* the first start begins with a reset */
        atomic_store(&st->gen_done, 0);
        if (pthread_create(&st->thr, NULL, rb_thread, st) == 0) {
            st->thr_ok = true;
            return st;
        }
    }
    rb_destroy(st);
#endif
    st->ws = ws_create(sample_rate);
    if (!st->ws) { free(st); return NULL; }
    return st;
}

void stretch_free(Stretch *st)
{
    if (!st) return;
#ifdef HAVE_RUBBERBAND
    if (st->rb) rb_destroy(st);
#endif
    ws_free(st->ws);
    free(st);
}

void stretch_reset(Stretch *st)
{
    if (!st) return;
#ifdef HAVE_RUBBERBAND
    if (st->rb) { atomic_fetch_add(&st->gen_req, 1); return; }   /* the background thread will reset it */
#endif
    ws_reset(st->ws);
}

bool stretch_release(Stretch *st)
{
    if (!st) return true;
#ifdef HAVE_RUBBERBAND
    if (st->rb) {
        atomic_store(&st->active, false);
        return !atomic_load(&st->busy);               /* true = the background thread no longer touches the stream */
    }
#endif
    return true;
}

void stretch_prepare(Stretch *st, double speed, StretchPull pull, void *ud)
{
    if (!st) return;
#ifdef HAVE_RUBBERBAND
    if (st->rb) {
        atomic_store(&st->speed, (float)speed);
        atomic_store(&st->pull, pull);
        atomic_store(&st->ud, ud);
        atomic_fetch_add(&st->gen_req, 1);
        atomic_store(&st->active, true);
    }
#else
    (void)speed; (void)pull; (void)ud;
#endif
}

bool stretch_finished(const Stretch *st)
{
    if (!st) return true;
#ifdef HAVE_RUBBERBAND
    if (st->rb)
        return atomic_load(&st->eof_done) && atomic_load(&st->wr) == atomic_load(&st->rd) &&
               atomic_load(&st->gen_done) == atomic_load(&st->gen_req);
#endif
    return false;                                     /* WSOLA: the end is taken from the stream */
}

bool stretch_threaded(const Stretch *st)
{
#ifdef HAVE_RUBBERBAND
    return st && st->rb;
#else
    (void)st;
    return false;
#endif
}

size_t stretch_pending(const Stretch *st)
{
    if (!st) return 0;
#ifdef HAVE_RUBBERBAND
    if (st->rb) {
        size_t fill = atomic_load(&st->wr) - atomic_load(&st->rd);
        double sp = atomic_load(&st->speed);
        return atomic_load(&st->inner_pending) + (size_t)((double)fill * sp);
    }
#endif
    return ws_pending(st->ws);
}

long stretch_frames_to_end(const Stretch *st)
{
#ifdef HAVE_RUBBERBAND
    if (st && st->rb && atomic_load(&st->end_known) && atomic_load(&st->gen_done) == atomic_load(&st->gen_req)) {
        size_t e = atomic_load(&st->end_wr), rd = atomic_load(&st->rd);
        return e > rd ? (long)(e - rd) : 0;
    }
#endif
    (void)st;
    return -1;
}

unsigned stretch_underruns(const Stretch *st)
{
#ifdef HAVE_RUBBERBAND
    if (st && st->rb) return atomic_load(&st->underruns);
#endif
    (void)st;
    return 0;
}

const char *stretch_engine_name(void)
{
#ifdef HAVE_RUBBERBAND
    return "Rubber Band R3";
#else
    return "WSOLA";
#endif
}

size_t stretch_process(Stretch *st, double speed, float *out, size_t n,
                       StretchPull pull, void *ud)
{
    if (!st) { memset(out, 0, n * 2 * sizeof(float)); return n; }
#ifdef HAVE_RUBBERBAND
    if (st->rb) {
        atomic_store(&st->speed, (float)speed);
        atomic_store(&st->pull, pull);
        atomic_store(&st->ud, ud);
        atomic_store(&st->active, true);
        size_t done = 0;
        if (atomic_load(&st->gen_done) == atomic_load(&st->gen_req)) {
            size_t rd = atomic_load(&st->rd), wr = atomic_load(&st->wr);
            size_t k = wr - rd;
            if (k > n) k = n;
            for (size_t i = 0; i < k; ++i) {
                size_t r = (rd + i) & (RING_CAP - 1);
                out[i * 2 + 0] = st->ring[r * 2 + 0];
                out[i * 2 + 1] = st->ring[r * 2 + 1];
            }
            atomic_store(&st->rd, rd + k);
            done = k;
            if (done < n && !atomic_load(&st->eof_done) && atomic_load(&st->primed))
                atomic_fetch_add(&st->underruns, 1);      /* a real "could not keep up" (not the very start) */
        }
        if (done < n) memset(out + done * 2, 0, (n - done) * 2 * sizeof(float));
        return n;
    }
#endif
    return ws_process(st->ws, speed, out, n, pull, ud);
}
