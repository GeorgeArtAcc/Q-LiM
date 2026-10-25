/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef PLUGWIN_H
#define PLUGWIN_H
/*
 * Plugin windows (step 35): separate X11 windows into which the plugin "embeds" its interface.
 * Plus a shared "run loop": plugins (VST3 IRunLoop, CLAP posix-fd/timer) register file descriptors
 * and timers that the main thread checks every frame (plugwin_poll).
 * Main thread only.
 */
#include <stdbool.h>

typedef struct PlugWin PlugWin;

bool          plugwin_available(void);                 /* is there X11 (on Wayland without XWayland - no) */
void         *plugwin_display(void);                   /* Display* (for VST2 plugins that want it) */
PlugWin      *plugwin_create(const char *title, int w, int h);
void          plugwin_show(PlugWin *w);
void          plugwin_raise(PlugWin *w);
unsigned long plugwin_xid(PlugWin *w);
void          plugwin_resize(PlugWin *w, int width, int height);
void          plugwin_get_size(PlugWin *w, int *width, int *height);
bool          plugwin_close_requested(PlugWin *w);     /* the user pressed X on the window */
void          plugwin_destroy(PlugWin *w);
#ifdef _WIN32
void         *plugwin_native(PlugWin *w);              /* Windows: the HWND the plugin attaches to */
#endif
/* the size of the first "child" window (the plugin's interface) - for plugins that do not report their size */
bool          plugwin_child_size(PlugWin *w, int *width, int *height);

/* run loop */
typedef void (*PlugFdCb)(void *ud, int fd);
typedef void (*PlugTimerCb)(void *ud, unsigned id);
void     plugwin_add_fd(int fd, PlugFdCb cb, void *ud);
void     plugwin_remove_fd(int fd, void *ud);
void     plugwin_remove_fds_of(void *ud);
unsigned plugwin_add_timer(unsigned period_ms, PlugTimerCb cb, void *ud);   /* 0 = error */
void     plugwin_remove_timer(unsigned id);
void     plugwin_remove_timers_of(void *ud);

void     plugwin_poll(void);                           /* X events + descriptors + timers (every frame) */

#endif
