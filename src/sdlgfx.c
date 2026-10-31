/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _POSIX_C_SOURCE 200809L
#include "sdlgfx.h"
#include "icon_data.h"

#include <SDL.h>
#include <SDL_ttf.h>
#include <unistd.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ====================================================================== */
/*  State                                                                  */
/* ====================================================================== */

static SDL_Window   *g_win;
static SDL_Renderer *g_ren;
static unsigned      g_cfg_flags;
static bool          g_should_close;
static int           g_target_fps = 60;
static Uint64        g_t0;
static Uint64        g_frame_start;
static bool          g_soft_render;      /* 1.3.4: the picture is drawn by the CPU (SDL software renderer) */
static SDL_Surface  *g_off;              /* 1.3.5: software mode: the picture is drawn here first ... */
static Uint8        *g_prev;             /* ... and only the parts that changed since the last frame go to the window */
static int           g_prev_w, g_prev_h;
static bool          g_push_all = true;  /* the whole picture to the window (resize, expose) */
static double        g_pushed;           /* QLIM_FPS: share of the window sent (sum over frames) */
static double        g_last_input = -1e9;/* 1.3.4: the last event from the user/window (seconds, SDL clock) */
static int           g_busy_fps;         /* 1.3.4: frames per second wanted while nothing is touched (0 = full) */
static int           g_max_fps;          /* 1.3.7: the cap at all times (Edit > Performance / automatic), 0 = none */
static int           g_render_pref;      /* 1.3.7: 0 auto, 1 graphics card, 2 light (software) - before InitWindow */
static char          g_render_desc[160] = "?";
static float         g_scale = 1.0f;       /* physical pixels per 1 UI unit */
static float         g_user_scale = 1.0f;  /* QLIM_SCALE */
static float         g_zoom = 1.0f;        /* 1.3.10: Ctrl+wheel - the whole interface (not the menu bar) */
static float         g_base_scale = 1.0f;  /* the scale without the zoom (the menu bar) */
static float         g_saved_scale;
static int           g_unzoom;
static int           g_drawable_w, g_drawable_h;

/* input */
#define NKEYS 512
static bool  g_key_down[NKEYS];
static bool  g_key_pressed[NKEYS];
static bool  g_key_repeat[NKEYS];
static bool  g_btn_down[3], g_btn_pressed[3], g_btn_released[3];
static int   g_btn_clicks[3];            /* 1.3.15: the system's count (2 = a double click) */
static float g_wheel;
static Vector2 g_mouse;            /* 1.3.10: in PHYSICAL pixels (the scale may differ between parts of a frame) */
static Vector2 g_mouse_delta;      /* movement for this frame (also works in "infinite" mode) */
static bool    g_mouse_rel;        /* "infinite" mode: the cursor is hidden and does not stop at the screen edge */
static Vector2 g_mouse_rel_home;   /* where to put the cursor back afterwards */
#define CHARQ 64
static int g_char_q[CHARQ];
static int g_char_n;

/* drop */
static char **g_drop_paths;
static unsigned g_drop_n, g_drop_cap;
static bool  g_drop_ready;
static Vector2 g_drop_pos;

/* cursors */
static SDL_Cursor *g_cursors[8];
static int g_cursor_req, g_cursor_cur = -1;

/* ====================================================================== */
/*  Helpers                                                                */
/* ====================================================================== */

void TraceLog(int level, const char *fmt, ...)
{
    static const char *names[] = { "ALL", "TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "FATAL", "NONE" };
    if (level < 0 || level > 7) level = 3;
    if (level < LOG_INFO) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", names[level]);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

double GetTime(void)
{
    return (double)(SDL_GetPerformanceCounter() - g_t0) / (double)SDL_GetPerformanceFrequency();
}

float GfxScale(void) { return g_scale; }

/* UI units -> physical pixels (rounded to a whole pixel) */
static inline int D(float v) { return (int)lroundf(v * g_scale); }

static inline void set_color(Color c)
{
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
}

/* 1.3.8: the Eco layer (see below) */
static void gfx_copy(SDL_Texture *t, unsigned serial, Color mod, SDL_Rect dst);
static void gfx_rects(const SDL_Rect *r, int n, Color col);
static void gfx_clear(Color col);
static void gfx_geom(const SDL_Vertex *v, int n, const int *idx, int ni);
static void gfx_clip(const SDL_Rect *r);

static void update_scale(void)
{
    int ww = 1, wh = 1, ow = 1, oh = 1;
    SDL_GetWindowSize(g_win, &ww, &wh);
    SDL_GetRendererOutputSize(g_ren, &ow, &oh);
    if (g_off) {                                       /* 1.3.5: the surface is bigger than the window */
        ow = ww < g_off->w ? ww : g_off->w;
        oh = wh < g_off->h ? wh : g_off->h;
    }
    g_drawable_w = ow;
    g_drawable_h = oh;
    float dpi = ww > 0 ? (float)ow / (float)ww : 1.0f;
#ifdef _WIN32
    /* Windows (DPI-aware program): the window is in real pixels -> the scale comes from the screen's DPI
     * (100% = 96 dpi, 125% = 120, 150% = 144 ...) */
    {
        float ddpi = 0.0f;
        int di = SDL_GetWindowDisplayIndex(g_win);
        if (di >= 0 && SDL_GetDisplayDPI(di, &ddpi, NULL, NULL) == 0 && ddpi > 0.0f) {
            float k = ddpi / 96.0f;
            if (k > 1.01f && k < 4.0f) dpi = k;
        }
    }
#endif
    g_base_scale = dpi * g_user_scale;
    if (g_base_scale < 0.5f) g_base_scale = 0.5f;
    g_scale = g_base_scale * g_zoom;
    if (g_scale < 0.4f) g_scale = 0.4f;
    {
        static int lw, lh, lo, lh2; static float ls;
        if (ww != lw || wh != lh || ow != lo || oh != lh2 || g_scale != ls) {
            lw = ww; lh = wh; lo = ow; lh2 = oh; ls = g_scale;
            printf("Q-LiM graphics: window %dx%d, drawable %dx%d, scale %.2f\n", ww, wh, ow, oh, g_scale);
        }
    }
}

/* ====================================================================== */
/*  Window                                                                 */
/* ====================================================================== */

void SetConfigFlags(unsigned int flags) { g_cfg_flags = flags; }
void SetExitKey(int key) { (void)key; }
void SetTargetFPS(int fps) { g_target_fps = fps; }
bool WindowShouldClose(void) { return g_should_close; }
void ClearWindowShouldClose(void) { g_should_close = false; }
void RequestWindowClose(void) { g_should_close = true; }
void SetWindowTitle(const char *title) { if (g_win) SDL_SetWindowTitle(g_win, title ? title : ""); }
void MaximizeWindow(void) { if (g_win) SDL_MaximizeWindow(g_win); }
/* Step 35c: the OpenGL context of Q-LiM and the plugins. Plugins with an OpenGL window (DPF/pugl, JUCE...) make
 * THEIR context current on the main thread. Then Q-LiM draws "into someone else's context" and its window freezes
 * (the program runs, but the picture does not change). So around every plugin call on the main thread:
 * GfxGLRelease() releases our context, GfxGLRestore() brings it back (for real - SDL keeps track and would otherwise skip it). */
static SDL_GLContext g_saved_ctx;
static int g_gl_depth;
bool GfxSoftwareRendering(void) { return g_soft_render; }
void GfxSetBusyFps(int fps) { g_busy_fps = fps; }
void GfxSetMaxFps(int fps) { g_max_fps = fps; }
void GfxSetRendererPref(int pref) { g_render_pref = pref; }
const char *GfxRendererDesc(void) { return g_render_desc; }

void GfxGLRelease(void)
{
    if (g_gl_depth++ > 0 || !g_win) return;
    SDL_GLContext c = SDL_GL_GetCurrentContext();
    if (!c) return;
    g_saved_ctx = c;
    SDL_GL_MakeCurrent(g_win, NULL);
}
void GfxGLRestore(void)
{
    if (g_gl_depth == 0 || --g_gl_depth > 0) return;
    if (g_saved_ctx && g_win) {
        SDL_GL_MakeCurrent(g_win, NULL);           /* resets SDL's bookkeeping -> the next one is real */
        SDL_GL_MakeCurrent(g_win, g_saved_ctx);
    }
}

/* the SDL window (for plugwin.c - its X11 id) */
void *GfxSdlWindow(void) { return g_win; }
int  GetScreenWidth(void)  { return (int)((float)g_drawable_w / g_scale + 0.5f); }
int  GetScreenHeight(void) { return (int)((float)g_drawable_h / g_scale + 0.5f); }

static void poll_events(void);

void InitWindow(int width, int height, const char *title)
{
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    /* the program's name for the system (menu/panel) -> matches qlim.desktop and the "qlim" icon */
    SDL_SetHint("SDL_VIDEO_X11_WMCLASS", "qlim");
    SDL_SetHint("SDL_VIDEO_WAYLAND_WMCLASS", "qlim");
    SDL_SetHint("SDL_APP_NAME", "Q-LiM");
#ifdef SDL_HINT_MOUSE_AUTO_CAPTURE
    /* Otherwise SDL "grabs" the mouse while a button is pressed. If we open an external window at that moment
     * (zenity for Save/Load), the button release never reaches SDL and the grab stays -> the mouse
     * does not work in the other window. On X11 dragging outside the window works without it anyway. */
    SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE, "0");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        exit(1);
    }
    if (TTF_Init() != 0) {
        fprintf(stderr, "TTF_Init: %s\n", TTF_GetError());
        exit(1);
    }
    const char *es = getenv("QLIM_SCALE");
    if (es && atof(es) > 0.25) g_user_scale = (float)atof(es);

    Uint32 wf = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN;
    if (g_cfg_flags & FLAG_WINDOW_RESIZABLE) wf |= SDL_WINDOW_RESIZABLE;
    g_win = SDL_CreateWindow(title ? title : "Q-LiM", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             width, height, wf);
    if (!g_win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        exit(1);
    }
    {   /* the window icon (built into the program) */
        SDL_Surface *ic = SDL_CreateRGBSurfaceWithFormatFrom((void *)qlim_icon_rgba, QLIM_ICON_W, QLIM_ICON_H, 32,
                                                             QLIM_ICON_W * 4, SDL_PIXELFORMAT_RGBA32);
        if (ic) { SDL_SetWindowIcon(g_win, ic); SDL_FreeSurface(ic); }
    }
    /* a smaller window makes no sense (the buttons on the left, the panels and the waveform do not fit) */
    SDL_SetWindowMinimumSize(g_win, 900, 560);

    const char *rn = getenv("QLIM_RENDERER");
    bool force_sw = rn ? strcmp(rn, "software") == 0 : g_render_pref == 2;
    bool force_gpu = rn ? strcmp(rn, "gpu") == 0 : g_render_pref == 1;
    char glname[120] = "";
    if (!force_sw)
        g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (g_ren && !force_gpu) {
        /* 1.3.4: an "OpenGL" that is really Mesa's CPU emulation (llvmpipe/softpipe - old or unsupported graphics
         * chips, e.g. the Intel GMA 950 of a ThinkPad T60) uses ALL the CPU cores for every frame and leaves too
         * little time for the sound. SDL's own software renderer draws the same picture on one core, much cheaper. */
        SDL_RendererInfo ri0;
        if (SDL_GetRendererInfo(g_ren, &ri0) == 0 && ri0.name && strncmp(ri0.name, "opengl", 6) == 0) {
            typedef const unsigned char *(*GetStr)(unsigned);
            GetStr gs = (GetStr)SDL_GL_GetProcAddress("glGetString");
            const char *glr = gs ? (const char *)gs(0x1F01 /* GL_RENDERER */) : NULL;
            if (glr) { fprintf(stderr, "Q-LiM graphics: OpenGL = %s\n", glr); snprintf(glname, sizeof glname, "%s", glr); }
            if (glr && (strstr(glr, "llvmpipe") || strstr(glr, "softpipe") || strstr(glr, "Software Rasterizer") ||
                        strstr(glr, "swrast"))) {
                fprintf(stderr, "Q-LiM graphics: no graphics acceleration -> the light software renderer "
                                "(QLIM_RENDERER=gpu keeps OpenGL)\n");
                SDL_DestroyRenderer(g_ren);
                g_ren = NULL;
            }
        }
    }
    if (!g_ren) {
        /* the finished picture goes to the window directly (X11 shared memory), not through OpenGL again */
        SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
        /* 1.3.5: drawn into our own image as big as the largest screen; EndDrawing sends only the changed
         * parts to the window. A whole frame through the X server (which itself may have no graphics
         * acceleration) 20 times a second was enough to break the sound of a ThinkPad T60 - and it stopped
         * as soon as the window was minimized. */
        int mw = 0, mh = 0;
        for (int d = 0; d < SDL_GetNumVideoDisplays(); ++d) {
            SDL_Rect b;
            if (SDL_GetDisplayBounds(d, &b) == 0) { if (b.w > mw) mw = b.w; if (b.h > mh) mh = b.h; }
        }
        if (mw < width) mw = width;
        if (mh < height) mh = height;
        if (mw < 640) mw = 1920;
        if (mh < 480) mh = 1200;
        g_off = SDL_CreateRGBSurfaceWithFormat(0, mw, mh, 32, SDL_PIXELFORMAT_RGB888);
        if (g_off) {
            SDL_SetSurfaceBlendMode(g_off, SDL_BLENDMODE_NONE);
            g_ren = SDL_CreateSoftwareRenderer(g_off);
            if (!g_ren) { SDL_FreeSurface(g_off); g_off = NULL; }
            else fprintf(stderr, "Q-LiM graphics: software, only the changed parts are sent (%dx%d)\n", mw, mh);
        }
        if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
        if (g_ren) g_soft_render = true;
    }
    if (g_soft_render)
        snprintf(g_render_desc, sizeof g_render_desc, "light drawing on the CPU (%s)",
                 force_sw ? "chosen" : glname[0] ? "no graphics acceleration" : "no graphics card driver");
    else {
        SDL_RendererInfo ri1;
        const char *nm = SDL_GetRendererInfo(g_ren, &ri1) == 0 && ri1.name ? ri1.name : "?";
        snprintf(g_render_desc, sizeof g_render_desc, "graphics card (%s%s%s)", nm, glname[0] ? ", " : "", glname);
    }
    if (!g_ren) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        exit(1);
    }
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    {
        SDL_RendererInfo ri;
        if (SDL_GetRendererInfo(g_ren, &ri) == 0)
            fprintf(stderr, "Q-LiM graphics: renderer=%s vsync=%s\n", ri.name,
                    (ri.flags & SDL_RENDERER_PRESENTVSYNC) ? "yes" : "no");
    }

    g_t0 = SDL_GetPerformanceCounter();
    g_frame_start = g_t0;
    update_scale();
    SDL_StartTextInput();
    poll_events();
}

