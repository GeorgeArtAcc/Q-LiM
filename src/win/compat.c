/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Windows compatibility layer - see compat.h */
#ifdef _WIN32
#undef fopen
#undef stat
#undef mkdir
#undef remove
#undef unlink
#undef rename
#undef access
#undef realpath
#undef link
#undef fsync
#undef DIR
#undef dirent
#undef opendir
#undef readdir
#undef closedir
#undef sf_open
#undef setenv
#undef getenv
#undef pipe

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <psapi.h>
#include <wchar.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <sndfile.h>
#include "compat.h"

SNDFILE *sf_wchar_open(const wchar_t *wpath, int mode, SF_INFO *sfinfo);   /* exported by the Windows libsndfile */

/* ================= UTF-8 <-> UTF-16 ================= */

static wchar_t *w_of(const char *s)          /* malloc'ed; '/' stays (Windows accepts it) */
{
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static int u8_of(const wchar_t *w, char *out, int out_size)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, out_size, NULL, NULL);
    if (n <= 0) { if (out_size > 0) out[0] = 0; return -1; }
    return 0;
}

void qw_slashes(char *p) { for (; p && *p; ++p) if (*p == '\\') *p = '/'; }

/* "C:" -> "C:/" (a bare drive means "the current folder on that drive" on Windows) */
static wchar_t *w_path(const char *p)
{
    if (p && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' && p[2] == 0) {
        char d[4] = { p[0], ':', '/', 0 };
        return w_of(d);
    }
    return w_of(p);
}

/* ================= start-up ================= */

static char g_exe_dir[MAX_PATH * 3];
static char g_home[MAX_PATH * 3];

const char *qw_exe_dir(void) { return g_exe_dir; }

char *qw_getenv(const char *name)
{
    if (name && !strcmp(name, "HOME") && g_home[0]) return g_home;
    static char buf[4][2048];
    static int k;
    wchar_t *wn = w_of(name);
    if (!wn) return NULL;
    const wchar_t *v = _wgetenv(wn);
    free(wn);
    if (!v) return NULL;
    char *b = buf[k++ & 3];
    u8_of(v, b, (int)sizeof buf[0]);
    return b;
}

int qw_setenv(const char *name, const char *value)
{
    wchar_t *wn = w_of(name), *wv = w_of(value);
    int r = (wn && wv) ? _wputenv_s(wn, wv) : -1;
    free(wn); free(wv);
    return r;
}

void qw_init(int *argc, char ***argv)
{
    /* arguments as UTF-8, file paths made absolute (we change the working folder below) */
    int n = 0;
    wchar_t **wa = CommandLineToArgvW(GetCommandLineW(), &n);
    if (wa && n > 0) {
        char **av = calloc((size_t)n + 1, sizeof(char *));
        for (int i = 0; av && i < n; ++i) {
            wchar_t full[4096];
            const wchar_t *src = wa[i];
            if (i > 0 && GetFileAttributesW(wa[i]) != INVALID_FILE_ATTRIBUTES &&
                GetFullPathNameW(wa[i], 4096, full, NULL) > 0) src = full;
            char tmp[8192];
            u8_of(src, tmp, sizeof tmp);
            if (i > 0) qw_slashes(tmp);
            av[i] = _strdup(tmp);
        }
        if (av) { *argc = n; *argv = av; }
        LocalFree(wa);
    }

    /* the folder of qlim.exe = the working folder (assets/ are found relative to it) */
    wchar_t exe[MAX_PATH];
    DWORD el = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (el > 0 && el < MAX_PATH) {
        wchar_t *bs = wcsrchr(exe, L'\\');
        if (bs) *bs = 0;
        _wchdir(exe);
        u8_of(exe, g_exe_dir, sizeof g_exe_dir);
        qw_slashes(g_exe_dir);
    }

    /* HOME: the user's profile folder (C:/Users/<name>), with '/' like on Linux */
    const wchar_t *h = _wgetenv(L"HOME");
    if (!h || !h[0]) h = _wgetenv(L"USERPROFILE");
    if (h && h[0]) { u8_of(h, g_home, sizeof g_home); qw_slashes(g_home); }

    /* Ctrl+C etc. and a sane locale for number parsing */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

    /* plugin windows (JUCE, drag & drop, clipboard ...) expect OLE on the main thread, like in every other host */
    OleInitialize(NULL);
}

