/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _POSIX_C_SOURCE 200809L
/* BPM detection on synthetic songs with a known tempo: t_<bpm>_<style>.wav */
#include "../src/bpm.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "data";
    DIR *d = opendir(dir);
    if (!d) { printf("no dir %s\n", dir); return 1; }
    struct dirent *e;
    int bad = 0, n = 0;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "t_", 2) != 0) continue;
        double want = atof(e->d_name + 2);
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        float got = bpm_detect_file(p, NULL);
        clock_gettime(CLOCK_MONOTONIC, &b);
        double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
        /* we also accept half/double tempo (musically the same), but we note it */
        double err = fabs(got - want);
        const char *tag = err <= 0.05 ? "OK" : (fabs(got * 2 - want) <= 0.1 || fabs(got - want * 2) <= 0.1) ? "OCTAVE" : "BAD";
        if (tag[0] == 'B') bad++;
        n++;
        printf("%-28s want %7.2f got %7.2f  %-6s %6.0f ms\n", e->d_name, want, got, tag, ms);
    }
    closedir(d);
    printf("%d files, %d bad\n", n, bad);
    return bad ? 1 : 0;
}
