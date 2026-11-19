/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.2: controller buttons for the whole program - see midimap.h */
#define _POSIX_C_SOURCE 200809L   /* fileno, getpid (safefile.h) */
#include "midimap.h"
#include "session.h"        /* MIDI_BIND_* */
#include "settings.h"       /* settings_get_config_path */
#include "safefile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int            g_code[MM_COUNT];          /* MIDI_BIND_MAKE code, 0 = none */
static int            g_learn = -1;
static int            g_wheel_ticks = 1;          /* clicks of a jog wheel per step (1 = every click ... 32 = very fine) */
static int            g_wmode[MM_COUNT];          /* jog wheel: 0 = not known yet, 1 = relative, 2 = absolute (0..127),
                                                     3 = relative 'low' (1 = +, 65 / 127 = -), 4 = relative binary offset (65 = +, 63 = -) */
static int            g_pad_w = 4, g_pad_h = 2;      /* 1.4.8: the pad box */
static int            g_pad[MM_PAD_MAX * MM_PAD_MAX]; /* MIDI_BIND_MAKE code of pad i (box order), 0 = none */
static int            g_npad;                        /* how many are learned */
static int            g_pad_learn = -1;              /* the pad waiting to be learned */
static MidiMapHandler g_fn;
static void          *g_ud;

static const char *const LABELS[MM_COUNT] = {
    "Select up", "Select down", "Select left", "Select right",
    "Jog wheel: up / down", "Jog wheel: left / right",
    "Launch selected", "Stop selected", "Stop all", "Master mute",
    "Next scene (fade)", "Scene back (fade)", "Stop automations", "Fade time knob",
};
static const char *const KEYS[MM_COUNT] = {
    "up", "down", "left", "right", "wheel_v", "wheel_h", "launch", "stop", "stop_all", "master_mute",
    "scene_next", "scene_back", "auto_stop", "fade_time",
};

void        midimap_set_handler(MidiMapHandler fn, void *user) { g_fn = fn; g_ud = user; }
const char *midimap_label(int a) { return a >= 0 && a < MM_COUNT ? LABELS[a] : "?"; }
bool        midimap_is_wheel(int a) { return a == MM_WHEEL_V || a == MM_WHEEL_H || a == MM_FADE_TIME; }
int         midimap_get(int a) { return a >= 0 && a < MM_COUNT ? g_code[a] : 0; }
int         midimap_learning(void) { return g_learn; }
int         midimap_wheel_ticks(void) { return g_wheel_ticks; }
void        midimap_learn(int a) { g_learn = a >= 0 && a < MM_COUNT ? a : -1; if (g_learn >= 0) g_pad_learn = -1; }

void midimap_describe(int a, char *out, int n)
{
    int c = midimap_get(a);
    if (!c) snprintf(out, (size_t)n, "-");
    else snprintf(out, (size_t)n, "%s %d", MIDI_BIND_KIND(c) == MIDI_BIND_CC ? "CC" : "Note", MIDI_BIND_NUM(c));
}

static void path(char *out, size_t n)
{
    char p[1024];
    settings_get_config_path(p, sizeof p);
    char *slash = strrchr(p, '/');
    if (slash) slash[1] = 0;
    snprintf(out, n, "%scontrols.conf", p);
}

static void save(void);

void midimap_pad_box(int *w, int *h) { if (w) *w = g_pad_w; if (h) *h = g_pad_h; }
int  midimap_pad_count(void) { return g_npad; }
bool midimap_pads_active(void) { return g_npad > 0 && g_pad_w * g_pad_h > 1; }
int  midimap_pads_learning(void) { return g_pad_learn; }

void midimap_set_pad_box(int w, int h)
{
    if (w < 1) w = 1;
    if (w > MM_PAD_MAX) w = MM_PAD_MAX;
    if (h < 1) h = 1;
    if (h > MM_PAD_MAX) h = MM_PAD_MAX;
    if (w * h != g_pad_w * g_pad_h && g_npad > w * h) g_npad = w * h;   /* smaller: the first pads stay */
    g_pad_w = w; g_pad_h = h;
    save();
}

