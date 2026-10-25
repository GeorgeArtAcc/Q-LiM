/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Cutting a clip without changing the file — see edit.h */
#define _POSIX_C_SOURCE 200809L
#include "edit.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static EditList *alloc_list(int cap)
{
    if (cap < 1) cap = 1;
    EditList *e = calloc(1, sizeof(EditList) + (size_t)cap * sizeof(EditSeg));
    return e;
}

EditList *edit_new(int64_t src_len)
{
    EditList *e = alloc_list(1);
    if (!e) return NULL;
    e->len = src_len > 0 ? src_len : 0;
    e->src_len = e->len;
    if (e->len > 0) {
        e->n = 1;
        e->seg[0] = (EditSeg){ .t = 0, .len = e->len, .src = 0 };
    }
    return e;
}

static EditList *clone_cap(const EditList *e, int extra)
{
    EditList *c = alloc_list(e->n + extra);
    if (!c) return NULL;
    c->n = e->n; c->len = e->len; c->src_len = e->src_len;
    memcpy(c->seg, e->seg, (size_t)e->n * sizeof(EditSeg));
    return c;
}

EditList *edit_clone(const EditList *e) { return e ? clone_cap(e, 0) : NULL; }
void edit_free(EditList *e) { free(e); }

static bool has_fades(const EditSeg *s) { return s->fi1 > s->fi0 || s->fo1 > s->fo0; }

/* The crossfades after every change: only for butted parts, within the file's bounds and without overlapping. */
static EditList *fix_xf(EditList *c)
{
    if (!c) return NULL;
    /* a part left ENTIRELY beyond the limit (e.g. another one was placed over its visible portion) - it is removed */
    {
        int64_t m = c->len > 0 ? c->len : 1, k = 0;
        for (int i = 0; i < c->n; ++i)
            if (!(c->seg[i].t + c->seg[i].len <= -m || c->seg[i].t >= c->len + m)) c->seg[k++] = c->seg[i];
        c->n = (int)k;
    }
    for (int i = 0; i < c->n; ++i) {
        EditSeg *s = &c->seg[i];
        if (s->xl < 0) s->xl = 0;
        if (s->xr < 0) s->xr = 0;
        if (i == 0 || c->seg[i - 1].t + c->seg[i - 1].len != s->t) { s->xl = s->xr = 0; continue; }
        const EditSeg *p = &c->seg[i - 1];
        int64_t pstart = p->t + p->xr;                     /* the zone of the previous seam ends here */
        int64_t ml = s->src < s->t - pstart ? s->src : s->t - pstart;
        if (ml < 0) ml = 0;
        if (s->xl > ml) s->xl = ml;
        int64_t mr = c->src_len - (p->src + p->len);
        if (mr > s->len) mr = s->len;
        if (mr < 0) mr = 0;
        if (s->xr > mr) s->xr = mr;
    }
    return c;
}

bool edit_is_identity(const EditList *e)
{
    if (!e) return true;
    if (e->len != e->src_len) return false;
    if (e->n != 1) return e->len == 0 && e->n == 0;
    const EditSeg *s = &e->seg[0];
    return s->t == 0 && s->src == 0 && s->len == e->len && !has_fades(s);
}

/* the first part that ends after t (it may start after t) */
static int first_after(const EditList *e, int64_t t)
{
    int lo = 0, hi = e->n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (e->seg[mid].t + e->seg[mid].len <= t) lo = mid + 1; else hi = mid;
    }
    return lo;
}

int edit_find(const EditList *e, int64_t t)
{
    if (!e) return -1;                                   /* step 36j: parts may also lie before 0 / after the end */
    int i = first_after(e, t);
    if (i < e->n && e->seg[i].t <= t) return i;
    return -1;
}

int64_t edit_map(const EditList *e, int64_t t)
{
    int i = edit_find(e, t);
    return i < 0 ? -1 : e->seg[i].src + (t - e->seg[i].t);
}

/* part trimmed on the left by cut frames (the local fade coordinates shift) */
static EditSeg cut_front(EditSeg s, int64_t cut)
{
    s.t += cut; s.src += cut; s.len -= cut;
    s.fi0 -= cut; s.fi1 -= cut; s.fo0 -= cut; s.fo1 -= cut;
    s.xl = s.xr = 0;                                     /* new left edge -> no seam with the previous one */
    return s;
}

