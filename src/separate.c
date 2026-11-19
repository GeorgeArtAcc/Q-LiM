/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.4.14 / 1.4.17: separation into parts - the Q-LiM side (see separate.h and src/sep/qlim_separate.c) */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "separate.h"
#include "export.h"
#include "edit.h"
#include <sndfile.h>
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
extern char **environ;
#endif

#if (defined(__x86_64__) || defined(_M_X64)) && (defined(_WIN32) || defined(__GLIBC__))
#define SEP_ARCH_OK 1
#else
#define SEP_ARCH_OK 0                     /* 32-bit, musl (Alpine): ONNX Runtime has no build for it */
#endif

/* what is downloaded on the first use */
#define ORT_VER     "1.22.0"
#ifdef _WIN32
#define ORT_URL     "https://github.com/microsoft/onnxruntime/releases/download/v" ORT_VER "/onnxruntime-win-x64-" ORT_VER ".zip"
#define ORT_ARCHIVE "onnxruntime-win-x64-" ORT_VER ".zip"
#define ORT_MEMBER  "onnxruntime-win-x64-" ORT_VER "/lib/onnxruntime.dll"
#define ORT_LIB     "onnxruntime.dll"
#define ORT_SIZE    0                     /* (checked by loading it) */
#define ORT_SHA     ""
#define HELPER_EXE  "qlim-separate.exe"
#else
#define ORT_URL     "https://github.com/microsoft/onnxruntime/releases/download/v" ORT_VER "/onnxruntime-linux-x64-" ORT_VER ".tgz"
#define ORT_ARCHIVE "onnxruntime-linux-x64-" ORT_VER ".tgz"
#define ORT_MEMBER  "onnxruntime-linux-x64-" ORT_VER "/lib/libonnxruntime.so." ORT_VER
#define ORT_LIB     "libonnxruntime.so." ORT_VER
#define ORT_SIZE    7798730.0
#define ORT_SHA     "8344d55f93d5bc5021ce342db50f62079daf39aaafb5d311a451846228be49b3"
#define HELPER_EXE  "qlim-separate"
#endif
#define STEM_VER    "melroformer-v1"     /* (the key of a vocals-only separation, as in 1.4.14) */

/* the models (downloaded once, when first needed) */
typedef struct {
    const char *title, *info, *kind, *file, *url, *url_env, *sha, *ver;
    double size;
    int ram_mb;
} SepModel;
static const SepModel MODELS[SEP_NMODEL] = {
    { "Mel-Band RoFormer (vocals)",
      "The cleanest vocals. Kimberley Jensen's weights, MIT.",
      "mel", "mel-roformer.fp16.onnx",
      "https://huggingface.co/audiojs/mel-roformer/resolve/main/mel-roformer.fp16.onnx", "QLIM_SEP_MODEL_URL",
      "adcbbb826f12453fbb86652b503242d565a9c81cdef1bca2fe0e54752eba91ef", "mel1", 464050097.0, 3000 },
    { "BS-RoFormer SW (6 parts)",
      "The best drums / bass / guitar / piano; slow. Community weights (licence not stated).",
      "bsrsw", "bs_roformer_sw_6stem_fp16.onnx",
      "https://huggingface.co/elicwhite/bs-roformer-sw-6stem-onnx/resolve/main/bs_roformer_sw_6stem_fp16.onnx", "QLIM_SEP_BSRSW_URL",
      "d3d2bac77a7023282cb5f35a5807179e34076b60589867b572275f1a8ec36444", "bsrsw1", 352778874.0, 3600 },
    { "HTDemucs 6s (6 parts, light)",
      "About 5x faster and lighter, a little less clean. Meta's weights, MIT.",
      "demucs", "htdemucs_6s_fp16weights.onnx",
      "https://huggingface.co/adowu/htdemucs-6s-onnx/resolve/main/htdemucs_6s_fp16weights.onnx", "QLIM_SEP_DEMUCS_URL",
      "7ce55792e2231c93fbf92de95f5fd5b3a5e6c89f7db690dfd693e8f1dce56869", "demucs1", 136428532.0, 2100 },
};
static const char *const PART_NAMES[SEP_NPART] = { "Vocals", "Drums", "Bass", "Guitar", "Piano" };
static const char *const PART_FILE[SEP_NPART] = { "vocals", "drums", "bass", "guitar", "piano" };   /* qlim-separate's names */
const char *sep_part_name(int i) { return i >= 0 && i < SEP_NPART ? PART_NAMES[i] : "Other"; }

static atomic_int g_running = 0;                  /* separations running (the models must not be removed meanwhile) */
bool sep_busy_with_models(void) { return atomic_load(&g_running) > 0; }

static char g_helper[1024], g_data[1024], g_cache[1024];

void sep_set_paths(const char *helper, const char *data_dir, const char *cache_dir)
{
    snprintf(g_helper, sizeof g_helper, "%s", helper ? helper : "");
    snprintf(g_data, sizeof g_data, "%s", data_dir ? data_dir : "");
    snprintf(g_cache, sizeof g_cache, "%s", cache_dir ? cache_dir : "");
}

static void mkdirs(const char *path)
{
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; ++c) if (*c == '/') { *c = 0; mkdir(p, 0755); *c = '/'; }
    mkdir(p, 0755);
}

static bool file_size(const char *p, double *sz)
{
    struct stat st;
    if (stat(p, &st) != 0) return false;
    if (sz) *sz = (double)st.st_size;
    return true;
}

static void helper_path(char *out, size_t n)
{
    if (g_helper[0]) { snprintf(out, n, "%s", g_helper); return; }
#ifdef _WIN32
    snprintf(out, n, "%s/" HELPER_EXE, qw_exe_dir());
#else
    char exe[1024];
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k <= 0) { out[0] = 0; return; }
    exe[k] = 0;
    char *sl = strrchr(exe, '/');
    if (sl) *sl = 0;
    snprintf(out, n, "%s/" HELPER_EXE, exe);
#endif
}

void sep_data_dir(char *out, size_t n)
{
    if (g_data[0]) { snprintf(out, n, "%s", g_data); return; }
#ifdef _WIN32
    const char *b = getenv("LOCALAPPDATA");
    snprintf(out, n, "%s/qlim/separation", b && *b ? b : ".");
#else
    const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
    if (x && *x) snprintf(out, n, "%s/qlim/separation", x);
    else snprintf(out, n, "%s/.local/share/qlim/separation", h && *h ? h : "/tmp");
#endif
}

