/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/*
 * 2.0.0: a plugin in a SEPARATE PROCESS ("plugin sandbox", like Bitwig's crash protection).
 *
 * The parent (Q-LiM) starts "qlim --plugin-host <socket> <shared memory>" for one plugin instance. The child loads the
 * plugin with the normal in-process code (host_*.c) and:
 *   - audio: every block the parent writes the input + MIDI into shared memory, wakes the child (futex) and waits for
 *     it - but never longer than ~2 blocks. A late, frozen or crashed child only gives SILENCE; Q-LiM goes on.
 *   - everything else (parameters, state, window) goes through the socket, always with a time limit.
 *   - the plugin window is the child's own top-level window (Gtk/Qt windows of LV2 plugins live there safely too).
 * If the child dies or stops answering, the parent says so in the log and on the screen and the plugin stays silent.
 *
 * Which plugins run like this is decided in plughost.c (qplug_wants_separate): the user's choice (off / automatic /
 * all), the list of plugins that crashed or froze Q-LiM before, and LV2 plugins whose window needs Gtk.
 * Linux: socketpair + memfd + futex. Windows (XP and newer): a named pipe + a named shared memory + two events,
 * the children in a job object (they end with Q-LiM), no "stopped working" box when a child crashes.
 */
#define _GNU_SOURCE
#include "plughost.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>

#ifdef _WIN32
/* Windows: a named pipe (overlapped I/O = time limits), a named shared memory section, two auto-reset events
 * instead of the futex, CreateProcess + a job object (the children go with Q-LiM). Works on XP too. */
#include <windows.h>
typedef HANDLE BrFd;
#define BR_NOFD NULL
#else
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <spawn.h>
#include <unistd.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
extern char **environ;
typedef int BrFd;
#define BR_NOFD (-1)
#endif

#define BR_MAXCH  QPLUG_MAXCH
/* Q-LiM uses only the first two inputs and outputs of a plugin (see plughost.h), so only those cross between the
 * processes; a plugin's other channels get their own buffers inside the separate process (any number, little memory) */
#define BR_SHMCH  2
#define BR_MAXP   2048
#define BR_TXT    48

typedef struct {
    atomic_int req;                 /* parent: number of the block asked for */
    atomic_int done;                /* child: number of the last finished block */
    atomic_int quit;
    int n;                          /* frames in this block */
    int nmidi;
    QMidiEv midi[QMIDI_MAX];
    int maxb;
    atomic_int npub;                /* parameters published by the child */
    atomic_uint pgen;
    float pval[BR_MAXP];
    float audio[];                  /* [BR_SHMCH inputs][BR_SHMCH outputs] x maxb */
} BrShm;

enum { M_OPEN = 1, M_PNAME, M_PTEXT, M_PSET, M_GETSTATE, M_SETSTATE, M_EDOPEN, M_EDCLOSE, M_EDRAISE, M_QUIT,
       M_REPLY = 50, E_CLOSED = 60, E_EDIT, E_OPENED, E_OPENFAIL };

typedef struct { uint32_t type, len; } MsgHdr;

static size_t shm_size(int maxb) { return sizeof(BrShm) + (size_t)(2 * BR_SHMCH) * (size_t)maxb * sizeof(float); }
static float *shm_in(BrShm *s, int c)  { return s->audio + (size_t)c * (size_t)s->maxb; }
static float *shm_out(BrShm *s, int c) { return s->audio + (size_t)(BR_SHMCH + c) * (size_t)s->maxb; }

#ifndef _WIN32
static void fx_wake(atomic_int *a) { syscall(SYS_futex, (int *)a, FUTEX_WAKE, INT_MAX, NULL, NULL, 0); }
static void fx_wait(atomic_int *a, int val, long ns)
{
    struct timespec ts = { ns / 1000000000L, ns % 1000000000L };
    syscall(SYS_futex, (int *)a, FUTEX_WAIT, val, &ts, NULL, 0);
}
#endif

static double now_s(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
#endif
}

static void nap_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

/* whole message with a time limit (ms, <0 = no limit). 0 = ok */
#ifdef _WIN32
static int io_all(BrFd fd, void *buf, size_t n, bool wr, int ms)
{
    static __thread HANDLE ev;                       /* one event per thread for the overlapped calls */
    if (!ev) ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ev) return -1;
    unsigned char *p = buf;
    double end = now_s() + ms / 1000.0;
    while (n > 0) {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        ov.hEvent = ev;
        ResetEvent(ev);
        DWORD chunk = n > (1u << 20) ? (1u << 20) : (DWORD)n, got = 0;
        BOOL ok = wr ? WriteFile(fd, p, chunk, NULL, &ov) : ReadFile(fd, p, chunk, NULL, &ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING) return -1;
        DWORD left = INFINITE;
        if (ms >= 0) { double l = (end - now_s()) * 1000.0; left = l < 0 ? 0 : (DWORD)l; }
        if (WaitForSingleObject(ev, left) != WAIT_OBJECT_0) {        /* too long: the call is taken back */
            CancelIo(fd);
            GetOverlappedResult(fd, &ov, &got, TRUE);
            return -1;
        }
        if (!GetOverlappedResult(fd, &ov, &got, FALSE) || got == 0) return -1;
        p += got; n -= got;
    }
    return 0;
}
/* is there something to read: 1 yes, 0 not within ms, -1 the other side is gone */
static int io_readable(BrFd fd, int ms)
{
    double end = now_s() + ms / 1000.0;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(fd, NULL, 0, NULL, &avail, NULL)) return -1;
        if (avail > 0) return 1;
        if (now_s() >= end) return 0;
        Sleep(1);
    }
}
#else
static int io_readable(BrFd fd, int ms)
{
    struct pollfd pf = { fd, POLLIN, 0 };
    int r = poll(&pf, 1, ms);
    if (r <= 0) return 0;
    if ((pf.revents & (POLLHUP | POLLERR)) && !(pf.revents & POLLIN)) return -1;
    return 1;
}
static int io_all(BrFd fd, void *buf, size_t n, bool wr, int ms)
{
    unsigned char *p = buf;
    double end = now_s() + ms / 1000.0;
    while (n > 0) {
        struct pollfd pf = { fd, (short)(wr ? POLLOUT : POLLIN), 0 };
        int left = ms < 0 ? -1 : (int)((end - now_s()) * 1000.0);
        if (ms >= 0 && left < 0) left = 0;
        int r = poll(&pf, 1, left);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        ssize_t k = wr ? send(fd, p, n, MSG_NOSIGNAL) : recv(fd, p, n, 0);
        if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (k <= 0) return -1;
        p += k; n -= (size_t)k;
    }
    return 0;
}
#endif

