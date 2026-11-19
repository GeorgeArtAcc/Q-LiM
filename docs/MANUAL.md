# Q-LiM – User Manual

*Version 2.0 · [Прочети на български](MANUAL.bg.md)*

Q-LiM is a live clip launcher: a grid of **channels** (columns) × **scenes** (rows). Every cell can hold an audio clip,
a MIDI clip or a note. You start clips and scenes live, mix them with faders, and send them to any speaker.

This manual goes from the first start to the details. The full list of keys is always in the program:
**Shortcuts** in the menu bar.

---

## 1. The first start

1. **Sound card**: Edit → **Audio…** (`Ctrl+,`). Pick the driver and the card:
   - Linux: **PipeWire** is the best choice on a modern desktop; ALSA or JACK work too.
   - Windows: **ASIO** if your card has a driver for it, else WASAPI.
   - The **buffer size**: smaller = less delay, bigger = safer. 256 frames is a good start; if you hear clicks,
     take the next bigger one.
2. **More cards at once** (Bluetooth speakers, headphones, a second interface): Edit → **Sound Cards…**. Their outputs
   come after the main card's outputs.
3. **MIDI controllers**: Edit → **MIDI…**. Click the inputs you use; each can be for playing, for control or both.

## 2. The main window

| Part | What it is |
|---|---|
| Menu bar | File, Edit, Play, Routing, Shortcuts |
| Top bar | Launch quantization, BPM, time signature, metronome, scene transitions (◀ Next ▶, time, AUTO), DSP load |
| Scene column (left) | Play a whole scene (▶ on the scene button), add scenes (+) |
| Channels | The cells; the channel header (name, ● record arm); below: fader, pan, SOLO, MUTE, output |
| Master (right) | The master fader and meters |
| Library (`L`) | Your folders; drag a file onto a cell |
| Bottom panel | The waveform of the selected audio clip, or the MIDI editor of a MIDI clip |

## 3. Clips and scenes

- **Load a clip**: drag an audio file from the library or the file manager onto a cell.
  WAV, AIFF, FLAC, Ogg, Opus and MP3 are built in; M4A/AAC and others need `ffmpeg` installed.
- **Play / restart**: the ▶ in the cell, or select it and press `Enter`. A channel plays one clip at a time.
- **A whole scene**: the scene button on the left.
- **Stop**: the ■ in an empty cell stops that channel; the big ■ (bottom left) stops everything.
  `Space` pauses / resumes everything.
- **Launch quantization** (top left, e.g. "Launch: 1 Bar"): clips and scenes start on the next beat / bar.
- **Loop**: the LOOP button (or the yellow loop bar in the waveform).
- **Move a clip**: drag it to another cell. **Copy / paste**: `Ctrl+C`, `Ctrl+V`.
- **Rename**: `Ctrl+R`. **Colour**: right click → Color….
- **Queue** (QUEUE button): clips play one after another with a crossfade.

## 4. The waveform (audio clips)

- **Markers**: drag the start (green), end (red) and loop markers. Right click a marker = reset it.
- **Zoom**: the mouse wheel over the waveform, or drag up/down on the time ruler. `Shift` + wheel = sideways.
- **Click** on the waveform = the playhead goes there.
- **Cutting** (the scissors tool): click = cut, drag a part = move it; fades on the corners of a part;
  `Delete` removes the selected part. Nothing is ever changed in the file itself.
- **Gain / Pan / Pitch** (above the waveform): drag or wheel; **double click = 0**. **Formants** keeps voices natural
  when the pitch changes.
- **Tempo** (BPM above the waveform): drag to change; **double click** = type a number;
  **`Ctrl` + double click** = back to the clip's own (detected) tempo. **WARP** fits the clip to the master BPM.
- **Separate** (right click a clip, 64-bit only): vocals, drums, bass, guitar, piano – each part gets its own level,
  mute and solo under the waveform.

## 5. MIDI clips and the MIDI editor

- **New MIDI clip**: double click an empty cell of an instrument channel (a channel with a VST/CLAP/LV2 instrument).
- **Notes**: double click = add / delete; drag = move; drag the right edge = length; `Shift` = off the grid;
  `Ctrl` + drag = copy.
