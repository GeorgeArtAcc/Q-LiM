/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/*
 * 32-bit Windows build only (Makefile.win32): lets qlim.exe start on Windows XP.
 *
 * The C++ runtime (libc++, libunwind, winpthreads) calls a few kernel32 functions that only exist since Vista / 7
 * (slim locks, condition variables, one-time init, fiber-local storage ...). A program that imports a function the
 * system does not have does not start at all. So this file defines the import slots ("__imp__Name@N") itself:
 * on the first call each slot looks the real function up; if Windows has it, the slot points to it from then on,
 * otherwise to a simple replacement that works on XP. On Vista and newer the real functions are used.
 */
#ifdef QLIM_XP_SHIM
#undef _WIN32_WINNT
#undef WINVER
#define _WIN32_WINNT 0x0601      /* the declarations of the Vista/7 functions and types */
#define WINVER 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static FARPROC xp_real(const char *dll, const char *name)
{
    /* test switch: QLIM_FORCE_XP=1 uses the XP replacements even on newer Windows (or Wine) */
    static int force = -1;
    if (force < 0) { char v[4]; force = GetEnvironmentVariableA("QLIM_FORCE_XP", v, sizeof v) > 0 && v[0] == '1'; }
    if (force && (!lstrcmpiA(dll, "kernel32.dll") || !lstrcmpiA(dll, "msvcrt.dll"))) return NULL;
    HMODULE h = GetModuleHandleA(dll);
    if (!h) h = LoadLibraryA(dll);
    return h ? GetProcAddress(h, name) : NULL;
}

