/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Included after <windows.h>. The 32-bit build compiles with the XP headers (_WIN32_WINNT 0x0501), which do not
 * declare the few Vista functions Q-LiM calls; xpshim.c provides them on XP (see there). */
#ifndef QLIM_XPSHIM_H
#define QLIM_XPSHIM_H
#if defined(_WIN32) && _WIN32_WINNT < 0x0600
__declspec(dllimport) ULONGLONG WINAPI GetTickCount64(void);
__declspec(dllimport) BOOL WINAPI QueryFullProcessImageNameW(HANDLE, DWORD, LPWSTR, PDWORD);
#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif
#endif
/* XP does not know PROCESS_QUERY_LIMITED_INFORMATION: then ask with the older right */
static inline HANDLE qw_open_process_query(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    return h ? h : OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
}
#endif
