#!/bin/sh
# Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE)
# 2.1: everything for the 32-bit Linux builds (T=musl32 and T=gnu32), from GitHub only, into $PKGROOT (/opt/lnx).
# Before 2.1 these steps were only described in HANDOFF.md; after a wiped machine they had to be redone by hand.
#   1. the i686 musl cross compiler (cross-tools/musl-cross)                        -> i686-unknown-linux-musl
#   2. glibc_version_header (old symbol versions: the gnu32 build needs only GLIBC_2.17)
#   3. gawk (glibc's configure needs it; the host may have only mawk)                -> tools/bin/gawk
#   4. glibc 2.42 for i686, compiled with the musl gcc (only to LINK against; the user's own glibc runs it)
#      + Linux headers from the musl toolchain                                        -> glibc32/sysroot, kh32
#   5. llvm-project 18.1.8 sources (compiler-rt, libc++ for gnu32)                    -> src/llvm-project-18.1.8.src
# Then: sh packaging/build-rt-gnu32.sh; sh packaging/make-x11-stubs.sh musl32; sh packaging/make-x11-stubs.sh gnu32;
#       sh packaging/build-deps.sh musl32; sh packaging/build-deps.sh gnu32
set -e
PKGROOT=${PKGROOT:-/opt/lnx}
J=${J:-2}
G=https://github.com
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$PKGROOT/src" && cd "$PKGROOT"

echo "=== 1. i686 musl compiler"
[ -d i686-unknown-linux-musl ] || { curl -fsSL -o musl32.tar.xz $G/cross-tools/musl-cross/releases/latest/download/i686-unknown-linux-musl.tar.xz && tar xf musl32.tar.xz && rm musl32.tar.xz; }
MCC=$PKGROOT/i686-unknown-linux-musl/bin/i686-unknown-linux-musl

echo "=== 2. glibc_version_header"
[ -d glibc_version_header ] || git clone -q --depth 1 $G/wheybags/glibc_version_header.git

echo "=== 3. gawk"
if [ ! -x tools/bin/gawk ]; then
    [ -d src/gawk ] || git clone -q --depth 1 $G/gnu-mirror-unofficial/gawk.git src/gawk
    (cd src/gawk && ./configure -q --prefix=$PKGROOT/tools --disable-nls --without-mpfr --disable-extensions >/dev/null && (cd support && make -s >/dev/null) && make -s -j$J gawk >/dev/null && mkdir -p $PKGROOT/tools/bin && cp gawk $PKGROOT/tools/bin/)
fi
export PATH=$PKGROOT/tools/bin:$PATH

echo "=== 4. glibc 2.42 (i686, link sysroot)"
if [ ! -f glibc32/sysroot/usr/lib/libc.so.6 ] && [ ! -f glibc32/sysroot/lib/libc.so.6 ]; then
    [ -d src/glibc ] || git clone -q --depth 1 -b glibc-2.42 $G/bminor/glibc.git src/glibc
    # the musl gcc has no ifunc attribute: __tls_get_addr without the ifunc
    python3 "$HERE/glibc_i386_noifunc.py" src/glibc/sysdeps/i386/dl-tls-get-addr.c
    # Linux headers: from the musl toolchain (linux/, asm/, asm-generic/ ...)
    KH=$PKGROOT/kh32/include; mkdir -p $KH
    for d in linux asm asm-generic mtd rdma scsi sound video xen drm misc; do
        [ -d "$PKGROOT/i686-unknown-linux-musl/i686-unknown-linux-musl/sysroot/usr/include/$d" ] && cp -r "$PKGROOT/i686-unknown-linux-musl/i686-unknown-linux-musl/sysroot/usr/include/$d" $KH/
    done
    rm -rf glibc32/build && mkdir -p glibc32/build glibc32/sysroot
    (cd glibc32/build && CC="$MCC-gcc -m32 -march=i686" CXX="$MCC-g++ -m32" AR=$MCC-ar RANLIB=$MCC-ranlib \
        ../../src/glibc/configure --host=i686-linux-gnu --build=x86_64-linux-gnu --prefix=/usr --with-headers=$KH \
        --disable-werror --disable-profile --disable-nscd --disable-timezone-tools --disable-crypt \
        --enable-kernel=3.2 libc_cv_slibdir=/lib > ../configure.log 2>&1 && \
     make -j$J > ../make.log 2>&1 && make install DESTDIR=$PKGROOT/glibc32/sysroot > ../install.log 2>&1)
    cp -r $KH/* glibc32/sysroot/usr/include/ 2>/dev/null || true
fi

echo "=== 5. llvm-project 18.1.8 sources"
[ -d src/llvm-project-18.1.8.src ] || { curl -fsSL -o src/llvm.tar.xz $G/llvm/llvm-project/releases/download/llvmorg-18.1.8/llvm-project-18.1.8.src.tar.xz && \
    (cd src && tar xf llvm.tar.xz --exclude='*/test/*' --exclude='*/clang/*' --exclude='*/lldb/*' --exclude='*/mlir/*' --exclude='*/flang/*' && rm llvm.tar.xz); }
echo "32-bit toolchain parts ready in $PKGROOT"
