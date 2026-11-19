# Changelog

## 2.0.0 — 2026-11-19
- **Song separation (File -> Separate) works again.** Microsoft took the ONNX Runtime project off GitHub on
  17.11.2026, together with its downloads, so the first separation stopped with "Could not download ONNX Runtime:
  curl: (22) The requested URL returned error: 404". Q-LiM now gets the same official ONNX Runtime 1.22.0 from
  Microsoft's NuGet package (nuget.org) and keeps only the one library from it. If separation was already set up on
  a computer, nothing is downloaded again. On Linux the package is unpacked with `unzip`, `python3` or `bsdtar`.
  (Corrected packages of 2.0.0, package number 3.)
- **A fresh look.** Five new themes (Graphite, Slate, Aurora, Paper, Ember) and a **Modern** style next to the
  Classic one (Edit -> Appearance): rounder corners, the channel colour as a strip on the headers, clips tinted in their
  channel's colour. The light themes (Paper - a calm grey, never bright white - and Light) have clear outlines.
  **Clip time**: a playing clip shows its time as a counter, a thin progress line, or both. The name at the top is now
  plain letters (Inter Display font), placed in the middle of the free space between the buttons (where the eye sees
  the centre). The scene-transition buttons (◀, Next, the fade time, AUTO) moved to the right, next to the library
  folder. **Smooth rounded corners** everywhere, also in the Eco look and with the light (CPU) drawing - before, the
  curves there were drawn in whole pixels and showed steps. The colour strip of the headers and the DSP meter's bar
  stay exactly inside their rounded outline.
  **Clearer colours**: in the Modern style the channel colours are more saturated and the clips more clearly tinted;
  brighter text and a clearer second text in the new themes, Solarized and Nord (Solarized looked like a colour filter
  over everything); the light themes have a dark, clear Master fader.
- **Eco look**: the frame around the thing under the mouse no longer goes over open menus and popups; the right-click
  menu of a clip is as wide as its longest line ("Add to queue (nothing playing)" went out of it).
- **Windows: the MIDI controllers you use are remembered again** (the MIDI mappings were kept, but not which
  controller: "safe_commit: cannot replace ...midi.conf" - Windows refused to replace the old file from that part of
  the program, so the list was never updated after the first save). Saving a file also tries again for a moment when
  an antivirus or the search indexer holds the old one.