static void cache_dir(char *out, size_t n)
{
    if (g_cache[0]) { snprintf(out, n, "%s", g_cache); return; }
#ifdef _WIN32
    const char *b = getenv("LOCALAPPDATA");
    snprintf(out, n, "%s/qlim/stems", b && *b ? b : ".");
#else
    const char *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");
    if (x && *x) snprintf(out, n, "%s/qlim/stems", x);
    else snprintf(out, n, "%s/.cache/qlim/stems", h && *h ? h : "/tmp");
#endif
}

bool sep_supported(void)
{
#if SEP_ARCH_OK
    char h[1100];
    helper_path(h, sizeof h);
    return h[0] && access(h, F_OK) == 0;
#else
    return false;
#endif
}

static void data_file(char *out, size_t n, const char *name)
{
    char d[1024];
    sep_data_dir(d, sizeof d);
    snprintf(out, n, "%s/%s", d, name);
}

const char *sep_model_title(int m) { return m >= 0 && m < SEP_NMODEL ? MODELS[m].title : ""; }
const char *sep_model_info(int m) { return m >= 0 && m < SEP_NMODEL ? MODELS[m].info : ""; }
double sep_model_download_mb(int m) { return m >= 0 && m < SEP_NMODEL ? MODELS[m].size / 1048576.0 : 0.0; }
bool sep_model_installed(int m)
{
    if (m < 0 || m >= SEP_NMODEL) return false;
    char f[1100];
    double sz = 0;
    data_file(f, sizeof f, MODELS[m].file);
    return file_size(f, &sz) && sz == MODELS[m].size;
}
double sep_model_disk_mb(int m)
{
    if (m < 0 || m >= SEP_NMODEL) return 0.0;
    char f[1100];
    double sz = 0;
    data_file(f, sizeof f, MODELS[m].file);
    return file_size(f, &sz) ? sz / 1048576.0 : 0.0;
}
int sep_model_remove(int m)
{
    if (m < 0 || m >= SEP_NMODEL || sep_busy_with_models()) return 1;
    char f[1100], pt[1200];
    data_file(f, sizeof f, MODELS[m].file);
    snprintf(pt, sizeof pt, "%s.part", f);
    remove(pt);
    return remove(f) == 0 || !file_size(f, NULL) ? 0 : 1;
}
bool sep_runtime_installed(void)
{
    char o[1100];
    data_file(o, sizeof o, ORT_LIB);
    return file_size(o, NULL);
}
double sep_runtime_disk_mb(void)
{
    char o[1100];
    double sz = 0;
    data_file(o, sizeof o, ORT_LIB);
    return file_size(o, &sz) ? sz / 1048576.0 : 0.0;
}
int sep_runtime_remove(void)
{
    if (sep_busy_with_models()) return 1;
    char o[1100];
    data_file(o, sizeof o, ORT_LIB);
    if (remove(o) != 0 && file_size(o, NULL)) return 1;
#ifdef _WIN32
    data_file(o, sizeof o, "onnxruntime_providers_shared.dll");
    remove(o);
#endif
    return 0;
}

static unsigned req_parts(const SepRequest *r) { return r ? (r->parts & SEP_P_ALL) : 0; }
int sep_request_models(const SepRequest *r, int out[2])
{
    unsigned p = req_parts(r);
    int n = 0, multi = r && r->multi == SEP_DEMUCS ? SEP_DEMUCS : SEP_BSRSW;
    if (!p) return 0;
    if (p == SEP_P_VOCALS) { out[n++] = SEP_MEL; return n; }
    if ((p & SEP_P_VOCALS) && r->best_vocals) out[n++] = SEP_MEL;
    out[n++] = multi;
    return n;
}
double sep_need_mb(const SepRequest *r)
{
    int m[2], n = sep_request_models(r, m);
    double mb = 0;
    for (int k = 0; k < n; ++k) if (!sep_model_installed(m[k])) mb += MODELS[m[k]].size / 1048576.0;
    if (n > 0 && !sep_runtime_installed()) mb += 8.0;
    return mb;
}
int sep_ram_need_mb(const SepRequest *r)
{
    int m[2], n = sep_request_models(r, m), need = 0;
    for (int k = 0; k < n; ++k) if (MODELS[m[k]].ram_mb > need) need = MODELS[m[k]].ram_mb;   /* one after another */
    return need;
}
int sep_ram_total_mb(void)
{
#ifdef _WIN32
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof ms;
    return GlobalMemoryStatusEx(&ms) ? (int)(ms.ullTotalPhys / 1048576ULL) : 0;
#else
    long pg = sysconf(_SC_PHYS_PAGES), ps = sysconf(_SC_PAGESIZE);
    return pg > 0 && ps > 0 ? (int)((double)pg * (double)ps / 1048576.0) : 0;
#endif
}
int sep_request_parts(const SepRequest *r, char names[][STEM_NAME_LEN])
{
    unsigned p = req_parts(r);
    int n = 0;
    if (!p) return 0;
    for (int k = 0; k < SEP_NPART; ++k) if (p & (1u << k)) snprintf(names[n++], STEM_NAME_LEN, "%s", PART_NAMES[k]);
    snprintf(names[n++], STEM_NAME_LEN, "%s", p == SEP_P_VOCALS ? "Instrumental" : "Other");   /* the rest */
    return n;
}

/* ---------------- 1.4.15: where the stems are kept, and how ---------------- */

static SepPrefs g_prefs;
static bool     g_prefs_loaded = false;

static void prefs_path(char *out, size_t n)
{
    const char *h = getenv("HOME");
    snprintf(out, n, "%s/.config/qlim/separation.conf", h && *h ? h : "/tmp");
}

void sep_default_dir(char *out, size_t n)
{
    const char *h = getenv("HOME");
    snprintf(out, n, "%s/Music/Q-LiM Stems", h && *h ? h : "/tmp");
}

