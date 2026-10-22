// ---- Render instances (cars, traffic, cops) ----
// Keeps each object's matrix history and produces the blended world matrix that gets drawn.
#include "../tdfps.h"

// Every frame, the instance update (1400CD040) first copies the drawn matrix to "previous", then copies the
// simulated matrix ([rcx+A0]) to the drawn one ([rsi]). The second copy is replaced by inst_stub.asm, which calls
// blendInstance(rcx, rsi) to write a blended matrix instead.
static u8* const INST_COPY = (u8*)0x1400CD0D4;
static const u8 INST_COPY_ORIG[] = {
    0x0F, 0x10, 0x81, 0xA0, 0x00, 0x00, 0x00, 0x0F, 0x11, 0x06, 0x0F, 0x10, 0x89, 0xB0, 0x00, 0x00, 0x00,
    0x0F, 0x11, 0x4E, 0x10, 0x0F, 0x10, 0x81, 0xC0, 0x00, 0x00, 0x00, 0x0F, 0x11, 0x46, 0x20, 0x0F, 0x10,
    0x89, 0xD0, 0x00, 0x00, 0x00, 0x0F, 0x11, 0x4E, 0x30};
extern "C" void instStub();
extern "C" uintptr_t instReturn = 0x1400CD0FF;
// Reads of the simulated matrix ([reg+A0..DF]) that only happen while drawing (found with Probe=1).
// Subtracting 0x90 from each read's offset makes it read the drawn (blended) matrix instead.
struct RedirectSite { u8* at; int len; u8 orig[15]; };
static const RedirectSite RENDER_READS[] = {
#include "../redirect_sites.inc"
};
extern const int NRENDER_READS = sizeof RENDER_READS / sizeof RENDER_READS[0], NREDIRECTS = NRENDER_READS + 4;
u8* redirectAt[NREDIRECTS + 1];
volatile LONG64 offRender[NREDIRECTS + 1];
// The game's vehicle world-matrix helper, 140171950(veh, out), returns local [veh+290] x body [[veh+10+8*[veh+354]]+60].
// It is mostly called by physics, but some calls come from inside Render. Its first 10 bytes can be safely copied
// elsewhere, which lets us still call the original.
static u8* const VEH_WORLD = (u8*)0x140171950;
static const u8 VEH_WORLD_ORIG[] = {0x4C, 0x8B, 0xDC, 0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00};
// The functions that return an object's simulated matrix (vtable entry 4 of instances and characters).
// Each is just `lea rax,[rcx+A0]; ret`.
static u8* const SIM_GETTERS[] = {(u8*)0x1404332B0, (u8*)0x140433260};
static const u8 SIM_GETTER_ORIG[] = {0x48, 0x8D, 0x81, 0xA0, 0x00, 0x00, 0x00, 0xC3};

// Records the simulated matrix if the simulation has stepped since this object was last seen.
// gap = how many steps apart prev and cur are; 0 means there is no usable previous matrix.
void track(Hist* e, const float* sim, uint64_t step) {
    if (e->step == step) return;
    uint64_t k = step - e->step; // for a new entry e->step is ~0, so k is huge and gap becomes 0
    e->gap = k >= 1 && k <= 5 ? (uint32_t)k : 0;
    memcpy(e->prev, e->gap ? e->cur : sim, 64);
    memcpy(e->cur, sim, 64);
    e->step = step;
}

InstHist insts[HIST_SLOTS];

// Blends the object's last two simulated matrices for this frame and writes the result to out.
// If the object jumped more than 20 m in one step (teleport or respawn), it is drawn where it is, unblended.
void blendHist(const Hist* e, const float* sim, float* out) {
    const float *a = e->prev, *b = e->cur;
    float dx = b[12] - a[12], dy = b[13] - a[13], dz = b[14] - a[14];
    if (dx * dx + dy * dy + dz * dz > 20.f * 20.f) { memcpy(out, sim, 64); return; }
    float t = blendT(e->gap);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) out[r * 4 + c] = a[r * 4 + c] + (b[r * 4 + c] - a[r * 4 + c]) * t;
        out[r * 4 + 3] = b[r * 4 + 3]; // column 3 is not part of the transform
    }
    // Blending two rotations in a straight line shrinks the result slightly. Restore each axis's length
    // (this also keeps scaled objects at the right size).
    for (int r = 0; r < 3; r++) {
        float* v = out + r * 4;
        float la = sqrtf(a[r * 4] * a[r * 4] + a[r * 4 + 1] * a[r * 4 + 1] + a[r * 4 + 2] * a[r * 4 + 2]);
        float lb = sqrtf(b[r * 4] * b[r * 4] + b[r * 4 + 1] * b[r * 4 + 1] + b[r * 4 + 2] * b[r * 4 + 2]);
        float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l > 1e-6f) { float s = (la + (lb - la) * t) / l; v[0] *= s; v[1] *= s; v[2] *= s; }
    }
}

