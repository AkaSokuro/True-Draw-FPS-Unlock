// ---- Wheels ----
// Player car: the draw code gets a private copy of each wheel with a blended matrix, where spin and steering are
// blended as rotations (slerp).
// Traffic: the wheel placement code is made to read the car's blended matrix instead of its simulated one.
#include "../tdfps.h"

// The draw code reads a wheel's matrix relative to its car (`movups xmm4,[rdi+10]`). wheel_stub.asm replaces rdi
// with the result of blendWheel(rdi).
static u8* const WHEEL_READ = (u8*)0x1401F8EBA;
static const u8 WHEEL_READ_ORIG[] = {0x0F, 0x10, 0x67, 0x10, 0x48, 0x8D, 0x05, 0x3B, 0x71, 0xE0, 0xFF};
extern "C" void wheelStub();
extern "C" uintptr_t wheelReturn = 0x1401F8EC5;
// Traffic wheel placement (140182A9B) reads the car's simulated matrix at [rsi+A0..DF]. These four reads are
// redirected to the drawn (blended) matrix at [rsi+10..4F].
static const struct { u8* at; u8 orig[8]; int len; uint32_t disp; } TRAFFIC_WHEEL_READS[] = {
    {(u8*)0x14018320F, {0x0F, 0x10, 0x96, 0xC0, 0x00, 0x00, 0x00}, 7, 0x30}, // movups xmm2,[rsi+C0]
    {(u8*)0x14018325F, {0x0F, 0x10, 0x8E, 0xB0, 0x00, 0x00, 0x00}, 7, 0x20}, // movups xmm1,[rsi+B0]
    {(u8*)0x140183276, {0x0F, 0x10, 0x86, 0xA0, 0x00, 0x00, 0x00}, 7, 0x10}, // movups xmm0,[rsi+A0]
    {(u8*)0x1401832A1, {0x0F, 0x58, 0x9E, 0xD0, 0x00, 0x00, 0x00}, 7, 0x40}, // addps  xmm3,[rsi+D0]
};

// Wheels are never blended in place: physics code on other threads reads the same data at the same time, and
// changing it broke physics. Instead the draw code gets a private copy ("shadow") with the blended matrix.
struct WheelHist : Hist { alignas(16) u8 shadow[0x70]; };
static WheelHist wheels[HIST_SLOTS];
int wheelsThisFrame, wheelsMax; // counters for the log: different wheels drawn per frame

static void toQuat(const float* m, float q[4]) { // rotation part (rows 0-2) of a matrix -> quaternion (x, y, z, w)
    float t = m[0] + m[5] + m[10];
    if (t > 0) {
        float s = sqrtf(t + 1) * 2;
        q[3] = s / 4; q[0] = (m[6] - m[9]) / s; q[1] = (m[8] - m[2]) / s; q[2] = (m[1] - m[4]) / s;
    } else if (m[0] > m[5] && m[0] > m[10]) {
        float s = sqrtf(1 + m[0] - m[5] - m[10]) * 2;
        q[3] = (m[6] - m[9]) / s; q[0] = s / 4; q[1] = (m[4] + m[1]) / s; q[2] = (m[8] + m[2]) / s;
    } else if (m[5] > m[10]) {
        float s = sqrtf(1 + m[5] - m[0] - m[10]) * 2;
        q[3] = (m[8] - m[2]) / s; q[0] = (m[4] + m[1]) / s; q[1] = s / 4; q[2] = (m[9] + m[6]) / s;
    } else {
        float s = sqrtf(1 + m[10] - m[0] - m[5]) * 2;
        q[3] = (m[1] - m[4]) / s; q[0] = (m[8] + m[2]) / s; q[1] = (m[9] + m[6]) / s; q[2] = s / 4;
    }
}

static void fromQuat(const float q[4], float* m) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + z * w);     m[2] = 2 * (x * z - y * w);
    m[4] = 2 * (x * y - z * w);     m[5] = 1 - 2 * (x * x + z * z); m[6] = 2 * (y * z + x * w);
    m[8] = 2 * (x * z + y * w);     m[9] = 2 * (y * z - x * w);     m[10] = 1 - 2 * (x * x + y * y);
}

// Blends matrix a towards b by t: rotation along the shortest arc (slerp), position in a straight line.
static void slerpMatrix(const float* a, const float* b, float t, float* out) {
    float qa[4], qb[4], q[4];
    toQuat(a, qa); toQuat(b, qb);
    float d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    if (d < 0) { d = -d; for (float& v : qb) v = -v; }
    float wa = 1 - t, wb = t;
    if (d < 0.9995f) {
        float th = acosf(d), s = sinf(th);
        wa = sinf((1 - t) * th) / s; wb = sinf(t * th) / s;
    }
    float l = 0;
    for (int i = 0; i < 4; i++) { q[i] = qa[i] * wa + qb[i] * wb; l += q[i] * q[i]; }
    l = 1 / sqrtf(l);
    for (float& v : q) v *= l;
    fromQuat(q, out);
    for (int r = 0; r < 4; r++) out[r * 4 + 3] = b[r * 4 + 3];
    for (int c = 0; c < 3; c++) out[12 + c] = a[12 + c] + (b[12 + c] - a[12 + c]) * t;
}

// Returns the struct the draw code should read from: the game's own, or a blended private copy.
extern "C" u8* blendWheel(u8* wheel) {
    if (!inRender) return wheel;
    uint64_t step = (uint64_t)stepCount;
    WheelHist* e = histFor(wheels, wheel, step);
    if (!e) return wheel;
    if (e->frame != frameNo) {
        e->frame = frameNo;
        wheelsThisFrame++;
        const float* m = (const float*)(wheel + 0x10);
        track(e, m, step);
        memcpy(e->shadow, wheel, sizeof e->shadow);
        const float *a = e->prev, *b = e->cur;
        float dx = b[12] - a[12], dy = b[13] - a[13], dz = b[14] - a[14];
        // Only blend across a single step. Slerp always takes the shortest rotation, so a wheel that spun a lot over
        // 2 or more steps could appear to turn backwards; in that case the simulated pose is shown as is.
        if (e->gap == 1 && dx * dx + dy * dy + dz * dz < 1.f) slerpMatrix(a, b, blendT(1), (float*)(e->shadow + 0x10));
    }
    return e->shadow;
}

void installWheels() {
    if (optObjects) { // traffic wheels follow the blended traffic car (part of SmoothObjects)
        bool ok = true;
        for (auto& r : TRAFFIC_WHEEL_READS) ok &= memcmp(r.at, r.orig, r.len) == 0;
        u8* p = ok ? allocNear(0x14038BEC0, 96 * 4) : nullptr;
        ok = p != nullptr;
        int i = NRENDER_READS;
        if (ok)
            for (auto& r : TRAFFIC_WHEEL_READS) redirectRead(p, i++, r.at, r.len, r.orig, r.disp);
        logf("traffic wheels %s", ok ? "patched" : "NOT patched (bytes differ)");
    }
    if (optWheels && memcmp(WHEEL_READ, WHEEL_READ_ORIG, sizeof WHEEL_READ_ORIG) == 0)
        patchJump(WHEEL_READ, sizeof WHEEL_READ_ORIG, jumpTo(wheelStub));
}
