/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* 32-bit glibc build (T=gnu32): force-included (-include) into every C/C++ file, also of the bundled libraries.
 * Links each glibc function to its OLD symbol version, so qlim runs on old 32-bit distributions too, although it is
 * linked against a new glibc. The list (MIT, github.com/wheybags/glibc_version_header) is in
 * $(PKGROOT)/glibc_version_header/version_headers/x86 (-I in the build flags). Baseline: glibc 2.17 (2012). */
#include <force_link_glibc_2.17.h>
