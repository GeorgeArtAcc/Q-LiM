/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Finding and checking the installed plugins - see plugins.h */
#define _GNU_SOURCE
#include "plugins.h"
#include "plughost.h"
#include "safefile.h"
#include "version.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#ifndef _WIN32
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef _WIN32
extern char **environ;
#endif

#define MAX_LOC     32
#define SCAN_TIMEOUT_S 30

/* ================= search locations ================= */
static const char *const k_fmt_name[PF_COUNT] = { "VST3", "VST2", "LV2", "CLAP" };
static const char *const k_fmt_code[PF_COUNT] = { "VST3", "VST", "LV2", "CLAP" };
#ifdef _WIN32
/* Windows: the standard folders ("%C" = C:/Program Files/Common Files, "%P" = C:/Program Files,
 * "%L" = C:/Users/<name>/AppData/Local). LV2 is not supported on Windows (no folders). */
static const char *const k_defaults[PF_COUNT][8] = {
    { "%C/VST3", "%L/Programs/Common/VST3", NULL },
    { "%P/VSTPlugins", "%P/Steinberg/VSTPlugins", "%C/VST2", "%C/Steinberg/VST2", NULL },
    { NULL },
    { "%C/CLAP", "%L/Programs/Common/CLAP", NULL },
};
#else
static const char *const k_defaults[PF_COUNT][8] = {
    { "~/.vst3", "/usr/lib/vst3", "/usr/local/lib/vst3", NULL },
    { "~/.vst", "~/.lxvst", "/usr/lib/vst", "/usr/local/lib/vst", "/usr/lib/lxvst", "/usr/local/lib/lxvst", NULL },
    { "~/.lv2", "/usr/lib/lv2", "/usr/local/lib/lv2", "/usr/lib/x86_64-linux-gnu/lv2", NULL },
    { "~/.clap", "/usr/lib/clap", "/usr/local/lib/clap", NULL },
};
#endif
static const char *const k_env[PF_COUNT] = { "VST3_PATH", "VST_PATH", "LV2_PATH", "CLAP_PATH" };

static char g_loc[PF_COUNT][MAX_LOC][PLUGIN_PATH_LEN];
static int  g_nloc[PF_COUNT];
static bool g_loc_loaded;

const char *plugins_fmt_name(int pf) { return (pf >= 0 && pf < PF_COUNT) ? k_fmt_name[pf] : "?"; }
const char *plugins_fmt_code(int pf) { return (pf >= 0 && pf < PF_COUNT) ? k_fmt_code[pf] : "?"; }
int plugins_fmt_index(const char *code)
{
    for (int i = 0; i < PF_COUNT; ++i) if (!strcmp(code, k_fmt_code[i]) || !strcmp(code, k_fmt_name[i])) return i;
    return PF_COUNT;
}

static void cfg_path(char *out, size_t n, const char *file)
{
    const char *h = getenv("HOME");
    snprintf(out, n, "%s/.config/qlim/%s", h ? h : ".", file);
}
static void ensure_dir(void)
{
    const char *h = getenv("HOME");
    char p[600];
    snprintf(p, sizeof p, "%s/.config", h ? h : ".");
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/.config/qlim", h ? h : ".");
    mkdir(p, 0755);
}

static void expand(const char *in, char *out, size_t n)
{
    const char *h = getenv("HOME");
    if (in[0] == '~') snprintf(out, n, "%s%s", h ? h : "", in + 1);
#ifdef _WIN32
    else if (in[0] == '%' && (in[1] == 'C' || in[1] == 'P' || in[1] == 'L')) {
        const char *k = qw_known_folder(in[1] == 'C' ? "common" : in[1] == 'P' ? "programs" : "localappdata");
        if (!k && in[1] == 'C') k = "C:/Program Files/Common Files";
        if (!k && in[1] == 'P') k = "C:/Program Files";
        snprintf(out, n, "%s%s", k ? k : "", in + 2);
    }
#endif
    else snprintf(out, n, "%s", in);
}

