# Bundled third-party code

Everything here is used unchanged. Each folder keeps its own license file.

| Folder | What | License | Used for |
|---|---|---|---|
| `rubberband/` | [Rubber Band Library](https://breakfastquay.com/rubberband/) 4.0.0 | GPL-2.0-or-later | Tempo change without pitch change. Compiled as a single file (`single/RubberBandSingle.cpp`). |
| `pipewire/` | PipeWire 1.0.5 / SPA headers (only the needed ones) | MIT | Direct PipeWire output on Linux. `libpipewire-0.3.so.0` is loaded at run time if present. |
| `clap/` | [CLAP](https://github.com/free-audio/clap) headers | MIT | Hosting CLAP plugins |
| `vst3/` | Steinberg VST 3 `pluginterfaces` | MIT | Hosting VST3 plugins |
| `lv2/`, `lilv/` | LV2 and lilv headers | ISC | Hosting LV2 plugins on Linux (`liblilv-0.so.0` is loaded at run time) |

Not in this repository (downloaded when building): the Steinberg ASIO SDK (Windows, used under its GPLv3 option),
SDL2, SDL2_ttf, libsndfile, PortAudio, RtMidi and the other libraries listed in [THIRD_PARTY.md](../THIRD_PARTY.md).
