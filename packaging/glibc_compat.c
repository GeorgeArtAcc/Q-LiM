/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Linux packages: glibc 2.38+ headers turn strtol/sscanf ... into new symbols (__isoc23_*) that older systems
 * (Ubuntu 22.04, Debian 12, Mint 21, RHEL 9 ...) do not have. These forward them to the classic functions,
 * so the same qlim runs on all of them. (Compiled WITHOUT _GNU_SOURCE, so the calls below are the old ones.) */
#define _POSIX_C_SOURCE 200809L
/* 2.0.0: everything here stays INSIDE qlim (compiled with -fvisibility=hidden, see Makefile.pkg). Exported, a plugin's call to hypotf/fmod went to the wrapper
 * below, and the wrapper's own call to the old libm version came back to the wrapper again - an endless loop
 * (Vitalium and other JUCE plugins froze while opening their window). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

long               __isoc23_strtol(const char *s, char **e, int b)   { return strtol(s, e, b); }
unsigned long      __isoc23_strtoul(const char *s, char **e, int b)  { return strtoul(s, e, b); }
long long          __isoc23_strtoll(const char *s, char **e, int b)  { return strtoll(s, e, b); }
unsigned long long __isoc23_strtoull(const char *s, char **e, int b) { return strtoull(s, e, b); }
intmax_t           __isoc23_strtoimax(const char *s, char **e, int b) { return strtoimax(s, e, b); }
uintmax_t          __isoc23_strtoumax(const char *s, char **e, int b) { return strtoumax(s, e, b); }
#include <wchar.h>
long               __isoc23_wcstol(const wchar_t *s, wchar_t **e, int b)   { return wcstol(s, e, b); }
unsigned long      __isoc23_wcstoul(const wchar_t *s, wchar_t **e, int b)  { return wcstoul(s, e, b); }
long long          __isoc23_wcstoll(const wchar_t *s, wchar_t **e, int b)  { return wcstoll(s, e, b); }
unsigned long long __isoc23_wcstoull(const wchar_t *s, wchar_t **e, int b) { return wcstoull(s, e, b); }

int __isoc23_vsscanf(const char *s, const char *f, va_list ap) { return vsscanf(s, f, ap); }
int __isoc23_vfscanf(FILE *fp, const char *f, va_list ap)      { return vfscanf(fp, f, ap); }
int __isoc23_vscanf(const char *f, va_list ap)                 { return vscanf(f, ap); }
int __isoc23_sscanf(const char *s, const char *f, ...) { va_list ap; va_start(ap, f); int r = vsscanf(s, f, ap); va_end(ap); return r; }
int __isoc23_fscanf(FILE *fp, const char *f, ...)      { va_list ap; va_start(ap, f); int r = vfscanf(fp, f, ap); va_end(ap); return r; }
int __isoc23_scanf(const char *f, ...)                 { va_list ap; va_start(ap, f); int r = vscanf(f, ap); va_end(ap); return r; }

/* ---- symbols that newer glibc versions added or re-versioned (baseline: glibc 2.34) ---- */
#include <string.h>
#include <wchar.h>
#include <stdint.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

/* the old versions of fmod/fmodf/hypotf (2.38 / 2.35 made new ones) */
double __qlim_fmod_old(double, double);
float  __qlim_fmodf_old(float, float);
float  __qlim_hypotf_old(float, float);
#ifdef __i386__                                   /* 32-bit build (T=gnu32): the i386 base version */
__asm__(".symver __qlim_fmod_old,fmod@GLIBC_2.0");
__asm__(".symver __qlim_fmodf_old,fmodf@GLIBC_2.0");
__asm__(".symver __qlim_hypotf_old,hypotf@GLIBC_2.0");
#else
__asm__(".symver __qlim_fmod_old,fmod@GLIBC_2.2.5");
__asm__(".symver __qlim_fmodf_old,fmodf@GLIBC_2.2.5");
__asm__(".symver __qlim_hypotf_old,hypotf@GLIBC_2.2.5");
#endif
double fmod(double x, double y)  { return __qlim_fmod_old(x, y); }
float  fmodf(float x, float y)   { return __qlim_fmodf_old(x, y); }
float  hypotf(float x, float y)  { return __qlim_hypotf_old(x, y); }

