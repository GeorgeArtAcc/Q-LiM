/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Tests without plugins: the mixer calls qplug_process only for channels with plugins (there are none here),
 * and project.c asks for the plugin settings on save/load. */
#include "../src/plughost.h"
#include "../src/fxchain.h"
void qplug_process(QPlug *p, float *l, float *r, unsigned n) { (void)p; (void)l; (void)r; (void)n; }
int fxchain_state_for_save(Session *s, int ch, int k, void **data, size_t *len) { (void)s; (void)ch; (void)k; *data = NULL; *len = 0; return -1; }
int fxstate_new(void) { return 0; }
void fxstate_append(int ref, const void *data, size_t len) { (void)ref; (void)data; (void)len; }
