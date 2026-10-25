/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* VST3 plugins. Only Steinberg's "pluginterfaces" interfaces are used (MIT license, in third_party/vst3).
 * Step 35: controller (IEditController) + window (IPlugView in X11) + IRunLoop + parameters from the window to
 * the audio (lock-free queue) + state (getState/setState) for saving into the project. */
#define INIT_CLASS_IID
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/base/funknown.cpp"

extern "C" {
#include "plughost.h"
}
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <exception>
#include <dlfcn.h>
#ifdef _WIN32
#define opendir  qw_opendir
#define readdir  qw_readdir
#define closedir qw_closedir
#define DIR      QW_DIR
#define dirent   qw_dirent
#define stat(p, s) qw_stat((p), (s))
#else
#include <dirent.h>
#endif
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

static bool iid_is(const TUID a, const TUID b) { return FUnknownPrivate::iidEqual(a, b); }

/* ---- reference-counted object (for the things the plugin frees itself: messages, attributes) ---- */
#define QREFCOUNT                                                                       \
    std::atomic<uint32> refs{1};                                                        \
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return ++refs; }                        \
    uint32 PLUGIN_API release() SMTG_OVERRIDE { uint32 r = --refs; if (!r) delete this; return r; }

class QAttrList : public IAttributeList {
public:
    QREFCOUNT
    virtual ~QAttrList() {}
    std::map<std::string, int64> ints;
    std::map<std::string, double> floats;
    std::map<std::string, std::u16string> strs;
    std::map<std::string, std::vector<char>> bins;
    tresult PLUGIN_API setInt(AttrID id, int64 v) SMTG_OVERRIDE { ints[id] = v; return kResultOk; }
    tresult PLUGIN_API getInt(AttrID id, int64 &v) SMTG_OVERRIDE { auto i = ints.find(id); if (i == ints.end()) return kResultFalse; v = i->second; return kResultOk; }
    tresult PLUGIN_API setFloat(AttrID id, double v) SMTG_OVERRIDE { floats[id] = v; return kResultOk; }
    tresult PLUGIN_API getFloat(AttrID id, double &v) SMTG_OVERRIDE { auto i = floats.find(id); if (i == floats.end()) return kResultFalse; v = i->second; return kResultOk; }
    tresult PLUGIN_API setString(AttrID id, const TChar *s) SMTG_OVERRIDE { strs[id] = std::u16string((const char16_t *)s); return kResultOk; }
    tresult PLUGIN_API getString(AttrID id, TChar *s, uint32 sizeBytes) SMTG_OVERRIDE
    {
        auto i = strs.find(id);
        if (i == strs.end() || sizeBytes < 2) return kResultFalse;
        size_t n = i->second.size(), cap = sizeBytes / 2 - 1;
        if (n > cap) n = cap;
        memcpy(s, i->second.data(), n * 2);
        s[n] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API setBinary(AttrID id, const void *d, uint32 n) SMTG_OVERRIDE { bins[id] = std::vector<char>((const char *)d, (const char *)d + n); return kResultOk; }
    tresult PLUGIN_API getBinary(AttrID id, const void *&d, uint32 &n) SMTG_OVERRIDE
    {
        auto i = bins.find(id);
        if (i == bins.end()) return kResultFalse;
        d = i->second.data(); n = (uint32)i->second.size();
        return kResultOk;
    }
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (iid_is(iid, FUnknown::iid) || iid_is(iid, IAttributeList::iid)) { addRef(); *obj = this; return kResultOk; }
        *obj = nullptr; return kNoInterface;
    }
};

class QMessage : public IMessage {
public:
    QREFCOUNT
    std::string id;
    QAttrList *attrs = new QAttrList();
    virtual ~QMessage() { attrs->release(); }
    FIDString PLUGIN_API getMessageID() SMTG_OVERRIDE { return id.c_str(); }
    void PLUGIN_API setMessageID(FIDString i) SMTG_OVERRIDE { id = i ? i : ""; }
    IAttributeList *PLUGIN_API getAttributes() SMTG_OVERRIDE { return attrs; }
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (iid_is(iid, FUnknown::iid) || iid_is(iid, IMessage::iid)) { addRef(); *obj = this; return kResultOk; }
        *obj = nullptr; return kNoInterface;
    }
};

