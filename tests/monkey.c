/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/*
 * 2.1: a "monkey" for stability tests - random mouse and keyboard input into the Q-LiM window (X11 + XTest).
 *
 *   monkey SECONDS SEED [PID]
 *
 * Clicks, double clicks, right clicks, drags (also with Shift/Ctrl), the wheel and keys (letters, digits, Space,
 * Enter, Esc, Delete, arrows, Ctrl+Z/Y/C/X/V/A/D, B, Tab ...) all over the window - menus, dialogs, the waveform, the
 * MIDI editor ... Never Ctrl+Q, Ctrl+W, Alt+F4 or Ctrl+N/O/S (those would end the test or block it with file dialogs).
 * With PID: stops as soon as that process is gone (a crash) and prints how many events were sent.
 * Every 2 s it may press Escape (closes stuck menus) so the run keeps exploring.
 *
 * Build:  cc -O2 -o monkey tests/monkey.c -lX11 -lXtst
 */
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

Bool XTestFakeMotionEvent(Display *, int, int, int, unsigned long);
Bool XTestFakeButtonEvent(Display *, unsigned int, Bool, unsigned long);
Bool XTestFakeKeyEvent(Display *, unsigned int, Bool, unsigned long);

static Display *d;
static int W = 1600, H = 1000;
static unsigned long long rs;
static int rnd(int n) { rs = rs * 6364136223846793005ULL + 1442695040888963407ULL; return (int)((rs >> 33) % (unsigned)n); }
static void slp(int ms) { usleep((useconds_t)ms * 1000); }
static void mv(int x, int y) { XTestFakeMotionEvent(d, -1, x, y, 0); XFlush(d); slp(4); }
static void bt(int b, int p) { XTestFakeButtonEvent(d, (unsigned)b, p, 0); XFlush(d); slp(8); }
static void ky(KeySym ks, int p) { KeyCode kc = XKeysymToKeycode(d, ks); if (kc) { XTestFakeKeyEvent(d, kc, p, 0); XFlush(d); } slp(6); }
static void tap(KeySym ks) { ky(ks, 1); ky(ks, 0); }

/* where to click: half the time anywhere, else the busy parts (menu bar, clip grid, waveform/MIDI editor, footer) */
static void pt(int *x, int *y)
{
    int r = rnd(100);
    if (r < 40)      { *x = rnd(W); *y = rnd(H); }
    else if (r < 50) { *x = rnd(400); *y = rnd(30); }                         /* menu bar */
    else if (r < 70) { *x = rnd(W); *y = 70 + rnd(H / 2); }                     /* clips and channels */
    else             { *x = rnd(W); *y = H / 2 + rnd(H / 2); }                  /* waveform / MIDI editor / dialogs */
}

static const KeySym KEYS[] = {
    XK_a, XK_b, XK_c, XK_d, XK_e, XK_f, XK_g, XK_h, XK_i, XK_j, XK_k, XK_l, XK_m, XK_p, XK_r, XK_s, XK_t, XK_u, XK_v, XK_x,
    XK_y, XK_z, XK_1, XK_2, XK_3, XK_5, XK_8, XK_0, XK_space, XK_Return, XK_Escape, XK_Delete, XK_BackSpace, XK_Tab,
    XK_Left, XK_Right, XK_Up, XK_Down, XK_Home, XK_End, XK_plus, XK_minus, XK_period, XK_F1, XK_F2,
};
static const KeySym CTRL_KEYS[] = { XK_z, XK_y, XK_c, XK_x, XK_v, XK_a, XK_d, XK_e, XK_l, XK_m, XK_f, XK_b };

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: monkey SECONDS SEED [PID]\n"); return 2; }
    double secs = atof(argv[1]);
    rs = strtoull(argv[2], NULL, 10) * 2654435761ULL + 1;
    int pid = argc > 3 ? atoi(argv[3]) : 0;
    d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "no display\n"); return 1; }
    W = DisplayWidth(d, DefaultScreen(d)); H = DisplayHeight(d, DefaultScreen(d));
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    struct timeval t0, t; gettimeofday(&t0, NULL);
    long n = 0; double last_esc = 0;
    for (;;) {
        gettimeofday(&t, NULL);
        double el = (double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_usec - t0.tv_usec) / 1e6;
        if (el >= secs || g_stop) break;
        if (pid && kill(pid, 0) != 0) { printf("monkey: the process %d is gone after %.1f s, %ld events\n", pid, el, n); return 3; }
        if (el - last_esc > 2.0) { last_esc = el; if (rnd(3) == 0) { tap(XK_Escape); tap(XK_Escape); } }
        int x, y, x2, y2, a = rnd(100);
        pt(&x, &y);
        if (a < 35) { mv(x, y); bt(1, 1); bt(1, 0); }
        else if (a < 42) { mv(x, y); bt(1, 1); bt(1, 0); bt(1, 1); bt(1, 0); }
        else if (a < 50) {                                                          /* a right click, sometimes a menu item */
            mv(x, y); bt(3, 1); bt(3, 0); slp(30);
            if (rnd(2)) { mv(x + 10 + rnd(140), y + 5 + rnd(260)); bt(1, 1); bt(1, 0); }
        }
        else if (a < 64) {                                                          /* a drag, sometimes with a modifier */
            KeySym mod = rnd(4) == 0 ? XK_Shift_L : rnd(5) == 0 ? XK_Control_L : 0;
            pt(&x2, &y2);
            if (rnd(2)) { x2 = x + rnd(301) - 150; y2 = y + rnd(201) - 100; }
            if (mod) ky(mod, 1);
            mv(x, y); bt(1, 1);
            int st = 3 + rnd(10);
            for (int k = 1; k <= st; ++k) mv(x + (x2 - x) * k / st, y + (y2 - y) * k / st);
            bt(1, 0);
            if (mod) ky(mod, 0);
        }
        else if (a < 72) {                                                          /* the wheel, sometimes with Ctrl / Shift */
            mv(x, y);
            int b = rnd(2) ? 4 : 5, k = 1 + rnd(6);
            KeySym mod = rnd(3) == 0 ? XK_Control_L : rnd(4) == 0 ? XK_Shift_L : 0;
            if (mod) ky(mod, 1);
            while (k--) { bt(b, 1); bt(b, 0); }
            if (mod) ky(mod, 0);
        }
        else if (a < 76) { mv(x, y); }                                              /* hover (tooltips, ghosts) */
        else if (a < 92) tap(KEYS[rnd((int)(sizeof KEYS / sizeof KEYS[0]))]);
        else if (a < 97) { ky(XK_Control_L, 1); tap(CTRL_KEYS[rnd((int)(sizeof CTRL_KEYS / sizeof CTRL_KEYS[0]))]); ky(XK_Control_L, 0); }
        else { mv(x, y); bt(2, 1); bt(2, 0); }                                     /* the middle button */
        ++n;
        slp(rnd(3) == 0 ? 40 + rnd(120) : 10);
    }
    /* never leave a key or a button pressed */
    ky(XK_Shift_L, 0); ky(XK_Control_L, 0); bt(1, 0);
    printf("monkey: %ld events\n", n);
    XCloseDisplay(d);
    return 0;
}
