// ---- Per-frame blend timing ----
// Works out `alpha` (where this frame sits between two simulation steps), wraps the game's Render function
// (the blended camera is applied before Render and removed after), and writes counters to the log.
#include "../tdfps.h"

static u8* const CALL_RENDER = (u8*)0x14038BEC0;    // the game's call to Render (14038CB30)
static const uintptr_t FN_RENDER = 0x14038CB30;
typedef void(__fastcall* RenderFn)(u8* game);

float alpha = 1;
uint32_t frameNo;
DWORD renderThread;
bool inRender;

// Returns how far to blend from an object's previous matrix (0) to its current one (1), when the two were recorded
// `gap` steps apart.
// A frame is drawn one step behind the latest step S, at time S - 1 + alpha (+ lookahead). So the result is
// (gap - 1 + alpha) / gap. This way, when frames are slow and cover several steps, objects still move at the right
// speed instead of jumping. A result above 1 (from lookahead) continues the object's last motion a little further.
float blendT(uint32_t gap) { return gap > 1 ? (gap - 1 + alpha + lookahead) / gap : alpha + lookahead; }

static void callRender(u8* game) {
    frameNo++;
    renderThread = GetCurrentThreadId();
    inRender = true;
    ((RenderFn)FN_RENDER)(game);
    inRender = false;
    if (wheelsThisFrame > wheelsMax) wheelsMax = wheelsThisFrame;
    wheelsThisFrame = 0;
    if (propsThisFrame > propsMax) propsMax = propsThisFrame;
    propsThisFrame = 0;
    static double nextLog;
    double t = now();
    if (t > nextLog) { // write the counters to the log every 10 s
        if (nextLog > 0) logf("frames=%u steps=%.0f wheels/frame(max)=%d moving props/frame(max)=%d", frameNo, stepCount, wheelsMax, propsMax);
        if (nextLog > 0 && hlCalls) logf("  headlight: calls %ld shifted %ld already following the drawn car %ld", hlCalls, hlShifted, hlRigid);
        hlCalls = hlShifted = hlRigid = 0;
        if (nextLog > 0 && lightCalls) logf("  car lights: %ld built, %ld moved with their car", lightCalls, lightShifted);
        lightCalls = lightShifted = 0;
        propsMax = 0;
        if (nextLog > 0 && probing) logProbes();
        logOffRender();
        nextLog = t + 10;
        wheelsMax = 0;
    }
}

// Replaces the game's call to Render.
static void __fastcall renderHook(u8* game) {
    // alpha: how far real time has moved past the latest step, as a fraction of a step (0 to 1).
    float t = (float)((now() - phase - stepCount * STEP) / STEP);
    alpha = t < 0 ? 0 : t > 1 ? 1 : t;
    if (optUI) { updateUI(game); updateMapCamera(); }
    applyView();
    callRender(game);
    restoreView();
}

bool hookRender() { return redirectCall(CALL_RENDER, FN_RENDER, (void*)renderHook); }
