/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Plugin windows (X11) + run loop - see plugwin.h */
#define _GNU_SOURCE
#include "plugwin.h"
#include <SDL.h>
#include <SDL_syswm.h>
void *GfxSdlWindow(void);   /* sdlgfx.c (sdlgfx.h is not included: its Font clashes with X11) */

/* X11 id of the Q-LiM main window (0 = not X11) */
static unsigned long main_xwindow(void)
{
    SDL_Window *w = GfxSdlWindow();
    if (!w) return 0;
    SDL_SysWMinfo wi;
    SDL_VERSION(&wi.version);
    if (!SDL_GetWindowWMInfo(w, &wi) || wi.subsystem != SDL_SYSWM_X11) return 0;
    return (unsigned long)wi.info.x11.window;
}
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct PlugWin {
    Window win;
    int    w, h;
    bool   close_req;
    bool   mapped;
};

static Display *g_dpy;
static int      g_tried;
static Atom     a_delete, a_protocols, a_wtype, a_wtype_dialog, a_wm_name, a_utf8;

#define MAX_WIN 64
static PlugWin *g_wins[MAX_WIN];
static int      g_nwin;

static int x_error(Display *d, XErrorEvent *e)
{
    char msg[160];
    XGetErrorText(d, e->error_code, msg, sizeof msg);
    fprintf(stderr, ">>> X11 error (ignored): %s (request %d.%d)\n", msg, e->request_code, e->minor_code);
    return 0;
}

static bool init(void)
{
    if (g_tried) return g_dpy != NULL;
    g_tried = 1;
    XInitThreads();
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) { printf(">>> Plugin windows: no X11 display - plugin interfaces cannot be shown\n"); return false; }
    XSetErrorHandler(x_error);
    a_delete = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
    a_protocols = XInternAtom(g_dpy, "WM_PROTOCOLS", False);
    a_wtype = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE", False);
    a_wtype_dialog = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    a_wm_name = XInternAtom(g_dpy, "_NET_WM_NAME", False);
    a_utf8 = XInternAtom(g_dpy, "UTF8_STRING", False);
    return true;
}

bool plugwin_available(void) { return init(); }
void *plugwin_display(void) { init(); return g_dpy; }

static void set_fixed_size(PlugWin *w)
{
    XSizeHints *hints = XAllocSizeHints();
    if (!hints) return;
    hints->flags = PMinSize | PMaxSize;
    hints->min_width = hints->max_width = w->w;
    hints->min_height = hints->max_height = w->h;
    XSetWMNormalHints(g_dpy, w->win, hints);
    XFree(hints);
}

PlugWin *plugwin_create(const char *title, int w, int h)
{
    if (!init() || g_nwin >= MAX_WIN) return NULL;
    if (w < 50) w = 50;
    if (h < 30) h = 30;
    PlugWin *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->w = w; p->h = h;
    int scr = DefaultScreen(g_dpy);
    XSetWindowAttributes at;
    memset(&at, 0, sizeof at);
    at.background_pixel = BlackPixel(g_dpy, scr);
    at.event_mask = StructureNotifyMask;
    p->win = XCreateWindow(g_dpy, RootWindow(g_dpy, scr), 0, 0, (unsigned)w, (unsigned)h, 0,
                           CopyFromParent, InputOutput, CopyFromParent, CWBackPixel | CWEventMask, &at);
    XStoreName(g_dpy, p->win, title);
    XChangeProperty(g_dpy, p->win, a_wm_name, a_utf8, 8, PropModeReplace, (const unsigned char *)title, (int)strlen(title));
    XSetWMProtocols(g_dpy, p->win, &a_delete, 1);
    XChangeProperty(g_dpy, p->win, a_wtype, XA_ATOM, 32, PropModeReplace, (unsigned char *)&a_wtype_dialog, 1);
    XClassHint *ch = XAllocClassHint();
    if (ch) { ch->res_name = (char *)"qlim-plugin"; ch->res_class = (char *)"Q-LiM"; XSetClassHint(g_dpy, p->win, ch); XFree(ch); }
    unsigned long mainw = main_xwindow();
    if (mainw) XSetTransientForHint(g_dpy, p->win, (Window)mainw);   /* stays above Q-LiM */
    set_fixed_size(p);
    XFlush(g_dpy);
    g_wins[g_nwin++] = p;
    return p;
}

void plugwin_show(PlugWin *w)
{
    if (!w) return;
    XMapRaised(g_dpy, w->win);
    w->mapped = true;
    XFlush(g_dpy);
}

void plugwin_raise(PlugWin *w) { if (w) { XMapRaised(g_dpy, w->win); XFlush(g_dpy); } }
unsigned long plugwin_xid(PlugWin *w) { return w ? (unsigned long)w->win : 0; }

void plugwin_resize(PlugWin *w, int width, int height)
{
    if (!w || width <= 0 || height <= 0) return;
    w->w = width; w->h = height;
    set_fixed_size(w);
    XResizeWindow(g_dpy, w->win, (unsigned)width, (unsigned)height);
    XFlush(g_dpy);
}

