/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 32-bit glibc build (libc++ instead of libstdc++): the one libstdc++ helper the VST3 SDK's funknown.cpp uses on Linux.
 * (With libc++ the SDK would take <stdatomic.h>, whose C macros clash with <atomic> in C++17.) */
#pragma once
#ifdef __cplusplus
namespace __gnu_cxx {
inline void __atomic_add(volatile int *p, int d) { __atomic_fetch_add(p, d, __ATOMIC_ACQ_REL); }
inline int __exchange_and_add(volatile int *p, int d) { return __atomic_fetch_add(p, d, __ATOMIC_ACQ_REL); }
}
#endif