/* Removes everything in [a, b) (cuts the parts that cross it). out must have room for e->n + 1. */
static void carve_into(const EditList *e, int64_t a, int64_t b, EditList *out)
{
    out->n = 0;
    out->len = e->len; out->src_len = e->src_len;
    for (int i = 0; i < e->n; ++i) {
        EditSeg s = e->seg[i];
        int64_t se = s.t + s.len;
        if (se <= a || s.t >= b) { out->seg[out->n++] = s; continue; }
        if (s.t < a) { EditSeg l = s; l.len = a - s.t; out->seg[out->n++] = l; }
        if (se > b) out->seg[out->n++] = cut_front(s, b - s.t);
    }
}

static void insert_sorted(EditList *e, EditSeg s)
{
    int i = e->n;
    while (i > 0 && e->seg[i - 1].t > s.t) { e->seg[i] = e->seg[i - 1]; --i; }
    e->seg[i] = s;
    e->n++;
}

EditList *edit_split(const EditList *e, int64_t t)
{
    int i = edit_find(e, t);
    if (i < 0 || t <= e->seg[i].t || e->n >= EDIT_MAX_SEGS) return NULL;
    EditList *c = clone_cap(e, 1);
    if (!c) return NULL;
    EditSeg s = e->seg[i];
    EditSeg l = s; l.len = t - s.t;
    EditSeg r = cut_front(s, t - s.t);
    memmove(&c->seg[i + 2], &c->seg[i + 1], (size_t)(e->n - i - 1) * sizeof(EditSeg));
    c->seg[i] = l;
    c->seg[i + 1] = r;
    c->n = e->n + 1;
    return fix_xf(c);
}

EditList *edit_delete(const EditList *e, int idx)
{
    if (!e || idx < 0 || idx >= e->n) return NULL;
    EditList *c = clone_cap(e, 0);
    if (!c) return NULL;
    memmove(&c->seg[idx], &c->seg[idx + 1], (size_t)(e->n - idx - 1) * sizeof(EditSeg));
    c->n--;
    if (idx < c->n) c->seg[idx].xl = c->seg[idx].xr = 0;   /* the next one loses its neighbour */
    return fix_xf(c);
}

/* The fade lengths of a part (fade in from its start, fade out to its end) */
void edit_seg_fades(const EditSeg *s, int64_t *fin, int64_t *fout)
{
    *fin = s->fi1 > s->fi0 && s->fi1 > 0 ? s->fi1 : 0;
    *fout = s->fo1 > s->fo0 && s->fo0 < s->len ? s->len - s->fo0 : 0;
    if (*fin > s->len) *fin = s->len;
    if (*fout > s->len) *fout = s->len;
}

static void put_fades(EditSeg *s, int64_t fin, int64_t fout)
{
    if (fin < 0) fin = 0;
    if (fout < 0) fout = 0;
    if (fin + fout > s->len) {                            /* they do not fit: shrink both proportionally */
        double k = (double)s->len / (double)(fin + fout);
        fin = (int64_t)(fin * k); fout = (int64_t)(fout * k);
    }
    s->fi0 = 0; s->fi1 = fin;
    s->fo0 = s->len - fout; s->fo1 = fout > 0 ? s->len : s->fo0;
    if (fin == 0) s->fi1 = 0;
}

/* Step 36j (as in Bitwig): a part may also sit OUTSIDE the clip (before 0 / after the end) - it is not cut, only
 * the portion inside plays. How far out: at most one clip length (as much as is visible at the farthest zoom). */
static int64_t out_margin(const EditList *e) { return e->len > 0 ? e->len : 1; }

/* Puts part s in its place: removes whatever is underneath. */
static EditList *place(const EditList *rest, EditSeg s)
{
    int64_t m = out_margin(rest);
    if (s.t + s.len <= -m || s.t >= rest->len + m) return NULL;   /* completely outside the visible range */
    if (s.len <= 0) return NULL;
    EditList *c = alloc_list(rest->n + 2);
    if (!c) return NULL;
    carve_into(rest, s.t, s.t + s.len, c);
    if (c->n >= EDIT_MAX_SEGS) { free(c); return NULL; }
    insert_sorted(c, s);
    return fix_xf(c);
}

