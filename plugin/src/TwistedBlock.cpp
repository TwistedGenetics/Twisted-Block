// Twisted Block - Phase 0 proof of concept, by Twisted Genetics
//
// WHAT THIS IS
//   A test of whether the MPC can show a live tracker grid. There is no audio
//   engine yet: the plugin outputs silence. It only proves the screen.
//
// THE TRICK
//   The MPC cannot draw from plugin code. Its screen is built from JSON skins.
//   But a FilmStrip image shows the frame chosen by a parameter, and the MPC
//   redraws it whenever that parameter changes. So every cell of the grid is a
//   film strip of glyphs ("C-3", "01", "0C"...), bound to a read-only "display
//   parameter" that this code writes. Scrolling = rewriting those parameters.
//
// SCREEN (one lane visible, 4 lanes in memory)
//   16 rows x 6 columns: line number, note, instrument, volume, command, value.
//   The centre row (row 8) is the current line, OctaMED style.
//   Cursor row and playhead are Indicator lamps bound to one parameter each.
//   Right side: lane buttons, lane activity, a 64-bar waveform (Meter widgets),
//   and the edit controls.
//
// EDITING
//   Q-Link 13 = row, 14 = field, 15 = value, 16 = lane (top row of knobs).
//   Touching a cell moves the cursor to it. Turning "Value" writes into the
//   cell under the cursor.
//
// PLAYBACK
//   - Hold a pad (any MIDI note): plays the block from the top, follows tempo.
//   - MPC transport playing: the grid follows the song position.
//   - "Stress" on: free-runs at the current tempo even when stopped, so every
//     visible cell changes on every line (worst case for redraw speed).
//
// THREADS
//   All grid logic runs on the audio thread in process(); it is the only
//   writer of the block data and the display parameters. Nothing allocates.

#include "mpc_plugin.h"
#include "dsp/wav.h"

#include <cmath>
#include <cstring>

namespace {

constexpr char kUid[5] = "TwBk";

// ---- grid geometry (keep in step with tools/gen_skin.py) -------------------
constexpr int kRows      = 16;   // visible rows
constexpr int kCentreRow = 8;    // the "current line" bar
constexpr int kCols      = 6;    // line, note, inst, vol, cmd, val
constexpr int kFields    = 5;    // editable: note, inst, vol, cmd, val
constexpr int kLanes     = 4;
constexpr int kMaxLines  = 128;
constexpr int kWaveBars  = 64;

// Film strip frame counts. The last frame of each strip is blank.
constexpr int kLineFrames = 129;  // 00..7F, blank
constexpr int kNoteFrames = 123;  // ---, OFF, C-0..B-9, blank
constexpr int kInstFrames = 257;  // --, 01..FF, blank
constexpr int kVolFrames  = 67;   // --, 00..40, blank
constexpr int kCmdFrames  = 38;   // -, 0..9 A..Z, blank
constexpr int kValFrames  = 258;  // 00..FF, --, blank

// Highest stored value per field.
constexpr int kFieldMax[kFields] = {121, 255, 65, 36, 255};

enum ParamIndex
{
    kVolume = 0,     // parameter 0 gets nudged by the host: keep it harmless
    kLane,           // 1..4
    kCursor,         // edit line
    kField,          // Note / Inst / Vol / Cmd / Val
    kValue,          // value of the cell under the cursor (edits it)
    kFollow,         // grid follows the playhead
    kLpb,            // lines per beat
    kLength,         // block length in lines
    kStress,         // free-run redraw stress test
    kRedraw,         // Poll (just write) / Notify (also tell the host)
    kParamCount,     // read-only: how many parameters the plugin has
    kTouchCell,      // written by the touch buttons: row*5+field, 80 = idle
    kRowBase,        // kRows * kCols display parameters start here
    kCursorSlot = kRowBase + kRows * kCols,   // which visible row the cursor is on (16 = none)
    kPlaySlot,                                // which visible row the playhead is on (16 = none)
    kActBase,                                 // kLanes lane-activity lamps
    kWaveBase = kActBase + kLanes,            // kWaveBars waveform bars
    kNumParams = kWaveBase + kWaveBars
};

constexpr int kTouchIdle = kRows * kFields;

struct Cell { uint8_t note = 0, inst = 0, vol = 0, cmd = 0, val = 0; };

struct Block
{
    Cell cells[kLanes][kMaxLines];
};

const char* const kNoteNames[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};

std::string noteText(int v)
{
    if (v <= 0) return "---";
    if (v == 1) return "OFF";
    int n = v - 2;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%s%d", kNoteNames[n % 12], n / 12);
    return buf;
}

std::string hex2(int v) { char b[8]; std::snprintf(b, sizeof(b), "%02X", v & 0xFF); return b; }

char cmdChar(int v) { return v <= 0 ? '-' : (v <= 10 ? (char)('0' + v - 1) : (char)('A' + v - 11)); }

int noteVal(int octave, int semi) { return 2 + octave * 12 + semi; }

} // namespace

