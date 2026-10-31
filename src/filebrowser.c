/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#include "filebrowser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>

// ============ Helpers ============

static bool has_ext(const char *name, const char *ext)
{
    size_t nlen = strlen(name);
    size_t elen = strlen(ext);
    if (nlen < elen) return false;
    for (size_t i = 0; i < elen; ++i) {
        if (tolower((unsigned char)name[nlen - elen + i]) !=
            tolower((unsigned char)ext[i])) {
            return false;
        }
    }
    return true;
}

bool fb_is_audio_file(const char *name)
{
    return has_ext(name, ".wav") ||
           has_ext(name, ".flac") ||
           has_ext(name, ".ogg") ||
           has_ext(name, ".mp3") ||
           has_ext(name, ".mid") ||            /* 1.3: MIDI files -> MIDI clips */
           has_ext(name, ".midi");
}

// ============ Init ============

void fb_init(FileBrowser *fb, const char *start_dir)
{
    if (!fb) return;
    memset(fb, 0, sizeof(*fb));
    fb->visible = false;
    fb->scroll_offset = 0;

    if (start_dir) {
        snprintf(fb->current_dir, sizeof(fb->current_dir), "%s", start_dir);
    } else {
        const char *home = getenv("HOME");
        snprintf(fb->current_dir, sizeof(fb->current_dir), "%s",
                 home ? home : "/");
    }
    fb_scan(fb);
}

// ============ Scan ============

// qsort comparison: folders first, then files, alphabetically
static int cmp_entries(const void *a, const void *b)
{
    const FbEntry *ea = (const FbEntry *)a;
    const FbEntry *eb = (const FbEntry *)b;

    // ".." always first
    if (ea->type == FB_ENTRY_PARENT && eb->type != FB_ENTRY_PARENT) return -1;
    if (eb->type == FB_ENTRY_PARENT && ea->type != FB_ENTRY_PARENT) return  1;

    // Folders before files
    bool da = (ea->type == FB_ENTRY_DIR);
    bool db = (eb->type == FB_ENTRY_DIR);
    if (da != db) return da ? -1 : 1;

    return strcasecmp(ea->name, eb->name);
}

void fb_scan(FileBrowser *fb)
{
    if (!fb) return;

    fb->entry_count = 0;
    fb->scroll_offset = 0;

    DIR *d = opendir(fb->current_dir);
    if (!d) {
        if (errno == ENOENT) printf("fb_scan: '%s' does not exist (yet)\n", fb->current_dir);   /* e.g. no Music folder: normal */
        else fprintf(stderr, "fb_scan: cannot open '%s'\n", fb->current_dir);
        return;
    }

    // ".." (unless we are at root)
    if (strcmp(fb->current_dir, "/") != 0) {
        FbEntry *e = &fb->entries[fb->entry_count++];
        snprintf(e->name, sizeof(e->name), "..");
        // parent path
        char parent[FB_MAX_PATH];
        snprintf(parent, sizeof(parent), "%s", fb->current_dir);
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) {
            *slash = '\0';
        } else {
            snprintf(parent, sizeof(parent), "/");
        }
        snprintf(e->full_path, sizeof(e->full_path), "%s", parent);
        e->type = FB_ENTRY_PARENT;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (fb->entry_count >= FB_MAX_ENTRIES) break;

        // Hide dotfiles
        if (de->d_name[0] == '.') continue;

        char full[FB_MAX_PATH];
        int written = snprintf(full, sizeof(full), "%s/%s",
                               fb->current_dir, de->d_name);
        if (written < 0 || written >= (int)sizeof(full)) continue;

        struct stat st;
        if (stat(full, &st) != 0) continue;

        FbEntry *e = &fb->entries[fb->entry_count];

        if (S_ISDIR(st.st_mode)) {
            snprintf(e->name, sizeof(e->name), "%s", de->d_name);
            snprintf(e->full_path, sizeof(e->full_path), "%s", full);
            e->type = FB_ENTRY_DIR;
            fb->entry_count++;
        } else if (S_ISREG(st.st_mode) && fb_is_audio_file(de->d_name)) {
            snprintf(e->name, sizeof(e->name), "%s", de->d_name);
            snprintf(e->full_path, sizeof(e->full_path), "%s", full);
            e->type = FB_ENTRY_AUDIO;
            fb->entry_count++;
        }
    }
    closedir(d);

    // Sorting (without "..")
    if (fb->entry_count > 1) {
        int start = (fb->entries[0].type == FB_ENTRY_PARENT) ? 1 : 0;
        qsort(&fb->entries[start],
              fb->entry_count - start,
              sizeof(FbEntry),
              cmp_entries);
    }

    printf("fb_scan: '%s' -> %d entries\n",
           fb->current_dir, fb->entry_count);
}

// ============ Navigation ============

void fb_enter(FileBrowser *fb, int entry_idx)
{
    if (!fb) return;
    if (entry_idx < 0 || entry_idx >= fb->entry_count) return;

    // Copy the path into a local buffer to avoid overlap
    char newdir[FB_MAX_PATH];
    snprintf(newdir, sizeof(newdir), "%s", fb->entries[entry_idx].full_path);

    FbEntryType t = fb->entries[entry_idx].type;

    if (t == FB_ENTRY_PARENT) {
        fb_go_up(fb);
    } else if (t == FB_ENTRY_DIR) {
        snprintf(fb->current_dir, sizeof(fb->current_dir), "%s", newdir);
        fb_scan(fb);
    }
}

void fb_go_up(FileBrowser *fb)
{
    if (!fb) return;
    if (strcmp(fb->current_dir, "/") == 0) return;

    char parent[FB_MAX_PATH];
    snprintf(parent, sizeof(parent), "%s", fb->current_dir);

    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = '\0';
    } else {
        snprintf(parent, sizeof(parent), "/");
    }

    snprintf(fb->current_dir, sizeof(fb->current_dir), "%s", parent);
    fb_scan(fb);
}

// ============ Toggle ============

void fb_toggle(FileBrowser *fb)
{
    if (!fb) return;
    fb->visible = !fb->visible;
}

// ============ Scroll ============

void fb_scroll(FileBrowser *fb, int delta)
{
    if (!fb) return;
    int max = fb->entry_count - 1;
    if (max < 0) max = 0;

    fb->scroll_offset += delta;
    if (fb->scroll_offset < 0) fb->scroll_offset = 0;
    if (fb->scroll_offset > max) fb->scroll_offset = max;
}
