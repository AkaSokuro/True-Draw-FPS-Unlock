// Self-tests for the blending code. The first check: blending a camera block captured from the game with itself
// must rebuild exactly the game's own matrices.
// Build and run with tests\run.bat. Uses camera blocks captured from the running game (tests\camblock.bin and
// tests\mirrorblock.bin).
// Every plugin source file is compiled into this one, so the tests can reach functions and variables that are
// private to a file.
#include "../src/dllmain.cpp"
#include "../src/config.cpp"
#include "../src/log.cpp"
#include "../src/patch.cpp"
#include "../src/pacer.cpp"
#include "../src/fpsoption.cpp"
#include "../src/gates.cpp"
#include "../src/framelatency.cpp"
#include "../src/crashguard.cpp"
#include "../src/replay.cpp"
#include "../src/probe.cpp"
#include "../src/ui.cpp"
#include "../src/blend/frame.cpp"
#include "../src/blend/objects.cpp"
#include "../src/blend/character.cpp"
#include "../src/blend/props.cpp"
#include "../src/blend/wheels.cpp"
#include "../src/blend/view.cpp"
#include "../src/blend/effects.cpp"
#include "../src/blend/readouts.cpp"
#include <cstdio>
// a particle renderer's stream decoding (1400EFD50 / 1400EE3B0)
static u8* particleStream(u8* desc, u8* job, int off, int stride) {
    uint32_t l = *(uint32_t*)(desc + off);
    return *(u8**)(job + (l & 1 ? 0x20 : 0x58)) + (l & 0x3FFF0) + (uintptr_t)*(uint32_t*)(job + 0x40) * stride;
}
static u8 *posAt, *ageAt;
static void writeNull() { __try { *(volatile int*)nullptr = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
static uint64_t __fastcall fakeRenderer(u8* desc, u8* job) { posAt = particleStream(desc, job, 4, 16); ageAt = particleStream(desc, job, 8, 4); return 7; }
int main() {
    Block b; int bad = 0;
    for (const char* file : {"tests/mirrorblock.bin", "tests/camblock.bin"}) { // rear-view mirror (view+200), main view
        FILE* f = fopen(file, "rb");
        if (!f || fread(&b, 1, sizeof b, f) != sizeof b) { printf("missing %s\n", file); return 2; }
        fclose(f);
        Block out = b;
        blendCamera(out.f, b, b, 1.0f);
        const struct { int off, len; const char* name; } parts[] = {{0x00, 0x30, "view 3x3"}, {0x30, 0x0C, "view pos"}, {0x80, 0x40, "viewproj"}, {0xC0, 0x30, "VP cols x,y,w"}};
        printf("%s\n", file);
        for (auto& p : parts) {
            float worst = 0;
            for (int i = p.off / 4; i < (p.off + p.len) / 4; i++) {
                float e = fabsf(out.f[i] - b.f[i]) / (1.0f + fabsf(b.f[i]));
                if (e > worst) worst = e;
            }
            printf("  %-12s max rel err %.2e\n", p.name, worst);
            bad += worst > 1e-3f;
        }
        bad += !camConsistent(b);
    } // b = main camera block from here on
    {   // not cameras in that convention: empty slot, basis row 0 not flipped
        Block z = {}, s = b;
        for (int c = 0; c < 3; c++) s.f[0x100 / 4 + c] = -s.f[0x100 / 4 + c];
        bool ok = camConsistent(b) && !camConsistent(z) && !camConsistent(s);
        printf("%-14s camera 1, empty / other convention rejected: %d\n", "cam check", ok);
        bad += !ok;
    }
    // multi-step frames (FPS drops): a constant-velocity object is drawn at time S - 1 + alpha for any step gap
    {
        static Hist h; h.key = &h; h.step = ~0ull;
        float sim[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, out[16], worst = 0, saveAlpha = alpha;
        for (int s : {10, 11, 13, 16, 17, 22})  // gaps 1, 2, 3, 1, 5 (2 m per step)
            for (float a : {0.f, 0.25f, 0.5f, 1.f}) {
                sim[12] = 2.f * s; track(&h, sim, s); alpha = a; blendHist(&h, sim, out);
                if (s > 10) worst = fmaxf(worst, fabsf(out[12] - 2.f * (s - 1 + a)));
            }
        Block p = b, c = b; // camera: 1 m per step along x, 3 steps apart
        p.f[0x130 / 4] -= 3.f; alpha = 0.5f;
        Block o = b; blendCamera(o.f, p, c, blendT(3));
        float camErr = fabsf(o.f[0x130 / 4] - (b.f[0x130 / 4] - 0.5f));
        alpha = saveAlpha;
        printf("%-14s object max err %.1e m, camera err %.1e m\n", "step gaps", worst, camErr);
        bad += worst > 1e-4f || camErr > 1e-3f;
    }
    // look back (camera turns in place): moved back along its last motion = where a plain blend would have put it
    {
        Block p = b, c = b, o = b, x = b; float vel[3] = {1.f, 0, 0};
        p.f[0x130 / 4] -= 1.f;
        float saveAlpha = alpha; alpha = 0.3f;
        blendCamera(o.f, p, c, blendT(1)); cutCamera(x.f, c, vel, alpha - 1);
        alpha = saveAlpha;
        float worst = 0;
        for (int i = 0; i < 0xF0 / 4; i++) worst = fmaxf(worst, fabsf(o.f[i] - x.f[i]) / (1.0f + fabsf(o.f[i])));
        printf("%-14s cut camera vs blend max rel err %.1e\n", "look back", worst);
        bad += worst > 1e-4f;
    }
    // lookahead: everything is drawn `lookahead` steps later (S - 1 + alpha + lookahead), extrapolated past cur
    {
        static Hist h; h.key = &h; h.step = ~0ull;
        float sim[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, out[16], worst = 0, saveAlpha = alpha;
        lookahead = 0.5f;
        for (int s : {10, 11, 13})                                  // gaps 1, 2 (2 m per step)
            for (float a : {0.f, 0.5f, 1.f}) {
                sim[12] = 2.f * s; track(&h, sim, s); alpha = a; blendHist(&h, sim, out);
                if (s > 10) worst = fmaxf(worst, fabsf(out[12] - 2.f * (s - 1 + a + 0.5f)));
            }
        Block p = b, c = b; p.f[0x130 / 4] -= 1.f; alpha = 0.75f;   // camera 1 m per step: drawn 0.25 m past cur
        Block o = b; blendCamera(o.f, p, c, blendT(1));
        float camErr = fabsf(o.f[0x130 / 4] - (b.f[0x130 / 4] + 0.25f));
        alignas(16) static u8 desc[0x40], job[0x80], pool[0x800];   // a motion blur spark: moved forward, not hidden
        *(uint32_t*)(desc + 4) = 0x20; *(uint32_t*)(desc + 8) = (1u << 18) | 0x2F0; *(uint32_t*)(desc + 0xC) = (2u << 18) | 0x5C0;
        *(u8**)(job + 0x58) = pool; *(uint32_t*)(job + 0x40) = 0; *(uint32_t*)(job + 0x44) = 1;
        ((float*)(pool + 0x2F0))[0] = 60; *(float*)(pool + 0x5C0) = 0.001f;
        static float px, py;
        motionBlurOrig = [](u8* d, u8* j) -> uint64_t { float* q = (float*)particleStream(d, j, 4, 16); px = q[0]; py = q[1]; return 0; };
        particleLogs = 100; motionBlurHook(desc, job);
        bool spark = fabsf(px - 60 * 0.25f / 60) < 1e-4f && py > -1;
        lookahead = 0; alpha = saveAlpha;
        printf("%-14s object err %.1e m, camera err %.1e m, spark ahead / not hidden %d\n", "lookahead", worst, camErr, spark);
        bad += worst > 1e-4f || camErr > 1e-3f || !spark;
    }
    // wheel slerp: spin about the axle (x) from 0 to 90 degrees, halfway must be exactly 45
    auto rotX = [](float deg, float* m) {
        float c = cosf(deg * 3.14159265f / 180), s = sinf(deg * 3.14159265f / 180);
        float r[16] = {1, 0, 0, 7, 0, c, s, 0, 0, -s, c, 0, 0.78f, 0.31f, 1.27f, 9};
        memcpy(m, r, sizeof r);
    };
    float qa[16], qb[16], want[16], got[16];
    rotX(0, qa); rotX(90, qb); rotX(45, want);
    slerpMatrix(qa, qb, 0.5f, got);
    float worst = 0;
    for (int i = 0; i < 16; i++) worst = fmaxf(worst, fabsf(got[i] - want[i]));
    printf("%-14s max abs err %.2e\n", "wheel slerp", worst);
    bad += worst > 1e-5f;
    slerpMatrix(qb, qb, 0.3f, got);
    worst = 0;
    for (int i = 0; i < 16; i++) worst = fmaxf(worst, fabsf(got[i] - qb[i]));
    printf("%-14s max abs err %.2e\n", "slerp identity", worst);
    bad += worst > 1e-5f;
    // physics prop getter: matrix at obj+50 moves A -> B in one step; render-thread call at alpha .5 gets halfway
    {
        alignas(16) static u8 obj[0x100];
        float* m = (float*)(obj + 0x50);
        rotX(0, m);
        inRender = true; renderThread = GetCurrentThreadId();
        stepCount = 200; frameNo = 1; matrixGetter<0x50>(obj);
        m[12] += 2; stepCount = 201; frameNo = 2; alpha = 0.5f;
        const float* got = matrixGetter<0x50>(obj);
        float midErr = fabsf(got[12] - (m[12] - 1));
        inRender = false;
        bool simOutside = matrixGetter<0x50>(obj) == m;     // gameplay / other threads get the sim matrix
        printf("%-14s mid err %.1e, sim outside render %d, aligned %d\n", "prop getter", midErr, simOutside, ((uintptr_t)got & 15) == 0);
        bad += midErr > 1e-5f || !simOutside || ((uintptr_t)got & 15);
    }
    // vehicle world helper: veh local = rotX(90) with offset; body moves 2 m along x in one step
    {
        alignas(16) static u8 veh[0x400], body[0x100];
        alignas(16) float out[16], want[16];
        *(int32_t*)(veh + 0x354) = 1; *(u8**)(veh + 0x18) = body;
        float* L = (float*)(veh + 0x290); float* B = (float*)(body + 0x60);
        rotX(90, L); L[12] = 0.5f; L[13] = 0.2f; L[14] = 1.5f;
        rotX(0, B);
        static int origCalls;
        vehWorldOrig = [](u8*, float* o) { origCalls++; return o; };
        inRender = true; renderThread = GetCurrentThreadId();
        stepCount = 300; vehWorld(veh, out);
        B[12] += 2; stepCount = 301; alpha = 0.5f;
        vehWorld(veh, out);
        memcpy(want, L, 64); want[12] += B[12] - 1; want[13] += B[13]; want[14] += B[14]; // identity rotation
        float worst = 0;
        for (int i : {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14}) worst = fmaxf(worst, fabsf(out[i] - want[i]));
        inRender = false; vehWorld(veh, out);
        printf("%-14s max err %.1e, original outside render %d\n", "vehicle world", worst, origCalls);
        bad += worst > 1e-5f || origCalls != 1;
    }
    // UI: every UI update gets step = real time since the previous one (clock dt, and in state 0xB the timer's
    // start = clock - elapsed); frames without a step get one extra update with the clock advanced by alpha
    {
        alignas(16) static u8 game[0x100];
        static u8 timerObj[0x24000], owner[16];
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); qpcInv = 1.0 / f.QuadPart; // real clock (else now() = 0)
        *(u8**)(owner + 8) = timerObj; uiTimerOwner = (u8**)&owner[0]; *(u8**)owner = owner;
        int64_t* ticks = (int64_t*)(game + UI_TICKS); float* dt = (float*)(game + UI_TICKS + 8);
        int64_t* start = (int64_t*)(timerObj + 0x23E00);
        int64_t* frozen = (int64_t*)(timerObj + 0x23DF8);       // flag set (menus): elapsed = [23DF8] - [23E00]
        *ticks = 1000000; *dt = 1.0f / 60; *start = 1000000 - 16666; *frozen = 1000000; timerObj[0x23DF0] = 1;
        static int64_t seenStep, stepElapsed, tagElapsed; static float seenDt, stepDt; static int calls, layerCalls, elemCalls, tagCalls;
        static u8* layerSeen;
        static int flow = 0xB;
        uiFn = [](void*, void*) { stepElapsed = *(int64_t*)(timerObj + 0x23DF8) - *(int64_t*)(timerObj + 0x23E00);
                                  stepDt = *(float*)(game + UI_TICKS + 8); calls++; };
        // UI object: layer slots at +18; shown steady layer (state 2, +509 set), hidden layer, closing layer (state 3)
        alignas(16) static u8 ui[0x81000], shown[0x800], hidden[0x800], closing[0x800], tag[0x800], elem[0x40];
        static void* vtbl[5], *evtbl[6];
        evtbl[5] = (void*)(ElemFn)[](u8* e, void* f) { elemCalls += e == elem && f != nullptr ? 1 : 100; };
        *(void***)elem = evtbl; ((u8**)(ui + 0x1B50))[7] = elem;
        vtbl[4] = (void*)(LayerFn)[](u8* l, u8* pool, void* owner, void*) {
            if (l == tag) { tagCalls++; tagElapsed = *(int64_t*)(timerObj + 0x23DF8) - *(int64_t*)(timerObj + 0x23E00); return; }
            seenStep = *(int64_t*)(timerObj + 0x23DF8) - *(int64_t*)(timerObj + 0x23E00);
            seenDt = *(float*)(game + UI_TICKS + 8); layerCalls++; layerSeen = l;
            if (pool != ui + 0x6C8 || owner != (void*)0x77) layerCalls += 100; };
        *(void**)ui = (void*)0x77;
        u8* layers[3] = {shown, hidden, closing};
        for (int i = 0; i < 3; i++) { *(void***)layers[i] = vtbl; ((u8**)(ui + 0x18))[i * 5] = layers[i]; }
        shown[0x509] = 1; *(int*)(shown + 0x7F4) = 2;
        *(int*)(hidden + 0x7F4) = 1;
        closing[0x509] = 1; *(int*)(closing + 0x7F4) = 3;
        // 3D layer (overhead tag): per step only, caught up at the next step by the time the frame calls covered
        *(void***)tag = vtbl; ((u8**)(ui + 0x18))[20] = tag; tag[0x509] = 1; *(int*)(tag + 0x7F4) = 2; *(int*)(tag + 0x500) = 2;
        stepCount = 500; updateUI(game);                    // frame 1: stepped (sets the game pointer)
        uiStepHook(ui, &flow);                              // the game's own (whole) UI update during the step
        bool okFirst = stepElapsed == 16666 && stepDt == 1.0f / 60; // first: the game's own step, clock dt untouched
        Sleep(20);
        timerObj[0x23DF0] = 0;                              // the flag is set only around the game's own UI update
        alpha = 0.5f; updateUI(game);                       // next frame, no step: animation only, real elapsed time
        bool okStep = layerCalls == 1 && elemCalls == 1 && layerSeen == shown && seenStep > 15000 && seenStep < 100000 && seenDt == 1.0f / 60;
        Sleep(5);
        timerObj[0x23DF0] = 1;
        stepCount = 501; uiStepHook(ui, &flow);             // the next step's own update: real time since the frame call
        okStep &= stepElapsed > 3000 && stepElapsed < 40000 && stepDt == 1.0f / 60;
        bool okTag = tagCalls == 1 && tagElapsed > 15000 && tagElapsed < 100000;
        bool restored = *ticks == 1000000 && *dt == 1.0f / 60 && *start == 1000000 - 16666;
        *frozen = *start; stepCount = 502; uiStepHook(ui, &flow); // paused (elapsed 0 at the step): no animation call
        timerObj[0x23DF0] = 0; updateUI(game); updateUI(game);
        // a lobby list active (ui+7F470 + 2*450, +438): no frame call
        ui[0x7F470 + 2 * 0x450 + 0x438] = 1; *frozen = *start + 16666; timerObj[0x23DF0] = 1;
        stepCount = 503; uiStepHook(ui, &flow); timerObj[0x23DF0] = 0; updateUI(game);
        bool okGate = layerCalls == 1 && elemCalls == 1;
        ui[0x7F470 + 2 * 0x450 + 0x438] = 0;
        printf("%-14s step calls %d, first %d, real-time elapsed ok %d, tag catch-up %d, lobby lists gate %d, restored %d\n", "ui", calls, okFirst, okStep, okTag, okGate, restored);
        bad += calls != 4 || layerCalls != 1 || elemCalls != 1 || !okFirst || !okStep || !okTag || !okGate || !restored;
    }
    // in-game frame rate labels: custom IDs resolve to our strings (length incl. terminator), others fall through
    {
        for (int i = 2; i < NCHOICES; i++) customStrings[i] = {FPS_LABELS[i], wcslen(FPS_LABELS[i]) + 1};
        static LocString game60 = {L"60", 3};
        lookupOrig = [](void*, uint32_t id) -> const LocString* { return id == FPS_LABEL_IDS[1] ? &game60 : nullptr; };
        const LocString* a = stringLookupHook(nullptr, CUSTOM_ID + 3);
        const LocString* b = stringLookupHook(nullptr, FPS_LABEL_IDS[1]);
        const LocString* c = stringLookupHook(nullptr, CUSTOM_ID - 1);
        bool ok = a && !wcscmp(a->text, L"144") && a->length == 4 && b == &game60 && !c && FPS_CHOICES[3] == 144;
        printf("%-14s custom %d, passthrough %d, other %d\n", "fps labels", a && !wcscmp(a->text, L"144"), b == &game60, !c);
        bad += !ok;
    }
    // particles: streams from the Position/Age locators; moved back by vel * (1 - alpha) * step, unborn ones hidden;
    // the renderer reads them from the private copy while the game's data stays untouched (also during the draw)
    {
        alignas(16) static u8 desc[0x40], job[0x80];
        alignas(16) static float pool[0x400];                // capacity 4: Position +0x20, Velocity +0x60, Age +0xA0
        *(uint32_t*)(desc + 4) = (0u << 18) | 0x20; *(uint32_t*)(desc + 8) = (2u << 18) | 0xA0;
        *(u8**)(job + 0x58) = (u8*)pool; *(uint32_t*)(job + 0x40) = 1; *(uint32_t*)(job + 0x44) = 1;
        float* pos = (float*)((u8*)pool + 0x20 + 16); float* vel = (float*)((u8*)pool + 0x60 + 16);
        float* age = (float*)((u8*)pool + 0xA0 + 4);
        pos[0] = 10; pos[2] = 30; vel[0] = 60; vel[2] = -60; *age = 0.1f;
        static float seen[3], game;
        spritesOrig = [](u8* d, u8* j) -> uint64_t {
            float* p = (float*)particleStream(d, j, 4, 16); seen[0] = p[0]; seen[1] = p[2]; seen[2] = p[1];
            game = ((float*)((u8*)pool + 0x30))[0]; return 7; };
        alpha = 0.5f; particleLogs = 100;
        uint64_t r = spritesHook(desc, job);
        bool shifted = fabsf(seen[0] - 9.5f) < 1e-4f && fabsf(seen[1] - 30.5f) < 1e-4f; // 60 m/s * 0.5 * 1/60
        *age = 0.001f; spritesHook(desc, job);               // younger than (1 - alpha) * step: hidden
        bool hidden = seen[2] < -1e4f && seen[0] == 10;
        bool restored = pos[0] == 10 && pos[1] == 0 && pos[2] == 30 && game == 10;
        *(uint32_t*)(desc + 8) = (3u << 18) | 0xA0;          // unexpected layout: untouched
        seen[0] = 0; spritesHook(desc, job);
        printf("%-14s shifted %d, unborn hidden %d, restored %d, odd layout untouched %d\n", "particles", shifted, hidden, restored, seen[0] == 10);
        bad += !shifted || !hidden || !restored || r != 7 || seen[0] != 10;
    }
    // motion blur sprites (sparks): Position / Velocity / Age at desc+4 / +8 / +C (the game's layout: 0x20, 0x2F0,
    // 0x5C0 for 45 per pool); same in-between-time move and hiding, game data untouched
    {
        alignas(16) static u8 desc[0x40], job[0x80];
        alignas(16) static u8 pool[0x800];
        *(uint32_t*)(desc + 4) = 0x20; *(uint32_t*)(desc + 8) = (1u << 18) | 0x2F0; *(uint32_t*)(desc + 0xC) = (2u << 18) | 0x5C0;
        *(u8**)(job + 0x58) = pool; *(uint32_t*)(job + 0x40) = 2; *(uint32_t*)(job + 0x44) = 1;
        float* pos = (float*)(pool + 0x20 + 2 * 16); float* vel = (float*)(pool + 0x2F0 + 2 * 16); float* age = (float*)(pool + 0x5C0 + 2 * 4);
        pos[0] = 5; pos[1] = 1; vel[0] = -120; *age = 0.2f;
        static float seen[2];
        motionBlurOrig = [](u8* d, u8* j) -> uint64_t { float* p = (float*)particleStream(d, j, 4, 16); seen[0] = p[0]; seen[1] = p[1]; return 9; };
        alpha = 0.25f; particleLogs = 100;
        bool shifted = motionBlurHook(desc, job) == 9 && fabsf(seen[0] - (5 + 120 * 0.75f / 60)) < 1e-4f && pos[0] == 5;
        *age = 0.005f; motionBlurHook(desc, job);
        bool hidden = seen[1] < -1e4f;
        printf("%-14s shifted %d, unborn hidden %d\n", "motion blur", shifted, hidden);
        bad += !shifted || !hidden;
    }
    // attached particles (scratch positions): moved by the nearest moving car's drawn - sim offset (private copy)
    {
        alignas(16) static u8 desc[0x40], job[0x80];
        alignas(16) static float scratch[0x100];
        *(uint32_t*)(desc + 4) = 0x21;                       // Position in scratch at +0x20
        *(u8**)(job + 0x20) = (u8*)scratch; *(uint32_t*)(job + 0x40) = 0; *(uint32_t*)(job + 0x44) = 1;
        float* p = scratch + 0x20 / 4;
        p[0] = 103; p[1] = 0; p[2] = 0;                      // 3 m behind the car
        stepCount = 900; alpha = 0.25f; particleLogs = 100;
        InstHist& car = insts[5];
        car.key = (void*)0x1234; car.step = 900;
        memset(car.prev, 0, 64); memset(car.cur, 0, 64);
        car.prev[12] = 99; car.cur[12] = 100;                // moved +1 m this step
        static float seen, game;
        attachedOrig = [](u8* d, u8* j) -> uint64_t { seen = *(float*)particleStream(d, j, 4, 16); game = scratch[0x20 / 4]; return 3; };
        uint64_t r = attachedHook(desc, job);
        bool shifted = fabsf(seen - 102.25f) < 1e-4f;        // drawn car is 0.75 m behind the sim car
        bool restored = p[0] == 103 && game == 103;
        car.step = 899; attachedHook(desc, job);             // car not updated yet this step: last step's motion
        bool early = fabsf(seen - 102.25f) < 1e-4f;
        car.step = 898; attachedHook(desc, job);             // stale car: untouched
        bool stale = seen == 103;
        car.step = 900; *(uint32_t*)(desc + 4) = 0x20;       // Position in the pool (flag 0) at +0x20
        *(u8**)(job + 0x58) = (u8*)scratch; *(u8**)(job + 0x20) = nullptr;
        attachedHook(desc, job);
        bool pool = fabsf(seen - 102.25f) < 1e-4f && p[0] == 103;
        *(uint32_t*)(desc + 4) = 0x21; *(u8**)(job + 0x20) = (u8*)scratch; // sprite batch with Position in scratch
        spritesOrig = [](u8* d, u8* j) -> uint64_t { seen = *(float*)particleStream(d, j, 4, 16); return 5; };
        bool sprites = spritesHook(desc, job) == 5 && fabsf(seen - 102.25f) < 1e-4f && p[0] == 103;
        car.key = nullptr;
        printf("%-14s shifted %d, restored %d, untracked untouched %d, pool batch %d, scratch sprites %d\n", "attached", shifted && early, restored, stale, pool, sprites);
        bad += !shifted || !early || !restored || !stale || !pool || !sprites || r != 3;
    }
    // prop packet sites: the expected bytes must match the unpacked game code. Needs a memory dump of the game
    // (dump.bin, not included in the repository); skipped when it's missing.
    if (FILE* d = fopen("dump.bin", "rb")) {
        int match = 0;
        for (int i = 0; i < 4; i++) {
            u8 want[PROP_COPY_LEN], have[PROP_COPY_LEN];
            propCopyOrig(PROP_COPIES[i], i >= 2, want);
            _fseeki64(d, (uintptr_t)PROP_COPIES[i] - 0x140000000, SEEK_SET);
            match += fread(have, 1, PROP_COPY_LEN, d) == PROP_COPY_LEN && !memcmp(want, have, PROP_COPY_LEN);
        }
        fclose(d);
        printf("%-14s %d of 4 match dump.bin\n", "prop sites", match);
        bad += match != 4;
    }
    // buffers handed to game code must be 16-byte aligned (game uses mulps/pshufd with memory operands)
    int misaligned = 0;
    for (int i = 0; i < 4; i++)
        misaligned += ((uintptr_t)insts[i].out | (uintptr_t)insts[i].cur | (uintptr_t)wheels[i].shadow) & 15 ? 1 : 0;
    printf("%-14s %d misaligned\n", "alignment", misaligned);
    bad += misaligned;
    // redirected reads: only the render thread inside Render gets the redirected address; others run the original
    {
        static const u8 code[] = {0x8B, 0x81, 0xA0, 0x00, 0x00, 0x00, 0xC3}; // mov eax, [rcx+A0]; ret
        u8* site = allocNear(0x140000000); // the stubs are rel32 from the site: both within allocNear's window
        memcpy(site, code, sizeof code);
        u8* stubs = allocNear((uintptr_t)site);
        redirectRead(stubs, 0, site, 6, code, 0x10);
        int data[0x40] = {}; data[0x10 / 4] = 1; data[0xA0 / 4] = 2;
        auto read = (int (*)(int*))site;
        offRender[0] = 0;
        int other = read(data);
        renderThread = GetCurrentThreadId(); inRender = true;
        int drawn = read(data);
        inRender = false;
        int after = read(data);
        printf("%-14s off-render %d, in Render %d, after %d, counted %lld\n", "redirects", other, drawn, after, offRender[0]);
        bad += !(other == 2 && drawn == 1 && after == 2 && offRender[0] == 2);
    }
    { // early instance refresh: previous <- drawn, drawn <- blend, stamp <- frame id; once per id
        alignas(16) static u8 obj[0x100];
        *(uintptr_t*)obj = INST_VTABLES[0];
        float* drawn = (float*)(obj + 0x10); float* prev = (float*)(obj + 0x50); float* sim = (float*)(obj + 0xA0);
        for (int i = 0; i < 16; i++) drawn[i] = 0, sim[i] = (i % 5 == 0) ? 1.f : 0.f;
        drawn[12] = 5; sim[12] = 100; *(uint16_t*)(obj + 0x90) = 41;
        stepCount = 900; alpha = 0.25f; inRender = true; renderThread = GetCurrentThreadId();
        InstHist* e = histFor(insts, obj, 900);
        e->instance = true; e->step = 899; memcpy(e->cur, sim, 64); e->cur[12] = 96;   // last step at 96, now 100
        frameNo = 10; refreshInstances(42);
        bool ok = prev[12] == 5 && fabsf(drawn[12] - 97) < 1e-4f && *(uint16_t*)(obj + 0x90) == 42;
        drawn[12] = 50; refreshInstances(42); ok &= drawn[12] == 50;                    // same frame: untouched
        // id past 16 bits (6+ minutes of play): still refreshed early; the game's own update later in the same frame
        // (stamp never matches: prev = drawn, then the blend) keeps the frame's true previous (motion vectors)
        frameNo = 11; refreshInstances(0x10043);
        bool longRun = prev[12] == 50 && fabsf(drawn[12] - 97) < 1e-4f;
        memcpy(prev, drawn, 64); blendInstance(obj, drawn);
        longRun &= prev[12] == 50 && fabsf(drawn[12] - 97) < 1e-4f;
        ok &= longRun;
        frameNo = 12; stepCount = 903; refreshInstances(43); ok &= *(uint16_t*)(obj + 0x90) == 43; // after a 3-step hitch: still refreshed
        e->key = nullptr; inRender = false;
        printf("%-14s refreshed %d, past frame id 0xFFFF %d\n", "early inst", ok, longRun);
        bad += !ok;
    }
    { // headlight: origin moved by the nearby car's drawn - sim offset, beam parameters / direction untouched
        stepCount = 900; alpha = 0.25f;
        InstHist& car = insts[7];
        car.key = (void*)0x9ABC; car.step = 900;
        memset(car.prev, 0, 64); memset(car.cur, 0, 64);
        car.prev[12] = 99; car.cur[12] = 100;
        static float got[12];
        setParam = [](void*, uint32_t id, float* src) -> uint64_t { memcpy(got, src, 48); return id; };
        float hl[12] = {100.1f, 0.9f, 0.1f, 0, 442, 470, 600, 0, 0, 0, 35, -35};
        bool ok = headlightHook(nullptr, 0xE, hl) == 0xE && fabsf(got[0] - 99.35f) < 1e-4f && got[1] == 0.9f && got[6] == 600 && got[10] == 35;
        // online: origin built from the drawn instance ([inst+10]) moves rigidly with it -> left alone; else shifted
        alignas(16) static u8 inst[0xE0];
        float* drawn = (float*)(inst + 0x10);
        float I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 99.5f, 0, 0, 1};
        memcpy(drawn, I, 64);
        car.key = inst; car.instance = true;
        float a[12] = {99.6f, 0.9f, 0.1f, 0, 442, 470, 600, 0, 0, 0, 35, -35}, b[12], c[12];
        memcpy(b, a, 48); memcpy(c, a, 48);
        frameNo = 50; headlightHook(nullptr, 0xE, a);                // first frame: no history, shifted
        bool first = fabsf(got[0] - (99.6f - 0.75f)) < 1e-4f;
        frameNo = 51; drawn[12] += 0.3f; b[0] += 0.3f; headlightHook(nullptr, 0xE, b); // moved with the drawn car
        bool rigid = got[0] == b[0];
        frameNo = 52; drawn[12] += 0.3f; headlightHook(nullptr, 0xE, c); // stayed at the sim pose: shifted
        bool sim = fabsf(got[0] - (99.6f - 0.75f)) < 1e-4f;    // (c is shifted in place)
        ok &= first && rigid && sim;
        car.key = nullptr; car.instance = false;
        printf("%-14s moved %d (first %d, rigid with drawn car untouched %d, sim origin shifted %d)\n", "headlight", ok, first, rigid, sim);
        bad += !ok;
    }
    { // input record / replay: a run starts when the race mode turns 3 (event state 7); replay gives the game the
      // recorded keys on the same polls, ignoring the real keyboard
        static u8 mode[0x34E00], evt[0x500], root[0x10], gameObj[0x4B0900];
        static u8 *pm = nullptr, *pe = evt, *pg = root;
        modeOwner = &pm; eventOwner = &pe; gameOwner = &pg;
        *(u8**)(root + 8) = gameObj + 0x10;                          // root = game + 0x10
        *(int*)(evt + 0x4D8) = 7;
        GetFullPathNameA("build\\replaytest.log", MAX_PATH, logPath, nullptr);
        static bool down[256];
        realKey = [](int vk) -> SHORT { return down[vk] ? (SHORT)0x8001 : 0; };
        static short stick; static short padSeen;                   // pad 0 left stick X: real value / what the game got
        realPad = [](DWORD user, void* st) -> DWORD { memset(st, 0, 16); ((short*)st)[4] = user ? 0 : stick; return 0; };
        auto poll = [](bool* seen) {
            for (int vk = 8; vk < 256; vk++) seen[vk] = (keyHook(vk) & 0x8000) != 0;
            u8 st[16]; padHook(0, st); padSeen = ((short*)st)[4];   // the game reads the pad once per step too
        };
        bool seen[256];
        replayMode = 1; lastRunning = false;
        poll(seen);                                                  // before the race: nothing recorded
        pm = mode; *(int*)(mode + 0x34DD8) = 3;                      // race running
        const int pattern[5][2] = {{0x26, 0}, {0x26, 0x25}, {0, 0x25}, {0x27, 0}, {0, 0}}; // up, up+left, left, right, none
        int n = 0;
        for (auto& k : pattern) {
            memset(down, 0, sizeof down); if (k[0]) down[k[0]] = 1; if (k[1]) down[k[1]] = 1;
            stick = (short)(1000 * (n + 1)); poll(seen);
            static const int states[5] = {7, 6, 0, 8, 9};           // rolling start, then GO: 7 -> 6 -> 0 -> 8 -> 9
            *(int*)(evt + 0x4D8) = states[n < 4 ? n + 1 : 4]; if (++n == 1) *(int*)(mode + 0x34DD8) = 0;
        }
        bool kept = runActive;
        *(int*)(evt + 0x4D8) = 3; poll(seen);                        // event left: run ends, file closed
        bool rec = kept && !runActive;
        *(int*)(evt + 0x4D8) = 7;
        replayMode = 2; memset(down, 0, sizeof down); down[0x28] = 1; // replay: the real keyboard holds "down"
        stick = -5;                                                  // and the real stick is somewhere else
        *(int*)(mode + 0x34DD8) = 3;
        bool same = true; n = 0;
        for (auto& k : pattern) {
            poll(seen);
            same &= padSeen == (short)(1000 * (n + 1));
            static const int states[5] = {7, 6, 0, 8, 9};
            *(int*)(evt + 0x4D8) = states[n < 4 ? n + 1 : 4]; if (++n == 1) *(int*)(mode + 0x34DD8) = 0;
            for (int vk = 8; vk < 256; vk++) same &= seen[vk] == (vk == k[0] || vk == k[1]) && (vk != 0x28 || !seen[vk]);
        }
        poll(seen); bool ended = !runActive && seen[0x28] && padSeen == -5; // file ended: real keyboard / pad again
        *(int*)(evt + 0x4D8) = 3; poll(seen); *(int*)(evt + 0x4D8) = 7;
        replayMode = 0; modeOwner = (u8**)0x141279210; eventOwner = (u8**)0x141279220; gameOwner = (u8**)0x14129C498;
        bool ok = rec && same && ended;
        printf("%-14s recorded %d, replayed same keys + stick per step %d, back to keyboard / pad at the end %d\n", "input replay", rec, same, ended);
        bad += !ok;
    }
    { // update gates: the stub calls the original with its arguments, or returns 0 on a no-step frame when switched on
        u8* page = (u8*)VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        static int calls; static uint64_t got[6];
        typedef uint64_t(__fastcall* F6)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
        F6 orig = [](uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) -> uint64_t {
            calls++; uint64_t v[6] = {a, b, c, d, e, f}; memcpy(got, v, sizeof v); return 0x55; };
        F6 stub = (F6)buildGateStub(page, 3, (uintptr_t)orig);
        page[0] = 1; page[4] = 0;                                   // no-step frame, gate off: runs
        bool pass = stub(1, 2, 3, 4, 5, 6) == 0x55 && calls == 1 && got[0] == 1 && got[3] == 4 && got[5] == 6;
        page[0] = 0; page[4] = 1;                                   // step frame, gate on: runs
        pass &= stub(7, 0, 0, 0, 0, 0) == 0x55 && calls == 2 && got[0] == 7;
        page[0] = 1; page[4] = 1;                                   // no-step frame, gate on: skipped
        bool skip = stub(9, 0, 0, 0, 0, 0) == 0 && calls == 2;
        VirtualFree(page, 0, MEM_RELEASE);
        printf("%-14s runs with args %d, skipped on no-step frames when on %d\n", "update gates", pass, skip);
        bad += !(pass && skip);
    }
    { // car light records: the stack copy's position moves with the per-step car, all 13 arguments pass through;
      // a car the game moves every frame (online opponents) is drawn as the game has it and shifts nothing
        stepCount = 900; alpha = 0.25f;
        InstHist& car = insts[7];
        car.key = (void*)0x9ABC; car.step = 900; car.gap = 1; car.perFrameStep = 0;
        memset(car.prev, 0, 64); memset(car.cur, 0, 64);
        car.prev[12] = 99; car.cur[12] = 100;
        static uint64_t args[13]; static float pos[3];
        lightOrig = [](uint64_t a, float* m, uint64_t c, uint64_t d, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8,
                       uint64_t s9, uint64_t s10, uint64_t s11, uint64_t s12, uint64_t s13) -> uint64_t {
            uint64_t v[13] = {a, (uint64_t)m, c, d, s5, s6, s7, s8, s9, s10, s11, s12, s13};
            memcpy(args, v, sizeof v); memcpy(pos, m + 12, 12); return 77; };
        alignas(16) float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 101.5f, 0.6f, 2.f, 1};
        bool through = lightHook(1, m, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13) == 77 && args[0] == 1 && args[12] == 13 && args[6] == 7;
        bool moved = fabsf(pos[0] - (101.5f - 0.75f)) < 1e-4f && pos[1] == 0.6f;
        car.perFrameStep = 900; m[12] = 101.5f;                    // moved by the game this step: left alone
        lightHook(1, m, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13);
        bool other = pos[0] == 101.5f;
        // blendInstance: sim changing within a step marks the object; it is then drawn at its sim matrix
        alignas(16) static u8 obj[0xE0]; alignas(16) float world[16];
        InstHist* e = histFor(insts, obj, 900);
        float* sim = (float*)(obj + 0xA0);
        float I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 10, 0, 0, 1};
        memcpy(sim, I, 64); stepCount = 899; blendInstance(obj, world);
        sim[12] = 11; stepCount = 900; blendInstance(obj, world);   // step: blended
        bool blended = world[12] != 11;
        sim[12] = 11.4f; blendInstance(obj, world);                  // same step, moved: the game moves it per frame
        bool asGame = movedPerFrame(e, 900) && world[12] == 11.4f;
        e->key = nullptr; car.key = nullptr; car.perFrameStep = 0; car.gap = 0;
        bool ok = through && moved && other && blended && asGame;
        printf("%-14s args through %d, moved %d, per-frame car left alone %d, per-frame instance drawn as is %d\n", "car lights", through, moved, other, blended && asGame);
        bad += !ok;
    }
    { // overhead tag: layer transform (obj+568) moved inside Render by the nearby car's offset, unchanged outside
        stepCount = 900; alpha = 0.25f;
        InstHist& car = insts[7];
        car.key = (void*)0x9ABC; car.step = 900;
        memset(car.prev, 0, 64); memset(car.cur, 0, 64);
        car.prev[12] = 99; car.cur[12] = 100;
        alignas(16) static u8 layer[0x600];
        float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 100.2f, 0.5f, 0, 1};
        memcpy(layer + 0x568, m, 64);
        float out[16];
        renderThread = GetCurrentThreadId(); inRender = true;
        bool ok = layerTransform(layer, out) == out && fabsf(out[12] - 99.45f) < 1e-4f && out[13] == 0.5f && out[0] == 1 && out[15] == 1;
        inRender = false;
        ok &= layerTransform(layer, out) == out && out[12] == 100.2f; // outside Render: the stored transform
        car.key = nullptr;
        printf("%-14s moved %d\n", "overhead tag", ok);
        bad += !ok;
    }
    { // map camera: element (class MAP_VTABLE) camera block 10 m apart between two steps; an item matrix equal to
      // the latest rows +C0..+FF gets the blended rows inside Render, other items / outside Render stay as stored
        alignas(16) static u8 mapui[0x3C00], mapel[0x1000], item[0x260], other[0x260];
        *(uintptr_t*)mapel = MAP_VTABLE; ((u8**)(mapui + 0x1B50))[3] = mapel;
        void* saveUi = uiObj; uiObj = mapui;
        Block p = b; p.f[0x130 / 4] += 10;
        memcpy(mapel + 0xC10, &p, sizeof p); stepCount = 1000; updateMapCamera();
        memcpy(mapel + 0xC10, &b, sizeof b); stepCount = 1001; alpha = 0.5f; updateMapCamera();
        Block want = b; blendCamera(want.f, p, b, 0.5f);
        memcpy(item + 0x210, M(b, 0xC0), 64);
        memcpy(other + 0x210, M(b, 0xC0), 64); ((float*)(other + 0x210))[3] += 1;
        float out[16], out2[16];
        renderThread = GetCurrentThreadId(); inRender = true;
        mapItemTransform(item, nullptr, out); mapItemTransform(other, nullptr, out2);
        bool ok = mapBlendOn && !memcmp(out, M(want, 0xC0), 64) && memcmp(out, M(b, 0xC0), 64) && !memcmp(out2, other + 0x210, 64);
        inRender = false;
        mapItemTransform(item, nullptr, out); ok &= !memcmp(out, item + 0x210, 64);
        updateMapCamera(); ok &= mapBlendOn;                                  // same step, next frame: still blended
        uiObj = saveUi; mapElem = nullptr; mapSnaps = 0;
        printf("%-14s blended %d\n", "map camera", ok);
        bad += !ok;
    }
    { // trail: points at locator desc+3C (attribute 1), car near the newest (last) point only, slid back by the car's
      // drawn - sim offset; the renderer's own write into the descriptor (last point) lands in the real descriptor
        alignas(16) static u8 desc[0x80], job[0x80];
        alignas(16) static float pool[0x100];
        *(uint32_t*)(desc + 0x3C) = (1u << 18) | 0x40; *(uint32_t*)(desc + 0x40) = 0x20; *(uint32_t*)(desc + 0x44) = 0x30;
        *(u8**)(job + 0x58) = (u8*)pool; *(uint32_t*)(job + 0x40) = 0; *(uint32_t*)(job + 0x44) = 3;
        float* pt = pool + 0x40 / 4;
        for (int i = 0; i < 3; i++) pt[i * 4] = 112.f - 4 * i, pt[i * 4 + 3] = (float)i;  // 12, 8, 4 m behind the car
        stepCount = 900; alpha = 0.25f; particleLogs = 100;
        InstHist& car = insts[6];
        car.key = (void*)0x5678; car.step = 900;
        memset(car.prev, 0, 64); memset(car.cur, 0, 64);
        car.prev[12] = 99; car.cur[12] = 100;
        static float seen[3], w;
        trailOrig = [](u8* d, u8* j) -> uint64_t {
            float* q = (float*)particleStream(d, j, 0x3C, 16);
            for (int i = 0; i < 3; i++) seen[i] = q[i * 4];
            w = q[11]; *(float*)(d + 0x20) = q[8]; return 9; };
        bool ok = trailHook(desc, job) == 9 && fabsf(seen[0] - 111.25f) < 1e-4f && fabsf(seen[2] - 103.25f) < 1e-4f && w == 2
            && pt[0] == 112 && *(float*)(desc + 0x20) == seen[2];
        car.key = nullptr;
        printf("%-14s slid back %d\n", "trail", ok);
        bad += !ok;
    }
    { // particle Position redirect: streams decoded like the game's renderers; Position from p, the other streams on the
      // same base and on the other base read the game's values, the game's data and job are unchanged
        static float scratch[0x400], pool[0x400];
        alignas(16) float p[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        for (int i = 0; i < 0x400; i++) scratch[i] = 1000.f + i, pool[i] = 5000.f + i;
        for (int posFlag = 0; posFlag < 2; posFlag++) {
            alignas(16) u8 desc[0x40] = {}, job[0x80] = {}, j0[0x80];
            uint32_t locs[7] = {0x120u | posFlag, 0x200u | posFlag, 0x80u | posFlag, 0x300u | (posFlag ^ 1), 0x40u | posFlag, 0x10u | (posFlag ^ 1), 0x380u | posFlag};
            memcpy(desc + 4, locs, sizeof locs);
            *(u8**)(job + 0x20) = (u8*)scratch; *(u8**)(job + 0x58) = (u8*)pool; *(uint32_t*)(job + 0x40) = 3; *(uint32_t*)(job + 0x44) = 2;
            memcpy(j0, job, sizeof j0);
            static float seenPos[8], seenOther[7];
            static const uint32_t* L; L = locs;
            ParticleFn fake = [](u8* d, u8* j) -> uint64_t {
                memcpy(seenPos, particleStream(d, j, 4, 16), 32);
                for (int i = 1; i < 7; i++) seenOther[i] = *(float*)particleStream(d, j, 4 + 4 * i, i & 1 ? 4 : 16);
                return 7; };
            uint64_t r = drawWithPositions(fake, desc, job, 4, 7, p, 2);
            bool ok = r == 7 && !memcmp(seenPos, p, 32) && !memcmp(j0, job, sizeof j0);
            for (int i = 1; i < 7; i++) ok &= seenOther[i] == *(float*)particleStream(desc, job, 4 + 4 * i, i & 1 ? 4 : 16);
            ok &= scratch[0] == 1000.f && pool[0x120 / 4 + 12] == 5000.f + 0x120 / 4 + 12 && scratch[0x120 / 4 + 12] == 1000.f + 0x120 / 4 + 12;
            printf("%-14s position base %d: %s\n", "particle copy", posFlag, ok ? "ok" : "BAD");
            bad += !ok;
        }
    }
    { // crash report: a fault is appended to the log (access, registers, stack); guarded copies are not reported
        GetFullPathNameA("build\\crashtest.log", MAX_PATH, logPath, nullptr); DeleteFileA(logPath);
        logOn = true; installCrashGuard();
        writeNull();
        u8 dst[16]; bool guarded = !copySpan(dst, (void*)16, sizeof dst);
        char text[8192] = {}; FILE* f = fopen(logPath, "rb");
        if (f) { fread(text, 1, sizeof text - 1, f); fclose(f); }
        int faults = 0;
        for (const char* q = text; (q = strstr(q, "fault ")); q++) faults++;
        bool ok = guarded && faults == 1 && strstr(text, "access violation: write of 0") && strstr(text, "rsp ") && strstr(text, "stack scan");
        printf("%-14s reported %d, guarded copy quiet %d\n", "crash report", ok, guarded);
        bad += !ok;
    }
    puts(bad ? "FAIL" : "OK");
    return bad;
}