void CloseWindow(void)
{
    for (int i = 0; i < 8; ++i)
        if (g_cursors[i]) { SDL_FreeCursor(g_cursors[i]); g_cursors[i] = NULL; }
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    g_ren = NULL;
    g_win = NULL;
    TTF_Quit();
    SDL_Quit();
}

/* ====================================================================== */
/*  Text (FreeType through SDL_ttf, exact size, cache of rendered images) */
/* ====================================================================== */

typedef struct FontSize {
    int px;                 /* requested line height in physical px */
    TTF_Font *f;
    struct FontSize *next;
} FontSize;

static unsigned g_tex_serial;            /* (1.3.8, see the Eco layer) */
static bool g_eco;                       /* (1.3.8/1.3.9, see the Eco layer) */
typedef struct TextEntry {
    char *text;
    int px;
    SDL_Texture *tex;
    unsigned serial;                   /* 1.3.8: for the Eco fingerprints */
    int w, h;
    struct TextEntry *next;
} TextEntry;

#define TBUCKETS 1024
static TextEntry *g_tbucket[TBUCKETS];
static size_t     g_tcache_px;       /* total pixels in the cache */
static FontSize  *g_fsizes;
static char       g_font_path[1024];
static bool       g_font_ok;
static float      g_font_ratio;      /* line height / ptsize for this font */

static const char *g_font_fallbacks[] = {
    "assets/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
    NULL
};

static bool file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static void drop_text_cache(void)
{
    for (int i = 0; i < TBUCKETS; ++i) {
        TextEntry *e = g_tbucket[i];
        while (e) {
            TextEntry *n = e->next;
            if (e->tex) SDL_DestroyTexture(e->tex);
            free(e->text);
            free(e);
            e = n;
        }
        g_tbucket[i] = NULL;
    }
    g_tcache_px = 0;
}

static void drop_font_sizes(void)
{
    FontSize *s = g_fsizes;
    while (s) {
        FontSize *n = s->next;
        if (s->f) TTF_CloseFont(s->f);
        free(s);
        s = n;
    }
    g_fsizes = NULL;
}

static bool set_font_path(const char *path)
{
    if (!path || !file_exists(path)) return false;
    if (strcmp(path, g_font_path) == 0 && g_font_ok) return true;
    drop_text_cache();
    drop_font_sizes();
    snprintf(g_font_path, sizeof g_font_path, "%s", path);
    TTF_Font *probe = TTF_OpenFont(g_font_path, 100);
    if (!probe) { g_font_ok = false; return false; }
    g_font_ratio = (float)TTF_FontHeight(probe) / 100.0f;
    TTF_CloseFont(probe);
    if (g_font_ratio < 0.5f) g_font_ratio = 1.16f;
    g_font_ok = true;
    return true;
}

static bool ensure_font(void)
{
    if (g_font_ok) return true;
    const char *env = getenv("QLIM_FONT");
    if (env && set_font_path(env)) return true;
    {   /* next to the program (installed packages: /opt/qlim/assets/DejaVuSans.ttf; Windows: C:/Program Files/Q-LiM/assets) */
        char exe[1024];
#ifdef _WIN32
        snprintf(exe, sizeof exe, "%s/assets/DejaVuSans.ttf", qw_exe_dir());
        if (set_font_path(exe)) return true;
#else
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 64);
        if (n > 0) {
            exe[n] = 0;
            char *sl = strrchr(exe, '/');
            if (sl) { strcpy(sl, "/assets/DejaVuSans.ttf"); if (set_font_path(exe)) return true; }
        }
#endif
    }
    for (int i = 0; g_font_fallbacks[i]; ++i)
        if (set_font_path(g_font_fallbacks[i])) return true;
    return false;
}

/* Finds a font whose LINE is exactly `line_px` physical pixels high.
 * (raylib defines "size" as the full line height; SDL_ttf — as the em. So we convert.) */
static TTF_Font *font_for_px(int line_px)
{
    if (line_px < 4) line_px = 4;
    for (FontSize *s = g_fsizes; s; s = s->next)
        if (s->px == line_px) return s->f;

    int pt = (int)lroundf((float)line_px / g_font_ratio);
    if (pt < 4) pt = 4;
    TTF_Font *best = NULL;
    int best_err = 1 << 30, best_pt = pt;
    for (int cand = pt - 1; cand <= pt + 1; ++cand) {
        if (cand < 4) continue;
        TTF_Font *f = TTF_OpenFont(g_font_path, cand);
        if (!f) continue;
        int err = abs(TTF_FontHeight(f) - line_px);
        if (err < best_err) {
            if (best) TTF_CloseFont(best);
            best = f; best_err = err; best_pt = cand;
        } else {
            TTF_CloseFont(f);
        }
    }
    (void)best_pt;
    if (!best) return NULL;
    TTF_SetFontHinting(best, TTF_HINTING_LIGHT);
    TTF_SetFontKerning(best, 1);
    FontSize *s = calloc(1, sizeof *s);
    if (!s) { TTF_CloseFont(best); return NULL; }
    s->px = line_px;
    s->f = best;
    s->next = g_fsizes;
    g_fsizes = s;
    return best;
}

static unsigned hash_text(const char *t, int px)
{
    unsigned h = 2166136261u ^ (unsigned)px * 16777619u;
    for (; *t; ++t) h = (h ^ (unsigned char)*t) * 16777619u;
    return h;
}

static TextEntry *text_lookup(const char *text, int px)
{
    unsigned b = hash_text(text, px) % TBUCKETS;
    for (TextEntry *e = g_tbucket[b]; e; e = e->next)
        if (e->px == px && strcmp(e->text, text) == 0) return e;

    if (g_tcache_px > (g_eco ? 6u : 24u) * 1000u * 1000u) {   /* ~96 MB (Eco: ~24 MB): clear everything */
        drop_text_cache();
        b = hash_text(text, px) % TBUCKETS;
    }
    TTF_Font *f = font_for_px(px);
    if (!f) return NULL;

    TextEntry *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->text = strdup(text);
    e->px = px;
    if (text[0] == '\0') {
        e->w = 0;
        e->h = TTF_FontHeight(f);
    } else {
        SDL_Color white = { 255, 255, 255, 255 };
        SDL_Surface *sf = TTF_RenderUTF8_Blended(f, text, white);
        if (sf) {
            e->w = sf->w;
            e->h = sf->h;
            e->tex = SDL_CreateTextureFromSurface(g_ren, sf);
            e->serial = ++g_tex_serial;
            if (e->tex) SDL_SetTextureBlendMode(e->tex, SDL_BLENDMODE_BLEND);
            SDL_FreeSurface(sf);
            g_tcache_px += (size_t)e->w * (size_t)e->h;
        } else {
            TTF_SizeUTF8(f, text, &e->w, &e->h);
        }
    }
    e->next = g_tbucket[b];
    g_tbucket[b] = e;
    return e;
}

Font GetFontDefault(void)
{
    Font f;
    memset(&f, 0, sizeof f);
    return f;                       /* texture.id == 0 -> ui.c uses DrawText/MeasureText */
}

Font LoadFontEx(const char *path, int size, int *codepoints, int count)
{
    (void)size; (void)codepoints; (void)count;
    Font f;
    memset(&f, 0, sizeof f);
    if (!ensure_font() && !set_font_path(path)) return f;
    if (path && file_exists(path)) set_font_path(path);
    f.baseSize = size;
    f.glyphCount = count;
    f.texture.id = 1;
    f.handle = g_font_path;
    return f;
}

void SetTextureFilter(Texture2D t, int filter) { (void)t; (void)filter; }

Vector2 MeasureTextEx(Font f, const char *text, float size, float spacing)
{
    (void)f; (void)spacing;
    Vector2 v = { 0, size };
    if (!text || !ensure_font()) return v;
    int px = D(size);
    TTF_Font *font = font_for_px(px);
    if (!font) return v;
    int w = 0, h = 0;
    if (text[0]) TTF_SizeUTF8(font, text, &w, &h);
    v.x = (float)w / g_scale;
    return v;
}

void DrawTextEx(Font f, const char *text, Vector2 pos, float size, float spacing, Color c)
{
    (void)f; (void)spacing;
    if (!text || !text[0] || !ensure_font()) return;
    TextEntry *e = text_lookup(text, D(size));
    if (!e || !e->tex) return;
    SDL_Rect dst = { D(pos.x), D(pos.y), e->w, e->h };
    gfx_copy(e->tex, e->serial, c, dst);
}

int MeasureText(const char *text, int size)
{
    return (int)MeasureTextEx(GetFontDefault(), text, (float)size, 0).x;
}

void DrawText(const char *text, int x, int y, int size, Color c)
{
    DrawTextEx(GetFontDefault(), text, (Vector2){ (float)x, (float)y }, (float)size, 0, c);
}

/* ====================================================================== */
/*  Shapes                                                                 */
/* ====================================================================== */

/* ====================================================================== */
/*  1.3.8: Eco interface - record, compare, redraw only what changed       */
/* ====================================================================== */
/* All drawing goes through the 5 calls below. Normally they draw at once. In Eco mode a frame is only RECORDED;
 * at the end (eco_present) the screen is cut into tiles, every tile gets a fingerprint of the commands that touch
 * it, and only the tiles whose fingerprint differs from the last frame are drawn again (into a picture kept between
 * frames). Nothing changed = nothing drawn and nothing sent. Shapes are drawn as whole-pixel spans (no soft edges)
 * and dialogs get a plain dark background instead of the blurred one. */
static bool g_eco;                       /* Eco look (Edit > Performance): spans, no hover, no blur, <= 30 fps */
static bool g_smart = true;              /* 1.3.9: record + redraw only what changed - the normal way now */
static bool g_rec;                       /* this frame is being recorded (= g_smart || g_eco at its start) */
static SDL_Texture *g_tgt;               /* the kept picture (graphics card) */
static int  g_tgt_w, g_tgt_h;
static bool g_frame_active;              /* Eco: a button, the wheel or a key in this frame (the mouse "counts") */
static float g_hov_area;                 /* (1.3.11, see GfxHoverCheck) */
static bool g_eco_full = true;           /* draw everything at the next eco frame */
enum { RC_CLEAR, RC_RECTS, RC_GEOM, RC_COPY };
typedef struct {
    unsigned char type, has_clip;
    SDL_Rect clip, bb, dst;
    Color    c;
    SDL_Texture *tex;
    unsigned hash;
    size_t   off;
    int      n, ni;                      /* rects / vertices, indices */
} RCmd;
static RCmd   *g_cmd;
static int     g_ncmd, g_capcmd;
static unsigned char *g_ar;
static size_t  g_arn, g_arcap;
static bool    g_clip_on;
static SDL_Rect g_clip_cur;
static unsigned g_tex_serial;            /* every text picture gets its own number (the pointer may be reused) */

static unsigned fnv(unsigned h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 16777619u;
    return h;
}
static void *ar_put(const void *p, size_t n, size_t *off)
{
    if (g_arn + n > g_arcap) {
        size_t nc = g_arcap ? g_arcap * 2 : (1u << 20);
        while (nc < g_arn + n) nc *= 2;
        unsigned char *a = (unsigned char *)realloc(g_ar, nc);
        if (!a) return NULL;
        g_ar = a; g_arcap = nc;
    }
    *off = g_arn;
    memcpy(g_ar + g_arn, p, n);
    g_arn += n;
    return g_ar + *off;
}
static RCmd *rc_new(int type)
{
    if (g_ncmd == g_capcmd) {
        int nc = g_capcmd ? g_capcmd * 2 : 4096;
        RCmd *c = (RCmd *)realloc(g_cmd, sizeof(RCmd) * (size_t)nc);
        if (!c) return NULL;
        g_cmd = c; g_capcmd = nc;
    }
    RCmd *c = &g_cmd[g_ncmd];
    memset(c, 0, sizeof *c);
    c->type = (unsigned char)type;
    c->has_clip = g_clip_on;
    c->clip = g_clip_cur;
    return c;
}
/* the bounding box (cut by the clip); false = nothing visible -> the command is dropped */
static bool rc_finish(RCmd *c, int x0, int y0, int x1, int y1, unsigned h)
{
    if (c->has_clip) {
        if (x0 < c->clip.x) x0 = c->clip.x;
        if (y0 < c->clip.y) y0 = c->clip.y;
        if (x1 > c->clip.x + c->clip.w) x1 = c->clip.x + c->clip.w;
        if (y1 > c->clip.y + c->clip.h) y1 = c->clip.y + c->clip.h;
    }
    if (x1 <= x0 || y1 <= y0) return false;
    c->bb = (SDL_Rect){ x0, y0, x1 - x0, y1 - y0 };
    h = fnv(h, &c->type, 1);
    if (c->has_clip) h = fnv(h, &c->clip, sizeof c->clip);
    c->hash = fnv(h, &c->c, sizeof c->c);
    g_ncmd++;
    return true;
}