/* ---- the host ---- */
class QHostApp : public IHostApplication {
public:
    tresult PLUGIN_API getName(String128 name) SMTG_OVERRIDE
    {
        const char *s = "Q-LiM";
        int i = 0;
        for (; s[i]; ++i) name[i] = (char16)s[i];
        name[i] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID cid, TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (iid_is(cid, IMessage::iid) && iid_is(iid, IMessage::iid)) { *obj = (IMessage *)new QMessage(); return kResultOk; }
        if (iid_is(cid, IAttributeList::iid) && iid_is(iid, IAttributeList::iid)) { *obj = (IAttributeList *)new QAttrList(); return kResultOk; }
        *obj = nullptr;
        return kNotImplemented;
    }
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (iid_is(iid, FUnknown::iid) || iid_is(iid, IHostApplication::iid)) { *obj = this; return kResultOk; }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return 1; }
    uint32 PLUGIN_API release() SMTG_OVERRIDE { return 1; }
};
static QHostApp g_hostapp;

/* ---- memory stream ---- */
class MemStream : public IBStream {
public:
    std::vector<char> d;
    int64 pos = 0;
    tresult PLUGIN_API read(void *buf, int32 n, int32 *got) SMTG_OVERRIDE
    {
        int64 left = (int64)d.size() - pos;
        if (n > left) n = (int32)(left > 0 ? left : 0);
        if (n > 0) memcpy(buf, d.data() + pos, (size_t)n);
        pos += n;
        if (got) *got = n;
        return kResultOk;
    }
    tresult PLUGIN_API write(void *buf, int32 n, int32 *put) SMTG_OVERRIDE
    {
        if (n < 0) return kInvalidArgument;
        if ((size_t)(pos + n) > d.size()) d.resize((size_t)(pos + n));
        memcpy(d.data() + pos, buf, (size_t)n);
        pos += n;
        if (put) *put = n;
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 p, int32 mode, int64 *res) SMTG_OVERRIDE
    {
        int64 np = mode == kIBSeekSet ? p : mode == kIBSeekCur ? pos + p : (int64)d.size() + p;
        if (np < 0) return kResultFalse;
        pos = np;
        if (res) *res = pos;
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64 *p) SMTG_OVERRIDE { if (p) *p = pos; return kResultOk; }
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (iid_is(iid, FUnknown::iid) || iid_is(iid, IBStream::iid)) { *obj = this; return kResultOk; }
        *obj = nullptr; return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return 1; }
    uint32 PLUGIN_API release() SMTG_OVERRIDE { return 1; }
};

/* ---- lock-free queue (one writer, one reader) ---- */
struct PEv { ParamID id; double v; };
template <int N> struct Spsc {
    PEv buf[N];
    std::atomic<unsigned> w{0}, r{0};
    bool push(PEv e) { unsigned ww = w.load(std::memory_order_relaxed); if (ww - r.load(std::memory_order_acquire) >= N) return false; buf[ww % N] = e; w.store(ww + 1, std::memory_order_release); return true; }
    bool pop(PEv &e) { unsigned rr = r.load(std::memory_order_relaxed); if (rr == w.load(std::memory_order_acquire)) return false; e = buf[rr % N]; r.store(rr + 1, std::memory_order_release); return true; }
};

/* ---- parameter changes for one block ---- */
#define MAXQ 128
class PQueue : public IParamValueQueue {
public:
    ParamID id = 0;
    double v = 0;
    int npts = 0;
    ParamID PLUGIN_API getParameterId() SMTG_OVERRIDE { return id; }
    int32 PLUGIN_API getPointCount() SMTG_OVERRIDE { return npts; }
    tresult PLUGIN_API getPoint(int32 i, int32 &off, ParamValue &val) SMTG_OVERRIDE
    {
        if (i != 0 || npts == 0) return kResultFalse;
        off = 0; val = v;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 off, ParamValue val, int32 &idx) SMTG_OVERRIDE { (void)off; v = val; npts = 1; idx = 0; return kResultOk; }
    tresult PLUGIN_API queryInterface(const TUID, void **obj) SMTG_OVERRIDE { *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return 1; }
    uint32 PLUGIN_API release() SMTG_OVERRIDE { return 1; }
};
class PChanges : public IParameterChanges {
public:
    PQueue q[MAXQ];
    int n = 0;
    void clear() { n = 0; }
    PQueue *find_or_add(ParamID id, int32 *idx)
    {
        for (int i = 0; i < n; ++i) if (q[i].id == id) { if (idx) *idx = i; return &q[i]; }
        if (n >= MAXQ) return nullptr;
        q[n].id = id; q[n].npts = 0;
        if (idx) *idx = n;
        return &q[n++];
    }
    int32 PLUGIN_API getParameterCount() SMTG_OVERRIDE { return n; }
    IParamValueQueue *PLUGIN_API getParameterData(int32 i) SMTG_OVERRIDE { return (i >= 0 && i < n) ? &q[i] : nullptr; }
    IParamValueQueue *PLUGIN_API addParameterData(const ParamID &id, int32 &idx) SMTG_OVERRIDE { return find_or_add(id, &idx); }
    tresult PLUGIN_API queryInterface(const TUID, void **obj) SMTG_OVERRIDE { *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return 1; }
    uint32 PLUGIN_API release() SMTG_OVERRIDE { return 1; }
};

