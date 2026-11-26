// Twisted Block - by Twisted Genetics
// Phase 1: a sample-based block tracker for the MPC (Renoise Redux style).
//
// WHAT IT DOES
//   - 32 blocks of 1-128 lines, 4 lanes each. In Play mode a MIDI note
//     triggers a block: Base Note (C1) = block 01, C#1 = block 02 and so on,
//     so blocks can be played from the MPC arranger, sequencer or pads.
//   - 16 instruments. WAVs are loaded from the plugin's Content/Samples
//     folder or from a folder called "Twisted Block Samples" at the root of
//     the same drive. Loading happens on a background thread.
//   - Non-destructive sample editing (start, end, loop, gain, reverse,
//     normalise) and slicing (equal grid or transients). A sliced instrument
//     plays slice n on note C-3 + n.
//   - Tracker commands on ticks (Ticks per line), listed below.
//
// THE SCREEN TRICK
//   The MPC cannot draw from plugin code. Every grid cell is a FilmStrip
//   image of glyphs bound to a "display parameter" that this code writes.
//   "Redraw" picks how the MPC is told about changes (all four worked on a
//   Live III; Notify is the default).
//
// MODES
//   Edit: pads write notes at the cursor and audition them; MPC Play plays
//         the block being edited (in time with the song).
//   Play: MIDI notes trigger blocks (Gate: plays while held, looping;
//         One-shot: plays once to the end). Optional launch quantise.
//
// COMMANDS (command column + 2-digit hex value)
//   0xy arpeggio (+x, +y semitones)     1xx pitch slide up    2xx pitch slide down
//   3xx tone portamento to the note     4xy vibrato (speed x, depth y)
//   9xx sample offset (xx * 256 samples from the start point)
//   Axy volume slide (x up, y down)     Bxx play backwards
//   Cxx set volume (00-40)              Dxx delay the note by xx ticks
//   Exx chord (01 maj 02 min 03 7 04 m7 05 maj7 06 sus2 07 sus4 08 dim
//              09 aug 0A 5th 0B octave 0C m9 0D add9)
//   Fxx cut the note after xx ticks     Rxx retrigger every xx ticks
//
// THREADS
//   process(): sequencer, voices, editing and drawing. Never allocates or
//   does I/O. A worker thread scans folders and loads WAVs; finished samples
//   are handed over through atomic pointers and old ones handed back to the
//   worker to free.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/wav.h"

#include <dirent.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr char kUid[5] = "TwBk";

// ---- sizes (keep in step with tools/gen_skin.py) ---------------------------
constexpr int kRows      = 16;
constexpr int kCentreRow = 8;
constexpr int kCols      = 6;
constexpr int kFields    = 5;
constexpr int kLanes     = 4;
constexpr int kMaxLines  = 128;
constexpr int kWaveBars  = 64;
constexpr int kBlocks    = 32;
constexpr int kInsts     = 16;
constexpr int kMaxFiles  = 256;
constexpr int kMaxSlices = 64;
constexpr int kChordMax  = 4;
constexpr int kVoices    = kChordMax * 2;   // current chord + previous one fading out

constexpr int kLineFrames = 129;
constexpr int kNoteFrames = 123;
constexpr int kInstFrames = 257;
constexpr int kVolFrames  = 67;
constexpr int kCmdFrames  = 38;
constexpr int kValFrames  = 258;

constexpr int kFieldMax[kFields] = {121, 255, 65, 36, 255};
constexpr int kNoteC3 = 2 + 36;

// Stored command values: 1..10 = '0'..'9', 11.. = 'A'..
constexpr int cmdOf(char c) { return c >= '0' && c <= '9' ? 1 + (c - '0') : 11 + (c - 'A'); }
constexpr int kCmdArp = cmdOf('0'), kCmdUp = cmdOf('1'), kCmdDown = cmdOf('2'), kCmdPorta = cmdOf('3'),
              kCmdVib = cmdOf('4'), kCmdOffs = cmdOf('9'), kCmdVSlide = cmdOf('A'), kCmdBack = cmdOf('B'),
              kCmdVol = cmdOf('C'), kCmdDelay = cmdOf('D'), kCmdChord = cmdOf('E'), kCmdCut = cmdOf('F'),
              kCmdRetrig = cmdOf('R');

// Chords for Exx (semitones above the note).
struct Chord { int n; int s[kChordMax]; };
const Chord kChords[] = {
    {1, {0}},           {3, {0, 4, 7}},       {3, {0, 3, 7}},      {4, {0, 4, 7, 10}},  {4, {0, 3, 7, 10}},
    {4, {0, 4, 7, 11}}, {3, {0, 2, 7}},       {3, {0, 5, 7}},      {3, {0, 3, 6}},      {3, {0, 4, 8}},
    {2, {0, 7}},        {2, {0, 12}},         {4, {0, 3, 10, 14}}, {4, {0, 4, 7, 14}},
};
constexpr int kNumChords = (int)(sizeof(kChords) / sizeof(kChords[0]));

enum ParamIndex
{
    kVolume = 0,
    kLane, kCursor, kField, kValue, kFollow, kLpb, kLength, kStress, kRedraw, kParamCount, kTouchCell,
    kMode, kInst, kOctave, kStep,
    kBlock, kBaseNote, kTrigger, kQuantise, kTicks,
    kFile, kLoad, kRescan,
    kStart, kEnd, kLoop, kLoopStart, kGain, kReverse, kNormalise, kSliceMode, kSlices, kSense,
    kSampleName, kPlaying,
    kRowBase,
    kCursorSlot = kRowBase + kRows * kCols,
    kPlaySlot,
    kActBase,
    kWaveBase   = kActBase + kLanes,
    kStartSlot  = kWaveBase + kWaveBars,
    kEndSlot,
    kLoopSlot,
    kSliceBase,
    kNumParams  = kSliceBase + kWaveBars
};

enum RedrawMode { kNotify = 0, kRebuild, kBoth, kPoll };
enum LoopMode   { kLoopOff = 0, kLoopFwd, kLoopPingPong };
enum SliceMode  { kSliceOff = 0, kSliceGrid, kSliceTransient };

constexpr int kTouchIdle = kRows * kFields;
constexpr int kNoBar     = kWaveBars;   // marker "hidden" value

struct Cell { uint8_t note = 0, inst = 0, vol = 0, cmd = 0, val = 0; };
struct Block { uint8_t length = 64; Cell cells[kLanes][kMaxLines]; };

// Saved per instrument.
struct InstSettings
{
    float start = 0.0f, end = 1.0f, loopStart = 0.0f, gainDb = 0.0f, sense = 0.5f;
    uint8_t loopMode = kLoopOff, reverse = 0, sliceMode = kSliceOff, slices = 8;
    char path[200] = {0};   // "@content/..." or "@samples/..."
};

// A loaded sample (made and freed by the worker thread).
struct SampleBuf
{
    std::vector<float> data;
    double rate = 44100.0;
    float peak = 1.0f;
    int bars[kWaveBars] = {};
    std::vector<uint32_t> onsets;        // transient positions, strongest first
    std::vector<float> onsetStrength;    // 0..1
    char name[64] = {0};
    char path[200] = {0};
    bool keepSettings = false;           // project reload: keep saved start/end/loop
};

