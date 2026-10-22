// ---- Effects ----
// Particles are drawn at each frame's in-between time. Effects attached to cars, and car lights, are moved along
// with the blended car.
#include "../tdfps.h"

// How the game's particle data is laid out:
//   - The sprite renderer, 1400EFD50(desc, job), runs on worker threads, once per batch of particles.
//   - desc holds "locators" for each attribute: (attribute << 18) | offset | flag.
//   - An attribute's data starts at (flag ? scratch [job+20] : pool [job+58]) + offset + first [job+40] * stride.
//     [job+44] is the number of particles in the batch.
//   - Pool attributes come in this order: 0 Position, 1 Velocity, 2 Age (in seconds). So the Velocity data sits
//     exactly halfway between the Position and Age data.
static u8* const FN_SPRITES = (u8*)0x1400EFD50;
static const u8 SPRITES_ORIG[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x50, 0x10}; // mov rax,rsp; mov [rax+10],rdx
typedef uint64_t(__fastcall* ParticleFn)(u8* desc, u8* job);
static ParticleFn spritesOrig;
static volatile long particleLogs;

// Copies n bytes; returns false if the source runs into memory that can't be read.
static bool copySpan(void* to, const void* from, size_t n) {
    bool ok = true;
    quietFaults++; // a fault here is expected and handled, so the crash report should ignore it
    __try { memcpy(to, from, n); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    quietFaults--;
    return ok;
}

// Finds a batch's position, velocity and age data. Returns false if the layout isn't the one we expect.
static bool particleStreams(u8* desc, u8* job, float** pos, float** vel, float** age) {
    uint32_t lp = *(uint32_t*)(desc + 4), la = *(uint32_t*)(desc + 8);
    if ((lp >> 18) != 0 || (la >> 18) != 2 || (lp & 1) || (la & 1)) return false; // need Position and Age, both in the pool
    uint32_t op = lp & 0x3FFF0, oa = la & 0x3FFF0;
    if (oa <= op || (oa - op) % 32) return false;
    uint32_t first = *(uint32_t*)(job + 0x40);
    u8* base = *(u8**)(job + 0x58) + (uintptr_t)first * 16;
    *age = (float*)(*(u8**)(job + 0x58) + oa + (uintptr_t)first * 4);
    *pos = (float*)(base + op);
    *vel = (float*)(base + op + (oa - op) / 2); // halfway between Position and Age
    return true;
}

// Runs the particle renderer fn, but with the particle positions taken from p (n entries of 4 floats).
// The game's particle data is never edited in place: the simulation updates it while worker threads draw it, and
// editing it broke physics. Instead, the batch's data is copied to a private buffer, the new positions are written
// into the copy, and the renderer is given a copy of the job that points at it.
// ponytail: assumes every attribute is at most 16 bytes per particle (reading too far is caught safely);
// check this if a renderer uses bigger ones.
static uint64_t drawWithPositions(ParticleFn fn, u8* desc, u8* job, int loc0, int nloc, const float* p, uint32_t n) {
    const size_t COPY = 0x50000;
    static thread_local u8* copy; // one buffer per drawing thread, kept for as long as the thread lives
    if (!copy && !(copy = (u8*)_aligned_malloc(COPY, 16))) return fn(desc, job);
    alignas(16) u8 j[0x80];
    uint32_t lp = *(uint32_t*)(desc + loc0), flag = lp & 1, first = *(uint32_t*)(job + 0x40), lo = 0x3FFF0, hi = 0;
    // Find the range of memory that holds all attributes stored in the same place as Position.
    for (int i = 0; i < nloc; i++) {
        uint32_t l = *(uint32_t*)(desc + loc0 + 4 * i);
        if ((l & 1) != flag) continue;
        lo = (l & 0x3FFF0) < lo ? l & 0x3FFF0 : lo;
        hi = (l & 0x3FFF0) > hi ? l & 0x3FFF0 : hi;
    }
    size_t size = hi + ((size_t)first + n) * 16 - lo;
    u8* base = *(u8**)(job + (flag ? 0x20 : 0x58));
    if (size > COPY || !copySpan(copy, base + lo, size)) return fn(desc, job); // can't copy it: draw unchanged
    memcpy(copy + (lp & 0x3FFF0) - lo + (size_t)first * 16, p, (size_t)n * 16);
    memcpy(j, job, sizeof j);
    *(u8**)(j + (flag ? 0x20 : 0x58)) = copy - lo;
    return fn(desc, j);
}

// Nitrous flames (1400EE3B0) are placed once per step at the car's simulated exhaust. So each batch is moved by the
// same amount as the nearest car was moved by blending.
// ponytail: only moves them, doesn't rotate them; add the car's rotation if flames visibly come off in hard turns.
static u8* const FN_ATTACHED = (u8*)0x1400EE3B0;
static ParticleFn attachedOrig;

// Finds the car closest (within 6 m) to the first or last particle (trails reach far behind their car), and returns
// in d how far blending moved that car from its simulated position. Returns false if there's nothing to move.
bool carShiftNear(const float* p, uint32_t n, float d[3]) {
    uint64_t step = (uint64_t)stepCount;
    const InstHist* hit = nullptr;
    float best = 6.f * 6.f;
    for (const float* q : {p, p + (n - 1) * 4})
    for (const InstHist& e : insts) {
        // Cars seen this step or the one before (worker threads may draw before this frame's car update has run).
        if (!e.key || e.step + 1 < step || e.step > step) continue;
        float dx = e.cur[12] - q[0], dy = e.cur[13] - q[1], dz = e.cur[14] - q[2], r = dx * dx + dy * dy + dz * dz;
        if (r < best) { best = r; hit = &e; }
    }
    if (!hit || movedPerFrame(hit, step)) return false; // the game already moves that car every frame: nothing to fix
    float m = 0;
    float k = 1.0f - blendT(hit->gap);
    for (int c = 0; c < 3; c++) { d[c] = (hit->prev[12 + c] - hit->cur[12 + c]) * k; m += d[c] * d[c]; }
    return m > 0 && m < 20.f * 20.f; // car not moving, or teleported: nothing to do
}

// Draws a batch that the game places once per step at or behind a car (flames, trail points), moved along with the
// nearest blended car.
static uint64_t drawCarShifted(ParticleFn fn, u8* desc, u8* job, int loc0, int nloc, const char* what) {
    alignas(16) static thread_local float moved[4096 * 4];
    uint32_t n = *(uint32_t*)(job + 0x44), lp = *(uint32_t*)(desc + loc0);
    if (!n || n > 4096) return fn(desc, job);
    float* pos = (float*)(*(u8**)(job + (lp & 1 ? 0x20 : 0x58)) + (lp & 0x3FFF0) + (uintptr_t)*(uint32_t*)(job + 0x40) * 16);
    float d[3];
    if (!carShiftNear(pos, n, d)) return fn(desc, job);
    if (particleLogs < 24 && InterlockedIncrement(&particleLogs) <= 24)
        logf("%s: n=%u pos=(%.2f %.2f %.2f) shift=(%.3f %.3f %.3f)", what, n, pos[0], pos[1], pos[2], d[0], d[1], d[2]);
    for (uint32_t i = 0; i < n; i++)
        for (int c = 0; c < 4; c++) moved[i * 4 + c] = pos[i * 4 + c] + (c < 3 ? d[c] : 0);
    return drawWithPositions(fn, desc, job, loc0, nloc, moved, n);
}

extern "C" uint64_t attachedHook(u8* desc, u8* job) {
    if (*(uint32_t*)(desc + 4) >> 18) return attachedOrig(desc, job); // the first locator must be Position (attribute 0)
    return drawCarShifted(attachedOrig, desc, job, 4, 7, "attached");
}

// Taillight trails (RenderTrail, 1400F1D10): one point is added per step (Position locator at desc+3C).
// The whole trail is moved along with the car.
static u8* const FN_TRAIL = (u8*)0x1400F1D10;
static ParticleFn trailOrig;
extern "C" uint64_t trailHook(u8* desc, u8* job) {
    return drawCarShifted(trailOrig, desc, job, 0x3C, 3, "trail");
}

// Draws a batch of particles where they were at this frame's drawn time, which is k seconds before the latest step
// (k = (1 - alpha) steps). Each particle is moved back by velocity * k. Particles younger than k didn't exist yet at
// that time, so they are hidden by moving them 100 km down.
static uint64_t drawAtDrawnTime(ParticleFn fn, u8* desc, u8* job, const float* pos, const float* vel, const float* age,
                                uint32_t n, int nloc, const char* what) {
    alignas(16) static thread_local float moved[4096 * 4];
    float k = (1.0f - alpha - lookahead) * (float)STEP; // below 0 with lookahead: particles move forward, none hidden
    int hidden = 0;
    for (uint32_t i = 0; i < n; i++) {
        for (int c = 0; c < 4; c++) moved[i * 4 + c] = pos[i * 4 + c];
        if (age[i] < k) { moved[i * 4 + 1] -= 1e5f; hidden++; continue; } // didn't exist yet at the drawn time
        for (int c = 0; c < 3; c++) moved[i * 4 + c] -= vel[i * 4 + c] * k;
    }
    if (n > 1 && particleLogs < 12 && InterlockedIncrement(&particleLogs) <= 12) {
        float lo = age[0], hi = age[0];
        for (uint32_t i = 1; i < n; i++) { lo = age[i] < lo ? age[i] : lo; hi = age[i] > hi ? age[i] : hi; }
        logf("%s: n=%u pos=(%.2f %.2f %.2f) age %.4f..%.4f hidden=%d k=%.4f", what, n, pos[0], pos[1], pos[2], lo, hi, hidden, k);
    }
    return drawWithPositions(fn, desc, job, 4, nloc, moved, n);
}

// Replaces the sprite renderer (smoke, dust, ...).
extern "C" uint64_t spritesHook(u8* desc, u8* job) {
    uint32_t n = *(uint32_t*)(job + 0x44);
    // Positions in scratch memory belong to effects placed at a car each step: move them with the car.
    if ((*(uint32_t*)(desc + 4) & 0xFFFC0001) == 1) return drawCarShifted(spritesOrig, desc, job, 4, 6, "sprites");
    float *pos, *vel, *age;
    if (!n || n > 4096 || !particleStreams(desc, job, &pos, &vel, &age)) return spritesOrig(desc, job);
    return drawAtDrawnTime(spritesOrig, desc, job, pos, vel, age, n, 6, "particles");
}

// Sparks (RenderSpriteMotionBlur, 1400F0830): sprites stretched along their velocity.
// Their Position, Velocity and Age locators are at desc+4, +8 and +C.
static u8* const FN_MOTIONBLUR = (u8*)0x1400F0830;
static ParticleFn motionBlurOrig;
extern "C" uint64_t motionBlurHook(u8* desc, u8* job) {
    uint32_t n = *(uint32_t*)(job + 0x44), first = *(uint32_t*)(job + 0x40);
    uint32_t lp = *(uint32_t*)(desc + 4), lv = *(uint32_t*)(desc + 8), la = *(uint32_t*)(desc + 0xC);
    if (!n || n > 4096 || lp >> 18 != 0 || lv >> 18 != 1 || la >> 18 != 2 || (lp | lv | la) & 1)
        return motionBlurOrig(desc, job);
    u8* pool = *(u8**)(job + 0x58);
    const float* pos = (const float*)(pool + (lp & 0x3FFF0) + (uintptr_t)first * 16);
    const float* vel = (const float*)(pool + (lv & 0x3FFF0) + (uintptr_t)first * 16);
    const float* age = (const float*)(pool + (la & 0x3FFF0) + (uintptr_t)first * 4);
    return drawAtDrawnTime(motionBlurOrig, desc, job, pos, vel, age, n, 7, "motion blur sprites");
}

// ---- Player headlight pool (gPlayerHeadlight) ----
// The pool of light on the road in front of the player's car. The game sets it at 1403E27EB from the car's
// simulated position. We move it by the same amount the nearest car was moved by blending, unless it already
// follows the drawn car exactly (as it does online, where moving it again made it lag).
// ponytail: only moves it, doesn't rotate it; also rotate its direction (src+8) if the pool visibly swings.
typedef uint64_t(__fastcall* SetParamFn)(void* commands, uint32_t id, float* src);
static u8* const CALL_HEADLIGHT = (u8*)0x1403E27EB;
static const uintptr_t FN_SET_PARAM = 0x1400CF050;
static SetParamFn setParam = (SetParamFn)FN_SET_PARAM;
long hlCalls, hlShifted, hlRigid;

// Converts point p into the local coordinates of the nearest car as drawn ([inst+10]: rows 0-2 = axes,
// row 3 = position). Returns false if no car is close enough.
static bool headlightLocal(const float* p, float local[3]) {
    const InstHist* car = nullptr; float best = 6.f * 6.f;
    for (const InstHist& e : insts) {
        if (!e.key || !e.instance || e.step != (uint64_t)stepCount) continue;
        float dx = e.cur[12] - p[0], dy = e.cur[13] - p[1], dz = e.cur[14] - p[2], r = dx * dx + dy * dy + dz * dz;
        if (r < best) { best = r; car = &e; }
    }
    if (!car) return false;
    const float* m = (const float*)((u8*)car->key + 0x10);
    for (int r = 0; r < 3; r++) {
        const float* b = m + r * 4;
        float l2 = b[0] * b[0] + b[1] * b[1] + b[2] * b[2];
        local[r] = l2 > 1e-6f ? ((p[0] - m[12]) * b[0] + (p[1] - m[13]) * b[1] + (p[2] - m[14]) * b[2]) / l2 : 0;
    }
    return true;
}

// Replaces the call that sets the headlight pool's position.
static uint64_t __fastcall headlightHook(void* commands, uint32_t id, float* src) {
    static float lastLocal[3]; static uint32_t lastFrame;
    float d[3], local[3];
    hlCalls++;
    bool have = headlightLocal(src, local), rigid = false;
    if (have && lastFrame + 1 == frameNo) {
        float dx = local[0] - lastLocal[0], dy = local[1] - lastLocal[1], dz = local[2] - lastLocal[2];
        // If its spot on the drawn car hasn't changed since last frame (< 1 cm), it already follows the drawn car.
        rigid = dx * dx + dy * dy + dz * dz < 0.01f * 0.01f;
    }
    if (have) { memcpy(lastLocal, local, 12); lastFrame = frameNo; }
    if (rigid) hlRigid++;
    else if (carShiftNear(src, 1, d)) { hlShifted++; for (int c = 0; c < 3; c++) src[c] += d[c]; }
    return setParam(commands, id, src);
}

// ---- Car lights (this is also how the headlight pool is drawn online) ----
// A worker thread builds each light (calling 1404778F0 at 140476F5A) from a copy of its per-step matrix on the stack.
// We move that copy by the same amount the nearest car was moved by blending.
// ponytail: only moves it, doesn't rotate it, same as the headlight pool.
typedef uint64_t(__fastcall* LightFn)(uint64_t, float*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                      uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static u8* const CALL_LIGHT = (u8*)0x140476F5A;
static const uintptr_t FN_LIGHT = 0x1404778F0; // takes 13 arguments: 4 in registers, 9 on the stack ([rsp+20..60])
static LightFn lightOrig = (LightFn)FN_LIGHT;
long lightCalls, lightShifted;
static uint64_t __fastcall lightHook(uint64_t a, float* m, uint64_t c, uint64_t d4, uint64_t s5, uint64_t s6, uint64_t s7,
                                     uint64_t s8, uint64_t s9, uint64_t s10, uint64_t s11, uint64_t s12, uint64_t s13) {
    float d[3];
    InterlockedIncrement(&lightCalls);
    if (carShiftNear(m + 12, 1, d)) { InterlockedIncrement(&lightShifted); for (int i = 0; i < 3; i++) m[12 + i] += d[i]; }
    return lightOrig(a, m, c, d4, s5, s6, s7, s8, s9, s10, s11, s12, s13);
}

void installEffects() {
    if (optObjects) logf("headlight %s", redirectCall(CALL_HEADLIGHT, FN_SET_PARAM, (void*)headlightHook) ? "patched" : "NOT patched");
    if (optObjects) logf("car lights %s", redirectCall(CALL_LIGHT, FN_LIGHT, (void*)lightHook) ? "patched" : "NOT patched");
    const struct { u8* fn; ParticleFn* orig; void* hook; } particleHooks[] = {
        {FN_SPRITES, &spritesOrig, (void*)spritesHook}, {FN_ATTACHED, &attachedOrig, (void*)attachedHook},
        {FN_TRAIL, &trailOrig, (void*)trailHook}, {FN_MOTIONBLUR, &motionBlurOrig, (void*)motionBlurHook}};
    for (int i = 0; i < 4 && optParticles; i++) {
        const auto& ph = particleHooks[i];
        if (memcmp(ph.fn, SPRITES_ORIG, sizeof SPRITES_ORIG) != 0) { logf("particle renderer %p NOT patched", ph.fn); continue; }
        // Keep a way to call the original: its first 7 bytes, copied elsewhere, followed by a jump back.
        *ph.orig = (ParticleFn)relocate(ph.fn, sizeof SPRITES_ORIG);
        patchJump(ph.fn, sizeof SPRITES_ORIG, jumpTo(ph.hook));
        logf("particle renderer %p patched", ph.fn);
    }
}