class QNoEvents : public IEventList {
public:
    int32 PLUGIN_API getEventCount() SMTG_OVERRIDE { return 0; }
    tresult PLUGIN_API getEvent(int32, Event &) SMTG_OVERRIDE { return kResultFalse; }
    tresult PLUGIN_API addEvent(Event &) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API queryInterface(const TUID, void **obj) SMTG_OVERRIDE { *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return 1; }
    uint32 PLUGIN_API release() SMTG_OVERRIDE { return 1; }
};
static QNoEvents g_noevents;

/* ---- modules (.so): loaded once each, with a usage counter ---- */
typedef bool (*ModEntry)(void *);
typedef bool (*ModExit)();
#ifdef _WIN32
#define MODEXIT_NAME "ExitDll"
#else
#define MODEXIT_NAME "ModuleExit"
#endif
typedef IPluginFactory *(*GetFactory)();
struct Mod { char path[1400]; void *lib; IPluginFactory *f; int refs; };
static Mod g_mods[64];

static bool find_so(const char *bundle, char *out, size_t sz)
{
    struct stat st;
    if (stat(bundle, &st) != 0) return false;
    if (S_ISREG(st.st_mode)) { snprintf(out, sz, "%s", bundle); return true; }
    char dir[1200];
#ifdef _WIN32
    snprintf(dir, sizeof dir, "%s/Contents/x86_64-win", bundle);
    const char *ext = ".vst3";
#else
    snprintf(dir, sizeof dir, "%s/Contents/x86_64-linux", bundle);
    const char *ext = ".so";
#endif
    size_t el = strlen(ext);
    const char *b = strrchr(bundle, '/');
    b = b ? b + 1 : bundle;
    char nm[512];
    snprintf(nm, sizeof nm, "%s", b);
    char *d = strstr(nm, ".vst3");
    if (d) *d = 0;
    snprintf(out, sz, "%.1150s/%.240s%s", dir, nm, ext);
    if (stat(out, &st) == 0) return true;
    DIR *dd = opendir(dir);                               /* a different .so name inside */
    if (!dd) return false;
    struct dirent *de;
    bool ok = false;
    while ((de = readdir(dd))) {
        size_t n = strlen(de->d_name);
        if (n > el && !strcasecmp(de->d_name + n - el, ext)) { snprintf(out, sz, "%.1150s/%.240s", dir, de->d_name); ok = true; break; }
    }
    closedir(dd);
    return ok;
}

static std::mutex g_mod_mu;          /* step 35d: modules may also be loaded from a background thread */
static std::condition_variable g_mod_cv;
static bool g_loading[64];            /* this slot is being loaded right now (without a lock - it may be slow) */

extern "C" int vst3_binary(const char *id, char *out, size_t sz)
{
    char bundle[1024];
    snprintf(bundle, sizeof bundle, "%s", id);
    char *bar = strrchr(bundle, '|');
    if (bar) *bar = 0;
    return find_so(bundle, out, sz) ? 0 : -1;
}

static Mod *mod_open(const char *bundle, char *err, int esz)
{
    char so[1400];
#ifdef _WIN32
    if (!find_so(bundle, so, sizeof so)) { snprintf(err, (size_t)esz, "no 64-bit Windows binary in this VST3"); return nullptr; }
#else
    if (!find_so(bundle, so, sizeof so)) { snprintf(err, (size_t)esz, "no Linux (x86_64) binary in this VST3"); return nullptr; }
#endif
    std::unique_lock<std::mutex> lk(g_mod_mu);
    for (;;) {
        int found = -1;
        for (int i = 0; i < 64; ++i) if ((g_mods[i].lib || g_loading[i]) && !strcmp(g_mods[i].path, so)) found = i;
        if (found < 0) break;
        if (!g_loading[found]) { ++g_mods[found].refs; return &g_mods[found]; }
        g_mod_cv.wait(lk);                                  /* the same file is being loaded by another thread */
    }
    int si = -1;
    for (int i = 0; i < 64; ++i) if (!g_mods[i].lib && !g_loading[i]) { si = i; break; }
    if (si < 0) { snprintf(err, (size_t)esz, "too many VST3 modules"); return nullptr; }
    g_loading[si] = true;
    snprintf(g_mods[si].path, sizeof g_mods[si].path, "%s", so);
    lk.unlock();
    void *lib = dlopen(so, RTLD_NOW | RTLD_LOCAL);
    IPluginFactory *f = nullptr;
    if (!lib) snprintf(err, (size_t)esz, "cannot load: %s", dlerror());
    else {
#ifdef _WIN32
        typedef bool (*InitDllFn)();                   /* Windows: InitDll() / ExitDll() */
        InitDllFn idl = (InitDllFn)dlsym(lib, "InitDll");
        if (idl && !idl()) { snprintf(err, (size_t)esz, "InitDll failed"); dlclose(lib); lib = nullptr; }
#else
        ModEntry me = (ModEntry)dlsym(lib, "ModuleEntry");
        if (me && !me(lib)) { snprintf(err, (size_t)esz, "ModuleEntry failed"); dlclose(lib); lib = nullptr; }
#endif
        else {
            GetFactory gf = (GetFactory)dlsym(lib, "GetPluginFactory");
            f = gf ? gf() : nullptr;
            if (!f) {
                snprintf(err, (size_t)esz, "not a VST3 plugin (no factory)");
                ModExit mx = (ModExit)dlsym(lib, MODEXIT_NAME);
                if (mx) mx();
                dlclose(lib); lib = nullptr;
            }
        }
    }
    lk.lock();
    g_loading[si] = false;
    if (lib) { g_mods[si].lib = lib; g_mods[si].f = f; g_mods[si].refs = 1; }
    else g_mods[si].path[0] = 0;
    g_mod_cv.notify_all();
    return lib ? &g_mods[si] : nullptr;
}

