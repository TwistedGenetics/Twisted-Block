// test_grid.cpp - desktop tests for Twisted Block.
//
// Loads the natively built plugin (.so/.dylib), drives it like the MPC would
// (parameters, MIDI, transport) and checks display parameters and audio.
// With --dump <file.json> it writes every parameter's normalised value and
// text, which tools/preview_skin.py uses to draw a mock-up of the MPC screen.
//
// Scenarios: edit, play, quantise, effects, samples, stress
//
// Build:  c++ -std=c++17 -O2 -I<sdk>/framework -o test_grid tools/test_grid.cpp -ldl

#include "mpc_vst2_abi.h"

#include <dlfcn.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

double gPpq = 0.0, gBpm = 170.0;
bool gPlaying = false;
VstTimeInfo gTime;

VstIntPtr host(AEffect*, VstInt32 opcode, VstInt32, VstIntPtr, void*, float)
{
    if (opcode == audioMasterVersion) return 2400;
    if (opcode == audioMasterGetTime)
    {
        std::memset(&gTime, 0, sizeof(gTime));
        gTime.sampleRate = 44100.0;
        gTime.tempo = gBpm;
        gTime.ppqPos = gPpq;
        gTime.barStartPos = std::floor(gPpq / 4.0) * 4.0;
        gTime.timeSigNumerator = 4;
        gTime.timeSigDenominator = 4;
        gTime.flags = kVstTempoValid | kVstPpqPosValid | kVstBarsValid | kVstTimeSigValid | (gPlaying ? kVstTransportPlaying : 0);
        return (VstIntPtr)&gTime;
    }
    return 0;
}

AEffect* fx = nullptr;
int failures = 0;
float bufL[512], bufR[512];
float lastPeak = 0.0f;

VstIntPtr disp(VstInt32 op, VstInt32 idx = 0, VstIntPtr v = 0, void* p = nullptr, float o = 0)
{ return fx->dispatcher(fx, op, idx, v, p, o); }

float getN(int i) { return fx->getParameter(fx, i); }
void setN(int i, float v) { fx->setParameter(fx, i, v); }
std::string name(int i) { char b[256] = {0}; disp(effGetParamName, i, 0, b); return b; }
std::string text(int i) { char b[256] = {0}; disp(effGetParamDisplay, i, 0, b); return b; }

void setInt(int i, int v, int minv, int maxv) { setN(i, (float)(v - minv) / (float)(maxv - minv)); }
int  getInt(int i, int minv, int maxv) { return minv + (int)std::lround(getN(i) * (maxv - minv)); }

void block(int frames = 128, std::vector<VstMidiEvent> midi = {})
{
    struct { VstInt32 n; VstIntPtr r; VstEvent* ev[16]; } evs{};
    evs.n = (VstInt32)midi.size();
    for (size_t k = 0; k < midi.size(); ++k) evs.ev[k] = reinterpret_cast<VstEvent*>(&midi[k]);
    disp(effProcessEvents, 0, 0, &evs);
    float* outs[2] = {bufL, bufR};
    fx->processReplacing(fx, nullptr, outs, frames);
    lastPeak = 0.0f;
    for (int k = 0; k < frames; ++k)
    {
        if (!std::isfinite(bufL[k])) { lastPeak = 1e9f; break; }
        lastPeak = std::fmax(lastPeak, std::fabs(bufL[k]));
    }
    if (gPlaying) gPpq += frames / 44100.0 * gBpm / 60.0;
}

// Render n seconds; returns the loudest sample.
float render(double seconds)
{
    float peak = 0.0f;
    for (int k = 0; k < (int)(seconds * 44100.0 / 128.0); ++k) { block(); peak = std::fmax(peak, lastPeak); }
    return peak;
}

VstMidiEvent note(bool on, int n)
{
    VstMidiEvent e{}; e.type = kVstMidiType; e.byteSize = sizeof(e);
    e.midiData[0] = (char)(on ? 0x90 : 0x80); e.midiData[1] = (char)n; e.midiData[2] = on ? 100 : 0;
    return e;
}

void check(bool ok, const std::string& what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

void waitWorker() { for (int k = 0; k < 20; ++k) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); block(); } }