/* ================= files ================= */

FILE *qw_fopen(const char *path, const char *mode)
{
    wchar_t *wp = w_path(path), *wm = w_of(mode);
    FILE *f = (wp && wm) ? _wfopen(wp, wm) : NULL;
    free(wp); free(wm);
    return f;
}

int qw_stat(const char *path, struct stat *st)
{
    wchar_t *wp = w_path(path);
    if (!wp) { errno = ENOENT; return -1; }
    /* "C:/folder/" with a trailing slash fails in _wstat - remove it (but keep "C:/") */
    size_t l = wcslen(wp);
    while (l > 3 && (wp[l - 1] == L'/' || wp[l - 1] == L'\\')) wp[--l] = 0;
    struct _stat64 s;
    int r = _wstat64(wp, &s);
    free(wp);
    if (r != 0) return -1;
    memset(st, 0, sizeof *st);
    st->st_dev   = s.st_dev;
    st->st_ino   = s.st_ino;
    st->st_mode  = s.st_mode;
    st->st_nlink = s.st_nlink;
    st->st_size  = (off_t)s.st_size;
    st->st_atime = (time_t)s.st_atime;
    st->st_mtime = (time_t)s.st_mtime;
    st->st_ctime = (time_t)s.st_ctime;
    return 0;
}

int qw_mkdir(const char *path, int mode)
{
    (void)mode;
    wchar_t *wp = w_path(path);
    int r = wp ? _wmkdir(wp) : -1;
    free(wp);
    return r;
}

int qw_remove(const char *path)
{
    wchar_t *wp = w_of(path);
    int r = wp ? _wremove(wp) : -1;
    free(wp);
    return r;
}

int qw_rename(const char *from, const char *to)
{
    wchar_t *a = w_of(from), *b = w_of(to);
    int r = (a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) ? 0 : -1;
    if (r != 0) errno = EACCES;
    free(a); free(b);
    return r;
}

int qw_access(const char *path, int mode)
{
    wchar_t *wp = w_path(path);
    int r = wp ? _waccess(wp, mode & 6) : -1;           /* X_OK does not exist on Windows */
    free(wp);
    return r;
}

char *qw_realpath(const char *path, char *resolved)
{
    wchar_t *wp = w_path(path);
    if (!wp) return NULL;
    wchar_t full[4096];
    DWORD n = GetFullPathNameW(wp, 4096, full, NULL);
    DWORD attr = GetFileAttributesW(wp);
    free(wp);
    if (n == 0 || n >= 4096 || attr == INVALID_FILE_ATTRIBUTES) { errno = ENOENT; return NULL; }
    char tmp[PATH_MAX];
    if (u8_of(full, tmp, sizeof tmp) != 0) return NULL;
    qw_slashes(tmp);
    if (!resolved) return _strdup(tmp);
    snprintf(resolved, PATH_MAX, "%s", tmp);
    return resolved;
}

int qw_link(const char *from, const char *to)
{
    wchar_t *a = w_of(from), *b = w_of(to);
    int r = (a && b && CreateHardLinkW(b, a, NULL)) ? 0 : -1;
    free(a); free(b);
    return r;
}

int qw_fsync(int fd) { return _commit(fd); }

/* ================= folders ================= */

struct qw_dir {
    HANDLE h;
    WIN32_FIND_DATAW fd;
    int first;
    struct qw_dirent ent;
};

QW_DIR *qw_opendir(const char *path)
{
    if (!path || !path[0]) { errno = ENOENT; return NULL; }
    size_t l = strlen(path);
    char *pat = malloc(l + 3);
    if (!pat) return NULL;
    memcpy(pat, path, l);
    if (path[l - 1] != '/' && path[l - 1] != '\\') pat[l++] = '/';
    pat[l++] = '*';
    pat[l] = 0;
    wchar_t *wp = w_of(pat);
    free(pat);
    if (!wp) return NULL;
    QW_DIR *d = calloc(1, sizeof *d);
    if (!d) { free(wp); return NULL; }
    d->h = FindFirstFileW(wp, &d->fd);
    free(wp);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); errno = ENOENT; return NULL; }
    d->first = 1;
    return d;
}

