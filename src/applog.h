/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef APPLOG_H
#define APPLOG_H

/*
 * Real-time program log (for the indicator at the top right, as in Ardour).
 *
 * Everything the program writes (printf / stderr — also from libraries, e.g. ALSA) still goes out
 * to the console, but in parallel it is kept in memory (the last APPLOG_MAX lines) and every line is classified:
 *   information / warning / error — by keywords (ERROR, failed, LOST, WARNING, dry, xrun...).
 * The harmless ALSA "noise" at startup ("ALSA lib ...") counts as information.
 */

#include <stdbool.h>
#include <stddef.h>

#define APPLOG_MAX 2000

enum { APPLOG_INFO = 0, APPLOG_WARN = 1, APPLOG_ERR = 2 };

void     applog_init(void);              /* called as early as possible in main */
int      applog_orig_stderr(void);       /* the real console (for the crash handler); -1 if there is none */

int      applog_count(void);             /* how many lines are in memory */
/* i = 0 is the oldest. out receives the text; *level and *t (seconds since start) may be NULL. */
bool     applog_get(int i, char *out, size_t n, int *level, double *t);
unsigned applog_seq(void);               /* grows with every new line */

/* New warnings/errors since the last "seen" (opening the window / Clear). */
int      applog_new_warnings(void);
int      applog_new_errors(void);
void     applog_mark_seen(void);
void     applog_clear(void);             /* clears the lines in memory and the counters */

int      applog_classify(const char *line);   /* for tests */

#endif