static void mod_close(Mod *m)
{
    std::lock_guard<std::mutex> lk(g_mod_mu);
    if (!m || --m->refs > 0) return;
    m->f->release();
    ModExit mx = (ModExit)dlsym(m->lib, MODEXIT_NAME);
    if (mx) mx();
    dlclose(m->lib);
    m->lib = nullptr; m->path[0] = 0;
}

static void cid_hex(const TUID cid, char out[33])
{
    for (int i = 0; i < 16; ++i) snprintf(out + i * 2, 3, "%02X", (unsigned char)cid[i]);
}
static bool hex_cid(const char *s, TUID cid)
{
    if (strlen(s) < 32) return false;
    for (int i = 0; i < 16; ++i) { unsigned v; if (sscanf(s + i * 2, "%2X", &v) != 1) return false; cid[i] = (char)v; }
    return true;
}

/* ---- instance ---- */
#define V3_MAXBUS 8
struct V3;

class QHandler : public IComponentHandler {          /* the window moves a knob -> to the audio */
public:
    V3 *v = nullptr;
    tresult PLUGIN_API beginEdit(ParamID) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue val) SMTG_OVERRIDE;
    tresult PLUGIN_API endEdit(ParamID) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (iid_is(iid, FUnknown::iid) || iid_is(iid, IComponentHandler::iid)) { *obj = this; return kResultOk; }
        *obj = nullptr; return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return 1; }
    uint32 PLUGIN_API release() SMTG_OVERRIDE { return 1; }
};

class QFrame : public IPlugFrame, public Linux::IRunLoop {   /* the window frame + run loop */
public:
    V3 *v = nullptr;
    tresult PLUGIN_API resizeView(IPlugView *view, ViewRect *r) SMTG_OVERRIDE;
    tresult PLUGIN_API registerEventHandler(Linux::IEventHandler *h, Linux::FileDescriptor fd) SMTG_OVERRIDE
    {
        plugwin_add_fd(fd, [](void *ud, int f) { ((Linux::IEventHandler *)ud)->onFDIsSet(f); }, h);
        return kResultOk;
    }
    tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler *h) SMTG_OVERRIDE { plugwin_remove_fds_of(h); return kResultOk; }
    tresult PLUGIN_API registerTimer(Linux::ITimerHandler *h, Linux::TimerInterval ms) SMTG_OVERRIDE
    {
        return plugwin_add_timer((unsigned)ms, [](void *ud, unsigned) { ((Linux::ITimerHandler *)ud)->onTimer(); }, h) ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler *h) SMTG_OVERRIDE { plugwin_remove_timers_of(h); return kResultOk; }
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (iid_is(iid, FUnknown::iid) || iid_is(iid, IPlugFrame::iid)) { *obj = (IPlugFrame *)this; return kResultOk; }
        if (iid_is(iid, Linux::IRunLoop::iid)) { *obj = (Linux::IRunLoop *)this; return kResultOk; }
        *obj = nullptr; return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return 1; }
    uint32 PLUGIN_API release() SMTG_OVERRIDE { return 1; }
};

struct V3 {
    Mod *mod;
    IComponent *comp;
    IAudioProcessor *proc;
    IEditController *ctrl;          /* may be NULL */
    bool ctrl_separate;             /* a separate object (not comp itself) */
    IConnectionPoint *cp_comp, *cp_ctrl;
    int nib, nob;
    AudioBusBuffers ib[V3_MAXBUS], ob[V3_MAXBUS];
    float **iptr[V3_MAXBUS], **optr[V3_MAXBUS];
    float *scratch;
    int maxb;
    ProcessContext ctx;
    double rate;
    QHandler handler;
    QFrame frame;
    IPlugView *view;
    PlugWin *win;
    Spsc<1024> to_audio, to_ui;
    PChanges in_ch, out_ch;
    std::atomic<bool> dead{false};   /* the plugin threw an error (e.g. yabridge lost its connection to Wine) - it is silent now */
};