struct qw_dirent *qw_readdir(QW_DIR *d)
{
    for (;;) {
        if (!d->first && !FindNextFileW(d->h, &d->fd)) return NULL;
        d->first = 0;
        if (!wcscmp(d->fd.cFileName, L".") || !wcscmp(d->fd.cFileName, L"..")) continue;
        if (d->fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (u8_of(d->fd.cFileName, d->ent.d_name, (int)sizeof d->ent.d_name) != 0) continue;
        return &d->ent;
    }
}

int qw_closedir(QW_DIR *d)
{
    if (!d) return -1;
    FindClose(d->h);
    free(d);
    return 0;
}

/* ================= libsndfile ================= */

void *qw_sf_open(const char *path, int mode, void *info)
{
    wchar_t *wp = w_of(path);
    SNDFILE *f = wp ? sf_wchar_open(wp, mode, (SF_INFO *)info) : NULL;
    free(wp);
    return f;
}

/* ================= processes ================= */

FILE *qw_tmpfile(void)
{
    wchar_t dir[MAX_PATH + 1], name[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH) {                           /* no %TEMP%: our own config folder */
        wchar_t *w = w_of(g_home);
        if (!w) return NULL;
        _snwprintf(dir, MAX_PATH, L"%ls\\.config\\qlim\\", w);
        free(w);
    }
    if (!GetTempFileNameW(dir, L"qlm", 0, name)) return NULL;
    /* T = keep it in memory if possible, D = delete when closed */
    FILE *f = _wfopen(name, L"w+bTD");
    if (!f) DeleteFileW(name);
    return f;
}

int qw_getpid(void) { return (int)GetCurrentProcessId(); }

int qw_process_alive(int pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return 0;
    DWORD code = 0;
    int alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

int qw_process_is_qlim(int pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return 0;
    wchar_t name[MAX_PATH];
    DWORD n = MAX_PATH;
    int ok = QueryFullProcessImageNameW(h, 0, name, &n);
    CloseHandle(h);
    if (!ok) return 0;
    const wchar_t *b = wcsrchr(name, L'\\');
    b = b ? b + 1 : name;
    return _wcsicmp(b, L"qlim.exe") == 0;
}

/* quotes one argument for CreateProcess (the MSVC rules) */
static void quote_arg(wchar_t *out, size_t cap, size_t *pos, const wchar_t *a)
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

int qw_run_wait(const char *const *argv, int hide_output)
{
    (void)hide_output;
    wchar_t cmd[32768];
    size_t pos = 0;
    cmd[0] = 0;
    for (int i = 0; argv[i]; ++i) {
        wchar_t *w = w_of(argv[i]);
        if (!w) return -1;
        quote_arg(cmd, 32768, &pos, w);
        free(w);
    }
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}


/* ================= dlopen & co. for the plugin hosts (see src/win/include/dlfcn.h) ================= */

static __thread char t_dlerr[512];

void *dlopen(const char *path, int mode)
{
    (void)mode;
    t_dlerr[0] = 0;
    wchar_t *w = w_of(path);
    if (!w) { snprintf(t_dlerr, sizeof t_dlerr, "bad path"); return NULL; }
    for (wchar_t *c = w; *c; ++c) if (*c == L'/') *c = L'\\';      /* ALTERED_SEARCH_PATH needs a native path */
    UINT old = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    HMODULE h = LoadLibraryExW(w, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);   /* the plugin's own DLLs next to it */
    SetErrorMode(old);
    free(w);
    if (!h) {
        DWORD e = GetLastError();
        wchar_t wm[400] = L"";
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, wm, 400, NULL);
        char m[400];
        u8_of(wm, m, sizeof m);
        m[strcspn(m, "\r\n")] = 0;
        if (e == ERROR_BAD_EXE_FORMAT) snprintf(t_dlerr, sizeof t_dlerr, "not a 64-bit Windows plugin (error 193)");
        else snprintf(t_dlerr, sizeof t_dlerr, "%s (error %lu)", m[0] ? m : "LoadLibrary failed", (unsigned long)e);
    }
    return (void *)h;
}

void *dlsym(void *lib, const char *name) { return lib ? (void *)GetProcAddress((HMODULE)lib, name) : NULL; }
int   dlclose(void *lib) { return (lib && FreeLibrary((HMODULE)lib)) ? 0 : -1; }
char *dlerror(void) { return t_dlerr[0] ? t_dlerr : (char *)"unknown error"; }

/* ================= the plugin scan child process ================= */

/* runs argv (UTF-8) with no window; its stdout is collected; killed after timeout_s.
 * *exit_code: the process exit code (a crash = an exception code like 0xC0000005). */
char *qw_spawn_capture(const char *const *argv, double timeout_s, int *exit_code, int *timed_out)
{
    *exit_code = -1; *timed_out = 0;
    wchar_t cmd[32768];
    size_t pos = 0;
    cmd[0] = 0;
    for (int i = 0; argv[i]; ++i) {
        wchar_t *w = w_of(argv[i]);
        if (!w) return NULL;
        quote_arg(cmd, 32768, &pos, w);
        free(w);
    }
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) return NULL;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul; si.hStdOutput = wr; si.hStdError = nul;
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return NULL; }
    char *out = NULL;
    size_t len = 0;
    ULONGLONG t0 = GetTickCount64();
    for (;;) {
        DWORD avail = 0;
        BOOL alive = PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL);
        if (alive && avail > 0) {
            char buf[4096];
            DWORD got = 0;
            if (!ReadFile(rd, buf, avail < sizeof buf ? avail : sizeof buf, &got, NULL) || got == 0) break;
            char *n = realloc(out, len + got + 1);
            if (!n) break;
            out = n;
            memcpy(out + len, buf, got);
            len += got;
            out[len] = 0;
            continue;
        }
        if (!alive) break;                                         /* the pipe is closed: the child is gone */
        if ((double)(GetTickCount64() - t0) / 1000.0 > timeout_s) { *timed_out = 1; TerminateProcess(pi.hProcess, 9); break; }
        Sleep(20);
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    *exit_code = (int)code;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (!out) out = calloc(1, 1);
    return out;
}