class TwistedBlock : public mpc::Plugin
{
public:
    TwistedBlock() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam ("volume", "Volume", -40.0f, 6.0f, 0.0f, "dB", 1));
        addParam(mpc::choiceParam("lane",   "Lane",   {"1", "2", "3", "4"}, 0));
        addParam(mpc::intParam   ("cursor", "Row",    0, kMaxLines - 1, 0));
        addParam(mpc::choiceParam("field",  "Field",  {"Note", "Inst", "Vol", "Cmd", "Val"}, 0));
        addParam(mpc::intParam   ("value",  "Value",  0, 255, 0));
        addParam(mpc::boolParam  ("follow", "Follow", true));
        addParam(mpc::intParam   ("lpb",    "LPB",    1, 16, 4));
        addParam(mpc::intParam   ("length", "Length", 1, kMaxLines, 64));
        addParam(mpc::boolParam  ("stress", "Stress", false));
        addParam(mpc::choiceParam("redraw", "Redraw", {"Poll", "Notify"}, 0));
        addDisplay("pcount", "Params", kNumParams + 1, kNumParams);   // shows the count as its value
        addDisplay("touch",  "Touch",  kTouchIdle + 1, kTouchIdle);

        static const char* const colNames[kCols] = {"Line", "Note", "Inst", "Vol", "Cmd", "Val"};
        static const int colFrames[kCols] = {kLineFrames, kNoteFrames, kInstFrames, kVolFrames, kCmdFrames, kValFrames};
        for (int r = 0; r < kRows; ++r)
            for (int c = 0; c < kCols; ++c)
            {
                char id[24], name[24];
                std::snprintf(id, sizeof(id), "d_r%02d_c%d", r, c);
                std::snprintf(name, sizeof(name), "R%02d %s", r, colNames[c]);
                addDisplay(id, name, colFrames[c], colFrames[c] - 1);
            }
        addDisplay("cslot", "CursorSlot", kRows + 1, kRows);
        addDisplay("pslot", "PlaySlot",   kRows + 1, kRows);
        for (int l = 0; l < kLanes; ++l)
        {
            char id[16], name[16];
            std::snprintf(id, sizeof(id), "act%d", l);
            std::snprintf(name, sizeof(name), "Lane%d Act", l + 1);
            mpc::Param p = mpc::boolParam(id, name, false); p.automatable = false; addParam(p);
        }
        for (int b = 0; b < kWaveBars; ++b)
        {
            char id[16], name[16];
            std::snprintf(id, sizeof(id), "wave%02d", b);
            std::snprintf(name, sizeof(name), "Wave %02d", b);
            mpc::Param p = mpc::floatParam(id, name, 0.0f, 1.0f, 0.0f); p.automatable = false; addParam(p);
        }

        makeDemoBlock();
        computeWaveBars();
    }

    void prepare(double sampleRate, int) override { sampleRate_ = sampleRate; }

    void reset() override
    {
        if (pendingLoad_.exchange(false)) block_ = pendingBlock_;
        waveDirty_ = true;
        last_ = EditKey{};
        lastValue_ = -1;
        internalPlaying_ = false;
        notesHeld_ = 0;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        if (pendingLoad_.exchange(false)) block_ = pendingBlock_;

        // --- silence (no engine yet) ------------------------------------
        for (int ch = 0; ch < ctx.numOutputs; ++ch)
            std::memset(ctx.outputs[ch], 0, sizeof(float) * (size_t)ctx.numFrames);

        const int length = mpc::clamp(getInt(kLength), 1, kMaxLines);
        const int lpb    = mpc::clamp(getInt(kLpb), 1, 16);
        const double bpm = (ctx.transport.tempoValid && ctx.transport.bpm > 1.0) ? ctx.transport.bpm : 120.0;
        const double linesPerSample = bpm / 60.0 * lpb / sampleRate_;

        // --- MIDI: hold a pad to play the block from the top --------------
        for (int i = 0; i < ctx.numMidiEvents; ++i)
        {
            const mpc::MidiEvent& e = ctx.midi[i];
            if (e.isNoteOn())
            {
                if (notesHeld_++ == 0) { internalPlaying_ = true; internalPos_ = 0.0; }
            }
            else if (e.isNoteOff())
            {
                if (notesHeld_ > 0 && --notesHeld_ == 0) internalPlaying_ = false;
            }
            else if (e.isAllNotesOff()) { notesHeld_ = 0; internalPlaying_ = false; }
        }

        // --- where is the playhead? ---------------------------------------
        int playLine = -1;
        const bool stress = getBool(kStress);
        if (internalPlaying_ || (stress && !ctx.transport.playing))
        {
            if (!internalPlaying_ && !stressRunning_) { internalPos_ = 0.0; }
            stressRunning_ = !internalPlaying_;
            playLine = (int)std::fmod(internalPos_, (double)length);
            internalPos_ += linesPerSample * ctx.numFrames;
            if (internalPos_ > 1.0e9) internalPos_ = 0.0;
        }
        else
        {
            stressRunning_ = false;
            if (ctx.transport.playing && ctx.transport.ppqValid)
            {
                double line = std::floor(ctx.transport.ppqPos * lpb);
                playLine = (int)std::fmod(std::fmax(0.0, line), (double)length);
            }
        }

        // --- editing --------------------------------------------------------
        int lane   = mpc::clamp(getInt(kLane), 0, kLanes - 1);
        int cursor = mpc::clamp(getInt(kCursor), 0, length - 1);
        int field  = mpc::clamp(getInt(kField), 0, kFields - 1);
        const bool follow = getBool(kFollow);
        const int centreLine = (follow && playLine >= 0) ? playLine : cursor;
        const int top = centreLine - kCentreRow;

        // A touched cell moves the cursor (and picks the field).
        int touch = getInt(kTouchCell);
        if (touch >= 0 && touch < kTouchIdle)
        {
            int line = top + touch / kFields;
            if (line >= 0 && line < length) { cursor = line; field = touch % kFields; }
            set(kCursor, (float)cursor);
            set(kField, (float)field);
            set(kTouchCell, (float)kTouchIdle);
        }
        else if (getInt(kCursor) != cursor) set(kCursor, (float)cursor);   // length shrank

        EditKey key{lane, cursor, field};
        int value = getInt(kValue);
        if (!(key == last_))
        {
            // Cursor moved: show the cell's value on the Value control.
            last_ = key;
            lastValue_ = cellValue(lane, cursor, field);
            set(kValue, (float)lastValue_);
        }
        else if (value != lastValue_)
        {
            // Value turned: write it into the cell.
            int v = mpc::clamp(value, 0, kFieldMax[field]);
            setCellValue(lane, cursor, field, v);
            lastValue_ = v;
            if (v != value) set(kValue, (float)v);
        }

        // --- draw ------------------------------------------------------------
        const bool notify = getInt(kRedraw) == 1;
        const int drawTop = (follow && playLine >= 0) ? playLine - kCentreRow : cursor - kCentreRow;
        for (int r = 0; r < kRows; ++r)
        {
            const int line = drawTop + r;
            const int base = kRowBase + r * kCols;
            if (line < 0 || line >= length)
            {
                show(base + 0, kLineFrames - 1, notify);
                show(base + 1, kNoteFrames - 1, notify);
                show(base + 2, kInstFrames - 1, notify);
                show(base + 3, kVolFrames - 1, notify);
                show(base + 4, kCmdFrames - 1, notify);
                show(base + 5, kValFrames - 1, notify);
                continue;
            }
            const Cell& c = block_.cells[lane][line];
            show(base + 0, line, notify);
            show(base + 1, c.note, notify);
            show(base + 2, c.inst, notify);
            show(base + 3, c.vol, notify);
            show(base + 4, c.cmd, notify);
            show(base + 5, (c.cmd == 0 && c.val == 0) ? 256 : c.val, notify);
        }
        const int cslot = cursor - drawTop;
        show(kCursorSlot, (cslot >= 0 && cslot < kRows) ? cslot : kRows, notify);
        const int pslot = playLine - drawTop;
        show(kPlaySlot, (playLine >= 0 && pslot >= 0 && pslot < kRows) ? pslot : kRows, notify);

        const int actLine = playLine >= 0 ? playLine : cursor;
        for (int l = 0; l < kLanes; ++l)
            show(kActBase + l, block_.cells[l][actLine].note >= 2 ? 1 : 0, notify);

        for (int b = 0; b < kWaveBars; ++b)
            if (waveDirty_ || std::fabs(get(kWaveBase + b) - waveBars_[b]) > 1e-4f) set(kWaveBase + b, waveBars_[b]);
        waveDirty_ = false;
        if (getInt(kParamCount) != kNumParams) set(kParamCount, (float)kNumParams);
    }

    // Value text for the edit controls (tracker style).
    bool formatValue(int index, float plain, std::string& out) override
    {
        const int v = (int)std::lround(plain);
        if (index == kCursor) { out = hex2(v); return true; }
        if (index == kValue)
        {
            switch (mpc::clamp(getInt(kField), 0, kFields - 1))
            {
                case 0: out = noteText(mpc::clamp(v, 0, 121)); return true;
                case 1: out = v == 0 ? "--" : hex2(v); return true;
                case 2: { int vv = mpc::clamp(v, 0, 65); out = vv == 0 ? "--" : hex2(vv - 1); return true; }
                case 3: out = std::string(1, cmdChar(mpc::clamp(v, 0, 36))); return true;
                default: out = hex2(v); return true;
            }
        }
        return false;
    }

    // Block data is saved with the MPC project.
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        const uint8_t header[4] = {'T', 'B', 'K', 1};
        out.insert(out.end(), header, header + 4);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&block_);
        out.insert(out.end(), p, p + sizeof(Block));
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        if (size != 4 + sizeof(Block) || std::memcmp(data, "TBK\x01", 4) != 0) return;
        std::memcpy(&pendingBlock_, data + 4, sizeof(Block));
        pendingLoad_.store(true);
    }

