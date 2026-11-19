#!/bin/sh
# Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE)
# For the musl (Alpine) build: a tiny "root" with the X11 + OpenGL headers and STUB libX11/libXext (same names and
# sonames, empty functions) - only to link against. On Alpine the real libraries are used at run time.
set -e
#   sh packaging/make-x11-stubs.sh [musl|musl32]
PKGROOT=${PKGROOT:-/opt/lnx}
T=${1:-musl}
X=$PKGROOT/$T/xroot
if [ "$T" = musl32 ]; then STUBCC="$PKGROOT/i686-unknown-linux-musl/bin/i686-unknown-linux-musl-gcc"
elif [ "$T" = gnu32 ]; then STUBCC="clang --target=i686-linux-gnu --sysroot=$PKGROOT/glibc32/sysroot -fuse-ld=lld -nostdlib"
else STUBCC="$PKGROOT/x86_64-unknown-linux-musl/bin/x86_64-unknown-linux-musl-gcc"; fi
rm -rf "$X" && mkdir -p "$X/usr/include" "$X/usr/lib"
cp -r /usr/include/X11 "$X/usr/include/"
cp -r "$PKGROOT/glinc/GL" "$PKGROOT/glinc/KHR" "$X/usr/include/"
for so in libX11.so.6 libXext.so.6; do
    lib=$(echo $so | sed 's/^lib\(.*\)\.so\.6$/\1/')
    nm -D --defined-only /usr/lib/x86_64-linux-gnu/$so | awk '$2 ~ /^[TtWi]$/ {print "void "$3"(void){}"} $2 ~ /^[BDRV]$/ {print "char "$3"[64];"}' \
        | grep -v "^void _\(init\|fini\)(" | sort -u > "$X/stub_$lib.c"
    $STUBCC -shared -fPIC -w -o "$X/usr/lib/$so" "$X/stub_$lib.c" -Wl,-soname,$so
    ln -sf $so "$X/usr/lib/lib$lib.so"
done
echo "stubs in $X/usr/lib"