static void expand(const char *in, char *out, size_t n);
static void loc_defaults(int pf)
{
    g_nloc[pf] = 0;
    for (int i = 0; k_defaults[pf][i] && g_nloc[pf] < MAX_LOC; ++i)
#ifdef _WIN32
        expand(k_defaults[pf][i], g_loc[pf][g_nloc[pf]++], PLUGIN_PATH_LEN);    /* shown as real folders */
#else
        snprintf(g_loc[pf][g_nloc[pf]++], PLUGIN_PATH_LEN, "%s", k_defaults[pf][i]);
#endif
}

static void loc_save(void)
{
    ensure_dir();
    char p[700], tmp[800];
    cfg_path(p, sizeof p, "plugins.conf");
    FILE *f = safe_open(p, tmp, sizeof tmp);
    if (!f) return;
    fprintf(f, "# Q-LiM plugin locations (Edit -> Plugins...). One folder per line.\n");
    for (int pf = 0; pf < PF_COUNT; ++pf) {
        fprintf(f, "[%s]\n", k_fmt_name[pf]);
        for (int i = 0; i < g_nloc[pf]; ++i) fprintf(f, "%s\n", g_loc[pf][i]);
    }
    safe_commit(f, tmp, p, 0);
}

static void loc_load(void)
{
    if (g_loc_loaded) return;
    g_loc_loaded = true;
    bool seen[PF_COUNT] = { false };
    char p[700];
    cfg_path(p, sizeof p, "plugins.conf");
    FILE *f = fopen(p, "r");
    if (f) {
        char line[PLUGIN_PATH_LEN + 8];
        int cur = -1;
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0] || line[0] == '#') continue;
            if (line[0] == '[') {
                char *e = strchr(line, ']');
                if (e) *e = 0;
                cur = plugins_fmt_index(line + 1);
                if (cur >= PF_COUNT) cur = -1;
                else if (!seen[cur]) { seen[cur] = true; g_nloc[cur] = 0; }
                continue;
            }
            if (cur >= 0 && g_nloc[cur] < MAX_LOC) snprintf(g_loc[cur][g_nloc[cur]++], PLUGIN_PATH_LEN, "%s", line);
        }
        fclose(f);
    }
    for (int pf = 0; pf < PF_COUNT; ++pf) if (!seen[pf]) loc_defaults(pf);
}

