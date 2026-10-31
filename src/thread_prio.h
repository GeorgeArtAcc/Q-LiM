/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 1.3.7: thread priorities, so the sound comes first when the computer is busy.
 *  thread_prio_low():  work nobody waits for (drawing the waveform overview, finding the tempo) - "nice 10"
 *  thread_prio_high(): what feeds the sound (reading from the disk, WARP) - only on Windows (Linux does not let a
 *                      normal program raise a thread; there the low ones step aside instead) */
#ifndef QLIM_THREAD_PRIO_H
#define QLIM_THREAD_PRIO_H
#ifdef _WIN32
#include <windows.h>
static inline void thread_prio_low(void)  { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }
static inline void thread_prio_high(void) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); }
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
extern long syscall(long number, ...);  /* declared by unistd.h only with _DEFAULT_SOURCE */
static inline void thread_prio_low(void)  { setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10); }
static inline void thread_prio_high(void) { }
#endif
#endif
