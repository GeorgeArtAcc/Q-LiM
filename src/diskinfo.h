/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
#ifndef DISKINFO_H
#define DISKINFO_H

/*
 * Detects the kind of disk a file is on (Linux, through /sys and /proc/self/mountinfo).
 * Used to choose the buffer size when reading clips: slow disk -> bigger buffer.
 * Nothing is read from the disk itself and no root is needed.
 */

typedef enum {
    DISK_UNKNOWN = 0,   /* unknown (or the file does not exist) */
    DISK_NVME,
    DISK_SSD,
    DISK_HDD,           /* spinning disk */
    DISK_REMOVABLE,     /* USB stick, SD card, eMMC */
    DISK_NETWORK,       /* NFS, SMB, sshfs ... */
    DISK_RAM            /* tmpfs / ramfs (/dev/shm) */
} DiskClass;

const char *diskinfo_class_name(DiskClass c);

/* Class by block device number (major:minor), through sysfs. DISK_UNKNOWN if it cannot be determined. */
DiskClass diskinfo_classify_dev(unsigned major_num, unsigned minor_num);

/* Class of the disk the file/folder is on. *dev_key = file system id (st_dev) or 0
 * if the path does not exist. The results are remembered (cache). */
DiskClass diskinfo_classify_path(const char *path, unsigned long long *dev_key);

/* For tests: replace /sys and /proc/self/mountinfo (NULL = the standard ones) and clear the cache. */
void diskinfo_set_roots(const char *sysfs_root, const char *mountinfo_path);

#endif
