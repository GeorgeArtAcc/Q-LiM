/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _POSIX_C_SOURCE 200809L
#include "devwatch.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static int is_generic(const char *t)
{
    static const char *const g[] = { "usb", "audio", "analog", "stereo", "digital", "output", "input", "built", "pro", "gen", "hdmi",
        "speaker", "speakers", "headphones", "surround", "mono", "default", "device", "interface", "line", "main", "iec958", "alsa",
        "jack", "pulse", "pipewire", "sound", "card", "the", "and", NULL };
    for (int i = 0; g[i]; ++i) if (strcasecmp(t, g[i]) == 0) return 1;
    return 0;
}

static int file_contains_all(const char *path, const DevWatch *w)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[8192]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f);
    buf[n] = 0;
    for (size_t i = 0; i < n; ++i) buf[i] = (char)tolower((unsigned char)buf[i]);
    for (int i = 0; i < w->ntok; ++i) {
        char low[32]; size_t k = 0;
        for (; w->tok[i][k] && k < sizeof low - 1; ++k) low[k] = (char)tolower((unsigned char)w->tok[i][k]);
        low[k] = 0;
        if (!strstr(buf, low)) return 0;
    }
    return 1;
}

void devwatch_init(DevWatch *w, const char *name, const char *root)
{
    memset(w, 0, sizeof *w);
    w->hw_card = -1;
    snprintf(w->root, sizeof w->root, "%s", root && root[0] ? root : "/proc/asound");
    if (!name) return;

    const char *hw = strstr(name, "(hw:");
    if (hw && isdigit((unsigned char)hw[4])) w->hw_card = atoi(hw + 4);

    /* significant words: up to the first ':' or '(' (after them comes a description of the output), at most 2 */
    char tmp[256]; snprintf(tmp, sizeof tmp, "%s", name);
    char *cut = strpbrk(tmp, ":("); if (cut) *cut = 0;
    char *save = NULL;
    for (char *t = strtok_r(tmp, " \t-_/", &save); t && w->ntok < 2; t = strtok_r(NULL, " \t-_/", &save)) {
        if (strlen(t) < 3 || is_generic(t)) continue;
        snprintf(w->tok[w->ntok++], sizeof w->tok[0], "%s", t);
    }
    if (w->ntok > 0) {
        char cards[300]; snprintf(cards, sizeof cards, "%s/cards", w->root);
        w->monitor_name = file_contains_all(cards, w);     /* we track the name only if we can see it at startup */
    }
}

int devwatch_present(const DevWatch *w)
{
    int known = 0;
    if (w->hw_card >= 0) {
        char p[300]; snprintf(p, sizeof p, "%s/card%d", w->root, w->hw_card);
        struct stat st;
        if (stat(p, &st) != 0) return 0;
        known = 1;
    }
    if (w->monitor_name) {
        char cards[300]; snprintf(cards, sizeof cards, "%s/cards", w->root);
        if (!file_contains_all(cards, w)) return 0;
        known = 1;
    }
    return known ? 1 : -1;
}

void heartbeat_reset(Heartbeat *h) { memset(h, 0, sizeof *h); }

int heartbeat_stalled(Heartbeat *h, unsigned count, double now, double timeout)
{
    if (!h->started) { h->started = 1; h->last = count; h->last_change = now; return 0; }
    if (count != h->last) { h->last = count; h->last_change = now; return 0; }
    return (now - h->last_change) >= timeout;
}