static int msg_send(BrFd fd, uint32_t type, const void *a, size_t alen, const void *b, size_t blen, int ms)
{
    MsgHdr h = { type, (uint32_t)(alen + blen) };
    if (io_all(fd, &h, sizeof h, true, ms) != 0) return -1;
    if (alen && io_all(fd, (void *)a, alen, true, ms) != 0) return -1;
    if (blen && io_all(fd, (void *)b, blen, true, ms) != 0) return -1;
    return 0;
}

/* reads one message; *data is malloc'ed (+1 zero byte at the end). 0 = ok */
static int msg_recv(BrFd fd, uint32_t *type, unsigned char **data, uint32_t *len, int ms)
{
    MsgHdr h;
    *data = NULL; *len = 0;
    if (io_all(fd, &h, sizeof h, false, ms) != 0) return -1;
    if (h.len > 256u * 1024u * 1024u) return -1;
    unsigned char *d = malloc((size_t)h.len + 1);
    if (!d) return -1;
    if (h.len && io_all(fd, d, h.len, false, ms < 0 ? -1 : (ms < 5000 ? 5000 : ms)) != 0) { free(d); return -1; }
    d[h.len] = 0;
    *type = h.type; *data = d; *len = h.len;
    return 0;
}

typedef struct {
    int  ok, nin, nout, has_midi, has_editor, nparams;
    char err[300];
} OpenReply;

/* ======================================================================================================
 * PARENT side
 * ====================================================================================================== */
typedef struct {
    /* what was asked for (a restart opens the same again) */
    char  fmt[8];
    char  id[1300];
    int   maxb;
    double rate;
    /* the running child */
#ifdef _WIN32
    HANDLE proc, map, ev_req, ev_done;
    DWORD pid;
#else
    pid_t pid;
#endif
    BrFd  sock;
    BrShm *sh;
    size_t shsz;
    int   nin, nout, nparams;
    bool  has_midi, has_editor;
    int   sent;                      /* the last block asked for */
    double late_since;               /* when the child stopped keeping up (0 = it keeps up) */
    int   seen_done;                 /* main thread: the child's progress ... */
    double seen_at;                  /* ... and when it last moved */
    atomic_bool dead;
    atomic_bool rt_busy;             /* the audio thread is using sh right now */
    atomic_bool ready;               /* the audio thread may use sh (started and the plugin is open) */
    bool  told;
    char  why[160];
    char  name[200];
    PlugWin *win;
    double ed_asked;                 /* the window was asked for (it opens in the other process); 0 = it answered */
    QMidiEv pm[QMIDI_MAX];           /* MIDI for the next block (RT thread) */
    int   npm;
    char  (*pname)[64];              /* cached parameter names */
    bool  *pname_ok;
    float *ptxt_v;                   /* cached parameter texts and the value they belong to */
    char  (*ptxt)[BR_TXT];
    /* for a restart after a crash: the last known settings */
    void  *last_state;
    size_t last_len;
    double restarts[3];              /* when the last restarts happened */
} Br;

/* ---- the OS part of the parent ---- */
static void br_kill_child(Br *b)
{
#ifdef _WIN32
    if (b->proc) { TerminateProcess(b->proc, 1); WaitForSingleObject(b->proc, 2000); CloseHandle(b->proc); b->proc = NULL; }
    b->pid = 0;
#else
    if (b->pid > 0) { kill(b->pid, SIGKILL); waitpid(b->pid, NULL, 0); b->pid = -1; }
#endif
}
static bool br_child_running(Br *b)
{
#ifdef _WIN32
    return b->proc != NULL;
#else
    return b->pid > 0;
#endif
}
/* has the child ended by itself? *crashed = by a crash (signal / exception) */
static bool br_child_ended(Br *b, bool *crashed)
{
#ifdef _WIN32
    if (!b->proc || WaitForSingleObject(b->proc, 0) != WAIT_OBJECT_0) return false;
    DWORD code = 0;
    GetExitCodeProcess(b->proc, &code);
    *crashed = code >= 0xC0000000u || code == 3;          /* an exception, or abort() */
    CloseHandle(b->proc); b->proc = NULL; b->pid = 0;
    return true;
#else
    int st;
    if (b->pid <= 0 || waitpid(b->pid, &st, WNOHANG) != b->pid) return false;
    *crashed = WIFSIGNALED(st);
    b->pid = -1;
    return true;
#endif
}
static bool br_child_wait(Br *b, int ms)                   /* waits for a nice end */
{
#ifdef _WIN32
    if (!b->proc) return true;
    if (WaitForSingleObject(b->proc, (DWORD)ms) != WAIT_OBJECT_0) return false;
    CloseHandle(b->proc); b->proc = NULL; b->pid = 0;
    return true;
#else
    for (int i = 0; i < ms / 10; ++i) {
        if (waitpid(b->pid, NULL, WNOHANG) == b->pid) { b->pid = -1; return true; }
        nap_ms(10);
    }
    return false;
#endif
}
static void br_wake_child(Br *b)
{
#ifdef _WIN32
    if (b->ev_req) SetEvent(b->ev_req);
#else
    fx_wake(&b->sh->req);
#endif
}
static void br_wait_child(Br *b, int cur, double secs)    /* until done changes from cur (or the time is up) */
{
#ifdef _WIN32
    (void)cur;
    DWORD ms = (DWORD)(secs * 1000.0);
    WaitForSingleObject(b->ev_done, ms ? ms : 1);
#else
    fx_wait(&b->sh->done, cur, (long)(secs * 1e9) + 1000);
#endif
}

static void br_mark_dead(Br *b, const char *why)
{
    if (atomic_load(&b->dead)) return;
    atomic_store(&b->ready, false);
    atomic_store(&b->dead, true);
    snprintf(b->why, sizeof b->why, "%s", why);
    br_kill_child(b);
}

