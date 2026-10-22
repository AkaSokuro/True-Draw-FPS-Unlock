// True Draw FPS Unlock: declarations shared between the source files.
// For an overview of how the mod works, start with dllmain.cpp.
#pragma once
#include <windows.h>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <malloc.h>
#include <initializer_list>

#define TDFPS_VERSION "v0.79"
typedef uint8_t u8;

// ---- log.cpp ----
extern char logPath[MAX_PATH];
extern bool logOn;
void logf(const char* fmt, ...);

// ---- config.cpp: TrueDrawFPS.ini ----
extern bool optCamera, optObjects, optWheels, optUI, optParticles, optProbe;
extern int frameLatency, replayMode;
extern uint32_t gateMask;
extern float lookahead;
void loadConfig(const char* ini);

// ---- patch.cpp ----
void write(void* at, const void* data, size_t n);
u8* allocNear(uintptr_t target, size_t size = 0x1000);
u8* trampAlloc(size_t n);                         // n bytes close enough to the game code for a 32-bit relative jump
u8* jumpTo(const void* target);                   // small stub that jumps to any 64-bit address
void patchJump(u8* site, size_t len, const void* to); // overwrite `len` bytes at site with a jump to `to`, padded with nops
u8* relocate(u8* fn, size_t n);                   // copy of fn's first n bytes + jump back: calling it runs the original fn
bool redirectCall(u8* site, uintptr_t expectTarget, void* hook);
void redirectRead(u8*& p, int idx, u8* at, int len, const u8* orig, uint32_t newDisp);

// ---- pacer.cpp: fixed-step scheduler, frame cap ----
static const double STEP = 166666e-7;             // length of one simulation step in seconds (the game uses 166666 x 100 ns)
extern double qpcInv;
inline double now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart * qpcInv; } // seconds
extern double stepCount;                          // simulation steps run since start
extern double phase;                              // time step 0 finished; step S finishes at phase + S * STEP
extern int targetFps;                             // the Max frame rate picked in the game's settings

// ---- fpsoption.cpp ----
void updateFpsChoice(u8* game);

// ---- gates.cpp ----
extern u8* gatePage;
uint32_t gatesOn();

// ---- crashguard.cpp ----
extern thread_local int quietFaults;              // > 0 while this thread does reads that may fault on purpose; not reported

// ---- ui.cpp ----
extern void* uiObj;
u8* uiTimer();
void updateUI(u8* game);

// ---- blend/frame.cpp ----
extern float alpha;                               // where this frame sits between the previous step (0) and the latest (1)
extern uint32_t frameNo;                          // frames drawn since start
extern DWORD renderThread;                        // the thread that runs the game's Render function
extern bool inRender;                             // true while Render runs; blended values may only be written then
float blendT(uint32_t gap);

// ---- blend/objects.cpp: per-object history ----
// For every moving object we remember its world matrix from the last two simulation steps, looked up by the
// object's address. Matrices are 16-byte aligned because the game reads the ones we hand out with aligned SSE loads.
struct Hist { void* key; uint64_t step; uint32_t frame, gap; alignas(16) float prev[16]; alignas(16) float cur[16]; };
static const int HIST_SLOTS = 8192;            // ponytail: fixed-size hash tables, make them bigger if objects exceed ~4k

template <int N = HIST_SLOTS, class T> static T* histFor(T* table, void* key, uint64_t step) {
    static_assert((N & (N - 1)) == 0, "power of two");
    size_t h = (size_t)(((uintptr_t)key >> 4) * 0x9E3779B97F4A7C15ull >> 40);
    for (int i = 0; i < 64; i++) {
        T& e = table[(h + i) & (N - 1)];
        if (e.key == key) return &e;
        // Take an empty slot, or one whose object hasn't been seen for 2 seconds (120 steps).
        if (!e.key || e.step + 120 < step) { e.key = key; e.step = ~0ull; e.frame = ~0u; return &e; }
    }
    return nullptr;
}

void track(Hist* e, const float* sim, uint64_t step);
void blendHist(const Hist* e, const float* sim, float* out);

struct InstHist : Hist { alignas(16) float out[16]; bool instance; uint64_t perFrameStep; uint32_t updFrame; alignas(16) float truePrev[16]; };
// Extra fields for render instances:
//   out          - the blended matrix for frame `frame`
//   instance     - the object uses the game's render-instance memory layout
//   updFrame     - the first update seen this frame
//   truePrev     - that update's previous world matrix (see blendInstance)
//   perFrameStep - the last step where the matrix changed between steps. Some objects (e.g. online opponents)
//                  are moved by the game every frame, not every step. Those are drawn as they are, unblended.
inline bool movedPerFrame(const InstHist* e, uint64_t step) { return e->perFrameStep && e->perFrameStep + 2 >= step; }
extern InstHist insts[HIST_SLOTS];
// Redirected reads only take effect on the render thread while Render runs. Any other time they are counted in
// offRender (the last slot counts instance copies made outside Render).
extern const int NRENDER_READS, NREDIRECTS;      // the render reads come first, then the 4 traffic wheel reads
extern u8* redirectAt[];
extern volatile LONG64 offRender[];
void logOffRender();

// ---- blend/view.cpp ----
void applyView();
void restoreView();
void updateMapCamera();

// ---- blend/effects.cpp ----
bool carShiftNear(const float* p, uint32_t n, float d[3]);
extern long hlCalls, hlShifted, hlRigid, lightCalls, lightShifted; // counters, written to the log every 10 s

// ---- blend/wheels.cpp, blend/props.cpp, probe.cpp: counters, written to the log every 10 s ----
extern int wheelsThisFrame, wheelsMax, propsThisFrame, propsMax;
extern bool probing;
void logProbes();

// ---- install functions (each one logs what it patched) ----
bool hookScheduler(); bool hookRender(); void unlockSteps(); bool gameCodeReady();
int installGates(uint32_t mask); void installReplay(); void installUI(); void installMapCamera();
void installObjects(); void installCharacters(); void installWheels(); void installProps(); void installEffects();
void installReadouts(); void installFpsOption(); void installFrameLatency(); void installProbes(); void installCrashGuard();
