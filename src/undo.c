/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _POSIX_C_SOURCE 200809L
#include "undo.h"
#include "project.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_UNDO        100
#define MAX_UNDO_BYTES  (96u * 1024u * 1024u)
#define SETTLE_SECONDS  0.12      /* wait a bit after the last touch */
#define POLL_SECONDS    2.0       /* and to be safe: a check at least once every 2 s */

typedef struct {
    char  *snap[MAX_UNDO];
    int    n;
} Stack;

static Stack  s_undo, s_redo;
static char  *s_cur;                 /* the last snapshot (for undo) */
static uint64_t s_saved_hash;        /* hash of the full snapshot at the last save */
static bool   s_dirty;
static bool   s_plug_dirty;                   /* step 35: settings in a plugin window */
static bool   s_pending;
static double s_last_poke;
static double s_last_check;

static uint64_t fnv(const char *p)
{
    uint64_t h = 1469598103934665603ull;
    for (; *p; ++p) { h ^= (unsigned char)*p; h *= 1099511628211ull; }
    return h;
}

static size_t stack_bytes(const Stack *st)
{
    size_t t = 0;
    for (int i = 0; i < st->n; ++i) t += strlen(st->snap[i]);
    return t;
}

static void stack_clear(Stack *st)
{
    for (int i = 0; i < st->n; ++i) free(st->snap[i]);
    st->n = 0;
}

static void stack_push(Stack *st, char *snap)
{
    if (st->n == MAX_UNDO) {
        free(st->snap[0]);
        memmove(&st->snap[0], &st->snap[1], (MAX_UNDO - 1) * sizeof(char *));
        st->n--;
    }
    st->snap[st->n++] = snap;
    while (st->n > 1 && stack_bytes(st) > MAX_UNDO_BYTES) {
        free(st->snap[0]);
        memmove(&st->snap[0], &st->snap[1], (size_t)(st->n - 1) * sizeof(char *));
        st->n--;
    }
}

static char *stack_pop(Stack *st) { return st->n ? st->snap[--st->n] : NULL; }

static void refresh_dirty(Session *s)
{
    char *full = project_snapshot(s, 1);
    if (!full) return;
    s_dirty = fnv(full) != s_saved_hash;
    free(full);
}

void undo_reset(Session *s)
{
    stack_clear(&s_undo);
    stack_clear(&s_redo);
    free(s_cur);
    s_cur = project_snapshot(s, 0);
    char *full = project_snapshot(s, 1);
    s_saved_hash = full ? fnv(full) : 0;
    free(full);
    s_dirty = false;
    s_plug_dirty = false;
    s_pending = false;
}

void undo_mark_saved(Session *s)
{
    char *full = project_snapshot(s, 1);
    s_saved_hash = full ? fnv(full) : 0;
    free(full);
    s_dirty = false;
    s_plug_dirty = false;
}

void undo_poke(double now) { s_pending = true; s_last_poke = now; }

void undo_commit_now(Session *s)
{
    char *u = project_snapshot(s, 0);
    if (!u) return;
    if (!s_cur) {
        s_cur = u;
    } else if (strcmp(u, s_cur) != 0) {
        stack_push(&s_undo, s_cur);
        stack_clear(&s_redo);
        s_cur = u;
    } else {
        free(u);
    }
    refresh_dirty(s);
    s_pending = false;
}

void undo_update(Session *s, double now, bool mouse_down)
{
    if (mouse_down) return;
    if (!s_pending && now - s_last_check < POLL_SECONDS) return;
    if (s_pending && now - s_last_poke < SETTLE_SECONDS) return;
    s_last_check = now;
    undo_commit_now(s);
}

bool undo_can_undo(void) { return s_undo.n > 0; }
bool undo_can_redo(void) { return s_redo.n > 0; }
bool undo_is_dirty(void) { return s_dirty || s_plug_dirty; }
void undo_mark_plugin_changed(void) { s_plug_dirty = true; }
int  undo_depth(void) { return s_undo.n; }
int  undo_redo_depth(void) { return s_redo.n; }

void undo_shutdown(void)
{
    stack_clear(&s_undo);
    stack_clear(&s_redo);
    free(s_cur);
    s_cur = NULL;
}

