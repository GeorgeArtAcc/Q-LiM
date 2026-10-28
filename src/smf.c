/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.3: reading a Standard MIDI File (.mid, format 0 and 1) into a MIDI clip - see smf.h.
 * All the tracks and channels are merged; the times are in beats (ticks / division), so the clip follows the master
 * tempo like any MIDI clip (the tempo of the file is not used). Notes that never end get a length of one beat. */
#include "smf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const unsigned char *p, *e; } Rd;

static unsigned rd32(Rd *r) { if (r->e - r->p < 4) { r->p = r->e; return 0; } unsigned v = ((unsigned)r->p[0] << 24) | ((unsigned)r->p[1] << 16) | ((unsigned)r->p[2] << 8) | r->p[3]; r->p += 4; return v; }
static unsigned rd16(Rd *r) { if (r->e - r->p < 2) { r->p = r->e; return 0; } unsigned v = ((unsigned)r->p[0] << 8) | r->p[1]; r->p += 2; return v; }
static unsigned rdvar(Rd *r)
{
    unsigned v = 0;
    for (int i = 0; i < 4 && r->p < r->e; ++i) {
        unsigned char c = *r->p++;
        v = (v << 7) | (c & 0x7F);
        if (!(c & 0x80)) break;
    }
    return v;
}

typedef struct { double on; unsigned char vel; bool active; } Open;

MidiSeq *smf_load(const char *path, char *err, int esz)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, (size_t)esz, "cannot open the file"); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 14 || sz > 64L * 1024 * 1024) { fclose(f); snprintf(err, (size_t)esz, "not a MIDI file"); return NULL; }
    unsigned char *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); snprintf(err, (size_t)esz, "read error"); return NULL; }
    fclose(f);
    Rd r = { buf, buf + sz };
    if (memcmp(r.p, "MThd", 4) != 0) { free(buf); snprintf(err, (size_t)esz, "not a MIDI file (no MThd)"); return NULL; }
    r.p += 4;
    unsigned hl = rd32(&r);
    const unsigned char *hstart = r.p;
    unsigned fmt = rd16(&r), ntr = rd16(&r), div = rd16(&r);
    r.p = hstart + hl;
    if (fmt > 1 && fmt != 2) { free(buf); snprintf(err, (size_t)esz, "unknown MIDI file format %u", fmt); return NULL; }
    if (div & 0x8000) { free(buf); snprintf(err, (size_t)esz, "SMPTE time is not supported"); return NULL; }
    if (div == 0) div = 96;

    int cap = 256, n = 0;
    MNote *notes = malloc(sizeof(MNote) * (size_t)cap);
    double last = 0.0;
    Open open[16][128];
    for (unsigned t = 0; t < ntr && r.p < r.e && notes; ++t) {
        if (r.e - r.p < 8) break;
        if (memcmp(r.p, "MTrk", 4) != 0) {               /* an unknown chunk: skip it */
            r.p += 4;
            unsigned l = rd32(&r);
            if ((size_t)(r.e - r.p) < l) break;
            r.p += l; --t; continue;
        }
        r.p += 4;
        unsigned tl = rd32(&r);
        Rd tr = { r.p, r.p + tl > r.e ? r.e : r.p + tl };
        r.p = tr.e;
        memset(open, 0, sizeof open);
        unsigned long tick = 0;
        unsigned char run = 0;
        while (tr.p < tr.e) {
            tick += rdvar(&tr);
            if (tr.p >= tr.e) break;
            unsigned char st = *tr.p;
            if (st & 0x80) { ++tr.p; if (st < 0xF0) run = st; }
            else st = run;                                   /* running status */
            double beat = (double)tick / div;
            if (st == 0xFF) {                                /* meta: skip (the end of track too) */
                if (tr.p >= tr.e) break;
                ++tr.p;
                unsigned l = rdvar(&tr);
                tr.p = (size_t)(tr.e - tr.p) < l ? tr.e : tr.p + l;
                continue;
            }
            if (st == 0xF0 || st == 0xF7) { unsigned l = rdvar(&tr); tr.p = (size_t)(tr.e - tr.p) < l ? tr.e : tr.p + l; continue; }
            if (st < 0x80) break;                            /* broken data */
            unsigned type = st & 0xF0, ch = st & 0x0F;
            int nd = (type == 0xC0 || type == 0xD0) ? 1 : 2;
            if (tr.e - tr.p < nd) break;
            unsigned d1 = tr.p[0] & 0x7F, d2 = nd > 1 ? (tr.p[1] & 0x7F) : 0;
            tr.p += nd;
            if (type == 0x90 && d2 > 0) {
                if (open[ch][d1].active) goto close_note;    /* the same key again: end the old one here */
                open_new:
                open[ch][d1].active = true; open[ch][d1].on = beat; open[ch][d1].vel = (unsigned char)d2;
                continue;
            }
            if (type == 0x80 || type == 0x90) {
            close_note:
                if (open[ch][d1].active) {
                    double len = beat - open[ch][d1].on;
                    if (len > 0.0 && n < MIDI_MAX_NOTES) {
                        if (n == cap) { cap *= 2; MNote *nn = realloc(notes, sizeof(MNote) * (size_t)cap); if (!nn) break; notes = nn; }
                        MNote *q = &notes[n++];
                        q->start = (float)open[ch][d1].on; q->len = (float)len; q->key = (unsigned char)d1;
                        q->vel = open[ch][d1].vel; q->sel = 0;
                        if (beat > last) last = beat;
                    }
                    open[ch][d1].active = false;
                }
                if (type == 0x90 && d2 > 0) goto open_new;
            }
        }
        for (int c = 0; c < 16; ++c)                         /* notes that never ended: one beat */
            for (int k = 0; k < 128; ++k)
                if (open[c][k].active && n < MIDI_MAX_NOTES && n < cap) {
                    MNote *q = &notes[n++];
                    q->start = (float)open[c][k].on; q->len = 1.0f; q->key = (unsigned char)k; q->vel = open[c][k].vel; q->sel = 0;
                    if (open[c][k].on + 1.0 > last) last = open[c][k].on + 1.0;
                }
    }
    free(buf);
    if (!notes) { snprintf(err, (size_t)esz, "out of memory"); return NULL; }
    if (n == 0) { free(notes); snprintf(err, (size_t)esz, "no notes in this MIDI file"); return NULL; }
    double bars = last / 4.0;
    long nb = (long)bars;
    if ((double)nb < bars - 1e-6) ++nb;
    if (nb < 1) nb = 1;
    if (nb > 256) nb = 256;
    MidiSeq *m = midiseq_new(n, (float)(nb * 4));
    if (!m) { free(notes); snprintf(err, (size_t)esz, "out of memory"); return NULL; }
    memcpy(m->note, notes, sizeof(MNote) * (size_t)n);
    m->n = n;
    free(notes);
    midiseq_sort(m);
    return m;
}

bool smf_is_midi_path(const char *path)
{
    const char *d = path ? strrchr(path, '.') : NULL;
    if (!d) return false;
    char e[8] = { 0 };
    for (int i = 0; i < 6 && d[i + 1]; ++i) e[i] = (char)(d[i + 1] | 0x20);
    return !strcmp(e, "mid") || !strcmp(e, "midi") || !strcmp(e, "smf") || !strcmp(e, "kar");
}
