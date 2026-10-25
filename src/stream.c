/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _POSIX_C_SOURCE 200809L
#include "stream.h"
#include "diskinfo.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* Three buffer sizes (tiers). Every stream has its own, according to the disk it reads from (see diskinfo.c). */
typedef struct { size_t ring, pre, chunk; } Tier;
static const Tier g_tiers[3] = {
    {  65536u, 24000u,  4096u },     /* 0 small : SSD/NVMe/RAM   ~1.4 s buffer, 0.5 s start  */
    { 131072u, 48000u,  8192u },     /* 1 medium: USB/SD/unknown ~2.7 s buffer, 1 s start    */
    { 262144u, 96000u, 16384u },     /* 2 large : HDD/network    ~5.5 s buffer, 2 s start    */
};

typedef struct { size_t start, end, back; bool loop; } Eff;

/* No clicks (step 23):
 *  - loop: the last LOOP_XF frames before the loop end are blended smoothly with the audio RIGHT before loop start ->
 *    at the jump back to the start the waveform continues without a break (like a "loop crossfade" in DAWs);
 *  - end (End marker in the middle of the file, no loop): a short fade-out of EDGE_FADE frames;
 *  - start (Start marker in the middle of the file): a short fade-in of EDGE_FADE frames on start. */
#define LOOP_XF   384      /* ~8 ms at 48 kHz */
#define EDGE_FADE 128      /* ~2.7 ms */

struct ClipStream {
    StreamSource   src;
    size_t         total;
    StreamRegionFn region_fn;
    void          *region_ud;

    int            tier;        /* 0..2, see g_tiers */
    size_t         ring_frames, ring_mask, pre_cap, chunk;
    int            dev_idx;     /* index into g_devs; -1 = disk unknown */
    DiskClass      dclass;
    atomic_uint    starved;     /* a real "the buffer ran dry" during playback (not a seek) */
    unsigned       seen_starved;
    size_t         c_ring_got;  /* audio thread: frames from the ring since the start */
    atomic_int     in_read;     /* the audio thread is inside stream_read */
    atomic_int     resizing;    /* the UI is replacing the buffers -> the audio thread returns silence */
    double         idle_since;  /* UI: since when it has not been playing (for freeing the buffer); 0 = playing / unknown */

    float         *ring;        /* ring_frames * 2 */
    uint32_t      *rpos;        /* file position of every ring frame */
    float         *pre;         /* preroll */
    float         *tmp;         /* chunk * 2, disk thread only */
    float          xbuf[LOOP_XF * 2];   /* the audio before loop start (for the loop crossfade); disk thread only / under g_mu */
    Eff            pre_eff;     /* the region the preroll was built with (the fades depend on it) */

    size_t         pre_start;
    size_t         pre_n;
    atomic_bool    pre_valid;

    /* request UI -> disk/audio, published through req_epoch */
    atomic_size_t  req_from;
    atomic_size_t  req_pre;
    atomic_uint    req_epoch;
    atomic_bool    active;

    /* owned by the audio thread */
    unsigned       c_epoch;
    size_t         c_pre_from, c_pre_n, c_pre_pos;
    bool           c_synced;
    atomic_size_t  rd;
    atomic_size_t  play_pos;
    atomic_bool    finished;
    atomic_uint    underruns;

    /* owned by the disk thread */
    unsigned       p_epoch;
    size_t         file_pos;
    bool           p_eff_valid;
    Eff            p_eff;
    atomic_size_t  wr;
    atomic_size_t  flush_wr;
    atomic_uint    ring_epoch;
    atomic_uint    eof_epoch;
};

/* ---------- global ---------- */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static ClipStream    **g_list;
static size_t          g_n, g_cap;
static pthread_t       g_thr;
static atomic_bool     g_run;
static bool            g_started;
static atomic_size_t   g_margin = 4096;
static atomic_int      g_mode = 0;               /* 0 = auto, 1/2/3 = small/medium/large for all */
static atomic_ulong    g_starved_total;

/* Known disks: class + how much we raised the tier because the buffer ran dry */
#define MAX_DEVS 32
typedef struct { unsigned long long key; DiskClass cls; int bump; double last_bump; } DevInfo;
static DevInfo         g_devs[MAX_DEVS];
static int             g_ndevs;
static pthread_mutex_t g_dev_mu = PTHREAD_MUTEX_INITIALIZER;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int class_tier(DiskClass c)
{
    switch (c) {
    case DISK_HDD: case DISK_NETWORK:          return 2;
    case DISK_REMOVABLE: case DISK_UNKNOWN:    return 1;
    default:                                   return 0;     /* NVMe, SSD, RAM */
    }
}

