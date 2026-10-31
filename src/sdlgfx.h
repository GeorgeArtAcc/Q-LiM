/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef SDLGFX_H
#define SDLGFX_H

/*
 * Q-LiM — graphics layer on top of SDL2 (replaces raylib).
 *
 * The names and behaviour of the functions used by ui.c/main.c are kept,
 * so the interface is not rewritten, only the "engine" underneath is replaced.
 *
 * What is different (and better):
 *   - Text is rendered with FreeType at the EXACT pixel size (not scaled down from a 64px
 *     image) -> clear, sharp letters.
 *   - Rectangles and lines are aligned to whole physical pixels -> no "blurry" edges.
 *   - Circles, triangles and diagonal lines are anti-aliased.
 *   - HiDPI: on screens with 150%/200% scaling everything is sharp, not magnified.
 *   - Drag & drop of files and folders from the file manager onto the window.
 *   - Works through OpenGL, OpenGL ES or fully in software (old graphics cards).
 *
 * Environment variables (optional):
 *   QLIM_RENDERER=software   forces software rendering (old graphics card)
 *   QLIM_SCALE=1.5           manual interface scale (taken from the system by default)
 *   QLIM_FONT=/path/to.ttf   a different font
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct { unsigned char r, g, b, a; } Color;
typedef struct { float x, y; } Vector2;
typedef struct { float x, y, width, height; } Rectangle;
typedef struct { unsigned int id; int width, height, mipmaps, format; } Texture2D;
typedef Texture2D Texture;
typedef struct { int baseSize; int glyphCount; Texture2D texture; void *handle; } Font;
typedef struct { unsigned int capacity; unsigned int count; char **paths; } FilePathList;

#define RAYWHITE ((Color){ 245, 245, 245, 255 })
#define WHITE    ((Color){ 255, 255, 255, 255 })
#define BLACK    ((Color){   0,   0,   0, 255 })
#define BLANK    ((Color){   0,   0,   0,   0 })
#define RED      ((Color){ 230,  41,  55, 255 })
#define GREEN    ((Color){   0, 228,  48, 255 })
#define BLUE     ((Color){   0, 121, 241, 255 })
#define YELLOW   ((Color){ 253, 249,   0, 255 })
#define ORANGE   ((Color){ 255, 161,   0, 255 })
#define PURPLE   ((Color){ 200, 122, 255, 255 })
#define GRAY     ((Color){ 130, 130, 130, 255 })
#define LIGHTGRAY ((Color){ 200, 200, 200, 255 })
#define DARKGRAY ((Color){  80,  80,  80, 255 })

typedef enum { FLAG_WINDOW_RESIZABLE = 0x4, FLAG_VSYNC_HINT = 0x40 } ConfigFlags;
typedef enum { MOUSE_BUTTON_LEFT = 0, MOUSE_BUTTON_RIGHT = 1, MOUSE_BUTTON_MIDDLE = 2 } MouseButton;
typedef enum {
    MOUSE_CURSOR_DEFAULT = 0, MOUSE_CURSOR_ARROW = 1, MOUSE_CURSOR_IBEAM = 2, MOUSE_CURSOR_CROSSHAIR = 3,
    MOUSE_CURSOR_POINTING_HAND = 4, MOUSE_CURSOR_RESIZE_EW = 5, MOUSE_CURSOR_RESIZE_NS = 6
} MouseCursor;
typedef enum { TEXTURE_FILTER_POINT = 0, TEXTURE_FILTER_BILINEAR = 1 } TextureFilter;
typedef enum { LOG_ALL = 0, LOG_TRACE, LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERROR, LOG_FATAL, LOG_NONE } TraceLogLevel;

/* The numbering matches raylib (so nothing that stores key codes breaks) */
typedef enum {
    KEY_NULL = 0,
    KEY_COMMA = 44, KEY_MINUS = 45, KEY_PERIOD = 46, KEY_SLASH = 47,
    KEY_ZERO = 48, KEY_ONE, KEY_TWO, KEY_THREE, KEY_FOUR, KEY_FIVE, KEY_SIX, KEY_SEVEN, KEY_EIGHT, KEY_NINE,
    KEY_SEMICOLON = 59, KEY_EQUAL = 61,
    KEY_A = 65, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,
    KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
    KEY_SPACE = 32,
    KEY_ESCAPE = 256, KEY_ENTER, KEY_TAB, KEY_BACKSPACE, KEY_INSERT, KEY_DELETE,
    KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP, KEY_PAGE_UP, KEY_PAGE_DOWN, KEY_HOME, KEY_END,
    KEY_F1 = 290, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_KP_0 = 320, KEY_KP_1, KEY_KP_2, KEY_KP_3, KEY_KP_4, KEY_KP_5, KEY_KP_6, KEY_KP_7, KEY_KP_8, KEY_KP_9,
    KEY_KP_ENTER = 335,
    KEY_LEFT_SHIFT = 340, KEY_LEFT_CONTROL, KEY_LEFT_ALT, KEY_LEFT_SUPER,
    KEY_RIGHT_SHIFT, KEY_RIGHT_CONTROL, KEY_RIGHT_ALT, KEY_RIGHT_SUPER
} KeyboardKey;