const char* const kNoteNames[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};

std::string noteText(int v)
{
    if (v <= 0) return "---";
    if (v == 1) return "OFF";
    int n = v - 2;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%s%d", kNoteNames[n % 12], n / 12);
    return buf;
}
std::string hex2(int v) { char b[8]; std::snprintf(b, sizeof(b), "%02X", v & 0xFF); return b; }
char cmdChar(int v) { return v <= 0 ? '-' : (v <= 10 ? (char)('0' + v - 1) : (char)('A' + v - 11)); }
int noteVal(int octave, int semi) { return 2 + octave * 12 + semi; }

std::string midiNoteName(int n)   // MPC convention: 36 = C1
{
    static const char* const names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char b[16];
    std::snprintf(b, sizeof(b), "%s%d", names[n % 12], n / 12 - 2);
    return b;
}

// ---------------------------------------------------------------------------
// Voice: one sample playing.
struct Voice
{
    const SampleBuf* buf = nullptr;
    int inst = -1;
    double pos = 0.0, baseInc = 1.0, inc = 1.0;
    double start = 0.0, end = 0.0, loopStart = 0.0;
    int loopMode = kLoopOff;
    int dir = 1;
    bool backwards = false;
    float gain = 0.0f, ramp = 0.0f, rampStep = 0.0f;
    bool active = false;

    void fadeOut(float sr) { if (active) rampStep = -1.0f / (0.004f * sr); }

    float next()
    {
        if (!active) return 0.0f;
        const auto& d = buf->data;
        const size_t i = (size_t)pos;
        if (pos < 0.0 || i + 1 >= d.size()) { active = false; return 0.0f; }
        const float f = (float)(pos - (double)i);
        const float s = d[i] + (d[i + 1] - d[i]) * f;

        pos += inc * dir;
        if (dir > 0 && pos >= end)
        {
            if (loopMode == kLoopFwd && end - loopStart > 2.0)            pos = loopStart + std::fmod(pos - end, end - loopStart);
            else if (loopMode == kLoopPingPong && end - loopStart > 2.0)  { pos = end - (pos - end) - 1.0; dir = -1; }
            else active = false;
        }
        else if (dir < 0)
        {
            const double lo = backwards ? start : loopStart;
            if (pos < lo)
            {
                if (!backwards && loopMode == kLoopPingPong) { pos = lo + (lo - pos); dir = 1; }
                else active = false;
            }
        }
        ramp += rampStep;
        if (ramp >= 1.0f) { ramp = 1.0f; rampStep = 0.0f; }
        if (ramp <= 0.0f && rampStep < 0.0f) { active = false; return 0.0f; }
        return s * gain * ramp;
    }
};