/* the shared memory may be thrown away only when the audio thread is not inside br_process */
static void br_release(Br *b)
{
    atomic_store(&b->ready, false);
    atomic_store(&b->dead, true);
    for (int i = 0; i < 200 && atomic_load(&b->rt_busy); ++i) nap_ms(1);
    br_kill_child(b);
#ifdef _WIN32
    if (b->sock) { CloseHandle(b->sock); b->sock = NULL; }
    if (b->sh) { UnmapViewOfFile(b->sh); b->sh = NULL; }
    if (b->map) { CloseHandle(b->map); b->map = NULL; }
    if (b->ev_req) { CloseHandle(b->ev_req); b->ev_req = NULL; }
    if (b->ev_done) { CloseHandle(b->ev_done); b->ev_done = NULL; }
#else
    if (b->sock >= 0) { close(b->sock); b->sock = -1; }
    if (b->sh) { munmap(b->sh, b->shsz); b->sh = NULL; }
#endif
}

static void br_event(Br *b, uint32_t type, const char *data)
{
    if (type == E_CLOSED) { if (b->win) plugwin_request_close(b->win); }
    else if (type == E_EDIT) qplug_note_edit();
    else if (type == E_OPENED) b->ed_asked = 0;
    else if (type == E_OPENFAIL) {
        b->ed_asked = 0;
        if (b->win) plugwin_request_close(b->win);
        char m[400];
        snprintf(m, sizeof m, "%s: %s", b->name, data && data[0] ? data : "the window did not open");
        printf(">>> Plugin window: %s\n", m);
        qplug_report(m);
    }
}

/* a request and its answer (the child's events on the way are handled) */
static int br_rpc(Br *b, uint32_t type, const void *a, size_t alen, const void *x, size_t xlen,
                  unsigned char **rep, uint32_t *rlen, int ms)
{
    if (rep) { *rep = NULL; *rlen = 0; }
    if (atomic_load(&b->dead) || b->sock == BR_NOFD) return -1;
    if (msg_send(b->sock, type, a, alen, x, xlen, ms) != 0) { br_mark_dead(b, "stopped answering"); return -1; }
    if (!rep) return 0;
    for (;;) {
        uint32_t t; unsigned char *d; uint32_t l;
        if (msg_recv(b->sock, &t, &d, &l, ms) != 0) {
            br_mark_dead(b, ms >= 0 ? "stopped answering" : "closed unexpectedly");
            return -1;
        }
        if (t == M_REPLY) { *rep = d; *rlen = l; return 0; }
        br_event(b, t, (const char *)d);
        free(d);
    }
}

static void br_process(void *h, float **in, float **out, unsigned n)
{
    Br *b = h;
    atomic_store(&b->rt_busy, true);
    BrShm *s = b->sh;
    if (!atomic_load(&b->ready) || atomic_load(&b->dead) || !s || n > (unsigned)s->maxb || atomic_load(&s->done) != b->sent) {   /* gone or busy: silence */
        for (int c = 0; c < b->nout; ++c) memset(out[c], 0, n * sizeof(float));
        b->npm = 0;
        if (!atomic_load(&b->dead) && b->late_since == 0) b->late_since = now_s();
        atomic_store(&b->rt_busy, false);
        return;
    }
    for (int c = 0; c < b->nin && c < BR_SHMCH; ++c) memcpy(shm_in(s, c), in[c], n * sizeof(float));
    s->n = (int)n;
    s->nmidi = b->npm;
    if (b->npm) memcpy(s->midi, b->pm, sizeof(QMidiEv) * (size_t)b->npm);
    b->npm = 0;
    int want = ++b->sent;
    atomic_store(&s->req, want);
    br_wake_child(b);
    /* wait: at most 2 blocks (at least 20 ms) - a slow plugin must not stop the whole sound */
    double lim = 2.0 * (double)n / (b->rate > 0 ? b->rate : 48000.0);
    if (lim < 0.020) lim = 0.020;
    double t0 = now_s();
    for (int spin = 0; atomic_load(&s->done) != want; ++spin) {
        double el = now_s() - t0;
        if (el > lim) break;
        if (spin < 64) continue;                         /* a very short spin: the usual answer comes in µs */
        int cur = atomic_load(&s->done);
        if (cur != want) br_wait_child(b, cur, lim - el);
    }
    if (atomic_load(&s->done) != want) {
        for (int c = 0; c < b->nout; ++c) memset(out[c], 0, n * sizeof(float));
        if (b->late_since == 0) b->late_since = now_s();
    } else {
        b->late_since = 0;
        for (int c = 0; c < b->nout; ++c) {
            if (c < BR_SHMCH) memcpy(out[c], shm_out(s, c), n * sizeof(float));
            else memset(out[c], 0, n * sizeof(float));
        }
    }
    atomic_store(&b->rt_busy, false);
}

static void br_midi(void *h, const QMidiEv *ev, int n)
{
    Br *b = h;
    for (int i = 0; i < n && b->npm < QMIDI_MAX; ++i) b->pm[b->npm++] = ev[i];
}

static int br_param_count(void *h) { return ((Br *)h)->nparams; }

static bool br_param_name(void *h, int i, char *name, int nsz)
{
    Br *b = h;
    if (i < 0 || i >= b->nparams) return false;
    if (i < BR_MAXP && b->pname && b->pname_ok[i]) { snprintf(name, (size_t)nsz, "%s", b->pname[i]); return true; }
    int32_t ii = i;
    unsigned char *r; uint32_t rl;
    if (br_rpc(b, M_PNAME, &ii, sizeof ii, NULL, 0, &r, &rl, 2000) != 0) return false;
    snprintf(name, (size_t)nsz, "%s", (char *)r);
    if (i < BR_MAXP && b->pname) { snprintf(b->pname[i], sizeof b->pname[i], "%s", (char *)r); b->pname_ok[i] = true; }
    free(r);
    return name[0] != 0;
}

static float br_param_get(void *h, int i)
{
    Br *b = h;
    if (!b->sh || i < 0 || i >= BR_MAXP || i >= atomic_load(&b->sh->npub)) return 0.0f;
    return b->sh->pval[i];
}

static void br_param_set(void *h, int i, float v)
{
    Br *b = h;
    if (i < 0) return;
    struct { int32_t i; float v; } m = { i, v };
    br_rpc(b, M_PSET, &m, sizeof m, NULL, 0, NULL, NULL, 500);
    if (b->sh && i < BR_MAXP) b->sh->pval[i] = v;       /* shown at once; the child publishes the real value soon */
}

