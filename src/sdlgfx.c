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
static float         g_scale = 1.0f;       /* physical pixels per 1 UI unit */
static float         g_user_scale = 1.0f;  /* QLIM_SCALE */
static int           g_drawable_w, g_drawable_h;

/* input */
#define NKEYS 512
static bool  g_key_down[NKEYS];
static bool  g_key_pressed[NKEYS];
static bool  g_key_repeat[NKEYS];
static bool  g_btn_down[3], g_btn_pressed[3], g_btn_released[3];
static float g_wheel;
static Vector2 g_mouse;
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

static void update_scale(void)
{
    int ww = 1, wh = 1, ow = 1, oh = 1;
    SDL_GetWindowSize(g_win, &ww, &wh);
    SDL_GetRendererOutputSize(g_ren, &ow, &oh);
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
    g_scale = dpi * g_user_scale;
    if (g_scale < 0.5f) g_scale = 0.5f;
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
    bool force_sw = rn && strcmp(rn, "software") == 0;
    if (!force_sw)
        g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren)
        g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
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

typedef struct TextEntry {
    char *text;
    int px;
    SDL_Texture *tex;
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

    if (g_tcache_px > 24u * 1000u * 1000u) {      /* ~96 MB: clear everything */
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
    SDL_SetTextureColorMod(e->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(e->tex, c.a);
    SDL_Rect dst = { D(pos.x), D(pos.y), e->w, e->h };
    SDL_RenderCopy(g_ren, e->tex, NULL, &dst);
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

static void fill_dev_rect(int x0, int y0, int x1, int y1, Color c)
{
    if (x1 <= x0 || y1 <= y0) return;
    set_color(c);
    SDL_Rect r = { x0, y0, x1 - x0, y1 - y0 };
    SDL_RenderFillRect(g_ren, &r);
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

void ClearBackground(Color c)
{
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, 255);
    SDL_RenderClear(g_ren);
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
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
    SDL_Vertex v[4];
    Color cs[4] = { top, top, bottom, bottom };
    float px[4] = { (float)x0, (float)x1, (float)x1, (float)x0 }, py[4] = { (float)y0, (float)y0, (float)y1, (float)y1 };
    for (int i = 0; i < 4; ++i) {
        v[i].position.x = px[i]; v[i].position.y = py[i];
        v[i].color.r = cs[i].r; v[i].color.g = cs[i].g; v[i].color.b = cs[i].b; v[i].color.a = cs[i].a;
        v[i].tex_coord.x = v[i].tex_coord.y = 0.0f;
    }
    static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    SDL_RenderGeometry(g_ren, NULL, v, 4, idx, 6);
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
        SDL_RenderGeometry(g_ren, NULL, m->v, m->n, m->idx, m->ni);
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
    SDL_RenderSetClipRect(g_ren, &r);
}
void EndScissorMode(void) { SDL_RenderSetClipRect(g_ren, NULL); }

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
        switch (e.type) {
        case SDL_QUIT:
            g_should_close = true;
            break;
        case SDL_MOUSEMOTION:
            {
                int ww = 1, wh = 1;
                SDL_GetWindowSize(g_win, &ww, &wh);
                float k = (float)g_drawable_w / (float)(ww > 0 ? ww : 1) / g_scale;
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
            if (b >= 0) { g_btn_down[b] = true; g_btn_pressed[b] = true; }
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
        float k = (float)g_drawable_w / (float)(ww > 0 ? ww : 1) / g_scale;
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

Vector2 GetMousePosition(void) { return MOUSE_OFF ? (Vector2){ -10000.0f, -10000.0f } : g_mouse; }
Vector2 GetMouseDelta(void) { return MOUSE_OFF ? (Vector2){ 0.0f, 0.0f } : g_mouse_delta; }

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
        float k = (float)g_drawable_w / (float)(ww > 0 ? ww : 1) / g_scale;
        if (k > 0.0f) SDL_WarpMouseInWindow(g_win, (int)(g_mouse_rel_home.x / k), (int)(g_mouse_rel_home.y / k));
        g_mouse = g_mouse_rel_home;
    }
}
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

Vector2 GetDropPosition(void) { return g_drop_pos; }

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
    if (SDL_RenderReadPixels(g_ren, NULL, SDL_PIXELFORMAT_ARGB8888, full, W * 4) != 0) { free(full); return; }

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
    g_bd_time = GetTime();
    free(small);
    g_bd_w = w; g_bd_h = h;
    g_bd_src_w = W; g_bd_src_h = H;
}

static bool g_bd_want;      /* GfxBackdropNeeded() said "draw the world" -> capture in this frame */

bool GfxBackdropNeeded(void)
{
    bool need = g_bd_calls == 0 &&
                (!g_bd_tex || g_bd_src_w != g_drawable_w || g_bd_src_h != g_drawable_h ||
                 GetTime() - g_bd_time >= BD_REFRESH_S);
    if (need) g_bd_want = true;
    return need;
}

void GfxDialogBackdrop(int dim_alpha)
{
    int first = (g_bd_calls++ == 0);
    if (first && g_bd_want) { g_bd_want = false; bd_capture(); }
    SDL_RenderSetClipRect(g_ren, NULL);
    if (first && g_bd_tex) {
        SDL_Rect dst = { 0, 0, g_drawable_w, g_drawable_h };
        SDL_RenderCopy(g_ren, g_bd_tex, NULL, &dst);
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), (Color){ 0, 0, 0, (unsigned char)dim_alpha });
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
    SDL_RenderSetClipRect(g_ren, NULL);
}

int GfxScreenshot(const char *path)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, g_drawable_w, g_drawable_h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return -1;
    int rc = SDL_RenderReadPixels(g_ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
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

void EndDrawing(void)
{
    SDL_RenderPresent(g_ren);

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
        if (frame >= period * 0.75) {
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