void midimap_pads_learn(bool on)
{
    g_pad_learn = on ? 0 : -1;
    if (on) g_learn = -1;
}

void midimap_pads_clear(void)
{
    memset(g_pad, 0, sizeof g_pad);
    g_npad = 0;
    g_pad_learn = -1;
    save();
}

void midimap_note_mask(unsigned out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
#define NM(c) do { int c_ = (c); if (c_ && MIDI_BIND_KIND(c_) == MIDI_BIND_NOTE) { int n_ = MIDI_BIND_NUM(c_) & 0x7F; out[n_ >> 5] |= 1u << (n_ & 31); } } while (0)
    for (int a = 0; a < MM_COUNT; ++a) NM(g_code[a]);
    for (int i = 0; i < g_npad; ++i) NM(g_pad[i]);
#undef NM
}

static void save(void)
{
    char p[1100], tmp[1200];
    path(p, sizeof p);
    FILE *f = safe_open(p, tmp, sizeof tmp);
    if (!f) return;
    fprintf(f, "# Q-LiM controller buttons (MIDI Settings). kind:number, kind 1 = note, 2 = CC\n");
    for (int a = 0; a < MM_COUNT; ++a)
        if (g_code[a]) fprintf(f, "%s=%d:%d\n", KEYS[a], MIDI_BIND_KIND(g_code[a]), MIDI_BIND_NUM(g_code[a]));
    fprintf(f, "wheel_clicks=%d\n", g_wheel_ticks);
    fprintf(f, "pad_box=%dx%d\n", g_pad_w, g_pad_h);
    for (int i = 0; i < g_npad; ++i) if (g_pad[i]) fprintf(f, "pad%d=%d:%d\n", i + 1, MIDI_BIND_KIND(g_pad[i]), MIDI_BIND_NUM(g_pad[i]));         /* (1.4.4: new name - the old "wheel_ticks" default 4 was too coarse) */
    for (int a = 0; a < MM_COUNT; ++a)
        if (midimap_is_wheel(a) && g_wmode[a])
            fprintf(f, "%s_mode=%s\n", KEYS[a], g_wmode[a] == 2 ? "abs" : g_wmode[a] == 3 ? "rel-low" : g_wmode[a] == 4 ? "rel-bo" : "rel");
    safe_commit(f, tmp, p, 0);
}

void midimap_set_wheel_ticks(int n)
{
    if (n < 1) n = 1;
    if (n > 32) n = 32;
    g_wheel_ticks = n;
    save();
}

void midimap_set(int a, int code)
{
    if (a < 0 || a >= MM_COUNT) return;
    if (code) for (int b = 0; b < MM_COUNT; ++b) if (g_code[b] == code) g_code[b] = 0;   /* one control, one job */
    if (code) for (int i = 0; i < g_npad; ++i) if (g_pad[i] == code) g_pad[i] = 0;
    if (g_code[a] != code) g_wmode[a] = 0;        /* another control: find out again how it counts */
    g_code[a] = code;
    save();
}