static void br_param_text(void *h, int i, char *out, int sz)
{
    Br *b = h;
    if (sz > 0) out[0] = 0;
    if (i < 0 || i >= b->nparams) return;
    float v = br_param_get(h, i);
    if (i < BR_MAXP && b->ptxt && b->ptxt_v[i] == v && b->ptxt[i][0]) { snprintf(out, (size_t)sz, "%s", b->ptxt[i]); return; }
    int32_t ii = i;
    unsigned char *r; uint32_t rl;
    if (br_rpc(b, M_PTEXT, &ii, sizeof ii, NULL, 0, &r, &rl, 1000) != 0) return;
    snprintf(out, (size_t)sz, "%s", (char *)r);
    if (i < BR_MAXP && b->ptxt) { snprintf(b->ptxt[i], BR_TXT, "%s", (char *)r); b->ptxt_v[i] = v; }
    free(r);
}

static void br_keep_state(Br *b, const void *data, size_t len)    /* the last known settings, for a restart */
{
    void *c = len ? malloc(len) : NULL;
    if (len && !c) return;
    if (len) memcpy(c, data, len);
    free(b->last_state);
    b->last_state = c; b->last_len = len;
}

static int br_get_state(void *h, void **data, size_t *len)
{
    Br *b = h;
    *data = NULL; *len = 0;
    unsigned char *r; uint32_t rl;
    if (br_rpc(b, M_GETSTATE, NULL, 0, NULL, 0, &r, &rl, 8000) != 0) return -1;
    if (rl < sizeof(int32_t)) { free(r); return -1; }
    int32_t rc;
    memcpy(&rc, r, sizeof rc);
    if (rc != 0 || rl == sizeof rc) { free(r); return -1; }
    size_t n = rl - sizeof rc;
    void *d = malloc(n);
    if (!d) { free(r); return -1; }
    memcpy(d, r + sizeof rc, n);
    free(r);
    br_keep_state(b, d, n);
    *data = d; *len = n;
    return 0;
}

static int br_set_state(void *h, const void *data, size_t len)
{
    Br *b = h;
    unsigned char *r; uint32_t rl;
    if (br_rpc(b, M_SETSTATE, data, len, NULL, 0, &r, &rl, 8000) != 0) return -1;
    int32_t rc = -1;
    if (rl >= sizeof rc) memcpy(&rc, r, sizeof rc);
    free(r);
    if (rc == 0) br_keep_state(b, data, len);
    return rc;
}

static bool br_has_editor(void *h) { Br *b = h; return !atomic_load(&b->dead) && b->has_editor; }

static void br_allow_front(Br *b)                         /* Windows: the child may bring its window to the front */
{
#ifdef _WIN32
    if (b->pid) AllowSetForegroundWindow(b->pid);
#else
    (void)b;
#endif
}

static void br_raise(void *ud)
{
    Br *b = ud;
    br_allow_front(b);
    br_rpc(b, M_EDRAISE, NULL, 0, NULL, 0, NULL, NULL, 500);
}

static int br_ed_open(void *h, PlugWin *w, int *W, int *H, char *err, int esz)
{
    Br *b = h;
    *W = *H = 0;
    if (atomic_load(&b->dead)) { snprintf(err, (size_t)esz, "the plugin's process has stopped (%s)", b->why); return -1; }
    uint64_t xid = plugwin_main_xid();
    const char *title = plugwin_title(w);
    br_allow_front(b);
    /* asked, not waited for: the window opens in the other process; if it fails, a message comes (E_OPENFAIL).
     * So a plugin that is slow or stuck while opening its window never stops Q-LiM's screen. */
    if (br_rpc(b, M_EDOPEN, &xid, sizeof xid, title, strlen(title) + 1, NULL, NULL, 2000) != 0) {
        snprintf(err, (size_t)esz, "the plugin's process %s", b->why[0] ? b->why : "did not answer");
        return -1;
    }
    b->ed_asked = now_s();
    b->win = w;
    plugwin_set_external(w, br_raise, b);              /* its window is in the other process: ours stays hidden */
    return 0;
}

static void br_ed_close(void *h)
{
    Br *b = h;
    b->ed_asked = 0;
    unsigned char *r; uint32_t rl;
    if (br_rpc(b, M_EDCLOSE, NULL, 0, NULL, 0, &r, &rl, 5000) == 0) free(r);
    b->win = NULL;
}

#ifdef _WIN32
static wchar_t g_self[MAX_PATH];
static HANDLE  g_job;                                   /* the children end together with Q-LiM */
static LONG    g_serial;
#else
static char g_self[PATH_MAX];
#endif