/* ================= Applying a snapshot ================= */

typedef struct {
    bool  present, note, loop;
    char  path[1024], name[NAME_LEN];
    size_t ls, le, sm, em;
    int   bind, color;
    float bpm_fix, bpm_play;
    bool  warp;
    char *edits;            /* step 27: the edit_* lines (malloc; NULL = not cut) */
    size_t edits_len;
} SClip;

typedef struct {
    char  name[NAME_LEN];
    int   color, cc, pan_cc, mbind, sbind;
    RouteMask rl, rr;
    int   nfx;
    ChFx  fx[MAX_CH_FX];
} SCh;

typedef struct {
    int   nch, nsc;
    RouteMask mrl, mrr;
    float tempo;
    SCh   ch[MAX_CHANNELS];
    SClip *clip;            /* nch * nsc */
} Model;

#define MC(m, c, sc) ((m)->clip[(size_t)(c) * (size_t)(m)->nsc + (size_t)(sc)])

static bool kv(const char *line, const char *key, const char **val)
{
    size_t k = strlen(key);
    if (strncmp(line, key, k) != 0 || line[k] != '=') return false;
    *val = line + k + 1;
    return true;
}

static void copy_str(char *dst, size_t size, const char *src)
{
    size_t n = strlen(src);
    if (n >= size) n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void chomp(char *s) { size_t n = strlen(s); while (n && (s[n-1] == '\n' || s[n-1] == '\r')) s[--n] = 0; }

static bool parse_model(const char *snap, Model *m)
{
    memset(m, 0, sizeof(*m));
    /* first: the sizes */
    const char *p = snap;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > 0 && len < 64) {
            char tmp[64]; memcpy(tmp, p, len); tmp[len] = 0; const char *v;
            if (kv(tmp, "channel_count", &v)) m->nch = atoi(v);
            else if (kv(tmp, "scene_count", &v)) m->nsc = atoi(v);
        }
        if (!e) break;
        p = e + 1;
        if (m->nch && m->nsc && p > snap + 400) break;
    }
    if (m->nch < 0 || m->nch > MAX_CHANNELS || m->nsc < 0 || m->nsc > MAX_SCENES) return false;
    for (int c = 0; c < m->nch; ++c) { m->ch[c].cc = -1; m->ch[c].pan_cc = -1; m->ch[c].rl = ROUTE_DEFAULT_L; m->ch[c].rr = ROUTE_DEFAULT_R; m->ch[c].color = c % 8; }
    m->mrl = MASTER_ROUTE_DEFAULT_L; m->mrr = MASTER_ROUTE_DEFAULT_R;
    if (m->nch && m->nsc) {
        m->clip = calloc((size_t)m->nch * (size_t)m->nsc, sizeof(SClip));
        if (!m->clip) return false;
    }

    int cc_ = -1, cs_ = -1; bool in_clip = false, in_ch = false;
    p = snap;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[2048];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len); line[len] = 0; chomp(line);
        p = e ? e + 1 : p + len;
        if (!line[0] || line[0] == '#') { if (!e) break; continue; }
        const char *v;
        if (line[0] == '[') {
            in_clip = in_ch = false; cc_ = cs_ = -1;
            if (strncmp(line, "[channel ", 9) == 0) {
                cc_ = atoi(line + 9);
                in_ch = cc_ >= 0 && cc_ < m->nch;
            } else if (strncmp(line, "[clip ", 6) == 0) {
                if (sscanf(line + 6, "%d %d", &cc_, &cs_) == 2 && cc_ >= 0 && cc_ < m->nch && cs_ >= 0 && cs_ < m->nsc) {
                    in_clip = true;
                    MC(m, cc_, cs_).present = true;
                }
            }
        } else if (in_clip) {
            SClip *k = &MC(m, cc_, cs_);
            if (kv(line, "type", &v)) k->note = strcmp(v, "note") == 0;
            else if (kv(line, "path", &v)) copy_str(k->path, sizeof(k->path), v);
            else if (kv(line, "name", &v)) copy_str(k->name, sizeof(k->name), v);
            else if (kv(line, "loop", &v)) k->loop = atoi(v) != 0;
            else if (kv(line, "loop_start", &v)) k->ls = (size_t)atoll(v);
            else if (kv(line, "loop_end", &v)) k->le = (size_t)atoll(v);
            else if (kv(line, "start_marker", &v)) k->sm = (size_t)atoll(v);
            else if (kv(line, "end_marker", &v)) k->em = (size_t)atoll(v);
            else if (kv(line, "bind", &v)) k->bind = atoi(v);
            else if (kv(line, "color", &v)) k->color = atoi(v);
            else if (kv(line, "bpm_fix", &v)) k->bpm_fix = (float)atof(v);
            else if (kv(line, "bpm_play", &v)) k->bpm_play = (float)atof(v);
            else if (kv(line, "warp", &v)) k->warp = atoi(v) != 0;
            else if (strncmp(line, "edit_len=", 9) == 0 || strncmp(line, "edit_seg=", 9) == 0) {
                size_t ll = strlen(line);
                char *ne = realloc(k->edits, k->edits_len + ll + 2);
                if (ne) {
                    k->edits = ne;
                    memcpy(k->edits + k->edits_len, line, ll);
                    k->edits_len += ll;
                    k->edits[k->edits_len++] = '\n';
                    k->edits[k->edits_len] = 0;
                }
            }
        } else if (in_ch) {
            SCh *h = &m->ch[cc_];
            if (kv(line, "name", &v)) copy_str(h->name, sizeof(h->name), v);
            else if (kv(line, "color_id", &v)) h->color = atoi(v) % 8;
            else if (kv(line, "color", &v)) { int cv = atoi(v); if (cv >= 0 && cv < N_PALETTE) h->color = cv; }
            else if (kv(line, "midi_cc", &v)) h->cc = atoi(v);
            else if (kv(line, "pan_midi_cc", &v)) h->pan_cc = atoi(v);
            else if (kv(line, "route_l", &v)) h->rl = (RouteMask)strtoull(v, NULL, 10);
            else if (kv(line, "route_r", &v)) h->rr = (RouteMask)strtoull(v, NULL, 10);
            else if (kv(line, "mute_bind", &v)) h->mbind = atoi(v);
            else if (kv(line, "solo_bind", &v)) h->sbind = atoi(v);
            else if (kv(line, "fx", &v) && h->nfx < MAX_CH_FX) {
                char tmp[1024];
                copy_str(tmp, sizeof tmp, v);
                char *b = strchr(tmp, '|'), *c = b ? strchr(b + 1, '|') : NULL, *d = c ? strchr(c + 1, '|') : NULL;
                if (b && c && d) {
                    *b = *c = *d = 0;
                    ChFx *f = &h->fx[h->nfx++];
                    memset(f, 0, sizeof *f);
                    copy_str(f->fmt, sizeof f->fmt, tmp);
                    f->bypass = atoi(b + 1) != 0;
                    copy_str(f->name, sizeof f->name, c + 1);
                    copy_str(f->path, sizeof f->path, d + 1);
                }
            }
            else if (kv(line, "fxref", &v) && h->nfx > 0) h->fx[h->nfx - 1].state_ref = atoi(v);
        } else {
            if (kv(line, "master_route_l", &v)) m->mrl = (RouteMask)strtoull(v, NULL, 10);
            else if (kv(line, "master_route_r", &v)) m->mrr = (RouteMask)strtoull(v, NULL, 10);
            else if (kv(line, "tempo", &v)) m->tempo = (float)atof(v);
        }
        if (!e) break;
    }
    return true;
}