void midimap_load(void)
{
    char p[1100], line[256];
    path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char key[64]; int k, num;
        if (sscanf(line, "wheel_clicks=%d", &k) == 1) { g_wheel_ticks = k < 1 ? 1 : k > 32 ? 32 : k; continue; }
        {
            int w, h, pi;
            if (sscanf(line, "pad_box=%dx%d", &w, &h) == 2) {
                g_pad_w = w < 1 ? 1 : w > MM_PAD_MAX ? MM_PAD_MAX : w;
                g_pad_h = h < 1 ? 1 : h > MM_PAD_MAX ? MM_PAD_MAX : h;
                continue;
            }
            if (sscanf(line, "pad%d=%d:%d", &pi, &k, &num) == 3) {
                if (pi >= 1 && pi <= MM_PAD_MAX * MM_PAD_MAX && (k == MIDI_BIND_NOTE || k == MIDI_BIND_CC) && num >= 0 && num <= 127) {
                    g_pad[pi - 1] = MIDI_BIND_MAKE(k, num);
                    if (pi > g_npad) g_npad = pi;
                }
                continue;
            }
        }
        {
            char mode[8];
            if (sscanf(line, "%63[^=]=%7s", key, mode) == 2 && strstr(key, "_mode")) {
                for (int a = 0; a < MM_COUNT; ++a) {
                    char want[64]; snprintf(want, sizeof want, "%s_mode", KEYS[a]);
                    if (!strcmp(key, want)) g_wmode[a] = !strcmp(mode, "abs") ? 2 : !strcmp(mode, "rel-low") ? 3 : !strcmp(mode, "rel-bo") ? 4 : !strcmp(mode, "rel") ? 1 : 0;
                }
                continue;
            }
        }
        if (sscanf(line, "%63[^=]=%d:%d", key, &k, &num) != 3) continue;
        if ((k != MIDI_BIND_NOTE && k != MIDI_BIND_CC) || num < 0 || num > 127) continue;
        for (int a = 0; a < MM_COUNT; ++a)
            if (!strcmp(key, KEYS[a])) g_code[a] = MIDI_BIND_MAKE(k, num);
    }
    fclose(f);
}

int midimap_wheel_delta(int v)
{
    if (v >= 1 && v <= 15) return v;                    /* two's complement: 1 = +1 ... */
    if (v >= 113 && v <= 127) return v - 128;           /* ... 127 = -1 */
    if (v >= 65 && v <= 79) return v - 64;              /* binary offset: 65 = +1 ... */
    if (v >= 49 && v <= 63) return v - 64;              /* ... 63 = -1 */
    return 0;
}

/* QLIM_MIDI_LOG=1: every message of a controller button / wheel goes to the log (to see what a controller sends) */
static int log_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("QLIM_MIDI_LOG"); on = e && *e && *e != '0'; } return on; }

/* A jog wheel. Each click of the wheel = one step in its direction (with "clicks per step" > 1: that many).
 * Two kinds of wheels:
 *   relative - sends the SAME value again and again while turning (1 / 127, 65 / 63 ...): the direction is in the value;
 *   absolute - counts 0..127 up and down (e.g. Korg nanoKONTROL Studio as it comes): the direction is the change.
 * Which one it is, is found out from the first values (and kept in controls.conf). An absolute wheel stops sending at 0
 * and at 127 - set it to "relative" / "Inc/Dec" in the controller's editor for endless turning. */
static void wheel(int a, int number, int value)
{
    static int last[128] = { [0 ... 127] = -1 };
    static int acc[MM_COUNT];
    int prev = last[number];
    last[number] = value;
    if (!g_wmode[a] && prev >= 0) {                          /* find out the kind */
        if ((value >= 16 && value <= 48) || (value >= 80 && value <= 112)) g_wmode[a] = 2;
        else if (value == prev && value != 0 && value != 127) g_wmode[a] = 1;
        else if (value != prev && abs(value - prev) <= 8) g_wmode[a] = 2;
        if (g_wmode[a]) {
            if (log_on()) fprintf(stderr, "MIDI wheel %s: %s values\n", midimap_label(a), g_wmode[a] == 2 ? "absolute" : "relative");
            save();
        }
    }
    if (a == MM_FADE_TIME && g_wmode[a] == 2) {             /* 1.4.17: an ordinary knob: its position = the time */
        if (g_fn) g_fn(a, 1000 + value, g_ud);
        return;
    }
    int clicks = 0;
    if (g_wmode[a] == 1 || g_wmode[a] == 3 || g_wmode[a] == 4) {
        /* Relative wheels send the direction in the value; the size is the controller's acceleration (= 1 click).
         * Three styles: two's complement (1 = right, 127 = left), sign + magnitude (1 = right, 65 = left - Korg
         * "Sign Magnitude") and binary offset (65 = right, 63 = left). 65 means "left" in the first two and "right"
         * in the last, so the style is learned from the other values: 1..15 or 113..127 -> 'low', 49..63 -> offset. */
        int old = g_wmode[a];
        if ((value >= 1 && value <= 15) || value >= 113) g_wmode[a] = 3;
        else if (value >= 49 && value <= 63) g_wmode[a] = 4;
        if (g_wmode[a] != old) {
            if (log_on()) fprintf(stderr, "MIDI wheel %s: relative, %s\n", midimap_label(a),
                                  g_wmode[a] == 3 ? "1 = right / 65 or 127 = left" : "65 = right / 63 = left");
            save();
        }
        if (value == 0 || value == 64) clicks = 0;
        else if (g_wmode[a] == 4 || (g_wmode[a] == 1 && value >= 65 && value <= 79)) clicks = value > 64 ? 1 : -1;
        else clicks = value < 64 ? 1 : -1;
    } else if (prev >= 0) {                                  /* absolute (or not known yet): the change = the clicks */
        clicks = value - prev;
        if (clicks > 8) clicks = 8;
        if (clicks < -8) clicks = -8;
    }
    if (log_on()) fprintf(stderr, "MIDI wheel %s: CC %d value %d -> %+d click(s)\n", midimap_label(a), number, value, clicks);
    for (int n = clicks < 0 ? -clicks : clicks; n > 0; --n) {
        int dir = clicks > 0 ? 1 : -1;
        if ((acc[a] > 0 && dir < 0) || (acc[a] < 0 && dir > 0)) acc[a] = 0;   /* turned back: count again */
        acc[a] += dir;
        if (acc[a] >= g_wheel_ticks || acc[a] <= -g_wheel_ticks) {
            acc[a] = 0;
            if (g_fn) g_fn(a, dir, g_ud);
        }
    }
}

