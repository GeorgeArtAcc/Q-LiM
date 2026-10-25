/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _POSIX_C_SOURCE 200809L
#include "applog.h"

#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define LINE_MAX_LEN 300

typedef struct { char text[LINE_MAX_LEN]; int level; double t; } Line;

static Line            g_lines[APPLOG_MAX];
static int             g_head, g_count;          /* ring buffer */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint     g_seq;
static atomic_int      g_new_warn, g_new_err;
static int             g_orig_out = -1, g_orig_err = -1;
static int             g_pipe[2] = { -1, -1 };
#ifdef _WIN32
static FILE           *g_logf;
#endif
static double          g_t0;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static bool has_ci(const char *s, const char *w)
{
    size_t n = strlen(w);
    for (; *s; ++s) {
        size_t i = 0;
        while (i < n && s[i] && tolower((unsigned char)s[i]) == w[i]) ++i;
        if (i == n) return true;
    }
    return false;
}

int applog_classify(const char *s)
{
    if (strncmp(s, "ALSA lib", 8) == 0) return APPLOG_INFO;          /* startup noise, harmless */
    if (has_ci(s, "warning") || has_ci(s, "ran dry") || has_ci(s, "xrun") || has_ci(s, "dropout") ||
        has_ci(s, "could not keep up") || has_ci(s, "no longer exists") || has_ci(s, "continuing anyway") ||
        has_ci(s, "overload") || has_ci(s, "did not end normally"))
        return APPLOG_WARN;
    if (has_ci(s, "error") || has_ci(s, "failed") || has_ci(s, "cannot") || has_ci(s, "lost") ||
        has_ci(s, "crash") || has_ci(s, "fatal") || has_ci(s, "not readable"))
        return APPLOG_ERR;
    return APPLOG_INFO;
}

static void add_line(const char *s, size_t len)
{
    while (len && (s[len - 1] == '\r' || s[len - 1] == '\n')) --len;
    if (len == 0) return;
    pthread_mutex_lock(&g_mu);
    int idx = (g_head + g_count) % APPLOG_MAX;
    if (g_count == APPLOG_MAX) g_head = (g_head + 1) % APPLOG_MAX;
    else g_count++;
    Line *L = &g_lines[idx];
    if (len >= LINE_MAX_LEN) len = LINE_MAX_LEN - 1;
    memcpy(L->text, s, len);
    L->text[len] = '\0';
    for (char *c = L->text; *c; ++c) if (*c == '\t') *c = ' ';
    L->level = applog_classify(L->text);
    L->t = now_s() - g_t0;
    pthread_mutex_unlock(&g_mu);
    if (L->level == APPLOG_WARN) atomic_fetch_add(&g_new_warn, 1);
    if (L->level == APPLOG_ERR)  atomic_fetch_add(&g_new_err, 1);
    atomic_fetch_add(&g_seq, 1);
}

/* reads everything the program writes, forwards it to the console and splits it into lines */
static void *reader(void *arg)
{
    (void)arg;
    char buf[4096], line[LINE_MAX_LEN * 2];
    size_t ll = 0;
    for (;;) {
        ssize_t r = read(g_pipe[0], buf, sizeof buf);
        if (r <= 0) break;
        if (g_orig_out >= 0) { ssize_t w = write(g_orig_out, buf, (size_t)r); (void)w; }
#ifdef _WIN32
        if (g_logf) { fwrite(buf, 1, (size_t)r, g_logf); fflush(g_logf); }   /* no console on Windows -> a log file */
#endif
        for (ssize_t i = 0; i < r; ++i) {
            if (buf[i] == '\n' || ll >= sizeof(line) - 1) { add_line(line, ll); ll = 0; }
            else line[ll++] = buf[i];
        }
    }
    return NULL;
}

void applog_init(void)
{
    g_t0 = now_s();
    if (pipe(g_pipe) != 0) return;
    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    /* a GUI program has no console: stdout/stderr are not connected -> give them a valid descriptor first,
     * and write everything also to ~/.config/qlim/qlim.log (C:/Users/<name>/.config/qlim/qlim.log) */
    {
        const char *h = getenv("HOME");
        char lp[2048];
        snprintf(lp, sizeof lp, "%s/.config", h ? h : ".");
        mkdir(lp, 0755);
        snprintf(lp, sizeof lp, "%s/.config/qlim", h ? h : ".");
        mkdir(lp, 0755);
        snprintf(lp, sizeof lp, "%s/.config/qlim/qlim.log", h ? h : ".");
        struct stat st;
        if (stat(lp, &st) == 0 && st.st_size > 2 * 1024 * 1024) {      /* keep it small: the old one -> qlim.log.old */
            char old[2100];
            snprintf(old, sizeof old, "%s.old", lp);
            rename(lp, old);
        }
        g_logf = fopen(lp, "a");                         /* append: a second Q-LiM (double click) may write too */
        if (g_logf) fprintf(g_logf, "\n===== Q-LiM started (pid %d) =====\n", getpid());
    }
    if (_fileno(stdout) < 0) freopen("NUL", "w", stdout);
    if (_fileno(stderr) < 0) freopen("NUL", "w", stderr);
    if (dup2(g_pipe[1], _fileno(stdout)) < 0 || dup2(g_pipe[1], _fileno(stderr)) < 0) return;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#else
    g_orig_out = dup(1);
    g_orig_err = dup(2);
    if (g_orig_out < 0 || dup2(g_pipe[1], 1) < 0 || dup2(g_pipe[1], 2) < 0) return;
    setvbuf(stdout, NULL, _IOLBF, 0);                  /* line by line (otherwise the log lags behind) */
    setvbuf(stderr, NULL, _IONBF, 0);
#endif
    pthread_t th;
    if (pthread_create(&th, NULL, reader, NULL) == 0) pthread_detach(th);
}

int applog_orig_stderr(void) { return g_orig_err; }

int applog_count(void)
{
    pthread_mutex_lock(&g_mu);
    int n = g_count;
    pthread_mutex_unlock(&g_mu);
    return n;
}

bool applog_get(int i, char *out, size_t n, int *level, double *t)
{
    bool ok = false;
    pthread_mutex_lock(&g_mu);
    if (i >= 0 && i < g_count) {
        const Line *L = &g_lines[(g_head + i) % APPLOG_MAX];
        snprintf(out, n, "%s", L->text);
        if (level) *level = L->level;
        if (t) *t = L->t;
        ok = true;
    }
    pthread_mutex_unlock(&g_mu);
    return ok;
}

unsigned applog_seq(void)        { return atomic_load(&g_seq); }
int      applog_new_warnings(void) { return atomic_load(&g_new_warn); }
int      applog_new_errors(void)   { return atomic_load(&g_new_err); }
void     applog_mark_seen(void)    { atomic_store(&g_new_warn, 0); atomic_store(&g_new_err, 0); }

void applog_clear(void)
{
    pthread_mutex_lock(&g_mu);
    g_head = g_count = 0;
    pthread_mutex_unlock(&g_mu);
    applog_mark_seen();
    atomic_fetch_add(&g_seq, 1);
}
