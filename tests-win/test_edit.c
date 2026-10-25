/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Test of the cutting (src/edit.c): pure functions, no sound card.
 * The source is a "sawtooth" — every frame knows where in the file it comes from, so every part is checked frame by frame. */
#include "../src/edit.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int fails = 0;
#define CHECK(c, ...) do { if (c) { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } \
                           else { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); ++fails; } } while (0)

#define SRC_LEN 200000
static float srcval(size_t f, int ch) { return (float)((f % 1000) + 1) / 1001.0f * (ch ? -1.0f : 1.0f); }
static size_t rd(void *ctx, size_t pos, float *out, size_t n)
{
    (void)ctx;
    size_t got = 0;
    for (; got < n && pos + got < SRC_LEN; ++got) {
        out[got * 2] = srcval(pos + got, 0);
        out[got * 2 + 1] = srcval(pos + got, 1);
    }
    return got;
}

/* The expected value for frame t according to the list (with edit_gain) */
static void expect(const EditList *e, int64_t t, float *l, float *r)
{
    int i = edit_find(e, t);
    if (i < 0 || t >= e->len) { *l = *r = 0.0f; return; }
    int64_t sf = e->seg[i].src + (t - e->seg[i].t);
    float g = edit_gain(e, i, t - e->seg[i].t) * edit_xfade_base_gain(e, t);
    *l = sf < SRC_LEN ? srcval((size_t)sf, 0) * g : 0.0f;
    *r = sf < SRC_LEN ? srcval((size_t)sf, 1) * g : 0.0f;
    int64_t xs; float xg;                                   /* crossfade: the other sound too */
    if (edit_xfade_extra(e, t, &xs, &xg) && xs >= 0 && xs < SRC_LEN) {
        *l += srcval((size_t)xs, 0) * xg;
        *r += srcval((size_t)xs, 1) * xg;
    }
}

/* reads the whole clip in chunks of awkward sizes and compares */
static int compare_all(const EditList *e, double *maxerr)
{
    static float buf[1237 * 2];
    int bad = 0;
    *maxerr = 0;
    for (int64_t pos = 0; pos < e->len + 500; ) {
        size_t n = 1 + (size_t)((pos * 7919) % 1237);
        edit_read(e, rd, NULL, pos, buf, n);
        for (size_t k = 0; k < n; ++k) {
            float l, r;
            expect(e, pos + (int64_t)k, &l, &r);
            double d = fabs(buf[k * 2] - l) + fabs(buf[k * 2 + 1] - r);
            if (d > *maxerr) *maxerr = d;
            if (d > 1e-6) ++bad;
        }
        pos += (int64_t)n;
    }
    return bad;
}

