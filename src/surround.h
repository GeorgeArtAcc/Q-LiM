/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef QLIM_SURROUND_H
#define QLIM_SURROUND_H
/*
 * 1.4.19: surround - a channel plays through 4 ... 16 outputs of the sound card placed around the room, and the
 * clips can move the sound around over their time.
 *
 * Panning: pairwise constant power between the two neighbouring speakers of the circle (2-D VBAP, Pulkki 1997 -
 * what Ableton's Surround Panner, Reaper's ReaSurround and SPAT use). "Spread" (0..1) blends in all speakers with
 * the same power (0 = a point, 1 = everywhere, the sum of the powers is always 1). "Width" places the channel's
 * left and right that many degrees apart (0 = the two summed into one point).
 * Angles in degrees: 0 = front, +90 = right, -90 = left, 180 = behind.
 * The automation of a clip: points (time in the clip, angle, spread); between two points the angle moves the short
 * way round, smoothly. Before the first / after the last point: that point. No points = the channel's position.
 * Pure math and text here (no session): the mixer calls sur_gains once per audio block.
 *
 * 1.4.22: ROOM mode - the hall seen from above (x: metres from the middle, - = left; y: metres from the back wall of
 * the audience, the stage at the top). The speakers stand anywhere (also in the middle of the room); the sound is a
 * point that can be anywhere too, also between the speakers: DBAP (distance-based amplitude panning, Lossius 2009 - as
 * theatre systems do it): every speaker gets 1 / distance^1.5 (9 dB per doubling), the powers summed = 1. "Spread"
 * blurs the point (0 = sharp ... 1 = every speaker). Stereo: the left half `width` metres to the left of the point,
 * the right half to the right (towards the walls of the hall, whatever the path does). The clip points then hold
 * x, y, width, spread; between two points everything moves in a straight line.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define SUR_MAX_SPK 16
#define SUR_MAX_PTS 256

typedef struct {
    int   n;                        /* speakers (0 = surround off) */
    int   out[SUR_MAX_SPK];         /* the sound card output of each (0 = output 1) */
    float az[SUR_MAX_SPK];          /* where it stands (degrees) */
    float width;                    /* left / right of the channel: degrees apart (0 = mono) */
    float az0, spread0;             /* the position when the clip has no automation */
    int   layout;                   /* SUR_L_* (what the dialog shows) */
    /* 1.4.22: the room mode */
    int   mode;                     /* SUR_MODE_CIRCLE / SUR_MODE_ROOM */
    float room_w, room_d;           /* the hall: width x depth (metres; the stage is the top part) */
    float stage_d;                  /* how deep the stage is (the top of the plan) */
    float x[SUR_MAX_SPK], y[SUR_MAX_SPK];   /* where every speaker stands */
    float x0, y0, w0;               /* the position and stereo width (metres) when the clip has no points */
    /* 2.0: the Speaker Map - where every speaker really plays: 0 = its output out[k]; else bit o = output o+1
     * (several = the same signal on all of them), SUR_MUTED = nowhere */
    uint64_t omask[SUR_MAX_SPK];
} SurSetup;
#define SUR_MUTED (1ull << 63)
/* 2.0: the outputs a signal plays on (default `def` when mask = 0); returns how many (o[] up to 62) */
int sur_mask_outputs(uint64_t mask, int def, int *o);

enum { SUR_MODE_CIRCLE = 0, SUR_MODE_ROOM = 1 };
enum { SUR_L_QUAD, SUR_L_50, SUR_L_51, SUR_L_RING6, SUR_L_70, SUR_L_71, SUR_L_RING8, SUR_L_N };
/* the room layouts */
enum { SUR_R_THEATER9, SUR_R_QUAD, SUR_R_THEATER6, SUR_R_N };
const char *sur_room_layout_name(int layout);
/* a ready room layout (outputs first, first+1 ...); keeps the room's size if it was set */
void  sur_room_layout(SurSetup *s, int layout, int first);
const char *sur_room_speaker_name(int layout, int k);
const char *sur_layout_name(int layout);
/* a ready layout from the output `first` on (5.1 / 7.1 leave the LFE output - the 4th - free) */
void  sur_layout(SurSetup *s, int layout, int first);
const char *sur_speaker_name(int layout, int k);   /* "L", "C", "Ls" ... */

typedef struct { float t, az, spread, x, y, w; } SurPt;   /* t: seconds in the clip's timeline; x, y, w: the room mode */

/* the gains of the n speakers for a point source at az with spread (constant power) */
void  sur_gains(const SurSetup *s, float az, float spread, float *g);
/* the position at time t (points sorted by t); n = 0 -> az0 / spread0 */
void  sur_eval(const SurPt *p, int n, float t, float az0, float spread0, float *az, float *spread);
float sur_wrap(float az);                           /* to (-180, 180] */
/* 1.4.22: the room mode - gains for a point at (x, y), and the position at time t (straight lines between points) */
void  sur_gains_room(const SurSetup *s, float x, float y, float spread, float *g);
void  sur_eval_room(const SurPt *p, int n, float t, const SurSetup *s, float *x, float *y, float *w, float *spread);
void  sur_sort(SurPt *p, int n);
/* 1.4.23: the gains of the channel's L and R for every speaker at time t (the mixer and the export); vis may be NULL */
void  sur_pair_gains(const SurSetup *su, const SurPt *p, int n, float t, float *gl, float *gr, float *vis);
void  sur_speaker_label(const SurSetup *su, int k, char *out, size_t n);

/* text forms (project, undo): setup "n|out,out,...|az,az,...|width|az0|spread0|layout" (+ 1.4.22:
 * "|mode|room_w|room_d|stage_d|x,x,...|y,y,...|x0|y0|w0" - older versions read only the first part);
 * points "t:az:spread:x:y:w ..." (older versions read t:az:spread) */
void  sur_setup_text(const SurSetup *s, char *out, size_t n);
bool  sur_setup_parse(const char *txt, SurSetup *s);
void  sur_points_text(const SurPt *p, int n, char *out, size_t sz);
int   sur_points_parse(const char *txt, SurPt *p, int max);
#endif
