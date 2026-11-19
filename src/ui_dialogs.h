/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef UI_DIALOGS_H
#define UI_DIALOGS_H

/* Dialogs: Routing (a matrix as in Ardour), Shortcuts, "Save changes?" on close.
 * They are drawn over the whole screen, like the other dialogs in ui.c. */

#include "session.h"
#include <stdbool.h>

bool dialogs_any_open(void);
void dialogs_draw(Session *s, int screen_w, int screen_h);   /* called only if dialogs_any_open() */

/* 1.4.24: a surround (multichannel) file */
void ui_open_mc(Session *s, int ch, int sc, bool import);
void dialogs_poll(Session *s);   /* every frame: asks about surround files just added */

/* 2.0: the Speaker Map - surround signals wired to any output */
void ui_open_speaker_map(Session *s, int ch, int sc);

#endif