// Writes the blended world matrix for instance `obj` into `world` (called from inst_stub.asm and refreshInstances).
extern "C" void blendInstance(u8* obj, float* world) {
    const float* sim = (const float*)(obj + 0xA0);
    if (!inRender || GetCurrentThreadId() != renderThread) InterlockedIncrement64(&offRender[NREDIRECTS]);
    uint64_t step = (uint64_t)stepCount;
    InstHist* e = histFor(insts, obj, step);
    if (!e) { memcpy(world, sim, 64); return; }
    e->instance = true;
    // The game marks each instance with a 16-bit frame id so it only updates once per frame. Once the frame id passes
    // 0xFFFF that mark never matches, so the instance gets updated several times in one frame. Each update would set
    // "previous" = drawn, which zeroes the motion vectors (used for motion blur). So later updates in the same frame
    // get the frame's first "previous" matrix [obj+50] back.
    float* prevWorld = (float*)(obj + 0x50);
    if (world == (float*)(obj + 0x10)) {
        if (e->updFrame != frameNo) { e->updFrame = frameNo; memcpy(e->truePrev, prevWorld, 64); }
        else memcpy(prevWorld, e->truePrev, 64);
    }
    if (e->step == step && memcmp(e->cur, sim, 64)) e->perFrameStep = step; // it moved without a simulation step
    track(e, sim, step);
    if (movedPerFrame(e, step)) memcpy(world, sim, 64); // the game already moves it every frame: draw as is
    else blendHist(e, sim, world);
}

// ---- Early refresh ----
// The game's instance update runs late in Render, after some code (like the headlight pool) has already read the
// drawn matrices. That code then lagged one frame behind the car.
// So just before that code runs (the call to 1403DE3E0 at 14038CCA0), we run the same update on every recently seen
// instance ourselves. Each one is marked with this frame's id ([timer+23E08]), so the game's own update skips it.
static const uintptr_t INST_VTABLES[] = {0x140F114E8, 0x140F108B0, 0x140F10798}; // classes that share update 1400CD050
typedef void(__fastcall* EarlyFn)(void*, uint32_t);
static u8* const CALL_EARLY = (u8*)0x14038CCA0;
static const uintptr_t FN_EARLY = 0x1403DE3E0;

static void refreshInstances(uint32_t id) {
    uint64_t step = (uint64_t)stepCount;
    for (InstHist& e : insts) {
        // Only instances seen within the last 5 steps (the most one frame can cover); others may no longer exist.
        if (!e.key || !e.instance || e.step + 5 < step) continue;
        u8* obj = (u8*)e.key;
        uintptr_t vt = *(uintptr_t*)obj;
        if (vt != INST_VTABLES[0] && vt != INST_VTABLES[1] && vt != INST_VTABLES[2]) continue;
        if (*(uint16_t*)(obj + 0x90) == id) continue; // already updated this frame
        memcpy(obj + 0x50, obj + 0x10, 64);           // same as the game: drawn -> previous, then the new drawn matrix
        blendInstance(obj, (float*)(obj + 0x10));
        *(uint16_t*)(obj + 0x90) = (uint16_t)id;
    }
}

static void __fastcall earlyHook(void* renderer, uint32_t arg) {
    if (u8* t = uiTimer()) refreshInstances(*(uint32_t*)(t + 0x23E08));
    ((EarlyFn)FN_EARLY)(renderer, arg);
}

