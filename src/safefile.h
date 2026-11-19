/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef SAFEFILE_H
#define SAFEFILE_H
/*
 * Safe file writing: write to a temporary file, check that everything has reached the disk and only then
 * replace the file in one go (rename is "all or nothing"). A crash, a power cut or a full disk during the write
 * can no longer ruin the old file. Optionally a backup copy (<file>.bak) is kept too.
 * Header only - needs no Makefile changes.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* 2.0.0: on Windows the UTF-8 / "replace the old file" versions, also from C++ (midi.cpp): there compat.h does not
 * redirect fopen / rename, and the plain Windows rename refuses to replace an existing file - midi.conf was then
 * never updated after the first save ("safe_commit: cannot replace ...", the controller was not remembered). */
#ifdef _WIN32
#define SAFE_FOPEN(p, m)   qw_fopen((p), (m))
#define SAFE_RENAME(a, b)  qw_rename((a), (b))
#else
#define SAFE_FOPEN(p, m)   fopen((p), (m))
#define SAFE_RENAME(a, b)  rename((a), (b))
#endif

static inline FILE *safe_open(const char *path, char *tmp, size_t tmp_size)
{
    snprintf(tmp, tmp_size, "%s.tmp%d", path, (int)getpid());
    return SAFE_FOPEN(tmp, "w");
}

/* 0 = success. On error the temporary file is deleted and the old one stays untouched. */
static inline int safe_commit(FILE *f, const char *tmp, const char *path, int keep_backup)
{
    int bad = 0;
    if (fflush(f) != 0 || ferror(f)) bad = 1;
    if (!bad && fsync(fileno(f)) != 0) bad = 1;
    if (fclose(f) != 0) bad = 1;
    if (bad) {
        fprintf(stderr, "safe_commit: write to '%s' failed (disk full?) - old file kept\n", path);
        unlink(tmp);
        return 1;
    }
    if (keep_backup) {
        char bak[1100];
        snprintf(bak, sizeof bak, "%s.bak", path);
        unlink(bak);
        if (link(path, bak) != 0) { /* no old file, or the file system does not support hard links - no big deal */ }
    }
    if (SAFE_RENAME(tmp, path) != 0) {
        fprintf(stderr, "safe_commit: cannot replace '%s'\n", path);
        unlink(tmp);
        return 1;
    }
    return 0;
}
#endif