/* a C++ exception from the plugin must not reach the C code (otherwise the whole program stops) */
#define GUARD_BEGIN try {
#define GUARD_END(v, where_)                                                                   \
    } catch (const std::exception &ex) {                                                     \
        if (!(v)->dead.exchange(true)) fprintf(stderr, ">>> VST3 plugin error in %s: %s - the plugin is switched off\n", where_, ex.what()); \
    } catch (...) {                                                                          \
        if (!(v)->dead.exchange(true)) fprintf(stderr, ">>> VST3 plugin error in %s - the plugin is switched off\n", where_); \
    }

tresult PLUGIN_API QHandler::performEdit(ParamID id, ParamValue val)
{
    v->to_audio.push(PEv{ id, val });
    qplug_note_edit();
    return kResultOk;
}

tresult PLUGIN_API QFrame::resizeView(IPlugView *view, ViewRect *r)
{
    if (!v->win || !r) return kResultFalse;
    plugwin_resize(v->win, r->getWidth(), r->getHeight());
    view->onSize(r);
    return kResultOk;
}

static void v3_process_inner(V3 *v, float **in, float **out, unsigned n);
static void v3_process(void *h, float **in, float **out, unsigned n)
{
    V3 *v = (V3 *)h;
    if (!v->dead.load(std::memory_order_relaxed)) {
        GUARD_BEGIN
        v3_process_inner(v, in, out, n);
        return;
        GUARD_END(v, "process")
    }
    for (int c = 0; c < v->ob[0].numChannels && c < 2; ++c) memset(out[c], 0, sizeof(float) * n);
}

static void v3_process_inner(V3 *v, float **in, float **out, unsigned n)
{
    if (v->nib > 0) for (int c = 0; c < v->ib[0].numChannels; ++c) v->iptr[0][c] = in[c];
    for (int c = 0; c < v->ob[0].numChannels; ++c) v->optr[0][c] = out[c];
    for (int b = 1; b < v->nib; ++b)                                 /* side inputs: silence */
        for (int c = 0; c < v->ib[b].numChannels; ++c) memset(v->iptr[b][c], 0, sizeof(float) * n);
    v->in_ch.clear();
    PEv e;
    while (v->to_audio.pop(e)) {                                     /* changes from the window */
        int32 idx;
        PQueue *q = v->in_ch.find_or_add(e.id, &idx);
        if (q) { q->v = e.v; q->npts = 1; }
    }
    v->out_ch.clear();
    ProcessData d;
    d.processMode = kRealtime;
    d.symbolicSampleSize = kSample32;
    d.numSamples = (int32)n;
    d.numInputs = v->nib; d.numOutputs = v->nob;
    d.inputs = v->nib ? v->ib : nullptr;
    d.outputs = v->nob ? v->ob : nullptr;
    d.inputParameterChanges = &v->in_ch;
    d.outputParameterChanges = &v->out_ch;
    d.inputEvents = &g_noevents;
    d.outputEvents = &g_noevents;
    d.processContext = &v->ctx;
    v->proc->process(d);
    v->ctx.projectTimeSamples += n;
    for (int i = 0; i < v->out_ch.n; ++i)                            /* meters etc. -> to the window */
        if (v->out_ch.q[i].npts) v->to_ui.push(PEv{ v->out_ch.q[i].id, v->out_ch.q[i].v });
}

static void v3_main_idle(void *h)
{
    V3 *v = (V3 *)h;
    if (v->dead) return;
    GUARD_BEGIN
    PEv e;
    while (v->to_ui.pop(e)) if (v->ctrl) v->ctrl->setParamNormalized(e.id, e.v);
    GUARD_END(v, "idle")
}

/* ---- window ---- */
static bool v3_has_editor(void *h) { return ((V3 *)h)->ctrl != nullptr && !((V3 *)h)->dead; }

static int v3_ed_open_inner(V3 *v, PlugWin *w, int *W, int *H, char *err, int esz);
static int v3_ed_open(void *h, PlugWin *w, int *W, int *H, char *err, int esz)
{
    V3 *v = (V3 *)h;
    GUARD_BEGIN
    return v3_ed_open_inner(v, w, W, H, err, esz);
    GUARD_END(v, "opening the window")
    snprintf(err, (size_t)esz, "the plugin failed (see the log)");
    return -1;
}