void plugwin_get_size(PlugWin *w, int *width, int *height) { if (w) { *width = w->w; *height = w->h; } }
bool plugwin_close_requested(PlugWin *w) { return w && w->close_req; }

bool plugwin_child_size(PlugWin *w, int *width, int *height)
{
    if (!w) return false;
    Window root, parent, *kids = NULL;
    unsigned nk = 0;
    if (!XQueryTree(g_dpy, w->win, &root, &parent, &kids, &nk) || nk == 0) { if (kids) XFree(kids); return false; }
    Window r; int x, y; unsigned cw, chh, b, d;
    bool ok = XGetGeometry(g_dpy, kids[0], &r, &x, &y, &cw, &chh, &b, &d) != 0;
    XFree(kids);
    if (ok && cw > 1 && chh > 1) { *width = (int)cw; *height = (int)chh; return true; }
    return false;
}

void plugwin_destroy(PlugWin *w)
{
    if (!w) return;
    for (int i = 0; i < g_nwin; ++i) if (g_wins[i] == w) { g_wins[i] = g_wins[--g_nwin]; break; }
    XDestroyWindow(g_dpy, w->win);
    XFlush(g_dpy);
    free(w);
}

/* ---- run loop ---- */
typedef struct { int fd; PlugFdCb cb; void *ud; bool dead; } FdEnt;
typedef struct { unsigned id, period; double next; PlugTimerCb cb; void *ud; bool dead; } TmEnt;
#define MAX_FD 128
#define MAX_TM 256
static FdEnt g_fd[MAX_FD]; static int g_nfd;
static TmEnt g_tm[MAX_TM]; static int g_ntm;
static unsigned g_tm_next = 1;

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

void plugwin_add_fd(int fd, PlugFdCb cb, void *ud)
{
    if (g_nfd >= MAX_FD) return;
    g_fd[g_nfd++] = (FdEnt){ fd, cb, ud, false };
}
void plugwin_remove_fd(int fd, void *ud)
{
    for (int i = 0; i < g_nfd; ++i) if (g_fd[i].fd == fd && g_fd[i].ud == ud) g_fd[i].dead = true;
}
void plugwin_remove_fds_of(void *ud) { for (int i = 0; i < g_nfd; ++i) if (g_fd[i].ud == ud) g_fd[i].dead = true; }

unsigned plugwin_add_timer(unsigned period_ms, PlugTimerCb cb, void *ud)
{
    if (g_ntm >= MAX_TM) return 0;
    if (period_ms < 1) period_ms = 1;
    unsigned id = g_tm_next++;
    if (g_tm_next == 0) g_tm_next = 1;
    g_tm[g_ntm++] = (TmEnt){ id, period_ms, now_ms() + period_ms, cb, ud, false };
    return id;
}
void plugwin_remove_timer(unsigned id) { for (int i = 0; i < g_ntm; ++i) if (g_tm[i].id == id) g_tm[i].dead = true; }
void plugwin_remove_timers_of(void *ud) { for (int i = 0; i < g_ntm; ++i) if (g_tm[i].ud == ud) g_tm[i].dead = true; }

static void compact(void)
{
    int w = 0;
    for (int i = 0; i < g_nfd; ++i) if (!g_fd[i].dead) g_fd[w++] = g_fd[i];
    g_nfd = w;
    w = 0;
    for (int i = 0; i < g_ntm; ++i) if (!g_tm[i].dead) g_tm[w++] = g_tm[i];
    g_ntm = w;
}

void plugwin_poll(void)
{
    if (g_dpy) {
        while (XPending(g_dpy)) {
            XEvent ev;
            XNextEvent(g_dpy, &ev);
            for (int i = 0; i < g_nwin; ++i) {
                PlugWin *p = g_wins[i];
                if (ev.xany.window != p->win) continue;
                if (ev.type == ClientMessage && ev.xclient.message_type == a_protocols &&
                    (Atom)ev.xclient.data.l[0] == a_delete) p->close_req = true;
            }
        }
    }
    /* descriptors: the callback may remove/add -> only up to the count from before */
    int n = g_nfd;
    if (n > 0) {
        struct pollfd pf[MAX_FD];
        for (int i = 0; i < n; ++i) { pf[i].fd = g_fd[i].dead ? -1 : g_fd[i].fd; pf[i].events = POLLIN; pf[i].revents = 0; }
        if (poll(pf, (nfds_t)n, 0) > 0)
            for (int i = 0; i < n; ++i)
                if (!g_fd[i].dead && (pf[i].revents & (POLLIN | POLLHUP | POLLERR))) g_fd[i].cb(g_fd[i].ud, g_fd[i].fd);
    }
    double t = now_ms();
    n = g_ntm;
    for (int i = 0; i < n; ++i) {
        TmEnt *e = &g_tm[i];
        if (e->dead || t < e->next) continue;
        e->next = t + e->period;
        e->cb(e->ud, e->id);
    }
    compact();
}
