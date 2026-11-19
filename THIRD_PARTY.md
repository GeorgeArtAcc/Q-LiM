# Third-party software

Q-LiM (GPL-3.0-or-later) uses the following software. Thank you to all of their authors.

## In this repository
See [third_party/README.md](third_party/README.md): Rubber Band Library (GPL-2.0-or-later), Signalsmith Stretch (MIT), the ONNX Runtime C API header (MIT), PipeWire/SPA headers (MIT),
CLAP headers (MIT), VST 3 pluginterfaces (MIT), LV2 and lilv headers (ISC).

## Linked when building
| Library | License | Linux packages | Windows (64-bit; the 32-bit `qlim.exe` has everything built in) |
|---|---|---|---|
| [SDL2](https://www.libsdl.org/), [SDL2_ttf](https://github.com/libsdl-org/SDL_ttf) (with FreeType, HarfBuzz) | zlib (FreeType: FTL, HarfBuzz: MIT) | built in | DLL |
| [libsndfile](https://libsndfile.github.io/libsndfile/) with libogg, libvorbis, FLAC, Opus, mpg123, LAME | LGPL-2.1+ (codecs: BSD / LGPL) | built in | DLL |
| [PortAudio](https://www.portaudio.com/) | MIT | built in (with its JACK driver) | built in |
| [JACK](https://jackaudio.org/) API headers ([jackaudio/headers](https://github.com/jackaudio/headers)) | LGPL-2.1+ | headers only, at build time; libjack is loaded at run time if installed (`src/jack_shim.c`) | — |
| [RtMidi](https://github.com/thestk/rtmidi) | MIT-style | built in | built in |
| [Steinberg ASIO SDK](https://www.steinberg.net/developers/asiosdk-open/) 2.3.4 | GPL-3.0 option | — | built in |
| ALSA (libasound), X11 (libX11) | LGPL-2.1+ / MIT | from the system | — |
| PipeWire (libpipewire-0.3), lilv | MIT / ISC | loaded at run time if present | — |
| LLVM libc++, libunwind; winpthreads | Apache-2.0 WITH LLVM-exception; MIT | 32-bit glibc package: libc++, libunwind, compiler-rt built in | DLL |
| [glibc_version_header](https://github.com/wheybags/glibc_version_header) | MIT | 32-bit glibc package: at build time only (old glibc symbol versions) | — |

## Assets
- DejaVu Sans font (`assets/DejaVuSans.ttf`) — Bitstream Vera / DejaVu license (free).
- Inter Display font, SemiBold (only the letters of the name at the top, in `src/logo_font.c`) — Copyright 2016-2023 The Inter Project Authors, SIL Open Font License 1.1 (`third_party/inter/OFL.txt`).

## Trademarks
VST is a registered trademark and ASIO is a trademark of Steinberg Media Technologies GmbH.

## Downloaded on first use (not part of Q-LiM)
- **Separate Vocals** (1.4.14) downloads, after the user agrees: ONNX Runtime 1.22.0 (Microsoft, MIT) and the
  Mel-Band RoFormer vocal model (`mel-roformer.fp16.onnx`): weights by Kimberley Jensen (MIT), architecture by
  Wang et al. (ByteDance), code by Phil Wang and ZFTurbo, ONNX export by audiojs (huggingface.co/audiojs/mel-roformer, MIT).