static void gfx_clip(const SDL_Rect *r)
{
    if (!g_rec) { SDL_RenderSetClipRect(g_ren, r); return; }
    g_clip_on = r != NULL;
    if (r) g_clip_cur = *r;
}
static void gfx_rects(const SDL_Rect *r, int n, Color col)
{
    if (n <= 0) return;
    if (!g_rec) { set_color(col); if (n == 1) SDL_RenderFillRect(g_ren, r); else SDL_RenderFillRects(g_ren, r, n); return; }
    RCmd *c = rc_new(RC_RECTS);
    if (!c || !ar_put(r, sizeof(SDL_Rect) * (size_t)n, &c->off)) return;
    c->n = n; c->c = col;
    int x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    for (int i = 0; i < n; ++i) {
        if (r[i].x < x0) x0 = r[i].x;
        if (r[i].y < y0) y0 = r[i].y;
        if (r[i].x + r[i].w > x1) x1 = r[i].x + r[i].w;
        if (r[i].y + r[i].h > y1) y1 = r[i].y + r[i].h;
    }
    rc_finish(c, x0, y0, x1, y1, fnv(2166136261u, r, sizeof(SDL_Rect) * (size_t)n));
}
static void gfx_clear(Color col)
{
    if (!g_rec) {
        SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(g_ren, col.r, col.g, col.b, 255);
        SDL_RenderClear(g_ren);
        SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
        return;
    }
    RCmd *c = rc_new(RC_CLEAR);
    if (!c) return;
    c->has_clip = 0;
    c->c = col; c->c.a = 255;
    rc_finish(c, 0, 0, 1 << 20, 1 << 20, 2166136261u);
}
static void gfx_geom(const SDL_Vertex *v, int n, const int *idx, int ni)
{
    if (n <= 0 || ni <= 0) return;
    if (!g_rec) { SDL_RenderGeometry(g_ren, NULL, v, n, idx, ni); return; }
    RCmd *c = rc_new(RC_GEOM);
    size_t o2;
    if (!c || !ar_put(v, sizeof(SDL_Vertex) * (size_t)n, &c->off) || !ar_put(idx, sizeof(int) * (size_t)ni, &o2)) return;
    c->n = n; c->ni = ni;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < n; ++i) {
        if (v[i].position.x < x0) x0 = v[i].position.x;
        if (v[i].position.y < y0) y0 = v[i].position.y;
        if (v[i].position.x > x1) x1 = v[i].position.x;
        if (v[i].position.y > y1) y1 = v[i].position.y;
    }
    unsigned h = fnv(2166136261u, v, sizeof(SDL_Vertex) * (size_t)n);
    h = fnv(h, idx, sizeof(int) * (size_t)ni);
    rc_finish(c, (int)floorf(x0) - 1, (int)floorf(y0) - 1, (int)ceilf(x1) + 1, (int)ceilf(y1) + 1, h);
}
static void gfx_copy(SDL_Texture *t, unsigned serial, Color mod, SDL_Rect dst)
{
    if (!t) return;
    if (!g_rec) {
        SDL_SetTextureColorMod(t, mod.r, mod.g, mod.b);
        SDL_SetTextureAlphaMod(t, mod.a);
        SDL_RenderCopy(g_ren, t, NULL, &dst);
        return;
    }
    RCmd *c = rc_new(RC_COPY);
    if (!c) return;
    c->tex = t; c->c = mod; c->dst = dst;
    unsigned h = fnv(2166136261u, &serial, sizeof serial);
    h = fnv(h, &dst, sizeof dst);
    rc_finish(c, dst.x, dst.y, dst.x + dst.w, dst.y + dst.h, h);
}

/* draws one recorded command, cut to the rectangle r */
static void rc_play(const RCmd *c, const SDL_Rect *r)
{
    SDL_Rect cl = *r;
    if (c->has_clip && !SDL_IntersectRect(&c->clip, r, &cl)) return;
    SDL_RenderSetClipRect(g_ren, &cl);
    switch (c->type) {
    case RC_CLEAR:                                     /* (RenderClear ignores the clip: a plain fill) */
        SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(g_ren, c->c.r, c->c.g, c->c.b, 255);
        SDL_RenderFillRect(g_ren, &cl);
        SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
        break;
    case RC_RECTS:
        set_color(c->c);
        SDL_RenderFillRects(g_ren, (const SDL_Rect *)(g_ar + c->off), c->n);
        break;
    case RC_GEOM:
        SDL_RenderGeometry(g_ren, NULL, (const SDL_Vertex *)(g_ar + c->off), c->n,
                           (const int *)(g_ar + c->off + sizeof(SDL_Vertex) * (size_t)c->n), c->ni);
        break;
    case RC_COPY:
        SDL_SetTextureColorMod(c->tex, c->c.r, c->c.g, c->c.b);
        SDL_SetTextureAlphaMod(c->tex, c->c.a);
        SDL_RenderCopy(g_ren, c->tex, NULL, &c->dst);
        break;
    }
}

/* ---- 1.3.4: the software renderer (no graphics acceleration) ----
 * SDL draws triangles very slowly without a graphics card (each one through temporary images), and the rounded
 * cells, knobs and lines are made of thousands of them. In that mode the shapes are drawn as horizontal spans
 * (whole-pixel rectangles, sent in batches): the same picture, without the soft edges, many times cheaper. */
#define SP_MAX 2048
static SDL_Rect g_sp[SP_MAX];
static int      g_nsp;
static void sp_flush(Color c)
{
    if (g_nsp > 0) gfx_rects(g_sp, g_nsp, c);
    g_nsp = 0;
}
static void sp_add(int x0, int x1, int y, Color c)
{
    if (x1 <= x0) return;
    for (int k = g_nsp - 1; k >= 0 && k >= g_nsp - 2; --k) {      /* the same span as on the row above: taller */
        SDL_Rect *q = &g_sp[k];
        if (q->x == x0 && q->w == x1 - x0 && q->y + q->h == y) { q->h++; return; }
    }
    if (g_nsp == SP_MAX) sp_flush(c);
    g_sp[g_nsp++] = (SDL_Rect){ x0, y, x1 - x0, 1 };
}
/* the horizontal extent of a rounded rectangle on the row whose middle is yc; false = outside */
static bool rr_row(float x0, float y0, float x1, float y1, float rad, float yc, float *l, float *r)
{
    if (yc < y0 || yc > y1 || x1 <= x0) return false;
    float dy = 0.0f;
    if (yc < y0 + rad) dy = y0 + rad - yc;
    else if (yc > y1 - rad) dy = yc - (y1 - rad);
    float d = rad * rad - dy * dy;
    float dx = rad - (d > 0.0f ? sqrtf(d) : 0.0f);
    *l = x0 + dx; *r = x1 - dx;
    return *r > *l;
}
static void sw_rrect(float x0, float y0, float x1, float y1, float rad, float t, Color c)
{
    float ix0 = x0 + t, iy0 = y0 + t, ix1 = x1 - t, iy1 = y1 - t, irad = rad - t > 0.0f ? rad - t : 0.0f;
    for (int y = (int)floorf(y0); y < (int)ceilf(y1); ++y) {
        float yc = (float)y + 0.5f, lo, ro, li, ri;
        if (!rr_row(x0, y0, x1, y1, rad, yc, &lo, &ro)) continue;
        int a = (int)lroundf(lo), b = (int)lroundf(ro);
        if (t > 0.0f && rr_row(ix0, iy0, ix1, iy1, irad, yc, &li, &ri)) {
            int c0 = (int)lroundf(li), c1 = (int)lroundf(ri);
            if (c0 <= a) c0 = a + 1;
            if (c1 >= b) c1 = b - 1;
            sp_add(a, c0, y, c); sp_add(c1, b, y, c);
        } else sp_add(a, b, y, c);
    }
    sp_flush(c);
}
static void sw_ring(float cx, float cy, float r0, float r1, Color c)     /* r0 = 0: a filled circle */
{
    for (int y = (int)floorf(cy - r1); y < (int)ceilf(cy + r1); ++y) {
        float dy = (float)y + 0.5f - cy, o = r1 * r1 - dy * dy;
        if (o <= 0.0f) continue;
        float ox = sqrtf(o), i = r0 * r0 - dy * dy;
        if (r0 > 0.0f && i > 0.0f) {
            float ix = sqrtf(i);
            int a = (int)lroundf(cx - ox), b = (int)lroundf(cx - ix), cc = (int)lroundf(cx + ix), d = (int)lroundf(cx + ox);
            if (b <= a) b = a + 1;
            if (cc >= d) cc = d - 1;
            sp_add(a, b, y, c); sp_add(cc, d, y, c);
        } else sp_add((int)lroundf(cx - ox), (int)lroundf(cx + ox), y, c);
    }
    sp_flush(c);
}
static void sw_poly(const float *p, int n, Color c)                     /* convex polygon */
{
    float ymin = p[1], ymax = p[1];
    for (int i = 1; i < n; ++i) { if (p[2 * i + 1] < ymin) ymin = p[2 * i + 1]; if (p[2 * i + 1] > ymax) ymax = p[2 * i + 1]; }
    for (int y = (int)floorf(ymin); y < (int)ceilf(ymax); ++y) {
        float yc = (float)y + 0.5f, l = 1e9f, r = -1e9f;
        for (int i = 0; i < n; ++i) {
            int j = (i + 1) % n;
            float ya = p[2 * i + 1], yb = p[2 * j + 1];
            if ((yc < ya && yc < yb) || (yc > ya && yc > yb) || ya == yb) continue;
            float x = p[2 * i] + (yc - ya) / (yb - ya) * (p[2 * j] - p[2 * i]);
            if (x < l) l = x;
            if (x > r) r = x;
        }
        if (r < l) continue;
        int a = (int)lroundf(l), b = (int)lroundf(r);
        if (b <= a) b = a + 1;                                  /* thin lines stay visible */
        sp_add(a, b, y, c);
    }
    sp_flush(c);
}

static void fill_dev_rect(int x0, int y0, int x1, int y1, Color c)
{
    if (x1 <= x0 || y1 <= y0) return;
    SDL_Rect r = { x0, y0, x1 - x0, y1 - y0 };
    gfx_rects(&r, 1, c);
}

/* Rectangle in UI units: both edges are rounded to a whole physical pixel,
 * so neighbouring elements touch without gaps and the edges are always crisp. */
static void fill_ui_rect(float x, float y, float w, float h, Color c)
{
    if (c.a == 0) return;
    int x0 = D(x), y0 = D(y), x1 = D(x + w), y1 = D(y + h);
    if (x1 == x0 && w > 0) x1 = x0 + 1;
    if (y1 == y0 && h > 0) y1 = y0 + 1;
    fill_dev_rect(x0, y0, x1, y1, c);
}

/* 1.3.14: a picture made by the program itself (e.g. the waveform): pixels -> one texture -> one command.
 * A still picture costs nothing (the same number = the same tiles); a changed one is one upload + one copy. */
struct GfxImage { SDL_Texture *t; int w, h; unsigned serial; };
GfxImage *GfxImageNew(void) { return (GfxImage *)calloc(1, sizeof(GfxImage)); }
float GfxScaleNow(void) { return g_scale; }
void GfxImageUpload(GfxImage *im, const uint32_t *argb, int w, int h)
{
    if (!im || w <= 0 || h <= 0) return;
    if (!im->t || im->w != w || im->h != h) {
        if (im->t) SDL_DestroyTexture(im->t);
        im->t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!im->t) return;
        SDL_SetTextureBlendMode(im->t, SDL_BLENDMODE_BLEND);
        im->w = w; im->h = h;
    }
    SDL_UpdateTexture(im->t, NULL, argb, w * 4);
    im->serial = ++g_tex_serial;
}
void GfxImageDraw(GfxImage *im, int px, int py)
{
    if (!im || !im->t) return;
    gfx_copy(im->t, im->serial, (Color){ 255, 255, 255, 255 }, (SDL_Rect){ px, py, im->w, im->h });
}