/* in the scan child: a crashing plugin must end the process at once (no "has stopped working" window) */
static LONG WINAPI scan_crash(EXCEPTION_POINTERS *ep)
{
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 3);
    return EXCEPTION_EXECUTE_HANDLER;
}
void qw_scan_child_prepare(void)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(scan_crash);
    _setmode(_fileno(stdout), 0x8000 /* _O_BINARY: "\n" stays "\n" */);
}

/* the default plugin folders: "C:/Program Files/Common Files" etc. with '/' */
const char *qw_known_folder(const char *which)
{
    static char buf[6][1024];
    static int k;
    const wchar_t *v = NULL;
    if (!strcmp(which, "common")) v = _wgetenv(L"CommonProgramFiles");
    else if (!strcmp(which, "programs")) v = _wgetenv(L"ProgramFiles");
    else if (!strcmp(which, "localappdata")) v = _wgetenv(L"LOCALAPPDATA");
    if (!v || !v[0]) return NULL;
    char *b = buf[k++ % 6];
    u8_of(v, b, (int)sizeof buf[0]);
    qw_slashes(b);
    return b;
}

/* ================= dialogs ================= */

static HWND owner(void) { return GetActiveWindow(); }

static void w_backslashes(wchar_t *p) { for (; p && *p; ++p) if (*p == L'/') *p = L'\\'; }

/* filter "Q-LiM project\0*.limproject\0All files\0*.*\0\0" */
static void make_filter(wchar_t *out, size_t cap, const char *name, const char *ext)
{
    size_t pos = 0;
    wchar_t *wn = w_of(name && name[0] ? name : "Files"), *we = w_of(ext && ext[0] ? ext : "*.*");
    const wchar_t *parts[4] = { wn, we, L"All files", L"*.*" };
    for (int i = 0; i < 4; ++i) {
        size_t l = parts[i] ? wcslen(parts[i]) : 0;
        if (pos + l + 2 >= cap) break;
        if (l) memcpy(out + pos, parts[i], l * sizeof(wchar_t));
        pos += l;
        out[pos++] = 0;
    }
    out[pos] = 0;
    free(wn); free(we);
}