void sep_prefs_get(SepPrefs *p)
{
    if (!g_prefs_loaded) {
        memset(&g_prefs, 0, sizeof g_prefs);
        g_prefs.format = EXP_FLAC; g_prefs.bits = 24; g_prefs.rate = 0; g_prefs.quality = EXP_Q_HIGH;
        g_prefs.last.parts = SEP_P_VOCALS; g_prefs.last.best_vocals = true; g_prefs.last.multi = SEP_BSRSW;
        char path[1100], line[1300];
        prefs_path(path, sizeof path);
        FILE *f = fopen(path, "r");
        if (f) {
            while (fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = 0;
                if (!strncmp(line, "dir=", 4)) snprintf(g_prefs.dir, sizeof g_prefs.dir, "%s", line + 4);
                else if (!strncmp(line, "asked=", 6)) g_prefs.asked = atoi(line + 6) != 0;
                else if (!strncmp(line, "format=", 7)) g_prefs.format = atoi(line + 7);
                else if (!strncmp(line, "bits=", 5)) g_prefs.bits = atoi(line + 5);
                else if (!strncmp(line, "rate=", 5)) g_prefs.rate = atoi(line + 5);
                else if (!strncmp(line, "quality=", 8)) g_prefs.quality = atoi(line + 8);
                else if (!strncmp(line, "parts=", 6)) g_prefs.last.parts = (unsigned)atoi(line + 6) & SEP_P_ALL;
                else if (!strncmp(line, "best_vocals=", 12)) g_prefs.last.best_vocals = atoi(line + 12) != 0;
                else if (!strncmp(line, "multi=", 6)) g_prefs.last.multi = atoi(line + 6) == SEP_DEMUCS ? SEP_DEMUCS : SEP_BSRSW;
            }
            fclose(f);
        }
        if (g_prefs.format < 0 || g_prefs.format >= EXP_NFMT) g_prefs.format = EXP_FLAC;
        if (g_prefs.quality < 0 || g_prefs.quality > 2) g_prefs.quality = EXP_Q_HIGH;
        if (!g_prefs.last.parts) g_prefs.last.parts = SEP_P_VOCALS;
        g_prefs_loaded = true;
    }
    *p = g_prefs;
}

void sep_prefs_set(const SepPrefs *p)
{
    g_prefs = *p;
    g_prefs_loaded = true;
    char path[1100], d[1100];
    prefs_path(path, sizeof path);
    snprintf(d, sizeof d, "%s", path);
    char *sl = strrchr(d, '/');
    if (sl) { *sl = 0; mkdirs(d); }
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Q-LiM: separation - where the separated tracks are kept, in what format, the last choice of parts\n"
               "dir=%s\nasked=%d\nformat=%d\nbits=%d\nrate=%d\nquality=%d\nparts=%u\nbest_vocals=%d\nmulti=%d\n",
            p->dir, p->asked ? 1 : 0, p->format, p->bits, p->rate, p->quality,
            p->last.parts, p->last.best_vocals ? 1 : 0, p->last.multi);
    fclose(f);
}

void sep_stems_dir(char *out, size_t n)
{
    if (g_cache[0]) { snprintf(out, n, "%s", g_cache); return; }
    SepPrefs p;
    sep_prefs_get(&p);
    if (p.dir[0]) snprintf(out, n, "%s", p.dir);
    else sep_default_dir(out, n);
}

/* what the choice is (the old key for vocals only: the songs separated by 1.4.14 / 1.4.15 are found) */
static void req_tag(const SepRequest *r, char *out, size_t n)
{
    unsigned p = req_parts(r);
    if (p == SEP_P_VOCALS) { snprintf(out, n, "%s", STEM_VER); return; }
    int m[2], k = sep_request_models(r, m);
    snprintf(out, n, "parts-v1:%u:%s%s%s", p, MODELS[m[0]].ver, k > 1 ? "+" : "", k > 1 ? MODELS[m[1]].ver : "");
}

/* what identifies the original file: its path, size and time (and the choice of parts / models) */
static int source_key(const char *path, const SepRequest *r, char *out, size_t n)
{
    struct stat st;
    if (stat(path, &st) != 0) return 1;
    char tag[100];
    req_tag(r, tag, sizeof tag);
    snprintf(out, n, "%s|%lld|%lld|%s", path, (long long)st.st_size, (long long)st.st_mtime, tag);
    return 0;
}

#define SOURCE_FILE ".qlim-source"         /* in every song folder: the key, then one line per part: name <TAB> file
                                              (1.4.15: just the vocals' and the instrumental's file names) */

static bool read_source(const char *dir, char *key, size_t kn, StemSet *set)
{
    char p[1200];
    snprintf(p, sizeof p, "%s/" SOURCE_FILE, dir);
    FILE *f = fopen(p, "r");
    if (!f) return false;
    char a[1400] = "", l[800];
    memset(set, 0, sizeof *set);
    bool ok = fgets(a, sizeof a, f) != NULL;
    while (ok && set->n < STEM_MAX && fgets(l, sizeof l, f)) {
        l[strcspn(l, "\r\n")] = 0;
        if (!l[0]) continue;
        char *tab = strchr(l, '\t');
        const char *nm = set->n == 0 ? "Vocals" : "Instrumental", *fn = l;
        if (tab) { *tab = 0; nm = l; fn = tab + 1; }
        snprintf(set->name[set->n], STEM_NAME_LEN, "%s", nm);
        snprintf(set->path[set->n], sizeof set->path[0], "%s/%s", dir, fn);
        set->n++;
    }
    fclose(f);
    if (!ok || set->n < 2) return false;
    a[strcspn(a, "\r\n")] = 0;
    snprintf(key, kn, "%s", a);
    return true;
}

/* calls fn for every song folder in the stems folder (one made by Q-LiM: it has the source file) */
static void each_song(void (*fn)(const char *dir, void *ud), void *ud)
{
    char root[1024];
    sep_stems_dir(root, sizeof root);
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char sub[1400], src[1500];
        snprintf(sub, sizeof sub, "%s/%s", root, e->d_name);
        snprintf(src, sizeof src, "%s/" SOURCE_FILE, sub);
        if (file_size(src, NULL)) fn(sub, ud);
    }
    closedir(d);
}

typedef struct { const char *key; StemSet *out; bool found; } FindCtx;
static void find_cb(const char *dir, void *ud)
{
    FindCtx *c = ud;
    char k[1400];
    static StemSet set;
    if (c->found || !read_source(dir, k, sizeof k, &set) || strcmp(k, c->key) != 0) return;
    for (int i = 0; i < set.n; ++i) if (!file_size(set.path[i], NULL)) return;
    *c->out = set;
    c->found = true;
}

