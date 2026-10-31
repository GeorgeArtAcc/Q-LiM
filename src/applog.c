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

static int classify_raw(const char *s);
int applog_classify(const char *line)
{
    /* 1.3.14: file and track names are written in quotes - a song called "Lost ..." or a folder "crash" is not an
     * error: what is inside '...' or "..." does not count */
    char b[1024];
    size_t n = 0;
    char q = 0;
    for (const char *p = line; *p && n < sizeof b - 1; ++p) {
        if (q) { if (*p == q) q = 0; b[n++] = ' '; continue; }
        if (*p == '\'' || *p == '"') { q = *p; b[n++] = ' '; continue; }
        b[n++] = *p;
    }
    b[n] = 0;
    if (strncmp(line, "Q-LiM ", 6) == 0 && strstr(line, " build:")) return APPLOG_INFO;   /* the version line */
    /* 1.3.15: harmless messages of the libraries while they look for sound cards / start up */
    if (strncmp(line, "Expression '", 12) == 0 && strstr(line, "pa_linux_alsa")) return APPLOG_INFO;   /* PortAudio probing ALSA */
    if (strstr(line, "pa_linux_alsa.c") || strstr(line, "pa_unix_util.c")) return APPLOG_INFO;
    if (strstr(line, "jack server is not running") || strstr(line, "Cannot connect to server") ||
        strstr(line, "JackShmReadWritePtr") || strstr(line, "jack_client_open") ||
        strstr(line, "connect(2) call to /dev/shm/jack")) return APPLOG_INFO;                  /* PortAudio looking for JACK */
    if (strncmp(line, "Gtk-", 4) == 0 || strstr(line, "(zenity:") || strstr(line, "dbind-WARNING") ||
        strstr(line, "Gdk-Message") || strstr(line, "Gtk-Message")) return APPLOG_INFO;        /* the zenity windows */
    if (strstr(line, "libmpg123/")) {                                                            /* the mp3 reader */
        int c = classify_raw(b);
        return strstr(line, "id3.c") ? APPLOG_INFO : (c == APPLOG_ERR ? APPLOG_WARN : c);      /* a damaged frame: it goes on */
    }
    return classify_raw(b);
}
static int classify_raw(const char *s)
{
    if (strncmp(s, "ALSA lib", 8) == 0) return APPLOG_INFO;          /* startup noise, harmless */
    /* 1.3.13: the mp3 reader (mpg123) complains about empty tag fields (e.g. an empty "comment") - the sound is fine */
    if (strstr(s, "libmpg123/id3.c") || (strstr(s, "mpg123") && has_ci(s, "id3"))) return APPLOG_INFO;
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
        /* 1.3.15: line by line - what the log counts as an error / a warning gets a mark in the terminal too
         * ([ERROR] / [WARN]), so the terminal can be copied and sent with the same picture as the log */
        for (ssize_t i = 0; i < r; ++i) {
            if (buf[i] == '\n' || ll >= sizeof(line) - 1) {
                line[ll] = 0;
                int lv = ll ? applog_classify(line) : APPLOG_INFO;
                const char *tag = lv == APPLOG_ERR ? "[ERROR] " : lv == APPLOG_WARN ? "[WARN]  " : "";
                if (g_orig_out >= 0) {
                    ssize_t w = write(g_orig_out, tag, strlen(tag)); (void)w;
                    w = write(g_orig_out, line, ll); (void)w;
                    w = write(g_orig_out, "\n", 1); (void)w;
                }
#ifdef _WIN32
                if (g_logf) { fputs(tag, g_logf); fwrite(line, 1, ll, g_logf); fputc('\n', g_logf); fflush(g_logf); }   /* no console on Windows -> a log file */
#endif
                add_line(line, ll); ll = 0;
            }
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

/* 1.3.13: for the crash report - the last lines, without the lock (the crash may have happened while it was held) */
void applog_dump_fd(int fd, int last)
{
    int cnt = g_count, head = g_head;
    if (cnt > APPLOG_MAX || cnt < 0) return;
    int from = cnt > last ? cnt - last : 0;
    for (int i = from; i < cnt; ++i) {
        const char *t = g_lines[(head + i) % APPLOG_MAX].text;
        size_t n = strnlen(t, LINE_MAX_LEN);
        if (write(fd, t, n) < 0 || write(fd, "\n", 1) < 0) return;
    }
}