static const char *tier_name(int t) { return t == 0 ? "small" : t == 1 ? "medium" : "large"; }

/* Finds/creates an entry for the disk. Returns an index or -1 (the table is full). */
static int dev_find(unsigned long long key, DiskClass cls, const char *path_for_log)
{
    pthread_mutex_lock(&g_dev_mu);
    for (int i = 0; i < g_ndevs; ++i)
        if (g_devs[i].key == key) { pthread_mutex_unlock(&g_dev_mu); return i; }
    int idx = -1;
    if (g_ndevs < MAX_DEVS) {
        idx = g_ndevs++;
        g_devs[idx].key = key; g_devs[idx].cls = cls; g_devs[idx].bump = 0; g_devs[idx].last_bump = -100.0;
    }
    pthread_mutex_unlock(&g_dev_mu);
    if (idx >= 0)
        printf(">>> Disk of '%s': %s -> %s read buffer (%.1f s)\n", path_for_log ? path_for_log : "?",
               diskinfo_class_name(cls), tier_name(class_tier(cls)),
               (double)g_tiers[class_tier(cls)].ring / 48000.0);
    return idx;
}

static int desired_tier(const ClipStream *s);

static void sleep_ms(long ms)
{
    struct timespec ts = { 0, ms * 1000000L };
    nanosleep(&ts, NULL);
}

static Eff make_eff(const StreamRegion *in, size_t total)
{
    size_t start = in->start > total ? total : in->start;
    size_t end, back;
    if (in->loop) {
        size_t le = in->loop_end;
        if (le == 0 || le > total) le = total;
        size_t ls = in->loop_start;
        if (ls > le) ls = le;
        end = le;
        back = ls;
    } else {
        size_t e = in->end;
        if (e == 0 || e > total) e = total;
        end = e;
        back = start;
    }
    if (start > end) start = end;
    if (back < start) back = start;     /* effective loop start = max(loop_start, start) */
    Eff r = { start, end, back, in->loop };
    return r;
}

static Eff get_eff(ClipStream *s)
{
    StreamRegion raw;
    memset(&raw, 0, sizeof(raw));
    s->region_fn(s->region_ud, &raw);
    return make_eff(&raw, s->total);
}

static bool eff_eq(const Eff *a, const Eff *b)
{
    return a->start == b->start && a->end == b->end &&
           a->back == b->back && a->loop == b->loop;
}

/* ---------- disk thread ---------- */

/* The smooth transitions on the audio read: buf = frames [pos, pos+n) of the file.
 * initial = this is the preroll (the first start from Start), only there is there a fade-in at the start. */
static void edge_fades(ClipStream *s, const Eff *r, size_t pos, float *buf, size_t n, bool initial)
{
    if (n == 0) return;
    size_t p_end = pos + n;

    if (r->loop && r->back < r->end) {
        size_t len = r->end - r->back;
        size_t L = LOOP_XF;
        if (L > len / 4) L = len / 4;
        if (L >= 8) {
            size_t x0 = r->end - L;                       /* the crossfade zone: [x0, end) */
            size_t a = pos > x0 ? pos : x0, b = p_end < r->end ? p_end : r->end;
            if (a < b) {
                /* the audio RIGHT before loop start: [back - L, back) (before the start of the file = silence) */
                size_t k0 = a - x0, cnt = b - a;
                memset(s->xbuf, 0, cnt * 2 * sizeof(float));
                long long pre0 = (long long)r->back - (long long)L + (long long)k0;   /* may be < 0 */
                size_t skip = pre0 < 0 ? (size_t)(-pre0) : 0;
                if (skip < cnt) {
                    size_t got = s->src.read(s->src.ctx, (size_t)(pre0 + (long long)skip), s->xbuf + skip * 2, cnt - skip);
                    (void)got;                                /* what was not read stays silence */
                }
                for (size_t i = 0; i < cnt; ++i) {
                    float t = ((float)(k0 + i) + 0.5f) / (float)L;
                    float w_in = 0.5f - 0.5f * cosf(t * 3.14159265f);   /* smooth S-curve, the sum is 1 */
                    float *o = buf + (a - pos + i) * 2;
                    o[0] = o[0] * (1.0f - w_in) + s->xbuf[i * 2 + 0] * w_in;
                    o[1] = o[1] * (1.0f - w_in) + s->xbuf[i * 2 + 1] * w_in;
                }
            }
        }
    } else if (!r->loop && r->end < s->total && r->end > r->start) {
        size_t F = EDGE_FADE;
        if (F > (r->end - r->start) / 2) F = (r->end - r->start) / 2;
        if (F >= 2) {
            size_t x0 = r->end - F;
            size_t a = pos > x0 ? pos : x0, b = p_end < r->end ? p_end : r->end;
            for (size_t f = a; f < b; ++f) {
                float t = ((float)(f - x0) + 0.5f) / (float)F;
                float g = 0.5f + 0.5f * cosf(t * 3.14159265f);           /* 1 -> 0 */
                buf[(f - pos) * 2 + 0] *= g;
                buf[(f - pos) * 2 + 1] *= g;
            }
        }
    }

    if (initial && r->start > 0 && r->end > r->start) {   /* start in the middle of the file */
        size_t F = EDGE_FADE;
        if (F > (r->end - r->start) / 2) F = (r->end - r->start) / 2;
        size_t a = pos > r->start ? pos : r->start, b = p_end < r->start + F ? p_end : r->start + F;
        for (size_t f = a; f < b; ++f) {
            float t = ((float)(f - r->start) + 0.5f) / (float)F;
            float g = 0.5f - 0.5f * cosf(t * 3.14159265f);               /* 0 -> 1 */
            buf[(f - pos) * 2 + 0] *= g;
            buf[(f - pos) * 2 + 1] *= g;
        }
    }
}

