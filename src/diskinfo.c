/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "diskinfo.h"

#include <dirent.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>

static char g_sys[PATH_MAX] = "/sys";
static char g_mi[PATH_MAX]  = "/proc/self/mountinfo";

#define CACHE_N 64
static struct { unsigned long long key; DiskClass c; } g_cache[CACHE_N];
static int g_cache_n = 0;

void diskinfo_set_roots(const char *sysfs_root, const char *mountinfo_path)
{
    snprintf(g_sys, sizeof g_sys, "%s", sysfs_root ? sysfs_root : "/sys");
    snprintf(g_mi,  sizeof g_mi,  "%s", mountinfo_path ? mountinfo_path : "/proc/self/mountinfo");
    g_cache_n = 0;
}

const char *diskinfo_class_name(DiskClass c)
{
    switch (c) {
    case DISK_NVME:      return "NVMe";
    case DISK_SSD:       return "SSD";
    case DISK_HDD:       return "HDD";
    case DISK_REMOVABLE: return "USB/SD";
    case DISK_NETWORK:   return "network";
    case DISK_RAM:       return "RAM";
    default:             return "unknown";
    }
}

/* the bigger, the slower (for a volume that spans several disks) */
static int slow_rank(DiskClass c)
{
    switch (c) {
    case DISK_RAM:       return 0;
    case DISK_NVME:      return 1;
    case DISK_SSD:       return 2;
    case DISK_UNKNOWN:   return 3;
    case DISK_REMOVABLE: return 3;
    case DISK_NETWORK:   return 4;
    case DISK_HDD:       return 5;
    }
    return 3;
}

static int read_int_file(const char *dir, const char *rel, int *out)
{
    char p[PATH_MAX + 64];
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int v = 0;
    int ok = (fscanf(f, "%d", &v) == 1);
    fclose(f);
    if (ok) *out = v;
    return ok;
}

static int path_exists(const char *dir, const char *rel)
{
    char p[PATH_MAX + 64];
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    struct stat st;
    return stat(p, &st) == 0;
}

/* dir = the real sysfs path of a block device (disk or partition) */
static DiskClass classify_dir(const char *dir_in, int depth)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", dir_in);

    if (path_exists(dir, "partition")) {                 /* partition -> the disk is the parent folder */
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof tmp, "%s", dir);
        snprintf(dir, sizeof dir, "%s", dirname(tmp));
    }

    /* device-mapper / RAID / LVM / LUKS: look at the disks below it and take the slowest */
    if (depth < 5) {
        char sl[PATH_MAX + 16];
        snprintf(sl, sizeof sl, "%s/slaves", dir);
        DIR *d = opendir(sl);
        if (d) {
            DiskClass worst = DISK_UNKNOWN;
            int any = 0;
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.') continue;
                char link[PATH_MAX * 2], real[PATH_MAX];
                snprintf(link, sizeof link, "%s/%s", sl, e->d_name);
                if (!realpath(link, real)) continue;
                DiskClass c = classify_dir(real, depth + 1);
                if (!any || slow_rank(c) > slow_rank(worst)) worst = c;
                any = 1;
            }
            closedir(d);
            if (any) return worst;
        }
    }

    const char *name = strrchr(dir, '/');
    name = name ? name + 1 : dir;

    if (strncmp(name, "nvme", 4) == 0) return DISK_NVME;
    if (strncmp(name, "mmcblk", 6) == 0) return DISK_REMOVABLE;
    if (strncmp(name, "loop", 4) == 0 || strncmp(name, "ram", 3) == 0 || strncmp(name, "zram", 4) == 0)
        return strncmp(name, "loop", 4) == 0 ? DISK_UNKNOWN : DISK_RAM;

    int rot = -1, rem = 0;
    if (!read_int_file(dir, "queue/rotational", &rot)) return DISK_UNKNOWN;
    read_int_file(dir, "removable", &rem);
    if (rot == 1) return DISK_HDD;
    if (rem == 1) return DISK_REMOVABLE;
    return DISK_SSD;
}

DiskClass diskinfo_classify_dev(unsigned major_num, unsigned minor_num)
{
    char link[PATH_MAX + 64], real[PATH_MAX];
    snprintf(link, sizeof link, "%s/dev/block/%u:%u", g_sys, major_num, minor_num);
    if (!realpath(link, real)) return DISK_UNKNOWN;
    return classify_dir(real, 0);
}