/* one slot: __imp__<name>@<bytes> = first-call resolver */
#define SLOT(ret, name, bytes, params, args)                                                           \
    static ret WINAPI xp_fb_##name params;                                                             \
    static ret WINAPI xp_first_##name params;                                                          \
    ret (WINAPI *xp_slot_##name) params __asm__("__imp__" #name "@" #bytes) = xp_first_##name;        \
    static ret WINAPI xp_first_##name params {                                                         \
        FARPROC xp_p_ = xp_real("kernel32.dll", #name);                                                \
        xp_slot_##name = xp_p_ ? (ret (WINAPI *) params)(void *)xp_p_ : xp_fb_##name;                 \
        return xp_slot_##name args;                                                                    \
    }                                                                                                  \
    /* the plain call stub too (code compiled without dllimport calls _Name@N directly) */            \
    ret WINAPI xp_thunk_##name params __asm__("_" #name "@" #bytes);                                   \
    ret WINAPI xp_thunk_##name params { return xp_slot_##name args; }

/* ---- slim reader/writer locks: a pointer-sized word, 0 = free (shared mode is taken exclusively here) ---- */
static void xp_lock(volatile LONG *w)
{
    for (int spin = 0; InterlockedCompareExchange(w, 1, 0) != 0; ++spin)
        if (spin < 64) YieldProcessor(); else Sleep(spin < 200 ? 0 : 1);
}
SLOT(void, AcquireSRWLockExclusive, 4, (PSRWLOCK l), (l))
SLOT(void, AcquireSRWLockShared, 4, (PSRWLOCK l), (l))
SLOT(void, ReleaseSRWLockExclusive, 4, (PSRWLOCK l), (l))
SLOT(void, ReleaseSRWLockShared, 4, (PSRWLOCK l), (l))
SLOT(BOOLEAN, TryAcquireSRWLockExclusive, 4, (PSRWLOCK l), (l))
SLOT(BOOLEAN, TryAcquireSRWLockShared, 4, (PSRWLOCK l), (l))
SLOT(void, InitializeSRWLock, 4, (PSRWLOCK l), (l))
static void WINAPI xp_fb_AcquireSRWLockExclusive(PSRWLOCK l) { xp_lock((volatile LONG *)l); }
static void WINAPI xp_fb_AcquireSRWLockShared(PSRWLOCK l) { xp_lock((volatile LONG *)l); }
static void WINAPI xp_fb_ReleaseSRWLockExclusive(PSRWLOCK l) { InterlockedExchange((volatile LONG *)l, 0); }
static void WINAPI xp_fb_ReleaseSRWLockShared(PSRWLOCK l) { InterlockedExchange((volatile LONG *)l, 0); }
static BOOLEAN WINAPI xp_fb_TryAcquireSRWLockExclusive(PSRWLOCK l) { return InterlockedCompareExchange((volatile LONG *)l, 1, 0) == 0; }
static BOOLEAN WINAPI xp_fb_TryAcquireSRWLockShared(PSRWLOCK l) { return InterlockedCompareExchange((volatile LONG *)l, 1, 0) == 0; }
static void WINAPI xp_fb_InitializeSRWLock(PSRWLOCK l) { *(volatile LONG *)l = 0; }

/* ---- condition variables: waiters poll every millisecond (wake-ups without a signal are allowed by the API) ---- */
static int xp_cv_wait(DWORD ms)
{
    static LONG fine;            /* XP sleeps in ~15 ms steps unless asked for 1 ms timer resolution (once) */
    if (!fine && InterlockedExchange(&fine, 1) == 0) {
        typedef UINT (WINAPI *TBP)(UINT);
        TBP f = (TBP)(void *)xp_real("winmm.dll", "timeBeginPeriod");
        if (f) f(1);
    }
    Sleep(ms == 0 ? 0 : 1);
    return ms == INFINITE || ms > 1;     /* TRUE = "maybe signalled"; with a short time-out report the time-out */
}
SLOT(BOOL, SleepConditionVariableSRW, 16, (PCONDITION_VARIABLE c, PSRWLOCK l, DWORD ms, ULONG f), (c, l, ms, f))
SLOT(BOOL, SleepConditionVariableCS, 12, (PCONDITION_VARIABLE c, PCRITICAL_SECTION cs, DWORD ms), (c, cs, ms))
SLOT(void, WakeConditionVariable, 4, (PCONDITION_VARIABLE c), (c))
SLOT(void, WakeAllConditionVariable, 4, (PCONDITION_VARIABLE c), (c))
SLOT(void, InitializeConditionVariable, 4, (PCONDITION_VARIABLE c), (c))
static BOOL WINAPI xp_fb_SleepConditionVariableSRW(PCONDITION_VARIABLE c, PSRWLOCK l, DWORD ms, ULONG f)
{
    xp_fb_ReleaseSRWLockExclusive(l);
    int r = xp_cv_wait(ms);
    xp_fb_AcquireSRWLockExclusive(l);
    if (!r) SetLastError(ERROR_TIMEOUT);
    return r;
}
static BOOL WINAPI xp_fb_SleepConditionVariableCS(PCONDITION_VARIABLE c, PCRITICAL_SECTION cs, DWORD ms)
{
    LeaveCriticalSection(cs);
    int r = xp_cv_wait(ms);
    EnterCriticalSection(cs);
    if (!r) SetLastError(ERROR_TIMEOUT);
    return r;
}
static void WINAPI xp_fb_WakeConditionVariable(PCONDITION_VARIABLE c) {}
static void WINAPI xp_fb_WakeAllConditionVariable(PCONDITION_VARIABLE c) {}
static void WINAPI xp_fb_InitializeConditionVariable(PCONDITION_VARIABLE c) { c->Ptr = NULL; }

/* ---- one-time initialisation: 0 = not yet, 1 = running, 2 = done ---- */
SLOT(BOOL, InitOnceExecuteOnce, 16, (PINIT_ONCE o, PINIT_ONCE_FN fn, PVOID p, LPVOID *ctx), (o, fn, p, ctx))
static BOOL WINAPI xp_fb_InitOnceExecuteOnce(PINIT_ONCE o, PINIT_ONCE_FN fn, PVOID p, LPVOID *ctx)
{
    volatile LONG *w = (volatile LONG *)o;
    if (*w == 2) return TRUE;
    if (InterlockedCompareExchange(w, 1, 0) == 0) {
        BOOL ok = fn(o, p, ctx);
        InterlockedExchange(w, ok ? 2 : 0);
        return ok;
    }
    while (*w == 1) Sleep(0);
    return *w == 2;
}

/* ---- fiber-local storage -> thread-local storage (no clean-up callback on XP) ---- */
SLOT(DWORD, FlsAlloc, 4, (PFLS_CALLBACK_FUNCTION cb), (cb))
SLOT(PVOID, FlsGetValue, 4, (DWORD i), (i))
SLOT(BOOL, FlsSetValue, 8, (DWORD i, PVOID v), (i, v))
SLOT(BOOL, FlsFree, 4, (DWORD i), (i))
static DWORD WINAPI xp_fb_FlsAlloc(PFLS_CALLBACK_FUNCTION cb) { return TlsAlloc(); }
static PVOID WINAPI xp_fb_FlsGetValue(DWORD i) { return TlsGetValue(i); }
static BOOL WINAPI xp_fb_FlsSetValue(DWORD i, PVOID v) { return TlsSetValue(i, v); }
static BOOL WINAPI xp_fb_FlsFree(DWORD i) { return TlsFree(i); }

/* ---- misc ---- */
SLOT(ULONGLONG, GetTickCount64, 0, (void), ())
static ULONGLONG WINAPI xp_fb_GetTickCount64(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return f.QuadPart ? (ULONGLONG)(c.QuadPart / (f.QuadPart / 1000 ? f.QuadPart / 1000 : 1)) : GetTickCount();
}
SLOT(DWORD, GetThreadId, 4, (HANDLE t), (t))
static DWORD WINAPI xp_fb_GetThreadId(HANDLE t)
{
    typedef struct { LONG ExitStatus; PVOID Teb; DWORD_PTR Pid, Tid; ULONG_PTR Affinity; LONG Prio, Base; } TBI;
    typedef LONG (WINAPI *NQIT)(HANDLE, int, PVOID, ULONG, PULONG);
    NQIT q = (NQIT)(void *)xp_real("ntdll.dll", "NtQueryInformationThread");
    TBI b;
    if (q && q(t, 0, &b, sizeof b, NULL) == 0) return (DWORD)b.Tid;
    return t == GetCurrentThread() ? GetCurrentThreadId() : 0;
}
SLOT(BOOL, K32EnumProcessModules, 16, (HANDLE p, HMODULE *m, DWORD cb, LPDWORD need), (p, m, cb, need))
static BOOL WINAPI xp_fb_K32EnumProcessModules(HANDLE p, HMODULE *m, DWORD cb, LPDWORD need)
{
    typedef BOOL (WINAPI *EPM)(HANDLE, HMODULE *, DWORD, LPDWORD);
    EPM f = (EPM)(void *)xp_real("psapi.dll", "EnumProcessModules");
    if (f) return f(p, m, cb, need);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}
SLOT(BOOL, QueryFullProcessImageNameW, 16, (HANDLE p, DWORD fl, LPWSTR name, PDWORD n), (p, fl, name, n))
static BOOL WINAPI xp_fb_QueryFullProcessImageNameW(HANDLE p, DWORD fl, LPWSTR name, PDWORD n)
{
    typedef DWORD (WINAPI *GPIFN)(HANDLE, LPWSTR, DWORD);
    GPIFN f = (GPIFN)(void *)xp_real("psapi.dll", "GetProcessImageFileNameW");   /* device path; the file name is the same */
    DWORD r = f ? f(p, name, *n) : 0;
    if (!r) return FALSE;
    *n = r;
    return TRUE;
}

/* ---- file functions that only the C++ <filesystem> parts use (never called by Q-LiM on XP) ---- */
SLOT(DWORD, GetFinalPathNameByHandleW, 16, (HANDLE h, LPWSTR b, DWORD n, DWORD f), (h, b, n, f))
SLOT(BOOLEAN, CreateSymbolicLinkW, 12, (LPCWSTR a, LPCWSTR b, DWORD f), (a, b, f))
SLOT(BOOL, GetFileInformationByHandleEx, 16, (HANDLE h, FILE_INFO_BY_HANDLE_CLASS c, LPVOID b, DWORD n), (h, c, b, n))
SLOT(BOOL, SetFileInformationByHandle, 16, (HANDLE h, FILE_INFO_BY_HANDLE_CLASS c, LPVOID b, DWORD n), (h, c, b, n))
static DWORD WINAPI xp_fb_GetFinalPathNameByHandleW(HANDLE h, LPWSTR b, DWORD n, DWORD f) { SetLastError(ERROR_CALL_NOT_IMPLEMENTED); return 0; }
static BOOLEAN WINAPI xp_fb_CreateSymbolicLinkW(LPCWSTR a, LPCWSTR b, DWORD f) { SetLastError(ERROR_CALL_NOT_IMPLEMENTED); return FALSE; }
static BOOL WINAPI xp_fb_GetFileInformationByHandleEx(HANDLE h, FILE_INFO_BY_HANDLE_CLASS c, LPVOID b, DWORD n) { SetLastError(ERROR_CALL_NOT_IMPLEMENTED); return FALSE; }
static BOOL WINAPI xp_fb_SetFileInformationByHandle(HANDLE h, FILE_INFO_BY_HANDLE_CLASS c, LPVOID b, DWORD n) { SetLastError(ERROR_CALL_NOT_IMPLEMENTED); return FALSE; }

/* ---- msvcrt.dll: the locale (_l) functions the C++ runtime uses exist only since Vista -> the plain ones ---- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>
#include <stdarg.h>
#define CSLOT(ret, name, params, args)                                                                 \
    static ret __cdecl xp_fb_##name params;                                                            \
    static ret __cdecl xp_first_##name params;                                                         \
    ret (__cdecl *xp_slot_##name) params __asm__("__imp__" #name) = xp_first_##name;                   \
    static ret __cdecl xp_first_##name params {                                                        \
        FARPROC xp_p_ = xp_real("msvcrt.dll", #name);                                                  \
        xp_slot_##name = xp_p_ ? (ret (__cdecl *) params)(void *)xp_p_ : xp_fb_##name;                \
        return xp_slot_##name args;                                                                    \
    }                                                                                                  \
    ret __cdecl xp_thunk_##name params __asm__("_" #name);                                             \
    ret __cdecl xp_thunk_##name params { return xp_slot_##name args; }
#define CTYPE_L(name, plain, T) CSLOT(int, name, (T c, void *l), (c, l)) \
    static int __cdecl xp_fb_##name(T c, void *l) { return plain(c); }
CSLOT(int, _isctype_l, (int c, int m, void *l), (c, m, l))
static int __cdecl xp_fb__isctype_l(int c, int m, void *l) { return _isctype(c, m); }
CTYPE_L(_iswalpha_l, iswalpha, wint_t)
CTYPE_L(_iswcntrl_l, iswcntrl, wint_t)
CTYPE_L(_iswdigit_l, iswdigit, wint_t)
CTYPE_L(_iswlower_l, iswlower, wint_t)
CTYPE_L(_iswprint_l, iswprint, wint_t)
CTYPE_L(_iswpunct_l, iswpunct, wint_t)
CTYPE_L(_iswspace_l, iswspace, wint_t)
CTYPE_L(_iswupper_l, iswupper, wint_t)
CTYPE_L(_iswxdigit_l, iswxdigit, wint_t)
CTYPE_L(_tolower_l, tolower, int)
CTYPE_L(_toupper_l, toupper, int)
CSLOT(wint_t, _towlower_l, (wint_t c, void *l), (c, l))
static wint_t __cdecl xp_fb__towlower_l(wint_t c, void *l) { return towlower(c); }
CSLOT(wint_t, _towupper_l, (wint_t c, void *l), (c, l))
static wint_t __cdecl xp_fb__towupper_l(wint_t c, void *l) { return towupper(c); }
CSLOT(int, _mbtowc_l, (wchar_t *w, const char *s, size_t n, void *l), (w, s, n, l))
static int __cdecl xp_fb__mbtowc_l(wchar_t *w, const char *s, size_t n, void *l) { return mbtowc(w, s, n); }
CSLOT(int, _strcoll_l, (const char *a, const char *b, void *l), (a, b, l))
static int __cdecl xp_fb__strcoll_l(const char *a, const char *b, void *l) { return strcoll(a, b); }
CSLOT(int, _wcscoll_l, (const wchar_t *a, const wchar_t *b, void *l), (a, b, l))
static int __cdecl xp_fb__wcscoll_l(const wchar_t *a, const wchar_t *b, void *l) { return wcscoll(a, b); }
CSLOT(size_t, _strxfrm_l, (char *d, const char *s, size_t n, void *l), (d, s, n, l))
static size_t __cdecl xp_fb__strxfrm_l(char *d, const char *s, size_t n, void *l) { return strxfrm(d, s, n); }
CSLOT(size_t, _wcsxfrm_l, (wchar_t *d, const wchar_t *s, size_t n, void *l), (d, s, n, l))
static size_t __cdecl xp_fb__wcsxfrm_l(wchar_t *d, const wchar_t *s, size_t n, void *l) { return wcsxfrm(d, s, n); }
CSLOT(double, _strtod_l, (const char *s, char **e, void *l), (s, e, l))
static double __cdecl xp_fb__strtod_l(const char *s, char **e, void *l) { return strtod(s, e); }
CSLOT(__int64, _strtoi64_l, (const char *s, char **e, int b, void *l), (s, e, b, l))
static __int64 __cdecl xp_fb__strtoi64_l(const char *s, char **e, int b, void *l) { return _strtoi64(s, e, b); }
CSLOT(unsigned __int64, _strtoui64_l, (const char *s, char **e, int b, void *l), (s, e, b, l))
static unsigned __int64 __cdecl xp_fb__strtoui64_l(const char *s, char **e, int b, void *l) { return _strtoui64(s, e, b); }
CSLOT(int, wcrtomb_s, (size_t *r, char *d, size_t n, wchar_t w, mbstate_t *st), (r, d, n, w, st))
static int __cdecl xp_fb_wcrtomb_s(size_t *r, char *d, size_t n, wchar_t w, mbstate_t *st)
{
    char tmp[8];
    size_t k = wcrtomb(tmp, w, st);
    if (k == (size_t)-1) { if (r) *r = (size_t)-1; return 42; /* EILSEQ */ }
    if (d && k > n) return 34; /* ERANGE */
    if (d) memcpy(d, tmp, k);
    if (r) *r = k;
    return 0;
}
CSLOT(int, _wputenv_s, (const wchar_t *n, const wchar_t *v), (n, v))
static int __cdecl xp_fb__wputenv_s(const wchar_t *n, const wchar_t *v)
{
    size_t a = wcslen(n), b = wcslen(v);
    wchar_t *s = (wchar_t *)malloc((a + b + 2) * sizeof(wchar_t));   /* _wputenv keeps the pointer */
    if (!s) return 12;
    memcpy(s, n, a * sizeof(wchar_t)); s[a] = L'='; memcpy(s + a + 1, v, (b + 1) * sizeof(wchar_t));
    return _wputenv(s) == 0 ? 0 : 22;
}
/* variadic: forwarded by hand */
int __cdecl __mingw_vsscanf(const char *, const char *, va_list);
int __cdecl xp_thunk__sscanf_l(const char *s, const char *f, void *l, ...) __asm__("__sscanf_l");
int __cdecl xp_thunk__sscanf_l(const char *s, const char *f, void *l, ...)
{
    va_list ap;
    va_start(ap, l);
    int r = __mingw_vsscanf(s, f, ap);      /* (msvcrt has no _vsscanf_l; the locale is ignored) */
    va_end(ap);
    return r;
}
int (__cdecl *xp_slot__sscanf_l)(const char *, const char *, void *, ...) __asm__("__imp___sscanf_l") = xp_thunk__sscanf_l;

#endif