// Replaces the simulated-matrix getter. While drawing (render thread, inside Render) it returns the blended matrix,
// so lights, shadows and attached parts follow the drawn car. Everyone else (gameplay, physics, worker threads)
// still gets the real simulated matrix.
template <int OFF> static const float* matrixGetter(u8* obj) {
    const float* sim = (const float*)(obj + OFF);
    if (!inRender || GetCurrentThreadId() != renderThread) return sim;
    uint64_t step = (uint64_t)stepCount;
    InstHist* e = histFor(insts, obj, step);
    if (!e) return sim;
    if (e->frame != frameNo) { // blend once per frame and reuse the result
        track(e, sim, step);
        // Not moving, or already moved every frame by the game: use it as is.
        if (memcmp(e->prev, e->cur, 64) == 0 || movedPerFrame(e, step)) memcpy(e->out, sim, 64);
        else blendHist(e, sim, e->out);
        e->frame = frameNo;
    }
    return e->out;
}

// Replaces the vehicle world-matrix helper (140171950). While drawing, it builds the matrix from the blended car
// body; everywhere else it calls the original.
typedef float*(__fastcall* VehWorldFn)(u8* veh, float* out);
static VehWorldFn vehWorldOrig;
extern "C" float* vehWorld(u8* veh, float* out) {
    if (!inRender || GetCurrentThreadId() != renderThread) return vehWorldOrig(veh, out);
    const float* B = (const float*)(*(u8**)(veh + 0x10 + 8 * (int64_t)*(int32_t*)(veh + 0x354)) + 0x60);
    alignas(16) float b[16];
    uint64_t step = (uint64_t)stepCount;
    InstHist* e = histFor(insts, (void*)B, step);
    if (e && e->step == step && memcmp(e->cur, B, 64)) e->perFrameStep = step; // it moved without a simulation step
    if (e) track(e, B, step);
    if (e && !movedPerFrame(e, step)) blendHist(e, B, b);
    else memcpy(b, B, 64);
    // out = local matrix x blended body matrix
    const float* L = (const float*)(veh + 0x290);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[r * 4 + c] = L[r * 4] * b[c] + L[r * 4 + 1] * b[4 + c] + L[r * 4 + 2] * b[8 + c] + (r == 3 ? b[12 + c] : 0);
    return out;
}

// Logs how often each redirected read (or instance copy) ran outside Render since the last log.
void logOffRender() {
    for (int i = 0; i <= NREDIRECTS; i++)
        if (LONG64 n = InterlockedExchange64(&offRender[i], 0))
            logf("  off-render: %s %p x%lld", i < NREDIRECTS ? "read" : "instance copy", i < NREDIRECTS ? redirectAt[i] : INST_COPY, n);
}

void installObjects() {
    if (!optObjects) return;
    logf("early instance refresh %s", redirectCall(CALL_EARLY, FN_EARLY, (void*)earlyHook) ? "patched" : "NOT patched");
    if (memcmp(INST_COPY, INST_COPY_ORIG, sizeof INST_COPY_ORIG) == 0) patchJump(INST_COPY, sizeof INST_COPY_ORIG, jumpTo(instStub));
    for (int i = 0; i < 2; i++) {
        if (memcmp(SIM_GETTERS[i], SIM_GETTER_ORIG, sizeof SIM_GETTER_ORIG) != 0) { logf("getter %d NOT patched", i); continue; }
        patchJump(SIM_GETTERS[i], 5, jumpTo((void*)matrixGetter<0xA0>));
    }
    logf("sim getters patched");
    bool ok = true;
    for (auto& r : RENDER_READS) ok &= memcmp(r.at, r.orig, r.len) == 0;
    u8* p = ok ? allocNear(0x14038BEC0, 96 * NRENDER_READS) : nullptr;
    ok = p != nullptr;
    int i = 0;
    if (ok)
        for (auto& r : RENDER_READS) redirectRead(p, i++, r.at, r.len, r.orig, *(uint32_t*)(r.orig + r.len - 4) - 0x90);
    logf("render reads %s (%d sites)", ok ? "patched" : "NOT patched (bytes differ)", NRENDER_READS);
    if (memcmp(VEH_WORLD, VEH_WORLD_ORIG, sizeof VEH_WORLD_ORIG) == 0) {
        vehWorldOrig = (VehWorldFn)relocate(VEH_WORLD, sizeof VEH_WORLD_ORIG);
        patchJump(VEH_WORLD, sizeof VEH_WORLD_ORIG, jumpTo((void*)vehWorld));
        logf("vehicle world helper patched");
    }
}
