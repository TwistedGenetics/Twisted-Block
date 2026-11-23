# Twisted Block

A sample-based block tracker for the Akai MPC Live 3, built with the official
[MPC Plugin SDK](https://github.com/inmusic-software/mpc-plugin-builder-sdk).
It works like Renoise Redux: blocks are triggered from keys in the MPC arranger
(C1 = block 1, C#1 = block 2 ...), with 4 lanes per block. One lane is shown on
screen at a time.

**Status: Phase 0, proof of concept.** There is no audio engine yet; the plugin
outputs silence. Phase 0 only answers one question: can the MPC show and scroll
a live tracker grid fast enough?

## How the grid is drawn

The MPC does not let plugins draw from code; the screen is built from JSON
skins. So every cell of the grid is a **film-strip image** (all the glyphs
`C-3`, `01`, `0C`... stacked in one tall PNG) bound to a read-only "display
parameter". The plugin writes those parameters and the MPC shows the matching
frame. Scrolling means rewriting about 100 parameters.

- 16 rows x 6 columns (line, note, instrument, volume, command, value).
- The centre row is the current line (OctaMED style).
- Grey bar = edit cursor. Black arrows and underline = playhead.
- Lane buttons L1–L4, with an activity bar under each that shows which lanes
  have a note on the current line.
- A 64-bar waveform overview made of meters (test break in `Content/`).
- 178 plugin parameters in total (shown on screen as PARAMS).

## Install on the MPC

1. Download `twisted-block-mpc` from the latest GitHub Actions run (Actions tab
   → latest run → Artifacts) and unzip it.
2. Copy the folder `Twisted Genetics - VST - Twisted Block` into a folder
   called `VST2 Plugins` on an SD card or USB drive (create it if needed).
3. The MPC needs **MPC OS 3.10.1 or later** and **External Plugin Support**
   turned on: Menu → Preferences → Enhancements (Beta) → Enable Enhancements,
   then External Plugin Support.
4. Add **Twisted Block** to a plugin track.

## Phase 0 hardware test

Please check these and report back:

1. **Does it load?** Does the GRID tab appear with black text on white?
2. **Parameter count:** does PARAMS show 178? Did the MPC complain about the
   number of parameters?
3. **Editing.** The first Q-Link bank is Row / Field / Value / Lane. That's
   Q-Links 13–16 on a 16-knob MPC, or the first bank on MPCs with fewer knobs.
   - Turn Row. The grid should scroll, and the grey cursor bar
     stays on the centre row.
   - Turn Field. The black underline moves under NOTE / INS / VOL / C / VAL.
   - Turn Value. The cell under the cursor should change.
   - Tap a cell. The cursor should jump to it.
4. **Lanes:** tap L1–L4. Does the grid switch lanes straight away?
5. **Playback:**
   - Hold a pad. The block should play from the top, with the grid scrolling
     and the playhead on the centre row.
   - Press Play on the MPC. The grid should follow the song position.
6. **Redraw speed:** turn on **Stress**. The grid scrolls at tempo with every
   cell changing, which is the worst case.
   - Is it smooth? Try 90 BPM and 170 BPM.
   - Does the MPC slow down, stutter or glitch the audio?
7. **If cells do not update at all**, set **Redraw** to **Notify** and try again.
8. **Readability:** is the text big enough from where you stand?

The answers decide Phase 1. If the redraw is too slow, the fall-back is 8 rows
(half the parameters).

## Repo layout

| Path | What |
|---|---|
| `plugin/` | The SDK project: `plugin.json`, `src/TwistedBlock.cpp`, `skins/`, `Content/` |
| `plugin/skins/` | Generated: `TUI.json`, `Q-Links.json`, glyph strips and widget images |
| `tools/gen_skin.py` | Generates `plugin/skins/` (run it after changing the layout) |
| `tools/make_test_wav.py` | Generates the synthetic test break in `plugin/Content/Samples/` |
| `tools/test_grid.cpp` | Desktop test of the grid logic; also dumps parameters for previews |
| `tools/preview_skin.py` | Draws a mock-up PNG of the MPC screen from a parameter dump |
| `.github/workflows/build.yml` | Builds arm32/arm64, runs every test, uploads the zip and the previews |

## Build locally

```sh
git clone https://github.com/inmusic-software/mpc-plugin-builder-sdk sdk
cp -r plugin "sdk/plugins/Twisted Block"
python3 sdk/tools/build.py "sdk/plugins/Twisted Block"
```

## Licence

MIT. The glyphs are drawn with DejaVu Sans Mono Bold (Bitstream Vera licence,
free to redistribute). The test break is synthesised by `tools/make_test_wav.py`.