int plugins_loc_count(int pf) { loc_load(); return (pf >= 0 && pf < PF_COUNT) ? g_nloc[pf] : 0; }
const char *plugins_loc_get(int pf, int i)
{
    loc_load();
    return (pf >= 0 && pf < PF_COUNT && i >= 0 && i < g_nloc[pf]) ? g_loc[pf][i] : "";
}
bool plugins_loc_exists(int pf, int i)
{
    char p[PLUGIN_PATH_LEN + 300];
    expand(plugins_loc_get(pf, i), p, sizeof p);
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
void plugins_loc_add(int pf, const char *dir)
{
    loc_load();
    if (pf < 0 || pf >= PF_COUNT || !dir || !dir[0] || g_nloc[pf] >= MAX_LOC) return;
    char d[PLUGIN_PATH_LEN];
    snprintf(d, sizeof d, "%s", dir);
    size_t n = strlen(d);
    while (n > 1 && d[n - 1] == '/') d[--n] = 0;
    const char *h = getenv("HOME");                       /* the home folder is written as ~ */
    if (h && h[0] && !strncmp(d, h, strlen(h)) && (d[strlen(h)] == '/' || !d[strlen(h)])) {
        char t[PLUGIN_PATH_LEN];
        snprintf(t, sizeof t, "~%s", d + strlen(h));
        snprintf(d, sizeof d, "%s", t);
    }
    for (int i = 0; i < g_nloc[pf]; ++i) if (!strcmp(g_loc[pf][i], d)) return;
    snprintf(g_loc[pf][g_nloc[pf]++], PLUGIN_PATH_LEN, "%s", d);
    loc_save();
}
void plugins_loc_remove(int pf, int i)
{
    loc_load();
    if (pf < 0 || pf >= PF_COUNT || i < 0 || i >= g_nloc[pf]) return;
    for (int k = i; k < g_nloc[pf] - 1; ++k) memcpy(g_loc[pf][k], g_loc[pf][k + 1], PLUGIN_PATH_LEN);
    --g_nloc[pf];
    loc_save();
}
void plugins_loc_reset(int pf)
{
    loc_load();
    if (pf < 0 || pf >= PF_COUNT) return;
    loc_defaults(pf);
    loc_save();
}

int plugins_ask_folder(char *out, int sz)
{
#ifdef _WIN32
    return qw_dlg_folder("Q-LiM - Add a plugin folder", getenv("HOME"), out, sz);
#else
    char tmp[128];
    snprintf(tmp, sizeof tmp, "/tmp/qlim_zenity_pl_%d.txt", (int)getpid());
    char cmd[512];
    const char *h = getenv("HOME");
    snprintf(cmd, sizeof cmd, "zenity --file-selection --directory --title='Q-LiM - Add a plugin folder' "
             "--filename='%s/' > %s 2>/dev/null", (h && !strchr(h, '\'')) ? h : "/", tmp);
    int r = system(cmd);
    if (r != 0) { unlink(tmp); return 1; }
    FILE *f = fopen(tmp, "r");
    if (!f) return 1;
    out[0] = 0;
    if (!fgets(out, sz, f)) out[0] = 0;
    fclose(f);
    unlink(tmp);
    out[strcspn(out, "\r\n")] = 0;
    return out[0] ? 0 : 1;
#endif
}

/* ================= results ================= */
typedef struct {
    PluginInfo    *pl; int npl;
    PluginProblem *pr; int npr;
} Lists;

static Lists g_cur;                          /* used only by the main thread */
static Lists g_ready;                        /* ready from the background thread, waiting to be swapped in */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static bool g_ready_flag;
static atomic_int g_scanning, g_done, g_total, g_gen;
static char g_current[200];
static bool g_started;
static pthread_t g_thr;
static bool g_thr_valid;
static bool g_full;
static bool g_again;                         /* a new scan was requested while this one is running */

static void lists_free(Lists *l) { free(l->pl); free(l->pr); memset(l, 0, sizeof *l); }

static void poll_ready(void)
{
    pthread_mutex_lock(&g_mu);
    if (g_ready_flag) {
        lists_free(&g_cur);
        g_cur = g_ready;
        memset(&g_ready, 0, sizeof g_ready);
        g_ready_flag = false;
        atomic_fetch_add(&g_gen, 1);
    }
    pthread_mutex_unlock(&g_mu);
    if (g_thr_valid && !atomic_load(&g_scanning)) {
        pthread_join(g_thr, NULL);
        g_thr_valid = false;
        if (g_again) plugins_rescan(g_full);              /* the locations were changed during the scan */
    }
}

int plugins_count(void) { if (!g_started) plugins_start(); poll_ready(); return g_cur.npl; }
const PluginInfo *plugins_get(int i) { poll_ready(); return (i >= 0 && i < g_cur.npl) ? &g_cur.pl[i] : NULL; }
int plugins_problem_count(void) { if (!g_started) plugins_start(); poll_ready(); return g_cur.npr; }
const PluginProblem *plugins_problem_get(int i) { return (i >= 0 && i < g_cur.npr) ? &g_cur.pr[i] : NULL; }
int plugins_generation(void) { poll_ready(); return atomic_load(&g_gen); }

bool plugins_scanning(int *done, int *total, char *current, int csz)
{
    poll_ready();
    if (done) *done = atomic_load(&g_done);
    if (total) *total = atomic_load(&g_total);
    if (current && csz > 0) { pthread_mutex_lock(&g_mu); snprintf(current, (size_t)csz, "%s", g_current); pthread_mutex_unlock(&g_mu); }
    return atomic_load(&g_scanning) != 0;
}

bool plugins_resolve(const char *fmt, const char *p, const char *name, char *out, int sz)
{
    if (!strcmp(fmt, "VST") || strchr(p, '|')) { snprintf(out, (size_t)sz, "%s", p); return true; }
    poll_ready();
    size_t n = strlen(p);
    int best = -1;
    for (int i = 0; i < g_cur.npl; ++i) {
        const PluginInfo *pi = &g_cur.pl[i];
        if (strcmp(pi->fmt, fmt) || strncmp(pi->path, p, n) || pi->path[n] != '|') continue;
        if (best < 0 || !strcmp(pi->name, name)) best = i;
        if (!strcmp(pi->name, name)) break;
    }
    if (best < 0) return false;
    snprintf(out, (size_t)sz, "%s", g_cur.pl[best].path);
    return true;
}

/* ================= cache ================= */
typedef struct { char fmt[8]; char path[PLUGIN_PATH_LEN]; long long mtime; char *lines; } CacheEnt;
static CacheEnt *g_cache; static int g_ncache;

static void cache_clear(void)
{
    for (int i = 0; i < g_ncache; ++i) free(g_cache[i].lines);
    free(g_cache); g_cache = NULL; g_ncache = 0;
}

static void str_append(char **s, const char *add)
{
    size_t a = *s ? strlen(*s) : 0, b = strlen(add);
    char *n = realloc(*s, a + b + 1);
    if (!n) return;
    memcpy(n + a, add, b + 1);
    *s = n;
}

static void cache_load(void)
{
    cache_clear();
    char p[700];
    cfg_path(p, sizeof p, "plugin-cache.txt");
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[4096];
    bool ok = false;
    CacheEnt *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        if (!ok) { ok = !strncmp(line, "QLIM-PLUGIN-CACHE " QLIM_VERSION "\n", strlen("QLIM-PLUGIN-CACHE " QLIM_VERSION "\n")); if (!ok) break; continue; }
        if (!strncmp(line, "F\t", 2)) {
            CacheEnt *n = realloc(g_cache, sizeof(CacheEnt) * (size_t)(g_ncache + 1));
            if (!n) break;
            g_cache = n;
            cur = &g_cache[g_ncache++];
            memset(cur, 0, sizeof *cur);
            char *t1 = line + 2, *t2 = strchr(t1, '\t'), *t3 = t2 ? strchr(t2 + 1, '\t') : NULL;
            if (!t2 || !t3) { --g_ncache; cur = NULL; continue; }
            *t2 = 0; *t3 = 0;
            snprintf(cur->fmt, sizeof cur->fmt, "%s", t1);
            snprintf(cur->path, sizeof cur->path, "%s", t2 + 1);
            cur->mtime = atoll(t3 + 1);
            str_append(&cur->lines, "");
        } else if (cur && (!strncmp(line, "PLUG\t", 5) || !strncmp(line, "ERR\t", 4))) {
            str_append(&cur->lines, line);
        }
    }
    fclose(f);
}