/* after a move/copy: the new place has no seams (neither with the previous one, nor the next one with it) */
static EditList *unjoin_at(EditList *c, int64_t t)
{
    if (!c) return NULL;
    for (int i = 0; i < c->n; ++i)
        if (c->seg[i].t == t) {
            c->seg[i].xl = c->seg[i].xr = 0;
            if (i + 1 < c->n) c->seg[i + 1].xl = c->seg[i + 1].xr = 0;
            break;
        }
    return c;
}

static EditList *without(const EditList *e, int idx)
{
    EditList *r = clone_cap(e, 0);
    if (!r) return NULL;
    memmove(&r->seg[idx], &r->seg[idx + 1], (size_t)(e->n - idx - 1) * sizeof(EditSeg));
    r->n--;
    return r;
}

EditList *edit_move(const EditList *e, int idx, int64_t new_t)
{
    if (!e || idx < 0 || idx >= e->n) return NULL;
    EditSeg s = e->seg[idx];
    if (new_t == s.t) return NULL;
    EditList *rest = without(e, idx);
    if (!rest) return NULL;
    if (idx < rest->n) rest->seg[idx].xl = rest->seg[idx].xr = 0;   /* the vacated place: the neighbour loses the seam */
    s.t = new_t;
    s.xl = s.xr = 0;
    EditList *c = place(rest, s);
    free(rest);
    if (!c) return NULL;
    return fix_xf(unjoin_at(c, new_t));
}

EditList *edit_copy(const EditList *e, int idx, int64_t new_t)
{
    if (!e || idx < 0 || idx >= e->n || new_t == e->seg[idx].t) return NULL;
    EditSeg s = e->seg[idx];
    s.t = new_t;
    s.xl = s.xr = 0;
    EditList *c = place(e, s);
    if (!c) return NULL;
    return fix_xf(unjoin_at(c, new_t));
}

EditList *edit_trim(const EditList *e, int idx, int side, int64_t edge)
{
    if (!e || idx < 0 || idx >= e->n) return NULL;
    EditSeg s = e->seg[idx];
    int64_t fin, fout;
    edit_seg_fades(&s, &fin, &fout);
    const int64_t MIN = 16;
    if (side == 0) {                                      /* left edge: inwards = shorter, outwards = reveals the old audio */
        int64_t lo = s.t - s.src;                         /* there is no file further left (outside the clip - allowed, step 36j) */
        int64_t hi = s.t + s.len - MIN;
        if (hi < lo) return NULL;                         /* the part is shorter than the minimum and touches the start of the file */
        if (edge < lo) edge = lo;
        if (edge > hi) edge = hi;
        if (edge == s.t) return NULL;
        int64_t d = edge - s.t;
        s.t = edge; s.src += d; s.len -= d;
    } else {
        int64_t hi = s.t + (e->src_len - s.src);          /* there is no file further right */
        int64_t lo = s.t + MIN;
        if (hi < lo) return NULL;                         /* the same, at the end of the file */
        if (edge > hi) edge = hi;
        if (edge < lo) edge = lo;
        if (edge == s.t + s.len) return NULL;
        s.len = edge - s.t;
    }
    if (s.len <= 0) return NULL;
    put_fades(&s, fin, fout);
    EditList *rest = without(e, idx);
    if (!rest) return NULL;
    EditList *c = place(rest, s);
    free(rest);
    return c;
}

/* step 36k: copy of a part (for pasting): a list with length = the part, the part is at 0 */
EditList *edit_extract(const EditList *e, int idx)
{
    if (!e || idx < 0 || idx >= e->n) return NULL;
    EditList *c = alloc_list(1);
    if (!c) return NULL;
    EditSeg s = e->seg[idx];
    s.t = 0; s.xl = s.xr = 0;
    c->n = 1; c->len = s.len; c->src_len = e->src_len;
    c->seg[0] = s;
    return c;
}

/* step 36k: pastes clip at t - replaces everything in [t, t + clip->len) */
EditList *edit_paste(const EditList *e, const EditList *clip, int64_t t)
{
    if (!e || !clip || clip->n <= 0 || clip->len <= 0) return NULL;
    if (e->n + clip->n + 2 > EDIT_MAX_SEGS) return NULL;
    int64_t m = out_margin(e);
    if (t + clip->len <= -m || t >= e->len + m) return NULL;
    EditList *c = alloc_list(e->n + clip->n + 2);
    if (!c) return NULL;
    carve_into(e, t, t + clip->len, c);
    for (int i = 0; i < clip->n; ++i) {
        EditSeg s = clip->seg[i];
        s.t += t;
        if (s.src < 0 || s.src + s.len > e->src_len) continue;   /* a different (shorter) file - not possible */
        insert_sorted(c, s);
        unjoin_at(c, s.t);
    }
    return fix_xf(c);
}

