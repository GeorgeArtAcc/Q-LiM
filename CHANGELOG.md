# Changelog

## 1.0.4 — 2026-10-25
- The author is shown with his full name: Georgi Stanimirov Ivanov (GitHub: GeorgeArtAcc), in the program, the packages and the documentation.

## 1.0.3 — 2026-10-25
- One source tree for Linux and Windows (the Windows code lives in `src/win/` and a few `#ifdef _WIN32` blocks).
- The sound card and driver are also remembered by name, so adding a new card or driver no longer changes your choice.
- Windows: the font is also looked for next to `qlim.exe`.

## 1.0.2 — 2026-10-25
- Linux: AppImage (`make -f packaging/Makefile.pkg T=gnu appimage`).
- Windows: fixed — closing did not ask whether to save changes, and Undo did not work
  (the temporary file for project snapshots was created in `C:\`, where a normal user may not write).

## 1.0.1 — 2026-10-24
- Linux packages: `.deb`, `.rpm` (glibc 2.34+) and `.apk` (Alpine/musl), with the libraries built in.
- `qlim --version`.
- Windows: VST3, VST2 and CLAP plugins with their windows; ASIO (Steinberg ASIO SDK, GPLv3 option) with an
  "ASIO Panel" button; plugins are scanned in a separate process; helper DLLs in plugin folders are ignored.

## 1.0.0 — 2026-10-24
First stable version.
- Large stability campaign (fuzzing, sanitizers, GUI monkey tests, long soak runs). Fixed: two fast clicks
  could turn into a drag, a rare crash after stopping a clip with time-stretch on, two thread races,
  editing edge cases.
- All code comments in English.
- First Windows build (Windows 7 SP1 – 11, 64-bit): installer, UTF-8 file names, native dialogs,
  WASAPI/DirectSound/MME/WDM-KS, `.limproject` file association.

## Before 1.0.0
Development versions 0.x (steps 1–36): clip grid, scenes, queue with crossfade, mixer and routing,
MIDI learn, waveform editor with non-destructive editing, Rubber Band time-stretch, BPM detection,
native PipeWire output and graph, VST3/VST2/LV2/CLAP hosting, autosave and crash recovery, themes.