/* 1.4.14 kept them in the cache: <cache>/<hash>/vocals.wav */
static int old_cache_dir(const char *path, char *out, size_t n)
{
    struct stat st;
    if (stat(path, &st) != 0) return 1;
    uint64_t h = 1469598103934665603ULL;
    #define MIX(p, l) do { const unsigned char *b_ = (const unsigned char *)(p); for (size_t i_ = 0; i_ < (l); ++i_) { h ^= b_[i_]; h *= 1099511628211ULL; } } while (0)
    long long sz = (long long)st.st_size, mt = (long long)st.st_mtime;
    MIX(path, strlen(path));
    MIX(&sz, sizeof sz);
    MIX(&mt, sizeof mt);
    MIX(STEM_VER, strlen(STEM_VER));
    #undef MIX
    char c[1024];
    cache_dir(c, sizeof c);
    snprintf(out, n, "%s/%016llx", c, (unsigned long long)h);
    return 0;
}

int sep_cached(const char *audio_path, const SepRequest *r, StemSet *out)
{
    char key[1400];
    if (!audio_path || !out || !req_parts(r) || source_key(audio_path, r, key, sizeof key) != 0) return 1;
    FindCtx c = { key, out, false };
    each_song(find_cb, &c);
    if (c.found) return 0;
    if (req_parts(r) != SEP_P_VOCALS) return 1;
    char d[1100];
    if (old_cache_dir(audio_path, d, sizeof d) != 0) return 1;
    memset(out, 0, sizeof *out);
    out->n = 2;
    snprintf(out->path[0], sizeof out->path[0], "%s/vocals.wav", d);
    snprintf(out->path[1], sizeof out->path[1], "%s/instrumental.wav", d);
    snprintf(out->name[0], STEM_NAME_LEN, "Vocals");
    snprintf(out->name[1], STEM_NAME_LEN, "Instrumental");
    return file_size(out->path[0], NULL) && file_size(out->path[1], NULL) ? 0 : 1;
}

/* space used: all the files of the song folders */
typedef struct { double bytes; int songs; bool del; } UseCtx;
static void use_cb(const char *dir, void *ud)
{
    UseCtx *c = ud;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char f[1600];
        double sz = 0;
        snprintf(f, sizeof f, "%s/%s", dir, e->d_name);
        if (file_size(f, &sz)) c->bytes += sz;
        if (c->del) remove(f);
    }
    closedir(d);
    if (c->del) rmdir(dir);
    c->songs++;
}

void sep_usage(double *bytes, int *songs)
{
    UseCtx c = { 0, 0, false };
    each_song(use_cb, &c);
    if (bytes) *bytes = c.bytes;
    if (songs) *songs = c.songs;
}

int sep_delete_all(void)
{
    UseCtx c = { 0, 0, true };
    each_song(use_cb, &c);
    return c.songs;
}

/* a new folder for a song: "<stems>/<name>", "<name> 2" ... */
static int new_song_dir(const char *audio_path, char *out, size_t n, char *base, size_t bn)
{
    char root[1024];
    sep_stems_dir(root, sizeof root);
    const char *b = strrchr(audio_path, '/');
#ifdef _WIN32
    const char *b2 = strrchr(audio_path, '\\');
    if (b2 && (!b || b2 > b)) b = b2;
#endif
    snprintf(base, bn, "%s", b ? b + 1 : audio_path);
    char *dot = strrchr(base, '.');
    if (dot && dot != base) *dot = 0;
    for (char *c = base; *c; ++c)
        if (*c == '/' || *c == '\\' || *c == ':' || *c == '*' || *c == '?' || *c == '"' || *c == '<' || *c == '>' || *c == '|') *c = '_';
    if (!base[0]) snprintf(base, bn, "song");
    for (int k = 1; k < 1000; ++k) {
        if (k == 1) snprintf(out, n, "%s/%s", root, base);
        else snprintf(out, n, "%s/%s %d", root, base, k);
        struct stat st;
        if (stat(out, &st) != 0) return 0;
    }
    return 1;
}

/* ---------------- child processes (no shell) ---------------- */

typedef struct {
#ifdef _WIN32
    HANDLE h;
#else
    pid_t pid;
#endif
    bool on;
} Proc;

#ifdef _WIN32
static wchar_t *wdup(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof(wchar_t)) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
static void wquote(wchar_t *out, size_t cap, size_t *pos, const wchar_t *a)
{
    #define PUT(c) do { if (*pos + 1 < cap) out[(*pos)++] = (c); } while (0)
    if (*pos) PUT(L' ');
    PUT(L'"');
    for (const wchar_t *p = a; *p; ++p) {
        size_t bs = 0;
        while (*p == L'\\') { ++bs; ++p; }
        if (!*p) { for (size_t i = 0; i < bs * 2; ++i) PUT(L'\\'); break; }
        if (*p == L'"') { for (size_t i = 0; i < bs * 2 + 1; ++i) PUT(L'\\'); PUT(L'"'); }
        else { for (size_t i = 0; i < bs; ++i) PUT(L'\\'); PUT(*p); }
    }
    PUT(L'"');
    out[*pos] = 0;
    #undef PUT
}
#endif