static void build_preroll(ClipStream *s)
{
    Eff r = get_eff(s);
    size_t n = s->total > r.start ? s->total - r.start : 0;
    if (n > s->pre_cap) n = s->pre_cap;
    /* The new preroll is read into a separate buffer and then swapped in with the Dekker handshake (in_read/resizing):
     * a stopped clip may still be read (fade-out, stretch thread) from the old buffer - we do not write over it. */
    float *nb = calloc(s->pre_cap * 2, sizeof(float));
    float *dst = nb ? nb : s->pre;
    if (!nb) {                                       /* out of memory: wait for the reader and write in place */
        atomic_store(&s->resizing, 1);
        while (atomic_load(&s->in_read)) sched_yield();
    }
    size_t got = n ? s->src.read(s->src.ctx, r.start, dst, n) : 0;
    edge_fades(s, &r, r.start, dst, got, true);
    if (nb) {
        atomic_store(&s->resizing, 1);
        while (atomic_load(&s->in_read)) sched_yield();
        float *old = s->pre;
        s->pre = nb;
        atomic_store(&s->resizing, 0);
        free(old);
    } else {
        atomic_store(&s->resizing, 0);
    }
    s->pre_eff = r;
    s->pre_start = r.start;
    s->pre_n = got;
    atomic_store_explicit(&s->pre_valid, true, memory_order_release);
}

/* The region/loop changed -> throw away the buffered audio not played yet
 * (keeping a margin), so the loop switch reacts within ~80 ms, not 2.7 s. */
static void trim(ClipStream *s)
{
    size_t wr = atomic_load_explicit(&s->wr, memory_order_relaxed);
    size_t rd = atomic_load_explicit(&s->rd, memory_order_acquire);
    size_t fl = atomic_load_explicit(&s->flush_wr, memory_order_relaxed);
    if (rd < fl) rd = fl;
    size_t keep = rd + atomic_load_explicit(&g_margin, memory_order_relaxed);
    if (wr > keep) {
        s->file_pos = (size_t)s->rpos[(keep - 1) & s->ring_mask] + 1;
        atomic_store_explicit(&s->wr, keep, memory_order_release);
        atomic_store_explicit(&s->eof_epoch, 0, memory_order_release);
    }
}

static void note_starved(ClipStream *s, unsigned delta);