static int file_dialog(int save, const char *title, const char *start, const char *fname, const char *fext,
                       char *out, int out_size)
{
    wchar_t file[4096] = L"", dir[4096] = L"", filter[512];
    make_filter(filter, 512, fname, fext);
    if (start && start[0]) {
        wchar_t *ws = w_of(start);
        if (ws) {
            w_backslashes(ws);
            DWORD a = GetFileAttributesW(ws);
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) wcsncpy(dir, ws, 4095);
            else {
                wchar_t *bs = wcsrchr(ws, L'\\');
                if (bs) { *bs = 0; wcsncpy(dir, ws, 4095); wcsncpy(file, bs + 1, 4095); }
                else wcsncpy(file, ws, 4095);
            }
            free(ws);
        }
    }
    wchar_t *wt = w_of(title);
    wchar_t defext[32] = L"";
    if (fext && fext[0] == '*' && fext[1] == '.' && !strchr(fext, ';')) MultiByteToWideChar(CP_UTF8, 0, fext + 2, -1, defext, 32);
    OPENFILENAMEW of;
    memset(&of, 0, sizeof of);
    of.lStructSize = sizeof of;
    of.hwndOwner = owner();
    of.lpstrFilter = filter;
    of.lpstrFile = file;
    of.nMaxFile = 4096;
    of.lpstrInitialDir = dir[0] ? dir : NULL;
    of.lpstrTitle = wt;
    of.lpstrDefExt = defext[0] ? defext : NULL;
    of.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST));
    BOOL ok = save ? GetSaveFileNameW(&of) : GetOpenFileNameW(&of);
    free(wt);
    if (!ok) return 1;
    u8_of(file, out, out_size);
    qw_slashes(out);
    return 0;
}

int qw_dlg_open_file(const char *title, const char *start_dir, const char *filter_name, const char *filter_ext,
                     char *out, int out_size)
{
    return file_dialog(0, title, start_dir, filter_name, filter_ext, out, out_size);
}

int qw_dlg_save_file(const char *title, const char *start_path, const char *filter_name, const char *filter_ext,
                     char *out, int out_size)
{
    return file_dialog(1, title, start_path, filter_name, filter_ext, out, out_size);
}

static int CALLBACK browse_cb(HWND h, UINT msg, LPARAM lp, LPARAM data)
{
    (void)lp;
    if (msg == BFFM_INITIALIZED && data) SendMessageW(h, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}

int qw_dlg_folder(const char *title, const char *start_dir, char *out, int out_size)
{
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    wchar_t *wt = w_of(title), *ws = w_of(start_dir);
    if (ws) w_backslashes(ws);
    BROWSEINFOW bi;
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = owner();
    bi.lpszTitle = wt;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = browse_cb;
    bi.lParam = (LPARAM)ws;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    int r = 1;
    if (pidl) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) { u8_of(path, out, out_size); qw_slashes(out); r = 0; }
        CoTaskMemFree(pidl);
    }
    free(wt); free(ws);
    return r;
}

/* ---- a small text-input dialog, built in memory (Windows has no ready one) ---- */
typedef struct { const wchar_t *prompt, *initial; wchar_t result[1024]; } TextDlg;