/* ---- window ---- */
void   SetConfigFlags(unsigned int flags);
void   InitWindow(int width, int height, const char *title);
void   CloseWindow(void);
bool   WindowShouldClose(void);
void   ClearWindowShouldClose(void);   /* cancel closing (e.g. "Cancel" in the save question) */
void   RequestWindowClose(void);
void   SetWindowTitle(const char *title);
void   SetExitKey(int key);
void   SetTargetFPS(int fps);
void   MaximizeWindow(void);
int    GetScreenWidth(void);      /* logical size (in "UI units") */
int    GetScreenHeight(void);
double GetTime(void);
void   TraceLog(int level, const char *fmt, ...);

/* ---- frame ---- */
void BeginDrawing(void);
bool GfxBackdropNeeded(void);            /* true = the background snapshot is missing -> draw the interface passively first */
void GfxMouseShield(bool on);
bool GfxMouseShieldGet(void);            /* true = the interface cannot see the mouse (mouse only) */
void GfxInputSuppress(bool off);         /* true = the interface sees no mouse/keys (for passive drawing) */
void GfxDialogBackdrop(int dim_alpha);   /* blurred interface behind + slight darkening (called at the start of a dialog) */
void EndDrawing(void);
void ClearBackground(Color c);
void BeginScissorMode(int x, int y, int w, int h);
void EndScissorMode(void);

/* ---- drawing ---- */
void DrawRectanglesBatch(const Rectangle *r, int n, Color c);
typedef struct GfxImage GfxImage;    /* 1.3.14: a picture the program draws itself, pixel by pixel (the waveform) */
GfxImage *GfxImageNew(void);
void      GfxImageUpload(GfxImage *im, const uint32_t *argb, int w, int h);   /* ARGB, not premultiplied */
void      GfxImageDraw(GfxImage *im, int px, int py);                         /* in real (physical) pixels */
float     GfxScaleNow(void);                                                  /* UI unit -> physical pixels */   /* 1.3.14: many rectangles of one colour, one command */
void DrawRectangle(int x, int y, int w, int h, Color c);
void DrawRectangleGradientV(float x, float y, float w, float h, Color top, Color bottom);   /* step 36c */
void DrawRectangleRounded(Rectangle r, float radius, Color c);                 /* step 36f: rounded corners */
void DrawRectangleRoundedLines(Rectangle r, float radius, float thick, Color c);
void GfxRoundAll(float radius);   /* step 36g: >0 = plain rectangles become rounded (dialogs/menus); 0 = off */
void DrawRectangleRec(Rectangle r, Color c);
void DrawRectangleLines(int x, int y, int w, int h, Color c);
void DrawRectangleLinesEx(Rectangle r, float thick, Color c);
void DrawLine(int x1, int y1, int x2, int y2, Color c);
void DrawLineEx(Vector2 a, Vector2 b, float thick, Color c);
void DrawTriangle(Vector2 a, Vector2 b, Vector2 c, Color col);
void DrawCircleV(Vector2 center, float radius, Color c);
void DrawCircleLines(int cx, int cy, float radius, Color c);

/* ---- text ---- */
Font    GetFontDefault(void);
Font    LoadFontEx(const char *path, int size, int *codepoints, int count);
void    SetTextureFilter(Texture2D t, int filter);
Vector2 MeasureTextEx(Font f, const char *text, float size, float spacing);
void    DrawTextEx(Font f, const char *text, Vector2 pos, float size, float spacing, Color c);
int     MeasureText(const char *text, int size);
void    DrawText(const char *text, int x, int y, int size, Color c);

