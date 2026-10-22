// ---- Input record / replay (InputReplay=1 to record, 2 to replay), offline events only ----
// Used to check that the frame rate doesn't change the physics: record a race once, replay it at another frame rate,
// and compare where the car went.
//
// The game reads the keyboard (GetAsyncKeyState for keys 8 to 255, called at 1400DB2A2) and controller 0
// (XInputGetState) once per simulation step.
//   - A run starts when the race's rolling start begins (mode 3, event state 7), which is the same step every time.
//   - Recording saves each step's input to TrueDrawFPS.input: 32 bytes of key bits, plus the XInputGetState result
//     and controller state. Replaying feeds that input back to the game instead of the real input.
//   - Both write the car's position and speed for every step to
//     TrueDrawFPS.trace-<fps>-g<gates>-<rec|play>-<time>.csv.
#include "tdfps.h"

typedef SHORT(WINAPI* KeyFn)(int vk);
typedef DWORD(WINAPI* PadFn)(DWORD user, void* state);
static u8* const CALL_KEYPOLL = (u8*)0x1400DB2A2;   // the game's 6-byte call to GetAsyncKeyState
static PadFn* const PAD_SLOT = (PadFn*)0x140DC1C20; // the game's import entry for xinput1_3!XInputGetState
static KeyFn realKey;
static PadFn realPad;
static const DWORD PAD_NONE = 1167;                 // ERROR_DEVICE_NOT_CONNECTED: the controller wasn't read this step
static u8 padNow[20];                               // controller 0 this step: XInputGetState result + XINPUT_STATE
static FILE *inputFile, *traceFile;
static bool runActive, lastRunning;
static uint32_t runPoll;                            // steps since the run started
static u8 keysNow[32];                              // key bits this step (recorded from the real keys, or read from the file)
static float lastPos[3];

static u8 **gameOwner = (u8**)0x14129C498, **modeOwner = (u8**)0x141279210, **eventOwner = (u8**)0x141279220;
static u8* gameRoot() { u8* o = *gameOwner; return o ? *(u8**)(o + 8) : nullptr; }

// Event states during a race: 7 while loaded and in the rolling start (mode 3). At GO it goes 6 -> 0 -> 8 -> 9,
// and stays 9 until the player leaves the event.
static bool raceStartPhase() {
    u8* m = *modeOwner; u8* e = *eventOwner;
    return m && e && *(int*)(m + 0x34DD8) == 3 && *(int*)(e + 0x4D8) == 7;
}
static bool reachedRace; // this run has reached state 9 (racing)
static bool inEvent() {
    u8* e = *eventOwner;
    int s = e ? *(int*)(e + 0x4D8) : -1;
    if (s == 9) reachedRace = true;
    return reachedRace ? s == 9 : s == 7 || s == 6 || s == 0 || s == 8 || s == 9;
}

static void endRun(const char* why) {
    if (!runActive) return;
    runActive = false;
    if (inputFile) { fclose(inputFile); inputFile = nullptr; }
    if (traceFile) { fclose(traceFile); traceFile = nullptr; }
    logf("input %s: run ended after %u steps (%s)", replayMode == 1 ? "record" : "replay", runPoll, why);
}

static void startRun() {
    char path[MAX_PATH], base[MAX_PATH];
    strcpy(base, logPath); *strrchr(base, '.') = 0;
    snprintf(path, sizeof path, "%s.input", base);
    inputFile = fopen(path, replayMode == 1 ? "wb" : "rb");
    if (!inputFile) { logf("input %s: cannot open %s", replayMode == 1 ? "record" : "replay", path); return; }
    uint32_t mask = gatesOn();
    SYSTEMTIME st; GetLocalTime(&st);
    snprintf(path, sizeof path, "%s.trace-%d-g%X-%s-%02d%02d%02d.csv", base, targetFps, mask, replayMode == 1 ? "rec" : "play",
             st.wHour, st.wMinute, st.wSecond);
    traceFile = fopen(path, "w");
    if (traceFile) fputs("step,x,y,z,speed\n", traceFile);
    runActive = true; runPoll = 0; reachedRace = false; lastPos[0] = lastPos[1] = lastPos[2] = 0;
    logf("input %s: run started (fps %d, gates %X), trace %s", replayMode == 1 ? "record" : "replay", targetFps, mask, path);
}