static void cache_save(void)
{
    ensure_dir();
    char p[700], tmp[800];
    cfg_path(p, sizeof p, "plugin-cache.txt");
    FILE *f = safe_open(p, tmp, sizeof tmp);
    if (!f) return;
    fprintf(f, "QLIM-PLUGIN-CACHE %s\n", QLIM_VERSION);
    for (int i = 0; i < g_ncache; ++i)
        fprintf(f, "F\t%s\t%s\t%lld\n%s", g_cache[i].fmt, g_cache[i].path, g_cache[i].mtime, g_cache[i].lines ? g_cache[i].lines : "");
    safe_commit(f, tmp, p, 0);
}

static CacheEnt *cache_find(const char *fmt, const char *path)
{
    for (int i = 0; i < g_ncache; ++i) if (!strcmp(g_cache[i].fmt, fmt) && !strcmp(g_cache[i].path, path)) return &g_cache[i];
    return NULL;
}

/* the newest modification time in the bundle (up to 4 levels deep) */
static long long newest_mtime(const char *p, int depth)
{
    struct stat st;
    if (stat(p, &st) != 0) return 0;
    long long m = (long long)st.st_mtime;
    if (!S_ISDIR(st.st_mode) || depth > 4) return m;
    DIR *d = opendir(p);
    if (!d) return m;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char q[PATH_MAX];
        if ((size_t)snprintf(q, sizeof q, "%s/%s", p, de->d_name) >= sizeof q) continue;
        long long x = newest_mtime(q, depth + 1);
        if (x > m) m = x;
    }
    closedir(d);
    return m;
}