static int v3_ed_open_inner(V3 *v, PlugWin *w, int *W, int *H, char *err, int esz)
{
    IPlugView *view = v->ctrl->createView(ViewType::kEditor);
    if (!view) { snprintf(err, (size_t)esz, "this plugin has no window of its own"); return -1; }
#ifdef _WIN32
    FIDString ptype = kPlatformTypeHWND;
    void *parent = plugwin_native(w);
#else
    FIDString ptype = kPlatformTypeX11EmbedWindowID;
    void *parent = (void *)(uintptr_t)plugwin_xid(w);
#endif
    if (view->isPlatformTypeSupported(ptype) != kResultTrue) {
        view->release();
        snprintf(err, (size_t)esz, "the plugin window is not supported on this system");
        return -1;
    }
    v->win = w;
    v->view = view;
    view->setFrame(&v->frame);
    ViewRect r;
    if (view->getSize(&r) == kResultOk && r.getWidth() > 0 && r.getHeight() > 0) plugwin_resize(w, r.getWidth(), r.getHeight());
    if (view->attached(parent, ptype) != kResultOk) {
        view->setFrame(nullptr);
        view->release();
        v->view = nullptr; v->win = nullptr;
        snprintf(err, (size_t)esz, "the plugin could not open its window");
        return -1;
    }
    if (view->getSize(&r) == kResultOk && r.getWidth() > 0 && r.getHeight() > 0) { *W = r.getWidth(); *H = r.getHeight(); }
    return 0;
}

static void v3_ed_close(void *h)
{
    V3 *v = (V3 *)h;
    if (!v->view) return;
    GUARD_BEGIN
    v->view->removed();
    v->view->setFrame(nullptr);
    v->view->release();
    GUARD_END(v, "closing the window")
    v->view = nullptr;
    v->win = nullptr;
}

/* ---- state: "QV3S" + length + component + length + controller ---- */
static int v3_get_state(void *h, void **data, size_t *len)
{
    V3 *v = (V3 *)h;
    if (v->dead) return -1;
    MemStream a, b;
    GUARD_BEGIN
    if (v->comp->getState(&a) != kResultOk) a.d.clear();
    if (v->ctrl && v->ctrl_separate && v->ctrl->getState(&b) != kResultOk) b.d.clear();
    GUARD_END(v, "saving settings")
    if (v->dead) return -1;
    size_t n = 4 + 4 + a.d.size() + 4 + b.d.size();
    unsigned char *p = (unsigned char *)malloc(n);
    if (!p) return -1;
    uint32_t la = (uint32_t)a.d.size(), lb = (uint32_t)b.d.size();
    memcpy(p, "QV3S", 4);
    memcpy(p + 4, &la, 4);
    if (la) memcpy(p + 8, a.d.data(), la);
    memcpy(p + 8 + la, &lb, 4);
    if (lb) memcpy(p + 12 + la, b.d.data(), lb);
    *data = p; *len = n;
    return 0;
}

static int v3_set_state(void *h, const void *data, size_t len)
{
    V3 *v = (V3 *)h;
    const unsigned char *p = (const unsigned char *)data;
    if (len < 12 || memcmp(p, "QV3S", 4)) return -1;
    uint32_t la, lb;
    memcpy(&la, p + 4, 4);
    if (8 + (size_t)la + 4 > len) return -1;
    memcpy(&lb, p + 8 + la, 4);
    if (12 + (size_t)la + lb > len) return -1;
    GUARD_BEGIN
    if (la) {
        MemStream a; a.d.assign(p + 8, p + 8 + la);
        v->comp->setState(&a);
        if (v->ctrl) { a.pos = 0; v->ctrl->setComponentState(&a); }
    }
    if (lb && v->ctrl && v->ctrl_separate) {
        MemStream b; b.d.assign(p + 12 + la, p + 12 + la + lb);
        v->ctrl->setState(&b);
    }
    return 0;
    GUARD_END(v, "loading settings")
    return -1;
}

static void ctrl_teardown(V3 *v)
{
    if (v->cp_comp && v->cp_ctrl) { v->cp_comp->disconnect(v->cp_ctrl); v->cp_ctrl->disconnect(v->cp_comp); }
    if (v->cp_comp) v->cp_comp->release();
    if (v->cp_ctrl) v->cp_ctrl->release();
    v->cp_comp = v->cp_ctrl = nullptr;
    if (v->ctrl) {
        v->ctrl->setComponentHandler(nullptr);
        if (v->ctrl_separate) { v->ctrl->terminate(); }
        v->ctrl->release();
        v->ctrl = nullptr;
    }
}