/* 1.3.14: many plain rectangles of one colour (UI units) as one command */
void DrawRectanglesBatch(const Rectangle *r, int n, Color c)
{
    if (n <= 0 || c.a == 0) return;
    static SDL_Rect *buf; static int cap;
    if (n > cap) { SDL_Rect *nb = (SDL_Rect *)realloc(buf, sizeof(SDL_Rect) * (size_t)n); if (!nb) return; buf = nb; cap = n; }
    int k = 0;
    for (int i = 0; i < n; ++i) {
        int x0 = D(r[i].x), y0 = D(r[i].y), x1 = D(r[i].x + r[i].width), y1 = D(r[i].y + r[i].height);
        if (x1 == x0 && r[i].width > 0) x1 = x0 + 1;
        if (y1 == y0 && r[i].height > 0) y1 = y0 + 1;
        if (x1 <= x0 || y1 <= y0) continue;
        buf[k++] = (SDL_Rect){ x0, y0, x1 - x0, y1 - y0 };
    }
    gfx_rects(buf, k, c);
}

void ClearBackground(Color c)
{
    gfx_clear(c);
}

/* step 36g: "round everything" (dialogs and drop-down menus): plain rectangles become slightly rounded.
 * Very thin ones (lines, ticks) and very small ones stay square. */
static float g_round_all = 0.0f;
static Rectangle g_round_big;
void GfxRoundAll(float radius) { g_round_all = radius; g_round_big = (Rectangle){ 0, 0, 0, 0 }; }
static void rr_geom(Rectangle r, float radius, float thick, Color c);
static bool round_ok(float w, float h) { return g_round_all > 0.0f && w >= 8.0f && h >= 8.0f; }
static float round_r(float w, float h)
{
    float m = w < h ? w : h;
    if (m >= 120.0f) return g_round_all * 2.5f;       /* the big ones (the dialogs themselves) - more rounded */
    m *= 0.25f;
    return g_round_all < m ? g_round_all : m;
}
/* g_round_big: the last big rectangle (the dialog) - for its title bar */

static void round_fill(Rectangle r, Color c)
{
    Rectangle b = g_round_big;
    if (b.width > 0 && fabsf(r.x - b.x) < 1 && fabsf(r.y - b.y) < 1 && fabsf(r.width - b.width) < 1 && r.height < b.height) {
        /* title bar at the very top of the dialog: rounded only at the top, like the dialog */
        float rad = round_r(b.width, b.height);
        if (rad > r.height * 0.5f) rad = r.height * 0.5f;
        rr_geom(r, rad, 0.0f, c);
        fill_ui_rect(r.x, r.y + r.height * 0.5f, r.width, r.height - r.height * 0.5f, c);
        return;
    }
    if ((r.width < r.height ? r.width : r.height) >= 120.0f) g_round_big = r;
    rr_geom(r, round_r(r.width, r.height), 0.0f, c);
}

void DrawRectangle(int x, int y, int w, int h, Color c)
{
    if (round_ok((float)w, (float)h)) { round_fill((Rectangle){ (float)x, (float)y, (float)w, (float)h }, c); return; }
    fill_ui_rect((float)x, (float)y, (float)w, (float)h, c);
}
/* step 36c: vertical gradient top -> bottom (one quad; the graphics card blends the colours) */
void DrawRectangleGradientV(float x, float y, float w, float h, Color top, Color bottom)
{
    int x0 = D(x), y0 = D(y), x1 = D(x + w), y1 = D(y + h);
    if (x1 <= x0 || y1 <= y0) return;
    if ((g_soft_render || g_eco)) {                               /* 1.3.4: in bands of 2 pixels */
        for (int yy = y0; yy < y1; yy += 2) {
            float f = (float)(yy - y0) / (float)(y1 - y0);
            Color c = { (unsigned char)(top.r + (bottom.r - top.r) * f), (unsigned char)(top.g + (bottom.g - top.g) * f),
                        (unsigned char)(top.b + (bottom.b - top.b) * f), (unsigned char)(top.a + (bottom.a - top.a) * f) };
            fill_dev_rect(x0, yy, x1, yy + 2 < y1 ? yy + 2 : y1, c);
        }
        return;
    }
    SDL_Vertex v[4];
    Color cs[4] = { top, top, bottom, bottom };
    float px[4] = { (float)x0, (float)x1, (float)x1, (float)x0 }, py[4] = { (float)y0, (float)y0, (float)y1, (float)y1 };
    for (int i = 0; i < 4; ++i) {
        v[i].position.x = px[i]; v[i].position.y = py[i];
        v[i].color.r = cs[i].r; v[i].color.g = cs[i].g; v[i].color.b = cs[i].b; v[i].color.a = cs[i].a;
        v[i].tex_coord.x = v[i].tex_coord.y = 0.0f;
    }
    static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    gfx_geom(v, 4, idx, 6);
}
void DrawRectangleRec(Rectangle r, Color c)
{
    if (round_ok(r.width, r.height)) { round_fill(r, c); return; }
    fill_ui_rect((float)(int)r.x, (float)(int)r.y, (float)(int)r.width, (float)(int)r.height, c);
}

/* Frame `t` UI units thick, INSIDE the rectangle (like raylib), in whole physical pixels */
static void frame_rect(float x, float y, float w, float h, float t, Color c)
{
    if (c.a == 0 || w <= 0 || h <= 0) return;
    int x0 = D(x), y0 = D(y), x1 = D(x + w), y1 = D(y + h);
    int tp = D(t);
    if (tp < 1) tp = 1;
    if (x1 - x0 <= 2 * tp || y1 - y0 <= 2 * tp) { fill_dev_rect(x0, y0, x1, y1, c); return; }
    fill_dev_rect(x0, y0, x1, y0 + tp, c);                 /* top    */
    fill_dev_rect(x0, y1 - tp, x1, y1, c);                 /* bottom */
    fill_dev_rect(x0, y0 + tp, x0 + tp, y1 - tp, c);       /* left   */
    fill_dev_rect(x1 - tp, y0 + tp, x1, y1 - tp, c);       /* right  */
}

void DrawRectangleLines(int x, int y, int w, int h, Color c)
{
    if (round_ok((float)w, (float)h)) { rr_geom((Rectangle){ (float)x, (float)y, (float)w, (float)h }, round_r((float)w, (float)h), 1.0f, c); return; }
    frame_rect((float)x, (float)y, (float)w, (float)h, 1.0f, c);
}
void DrawRectangleLinesEx(Rectangle r, float thick, Color c)
{
    if (round_ok(r.width, r.height)) { rr_geom(r, round_r(r.width, r.height), thick < 1 ? 1 : thick, c); return; }
    frame_rect((float)(int)r.x, (float)(int)r.y, (float)(int)r.width, (float)(int)r.height, (float)(int)thick, c);
}

/* ---- anti-aliased geometry (everything is in PHYSICAL coordinates) ---- */

#define MAXV 512
typedef struct { SDL_Vertex v[MAXV]; int n; int idx[MAXV * 3]; int ni; } Mesh;

static void mesh_flush(Mesh *m)
{
    if (m->ni > 0)
        gfx_geom(m->v, m->n, m->idx, m->ni);
    m->n = 0;
    m->ni = 0;
}

static int mesh_vtx(Mesh *m, float x, float y, Color c, float alpha_mul)
{
    SDL_Vertex *v = &m->v[m->n];
    v->position.x = x;
    v->position.y = y;
    v->color.r = c.r; v->color.g = c.g; v->color.b = c.b;
    int a = (int)lroundf((float)c.a * alpha_mul);
    v->color.a = (Uint8)(a < 0 ? 0 : a > 255 ? 255 : a);
    v->tex_coord.x = 0; v->tex_coord.y = 0;
    return m->n++;
}

static void mesh_tri(Mesh *m, int a, int b, int c)
{
    m->idx[m->ni++] = a; m->idx[m->ni++] = b; m->idx[m->ni++] = c;
}

/* Convex polygon with a 1 physical pixel soft (AA) edge */
static void aa_poly(const float *p, int n, Color col)
{
    if (n < 3 || n > 16 || col.a == 0) return;
    if ((g_soft_render || g_eco)) { sw_poly(p, n, col); return; }
    float area = 0;
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        area += p[2 * i] * p[2 * j + 1] - p[2 * j] * p[2 * i + 1];
    }
    float sgn = area > 0 ? 1.0f : -1.0f;       /* orientation */
    float nx[16], ny[16];
    for (int i = 0; i < n; ++i) {              /* outward normals of the edges */
        int j = (i + 1) % n;
        float dx = p[2 * j] - p[2 * i], dy = p[2 * j + 1] - p[2 * i + 1];
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 1e-6f) len = 1e-6f;
        nx[i] = sgn * dy / len;
        ny[i] = -sgn * dx / len;
    }
    Mesh *m = &(Mesh){ .n = 0, .ni = 0 };
    int in[16], out[16];
    const float feather = 0.5f;
    for (int i = 0; i < n; ++i) {
        int k = (i + n - 1) % n;
        float dot = nx[k] * nx[i] + ny[k] * ny[i];
        float den = 1.0f + dot;
        if (den < 0.25f) den = 0.25f;
        float ox = (nx[k] + nx[i]) / den, oy = (ny[k] + ny[i]) / den;
        in[i]  = mesh_vtx(m, p[2 * i] - ox * feather, p[2 * i + 1] - oy * feather, col, 1.0f);
        out[i] = mesh_vtx(m, p[2 * i] + ox * feather, p[2 * i + 1] + oy * feather, col, 0.0f);
    }
    for (int i = 1; i + 1 < n; ++i) mesh_tri(m, in[0], in[i], in[i + 1]);
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        mesh_tri(m, in[i], in[j], out[j]);
        mesh_tri(m, in[i], out[j], out[i]);
    }
    mesh_flush(m);
}

void DrawTriangle(Vector2 a, Vector2 b, Vector2 c, Color col)
{
    float p[6] = { a.x * g_scale, a.y * g_scale, b.x * g_scale, b.y * g_scale, c.x * g_scale, c.y * g_scale };
    aa_poly(p, 3, col);
}

static void line_dev(float x0, float y0, float x1, float y1, float thick, Color c)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    float hx = -dy / len * thick * 0.5f, hy = dx / len * thick * 0.5f;
    float p[8] = { x0 + hx, y0 + hy, x1 + hx, y1 + hy, x1 - hx, y1 - hy, x0 - hx, y0 - hy };
    aa_poly(p, 4, c);
}

void DrawLineEx(Vector2 a, Vector2 b, float thick, Color c)
{
    if (c.a == 0) return;
    float t = thick * g_scale;
    if (t < 1.0f) t = 1.0f;
    line_dev(a.x * g_scale, a.y * g_scale, b.x * g_scale, b.y * g_scale, t, c);
}

void DrawLine(int x1, int y1, int x2, int y2, Color c)
{
    if (c.a == 0) return;
    int tp = D(1.0f);
    if (tp < 1) tp = 1;
    if (y1 == y2) {                                   /* horizontal: whole pixels */
        int a = x1 < x2 ? x1 : x2, b = x1 < x2 ? x2 : x1;
        int py = D((float)y1);
        fill_dev_rect(D((float)a), py, D((float)b), py + tp, c);
    } else if (x1 == x2) {                            /* vertical */
        int a = y1 < y2 ? y1 : y2, b = y1 < y2 ? y2 : y1;
        int px = D((float)x1);
        fill_dev_rect(px, D((float)a), px + tp, D((float)b), c);
    } else {
        line_dev(((float)x1 + 0.5f) * g_scale, ((float)y1 + 0.5f) * g_scale,
                 ((float)x2 + 0.5f) * g_scale, ((float)y2 + 0.5f) * g_scale, (float)tp, c);
    }
}

static int circle_segments(float r_dev)
{
    int n = (int)(r_dev * 1.6f) + 16;
    if (n < 24) n = 24;
    if (n > 120) n = 120;
    return n;
}

void DrawCircleV(Vector2 center, float radius, Color c)
{
    if (c.a == 0 || radius <= 0) return;
    float cx = center.x * g_scale, cy = center.y * g_scale, r = radius * g_scale;
    if ((g_soft_render || g_eco)) { sw_ring(cx, cy, 0.0f, r, c); return; }
    int n = circle_segments(r);
    Mesh *m = &(Mesh){ .n = 0, .ni = 0 };
    float r_in = r - 0.5f;
    if (r_in < 0) r_in = 0;
    int ctr = mesh_vtx(m, cx, cy, c, 1.0f);
    int ring_in[130], ring_out[130];
    for (int i = 0; i < n; ++i) {
        float a = 2.0f * (float)M_PI * (float)i / (float)n;
        float ca = cosf(a), sa = sinf(a);
        ring_in[i]  = mesh_vtx(m, cx + ca * r_in, cy + sa * r_in, c, 1.0f);
        ring_out[i] = mesh_vtx(m, cx + ca * (r + 0.5f), cy + sa * (r + 0.5f), c, 0.0f);
    }
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        mesh_tri(m, ctr, ring_in[i], ring_in[j]);
        mesh_tri(m, ring_in[i], ring_out[i], ring_out[j]);
        mesh_tri(m, ring_in[i], ring_out[j], ring_in[j]);
    }
    mesh_flush(m);
}