/* starts the child and opens the plugin in it. 0 = ok */
static int br_start(Br *b, char *err, int esz)
{
    b->sock = BR_NOFD; b->sh = NULL;
    b->sent = 0; b->late_since = 0; b->seen_done = 0; b->seen_at = 0; b->ed_asked = 0; b->npm = 0;
    b->why[0] = 0;
    b->shsz = shm_size(b->maxb);
#ifdef _WIN32
    b->proc = NULL; b->pid = 0;
    if (!g_self[0] && !GetModuleFileNameW(NULL, g_self, MAX_PATH)) { snprintf(err, (size_t)esz, "cannot find the Q-LiM program file"); return -1; }
    if (!g_job) {
        g_job = CreateJobObjectW(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        if (g_job) SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    wchar_t base[96], nm[128];
    swprintf(base, 96, L"qlim-%lu-%ld", (unsigned long)GetCurrentProcessId(), (long)InterlockedIncrement(&g_serial));
    swprintf(nm, 128, L"Local\\%ls-shm", base);
    b->map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)b->shsz, nm);
    if (!b->map) { snprintf(err, (size_t)esz, "cannot create shared memory"); return -1; }
    BrShm *sh = MapViewOfFile(b->map, FILE_MAP_ALL_ACCESS, 0, 0, b->shsz);
    if (!sh) { snprintf(err, (size_t)esz, "cannot map shared memory"); return -1; }
    memset(sh, 0, sizeof *sh);
    sh->maxb = b->maxb;
    b->sh = sh;
    swprintf(nm, 128, L"Local\\%ls-req", base);
    b->ev_req = CreateEventW(NULL, FALSE, FALSE, nm);
    swprintf(nm, 128, L"Local\\%ls-done", base);
    b->ev_done = CreateEventW(NULL, FALSE, FALSE, nm);
    swprintf(nm, 128, L"\\\\.\\pipe\\%ls", base);
    b->sock = CreateNamedPipeW(nm, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 1 << 20, 1 << 20, 0, NULL);
    if (b->sock == INVALID_HANDLE_VALUE) b->sock = NULL;
    if (!b->ev_req || !b->ev_done || !b->sock) { snprintf(err, (size_t)esz, "cannot create the connection to the plugin's process"); return -1; }
    wchar_t cmd[MAX_PATH + 200];
    swprintf(cmd, MAX_PATH + 200, L"\"%ls\" --plugin-host %ls %lu", g_self, base, (unsigned long)GetCurrentProcessId());
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(g_self, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        snprintf(err, (size_t)esz, "cannot start a separate process");
        return -1;
    }
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    b->proc = pi.hProcess; b->pid = pi.dwProcessId;
    /* the child connects to the pipe */
    {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        BOOL ok = ConnectNamedPipe(b->sock, &ov);
        DWORD e = ok ? 0 : GetLastError();
        bool connected = ok || e == ERROR_PIPE_CONNECTED;
        if (!connected && e == ERROR_IO_PENDING) {
            HANDLE w[2] = { ov.hEvent, b->proc };
            DWORD got = 0;
            if (WaitForMultipleObjects(2, w, FALSE, 30000) == WAIT_OBJECT_0 && GetOverlappedResult(b->sock, &ov, &got, FALSE)) connected = true;
            else { CancelIo(b->sock); GetOverlappedResult(b->sock, &ov, &got, TRUE); }
        }
        CloseHandle(ov.hEvent);
        if (!connected) { snprintf(err, (size_t)esz, "the plugin's separate process did not start"); return -1; }
    }
#else
    b->pid = -1;
    if (!g_self[0]) {
        ssize_t k = readlink("/proc/self/exe", g_self, sizeof g_self - 1);
        if (k <= 0) { snprintf(err, (size_t)esz, "cannot find the Q-LiM program file"); return -1; }
        g_self[k] = 0;
    }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) { snprintf(err, (size_t)esz, "socketpair failed"); return -1; }
    int mfd = (int)syscall(SYS_memfd_create, "qlim-plugin", 1u /* MFD_CLOEXEC */);
    if (mfd < 0 || ftruncate(mfd, (off_t)b->shsz) != 0) {
        snprintf(err, (size_t)esz, "cannot create shared memory");
        if (mfd >= 0) close(mfd);
        close(sv[0]); close(sv[1]); return -1;
    }
    BrShm *sh = mmap(NULL, b->shsz, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (sh == MAP_FAILED) { snprintf(err, (size_t)esz, "cannot map shared memory"); close(mfd); close(sv[0]); close(sv[1]); return -1; }
    sh->maxb = b->maxb;
    b->sh = sh;
    /* the child gets the socket as fd 3 and the memory as fd 4 */
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 3);
    posix_spawn_file_actions_adddup2(&fa, mfd, 4);
    char *argv[] = { g_self, (char *)"--plugin-host", (char *)"3", (char *)"4", NULL };
    pid_t pid;
    int rc = posix_spawn(&pid, g_self, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(sv[1]); close(mfd);
    b->sock = sv[0];
    if (rc != 0) { snprintf(err, (size_t)esz, "cannot start a separate process"); return -1; }
    b->pid = pid;
#endif
    atomic_store(&b->dead, false);
    /* open */
    size_t fl = strlen(b->fmt) + 1, il = strlen(b->id) + 1;
    size_t ml = fl + il + sizeof(double) + sizeof(int32_t);
    unsigned char *m = malloc(ml);
    if (!m) { snprintf(err, (size_t)esz, "out of memory"); return -1; }
    int32_t mb = b->maxb;
    memcpy(m, b->fmt, fl); memcpy(m + fl, b->id, il); memcpy(m + fl + il, &b->rate, sizeof b->rate); memcpy(m + fl + il + sizeof b->rate, &mb, sizeof mb);
    unsigned char *r; uint32_t rl;
    int q = br_rpc(b, M_OPEN, m, ml, NULL, 0, &r, &rl, 60000);    /* a plugin through Wine can take a while */
    free(m);
    if (q != 0 || rl < sizeof(OpenReply)) {
        snprintf(err, (size_t)esz, "the plugin's separate process %s", b->why[0] ? b->why : "did not start");
        if (q == 0) free(r);
        return -1;
    }
    OpenReply o;
    memcpy(&o, r, sizeof o);
    free(r);
    if (!o.ok) { snprintf(err, (size_t)esz, "%s", o.err); return -1; }
    if (o.nin > BR_MAXCH || o.nout > BR_MAXCH) { snprintf(err, (size_t)esz, "too many channels"); return -1; }
    b->nin = o.nin; b->nout = o.nout; b->nparams = o.nparams;
    b->has_midi = o.has_midi; b->has_editor = o.has_editor;
    atomic_store(&b->ready, true);
    return 0;
}

/* after a crash / freeze: the plugin comes back with its last settings (at most 3 times a minute) */
static bool br_restart(Br *b)
{
    double tn = now_s();
    int recent = 0;
    for (int i = 0; i < 3; ++i) if (b->restarts[i] > 0 && tn - b->restarts[i] < 60.0) ++recent;
    if (recent >= 3) return false;
    int oldest = 0;
    for (int i = 1; i < 3; ++i) if (b->restarts[i] < b->restarts[oldest]) oldest = i;
    b->restarts[oldest] = tn;
    /* the knob positions just before (the child published them) - put back after the saved settings */
    int np = 0;
    float *pv = NULL;
    if (b->sh) {
        np = atomic_load(&b->sh->npub);
        if (np > BR_MAXP) np = BR_MAXP;
        pv = np > 0 ? malloc(sizeof(float) * (size_t)np) : NULL;
        if (pv) memcpy(pv, b->sh->pval, sizeof(float) * (size_t)np); else np = 0;
    }
    int nin = b->nin, nout = b->nout;
    br_release(b);
    char err[300];
    if (br_start(b, err, (int)sizeof err) != 0 || b->nin != nin || b->nout != nout) {
        br_release(b);
        free(pv);
        snprintf(b->why + strlen(b->why), sizeof b->why - strlen(b->why), " (restart failed)");
        return false;
    }
    if (b->last_state && b->last_len) {
        unsigned char *r; uint32_t rl;
        if (br_rpc(b, M_SETSTATE, b->last_state, b->last_len, NULL, 0, &r, &rl, 8000) == 0) free(r);
    }
    for (int i = 0; i < np && i < b->nparams; ++i) {
        struct { int32_t i; float v; } m = { i, pv[i] };
        br_rpc(b, M_PSET, &m, sizeof m, NULL, 0, NULL, NULL, 500);
    }
    free(pv);
    return !atomic_load(&b->dead);
}