/* ---- /proc/self/mountinfo ---- */

static void unescape_mp(char *s)
{
    char *w = s;
    for (const char *r = s; *r; ) {
        if (r[0] == '\\' && r[1] >= '0' && r[1] <= '7' && r[2] >= '0' && r[2] <= '7' && r[3] >= '0' && r[3] <= '7') {
            *w++ = (char)(((r[1] - '0') << 6) | ((r[2] - '0') << 3) | (r[3] - '0'));
            r += 4;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

static int under_mount(const char *path, const char *mp)
{
    size_t n = strlen(mp);
    if (n == 1 && mp[0] == '/') return 1;
    return strncmp(path, mp, n) == 0 && (path[n] == '/' || path[n] == 0);
}

static int is_network_fs(const char *t)
{
    static const char *const names[] = { "nfs", "nfs4", "cifs", "smb3", "smbfs", "sshfs", "fuse.sshfs", "davfs",
                                         "fuse.davfs", "afs", "ceph", "glusterfs", "fuse.glusterfs", "9p", NULL };
    for (int i = 0; names[i]; ++i) if (strcmp(t, names[i]) == 0) return 1;
    return 0;
}

static DiskClass classify_by_mountinfo(const char *real_path)
{
    FILE *f = fopen(g_mi, "r");
    if (!f) return DISK_UNKNOWN;

    char line[4096];
    size_t best_len = 0;
    char best_type[64] = "", best_src[PATH_MAX] = "";
    int have = 0;

    while (fgets(line, sizeof line, f)) {
        char *tok[48];
        int n = 0;
        for (char *p = strtok(line, " \t\r\n"); p && n < 48; p = strtok(NULL, " \t\r\n")) tok[n++] = p;
        if (n < 10) continue;
        int dash = -1;
        for (int i = 6; i < n; ++i) if (strcmp(tok[i], "-") == 0) { dash = i; break; }
        if (dash < 0 || dash + 2 >= n) continue;

        char mp[PATH_MAX];
        snprintf(mp, sizeof mp, "%s", tok[4]);
        unescape_mp(mp);
        if (!under_mount(real_path, mp)) continue;
        size_t l = strlen(mp);
        if (have && l < best_len) continue;
        best_len = l;
        have = 1;
        snprintf(best_type, sizeof best_type, "%s", tok[dash + 1]);
        snprintf(best_src, sizeof best_src, "%s", tok[dash + 2]);
        unescape_mp(best_src);
    }
    fclose(f);
    if (!have) return DISK_UNKNOWN;

    if (is_network_fs(best_type)) return DISK_NETWORK;
    if (strcmp(best_type, "tmpfs") == 0 || strcmp(best_type, "ramfs") == 0) return DISK_RAM;
    if (strncmp(best_src, "/dev/", 5) == 0) {
        struct stat st;
        if (stat(best_src, &st) == 0 && S_ISBLK(st.st_mode))
            return diskinfo_classify_dev(major(st.st_rdev), minor(st.st_rdev));
    }
    return DISK_UNKNOWN;
}

DiskClass diskinfo_classify_path(const char *path, unsigned long long *dev_key)
{
    if (dev_key) *dev_key = 0;
    if (!path || !path[0]) return DISK_UNKNOWN;

    char real[PATH_MAX];
    struct stat st;
    if (!realpath(path, real) || stat(real, &st) != 0) {
        /* the file may not exist yet: try its folder */
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof tmp, "%s", path);
        if (!realpath(dirname(tmp), real) || stat(real, &st) != 0) return DISK_UNKNOWN;
    }

    unsigned long long key = (unsigned long long)st.st_dev;
    if (dev_key) *dev_key = key ? key : 1;      /* 0 is reserved for "no information" */

    for (int i = 0; i < g_cache_n; ++i)
        if (g_cache[i].key == key) return g_cache[i].c;

    DiskClass c = DISK_UNKNOWN;
    if (major(st.st_dev) != 0) c = diskinfo_classify_dev(major(st.st_dev), minor(st.st_dev));
    if (c == DISK_UNKNOWN) {
        DiskClass m = classify_by_mountinfo(real);
        if (m != DISK_UNKNOWN) c = m;
    }

    if (g_cache_n < CACHE_N) { g_cache[g_cache_n].key = key; g_cache[g_cache_n].c = c; ++g_cache_n; }
    return c;
}