static bool service(ClipStream *s)
{
    unsigned st = atomic_load_explicit(&s->starved, memory_order_relaxed);
    if (st != s->seen_starved) {
        unsigned d = st - s->seen_starved;
        s->seen_starved = st;
        note_starved(s, d);
    }

    if (!atomic_load_explicit(&s->active, memory_order_acquire)) {
        if (!atomic_load_explicit(&s->pre_valid, memory_order_acquire)) {
            build_preroll(s);
        } else {
            /* loop/markers changed -> the preroll has other transitions. ONLY mark it for rebuilding (in the next
             * round): at this moment the UI may already have started the stream with it, and overwriting it "on the fly" breaks the audio. */
            Eff cur = get_eff(s);
            if (!eff_eq(&cur, &s->pre_eff)) atomic_store_explicit(&s->pre_valid, false, memory_order_release);
        }
        return false;
    }

    unsigned e = atomic_load_explicit(&s->req_epoch, memory_order_acquire);
    if (e != s->p_epoch) {
        s->p_epoch = e;
        s->file_pos = atomic_load_explicit(&s->req_from, memory_order_relaxed) +
                      atomic_load_explicit(&s->req_pre,  memory_order_relaxed);
        size_t w = atomic_load_explicit(&s->wr, memory_order_relaxed);
        atomic_store_explicit(&s->flush_wr, w, memory_order_relaxed);
        atomic_store_explicit(&s->ring_epoch, e, memory_order_release);
        s->p_eff_valid = false;
    }
    if (e == 0) return false;

    Eff r = get_eff(s);
    if (s->p_eff_valid && !eff_eq(&r, &s->p_eff)) trim(s);
    s->p_eff = r;
    s->p_eff_valid = true;

    if (atomic_load_explicit(&s->eof_epoch, memory_order_acquire) == s->p_epoch)
        return false;

    bool did = false;
    for (int pass = 0; pass < 4; ++pass) {
        size_t wr = atomic_load_explicit(&s->wr, memory_order_relaxed);
        size_t rd = atomic_load_explicit(&s->rd, memory_order_acquire);
        size_t fl = atomic_load_explicit(&s->flush_wr, memory_order_relaxed);
        if (rd < fl) rd = fl;
        size_t used = wr > rd ? wr - rd : 0;
        if (s->ring_frames - used < s->chunk) break;

        size_t pos = s->file_pos;
        if (pos < r.start) pos = r.start;
        if (pos >= r.end) {
            if (r.loop && r.back < r.end) {
                pos = r.back;
            } else {
                s->file_pos = pos;
                atomic_store_explicit(&s->eof_epoch, s->p_epoch, memory_order_release);
                break;
            }
        }
        size_t n = s->chunk;
        if (n > r.end - pos) n = r.end - pos;

        size_t got = s->src.read(s->src.ctx, pos, s->tmp, n);
        if (got == 0) {         /* short file / read error */
            atomic_store_explicit(&s->eof_epoch, s->p_epoch, memory_order_release);
            break;
        }
        edge_fades(s, &r, pos, s->tmp, got, false);

        size_t at = wr & s->ring_mask;
        size_t first = s->ring_frames - at;
        if (first > got) first = got;
        memcpy(s->ring + at * 2, s->tmp, first * 2 * sizeof(float));
        if (got > first)
            memcpy(s->ring, s->tmp + first * 2, (got - first) * 2 * sizeof(float));
        for (size_t i = 0; i < got; ++i)
            s->rpos[(wr + i) & s->ring_mask] = (uint32_t)(pos + i);

        s->file_pos = pos + got;
        atomic_store_explicit(&s->wr, wr + got, memory_order_release);
        did = true;
    }
    return did;
}

/* How much audio is buffered (the less, the more urgent). Inactive ones -> SIZE_MAX. */
static size_t fill_of(ClipStream *s)
{
    if (!atomic_load_explicit(&s->active, memory_order_acquire) || !s->ring) return (size_t)-1;
    size_t wr = atomic_load_explicit(&s->wr, memory_order_relaxed);
    size_t rd = atomic_load_explicit(&s->rd, memory_order_acquire);
    size_t fl = atomic_load_explicit(&s->flush_wr, memory_order_relaxed);
    if (rd < fl) rd = fl;
    return wr > rd ? wr - rd : 0;
}

#define MAX_SORT 256

static void *disk_main(void *arg)
{
    (void)arg;
    ClipStream *act[MAX_SORT];
    size_t      fil[MAX_SORT];
    while (atomic_load(&g_run)) {
        bool did = false;
        pthread_mutex_lock(&g_mu);
        size_t na = 0;
        for (size_t i = 0; i < g_n; ++i) {
            ClipStream *s = g_list[i];
            size_t f = fill_of(s);
            if (f == (size_t)-1 || na >= MAX_SORT) did |= service(s);      /* inactive: preroll / report */
            else { act[na] = s; fil[na] = f; ++na; }
        }
        /* with a slow disk the stream that will run out of audio soonest is served first */
        for (size_t i = 1; i < na; ++i) {
            ClipStream *cs = act[i]; size_t cf = fil[i]; size_t j = i;
            while (j > 0 && fil[j - 1] > cf) { act[j] = act[j - 1]; fil[j] = fil[j - 1]; --j; }
            act[j] = cs; fil[j] = cf;
        }
        for (size_t i = 0; i < na; ++i) did |= service(act[i]);
        pthread_mutex_unlock(&g_mu);
        if (!did) sleep_ms(2);
    }
    return NULL;
}