/* Does a session cell match a model cell (by content: file/note + name)? */
static bool cell_same_cur_snap(Session *s, int c, int sc, const SClip *k)
{
    Clip *cl = &s->channels[c].clips[sc];
    bool has = atomic_load(&cl->has_clip);
    if (has != k->present) return false;
    if (!has) return true;
    bool note = atomic_load(&cl->is_note);
    if (note != k->note) return false;
    if (note) return strcmp(cl->name, k->name) == 0;
    return strcmp(cl->path, k->path) == 0 && strcmp(cl->name, k->name) == 0;
}

/* Does channel c of the session match channel cm of the model (all cells up to nsc)? */
static bool chan_same(Session *s, int c, const Model *m, int cm)
{
    if (strcmp(s->channels[c].name, m->ch[cm].name) != 0) return false;
    int n = s->scene_count < m->nsc ? s->scene_count : m->nsc;
    for (int sc = 0; sc < n; ++sc)
        if (!cell_same_cur_snap(s, c, sc, &MC(m, cm, sc))) return false;
    return true;
}

static bool scene_same(Session *s, int sc, const Model *m, int sm)
{
    int n = s->channel_count < m->nch ? s->channel_count : m->nch;
    for (int c = 0; c < n; ++c)
        if (!cell_same_cur_snap(s, c, sc, &MC(m, c, sm))) return false;
    return true;
}

