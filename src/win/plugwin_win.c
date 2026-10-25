/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/*
 * Plugin windows on Windows - see plugwin.h. A plain Win32 window (owned by the Q-LiM window, so it stays above it)
 * into which the plugin puts its interface as a child window (VST2 effEditOpen, VST3 kPlatformTypeHWND,
 * CLAP CLAP_WINDOW_API_WIN32). The window messages are dispatched by SDL's message loop (same thread).
 * The "run loop" here only has timers (CLAP timer-support); file descriptors do not exist on Windows.
 */
#undef stat
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../plugwin.h"
#include <SDL.h>
#include <SDL_syswm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *GfxSdlWindow(void);   /* sdlgfx.c */

struct PlugWin {
    HWND hwnd;
    int  w, h;
    bool close_req;
};

#define WCLASS L"QLiMPluginWindow"
static const DWORD k_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
static const DWORD k_exstyle = WS_EX_DLGMODALFRAME;

static HWND main_hwnd(void)
{
    SDL_Window *w = GfxSdlWindow();
    if (!w) return NULL;
    SDL_SysWMinfo wi;
    SDL_VERSION(&wi.version);
    if (!SDL_GetWindowWMInfo(w, &wi) || wi.subsystem != SDL_SYSWM_WINDOWS) return NULL;
    return wi.info.win.window;
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    PlugWin *pw = (PlugWin *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_CLOSE:                                   /* X: Q-LiM closes it (qplug_idle_all), the plugin first */
        if (pw) pw->close_req = true;
        return 0;
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(h, &r);
        FillRect((HDC)wp, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    default:
        return DefWindowProcW(h, msg, wp, lp);
    }
}

static bool g_class_ok;
static bool init(void)
{
    if (g_class_ok) return true;
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = WCLASS;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    g_class_ok = true;
    return true;
}

bool plugwin_available(void) { return init(); }
void *plugwin_display(void) { return NULL; }

/* outer size for a given inner (client) size */
static void outer_size(int w, int h, int *ow, int *oh)
{
    RECT r = { 0, 0, w, h };
    AdjustWindowRectEx(&r, k_style, FALSE, k_exstyle);
    *ow = r.right - r.left;
    *oh = r.bottom - r.top;
}

PlugWin *plugwin_create(const char *title, int w, int h)
{
    if (!init()) return NULL;
    PlugWin *pw = calloc(1, sizeof *pw);
    if (!pw) return NULL;
    pw->w = w > 0 ? w : 640;
    pw->h = h > 0 ? h : 400;
    wchar_t wt[512];
    if (!MultiByteToWideChar(CP_UTF8, 0, title ? title : "Plugin", -1, wt, 512)) wcscpy(wt, L"Plugin");
    int ow, oh;
    outer_size(pw->w, pw->h, &ow, &oh);
    pw->hwnd = CreateWindowExW(k_exstyle, WCLASS, wt, k_style, CW_USEDEFAULT, CW_USEDEFAULT, ow, oh,
                               main_hwnd(), NULL, GetModuleHandleW(NULL), NULL);
    if (!pw->hwnd) { free(pw); return NULL; }
    SetWindowLongPtrW(pw->hwnd, GWLP_USERDATA, (LONG_PTR)pw);
    return pw;
}

void plugwin_show(PlugWin *w)
{
    if (!w) return;
    ShowWindow(w->hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(w->hwnd);
    HWND c = GetWindow(w->hwnd, GW_CHILD);                 /* for the log: did the plugin put its interface in? */
    char cls[128] = "";
    RECT r = { 0, 0, 0, 0 };
    if (c) { GetClassNameA(c, cls, sizeof cls); GetWindowRect(c, &r); }
    printf(">>> Plugin window %dx%d: %s%s (%ldx%ld, %s)\n", w->w, w->h, c ? "plugin interface " : "NO plugin interface",
           cls, (long)(r.right - r.left), (long)(r.bottom - r.top), (c && IsWindowVisible(c)) ? "visible" : "hidden");
}

void plugwin_raise(PlugWin *w)
{
    if (!w) return;
    if (IsIconic(w->hwnd)) ShowWindow(w->hwnd, SW_RESTORE);
    SetForegroundWindow(w->hwnd);
}

unsigned long plugwin_xid(PlugWin *w) { return w ? (unsigned long)(uintptr_t)w->hwnd : 0; }
void *plugwin_native(PlugWin *w) { return w ? (void *)w->hwnd : NULL; }

void plugwin_resize(PlugWin *w, int width, int height)
{
    if (!w || width <= 0 || height <= 0) return;
    w->w = width; w->h = height;
    int ow, oh;
    outer_size(width, height, &ow, &oh);
    SetWindowPos(w->hwnd, NULL, 0, 0, ow, oh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void plugwin_get_size(PlugWin *w, int *width, int *height)
{
    if (width) *width = w ? w->w : 0;
    if (height) *height = w ? w->h : 0;
}

bool plugwin_close_requested(PlugWin *w) { return w && w->close_req; }

void plugwin_destroy(PlugWin *w)
{
    if (!w) return;
    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, 0);
    DestroyWindow(w->hwnd);
    HWND m = main_hwnd();
    if (m) SetForegroundWindow(m);
    free(w);
}

bool plugwin_child_size(PlugWin *w, int *width, int *height)
{
    if (!w) return false;
    HWND c = GetWindow(w->hwnd, GW_CHILD);
    if (!c) return false;
    RECT r;
    if (!GetWindowRect(c, &r)) return false;
    *width = r.right - r.left;
    *height = r.bottom - r.top;
    return *width > 0 && *height > 0;
}

/* ---- run loop: timers only ---- */
typedef struct { unsigned id; unsigned period; ULONGLONG next; PlugTimerCb cb; void *ud; } Tm;
#define MAX_TM 256
static Tm g_tm[MAX_TM];
static int g_ntm;
static unsigned g_next_id = 1;

void plugwin_add_fd(int fd, PlugFdCb cb, void *ud) { (void)fd; (void)cb; (void)ud; }
void plugwin_remove_fd(int fd, void *ud) { (void)fd; (void)ud; }
void plugwin_remove_fds_of(void *ud) { (void)ud; }

unsigned plugwin_add_timer(unsigned period_ms, PlugTimerCb cb, void *ud)
{
    if (g_ntm >= MAX_TM || !cb) return 0;
    Tm *t = &g_tm[g_ntm++];
    t->id = g_next_id++;
    if (!g_next_id) g_next_id = 1;
    t->period = period_ms ? period_ms : 30;
    t->next = GetTickCount64() + t->period;
    t->cb = cb; t->ud = ud;
    return t->id;
}

void plugwin_remove_timer(unsigned id)
{
    for (int i = 0; i < g_ntm; ++i) if (g_tm[i].id == id) { g_tm[i] = g_tm[--g_ntm]; return; }
}

void plugwin_remove_timers_of(void *ud)
{
    for (int i = 0; i < g_ntm; ) { if (g_tm[i].ud == ud) g_tm[i] = g_tm[--g_ntm]; else ++i; }
}

void plugwin_poll(void)
{
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < g_ntm; ++i) {
        Tm *t = &g_tm[i];
        if (now < t->next) continue;
        t->next = now + t->period;
        unsigned id = t->id;
        PlugTimerCb cb = t->cb;
        void *ud = t->ud;
        cb(ud, id);                      /* may remove timers: re-check the slot */
        if (i < g_ntm && g_tm[i].id != id) --i;
    }
}