// ---- indices (keep in step with the plugin) -----------------------------------
enum
{
    kVolume = 0, kLane, kCursor, kField, kValue, kFollow, kLpb, kLength, kStress, kRedraw, kParamCount, kTouch,
    kMode, kInst, kOctave, kStep, kBlock, kBaseNote, kTrigger, kQuantise, kTicks,
    kFile, kLoad, kRescan, kStart, kEnd, kLoop, kLoopStart, kGain, kReverse, kNormalise, kSliceMode, kSlices, kSense,
    kSampleName, kPlaying, kRowBase
};
const int kRows = 16, kCols = 6;
const int kCursorSlot = kRowBase + kRows * kCols, kPlaySlot = kCursorSlot + 1, kActBase = kPlaySlot + 1;
const int kWaveBase = kActBase + 4, kStartSlot = kWaveBase + 64, kEndSlot = kStartSlot + 1, kLoopSlot = kEndSlot + 1;
const int kSliceBase = kLoopSlot + 1;
const int kFrames[6] = {129, 123, 257, 67, 38, 258};

int cell(int row, int col) { return getInt(kRowBase + row * kCols + col, 0, kFrames[col] - 1); }
int sliceMarks() { int n = 0; for (int b = 0; b < 64; ++b) n += getInt(kSliceBase + b, 0, 1); return n; }
int waveSum() { int n = 0; for (int b = 0; b < 64; ++b) n += getInt(kWaveBase + b, 0, 100); return n; }

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: test_grid <plugin> [--scenario name] [--dump file.json]\n"); return 2; }
    const char* dumpPath = nullptr;
    std::string scenario = "edit";
    for (int i = 2; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) dumpPath = argv[++i];
        else if (!std::strcmp(argv[i], "--scenario") && i + 1 < argc) scenario = argv[++i];
    }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    auto entry = (AEffect* (*)(audioMasterCallback))dlsym(lib, "VSTPluginMain");
    fx = entry(host);
    disp(effSetSampleRate, 0, 0, nullptr, 44100.0f);
    disp(effSetBlockSize, 0, 512);
    disp(effOpen);
    disp(effMainsChanged, 0, 1);
    disp(effStartProcess);

    std::printf("plugin reports %d parameters (scenario: %s)\n", fx->numParams, scenario.c_str());
    block();

    if (scenario == "edit")
    {
        check(getInt(kCursorSlot, 0, 16) == 8, "cursor drawn on centre row");
        check(cell(8, 0) == 0 && cell(8, 1) == 2 + 36 && cell(8, 2) == 1, "block 01 line 00: C-3 01 on lane 1");
        check(cell(7, 1) == 122 && cell(0, 0) == 128, "rows above line 00 are blank");
        check(text(kValue) == "C-3", "Value shows the cell under the cursor (C-3)");

        setInt(kCursor, 4, 0, 127); setInt(kField, 3, 0, 4); block();
        check(cell(8, 0) == 4 && text(kValue) == "9", "cursor to line 04, Value shows command 9");
        setInt(kValue, 13, 0, 255); block();
        check(cell(8, 4) == 13, "command edited to C");
        setInt(kField, 4, 0, 4); block();
        check(text(kValue) == "3D", "command value shows 3D (slice offset)");
        setInt(kValue, 0x30, 0, 255); block();
        check(cell(8, 5) == 0x30, "command value edited to 30");

        setInt(kTouch, 10 * 5 + 1, 0, 80); block();
        check(getInt(kCursor, 0, 127) == 6 && getInt(kField, 0, 4) == 1, "touching a cell moves cursor and field");
        check(getInt(kTouch, 0, 80) == 80, "touch parameter returns to idle");

        // Pad entry (Edit mode is the default).
        setInt(kLane, 1, 0, 3); setInt(kCursor, 2, 0, 127); setInt(kInst, 2, 1, 16); setInt(kOctave, 2, 0, 8); block();
        block(128, {note(true, 36 + 7)});
        block();
        check(lastPeak > 0.01f, "pad entry auditions the note");
        block(128, {note(false, 36 + 7)});
        check(getInt(kCursor, 0, 127) == 3, "pad entry moved the cursor down one line");
        setInt(kCursor, 2, 0, 127); block();
        check(cell(8, 1) == 2 + 24 + 7 && cell(8, 2) == 2, "pad entry wrote G-2 with instrument 02");
        setInt(kLane, 0, 0, 3);

        // Blocks: switching shows another block and its length.
        setInt(kBlock, 2, 1, 32); setInt(kCursor, 0, 0, 127); block(); block();
        check(getInt(kLength, 1, 128) == 32, "block 02 has length 32");
        check(cell(8, 1) == 2 + 36 && cell(8, 4) == 10, "block 02 line 00 shows its own data");
        setInt(kLength, 48, 1, 128); block();
        setInt(kBlock, 1, 1, 32); block(); block();
        check(getInt(kLength, 1, 128) == 64, "block 01 keeps length 64");
        setInt(kBlock, 2, 1, 32); block(); block();
        check(getInt(kLength, 1, 128) == 48, "block 02 remembers its new length 48");
        setInt(kBlock, 1, 1, 32); block(); block();

        // Saved state restores edits after a reload.
        void* chunk = nullptr;
        VstIntPtr n = disp(effGetChunk, 0, 0, &chunk);
        std::vector<char> saved((char*)chunk, (char*)chunk + n);
        setInt(kCursor, 4, 0, 127); setInt(kField, 3, 0, 4); block();
        setInt(kValue, 0, 0, 255); block();
        check(cell(8, 4) == 0, "command cleared before reload");
        disp(effSetChunk, 0, (VstIntPtr)saved.size(), saved.data());
        block(); setInt(kCursor, 4, 0, 127); block(); block();
        check(cell(8, 4) == 13, "block data restored from the saved chunk");
        setInt(kCursor, 0, 0, 127); setInt(kField, 0, 0, 4); block();
    }
    else if (scenario == "play")
    {
        setInt(kMode, 0, 0, 1); block();
        // C1 (36) = block 01, gate.
        block(128, {note(true, 36)});
        check(render(0.05) > 0.01f, "C1 plays block 01 (sound)");
        check(getInt(kPlaying, 0, 32) == 1, "Playing shows 01");
        render(60.0 / 170.0 / 4.0 * 5.0);
        check(getInt(kPlaySlot, 0, 16) == 8 && cell(8, 0) >= 5, "grid follows the playing line");
        block(128, {note(false, 36)});
        check(getInt(kPlaying, 0, 32) == 0, "gate: note off stops the block");
        // C#1 = block 02 (32 lines).
        block(128, {note(true, 37)});
        block();
        check(getInt(kPlaying, 0, 32) == 2, "C#1 plays block 02");
        check(cell(8, 4) == 10 || cell(8, 0) <= 1, "grid shows block 02");
        block(128, {note(false, 37)});
        // One-shot: D1 = block 03 (16 lines) stops by itself.
        setInt(kTrigger, 1, 0, 1); block();
        block(128, {note(true, 38)});
        block(128, {note(false, 38)});
        check(getInt(kPlaying, 0, 32) == 3, "one-shot keeps playing after note off");
        render(60.0 / 170.0 / 4.0 * 17.0);
        check(getInt(kPlaying, 0, 32) == 0, "one-shot stops at the end of the block");
        setInt(kTrigger, 0, 0, 1);
        // Notes outside the 32 blocks are ignored.
        block(128, {note(true, 36 + 40)});
        check(getInt(kPlaying, 0, 32) == 0, "note above block 32 ignored");
        block(128, {note(false, 36 + 40)});
        // Leave block 01 playing for the screen preview.
        block(128, {note(true, 36)});
        render(60.0 / 170.0 / 4.0 * 18.5);
    }
    else if (scenario == "quantise")
    {
        setInt(kMode, 0, 0, 1); setInt(kQuantise, 1, 0, 2); block();
        gPlaying = true; gPpq = 0.3; block();
        block(128, {note(true, 36)});
        check(getInt(kPlaying, 0, 32) == 0, "quantise Beat: waits for the beat");
        render(60.0 / 170.0 * 0.75);
        check(getInt(kPlaying, 0, 32) == 1, "quantise Beat: starts on the next beat");
        block(128, {note(false, 36)});
        gPlaying = false;
    }
    else if (scenario == "effects")
    {
        // Block 01 has 9xx offsets, R retrigger, 3xx porta and E chords; 02 has B reverse and 2xx slide.
        setInt(kMode, 0, 0, 1); block();
        block(128, {note(true, 36)});
        float p1 = render(60.0 / 170.0 * 16.0);
        block(128, {note(false, 36)});
        check(p1 > 0.05f && p1 < 1.5f, "block 01 with commands renders (peak " + std::to_string(p1) + ")");
        block(128, {note(true, 37)});
        float p2 = render(60.0 / 170.0 * 8.0);
        block(128, {note(false, 37)});
        check(p2 > 0.05f && p2 < 1.5f, "block 02 (reverse, slide, chords) renders (peak " + std::to_string(p2) + ")");
        // Every command on a lane, one line each, must not blow up.
        setInt(kMode, 1, 0, 1); setInt(kBlock, 5, 1, 32); setInt(kLane, 0, 0, 3); setInt(kStep, 1, 0, 16); block(); block();
        const char cmds[] = "0123459ABCDEFR";
        for (int k = 0; cmds[k]; ++k)
        {
            setInt(kCursor, k, 0, 127); setInt(kField, 0, 0, 4); block();
            setInt(kValue, 2 + 36, 0, 255); block();
            setInt(kField, 1, 0, 4); block(); setInt(kValue, 2, 0, 255); block();
            const int c = cmds[k] <= '9' ? 1 + (cmds[k] - '0') : 11 + (cmds[k] - 'A');
            setInt(kField, 3, 0, 4); block(); setInt(kValue, c, 0, 255); block();
            setInt(kField, 4, 0, 4); block(); setInt(kValue, 0x23, 0, 255); block();
        }
        setInt(kMode, 0, 0, 1); block();
        block(128, {note(true, 36 + 4)});
        float p3 = render(60.0 / 170.0 * 4.0);
        block(128, {note(false, 36 + 4)});
        check(p3 > 0.01f && p3 < 1.5f, "all commands 0-4 9 A-F R play cleanly (peak " + std::to_string(p3) + ")");
    }
    else if (scenario == "samples")
    {
        // Rescan finds the user folder next to VST2 Plugins.
        setInt(kRescan, 1, 0, 1); waitWorker();
        int userIdx = -1, total = 0;
        bool demo = false;
        std::string prev;
        for (int i = 0; i < 256; ++i)
        {
            setInt(kFile, i, 0, 255);
            const std::string t = text(kFile);
            if (i > 0 && t == prev) break;   // past the end: the last name repeats
            prev = t;
            ++total;
            if (t.find("user_loop") != std::string::npos) userIdx = i;
            if (t.find("demo/inst04_stab") != std::string::npos) demo = true;
        }
        std::printf("INFO  %d files listed\n", total);
        check(userIdx >= 0, "file list includes Twisted Block Samples/user_loop.wav");
        check(demo, "file list includes the demo samples");

        // Load it into instrument 05.
        setInt(kInst, 5, 1, 16); block(); block();
        check(text(kSampleName) == "(empty)", "instrument 05 starts empty");
        setInt(kFile, userIdx, 0, 255); setInt(kLoad, 1, 0, 1); block(); waitWorker();
        check(text(kSampleName) == "user_loop", "instrument 05 loaded: " + text(kSampleName));
        check(waveSum() > 200, "waveform drawn for instrument 05");
        check(getInt(kLoad, 0, 1) == 0, "Load button switches itself off");

        // Start / end / loop markers.
        setN(kStart, 0.25f); setN(kEnd, 0.75f); setInt(kLoop, 1, 0, 2); setN(kLoopStart, 0.5f); block(); block();
        check(getInt(kStartSlot, 0, 64) == 16 && getInt(kEndSlot, 0, 64) == 47 && getInt(kLoopSlot, 0, 64) == 32,
              "start/end/loop markers at 25/75/50 %");
        setN(kStart, 0.0f); setN(kEnd, 1.0f); setInt(kLoop, 0, 0, 2); block(); block();
        check(getInt(kLoopSlot, 0, 64) == 64, "loop marker hidden when loop is off");

        // Slicing.
        setInt(kSliceMode, 1, 0, 2); setInt(kSlices, 8, 2, 64); block(); block();
        check(sliceMarks() == 8, "grid slicing: 8 markers");
        setInt(kInst, 1, 1, 16); block(); block();
        setInt(kSliceMode, 2, 0, 2); setN(kSense, 0.6f); block(); block();
        const int tr = sliceMarks();
        check(tr >= 6 && tr <= 40, "transient slicing on the break: " + std::to_string(tr) + " markers");
        // Slice 3 plays on D#3 (C-3 + 3).
        setInt(kMode, 1, 0, 1); setInt(kOctave, 3, 0, 8); setInt(kLane, 0, 0, 3); block();
        block(128, {note(true, 36 + 3)}); block();
        check(lastPeak > 0.001f, "a slice plays from its pad");
        block(128, {note(false, 36 + 3)});
        setInt(kSliceMode, 0, 0, 2); block();

        // Normalise raises the gain of a quiet sample.
        setInt(kInst, 5, 1, 16); block(); block();
        const float g0 = getN(kGain);
        setInt(kNormalise, 1, 0, 1); block(); block();
        check(getN(kGain) > g0 && getInt(kNormalise, 0, 1) == 0, "Normalise raises the gain: " + text(kGain));

        // Settings follow the instrument.
        setInt(kInst, 1, 1, 16); block(); block();
        check(getInt(kSliceMode, 0, 2) == 0 && text(kSampleName) == "inst01_break", "instrument 01 shows its own settings");
        setInt(kInst, 5, 1, 16); block(); block();
        check(getInt(kSliceMode, 0, 2) == 1 && getInt(kSlices, 2, 64) == 8, "instrument 05 keeps its slice settings");

        // Project reload brings instrument 05 back from disk.
        void* chunk = nullptr;
        VstIntPtr n = disp(effGetChunk, 0, 0, &chunk);
        std::vector<char> saved((char*)chunk, (char*)chunk + n);
        setInt(kFile, 0, 0, 255);
        for (int i = 0; i < 256; ++i) { setInt(kFile, i, 0, 255); if (text(kFile).find("demo/inst03_hat") != std::string::npos) break; }
        setInt(kLoad, 1, 0, 1); block(); waitWorker();
        check(text(kSampleName) == "inst03_hat", "instrument 05 replaced with the hat");
        disp(effSetChunk, 0, (VstIntPtr)saved.size(), saved.data());
        waitWorker();
        check(text(kSampleName) == "user_loop" && getInt(kSliceMode, 0, 2) == 1, "project reload restores user_loop with its slices");
    }
    else if (scenario == "stress")
    {
        setInt(kStress, 1, 0, 1);
        int changes = 0, last = -1;
        for (int b = 0; b < 344; ++b)
        {
            block(128);
            int v = cell(8, 0);
            if (v != last) { ++changes; last = v; }
        }
        std::printf("INFO  stress: centre line changed %d times in 1 s (expect about %d)\n", changes, (int)(170.0 / 60.0 * 4));
        check(changes >= 10 && changes <= 13, "stress mode free-runs at tempo");
    }

    if (dumpPath)
    {
        FILE* f = std::fopen(dumpPath, "w");
        std::fprintf(f, "{\"params\": [");
        for (int i = 0; i < fx->numParams; ++i)
        {
            std::string t = text(i);
            for (auto& ch : t) if (ch == '"' || ch == '\\') ch = '\'';
            std::fprintf(f, "%s{\"index\": %d, \"name\": \"%s\", \"norm\": %.6f, \"text\": \"%s\"}", i ? ", " : "", i,
                         name(i).c_str(), (double)getN(i), t.c_str());
        }
        std::fprintf(f, "]}\n");
        std::fclose(f);
        std::printf("dumped %s\n", dumpPath);
    }

    disp(effStopProcess);
    disp(effMainsChanged, 0, 0);
    disp(effClose);
    std::printf("%s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