/* main thread, every frame: the child's messages, is it alive, is it stuck */
static void br_main_idle(void *h)
{
    Br *b = h;
    if (b->told) return;                               /* gone for good */
    if (!atomic_load(&b->dead)) {
        for (int k = 0; k < 32; ++k) {
            int rd = io_readable(b->sock, 0);
            if (rd == 0) break;
            if (rd < 0) { br_mark_dead(b, "crashed"); break; }
            uint32_t t; unsigned char *d; uint32_t l;
            if (msg_recv(b->sock, &t, &d, &l, 2000) != 0) { br_mark_dead(b, "crashed"); break; }
            br_event(b, t, (const char *)d);
            free(d);
        }
    }
    if (!atomic_load(&b->dead) && b->ed_asked > 0 && now_s() - b->ed_asked > 15.0) br_mark_dead(b, "froze while opening its window");
    if (!atomic_load(&b->dead)) {
        bool crashed = false;
        if (br_child_ended(b, &crashed)) br_mark_dead(b, crashed ? "crashed" : "closed unexpectedly");
    }
    /* frozen = a block asked for and NO progress for 8 s (only slow - e.g. the CPU is overloaded - is not frozen).
     * While its window is opening the plugin may hold its sound back (JUCE does) - that has its own limit above. */
    if (!atomic_load(&b->dead) && b->sh) {
        int d = atomic_load(&b->sh->done);
        double tn = now_s();
        if (d != b->seen_done || d == b->sent || b->ed_asked > 0) { b->seen_done = d; b->seen_at = tn; }
        else if (b->seen_at > 0 && tn - b->seen_at > 8.0) br_mark_dead(b, "froze");
    }
    if (atomic_load(&b->dead)) {
        if (b->win) plugwin_request_close(b->win);
        char why[160];
        snprintf(why, sizeof why, "%s", b->why);
        char m[500];
        if (br_restart(b)) {
            snprintf(m, sizeof m, "Plugin '%s' %s - it ran in a separate process, so Q-LiM is fine. "
                     "It was started again with its last settings.", b->name, why);
            printf("WARNING: %s\n", m);
            qplug_report(m);
            return;
        }
        b->told = true;
        snprintf(m, sizeof m, "Plugin '%s' %s - it ran in a separate process, so Q-LiM is fine. It is silent now; "
                 "remove it and add it again to restart it.", b->name, b->why);
        printf("WARNING: %s\n", m);
        qplug_report(m);
    }
}

static void br_destroy(void *h)
{
    Br *b = h;
    if (!b) return;
    if (!atomic_load(&b->dead) && br_child_running(b) && b->sh) {
        atomic_store(&b->sh->quit, 1);
        br_wake_child(b);
        msg_send(b->sock, M_QUIT, NULL, 0, NULL, 0, 300);
        br_child_wait(b, 1000);                          /* up to 1 s to finish nicely */
    }
    br_release(b);
    free(b->pname); free(b->pname_ok); free(b->ptxt); free(b->ptxt_v);
    free(b->last_state);
    free(b);
}

int bridge_open(const char *fmt, const char *id, double rate, int maxb, QInst *out, char *err, int esz)
{
    Br *b = calloc(1, sizeof *b);
    if (!b) { snprintf(err, (size_t)esz, "out of memory"); return -1; }
    snprintf(b->fmt, sizeof b->fmt, "%s", fmt);
    snprintf(b->id, sizeof b->id, "%s", id);
    b->rate = rate; b->maxb = maxb;
    {                                                    /* a short name for messages: the file / bundle name */
        char t[1300];
        snprintf(t, sizeof t, "%s", id);
        char *bar = strchr(t, '|');
        if (bar) *bar = 0;
        size_t tl = strlen(t);
        while (tl > 1 && t[tl - 1] == '/') t[--tl] = 0;
        char *base = strrchr(t, '/');
        base = base ? base + 1 : t;
        char *dot = strrchr(base, '.');
        if (dot && dot != base) *dot = 0;
        snprintf(b->name, sizeof b->name, "%s", base);
    }
    if (br_start(b, err, esz) != 0) { br_destroy(b); return -1; }
    int np = b->nparams < BR_MAXP ? b->nparams : BR_MAXP;
    if (np > 0) {
        b->pname = calloc((size_t)np, sizeof *b->pname);
        b->pname_ok = calloc((size_t)np, sizeof *b->pname_ok);
        b->ptxt = calloc((size_t)np, sizeof *b->ptxt);
        b->ptxt_v = calloc((size_t)np, sizeof *b->ptxt_v);
    }
    memset(out, 0, sizeof *out);
    out->h = b;
    out->nin = b->nin; out->nout = b->nout;
    out->process = br_process;
    out->destroy = br_destroy;
    out->has_editor = b->has_editor ? br_has_editor : NULL;
    out->ed_open = br_ed_open;
    out->ed_close = br_ed_close;
    out->main_idle = br_main_idle;
    out->get_state = br_get_state;
    out->set_state = br_set_state;
    out->midi = b->has_midi ? br_midi : NULL;
    out->param_count = br_param_count;
    out->param_name = br_param_name;
    out->param_get = br_param_get;
    out->param_set = br_param_set;
    out->param_text = br_param_text;
    out->separate = true;
    return 0;
}

/* ======================================================================================================
 * CHILD side: "qlim --plugin-host 3 4"
 * ====================================================================================================== */
static QInst  c_in;
static BrShm *c_sh;
static BrFd   c_sock = BR_NOFD;
static PlugWin *c_win;
#ifdef _WIN32
static HANDLE c_ev_req, c_ev_done, c_parent;
static void c_wait_req(int seen) { (void)seen; WaitForSingleObject(c_ev_req, 500); }
static void c_wake_done(void) { SetEvent(c_ev_done); }
#else
static void c_wait_req(int seen) { fx_wait(&c_sh->req, seen, 500000000L); }
static void c_wake_done(void) { fx_wake(&c_sh->done); }
#endif

