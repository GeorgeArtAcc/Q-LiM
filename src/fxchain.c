/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* The channels' plugin chains - see fxchain.h */
#define _GNU_SOURCE
#include "fxchain.h"
#include "plugins.h"
#include "ui.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FX_MAX_BLOCK 1024              /* = MAX_BLOCK in mixer.c */
#define MAX_CHAINS   (MAX_CHANNELS * 2)

static FxChain *g_live[MAX_CHAINS];   /* the chains that may be in some channel */
static int      g_nlive;
typedef struct { FxChain *c; unsigned cycles; } Retired;
static Retired  g_ret[MAX_CHAINS * 2];
static int      g_nret;

static void chain_free(FxChain *c)
{
    if (!c) return;
    for (int i = 0; i < c->n; ++i) if (!c->moved[i]) qplug_free(c->p[i]);
    free(c);
}

static void retire(Session *s, FxChain *c)
{
    for (int i = 0; i < g_nlive; ++i) if (g_live[i] == c) { g_live[i] = g_live[--g_nlive]; break; }
    if (g_nret >= (int)(sizeof g_ret / sizeof g_ret[0])) {   /* full (should not happen) - wait */
        return;
    }
    g_ret[g_nret].c = c;
    g_ret[g_nret].cycles = atomic_load(&s->audio_cycles);
    ++g_nret;
}

static void collect(Session *s)
{
    bool running = atomic_load(&s->audio_running);
    unsigned now = atomic_load(&s->audio_cycles);
    for (int i = 0; i < g_nret; ) {
        if (!running || now - g_ret[i].cycles >= 2) {
            chain_free(g_ret[i].c);
            g_ret[i] = g_ret[--g_nret];
        } else {
            ++i;
        }
    }
}

static bool is_live(FxChain *c)
{
    for (int i = 0; i < g_nlive; ++i) if (g_live[i] == c) return true;
    return false;
}

static void make_sig(Session *s, Channel *ch, char *sig, size_t sz)
{
    size_t o = (size_t)snprintf(sig, sz, "%d/%d;", s->sample_rate, plugins_generation());
    for (int k = 0; k < ch->fx_count && o < sz; ++k)
        o += (size_t)snprintf(sig + o, sz - o, "%d:%s|%s|%d;", k, ch->fx[k].fmt, ch->fx[k].path, ch->fx[k].bypass ? 1 : 0);
}

static bool any_active(Channel *ch) { return ch->fx_count > 0; }   /* bypassed ones stay loaded too */

/* ---- settings store ---- */
typedef struct { unsigned char *d; size_t n; } Blob;
static Blob *g_blobs;
static int   g_nblobs;
int fxstate_new(void)
{
    Blob *nb = realloc(g_blobs, sizeof(Blob) * (size_t)(g_nblobs + 1));
    if (!nb) return 0;
    g_blobs = nb;
    g_blobs[g_nblobs].d = NULL; g_blobs[g_nblobs].n = 0;
    return ++g_nblobs;                                     /* the numbers start from 1 */
}
void fxstate_append(int ref, const void *data, size_t len)
{
    if (ref < 1 || ref > g_nblobs || !len) return;
    Blob *b = &g_blobs[ref - 1];
    unsigned char *nd = realloc(b->d, b->n + len);
    if (!nd) return;
    memcpy(nd + b->n, data, len);
    b->d = nd; b->n += len;
}
void fxstate_set(int ref, const void *data, size_t len)
{
    if (ref < 1 || ref > g_nblobs) return;
    g_blobs[ref - 1].n = 0;
    fxstate_append(ref, data, len);
}
const void *fxstate_get(int ref, size_t *len)
{
    if (ref < 1 || ref > g_nblobs) { *len = 0; return NULL; }
    *len = g_blobs[ref - 1].n;
    return g_blobs[ref - 1].n ? g_blobs[ref - 1].d : NULL;
}

/* Takes an already loaded plugin from a donor chain (without changing the donor's p[]/n: the audio thread may be
 * walking the donor right now - step 34b: that was the crash). */
static QPlug *take_from(FxChain *d, const char *fmt, const char *id)
{
    if (!d) return NULL;
    for (int i = 0; i < d->n; ++i)
        if (!d->moved[i] && !strcmp(d->id[i], id) && !strcmp(d->fmt[i], fmt)) { d->moved[i] = true; return d->p[i]; }
    return NULL;
}

