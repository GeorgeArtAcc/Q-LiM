/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef EDIT_H
#define EDIT_H
/*
 * Cutting a clip WITHOUT touching the file (step 27).
 *
 * The clip has a "timeline". Parts (segments) lie on it: every part says
 * "from frame t of the timeline, len frames, play the file from frame src". Where there is no part — silence.
 * An uncut clip = one part covering the whole file (then the audio is bit-exact like the file).
 *
 * All operations are PURE functions: they return a NEW list (they do not touch the old one). So the UI prepares the new
 * list calmly, then swaps it in at once (see session_clip_apply_edits), and the disk thread always reads
 * a whole, unchanging list.
 *
 * Smooth transitions (no clicks):
 *   - at every "cut" (where the audio jumps) there is a micro-fade of EDIT_MICRO_FADE frames;
 *     where two neighbouring parts continue the file into each other (simply cut, not moved) — nothing;
 *   - fade in / fade out over a marked range: an equal-power curve (quarter sine).
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define EDIT_MAX_SEGS    2048
#define EDIT_MICRO_FADE  64          /* ~1.3 ms @ 48 kHz */

typedef struct {
    int64_t t;          /* start on the timeline */
    int64_t len;        /* length (> 0) */
    int64_t src;        /* where in the file it comes from */
    int64_t fi0, fi1;   /* fade in: from fi0 to fi1 (frames RELATIVE to the start of the part; may be outside it); fi1 <= fi0 = none */
    int64_t fo0, fo1;   /* fade out: from fo0 to fo1 (the same) */
    /* Crossfade with the PREVIOUS part (step 28b, as in Bitwig) - only if it ends exactly where this one starts.
     * The "seam" stays at t; around it the two are mixed (equal power):
     *   xl = how many frames BEFORE t this part's earlier audio (the file before src) also sounds, while the previous one fades out;
     *   xr = how many frames AFTER t the previous part (the file after its end) also continues, while this one fades in. */
    int64_t xl, xr;
} EditSeg;

typedef struct {
    int     n;
    int64_t len;        /* length of the timeline (= the clip length) */
    int64_t src_len;    /* length of the file */
    EditSeg seg[];      /* sorted by t, not overlapping */
} EditList;

EditList *edit_new(int64_t src_len);               /* one part = the whole file */
EditList *edit_clone(const EditList *e);
void      edit_free(EditList *e);
bool      edit_is_identity(const EditList *e);      /* nothing has been touched */

int       edit_find(const EditList *e, int64_t t);  /* the part that contains t; -1 = empty space */
int64_t   edit_map(const EditList *e, int64_t t);   /* the file frame for t; -1 = silence */

/* Operations: return a new list, or NULL if there is nothing to do (then the old one stays). */
EditList *edit_split(const EditList *e, int64_t t);                 /* cuts at t */
EditList *edit_delete(const EditList *e, int idx);                   /* removes a part (silence remains) */
EditList *edit_move(const EditList *e, int idx, int64_t new_t);      /* moves a part; covers whatever is underneath; may lie outside the clip (up to one clip length) */
EditList *edit_copy(const EditList *e, int idx, int64_t new_t);      /* like move, but the original stays (Ctrl + drag) */
/* dragging an edge: side 0 = left, 1 = right; edge = the new position of the edge. Outwards the old audio is revealed
 * (up to the start/end of the file), the fades keep their length. */
EditList *edit_trim(const EditList *e, int idx, int side, int64_t edge);
EditList *edit_set_fades(const EditList *e, int idx, int64_t fin, int64_t fout);
EditList *edit_extract(const EditList *e, int idx);                  /* step 36k: copy of a part (at 0, len = the part) */
EditList *edit_paste(const EditList *e, const EditList *clip, int64_t t);   /* pastes at t; replaces [t, t+len) */
EditList *edit_set_len(const EditList *e, int64_t len);              /* new clip length */  /* fade in / fade out lengths */
/* Crossfade at the seam between part idx and the previous one (they must be butted). Limited to what is possible. */
EditList *edit_set_xfade(const EditList *e, int idx, int64_t xl, int64_t xr);
bool      edit_joined(const EditList *e, int idx);   /* part idx is butted against the previous one (there is a seam) */
/* For the picture: in a crossfade zone - the level of the main audio at t (1 outside the zones) and the additional audio
 * (file frame + level). false = t is not in a crossfade. */
float     edit_xfade_base_gain(const EditList *e, int64_t t);
bool      edit_xfade_extra(const EditList *e, int64_t t, int64_t *src, float *gain);
void      edit_seg_fades(const EditSeg *s, int64_t *fin, int64_t *fout);
EditList *edit_crop(const EditList *e, int64_t a, int64_t b);        /* only [a, b) remains, it becomes the start */
EditList *edit_fade(const EditList *e, int64_t a, int64_t b, int dir);   /* dir > 0: fade in, < 0: fade out */
EditList *edit_scale(const EditList *e, double k, int64_t new_src_len);  /* sample rate change */

/* Reading: n stereo frames (L R L R ...) from position pos of the timeline. readfn reads from the file. */
typedef size_t (*EditReadFn)(void *ctx, size_t pos, float *out, size_t n);
size_t    edit_read(const EditList *e, EditReadFn readfn, void *ctx, int64_t pos, float *out, size_t n);
/* The level (0..1) at which part idx sounds at local frame p (fades + micro-fades) */
float     edit_gain(const EditList *e, int idx, int64_t p);

/* Text for the project/undo: lines "edit_len=..." and "edit_seg=t,len,src,fi0,fi1,fo0,fo1[,xl,xr]". malloc'ed. */
char     *edit_to_text(const EditList *e);
/* The reverse (reads only the edit_* lines; skips the rest). NULL on error. */
EditList *edit_from_text(const char *text, int64_t src_len);
/* List check (tests/loading): sorted, no overlap, within bounds */
bool      edit_valid(const EditList *e);

#endif