/* step 36k: new clip length (e.g. the End marker was dragged past the end: the clip gets longer, silence at the back) */
EditList *edit_set_len(const EditList *e, int64_t len)
{
    if (!e || len < 2 || len == e->len) return NULL;
    EditList *c = edit_clone(e);
    if (!c) return NULL;
    c->len = len;
    if (!edit_valid(c)) { free(c); return NULL; }
    return fix_xf(c);
}

EditList *edit_set_fades(const EditList *e, int idx, int64_t fin, int64_t fout)
{
    if (!e || idx < 0 || idx >= e->n) return NULL;
    EditList *c = edit_clone(e);
    if (!c) return NULL;
    put_fades(&c->seg[idx], fin, fout);
    return c;
}

bool edit_joined(const EditList *e, int idx)
{
    return e && idx > 0 && idx < e->n && e->seg[idx - 1].t + e->seg[idx - 1].len == e->seg[idx].t;
}

EditList *edit_set_xfade(const EditList *e, int idx, int64_t xl, int64_t xr)
{
    if (!edit_joined(e, idx)) return NULL;
    EditList *c = edit_clone(e);
    if (!c) return NULL;
    c->seg[idx].xl = xl; c->seg[idx].xr = xr;
    fix_xf(c);
    if (c->seg[idx].xl == e->seg[idx].xl && c->seg[idx].xr == e->seg[idx].xr) { free(c); return NULL; }
    return c;
}

/* the crossfade (if any) around t: seam k, the start and length of the zone */
static int xf_at(const EditList *e, int64_t t, int64_t *z0, int64_t *zl)
{
    int i = first_after(e, t);                            /* the part containing t or the next one */
    for (int k = i; k <= i + 1 && k < e->n; ++k) {
        if (k < 1) continue;
        const EditSeg *s = &e->seg[k];
        if (s->xl + s->xr <= 0) continue;
        int64_t a = s->t - s->xl, b = s->t + s->xr;
        if (t >= a && t < b) { *z0 = a; *zl = s->xl + s->xr; return k; }
    }
    return -1;
}

static float xf_in(int64_t t, int64_t z0, int64_t zl)          /* the new (right) one */
{
    return (float)sin(((double)(t - z0) + 0.5) / (double)zl * M_PI * 0.5);
}
static float xf_out(int64_t t, int64_t z0, int64_t zl)         /* the old (left) one */
{
    return (float)cos(((double)(t - z0) + 0.5) / (double)zl * M_PI * 0.5);
}

float edit_xfade_base_gain(const EditList *e, int64_t t)
{
    int64_t z0, zl;
    int k = e ? xf_at(e, t, &z0, &zl) : -1;
    if (k < 0) return 1.0f;
    return t < e->seg[k].t ? xf_out(t, z0, zl) : xf_in(t, z0, zl);
}

bool edit_xfade_extra(const EditList *e, int64_t t, int64_t *src, float *gain)
{
    int64_t z0, zl;
    int k = e ? xf_at(e, t, &z0, &zl) : -1;
    if (k < 0) return false;
    const EditSeg *s = &e->seg[k], *p = &e->seg[k - 1];
    if (t < s->t) { *src = s->src - (s->t - t); *gain = xf_in(t, z0, zl); }
    else          { *src = p->src + p->len + (t - s->t); *gain = xf_out(t, z0, zl); }
    return true;
}

EditList *edit_crop(const EditList *e, int64_t a, int64_t b)
{
    if (!e) return NULL;
    if (a < 0) a = 0;
    if (b > e->len) b = e->len;
    if (b - a < 2) return NULL;
    EditList *c = alloc_list(e->n);
    if (!c) return NULL;
    c->len = b - a; c->src_len = e->src_len;
    for (int i = 0; i < e->n; ++i) {
        EditSeg s = e->seg[i];
        int64_t st = s.t > a ? s.t : a, en = s.t + s.len < b ? s.t + s.len : b;
        if (en <= st) continue;
        EditSeg k = cut_front(s, st - s.t);
        k.len = en - st;
        k.t = st - a;
        c->seg[c->n++] = k;
    }
    return fix_xf(c);
}