/* strlcpy & co. (glibc 2.38) - BSD semantics */
size_t strlcpy(char *d, const char *s, size_t n)
{
    size_t l = strlen(s);
    if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; }
    return l;
}
size_t strlcat(char *d, const char *s, size_t n)
{
    size_t dl = strnlen(d, n);
    if (dl == n) return n + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}
size_t wcslcpy(wchar_t *d, const wchar_t *s, size_t n)
{
    size_t l = wcslen(s);
    if (n) { size_t c = l < n - 1 ? l : n - 1; wmemcpy(d, s, c); d[c] = 0; }
    return l;
}
size_t wcslcat(wchar_t *d, const wchar_t *s, size_t n)
{
    size_t dl = wcsnlen(d, n);
    if (dl == n) return n + wcslen(s);
    return dl + wcslcpy(d + dl, s, n - dl);
}

/* arc4random (glibc 2.36; used by std::random_device) */
static int qlim_urandom(void *buf, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < n) { ssize_t r = read(fd, (char *)buf + got, n - got); if (r <= 0) break; got += (size_t)r; }
    close(fd);
    return got == n ? 0 : -1;
}
uint32_t arc4random(void) { uint32_t v = 0; qlim_urandom(&v, sizeof v); return v; }
void arc4random_buf(void *b, size_t n) { qlim_urandom(b, n); }
uint32_t arc4random_uniform(uint32_t up)
{
    if (up < 2) return 0;
    uint32_t min = -up % up, r;
    do r = arc4random(); while (r < min);
    return r % up;
}

#ifdef __i386__
/* ---- 32-bit build (T=gnu32, baseline glibc 2.17): functions that newer glibc headers call directly, but that old
 * glibc only has under other names (stat & co. were __xstat* until 2.33, fcntl64 came in 2.28) ---- */
#include <sys/stat.h>
#include <locale.h>
struct stat;
struct stat64;
int __xstat(int, const char *, struct stat *);
int __xstat64(int, const char *, struct stat64 *);
int __lxstat(int, const char *, struct stat *);
int __lxstat64(int, const char *, struct stat64 *);
int __fxstat(int, int, struct stat *);
int __fxstat64(int, int, struct stat64 *);
#define QLIM_STAT_VER 3                                   /* _STAT_VER_LINUX on i386 */
int stat(const char *p, struct stat *b)        { return __xstat(QLIM_STAT_VER, p, b); }
int lstat(const char *p, struct stat *b)       { return __lxstat(QLIM_STAT_VER, p, b); }
int fstat(int fd, struct stat *b)              { return __fxstat(QLIM_STAT_VER, fd, b); }
int stat64(const char *p, struct stat64 *b)    { return __xstat64(QLIM_STAT_VER, p, b); }
int lstat64(const char *p, struct stat64 *b)   { return __lxstat64(QLIM_STAT_VER, p, b); }
int fstat64(int fd, struct stat64 *b)          { return __fxstat64(QLIM_STAT_VER, fd, b); }
int fcntl64(int fd, int cmd, ...)
{
    va_list ap; va_start(ap, cmd); void *arg = va_arg(ap, void *); va_end(ap);
    return fcntl(fd, cmd, arg);
}
long long strtoll_l(const char *, char **, int, locale_t);
unsigned long long strtoull_l(const char *, char **, int, locale_t);
long long __isoc23_strtoll_l(const char *s, char **e, int b, locale_t l)            { return strtoll_l(s, e, b, l); }
unsigned long long __isoc23_strtoull_l(const char *s, char **e, int b, locale_t l)  { return strtoull_l(s, e, b, l); }
#endif
