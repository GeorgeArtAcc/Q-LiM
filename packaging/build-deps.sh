#!/bin/sh
# Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE)
# Builds the libraries Q-LiM bundles (static, -fPIC) for the Linux packages:
#   SDL2, SDL2_ttf (+FreeType, HarfBuzz), libsndfile (+Ogg, Vorbis, FLAC, Opus, mpg123, LAME), PortAudio, ALSA (link only)
#   sh packaging/build-deps.sh gnu    -> $PKGROOT/gnu/prefix   (glibc: Debian/Ubuntu/Mint/Fedora/openSUSE ...)
#   sh packaging/build-deps.sh musl   -> $PKGROOT/musl/prefix  (musl: Alpine)
#   sh packaging/build-deps.sh musl32 -> $PKGROOT/musl32/prefix (32-bit musl, i686 without SSE2)
#   sh packaging/build-deps.sh gnu32  -> $PKGROOT/gnu32/prefix  (32-bit glibc, i686; needs build-rt-gnu32.sh first)
#   sh packaging/build-deps.sh win32  -> $PKGROOT/win32/prefix (32-bit Windows XP and newer, llvm-mingw i686 msvcrt)
#   sh packaging/build-deps.sh win64  -> $PKGROOT/win64/prefix (1.4.7: only libsndfile + its codecs, static, for Makefile.win)
# packaging/patches/*.patch are applied to the sources first (1.4.7: mpg123 - no holes of silence on bad MP3 frames).
# Sources are expected in $PKGROOT/src (see packaging/fetch-sources.sh).
set -e
T=${1:-gnu}
PKGROOT=${PKGROOT:-/opt/lnx}
SRC=$PKGROOT/src
P=$PKGROOT/$T/prefix
B=$PKGROOT/$T/build
mkdir -p "$P" "$B"
J=${J:-2}

ARCH=""
if [ "$T" = win64 ]; then
    LLVM=${LLVM:-/opt/win/llvm-mingw-20241217-msvcrt-ubuntu-20.04-x86_64}
    TC=$LLVM/bin/x86_64-w64-mingw32
    export CC="$TC-clang" CXX="$TC-clang++" AR="$LLVM/bin/llvm-ar" RANLIB="$LLVM/bin/llvm-ranlib" RC="$TC-windres"
    HOSTARG="--host=x86_64-w64-mingw32"
    XINC=""
    ARCH="-D_WIN32_WINNT=0x0601 -DWINVER=0x0601"
    CROSS="-DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=AMD64 -DCMAKE_RC_COMPILER=$RC -DCMAKE_FIND_ROOT_PATH=$P \
           -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
           -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER"
elif [ "$T" = win32 ]; then
    LLVM=${LLVM:-/opt/win32/llvm-mingw-20241217-msvcrt-ubuntu-20.04-x86_64}
    TC=$LLVM/bin/i686-w64-mingw32
    export CC="$TC-clang" CXX="$TC-clang++" AR="$LLVM/bin/llvm-ar" RANLIB="$LLVM/bin/llvm-ranlib" RC="$TC-windres"
    HOSTARG="--host=i686-w64-mingw32"
    XINC=""
    # XP: _WIN32_WINNT 0x0501; no SSE/SSE2 required (pentiumpro = i686); SIMD paths are picked at run time
    ARCH="-march=pentiumpro -D_WIN32_WINNT=0x0501 -DWINVER=0x0501"
    CROSS="-DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=x86 -DCMAKE_RC_COMPILER=$RC -DCMAKE_FIND_ROOT_PATH=$P \
           -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
           -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER"
elif [ "$T" = gnu32 ]; then
    # clang + glibc link sysroot + libc++ (packaging/build-rt-gnu32.sh first), through a wrapper (cmake needs one word)
    HERE=$(cd "$(dirname "$0")" && pwd)
    mkdir -p $PKGROOT/gnu32/bin && cp "$HERE/gnu32-cc.sh" $PKGROOT/gnu32/bin/cc && cp "$HERE/gnu32-cc.sh" $PKGROOT/gnu32/bin/c++
    cp "$HERE/glibc32_compat.h" $PKGROOT/gnu32/
    clang --target=i686-linux-gnu --sysroot=$PKGROOT/glibc32/sysroot -march=i686 -O2 -fno-pic -c "$HERE/gnu32_start.c" \
        -o $PKGROOT/gnu32/gnu32_start.o
    export CC=$PKGROOT/gnu32/bin/cc CXX=$PKGROOT/gnu32/bin/c++ AR=llvm-ar-18 RANLIB=llvm-ranlib-18
    HOSTARG="--host=i686-linux-gnu"
    XROOT=$PKGROOT/gnu32/xroot
    XINC="-I$XROOT/usr/include"
    CROSS="-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=i686 -DCMAKE_FIND_ROOT_PATH=$P;$XROOT;$PKGROOT/glibc32/sysroot \
           -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
           -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER -DCMAKE_AR=$(command -v llvm-ar-18) -DCMAKE_RANLIB=$(command -v llvm-ranlib-18)"