EditList *edit_fade(const EditList *e, int64_t a, int64_t b, int dir)
{
    if (!e || dir == 0) return NULL;
    if (a < 0) a = 0;
    if (b > e->len) b = e->len;
    if (b - a < 2) return NULL;
    EditList *c = edit_clone(e);
    if (!c) return NULL;
    EditList *t1 = edit_split(c, a);
    if (t1) { free(c); c = t1; }
    EditList *t2 = edit_split(c, b);
    if (t2) { free(c); c = t2; }
    int hit = 0;
    for (int i = 0; i < c->n; ++i) {
        EditSeg *s = &c->seg[i];
        if (s->t < a || s->t + s->len > b) continue;
        if (dir > 0) { s->fi0 = a - s->t; s->fi1 = b - s->t; }
        else         { s->fo0 = a - s->t; s->fo1 = b - s->t; }
        ++hit;
    }
    if (!hit) { free(c); return NULL; }
    return fix_xf(c);
}

EditList *edit_scale(const EditList *e, double k, int64_t new_src_len)
{
    if (!e || k <= 0.0) return NULL;
    EditList *c = alloc_list(e->n);
    if (!c) return NULL;
    #define SC(v) ((int64_t)llround((double)(v) * k))
    c->len = SC(e->len);
    c->src_len = new_src_len;
    for (int i = 0; i < e->n; ++i) {
        const EditSeg *o = &e->seg[i];
        EditSeg s = *o;
        s.t = SC(o->t);
        s.len = SC(o->t + o->len) - s.t;
        s.src = SC(o->src);
        s.fi0 = SC(o->fi0); s.fi1 = SC(o->fi1); s.fo0 = SC(o->fo0); s.fo1 = SC(o->fo1);
        s.xl = SC(o->xl); s.xr = SC(o->xr);
        if (c->n > 0) {                                  /* continuation of the previous one -> stays an exact continuation */
            const EditSeg *po = &e->seg[i - 1];
            EditSeg *p = &c->seg[c->n - 1];
            if (po->t + po->len == o->t) s.t = p->t + p->len;
            if (po->t + po->len == o->t && po->src + po->len == o->src) s.src = p->src + p->len;
        }
        if (s.src + s.len > new_src_len) s.len = new_src_len - s.src;
        if (s.len <= 0 || s.src < 0) continue;
        c->seg[c->n++] = s;
    }
    #undef SC
    return fix_xf(c);
}

/* --- audio --- */

/* fade in that starts exactly at the start of the part / fade out that ends exactly at its end */
static bool fade_in_at_start(const EditSeg *s) { return s->fi1 > s->fi0 && s->fi0 >= 0; }
static bool fade_out_at_end(const EditSeg *s)  { return s->fo1 > s->fo0 && s->fo1 <= s->len; }

/* "Continuation": the audio passes from the previous part into this one without a jump -> no micro-fade.
 * There is a jump if the file does not continue, OR if one side has a fade (the level there is ~0, and full on the other side). */
static bool cont_start(const EditList *e, int i)
{
    const EditSeg *s = &e->seg[i];
    if (fade_in_at_start(s)) return true;                /* starts from 0 anyway */
    if (s->xl + s->xr > 0) return true;                  /* crossfade with the previous one - no micro-fade */
    if (i > 0) {
        const EditSeg *p = &e->seg[i - 1];
        if (p->t + p->len == s->t) {
            if (fade_out_at_end(p)) return false;        /* the previous one ends in silence, this one starts loud */
            if (p->src + p->len == s->src) return true;
        }
    }
    return s->src == 0;                                  /* the start of the file is a natural start */
}

static bool cont_end(const EditList *e, int i)
{
    const EditSeg *s = &e->seg[i];
    if (fade_out_at_end(s)) return true;                 /* ends at 0 anyway */
    if (i + 1 < e->n && e->seg[i + 1].xl + e->seg[i + 1].xr > 0) return true;
    if (i + 1 < e->n) {
        const EditSeg *q = &e->seg[i + 1];
        if (s->t + s->len == q->t) {
            if (fade_in_at_start(q)) return false;       /* the next one starts from silence */
            if (s->src + s->len == q->src) return true;
        }
    }
    return s->src + s->len >= e->src_len;                /* the end of the file is a natural end */
}

