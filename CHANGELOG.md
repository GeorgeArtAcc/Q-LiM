# Changelog

## 1.3.3 — 2026-10-28
- Dragging on the ruler of the waveform or of the MIDI editor (or with the middle mouse button) hides the mouse
  cursor and does not stop at the edge of the screen, so one long drag is enough (no need to grab it again). When
  the button is released the cursor appears again where the drag started.

## 1.3.2 — 2026-10-28
- **MIDI editor**:
  - Notes move finely, as much as the zoom allows (to 1/960 of a beat), and a light **magnet** sticks a note exactly
    to a grid line when it comes near it (12 pixels). Zoomed out the lines are close, so the notes always land on
    the grid; zoomed in they can go anywhere between the lines. The same for the length (right edge).
    `Shift` = no magnet.
  - **Ctrl + drag** a note = copy it (the copies move, the originals stay).
  - **Copy / paste notes**: select notes, `Ctrl+C` (or `Ctrl+X`), click on an empty place in the grid (an orange line
    shows where), `Ctrl+V`. The notes keep their pitches; `Ctrl+V` again puts the next copy right after.
  - **Quantize** (button in the header, or `Ctrl+U`): the starts of the selected notes (all notes if none is selected)
    go to the nearest line of the grid (1/16 when the grid is off).

## 1.3.1 — 2026-10-28
- **Metronome**: the button next to the BPM field. While clips play it ticks on every beat of the master tempo
  (higher and louder on the 1st beat of the bar). It is added after the master volume, so it is heard even with the
  master turned down. Saved in the project.
- **MIDI editor**:
  - The ruler works like the waveform's: drag on it up/down = zoom (around the place you grabbed), left/right = move;
    the middle mouse button does the same anywhere in the notes. **Double click on the ruler = play the clip from
    there** (the beat counter and the metronome follow that place).
  - When zoomed in, the ruler shows beats and sixteenths too ("3.2", "3.2.3").
  - The grid setting (1/4 … 1/32, Off) can now really be seen: the lines of the chosen grid are clearly drawn, and
    every second bar is a little darker.
  - The − / + length buttons are gone: the length is changed only by dragging the triangle at the end of the ruler.
- **The buffer size in Edit → Audio really applies now**:
  - PipeWire: the graph is set to that buffer while Q-LiM plays (`node.force-quantum`, PipeWire 0.3.65+). Before it
    was only a request, and the system could keep a bigger one (e.g. 1024).
  - PortAudio (ALSA/JACK/WASAPI/DirectSound): the delay asked from the driver follows the buffer (before, the
    driver's "low" default was used, often 20–40 ms whatever the buffer was).
  - The DSP window (click on DSP) shows the buffer really in use and its delay in ms ("(asked N)" when the system
    gives another one).

## 1.3.0 — 2026-10-28
- **MIDI clips**: double click an empty cell of an instrument channel (or right click → New MIDI clip). They launch,
  loop, follow the master tempo and start on the beat/bar like audio clips (sample accurate). Copy/cut/paste,
  undo/redo and the project file keep their notes. A small picture of the notes is drawn in the cell.
- **MIDI editor (piano roll)** in place of the waveform when a MIDI clip is selected: double click = add / delete a
  note, drag = move (time + pitch), drag the right edge = length, drag on empty space = select a box, Shift = add to
  the selection / off the grid. Velocity lane, grid (1/4 … 1/32, off), clip length (− / + / ×2, or drag the triangle
  in the ruler), Loop on/off. Keys: Delete, ↑↓ (Shift = octave), ←→ (one grid step), Ctrl+A, Esc. Click the piano keys
  to hear them; Ctrl+wheel = zoom, Shift+wheel = sideways.
- **.mid files**: drop one on a cell (or load it from the library) → a MIDI clip with its notes (all tracks merged).
- **Recording** (Session View style):
  - The ● button on each channel header arms it. An empty cell of an armed channel shows a red circle: click it to
    record, click the cell again to stop. Start and stop follow the launch quantization (e.g. whole bars), and the new
    clip goes on playing, looped, in time.
  - An **instrument channel records MIDI** (from the MIDI keyboard or the computer keys) into a MIDI clip; the notes
    appear while you play.
  - Other channels **record audio** from the sound card's input (24-bit WAV in the project's `Recordings` folder, or
    `~/Q-LiM Recordings` before the project is saved). The delay of the card is compensated.
  - Right click on ● : which input(s) (a stereo pair or one mono input) and **monitoring** (hear the input through the
    channel and its plugins). Saved in the project.
  - "Stop all" also ends the recordings at once.
