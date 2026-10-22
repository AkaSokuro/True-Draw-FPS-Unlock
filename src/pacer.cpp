// Frame pacing.
//
// The game's scheduler runs the simulation in fixed 1/60 s steps. Before drawing a frame it waits until at least
// `minSteps` steps are due (1 in 60 FPS mode, 2 in 30 FPS mode). We patch minSteps to 0, so a frame can be drawn
// even when no step is due. The simulation itself still runs at exactly 60 Hz.
// Because the game no longer limits the frame rate, we cap it ourselves at the in-game Max frame rate choice.
#include "tdfps.h"

#pragma comment(lib, "winmm.lib")

static u8* const PATCH_MINSTEPS = (u8*)0x14038BEFD; // movsx edx, byte [rbx+A3275A]
static const u8 MINSTEPS_ORIG[] = {0x0F, 0xBE, 0x93, 0x5A, 0x27, 0xA3, 0x00};
static const u8 MINSTEPS_NEW[]  = {0x31, 0xD2, 0x90, 0x90, 0x90, 0x90, 0x90}; // xor edx,edx
static u8* const CALL_SCHED  = (u8*)0x14038BF0C;    // call 1405F1D30 (fixed-step scheduler)
static const uintptr_t FN_SCHED = 0x1405F1D30;
typedef int(__fastcall* SchedFn)(void* sched, int minSteps, int maxSteps, int* out);

double qpcInv;
int targetFps = 60;
double stepCount;
double phase;
static bool phaseInit;
static double nextFrame;

// The game's code is encrypted on disk (EA .ooa). The patch site only shows the expected bytes once the
// code has been unpacked in memory.
bool gameCodeReady() { return memcmp(PATCH_MINSTEPS, MINSTEPS_ORIG, sizeof MINSTEPS_ORIG) == 0; }

static int __fastcall schedHook(void* sched, int minSteps, int maxSteps, int* out) {
    updateFpsChoice((u8*)sched - 0xA32718);
    if (targetFps > 0) { // Frame limiter: Sleep() while far from the deadline, busy-wait the last ~1.5 ms for accuracy.
        double t = now();
        if (nextFrame - t > 0.25) nextFrame = t;  // deadline is unreasonably far away: reset it to now
        while ((t = now()) < nextFrame) {
            if (nextFrame - t > 0.0015) Sleep(1);
        }
        nextFrame = (nextFrame + 1.0 / targetFps < t) ? t : nextFrame + 1.0 / targetFps;
    }
    int steps = ((SchedFn)FN_SCHED)(sched, minSteps, maxSteps, out);
    double t = now();
    stepCount += steps;
    if (gatePage) gatePage[0] = steps == 0; // tells the update gates whether this frame runs any steps (gates.cpp)

    // Track `phase`: the time at which step 0 would have finished. Step S then finishes at phase + S * STEP,
    // which lets us place each drawn frame between two steps.
    // All steps up to stepCount have finished by now, so phase can be at most `base`.
    double base = t - stepCount * STEP;
    if (!phaseInit) { phase = base; phaseInit = true; }
    if (phase > base) phase = base;                   // a step arrived earlier than expected
    if (phase < base - STEP) phase = base - STEP;     // the next step hasn't happened yet, so it must be due after now
    return steps;
}

bool hookScheduler() {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); qpcInv = 1.0 / f.QuadPart;
    timeBeginPeriod(1);
    return redirectCall(CALL_SCHED, FN_SCHED, (void*)schedHook);
}

void unlockSteps() { write(PATCH_MINSTEPS, MINSTEPS_NEW, sizeof MINSTEPS_NEW); }
