#!/bin/sh
# Q-LiM - test files for test_sniff (needs ffmpeg). GPL-3.0-or-later.
d=${1:-/tmp/qlim_sniff}; mkdir -p "$d"; cd "$d" || exit 1
f() { ffmpeg -v error -nostdin -y -f lavfi -i "sine=f=440:d=1:r=48000" -ac 2 "$@"; }
f a.wav; f a.flac; f -c:a libvorbis a.ogg 2>/dev/null || f a.ogg; f -c:a libopus a.opus; f a.aiff
f -c:a aac a.m4a; cp a.m4a m4a_named.mp3; f -c:a aac -f adts a.aac
f -c:a libmp3lame -id3v2_version 0 -write_xing 0 a.mp3; f -c:a libmp3lame -id3v2_version 3 -metadata title=x id3.mp3
cp a.mp3 mp3_named.m4a; cp a.wav wav_named.mp3; cp a.flac flac_named.wav
head -c 20000 /dev/urandom | tr '\377' '\0' > junk.mp3