- **The pencil** (the pen icon in the editor's header, or `B`; `Esc` = back to the arrow): a pale note shows where a
  click would put one, as long as the last note you placed. Click = the note, drag = its length.
- **Note height** (vertical zoom): **Height − / + / Fit** in the editor's header (Fit = all the notes fill the
  height), or `Ctrl+Shift` + wheel anywhere, or `Ctrl` + wheel on the keys.
- **Horizontal zoom**: `Ctrl` + wheel, or drag on the ruler.
- `Ctrl+U` = quantize, `Ctrl+A` = select all, arrows = move (Shift + up/down = an octave).
- **Computer keyboard as piano**: `Ctrl+K`, then A W S E D F T G Y H U J K; Z / X = octave, C / V = velocity.

## 6. The mixer and the outputs

- Fader, pan, SOLO, MUTE for every channel; `Ctrl+F` hides / shows the faders.
- **Routing** (menu): send any channel to any output of any card. The **Delay** row lines up speakers that stand
  at different distances (0 … 2000 ms per output).
- **Effects / instruments**: click a channel name → its devices appear below; **+** adds a plugin
  (VST3, VST2, CLAP, LV2 on Linux). Double click a device = its window.
- **Plugins in a separate process** (Linux and Windows): a plugin there cannot crash or freeze Q-LiM - if it fails, only that
  plugin goes silent for a moment and starts again with its last settings. **Edit → Plugins → Safety**:
  *Never*, *Automatic* (the default: plugins that crashed or froze Q-LiM before go there by themselves, plus LV2
  plugins whose window needs Gtk, e.g. Calf) or *All plugins* (the safest, a little more CPU). One plugin: right click
  it → *Run in a separate process*.
- **MIDI Learn** (`Ctrl+M`): click a fader / knob / button / clip, then move or press the control on your controller.
  For plugin knobs: right click the knob → MIDI Learn.

## 7. Recording

1. Arm a channel: the **●** in its header (right click it: which input, monitoring).
2. Click an empty cell: recording starts on the next bar. The cell shows a **■** – click it to stop (on the bar).
3. While recording, the bottom panel shows the waveform growing (or the notes, for MIDI).

## 8. Surround

### 8.1 A channel in surround
Right click a channel → **Surround…**.

- **Circle**: the speakers around you (Quad, 5.0, 5.1, 6 ring, 7.0, 7.1, 8 ring) and their outputs.
- **Room**: the hall from above, the speakers where they really stand (Theater 9, Theater 6, Quad) – for theatres
  and big rooms.

Both windows have three tools:

| Tool | What you do |
|---|---|
| **Speakers** | Drag a speaker to its place. In Circle: the mouse wheel on a speaker = its output. |
| **Position** | Drag the orange dot: where the channel sounds when its clip has no path. |
| **Path** | The sound **moves** while the clip plays (see below). |

**A path** (the sound moving): first click the clip in its cell, then open the Surround window (or the **Surround**
button above the waveform). At the bottom is the clip's timeline:

- the **blue cursor** = the time a new point gets; drag it;
- **click in the circle / on the plan** = a point at the cursor's time (while playing: at the playhead);
- **drag a point** = move it; **right click** = delete it; its mark on the timeline can be dragged in time;
- **nearer the middle** of the circle = more spread (more speakers at once); the **Spread** buttons set it exactly;
- **Play from cursor** / **Stop**, **Clear path**.

### 8.2 Surround files (5.1, 7.1 …)
Drop a multichannel file (WAV, FLAC, Ogg, AIFF, and M4A/AAC with ffmpeg) onto a cell. Q-LiM asks how to play it
(later: right click → **Surround File…**):

- **Multichannel**: every channel on its own output. The waveform shows a lane per channel and where it plays.
- **As a path**: a stereo mix with the movement found automatically as editable surround points (the original is kept).
- **Stereo**: as a normal stereo clip.

