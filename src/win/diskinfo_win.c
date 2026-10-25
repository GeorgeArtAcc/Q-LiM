/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Windows: no /sys - the disk kind is "unknown" (-> the medium read buffer, which is raised automatically if it runs dry) */
#include "../diskinfo.h"

/* ---------------- disk detection: unknown (-> the medium buffer) ---------------- */
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
DiskClass diskinfo_classify_dev(unsigned major_num, unsigned minor_num) { (void)major_num; (void)minor_num; return DISK_UNKNOWN; }
DiskClass diskinfo_classify_path(const char *path, unsigned long long *dev_key)
{
    (void)path;
    if (dev_key) *dev_key = 0;
    return DISK_UNKNOWN;
}
void diskinfo_set_roots(const char *sysfs_root, const char *mountinfo_path) { (void)sysfs_root; (void)mountinfo_path; }

