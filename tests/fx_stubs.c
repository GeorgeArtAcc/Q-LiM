/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* stand-ins for ui.c / plugins.c / sdlgfx.c for the plugin tests */
#include "../src/plugins.h"
#include <stdio.h>
void ui_flash_message(const char *m) { (void)m; }
bool plugins_resolve(const char *fmt, const char *p, const char *name, char *out, int sz) { (void)fmt; (void)name; snprintf(out, (size_t)sz, "%s", p); return true; }
bool plugins_scanning(int *a, int *b, char *c, int d) { (void)a; (void)b; (void)c; (void)d; return false; }
int plugins_generation(void) { return 0; }
void *GfxSdlWindow(void) { return NULL; }
bool plugins_is_instrument(const char *fmt, const char *id) { (void)fmt; (void)id; return false; }