void DrawCircleLines(int cx_i, int cy_i, float radius, Color c)
{
    if (c.a == 0 || radius <= 0) return;
    float cx = ((float)cx_i) * g_scale, cy = ((float)cy_i) * g_scale, r = radius * g_scale;
    float t = g_scale < 1.0f ? 1.0f : g_scale;     /* 1 UI unit, at least 1 physical px */
    float r0 = r - t * 0.5f, r1 = r + t * 0.5f;
    if (r0 < 0) r0 = 0;
    if ((g_soft_render || g_eco)) { sw_ring(cx, cy, r0 > 0.0f ? r0 : 0.01f, r1, c); return; }
    int n = circle_segments(r);
    Mesh *m = &(Mesh){ .n = 0, .ni = 0 };
    int a0[130], a1[130], b0[130], b1[130];      /* (r0-.5)(a=0) (r0+.5)(a=1) (r1-.5)(a=1) (r1+.5)(a=0) */
    for (int i = 0; i < n; ++i) {
        float a = 2.0f * (float)M_PI * (float)i / (float)n;
        float ca = cosf(a), sa = sinf(a);
        float ra = r0 - 0.5f; if (ra < 0) ra = 0;
        a0[i] = mesh_vtx(m, cx + ca * ra, cy + sa * ra, c, 0.0f);
        a1[i] = mesh_vtx(m, cx + ca * (r0 + 0.5f), cy + sa * (r0 + 0.5f), c, 1.0f);
        b0[i] = mesh_vtx(m, cx + ca * (r1 - 0.5f), cy + sa * (r1 - 0.5f), c, 1.0f);
        b1[i] = mesh_vtx(m, cx + ca * (r1 + 0.5f), cy + sa * (r1 + 0.5f), c, 0.0f);
    }
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        mesh_tri(m, a0[i], a1[i], a1[j]); mesh_tri(m, a0[i], a1[j], a0[j]);
        mesh_tri(m, a1[i], b0[i], b0[j]); mesh_tri(m, a1[i], b0[j], a1[j]);
        mesh_tri(m, b0[i], b1[i], b1[j]); mesh_tri(m, b0[i], b1[j], b0[j]);
    }
    mesh_flush(m);
}

/* ---- step 36f: rounded rectangles (with soft edges) ---- */
#define RR_SEG 7                                       /* points per corner */
/* the outline of a rounded rectangle (physical px), shifted inwards by inset (negative = outwards) */
static int rr_points(float x0, float y0, float x1, float y1, float rad, float inset, float *px, float *py)
{
    float cxs[4] = { x0 + rad, x1 - rad, x1 - rad, x0 + rad }, cys[4] = { y0 + rad, y0 + rad, y1 - rad, y1 - rad };
    float sx[4] = { x0 + inset, x1 - inset, x1 - inset, x0 + inset }, sy[4] = { y0 + inset, y0 + inset, y1 - inset, y1 - inset };
    float a0[4] = { (float)M_PI, (float)M_PI * 1.5f, 0.0f, (float)M_PI * 0.5f };
    float rr = rad - inset;
    int n = 0;
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < RR_SEG; ++i, ++n) {
            if (rr > 0.0f) {
                static float tc[4][RR_SEG], ts[4][RR_SEG];      /* cos/sin are computed once (hundreds of outlines per frame) */
                static bool tinit;
                if (!tinit) {
                    for (int kk = 0; kk < 4; ++kk)
                        for (int ii = 0; ii < RR_SEG; ++ii) {
                            float a = a0[kk] + (float)M_PI * 0.5f * (float)ii / (float)(RR_SEG - 1);
                            tc[kk][ii] = cosf(a); ts[kk][ii] = sinf(a);
                        }
                    tinit = true;
                }
                px[n] = cxs[k] + tc[k][i] * rr; py[n] = cys[k] + ts[k][i] * rr;
            } else {                                   /* inwards by more than the radius -> sharp corner */
                px[n] = sx[k]; py[n] = sy[k];
            }
        }
    return n;
}

static void rr_geom(Rectangle r, float radius, float thick, Color c)
{
    if (c.a == 0 || r.width <= 0 || r.height <= 0) return;
    float x0 = (float)D(r.x), y0 = (float)D(r.y), x1 = (float)D(r.x + r.width), y1 = (float)D(r.y + r.height);
    float rad = radius * g_scale;
    float maxr = ((x1 - x0) < (y1 - y0) ? (x1 - x0) : (y1 - y0)) * 0.5f;
    if (rad > maxr) rad = maxr;
    if (rad < 0.5f) {                                  /* no rounding */
        if (thick <= 0) fill_dev_rect((int)x0, (int)y0, (int)x1, (int)y1, c);
        else frame_rect(r.x, r.y, r.width, r.height, thick, c);
        return;
    }
    if ((g_soft_render || g_eco)) {                               /* 1.3.4 */
        float t = thick > 0 ? thick * g_scale : 0.0f;
        if (thick > 0 && t < 1.0f) t = 1.0f;
        sw_rrect(x0, y0, x1, y1, rad, t, c);
        return;
    }
    const int N = 4 * RR_SEG;
    float ax[4 * RR_SEG], ay[4 * RR_SEG], bx[4 * RR_SEG], by[4 * RR_SEG];
    Mesh *m = &(Mesh){ .n = 0, .ni = 0 };
    if (thick <= 0) {                                  /* filled: inner outline (solid) + soft edge outwards */
        rr_points(x0, y0, x1, y1, rad, 0.5f, ax, ay);
        rr_points(x0, y0, x1, y1, rad, -0.5f, bx, by);
        int in[4 * RR_SEG], out[4 * RR_SEG];
        for (int i = 0; i < N; ++i) { in[i] = mesh_vtx(m, ax[i], ay[i], c, 1.0f); out[i] = mesh_vtx(m, bx[i], by[i], c, 0.0f); }
        for (int i = 1; i + 1 < N; ++i) mesh_tri(m, in[0], in[i], in[i + 1]);
        for (int i = 0; i < N; ++i) { int j = (i + 1) % N; mesh_tri(m, in[i], in[j], out[j]); mesh_tri(m, in[i], out[j], out[i]); }
        mesh_flush(m);
        return;
    }
    float t = thick * g_scale;
    if (t < 1.0f) t = 1.0f;
    /* frame inside the rectangle: soft outer edge, solid band, soft inner edge */
    float insets[4] = { -0.5f, 0.5f, t - 0.5f, t + 0.5f };
    float alph[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
    if (insets[2] < insets[1]) insets[2] = insets[1];
    int ring[4][4 * RR_SEG];
    for (int k = 0; k < 4; ++k) {
        rr_points(x0, y0, x1, y1, rad, insets[k], ax, ay);
        for (int i = 0; i < N; ++i) ring[k][i] = mesh_vtx(m, ax[i], ay[i], c, alph[k]);
    }
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < N; ++i) {
            int j = (i + 1) % N;
            mesh_tri(m, ring[k][i], ring[k + 1][i], ring[k + 1][j]);
            mesh_tri(m, ring[k][i], ring[k + 1][j], ring[k][j]);
        }
    mesh_flush(m);
    (void)bx; (void)by;
}

void DrawRectangleRounded(Rectangle r, float radius, Color c) { rr_geom(r, radius, 0.0f, c); }
void DrawRectangleRoundedLines(Rectangle r, float radius, float thick, Color c) { rr_geom(r, radius, thick < 1 ? 1 : thick, c); }

/* ---- scissor ---- */
void BeginScissorMode(int x, int y, int w, int h)
{
    SDL_Rect r = { D((float)x), D((float)y), D((float)(x + w)) - D((float)x), D((float)(y + h)) - D((float)y) };
    gfx_clip(&r);
}
void EndScissorMode(void) { gfx_clip(NULL); }

/* ====================================================================== */
/*  Input                                                                  */
/* ====================================================================== */

static int map_scancode(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return KEY_A + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return KEY_ONE + (sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return KEY_F1 + (sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return KEY_KP_1 + (sc - SDL_SCANCODE_KP_1);
    switch (sc) {
    case SDL_SCANCODE_0: return KEY_ZERO;
    case SDL_SCANCODE_KP_0: return KEY_KP_0;
    case SDL_SCANCODE_SPACE: return KEY_SPACE;
    case SDL_SCANCODE_COMMA: return KEY_COMMA;
    case SDL_SCANCODE_MINUS: return KEY_MINUS;
    case SDL_SCANCODE_PERIOD: return KEY_PERIOD;
    case SDL_SCANCODE_SLASH: return KEY_SLASH;
    case SDL_SCANCODE_SEMICOLON: return KEY_SEMICOLON;
    case SDL_SCANCODE_EQUALS: return KEY_EQUAL;
    case SDL_SCANCODE_ESCAPE: return KEY_ESCAPE;
    case SDL_SCANCODE_RETURN: return KEY_ENTER;
    case SDL_SCANCODE_KP_ENTER: return KEY_KP_ENTER;
    case SDL_SCANCODE_TAB: return KEY_TAB;
    case SDL_SCANCODE_BACKSPACE: return KEY_BACKSPACE;
    case SDL_SCANCODE_INSERT: return KEY_INSERT;
    case SDL_SCANCODE_DELETE: return KEY_DELETE;
    case SDL_SCANCODE_RIGHT: return KEY_RIGHT;
    case SDL_SCANCODE_LEFT: return KEY_LEFT;
    case SDL_SCANCODE_DOWN: return KEY_DOWN;
    case SDL_SCANCODE_UP: return KEY_UP;
    case SDL_SCANCODE_PAGEUP: return KEY_PAGE_UP;
    case SDL_SCANCODE_PAGEDOWN: return KEY_PAGE_DOWN;
    case SDL_SCANCODE_HOME: return KEY_HOME;
    case SDL_SCANCODE_END: return KEY_END;
    case SDL_SCANCODE_LSHIFT: return KEY_LEFT_SHIFT;
    case SDL_SCANCODE_LCTRL: return KEY_LEFT_CONTROL;
    case SDL_SCANCODE_LALT: return KEY_LEFT_ALT;
    case SDL_SCANCODE_LGUI: return KEY_LEFT_SUPER;
    case SDL_SCANCODE_RSHIFT: return KEY_RIGHT_SHIFT;
    case SDL_SCANCODE_RCTRL: return KEY_RIGHT_CONTROL;
    case SDL_SCANCODE_RALT: return KEY_RIGHT_ALT;
    case SDL_SCANCODE_RGUI: return KEY_RIGHT_SUPER;
    default: return 0;
    }
}

static int map_button(Uint8 b)
{
    if (b == SDL_BUTTON_LEFT) return 0;
    if (b == SDL_BUTTON_RIGHT) return 1;
    if (b == SDL_BUTTON_MIDDLE) return 2;
    return -1;
}

static void drop_clear(void)
{
    for (unsigned i = 0; i < g_drop_n; ++i) free(g_drop_paths[i]);
    g_drop_n = 0;
}

static void drop_add(const char *path)
{
    if (g_drop_n == g_drop_cap) {
        unsigned nc = g_drop_cap ? g_drop_cap * 2 : 16;
        char **np = realloc(g_drop_paths, nc * sizeof *np);
        if (!np) return;
        g_drop_paths = np;
        g_drop_cap = nc;
    }
    g_drop_paths[g_drop_n++] = strdup(path);
}

static void char_push(const char *utf8)
{
    const unsigned char *s = (const unsigned char *)utf8;
    while (*s) {
        int cp, len;
        if (s[0] < 0x80) { cp = s[0]; len = 1; }
        else if ((s[0] & 0xE0) == 0xC0 && s[1]) { cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); len = 2; }
        else if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) { cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); len = 3; }
        else if ((s[0] & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) { cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); len = 4; }
        else { cp = '?'; len = 1; }
        if (g_char_n < CHARQ) g_char_q[g_char_n++] = cp;
        s += len;
    }
}

static SDL_Event g_held;          /* mouse: an event left for the next frame (see poll_events) */
static bool      g_have_held;

