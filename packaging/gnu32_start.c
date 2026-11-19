/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 32-bit glibc build: the program entry point (replaces glibc's crt1.o, linked by packaging/gnu32-cc.sh).
 * The crt1.o of a new glibc calls __libc_start_main@GLIBC_2.34, which old systems do not have. This one calls the
 * original GLIBC_2.0 version and passes its own "init" (runs the constructors), as programs built on old glibc did;
 * new glibc versions still accept that. Non-PIE (-no-pie). */
typedef void (*fn_t)(void);
extern fn_t __preinit_array_start[] __attribute__((weak, visibility("hidden")));
extern fn_t __preinit_array_end[] __attribute__((weak, visibility("hidden")));
extern fn_t __init_array_start[] __attribute__((weak, visibility("hidden")));
extern fn_t __init_array_end[] __attribute__((weak, visibility("hidden")));
extern void _init(void);

void qlim_csu_init(int argc, char **argv, char **envp)
{
    for (fn_t *f = __preinit_array_start; f < __preinit_array_end; ++f) ((void (*)(int, char **, char **))*f)(argc, argv, envp);
    _init();
    for (fn_t *f = __init_array_start; f < __init_array_end; ++f) ((void (*)(int, char **, char **))*f)(argc, argv, envp);
}
void qlim_csu_fini(void) {}

__asm__(".symver __libc_start_main,__libc_start_main@GLIBC_2.0");
__asm__(
    ".text\n"
    ".globl _start\n"
    ".type _start,@function\n"
    "_start:\n"
    "    xorl %ebp, %ebp\n"
    "    popl %esi\n"                 /* argc */
    "    movl %esp, %ecx\n"           /* argv */
    "    andl $-16, %esp\n"
    "    pushl %eax\n"                /* padding: 16-byte alignment at the call */
    "    pushl %esp\n"                /* stack_end */
    "    pushl %edx\n"                /* rtld_fini (from the dynamic loader) */
    "    pushl $qlim_csu_fini\n"
    "    pushl $qlim_csu_init\n"
    "    pushl %ecx\n"
    "    pushl %esi\n"
    "    pushl $main\n"
    "    call __libc_start_main\n"
    "    hlt\n"
);
int __libc_start_main();
/* keep a reference so the .symver above applies */
void *qlim_start_ref = (void *)__libc_start_main;
