/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef PROJECT_H
#define PROJECT_H

#include "session.h"
#include <stdbool.h>
#include <stddef.h>

void project_get_base_dir(char *out_path, size_t size);

int project_save(Session *s, const char *path);
int project_load(Session *s, const char *path);
void project_new(Session *s);

// Snapshot of the state in memory (for undo and for detecting unsaved changes). Free it with free().
// live=0: without volume/pan/mute/solo (these "live" things are not part of undo); live=1: with them.
char *project_snapshot(Session *s, int live);
// Loads ONE cell without stopping the other playing clips (for undo).
void project_load_clip_cell(Session *s, int c, int sc, bool is_note, const char *path, const char *name,
                            bool loop_on, size_t loop_start, size_t loop_end,
                            size_t start_marker, size_t end_marker, const char *edits);

int project_collect_and_save(Session *s, const char *project_dir,
                             const char *project_name);

int project_ask_name(char *out_name, size_t size);
int project_ask_save_path(char *out_path, size_t size);
int project_ask_open_path(char *out_path, size_t size);
int project_ask_directory(char *out_path, size_t size);

const char *project_get_current_path(void);
void project_set_current_path(const char *path);
bool project_has_current(void);

#endif