int streams_init(void)
{
    if (g_started) return 0;
    atomic_store(&g_run, true);
    if (pthread_create(&g_thr, NULL, disk_main, NULL) != 0) {
        atomic_store(&g_run, false);
        return 1;
    }
    g_started = true;
    return 0;
}

void streams_shutdown(void)
{
    if (!g_started) return;
    atomic_store(&g_run, false);
    pthread_join(g_thr, NULL);
    g_started = false;
}

void streams_set_trim_margin(size_t frames)
{
    if (frames < 1024) frames = 1024;
    if (frames > STREAM_RING_FRAMES / 4) frames = STREAM_RING_FRAMES / 4;
    atomic_store(&g_margin, frames);
}

/* ---------- UI thread ---------- */

static void apply_tier_params(ClipStream *s, int tier)
{
    const Tier *t = &g_tiers[tier];
    s->tier = tier;
    s->ring_frames = t->ring;
    s->ring_mask = t->ring - 1;
    s->pre_cap = t->pre;
    s->chunk = t->chunk;
}

static int desired_tier(const ClipStream *s)
{
    int m = atomic_load(&g_mode);
    if (m >= 1 && m <= 3) return m - 1;                 /* manual: one size for all */
    if (s->dev_idx < 0) return 0;
    pthread_mutex_lock(&g_dev_mu);
    int t = class_tier(g_devs[s->dev_idx].cls) + g_devs[s->dev_idx].bump;   /* + raised because the buffer ran dry */
    pthread_mutex_unlock(&g_dev_mu);
    return t > 2 ? 2 : t;
}

/* Disk thread: a stream's buffer ran dry during playback -> in auto mode raise the tier of THAT disk
 * (applies to its clips the next time they are started). Not more often than once every 2 s. */
static void note_starved(ClipStream *s, unsigned delta)
{
    atomic_fetch_add(&g_starved_total, delta);
    if (atomic_load(&g_mode) != 0 || s->dev_idx < 0) return;
    int raised = -1;
    double now = now_s();
    pthread_mutex_lock(&g_dev_mu);
    DevInfo *d = &g_devs[s->dev_idx];
    if (now - d->last_bump >= 2.0 && class_tier(d->cls) + d->bump < 2) {
        d->bump++;
        d->last_bump = now;
        raised = class_tier(d->cls) + d->bump;
    }
    pthread_mutex_unlock(&g_dev_mu);
    if (raised >= 0)
        printf(">>> Disk buffer ran dry (%s disk) -> raising read buffer to %s (applies when clips are triggered again)\n",
               diskinfo_class_name(s->dclass), tier_name(raised));
}

ClipStream *stream_open_path(const StreamSource *src, StreamRegionFn fn, void *ud, const char *path)
{
    if (!src || !src->read || !src->frames || !fn) return NULL;
    ClipStream *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->src = *src;
    s->total = src->frames(src->ctx);
    s->region_fn = fn;
    s->region_ud = ud;
    s->dev_idx = -1;
    s->dclass = DISK_UNKNOWN;
    if (path && path[0]) {
        unsigned long long key = 0;
        DiskClass c = diskinfo_classify_path(path, &key);
        if (key) { s->dclass = c; s->dev_idx = dev_find(key, c, path); }
    }
    apply_tier_params(s, desired_tier(s));
    /* ring/rpos/tmp are allocated lazily at the first stream_start() */
    s->pre  = calloc(s->pre_cap * 2, sizeof(float));
    if (!s->pre) { free(s); return NULL; }
    build_preroll(s);               /* synchronous: the first start is instant */

    pthread_mutex_lock(&g_mu);
    if (g_n == g_cap) {
        size_t nc = g_cap ? g_cap * 2 : 64;
        ClipStream **nl = realloc(g_list, nc * sizeof(*nl));
        if (!nl) {
            pthread_mutex_unlock(&g_mu);
            free(s->pre); free(s);
            return NULL;
        }
        g_list = nl;
        g_cap = nc;
    }
    g_list[g_n++] = s;
    pthread_mutex_unlock(&g_mu);
    return s;
}

ClipStream *stream_open(const StreamSource *src, StreamRegionFn fn, void *ud)
{
    return stream_open_path(src, fn, ud, NULL);
}