- Audio input: PortAudio opens the card's input together with its output (ALSA/JACK/ASIO/WASAPI); PipeWire opens the
  system's default input (stereo) only while a channel is armed.
- New tests: tests/test_midiclip.c (MIDI clips, with the DPF Kars synth) and tests/test_rec.c (recording, with a
  "frame counter" input that shows exactly which frames were recorded).

## 1.2.1 — 2026-10-28
- New wordmark in the top bar: "Q", the waveform with the orange X (in place of the dash) and "LiM" in one sign,
  exactly in the middle of the window. The separate "Q-LiM" text at the top left is gone, so the Launch / BPM
  controls start at the left edge and there is more room.

## 1.2.0 — 2026-10-28
- **VST instruments** (VST3, VST2, CLAP, LV2): add one to a channel with `+` (instruments are marked `INST` and go
  first in the chain; effects after it process its sound). Several instruments on one channel are layered.
- **MIDI keyboard → instrument**: the keyboard plays the selected instrument channel (like auto-arm in Ableton).
  Selecting a channel without an instrument keeps the last one, so you can launch clips while you play.
  A note always ends on the channel it started on. Pads bound to clips/buttons do not play notes, and nothing plays
  while a MIDI learn waits. CC (sustain pedal, mod wheel), pitch bend and aftertouch go to the instrument too.
- **Computer keys** (`Ctrl+K` or the button in the panel): `A W S E D F T G Y H U J K O L P` = notes,
  `Z`/`X` = octave, `C`/`V` = velocity.
- **Device panel** (like Bitwig): click a channel name and, in place of the waveform, its plugins appear one next to
  another, each with on/off, a window button and knobs for its parameters (8 per page; drag, Shift = fine, mouse wheel).
  `+` at the end adds a plugin. Right click on a device: open window, bypass, **move left/right**, remove.
- A small keyboard icon on the header of an instrument channel (filled = it gets the MIDI keyboard; click = play it).
- Loading a project or a new project sends "all notes off".
- New tests: tests/test_inst.c (all 4 formats, also built for Windows) and tests/test_instmix.c (routing in the mixer),
  with the open-source DISTRHO Kars and Nekobi synths.

## 1.1.0 — 2026-10-28
- Launch quantization, like "Global Quantization" in Ableton Live / Bitwig: the "Launch" button in the top bar
  (Now / 1 Beat / 1/2 Bar / 1 Bar / 2 Bars / 4 Bars, default 1 Bar; left click = next, right click = previous).
  While something plays, a pressed clip or scene waits (its cell blinks; red = waits to stop) and starts exactly on
  the next beat/bar line, sample accurate. The count starts with the first clip launched; "Stop all" resets it.
  A scene stops the channels it has no clip in on the same line. The setting is saved in the project.
- Master tempo (BPM field in the top bar, with a beat counter). The grid follows it.
- Per clip: `[ clip BPM ] [WARP]`. The BPM field is the clip's own tempo (fix it if detection was wrong);
  with WARP on, the clip is time-stretched to the master tempo.
  **Change:** older projects that had their own "playing tempo" per clip now follow the project's master tempo.
- New test: tests/test_quant.c (also built for Windows).

## 1.0.5 — 2026-10-27
- Turning the tempo tick (Rubber Band time-stretch) on while a clip plays no longer clicks: the old sound fades out
  and the stretched sound fades in. Turning it off is smooth too.
- During an automatic crossfade a "ghost" handle shows on each fader how loud that channel really is at that moment.
  The fader setting itself is not changed.
- New test: tests/test_warpsw.c (no click or crackle when the tempo tick is switched or the tempo is dragged).

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