static float fade_curve_in(double x)       /* 0..1 -> 0..1, equal power */
{
    if (x <= 0.0) return 0.0f;
    if (x >= 1.0) return 1.0f;
    return (float)sin(x * M_PI * 0.5);
}

static float gain_at(const EditSeg *s, int64_t p, bool cs, bool ce, int64_t micro)
{
    float g = 1.0f;
    if (s->fi1 > s->fi0) g *= fade_curve_in(((double)(p - s->fi0) + 0.5) / (double)(s->fi1 - s->fi0));
    if (s->fo1 > s->fo0) g *= fade_curve_in(1.0 - ((double)(p - s->fo0) + 0.5) / (double)(s->fo1 - s->fo0));
    if (micro > 0) {
        if (!cs && p < micro) g *= 0.5f - 0.5f * (float)cos(((double)p + 0.5) / (double)micro * M_PI);
        int64_t q = s->len - 1 - p;
        if (!ce && q < micro) g *= 0.5f - 0.5f * (float)cos(((double)q + 0.5) / (double)micro * M_PI);
    }
    return g;
}

float edit_gain(const EditList *e, int idx, int64_t p)
{
    if (!e || idx < 0 || idx >= e->n) return 0.0f;
    const EditSeg *s = &e->seg[idx];
    int64_t micro = s->len / 2 < EDIT_MICRO_FADE ? s->len / 2 : EDIT_MICRO_FADE;
    return gain_at(s, p, cont_start(e, idx), cont_end(e, idx), micro);
}

size_t edit_read(const EditList *e, EditReadFn readfn, void *ctx, int64_t pos, float *out, size_t n)
{
    if (!e || n == 0) return 0;
    if (pos < 0) pos = 0;
    if (pos >= e->len) { memset(out, 0, n * 2 * sizeof(float)); return 0; }
    size_t avail = (size_t)(e->len - pos);
    size_t want = n < avail ? n : avail;
    if (want < n) memset(out + want * 2, 0, (n - want) * 2 * sizeof(float));
    size_t done = 0;
    int i = first_after(e, pos);
    while (done < want) {
        int64_t p = pos + (int64_t)done;
        while (i < e->n && e->seg[i].t + e->seg[i].len <= p) ++i;
        if (i >= e->n || e->seg[i].t > p) {               /* empty space until the next part */
            int64_t gap_end = i < e->n ? e->seg[i].t : e->len;
            size_t cnt = (size_t)(gap_end - p);
            if (cnt > want - done) cnt = want - done;
            memset(out + done * 2, 0, cnt * 2 * sizeof(float));
            done += cnt;
            continue;
        }
        const EditSeg *s = &e->seg[i];
        int64_t lp = p - s->t;
        size_t cnt = (size_t)(s->len - lp);
        if (cnt > want - done) cnt = want - done;
        float *o = out + done * 2;
        size_t got = readfn(ctx, (size_t)(s->src + lp), o, cnt);
        if (got < cnt) memset(o + got * 2, 0, (cnt - got) * 2 * sizeof(float));
        bool cs = cont_start(e, i), ce = cont_end(e, i);
        int64_t micro = s->len / 2 < EDIT_MICRO_FADE ? s->len / 2 : EDIT_MICRO_FADE;
        if (has_fades(s) || !cs || !ce) {
            for (size_t k = 0; k < cnt; ++k) {
                int64_t q = lp + (int64_t)k;
                /* fast: the middle of a part without fades is 1.0 */
                if (!has_fades(s) && q >= micro && q < s->len - micro) continue;
                float g = gain_at(s, q, cs, ce, micro);
                o[k * 2] *= g;
                o[k * 2 + 1] *= g;
            }
        }
        done += cnt;
    }
    /* the crossfades: the main audio is multiplied by its curve and the other one is added (equal power) */
    int k0 = first_after(e, pos);
    if (k0 < 1) k0 = 1;
    int64_t end = pos + (int64_t)want;
    for (int k = k0; k < e->n; ++k) {
        const EditSeg *s = &e->seg[k];
        if (s->t - s->xl >= end) break;
        if (s->xl + s->xr <= 0) continue;
        const EditSeg *pr = &e->seg[k - 1];
        int64_t z0 = s->t - s->xl, zl = s->xl + s->xr;
        int64_t a = pos > z0 ? pos : z0, b = end < s->t + s->xr ? end : s->t + s->xr;
        float tmp[512 * 2];
        while (a < b) {
            bool pre = a < s->t;                          /* before the seam: additionally the right one's earlier audio */
            int64_t lim = pre ? (b < s->t ? b : s->t) : b;
            size_t cnt = (size_t)(lim - a);
            if (cnt > 512) cnt = 512;
            int64_t src = pre ? s->src - (s->t - a) : pr->src + pr->len + (a - s->t);
            size_t got = src >= 0 ? readfn(ctx, (size_t)src, tmp, cnt) : 0;
            if (got < cnt) memset(tmp + got * 2, 0, (cnt - got) * 2 * sizeof(float));
            float *o = out + (size_t)(a - pos) * 2;
            for (size_t q = 0; q < cnt; ++q) {
                int64_t t = a + (int64_t)q;
                float gi = xf_in(t, z0, zl), go = xf_out(t, z0, zl);
                float gb = pre ? go : gi, gx = pre ? gi : go;
                o[q * 2]     = o[q * 2] * gb + tmp[q * 2] * gx;
                o[q * 2 + 1] = o[q * 2 + 1] * gb + tmp[q * 2 + 1] * gx;
            }
            a += (int64_t)cnt;
        }
    }
    return want;
}

