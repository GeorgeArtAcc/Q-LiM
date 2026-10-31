/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.3.7: performance - how the window is drawn and how often, so the sound always has room (Edit > Performance).
 *  - drawing: Auto (graphics card, or the light software drawing when there is no acceleration) / card / light
 *  - frames per second: Auto (as many as fit in ~40 % of one CPU core, measured all the time) or a fixed maximum
 *  - fewer frames while nothing moves (10 per second)
 *  - sound first: when the sound breaks up (card, disk, WARP), fewer frames for 30 s and a message saying why
 * Saved in ~/.config/qlim/performance.conf. */
#ifndef QLIM_PERF_H
#define QLIM_PERF_H
#include <stdbool.h>

enum { PERF_DRAW_AUTO = 0, PERF_DRAW_GPU = 1, PERF_DRAW_LIGHT = 2 };

typedef struct {
    int  draw;          /* PERF_DRAW_* (applies at the next start) */
    int  fps;           /* 0 = Auto, else the maximum (60/30/20/12) */
    bool idle_save;     /* 10 frames per second while nothing moves */
    bool sound_first;   /* fewer frames for a while after a dropout */
    int  eco;           /* 1.3.8: Eco interface (redraw only what changed, simple shapes, no blur):
                         * 0 = Auto (Eco with the light CPU drawing), 1 = Normal, 2 = Eco */
    bool  meters;       /* 1.3.10: the level meters are drawn (Ctrl+Shift+U) */
    float zoom;         /* 1.3.10: the size of the interface (Ctrl+wheel, Ctrl+= / Ctrl+- / Ctrl+0) */
    int   wave;         /* 1.3.14: the waveform: 0 = Auto (light with Eco), 1 = smooth, 2 = light
                         * (light: SCROLL moves 15 times per second, whole-pixel steps) */
} PerfSettings;

void          perf_load(void);
void          perf_save(void);
PerfSettings *perf_settings(void);

/* every frame (main loop): measures, watches the sound, sets the frame rate in sdlgfx */
void perf_frame(bool playing, bool plugin_windows_open);

/* for the dialog */
int   perf_max_fps_now(void);       /* the cap now (0 = none) */
float perf_ui_load(void);           /* share of one CPU core the drawing uses at the current rate (0..1+) */
void  perf_problem_counts(unsigned *card, unsigned *disk, unsigned *warp, unsigned *over);
bool  perf_slowed_for_sound(void);
bool  perf_wave_light(void);        /* 1.3.14: draw the waveform the light way now */  /* "sound first" is holding the frame rate down right now */

#endif
