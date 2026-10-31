# Changelog

## 1.4.0 — 2026-10-31
- **JACK on Linux, when it is there.** The Linux packages now contain PortAudio's JACK driver, without depending on
  JACK: Q-LiM loads libjack itself when it starts (`src/jack_shim.c`).
  - JACK server running (jackd, or PipeWire's JACK): **"JACK Audio Connection Kit"** appears in Edit → Audio;
  - JACK installed but not running, or not installed at all: nothing changes - PipeWire and ALSA as before.
  - The terminal says which it is (`>>> JACK: libjack found (...)` / `>>> JACK: not installed ...`).
  - For older systems and for people who prefer JACK. Tested with a JACK2 server (dummy driver): Q-LiM plays
    through it; and with libjack missing: Q-LiM starts as always.
- **Behind dialogs** the window is now always darkened (as in Eco), never blurred - in every interface mode. It is
  also lighter: the window behind is not captured and blurred any more.
- README: the newer features (Fit, interface size, hiding the master / meters, crash report, `[ERROR]` marks).
- Together with 1.3.4 – 1.3.16 (see below): much less CPU on old computers (smart redraw, Eco, the light CPU drawing,
  the cheaper waveform), Edit → Performance, interface size, Fit, the crash report and a clean log.

## 1.3.16 — 2026-10-31
- **Pan knobs** (channels and master) no longer jump to where you click: they are dragged from where they are, like
  the faders (up = right, down = left, Shift = fine, a light stop in the centre). Double click = centre, as before.

## 1.3.15 — 2026-10-31
- **Fit** (a new button next to the pointer and the scissors, above the waveform): shows the waveform exactly from
  the Start to the End marker (with a little air, so they can be grabbed) and turns SCROLL off - handy after zooming
  and moving around.
- **Double click on a pan knob** (channels and master) puts it back in the centre - now also with the system's
  double-click timing, so a slower double click works too.
- **The log** (top right) no longer shows errors for harmless messages: PortAudio / JACK looking for sound cards
  ("Expression ... failed", "Cannot connect to server"), the zenity windows, a missing Music folder, "0 failed",
  formats read through ffmpeg, X11 notes from plugin windows. A damaged mp3 frame is a warning, not an error.
- **The terminal** shows the same marks as the log: `[ERROR]` and `[WARN]` in front of those lines, so the
  terminal output can be copied and sent as it is.

## 1.3.14 — 2026-10-31
- **The waveform costs much less, and looks the same.** Q-LiM now draws it itself into one picture: a still view
  costs nothing, and a moving one (SCROLL, follow) computes only the new columns. Before → now, main thread, light
  CPU drawing, 60 frames per second, a clip playing: waveform 11 % → 7 % (hidden: 7 %), SCROLL 33 % → 20 %.
- **Edit → Performance → Waveform: Auto / Smooth / Light.** Light: in SCROLL the waveform moves 15 times per second
  and in whole pixels: SCROLL 20 % → 9 %. Auto = light together with Eco (Eco: SCROLL 16 % → 7 %).
- **Clip lamps** above the meters light only while the sound is too loud (and a moment after, so a short peak is
  seen) and then go out by themselves - like on a mixing desk.
- **The log** no longer counts a line as an error because of a file or song name (e.g. a song called "Lost …" or a
  folder called "crash"): names in quotes are not looked at.

## 1.3.13 — 2026-10-30
- **No false errors in the log**: mp3 files with an empty tag field (often files from YouTube: "No comment text /
  valid description?") are no longer counted as errors - the sound was always fine. "Collect All & Save" no longer
  shows an error when every file was copied ("copied 9 files, all OK").
- **Crash report**: if Q-LiM ever crashes, it writes `~/.config/qlim/crash-report.txt` (where it happened and the
  last 150 lines of the log), so it can be sent later even when Q-LiM was not started from a terminal.

## 1.3.12 — 2026-10-30
- **Eco**: the outline under the mouse no longer lands on the clips behind an open menu (File / Edit), a context
  menu, a list or a dialog, and the thin edges that are dragged (the top of the fader panel, the waveform bar, the
  master edge) always win, with the resize mouse cursor.
- **Hidden level meters** (Ctrl+Shift+U) now leave only the **dB scale** (0, 6, 12, 24, 48 …) next to the faders
  and the master - a ruler, no boxes, no clip lamps, nothing that moves.
- **The top bar** (Launch, BPM, metronome, the logo, the library button, DSP) keeps its normal size when the
  interface is zoomed - like the menu bar and the dialogs. The DSP window too.
- **Interface size** moved from Edit → Performance to **Edit → Appearance**.

## 1.3.11 — 2026-10-30
- **Dialogs keep their normal size** when the interface is zoomed (Ctrl+wheel / Ctrl+= / Ctrl+-): only the main
  window grows or shrinks.
- **Eco**: scrolling the clips no longer lights up the clips under the mouse.
- **Eco**: a very simple highlight instead of none - a thin outline around the thing under the mouse, drawn at most
  15 times per second. Thin places that can be dragged (the edge of the master, the size bars of the faders and the
  waveform) also show the resize mouse cursor again.
  Measured with the mouse moving fast and clips playing: main thread about 6 % (graphics card and CPU drawing).

## 1.3.10 — 2026-10-30
- **Hide the master**: drag its left edge far to the right, or **Ctrl+Shift+M**. A thin handle stays on the right:
  click it (or Ctrl+Shift+M again) and the master is back. Remembered.
- **Hide the level meters**: **Ctrl+Shift+U** or Edit → Performance → "Level meters". The meters of the channels and
  the master stay empty boxes - nothing moves there, so nothing is drawn again. Remembered.
- **Interface size**: **Ctrl+wheel** over the clips, **Ctrl+=** / **Ctrl+-**, **Ctrl+0** = 100 %, or the buttons in
  Edit → Performance (50 % … 250 %). The menu bar and its menus keep their normal size. Remembered.
  (Over the waveform and the MIDI editor, Ctrl+wheel still zooms those.)
- Together with Ctrl+W (waveform) and Ctrl+F (faders), the window can be reduced to the clips alone.

## 1.3.9 — 2026-10-30
- **Redrawing only what changed is now the normal way** (every interface, every computer): Q-LiM records each
  frame, compares it with the last one in tiles and draws again only the changed tiles. The look stays the same
  (soft edges, the blurred background behind dialogs - now made once when a dialog opens, from the window as it
  was). Measured with the mouse moving and clips playing: main thread 40 % → 25 %, at 60 frames per second.
  `QLIM_SMART=0` turns it off (the old way, for comparison).
- **Eco** now goes much further, for old computers - everything for the sound:
  - no highlights under the mouse: a mouse that only moves does not wake the window at all; it counts again as
    soon as a button, the wheel or a key is used (clicks, drags, menus, scrolling work as before);
  - at most 30 frames per second, no soft edges, no blur, a smaller text cache (less RAM).
  - Measured with the mouse moving fast and clips playing: main thread 8 % (graphics card) / 5 % (CPU drawing).

## 1.3.8 — 2026-10-30
- **Eco interface** (Edit → Performance → Interface: Auto / Normal / Eco). Q-LiM records what it wants to draw,
  compares it with the last frame in small tiles and draws again **only the tiles that changed** (while clips play:
  about 1 % of the window); nothing changed = nothing drawn and nothing sent to the screen. Shapes are drawn
  without soft edges and the window behind dialogs is darkened instead of blurred. Measured (60 frames per
  second, 3 clips playing, MIDI editor open): light CPU drawing 29 % → 6 % of one core; OpenGL main thread
  39 % → 10 %. **Auto** = Eco with the light CPU drawing, Normal otherwise.
- `QLIM_ECO=1` turns Eco on for one run; `QLIM_FPS=1` also shows how much of the window Eco draws per frame.

## 1.3.7 — 2026-10-30
- **Edit → Performance…** (new): how the window is drawn and how often, so the sound always has room.
  - **Drawing**: Auto (the graphics card, or the light CPU drawing when there is no acceleration) / Graphics card /
    Light (CPU). Applies at the next start. The dialog shows how it is drawn right now.
  - **Frames per second**: Auto, or at most 60 / 30 / 20 / 12. **Auto** measures all the time how much CPU one frame
    takes and draws as many frames as fit in about 40 % of one CPU core (a fast computer keeps its full rate, an
    old one gets fewer, but steady frames).
  - **Fewer frames while nothing moves** (10 per second; on by default).
  - **Sound first** (on by default): when the sound breaks up - the sound card, reading from the disk, WARP
    (Rubber Band) or processing over 100 % - Q-LiM draws at most 15 frames per second for 30 seconds and says why
    in a message. The dialog shows the dropout counts.
- The background work nobody waits for (the waveform overview, finding the tempo of a file) now runs with a lower
  priority, so it never takes time from the sound or the picture. On Windows, reading from the disk and WARP get a
  higher priority.

## 1.3.6 — 2026-10-30
- Fixes for 1.3.5 on computers without graphics acceleration:
  - Opening a dialog (Edit → Audio, etc.) crashed Q-LiM: the blurred background read the whole drawing image (as
    big as the screen) into a buffer the size of the window.
  - The waveform (and anything else that changes across the whole width) was not sent to the screen: a run of
    changed parts that reached the right edge of the window was forgotten.

## 1.3.5 — 2026-10-30
- Computers without graphics acceleration (ThinkPad T60): the sound still crackled while the window was visible
  and stopped as soon as it was minimized - every frame went whole through the X server. Now the picture is drawn
  into Q-LiM's own image and only the parts that changed since the last frame are sent to the window (while clips
  play that is about 1 % of the window instead of all of it). While clips play such a computer redraws 12 times per
  second (`QLIM_PLAY_FPS=<n>` changes it), at most 25 while you work.
- `QLIM_FPS=1` also shows how much of the window is sent per frame.

## 1.3.4 — 2026-10-30
- **Much less CPU, above all on old computers** (found on a ThinkPad T60: 60–70 % CPU with nothing playing, and the
  sound crackled):
  - Graphics chips without a working driver (Mesa's "llvmpipe", e.g. the Intel GMA 950) made every frame on ALL the
    CPU cores and left too little for the sound. Q-LiM now notices it and draws with SDL's light software renderer
    on one core (shapes as whole-pixel spans, no soft edges), at most 30 frames per second. Measured: playing
    117 % → 18 %, nothing playing 137 % → 9 %. `QLIM_RENDERER=gpu` keeps OpenGL anyway.
  - On every computer: while nothing plays and nobody touches anything, the window is redrawn only 10 times per
    second (it reacts to the mouse and the keys at once, as before). While clips play: full speed with a graphics
    card, 20 frames per second without one. Plugin windows open = full speed.
  - `QLIM_FPS=1` writes the frames per second and the work per frame into the log every 5 seconds.

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