static void poll_events(void)
{
    /* reset the "one-shot" states of the previous frame */
    memset(g_key_pressed, 0, sizeof g_key_pressed);
    memset(g_key_repeat, 0, sizeof g_key_repeat);
    memset(g_btn_pressed, 0, sizeof g_btn_pressed);
    memset(g_btn_released, 0, sizeof g_btn_released);
    g_wheel = 0;
    g_mouse_delta = (Vector2){ 0.0f, 0.0f };
    g_char_n = 0;
    g_drop_ready = false;

    SDL_Event e;
    bool drop_batch = false;
    /* At most ONE mouse button change per frame. Otherwise with a slow frame (or a very short tap
     * on a touchpad) press + release (or two clicks at different places) merge into one frame:
     * the interface "misses" the release and the next click becomes a drag (moving a clip).
     * After a button event everything mouse-related (button/motion) is left for the next frame. */
    bool btn_now = false;
    for (;;) {
        if (g_have_held) { e = g_held; g_have_held = false; }
        else if (!SDL_PollEvent(&e)) break;
        bool is_btn = e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP;
        if (btn_now && (is_btn || e.type == SDL_MOUSEMOTION)) {
            g_held = e; g_have_held = true;
            break;
        }
        if (is_btn) btn_now = true;
        /* 1.3.4: "touched" = full speed; 1.3.9 Eco: moving the mouse alone does not count */
        if (!(g_eco && e.type == SDL_MOUSEMOTION && !g_btn_down[0] && !g_btn_down[1] && !g_btn_down[2] && !g_mouse_rel))
            g_last_input = (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
        switch (e.type) {
        case SDL_QUIT:
            g_should_close = true;
            break;
        case SDL_MOUSEMOTION:
            {
                int ww = 1, wh = 1;
                SDL_GetWindowSize(g_win, &ww, &wh);
                float k = (float)g_drawable_w / (float)(ww > 0 ? ww : 1);
                g_mouse_delta.x += (float)e.motion.xrel * k;
                g_mouse_delta.y += (float)e.motion.yrel * k;
                if (!g_mouse_rel) {               /* in "infinite" mode the position stays put */
                    g_mouse.x = (float)e.motion.x * k;
                    g_mouse.y = (float)e.motion.y * k;
                }
            }
            break;
        case SDL_MOUSEBUTTONDOWN: {
            int b = map_button(e.button.button);
            if (b >= 0) { g_btn_down[b] = true; g_btn_pressed[b] = true; g_btn_clicks[b] = e.button.clicks; }
            break;
        }
        case SDL_MOUSEBUTTONUP: {
            int b = map_button(e.button.button);
            if (b >= 0) { g_btn_down[b] = false; g_btn_released[b] = true; }
            break;
        }
        case SDL_MOUSEWHEEL:
            g_wheel += e.wheel.preciseY * (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f);
            break;
        case SDL_KEYDOWN: {
            int k = map_scancode(e.key.keysym.scancode);
            if (k > 0 && k < NKEYS) {
                if (e.key.repeat) g_key_repeat[k] = true;
                else { g_key_pressed[k] = true; g_key_down[k] = true; }
            }
            break;
        }
        case SDL_KEYUP: {
            int k = map_scancode(e.key.keysym.scancode);
            if (k > 0 && k < NKEYS) g_key_down[k] = false;
            break;
        }
        case SDL_TEXTINPUT:
            char_push(e.text.text);
            break;
        case SDL_DROPBEGIN:
            drop_clear();
            drop_batch = true;
            break;
        case SDL_DROPFILE:
            if (e.drop.file) {
                if (!drop_batch) drop_clear();
#ifdef _WIN32
                qw_slashes(e.drop.file);           /* Windows gives C:\folder\file - Q-LiM uses '/' */
#endif
                drop_add(e.drop.file);
                SDL_free(e.drop.file);
                g_drop_ready = true;
            }
            break;
        case SDL_DROPCOMPLETE:
            drop_batch = false;
            break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_LEAVE) {
                /* the cursor has left: release the buttons so they do not "stick" */
            }
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                memset(g_key_down, 0, sizeof g_key_down);
            }
            if (e.window.event == SDL_WINDOWEVENT_EXPOSED || e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                e.window.event == SDL_WINDOWEVENT_RESTORED || e.window.event == SDL_WINDOWEVENT_SHOWN ||
                e.window.event == SDL_WINDOWEVENT_MAXIMIZED)
                g_push_all = true;                     /* 1.3.5: the window needs the whole picture again */
            break;
        case SDL_RENDER_TARGETS_RESET:
        case SDL_RENDER_DEVICE_RESET:
            g_eco_full = true;                         /* 1.3.8: the kept picture is lost */
            g_push_all = true;
            break;
        default:
            break;
        }
    }

    if (g_drop_ready) {
        /* During a drag from outside X11 does not send us mouse motion,
         * so we ask the system where the cursor is at the moment of the drop. */
        int gx = 0, gy = 0, wx = 0, wy = 0, ww = 1;
        SDL_GetGlobalMouseState(&gx, &gy);
        SDL_GetWindowPosition(g_win, &wx, &wy);
        SDL_GetWindowSize(g_win, &ww, NULL);
        float k = (float)g_drawable_w / (float)(ww > 0 ? ww : 1);
        g_drop_pos.x = (float)(gx - wx) * k;
        g_drop_pos.y = (float)(gy - wy) * k;
        g_mouse = g_drop_pos;
    }

    if (g_cursor_req != g_cursor_cur) {
        static const int sys[] = { SDL_SYSTEM_CURSOR_ARROW, SDL_SYSTEM_CURSOR_ARROW, SDL_SYSTEM_CURSOR_IBEAM,
                                   SDL_SYSTEM_CURSOR_CROSSHAIR, SDL_SYSTEM_CURSOR_HAND,
                                   SDL_SYSTEM_CURSOR_SIZEWE, SDL_SYSTEM_CURSOR_SIZENS };
        int id = g_cursor_req;
        if (id < 0 || id > 6) id = 0;
        if (!g_cursors[id]) g_cursors[id] = SDL_CreateSystemCursor((SDL_SystemCursor)sys[id]);
        if (g_cursors[id]) SDL_SetCursor(g_cursors[id]);
        g_cursor_cur = g_cursor_req;
    }
}

static bool g_input_off;     /* while it is raised, the interface "sees" no mouse and keyboard (passive drawing of the background) */
void GfxInputSuppress(bool off) { g_input_off = off; }
static bool g_mouse_shield;  /* mouse only: e.g. while a popup is open, the elements below it do not see it */
void GfxMouseShield(bool on) { g_mouse_shield = on; }
bool GfxMouseShieldGet(void) { return g_mouse_shield; }
#define MOUSE_OFF (g_input_off || g_mouse_shield)

Vector2 GetMousePosition(void)
{
    if (MOUSE_OFF) return (Vector2){ -10000.0f, -10000.0f };
    /* 1.3.9 Eco: a mouse that only moves does not exist for the interface (no hover highlights, nothing to redraw);
     * it is there as soon as a button, the wheel or a key is used */
    if (g_eco && !g_frame_active) return (Vector2){ -20000.0f, -20000.0f };   /* (see GfxHoverCheck) */
    return (Vector2){ g_mouse.x / g_scale, g_mouse.y / g_scale };
}
Vector2 GfxMouseTrue(void)
{
    if (MOUSE_OFF) return (Vector2){ -10000.0f, -10000.0f };
    return (Vector2){ g_mouse.x / g_scale, g_mouse.y / g_scale };
}
Vector2 GetMouseDelta(void)
{
    return MOUSE_OFF ? (Vector2){ 0.0f, 0.0f } : (Vector2){ g_mouse_delta.x / g_scale, g_mouse_delta.y / g_scale };
}

void GfxMouseRelative(bool on)
{
    if (on == g_mouse_rel) return;
    if (on) {
        g_mouse_rel_home = g_mouse;
        if (SDL_SetRelativeMouseMode(SDL_TRUE) == 0) g_mouse_rel = true;
    } else {
        SDL_SetRelativeMouseMode(SDL_FALSE);
        g_mouse_rel = false;
        int ww = 1;                               /* the cursor goes back to where the drag started */
        SDL_GetWindowSize(g_win, &ww, NULL);
        float k = (float)g_drawable_w / (float)(ww > 0 ? ww : 1);
        if (k > 0.0f) SDL_WarpMouseInWindow(g_win, (int)(g_mouse_rel_home.x / k), (int)(g_mouse_rel_home.y / k));
        g_mouse = g_mouse_rel_home;
    }
}
int GetMouseClicks(int b) { return (!MOUSE_OFF && b >= 0 && b < 3 && g_btn_pressed[b]) ? g_btn_clicks[b] : 0; }
bool IsMouseButtonPressed(int b)  { return !MOUSE_OFF && b >= 0 && b < 3 && g_btn_pressed[b]; }
static volatile int g_ext_dialog;     /* step 36l: a foreign window is open (file chooser) - the "freeze" is not an error */
bool GfxExternalDialogActive(void) { return g_ext_dialog > 0; }
void GfxRaiseWindow(void) { if (g_win) { SDL_RestoreWindow(g_win); SDL_RaiseWindow(g_win); } }

void GfxExternalDialogBegin(void)
{
    __atomic_add_fetch(&g_ext_dialog, 1, __ATOMIC_SEQ_CST);
    SDL_CaptureMouse(SDL_FALSE);          /* release any mouse grab */
    SDL_PumpEvents();
}

void GfxExternalDialogEnd(void)
{
    __atomic_sub_fetch(&g_ext_dialog, 1, __ATOMIC_SEQ_CST);
    /* buttons/keys released while the foreign window was running did not reach us -> reset */
    memset(g_btn_down, 0, sizeof g_btn_down);
    memset(g_btn_pressed, 0, sizeof g_btn_pressed);
    memset(g_btn_released, 0, sizeof g_btn_released);
    memset(g_key_down, 0, sizeof g_key_down);
    g_have_held = false;
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_MOUSEMOTION, SDL_MOUSEWHEEL);
}

bool IsMouseButtonDown(int b)     { return !MOUSE_OFF && b >= 0 && b < 3 && g_btn_down[b]; }
bool IsMouseButtonReleased(int b) { return !MOUSE_OFF && b >= 0 && b < 3 && g_btn_released[b]; }
float GetMouseWheelMove(void) { return MOUSE_OFF ? 0.0f : g_wheel; }
void SetMouseCursor(int cursor) { if (!MOUSE_OFF) g_cursor_req = cursor; }
bool IsKeyPressed(int k)       { return !g_input_off && k > 0 && k < NKEYS && g_key_pressed[k]; }
bool IsKeyPressedRepeat(int k) { return !g_input_off && k > 0 && k < NKEYS && g_key_repeat[k]; }
bool IsKeyDown(int k)          { return !g_input_off && k > 0 && k < NKEYS && g_key_down[k]; }

int GetCharPressed(void)
{
    if (g_input_off) return 0;
    if (g_char_n == 0) return 0;
    int c = g_char_q[0];
    memmove(g_char_q, g_char_q + 1, (size_t)(g_char_n - 1) * sizeof(int));
    --g_char_n;
    return c;
}

bool IsFileDropped(void) { return g_drop_ready && g_drop_n > 0; }

FilePathList LoadDroppedFiles(void)
{
    FilePathList l = { 0, 0, NULL };
    if (g_drop_n == 0) return l;
    l.paths = calloc(g_drop_n, sizeof(char *));
    if (!l.paths) return l;
    for (unsigned i = 0; i < g_drop_n; ++i) l.paths[i] = strdup(g_drop_paths[i]);
    l.count = l.capacity = g_drop_n;
    return l;
}

void UnloadDroppedFiles(FilePathList files)
{
    for (unsigned i = 0; i < files.count; ++i) free(files.paths[i]);
    free(files.paths);
    g_drop_ready = false;
    drop_clear();
}

Vector2 GetDropPosition(void) { return (Vector2){ g_drop_pos.x / g_scale, g_drop_pos.y / g_scale }; }

/* ====================================================================== */
/*  Frame                                                                  */
/* ====================================================================== */

/* ====================================================================== */
/*  Blurred background behind dialogs                                      */
/* ====================================================================== */
/* The first time a dialog asks for a background, we capture the finished frame (the interface behind), shrink it
 * 4 times, blur it slightly and keep it as a texture. The following frames only draw it stretched
 * (linear filtering = even more blur) -> almost free, even on an old laptop.
 * While the dialog is open, the background is "frozen" - the screen is not read every frame. */

#define BD_DIV 4
static SDL_Texture *g_bd_tex;
static unsigned     g_bd_serial;             /* 1.3.9: for the redraw fingerprints */
static int          g_bd_w, g_bd_h;            /* size of the shrunk texture */
static int          g_bd_src_w, g_bd_src_h;    /* the window size it was made for */
static int          g_bd_calls;                /* calls in this frame */
static double       g_bd_time;                 /* when the last capture was made (seconds) */
#define BD_REFRESH_S 0.066                     /* ~15 captures per second: the background is alive, but light on resources */
static int          g_bd_prev_calls;

static void bd_free(void)
{
    if (g_bd_tex) { SDL_DestroyTexture(g_bd_tex); g_bd_tex = NULL; }
}

static void bd_blur_pass(Uint8 *px, Uint8 *tmp, int w, int h)
{
    for (int pass = 0; pass < 2; ++pass) {
        /* horizontal 3-tap, then vertical */
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 4; ++c) {
                    int a = px[(y * w + (x > 0 ? x - 1 : x)) * 4 + c];
                    int b = px[(y * w + x) * 4 + c];
                    int d = px[(y * w + (x < w - 1 ? x + 1 : x)) * 4 + c];
                    tmp[(y * w + x) * 4 + c] = (Uint8)((a + 2 * b + d) / 4);
                }
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 4; ++c) {
                    int a = tmp[((y > 0 ? y - 1 : y) * w + x) * 4 + c];
                    int b = tmp[(y * w + x) * 4 + c];
                    int d = tmp[((y < h - 1 ? y + 1 : y) * w + x) * 4 + c];
                    px[(y * w + x) * 4 + c] = (Uint8)((a + 2 * b + d) / 4);
                }
    }
}

