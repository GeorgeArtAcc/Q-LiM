# Third-party software

Q-LiM (GPL-3.0-or-later) uses the following software. Thank you to all of their authors.

## In this repository
See [third_party/README.md](third_party/README.md): Rubber Band Library (GPL-2.0-or-later), PipeWire/SPA headers (MIT),
CLAP headers (MIT), VST 3 pluginterfaces (MIT), LV2 and lilv headers (ISC).

## Linked when building
| Library | License | Linux packages | Windows |
|---|---|---|---|
| [SDL2](https://www.libsdl.org/), [SDL2_ttf](https://github.com/libsdl-org/SDL_ttf) (with FreeType, HarfBuzz) | zlib (FreeType: FTL, HarfBuzz: MIT) | built in | DLL |
| [libsndfile](https://libsndfile.github.io/libsndfile/) with libogg, libvorbis, FLAC, Opus, mpg123, LAME | LGPL-2.1+ (codecs: BSD / LGPL) | built in | DLL |
| [PortAudio](https://www.portaudio.com/) | MIT | built in | built in |
| [RtMidi](https://github.com/thestk/rtmidi) | MIT-style | built in | built in |
| [Steinberg ASIO SDK](https://www.steinberg.net/developers/asiosdk-open/) 2.3.4 | GPL-3.0 option | — | built in |
| ALSA (libasound), X11 (libX11) | LGPL-2.1+ / MIT | from the system | — |
| PipeWire (libpipewire-0.3), lilv | MIT / ISC | loaded at run time if present | — |
| LLVM libc++, libunwind; winpthreads | Apache-2.0 WITH LLVM-exception; MIT | — | DLL |

## Assets
- DejaVu Sans font (`assets/DejaVuSans.ttf`) — Bitstream Vera / DejaVu license (free).

## Trademarks
VST is a registered trademark and ASIO is a trademark of Steinberg Media Technologies GmbH.