/* ---- input ---- */
Vector2 GetMousePosition(void);
Vector2 GetMouseDelta(void);               /* mouse movement for this frame */
void    GfxMouseRelative(bool on);         /* "infinite" dragging: hidden cursor, no edge (as in Bitwig) */
bool    IsMouseButtonPressed(int b);
bool    IsMouseButtonDown(int b);
int     GetMouseClicks(int b);        /* 1.3.15: 2 = this press is a double click (the system's timing) */
/* Call before/after an external window (zenity): releases the mouse and resets "stuck" buttons */
void   *GfxSdlWindow(void);
bool    GfxSoftwareRendering(void);   /* 1.3.4: the CPU draws the picture (no graphics acceleration) */
void    GfxSetBusyFps(int fps);       /* 1.3.4: frames per second needed while nothing is touched (0 = full) */
void    GfxSetMaxFps(int fps);        /* 1.3.7: the cap at all times (0 = none) */
void    GfxSetRendererPref(int pref); /* 1.3.7: before InitWindow: 0 auto, 1 graphics card, 2 light (QLIM_RENDERER wins) */
const char *GfxRendererDesc(void);    /* 1.3.7: how the window is drawn, in words */
void    GfxSetEco(bool on);           /* 1.3.8: Eco interface - redraw only what changed, simple shapes, no blur */
bool    GfxEco(void);
void    GfxSetSmart(bool on);
void    GfxSetZoom(float z);          /* 1.3.10: the size of the whole interface (0.5 .. 2.5), the menu bar excluded */
float   GfxGetZoom(void);
void    GfxUnzoomBegin(void);         /* 1.3.10: draw at the normal size (the menu bar and its menus) ... */
void    GfxUnzoomEnd(void);           /* ... and back */         /* 1.3.9: redraw only what changed (on by default; QLIM_SMART=0 turns it off) */
void    GfxGLRelease(void);     /* before calling a plugin on the main thread */
void    GfxGLRestore(void);     /* after it */
void    GfxExternalDialogBegin(void);
void    GfxExternalDialogEnd(void);
bool    GfxExternalDialogActive(void);   /* step 36l */
void    GfxRaiseWindow(void);            /* brings the window to the front (a project was opened from the file manager) */
bool    IsMouseButtonReleased(int b);
float   GetMouseWheelMove(void);
void    SetMouseCursor(int cursor);
bool    IsKeyPressed(int key);
bool    IsKeyPressedRepeat(int key);
bool    IsKeyDown(int key);
int     GetCharPressed(void);

/* ---- files dropped onto the window (drag & drop) ---- */
bool         IsFileDropped(void);
FilePathList LoadDroppedFiles(void);
void         UnloadDroppedFiles(FilePathList files);
Vector2      GetDropPosition(void);   /* where the cursor was at the drop (UI coordinates) */

/* ---- helpers ---- */
extern bool g_gfx_hover_filter;       /* 1.3.11: Eco - the hover checks go through GfxHoverCheck */
bool GfxHoverCheck(Vector2 p, Rectangle r);
void GfxHoverLayer(Rectangle r);     /* 1.3.12: Eco - something is drawn on top here: what was under the mouse below it does not count */
Vector2 GfxMouseTrue(void);           /* 1.3.12: the real mouse even in Eco (for "is the mouse over a popup" checks) */
static inline bool GfxPointIn(Vector2 p, Rectangle r)   /* a plain test (never a hover in Eco) */
{
    return p.x >= r.x && p.x < r.x + r.width && p.y >= r.y && p.y < r.y + r.height;
}
static inline bool CheckCollisionPointRec(Vector2 p, Rectangle r)
{
    if (g_gfx_hover_filter) return GfxHoverCheck(p, r);
    return p.x >= r.x && p.x < r.x + r.width && p.y >= r.y && p.y < r.y + r.height;
}

/* For tests: saves the current frame as a BMP. Called between drawing and EndDrawing(). */
int GfxScreenshot(const char *path);
/* The UI scale (1.0 = an ordinary screen) */
float GfxScale(void);

#endif