/* ---- step 35d: plugins through yabridge (Wine) are loaded on a background thread (seconds) ---- */
typedef struct {
    char fmt[8], id[520], name[96];
    double rate;
    void *state; size_t state_len;
    QPlug *res;
    char err[300];
    atomic_int done;
    bool taken;
} ALoad;
#define MAX_AL 64
static ALoad *g_al[MAX_AL];
static int    g_nal;

static void *aload_thread(void *arg)
{
    ALoad *a = arg;
    a->res = qplug_load(a->fmt, a->id, a->rate, FX_MAX_BLOCK, a->err, (int)sizeof a->err);
    if (a->res && a->state) qplug_set_state(a->res, a->state, a->state_len);
    atomic_store(&a->done, 1);
    return NULL;
}

static ALoad *aload_find(const char *fmt, const char *id)
{
    for (int i = 0; i < g_nal; ++i) if (!g_al[i]->taken && !strcmp(g_al[i]->fmt, fmt) && !strcmp(g_al[i]->id, id)) return g_al[i];
    return NULL;
}

static ALoad *aload_start(Session *s, const ChFx *fx, const char *id, const char *chname)
{
    if (g_nal >= MAX_AL) return NULL;
    ALoad *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    snprintf(a->fmt, sizeof a->fmt, "%s", fx->fmt);
    snprintf(a->id, sizeof a->id, "%s", id);
    snprintf(a->name, sizeof a->name, "%s", fx->name);
    a->rate = (double)s->sample_rate;
    size_t sl = 0;
    const void *sd = fxstate_get(fx->state_ref, &sl);
    if (sd && sl) { a->state = malloc(sl); if (a->state) { memcpy(a->state, sd, sl); a->state_len = sl; } }
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, aload_thread, a) != 0) { pthread_attr_destroy(&at); free(a->state); free(a); return NULL; }
    pthread_attr_destroy(&at);
    g_al[g_nal++] = a;
    printf(">>> Plugin '%s' (%s) on channel '%s': starting it in the background (Wine/yabridge)...\n", fx->name, fx->fmt, chname);
    char msg[200];
    snprintf(msg, sizeof msg, "Starting '%s' (Windows plugin) - a few seconds...", fx->name);
    ui_flash_message(msg);
    return a;
}

static void aload_drop(int i)
{
    ALoad *a = g_al[i];
    free(a->state);
    free(a);
    g_al[i] = g_al[--g_nal];
}

/* is there a channel on which a plugin is still loading in the background (then the old chain stays) */
static bool channel_pending(Session *s, Channel *ch)
{
    (void)s;
    for (int k = 0; k < ch->fx_count; ++k) {
        char id[520];
        if (!plugins_resolve(ch->fx[k].fmt, ch->fx[k].path, ch->fx[k].name, id, sizeof id)) continue;
        ALoad *a = aload_find(ch->fx[k].fmt, id);
        if (a && !atomic_load(&a->done)) return true;
    }
    return false;
}

/* A new chain for the channel. old may be NULL; plugins from old (and from chains without a channel - e.g. after Undo)
 * that are needed again are moved - not loaded again. */