/* starts argv[0] (a full path, or a name searched in PATH); its stdout (and stderr) go to out_file */
static int proc_start(Proc *p, const char *const *argv, const char *out_file, bool low_priority)
{
    memset(p, 0, sizeof *p);
#ifdef _WIN32
    static wchar_t cmd[32768];
    size_t pos = 0;
    cmd[0] = 0;
    for (int i = 0; argv[i]; ++i) {
        wchar_t *w = wdup(argv[i]);
        if (!w) return 1;
        for (wchar_t *c = w; i == 0 && *c; ++c) if (*c == L'/') *c = L'\\';
        wquote(cmd, 32768, &pos, w);
        free(w);
    }
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    wchar_t *wo = wdup(out_file);
    HANDLE fh = wo ? CreateFileW(wo, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    free(wo);
    if (fh == INVALID_HANDLE_VALUE) return 1;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = fh; si.hStdError = fh; si.hStdInput = NULL;
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | (low_priority ? BELOW_NORMAL_PRIORITY_CLASS : 0), NULL, NULL, &si, &pi);
    CloseHandle(fh);
    if (!ok) return 1;
    CloseHandle(pi.hThread);
    p->h = pi.hProcess;
#else
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    pid_t pid;
    int rc = strchr(argv[0], '/') ? posix_spawn(&pid, argv[0], &fa, NULL, (char *const *)argv, environ)
                                  : posix_spawnp(&pid, argv[0], &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return 1;
    if (low_priority) setpriority(PRIO_PROCESS, (id_t)pid, 10);   /* the audio of Q-LiM keeps priority */
    p->pid = pid;
#endif
    p->on = true;
    return 0;
}

/* 1 = still running, 0 = ended (*code = its exit code) */
static int proc_poll(Proc *p, int *code)
{
    if (!p->on) { if (code) *code = -1; return 0; }
#ifdef _WIN32
    if (WaitForSingleObject(p->h, 0) == WAIT_TIMEOUT) return 1;
    DWORD c = 1;
    GetExitCodeProcess(p->h, &c);
    CloseHandle(p->h);
    if (code) *code = (int)c;
#else
    int st = 0;
    pid_t r = waitpid(p->pid, &st, WNOHANG);
    if (r == 0) return 1;
    if (code) *code = r == p->pid && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
    p->on = false;
    return 0;
}

static void proc_kill(Proc *p)
{
    if (!p->on) return;
#ifdef _WIN32
    TerminateProcess(p->h, 1);
    WaitForSingleObject(p->h, 5000);
    CloseHandle(p->h);
#else
    kill(p->pid, SIGTERM);
    for (int i = 0; i < 100; ++i) { if (waitpid(p->pid, NULL, WNOHANG) == p->pid) { p->on = false; return; } struct timespec t = { 0, 20000000 }; nanosleep(&t, NULL); }
    kill(p->pid, SIGKILL);
    waitpid(p->pid, NULL, 0);
#endif
    p->on = false;
}

/* ---------------- the job ---------------- */

struct SepJob {
    pthread_t   thr;
    atomic_int  state;
    _Atomic float progress;
    atomic_int  cancel;
    pthread_mutex_t mu;
    char        text[200];
    char        err[300];
    int         ch, scene;
    char        path[1024];                /* the clip's file */
    ClipSnapshot snap;
    StreamSource src;
    bool        have_src;
    SepRequest  req;
    int         models[2], nmodel;
    atomic_int  step;                      /* the model running now (1..nmodel) */
    StemSet     out;
    char        dir[1100];
    char        base[300];
    int         threads;
};

static void set_text(SepJob *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_text(SepJob *j, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&j->mu);
    vsnprintf(j->text, sizeof j->text, fmt, ap);
    pthread_mutex_unlock(&j->mu);
    va_end(ap);
}

static void nap_ms(int ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }

/* runs a program to the end (or until cancelled); poll() is called ~10 times a second. 0 = exit code 0 */
static int run_wait(SepJob *j, const char *const *argv, const char *log, void (*poll)(SepJob *, void *), void *ud)
{
    Proc p;
    if (proc_start(&p, argv, log, true) != 0) return -2;
    int code = -1;
    while (proc_poll(&p, &code)) {
        if (atomic_load(&j->cancel)) { proc_kill(&p); return -3; }
        if (poll) poll(j, ud);
        nap_ms(100);
    }
    return code;
}

/* the hash a file must have: the helper computes it (it is a 64-bit program anyway) */
static bool sha_ok(SepJob *j, const char *file, const char *want)
{
    if (!want || !*want) return true;
    char h[1100], log[1200];
    helper_path(h, sizeof h);
    snprintf(log, sizeof log, "%s.sha", file);
    const char *argv[] = { h, "--sha256", file, NULL };
    int rc = run_wait(j, argv, log, NULL, NULL);
    char line[200] = "";
    FILE *f = fopen(log, "r");
    if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
    remove(log);
    return rc == 0 && strncmp(line, want, 64) == 0;
}

typedef struct { const char *file; double total; const char *what; } DlPoll;
static void dl_poll(SepJob *j, void *ud)
{
    DlPoll *d = ud;
    double sz = 0;
    file_size(d->file, &sz);
    if (d->total > 0) {
        atomic_store(&j->progress, (float)(sz / d->total));
        set_text(j, "Downloading %s: %.0f of %.0f MB", d->what, sz / 1048576.0, d->total / 1048576.0);
    } else set_text(j, "Downloading %s: %.1f MB", d->what, sz / 1048576.0);
}

/* curl (or wget) -> file.part -> file; 0 = ok */
static int download(SepJob *j, const char *url, const char *file, double size, const char *sha, const char *what)
{
    char part[1200], log[1200];
    snprintf(part, sizeof part, "%s.part", file);
    snprintf(log, sizeof log, "%s.log", file);
    remove(part);
    DlPoll d = { part, size, what };
    atomic_store(&j->state, SEP_DOWNLOADING);
    const char *curl[] = {
#ifdef _WIN32
        "curl.exe",
#else
        "curl",
#endif
        "-L", "-f", "-sS", "--retry", "3", "-o", part, url, NULL };
    int rc = run_wait(j, curl, log, dl_poll, &d);
#ifndef _WIN32
    if (rc == -2 || rc == 127) {                       /* no curl -> wget */
        const char *wget[] = { "wget", "-q", "-O", part, url, NULL };
        rc = run_wait(j, wget, log, dl_poll, &d);
    }
#endif
    if (rc == -3) { remove(part); remove(log); return -3; }
    double got = 0;
    file_size(part, &got);
    if (rc != 0 || (size > 0 && got != size)) {
        char line[200] = "";
        FILE *f = fopen(log, "r");
        if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
        line[strcspn(line, "\r\n")] = 0;
        snprintf(j->err, sizeof j->err, "Could not download %s%s%s%s", what,
                 rc == -2 ? " (curl / wget is missing)" : "", line[0] ? ": " : "", line);
        remove(part); remove(log);
        return 1;
    }
    remove(log);
    set_text(j, "Checking %s...", what);
    if (!sha_ok(j, part, sha)) {
        snprintf(j->err, sizeof j->err, "The downloaded %s is damaged (wrong checksum) - try again", what);
        remove(part);
        return 1;
    }
    remove(file);
    if (rename(part, file) != 0) { snprintf(j->err, sizeof j->err, "Cannot save %s", file); return 1; }
    return 0;
}

/* ONNX Runtime + the models this choice needs (each only once) */
static int install(SepJob *j)
{
    char d[1024], lib[1100], arch[1100];
    sep_data_dir(d, sizeof d);
    mkdirs(d);
    data_file(lib, sizeof lib, ORT_LIB);
    data_file(arch, sizeof arch, ORT_ARCHIVE);
    if (!file_size(lib, NULL)) {
        const char *u = getenv("QLIM_SEP_ORT_URL");          /* (tests: a file:// copy) */
        int rc = download(j, u && *u ? u : ORT_URL, arch, ORT_SIZE, ORT_SHA, "ONNX Runtime");
        if (rc) return rc;
        set_text(j, "Unpacking ONNX Runtime...");
        char log[1200];
        snprintf(log, sizeof log, "%s/tar.log", d);
        const char *tar[] = {
#ifdef _WIN32
            "tar.exe", "-xf",
#else
            "tar", "-xzf",
#endif
            arch, "-C", d, "--strip-components=2", ORT_MEMBER, NULL };
        rc = run_wait(j, tar, log, NULL, NULL);
        remove(log);
        remove(arch);
        if (rc == -3) return -3;
        if (rc != 0 || !file_size(lib, NULL)) { snprintf(j->err, sizeof j->err, "Could not unpack ONNX Runtime (tar)"); return 1; }
    }
    for (int k = 0; k < j->nmodel; ++k) {
        const SepModel *m = &MODELS[j->models[k]];
        if (sep_model_installed(j->models[k])) continue;
        char f[1100], what[120];
        data_file(f, sizeof f, m->file);
        snprintf(what, sizeof what, "the AI model %s", m->title);
        const char *u = getenv(m->url_env);
        int rc = download(j, u && *u ? u : m->url, f, m->size, m->sha, what);
        if (rc) return rc;
    }
    return 0;
}

typedef struct { time_t t0; const char *what; } RunPoll;
static void run_poll(SepJob *j, void *ud)
{
    RunPoll *r = ud;
    char log[1200];
    snprintf(log, sizeof log, "%s/separate.log", j->dir);
    FILE *f = fopen(log, "r");
    if (!f) return;
    char line[300], last[300] = "";
    bool loaded = false;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "PROGRESS ", 9)) snprintf(last, sizeof last, "%s", line);
        else if (!strncmp(line, "LOADED", 6)) { loaded = true; r->t0 = r->t0 > 0 ? r->t0 : time(NULL); }
    }
    fclose(f);
    int st = atomic_load(&j->step), ns = j->nmodel;
    char pre[40] = "";
    if (ns > 1) snprintf(pre, sizeof pre, "Step %d of %d: ", st, ns);
    long a = 0, b = 0;
    if (!loaded || sscanf(last, "PROGRESS %ld %ld", &a, &b) != 2 || b <= 0) {
        set_text(j, "%sLoading the AI model (%s)...", pre, MODELS[j->models[st - 1]].title);
        return;
    }
    if (r->t0 <= 0) r->t0 = time(NULL);
    float p = (float)a / (float)b;
    atomic_store(&j->progress, ((float)(st - 1) + p) / (float)ns);
    double el = difftime(time(NULL), r->t0);
    if (a >= 1 && p < 1.0f) {
        double left = el / p * (1.0 - p);
        if (left >= 90) set_text(j, "%sSeparating %s: %d%% (about %d min left)", pre, r->what, (int)(p * 100), (int)(left / 60 + 0.5));
        else set_text(j, "%sSeparating %s: %d%% (less than %d s left)", pre, r->what, (int)(p * 100), (int)(left / 10 + 1) * 10);
    } else set_text(j, "%sSeparating %s: %d%%", pre, r->what, (int)(p * 100));
}

