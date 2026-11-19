/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.17: launches at a clock time (see schedule.h) */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "schedule.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static SchedItem g_it[SCHED_MAX];
static int       g_n = 0;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;   /* the list: main thread writes, the timer thread reads `at` */
static void    (*g_wake)(void);
static pthread_t g_thr;
static bool      g_thr_on = false;
static atomic_bool g_kick;                                 /* the list changed: the timer thread looks again */

double sched_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* the next time the clock shows hh:mm:ss (today, or tomorrow if it has passed) */
static double next_at(int hh, int mm, int ss, double after)
{
    time_t now = (time_t)after;
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    lt.tm_hour = hh; lt.tm_min = mm; lt.tm_sec = ss; lt.tm_isdst = -1;
    time_t t = mktime(&lt);
    if ((double)t <= after) { lt.tm_mday += 1; lt.tm_isdst = -1; t = mktime(&lt); }   /* (mktime normalizes the day) */
    return (double)t;
}

/* the timer thread: sleeps until the next one is due (at most 0.5 s at a time), then wakes the main loop */
static void *timer_main(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        double next = 0;
        for (int i = 0; i < g_n; ++i)
            if (g_it[i].state == SCHED_WAITING && (next == 0 || g_it[i].at < next)) next = g_it[i].at;
        int n = g_n;
        pthread_mutex_unlock(&g_mu);
        if (n == 0) { struct timespec ts = { 0, 200000000L }; nanosleep(&ts, NULL); continue; }
        double now = sched_now(), wait = next > 0 ? next - now : 0.5;
        if (wait > 0.5) wait = 0.5;
        if (wait > 0) {
            struct timespec ts = { (time_t)wait, (long)((wait - (double)(time_t)wait) * 1e9) };
            nanosleep(&ts, NULL);
        }
        if (atomic_exchange(&g_kick, false)) continue;
        if (next > 0 && sched_now() >= next - 0.0005 && g_wake) {
            g_wake();
            struct timespec ts = { 0, 20000000L };       /* (the main loop takes it) */
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

static void ensure_thread(void)
{
    if (g_thr_on) return;
    if (pthread_create(&g_thr, NULL, timer_main, NULL) == 0) { pthread_detach(g_thr); g_thr_on = true; }
}

void sched_set_waker(void (*wake)(void)) { g_wake = wake; }
void sched_clock(double t, int *hh, int *mm, int *ss)
{
    time_t tt = (time_t)t;
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    *hh = lt.tm_hour; *mm = lt.tm_min; *ss = lt.tm_sec;
}
double sched_next_time(int hh, int mm, int ss) { return next_at(hh, mm, ss, sched_now()); }

int sched_add(int kind, int ch, int sc, int hh, int mm, int ss, bool fade, bool daily)
{
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59) return -1;
    pthread_mutex_lock(&g_mu);
    if (g_n >= SCHED_MAX) { pthread_mutex_unlock(&g_mu); return -1; }
    SchedItem *it = &g_it[g_n];
    memset(it, 0, sizeof *it);
    it->kind = kind; it->ch = kind == SCHED_SCENE ? -1 : ch; it->sc = sc;
    it->hh = hh; it->mm = mm; it->ss = ss;
    it->fade = fade; it->daily = daily;
    it->state = SCHED_WAITING;
    it->at = next_at(hh, mm, ss, sched_now());
    int i = g_n++;
    pthread_mutex_unlock(&g_mu);
    atomic_store(&g_kick, true);
    ensure_thread();
    return i;
}

void sched_set(int i, int hh, int mm, int ss, bool fade, bool daily)
{
    if (i < 0 || i >= g_n || hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59) return;
    pthread_mutex_lock(&g_mu);
    SchedItem *it = &g_it[i];
    it->hh = hh; it->mm = mm; it->ss = ss; it->fade = fade; it->daily = daily;
    it->state = SCHED_WAITING;
    it->at = next_at(hh, mm, ss, sched_now());
    pthread_mutex_unlock(&g_mu);
    atomic_store(&g_kick, true);
}

void sched_remove(int i)
{
    pthread_mutex_lock(&g_mu);
    if (i >= 0 && i < g_n) { memmove(&g_it[i], &g_it[i + 1], (size_t)(g_n - i - 1) * sizeof g_it[0]); g_n--; }
    pthread_mutex_unlock(&g_mu);
    atomic_store(&g_kick, true);
}

void sched_clear(void)
{
    pthread_mutex_lock(&g_mu);
    g_n = 0;
    pthread_mutex_unlock(&g_mu);
    atomic_store(&g_kick, true);
}

int sched_count(void) { return g_n; }
const SchedItem *sched_get(int i) { return i >= 0 && i < g_n ? &g_it[i] : NULL; }
int sched_find(int kind, int ch, int sc)
{
    for (int i = 0; i < g_n; ++i)
        if (g_it[i].kind == kind && g_it[i].sc == sc && (kind == SCHED_SCENE || g_it[i].ch == ch)) return i;
    return -1;
}
double sched_seconds_left(int i) { return i >= 0 && i < g_n ? g_it[i].at - sched_now() : 0.0; }

void sched_cell_moved(int ch, int sc, int to_ch, int to_sc)
{
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_n; ++i) {
        SchedItem *it = &g_it[i];
        if (it->kind != SCHED_CLIP || it->ch != ch || it->sc != sc) continue;
        if (to_ch < 0) { memmove(it, it + 1, (size_t)(g_n - i - 1) * sizeof *it); g_n--; --i; }
        else { it->ch = to_ch; it->sc = to_sc; }
    }
    pthread_mutex_unlock(&g_mu);
}