/* --- text --- */

char *edit_to_text(const EditList *e)
{
    if (!e) return NULL;
    size_t cap = 64 + (size_t)e->n * 230;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    size_t at = (size_t)snprintf(buf, cap, "edit_len=%lld\n", (long long)e->len);
    for (int i = 0; i < e->n && at < cap; ++i) {
        const EditSeg *s = &e->seg[i];
        if (s->xl + s->xr > 0)
            at += (size_t)snprintf(buf + at, cap - at, "edit_seg=%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n",
                                   (long long)s->t, (long long)s->len, (long long)s->src,
                                   (long long)s->fi0, (long long)s->fi1, (long long)s->fo0, (long long)s->fo1,
                                   (long long)s->xl, (long long)s->xr);
        else
            at += (size_t)snprintf(buf + at, cap - at, "edit_seg=%lld,%lld,%lld,%lld,%lld,%lld,%lld\n",
                                   (long long)s->t, (long long)s->len, (long long)s->src,
                                   (long long)s->fi0, (long long)s->fi1, (long long)s->fo0, (long long)s->fo1);
    }
    return buf;
}

bool edit_valid(const EditList *e)
{
    if (!e || e->n < 0 || e->n > EDIT_MAX_SEGS || e->len < 0) return false;
    int64_t prev_end = INT64_MIN;
    int64_t m = out_margin(e);
    for (int i = 0; i < e->n; ++i) {
        const EditSeg *s = &e->seg[i];
        if (s->len <= 0 || s->t < prev_end || s->src < 0 || s->xl < 0 || s->xr < 0) return false;
        if (s->t + s->len <= -m || s->t >= e->len + m) return false;   /* step 36j: outwards - up to one length */
        if (e->src_len > 0 && s->src + s->len > e->src_len) return false;
        prev_end = s->t + s->len;
    }
    return true;
}

EditList *edit_from_text(const char *text, int64_t src_len)
{
    if (!text) return NULL;
    int count = 0;
    for (const char *p = text; (p = strstr(p, "edit_seg=")) != NULL; p += 9) ++count;
    if (count > EDIT_MAX_SEGS) return NULL;
    EditList *e = alloc_list(count);
    if (!e) return NULL;
    e->src_len = src_len;
    e->len = -1;
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        char line[256];
        if (ll < sizeof line) {
            memcpy(line, p, ll); line[ll] = 0;
            long long v[9] = { 0 };
            if (strncmp(line, "edit_len=", 9) == 0) e->len = atoll(line + 9);
            else if (strncmp(line, "edit_seg=", 9) == 0 &&
                     sscanf(line + 9, "%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                            &v[6], &v[7], &v[8]) >= 7 &&
                     e->n < count) {
                EditSeg s = { v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8] };
                /* the file may have become shorter: cut what no longer exists */
                if (src_len > 0 && s.src + s.len > src_len) s.len = src_len - s.src;
                if (s.len > 0 && s.src >= 0) e->seg[e->n++] = s;      /* t < 0 - allowed (step 36j) */
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    if (e->len < 0) { free(e); return NULL; }
    if (!edit_valid(e)) { free(e); return NULL; }
    return fix_xf(e);
}