static INT_PTR CALLBACK text_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    TextDlg *t = (TextDlg *)GetWindowLongPtrW(h, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(h, DWLP_USER, lp);
        t = (TextDlg *)lp;
        SetDlgItemTextW(h, 100, t->prompt ? t->prompt : L"");
        SetDlgItemTextW(h, 101, t->initial ? t->initial : L"");
        SendDlgItemMessageW(h, 101, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(h, 101));
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) { GetDlgItemTextW(h, 101, t->result, 1024); EndDialog(h, IDOK); return TRUE; }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(h, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

static WORD *align4(WORD *p) { return (WORD *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3); }

static WORD *put_wstr(WORD *p, const wchar_t *s) { while ((*p++ = (WORD)*s++) != 0) {} return p; }

static WORD *add_item(WORD *p, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t *text)
{
    p = align4(p);
    DLGITEMTEMPLATE *it = (DLGITEMTEMPLATE *)p;
    it->style = style | WS_CHILD | WS_VISIBLE;
    it->dwExtendedStyle = 0;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy; it->id = id;
    p = (WORD *)(it + 1);
    *p++ = 0xFFFF; *p++ = cls;              /* 0x80 button, 0x81 edit, 0x82 static */
    p = put_wstr(p, text);
    *p++ = 0;                               /* no creation data */
    return p;
}

int qw_dlg_text(const char *title, const char *prompt, const char *initial, char *out, int out_size)
{
    static WORD buf[1024];
    memset(buf, 0, sizeof buf);
    WORD *p = align4(buf);
    DLGTEMPLATE *dt = (DLGTEMPLATE *)p;
    dt->style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dt->cdit = 4;
    dt->x = 0; dt->y = 0; dt->cx = 240; dt->cy = 74;
    p = (WORD *)(dt + 1);
    *p++ = 0; *p++ = 0;                     /* no menu, default class */
    wchar_t *wt = w_of(title ? title : "Q-LiM");
    p = put_wstr(p, wt ? wt : L"Q-LiM");
    free(wt);
    *p++ = 9;                               /* font size */
    p = put_wstr(p, L"Segoe UI");
    p = add_item(p, SS_LEFT, 8, 8, 224, 10, 100, 0x0082, L"");
    p = add_item(p, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 8, 22, 224, 13, 101, 0x0081, L"");
    p = add_item(p, BS_DEFPUSHBUTTON | WS_TABSTOP, 128, 50, 50, 15, IDOK, 0x0080, L"OK");
    p = add_item(p, BS_PUSHBUTTON | WS_TABSTOP, 182, 50, 50, 15, IDCANCEL, 0x0080, L"Cancel");
    TextDlg t;
    memset(&t, 0, sizeof t);
    wchar_t *wp = w_of(prompt), *wi = w_of(initial);
    t.prompt = wp; t.initial = wi;
    INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(NULL), (DLGTEMPLATE *)align4(buf), owner(), text_proc, (LPARAM)&t);
    free(wp); free(wi);
    if (r != IDOK) return 1;
    u8_of(t.result, out, out_size);
    return 0;
}

/* ================= single instance ================= */

int qw_single_instance_claim(void)
{
    HANDLE m = CreateMutexW(NULL, FALSE, L"Local\\QLiM-single-instance");
    if (m && GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(m); return 0; }
    return 1;                               /* the handle stays open while we run */
}

void qw_raise_other_instance(void) { AllowSetForegroundWindow(ASFW_ANY); }

/* ================= crash diagnostics ================= */

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    char msg[512];
    void *addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : NULL;
    HMODULE mod = NULL;
    wchar_t wname[MAX_PATH] = L"?";
    char name[MAX_PATH * 3] = "?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &mod) && GetModuleFileNameW(mod, wname, MAX_PATH)) u8_of(wname, name, sizeof name);
    snprintf(msg, sizeof msg, "\n*** Q-LiM CRASH: exception 0x%08lx at %p (%s + 0x%llx) ***\n",
             ep && ep->ExceptionRecord ? (unsigned long)ep->ExceptionRecord->ExceptionCode : 0ul, addr, name,
             (unsigned long long)((char *)addr - (char *)mod));
    fputs(msg, stderr);
    fflush(stderr);
    /* also straight into the log file (the log thread may not get to it any more) */
    char lp[MAX_PATH * 3 + 64];
    snprintf(lp, sizeof lp, "%s/.config/qlim/qlim.log", g_home);
    FILE *f = qw_fopen(lp, "a");
    if (f) { fputs(msg, f); fclose(f); }
    return EXCEPTION_CONTINUE_SEARCH;
}

void qw_install_crash_handler(void) { SetUnhandledExceptionFilter(crash_filter); }

#endif /* _WIN32 */