void stream_close(ClipStream *s)
{
    if (!s) return;
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < g_n; ++i) {
        if (g_list[i] == s) { g_list[i] = g_list[--g_n]; break; }
    }
    pthread_mutex_unlock(&g_mu);

    if (s->src.close) s->src.close(s->src.ctx);
    free(s->ring); free(s->rpos); free(s->pre); free(s->tmp);
    free(s);
}

size_t stream_frames(const ClipStream *s) { return s ? s->total : 0; }
void stream_set_total_nolock(ClipStream *s, size_t total) { if (s) s->total = total; }

void stream_invalidate_preroll(ClipStream *s)
{
    if (s) atomic_store_explicit(&s->pre_valid, false, memory_order_release);
}

/* Changes the buffer sizes. ONLY while the stream is not active. The audio thread is briefly kept from reading
 * (resizing/in_read), the disk thread is kept out through g_mu. */
static void retier(ClipStream *s, int tier)
{
    const Tier *t = &g_tiers[tier];
    float *pre = calloc(t->pre * 2, sizeof(float));
    float *ring = NULL, *tmp = NULL;
    uint32_t *rpos = NULL;
    bool bad = (pre == NULL);
    if (s->ring) {
        ring = calloc(t->ring * 2, sizeof(float));
        rpos = calloc(t->ring, sizeof(uint32_t));
        tmp  = calloc(t->chunk * 2, sizeof(float));
        if (!ring || !rpos || !tmp) bad = true;
    }
    if (bad) { free(pre); free(ring); free(rpos); free(tmp); return; }     /* out of memory -> stay as we were */

    pthread_mutex_lock(&g_mu);
    atomic_store(&s->resizing, 1);
    while (atomic_load(&s->in_read)) sched_yield();

    float *o_ring = s->ring, *o_tmp = s->tmp, *o_pre = s->pre;
    uint32_t *o_rpos = s->rpos;
    s->ring = ring; s->rpos = rpos; s->tmp = tmp; s->pre = pre;
    apply_tier_params(s, tier);
    atomic_store_explicit(&s->pre_valid, false, memory_order_release);
    build_preroll(s);                                   /* synchronous, so the start is instant again */
    atomic_store(&s->resizing, 0);
    pthread_mutex_unlock(&g_mu);

    free(o_ring); free(o_rpos); free(o_tmp); free(o_pre);
}

/* UI thread, once per second: a clip that has not played for over IDLE_FREE_SEC gives back its read buffer (1.5-3 MB).
 * Otherwise memory only grows with every clip ever played. On the next start the buffer is allocated again,
 * and the start is still instant (the preroll stays). */
void streams_trim_idle(double idle_sec)
{
    double now = now_s();
    pthread_mutex_lock(&g_mu);                       /* the disk thread does not work on the streams while we hold g_mu */
    for (size_t i = 0; i < g_n; ++i) {
        ClipStream *s = g_list[i];
        if (atomic_load_explicit(&s->active, memory_order_acquire)) { s->idle_since = 0.0; continue; }
        if (!s->ring) continue;
        if (s->idle_since <= 0.0) { s->idle_since = now; if (idle_sec > 0.0) continue; }
        if (now - s->idle_since < idle_sec) continue;
        atomic_store(&s->resizing, 1);               /* audio thread: silence instead of reading */
        while (atomic_load(&s->in_read)) sched_yield();
        if (!atomic_load_explicit(&s->active, memory_order_acquire)) {
            free(s->ring); free(s->rpos); free(s->tmp);
            s->ring = NULL; s->rpos = NULL; s->tmp = NULL;
        }
        atomic_store(&s->resizing, 0);
        s->idle_since = 0.0;
    }
    pthread_mutex_unlock(&g_mu);
}

