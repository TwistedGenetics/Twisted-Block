// Twisted Block - by Twisted Genetics
// Phase 0b: tracker grid + a first sample engine and pad note entry.
//
// THE SCREEN TRICK
//   The MPC cannot draw from plugin code; its screen is built from JSON skins.
//   Every grid cell is a FilmStrip image of glyphs ("C-3", "01", "0C"...)
//   bound to a "display parameter" that this code writes. Scrolling means
//   rewriting those parameters, then telling the MPC to redraw. How the MPC
//   is told is the "Redraw" setting:
//     Notify  - report each changed display parameter to the host (default)
//     Rebuild - ask the MPC to rebuild the page (at most ~8 times a second)
//     Both    - Notify and Rebuild
//     Poll    - only write the value and hope the MPC re-reads it
//
// SCREEN
//   One lane visible (16 rows: line, note, inst, vol, cmd, val), 4 lanes in
//   the engine. The centre row is the current line (OctaMED style).
//
// MODES
//   Edit: pads write a note at the cursor (and you hear it), cursor moves on
//         by Step. Q-Links: Row / Field / Value / Lane.
//   Play: holding a pad plays the block from the top. MPC Play also plays it,
//         following the song position.
//
// ENGINE (first version)
//   4 lanes, each one sample voice (plus a short fade-out of the previous
//   note so there are no clicks). Instruments 01-04 are WAVs in
//   Content/Samples. Note C-3 plays a sample at its own pitch. Volume column
//   00-40. Command 9xx = sample offset (xx * 256 samples). OFF stops a lane.
//
// THREADS
//   All grid and engine logic runs on the audio thread in process(). Samples
//   load in the constructor. Nothing allocates in process().

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/wav.h"

#include <cmath>
#include <cstring>

namespace {

constexpr char kUid[5] = "TwBk";

// ---- grid geometry (keep in step with tools/gen_skin.py) -------------------
constexpr int kRows      = 16;
constexpr int kCentreRow = 8;
constexpr int kCols      = 6;
constexpr int kFields    = 5;
constexpr int kLanes     = 4;
constexpr int kMaxLines  = 128;
constexpr int kWaveBars  = 64;
constexpr int kNumInsts  = 4;

constexpr int kLineFrames = 129;  // 00..7F, blank
constexpr int kNoteFrames = 123;  // ---, OFF, C-0..B-9, blank
constexpr int kInstFrames = 257;  // --, 01..FF, blank
constexpr int kVolFrames  = 67;   // --, 00..40, blank
constexpr int kCmdFrames  = 38;   // -, 0..9 A..Z, blank
constexpr int kValFrames  = 258;  // 00..FF, --, blank

constexpr int kFieldMax[kFields] = {121, 255, 65, 36, 255};
constexpr int kNoteC3  = 2 + 36;   // stored value of C-3 (sample's own pitch)
constexpr int kCmdOffs = 10;       // stored value of command '9'

enum ParamIndex
{
    kVolume = 0,
    kLane, kCursor, kField, kValue, kFollow, kLpb, kLength, kStress, kRedraw, kParamCount, kTouchCell,
    kMode, kInst, kOctave, kStep,
    kRowBase,
    kCursorSlot = kRowBase + kRows * kCols,
    kPlaySlot,
    kActBase,
    kWaveBase = kActBase + kLanes,
    kNumParams = kWaveBase + kWaveBars
};

enum RedrawMode { kNotify = 0, kRebuild, kBoth, kPoll };

constexpr int kTouchIdle = kRows * kFields;

struct Cell { uint8_t note = 0, inst = 0, vol = 0, cmd = 0, val = 0; };
struct Block { Cell cells[kLanes][kMaxLines]; };

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

// One playing sample.
struct Voice
{
    const std::vector<float>* data = nullptr;
    double pos = 0.0, inc = 1.0;
    float gain = 0.0f;
    float ramp = 1.0f, rampStep = 0.0f;   // fade in / fade out
    bool active = false;