/* ================= finding the candidates ================= */
typedef struct { int pf; char path[PLUGIN_PATH_LEN]; } Cand;
static Cand *g_cand; static int g_ncand;

static bool ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcasecmp(s + a - b, suf) == 0;
}

static void cand_add(int pf, const char *p)
{
    char rp[PATH_MAX];
    const char *use = realpath(p, rp) ? rp : p;
    if (strlen(use) >= PLUGIN_PATH_LEN - 1) return;
    for (int i = 0; i < g_ncand; ++i) if (g_cand[i].pf == pf && !strcmp(g_cand[i].path, use)) return;
    Cand *n = realloc(g_cand, sizeof(Cand) * (size_t)(g_ncand + 1));
    if (!n) return;
    g_cand = n;
    g_cand[g_ncand].pf = pf;
    snprintf(g_cand[g_ncand].path, PLUGIN_PATH_LEN, "%s", use);
    ++g_ncand;
}

static void walk(int pf, const char *dir, int depth)
{
    if (depth > 5) return;
#ifdef _WIN32
    if (pf == PF_LV2) return;                  /* no LV2 on Windows */
#endif
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[PATH_MAX];
        if ((size_t)snprintf(p, sizeof p, "%s/%s", dir, de->d_name) >= sizeof p) continue;
        struct stat st;
        if (stat(p, &st) != 0) continue;
        bool isdir = S_ISDIR(st.st_mode);
        const char *n = de->d_name;
        switch (pf) {
        case PF_VST3:
            if (ends_with(n, ".vst3")) cand_add(pf, p);
            else if (isdir) walk(pf, p, depth + 1);
            break;
        case PF_VST2:
            if (isdir) { if (!ends_with(n, ".vst3") && !ends_with(n, ".lv2") && !ends_with(n, ".clap")) walk(pf, p, depth + 1); }
#ifdef _WIN32
            else if (ends_with(n, ".dll")) cand_add(pf, p);
#else
            else if (ends_with(n, ".so")) cand_add(pf, p);
#endif
            break;
        case PF_LV2:
            if (isdir && ends_with(n, ".lv2")) cand_add(pf, p);
            else if (isdir) walk(pf, p, depth + 1);
            break;
        case PF_CLAP:
            if (ends_with(n, ".clap")) cand_add(pf, p);
            else if (isdir) walk(pf, p, depth + 1);
            break;
        }
    }
    closedir(d);
}

/* ================= the child process ================= */
static double scan_timeout(void)
{
    const char *te = getenv("QLIM_SCAN_TIMEOUT");             /* for tests */
    return (te && atof(te) > 0) ? atof(te) : SCAN_TIMEOUT_S;
}

