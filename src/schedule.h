/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef QLIM_SCHEDULE_H
#define QLIM_SCHEDULE_H
/*
 * 1.4.17: launch a clip or a scene at an exact time of the computer's clock (e.g. 21:30:00).
 *
 * A launch at a time is not quantized: it starts at that second (to about one screen frame, ~16 ms - a small
 * thread wakes the main loop at the moment). A scene can start with the scene transition (fades in, what plays
 * fades out). "Stop automations" holds them: while held nothing starts; launches whose time passed meanwhile
 * are then marked "missed" (they do not start late by surprise). Once = it is removed after it started;
 * daily = again the next day. Kept in the project (the clock time, not the date).
 * Nothing runs while the list is empty.
 */
#include "session.h"
#include <stdbool.h>
#include <stdio.h>

#define SCHED_MAX 64
enum { SCHED_CLIP = 0, SCHED_SCENE = 1 };
enum { SCHED_WAITING = 0, SCHED_MISSED = 1 };

typedef struct {
    int    kind, ch, sc;          /* SCHED_*; a scene: ch = -1 */
    int    hh, mm, ss;            /* the clock time */
    bool   fade;                  /* a scene: with the scene transition */
    bool   daily;
    int    state;                 /* SCHED_WAITING / SCHED_MISSED */
    double at;                    /* when (seconds since 1970, local clock), the next time */
} SchedItem;

int   sched_add(int kind, int ch, int sc, int hh, int mm, int ss, bool fade, bool daily);   /* index, -1 = full */
void  sched_remove(int i);
void  sched_clear(void);
int   sched_count(void);
const SchedItem *sched_get(int i);
void  sched_set(int i, int hh, int mm, int ss, bool fade, bool daily);   /* change one (then it waits again) */
int   sched_find(int kind, int ch, int sc);           /* the first one for this cell / scene, -1 = none */
double sched_seconds_left(int i);                     /* until it starts (negative = passed) */
/* a cell moved / deleted, a scene deleted: what was planned for it follows (or goes) */
void  sched_cell_moved(int ch, int sc, int to_ch, int to_sc);   /* to_ch < 0 = removed */
void  sched_scene_removed(int sc);
void  sched_scene_inserted(int idx);
void  sched_channel_removed(int ch);

/* main thread, every frame: starts what is due. Returns how many started now. */
int   sched_tick(Session *s);
void  sched_set_waker(void (*wake)(void));            /* called (from the timer thread) when one is due */
double sched_now(void);
void  sched_clock(double t, int *hh, int *mm, int *ss);   /* the local clock time of t */
double sched_next_time(int hh, int mm, int ss);        /* the next moment the clock shows it (today / tomorrow) */                               /* the local clock (seconds since 1970, with fractions) */
void  sched_format(const SchedItem *it, char *out, size_t n);   /* "21:30:00" */

void  sched_save(FILE *f);                            /* "sched=kind|ch|sc|hh:mm:ss|fade|daily" lines */
void  sched_load_line(const char *value);             /* the part after "sched=" */
#endif