    float next()
    {
        if (!active) return 0.0f;
        const size_t n = data->size();
        size_t i = (size_t)pos;
        if (i + 1 >= n) { active = false; return 0.0f; }
        const float f = (float)(pos - (double)i);
        float s = (*data)[i] + ((*data)[i + 1] - (*data)[i]) * f;
        pos += inc;
        ramp += rampStep;
        if (ramp >= 1.0f) { ramp = 1.0f; rampStep = 0.0f; }
        if (ramp <= 0.0f) { active = false; return 0.0f; }
        return s * gain * ramp;
    }
};

struct Lane
{
    Voice v[2];   // current note + previous note fading out
    int cur = 0;
};

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
        addParam(mpc::choiceParam("redraw", "Redraw", {"Notify", "Rebuild", "Both", "Poll"}, kNotify));
        addDisplay("pcount", "Params", kNumParams + 1, kNumParams);
        addDisplay("touch",  "Touch",  kTouchIdle + 1, kTouchIdle);
        addParam(mpc::choiceParam("mode",   "Mode",   {"Play", "Edit"}, 1));
        addParam(mpc::intParam   ("inst",   "Inst",   1, kNumInsts, 1));
        addParam(mpc::intParam   ("octave", "Octave", 0, 8, 3));
        addParam(mpc::intParam   ("step",   "Step",   0, 16, 1));

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
            addDisplay(id, name, 2, 0);
        }
        for (int b = 0; b < kWaveBars; ++b)
        {
            char id[16], name[16];
            std::snprintf(id, sizeof(id), "wave%02d", b);
            std::snprintf(name, sizeof(name), "Wave %02d", b);
            addDisplay(id, name, 101, 0);   // 0..100 = bar height in percent
        }

        loadInstruments();
        makeDemoBlock();
    }

    void prepare(double sampleRate, int) override { sampleRate_ = sampleRate; }

    void reset() override
    {
        if (pendingLoad_.exchange(false)) block_ = pendingBlock_;
        for (auto& l : lanes_) for (auto& v : l.v) v.active = false;
        last_ = EditKey{};
        lastValue_ = -1;
        padPlaying_ = false;
        padsHeld_ = 0;
        lastLine_ = -1;
        forceRedraw_ = true;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        if (pendingLoad_.exchange(false)) { block_ = pendingBlock_; forceRedraw_ = true; }

        const int length = mpc::clamp(getInt(kLength), 1, kMaxLines);
        const int lpb    = mpc::clamp(getInt(kLpb), 1, 16);
        const bool editMode = getInt(kMode) == 1;
        const bool stress = getBool(kStress);
        const double bpm = (ctx.transport.tempoValid && ctx.transport.bpm > 1.0) ? ctx.transport.bpm : 120.0;
        const double linesPerSample = bpm / 60.0 * lpb / sampleRate_;
        const float master = mpc::dsp::dbToGain(get(kVolume));

        const bool hostPlaying = ctx.transport.playing && ctx.transport.ppqValid;
        if (hostPlaying && !wasHostPlaying_) lastLine_ = -1;
        wasHostPlaying_ = hostPlaying;
        if (stress && !stressWasOn_) { stressPos_ = 0.0; lastLine_ = -1; }
        stressWasOn_ = stress;

        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        int ev = 0;

        for (int i = 0; i < ctx.numFrames; ++i)
        {
            while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= i)
                handleMidi(ctx.midi[ev++], editMode, length);

            // Sequencer: which line are we on?
            double pos = -1.0;
            bool audible = true;
            if (padPlaying_)       { pos = padPos_; padPos_ += linesPerSample; }
            else if (hostPlaying)  { pos = ctx.transport.ppqPos * lpb + i * linesPerSample; }
            else if (stress)       { pos = stressPos_; stressPos_ += linesPerSample; audible = false; }

            if (pos >= 0.0)
            {
                const long abs = (long)std::floor(pos);
                const int line = (int)(abs % length);
                if (abs != lastAbsLine_ || lastLine_ < 0)
                {
                    lastAbsLine_ = abs;
                    lastLine_ = line;
                    if (audible) triggerLine(line);
                }
                playLine_ = line;
            }
            else
            {
                playLine_ = -1;
                lastLine_ = -1;
            }

            float s = 0.0f;
            for (auto& l : lanes_) s += l.v[0].next() + l.v[1].next();
            s *= master;
            outL[i] = s;
            if (outR) outR[i] = s;
        }
        while (ev < ctx.numMidiEvents) handleMidi(ctx.midi[ev++], editMode, length);
        for (int ch = 2; ch < ctx.numOutputs; ++ch)
            std::memset(ctx.outputs[ch], 0, sizeof(float) * (size_t)ctx.numFrames);
        if (stressPos_ > 1.0e9) stressPos_ = 0.0;

        updateEditor(length);
        draw(length, ctx.numFrames);
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        const int v = (int)std::lround(plain);
        if (index == kCursor || index == kInst) { out = hex2(v); return true; }
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
        info.version      = 1001;
        info.isInstrument = true;
        info.numInputs    = 0;
        info.numOutputs   = 2;
        info.tailSamples  = 4410;
        return info;
    }

    // Display parameters are left automatable so the MPC accepts change reports for them.
    void addDisplay(const char* id, const char* name, int frames, int def)
    {
        addParam(mpc::intParam(id, name, 0, frames - 1, def));
    }

    // ---- MIDI --------------------------------------------------------------
    void handleMidi(const mpc::MidiEvent& e, bool editMode, int length)
    {
        if (e.isNoteOn())
        {
            if (editMode)
            {
                // Write the note at the cursor and audition it.
                const int lane = mpc::clamp(getInt(kLane), 0, kLanes - 1);
                const int line = mpc::clamp(getInt(kCursor), 0, length - 1);
                const int inst = mpc::clamp(getInt(kInst), 1, kNumInsts);
                const int note = mpc::clamp(2 + (e.note() - 36) + getInt(kOctave) * 12, 2, 121);
                Cell& c = block_.cells[lane][line];
                c.note = (uint8_t)note;
                c.inst = (uint8_t)inst;
                startNote(lane, note, inst, 0, 0, 0);
                const int step = getInt(kStep);
                if (step > 0) set(kCursor, (float)((line + step) % length));
                last_ = EditKey{};   // re-sync the Value control
                forceRedraw_ = true;
            }
            else if (padsHeld_++ == 0)
            {
                padPlaying_ = true;
                padPos_ = 0.0;
                lastLine_ = -1;
            }
        }
        else if (e.isNoteOff())
        {
            if (editMode)
                stopLane(mpc::clamp(getInt(kLane), 0, kLanes - 1));
            else if (padsHeld_ > 0 && --padsHeld_ == 0)
            {
                padPlaying_ = false;
                for (int l = 0; l < kLanes; ++l) stopLane(l);
            }
        }
        else if (e.isAllNotesOff())
        {
            padsHeld_ = 0;
            padPlaying_ = false;
            for (int l = 0; l < kLanes; ++l) stopLane(l);
        }
    }

    // ---- engine -------------------------------------------------------------
    void triggerLine(int line)
    {
        for (int l = 0; l < kLanes; ++l)
        {
            const Cell& c = block_.cells[l][line];
            if (c.note == 1) { stopLane(l); continue; }
            if (c.note >= 2) startNote(l, c.note, c.inst, c.vol, c.cmd, c.val);
        }
    }

    void startNote(int lane, int note, int inst, int vol, int cmd, int val)
    {
        if (inst < 1 || inst > kNumInsts || samples_[inst - 1].empty()) return;
        Lane& L = lanes_[lane];
        Voice& old = L.v[L.cur];
        if (old.active) old.rampStep = -1.0f / (0.004f * (float)sampleRate_);
        L.cur ^= 1;
        Voice& v = L.v[L.cur];
        v.data = &samples_[inst - 1];
        v.inc = std::pow(2.0, (note - kNoteC3) / 12.0) * sampleRates_[inst - 1] / sampleRate_;
        v.pos = (cmd == kCmdOffs) ? (double)val * 256.0 : 0.0;
        v.gain = (vol == 0 ? 1.0f : (float)(vol - 1) / 64.0f) * 0.4f;
        v.ramp = 0.0f;
        v.rampStep = 1.0f / (0.001f * (float)sampleRate_);
        v.active = v.pos + 1.0 < (double)v.data->size();
    }

    void stopLane(int lane)
    {
        for (auto& v : lanes_[lane].v)
            if (v.active) v.rampStep = -1.0f / (0.004f * (float)sampleRate_);
    }

    // ---- editing --------------------------------------------------------------
    void updateEditor(int length)
    {
        const int mode = mpc::clamp(getInt(kRedraw), 0, 3);
        notifyOn_ = (mode == kNotify || mode == kBoth);

        int lane   = mpc::clamp(getInt(kLane), 0, kLanes - 1);
        int cursor = mpc::clamp(getInt(kCursor), 0, length - 1);
        int field  = mpc::clamp(getInt(kField), 0, kFields - 1);
        const bool follow = getBool(kFollow);
        const int top = ((follow && playLine_ >= 0) ? playLine_ : cursor) - kCentreRow;

        const int touch = getInt(kTouchCell);
        if (touch >= 0 && touch < kTouchIdle)
        {
            const int line = top + touch / kFields;
            if (line >= 0 && line < length) { cursor = line; field = touch % kFields; }
            set(kCursor, (float)cursor);
            set(kField, (float)field);
            set(kTouchCell, (float)kTouchIdle);
            notify(kTouchCell);
        }
        else if (getInt(kCursor) != cursor) set(kCursor, (float)cursor);

        EditKey key{lane, cursor, field};
        const int value = getInt(kValue);
        if (!(key == last_))
        {
            last_ = key;
            lastValue_ = cellValue(lane, cursor, field);
            set(kValue, (float)lastValue_);
            notify(kCursor); notify(kField); notify(kValue);
            forceRedraw_ = true;
        }
        else if (value != lastValue_)
        {
            const int v = mpc::clamp(value, 0, kFieldMax[field]);
            setCellValue(lane, cursor, field, v);
            lastValue_ = v;
            if (v != value) { set(kValue, (float)v); notify(kValue); }
            forceRedraw_ = true;
        }
    }

    // ---- drawing --------------------------------------------------------------
    void draw(int length, int frames)
    {
        const int mode = mpc::clamp(getInt(kRedraw), 0, 3);
        changed_ = false;

        const int lane   = mpc::clamp(getInt(kLane), 0, kLanes - 1);
        const int cursor = mpc::clamp(getInt(kCursor), 0, length - 1);
        const bool follow = getBool(kFollow);
        const int top = ((follow && playLine_ >= 0) ? playLine_ : cursor) - kCentreRow;

        for (int r = 0; r < kRows; ++r)
        {
            const int line = top + r;
            const int base = kRowBase + r * kCols;
            if (line < 0 || line >= length)
            {
                show(base + 0, kLineFrames - 1); show(base + 1, kNoteFrames - 1); show(base + 2, kInstFrames - 1);
                show(base + 3, kVolFrames - 1);  show(base + 4, kCmdFrames - 1);  show(base + 5, kValFrames - 1);
                continue;
            }
            const Cell& c = block_.cells[lane][line];
            show(base + 0, line);
            show(base + 1, c.note);
            show(base + 2, c.inst);
            show(base + 3, c.vol);
            show(base + 4, c.cmd);
            show(base + 5, (c.cmd == 0 && c.val == 0) ? 256 : c.val);
        }
        const int cslot = cursor - top;
        show(kCursorSlot, (cslot >= 0 && cslot < kRows) ? cslot : kRows);
        const int pslot = playLine_ - top;
        show(kPlaySlot, (playLine_ >= 0 && pslot >= 0 && pslot < kRows) ? pslot : kRows);
        const int actLine = playLine_ >= 0 ? playLine_ : cursor;
        for (int l = 0; l < kLanes; ++l)
            show(kActBase + l, block_.cells[l][actLine].note >= 2 ? 1 : 0);
        for (int b = 0; b < kWaveBars; ++b)
            show(kWaveBase + b, waveBars_[b]);
        show(kParamCount, kNumParams);

        // Rebuild mode: ask the MPC to rebuild the page, at most ~8 times a second.
        rebuildTimer_ += frames;
        if ((mode == kRebuild || mode == kBoth) && changed_ && rebuildTimer_ >= (int)(sampleRate_ / 8.0))
        {
            rebuildTimer_ = 0;
            requestDisplayUpdate();
        }
        forceRedraw_ = false;
    }

    // Write a display parameter. Report it when it changed (or on a forced redraw).
    void show(int index, int v)
    {
        const bool differs = getInt(index) != v;
        if (!differs && !forceRedraw_) return;
        if (differs) set(index, (float)v);
        changed_ = true;
        notify(index);
    }

    void notify(int index) { if (notifyOn_) notifyHostParameterChanged(index); }

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

    // ---- content ----------------------------------------------------------------
    void loadInstruments()
    {
        static const char* const files[kNumInsts] = {"Samples/inst01_break.wav", "Samples/inst02_sub.wav",
                                                      "Samples/inst03_hat.wav", "Samples/inst04_stab.wav"};
        for (int i = 0; i < kNumInsts; ++i)
        {
            dsp::WavData w;
            if (dsp::loadWav(mpc::contentPath(files[i]), w) && w.numFrames() > 1)
            {
                samples_[i].assign(w.numFrames(), 0.0f);
                for (int ch = 0; ch < w.numChannels(); ++ch)
                    for (size_t f = 0; f < w.numFrames(); ++f)
                        samples_[i][f] += w.channels[(size_t)ch][f] / (float)w.numChannels();
                sampleRates_[i] = w.sampleRate;
            }
        }
        const auto& s = samples_[0];
        for (int b = 0; b < kWaveBars; ++b)
        {
            float peak = 0.0f;
            if (!s.empty())
            {
                const size_t s0 = s.size() * (size_t)b / kWaveBars, s1 = s.size() * (size_t)(b + 1) / kWaveBars;
                for (size_t i = s0; i < s1; ++i) peak = std::fmax(peak, std::fabs(s[i]));
            }
            waveBars_[b] = (int)std::lround(mpc::clamp(peak, 0.0f, 1.0f) * 100.0f);
        }
    }

    void makeDemoBlock()
    {
        auto put = [this](int lane, int line, int note, int inst, int vol = 0, int cmd = 0, int val = 0)
        {
            Cell& c = block_.cells[lane][line];
            c.note = (uint8_t)note; c.inst = (uint8_t)inst; c.vol = (uint8_t)vol; c.cmd = (uint8_t)cmd; c.val = (uint8_t)val;
        };
        // The test break is 2 bars of 16ths at 170 BPM; slice(n) = offset of 16th step n in 9xx units.
        auto slice = [](int step) { return (int)std::lround(step * (44100.0 * 60.0 / 170.0 / 4.0) / 256.0); };
        for (int bar = 0; bar < 4; ++bar)
        {
            const int o = bar * 16;
            put(0, o + 0,  kNoteC3, 1, 0, kCmdOffs, slice(0));
            put(0, o + 4,  kNoteC3, 1, 0, kCmdOffs, slice(4));
            put(0, o + 7,  kNoteC3, 1, 0, kCmdOffs, slice(7));
            put(0, o + 10, kNoteC3, 1, 0, kCmdOffs, slice(10));
            put(0, o + 12, kNoteC3, 1, 0, kCmdOffs, slice(bar == 3 ? 28 : 12));
            if (bar == 3) put(0, o + 14, kNoteC3, 1, 0x31, kCmdOffs, slice(20));
            put(1, o + 0, noteVal(3, bar == 2 ? 3 : 0), 2, 0x41);
            put(1, o + 6, 1, 0);
            put(1, o + 8, noteVal(3, bar == 2 ? 5 : 7), 2, 0x39);
            put(1, o + 14, 1, 0);
            for (int s = 2; s < 16; s += 4) put(2, o + s, kNoteC3, 3, s == 14 ? 0x19 : 0x2D);
            if (bar % 2 == 0) put(3, o + 0, noteVal(3, 2), 4, 0x35);
        }
    }

    Block block_, pendingBlock_;
    std::atomic<bool> pendingLoad_{false};
    std::vector<float> samples_[kNumInsts];
    double sampleRates_[kNumInsts] = {44100.0, 44100.0, 44100.0, 44100.0};
    int waveBars_[kWaveBars] = {};
    Lane lanes_[kLanes];

    EditKey last_;
    int lastValue_ = -1;
    double sampleRate_ = 44100.0;

    bool padPlaying_ = false, wasHostPlaying_ = false, stressWasOn_ = false;
    int padsHeld_ = 0;
    double padPos_ = 0.0, stressPos_ = 0.0;
    long lastAbsLine_ = -1;
    int lastLine_ = -1, playLine_ = -1;

    bool notifyOn_ = true, changed_ = false, forceRedraw_ = true;
    int rebuildTimer_ = 0;
};

MPC_PLUGIN_ENTRY(TwistedBlock)