#ifdef _WIN32
static char *run_child(const char *code, const char *path, int *status, bool *timed_out)
{
    char exe[PATH_MAX];
    snprintf(exe, sizeof exe, "%s/qlim.exe", qw_exe_dir());
    const char *argv[] = { exe, "--plugin-scan", code, path, NULL };
    int to = 0;
    char *out = qw_spawn_capture(argv, scan_timeout(), status, &to);
    *timed_out = to != 0;
    return out;
}
/* Windows: the exit code is the status (0 = fine; a crash = an exception code like 0xC0000005) */
#define CHILD_FAILED(st) ((st) != 0)
#else
#define CHILD_FAILED(st) (WIFSIGNALED(st) || (WIFEXITED(st) && WEXITSTATUS(st)))
static char *run_child(const char *code, const char *path, int *status, bool *timed_out)
{
    *timed_out = false;
    *status = 0;
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) != 0) return NULL;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pfd[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    char exe[PATH_MAX];
    ssize_t el = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (el <= 0) { close(pfd[0]); close(pfd[1]); posix_spawn_file_actions_destroy(&fa); return NULL; }
    exe[el] = 0;
    char *argv[] = { exe, (char *)"--plugin-scan", (char *)code, (char *)path, NULL };
    pid_t pid;
    int rc = posix_spawn(&pid, exe, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pfd[1]);
    if (rc != 0) { close(pfd[0]); return NULL; }
    char *out = NULL;
    size_t len = 0;
    double timeout_s = scan_timeout();
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double el_s = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
        if (el_s > timeout_s) { *timed_out = true; kill(pid, SIGKILL); break; }
        struct pollfd pp = { pfd[0], POLLIN, 0 };
        int pr = poll(&pp, 1, 200);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) continue;
        char buf[4096];
        ssize_t r = read(pfd[0], buf, sizeof buf);
        if (r <= 0) break;
        char *n = realloc(out, len + (size_t)r + 1);
        if (!n) break;
        out = n;
        memcpy(out + len, buf, (size_t)r);
        len += (size_t)r;
        out[len] = 0;
    }
    close(pfd[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    *status = st;
    if (!out) out = calloc(1, 1);
    return out;
}
#endif

/* output of the child process -> only PLUG/ERR lines (TRY without an answer = this plugin crashed the process) */
static char *child_to_lines(const char *code, const char *path, const char *raw, int status, bool timed_out)
{
    char *res = NULL;
    str_append(&res, "");
    char pending[1500] = "", pend_name[200] = "";
    bool skip = raw && (!strncmp(raw, "SKIP\t", 5) || strstr(raw, "\nSKIP\t"));     /* not a plugin at all (Windows helper DLL) */
    const char *s = raw;
    while (s && *s) {
        const char *e = strchr(s, '\n');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        char line[4096];
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, s, n); line[n] = 0;
        s = e ? e + 1 : NULL;
        if (!strncmp(line, "TRY\t", 4)) {
            char *f = line + 4, *nm = strchr(f, '\t'), *id = nm ? strchr(nm + 1, '\t') : NULL;
            if (nm && id) { *id = 0; snprintf(pend_name, sizeof pend_name, "%s", nm + 1); snprintf(pending, sizeof pending, "%s", id + 1); }
            continue;
        }
        if (!strncmp(line, "PLUG\t", 5) || !strncmp(line, "ERR\t", 4)) { str_append(&res, line); str_append(&res, "\n"); pending[0] = 0; }
    }
    char tmo[64];
    snprintf(tmo, sizeof tmo, "timed out (no answer in %.0f s)", scan_timeout());
    const char *why = timed_out ? tmo : "crashed while loading";
    if (pending[0]) {
        char l[2200];
        snprintf(l, sizeof l, "ERR\t%s\t%s\t%s\t%s\n", code, pend_name, pending, why);
        str_append(&res, l);
    } else if ((!res || !res[0]) && !(skip && !timed_out && !CHILD_FAILED(status))) {
        const char *b = strrchr(path, '/');
        char l[1400];
        snprintf(l, sizeof l, "ERR\t%s\t%s\t%s\t%s\n", code, b ? b + 1 : path, path,
                 (timed_out || CHILD_FAILED(status)) ? why : "nothing usable found");
        str_append(&res, l);
    }
    return res;
}

/* ================= the background thread ================= */

static int fmt_rank(const char *fmt) { return plugins_fmt_index(fmt); }
static int cmp_pl(const void *a, const void *b)
{
    const PluginInfo *x = a, *y = b;
    int r = fmt_rank(x->fmt) - fmt_rank(y->fmt);
    if (r) return r;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->path, y->path);
}
static int cmp_pr(const void *a, const void *b)
{
    const PluginProblem *x = a, *y = b;
    int r = fmt_rank(x->fmt) - fmt_rank(y->fmt);
    return r ? r : strcasecmp(x->name, y->name);
}