static void apply_cell(Session *s, int c, int sc, const SClip *k)
{
    Clip *cl = &s->channels[c].clips[sc];
    if (!k->present) {
        if (atomic_load(&cl->has_clip)) session_clear_clip(s, c, sc);
        session_set_clip_bind(s, c, sc, 0);
        return;
    }
    if (cell_same_cur_snap(s, c, sc, k)) {
        if (k->note) {
            if (session_get_note_color(s, c, sc) != k->color) session_set_note_color(s, c, sc, k->color);
        } else {
            session_clip_set_edits_text(s, c, sc, k->edits);     /* the parts - before the markers */
            bool on; size_t ls, le, sm, em;
            session_get_loop_state(s, c, sc, &on, &ls, &le);
            session_get_markers(s, c, sc, &sm, &em);
            if (ls != k->ls || le != k->le) session_set_loop_region(s, c, sc, k->ls, k->le);
            if (on != k->loop) session_toggle_loop(s, c, sc);
            if (sm != k->sm) session_set_start_marker(s, c, sc, k->sm);
            if (em != k->em) session_set_end_marker(s, c, sc, k->em);
        }
    } else {
        bool same_file = atomic_load(&cl->has_clip) && !atomic_load(&cl->is_note) && !k->note
                         && strcmp(cl->path, k->path) == 0;
        if (same_file) {                       /* only name/markers — the file stays loaded */
            if (strcmp(cl->name, k->name) != 0 && k->name[0]) session_rename_clip(s, c, sc, k->name);
            session_clip_set_edits_text(s, c, sc, k->edits);
            bool on; size_t ls, le, sm, em;
            session_get_loop_state(s, c, sc, &on, &ls, &le);
            session_get_markers(s, c, sc, &sm, &em);
            if (ls != k->ls || le != k->le) session_set_loop_region(s, c, sc, k->ls, k->le);
            if (on != k->loop) session_toggle_loop(s, c, sc);
            if (sm != k->sm) session_set_start_marker(s, c, sc, k->sm);
            if (em != k->em) session_set_end_marker(s, c, sc, k->em);
        } else {
            if (atomic_load(&cl->has_clip)) session_clear_clip(s, c, sc);
            project_load_clip_cell(s, c, sc, k->note, k->path, k->name, k->loop, k->ls, k->le, k->sm, k->em, k->edits);
            if (k->note) session_set_note_color(s, c, sc, k->color);
        }
    }
    if (!k->note) {                            /* the clip tempo (the detected one is not in the snapshot -> it stays) */
        session_set_clip_bpm_fields(s, c, sc, 0.0f, k->bpm_fix, k->bpm_play);
        if (session_get_clip_warp(s, c, sc) != k->warp) session_set_clip_warp(s, c, sc, k->warp);
    }
    session_set_clip_bind(s, c, sc, k->bind);
}