void sched_scene_removed(int sc)
{
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_n; ++i) {
        SchedItem *it = &g_it[i];
        if (it->sc == sc) { memmove(it, it + 1, (size_t)(g_n - i - 1) * sizeof *it); g_n--; --i; }
        else if (it->sc > sc) it->sc--;
    }
    pthread_mutex_unlock(&g_mu);
}

void sched_scene_inserted(int idx)
{
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_n; ++i) if (g_it[i].sc >= idx) g_it[i].sc++;
    pthread_mutex_unlock(&g_mu);
}

void sched_channel_removed(int ch)
{
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_n; ++i) {
        SchedItem *it = &g_it[i];
        if (it->kind != SCHED_CLIP) continue;
        if (it->ch == ch) { memmove(it, it + 1, (size_t)(g_n - i - 1) * sizeof *it); g_n--; --i; }
        else if (it->ch > ch) it->ch--;
    }
    pthread_mutex_unlock(&g_mu);
}

int sched_tick(Session *s)
{
    if (g_n == 0 || !s) return 0;
    double now = sched_now();
    bool held = session_automation_held(s);
    int fired = 0;
    for (int i = 0; i < g_n; ++i) {
        SchedItem *it = &g_it[i];
        if (it->state != SCHED_WAITING || now < it->at - 0.0005) continue;
        if (held || now - it->at > 3.0) {                 /* held, or the program was busy / asleep: not late by surprise */
            pthread_mutex_lock(&g_mu);
            if (it->daily) it->at = next_at(it->hh, it->mm, it->ss, now);
            else it->state = SCHED_MISSED;
            pthread_mutex_unlock(&g_mu);
            continue;
        }
        if (it->kind == SCHED_SCENE) {
            if (it->sc >= 0 && it->sc < s->scene_count) {
                if (it->fade) session_transition_to(s, it->sc);
                else session_play_scene_now(s, it->sc);
            }
        } else if (it->ch >= 0 && it->ch < s->channel_count && it->sc >= 0 && it->sc < MAX_SCENES) {
            Clip *cl = &s->channels[it->ch].clips[it->sc];
            if (atomic_load(&cl->has_clip) && !atomic_load(&cl->is_note)) {
                if (atomic_load(&cl->is_midi)) {
                    if (!atomic_load(&cl->playing)) { s->play_now = true; session_toggle_clip(s, it->ch, it->sc); s->play_now = false; }
                } else session_play_clip_from(s, it->ch, it->sc, atomic_load(&cl->start_marker));
            }
        }
        printf("sched: %02d:%02d:%02d %s %d/%d started (%.0f ms after the second)\n", it->hh, it->mm, it->ss,
               it->kind == SCHED_SCENE ? "scene" : "clip", it->ch, it->sc, (now - it->at) * 1000.0);
        ++fired;
        pthread_mutex_lock(&g_mu);
        if (it->daily) it->at = next_at(it->hh, it->mm, it->ss, now + 1.0);
        else { memmove(it, it + 1, (size_t)(g_n - i - 1) * sizeof *it); g_n--; --i; }
        pthread_mutex_unlock(&g_mu);
    }
    return fired;
}

void sched_format(const SchedItem *it, char *out, size_t n)
{
    if (!it) { snprintf(out, n, "--:--:--"); return; }
    snprintf(out, n, "%02d:%02d:%02d", it->hh, it->mm, it->ss);
}

void sched_save(FILE *f)
{
    for (int i = 0; i < g_n; ++i) {
        const SchedItem *it = &g_it[i];
        if (it->state == SCHED_MISSED && !it->daily) continue;
        fprintf(f, "sched=%d|%d|%d|%02d:%02d:%02d|%d|%d\n", it->kind, it->ch, it->sc, it->hh, it->mm, it->ss, it->fade ? 1 : 0, it->daily ? 1 : 0);
    }
}

void sched_load_line(const char *v)
{
    int kind, ch, sc, hh, mm, ss, fade = 0, daily = 0;
    if (sscanf(v, "%d|%d|%d|%d:%d:%d|%d|%d", &kind, &ch, &sc, &hh, &mm, &ss, &fade, &daily) >= 6 &&
        (kind == SCHED_CLIP || kind == SCHED_SCENE) && sc >= 0 && sc < MAX_SCENES)
        sched_add(kind, ch, sc, hh, mm, ss, fade != 0, daily != 0);
}