static void bd_capture(void)
{
    int W = g_drawable_w, H = g_drawable_h;
    if (W < BD_DIV * 2 || H < BD_DIV * 2) return;
    Uint8 *full = (Uint8 *)malloc((size_t)W * H * 4);
    if (!full) return;
    SDL_Rect area = { 0, 0, W, H };                  /* 1.3.6: not NULL - in software mode the image is bigger than the window */
    if (g_rec) {
        /* 1.3.9: this frame is only being recorded: what has been recorded so far (the window without the dialog)
         * is drawn now, whole, into the kept picture and read from there; the next present draws everything again */
        bool ok = false;
        if (!g_off && (!g_tgt || g_tgt_w != W || g_tgt_h != H)) {
            if (g_tgt) SDL_DestroyTexture(g_tgt);
            g_tgt = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, W, H);
            if (g_tgt) SDL_SetTextureBlendMode(g_tgt, SDL_BLENDMODE_NONE);
            g_tgt_w = W; g_tgt_h = H;
        }
        if (g_off || g_tgt) {
            if (!g_off) SDL_SetRenderTarget(g_ren, g_tgt);
            for (int k = 0; k < g_ncmd; ++k) rc_play(&g_cmd[k], &area);
            SDL_RenderSetClipRect(g_ren, NULL);
            SDL_RenderFlush(g_ren);
            if (!g_off) SDL_SetRenderTarget(g_ren, NULL);
            g_eco_full = true;
        }
        if (g_off && g_off->w >= W && g_off->h >= H) {
            for (int y = 0; y < H; ++y) {
                const Uint32 *src = (const Uint32 *)((const Uint8 *)g_off->pixels + (size_t)y * g_off->pitch);
                Uint32 *dst = (Uint32 *)(full + (size_t)y * W * 4);
                for (int x = 0; x < W; ++x) dst[x] = src[x] | 0xFF000000u;
            }
            ok = true;
        } else if (g_tgt && g_tgt_w == W && g_tgt_h == H) {
            SDL_SetRenderTarget(g_ren, g_tgt);
            ok = SDL_RenderReadPixels(g_ren, &area, SDL_PIXELFORMAT_ARGB8888, full, W * 4) == 0;
            SDL_SetRenderTarget(g_ren, NULL);
        }
        if (!ok) { free(full); return; }
    } else if (SDL_RenderReadPixels(g_ren, &area, SDL_PIXELFORMAT_ARGB8888, full, W * 4) != 0) { free(full); return; }

    int w = W / BD_DIV, h = H / BD_DIV;
    Uint8 *small = (Uint8 *)malloc((size_t)w * h * 4 * 2);
    if (!small) { free(full); return; }
    Uint8 *tmp = small + (size_t)w * h * 4;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int sum[4] = { 0, 0, 0, 0 };
            for (int dy = 0; dy < BD_DIV; ++dy)
                for (int dx = 0; dx < BD_DIV; ++dx) {
                    const Uint8 *p = full + ((size_t)(y * BD_DIV + dy) * W + (x * BD_DIV + dx)) * 4;
                    sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; sum[3] += p[3];
                }
            Uint8 *o = small + ((size_t)y * w + x) * 4;
            for (int c = 0; c < 4; ++c) o[c] = (Uint8)(sum[c] / (BD_DIV * BD_DIV));
        }
    free(full);
    bd_blur_pass(small, tmp, w, h);

    if (g_bd_tex && (g_bd_w != w || g_bd_h != h)) bd_free();
    if (!g_bd_tex) {
        SDL_SetHintWithPriority(SDL_HINT_RENDER_SCALE_QUALITY, "1", SDL_HINT_OVERRIDE);
        g_bd_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    }
    if (g_bd_tex) SDL_UpdateTexture(g_bd_tex, NULL, small, w * 4);
    g_bd_serial = ++g_tex_serial;
    g_bd_time = GetTime();
    free(small);
    g_bd_w = w; g_bd_h = h;
    g_bd_src_w = W; g_bd_src_h = H;
}

static bool g_bd_want;      /* GfxBackdropNeeded() said "draw the world" -> capture in this frame */
static const bool g_bd_blur = false;   /* 1.3.17: the blurred background behind dialogs is not used any more (the user prefers the darkened one) */

bool GfxBackdropNeeded(void)
{
    if (g_eco || !g_bd_blur) return true;        /* 1.3.8: Eco - the window darkened (not blurred) behind dialogs; it does
                                                  * not change from frame to frame, so it is not drawn again.
                                                  * 1.3.17: the darkened look is now used always */
    bool need = g_bd_calls == 0 &&
                (!g_bd_tex || g_bd_src_w != g_drawable_w || g_bd_src_h != g_drawable_h ||
                 (!g_rec && GetTime() - g_bd_time >= BD_REFRESH_S));   /* 1.3.9: recorded frames: captured once */
    if (need) g_bd_want = true;
    return need;
}

void GfxDialogBackdrop(int dim_alpha)
{
    int first = (g_bd_calls++ == 0);
    bool dark = g_eco || !g_bd_blur;                  /* 1.3.17: always darkened, never blurred (as in Eco) */
    if (first && g_bd_want && !dark) { g_bd_want = false; bd_capture(); }
    gfx_clip(NULL);
    if (first && g_bd_tex && !dark) {
        SDL_Rect dst = { 0, 0, g_drawable_w, g_drawable_h };
        gfx_copy(g_bd_tex, g_bd_serial, (Color){ 255, 255, 255, 255 }, dst);
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), (Color){ 0, 0, 0, (unsigned char)dim_alpha });
    } else if (first && dark) {
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), (Color){ 0, 0, 0, 150 });
    } else {
        /* nested dialog (or no texture): plain darkening */
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                      (Color){ 0, 0, 0, (unsigned char)(first ? 180 : dim_alpha) });
    }
}

void BeginDrawing(void)
{
    /* if there was no dialog in the previous frame, the capture is stale */
    if (g_bd_prev_calls == 0 && g_bd_calls == 0) bd_free();
    g_bd_prev_calls = g_bd_calls;
    g_bd_calls = 0;
    g_frame_start = SDL_GetPerformanceCounter();
    update_scale();
    g_cursor_req = MOUSE_CURSOR_DEFAULT;     /* ui.c raises it only while over the handle */
    {   /* 1.3.9 Eco: is the mouse "used" in this frame (see GetMousePosition) */
        bool a = g_btn_down[0] || g_btn_down[1] || g_btn_down[2] || g_btn_pressed[0] || g_btn_pressed[1] ||
                 g_btn_pressed[2] || g_btn_released[0] || g_btn_released[1] || g_btn_released[2] ||
                 g_mouse_rel || g_drop_ready;
        for (int k = 0; k < NKEYS && !a; ++k) a = g_key_pressed[k] || g_key_down[k];
        g_frame_active = a || g_wheel != 0.0f;
        /* 1.3.11: no button / key -> the hover checks are filtered: the interface sees no hover (nothing lights up,
         * also not while scrolling), Q-LiM only notes the smallest thing under the mouse and outlines it itself */
        g_gfx_hover_filter = g_eco && !a;
        g_hov_area = 1e30f;
    }
    {
        bool r = g_smart || g_eco;
        static bool last_eco;
        if (g_rec != r || last_eco != g_eco) g_eco_full = true;   /* 1.3.8: switched on/off */
        g_rec = r; last_eco = g_eco;
    }
    g_ncmd = 0; g_arn = 0; g_clip_on = false;
    SDL_RenderSetClipRect(g_ren, NULL);
}

int GfxScreenshot(const char *path)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, g_drawable_w, g_drawable_h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return -1;
    SDL_Rect area = { 0, 0, g_drawable_w, g_drawable_h };
    int rc = SDL_RenderReadPixels(g_ren, &area, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
    if (rc == 0) rc = SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
    return rc;
}

/* The screen refresh rate (Hz), checked from time to time (the window may be moved to another screen) */
static int display_hz(void)
{
    static int hz = 0;
    static Uint64 last = 0;
    Uint64 now = SDL_GetPerformanceCounter();
    if (hz == 0 || now - last > 2 * SDL_GetPerformanceFrequency()) {
        last = now;
        SDL_DisplayMode dm;
        int idx = g_win ? SDL_GetWindowDisplayIndex(g_win) : 0;
        int r = (idx >= 0 && SDL_GetCurrentDisplayMode(idx, &dm) == 0) ? dm.refresh_rate : 0;
        int nhz = (r >= 30 && r <= 500) ? r : 60;
        if (nhz != hz) fprintf(stderr, "Q-LiM graphics: display %d Hz\n", nhz);
        hz = nhz;
    }
    return hz;
}

/* 1.3.5: software mode - compare the new picture with the last one in tiles and send only the changed tiles */
#define PT_W 64
#define PT_H 16
static void soft_present(void)
{
    SDL_RenderPresent(g_ren);                          /* draws the queued commands into g_off */
    if (SDL_GetWindowFlags(g_win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) { g_push_all = true; return; }
    SDL_Surface *ws = SDL_GetWindowSurface(g_win);
    if (!ws) return;
    int W = ws->w < g_off->w ? ws->w : g_off->w, H = ws->h < g_off->h ? ws->h : g_off->h;
    static SDL_Surface *last_ws;
    if (ws != last_ws || W != g_prev_w || H != g_prev_h || !g_prev) {
        free(g_prev);
        g_prev = (Uint8 *)malloc((size_t)W * (size_t)H * 4);
        g_prev_w = W; g_prev_h = H; last_ws = ws;
        g_push_all = true;
        if (!g_prev) { g_prev_w = g_prev_h = 0; return; }
    }
    static SDL_Rect rr[1024];
    int nr = 0;
    static int dbg_all = -1;
    if (dbg_all < 0) dbg_all = getenv("QLIM_PUSH_ALL") != NULL;
    bool all = g_push_all || dbg_all;
    g_push_all = false;
    if (SDL_MUSTLOCK(g_off)) SDL_LockSurface(g_off);
    const Uint8 *src = (const Uint8 *)g_off->pixels;
    for (int ty = 0; ty < H && !all; ty += PT_H) {
        int th = H - ty < PT_H ? H - ty : PT_H;
        int run0 = -1;
        for (int tx = 0; ; tx += PT_W) {
            if (tx > W) tx = W;                        /* one last step exactly at the right edge: closes the run */
            bool ch = false;
            if (tx < W) {
                int tw = W - tx < PT_W ? W - tx : PT_W;
                for (int y = ty; y < ty + th; ++y) {
                    const Uint8 *a = src + (size_t)y * g_off->pitch + (size_t)tx * 4;
                    Uint8 *b = g_prev + ((size_t)y * W + tx) * 4;
                    if (memcmp(a, b, (size_t)tw * 4) != 0) { ch = true; break; }
                }
                if (ch)
                    for (int y = ty; y < ty + th; ++y)
                        memcpy(g_prev + ((size_t)y * W + tx) * 4, src + (size_t)y * g_off->pitch + (size_t)tx * 4, (size_t)tw * 4);
            }
            if (ch && run0 < 0) run0 = tx;
            if (!ch && run0 >= 0) {                    /* neighbouring changed tiles = one rectangle */
                int x1 = tx < W ? tx : W;
                if (nr < 1024) rr[nr++] = (SDL_Rect){ run0, ty, x1 - run0, th };
                else all = true;
                run0 = -1;
            }
            if (tx >= W) break;
        }
    }
    if (all) {
        for (int y = 0; y < H; ++y) memcpy(g_prev + (size_t)y * W * 4, src + (size_t)y * g_off->pitch, (size_t)W * 4);
        nr = 1;
        rr[0] = (SDL_Rect){ 0, 0, W, H };
    }
    if (SDL_MUSTLOCK(g_off)) SDL_UnlockSurface(g_off);
    { double a = 0; for (int i = 0; i < nr; ++i) a += (double)rr[i].w * rr[i].h; g_pushed += a / ((double)W * H); }
    if (nr == 0) return;
    for (int i = 0; i < nr; ++i) { SDL_Rect r = rr[i]; SDL_BlitSurface(g_off, &rr[i], ws, &r); }
    SDL_UpdateWindowSurfaceRects(g_win, rr, nr);
}

/* 1.3.8: Eco - software mode: send the given rectangles of g_off to the window (all = the whole window) */
static void soft_push(const SDL_Rect *rr, int nr)
{
    if (SDL_GetWindowFlags(g_win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) { g_push_all = true; return; }
    SDL_Surface *ws = SDL_GetWindowSurface(g_win);
    if (!ws) return;
    static SDL_Surface *last_ws;
    if (ws != last_ws) { last_ws = ws; g_push_all = true; }
    int W = ws->w < g_off->w ? ws->w : g_off->w, H = ws->h < g_off->h ? ws->h : g_off->h;
    SDL_Rect all = { 0, 0, W, H };
    if (g_push_all) { rr = &all; nr = 1; g_push_all = false; }
    if (nr <= 0) return;
    { double a = 0; for (int i = 0; i < nr; ++i) a += (double)rr[i].w * rr[i].h; g_pushed += a / ((double)W * H); }
    for (int i = 0; i < nr; ++i) { SDL_Rect s2 = rr[i], d2 = rr[i]; SDL_BlitSurface(g_off, &s2, ws, &d2); }
    SDL_UpdateWindowSurfaceRects(g_win, rr, nr);
    g_prev_w = 0;                                     /* the normal (non-Eco) path compares from scratch */
}

#define ET_W 64
#define ET_H 32
static unsigned *g_th, *g_thp;                        /* tile fingerprints: this frame / the last one */
static int       g_tnx, g_tny;
static int       g_eco_drawn;                         /* QLIM_FPS: tiles drawn (sum) */

static void eco_present(void)
{
    int W = g_drawable_w, H = g_drawable_h;
    if (W <= 0 || H <= 0) return;
    int nx = (W + ET_W - 1) / ET_W, ny = (H + ET_H - 1) / ET_H;
    if (nx != g_tnx || ny != g_tny || !g_th || !g_thp) {
        free(g_th); free(g_thp);
        g_th = (unsigned *)calloc((size_t)nx * ny, sizeof(unsigned));
        g_thp = (unsigned *)calloc((size_t)nx * ny, sizeof(unsigned));
        g_tnx = nx; g_tny = ny;
        g_eco_full = true;
        if (!g_th || !g_thp) { g_tnx = g_tny = 0; return; }
    }
    if (!g_off && (!g_tgt || g_tgt_w != W || g_tgt_h != H)) {
        if (g_tgt) SDL_DestroyTexture(g_tgt);
        g_tgt = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, W, H);
        if (g_tgt) SDL_SetTextureBlendMode(g_tgt, SDL_BLENDMODE_NONE);
        g_tgt_w = W; g_tgt_h = H;
        g_eco_full = true;
    }
    bool direct = !g_off && !g_tgt;                   /* no kept picture possible: draw everything every time */
    for (int i = 0; i < nx * ny; ++i) g_th[i] = 2166136261u;
    for (int k = 0; k < g_ncmd; ++k) {
        const RCmd *c = &g_cmd[k];
        int tx0 = c->bb.x / ET_W, ty0 = c->bb.y / ET_H;
        int tx1 = (c->bb.x + c->bb.w - 1) / ET_W, ty1 = (c->bb.y + c->bb.h - 1) / ET_H;
        if (tx0 < 0) tx0 = 0;
        if (ty0 < 0) ty0 = 0;
        if (tx1 >= nx) tx1 = nx - 1;
        if (ty1 >= ny) ty1 = ny - 1;
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx) { unsigned *t = &g_th[ty * nx + tx]; *t = (*t ^ c->hash) * 16777619u; }
    }
    /* the changed tiles -> rectangles (runs in a row, then the same run in the rows below joined) */
    static SDL_Rect rr[2048];
    int nr = 0, ndirty = 0;
    bool full = g_eco_full || direct;
    for (int ty = 0; ty < ny; ++ty) {
        int run0 = -1;
        for (int tx = 0; tx <= nx; ++tx) {
            bool d = tx < nx && (full || g_th[ty * nx + tx] != g_thp[ty * nx + tx]);
            if (d) ++ndirty;
            if (d && run0 < 0) run0 = tx;
            if (!d && run0 >= 0) {
                SDL_Rect r = { run0 * ET_W, ty * ET_H, (tx - run0) * ET_W, ET_H };
                if (r.x + r.w > W) r.w = W - r.x;
                if (r.y + r.h > H) r.h = H - r.y;
                bool merged = false;
                for (int q = nr - 1; q >= 0 && rr[q].y + rr[q].h >= r.y; --q)
                    if (rr[q].x == r.x && rr[q].w == r.w && rr[q].y + rr[q].h == r.y) { rr[q].h += r.h; merged = true; break; }
                if (!merged) {
                    if (nr < 2048) rr[nr++] = r;
                    else { full = true; }
                }
                run0 = -1;
            }
        }
    }
    memcpy(g_thp, g_th, sizeof(unsigned) * (size_t)nx * ny);
    if (full) { nr = 1; rr[0] = (SDL_Rect){ 0, 0, W, H }; ndirty = nx * ny; }
    g_eco_drawn += ndirty;
    g_eco_full = false;

    if (nr == 0) {                                    /* nothing changed */
        if (g_off) { if (g_push_all) soft_push(NULL, 0); }
        else if (g_push_all && g_tgt) {               /* the window was uncovered: show the kept picture again */
            g_push_all = false;
            SDL_RenderSetClipRect(g_ren, NULL);
            SDL_RenderCopy(g_ren, g_tgt, NULL, NULL);
            SDL_RenderPresent(g_ren);
        }
        return;
    }
    if (g_tgt && !g_off) SDL_SetRenderTarget(g_ren, g_tgt);
    for (int i = 0; i < nr; ++i)
        for (int k = 0; k < g_ncmd; ++k) {
            const RCmd *c = &g_cmd[k];
            if (c->bb.x >= rr[i].x + rr[i].w || c->bb.x + c->bb.w <= rr[i].x ||
                c->bb.y >= rr[i].y + rr[i].h || c->bb.y + c->bb.h <= rr[i].y) continue;
            rc_play(c, &rr[i]);
        }
    SDL_RenderSetClipRect(g_ren, NULL);
    if (g_off) {
        SDL_RenderPresent(g_ren);                     /* draws into g_off */
        soft_push(rr, nr);
    } else {
        if (g_tgt) {
            SDL_SetRenderTarget(g_ren, NULL);
            SDL_RenderCopy(g_ren, g_tgt, NULL, NULL);
        }
        g_push_all = false;
        SDL_RenderPresent(g_ren);
    }
}
void GfxSetEco(bool on) { g_eco = on; }
void GfxSetSmart(bool on) { g_smart = on; }
void  GfxSetZoom(float z) { g_zoom = z < 0.5f ? 0.5f : z > 2.5f ? 2.5f : z; }
float GfxGetZoom(void) { return g_zoom; }
void GfxUnzoomBegin(void) { if (g_unzoom++ == 0) { g_saved_scale = g_scale; g_scale = g_base_scale; } }
void GfxUnzoomEnd(void) { if (g_unzoom > 0 && --g_unzoom == 0) g_scale = g_saved_scale; }
bool GfxEco(void) { return g_eco; }