static FxChain *build(Session *s, int c, FxChain *old, const char *sig, bool *wait)
{
    Channel *ch = &s->channels[c];
    *wait = false;
    FxChain *nc = calloc(1, sizeof *nc);
    if (!nc) return NULL;
    snprintf(nc->sig, sizeof nc->sig, "%s", sig);
    bool scanning = plugins_scanning(NULL, NULL, NULL, 0);
    for (int k = 0; k < ch->fx_count; ++k) {
        const ChFx *fx = &ch->fx[k];
        char id[520];
        if (!plugins_resolve(fx->fmt, fx->path, fx->name, id, sizeof id)) {
            if (scanning) { *wait = true; continue; }     /* the list is still being built - later again */
            nc->fail_mask |= 1u << k;
            printf(">>> Plugin '%s' (%s) on channel '%s': not found - is it still installed?\n", fx->name, fx->fmt, ch->name);
            continue;
        }
        QPlug *p = take_from(old, fx->fmt, id);
        for (int d = 0; !p && d < g_nlive; ++d)            /* chains without a channel */
            if (!g_live[d]->claimed) p = take_from(g_live[d], fx->fmt, id);
        bool from_bg = false;
        char err[300] = "";
        if (!p && qplug_is_bridged(fx->fmt, id)) {               /* Windows plugin: on a background thread */
            ALoad *a = aload_find(fx->fmt, id);
            if (!a) a = aload_start(s, fx, id, ch->name);
            if (!a || !atomic_load(&a->done)) {
                *wait = true;
                nc->loading_mask |= 1u << k;
                if (a) continue;
            } else {
                a->taken = true;
                p = a->res;
                from_bg = true;
                if (!p) snprintf(err, sizeof err, "%s", a->err);
                else printf(">>> Plugin '%s' (%s) loaded on channel '%s'%s\n", fx->name, fx->fmt, ch->name,
                            a->state ? " (settings restored)" : "");
                if (!p) {
                    nc->fail_mask |= 1u << k;
                    printf(">>> Plugin '%s' (%s) on channel '%s' failed: %s\n", fx->name, fx->fmt, ch->name, err);
                    char msg[256];
                    snprintf(msg, sizeof msg, "Plugin '%s' could not start: %s", fx->name, err);
                    ui_flash_message(msg);
                    continue;
                }
            }
        }
        if (!p && !from_bg) {
            p = qplug_load(fx->fmt, id, (double)s->sample_rate, FX_MAX_BLOCK, err, (int)sizeof err);
            if (!p) {
                nc->fail_mask |= 1u << k;
                printf(">>> Plugin '%s' (%s) on channel '%s' failed: %s\n", fx->name, fx->fmt, ch->name, err);
                char msg[256];
                snprintf(msg, sizeof msg, "Plugin '%s' could not start: %s", fx->name, err);
                ui_flash_message(msg);
                continue;
            }
            size_t sl = 0;
            const void *sd = fxstate_get(fx->state_ref, &sl);
            bool st_ok = sd && qplug_set_state(p, sd, sl) == 0;
            printf(">>> Plugin '%s' (%s) loaded on channel '%s'%s\n", fx->name, fx->fmt, ch->name,
                   sd ? (st_ok ? " (settings restored)" : " (could not restore its settings)") : "");
        }
        int i = nc->n++;
        nc->p[i] = p;
        nc->byp[i] = fx->bypass;
        if (!fx->bypass) ++nc->nactive;
        nc->slot[i] = k;
        snprintf(nc->id[i], sizeof nc->id[i], "%s", id);
        snprintf(nc->fmt[i], sizeof nc->fmt[i], "%s", fx->fmt);
    }
    return nc;
}

void fxchain_sync(Session *s)
{
    clap_host_set_main_thread();
    collect(s);
    for (int i = 0; i < g_nlive; ++i) g_live[i]->claimed = false;
    int nch = atomic_load(&s->channel_count);
    /* 1) which channel uses which chain */
    for (int c = 0; c < nch; ++c) {
        Channel *ch = &s->channels[c];
        FxChain *cur = atomic_load(&ch->fxchain);
        if (cur && (!is_live(cur) || cur->claimed)) {
            /* a copy of a channel (duplicate) - the chain belongs to the other channel; this one needs its own */
            atomic_store(&ch->fxchain, NULL);
            cur = NULL;
        }
        if (cur) cur->claimed = true;
    }
    /* 2) changes (slow loads - at most ~0.15 s per frame, so drawing happens between them) */
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int c = 0; c < nch; ++c) {
        Channel *ch = &s->channels[c];
        FxChain *cur = atomic_load(&ch->fxchain);
        char sig[FXC_SIG_LEN];
        make_sig(s, ch, sig, sizeof sig);
        if (cur && !strcmp(cur->sig, sig)) continue;
        if (cur && channel_pending(s, ch)) continue;            /* Windows plugin still starting - the old chain plays */
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if ((double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9 > 0.15) break;
        if (!cur && !any_active(ch)) continue;
        if (!any_active(ch)) {                             /* all removed/bypassed */
            atomic_store(&ch->fxchain, NULL);
            retire(s, cur);
            continue;
        }
        if (g_nlive >= MAX_CHAINS) continue;
        if (plugins_scanning(NULL, NULL, NULL, 0)) {       /* old entry without a full id: wait for the list */
            bool need = false;
            char tmp[520];
            for (int k = 0; k < ch->fx_count; ++k)
                if (!ch->fx[k].bypass && !plugins_resolve(ch->fx[k].fmt, ch->fx[k].path, ch->fx[k].name, tmp, sizeof tmp)) need = true;
            if (need) continue;
        }
        bool wait;
        FxChain *nc = build(s, c, cur, sig, &wait);
        if (!nc) continue;
        if (wait) nc->sig[0] = 0;                          /* incomplete - it will try again */
        nc->claimed = true;
        g_live[g_nlive++] = nc;
        atomic_store(&ch->fxchain, nc);
        if (cur) retire(s, cur);
    }
    /* background loads that nobody waits for any more (the row was removed while it was starting) */
    for (int i = 0; i < g_nal; ) {
        ALoad *a = g_al[i];
        if (a->taken) { aload_drop(i); continue; }
        if (atomic_load(&a->done)) {
            bool wanted = false;
            for (int c = 0; c < nch && !wanted; ++c)
                for (int k = 0; k < s->channels[c].fx_count && !wanted; ++k) {
                    char id[520];
                    if (plugins_resolve(s->channels[c].fx[k].fmt, s->channels[c].fx[k].path, s->channels[c].fx[k].name, id, sizeof id) &&
                        !strcmp(id, a->id) && !strcmp(s->channels[c].fx[k].fmt, a->fmt)) wanted = true;
                }
            if (!wanted) { if (a->res) qplug_free(a->res); aload_drop(i); continue; }
        }
        ++i;
    }
    /* chains without a channel (the channel was deleted / the project was changed) */
    for (int i = 0; i < g_nlive; ) {
        if (!g_live[i]->claimed) retire(s, g_live[i]);    /* retire removes it from g_live */
        else ++i;
    }
}