bool midimap_handle(int kind, int number, int value)
{
    if (log_on()) fprintf(stderr, "MIDI in: %s %d value %d\n", kind == MIDI_BIND_CC ? "CC" : "note", number, value);
    if (number < 0 || number > 127) return false;
    int code = MIDI_BIND_MAKE(kind, number);

    if (g_pad_learn >= 0) {                                  /* 1.4.8: the pads, one after the other */
        if (value == 0) return true;                         /* the release */
        for (int i = 0; i < g_pad_learn; ++i) if (g_pad[i] == code) return true;   /* the same pad twice: wait */
        for (int a = 0; a < MM_COUNT; ++a) if (g_code[a] == code) g_code[a] = 0;   /* one control, one job */
        g_pad[g_pad_learn++] = code;
        if (g_pad_learn > g_npad) g_npad = g_pad_learn;
        if (g_pad_learn >= g_pad_w * g_pad_h) { g_npad = g_pad_learn; g_pad_learn = -1; }
        save();
        return true;
    }

    if (g_learn >= 0) {
        /* a wheel can only be a CC; buttons: the press (not the release) */
        if (midimap_is_wheel(g_learn) ? kind != MIDI_BIND_CC : value == 0) return true;
        int a = g_learn;
        g_learn = -1;
        midimap_set(a, code);
        return true;
    }

    static bool cc_down[128];
    for (int i = 0; i < g_npad && i < g_pad_w * g_pad_h; ++i) {     /* 1.4.8: a pad of the box */
        if (g_pad[i] != code) continue;
        bool pressed;
        if (kind == MIDI_BIND_CC) { bool now = value >= 64; pressed = now && !cc_down[number]; cc_down[number] = now; }
        else pressed = value > 0;
        if (pressed && g_fn) g_fn(MM_PAD, i, g_ud);
        return true;
    }
    for (int a = 0; a < MM_COUNT; ++a) {
        if (g_code[a] != code) continue;
        if (midimap_is_wheel(a)) {
            wheel(a, number, value);
            return true;
        }
        bool pressed;
        if (kind == MIDI_BIND_CC) {
            bool now = value >= 64;
            pressed = now && !cc_down[number];
            cc_down[number] = now;
        } else {
            pressed = value > 0;
        }
        if (pressed && g_fn) g_fn(a, (a == MM_UP || a == MM_LEFT) ? -1 : +1, g_ud);
        return true;
    }
    return false;
}
