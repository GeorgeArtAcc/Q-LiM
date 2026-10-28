/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* VST2 (Linux .so / Windows .dll). Own definitions of the binary interface (without Steinberg's SDK). */
#define _GNU_SOURCE
#include "plughost.h"
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct AEffect AEffect;
typedef intptr_t (*HostCb)(AEffect *, int32_t op, int32_t idx, intptr_t val, void *ptr, float opt);
typedef intptr_t (*DispatchFn)(AEffect *, int32_t op, int32_t idx, intptr_t val, void *ptr, float opt);
typedef void (*ProcFn)(AEffect *, float **in, float **out, int32_t n);
typedef void (*SetParFn)(AEffect *, int32_t idx, float v);
typedef float (*GetParFn)(AEffect *, int32_t idx);
struct AEffect {
    int32_t   magic;
    DispatchFn dispatcher;
    ProcFn    process;
    SetParFn  setParameter;
    GetParFn  getParameter;
    int32_t   numPrograms, numParams, numInputs, numOutputs;
    int32_t   flags;
    intptr_t  resvd1, resvd2;
    int32_t   initialDelay, realQualities, offQualities;
    float     ioRatio;
    void     *object, *user;
    int32_t   uniqueID, version;
    ProcFn    processReplacing;
    void     *processDoubleReplacing;
    char      future[56];
};
#define VST_MAGIC 0x56737450      /* 'VstP' */
enum { effOpen = 0, effClose = 1, effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12,
       effEditGetRect = 13, effEditOpen = 14, effEditClose = 15, effEditIdle = 19, effGetChunk = 23, effSetChunk = 24,
       effGetEffectName = 45, effGetProductString = 48, effStartProcess = 71, effStopProcess = 72,
       effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8, effProcessEvents = 25, effCanDo = 51 };
enum { effFlagsHasEditor = 1 << 0, effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5, effFlagsIsSynth = 1 << 8 };
enum { amSizeWindow = 15 };
typedef struct { int16_t top, left, bottom, right; } ERect;
/* 1.2: MIDI events (VstEvents / VstMidiEvent, the binary layout of the SDK) */
typedef struct { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; char midiData[4];
                 char detune, noteOffVelocity, reserved1, reserved2; } VstMidiEvent;
enum { kVstMidiType = 1, kVstMidiEventIsRealtime = 1 };
typedef struct { int32_t numEvents; intptr_t reserved; void *events[QMIDI_MAX]; } VstEvents;
typedef struct { void *lib; AEffect *e; PlugWin *win;
                 VstEvents evs; VstMidiEvent ev[QMIDI_MAX]; int nev; } V2;
enum { amAutomate = 0, amVersion = 1, amCurrentId = 2, amIdle = 3, amGetTime = 7, amGetSampleRate = 16,
       amGetBlockSize = 17, amGetCurrentProcessLevel = 23, amGetVendorString = 32, amGetProductString = 33,
       amGetVendorVersion = 34, amCanDo = 37, amGetLanguage = 38, amUpdateDisplay = 42 };

static __thread double t_rate = 48000;
static __thread int    t_block = 1024;

static intptr_t host_cb(AEffect *e, int32_t op, int32_t idx, intptr_t val, void *ptr, float opt)
{
    (void)opt;
    switch (op) {
    case amAutomate: qplug_note_edit(); return 0;
    case amSizeWindow: {                                   /* the plugin wants a different window size */
        V2 *v = e ? (V2 *)e->user : NULL;
        if (v && v->win && idx > 0 && val > 0) plugwin_resize(v->win, idx, (int)val);
        return 1;
    }
    case amVersion: return 2400;
    case amGetSampleRate: return (intptr_t)t_rate;
    case amGetBlockSize: return t_block;
    case amGetCurrentProcessLevel: return 0;
    case amGetVendorString: if (ptr) strcpy(ptr, "Georgi Stanimirov Ivanov"); return 1;
    case amGetProductString: if (ptr) strcpy(ptr, "Q-LiM"); return 1;
    case amGetVendorVersion: return 1;
    case amCanDo:
        if (ptr && !strcmp(ptr, "sizeWindow")) return 1;
        return 0;
    case amGetLanguage: return 1;
    default: return 0;
    }
}


static AEffect *v2_load(const char *path, void **lib_out, char *err, int esz)
{
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { snprintf(err, (size_t)esz, "cannot load: %s", dlerror()); return NULL; }
    typedef AEffect *(*MainFn)(HostCb);
    MainFn m = (MainFn)dlsym(lib, "VSTPluginMain");
    if (!m) m = (MainFn)dlsym(lib, "main");
    if (!m) { snprintf(err, (size_t)esz, "not a VST2 plugin (no VSTPluginMain)"); dlclose(lib); return NULL; }
    AEffect *e = m(host_cb);
    if (!e || e->magic != VST_MAGIC) { snprintf(err, (size_t)esz, "plugin refused to start"); dlclose(lib); return NULL; }
    *lib_out = lib;
    return e;
}

static void v2_midi(void *h, const QMidiEv *ev, int n)
{
    V2 *v = h;
    for (int i = 0; i < n && v->nev < QMIDI_MAX; ++i) {
        VstMidiEvent *m = &v->ev[v->nev++];
        memset(m, 0, sizeof *m);
        m->type = kVstMidiType; m->byteSize = (int32_t)sizeof *m; m->deltaFrames = (int32_t)ev[i].frame;
        m->flags = kVstMidiEventIsRealtime;
        memcpy(m->midiData, ev[i].d, 3);
    }
}

static void v2_process(void *h, float **in, float **out, unsigned n)
{
    V2 *v = h;
    if (v->nev > 0) {                                   /* 1.2: MIDI goes in just before the audio */
        v->evs.numEvents = v->nev;
        v->evs.reserved = 0;
        for (int i = 0; i < v->nev; ++i) {
            if (v->ev[i].deltaFrames >= (int32_t)n) v->ev[i].deltaFrames = (int32_t)n - 1;
            v->evs.events[i] = &v->ev[i];
        }
        v->e->dispatcher(v->e, effProcessEvents, 0, 0, &v->evs, 0);
        v->nev = 0;
    }
    v->e->processReplacing(v->e, in, out, (int32_t)n);
}

/* 1.2: parameters */
static int v2_param_count(void *h) { V2 *v = h; return v->e->numParams > 0 ? v->e->numParams : 0; }
static bool v2_param_name(void *h, int i, char *name, int nsz)
{
    V2 *v = h;
    char b[256] = "";                                   /* the SDK says 8 chars; many plugins write more */
    v->e->dispatcher(v->e, effGetParamName, i, 0, b, 0);
    b[255] = 0;
    snprintf(name, (size_t)nsz, "%s", b);
    return true;
}
static float v2_param_get(void *h, int i) { V2 *v = h; return v->e->getParameter(v->e, i); }
static void v2_param_set(void *h, int i, float x) { V2 *v = h; v->e->setParameter(v->e, i, x); }
static void v2_param_text(void *h, int i, char *out, int sz)
{
    V2 *v = h;
    char d[256] = "", l[256] = "";
    v->e->dispatcher(v->e, effGetParamDisplay, i, 0, d, 0);
    v->e->dispatcher(v->e, effGetParamLabel, i, 0, l, 0);
    d[255] = l[255] = 0;
    char *s = d; while (*s == ' ') ++s;
    if (*s) snprintf(out, (size_t)sz, l[0] ? "%s %s" : "%s", s, l);
}

static bool v2_has_editor(void *h) { return (((V2 *)h)->e->flags & effFlagsHasEditor) != 0; }

static int v2_ed_open(void *h, PlugWin *w, int *W, int *H, char *err, int esz)
{
    V2 *v = h;
    ERect *r = NULL;
    v->win = w;
    v->e->dispatcher(v->e, effEditGetRect, 0, 0, &r, 0);
    if (r && r->right > r->left && r->bottom > r->top) plugwin_resize(w, r->right - r->left, r->bottom - r->top);
#ifdef _WIN32
    void *parent = plugwin_native(w);                       /* Windows: the HWND */
#else
    void *parent = (void *)(uintptr_t)plugwin_xid(w);       /* X11: the window id */
#endif
    if (!v->e->dispatcher(v->e, effEditOpen, 0, (intptr_t)plugwin_display(), parent, 0)) {
        /* some return 0 even on success - check the size */
    }
    r = NULL;
    v->e->dispatcher(v->e, effEditGetRect, 0, 0, &r, 0);
    if (r && r->right > r->left && r->bottom > r->top) { *W = r->right - r->left; *H = r->bottom - r->top; }
    (void)err; (void)esz;
    return 0;
}

static void v2_ed_close(void *h)
{
    V2 *v = h;
    v->e->dispatcher(v->e, effEditClose, 0, 0, NULL, 0);
    v->win = NULL;
}

static void v2_ed_idle(void *h) { V2 *v = h; v->e->dispatcher(v->e, effEditIdle, 0, 0, NULL, 0); }

/* state: "QV2C" + a chunk from the plugin, or "QV2P" + the values of all parameters */
static int v2_get_state(void *h, void **data, size_t *len)
{
    V2 *v = h;
    AEffect *e = v->e;
    if (e->flags & effFlagsProgramChunks) {
        void *chunk = NULL;
        intptr_t n = e->dispatcher(e, effGetChunk, 0, 0, &chunk, 0);
        if (n > 0 && chunk) {
            unsigned char *b = malloc((size_t)n + 4);
            if (!b) return -1;
            memcpy(b, "QV2C", 4); memcpy(b + 4, chunk, (size_t)n);
            *data = b; *len = (size_t)n + 4;
            return 0;
        }
    }
    int np = e->numParams > 0 ? e->numParams : 0;
    unsigned char *b = malloc(4 + (size_t)np * sizeof(float));
    if (!b) return -1;
    memcpy(b, "QV2P", 4);
    for (int i = 0; i < np; ++i) { float f = e->getParameter(e, i); memcpy(b + 4 + (size_t)i * 4, &f, 4); }
    *data = b; *len = 4 + (size_t)np * 4;
    return 0;
}

static int v2_set_state(void *h, const void *data, size_t len)
{
    V2 *v = h;
    AEffect *e = v->e;
    const unsigned char *b = data;
    if (len < 4) return -1;
    if (!memcmp(b, "QV2C", 4)) {
        e->dispatcher(e, effSetChunk, 0, (intptr_t)(len - 4), (void *)(b + 4), 0);
        return 0;
    }
    if (!memcmp(b, "QV2P", 4)) {
        size_t np = (len - 4) / 4;
        for (size_t i = 0; i < np && (int)i < e->numParams; ++i) { float f; memcpy(&f, b + 4 + i * 4, 4); e->setParameter(e, (int32_t)i, f); }
        return 0;
    }
    return -1;
}

static void v2_destroy(void *h)
{
    V2 *v = h;
    v->e->dispatcher(v->e, effStopProcess, 0, 0, NULL, 0);
    v->e->dispatcher(v->e, effMainsChanged, 0, 0, NULL, 0);
    v->e->dispatcher(v->e, effClose, 0, 0, NULL, 0);
    dlclose(v->lib);
    free(v);
}

int vst2_open(const char *id, double rate, int maxb, QInst *out, char *err, int esz)
{
    t_rate = rate; t_block = maxb;
    void *lib = NULL;
    AEffect *e = v2_load(id, &lib, err, esz);
    if (!e) return -1;
    if (!(e->flags & effFlagsCanReplacing) || !e->processReplacing) {
        snprintf(err, (size_t)esz, "old VST (no processReplacing) - not supported");
        dlclose(lib); return -1;
    }
    e->dispatcher(e, effOpen, 0, 0, NULL, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, NULL, (float)rate);
    e->dispatcher(e, effSetBlockSize, 0, maxb, NULL, 0);
    e->dispatcher(e, effMainsChanged, 0, 1, NULL, 0);
    e->dispatcher(e, effStartProcess, 0, 0, NULL, 0);
    V2 *v = calloc(1, sizeof *v);
    v->lib = lib; v->e = e;
    e->user = v;
    out->h = v; out->nin = e->numInputs; out->nout = e->numOutputs;
    out->process = v2_process; out->destroy = v2_destroy;
    out->has_editor = v2_has_editor; out->ed_open = v2_ed_open; out->ed_close = v2_ed_close; out->ed_idle = v2_ed_idle;
    out->get_state = v2_get_state; out->set_state = v2_set_state;
    bool wants = (e->flags & effFlagsIsSynth) || e->dispatcher(e, effCanDo, 0, 0, (void *)"receiveVstMidiEvent", 0) > 0;
    if (wants) out->midi = v2_midi;
    out->param_count = v2_param_count; out->param_name = v2_param_name; out->param_get = v2_param_get;
    out->param_set = v2_param_set; out->param_text = v2_param_text;
    return 0;
}

int vst2_scan(const char *path)
{
    char err[256];
    void *lib = NULL;
    AEffect *e = v2_load(path, &lib, err, sizeof err);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
#ifdef _WIN32
    /* Windows plugin folders often hold helper DLLs too: a DLL that is not a plugin is simply skipped */
    if (!e && strstr(err, "no VSTPluginMain")) { printf("SKIP\t%s\n", path); fflush(stdout); return 0; }
#endif
    if (!e) { plug_print_line("ERR", "VST", base, path, err); return 0; }
    e->dispatcher(e, effOpen, 0, 0, NULL, 0);
    char name[256] = "";
    e->dispatcher(e, effGetEffectName, 0, 0, name, 0);
    if (!name[0]) e->dispatcher(e, effGetProductString, 0, 0, name, 0);
    if (!name[0]) { snprintf(name, sizeof name, "%s", base); char *d = strrchr(name, '.'); if (d) *d = 0; }
    char extra[64];
    snprintf(extra, sizeof extra, "%s\t%d\t%d", (e->flags & effFlagsIsSynth) ? "inst" : "fx", e->numInputs, e->numOutputs);
    e->dispatcher(e, effClose, 0, 0, NULL, 0);
    dlclose(lib);
    plug_print_line("PLUG", "VST", name, path, extra);
    return 0;
}
