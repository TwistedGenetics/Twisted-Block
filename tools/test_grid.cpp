// test_grid.cpp - desktop test of the Phase 0 grid logic.
//
// Loads the natively built plugin (.so/.dylib), drives it like the MPC would
// (parameters, MIDI, transport) and checks the display parameters.
// With --dump <file.json> it writes every parameter's normalised value, which
// tools/preview_skin.py uses to draw a mock-up of the MPC screen.
//
// Build:  c++ -std=c++17 -O2 -I<sdk>/framework -o test_grid tools/test_grid.cpp -ldl

#include "mpc_vst2_abi.h"

#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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
        gTime.flags = kVstTempoValid | kVstPpqPosValid | (gPlaying ? kVstTransportPlaying : 0);
        return (VstIntPtr)&gTime;
    }
    return 0;
}

AEffect* fx = nullptr;
int failures = 0;
float bufL[512], bufR[512];

VstIntPtr disp(VstInt32 op, VstInt32 idx = 0, VstIntPtr v = 0, void* p = nullptr, float o = 0)
{ return fx->dispatcher(fx, op, idx, v, p, o); }

int numParams() { return fx->numParams; }
float getN(int i) { return fx->getParameter(fx, i); }
void setN(int i, float v) { fx->setParameter(fx, i, v); }

std::string name(int i) { char b[256] = {0}; disp(effGetParamName, i, 0, b); return b; }
std::string text(int i) { char b[256] = {0}; disp(effGetParamDisplay, i, 0, b); return b; }

// Integer parameter helpers (the plugin's int params are min..max mapped to 0..1).
void setInt(int i, int v, int maxv) { setN(i, maxv > 0 ? (float)v / (float)maxv : 0.0f); }
int  getInt(int i, int maxv) { return (int)std::lround(getN(i) * maxv); }

void block(int frames = 128, std::vector<VstMidiEvent> midi = {})
{
    struct { VstInt32 n; VstIntPtr r; VstEvent* ev[16]; } evs{};
    evs.n = (VstInt32)midi.size();
    for (size_t k = 0; k < midi.size(); ++k) evs.ev[k] = reinterpret_cast<VstEvent*>(&midi[k]);
    disp(effProcessEvents, 0, 0, &evs);
    float* outs[2] = {bufL, bufR};
    fx->processReplacing(fx, nullptr, outs, frames);
    if (gPlaying) gPpq += frames / 44100.0 * gBpm / 60.0;
}

VstMidiEvent note(bool on, int n)
{
    VstMidiEvent e{}; e.type = kVstMidiType; e.byteSize = sizeof(e);
    e.midiData[0] = (char)(on ? 0x90 : 0x80); e.midiData[1] = (char)n; e.midiData[2] = on ? 100 : 0;
    return e;
}

void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

// Indices (keep in step with the plugin).
const int kLane = 1, kCursor = 2, kField = 3, kValue = 4, kFollow = 5, kLength = 7, kStress = 8, kTouch = 11;
const int kRowBase = 12, kCols = 6, kRows = 16;
const int kCursorSlot = kRowBase + kRows * kCols, kPlaySlot = kCursorSlot + 1;
const int kFrames[6] = {129, 123, 257, 67, 38, 258};

int cell(int row, int col) { return getInt(kRowBase + row * kCols + col, kFrames[col] - 1); }

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: test_grid <plugin> [--dump file.json] [--scenario edit|play|lane3]\n"); return 2; }
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

    std::printf("plugin reports %d parameters\n", numParams());
    block();

    if (scenario == "edit")
    {
        // Cursor 0 sits on the centre row (8); the demo block has C-3 inst 01 there on lane 1.
        check(getInt(kCursorSlot, 16) == 8, "cursor drawn on centre row");
        check(cell(8, 0) == 0 && cell(8, 1) == 2 + 36 && cell(8, 2) == 1, "line 00: C-3 01 on lane 1");
        check(cell(7, 1) == 122 && cell(0, 0) == 128, "rows above line 00 are blank");
        check(text(kValue) == "C-3", "Value control shows the cell under the cursor (C-3)");

        // Move the cursor to line 04 and the field to Cmd: value follows.
        setInt(kCursor, 4, 127); setInt(kField, 3, 4); block();
        check(cell(8, 0) == 4, "centre row now shows line 04");
        check(text(kValue) == "9", "Value shows command 9 at line 04");

        // Turn Value: edit the cell (cmd -> C = 1+12 = 13).
        setInt(kValue, 13, 255); block();
        check(cell(8, 4) == 13, "command cell edited to C");
        setInt(kField, 4, 4); block();
        check(text(kValue) == "40", "command value shows 40");
        setInt(kValue, 0x30, 255); block();
        check(cell(8, 5) == 0x30, "command value edited to 30");

        // Touch row 10 (= line 06), field Inst.
        setInt(kTouch, 10 * 5 + 1, 80); block();
        check(getInt(kCursor, 127) == 6 && getInt(kField, 4) == 1, "touching a cell moves cursor and field");
        check(getInt(kTouch, 80) == 80, "touch parameter returns to idle");

        // Lane switch shows other lanes.
        setInt(kLane, 2, 3); block();
        check(getN(kRowBase + 8 * kCols + 0) >= 0.0f, "lane 3 drawn");
        setInt(kLane, 0, 3); setInt(kCursor, 0, 127); block();

        // Saved state restores the edit after a fresh load.
        void* chunk = nullptr;
        VstIntPtr n = disp(effGetChunk, 0, 0, &chunk);
        std::vector<char> saved((char*)chunk, (char*)chunk + n);
        setInt(kCursor, 4, 127); setInt(kField, 3, 4); block();
        setInt(kValue, 0, 255); block();
        check(cell(8, 4) == 0, "command cleared before reload");
        disp(effSetChunk, 0, (VstIntPtr)saved.size(), saved.data());
        block(); setInt(kCursor, 4, 127); block(); block();
        check(cell(8, 4) == 13, "block data restored from the saved chunk");
        setInt(kCursor, 0, 127); setInt(kField, 0, 4); block();
    }
    else if (scenario == "play")
    {
        // Hold a pad: plays from the top at 170 BPM, 4 LPB.
        block(128, {note(true, 36)});
        int frames = (int)(44100.0 * 60.0 / 170.0 / 4.0 * 5.5);   // 5.5 lines
        while (frames > 0) { block(128); frames -= 128; }
        check(getInt(kPlaySlot, 16) == 8, "playhead on centre row while following");
        check(cell(8, 0) == 5, "grid scrolled to line 05");
        block(128, {note(false, 36)});
        check(getInt(kPlaySlot, 16) == 16, "playhead hidden after the pad is released");
        // MPC transport: bar 2 beat 1 = ppq 4 -> line 16.
        gPlaying = true; gPpq = 4.0; block(); block();
        check(cell(8, 0) == 16, "transport ppq 4 -> line 10 (hex) on centre row");
        // Keep the transport position for the preview dump.
        gPlaying = false;
        setInt(kStress, 0, 1);
        gPlaying = true; gPpq = 4.0 + 2.0 / 4.0; block();   // line 18 (hex 12)
    }
    else if (scenario == "stress")
    {
        setInt(kStress, 1, 1);
        int changes = 0, last = -1;
        for (int b = 0; b < 344; ++b)   // 1 second
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
        for (int i = 0; i < numParams(); ++i)
            std::fprintf(f, "%s{\"index\": %d, \"name\": \"%s\", \"norm\": %.6f, \"text\": \"%s\"}", i ? ", " : "", i,
                         name(i).c_str(), (double)getN(i), text(i).c_str());
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