// Called when the game asks for the first key of its key poll, which happens once per simulation step.
static void pollStart() {
    if (runActive && replayMode == 1 && runPoll && inputFile) { // save the input collected during the previous step
        fwrite(keysNow, 1, sizeof keysNow, inputFile); fwrite(padNow, 1, sizeof padNow, inputFile);
    }
    bool starting = raceStartPhase();
    if (starting && !lastRunning) { endRun("restarted"); startRun(); }
    else if (runActive && !inEvent()) endRun("event left");
    lastRunning = starting;
    if (!runActive) return;
    u8* root = gameRoot();
    if (root && traceFile) { // write the player car's position, and its speed in m/s, to the trace
        const float* p = (const float*)(root - 0x10 + 0x4B0760 + 0xD0);
        float dx = p[0] - lastPos[0], dy = p[1] - lastPos[1], dz = p[2] - lastPos[2];
        fprintf(traceFile, "%u,%.4f,%.4f,%.4f,%.3f\n", runPoll, p[0], p[1], p[2], runPoll ? sqrtf(dx * dx + dy * dy + dz * dz) * 60 : 0.f);
        memcpy(lastPos, p, 12);
    }
    // Recording: clear this step's input so the hooks can fill it in. Replaying: load this step's input from the file.
    if (replayMode == 1) { memset(keysNow, 0, sizeof keysNow); memset(padNow, 0, sizeof padNow); *(DWORD*)padNow = PAD_NONE; }
    else if (!inputFile || fread(keysNow, 1, sizeof keysNow, inputFile) != sizeof keysNow || fread(padNow, 1, sizeof padNow, inputFile) != sizeof padNow) {
        endRun("input file ended");
        memset(keysNow, 0, sizeof keysNow);
    }
    runPoll++;
}

// Replaces GetAsyncKeyState: records the real key state, or answers with the recorded one.
extern "C" SHORT WINAPI keyHook(int vk) {
    if (vk == 8) pollStart();
    SHORT real = realKey(vk);
    if (!runActive || (unsigned)vk > 255) return real;
    if (replayMode == 1) { if (real & 0x8000) keysNow[vk >> 3] |= 1 << (vk & 7); return real; }
    return (keysNow[vk >> 3] >> (vk & 7)) & 1 ? (SHORT)0x8000 : 0;
}

// Replaces XInputGetState for controller 0: records the real state, or answers with the recorded one.
extern "C" DWORD WINAPI padHook(DWORD user, void* state) {
    if (!runActive || user != 0) return realPad(user, state);
    if (replayMode == 1) {
        DWORD r = realPad(user, state);
        *(DWORD*)padNow = r;
        if (r == 0) memcpy(padNow + 4, state, 16);
        return r;
    }
    DWORD r = *(DWORD*)padNow;
    if (r == 0) memcpy(state, padNow + 4, 16);
    return r;
}

void installReplay() {
    if (replayMode != 1 && replayMode != 2) return;
    static const u8 KEYPOLL_ORIG[] = {0xFF, 0x15, 0x18, 0x67, 0xCE, 0x00};
    bool ok = !memcmp(CALL_KEYPOLL, KEYPOLL_ORIG, sizeof KEYPOLL_ORIG);
    if (ok) { // replace the game's indirect call with a direct call to our trampoline, padded with a nop
        realKey = *(KeyFn*)(CALL_KEYPOLL + 6 + *(int32_t*)(CALL_KEYPOLL + 2));
        u8* t = trampAlloc(12);
        t[0] = 0x48; t[1] = 0xB8; *(void**)(t + 2) = (void*)keyHook; t[10] = 0xFF; t[11] = 0xE0; // mov rax,hook; jmp rax
        u8 call[6] = {0xE8, 0, 0, 0, 0, 0x90};
        *(int32_t*)(call + 1) = (int32_t)(t - (CALL_KEYPOLL + 5));
        write(CALL_KEYPOLL, call, 6);
    }
    logf("input %s %s", replayMode == 1 ? "record" : "replay", ok ? "patched" : "NOT patched (bytes differ)");
    if (ok && (realPad = *PAD_SLOT)) {
        PadFn hook = padHook;
        write(PAD_SLOT, &hook, sizeof hook);
        logf("controller %s patched", replayMode == 1 ? "record" : "replay");
    } else logf("controller NOT patched (no XInputGetState import)");
}
