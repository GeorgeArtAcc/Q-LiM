#!/bin/sh
# Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE)
# 32-bit glibc build (T=gnu32), step 2 of 3:
#   1. a glibc i686 "link sysroot" in $PKGROOT/glibc32/sysroot (glibc 2.42 from source, see HANDOFF.md)
#   2. THIS: compiler-rt (builtins + crtbegin/end) and libc++/libc++abi/libunwind (static) for i686-linux-gnu,
#      built with the host clang, -march=i686 (no SSE needed)          -> $PKGROOT/gnu32/rt
#   3. sh packaging/build-deps.sh gnu32, then make -f packaging/Makefile.pkg T=gnu32
# Sources: llvm-project-18.1.8.src (compiler-rt, libcxx, libcxxabi, libunwind, runtimes, cmake) in $PKGROOT/src.
set -e
PKGROOT=${PKGROOT:-/opt/lnx}
LLVM_SRC=${LLVM_SRC:-$PKGROOT/src/llvm-project-18.1.8.src}
SYSROOT=$PKGROOT/glibc32/sysroot
RT=$PKGROOT/gnu32/rt
B=$PKGROOT/gnu32/rtbuild
J=${J:-2}
TGT=i686-linux-gnu
HERE=$(cd "$(dirname "$0")" && pwd)
GV="-D_REENTRANT -I$PKGROOT/glibc_version_header/version_headers/x86 -include $HERE/glibc32_compat.h"   # old glibc symbol versions
mkdir -p "$RT" "$B"

# our own clang resource dir: the host's headers + the i686 compiler-rt libraries
RES=$RT/clang
if [ ! -d "$RES/include" ]; then
    mkdir -p "$RES" && cp -r "$(clang -print-resource-dir)/include" "$RES/"
fi

# ---- compiler-rt: builtins + crtbegin/crtend ----
if [ ! -f "$B/crt.done" ]; then
    echo "=== compiler-rt"
    rm -rf "$B/crt"
    cmake -G Ninja -S "$LLVM_SRC/compiler-rt" -B "$B/crt" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_ASM_COMPILER=clang \
        -DCMAKE_C_COMPILER_TARGET=$TGT -DCMAKE_CXX_COMPILER_TARGET=$TGT -DCMAKE_ASM_COMPILER_TARGET=$TGT \
        -DCMAKE_SYSROOT=$SYSROOT -DCMAKE_C_FLAGS="-march=i686 -mtune=generic" -DCMAKE_ASM_FLAGS="-march=i686" \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON -DCOMPILER_RT_BUILD_BUILTINS=ON -DCOMPILER_RT_BUILD_CRT=ON \
        -DCOMPILER_RT_BUILD_SANITIZERS=OFF -DCOMPILER_RT_BUILD_XRAY=OFF -DCOMPILER_RT_BUILD_LIBFUZZER=OFF \
        -DCOMPILER_RT_BUILD_PROFILE=OFF -DCOMPILER_RT_BUILD_MEMPROF=OFF -DCOMPILER_RT_BUILD_ORC=OFF \
        -DCOMPILER_RT_BUILD_GWP_ASAN=OFF -DCOMPILER_RT_BUILD_CTX_PROFILE=OFF \
        -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF -DCMAKE_INSTALL_PREFIX=$RES > "$B/crt.log" 2>&1 \
        && ninja -C "$B/crt" -j$J >> "$B/crt.log" 2>&1 && ninja -C "$B/crt" install >> "$B/crt.log" 2>&1 \
        || { tail -n 30 "$B/crt.log"; exit 1; }
    touch "$B/crt.done"
fi

# ---- libunwind + libc++abi + libc++ (static only) ----
if [ ! -f "$B/cxx.done" ]; then
    echo "=== libc++"
    rm -rf "$B/cxx"
    cmake -G Ninja -S "$LLVM_SRC/runtimes" -B "$B/cxx" -DCMAKE_BUILD_TYPE=Release \
        -DLLVM_ENABLE_RUNTIMES="libunwind;libcxxabi;libcxx" \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_ASM_COMPILER=clang \
        -DCMAKE_C_COMPILER_TARGET=$TGT -DCMAKE_CXX_COMPILER_TARGET=$TGT -DCMAKE_ASM_COMPILER_TARGET=$TGT \
        -DCMAKE_SYSROOT=$SYSROOT \
        -DCMAKE_C_FLAGS="-march=i686 -mtune=generic -resource-dir=$RES $GV" \
        -DCMAKE_CXX_FLAGS="-march=i686 -mtune=generic -resource-dir=$RES $GV" \
        -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld --rtlib=compiler-rt -resource-dir=$RES" \
        -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=lld --rtlib=compiler-rt -resource-dir=$RES" \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        -DLIBUNWIND_ENABLE_SHARED=OFF -DLIBUNWIND_USE_COMPILER_RT=ON \
        -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBCXXABI_USE_COMPILER_RT=ON -DLIBCXXABI_USE_LLVM_UNWINDER=ON \
        -DLIBCXXABI_ENABLE_STATIC_UNWINDER=ON \
        -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXX_USE_COMPILER_RT=ON -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON \
        -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF \
        -DLIBUNWIND_INCLUDE_TESTS=OFF -DLIBCXX_HAS_ATOMIC_LIB=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX=$RT > "$B/cxx.log" 2>&1 \
        && ninja -C "$B/cxx" -j$J >> "$B/cxx.log" 2>&1 && ninja -C "$B/cxx" install >> "$B/cxx.log" 2>&1 \
        || { tail -n 40 "$B/cxx.log"; exit 1; }
    touch "$B/cxx.done"
fi
echo "=== runtime in $RT"
