#!/bin/sh
# Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE)
# Builds the libraries Q-LiM bundles (static, -fPIC) for the Linux packages:
#   SDL2, SDL2_ttf (+FreeType, HarfBuzz), libsndfile (+Ogg, Vorbis, FLAC, Opus, mpg123, LAME), PortAudio, ALSA (link only)
#   sh packaging/build-deps.sh gnu    -> $PKGROOT/gnu/prefix   (glibc: Debian/Ubuntu/Mint/Fedora/openSUSE ...)
#   sh packaging/build-deps.sh musl   -> $PKGROOT/musl/prefix  (musl: Alpine)
# Sources are expected in $PKGROOT/src (see packaging/fetch-sources.sh).
set -e
T=${1:-gnu}
PKGROOT=${PKGROOT:-/opt/lnx}
SRC=$PKGROOT/src
P=$PKGROOT/$T/prefix
B=$PKGROOT/$T/build
mkdir -p "$P" "$B"
J=${J:-2}

if [ "$T" = musl ]; then
    TC=$PKGROOT/x86_64-unknown-linux-musl/bin/x86_64-unknown-linux-musl
    export CC="$TC-gcc" CXX="$TC-g++" AR="$TC-ar" RANLIB="$TC-ranlib"
    HOSTARG="--host=x86_64-linux-musl"
    # A small "root" with only the X11/OpenGL headers and stub X11 libraries (made by packaging/make-x11-stubs.sh):
    # SDL loads the real libX11 at run time. The host's /usr/include (glibc) must never be used for musl.
    XROOT=$PKGROOT/musl/xroot
    XINC="-I$XROOT/usr/include"
    CROSS="-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_FIND_ROOT_PATH=$P;$XROOT -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
           -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER"
else
    export CC=gcc CXX=g++ AR=ar RANLIB=ranlib
    HOSTARG=""
    XINC="-I$PKGROOT/glinc"            # OpenGL headers (from libglvnd) for SDL's OpenGL renderer
    CROSS=""
fi
export CFLAGS="-O2 -fPIC $XINC" CXXFLAGS="-O2 -fPIC $XINC"
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
if [ ! -f "$B/alsa.done" ]; then
    step alsa
    rm -rf "$B/alsa" && cp -r "$SRC/alsa-lib" "$B/alsa" && cd "$B/alsa"
    (autoreconf -fi && ./configure $HOSTARG --prefix="$P" --enable-shared --disable-static --disable-python \
        --disable-alisp --disable-topology --disable-ucm --without-debug && make -j$J && make install) > "$B/alsa.log" 2>&1 \
        || { tail -n 30 "$B/alsa.log"; exit 1; }
    touch "$B/alsa.done"
fi

cmk ogg    "$SRC/ogg"    -DINSTALL_DOCS=OFF -DBUILD_TESTING=OFF
cmk vorbis "$SRC/vorbis"
cmk flac   "$SRC/flac"   -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF -DINSTALL_MANPAGES=OFF -DWITH_OGG=ON
cmk opus   "$SRC/opus"   -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF
cmk mpg123 "$SRC/mpg123/ports/cmake" -DBUILD_PROGRAMS=OFF -DBUILD_LIBOUT123=OFF

if [ ! -f "$B/lame.done" ]; then
    step lame
    rm -rf "$B/lame" && cp -r "$SRC/lame" "$B/lame" && cd "$B/lame"
    (./configure $HOSTARG --prefix="$P" --disable-shared --enable-static --disable-frontend --disable-decoder \
        --disable-gtktest && make -j$J && make install) > "$B/lame.log" 2>&1 || { tail -n 30 "$B/lame.log"; exit 1; }
    touch "$B/lame.done"
fi

cmk sndfile "$SRC/libsndfile" -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DENABLE_CPACK=OFF \
    -DENABLE_EXTERNAL_LIBS=ON -DENABLE_MPEG=ON -DINSTALL_MANPAGES=OFF -DBUILD_REGTEST=OFF

cmk portaudio "$SRC/portaudio" -DPA_BUILD_SHARED_LIBS=OFF -DPA_USE_ALSA=ON -DPA_USE_JACK=OFF \
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