static FxChain *chain_of(Session *s, int c)
{
    if (c < 0 || c >= atomic_load(&s->channel_count)) return NULL;
    FxChain *f = atomic_load(&s->channels[c].fxchain);
    return (f && is_live(f)) ? f : NULL;
}

bool fxchain_slot_failed(Session *s, int c, int k)
{
    FxChain *f = chain_of(s, c);
    return f && k >= 0 && k < 32 && (f->fail_mask & (1u << k));
}

bool fxchain_slot_loading(Session *s, int c, int k)
{
    FxChain *f = chain_of(s, c);
    if (f && k >= 0 && k < 32 && (f->loading_mask & (1u << k))) return true;
    if (c < 0 || c >= s->channel_count || k < 0 || k >= s->channels[c].fx_count) return false;
    const ChFx *fx = &s->channels[c].fx[k];
    char id[520];
    if (!plugins_resolve(fx->fmt, fx->path, fx->name, id, sizeof id)) return false;
    ALoad *a = aload_find(fx->fmt, id);
    return a && !atomic_load(&a->done);
}

bool fxchain_slot_loaded(Session *s, int c, int k)
{
    FxChain *f = chain_of(s, c);
    if (!f) return false;
    for (int i = 0; i < f->n; ++i) if (f->slot[i] == k) return true;
    return false;
}

QPlug *fxchain_find(Session *s, int c, int k)
{
    FxChain *f = chain_of(s, c);
    if (!f) return NULL;
    for (int i = 0; i < f->n; ++i) if (f->slot[i] == k && !f->moved[i]) return f->p[i];
    return NULL;
}

int fxchain_state_for_save(Session *s, int c, int k, void **data, size_t *len)
{
    *data = NULL; *len = 0;
    QPlug *p = fxchain_find(s, c, k);
    if (p && qplug_get_state(p, data, len) == 0 && *len > 0) return 0;
    if (c < 0 || c >= s->channel_count || k < 0 || k >= s->channels[c].fx_count) return -1;
    size_t n;
    const void *d = fxstate_get(s->channels[c].fx[k].state_ref, &n);
    if (!d) return -1;
    *data = malloc(n);
    if (!*data) return -1;
    memcpy(*data, d, n);
    *len = n;
    return 0;
}

void fxchain_shutdown(Session *s)
{
    for (int i = 0; i < g_nal; ) {                     /* the finished background ones - free them; the ones still starting stay */
        if (atomic_load(&g_al[i]->done) && !g_al[i]->taken && g_al[i]->res) { qplug_free(g_al[i]->res); g_al[i]->res = NULL; }
        ++i;
    }
    int nch = atomic_load(&s->channel_count);
    for (int c = 0; c < nch; ++c) atomic_store(&s->channels[c].fxchain, NULL);
    for (int i = 0; i < g_nlive; ++i) chain_free(g_live[i]);
    g_nlive = 0;
    for (int i = 0; i < g_nret; ++i) chain_free(g_ret[i].c);
    g_nret = 0;
}