- **Edit -> About**: an optional way to support the project (Revolut link and QR code). Q-LiM stays free.
- **Alpine Linux**: the .apk installs with `apk add --allow-untrusted` again on Alpine 3.23+ ("v2 package format
  error" with apk v3): the packaging tool wrote the folders' permissions in a form apk v3 does not accept - now fixed
  after packaging (`packaging/apk-fix.py`) and checked with apk 3.0.8; the description is also on one line.
- **Old graphics cards + USB sound cards (Linux)**: `/opt/qlim/qlim-old-gpu-fix.sh on|off|status` - turns off the
  graphics card's 2D acceleration, which on some old laptops (found on a ThinkPad T60, ATI Radeon X1400) breaks the
  stream of USB sound cards (clicks while the screen changes). See the README.
- **Plugins that froze or crashed Q-LiM on Linux now open and work** (reported with Vitalium, GitHub issue #2).
  The Linux packages offered some of Q-LiM's own insides to every plugin: a plugin asking the system for `hypotf`
  (used by JUCE to draw lines) got Q-LiM's helper instead, which went round in an endless loop - the plugin window
  never opened and Q-LiM stopped answering. The same could happen with other math functions and with the libraries
  built into Q-LiM (C++ library, FLAC, Ogg, Vorbis, HarfBuzz ...), for any plugin type (LV2, VST3, VST2, CLAP), also
  while playing. Now nothing of Q-LiM is visible to plugins (and the build refuses to finish if something is).
- **Plugins in a separate process ("sandbox", like Bitwig's crash protection), Linux and Windows (XP too).** A plugin there cannot crash
  or freeze Q-LiM: if it crashes or stops answering, only that plugin is silent for a moment, Q-LiM says so and starts
  it again with its last settings (up to 3 times a minute). The sound waits for it at most two blocks, never more.
  Its window is its own window in that process; opening it never stops Q-LiM's screen.
  Edit -> Plugins -> **Safety**: *Never* / *Automatic* (default) / *All plugins*. *Automatic* = plugins that crashed
  or froze Q-LiM before (Q-LiM notes which plugin it was and moves it there next time by itself), the ones you choose
  (right click a plugin -> *Run in a separate process*; its settings go with it) and LV2 plugins whose window needs Gtk.
- **Plugins with many inputs / outputs work** (any number, e.g. LSP Crossover x8, LSP Mixer x16, LSP Multi-Sampler x48 DirectOut with 98 outputs):
  Q-LiM's stereo goes to the first two inputs (the rest get silence) and comes from the first two (main) outputs.
  Before they were refused ("too many channels"). In a separate process only the two used channels cross between the processes (less memory).
- **Plugin scan: the time limit is per plugin**, not per file: a file with many plugins (LSP has ~200 in one LV2
  bundle) no longer runs out of time on a slow computer, and a plugin that crashes or hangs in the middle no longer
  hides the ones after it (a new scan process continues after it).
- **LV2 plugins with Gtk windows** (e.g. Calf; Gtk 3 and Gtk 2) open - in a separate process, so Gtk never mixes with
  Q-LiM. (Qt-only LV2 windows are still not supported.)
- **LV2 plugins with an "external" window** (their own window, e.g. DISTRHO / JUCE ports, Carla) can be opened too;
  a window that asks to be closed is closed; LV2 windows without `port_event` no longer crash Q-LiM.
- **Much lighter on old computers - about half the CPU, the picture and the sound exactly the same.** Measured with 6
  playing tracks: the drawing 5.1 % -> 2.3 % of a core, Q-LiM in all 9.3 % -> 4.6 %, idle 2.7 % -> 1.2 % (the
  screenshots before / after are identical pixel by pixel).
  - Drawing without a graphics card: opaque areas are filled the fast way (SDL blended every pixel of them);
    rounded shapes, circles and gradients are recorded as a few numbers and drawn only where the window really
    changes; the window is checked in smaller pieces (32x16), so only the parts that move are drawn again (the
    meters); text widths are remembered; faster fingerprints of the drawing commands.
  - The waveform is kept in strips: a change sends only the strips that changed.
  - Sample rate conversion (e.g. 44.1 -> 48 kHz) is 3x faster and even cleaner (exact filters for every position,
    4 multiplications at once; the rest is >= 135 dB down, was >= 129 dB).
  - Less work when nothing happens: the disk thread naps when it has nothing to read, the tempo detection is not
    looked for in every cell every frame, Eco checks for input half as often.
- **Collect All & Save asks what compressed files become** (MP3, FLAC, Ogg, M4A ...): keep them as they are, or
  **WAV 32-bit float** (not one bit lost, nothing decoded while playing - the lightest), **WAV 24-bit** or **FLAC 24-bit**,
  all at the project's sample rate (no live conversion either). The names stay (song.mp3 -> song.wav). The collected
  project then plays from the new files. The old "Convert files with a different sample rate" (Edit > Audio, hidden
  copies in a cache folder) is gone - files of another rate play converted live, 3x cheaper now.
- **Drawing = Graphics card** falls back to the light drawing when the "graphics card" is only the CPU emulation
  (llvmpipe) - it took 2 x 57 % of an old laptop's CPU.
- **Edit > Performance > "Gentle on the graphics card"**: changed pictures go to the graphics card in small pieces -
  for old laptops whose USB sound clicks when the view changes (found on a ThinkPad T60: the graphics card's big
  copies disturb the USB sound card; without graphics acceleration it is clean).
- **Speaker Map** (Edit → Speaker Map..., also from the Surround window and Surround File...): a graph inside Q-LiM,
  like the PipeWire graph. Left: the signals of a channel - the channels of its surround file (L R C LFE Ls Rs ...)
  or its surround speakers; right: every output, by sound card (the main card, extra cards - Bluetooth, the
  headphones ...). Drag from a signal to an output to wire them (again = unwire), right click a wire to remove it.
  One signal can go to several outputs (e.g. the center on the main speakers and the Bluetooth ones); a signal
  without wires is silent. Default = back to the outputs in order. Saved in the project, with undo; the scene export
  records the outputs as wired.
- **Recording: a stop button and a live view.** The recording cell shows a square - it stops the recording; a click
  elsewhere on the cell only selects it. Below, an audio recording is drawn live (L / R, it grows, a red line where it
  is, Stop in the header); a MIDI recording shows in the MIDI editor as you play (the view follows it, a red line,
  REC Stop in the header).
- **The MIDI editor's pencil** (next to the arrow in its header, or B): a pale note follows the mouse where a click
  would put one, as long as the last placed / touched note; click = the note, drag = its length; Shift = no magnet;
  Esc = the arrow.
- A clip's tempo: **double click = type a number**, **Ctrl + double click = back to its own (detected) tempo**.
  A recorded clip's own tempo is the tempo it was recorded at.
- **Menus**: a new **Play** menu (Launches at a Time..., Loudness...); Separation... moved from File to Edit.
- **Cleaner code**: the main window's code (ui.c, 12 000 lines) is split into parts (top bar, channels, waveform,
  device panel, MIDI editor, settings windows); 43 functions nothing used are gone. No visible change.
- **Surround - the clip's path is in the Surround window now, in both modes.** The Circle window got the tools of the
  Room window: Speakers / Position / **Path** and the clip's timeline below (the cursor, the points' marks, Play from
  cursor, Clear path). Click in the circle = a point at the cursor's time (playing: at the playhead), drag it, right
  click = delete; nearer the middle = more spread. The lane over the waveform is gone; the waveform's **Surround**
  button opens the window.
- **MIDI editor: the note height** - **Height - / + / Fit** in its header (Fit: all the clip's notes fill the
  height), or Ctrl+Shift+wheel anywhere (Ctrl+wheel on the keys as before).
- **Surround M4A / AAC** (files only ffmpeg reads) are known as surround files now: all their channels are kept.
- **Collect All & Save** also copies a surround path's original file and the separated parts.
- Ctrl + double click on the tempo of a clip without a tempo of its own keeps its tempo (it said "---" before).
- **Stability** (found by the new stress tests - fuzzing of the engine and of project files, a random mouse/keyboard
  "monkey" for hours under AddressSanitizer, a 2-hour playback soak):
  - a use-after-free: when the audio thread was late (a loaded machine), deleting a playing clip could free its stream
    while a render still read it (a possible crash on stage);
  - at the exit, the freeze watchdog thread read the already freed session;
  - a damaged project with huge cut lengths could overflow the arithmetic;
  - opening a project / New leaked the surround setups and paths of the old one;
  - the text image cache is smaller (32 MB, Eco 12 MB; was 96 / 24).
- **Speaker Map**: wired outputs no sound card has are listed at the bottom ("Not on any sound card (silent)");
  the lanes of a surround file say where each channel really plays (as wired in the Speaker Map).
- **Ctrl+C** in the terminal = a normal quit (asks about unsaved work).
- **User manual**: docs/MANUAL.md (English) and docs/MANUAL.bg.md (Bulgarian), with a stage backup plan; the
  Shortcuts window lists the newer actions too.
- Tests: tests/test_roundtrip.c, test_collect.c, test_mc part 9, tests/monkey.c; fuzz_engine / fuzz_project cover the
  newer features. Build: packaging/setup-32bit-toolchain.sh makes the 32-bit Linux toolchain from GitHub.

## 1.4.24 — 2026-11-15
- **Surround (multichannel) files**: put a 5.1 / 7.1 / Quad ... WAV, AIFF, FLAC or Ogg file into a cell and Q-LiM asks
  how it should play (right click the clip → **Surround File...** changes it later):
  - **Multichannel**: every channel of the file on its own output (from the "First output" on: 5.1 = L R C LFE Ls Rs
    on 6 outputs). The waveform shows every channel in its own lane, with its name and output. Cuts, markers, loop,
    fades, the clip gain, the channel's fader, the queue and the scene transitions work; warp / pitch do not
    (it plays straight from the file). Other sample rates are converted live, every channel.
  - **As a surround path**: the file is mixed down to stereo and Q-LiM finds where the sound is every 0.1 s (from the
    loudness of each channel and its direction - from the file's channel layout), then keeps only the points where the
    movement changes. They become the clip's surround points - see and edit them with Surround above the waveform.
    The channel gets the file's speakers if it had none. An estimate: a sound coming from two places at once becomes
    one place. The original file is kept - switch back to Multichannel at any time.
  - **Stereo**: the first two channels, as before.
  - "The same for the other surround files just added" when several are dropped at once.
  - Export Audio of a multichannel clip: one file with all its channels or a file per channel ("name - LFE.wav"),
    with its cuts, markers and gain. Export Scene counts its outputs.
  - Saved in the project, with undo, copy / paste; files from a project are not asked about again.
  - Tested: each channel arrives exactly on its own output (also through PipeWire: peaks 0.1 ... 0.6 on outputs 1-6),
    the fader moves all, a restart has no click, cuts / 44.1 kHz / the project / the export; the analysis of a sound
    walking from L to Rs gives L (-30) ... Rs (110) with 6 points.

## 1.4.23 — 2026-11-15
- **Surround export** (Export Audio / Export Scene Audio → **Surround**):
  - **A clip through its path**: the clip moves through its surround path (Circle or Room) exactly as the mixer plays
    it - **One file** with a channel per speaker, or **A file per speaker** (mono, "name - Stage C.wav" ...).
    5.0 / 5.1 / 7.0 / 7.1 / Quad get the standard channel layout in the file (5.1 / 7.1 with an empty LFE), so other
    programs know which channel is which.
  - **A scene on every output**: everything as it plays - the surround channels, the routes to other outputs and the
    master - **One file** with a channel per output, or **A file per output** ("name - Out 3.wav" ...).
  - Stereo stays the default and works as before.
  - At the session sample rate. WAV (WAVE extensible), AIFF, Ogg and Opus can hold many channels; FLAC up to 8,
    MP3 only 2 - the formats that cannot hold that many are greyed out (a file per speaker = mono: every format).
  - Not in graph mode (Audio Settings): there the outputs are links in the graph.
  - Tested: the file is the same as what the mixer plays (difference 0), the theatre walk starts 100 % on Back L/R and
    ends 100 % on Stage C, 5.1 has 6 channels with a silent LFE and the right layout.

## 1.4.22 — 2026-11-15
- **Surround - Room mode** (Surround window → **Room**): the hall seen from above, the stage at the top. The speakers
  stand where they really are - also in the middle of the room - and the sound can be anywhere, also between them, so
  it can walk straight through the hall (the Circle mode can only go round the audience).
  - Ready layouts: **Theater (9)** (Back L/R behind the audience, House L/R at its sides, Front L/R before the stage,
    Stage L/R and Stage C at the back of the stage), Theater (6), Quad; every speaker can be dragged to its place,
    given its output, deleted, and more added (up to 16); the size of the hall and of the stage can be set.
  - Panning: DBAP (distance-based, as theatre systems do): every speaker gets 1 / distance^1.5, the power stays the
    same. Spread blurs the point (0 = sharp ... All = every speaker).
  - **Stereo** in the room: the left half goes towards the left wall, the right half towards the right wall, the
    distance between them in metres (Mono, 4, 8, 12 m, wall to wall) - whatever the path does.
  - **The path is drawn on the plan**: select the clip, then Surround above its waveform. Click on the plan = a point
    at the cursor's time (a timeline of the clip below the plan; drag the cursor or a point's mark), drag a point,
    right click deletes; every point has its own stereo width and spread. While the clip plays, a click puts a point
    where it is. Between the points the sound moves in a straight line (the width too). "Play from cursor" to hear it.
  - E.g. from the back, 12 m wide (Back L / Back R), through the hall (House, Front) to Stage C, mono - measured:
    at the start the back pair, at the end 100 % Stage C.
  - The speakers' bars on the plan show how much of the sound each one gets right now. Saved in the project, with
    undo; older projects and Q-LiM versions read the circle part as before.

## 1.4.21 — 2026-11-15
- **More than one sound card at the same time** (Edit → Sound Cards..., or the button in Routing / Audio Settings):
  add a second card, the computer's headphone output or a Bluetooth speaker next to the main card. Its outputs get the
  numbers after the main card's (e.g. the main card 1-2, the headphones 3-4, the speaker 5-6) - send any channel or the
  master to them in Routing, as many places at once as you like; a surround setup can use them too.
  - Added / removed while the sound plays; saved for the computer (`~/.config/qlim/outputs.conf`), opened again at
    every start. A card that is not there (the speaker off, a USB card unplugged) is tried again by itself.
  - The window shows every card, its outputs and how it plays: how far behind the main card it is (ms), its clock
    difference (ppm), dropouts.
  - Two cards never run on exactly the same clock: every extra card reads through a ring + a resampler (windowed sinc,
    32 taps) whose ratio a slow two-gear controller adjusts (finds the clock in ~10 s, then barely moves) - no drift,
    no growing delay, no clicks. It also converts other rates (e.g. a 44.1 kHz speaker). Measured: ~85 dB clean, the
    clock difference found exactly (150 / -320 / +700 ppm), no dropouts with irregular Bluetooth-like blocks.
  - PipeWire (every card and Bluetooth speaker is listed), ALSA / JACK (PortAudio), Windows (the main card's driver).
    Not in the PipeWire graph mode (there the channels are linked to any card in qpwgraph); the window offers to
    switch to the normal mode with one click.
- **A delay for every output** (Routing → the new **Delay (ms)** row): 0 ... 2000 ms, for the main card's outputs
  too - e.g. hold the main speakers back so they meet a Bluetooth speaker. Click a box, then drag it up / down (Shift =
  fine) or use the buttons (-10 ... +10, 0, "Same for the card"); right click = 0. A change glides over 10 ms (no
  click).
- Light: with no extra card and no delay the sound goes exactly the old way (one check per audio block).
- Routing shows the name of every card over its outputs.

## 1.4.20 — 2026-11-15
- **Fix:** opening the Surround window (right click a channel → Surround...) crashed Q-LiM ("stack smashing
  detected") when the picture is drawn by the graphics card (OpenGL). The cause was an old bug in the drawing of
  big circle outlines (the 120 segments of a circle over ~43 px did not fit in the drawing buffer); the Surround
  circle was the first one that big. The circle now uses at most 84 segments, and the drawing buffer can no
  longer be overrun by any shape.

## 1.4.19 — 2026-11-15
- **Surround** (right click a channel → Surround...): the channel plays through speakers placed around the room
  instead of its two outputs.
  - Layouts: Quad, 5.0, 5.1, 6 in a ring, 7.0, 7.1, 8 in a ring (5.1 / 7.1 leave the LFE output free), from any
    output of the sound card; every speaker can be moved on the circle and given another output.
  - The channel's position (drag the orange dot; nearer the middle = wider), **Width** (the left and right of the
    channel that many degrees apart, or Mono) and **Spread** (0 = a point ... All = every speaker the same).
  - Panning: constant power between the two neighbouring speakers (2-D VBAP, as in Ableton's Surround Panner,
    Reaper's ReaSurround, SPAT); the loudness stays the same wherever the sound is.
- **The clips move the sound**: select a clip of a surround channel → **Surround** above the waveform → a lane over
  the waveform: up = left, the middle = front, down = right, the edges = behind. Click = a point, drag it (Shift: only
  the place), right click = delete, mouse wheel on a point = spread. Between the points the sound moves smoothly the
  short way round (no clicks: the gains glide); before the first / after the last point it stays there. Saved in the
  project, with undo; follows WARP (the points are in the music's time).
- Light: a few multiplications per sample for a surround channel, nothing for the others - fine on the T60.
  Not in the PipeWire graph mode (there a channel has only two ports) - the dialog says so.

## 1.4.18 — 2026-11-15
- **Export Scene Audio** (right click a scene): saves what the scene plays - exactly as heard, with the faders, pan,
  mute / solo, plugins and the master (without the metronome), in any format of Export Audio.
  - The length: until the longest clip **without loop** ends (the looping clips play along until then). If every
    clip loops: one pass of the longest one.
  - It is recorded in real time: the scene starts at once and you hear it; the recording begins with its first
    sound and ends exactly at that length. Cancel any time.

## 1.4.17 — 2026-11-15
- **Separate - choose the parts** (right click a clip → Separate...): **Vocals, Drums, Bass, Guitar, Piano**, any of
  them; whatever is not chosen stays together as **Other** ("Instrumental" when only the vocals are taken), so all
  the parts together are exactly the song (tested to 150 dB).
  - Only the vocals → Mel-Band RoFormer, as before. Other parts → a 6-part model, chosen in the window:
    **BS-RoFormer SW** (the best open model for drums / bass / guitar / piano; slow, ~3.5 GB RAM) or
    **HTDemucs 6s** (Meta, MIT; ~5x faster, ~2 GB RAM). With "Vocals by Mel-Band RoFormer first" the vocals come from
    the vocal model and the 6-part model works on the rest.
  - The window shows how many tracks it makes, what it must download first and how much RAM it needs; a warning
    comes when the computer has too little.
  - The progress says "Loading the AI model..." until the model is in memory, then "Separating ...: 40% (about
    3 min left)", and "Step 1 of 2" when two models run.
  - The checked pipeline: BS-RoFormer SW's STFT / iSTFT match its published reference output to 77-129 dB,
    HTDemucs matches its own Python reference to 60-88 dB (on the parts that sound).
  - The parts are written while it separates (memory: one piece of the song, not all of it).
- **Remove the downloaded models** (File → Separation...): each model and ONNX Runtime with its size on disk and
  **Remove** (click twice). A removed model is downloaded again only if a separation needs it.
- **Every part has its own colour, level, M (mute) and S (solo)** under the waveform (Ctrl + S = only this one);
  with more than two parts the waveform shows them stacked in their colours (how loud each part is, where).
- **MIDI Learn for the part levels**: right click a level → move a knob or fader (right click again: forget it;
  or click it in the MIDI Learn mode). CC value 100 = 0 dB, 0 = silence. Saved in the project.
- **Split to clips** no longer makes new channels: the parts (the ones that are heard) hang on the mouse, the cells
  where they would go turn green (free) or red (taken), and a click puts them there one under another (missing scenes
  are added). Right click / Esc cancels.
- **Scene transitions**: **Next** in the top bar (or a MIDI button) - what plays fades out and the next scene with
  clips fades in, equal power, not quantized. The time is next to it (drag, mouse wheel, double click = 4 s, or a MIDI
  knob: 0.1 ... 30 s). **◀** goes back to the scene before (step by step). **AUTO → HELD** ("Stop automations",
  also a MIDI button) holds every fade exactly where it is, so you go on by hand; press again to go on. Right click a
  scene: "Fade to This Scene". All four can be MIDI-learned (MIDI learn mode, click them).
- **Launch at a clock time**: right click a clip or a scene → "Launch at Time..." - hour : minute : second (or
  "now + 1 min", "+ 5 min", "full hour"), once or every day, a scene with or without the transition. It starts on that
  second (measured: 1-6 ms after it), not quantized; a small clock (the last minute: a countdown) is on the cell.
  While the automations are held nothing starts - what was missed is marked "missed" and does not start late.
  They follow a moved clip, go with a deleted one, are kept in the project; File → Launches at a Time... lists all.

## 1.4.16 — 2026-11-14
- **Windows: Separate Vocals failed with "cannot load ...onnxruntime.dll".** The path is now given to Windows with
  backslashes and the DLL's own folder is searched for what it needs. If something is still missing, the message says
  exactly what: the Microsoft Visual C++ Redistributable (x64) with its download link, an old Windows, or a damaged
  download.

## 1.4.15 — 2026-11-14
- **The separated tracks go where you want, named after the song**: the first time you separate, Q-LiM asks where
  to keep them (by default `~/Music/Q-LiM Stems`). Every song gets its own folder with the song's name:
  `Song/Song (Vocals).flac` and `Song/Song (Instrumental).flac` - easy to find and to use elsewhere.
- **File → Vocal Separation...**: the folder, the format (WAV / AIFF / FLAC / Ogg / Opus / MP3), bits or quality and
  the sample rate of the separated tracks, like Export Audio. FLAC 24-bit by default (lossless, about half the size of
  WAV). It shows how much space they take, with **Delete all** (they can be separated again at any time, offline).
- Songs separated with 1.4.14 (in the cache) are still found.

## 1.4.14 — 2026-11-14
- **Separate Vocals** (right click on a clip, 64-bit Linux / Windows): the vocals and the instrumental of a song,
  with Mel-Band RoFormer, one of the best open models (Kimberley Jensen's weights, MIT).
  - Done once in the background (a few minutes per song); the result is kept, so the next time it is instant.
  - Then the clip plays the two stems with its cuts, markers, WARP and pitch: **VOCALS / INSTR.** level and mute
    under the waveform (heard at once), the vocals drawn in their own colour over the waveform.
  - **Split to clips** (two new channels), **Export** of the vocals, the instrumental or both.
  - Invisible until used: the network runs in a separate small program (`qlim-separate`) started only for a
    separation. Q-LiM itself loads nothing of it and uses no extra memory or CPU. The model (443 MB) and ONNX Runtime
    (8 MB) are downloaded once, after you agree, and checked (SHA-256). Not offered on 32-bit / Alpine builds.
  - Measured (`tests/test_separate.c`, a synthetic singer over music): vocals SDR 27 dB, vocals + instrumental =
    the original exactly; music without a voice gives a vocal stem ~89 dB down. Needs ~3 GB of RAM while it runs.
- **Export Audio** (right click on a clip): from the start to the stop marker, the way it sounds (cuts, fades, WARP,
  pitch, gain, pan, loudness normalization). WAV / AIFF (16, 24, 32 float), FLAC, Ogg Vorbis, Opus, MP3; any sample
  rate; 16 bit with dither. Rendered offline on its own thread (the clip can keep playing); the peak is reported.
- **MIDI Learn for plugin parameters** (VST3, VST2, CLAP, LV2): right click a knob in the device panel -> MIDI Learn,
  or on the plugin's header -> "MIDI Learn (move a parameter)" and move it in the plugin's own window. Then move a
  knob / fader on the controller. Saved in the project, moves with the plugin, Forget / Forget all.
- **The format is found from the file's content, not its name**: an .m4a named .mp3 no longer makes mpg123 print
  "Giving up searching valid MPEG header" and a wrong "File does not exist" (also during playback); it goes straight
  to ffmpeg.

## 1.4.13 — 2026-11-13
- **Pitch for every clip** (above the waveform, next to Gain / Pan): -24…+24 semitones without changing the speed.
  Drag = whole semitones, `Shift` + drag = cents, mouse wheel = 1 semitone (`Shift` = 10 cents), double click = 0.
  Saved in the project, with undo. Works together with WARP (tempo and pitch at once) and can be changed while the
  clip plays (no clicks, no dropouts).
- **How it is made, and why** (measured, `tests/test_pitch.c` and real stems): Signalsmith Stretch (bundled,
  MIT) changes only the length, then a windowed-sinc resampler (32 zero crossings, Kaiser) plays it faster or slower.
  - The note is **exact** (measured error below 0.1 cent).
  - Chords, bass and full mixes are cleaner than with Rubber Band R3's pitch mode (log-spectral distance of real
    stems 4.2 dB vs 6.2 dB).
  - About **12x less CPU** (≈1.5 % of one core per clip instead of ≈20 %).
  - Signalsmith's own transpose was 2…17 cents out of tune, depending on the note, so it is not used.
  - Without a pitch shift, nothing changes: tempo-only WARP is still Rubber Band R3, and a clip at 0 / no WARP is
    bit exact.
- **Formants** button: keeps the spectral envelope where it was, so voices do not become "chipmunks" / "giants".
- Restarting a stretched clip (seek, or pitch 0 ↔ not 0 with WARP) now fades the new sound in from its first sample
  (before, the fade-in could pass over the few ms of silence and the start clicked).
- Linux packages: zenity is required (Alpine) / recommended (deb, rpm) - it shows the Open / Save windows.
- Graphics: the check for a software-only OpenGL (llvmpipe) now really runs (it read the renderer before SDL had made
  its context current, got nothing and kept the slow renderer - seen on a ThinkPad T60).

## 1.4.12 — 2026-11-11
- **Light theme**: warm cream instead of near-white, easier on the eyes.
- **OLED Black is the default theme** for a new installation (an existing choice stays).
- **New channels**: orange, navy, dark green, dark red for the first four (then purple, teal, ochre, mint).
- **Clip names with a big text size** (Appearance): when the name and the time no longer fit in a cell, only the name
  stays, in the middle; clip names are always drawn in the full text colour (clearer than before).
- **The library's start folder**: in the library (L), the star makes the current folder the one the library opens in
  (saved in ~/.config/qlim/library.conf); the house goes back to it. Before it was always ~/Music.

## 1.4.11 — 2026-11-11
- **Loudness normalization** (Edit → Loudness…), like Spotify / YouTube / Apple Music: Off, -14 LUFS (Spotify,
  YouTube, Tidal), -16 LUFS (Apple Music) or -23 LUFS (TV / radio, EBU R128). Every audio clip is measured once in the
  background, in the same pass as its waveform (ITU-R BS.1770-4 integrated loudness with gating - checked against the
  EBU Tech 3341 test signals and FFmpeg's ebur128), and played at the target loudness. Quiet clips are turned up only
  as far as their highest sample stays at -1 dBFS (never distorted); "Only turn loud clips down" works like YouTube.
  The clip's own Gain is added on top. Off (the default) leaves the sound bit exact. A global setting
  (~/.config/qlim/loudness.conf), not saved in the project.
- Above the waveform: the clip's loudness in LUFS (and what the normalization does to it).

## 1.4.10 — 2026-11-11
- **The chosen sample rate really plays with PipeWire.** PipeWire 1.0 often does not switch the card to another rate by
  itself, even when it is allowed (tested on PipeWire 1.0.5 + Scarlett 2i2: 88200 Hz was allowed, the card stayed at
  48000). Now, when that happens, Q-LiM holds PipeWire at the chosen rate while it runs (clock.force-rate) and gives
  the old value back when it closes or another rate / card is chosen. Other programs keep playing (PipeWire converts
  them). Only if the card cannot do the rate at all, Q-LiM uses the card's rate (right pitch) and says so.
  Works in both PipeWire modes (every channel in the graph, or one stream).

## 1.4.9 — 2026-11-11
- **Fixed: a clip played transposed (slower, lower) after choosing another sample rate (e.g. 88200 Hz) with PipeWire.**
  In PipeWire graph mode the channels run at the rate of the PipeWire graph, and PipeWire does not convert them; most
  systems allow the graph only 48000 Hz. Now Q-LiM asks PipeWire to allow the chosen rate (clock.allowed-rates, for
  this login only) and checks what the graph really runs at. If PipeWire or the sound card cannot do it, Q-LiM works
  at the graph's rate (the files are converted by Q-LiM's resampler, the pitch is right) and says so. If the graph
  changes its rate later (another program), Q-LiM follows. The chosen rate stays saved and is tried at every start.
- **Clip gain and pan** (like Bitwig): in the header above the waveform, "Gain" (-60 .. +24 dB) and "Pan" for every
  clip on its own. Drag up/down (Shift = fine), the mouse wheel = 0.5 dB / 5 %, double click = back to 0. 0 dB and C
  leave the sound bit exact. Saved in the project, undo works, kept when the sample rate changes.

## 1.4.8 — 2026-11-11
- **Pads play a box of clips** (like the session ring in Ableton / Push): in Edit → MIDI… choose the size (1..8
  channels x 1..8 scenes, e.g. 4 x 2 for the 8 pads of an MPK mini) and learn the pads (press them top row first).
  The box is an orange frame that moves with the selection (the jog wheel moves it). A pad plays the clip in its cell;
  an empty cell stops that channel. With a box, the Play button plays the scene of the selected row and Stop stops the
  channels of the box. The notes of the pads (and of the controller buttons) never play the armed instrument.
- **Time signature**: double-click the "4/4" next to the BPM and type any one (7/8, 5/4, 12/8 ... top 1-32, bottom
  1/2/4/8/16/32). The launch quantization ("1 Bar" = a 7/8 bar), the metronome (counts the beats of the signature,
  accent on the 1), the beat dots, the bar.beat numbers, the piano roll and the bars in the waveform follow it. Saved
  in the project (old projects: 4/4).
- **Above the waveform**: the file's sample rate and bits after its name, e.g. "(44100 Hz 16-bit -> 48000 Hz)" when it
  is converted for the sound card.

## 1.4.7 — 2026-11-11
- **MP3 without holes of silence**: frames that some encoders write wrongly, or that are damaged (mpg123: "dequantization
  failed!"), are now decoded as well as possible, like FFmpeg does, instead of leaving ~26 ms of silence. Our libmpg123
  is patched (packaging/patches/mpg123-granule-overrun.patch) in every build: Linux, 32-bit and Windows.
- **Windows 64-bit**: libsndfile and its codecs are built in (no sndfile.dll any more), so the same fix is there too.
- **Cleaner sample rate conversion** (when a file's rate differs from the sound card's): flat 0..20 kHz within 0.01 dB,
  noise + distortion + aliasing at least 128 dB below the signal (before: 92..123 dB, and a mirror image near 21 kHz only
  25 dB down). tests/test_resample.c checks it.
- Files that only ffmpeg can read (m4a, aac, wma ...) are decoded at their own rate and then converted by Q-LiM's converter.
- The conversion cache is made again once (new quality version), so old copies with holes are not used any more.
- README: a "Sound quality" section.

## 1.4.6 — 2026-11-11
- **Several MIDI inputs at the same time** (up to 32): a MIDI keyboard, a controller, the sound card's MIDI in ...
  Edit → MIDI…: click a device to connect or disconnect it. Every connected input has two switches:
  **Play** (its notes go to the instruments) and **Control** (MIDI learn, faders, pads, controller buttons).
  E.g. a keyboard only on Play: its keys never launch clips that are bound to the same notes on the controller.
- **Devices are remembered and connect by themselves**: plugged in later or pulled out and plugged in again, Q-LiM
  connects them within 2 seconds (also when the system gives them another number). A remembered device that is not
  plugged in is shown in the list ("Forget" removes it). The old one-port `midi.conf` is read too.

## 1.4.5 — 2026-11-11
- **Jog wheels in "Sign Magnitude" mode** (Korg KONTROL Editor: Jog Wheel Type = Sign Magnitude; right = 1, left = 65)
  now turn the right way. Before, 65 was always read as "right". Q-LiM tells the relative styles apart by the other
  values it sees (1…15 / 113…127 = this style, 49…63 = "binary offset", where 65 is right) and keeps it.

## 1.4.4 — 2026-11-11
- **Jog wheel: every click is one step, in the right direction.** Some controllers (e.g. Korg nanoKONTROL Studio)
  send the wheel as a value that counts 0…127 up and down, others send "+1 / -1". Q-LiM now finds out which kind
  it is from the first clicks (and keeps it), so slow turning works and the direction no longer flips by itself.
  "Clicks per step" now starts at 1 (every click).
  Note: a wheel that counts 0…127 stops at both ends - for endless turning set it to relative / "Inc/Dec" in the
  controller's editor (for Korg: KORG KONTROL Editor), if it offers that.

## 1.4.3 — 2026-11-11
- **Jog wheel: fine and even.** Only the direction of each click counts now (a controller's acceleration made the
  selection jump), and you set how many clicks make one step: Edit → MIDI… → "Jog wheel: clicks per step"
  (1 = coarse … 32 = very fine; default 4). Turning back starts counting again. Wheels that send absolute values work
  too. `QLIM_MIDI_LOG=1` writes every controller message to the log (to see what a controller sends).
- **Stop all and the master MUTE are learned like the faders**: MIDI learn (Ctrl+M), click the button, press the
  controller button. In MIDI learn mode the buttons show their binding. They are no longer in the MIDI Settings list.

## 1.4.2 — 2026-11-11
- **Controller buttons for any MIDI controller** (Edit → MIDI… → Controller buttons): click a row, then press or turn
  the control. Select up / down / left / right (buttons), a jog wheel for up / down and one for left / right
  (relative CC, both common styles), Launch selected (like Enter: a clip, or a whole scene), Stop selected, Stop all
  and Master mute. Stop all and the master MUTE can also be learned with MIDI learn (Ctrl+M, then click them).
  Kept in `~/.config/qlim/controls.conf`, for every project. Right click a row = clear.
- **No more lag with a MIDI controller**: a MIDI message now wakes the program at once. Before, when nothing was
  playing, the knobs and faders followed at the "idle" rate of 10 frames per second.
- **Text notes take the colour of their channel** (or the colour you give them) - in the cell, in the big view at the
  bottom and while dragging. Before they were always yellow.

## 1.4.1 — 2026-11-04
- **Q-LiM for 32-bit computers and old systems.**
  - **Windows XP, Vista, 7, 8, 10, 11 - 32 and 64-bit - with one installer** (`Q-LiM-Setup-1.4.1.exe`, Inno Setup
    5.6.1): on 64-bit Windows 7 and newer it installs the 64-bit Q-LiM, otherwise the 32-bit one. The 32-bit Q-LiM is
    one `qlim.exe` with everything built in (also as a portable zip). Sound: ASIO, DirectSound, MME, and on Vista and
    newer WASAPI / WDM-KS. On XP the few newer Windows functions are replaced by simple ones (`src/win/xpshim.c`).
    Tested on Windows XP (VirtualBox). Needs a processor with SSE2.
  - **32-bit Linux**: `.deb` / `.rpm` (glibc 2.17 or newer - also old distributions) and `.apk` (Alpine), for any
    i686 processor (no SSE2 needed).
- **Windows, MME and DirectSound**: the sound no longer breaks up with small buffers - the driver keeps its own safe
  minimum, the buffer size only sets the block size.
- **A sound card setting that crashes no longer locks you out**: if Q-LiM crashed while opening the sound card, the
  next start uses the default device (buffer 2048) and keeps the old settings in `audio.conf.crashed`. On Windows XP
  WASAPI is not offered any more and is never chosen automatically (it needs Vista; on XP it crashed Q-LiM) -
  the default there is DirectSound.
- **Waveform on 32-bit systems**: zooming by dragging no longer jumps far away, and Fit no longer centres behind the
  end of the clip (a view starting before the clip was read as a huge number).

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
