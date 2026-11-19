#!/bin/sh
# Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE)
# 32-bit glibc build: compiler wrapper (installed as $PKGROOT/gnu32/bin/cc and c++ by build-deps.sh gnu32).
# clang for i686-linux-gnu with the glibc link sysroot, compiler-rt, libc++ (static) and the old-glibc symbol pins.
PKGROOT=${PKGROOT:-/opt/lnx}
RT=$PKGROOT/gnu32/rt
SR=$PKGROOT/glibc32/sysroot
PIN="-D_REENTRANT -I$PKGROOT/glibc_version_header/version_headers/x86 -include $PKGROOT/gnu32/glibc32_compat.h"
BASE="--target=i686-linux-gnu --sysroot=$SR -march=i686 -mtune=generic -fuse-ld=lld --rtlib=compiler-rt --unwindlib=none -resource-dir=$RT/clang"
link=1; exe=1
for a in "$@"; do case "$a" in -c|-S|-E|-M|-MM) link=0;; -shared|-r) exe=0;; esac; done
# executables: own entry point (gnu32_start.o: __libc_start_main@GLIBC_2.0), crti/crtn + compiler-rt crtbegin/end
PRE=""; POST=""
if [ $link = 1 ] && [ $exe = 1 ]; then
    PRE="-nostartfiles -no-pie $SR/usr/lib/crti.o $RT/clang/lib/linux/clang_rt.crtbegin-i386.o $PKGROOT/gnu32/gnu32_start.o"
    POST="$RT/clang/lib/linux/clang_rt.crtend-i386.o $SR/usr/lib/crtn.o"
fi
case "$(basename "$0")" in
    c++|*++)
        if [ $link = 1 ]; then exec clang++ $BASE -stdlib=libc++ -nostdinc++ -isystem $RT/include/c++/v1 $PIN $PRE "$@" \
            -L$RT/lib -Wl,-Bstatic -lc++ -lc++abi -lunwind -Wl,-Bdynamic -lpthread -ldl $POST
        else exec clang++ $BASE -stdlib=libc++ -nostdinc++ -isystem $RT/include/c++/v1 $PIN "$@"; fi ;;
    *)  if [ $link = 1 ]; then exec clang $BASE $PIN $PRE "$@" $POST; else exec clang $BASE $PIN "$@"; fi ;;
esac