/* runs one model: 0 ok, -3 cancelled, 1 failed (j->err) */
static int run_model(SepJob *j, int m, const char *in, const char *a1, const char *a2, const char *want, const char *what)
{
    char h[1100], model[1100], lib[1100], log[1200], th[16];
    helper_path(h, sizeof h);
    data_file(model, sizeof model, MODELS[m].file);
    data_file(lib, sizeof lib, ORT_LIB);
    snprintf(log, sizeof log, "%s/separate.log", j->dir);
    snprintf(th, sizeof th, "%d", j->threads);
    const char *argv[24];
    int n = 0;
    argv[n++] = h; argv[n++] = "--ort"; argv[n++] = lib; argv[n++] = "--model"; argv[n++] = model;
    argv[n++] = "--in"; argv[n++] = in; argv[n++] = "--kind"; argv[n++] = MODELS[m].kind;
    if (m == SEP_MEL) { argv[n++] = "--vocals"; argv[n++] = a1; argv[n++] = "--inst"; argv[n++] = a2; }
    else { argv[n++] = "--outdir"; argv[n++] = a1; if (want && *want) { argv[n++] = "--want"; argv[n++] = want; } }
    argv[n++] = "--threads"; argv[n++] = th;
    argv[n] = NULL;
    RunPoll rp = { 0, what };
    remove(log);
    int r = run_wait(j, argv, log, run_poll, &rp);
    if (r == -3) { remove(log); return -3; }
    if (r != 0) {
        char line[300] = "", l2[300] = "";
        FILE *f = fopen(log, "r");
        if (f) { while (fgets(l2, sizeof l2, f)) if (!strncmp(l2, "ERROR", 5)) snprintf(line, sizeof line, "%s", l2 + 6); fclose(f); }
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] && r == -2) snprintf(line, sizeof line, "qlim-separate could not be started");
        if (!line[0]) snprintf(line, sizeof line, "it stopped (out of memory? it needs about %.1f GB of RAM)", MODELS[m].ram_mb / 1024.0);
        snprintf(j->err, sizeof j->err, "Separation failed: %s", line);
        remove(log);
        return 1;
    }
    remove(log);
    return 0;
}

