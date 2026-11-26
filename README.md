# Twisted Block

A sample-based block tracker for the Akai MPC (built for the MPC Live III),
made with the official [MPC Plugin SDK](https://github.com/inmusic-software/mpc-plugin-builder-sdk).
It works like Renoise Redux: blocks of tracker pattern are triggered from MIDI
notes, so you play them from the MPC arranger, sequencer or pads.
**C1 = block 01, C#1 = block 02**, and so on.

**Status: Phase 1.**

- 32 blocks, each 1–128 lines long, with 4 lanes.
- 16 instruments, loaded from WAVs on the MPC drive.
- Tracker effect commands and chords.
- Sample start, end, loop, gain, reverse and normalise.
- Slicing, by equal grid or by transients.

## Download and install

1. Download **twisted-block-…-mpc.zip** from
   [the latest build](https://github.com/TwistedGenetics/Twisted-Block/releases/tag/latest-build)
   and unzip it.
2. Copy the folder `Twisted Genetics - VST - Twisted Block` into `VST2 Plugins`
   at the top of your SD card, USB drive or the MPC's internal drive. Replace
   any older copy.
3. You need MPC OS 3.10.1 or later. Turn on **Preferences → Enhancements (Beta)
   → Enable Enhancements** and **External Plugin Support**.
4. Load **Twisted Block** on a plugin track.

## Your own samples

Make a folder called **`Twisted Block Samples`** at the top of the same drive
as `VST2 Plugins`, and put WAV files in it. One level of subfolders is fine,
e.g. `Twisted Block Samples/Breaks/amen.wav`.

To load one:

1. Open the **SAMPLE** page.
2. Choose the instrument with **Inst**.
3. Choose the file with **File**.
4. Tap **LOAD**.

Tap **RESCAN** after copying in new files. The demo sounds are listed as `demo/…`.
Projects remember which file each instrument uses and reload it when you open them.

## The three pages

### GRID: the tracker

- **Grid:** one lane shown at a time (L1–L4 buttons). The centre row is the
  current line. The grey bar is the edit cursor; the black arrows show the
  playhead.
- **Mode → Edit:**
  - Pads write a note at the cursor and play it.
  - **Inst** and **Octave** choose what is written. **Step** is how far the
    cursor moves after each note.
  - **Row**, **Field** and **Value** edit any column. You can also tap a cell
    to move the cursor there.
  - Press MPC **Play** to hear the block you're editing, in time with the song.
- **Mode → Play:** MIDI notes trigger blocks. **Block** chooses which block you
  edit; **Length** is that block's length.
- Above the controls: BLOCK is the block being edited, PLAY is the block
  playing, then the current instrument's sample name.

### SETUP

- **Base Note:** the key for block 01 (default C1).
- **Trigger:**
  - **Gate:** the block plays (and loops) while the note is held.
  - **One-shot:** the block plays once to the end.
- **Quantise:** Off, Beat or Bar. With Beat or Bar, a block waits for the next
  beat or bar before it starts (when the MPC is playing).
- **Ticks:** ticks per line, used by the effect commands. **LPB:** lines per
  beat. **Redraw:** how the screen is updated (all four work on a Live III).
- The page also shows the command and chord reference.

### SAMPLE

- **Waveform:** shown with **S**tart, **E**nd and **L**oop markers, plus slice
  lines.
- **Start, End, Loop, Loop Start, Gain, Reverse:** non-destructive, so the WAV
  is never changed. **NORMALISE** sets Gain so the sample peaks just under 0 dB.
- **Slice:**
  - **Grid:** cuts between Start and End into **Slices** equal parts.
  - **Transient:** cuts at hits. **Sense** sets how many hits count.
  - A sliced instrument plays **slice n on note C-3 + n**, so slices map
    across the pads from C-3.

## Effect commands

The C column holds the command and VAL holds its value, in hex.
One *tick* = one line ÷ Ticks.

| Cmd | What it does |
|---|---|
| `0xy` | Arpeggio: note, +x, +y semitones, cycling every tick |
| `1xx` / `2xx` | Pitch slide up / down, xx/16 semitone per tick |
| `3xx` | Glide (tone portamento) from the playing note to this note, xx/16 semitone per tick |
| `4xy` | Vibrato: speed x, depth y |
| `9xx` | Sample offset: start xx × 256 samples after the start point |
| `Axy` | Volume slide: up x / down y per tick |
| `Bxx` | Play this note backwards |
| `Cxx` | Set volume 00–40 |
| `Dxx` | Delay the note by xx ticks |
| `Exx` | Chord: 01 maj, 02 min, 03 7th, 04 m7, 05 maj7, 06 sus2, 07 sus4, 08 dim, 09 aug, 0A 5th, 0B octave, 0C m9, 0D add9 |
| `Fxx` | Cut the note after xx ticks (F00 = cut now) |
| `Rxx` | Retrigger every xx ticks |

## Demo blocks

- **01** (C1): chopped break, sub bass with a glide, hats and minor chord stabs.
- **02** (C#1): half-time with a reversed hit, a sub drop and chords.
- **03** (D1): rolling hats with retriggers.

## Q-Links

Each page maps its controls in banks of four, in the same columns as on
screen. On the GRID page:

| Bank | Q-Links |
|---|---|
| 1 | Row, Field, Value, Lane |
| 2 | Mode, Inst, Octave, Step |
| 3 | Block, Length, LPB, Follow |
| 4 | Volume, Ticks, Trigger, Quantise |

## How the grid is drawn

The MPC doesn't let plugins draw from code; the screen is built from JSON
skins. Every cell of the grid is a **film-strip image**: all the glyphs (`C-3`,
`01`, `0C`…) stacked in one tall PNG, bound to a "display parameter" the plugin
writes. Scrolling means rewriting those parameters. That's why the plugin has
269 parameters.

## Repo layout

| Path | What |
|---|---|
| `plugin/` | The SDK project: `plugin.json`, `src/TwistedBlock.cpp`, `skins/`, `Content/` |
| `plugin/skins/` | Generated: `TUI.json`, `Q-Links.json`, glyph strips and widget images |
| `tools/gen_skin.py` | Generates `plugin/skins/` (run it after changing the layout) |
| `tools/make_test_wav.py` | Synthesises the demo instruments (and a test "user" sample) |
| `tools/test_grid.cpp` | Desktop tests: editing, blocks, triggering, quantise, commands, sample loading, slicing, project reload |
| `tools/preview_skin.py` | Draws a mock-up PNG of an MPC page from a parameter dump |
| `.github/workflows/build.yml` | Builds arm32/arm64, runs every test, publishes the `latest-build` release |

## Licence

MIT. The glyphs are drawn with DejaVu Sans Mono Bold (Bitstream Vera licence,
free to redistribute). The demo sounds are synthesised by `tools/make_test_wav.py`.