static void parse_lines(const char *lines, Lists *L)
{
    const char *s = lines;
    while (s && *s) {
        const char *e = strchr(s, '\n');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        char line[4096];
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, s, n); line[n] = 0;
        s = e ? e + 1 : NULL;
        char *f[8] = { 0 };
        int nf = 0;
        for (char *t = line; t && nf < 8; ) { f[nf++] = t; t = strchr(t, '\t'); if (t) *t++ = 0; }
        if (!strcmp(f[0], "PLUG") && nf >= 5) {
            bool inst = !strcmp(f[4], "inst");
            if (inst) {
                PluginProblem *np = realloc(L->pr, sizeof *np * (size_t)(L->npr + 1));
                if (!np) continue;
                L->pr = np;
                PluginProblem *q = &L->pr[L->npr++];
                snprintf(q->fmt, sizeof q->fmt, "%s", f[1]);
                snprintf(q->name, sizeof q->name, "%s", f[2]);
                snprintf(q->path, sizeof q->path, "%s", f[3]);
                snprintf(q->reason, sizeof q->reason, "instrument (needs MIDI notes) - not supported yet");
                continue;
            }
            if (strlen(f[3]) >= PLUGIN_PATH_LEN) continue;
            bool dup = false;
            for (int i = 0; i < L->npl; ++i) if (!strcmp(L->pl[i].fmt, f[1]) && !strcmp(L->pl[i].path, f[3])) dup = true;
            if (dup) continue;
            PluginInfo *np = realloc(L->pl, sizeof *np * (size_t)(L->npl + 1));
            if (!np) continue;
            L->pl = np;
            PluginInfo *p = &L->pl[L->npl++];
            snprintf(p->fmt, sizeof p->fmt, "%s", f[1]);
            snprintf(p->name, sizeof p->name, "%s", f[2]);
            snprintf(p->path, sizeof p->path, "%s", f[3]);
            p->inst = false;
        } else if (!strcmp(f[0], "ERR") && nf >= 5) {
            PluginProblem *np = realloc(L->pr, sizeof *np * (size_t)(L->npr + 1));
            if (!np) continue;
            L->pr = np;
            PluginProblem *q = &L->pr[L->npr++];
            snprintf(q->fmt, sizeof q->fmt, "%s", f[1]);
            snprintf(q->name, sizeof q->name, "%s", f[2]);
            snprintf(q->path, sizeof q->path, "%s", f[3]);
            snprintf(q->reason, sizeof q->reason, "%s", f[4]);
        }
    }
}

/* snapshot of the search locations (the UI may change them while we scan) */
static char g_snap[PF_COUNT][MAX_LOC][PLUGIN_PATH_LEN];
static int  g_nsnap[PF_COUNT];

#define SCAN_WORKERS 4
typedef struct { char *lines; long long mt; bool fresh; } Res;
static Res *g_res;
static atomic_int g_next;

static void *scan_worker(void *arg)
{
    (void)arg;
    for (;;) {
        int i = atomic_fetch_add(&g_next, 1);
        if (i >= g_ncand) break;
        const Cand *c = &g_cand[i];
        const char *code = k_fmt_code[c->pf];
        long long mt = newest_mtime(c->path, 0);
        CacheEnt *ce = cache_find(code, c->path);          /* the cache is only read here */
        if (ce && ce->mtime == mt && ce->lines) {
            g_res[i].lines = strdup(ce->lines);
        } else {
            const char *b = strrchr(c->path, '/');
            pthread_mutex_lock(&g_mu);
            snprintf(g_current, sizeof g_current, "%s  %.150s", k_fmt_name[c->pf], b ? b + 1 : c->path);
            pthread_mutex_unlock(&g_mu);
            int status; bool to;
            char *raw = run_child(code, c->path, &status, &to);
            g_res[i].lines = child_to_lines(code, c->path, raw ? raw : "", status, to);
            g_res[i].fresh = true;
            free(raw);
        }
        g_res[i].mt = mt;
        atomic_fetch_add(&g_done, 1);
    }
    return NULL;
}