/* the rest = the mix - the chosen parts (so that everything adds up to the original exactly) */
static int make_rest(const char *mix, const char *const *parts, int np, const char *out, char *err, size_t en)
{
    SF_INFO mi;
    memset(&mi, 0, sizeof mi);
    SNDFILE *m = sf_open(mix, SFM_READ, &mi);
    if (!m) { snprintf(err, en, "cannot read the mix"); return 1; }
    SNDFILE *p[SEP_NPART] = { NULL };
    for (int k = 0; k < np; ++k) {
        SF_INFO pi;
        memset(&pi, 0, sizeof pi);
        p[k] = sf_open(parts[k], SFM_READ, &pi);
        if (!p[k] || pi.channels != 2) { snprintf(err, en, "cannot read a separated part"); for (int q = 0; q <= k; ++q) if (p[q]) sf_close(p[q]); sf_close(m); return 1; }
    }
    SF_INFO oi;
    memset(&oi, 0, sizeof oi);
    oi.samplerate = mi.samplerate; oi.channels = 2; oi.format = (mi.frames > 200000000 ? SF_FORMAT_RF64 : SF_FORMAT_WAV) | SF_FORMAT_FLOAT;
    SNDFILE *o = sf_open(out, SFM_WRITE, &oi);
    int rc = o ? 0 : 1;
    if (!o) snprintf(err, en, "cannot write the rest");
    static float a[8192 * 2], b[8192 * 2];
    while (rc == 0) {
        sf_count_t n = sf_readf_float(m, a, 8192);
        if (n <= 0) break;
        for (int k = 0; k < np; ++k) {
            sf_count_t g = sf_readf_float(p[k], b, n);
            for (sf_count_t i = 0; i < g * 2; ++i) a[i] -= b[i];
        }
        if (sf_writef_float(o, a, n) != n) { snprintf(err, en, "writing the rest failed (disk full?)"); rc = 1; }
    }
    if (o) sf_close(o);
    for (int k = 0; k < np; ++k) sf_close(p[k]);
    sf_close(m);
    return rc;
}

static void *job_main(void *arg)
{
    SepJob *j = arg;
    int rc = install(j);
    if (rc != 0) { j->src.close(j->src.ctx); j->have_src = false; }
    char mix[1200], vt[1200], it[1200], md[1200], rest[1200];
    snprintf(mix, sizeof mix, "%s/mix.tmp.wav", j->dir);
    snprintf(vt, sizeof vt, "%s/vocals.tmp.wav", j->dir);
    snprintf(it, sizeof it, "%s/instrumental.tmp.wav", j->dir);
    snprintf(md, sizeof md, "%s/parts.tmp", j->dir);
    snprintf(rest, sizeof rest, "%s/rest.tmp.wav", j->dir);
    unsigned parts = req_parts(&j->req);
    char tmp[SEP_NPART][1300];                         /* where each chosen part was written */
    if (rc == 0) {
        /* 1) the clip's whole file, as it is, at 44.1 kHz (the models' rate) */
        atomic_store(&j->state, SEP_PREPARING);
        atomic_store(&j->progress, 0.0f);
        set_text(j, "Preparing the audio...");
        mkdirs(j->dir);
        ExportOpts o = { EXP_WAV, 32, 44100, 0, 0 };
        char err[300];
        j->have_src = false;
        if (export_clip_now(&j->snap, &j->src, &o, mix, NULL, err, sizeof err) != 0) {
            snprintf(j->err, sizeof j->err, "Could not prepare the audio: %s", err);
            rc = 1;
        }
    }
    /* 2) the models, one after another, in their own program */
    if (rc == 0 && !atomic_load(&j->cancel)) {
        atomic_store(&j->state, SEP_SEPARATING);
        const char *multi_in = mix;
        for (int k = 0; k < j->nmodel && rc == 0; ++k) {
            int m = j->models[k];
            atomic_store(&j->step, k + 1);
            atomic_store(&j->progress, (float)k / (float)j->nmodel);
            set_text(j, "Loading the AI model (%s)...", MODELS[m].title);
            if (m == SEP_MEL) {
                rc = run_model(j, m, mix, vt, it, NULL, "the vocals");
                snprintf(tmp[0], sizeof tmp[0], "%s", vt);
                multi_in = it;                            /* the 6-part model works on what is left */
            } else {
                char want[120] = "";
                bool voc_here = (parts & SEP_P_VOCALS) && j->nmodel == 1;
                for (int q = 0; q < SEP_NPART; ++q) {
                    if (!(parts & (1u << q)) || (q == 0 && !voc_here)) continue;
                    if (want[0]) strcat(want, ",");
                    strcat(want, PART_FILE[q]);
                    snprintf(tmp[q], sizeof tmp[q], "%s/%s.wav", md, PART_FILE[q]);
                }
                mkdirs(md);
                rc = run_model(j, m, multi_in, md, NULL, want, voc_here ? "the parts" : "the instruments");
            }
        }
        if (atomic_load(&j->cancel)) rc = -3;
    }
    /* 3) the rest, then everything saved the way the settings say (File -> Separation...) */
    char fn[STEM_MAX][400];
    int nout = 0;
    if (rc == 0) {
        set_text(j, "Saving the parts...");
        const char *pl[SEP_NPART];
        int np = 0;
        for (int q = 0; q < SEP_NPART; ++q) if (parts & (1u << q)) pl[np++] = tmp[q];
        char e2[300] = "";
        if (parts == SEP_P_VOCALS) snprintf(rest, sizeof rest, "%s", it);       /* mix - vocals: made by the helper */
        else if (make_rest(mix, pl, np, rest, e2, sizeof e2) != 0) { snprintf(j->err, sizeof j->err, "Cannot save the separated tracks: %s", e2); rc = 1; }
        SepPrefs pr;
        sep_prefs_get(&pr);
        ExportOpts so = { pr.format, pr.bits, pr.rate > 0 ? pr.rate : 44100, pr.quality, 0 };
        const char *ext = export_format_ext(pr.format);
        char names[STEM_MAX][STEM_NAME_LEN];
        int nn = sep_request_parts(&j->req, names);
        memset(&j->out, 0, sizeof j->out);
        for (int k = 0; k < nn && rc == 0; ++k) {
            const char *src = k < np ? pl[k] : rest;
            snprintf(fn[k], sizeof fn[k], "%s (%s).%s", j->base, names[k], ext);
            snprintf(j->out.path[k], sizeof j->out.path[k], "%s/%s", j->dir, fn[k]);
            snprintf(j->out.name[k], STEM_NAME_LEN, "%s", names[k]);
            set_text(j, "Saving %s (%s)...", names[k], export_format_name(pr.format));
            if (export_file_now(src, &so, j->out.path[k], e2, sizeof e2) != 0) {
                snprintf(j->err, sizeof j->err, "Cannot save the separated tracks: %s", e2);
                rc = 1;
            } else nout = k + 1;
            if (atomic_load(&j->cancel)) rc = -3;
        }
        j->out.n = nout;
        if (rc == 0) {
            char sp[1200], key[1400];
            snprintf(sp, sizeof sp, "%s/" SOURCE_FILE, j->dir);
            FILE *f = fopen(sp, "w");
            if (f && source_key(j->path, &j->req, key, sizeof key) == 0) {
                fprintf(f, "%s\n", key);
                for (int k = 0; k < nout; ++k) fprintf(f, "%s\t%s\n", j->out.name[k], fn[k]);
                fclose(f);
            } else if (f) fclose(f);
        } else {
            for (int k = 0; k < nout; ++k) remove(j->out.path[k]);
            j->out.n = 0;
        }
    }
    /* the temporary files */
    remove(mix); remove(vt); remove(it); remove(rest);
    for (int q = 0; q < SEP_NPART; ++q) { char t[1300]; snprintf(t, sizeof t, "%s/%s.wav", md, PART_FILE[q]); remove(t); }
    { char t[1300]; snprintf(t, sizeof t, "%s/other.wav", md); remove(t); }
    rmdir(md);
    if (rc != 0) rmdir(j->dir);                       /* (only if it stayed empty) */
    if (rc == -3 || atomic_load(&j->cancel)) atomic_store(&j->state, SEP_CANCELLED);
    else if (rc != 0) atomic_store(&j->state, SEP_FAILED);
    else { atomic_store(&j->progress, 1.0f); atomic_store(&j->state, SEP_DONE); }
    atomic_fetch_sub(&g_running, 1);
    return NULL;
}