static void v3_destroy(void *h)
{
    V3 *v = (V3 *)h;
    v3_ed_close(v);
    GUARD_BEGIN
    v->proc->setProcessing(false);
    v->comp->setActive(false);
    ctrl_teardown(v);
    v->proc->release();
    v->comp->terminate();
    v->comp->release();
    GUARD_END(v, "closing")
    for (int b = 0; b < V3_MAXBUS; ++b) { free(v->iptr[b]); free(v->optr[b]); }
    free(v->scratch);
    mod_close(v->mod);
    delete v;
}

/* the buses: the main one stereo (if the plugin accepts it), the others with whatever channels they have */
static int setup_buses(V3 *v)
{
    IComponent *c = v->comp;
    v->nib = c->getBusCount(kAudio, kInput);
    v->nob = c->getBusCount(kAudio, kOutput);
    if (v->nib > V3_MAXBUS) v->nib = V3_MAXBUS;
    if (v->nob > V3_MAXBUS) v->nob = V3_MAXBUS;
    SpeakerArrangement ia[V3_MAXBUS], oa[V3_MAXBUS];
    for (int b = 0; b < v->nib; ++b) { ia[b] = 0; v->proc->getBusArrangement(kInput, b, ia[b]); }
    for (int b = 0; b < v->nob; ++b) { oa[b] = 0; v->proc->getBusArrangement(kOutput, b, oa[b]); }
    if (v->nib > 0) ia[0] = SpeakerArr::kStereo;
    if (v->nob > 0) oa[0] = SpeakerArr::kStereo;
    if (v->proc->setBusArrangements(ia, v->nib, oa, v->nob) != kResultTrue) {
        for (int b = 0; b < v->nib; ++b) v->proc->getBusArrangement(kInput, b, ia[b]);
        for (int b = 0; b < v->nob; ++b) v->proc->getBusArrangement(kOutput, b, oa[b]);
    }
    int total = 0;
    for (int b = 0; b < v->nib; ++b) { v->ib[b].numChannels = SpeakerArr::getChannelCount(ia[b]); total += v->ib[b].numChannels; }
    for (int b = 0; b < v->nob; ++b) { v->ob[b].numChannels = SpeakerArr::getChannelCount(oa[b]); total += v->ob[b].numChannels; }
    v->scratch = (float *)calloc((size_t)(total + 1) * (size_t)v->maxb, sizeof(float));
    float *s = v->scratch;
    for (int b = 0; b < v->nib; ++b) {
        int nc = v->ib[b].numChannels;
        v->iptr[b] = (float **)calloc((size_t)nc + 1, sizeof(float *));
        for (int k = 0; k < nc; ++k) { v->iptr[b][k] = s; s += v->maxb; }
        v->ib[b].channelBuffers32 = v->iptr[b];
        c->activateBus(kAudio, kInput, b, b == 0);
    }
    for (int b = 0; b < v->nob; ++b) {
        int nc = v->ob[b].numChannels;
        v->optr[b] = (float **)calloc((size_t)nc + 1, sizeof(float *));
        for (int k = 0; k < nc; ++k) { v->optr[b][k] = s; s += v->maxb; }
        v->ob[b].channelBuffers32 = v->optr[b];
        c->activateBus(kAudio, kOutput, b, b == 0);
    }
    return 0;
}

/* the controller (the knobs and the window): the same object or a separate class */
static void ctrl_setup(V3 *v)
{
    IEditController *ec = nullptr;
    if (v->comp->queryInterface(IEditController::iid, (void **)&ec) == kResultOk && ec) {
        v->ctrl = ec;
        v->ctrl_separate = false;
    } else {
        TUID ccid;
        if (v->comp->getControllerClassId(ccid) != kResultOk) return;
        if (v->mod->f->createInstance(ccid, IEditController::iid, (void **)&ec) != kResultOk || !ec) return;
        if (ec->initialize(&g_hostapp) != kResultOk) { ec->release(); return; }
        v->ctrl = ec;
        v->ctrl_separate = true;
        IConnectionPoint *a = nullptr, *b = nullptr;
        v->comp->queryInterface(IConnectionPoint::iid, (void **)&a);
        ec->queryInterface(IConnectionPoint::iid, (void **)&b);
        if (a && b) { a->connect(b); b->connect(a); v->cp_comp = a; v->cp_ctrl = b; }
        else { if (a) a->release(); if (b) b->release(); }
        MemStream st;                                                /* the controller learns the state */
        if (v->comp->getState(&st) == kResultOk) { st.pos = 0; ec->setComponentState(&st); }
    }
    v->handler.v = v;
    v->ctrl->setComponentHandler(&v->handler);
}