static void *scan_thread(void *arg)
{
    (void)arg;
    cache_load();
    if (g_full) cache_clear();
    free(g_cand); g_cand = NULL; g_ncand = 0;
    for (int pf = 0; pf < PF_COUNT; ++pf) {
        for (int i = 0; i < g_nsnap[pf]; ++i) {
            char p[PLUGIN_PATH_LEN + 300];
            expand(g_snap[pf][i], p, sizeof p);
            walk(pf, p, 0);
        }
        const char *ev = getenv(k_env[pf]);                       /* and from the environment variable */
        if (ev && *ev) {
            char buf[4096];
            snprintf(buf, sizeof buf, "%s", ev);
            char *sv = NULL;
#ifdef _WIN32
            for (char *t = strtok_r(buf, ";", &sv); t; t = strtok_r(NULL, ";", &sv)) { qw_slashes(t); walk(pf, t, 0); }
#else
            for (char *t = strtok_r(buf, ":", &sv); t; t = strtok_r(NULL, ":", &sv)) walk(pf, t, 0);
#endif
        }
    }
    atomic_store(&g_total, g_ncand);
    g_res = calloc((size_t)g_ncand + 1, sizeof(Res));
    atomic_store(&g_next, 0);
    pthread_t w[SCAN_WORKERS];
    int nw = 0;
    if (g_res)
        for (int k = 0; k < SCAN_WORKERS && k < g_ncand; ++k)
            if (pthread_create(&w[nw], NULL, scan_worker, NULL) == 0) ++nw;
    if (g_res && g_ncand > 0 && nw == 0) scan_worker(NULL);
    for (int k = 0; k < nw; ++k) pthread_join(w[k], NULL);

    Lists L = { 0 };
    CacheEnt *nc = calloc((size_t)g_ncand + 1, sizeof(CacheEnt));
    int nnc = 0, fresh = 0;
    for (int i = 0; g_res && i < g_ncand; ++i) {
        if (!g_res[i].lines) continue;
        parse_lines(g_res[i].lines, &L);
        if (g_res[i].fresh) ++fresh;
        if (nc) {
            CacheEnt *e = &nc[nnc++];
            snprintf(e->fmt, sizeof e->fmt, "%s", k_fmt_code[g_cand[i].pf]);
            snprintf(e->path, sizeof e->path, "%s", g_cand[i].path);
            e->mtime = g_res[i].mt;
            e->lines = g_res[i].lines;
        } else {
            free(g_res[i].lines);
        }
    }
    free(g_res); g_res = NULL;
    cache_clear();
    g_cache = nc; g_ncache = nnc;
    cache_save();
    cache_clear();
    if (L.npl) qsort(L.pl, (size_t)L.npl, sizeof *L.pl, cmp_pl);
    if (L.npr) qsort(L.pr, (size_t)L.npr, sizeof *L.pr, cmp_pr);
    printf(">>> Plugins: %d usable, %d with problems (%d files checked now, %d from cache)\n",
           L.npl, L.npr, fresh, g_ncand - fresh);
    pthread_mutex_lock(&g_mu);
    lists_free(&g_ready);
    g_ready = L;
    g_ready_flag = true;
    g_current[0] = 0;
    pthread_mutex_unlock(&g_mu);
    atomic_store(&g_scanning, 0);
    return NULL;
}

void plugins_rescan(bool full)
{
    loc_load();
    g_started = true;
    poll_ready();
    if (atomic_load(&g_scanning)) { g_again = true; g_full = g_full || full; return; }
    if (g_thr_valid) { pthread_join(g_thr, NULL); g_thr_valid = false; }
    g_full = full;
    g_again = false;
    memcpy(g_snap, g_loc, sizeof g_snap);
    memcpy(g_nsnap, g_nloc, sizeof g_nsnap);
    atomic_store(&g_done, 0);
    atomic_store(&g_total, 0);
    atomic_store(&g_scanning, 1);
    if (pthread_create(&g_thr, NULL, scan_thread, NULL) != 0) { atomic_store(&g_scanning, 0); return; }
    g_thr_valid = true;
}

void plugins_start(void)
{
    if (g_started) return;
    plugins_rescan(false);
}

/* ================= qlim --plugin-scan FMT PATH ================= */
int plugins_scan_child_main(int argc, char **argv)
{
    if (argc < 4) return 2;
#ifdef _WIN32
    qw_scan_child_prepare();
    setvbuf(stdout, NULL, _IONBF, 0);
#else
    setvbuf(stdout, NULL, _IOLBF, 0);
#endif
    qplug_scan_child(argv[2], argv[3]);
    fflush(stdout);
    _exit(0);                         /* no destructors: some plugins crash on exit */
}