elif [ "$T" = musl ] || [ "$T" = musl32 ]; then
    if [ "$T" = musl32 ]; then
        TC=$PKGROOT/i686-unknown-linux-musl/bin/i686-unknown-linux-musl; HOSTARG="--host=i686-linux-musl"
        ARCH="-march=i686 -mtune=generic -fno-stack-protector"; CPROC="-DCMAKE_SYSTEM_PROCESSOR=i686"
    else
        TC=$PKGROOT/x86_64-unknown-linux-musl/bin/x86_64-unknown-linux-musl; HOSTARG="--host=x86_64-linux-musl"; CPROC=""
    fi
    export CC="$TC-gcc" CXX="$TC-g++" AR="$TC-ar" RANLIB="$TC-ranlib"
    # A small "root" with only the X11/OpenGL headers and stub X11 libraries (made by packaging/make-x11-stubs.sh):
    # SDL loads the real libX11 at run time. The host's /usr/include (glibc) must never be used for musl.
    XROOT=$PKGROOT/$T/xroot
    XINC="-I$XROOT/usr/include"
    CROSS="-DCMAKE_SYSTEM_NAME=Linux $CPROC -DCMAKE_FIND_ROOT_PATH=$P;$XROOT -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
           -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER"
else
    export CC=gcc CXX=g++ AR=ar RANLIB=ranlib
    HOSTARG=""
    XINC="-I$PKGROOT/glinc"            # OpenGL headers (from libglvnd) for SDL's OpenGL renderer
    CROSS=""
fi
if [ "$T" = win32 ] || [ "$T" = win64 ]; then export CFLAGS="-O2 $ARCH" CXXFLAGS="-O2 $ARCH"
else export CFLAGS="-O2 -fPIC $ARCH $XINC" CXXFLAGS="-O2 -fPIC $ARCH $XINC"; fi
export PKG_CONFIG_PATH="$P/lib/pkgconfig" PKG_CONFIG_LIBDIR="$P/lib/pkgconfig"
CM="cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_PREFIX_PATH=$P -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF -DCMAKE_C_COMPILER=$CC -DCMAKE_CXX_COMPILER=$CXX $CROSS"
command -v ninja >/dev/null || CM=$(echo "$CM" | sed 's/-G Ninja//')

step() { echo "=== [$T] $1"; }
cmk() {   # cmk <name> <srcdir> <args...>
    n=$1; s=$2; shift 2
    [ -f "$B/$n.done" ] && return 0
    step "$n"
    rm -rf "$B/$n"
    $CM -S "$s" -B "$B/$n" "$@" > "$B/$n.log" 2>&1 && cmake --build "$B/$n" -j$J >> "$B/$n.log" 2>&1 && \
        cmake --install "$B/$n" >> "$B/$n.log" 2>&1 || { tail -n 30 "$B/$n.log"; exit 1; }
    touch "$B/$n.done"
}

