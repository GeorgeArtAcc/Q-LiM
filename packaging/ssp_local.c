/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 32-bit musl build: some bundled libraries (Opus ...) turn on -fstack-protector themselves. On i386 that calls
 * the hidden helper __stack_chk_fail_local, which a dynamically linked musl program must bring itself. */
void __stack_chk_fail(void);
__attribute__((visibility("hidden"))) void __stack_chk_fail_local(void) { __stack_chk_fail(); }
