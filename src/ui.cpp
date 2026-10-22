// ---- UI (SmoothUI setting): menu and HUD animations updated every frame ----
// The game updates its UI (1403054E0, called at 1402B0771) once per simulation step. On frames without a step,
// we run only the UI's animation code (uiAnimate), not the whole update.
//
// Each UI update must see how much real time has passed since the previous one. We do that by temporarily moving
// the UI timer's start time ([timer+23E00], where timer = [[14129C4F8]+8]). The game's own clock is never changed:
// changing it made screen transitions slower.
#include "tdfps.h"

static u8* const CALL_UI = (u8*)0x1402B0771;
static const uintptr_t FN_UI = 0x1403054E0;
static const int UI_TICKS = 0x30;
typedef void(__fastcall* UiFn)(void* ui, void* flow);
static UiFn uiFn = (UiFn)FN_UI;
void* uiObj;
static void* uiFlow;
static uint64_t uiStep;
static u8* uiGame;           // the game object, set by updateUI. Its clock is at +28 (+30 microsecond ticks, +38 dt, +3C scale)
static u8** uiTimerOwner = (u8**)0x14129C4F8;
static double uiLast;        // real time of the previous animated UI update
static bool uiMoving;        // UI time was running (not paused, flow state 0xB) in the game's last UI update

u8* uiTimer() { u8* o = *uiTimerOwner; return o ? *(u8**)(o + 8) : nullptr; }

// On extra frames we run only the animation code, never the whole UI update (that ended cutscenes early and crashed
// at the start and end of races). Two kinds of objects are animated:
//   - Layers: the 70 slots at ui+18, calling vtable entry 4 for each layer that is shown and settled
//     (+7F4 = 2 and +509 set). 3D layers, like the name tags above cars ([layer+500] >= 2), flickered when updated
//     every frame, so they stay once per step; uiCatchUp3D gives them the time they missed.
//   - Elements: the 1024 slots at ui+1B50, calling vtable entry 5. These are highlights, slides and fades, which is
//     most of what moves on screen.
typedef void(__fastcall* LayerFn)(u8* layer, u8* pool, void* owner, void* flow);
typedef void(__fastcall* ElemFn)(u8* elem, void* flow);
static void animateLayers(void* ui, void* flow, bool only3D) {
    u8** slot = (u8**)((u8*)ui + 0x18);
    for (int i = 0; i < 0x46; i++) {
        u8* l = slot[i];
        if (!l || !l[0x509] || *(int*)(l + 0x7F4) != 2 || (*(int*)(l + 0x500) >= 2) != only3D) continue;
        (*(LayerFn*)(*(u8**)l + 0x20))(l, (u8*)ui + 0x6C8, *(void**)ui, flow);
    }
}

static void uiCatchUp3D(void* ui, void* flow) { animateLayers(ui, flow, true); }

static void uiAnimate(void* ui, void* flow) {
    animateLayers(ui, flow, false);
    u8** elems = (u8**)((u8*)ui + 0x1B50);
    for (int i = 0; i < 0x400; i++)
        if (u8* e = elems[i]) (*(ElemFn*)(*(u8**)e + 0x28))(e, flow);
}

// Runs fn so that the UI timer reports the real time passed since the previous animated UI update.
//   - extra: this is one of our extra calls, not the game's own call.
//   - fixed: when 0 or more, report this much time instead, and leave uiLast alone (used for catch-up calls).
// If the timer isn't in use or is paused, extra calls are skipped (returns false) and the game's own call runs
// unchanged.
static bool runUI(void* ui, void* flow, UiFn fn, bool extra, double fixed = -1) {
    u8* timer = *(int*)flow == 0xB && uiGame ? uiTimer() : nullptr;
    int64_t* start = timer ? (int64_t*)(timer + 0x23E00) : nullptr;
    int64_t base = !timer ? 0 : timer[0x23DF0] ? *(int64_t*)(timer + 0x23DF8) : *(int64_t*)(uiGame + UI_TICKS);
    double t = now();
    if (!extra) uiMoving = timer && base - *start > 0;
    if (!timer || base - *start <= 0) {
        if (fixed < 0) uiLast = t;
        if (!extra) fn(ui, flow);
        return !extra;
    }
    double e = fixed >= 0 ? fixed : uiLast > 0 ? t - uiLast : (base - *start) * 1e-6;
    if (fixed < 0) uiLast = t;
    e = e < 0 ? 0 : e > 0.1 ? 0.1 : e; // never report more than 0.1 s, so a hitch doesn't make animations jump
    // Move the timer's start so it reports exactly e seconds, run fn, then put the real start back.
    int64_t save = *start;
    *start = base - llround(e * 1e6);
    fn(ui, flow);
    *start = save;
    return true;
}

static double uiLastStep;    // uiLast right after the game's previous (once per step) UI update

// Replaces the game's once-per-step UI update.
static void __fastcall uiStepHook(void* ui, void* flow) {
    uiObj = ui; uiFlow = flow; uiStep = (uint64_t)stepCount;
    // 3D layers skipped our extra per-frame calls since the last step. Give them that time first;
    // the game's update below then adds the rest.
    if (uiLastStep > 0 && uiLast > uiLastStep) runUI(ui, flow, uiCatchUp3D, true, uiLast - uiLastStep);
    runUI(ui, flow, uiFn, false);
    uiLastStep = uiLast;
}

// True while an online lobby's item lists are active (5 lists of 0x450 bytes at ui+7F470, active flag at +438).
// Extra per-frame calls are skipped then, because the lists' next sort crashed after them (at 1401B670E).
static bool uiListsActive(void* ui) {
    for (int i = 0; i < 5; i++)
        if (((u8*)ui + 0x7F470 + i * 0x450)[0x438]) return true;
    return false;
}

// Called every frame. On frames without a simulation step, runs the UI animations.
void updateUI(u8* game) {
    static double lastStep = -1;
    uiGame = game;
    bool stepped = stepCount != lastStep; // this frame ran simulation steps, so the game updated the UI itself
    lastStep = stepCount;
    // If the game hasn't updated the UI for a few steps, it is in some other state: leave the UI alone.
    if (stepped || !uiObj || (uint64_t)stepCount - uiStep > 2) return;
    // The timer's paused flag is only valid around the game's own update, so use what that update saw (uiMoving).
    if (*(int*)uiFlow != 0xB || !uiMoving) return;
    bool lists = uiListsActive(uiObj);
    static bool loggedLists;
    if (lists != loggedLists) { loggedLists = lists; logf("UI per-frame animation %s (lobby lists %s)", lists ? "paused" : "resumed", lists ? "active" : "inactive"); }
    if (lists) return; // uiLast isn't moved, so the game's next update gets all the time that passed
    runUI(uiObj, uiFlow, uiAnimate, true);
}

void installUI() {
    if (optUI) logf("UI %s", redirectCall(CALL_UI, FN_UI, (void*)uiStepHook) ? "patched" : "NOT patched");
}