extern "C" int vst3_open(const char *id, double rate, int maxb, QInst *out, char *err, int esz)
{
    char bundle[1024];
    snprintf(bundle, sizeof bundle, "%s", id);
    char *bar = strrchr(bundle, '|');
    TUID cid;
    if (!bar || !hex_cid(bar + 1, cid)) { snprintf(err, (size_t)esz, "bad VST3 id"); return -1; }
    *bar = 0;
    Mod *m = mod_open(bundle, err, esz);
    if (!m) return -1;
    IComponent *comp = nullptr;
    if (m->f->createInstance(cid, IComponent::iid, (void **)&comp) != kResultOk || !comp) {
        snprintf(err, (size_t)esz, "cannot create the plugin");
        mod_close(m); return -1;
    }
    if (comp->initialize(&g_hostapp) != kResultOk) {
        snprintf(err, (size_t)esz, "plugin failed to initialize");
        comp->release(); mod_close(m); return -1;
    }
    IAudioProcessor *proc = nullptr;
    if (comp->queryInterface(IAudioProcessor::iid, (void **)&proc) != kResultOk || !proc) {
        snprintf(err, (size_t)esz, "not an audio processor");
        comp->terminate(); comp->release(); mod_close(m); return -1;
    }
    if (proc->canProcessSampleSize(kSample32) != kResultTrue) {
        snprintf(err, (size_t)esz, "plugin does not support 32-bit float");
        proc->release(); comp->terminate(); comp->release(); mod_close(m); return -1;
    }
    V3 *v = new V3();
    v->mod = m; v->comp = comp; v->proc = proc; v->maxb = maxb; v->rate = rate;
    v->frame.v = v;
    ctrl_setup(v);
    setup_buses(v);
    ProcessSetup ps;
    ps.processMode = kRealtime;
    ps.symbolicSampleSize = kSample32;
    ps.maxSamplesPerBlock = maxb;
    ps.sampleRate = rate;
    if (proc->setupProcessing(ps) != kResultOk) {
        snprintf(err, (size_t)esz, "plugin rejected sample rate / block size");
        ctrl_teardown(v);
        proc->release(); comp->terminate(); comp->release(); mod_close(m);
        for (int b = 0; b < V3_MAXBUS; ++b) { free(v->iptr[b]); free(v->optr[b]); }
        free(v->scratch); delete v;
        return -1;
    }
    if (comp->setActive(true) != kResultOk) {
        snprintf(err, (size_t)esz, "plugin refused to activate");
        ctrl_teardown(v);
        proc->release(); comp->terminate(); comp->release(); mod_close(m);
        for (int b = 0; b < V3_MAXBUS; ++b) { free(v->iptr[b]); free(v->optr[b]); }
        free(v->scratch); delete v;
        return -1;
    }
    proc->setProcessing(true);
    memset(&v->ctx, 0, sizeof v->ctx);
    v->ctx.sampleRate = rate;
    v->ctx.tempo = 120.0;
    v->ctx.timeSigNumerator = 4; v->ctx.timeSigDenominator = 4;
    v->ctx.state = ProcessContext::kTempoValid | ProcessContext::kTimeSigValid;
    out->h = v;
    out->nin = v->nib > 0 ? v->ib[0].numChannels : 0;
    out->nout = v->nob > 0 ? v->ob[0].numChannels : 0;
    out->process = v3_process; out->destroy = v3_destroy;
    out->has_editor = v3_has_editor; out->ed_open = v3_ed_open; out->ed_close = v3_ed_close;
    out->main_idle = v3_main_idle;
    out->get_state = v3_get_state; out->set_state = v3_set_state;
    return 0;
}

extern "C" int vst3_scan(const char *bundle)
{
    char err[256];
    const char *base = strrchr(bundle, '/');
    base = base ? base + 1 : bundle;
    Mod *m = mod_open(bundle, err, sizeof err);
    if (!m) { plug_print_line("ERR", "VST3", base, bundle, err); return 0; }
    IPluginFactory2 *f2 = nullptr;
    m->f->queryInterface(IPluginFactory2::iid, (void **)&f2);
    int n = m->f->countClasses(), found = 0;
    for (int i = 0; i < n; ++i) {
        PClassInfo ci;
        if (m->f->getClassInfo(i, &ci) != kResultOk) continue;
        if (strcmp(ci.category, kVstAudioEffectClass) != 0) continue;
        bool inst = false;
        if (f2) {
            PClassInfo2 c2;
            if (f2->getClassInfo2(i, &c2) == kResultOk && strstr(c2.subCategories, "Instrument")) inst = true;
        }
        char hex[33], id[1100], extra[64];
        cid_hex(ci.cid, hex);
        snprintf(id, sizeof id, "%s|%s", bundle, hex);
        snprintf(extra, sizeof extra, "%s\t2\t2", inst ? "inst" : "fx");
        plug_print_line("PLUG", "VST3", ci.name[0] ? ci.name : base, id, extra);
        ++found;
    }
    if (f2) f2->release();
    if (!found) plug_print_line("ERR", "VST3", base, bundle, "no audio plugins inside");
    mod_close(m);
    return 0;
}
