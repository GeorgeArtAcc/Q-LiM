/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef UI_DIALOGS_H
#define UI_DIALOGS_H

/* Dialogs: Routing (a matrix as in Ardour), Shortcuts, "Save changes?" on close.
 * They are drawn over the whole screen, like the other dialogs in ui.c. */

#include "session.h"
#include <stdbool.h>

bool dialogs_any_open(void);
void dialogs_draw(Session *s, int screen_w, int screen_h);   /* called only if dialogs_any_open() */

#endif