// Tracker channel state for one lane.
struct LaneState
{
    Voice v[kVoices];
    int group = 0;               // which half of v[] holds the current chord
    int note = 0, inst = 0;      // last note/instrument played
    float vol = 64.0f;
    double pitch = 0.0;          // semitone offset from slides
    double portaTarget = 0.0;
    double vibPhase = 0.0;
    int cmd = 0, val = 0;        // command on the current line
    int delayTick = -1;
    Cell delayed;
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
        addParam(mpc::intParam   ("inst",   "Inst",   1, kInsts, 1));
        addParam(mpc::intParam   ("octave", "Octave", 0, 8, 3));
        addParam(mpc::intParam   ("step",   "Step",   0, 16, 1));
        addParam(mpc::intParam   ("block",  "Block",  1, kBlocks, 1));
        addParam(mpc::intParam   ("base",   "Base Note", 0, 96, 36));
        addParam(mpc::choiceParam("trigger","Trigger", {"Gate", "One-shot"}, 0));
        addParam(mpc::choiceParam("quant",  "Quantise", {"Off", "Beat", "Bar"}, 0));
        addParam(mpc::intParam   ("ticks",  "Ticks",  1, 12, 6));
        addParam(mpc::intParam   ("file",   "File",   0, kMaxFiles - 1, 0));
        addParam(mpc::boolParam  ("load",   "Load",   false));
        addParam(mpc::boolParam  ("rescan", "Rescan", false));
        addParam(mpc::floatParam ("start",  "Start",  0.0f, 100.0f, 0.0f, "%", 1));
        addParam(mpc::floatParam ("end",    "End",    0.0f, 100.0f, 100.0f, "%", 1));
        addParam(mpc::choiceParam("loop",   "Loop",   {"Off", "Forward", "PingPong"}, 0));
        addParam(mpc::floatParam ("lstart", "Loop Start", 0.0f, 100.0f, 0.0f, "%", 1));
        addParam(mpc::floatParam ("gain",   "Gain",   -24.0f, 24.0f, 0.0f, "dB", 1));
        addParam(mpc::boolParam  ("reverse","Reverse", false));
        addParam(mpc::boolParam  ("normal", "Normalise", false));
        addParam(mpc::choiceParam("slmode", "Slice",  {"Off", "Grid", "Transient"}, 0));
        addParam(mpc::intParam   ("slices", "Slices", 2, kMaxSlices, 8));
        addParam(mpc::floatParam ("sense",  "Sense",  0.0f, 100.0f, 50.0f, "%", 0));
        addDisplay("sname",  "Sample", 4096, 0);
        addDisplay("playing","Playing", kBlocks + 1, 0);

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
            addDisplay(id, name, 101, 0);
        }
        addDisplay("mstart", "StartMark", kWaveBars + 1, 0);
        addDisplay("mend",   "EndMark",   kWaveBars + 1, kWaveBars - 1);
        addDisplay("mloop",  "LoopMark",  kWaveBars + 1, kNoBar);
        for (int b = 0; b < kWaveBars; ++b)
        {
            char id[16], name[16];
            std::snprintf(id, sizeof(id), "slice%02d", b);
            std::snprintf(name, sizeof(name), "Slice %02d", b);
            addDisplay(id, name, 2, 0);
        }

        driveSamplesFolder_ = mpc::detail::parentOf(mpc::detail::parentOf(mpc::pluginFolder())) + "/Twisted Block Samples";
        loadDemoInstruments();
        makeDemoBlocks();
        scanFiles();
        worker_ = std::thread([this] { workerLoop(); });
    }

    ~TwistedBlock() override
    {
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
        for (int i = 0; i < kInsts; ++i)
        {
            delete buf_[i];
            delete pending_[i].exchange(nullptr);
        }
        for (auto& r : retire_) delete r.exchange(nullptr);
    }

    void prepare(double sampleRate, int) override { sampleRate_ = sampleRate; }

    void reset() override
    {
        applyPendingState();
        for (auto& L : lanes_) for (auto& v : L.v) v.active = false;
        last_ = EditKey{};
        lastValue_ = -1;
        lastBlockSync_ = -1;
        lastInstSync_ = -1;
        playing_ = false;
        startPending_ = false;
        heldNote_ = -1;
        lastAbsLine_ = -1;
        forceRedraw_ = true;
    }

    // =========================================================================
    void process(mpc::ProcessContext& ctx) override
    {
        applyPendingState();
        adoptLoadedSamples();
        handleButtons();

        const int lpb   = mpc::clamp(getInt(kLpb), 1, 16);
        const int ticks = mpc::clamp(getInt(kTicks), 1, 12);
        const bool editMode = getInt(kMode) == 1;
        const bool stress = getBool(kStress);
        const double bpm = (ctx.transport.tempoValid && ctx.transport.bpm > 1.0) ? ctx.transport.bpm : 120.0;
        const double linesPerSample = bpm / 60.0 * lpb / sampleRate_;
        const double ppqPerSample = bpm / 60.0 / sampleRate_;
        const float master = mpc::dsp::dbToGain(get(kVolume));
        const bool hostPlaying = ctx.transport.playing && ctx.transport.ppqValid;
        const int editBlock = mpc::clamp(getInt(kBlock), 1, kBlocks) - 1;

        // Edit mode + MPC Play: play the edit block in time with the song.
        if (editMode && hostPlaying && !wasHostPlaying_) lastAbsLine_ = -1;
        if (editMode && !hostPlaying && wasHostPlaying_) stopAll();
        wasHostPlaying_ = hostPlaying;
        if (stress && !stressWasOn_) { stressPos_ = 0.0; lastAbsLine_ = -1; }
        stressWasOn_ = stress;
        if (editMode && (playing_ || startPending_)) { playing_ = false; startPending_ = false; stopAll(); }

        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        int ev = 0;
        playLine_ = -1;

        for (int i = 0; i < ctx.numFrames; ++i)
        {
            while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= i)
                handleMidi(ctx.midi[ev++], editMode, ctx.transport, i, ppqPerSample);

            // Quantised start waiting for its beat/bar?
            if (startPending_ && hostPlaying && ctx.transport.ppqPos + i * ppqPerSample >= startPpq_)
                startBlock(pendingBlock_);

            // Which block/line is playing?
            double pos = -1.0;
            int block = editBlock;
            bool audible = true;
            if (playing_)                       { pos = playPos_; playPos_ += linesPerSample; block = playBlock_; }
            else if (editMode && hostPlaying)   { pos = ctx.transport.ppqPos * lpb + i * linesPerSample; }
            else if (stress)                    { pos = stressPos_; stressPos_ += linesPerSample; audible = false; }

            if (pos >= 0.0)
            {
                const int length = blocks_[block].length;
                const long abs = (long)std::floor(pos);
                if (playing_ && getInt(kTrigger) == 1 && abs >= length)
                {
                    playing_ = false;   // one-shot finished
                    stopAll();
                    pos = -1.0;
                }
                else
                {
                    const int line = (int)(abs % length);
                    const int tick = (int)((pos - std::floor(pos)) * ticks);
                    if (abs != lastAbsLine_)
                    {
                        lastAbsLine_ = abs;
                        lastTick_ = 0;
                        if (audible) doLine(block, line);
                    }
                    else if (tick != lastTick_)
                    {
                        lastTick_ = tick;
                        if (audible) doTick(tick);
                    }
                    playLine_ = line;
                    shownBlock_ = block;
                }
            }
            if (pos < 0.0) lastAbsLine_ = -1;

            float s = 0.0f;
            for (auto& L : lanes_)
                for (auto& v : L.v) s += v.next();
            s *= master;
            outL[i] = s;
            if (outR) outR[i] = s;
        }
        while (ev < ctx.numMidiEvents) handleMidi(ctx.midi[ev++], editMode, ctx.transport, ctx.numFrames, ppqPerSample);
        for (int ch = 2; ch < ctx.numOutputs; ++ch)
            std::memset(ctx.outputs[ch], 0, sizeof(float) * (size_t)ctx.numFrames);
        if (stressPos_ > 1.0e9) stressPos_ = 0.0;

        syncBlockSettings(editBlock);
        syncInstSettings();
        updateEditor(editBlock);
        draw(editBlock, ctx.numFrames);
    }

    // =========================================================================
    bool formatValue(int index, float plain, std::string& out) override
    {
        const int v = (int)std::lround(plain);
        switch (index)
        {
            case kCursor: case kInst: case kBlock: out = hex2(v); return true;
            case kBaseNote: out = midiNoteName(v); return true;
            case kPlaying: out = v == 0 ? "--" : hex2(v); return true;
            case kFile:
            {
                std::lock_guard<std::mutex> lock(filesMutex_);
                out = fileNames_.empty() ? std::string("(no files)") : fileNames_[(size_t)mpc::clamp(v, 0, (int)fileNames_.size() - 1)];
                return true;
            }
            case kSampleName:
            {
                const int inst = mpc::clamp(getInt(kInst), 1, kInsts) - 1;
                out = instName_[inst][0] ? std::string(instName_[inst]) : std::string("(empty)");
                return true;
            }
            case kValue:
                switch (mpc::clamp(getInt(kField), 0, kFields - 1))
                {
                    case 0: out = noteText(mpc::clamp(v, 0, 121)); return true;
                    case 1: out = v == 0 ? "--" : hex2(v); return true;
                    case 2: { int vv = mpc::clamp(v, 0, 65); out = vv == 0 ? "--" : hex2(vv - 1); return true; }
                    case 3: out = std::string(1, cmdChar(mpc::clamp(v, 0, 36))); return true;
                    default: out = hex2(v); return true;
                }
            default: return false;
        }
    }

    // ---- state: blocks + instrument settings --------------------------------
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        const uint8_t header[4] = {'T', 'B', 'K', 2};
        out.insert(out.end(), header, header + 4);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(blocks_);
        out.insert(out.end(), p, p + sizeof(blocks_));
        const uint8_t* q = reinterpret_cast<const uint8_t*>(inst_);
        out.insert(out.end(), q, q + sizeof(inst_));
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        if (size == 4 + sizeof(blocks_) + sizeof(inst_) && std::memcmp(data, "TBK\x02", 4) == 0)
        {
            std::memcpy(stagedBlocks_, data + 4, sizeof(blocks_));
            std::memcpy(stagedInst_, data + 4 + sizeof(blocks_), sizeof(inst_));
        }
        else if (size == 4 + sizeof(Cell) * kLanes * kMaxLines && std::memcmp(data, "TBK\x01", 4) == 0)
        {
            // Phase 0 project: one block, demo instruments.
            for (auto& b : stagedBlocks_) b = Block{};
            std::memcpy(stagedBlocks_[0].cells, data + 4, sizeof(stagedBlocks_[0].cells));
            for (int i = 0; i < kInsts; ++i) stagedInst_[i] = inst_[i];
        }
        else return;
        for (auto& b : stagedBlocks_) b.length = (uint8_t)mpc::clamp((int)b.length, 1, kMaxLines);
        {
            // Ask the worker to reload every instrument that has a file.
            std::lock_guard<std::mutex> lock(reloadMutex_);
            for (int i = 0; i < kInsts; ++i)
            {
                stagedInst_[i].path[sizeof(stagedInst_[i].path) - 1] = 0;
                const bool same = std::strcmp(stagedInst_[i].path, inst_[i].path) == 0 && buf_[i] != nullptr;
                reloadPath_[i] = stagedInst_[i].path;
                reloadReq_[i].store((reloadPath_[i].empty() || same) ? 0 : 1);
            }
        }
        stateLoadPending_.store(true);
    }