static int ncpu(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    return (int)n;
}

SepJob *sep_start(Session *s, int ch, int scene, const SepRequest *r, char *err, size_t en)
{
    if (!sep_supported()) { snprintf(err, en, "Separation needs the 64-bit version of Q-LiM"); return NULL; }
    if (!req_parts(r)) { snprintf(err, en, "Nothing chosen to separate"); return NULL; }
    SepJob *j = calloc(1, sizeof *j);
    if (!j) { snprintf(err, en, "Out of memory"); return NULL; }
    if (session_clip_snapshot(s, ch, scene, &j->snap) != 0) { free(j); snprintf(err, en, "Only audio clips can be separated"); return NULL; }
    /* the WHOLE file, untouched: the stems then follow the clip's cuts, markers, tempo and pitch like the original */
    int64_t src_len = j->snap.edits->src_len;
    edit_free(j->snap.edits);
    j->snap.edits = edit_new(src_len);
    j->snap.start = 0; j->snap.end = (size_t)src_len;
    j->snap.stretch = false; j->snap.speed = 1.0; j->snap.pitch_st = 0.0f;
    j->snap.gain_db = 0.0f; j->snap.pan = 0.0f;
    j->snap.stems = false;
    snprintf(j->path, sizeof j->path, "%s", j->snap.path);
    if (!j->snap.edits || new_song_dir(j->path, j->dir, sizeof j->dir, j->base, sizeof j->base) != 0) {
        edit_free(j->snap.edits); free(j);
        snprintf(err, en, "Cannot read the clip's file");
        return NULL;
    }
    if (session_open_clip_source(s, j->path, &j->src) != 0) {
        edit_free(j->snap.edits); free(j);
        snprintf(err, en, "Cannot read the clip's file");
        return NULL;
    }
    j->have_src = true;
    j->ch = ch; j->scene = scene;
    j->req = *r;
    j->nmodel = sep_request_models(r, j->models);
    atomic_store(&j->step, 1);
    j->threads = ncpu() > 2 ? ncpu() - 1 : ncpu();      /* one core stays free for the audio */
    pthread_mutex_init(&j->mu, NULL);
    atomic_store(&j->state, SEP_PREPARING);
    set_text(j, "Starting...");
    atomic_fetch_add(&g_running, 1);
    if (pthread_create(&j->thr, NULL, job_main, j) != 0) {
        atomic_fetch_sub(&g_running, 1);
        j->src.close(j->src.ctx);
        edit_free(j->snap.edits);
        pthread_mutex_destroy(&j->mu);
        free(j);
        snprintf(err, en, "Cannot start the separation");
        return NULL;
    }
    return j;
}

int sep_state(SepJob *j, float *progress, char *text, size_t n)
{
    if (!j) return SEP_FAILED;
    int st = atomic_load(&j->state);
    if (progress) *progress = atomic_load(&j->progress);
    if (text && n) {
        if (st == SEP_FAILED) snprintf(text, n, "%s", j->err);
        else if (st == SEP_CANCELLED) snprintf(text, n, "Cancelled");
        else if (st == SEP_DONE) snprintf(text, n, "Done");
        else { pthread_mutex_lock(&j->mu); snprintf(text, n, "%s", j->text); pthread_mutex_unlock(&j->mu); }
    }
    return st;
}

bool sep_result(SepJob *j, StemSet *out)
{
    if (!j || !out || atomic_load(&j->state) != SEP_DONE) return false;
    *out = j->out;
    return true;
}

int sep_step(SepJob *j, int *nsteps)
{
    if (!j) return 0;
    if (nsteps) *nsteps = j->nmodel;
    return atomic_load(&j->step);
}

void sep_target(SepJob *j, int *ch, int *scene, char *path, size_t n)
{
    if (!j) return;
    if (ch) *ch = j->ch;
    if (scene) *scene = j->scene;
    if (path && n) snprintf(path, n, "%s", j->path);
}

void sep_cancel(SepJob *j) { if (j) atomic_store(&j->cancel, 1); }

void sep_free(SepJob *j)
{
    if (!j) return;
    atomic_store(&j->cancel, 1);
    pthread_join(j->thr, NULL);
    edit_free(j->snap.edits);
    pthread_mutex_destroy(&j->mu);
    free(j);
}
