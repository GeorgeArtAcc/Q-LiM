#!/bin/sh
# Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE)
# Downloads everything for the Linux packages into $PKGROOT (default /opt/lnx): library sources (exact versions),
# the musl toolchain (Alpine build), OpenGL headers and nfpm (makes .deb/.rpm/.apk), appimagetool. Everything comes from GitHub.
set -e
PKGROOT=${PKGROOT:-/opt/lnx}
G=https://github.com
mkdir -p "$PKGROOT/src" && cd "$PKGROOT/src"
cl() { [ -d "$3" ] || git clone -q --depth 1 ${2:+-b "$2"} "$G/$1.git" "$3"; }
cl libsdl-org/SDL release-2.30.10 SDL
[ -d SDL_ttf ] || git clone -q --depth 1 -b release-2.22.0 --recurse-submodules --shallow-submodules $G/libsdl-org/SDL_ttf.git SDL_ttf
cl xiph/ogg v1.3.5 ogg
cl xiph/vorbis v1.3.7 vorbis
cl xiph/flac 1.4.3 flac
cl xiph/opus v1.5.2 opus
cl libsndfile/libsndfile 1.2.2 libsndfile
cl madebr/mpg123 "" mpg123
cl lameproject/lame "" lame
cl PortAudio/portaudio v19.7.0 portaudio
cl thestk/rtmidi 6.0.0 rtmidi
cl alsa-project/alsa-lib v1.2.12 alsa-lib
cd "$PKGROOT"
[ -d glvnd ] || git clone -q --depth 1 $G/NVIDIA/libglvnd.git glvnd
mkdir -p glinc && cp -r glvnd/include/GL glvnd/include/KHR glinc/
[ -x nfpm ] || curl -fsSL $G/goreleaser/nfpm/releases/download/v2.41.1/nfpm_2.41.1_Linux_x86_64.tar.gz | tar xz nfpm
# AppImage: appimagetool (extracted - no FUSE needed for building) + the type2 runtime
mkdir -p appimage
[ -f appimage/runtime-x86_64 ] || curl -fsSL -o appimage/runtime-x86_64 $G/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64
[ -x appimage/squashfs-root/AppRun ] || { curl -fsSL -o appimage/appimagetool-x86_64.AppImage \
    $G/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage && chmod +x appimage/appimagetool-x86_64.AppImage && \
    (cd appimage && ./appimagetool-x86_64.AppImage --appimage-extract >/dev/null); }
[ -d x86_64-unknown-linux-musl ] || { curl -fsSL -o musl.tar.xz $G/cross-tools/musl-cross/releases/latest/download/x86_64-unknown-linux-musl.tar.xz && tar xf musl.tar.xz; }
echo "sources and tools ready in $PKGROOT"