private:
    struct EditKey
    {
        int block = -1, lane = -1, line = -1, field = -1;
        bool operator==(const EditKey& o) const { return block == o.block && lane == o.lane && line == o.line && field == o.field; }
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Twisted Block";
        info.vendor       = "Twisted Genetics";
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);
        info.version      = 1100;
        info.isInstrument = true;
        info.numInputs    = 0;
        info.numOutputs   = 2;
        info.tailSamples  = 4410;
        return info;
    }

    void addDisplay(const char* id, const char* name, int frames, int def)
    {
        addParam(mpc::intParam(id, name, 0, frames - 1, def));
    }

    void applyPendingState()
    {
        if (!stateLoadPending_.exchange(false)) return;
        std::memcpy(blocks_, stagedBlocks_, sizeof(blocks_));
        std::memcpy(inst_, stagedInst_, sizeof(inst_));
        for (int i = 0; i < kInsts; ++i) rebuildSlices(i);
        lastBlockSync_ = -1;
        lastInstSync_ = -1;
        forceRedraw_ = true;
    }

    // =========================================================================
    // MIDI
    void handleMidi(const mpc::MidiEvent& e, bool editMode, const mpc::Transport& t, int offset, double ppqPerSample)
    {
        if (e.isNoteOn())
        {
            if (editMode) { enterNote(e.note()); return; }
            const int block = e.note() - getInt(kBaseNote);
            if (block < 0 || block >= kBlocks) return;
            heldNote_ = e.note();
            const int q = getInt(kQuantise);
            if (q == 0 || !(t.playing && t.ppqValid)) { startBlock(block); return; }
            // Wait for the next beat or bar.
            const double now = t.ppqPos + offset * ppqPerSample;
            const double unit = q == 1 ? 1.0 : (double)std::max(1, t.timeSigNumerator) * 4.0 / std::max(1, t.timeSigDenominator);
            const double origin = q == 2 ? t.barStartPpq : 0.0;
            double target = origin + std::ceil((now - origin) / unit - 1e-6) * unit;
            if (target < now) target += unit;
            startPpq_ = target;
            pendingBlock_ = block;
            startPending_ = true;
        }
        else if (e.isNoteOff())
        {
            if (editMode) { stopLane(mpc::clamp(getInt(kLane), 0, kLanes - 1)); return; }
            if (e.note() == heldNote_)
            {
                heldNote_ = -1;
                startPending_ = false;
                if (getInt(kTrigger) == 0 && playing_) { playing_ = false; stopAll(); }
            }
        }
        else if (e.isAllNotesOff())
        {
            heldNote_ = -1;
            startPending_ = false;
            playing_ = false;
            stopAll();
        }
    }

    void startBlock(int block)
    {
        startPending_ = false;
        playing_ = true;
        playBlock_ = block;
        playPos_ = 0.0;
        lastAbsLine_ = -1;
        for (auto& L : lanes_) { L.pitch = 0.0; L.cmd = 0; L.delayTick = -1; }
    }

    // Edit mode: a pad writes a note at the cursor and plays it.
    void enterNote(int midiNote)
    {
        const int block = mpc::clamp(getInt(kBlock), 1, kBlocks) - 1;
        const int length = blocks_[block].length;
        const int lane = mpc::clamp(getInt(kLane), 0, kLanes - 1);
        const int line = mpc::clamp(getInt(kCursor), 0, length - 1);
        const int inst = mpc::clamp(getInt(kInst), 1, kInsts);
        const int note = mpc::clamp(2 + (midiNote - 36) + getInt(kOctave) * 12, 2, 121);
        Cell& c = blocks_[block].cells[lane][line];
        c.note = (uint8_t)note;
        c.inst = (uint8_t)inst;
        Cell play = c;
        play.vol = 0;
        if (c.cmd != kCmdChord) { play.cmd = 0; play.val = 0; }
        triggerCell(lane, play);
        const int step = getInt(kStep);
        if (step > 0) set(kCursor, (float)((line + step) % length));
        last_ = EditKey{};
        forceRedraw_ = true;
    }

    // =========================================================================
    // Sequencer
    void doLine(int block, int line)
    {
        for (int l = 0; l < kLanes; ++l)
        {
            LaneState& L = lanes_[l];
            const Cell& c = blocks_[block].cells[l][line];
            L.cmd = c.cmd;
            L.val = c.val;
            L.delayTick = -1;
            if (c.cmd == kCmdDelay && c.val > 0 && c.note >= 1)
            {
                L.delayTick = c.val;
                L.delayed = c;
                continue;
            }
            triggerCell(l, c);
            if (c.cmd == kCmdVol) setLaneVol(l, (float)std::min((int)c.val, 64));
            if (c.cmd == kCmdCut && c.val == 0) stopLane(l);
        }
    }

    void doTick(int tick)
    {
        for (int l = 0; l < kLanes; ++l)
        {
            LaneState& L = lanes_[l];
            if (L.delayTick == tick) { triggerCell(l, L.delayed); L.delayTick = -1; }
            const int x = L.val >> 4, y = L.val & 15;
            switch (L.cmd)
            {
                case kCmdArp:    if (L.val) setLanePitch(l, L.pitch + (tick % 3 == 1 ? x : tick % 3 == 2 ? y : 0)); break;
                case kCmdUp:     L.pitch += L.val / 16.0; setLanePitch(l, L.pitch); break;
                case kCmdDown:   L.pitch -= L.val / 16.0; setLanePitch(l, L.pitch); break;
                case kCmdPorta:
                {
                    const double step = L.val / 16.0;
                    if (L.pitch < L.portaTarget) L.pitch = std::min(L.portaTarget, L.pitch + step);
                    else                         L.pitch = std::max(L.portaTarget, L.pitch - step);
                    setLanePitch(l, L.pitch);
                    break;
                }
                case kCmdVib:
                    L.vibPhase += x * 0.25;
                    setLanePitch(l, L.pitch + std::sin(L.vibPhase) * y / 8.0);
                    break;
                case kCmdVSlide: setLaneVol(l, mpc::clamp(L.vol + (float)(x - y), 0.0f, 64.0f)); break;
                case kCmdCut:    if (tick == L.val) stopLane(l); break;
                case kCmdRetrig: if (L.val > 0 && tick % L.val == 0) retrigLane(l); break;
                default: break;
            }
        }
    }

    void triggerCell(int lane, const Cell& c)
    {
        LaneState& L = lanes_[lane];
        if (c.note == 1) { stopLane(lane); return; }
        if (c.note < 2) return;
        const int inst = c.inst ? c.inst : L.inst;
        if (inst < 1 || inst > kInsts) return;
        if (c.vol) L.vol = (float)(c.vol - 1);
        else if (c.inst) L.vol = 64.0f;
        if (c.cmd == kCmdVol) L.vol = (float)std::min((int)c.val, 64);

        // Tone portamento: glide the playing note instead of retriggering.
        if (c.cmd == kCmdPorta && laneActive(lane) && L.inst == inst)
        {
            L.portaTarget = (double)(c.note - L.note);
            return;
        }
        L.note = c.note;
        L.inst = inst;
        L.pitch = 0.0;
        L.portaTarget = 0.0;
        L.vibPhase = 0.0;

        const Chord& ch = kChords[(c.cmd == kCmdChord && c.val < kNumChords) ? c.val : 0];
        const bool backwards = c.cmd == kCmdBack;
        const double offset = c.cmd == kCmdOffs ? c.val * 256.0 : 0.0;

        // Fade the old chord, start the new one in the other half.
        for (int k = 0; k < kChordMax; ++k) L.v[L.group * kChordMax + k].fadeOut((float)sampleRate_);
        L.group ^= 1;
        for (int k = 0; k < kChordMax; ++k)
        {
            Voice& v = L.v[L.group * kChordMax + k];
            v.active = false;
            if (k < ch.n) startVoice(v, inst - 1, c.note + ch.s[k], backwards, offset);
        }
        applyLaneGain(lane);
    }

    void startVoice(Voice& v, int inst, int note, bool backwards, double offset)
    {
        const SampleBuf* b = buf_[inst];
        if (!b || b->data.size() < 4) return;
        const InstSettings& s = inst_[inst];
        const double n = (double)b->data.size() - 2.0;
        double start = s.start * n, end = std::max(start + 2.0, (double)s.end * n);
        int semis = note - kNoteC3;

        // Sliced instrument: note C-3 + k plays slice k at its own pitch.
        if (s.sliceMode != kSliceOff)
        {
            const int k = semis;
            if (k < 0 || k >= numSlices_[inst]) return;
            start = sliceStart_[inst][k] * n;
            end = (k + 1 < numSlices_[inst] ? sliceStart_[inst][k + 1] : s.end) * n;
            semis = 0;
        }
        const bool rev = backwards != (s.reverse != 0);
        v.buf = b;
        v.inst = inst;
        v.start = start;
        v.end = end;
        v.loopStart = std::min(std::max((double)s.loopStart * n, start), end - 2.0);
        v.loopMode = (s.sliceMode == kSliceOff && !rev) ? (int)s.loopMode : (int)kLoopOff;
        v.backwards = rev;
        v.dir = rev ? -1 : 1;
        v.pos = rev ? end - 1.0 - offset : start + offset;
        v.baseInc = std::pow(2.0, semis / 12.0) * b->rate / sampleRate_;
        v.inc = v.baseInc;
        v.gain = 0.0f;   // set by applyLaneGain
        v.ramp = 0.0f;
        v.rampStep = 1.0f / (0.001f * (float)sampleRate_);
        v.active = rev ? v.pos > start : v.pos < end;
    }

    bool laneActive(int lane) const
    {
        for (const auto& v : lanes_[lane].v) if (v.active && v.rampStep >= 0.0f) return true;
        return false;
    }

    void setLanePitch(int lane, double semis)
    {
        LaneState& L = lanes_[lane];
        const double ratio = std::pow(2.0, semis / 12.0);
        for (int k = 0; k < kChordMax; ++k)
        {
            Voice& v = L.v[L.group * kChordMax + k];
            if (v.active) v.inc = v.baseInc * ratio;
        }
    }

    void setLaneVol(int lane, float vol)
    {
        lanes_[lane].vol = vol;
        applyLaneGain(lane);
    }

    void applyLaneGain(int lane)
    {
        LaneState& L = lanes_[lane];
        int n = 0;
        for (int j = 0; j < kChordMax; ++j) if (L.v[L.group * kChordMax + j].active) ++n;
        const float chord = n > 1 ? 1.0f / std::sqrt((float)n) : 1.0f;
        for (int k = 0; k < kChordMax; ++k)
        {
            Voice& v = L.v[L.group * kChordMax + k];
            if (!v.active || v.inst < 0) continue;
            v.gain = mpc::dsp::dbToGain(inst_[v.inst].gainDb) * (L.vol / 64.0f) * chord * 0.4f;
        }
    }

    void retrigLane(int lane)
    {
        LaneState& L = lanes_[lane];
        for (int k = 0; k < kChordMax; ++k)
        {
            Voice& v = L.v[L.group * kChordMax + k];
            if (!v.buf) continue;
            v.pos = v.backwards ? v.end - 1.0 : v.start;
            v.dir = v.backwards ? -1 : 1;
            v.ramp = 0.0f;
            v.rampStep = 1.0f / (0.001f * (float)sampleRate_);
            v.active = true;
        }
    }

    void stopLane(int lane) { for (auto& v : lanes_[lane].v) v.fadeOut((float)sampleRate_); }
    void stopAll() { for (int l = 0; l < kLanes; ++l) stopLane(l); }

    // =========================================================================
    // Editing (audio thread, after the audio is rendered)
    void syncBlockSettings(int block)
    {
        if (block != lastBlockSync_)
        {
            lastBlockSync_ = block;
            set(kLength, (float)blocks_[block].length);
            notify(kLength);
            last_ = EditKey{};
        }
        else
        {
            const int len = mpc::clamp(getInt(kLength), 1, kMaxLines);
            if (len != blocks_[block].length) { blocks_[block].length = (uint8_t)len; forceRedraw_ = true; }
        }
    }

    void syncInstSettings()
    {
        const int inst = mpc::clamp(getInt(kInst), 1, kInsts) - 1;
        InstSettings& s = inst_[inst];
        if (inst != lastInstSync_)
        {
            lastInstSync_ = inst;
            writeInstParams(s);
            sampleNameDirty_ = true;
            forceRedraw_ = true;
            return;
        }
        bool changed = false;
        auto pull = [&](int p, float& field, float scale) {
            const float v = get(p);
            if (v != lastInst_[p - kStart]) { field = v / scale; lastInst_[p - kStart] = v; changed = true; }
        };
        auto pullI = [&](int p, uint8_t& field) {
            const float v = (float)getInt(p);
            if (v != lastInst_[p - kStart]) { field = (uint8_t)v; lastInst_[p - kStart] = v; changed = true; }
        };
        pull(kStart, s.start, 100.0f);
        pull(kEnd, s.end, 100.0f);
        pullI(kLoop, s.loopMode);
        pull(kLoopStart, s.loopStart, 100.0f);
        pull(kGain, s.gainDb, 1.0f);
        pullI(kReverse, s.reverse);
        pullI(kSliceMode, s.sliceMode);
        pullI(kSlices, s.slices);
        pull(kSense, s.sense, 100.0f);
        if (changed)
        {
            if (s.end < s.start + 0.001f) s.end = std::min(1.0f, s.start + 0.001f);
            rebuildSlices(inst);
            for (int l = 0; l < kLanes; ++l) applyLaneGain(l);
            forceRedraw_ = true;
        }
    }

    void writeInstParams(const InstSettings& s)
    {
        set(kStart, s.start * 100.0f);
        set(kEnd, s.end * 100.0f);
        set(kLoop, (float)s.loopMode);
        set(kLoopStart, s.loopStart * 100.0f);
        set(kGain, s.gainDb);
        set(kReverse, (float)s.reverse);
        set(kSliceMode, (float)s.sliceMode);
        set(kSlices, (float)s.slices);
        set(kSense, s.sense * 100.0f);
        // Remember what the parameters hold now, so only real changes are pulled back.
        for (int p = kStart; p <= kSense; ++p)
        {
            const bool isInt = p == kLoop || p == kReverse || p == kSliceMode || p == kSlices || p == kNormalise;
            lastInst_[p - kStart] = isInt ? (float)getInt(p) : get(p);
            notify(p);
        }
    }

    // Buttons: Load, Rescan, Normalise (the plugin switches them back off).
    void handleButtons()
    {
        if (getBool(kLoad))
        {
            const int inst = mpc::clamp(getInt(kInst), 1, kInsts) - 1;
            loadIndex_[inst].store(getInt(kFile));
            loadReq_[inst].store(1);
            set(kLoad, 0.0f); notify(kLoad);
        }
        if (getBool(kRescan))
        {
            rescanReq_.store(true);
            set(kRescan, 0.0f); notify(kRescan);
        }
        if (getBool(kNormalise))
        {
            const int inst = mpc::clamp(getInt(kInst), 1, kInsts) - 1;
            if (buf_[inst] && buf_[inst]->peak > 1e-4f)
            {
                set(kGain, mpc::clamp(-20.0f * std::log10(buf_[inst]->peak) - 0.3f, -24.0f, 24.0f));
                notify(kGain);
            }
            set(kNormalise, 0.0f); notify(kNormalise);
        }
    }

    void rebuildSlices(int inst)
    {
        const InstSettings& s = inst_[inst];
        int n = 0;
        if (s.sliceMode == kSliceGrid)
        {
            n = mpc::clamp((int)s.slices, 2, kMaxSlices);
            for (int k = 0; k < n; ++k) sliceStart_[inst][k] = s.start + (s.end - s.start) * (float)k / (float)n;
        }
        else if (s.sliceMode == kSliceTransient && buf_[inst])
        {
            const SampleBuf* b = buf_[inst];
            const float len = (float)std::max<size_t>(1, b->data.size());
            const float threshold = 1.0f - s.sense;
            float pos[kMaxSlices];
            pos[n++] = s.start;
            for (size_t k = 0; k < b->onsets.size() && n < kMaxSlices; ++k)
            {
                if (b->onsetStrength[k] < threshold) break;   // strongest first
                const float p = (float)b->onsets[k] / len;
                if (p > s.start + 0.002f && p < s.end - 0.002f) pos[n++] = p;
            }
            std::sort(pos, pos + n);
            for (int k = 0; k < n; ++k) sliceStart_[inst][k] = pos[k];
        }
        numSlices_[inst] = n;
    }

    void updateEditor(int block)
    {
        const int mode = mpc::clamp(getInt(kRedraw), 0, 3);
        notifyOn_ = (mode == kNotify || mode == kBoth);
        const int length = blocks_[block].length;

        int lane   = mpc::clamp(getInt(kLane), 0, kLanes - 1);
        int cursor = mpc::clamp(getInt(kCursor), 0, length - 1);
        int field  = mpc::clamp(getInt(kField), 0, kFields - 1);
        const bool follow = getBool(kFollow);
        const bool showingPlay = follow && playLine_ >= 0;
        const int top = (showingPlay ? playLine_ : cursor) - kCentreRow;

        const int touch = getInt(kTouchCell);
        if (touch >= 0 && touch < kTouchIdle)
        {
            const int line = top + touch / kFields;
            if (!(showingPlay && shownBlock_ != block) && line >= 0 && line < length) { cursor = line; field = touch % kFields; }
            set(kCursor, (float)cursor);
            set(kField, (float)field);
            set(kTouchCell, (float)kTouchIdle);
            notify(kTouchCell);
        }
        else if (getInt(kCursor) != cursor) set(kCursor, (float)cursor);

        EditKey key{block, lane, cursor, field};
        const int value = getInt(kValue);
        if (!(key == last_))
        {
            last_ = key;
            lastValue_ = cellValue(block, lane, cursor, field);
            set(kValue, (float)lastValue_);
            notify(kCursor); notify(kField); notify(kValue);
            forceRedraw_ = true;
        }
        else if (value != lastValue_)
        {
            const int v = mpc::clamp(value, 0, kFieldMax[field]);
            setCellValue(block, lane, cursor, field, v);
            lastValue_ = v;
            if (v != value) { set(kValue, (float)v); notify(kValue); }
            forceRedraw_ = true;
        }
    }

    // =========================================================================
    // Drawing
    void draw(int editBlock, int frames)
    {
        const int mode = mpc::clamp(getInt(kRedraw), 0, 3);
        changed_ = false;

        // With Follow on, show whatever is playing.
        const bool follow = getBool(kFollow);
        const bool showingPlay = follow && playLine_ >= 0;
        const int block = showingPlay ? shownBlock_ : editBlock;
        const int length = blocks_[block].length;
        const int lane   = mpc::clamp(getInt(kLane), 0, kLanes - 1);
        const int cursor = mpc::clamp(getInt(kCursor), 0, blocks_[editBlock].length - 1);
        const int top = (showingPlay ? playLine_ : cursor) - kCentreRow;

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
            const Cell& c = blocks_[block].cells[lane][line];
            show(base + 0, line);
            show(base + 1, c.note);
            show(base + 2, c.inst);
            show(base + 3, c.vol);
            show(base + 4, c.cmd);
            show(base + 5, (c.cmd == 0 && c.val == 0) ? 256 : c.val);
        }
        const int cslot = (block == editBlock) ? cursor - top : -1;
        show(kCursorSlot, (cslot >= 0 && cslot < kRows) ? cslot : kRows);
        const int pslot = playLine_ - top;
        show(kPlaySlot, (playLine_ >= 0 && pslot >= 0 && pslot < kRows) ? pslot : kRows);
        const int actLine = playLine_ >= 0 ? playLine_ : cursor;
        for (int l = 0; l < kLanes; ++l)
            show(kActBase + l, (actLine < length && blocks_[block].cells[l][actLine].note >= 2) ? 1 : 0);
        show(kPlaying, playing_ ? playBlock_ + 1 : 0);

        // Sample view of the current instrument.
        const int inst = mpc::clamp(getInt(kInst), 1, kInsts) - 1;
        const SampleBuf* b = buf_[inst];
        const InstSettings& s = inst_[inst];
        auto bar = [](float pos) { return mpc::clamp((int)(pos * kWaveBars), 0, kWaveBars - 1); };
        for (int k = 0; k < kWaveBars; ++k) show(kWaveBase + k, b ? b->bars[k] : 0);
        show(kStartSlot, b ? bar(s.start) : kNoBar);
        show(kEndSlot, b ? bar(s.end - 0.0001f) : kNoBar);
        show(kLoopSlot, (b && s.loopMode != kLoopOff) ? bar(s.loopStart) : kNoBar);
        int sliceBars[kWaveBars] = {};
        if (b && s.sliceMode != kSliceOff)
            for (int k = 0; k < numSlices_[inst]; ++k) sliceBars[bar(sliceStart_[inst][k])] = 1;
        for (int k = 0; k < kWaveBars; ++k) show(kSliceBase + k, sliceBars[k]);
        show(kParamCount, kNumParams);
        if (sampleNameDirty_)
        {
            // A new value makes the MPC re-read the name text.
            sampleNameDirty_ = false;
            set(kSampleName, (float)((sampleSerial_ * kInsts + inst) % 4096));
            changed_ = true;
            notify(kSampleName);
        }

        rebuildTimer_ += frames;
        if ((mode == kRebuild || mode == kBoth) && changed_ && rebuildTimer_ >= (int)(sampleRate_ / 8.0))
        {
            rebuildTimer_ = 0;
            requestDisplayUpdate();
        }
        forceRedraw_ = false;
    }

    void show(int index, int v)
    {
        const bool differs = getInt(index) != v;
        if (!differs && !forceRedraw_) return;
        if (differs) set(index, (float)v);
        changed_ = true;
        notify(index);
    }

    void notify(int index) { if (notifyOn_) notifyHostParameterChanged(index); }

    int cellValue(int block, int lane, int line, int field) const
    {
        const Cell& c = blocks_[block].cells[lane][line];
        switch (field) { case 0: return c.note; case 1: return c.inst; case 2: return c.vol; case 3: return c.cmd; default: return c.val; }
    }

    void setCellValue(int block, int lane, int line, int field, int v)
    {
        Cell& c = blocks_[block].cells[lane][line];
        switch (field) { case 0: c.note = (uint8_t)v; break; case 1: c.inst = (uint8_t)v; break;
                         case 2: c.vol = (uint8_t)v; break; case 3: c.cmd = (uint8_t)v; break; default: c.val = (uint8_t)v; break; }
    }

    // =========================================================================
    // Samples: worker thread side
    SampleBuf* loadSample(const std::string& ref)
    {
        const std::string path = resolve(ref);
        dsp::WavData w;
        if (path.empty() || !dsp::loadWav(path, w) || w.numFrames() < 4) return nullptr;
        auto* b = new SampleBuf();
        b->rate = w.sampleRate;
        b->data.assign(w.numFrames(), 0.0f);
        for (int ch = 0; ch < w.numChannels(); ++ch)
            for (size_t f = 0; f < w.numFrames(); ++f)
                b->data[f] += w.channels[(size_t)ch][f] / (float)w.numChannels();
        float peak = 0.0f;
        for (float x : b->data) peak = std::max(peak, std::fabs(x));
        b->peak = peak;
        for (int k = 0; k < kWaveBars; ++k)
        {
            const size_t s0 = b->data.size() * (size_t)k / kWaveBars, s1 = b->data.size() * (size_t)(k + 1) / kWaveBars;
            float p = 0.0f;
            for (size_t i = s0; i < s1; ++i) p = std::max(p, std::fabs(b->data[i]));
            b->bars[k] = (int)std::lround(std::min(1.0f, p) * 100.0f);
        }
        findOnsets(*b);
        std::string base = mpc::detail::baseNameOf(path);
        const size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) base = base.substr(0, dot);
        std::snprintf(b->name, sizeof(b->name), "%s", base.c_str());
        std::snprintf(b->path, sizeof(b->path), "%s", ref.c_str());
        return b;
    }

    // Energy-based transient detection: rises in 256-sample RMS.
    static void findOnsets(SampleBuf& b)
    {
        const size_t hop = 256;
        const size_t frames = b.data.size() / hop;
        if (frames < 4) return;
        std::vector<float> env(frames);
        for (size_t f = 0; f < frames; ++f)
        {
            double e = 0.0;
            for (size_t i = 0; i < hop; ++i) { const float x = b.data[f * hop + i]; e += x * x; }
            env[f] = (float)std::sqrt(e / hop);
        }
        std::vector<std::pair<float, uint32_t>> cand;
        float maxFlux = 1e-9f;
        for (size_t f = 1; f + 1 < frames; ++f)
        {
            const float flux = env[f] - env[f - 1];
            if (flux > 0.0f && flux >= env[f + 1] - env[f])
            {
                cand.push_back({flux, (uint32_t)((f - 1) * hop)});
                maxFlux = std::max(maxFlux, flux);
            }
        }
        std::sort(cand.begin(), cand.end(), [](const auto& a, const auto& c) { return a.first > c.first; });
        const uint32_t minGap = (uint32_t)(b.rate * 0.05);   // keep onsets ~50 ms apart
        for (const auto& c : cand)
        {
            bool close = false;
            for (uint32_t o : b.onsets) if ((c.second > o ? c.second - o : o - c.second) < minGap) { close = true; break; }
            if (close) continue;
            b.onsets.push_back(c.second);
            b.onsetStrength.push_back(c.first / maxFlux);
            if (b.onsets.size() >= 256) break;
        }
    }

    std::string resolve(const std::string& ref) const
    {
        if (ref.rfind("@content/", 0) == 0) return mpc::contentPath(ref.substr(9));
        if (ref.rfind("@samples/", 0) == 0) return driveSamplesFolder_ + "/" + ref.substr(9);
        return ref;
    }

    static bool isWav(const std::string& n)
    {
        if (n.size() < 5 || n[0] == '.') return false;
        std::string e = n.substr(n.size() - 4);
        for (char& ch : e) ch = (char)std::tolower((unsigned char)ch);
        return e == ".wav";
    }

    // Lists WAVs in <drive>/Twisted Block Samples (and its subfolders, one level)
    // and in the plugin's Content/Samples.
    void scanFiles()
    {
        std::vector<std::string> refs, names;
        auto scanDir = [&](const std::string& dir, const std::string& ref, const std::string& label, bool recurse, auto& self) -> void {
            DIR* d = opendir(dir.c_str());
            if (!d) return;
            std::vector<std::string> entries;
            while (dirent* e = readdir(d)) entries.push_back(e->d_name);
            closedir(d);
            std::sort(entries.begin(), entries.end());
            for (const auto& n : entries)
            {
                if (n.empty() || n[0] == '.' || refs.size() >= (size_t)kMaxFiles) continue;
                if (isWav(n))
                {
                    refs.push_back(ref + n);
                    names.push_back(label + n.substr(0, n.size() - 4));
                }
                else if (recurse)
                {
                    DIR* sub = opendir((dir + "/" + n).c_str());
                    if (sub) { closedir(sub); self(dir + "/" + n, ref + n + "/", label + n + "/", false, self); }
                }
            }
        };
        scanDir(driveSamplesFolder_, "@samples/", "", true, scanDir);
        scanDir(mpc::contentPath("Samples"), "@content/Samples/", "demo/", false, scanDir);
        std::lock_guard<std::mutex> lock(filesMutex_);
        files_ = refs;
        fileNames_ = names;
    }

    void workerLoop()
    {
        while (!stop_.load())
        {
            if (rescanReq_.exchange(false)) scanFiles();
            for (int i = 0; i < kInsts; ++i)
            {
                SampleBuf* b = nullptr;
                if (loadReq_[i].exchange(0))
                {
                    std::string ref;
                    {
                        std::lock_guard<std::mutex> lock(filesMutex_);
                        const int idx = loadIndex_[i].load();
                        if (idx >= 0 && idx < (int)files_.size()) ref = files_[(size_t)idx];
                    }
                    if (!ref.empty()) b = loadSample(ref);
                }
                else if (reloadReq_[i].exchange(0))
                {
                    std::string ref;
                    { std::lock_guard<std::mutex> lock(reloadMutex_); ref = reloadPath_[i]; }
                    b = loadSample(ref);
                    if (b) b->keepSettings = true;
                }
                if (b) delete pending_[i].exchange(b);
            }
            for (auto& r : retire_) delete r.exchange(nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }

    // Audio thread: take over finished loads.
    void adoptLoadedSamples()
    {
        for (int i = 0; i < kInsts; ++i)
        {
            if (!pending_[i].load(std::memory_order_acquire)) continue;
            int slot = -1;
            for (int r = 0; r < (int)(sizeof(retire_) / sizeof(retire_[0])); ++r)
                if (!retire_[r].load()) { slot = r; break; }
            if (slot < 0) continue;   // try again next block
            SampleBuf* b = pending_[i].exchange(nullptr);
            if (!b) continue;
            for (auto& L : lanes_)
                for (auto& v : L.v)
                    if (v.inst == i) { v.active = false; v.buf = nullptr; v.inst = -1; }
            SampleBuf* old = buf_[i];
            buf_[i] = b;
            if (old) retire_[slot].store(old);
            std::memcpy(instName_[i], b->name, sizeof(instName_[i]));
            InstSettings& s = inst_[i];
            if (!b->keepSettings) s = InstSettings{};
            std::memcpy(s.path, b->path, sizeof(s.path));
            rebuildSlices(i);
            ++sampleSerial_;
            sampleNameDirty_ = true;
            if (i == mpc::clamp(getInt(kInst), 1, kInsts) - 1) lastInstSync_ = -1;
            forceRedraw_ = true;
        }
    }

    // =========================================================================
    // Content
    void loadDemoInstruments()
    {
        static const char* const files[4] = {"@content/Samples/inst01_break.wav", "@content/Samples/inst02_sub.wav",
                                             "@content/Samples/inst03_hat.wav", "@content/Samples/inst04_stab.wav"};
        for (int i = 0; i < 4; ++i)
        {
            buf_[i] = loadSample(files[i]);
            if (buf_[i])
            {
                std::memcpy(instName_[i], buf_[i]->name, sizeof(instName_[i]));
                std::snprintf(inst_[i].path, sizeof(inst_[i].path), "%s", files[i]);
            }
        }
        for (int i = 0; i < kInsts; ++i) rebuildSlices(i);
    }

    void makeDemoBlocks()
    {
        auto put = [this](int block, int lane, int line, int note, int inst, int vol = 0, int cmd = 0, int val = 0)
        {
            Cell& c = blocks_[block].cells[lane][line];
            c.note = (uint8_t)note; c.inst = (uint8_t)inst; c.vol = (uint8_t)vol; c.cmd = (uint8_t)cmd; c.val = (uint8_t)val;
        };
        auto slice = [](int step) { return (int)std::lround(step * (44100.0 * 60.0 / 170.0 / 4.0) / 256.0); };
        // Block 01: chopped break, sub with a glide, hats, minor stab chords.
        for (int bar = 0; bar < 4; ++bar)
        {
            const int o = bar * 16;
            put(0, 0, o + 0,  kNoteC3, 1, 0, kCmdOffs, slice(0));
            put(0, 0, o + 4,  kNoteC3, 1, 0, kCmdOffs, slice(4));
            put(0, 0, o + 7,  kNoteC3, 1, 0, kCmdOffs, slice(7));
            put(0, 0, o + 10, kNoteC3, 1, 0, kCmdOffs, slice(10));
            put(0, 0, o + 12, kNoteC3, 1, 0, kCmdOffs, slice(bar == 3 ? 28 : 12));
            if (bar == 3) put(0, 0, o + 14, kNoteC3, 1, 0x31, kCmdRetrig, 2);
            put(0, 1, o + 0, noteVal(3, bar == 2 ? 3 : 0), 2, 0x41);
            put(0, 1, o + 8, noteVal(3, bar == 2 ? 5 : 7), 2, 0x39, kCmdPorta, 0x20);
            put(0, 1, o + 14, 1, 0);
            for (int s = 2; s < 16; s += 4) put(0, 2, o + s, kNoteC3, 3, s == 14 ? 0x19 : 0x2D);
            if (bar % 2 == 0) put(0, 3, o + 0, noteVal(3, 2), 4, 0x35, kCmdChord, 2);
        }
        // Block 02: half-time break with a reversed hit, sub drop, chords.
        blocks_[1].length = 32;
        put(1, 0, 0,  kNoteC3, 1, 0, kCmdOffs, slice(0));
        put(1, 0, 8,  kNoteC3, 1, 0, kCmdOffs, slice(4));
        put(1, 0, 16, kNoteC3, 1, 0, kCmdOffs, slice(0));
        put(1, 0, 24, kNoteC3, 1, 0, kCmdBack, 0);
        put(1, 1, 0,  noteVal(2, 10), 2, 0x41, kCmdDown, 0x04);
        put(1, 1, 16, noteVal(3, 3), 2, 0x41);
        put(1, 3, 0,  noteVal(3, 0), 4, 0x30, kCmdChord, 4);
        put(1, 3, 16, noteVal(2, 10), 4, 0x30, kCmdChord, 3);
        // Block 03: rolling hats with retrigger and a volume fade.
        blocks_[2].length = 16;
        for (int s = 0; s < 16; s += 2) put(2, 2, s, kNoteC3, 3, 0x40, s % 4 == 0 ? kCmdRetrig : 0, s % 4 == 0 ? 3 : 0);
        put(2, 2, 14, kNoteC3, 3, 0x40, kCmdVSlide, 0x04);
    }

    // =========================================================================
    Block blocks_[kBlocks];
    Block stagedBlocks_[kBlocks];
    InstSettings inst_[kInsts];
    InstSettings stagedInst_[kInsts];
    std::atomic<bool> stateLoadPending_{false};

    SampleBuf* buf_[kInsts] = {};
    std::atomic<SampleBuf*> pending_[kInsts] = {};
    std::atomic<SampleBuf*> retire_[8] = {};
    char instName_[kInsts][64] = {};
    float sliceStart_[kInsts][kMaxSlices] = {};
    int numSlices_[kInsts] = {};
    int sampleSerial_ = 0;
    bool sampleNameDirty_ = true;

    // worker
    std::thread worker_;
    std::atomic<bool> stop_{false}, rescanReq_{false};
    std::atomic<int> loadReq_[kInsts] = {}, loadIndex_[kInsts] = {}, reloadReq_[kInsts] = {};
    std::mutex filesMutex_, reloadMutex_;
    std::vector<std::string> files_, fileNames_;
    std::string reloadPath_[kInsts];
    std::string driveSamplesFolder_;

    LaneState lanes_[kLanes];

    EditKey last_;
    int lastValue_ = -1, lastBlockSync_ = -1, lastInstSync_ = -1;
    float lastInst_[kSense - kStart + 1] = {};
    double sampleRate_ = 44100.0;

    bool playing_ = false, startPending_ = false, wasHostPlaying_ = false, stressWasOn_ = false;
    int playBlock_ = 0, pendingBlock_ = 0, heldNote_ = -1, shownBlock_ = 0;
    double playPos_ = 0.0, stressPos_ = 0.0, startPpq_ = 0.0;
    long lastAbsLine_ = -1;
    int lastTick_ = 0, playLine_ = -1;

    bool notifyOn_ = true, changed_ = false, forceRedraw_ = true;
    int rebuildTimer_ = 0;
};

MPC_PLUGIN_ENTRY(TwistedBlock)