static void *child_audio(void *arg)
{
    (void)arg;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#else
    struct sched_param sp = { .sched_priority = 70 };          /* like Q-LiM's own audio thread (when allowed) */
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
#endif
    int seen = atomic_load(&c_sh->done);          /* a block asked for before this thread started is done too */
    /* the first BR_SHMCH channels are in the shared memory, the others are local (made once, here) */
    int nb_in = c_in.nin > BR_SHMCH ? c_in.nin : BR_SHMCH, nb_out = c_in.nout > BR_SHMCH ? c_in.nout : BR_SHMCH;
    float **in = calloc((size_t)nb_in, sizeof(float *)), **out = calloc((size_t)nb_out, sizeof(float *));
    float *extra = calloc((size_t)(nb_in + nb_out - 2 * BR_SHMCH) * (size_t)c_sh->maxb + 1, sizeof(float));
    if (!in || !out || !extra) return NULL;
    for (int c = 0; c < nb_in; ++c)
        in[c] = c < BR_SHMCH ? shm_in(c_sh, c) : extra + (size_t)(c - BR_SHMCH) * (size_t)c_sh->maxb;
    for (int c = 0; c < nb_out; ++c)
        out[c] = c < BR_SHMCH ? shm_out(c_sh, c) : extra + (size_t)(nb_in - BR_SHMCH + c - BR_SHMCH) * (size_t)c_sh->maxb;
    for (;;) {
        while (atomic_load(&c_sh->req) == seen) {
            if (atomic_load(&c_sh->quit)) return NULL;
            c_wait_req(seen);
        }
        if (atomic_load(&c_sh->quit)) return NULL;
        seen = atomic_load(&c_sh->req);
        unsigned n = (unsigned)c_sh->n;
        if (n > (unsigned)c_sh->maxb) n = (unsigned)c_sh->maxb;
        if (c_sh->nmidi > 0 && c_in.midi) c_in.midi(c_in.h, c_sh->midi, c_sh->nmidi < QMIDI_MAX ? c_sh->nmidi : QMIDI_MAX);
        for (int c = BR_SHMCH; c < c_in.nin; ++c) memset(in[c], 0, n * sizeof(float));   /* silence on the extra inputs */
        for (int c = 0; c < c_in.nout; ++c) memset(out[c], 0, n * sizeof(float));
        c_in.process(c_in.h, in, out, n);
        atomic_store(&c_sh->done, seen);
        c_wake_done();
    }
}

static int reply(const void *a, size_t al, const void *b, size_t bl)
{
    return msg_send(c_sock, M_REPLY, a, al, b, bl, 10000);
}

static void child_close_editor(void)
{
    if (!c_win) return;
    if (c_in.ed_close) c_in.ed_close(c_in.h);
    plugwin_destroy(c_win);
    c_win = NULL;
}

static void child_publish(void)
{
    int n = c_in.param_count ? c_in.param_count(c_in.h) : 0;
    if (n > BR_MAXP) n = BR_MAXP;
    for (int i = 0; i < n; ++i) c_sh->pval[i] = c_in.param_get ? c_in.param_get(c_in.h, i) : 0.0f;
    atomic_store(&c_sh->npub, n);
    atomic_fetch_add(&c_sh->pgen, 1);
}

static bool child_message(void)
{
    uint32_t t; unsigned char *d; uint32_t l;
    if (msg_recv(c_sock, &t, &d, &l, 10000) != 0) return false;
    bool go_on = true;
    switch (t) {
    case M_PNAME: {
        int32_t i = 0; char nm[256] = "";
        if (l >= sizeof i) memcpy(&i, d, sizeof i);
        if (c_in.param_name) c_in.param_name(c_in.h, i, nm, (int)sizeof nm);
        reply(nm, strlen(nm) + 1, NULL, 0);
        break;
    }
    case M_PTEXT: {
        int32_t i = 0; char tx[256] = "";
        if (l >= sizeof i) memcpy(&i, d, sizeof i);
        if (c_in.param_text) c_in.param_text(c_in.h, i, tx, (int)sizeof tx);
        reply(tx, strlen(tx) + 1, NULL, 0);
        break;
    }
    case M_PSET: {
        struct { int32_t i; float v; } m;
        if (l >= sizeof m) { memcpy(&m, d, sizeof m); if (c_in.param_set) c_in.param_set(c_in.h, m.i, m.v); }
        break;
    }
    case M_GETSTATE: {
        void *sd = NULL; size_t sl = 0;
        int32_t rc = c_in.get_state ? c_in.get_state(c_in.h, &sd, &sl) : -1;
        reply(&rc, sizeof rc, rc == 0 ? sd : NULL, rc == 0 ? sl : 0);
        free(sd);
        break;
    }
    case M_SETSTATE: {
        int32_t rc = c_in.set_state ? c_in.set_state(c_in.h, d, l) : -1;
        reply(&rc, sizeof rc, NULL, 0);
        child_publish();
        break;
    }
    case M_EDOPEN: {
        int32_t rc = -1;
        char err[300] = "";
        uint64_t xid = 0;
        if (l >= sizeof xid) memcpy(&xid, d, sizeof xid);
        const char *title = l > sizeof xid ? (const char *)d + sizeof xid : "Plugin";
        if (c_win) { plugwin_raise(c_win); rc = 0; }
        else if (!c_in.has_editor || !c_in.ed_open || !c_in.has_editor(c_in.h)) snprintf(err, sizeof err, "this plugin has no window of its own");
        else if (!plugwin_available()) snprintf(err, sizeof err, "plugin windows are not possible here (no display)");
        else {
            plugwin_set_main_xid((unsigned long)xid);
            PlugWin *w = plugwin_create(title, 640, 400);
            int W = 0, H = 0;
            if (!w) snprintf(err, sizeof err, "cannot create a window");
            else if (c_in.ed_open(c_in.h, w, &W, &H, err, (int)sizeof err) != 0) plugwin_destroy(w);
            else {
                if (W <= 0 || H <= 0) plugwin_child_size(w, &W, &H);
                if (W > 0 && H > 0) plugwin_resize(w, W, H);
                plugwin_show(w);
                c_win = w;
                rc = 0;
            }
        }
        if (rc == 0) msg_send(c_sock, E_OPENED, NULL, 0, NULL, 0, 2000);
        else msg_send(c_sock, E_OPENFAIL, err, strlen(err) + 1, NULL, 0, 2000);
        break;
    }
    case M_EDCLOSE: {
        child_close_editor();
        int32_t rc = 0;
        reply(&rc, sizeof rc, NULL, 0);
        break;
    }
    case M_EDRAISE:
        if (c_win) plugwin_raise(c_win);
        break;
    case M_QUIT:
        go_on = false;
        break;
    default:
        break;
    }
    free(d);
    return go_on;
}