private:
    struct EditKey
    {
        int lane = -1, line = -1, field = -1;
        bool operator==(const EditKey& o) const { return lane == o.lane && line == o.line && field == o.field; }
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Twisted Block";
        info.vendor       = "Twisted Genetics";
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);
        info.version      = 1000;
        info.isInstrument = true;
        info.numInputs    = 0;
        info.numOutputs   = 2;
        info.tailSamples  = 1;
        return info;
    }

    // A read-only integer parameter 0..frames-1 that drives a film strip.
    void addDisplay(const char* id, const char* name, int frames, int def)
    {
        mpc::Param p = mpc::intParam(id, name, 0, frames - 1, def);
        p.automatable = false;
        addParam(p);
    }

    // Write a display parameter only when it differs from what the host sees.
    // (Compared with the live value, not a cache, so a host that writes a
    // display parameter, e.g. a preset or project load, is corrected next block.)
    void show(int index, int v, bool notify)
    {
        if (getInt(index) == v) return;
        set(index, (float)v);
        if (notify) notifyHostParameterChanged(index);
    }

    int cellValue(int lane, int line, int field) const
    {
        const Cell& c = block_.cells[lane][line];
        switch (field) { case 0: return c.note; case 1: return c.inst; case 2: return c.vol; case 3: return c.cmd; default: return c.val; }
    }

    void setCellValue(int lane, int line, int field, int v)
    {
        Cell& c = block_.cells[lane][line];
        switch (field) { case 0: c.note = (uint8_t)v; break; case 1: c.inst = (uint8_t)v; break;
                         case 2: c.vol = (uint8_t)v; break; case 3: c.cmd = (uint8_t)v; break; default: c.val = (uint8_t)v; break; }
    }

    // Test data so every lane has something to show.
    void makeDemoBlock()
    {
        auto put = [this](int lane, int line, int note, int inst, int vol = 0, int cmd = 0, int val = 0)
        {
            Cell& c = block_.cells[lane][line];
            c.note = (uint8_t)note; c.inst = (uint8_t)inst; c.vol = (uint8_t)vol; c.cmd = (uint8_t)cmd; c.val = (uint8_t)val;
        };
        for (int bar = 0; bar < 4; ++bar)
        {
            const int o = bar * 16;
            // Lane 1: break slices (9xx = sample offset).
            put(0, o + 0,  noteVal(3, 0), 1, 0, 10, 0x00);
            put(0, o + 4,  noteVal(3, 0), 1, 0, 10, 0x40);
            put(0, o + 6,  noteVal(3, 0), 1, 0, 10, 0x20);
            put(0, o + 10, noteVal(3, 0), 1, 0, 10, 0x00);
            put(0, o + 12, noteVal(3, 0), 1, 0, 10, 0x40);
            if (bar == 3) put(0, o + 14, noteVal(3, 0), 1, 0x31, 10, 0x60);
            // Lane 2: sub bass.
            put(1, o + 0, noteVal(1, bar == 2 ? 3 : 0), 2, 0x41);
            put(1, o + 7, 1, 0);
            put(1, o + 8, noteVal(1, bar == 2 ? 5 : 7), 2, 0x39, 4, 0x08);
            // Lane 3: hats with volume.
            for (int s = 2; s < 16; s += 4) put(2, o + s, noteVal(5, 6), 3, s == 14 ? 0x19 : 0x2D);
            // Lane 4: chord stab every other bar.
            if (bar % 2 == 0) put(3, o + 0, noteVal(4, 2), 4, 0x35, 22, 0x37);
        }
    }

    // 64-bar peak overview of the test WAV in Content/Samples.
    void computeWaveBars()
    {
        dsp::WavData w;
        const bool ok = dsp::loadWav(mpc::contentPath("Samples/test_break.wav"), w) && w.numFrames() > 0;
        for (int b = 0; b < kWaveBars; ++b)
        {
            float peak = 0.0f;
            if (ok)
            {
                const size_t n = w.numFrames();
                const size_t s0 = n * (size_t)b / kWaveBars, s1 = n * (size_t)(b + 1) / kWaveBars;
                for (size_t i = s0; i < s1; ++i)
                    for (int ch = 0; ch < w.numChannels(); ++ch)
                        peak = std::fmax(peak, std::fabs(w.channels[(size_t)ch][i]));
            }
            else
            {
                // Fallback shape: decaying hits on every 8th bar.
                peak = std::exp(-0.45f * (float)(b % 8));
            }
            waveBars_[b] = mpc::clamp(peak, 0.0f, 1.0f);
        }
        waveDirty_ = true;
    }

    Block block_, pendingBlock_;
    std::atomic<bool> pendingLoad_{false};
    float waveBars_[kWaveBars] = {};
    bool waveDirty_ = true;
    EditKey last_;
    int lastValue_ = -1;
    double sampleRate_ = 44100.0;
    bool internalPlaying_ = false, stressRunning_ = false;
    int notesHeld_ = 0;
    double internalPos_ = 0.0;
};

MPC_PLUGIN_ENTRY(TwistedBlock)