static void apply_model(Session *s, const Model *m)
{
    /* 1) Channels (step 36: several at once, and reordered too): find which ones were inserted/deleted/moved
     *    and move them without stopping the neighbours. A wrong guess breaks nothing - step 3 fixes the cells. */
    while (s->channel_count > m->nch) {
        int k = 0;
        while (k < m->nch && chan_same(s, k, m, k)) k++;
        int del = s->channel_count - 1;                      /* unclear which one - remove the last */
        if (k >= m->nch || (k + 1 < s->channel_count && chan_same(s, k + 1, m, k))) del = k;
        session_delete_channel(s, del);
    }
    while (s->channel_count < m->nch) {
        int k = 0;
        while (k < s->channel_count && chan_same(s, k, m, k)) k++;
        if (k < s->channel_count && !chan_same(s, k, m, k + 1)) k = s->channel_count;
        if (session_insert_channel(s, k) < 0 && session_add_channel(s, NULL) < 0) break;
    }
    for (int t = 0; t < m->nch && t < s->channel_count; ++t) {   /* reordered channels */
        if (chan_same(s, t, m, t)) continue;
        for (int j = t + 1; j < s->channel_count; ++j)
            if (chan_same(s, j, m, t) && !chan_same(s, j, m, j)) { session_move_channel(s, j, t); break; }
    }

    /* 2) The same for scenes */
    if (s->scene_count == m->nsc - 1) {
        int k = 0;
        while (k < s->scene_count && scene_same(s, k, m, k)) k++;
        bool ok = true;
        for (int sc = k; sc < s->scene_count && ok; ++sc) ok = scene_same(s, sc, m, sc + 1);
        session_insert_scene(s, ok ? k : s->scene_count);
    } else if (s->scene_count == m->nsc + 1) {
        int k = 0;
        while (k < m->nsc && scene_same(s, k, m, k)) k++;
        bool ok = true;
        for (int sc = k; sc < m->nsc && ok; ++sc) ok = scene_same(s, sc + 1, m, sc);
        session_delete_scene(s, ok ? k : s->scene_count - 1);
    }
    while (s->scene_count < m->nsc) { if (session_add_scene(s) < 0) break; }
    while (s->scene_count > m->nsc) session_delete_scene(s, s->scene_count - 1);

    /* 3) The cells and the channel settings */
    for (int c = 0; c < m->nch && c < s->channel_count; ++c) {
        Channel *ch = &s->channels[c];
        for (int sc = 0; sc < m->nsc; ++sc) apply_cell(s, c, sc, &MC(m, c, sc));
        const SCh *h = &m->ch[c];
        copy_str(ch->name, sizeof(ch->name), h->name);
        ch->color_id = h->color;
        atomic_store(&ch->midi_cc, h->cc);
        atomic_store(&ch->pan_midi_cc, h->pan_cc);
        session_route_set(s, c, 0, h->rl);
        session_route_set(s, c, 1, h->rr);
        session_set_mute_bind(s, c, h->mbind);
        session_set_solo_bind(s, c, h->sbind);
        ch->fx_count = h->nfx;
        memcpy(ch->fx, h->fx, sizeof(ChFx) * (size_t)h->nfx);
    }
    session_master_route_set(s, 0, m->mrl);
    session_master_route_set(s, 1, m->mrr);
    if (m->tempo > 0.0f) session_set_tempo(s, m->tempo);
}

static bool apply_snapshot(Session *s, const char *snap)
{
    Model m;
    bool ok = parse_model(snap, &m);
    if (ok) apply_model(s, &m);
    if (m.clip)
        for (size_t i = 0; i < (size_t)m.nch * (size_t)m.nsc; ++i) free(m.clip[i].edits);
    free(m.clip);
    return ok;
}

bool undo_undo(Session *s)
{
    undo_commit_now(s);                    /* if there is an unrecorded change — it becomes a step first */
    char *prev = stack_pop(&s_undo);
    if (!prev) return false;
    stack_push(&s_redo, s_cur);
    s_cur = prev;
    apply_snapshot(s, prev);
    char *now = project_snapshot(s, 0);    /* the real state after applying */
    if (now) { free(s_cur); s_cur = now; }
    refresh_dirty(s);
    return true;
}

bool undo_redo(Session *s)
{
    undo_commit_now(s);
    char *next = stack_pop(&s_redo);
    if (!next) return false;
    stack_push(&s_undo, s_cur);
    s_cur = next;
    apply_snapshot(s, next);
    char *now = project_snapshot(s, 0);
    if (now) { free(s_cur); s_cur = now; }
    refresh_dirty(s);
    return true;
}