int main(void)
{
    double me;
    EditList *e = edit_new(SRC_LEN);
    CHECK(edit_is_identity(e) && edit_valid(e), "new clip = one part over the whole file");

    /* 1) uncut: bit-exact like the file */
    {
        static float b[4096 * 2];
        edit_read(e, rd, NULL, 12345, b, 4096);
        int same = 1;
        for (int k = 0; k < 4096; ++k) if (b[k * 2] != srcval(12345 + k, 0) || b[k * 2 + 1] != srcval(12345 + k, 1)) same = 0;
        CHECK(same, "uncut clip plays bit-exact like the file");
    }

    /* 2) cut, but nothing moved -> still bit-exact (no micro-fades at the cut) */
    EditList *s1 = edit_split(e, 50000);
    EditList *s2 = edit_split(s1, 120000);
    {
        static float b[100000 * 2];
        edit_read(s2, rd, NULL, 20000, b, 100000);
        int same = 1;
        for (int k = 0; k < 100000; ++k) if (b[k * 2] != srcval(20000 + k, 0)) same = 0;
        CHECK(s2->n == 3 && same, "split in 3 without moving: still bit-exact across the cuts");
    }
    CHECK(edit_split(s2, 50000) == NULL, "splitting exactly on a cut does nothing");

    /* 3) deleting the middle part -> silence there, smooth fade at the edges */
    EditList *d = edit_delete(s2, 1);
    {
        float l, r;
        expect(d, 80000, &l, &r);
        static float b[200 * 2];
        edit_read(d, rd, NULL, 50000 - 100, b, 200);
        float last = fabsf(b[99 * 2]), before = fabsf(b[0]);
        CHECK(l == 0.0f && r == 0.0f && last < 0.01f && before > 0.0f && compare_all(d, &me) == 0,
              "delete middle part: silence there, edge fades to %.4f (no click), all frames as expected", last);
    }

    /* 4) moving: the third part goes to 60000 (covers the silence) */
    EditList *m = edit_move(d, 1, 60000);
    {
        int i = edit_find(m, 70000);
        CHECK(m && edit_valid(m) && i >= 0 && edit_map(m, 70000) == 130000 && compare_all(m, &me) == 0,
              "move a part: plays the right part of the file at the new place (max err %.2g)", me);
    }
    /* onto another part -> covers it */
    EditList *m2 = edit_move(m, 1, 30000);
    CHECK(m2 && edit_valid(m2) && edit_map(m2, 30000) == 120000 && edit_map(m2, 29999) == 29999 && compare_all(m2, &me) == 0,
          "move a part on top of another: it covers what is under it");
    /* after the end -> the clip gets longer */
    EditList *m3 = edit_move(m, 0, 180000);
    CHECK(m3 && m3->len == SRC_LEN && edit_valid(m3) && edit_map(m3, 199999) == 19999 && compare_all(m3, &me) == 0,
          "move past the end: what goes out of the clip is not played, clip length stays");
    EditList *m4 = edit_move(e, 0, -50000);
    CHECK(m4 && edit_valid(m4) && edit_map(m4, 0) == 50000 && edit_map(m4, 150000) < 0 && compare_all(m4, &me) == 0,
          "move the whole clip left past its start: plays from later in the file, silence at the end");
    /* edges: inwards cuts, outwards reveals the hidden part, stops at the start/end of the file */
    {
        EditList *t1 = edit_trim(s2, 1, 0, 60000);                 /* the middle part starts later */
        EditList *t2 = t1 ? edit_trim(t1, 1, 0, 0) : NULL;         /* pull outwards all the way: stops at frame 0 of the file... */
        int i2 = t2 ? edit_find(t2, 10000) : -1;
        CHECK(t1 && edit_map(t1, 55000) == -1 && edit_map(t1, 60000) == 60000 && t2 && i2 >= 0 && edit_map(t2, 10000) == 10000 &&
              edit_valid(t2) && compare_all(t2, &me) == 0, "trim edges: inward shortens, outward shows the hidden sound again");
        EditList *mv = edit_move(e, 0, 30000);                     /* the whole file from 30000 */
        EditList *t3 = mv ? edit_trim(mv, 0, 0, 0) : NULL;         /* outwards to the left: the file starts at 30000 -> stops there */
        EditList *t3b = mv ? edit_trim(mv, 0, 0, 40000) : NULL;     /* inwards is possible */
        CHECK(t3 == NULL && t3b && t3b->seg[0].t == 40000 && t3b->seg[0].src == 10000, "trim outward stops at the start of the file");
        edit_free(t3b);
        EditList *t4 = edit_trim(s2, 2, 1, SRC_LEN + 50000);
        CHECK(t4 == NULL, "trim outward stops at the end of the file / clip");
        edit_free(t1); edit_free(t2); edit_free(mv); edit_free(t3); edit_free(t4);
    }
    /* copying (Ctrl + drag) */
    {
        EditList *cp = edit_copy(s2, 0, 150000);
        CHECK(cp && edit_map(cp, 10000) == 10000 && edit_map(cp, 160000) == 10000 && edit_valid(cp) && compare_all(cp, &me) == 0,
              "copy a part: the original stays, the copy plays the same sound");
        edit_free(cp);
    }
    /* fades by dragging the corners: lengths from the ends of the part, kept when the edge is trimmed */
    {
        EditList *f = edit_set_fades(s2, 1, 4000, 8000);
        int64_t fi, fo;
        edit_seg_fades(&f->seg[1], &fi, &fo);
        float g0 = edit_gain(f, 1, 0), gin = edit_gain(f, 1, 4000), gout = edit_gain(f, 1, f->seg[1].len - 1);
        EditList *tr = edit_trim(f, 1, 0, 55000);
        int64_t fi2, fo2;
        edit_seg_fades(&tr->seg[edit_find(tr, 60000)], &fi2, &fo2);
        CHECK(fi == 4000 && fo == 8000 && g0 < 0.001f && gin > 0.999f && gout < 0.001f && fi2 == 4000 && fo2 == 8000 &&
              compare_all(f, &me) == 0, "corner fades: fade in 4000 / out 8000, kept after trimming the edge");
        EditList *big = edit_set_fades(s2, 1, 60000, 60000);
        edit_seg_fades(&big->seg[1], &fi, &fo);
        CHECK(fi + fo <= big->seg[1].len, "fades longer than the part are shrunk to fit");
        edit_free(f); edit_free(tr); edit_free(big);
    }

    /* 5) crop */
    EditList *c = edit_crop(e, 30000, 40000);
    CHECK(c && c->len == 10000 && edit_map(c, 0) == 30000 && edit_map(c, 9999) == 39999 && compare_all(c, &me) == 0,
          "keep only the selection: 10000 frames, starts at the selection");
    {
        static float b[64 * 2];
        edit_read(c, rd, NULL, 0, b, 64);
        CHECK(fabsf(b[0]) < 0.01f, "crop starts with a micro fade-in (first sample %.5f)", b[0]);
    }

    /* 6) fade in / out over a marked range */
    EditList *fi = edit_fade(e, 10000, 20000, +1);
    EditList *fo = edit_fade(fi, 150000, 170000, -1);
    {
        float a, b2, c2, x, y;
        expect(fo, 10000, &a, &x);
        expect(fo, 15000, &b2, &x);
        expect(fo, 19999, &c2, &x);
        float g0 = edit_gain(fo, edit_find(fo, 10000), 0);
        float gm = edit_gain(fo, edit_find(fo, 15000), 15000 - fo->seg[edit_find(fo, 15000)].t);
        float g1 = edit_gain(fo, edit_find(fo, 19999), 19999 - fo->seg[edit_find(fo, 19999)].t);
        float go = edit_gain(fo, edit_find(fo, 169999), 169999 - fo->seg[edit_find(fo, 169999)].t);
        float after = edit_gain(fo, edit_find(fo, 175000), 175000 - fo->seg[edit_find(fo, 175000)].t);
        (void)a; (void)b2; (void)c2; (void)y;
        CHECK(g0 < 0.001f && fabsf(gm - 0.7071f) < 0.01f && g1 > 0.999f, "fade in: 0 -> -3 dB in the middle -> full (%.4f %.4f %.4f)", g0, gm, g1);
        CHECK(go < 0.001f && after > 0.999f && compare_all(fo, &me) == 0, "fade out: silent at the end of the selection, after it the sound goes on");
        /* no clicks at the boundaries: before a fade in the sound fades out (micro), after a fade out it fades in smoothly */
        float pre = edit_gain(fo, edit_find(fo, 9999), 9999 - fo->seg[edit_find(fo, 9999)].t);
        float post = edit_gain(fo, edit_find(fo, 170000), 0);
        CHECK(pre < 0.01f && post < 0.01f, "no click at the fade edges (%.4f before fade in, %.4f after fade out)", pre, post);
        /* a fade that crosses the cut between 2 parts: the curve is one and the same */
        EditList *f2 = edit_fade(s2, 40000, 60000, +1);
        float ga = edit_gain(f2, edit_find(f2, 49999), 49999 - f2->seg[edit_find(f2, 49999)].t);
        float gb = edit_gain(f2, edit_find(f2, 50000), 50000 - f2->seg[edit_find(f2, 50000)].t);
        CHECK(f2 && fabsf(ga - gb) < 0.001f, "fade across a cut: one smooth curve (%.4f | %.4f)", ga, gb);
        edit_free(f2);
    }

    /* 7) text (project/undo) */
    {
        char *t = edit_to_text(m2);
        EditList *back = edit_from_text(t, SRC_LEN);
        int same = back && back->n == m2->n && back->len == m2->len &&
                   memcmp(back->seg, m2->seg, (size_t)m2->n * sizeof(EditSeg)) == 0;
        CHECK(same, "save / load of the parts is exact");
        EditList *shorter = edit_from_text(t, 100000);
        CHECK(shorter && edit_valid(shorter), "file got shorter: parts are trimmed, still valid");
        CHECK(edit_from_text("garbage\nedit_seg=1,2\n", SRC_LEN) == NULL, "broken text is rejected");
        free(t); edit_free(back); edit_free(shorter);
    }

    /* 8) sample rate change: cuts without moves stay a "continuation" (no micro-fades) */
    {
        EditList *k = edit_scale(s2, 44100.0 / 48000.0, (int64_t)llround(SRC_LEN * 44100.0 / 48000.0));
        int ok = k && edit_valid(k) && k->n == 3;
        for (int i = 1; ok && i < k->n; ++i)
            ok = k->seg[i].t == k->seg[i - 1].t + k->seg[i - 1].len && k->seg[i].src == k->seg[i - 1].src + k->seg[i - 1].len;
        CHECK(ok, "sample-rate change keeps plain cuts seamless");
        edit_free(k);
    }

    /* 9) random operations: the list is always valid and the audio is always what it should be */
    {
        srand(7);
        EditList *cur = edit_new(SRC_LEN);
        int ops = 0, invalid = 0, wrong = 0;
        for (int it = 0; it < 8000; ++it) {
            EditList *nx = NULL;
            int64_t t = rand() % (cur->len + 1);
            switch (rand() % 10) {
            case 9: if (cur->n > 1) { int k = 1 + rand() % (cur->n - 1);
                        nx = (rand() & 1) ? edit_set_xfade(cur, k, rand() % 8000, 0) : edit_set_xfade(cur, k, rand() % 3000, rand() % 6000); }
                    break;
            case 6: if (cur->n) nx = edit_trim(cur, rand() % cur->n, rand() & 1, rand() % (cur->len + 1)); break;
            case 7: if (cur->n) nx = edit_copy(cur, rand() % cur->n, rand() % (cur->len + 1) - 20000); break;
            case 8: if (cur->n) nx = edit_set_fades(cur, rand() % cur->n, rand() % 20000, rand() % 20000); break;
            case 0: nx = edit_split(cur, t); break;
            case 1: if (cur->n > 3) nx = edit_delete(cur, rand() % cur->n); break;
            case 2: if (cur->n) nx = edit_move(cur, rand() % cur->n, rand() % (cur->len + 40000) - 20000); break;
            case 3: nx = edit_fade(cur, t, t + rand() % 30000, (rand() & 1) ? 1 : -1); break;
            case 4: if (rand() % 30 == 0 && cur->len > 150000) nx = edit_crop(cur, t / 4, t / 4 + 120000); break;
            case 5: if (cur->len > 400000) nx = edit_crop(cur, 0, 200000);
                    else if (cur->n < 2 || cur->len < 100000) nx = edit_new(SRC_LEN);      /* from the start */
                    break;
            }
            if (!nx) continue;
            ++ops;
            if (!edit_valid(nx)) ++invalid;
            if (it % 150 == 0 && compare_all(nx, &me) != 0) { ++wrong; printf("   wrong after op at it=%d\n", it); }
            if (!edit_valid(nx)) printf("   invalid at it=%d\n", it);
            edit_free(cur);
            cur = nx;
        }
        CHECK(ops > 1500 && invalid == 0 && wrong == 0, "8000 random edits (%d applied): always valid, sound always as expected", ops);
        edit_free(cur);
    }

    {   /* ===== seam crossfade (step 28b) ===== */
        EditList *b0 = edit_new(SRC_LEN);
        EditList *b1 = edit_split(b0, 50000), *b2 = edit_split(b1, 80000);
        EditList *b3 = edit_delete(b2, 1);                    /* 50000..80000 = silence */
        EditList *b4 = edit_move(b3, 1, 50000);               /* the right one is butted against it: seam at 50000 (file 50000 | 80000) */
        CHECK(b4 && b4->n == 2 && edit_joined(b4, 1) && b4->seg[1].src == 80000, "two parts glued (seam at 50000)");
        EditList *xl = edit_set_xfade(b4, 1, 4000, 0);        /* dragging left: the right one's earlier audio mixes with the left one */
        float buf[8000 * 2];
        edit_read(xl, rd, NULL, 46000, buf, 8000);
        double me = 0, pw = 0;
        for (int q = 0; q < 8000; ++q) {
            int64_t t = 46000 + q;
            float L;
            if (t < 50000) {
                double x = ((double)(t - 46000) + 0.5) / 4000.0 * M_PI / 2;
                L = srcval((size_t)t, 0) * (float)cos(x) + srcval((size_t)(80000 - (50000 - t)), 0) * (float)sin(x);
                if (q == 2000) pw = cos(x) * cos(x) + sin(x) * sin(x);
            } else L = srcval((size_t)(80000 + (t - 50000)), 0);
            double d = fabs(buf[q * 2] - L);
            if (d > me) me = d;
        }
        CHECK(xl && me < 1e-5 && fabs(pw - 1.0) < 1e-9, "crossfade to the left: both sounds mixed, equal power (err %.2g)", me);
        EditList *xr = edit_set_xfade(b4, 1, 0, 3000);        /* right: the left one continues and mixes with the right one */
        edit_read(xr, rd, NULL, 49000, buf, 6000);
        me = 0;
        for (int q = 0; q < 6000; ++q) {
            int64_t t = 49000 + q;
            float L;
            if (t < 50000) L = srcval((size_t)t, 0);
            else if (t < 53000) {
                double x = ((double)(t - 50000) + 0.5) / 3000.0 * M_PI / 2;
                L = srcval((size_t)(80000 + (t - 50000)), 0) * (float)sin(x) + srcval((size_t)t, 0) * (float)cos(x);
            } else L = srcval((size_t)(80000 + (t - 50000)), 0);
            double d = fabs(buf[q * 2] - L);
            if (d > me) me = d;
        }
        CHECK(xr && me < 1e-5, "crossfade to the right: the left part continues into the right one (err %.2g)", me);
        EditList *big = edit_set_xfade(b4, 1, 999999, 999999);
        CHECK(big && edit_valid(big) && big->seg[1].xl == 50000 && big->seg[1].xr == 120000, "too long crossfade is limited (xl %lld, xr %lld)", (long long)big->seg[1].xl, (long long)big->seg[1].xr);
        char *tx = edit_to_text(xl);
        EditList *back = edit_from_text(tx, SRC_LEN);
        CHECK(back && back->seg[1].xl == 4000 && back->seg[1].xr == 0, "crossfade saved in the project text");
        EditList *old = edit_from_text("edit_len=200000\nedit_seg=0,200000,0,0,0,0,0\n", SRC_LEN);
        CHECK(old && old->n == 1 && old->seg[0].xl == 0, "old project text (7 numbers) still loads");
        EditList *away = edit_move(xl, 1, 60000);
        CHECK(away && away->seg[1].xl == 0 && away->seg[1].xr == 0, "moving a part away removes its crossfade");
        CHECK(edit_set_xfade(b3, 1, 1000, 0) == NULL, "no crossfade where parts are not glued");
        double me2;
        CHECK(compare_all(xl, &me2) == 0 && compare_all(xr, &me2) == 0, "whole clip with crossfades reads as expected");
        free(tx);
        edit_free(b0); edit_free(b1); edit_free(b2); edit_free(b3); edit_free(b4); edit_free(xl); edit_free(xr);
        edit_free(big); edit_free(back); edit_free(old); edit_free(away);
    }

    {   /* step 36j: a part outside the clip (as in Bitwig) - not deleted, only the part inside plays */
        EditList *base = edit_new(SRC_LEN);
        EditList *sp = edit_split(base, 100000);
        EditList *out = sp ? edit_move(sp, 0, -30000) : NULL;      /* the first part - 30000 before the start */
        double meo;
        CHECK(out && edit_valid(out) && out->seg[0].t == -30000 && out->seg[0].len == 100000 && out->len == SRC_LEN,
              "part moved before the start keeps its full length (not cut)");
        CHECK(out && edit_map(out, 0) == 30000 && edit_map(out, 69999) == 99999 && compare_all(out, &meo) == 0,
              "only the part inside the clip plays (from the right place)");
        EditList *back = out ? edit_move(out, 0, 0) : NULL;
        CHECK(back && back->seg[0].t == 0 && back->seg[0].len == 100000 && edit_map(back, 0) == 0,
              "dragged back: the beginning is still there");
        EditList *after = sp ? edit_move(sp, 1, SRC_LEN - 1000) : NULL;   /* the second - almost entirely after the end */
        CHECK(after && edit_valid(after) && after->seg[1].len == SRC_LEN - 100000 && compare_all(after, &meo) == 0,
              "part moved past the end keeps its length; only 1000 frames play");
        char *tx = out ? edit_to_text(out) : NULL;
        EditList *rt = tx ? edit_from_text(tx, SRC_LEN) : NULL;
        CHECK(rt && rt->n == out->n && rt->seg[0].t == -30000 && rt->len == SRC_LEN, "saved and loaded with a negative start");
        EditList *far = sp ? edit_move(sp, 0, -5 * SRC_LEN) : NULL;
        CHECK(far == NULL, "not further out than one clip length");
        EditList *tr = out ? edit_trim(out, 0, 1, 150000) : NULL;
        CHECK(tr && edit_valid(tr), "trimming a part that sticks out works");
        free(tx);
        edit_free(base); edit_free(sp); edit_free(out); edit_free(back); edit_free(after); edit_free(rt); edit_free(far); edit_free(tr);
    }

    {   /* step 36k: copying/pasting a part, extending the clip */
        EditList *base = edit_new(SRC_LEN);
        EditList *a1 = edit_split(base, 50000);
        EditList *a2 = a1 ? edit_split(a1, 80000) : NULL;          /* parts: [0,50000) [50000,80000) [80000,..) */
        EditList *cp = a2 ? edit_extract(a2, 1) : NULL;
        CHECK(cp && cp->n == 1 && cp->len == 30000 && cp->seg[0].t == 0 && cp->seg[0].src == 50000, "copy of a part");
        EditList *ps = cp ? edit_paste(a2, cp, 10000) : NULL;      /* onto the first part */
        double mep;
        CHECK(ps && edit_valid(ps) && edit_map(ps, 10000) == 50000 && edit_map(ps, 39999) == 79999 &&
              edit_map(ps, 9999) == 9999 && edit_map(ps, 40000) == 40000 && compare_all(ps, &mep) == 0,
              "paste replaces only the place it takes (before and after stay)");
        EditList *lg = edit_set_len(a2, SRC_LEN + 40000);
        CHECK(lg && lg->len == SRC_LEN + 40000 && edit_map(lg, SRC_LEN + 100) == -1 && compare_all(lg, &mep) == 0,
              "clip made longer: silence after the old end");
        EditList *pe = (lg && cp) ? edit_paste(lg, cp, SRC_LEN + 5000) : NULL;
        CHECK(pe && edit_map(pe, SRC_LEN + 5000) == 50000 && compare_all(pe, &mep) == 0, "paste into the new (longer) part");
        edit_free(base); edit_free(a1); edit_free(a2); edit_free(cp); edit_free(ps); edit_free(lg); edit_free(pe);
    }

    edit_free(e); edit_free(s1); edit_free(s2); edit_free(d); edit_free(m); edit_free(m2); edit_free(m3); edit_free(m4);
    edit_free(c); edit_free(fi); edit_free(fo);
    printf(fails ? "EDIT: %d FAILED\n" : "EDIT: all ok\n", fails);
    return fails != 0;
}
