/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Windows: the few dlfcn functions the plugin hosts use, on top of LoadLibrary (implemented in src/win/compat.c).
 * Paths are UTF-8 (with '/' or '\'); the folder of the DLL is searched for the DLLs it needs. */
#ifndef QLIM_WIN_DLFCN_H
#define QLIM_WIN_DLFCN_H
#ifdef __cplusplus
extern "C" {
#endif
#define RTLD_LAZY   1
#define RTLD_NOW    2
#define RTLD_LOCAL  0
#define RTLD_GLOBAL 0x100
void *dlopen(const char *path, int mode);
void *dlsym(void *lib, const char *name);
int   dlclose(void *lib);
char *dlerror(void);
#ifdef __cplusplus
}
#endif
#endif