#ifdef _WIN32
static LONG WINAPI child_crash(EXCEPTION_POINTERS *ep)       /* no "stopped working" box: just end, Q-LiM sees it */
{
    TerminateProcess(GetCurrentProcess(), ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 3);
    return EXCEPTION_EXECUTE_HANDLER;
}
static void child_abort(int sig) { (void)sig; TerminateProcess(GetCurrentProcess(), 3); }
#endif

int qplug_host_child_main(int argc, char **argv)
{
    if (argc < 4) return 2;
    qplug_mark_child();
#ifdef _WIN32
    /* qlim.exe --plugin-host <base name> <Q-LiM's process id> */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(child_crash);
    timeBeginPeriod(1);                                  /* Sleep(1) = 1 ms: messages from Q-LiM are taken at once */
    signal(SIGABRT, child_abort);
    wchar_t base[96], nm[128];
    MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, base, 96);
    c_parent = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)strtoul(argv[3], NULL, 10));
    swprintf(nm, 128, L"Local\\%ls-shm", base);
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, nm);
    if (!map) return 3;
    c_sh = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!c_sh) return 3;
    swprintf(nm, 128, L"Local\\%ls-req", base);
    c_ev_req = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, nm);
    swprintf(nm, 128, L"Local\\%ls-done", base);
    c_ev_done = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, nm);
    swprintf(nm, 128, L"\\\\.\\pipe\\%ls", base);
    for (int i = 0; i < 100; ++i) {
        c_sock = CreateFileW(nm, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (c_sock != INVALID_HANDLE_VALUE) break;
        c_sock = NULL;
        WaitNamedPipeW(nm, 100);
    }
    if (!c_sock || !c_ev_req || !c_ev_done) return 3;
#else
    c_sock = atoi(argv[2]);
    int mfd = atoi(argv[3]);
    prctl(PR_SET_PDEATHSIG, SIGKILL);                       /* Q-LiM gone -> this one goes too */
    signal(SIGPIPE, SIG_IGN);
    struct stat st;
    if (fstat(mfd, &st) != 0) return 3;
    c_sh = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (c_sh == MAP_FAILED) return 3;
    close(mfd);
#endif
    /* 1) the plugin */
    uint32_t t; unsigned char *d; uint32_t l;
    if (msg_recv(c_sock, &t, &d, &l, 60000) != 0 || t != M_OPEN) return 4;
    const char *fmt = (const char *)d;
    const char *id = fmt + strlen(fmt) + 1;
    double rate = 48000.0; int32_t mb = 1024;
    size_t off = strlen(fmt) + 1 + strlen(id) + 1;
    if (l >= off + sizeof rate + sizeof mb) { memcpy(&rate, d + off, sizeof rate); memcpy(&mb, d + off + sizeof rate, sizeof mb); }
    if (mb > c_sh->maxb) mb = c_sh->maxb;
    OpenReply o;
    memset(&o, 0, sizeof o);
    clap_host_set_main_thread();
    if (qplug_open_inst_here(fmt, id, rate, mb, &c_in, o.err, (int)sizeof o.err) == 0) {
        if (c_in.nin > BR_MAXCH || c_in.nout > BR_MAXCH) snprintf(o.err, sizeof o.err, "too many channels (%d in / %d out)", c_in.nin, c_in.nout);
        else {
            o.ok = 1;
            o.nin = c_in.nin; o.nout = c_in.nout;
            o.has_midi = c_in.midi != NULL;
            o.has_editor = c_in.has_editor && c_in.ed_open && c_in.has_editor(c_in.h);
            o.nparams = c_in.param_count ? c_in.param_count(c_in.h) : 0;
        }
    }
    free(d);
    if (o.ok) child_publish();
    reply(&o, sizeof o, NULL, 0);
    if (!o.ok) return 5;
    pthread_t at;
    if (pthread_create(&at, NULL, child_audio, NULL) != 0) return 6;
    /* 2) requests, window, idle */
    unsigned last_edit = qplug_edit_count();
    double next_pub = 0;
    for (;;) {
        int r = io_readable(c_sock, 15);
        if (r < 0) break;
        if (r > 0 && !child_message()) break;
        clap_host_set_main_thread();
#ifdef _WIN32
        MSG wm;                                          /* this process has no SDL: the window messages are ours */
        for (int k = 0; k < 200 && PeekMessageW(&wm, NULL, 0, 0, PM_REMOVE); ++k) { TranslateMessage(&wm); DispatchMessageW(&wm); }
#endif
        plugwin_poll();
        if (c_win && plugwin_close_requested(c_win)) {
            child_close_editor();
            msg_send(c_sock, E_CLOSED, NULL, 0, NULL, 0, 2000);
        }
        if (c_in.main_idle) c_in.main_idle(c_in.h);
        if (c_win && c_in.ed_idle) c_in.ed_idle(c_in.h);
        unsigned ec = qplug_edit_count();
        if (ec != last_edit) { last_edit = ec; msg_send(c_sock, E_EDIT, NULL, 0, NULL, 0, 2000); }
        double tn = now_s();
        if (tn >= next_pub) { next_pub = tn + 0.1; child_publish(); }
#ifdef _WIN32
        if (c_parent && WaitForSingleObject(c_parent, 0) == WAIT_OBJECT_0) break;
#else
        if (getppid() == 1) break;
#endif
    }
    atomic_store(&c_sh->quit, 1);
#ifdef _WIN32
    SetEvent(c_ev_req);
#else
    fx_wake(&c_sh->req);
#endif
    pthread_join(at, NULL);
    child_close_editor();
    if (c_in.destroy) c_in.destroy(c_in.h);
    return 0;
}