# ---- ALSA (shared, only to link against; at run time the system's libasound.so.2 is used) ----
# ---- our patches to the sources (each one only once) ----
HERE_P=$(cd "$(dirname "$0")" && pwd)/patches
for pf in "$HERE_P"/*.patch; do
    [ -f "$pf" ] || continue
    lib=$(basename "$pf" | cut -d- -f1)
    if patch -d "$SRC/$lib" -p1 -N --dry-run -s < "$pf" >/dev/null 2>&1; then
        echo "=== patch $(basename "$pf")"; patch -d "$SRC/$lib" -p1 -N -s < "$pf"
    fi
done

if [ "$T" != win32 ] && [ "$T" != win64 ] && [ ! -f "$B/alsa.done" ]; then
    step alsa
    rm -rf "$B/alsa" && cp -r "$SRC/alsa-lib" "$B/alsa" && cd "$B/alsa"
    (autoreconf -fi && ./configure $HOSTARG --prefix="$P" --enable-shared --disable-static --disable-python \
        --disable-alisp --disable-topology --disable-ucm --without-debug && make -j$J && make install) > "$B/alsa.log" 2>&1 \
        || { tail -n 30 "$B/alsa.log"; exit 1; }
    touch "$B/alsa.done"
fi

cmk ogg    "$SRC/ogg"    -DINSTALL_DOCS=OFF -DBUILD_TESTING=OFF
cmk vorbis "$SRC/vorbis"
cmk flac   "$SRC/flac"   -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF -DINSTALL_MANPAGES=OFF -DWITH_OGG=ON -DWITH_STACK_PROTECTOR=OFF
cmk opus   "$SRC/opus"   -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF
cmk mpg123 "$SRC/mpg123/ports/cmake" -DBUILD_PROGRAMS=OFF -DBUILD_LIBOUT123=OFF

if [ ! -f "$B/lame.done" ]; then
    step lame
    rm -rf "$B/lame" && cp -r "$SRC/lame" "$B/lame" && cd "$B/lame"
    (./configure $HOSTARG --prefix="$P" --disable-shared --enable-static --disable-frontend --disable-decoder \
        --disable-gtktest && make -j$J && make install) > "$B/lame.log" 2>&1 || { tail -n 30 "$B/lame.log"; exit 1; }
    touch "$B/lame.done"
fi

{ [ "$T" = win32 ] || [ "$T" = win64 ]; } && SNDF_CF="-DFLAC__NO_DLL" || SNDF_CF=""
CFLAGS="$CFLAGS $SNDF_CF" cmk sndfile "$SRC/libsndfile" -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DENABLE_CPACK=OFF \
    -DENABLE_EXTERNAL_LIBS=ON -DENABLE_MPEG=ON -DINSTALL_MANPAGES=OFF -DBUILD_REGTEST=OFF
[ "$T" = win64 ] && { echo "=== [$T] libsndfile + codecs in $P"; exit 0; }

if [ "$T" = win32 ]; then
# Windows: MME + DirectSound (XP), WASAPI + WDM-KS (Vista+ - PortAudio loads what it needs at run time)
# clang says __GNUC__ 4 and i686 has no __MINGW64__: PortAudio then re-declares types the mingw-w64 headers already have
sed -i 's/!defined(__MINGW64__))/!defined(__MINGW64_VERSION_MAJOR))/' "$SRC/portaudio/src/hostapi/wasapi/pa_win_wasapi.c"
# (compiled with the Vista headers: WASAPI needs them; every Vista function is loaded at run time - checked with imports)
CFLAGS="-O2 -march=pentiumpro -D_WIN32_WINNT=0x0600 -DWINVER=0x0600 -DNTDDI_VERSION=0x06000000" \
# ASIO when the Steinberg ASIO SDK (GPLv3, the user's zip) is there: ASIOSDK=/opt/win/asiosdk/ASIOSDK by default
ASIOSDK=${ASIOSDK:-/opt/win/asiosdk/ASIOSDK}
if [ -f "$ASIOSDK/common/asio.h" ]; then PA_ASIO="-DPA_USE_ASIO=ON -DASIOSDK_ROOT_DIR=$ASIOSDK -DASIOSDK_INCLUDE_DIR=$ASIOSDK/common"
else PA_ASIO="-DPA_USE_ASIO=OFF"; echo "=== [$T] (no ASIO SDK in $ASIOSDK: PortAudio without ASIO)"; fi
cmk portaudio "$SRC/portaudio" -DPA_BUILD_SHARED_LIBS=OFF $PA_ASIO -DPA_USE_DS=ON -DPA_USE_WMME=ON \
    -DPA_USE_WASAPI=ON -DPA_USE_WDMKS=ON
cmk sdl2 "$SRC/SDL" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_AUDIO=OFF -DSDL_HIDAPI=OFF \
    -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF -DSDL_SENSOR=OFF -DSDL_VULKAN=OFF -DSDL_RENDER_D3D11=OFF -DSDL_RENDER_D3D12=OFF
cmk sdl2_ttf "$SRC/SDL_ttf" -DSDL2TTF_VENDORED=ON -DSDL2TTF_HARFBUZZ=ON -DSDL2TTF_SAMPLES=OFF -DSDL2TTF_INSTALL=ON \
    -DSDL2TTF_BUILD_SHARED_LIBS=OFF
echo "=== [$T] all libraries in $P"; exit 0
fi

# 1.4.0: with the JACK driver; libjack is NOT linked - Q-LiM loads it at run time if it is there (src/jack_shim.c)
cmk portaudio "$SRC/portaudio" -DPA_BUILD_SHARED_LIBS=OFF -DPA_USE_ALSA=ON -DPA_USE_JACK=ON \
    -DJACK_INCLUDE_DIRS="$SRC/jackinc" -DJACK_LIBRARIES=dl \
    -DALSA_INCLUDE_DIR="$P/include" -DALSA_LIBRARY="$P/lib/libasound.so"

# SDL2: X11 (loaded at run time), OpenGL/GLES renderers; no SDL audio (Q-LiM has its own), no Wayland/KMS
cmk sdl2 "$SRC/SDL" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON -DSDL_TEST=OFF \
    -DSDL_X11=ON -DSDL_X11_SHARED=ON -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_VULKAN=OFF -DSDL_RPI=OFF \
    -DSDL_AUDIO=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_LIBUDEV=OFF -DSDL_PIPEWIRE=OFF -DSDL_PULSEAUDIO=OFF \
    -DSDL_ALSA=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_OSS=OFF -DSDL_HIDAPI=OFF -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF \
    -DSDL_SENSOR=OFF -DSDL_X11_XINPUT=OFF -DSDL_X11_XSCRNSAVER=OFF -DSDL_X11_XRANDR=OFF -DSDL_X11_XCURSOR=OFF \
    -DSDL_X11_XFIXES=OFF -DSDL_X11_XDBE=OFF -DSDL_X11_XSHAPE=OFF

cmk sdl2_ttf "$SRC/SDL_ttf" -DSDL2TTF_VENDORED=ON -DSDL2TTF_HARFBUZZ=ON -DSDL2TTF_SAMPLES=OFF -DSDL2TTF_INSTALL=ON \
    -DSDL2TTF_BUILD_SHARED_LIBS=OFF

echo "=== [$T] all libraries in $P"