/* ---- 1.3.11: Eco - a simple hover: one thin outline around the smallest thing under the mouse ---- */
bool  g_gfx_hover_filter;
static float     g_hov_area;
static SDL_Rect  g_hov_px;                /* in physical pixels */
static int       g_hov_cursor;
#define ECO_SENT (-20000.0f)
bool GfxHoverCheck(Vector2 p, Rectangle r)
{
    bool in = p.x >= r.x && p.x < r.x + r.width && p.y >= r.y && p.y < r.y + r.height;
    Vector2 mu = { g_mouse.x / g_scale, g_mouse.y / g_scale };
    bool sent = p.x == ECO_SENT && p.y == ECO_SENT;
    if (!sent && !(p.x == mu.x && p.y == mu.y)) return in;          /* not about the mouse */
    float area = r.width * r.height;
    float screen = (float)GetScreenWidth() * (float)GetScreenHeight();
    SDL_Rect px = { (int)lroundf(r.x * g_scale), (int)lroundf(r.y * g_scale),
                    (int)lroundf(r.width * g_scale), (int)lroundf(r.height * g_scale) };
    /* 1.3.12: a thing cut away by the scissor (scrolled out, hidden part) is not under the mouse */
    bool vis = !g_clip_on || (g_mouse.x >= g_clip_cur.x && g_mouse.x < g_clip_cur.x + g_clip_cur.w &&
                              g_mouse.y >= g_clip_cur.y && g_mouse.y < g_clip_cur.y + g_clip_cur.h);
    /* thin grips (the edges that are dragged) win over everything else; otherwise the smallest thing wins */
    int cur = r.width <= 10.0f && r.height > r.width * 2 ? MOUSE_CURSOR_RESIZE_EW
            : r.height <= 10.0f && r.width > r.height * 2 ? MOUSE_CURSOR_RESIZE_NS : MOUSE_CURSOR_DEFAULT;
    bool grip = cur != MOUSE_CURSOR_DEFAULT;
    bool old_grip = g_hov_area < 1e29f && g_hov_cursor != MOUSE_CURSOR_DEFAULT;
    bool better = grip != old_grip ? grip : area < g_hov_area;
    if (vis && area >= 1.0f && area < screen * 0.3f && better &&
        mu.x >= r.x && mu.x < r.x + r.width && mu.y >= r.y && mu.y < r.y + r.height) {
        g_hov_area = area;
        g_hov_px = px;
        if (g_clip_on) SDL_IntersectRect(&px, &g_clip_cur, &g_hov_px);
        g_hov_cursor = cur;
    }
    if (sent) return false;
    return area >= 30000.0f ? in : false;     /* the wheel: big areas (lists, the waveform) yes, small things not */
}
void GfxHoverLayer(Rectangle r)
{
    if (!g_gfx_hover_filter) return;
    Vector2 mu = { g_mouse.x / g_scale, g_mouse.y / g_scale };
    if (mu.x >= r.x && mu.x < r.x + r.width && mu.y >= r.y && mu.y < r.y + r.height) g_hov_area = 1e30f;
}
static void eco_hover_draw(void)
{
    if (!g_gfx_hover_filter || g_hov_area >= 1e29f || MOUSE_OFF) return;
    gfx_clip(NULL);
    SDL_Rect r = g_hov_px;
    int t = g_scale >= 1.5f ? 2 : 1;
    Color c = { 255, 255, 255, 110 };
    SDL_Rect e[4] = { { r.x, r.y, r.w, t }, { r.x, r.y + r.h - t, r.w, t },
                      { r.x, r.y + t, t, r.h - 2 * t }, { r.x + r.w - t, r.y + t, t, r.h - 2 * t } };
    gfx_rects(e, 4, c);
    if (g_hov_cursor != MOUSE_CURSOR_DEFAULT) g_cursor_req = g_hov_cursor;
}

void EndDrawing(void)
{
    eco_hover_draw();                                  /* 1.3.11 */
    if (g_rec) eco_present();                          /* 1.3.8 */
    else if (g_off) soft_present();
    else SDL_RenderPresent(g_ren);
    {   /* QLIM_FPS=1: frames per second and the drawing time per frame, every 5 s (for slow computers) */
        static int dbg = -1, nf;
        static double t0, work;
        if (dbg < 0) dbg = getenv("QLIM_FPS") != NULL;
        if (dbg) {
            double fr = (double)SDL_GetPerformanceFrequency(), now = (double)SDL_GetPerformanceCounter() / fr;
            work += now - (double)g_frame_start / fr;
            if (++nf, t0 == 0.0) t0 = now;
            if (now - t0 >= 5.0) {
                fprintf(stderr, "Q-LiM graphics: %.1f frames/s, %.1f ms of work per frame (with drawing)", nf / (now - t0), 1000.0 * work / nf);
                if (g_off) fprintf(stderr, ", %.0f %% of the window sent per frame", 100.0 * g_pushed / nf);
                if (g_rec && g_tnx > 0) fprintf(stderr, ", Eco: %.1f %% of the window drawn again per frame",
                                                100.0 * g_eco_drawn / ((double)nf * g_tnx * g_tny));
                g_eco_drawn = 0;
                fprintf(stderr, "\n");
                nf = 0; work = 0.0; t0 = now; g_pushed = 0.0;
            }
        }
    }

    /* Frame pacing (step 19). If vsync works, SDL_RenderPresent itself waits for the screen refresh ->
     * the frames are even and at the screen's rate (60/120/144 Hz) — we touch nothing.
     * The old "limit to 60" on a 144 Hz screen made frames alternate unevenly (2-3 refreshes) = choppy
     * motion. If vsync does NOT work (the frame finished very quickly), we wait EXACTLY until the next frame on
     * a fixed schedule (not "whatever is left" in whole milliseconds, which also gives uneven frames). */
    if (g_target_fps > 0) {
        double freq = (double)SDL_GetPerformanceFrequency();
        double now = (double)SDL_GetPerformanceCounter() / freq;
        double frame = now - (double)g_frame_start / freq;
        double period = 1.0 / (double)display_hz();
        static double deadline = 0.0;
        /* 1.3.4: fewer frames when nobody touches anything: the main loop says how many it needs (g_busy_fps:
         * e.g. 10 when nothing plays); the software renderer at most 30. The wait ends AT ONCE when an event
         * comes (mouse, key), so the program still answers immediately. */
        int want = display_hz();
        if (g_soft_render && !g_rec && want > 25) want = 25;   /* (redrawing only what changed is cheap enough) */
        if (g_eco && want > 30) want = 30;                       /* 1.3.9 Eco: at most 30 frames per second */
        bool touched = g_have_held || now - g_last_input < 0.75;
        if (g_max_fps > 0 && g_max_fps < want) want = g_max_fps;
        if (!touched && g_busy_fps > 0 && g_busy_fps < want) want = g_busy_fps;
        if (want < display_hz()) {
            double p2 = 1.0 / (double)want;
            double wait = p2 - frame;
            deadline = 0.0;
            if (wait > 0.001) {
                if (!g_eco) SDL_WaitEventTimeout(NULL, (int)(wait * 1000.0));
                else {
                    /* 1.3.9 Eco: a moving mouse does not wake the window - only a button, the wheel, a key, the window */
                    double end = now + wait;
                    for (;;) {
                        SDL_PumpEvents();
                        if (SDL_PeepEvents(NULL, 1, SDL_PEEKEVENT, SDL_FIRSTEVENT, SDL_MOUSEMOTION - 1) > 0 ||
                            SDL_PeepEvents(NULL, 1, SDL_PEEKEVENT, SDL_MOUSEMOTION + 1, SDL_LASTEVENT) > 0) break;
                        double t = (double)SDL_GetPerformanceCounter() / freq;
                        /* 1.3.11: the moving mouse updates the hover outline, at most ~15 times per second */
                        if (t - (double)g_frame_start / freq >= 1.0 / 15.0 &&
                            SDL_PeepEvents(NULL, 1, SDL_PEEKEVENT, SDL_MOUSEMOTION, SDL_MOUSEMOTION) > 0) break;
                        if (t >= end) break;
                        double st = end - t < 0.005 ? end - t : 0.005;
                        SDL_Delay((Uint32)(st * 1000.0) + 1);
                    }
                }
            }
        } else if (frame >= period * 0.75) {
            deadline = 0.0;                                  /* vsync does its job (or the frame is slow) */
        } else {
            if (deadline <= 0.0 || now > deadline + period) deadline = now;
            deadline += period;
            double wait = deadline - now;
            if (wait > 0.0005) {
                struct timespec ts = { (time_t)wait, (long)((wait - (double)(time_t)wait) * 1e9) };
                nanosleep(&ts, NULL);
            }
        }
    }
    poll_events();
}