### 8.3 The Speaker Map
Edit → **Speaker Map…**: wire every surround signal (a file's channels, or the surround speakers) to any output of any
card – one signal can go to several outputs (e.g. the center on the main speakers *and* the Bluetooth ones).
Drag from a signal to an output; again = disconnect; right click a wire = remove it. Outputs that no sound card has
are listed at the bottom as "Not on any sound card (silent)".

### 8.4 Surround export
Right click a clip or a scene → Export: **one multichannel file** or **a file per speaker / output**.

## 9. Playing a show

- **Scene transitions** (top bar): **Next ▶** fades to the next scene over the time next to it; **◀** goes back;
  **AUTO** stops all fades where they are (take over by hand). Right click a scene: "Fade to This Scene".
- **Launch at a clock time**: right click a clip or a scene → **Launch at Time…** (e.g. 21:30:00, once or daily).
  Play → **Launches at a Time…** lists them all.
- **Loudness** (Play → Loudness…): every clip at the same loudness (e.g. -14 LUFS).
- **Pads** (Edit → MIDI…): a box of clips on your pad controller; it moves with the selection.

## 10. Files and saving

- `Ctrl+S` save, `Ctrl+Shift+S` save as, `Ctrl+O` open, `Ctrl+N` new.
- **Collect All & Save** (`Ctrl+Shift+E`): a folder with the project and **copies of all its audio files**
  (also the originals of surround paths and the separated parts) – use it before you move a show to another computer.
  If clips use compressed files (MP3, FLAC, Ogg, M4A …) it asks what they become: as they are, **WAV 32-bit float**
  (lossless, nothing to decode while playing – best for an old computer), WAV 24-bit or FLAC 24-bit.
- **Autosave** every minute; after a crash or a power cut Q-LiM offers the recovered work at the next start.
- **Undo / redo**: `Ctrl+Z`, `Ctrl+Shift+Z` / `Ctrl+Y`.
- **Export Audio** (right click a clip) / **Export Scene Audio** (right click a scene): WAV, AIFF, FLAC, Ogg, Opus, MP3.

## 11. A backup plan for the stage

Things go wrong on stage even with good software. Prepare:

1. **Collect All & Save** the show, and copy the folder to a USB stick. Install Q-LiM on a second computer (or keep
   the AppImage / portable Windows zip on the stick) and open the show there once **before** the gig.
2. **Test the whole set** with the real sound card and cables at the venue's buffer size. If the DSP load (top right)
   goes over ~70 %, take a bigger buffer.
3. **Bluetooth speakers have a delay** of 100–300 ms – line them up with the **Delay** row in Routing, or keep them
   for things where timing does not matter.
4. If the sound stops: Edit → Audio… → **Refresh**, or choose the card again. A message "AUDIO DEVICE LOST" at the top
   means the card was unplugged or taken by another program.
5. If Q-LiM itself stops: start it again – it offers the autosaved work (at most one minute old).
6. Keep a **stereo export of every important scene** (Export Scene Audio) on the phone or stick: in the worst case you
   can still play the show from any player.
7. Turn off system updates, notifications and screen savers on the show computer.
8. **An old laptop with a USB sound card**: if the sound clicks when the waveform changes or the window is resized,
   turn on Edit → Performance → **Gentle on the graphics card**, or choose **Drawing: Light (CPU)** (after a restart).
   Some old graphics cards disturb USB sound when they copy big pictures. Try it at home, not at the venue.

## 12. Hidden actions (quick list)

| Where | Action | Does |
|---|---|---|
| Any field (Gain, Pan, Pitch, BPM) | double click | 0 / type a value |
| Clip's tempo | `Ctrl` + double click | back to its own tempo |
| A marker | right click | reset it |
| Channel name edge | drag / double click | width / default width |
| Faders' top edge | drag | taller / hide |
| A cell (instrument channel) | double click | new MIDI clip |
| An empty cell | double click | a note |
| ● on a channel | right click | input and monitoring |
| A plugin knob | right click | MIDI Learn / Forget |
| A Speaker Map wire | right click | remove it |
| A surround path point | right click | delete it |
| MIDI editor | `B` / `Esc` | pencil / arrow |
| Anywhere | `Ctrl` + wheel, `Ctrl+0` | interface size / 100 % |
