/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/*
 * Windows compatibility layer. Force-included (-include src/win/compat.h) into every C/C++ file of the Windows build.
 *
 * Q-LiM keeps all paths internally as UTF-8 with '/' separators (exactly as on Linux). The C runtime on Windows
 * opens files through the ANSI code page, so names with Cyrillic (or any non-Latin) characters would fail.
 * The wrappers below convert UTF-8 -> UTF-16 and call the wide-character Windows functions.
 *
 * Deliberately does NOT include <windows.h>: it defines names (DrawText, LoadImage, Rectangle, near, far ...)
 * that clash with Q-LiM's own. Only compat.c and the Windows-specific files include it.
 */
#ifndef QLIM_WIN_COMPAT_H
#define QLIM_WIN_COMPAT_H
#ifdef _WIN32

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

/* every system header that declares a function wrapped below is included HERE, before the macros */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <io.h>
#include <direct.h>
#include <dirent.h>
#include <time.h>
#include <limits.h>
#ifndef __cplusplus
#include <sndfile.h>
#endif

/* MinGW's PATH_MAX is 260 bytes; UTF-8 paths (Cyrillic = 2 bytes per letter) need more */
#undef PATH_MAX
#define PATH_MAX 4096

#ifdef __cplusplus
extern "C" {
#endif

/* ---- start-up: HOME, working folder, UTF-8 arguments ---- */
void qw_init(int *argc, char ***argv);
const char *qw_exe_dir(void);                 /* folder of qlim.exe, with '/' */
void qw_slashes(char *path);                  /* '\' -> '/' in place */

/* ---- files with UTF-8 names ---- */
FILE *qw_fopen(const char *path, const char *mode);
int   qw_stat(const char *path, struct stat *st);
int   qw_mkdir(const char *path, int mode);
int   qw_remove(const char *path);
int   qw_rename(const char *from, const char *to);   /* replaces an existing target, like POSIX */
int   qw_access(const char *path, int mode);
char *qw_realpath(const char *path, char *resolved);
int   qw_link(const char *from, const char *to);     /* hard link (NTFS) */
int   qw_fsync(int fd);

#ifndef __cplusplus            /* C++ files (midi.cpp, host_vst3.cpp) call the qw_* functions directly */
#define fopen(p, m)        qw_fopen((p), (m))
#define stat(p, s)         qw_stat((p), (s))
#define remove(p)          qw_remove(p)
#define rename(a, b)       qw_rename((a), (b))
#define access(p, m)       qw_access((p), (m))
#define realpath(p, r)     qw_realpath((p), (r))
#endif
/* safe in C++ too (no std:: functions with these names) */
#define fsync(fd)          qw_fsync(fd)
#define link(a, b)         qw_link((a), (b))
#define mkdir(p, m)        qw_mkdir((p), (m))
#define unlink(p)          qw_remove(p)
#define pipe(f)            _pipe((f), 65536, 0x8000 /* _O_BINARY */)

#ifndef F_OK
#define F_OK 0
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef X_OK
#define X_OK 0
#endif

/* ---- folders with UTF-8 names (replaces <dirent.h>) ---- */
struct qw_dirent { char d_name[1024]; };
typedef struct qw_dir QW_DIR;
QW_DIR           *qw_opendir(const char *path);
struct qw_dirent *qw_readdir(QW_DIR *d);
int               qw_closedir(QW_DIR *d);
#ifndef __cplusplus
#define DIR       QW_DIR
#define dirent    qw_dirent
#define opendir   qw_opendir
#define readdir   qw_readdir
#define closedir  qw_closedir
#endif

/* ---- libsndfile with UTF-8 names (sf_wchar_open) ---- */
void *qw_sf_open(const char *path, int mode, void *info);
#ifndef __cplusplus
#define sf_open(p, m, i)   qw_sf_open((p), (m), (i))
#endif

/* ---- process / misc ---- */
FILE *qw_tmpfile(void);                      /* temporary file in %TEMP% (deleted when closed), mode w+b */
int  qw_getpid(void);
int  qw_process_alive(int pid);               /* 1 = a process with this id is running */
int  qw_process_is_qlim(int pid);             /* 1 = that process is qlim.exe */
int  qw_setenv(const char *name, const char *value);
char *qw_getenv(const char *name);           /* UTF-8; "HOME" = the user's profile folder with '/' */
#ifndef __cplusplus
#define setenv(n, v, o)    qw_setenv((n), (v))
#define getenv(n)          qw_getenv(n)
#endif

/* run a program without a console window and wait for it; argv[0] is looked up in PATH. 0 = exit code 0 */
int  qw_run_wait(const char *const *argv, int hide_output);

/* run a program (no window), collect its stdout, kill it after timeout_s; malloc'ed output (NULL = could not start) */
char *qw_spawn_capture(const char *const *argv, double timeout_s, int *exit_code, int *timed_out);
void  qw_scan_child_prepare(void);            /* the "--plugin-scan" process: crash = exit at once, binary stdout */
const char *qw_known_folder(const char *which);   /* "common" / "programs" / "localappdata" (UTF-8, '/'); NULL = unknown */

/* ---- native dialogs (replace zenity). 0 = OK, 1 = cancelled. Results are UTF-8 with '/'. ---- */
int qw_dlg_open_file(const char *title, const char *start_dir, const char *filter_name, const char *filter_ext,
                     char *out, int out_size);
int qw_dlg_save_file(const char *title, const char *start_path, const char *filter_name, const char *filter_ext,
                     char *out, int out_size);
int qw_dlg_folder(const char *title, const char *start_dir, char *out, int out_size);
int qw_dlg_text(const char *title, const char *prompt, const char *initial, char *out, int out_size);

/* ---- single instance ---- */
int qw_single_instance_claim(void);           /* 1 = we are the first Q-LiM; 0 = another one is running */
void qw_raise_other_instance(void);           /* best effort: bring the running Q-LiM to the front */

/* ---- crash / freeze diagnostics ---- */
void qw_install_crash_handler(void);

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */
#endif