bool stream_start(ClipStream *s, size_t from)
{
    if (!s) return false;
    s->idle_since = 0.0;
    /* has the buffer tier changed (another disk/auto raise/manual setting)? Change it while the stream is silent. */
    {
        int want = desired_tier(s);
        if (want != s->tier && !atomic_load_explicit(&s->active, memory_order_acquire)) retier(s, want);
    }
    if (!s->ring) {                 /* first start: allocate the memory (UI thread) */
        s->ring = calloc(s->ring_frames * 2, sizeof(float));
        s->rpos = calloc(s->ring_frames, sizeof(uint32_t));
        s->tmp  = calloc(s->chunk * 2, sizeof(float));
        if (!s->ring || !s->rpos || !s->tmp) {
            free(s->ring); free(s->rpos); free(s->tmp);
            s->ring = NULL; s->rpos = NULL; s->tmp = NULL;
            return false;
        }
    }
    Eff r = get_eff(s);
    if (from == STREAM_FROM_START) from = r.start;
    if (from > s->total) from = s->total;

    /* the loop/markers have changed since the preroll was built -> rebuild it now (otherwise the first
     * transition would have no crossfade or the start would wait for the disk) */
    if (!atomic_load_explicit(&s->active, memory_order_acquire) &&
        atomic_load_explicit(&s->pre_valid, memory_order_acquire) && !eff_eq(&s->pre_eff, &r)) {
        pthread_mutex_lock(&g_mu);
        build_preroll(s);
        pthread_mutex_unlock(&g_mu);
    }

    size_t pre = 0;
    if (atomic_load_explicit(&s->pre_valid, memory_order_acquire)) {
        size_t ps = s->pre_start, pn = s->pre_n;
        if (atomic_load_explicit(&s->pre_valid, memory_order_acquire)) {
            if (ps == from && pn > 0 && eff_eq(&s->pre_eff, &r)) {
                size_t lim = r.end > from ? r.end - from : 0;
                pre = pn < lim ? pn : lim;
            } else if (ps != r.start) {
                stream_invalidate_preroll(s);   /* start_marker has moved */
            }
        }
    }

    atomic_store_explicit(&s->req_from, from, memory_order_relaxed);
    atomic_store_explicit(&s->req_pre, pre, memory_order_relaxed);
    atomic_store_explicit(&s->finished, false, memory_order_relaxed);
    atomic_store_explicit(&s->play_pos, from, memory_order_relaxed);
    atomic_store_explicit(&s->active, true, memory_order_release);
    atomic_fetch_add_explicit(&s->req_epoch, 1, memory_order_release);
    return true;
}

void streams_lock(void)   { pthread_mutex_lock(&g_mu); }
void streams_unlock(void) { pthread_mutex_unlock(&g_mu); }
void stream_rebind_nolock(ClipStream *s, StreamRegionFn fn, void *ud)
{
    if (!s) return;
    s->region_fn = fn;
    s->region_ud = ud;
}

void stream_rebind(ClipStream *s, StreamRegionFn fn, void *ud)
{
    if (!s) return;
    pthread_mutex_lock(&g_mu);      /* the disk thread holds g_mu while it reads the region */
    s->region_fn = fn;
    s->region_ud = ud;
    pthread_mutex_unlock(&g_mu);
}

/* ---------- audio thread ---------- */

void stream_stop(ClipStream *s)
{
    if (s) atomic_store_explicit(&s->active, false, memory_order_release);
}

size_t stream_position(const ClipStream *s)
{
    return s ? atomic_load_explicit(&((ClipStream *)s)->play_pos, memory_order_relaxed) : 0;
}

bool stream_finished(const ClipStream *s)
{
    return s ? atomic_load_explicit(&((ClipStream *)s)->finished, memory_order_acquire) : true;
}

unsigned stream_underruns(const ClipStream *s)
{
    return s ? atomic_load_explicit(&((ClipStream *)s)->underruns, memory_order_relaxed) : 0;
}

