#!/bin/sh
# Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE)
# Downloads everything needed to build the Windows version on Linux into /opt/win (or $WIN):
#   llvm-mingw (compiler), SDL2 + SDL2_ttf (mingw builds), libsndfile (win64), PortAudio (built here),
#   RtMidi (source), Wine 10 (to run the installer compiler and for quick tests), Inno Setup 6 (installer),
#   Steinberg ASIO SDK 2.3.4 (used under its GPLv3 option; for ASIO in PortAudio).
# Then:  make -f Makefile.win installer
set -e
WIN=${WIN:-/opt/win}
G=https://github.com
mkdir -p "$WIN" && cd "$WIN"
get() { [ -f "$2" ] || curl -fsSL -o "$2" "$1"; }

get $G/mstorsjo/llvm-mingw/releases/download/20241217/llvm-mingw-20241217-msvcrt-ubuntu-20.04-x86_64.tar.xz llvm-mingw.tar.xz
get $G/libsdl-org/SDL/releases/download/release-2.30.10/SDL2-devel-2.30.10-mingw.tar.gz sdl2.tar.gz
get $G/libsdl-org/SDL_ttf/releases/download/release-2.22.0/SDL2_ttf-devel-2.22.0-mingw.tar.gz ttf.tar.gz
get $G/libsndfile/libsndfile/releases/download/1.2.2/libsndfile-1.2.2-win64.zip sndfile.zip
get $G/Kron4ek/Wine-Builds/releases/download/10.0/wine-10.0-amd64-wow64.tar.xz wine10.tar.xz
get $G/jrsoftware/issrc/releases/download/is-6_3_3/innosetup-6.3.3.exe is.exe
# ASIO SDK (Steinberg's site; if it cannot be downloaded, put the zip here by hand as asiosdk.zip)
get https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip asiosdk.zip || true
[ -f asiosdk.zip ] && echo "d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca  asiosdk.zip" | sha256sum -c - \
    && { mkdir -p asiosdk && python3 -c "import zipfile; zipfile.ZipFile('asiosdk.zip').extractall('asiosdk')"; }
for f in llvm-mingw.tar.xz sdl2.tar.gz ttf.tar.gz wine10.tar.xz; do tar xf "$f"; done
python3 -c "import zipfile; zipfile.ZipFile('sndfile.zip').extractall('.')"
[ -d portaudio ] || git clone -q --depth 1 -b v19.7.0 $G/PortAudio/portaudio.git
[ -d rtmidi ] || git clone -q --depth 1 -b 6.0.0 $G/thestk/rtmidi.git

L=$WIN/llvm-mingw-20241217-msvcrt-ubuntu-20.04-x86_64/bin
# PortAudio with ASIO + WASAPI + WDM-KS + DirectSound + MME -> $WIN/prefix-asio (Makefile.win uses it)
[ -d asiosdk/ASIOSDK ] || { echo "ASIO SDK missing (asiosdk.zip) - see above"; exit 1; }
(cd portaudio && cmake -S . -B build-asio -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=$L/x86_64-w64-mingw32-gcc \
    -DCMAKE_CXX_COMPILER=$L/x86_64-w64-mingw32-g++ -DCMAKE_RC_COMPILER=$L/x86_64-w64-mingw32-windres \
    -DCMAKE_BUILD_TYPE=Release -DPA_BUILD_SHARED_LIBS=OFF -DPA_USE_ASIO=ON -DASIOSDK_ROOT_DIR=$WIN/asiosdk/ASIOSDK \
    -DPA_USE_WASAPI=ON -DPA_USE_WMME=ON -DPA_USE_DS=ON -DPA_USE_WDMKS=ON -DCMAKE_INSTALL_PREFIX=$WIN/prefix-asio >/dev/null && \
    cmake --build build-asio -j2 >/dev/null && cmake --install build-asio >/dev/null)

# Wine prefix with Inno Setup (no .NET/Gecko prompts)
export WINEPREFIX=$WIN/wp10 WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
W=$WIN/wine-10.0-amd64-wow64/bin/wine
$W wineboot -i >/dev/null 2>&1 || true
$W is.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DIR="C:\\InnoSetup" >/dev/null 2>&1
echo "Windows toolchain ready in $WIN"
