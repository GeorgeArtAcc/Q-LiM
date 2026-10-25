/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _POSIX_C_SOURCE 200809L
#include "dropfiles.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static const char *k_ext[] = { "wav", "flac", "ogg", "oga", "opus", "mp3", "aif", "aiff", "aifc", "m4a", "aac",
                               "wma", "mp2", "au", "caf", "w64", "wv", "ape", "mka", NULL };

int dropfiles_is_audio(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash)) return 0;
    ++dot;
    for (int i = 0; k_ext[i]; ++i)
        if (strcasecmp(dot, k_ext[i]) == 0) return 1;
    return 0;
}

typedef struct { char **v; size_t n, cap; } PathList;

static void pl_add(PathList *l, const char *p)
{
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 32;
        char **nv = realloc(l->v, nc * sizeof *nv);
        if (!nv) return;
        l->v = nv;
        l->cap = nc;
    }
    char *d = strdup(p);
    if (d) l->v[l->n++] = d;
}

static int cmp_str(const void *a, const void *b)
{
    return strcoll(*(char *const *)a, *(char *const *)b);
}

static void collect(PathList *out, const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return;
        PathList tmp = { 0 };
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char full[1100];
            snprintf(full, sizeof full, "%s/%s", path, e->d_name);
            struct stat s2;
            if (stat(full, &s2) == 0 && S_ISREG(s2.st_mode) && dropfiles_is_audio(full))
                pl_add(&tmp, full);
        }
        closedir(d);
        if (tmp.n > 1) qsort(tmp.v, tmp.n, sizeof *tmp.v, cmp_str);
        for (size_t i = 0; i < tmp.n; ++i) { pl_add(out, tmp.v[i]); free(tmp.v[i]); }
        free(tmp.v);
    } else if (S_ISREG(st.st_mode) && dropfiles_is_audio(path)) {
        pl_add(out, path);
    }
}

static int cell_empty(Session *s, int ch, int sc)
{
    Clip *cl = &s->channels[ch].clips[sc];
    return !atomic_load(&cl->has_clip);
}

int dropfiles_load(Session *s, const char *const *paths, unsigned count,
                   int hov_ch, int hov_scene, int sel_ch, int sel_scene,
                   char *msg, size_t msg_size)
{
    if (msg && msg_size) msg[0] = '\0';
    if (!s || s->channel_count <= 0) return 0;

    PathList files = { 0 };
    for (unsigned i = 0; i < count; ++i) collect(&files, paths[i]);
    if (files.n == 0) {
        free(files.v);
        if (msg) snprintf(msg, msg_size, "Drop: no audio files found");
        return 0;
    }

    int ch = -1, sc = -1;
    if (hov_ch >= 0 && hov_scene >= 0 && hov_ch < s->channel_count) { ch = hov_ch; sc = hov_scene; }
    else if (sel_ch >= 0 && sel_scene >= 0 && sel_ch < s->channel_count) { ch = sel_ch; sc = sel_scene; }
    else {
        ch = 0; sc = 0;
        while (sc < s->scene_count && !cell_empty(s, ch, sc)) ++sc;
    }
    if (sc >= MAX_SCENES) sc = MAX_SCENES - 1;
    while (sc >= s->scene_count && s->scene_count < MAX_SCENES) session_add_scene(s);

    int loaded = 0, failed = 0;
    for (size_t i = 0; i < files.n; ++i) {
        if (i > 0) {                                   /* the following ones: only into empty cells */
            ++sc;
            while (sc < MAX_SCENES && sc < s->scene_count && !cell_empty(s, ch, sc)) ++sc;
        }
        if (sc >= MAX_SCENES) { failed += (int)(files.n - i); break; }
        while (sc >= s->scene_count && s->scene_count < MAX_SCENES) session_add_scene(s);
        if (session_set_clip(s, ch, sc, files.v[i]) == 0) ++loaded; else ++failed;
    }

    if (msg) {
        if (failed == 0) snprintf(msg, msg_size, loaded == 1 ? "Loaded 1 file" : "Loaded %d files", loaded);
        else snprintf(msg, msg_size, "Loaded %d, could not load %d", loaded, failed);
    }
    for (size_t i = 0; i < files.n; ++i) free(files.v[i]);
    free(files.v);
    return loaded;
}