static size_t stream_read_impl(ClipStream *s, float *out, size_t n)
{
    size_t got = 0;
    unsigned e = atomic_load_explicit(&s->req_epoch, memory_order_acquire);

    if (e != s->c_epoch) {                      /* new start / seek */
        s->c_epoch    = e;
        s->c_pre_from = atomic_load_explicit(&s->req_from, memory_order_relaxed);
        s->c_pre_n    = atomic_load_explicit(&s->req_pre,  memory_order_relaxed);
        s->c_pre_pos  = 0;
        s->c_ring_got = 0;
        s->c_synced   = false;
        atomic_store_explicit(&s->finished, false, memory_order_relaxed);
    }

    if (e != 0 && !atomic_load_explicit(&s->finished, memory_order_relaxed)) {
        /* 1) preroll from RAM */
        if (s->c_pre_pos < s->c_pre_n) {
            size_t k = s->c_pre_n - s->c_pre_pos;
            if (k > n) k = n;
            memcpy(out, s->pre + s->c_pre_pos * 2, k * 2 * sizeof(float));
            s->c_pre_pos += k;
            got += k;
            atomic_store_explicit(&s->play_pos, s->c_pre_from + s->c_pre_pos,
                                  memory_order_relaxed);
        }

        /* 2) ring buffer (NULL = the buffer was freed for an unused clip - e.g. a late stretch after stop: silence) */
        if (got < n && s->c_pre_pos >= s->c_pre_n && s->ring && s->rpos) {
            bool eof = atomic_load_explicit(&s->eof_epoch, memory_order_acquire) == e;

            if (!s->c_synced &&
                atomic_load_explicit(&s->ring_epoch, memory_order_acquire) == e) {
                size_t fl = atomic_load_explicit(&s->flush_wr, memory_order_relaxed);
                atomic_store_explicit(&s->rd, fl, memory_order_release);
                s->c_synced = true;
            }

            if (s->c_synced) {
                size_t rd = atomic_load_explicit(&s->rd, memory_order_relaxed);
                size_t wr = atomic_load_explicit(&s->wr, memory_order_acquire);
                size_t avail = wr > rd ? wr - rd : 0;
                size_t k = n - got;
                if (k > avail) k = avail;

                size_t at = rd & s->ring_mask;
                size_t first = s->ring_frames - at;
                if (first > k) first = k;
                memcpy(out + got * 2, s->ring + at * 2, first * 2 * sizeof(float));
                if (k > first)
                    memcpy(out + (got + first) * 2, s->ring,
                           (k - first) * 2 * sizeof(float));
                rd += k;
                got += k;
                s->c_ring_got += k;
                /* the position is computed BEFORE we release the slots (otherwise the disk thread
                 * may overwrite rpos[] while we read it) */
                if (k > 0) {
                    size_t pp = (rd < wr) ? (size_t)s->rpos[rd & s->ring_mask]
                                          : (size_t)s->rpos[(rd - 1) & s->ring_mask] + 1;
                    atomic_store_explicit(&s->play_pos, pp, memory_order_relaxed);
                }
                atomic_store_explicit(&s->rd, rd, memory_order_release);
                if (eof && rd >= wr && got < n)
                    atomic_store_explicit(&s->finished, true, memory_order_release);
            }
            if (got < n && !atomic_load_explicit(&s->finished, memory_order_relaxed)) {
                atomic_fetch_add_explicit(&s->underruns, 1, memory_order_relaxed);
                /* a "real" running dry: there was a start (preroll) or it was already flowing from the ring, and the end of the file was not reached.
                 * A delay on a seek (no preroll) does not count - a bigger buffer does not fix that. */
                if (!eof && (s->c_pre_n > 0 || s->c_ring_got >= 4096))
                    atomic_fetch_add_explicit(&s->starved, 1, memory_order_relaxed);
            }
        }
    }

    if (got < n) memset(out + got * 2, 0, (n - got) * 2 * sizeof(float));
    return got;
}

size_t stream_read(ClipStream *s, float *out, size_t n)
{
    /* Dekker handshake with retier(): either the audio thread sees resizing and stays silent, or retier waits for us to leave */
    atomic_store(&s->in_read, 1);
    if (atomic_load(&s->resizing)) {
        atomic_store(&s->in_read, 0);
        memset(out, 0, n * 2 * sizeof(float));
        return 0;
    }
    size_t got = stream_read_impl(s, out, n);
    atomic_store_explicit(&s->in_read, 0, memory_order_release);
    return got;
}

/* ---------- settings and report ---------- */

void streams_set_disk_mode(int mode)
{
    atomic_store(&g_mode, (mode >= 1 && mode <= 3) ? mode : 0);
}

int streams_get_disk_mode(void) { return atomic_load(&g_mode); }

unsigned long streams_starved_total(void) { return atomic_load(&g_starved_total); }

void streams_status_text(char *buf, size_t n)
{
    if (!buf || n == 0) return;
    buf[0] = 0;
    size_t used = 0;
    pthread_mutex_lock(&g_dev_mu);
    for (int i = 0; i < g_ndevs && used + 40 < n; ++i) {
        int t = class_tier(g_devs[i].cls) + g_devs[i].bump;
        if (t > 2) t = 2;
        int w = snprintf(buf + used, n - used, "%s%s: %s", used ? ", " : "", diskinfo_class_name(g_devs[i].cls), tier_name(t));
        if (w < 0) break;
        used += (size_t)w;
    }
    pthread_mutex_unlock(&g_dev_mu);
    if (!used) snprintf(buf, n, "no clips loaded yet");
}

int stream_tier(const ClipStream *s) { return s ? s->tier : 0; }
size_t stream_ring_frames(const ClipStream *s) { return s ? s->ring_frames : 0; }
