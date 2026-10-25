/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef FILEBROWSER_H
#define FILEBROWSER_H

#include <stdbool.h>
#include <stddef.h>

#define FB_MAX_PATH     512
#define FB_MAX_ENTRIES  512
#define FB_MAX_NAME     256

typedef enum {
    FB_ENTRY_PARENT = 0,   // ".."
    FB_ENTRY_DIR,          // folder
    FB_ENTRY_AUDIO,        // .wav/.flac/.ogg/.mp3
} FbEntryType;

typedef struct {
    char        name[FB_MAX_NAME];
    char        full_path[FB_MAX_PATH];
    FbEntryType type;
} FbEntry;

typedef struct {
    char     current_dir[FB_MAX_PATH];
    FbEntry  entries[FB_MAX_ENTRIES];
    int      entry_count;
    int      scroll_offset;      // for vertical scrolling of the list
    bool     visible;            // is the panel open
} FileBrowser;

// Initialization — sets the starting directory
void fb_init(FileBrowser *fb, const char *start_dir);

// Rescans the current directory
void fb_scan(FileBrowser *fb);

// Enters a subfolder (by index into entries)
void fb_enter(FileBrowser *fb, int entry_idx);

// Goes one folder up
void fb_go_up(FileBrowser *fb);

// Toggles the panel's visibility
void fb_toggle(FileBrowser *fb);

// Helpers — extension check
bool fb_is_audio_file(const char *name);

// Scrolling the list
void fb_scroll(FileBrowser *fb, int delta);

#endif
