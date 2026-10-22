// ---- Camera ----
// The main view and the rear-view mirror are blended in place just before Render, and the real values are put
// back right after it. Also blends the career map's camera.
#include "../tdfps.h"

static const uintptr_t VIEW_OWNER = 0x14129C4E8;    // the camera block (0x1A0 bytes) is at [[VIEW_OWNER]+8]+0x10

// Layout of a camera block (floats, row-major, row vectors):
//   +000 view matrix (4x3; element [3][3] is 0)
//   +040 projection
//   +080 view x projection
//   +0C0 three rows: columns x, y and w of view x projection
//   +100 camera world matrix (rows 0-2 = axes, row 3 = position; column 3 holds unrelated data)
struct Block { float f[0x1A0 / 4]; };
static const int VIEW_OFF[2] = {0x10, 0x200};       // main view, rear-view mirror (the mirror isn't updated when hidden)
// Per camera:
//   gap - steps between prev and cur
//   vel - how far the camera moves per step
//   cut - in this step the camera turned around on the spot (e.g. look back)
struct Cam { Block prev, cur; int snaps; uint32_t gap; bool prevOk, curOk, velOk, cut; float vel[3]; };
static Cam cams[2];

static float* M(Block& b, int off) { return b.f + off / 4; }

// Blends camera a towards camera b by t, and rebuilds the view matrices in `out` from the blended camera.
static void blendCamera(float* out, Block& a, Block& b, float t) {
    float* W = out + 0x100 / 4;
    float* Wa = M(a, 0x100); float* Wb = M(b, 0x100);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 3; c++) W[r * 4 + c] = Wa[r * 4 + c] + (Wb[r * 4 + c] - Wa[r * 4 + c]) * t;
    // Blending can leave the axes slightly skewed or shortened. Make them unit length and at right angles again
    // (Gram-Schmidt), keeping the same handedness.
    auto dot = [](float* x, float* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
    auto norm = [&](float* x) { float l = sqrtf(dot(x, x)); for (int i = 0; i < 3; i++) x[i] /= l; };
    auto sub = [&](float* x, float* y) { float d = dot(x, y); for (int i = 0; i < 3; i++) x[i] -= d * y[i]; };
    float *r0 = W, *r1 = W + 4, *r2 = W + 8, *p = W + 12;
    norm(r2); sub(r1, r2); norm(r1); sub(r0, r2); sub(r0, r1); norm(r0);

    // The game's view matrix is the inverse of the camera's world matrix, with axis 0 flipped.
    // With R = rows (-r0, r1, r2): the view's 3x3 part is R transposed, and its row 3 is -position x R transposed.
    float R[3][3] = {{-r0[0], -r0[1], -r0[2]}, {r1[0], r1[1], r1[2]}, {r2[0], r2[1], r2[2]}};
    float V[4][4] = {};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) V[i][j] = R[j][i];
    for (int j = 0; j < 3; j++) V[3][j] = -(p[0] * V[0][j] + p[1] * V[1][j] + p[2] * V[2][j]);
    float* outV = out;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) outV[i * 4 + j] = V[i][j]; // [3][3] is written as 0, like the game does
    V[3][3] = 1;
    // view x projection, and the three column rows the game keeps at +0C0
    float* P = M(b, 0x40);
    float VP[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) VP[i][j] = V[i][0] * P[j] + V[i][1] * P[4 + j] + V[i][2] * P[8 + j] + V[i][3] * P[12 + j];
    float* outVP = out + 0x80 / 4;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) outVP[i * 4 + j] = VP[i][j];
    float* outT = out + 0xC0 / 4;
    const int cols[3] = {0, 1, 3};
    for (int r = 0; r < 3; r++)
        for (int i = 0; i < 4; i++) outT[r * 4 + i] = VP[i][cols[r]];
}

static uintptr_t* viewOwner = (uintptr_t*)VIEW_OWNER;
static float* cameraBlock(int view) {
    uintptr_t o = *viewOwner;
    if (!o) return nullptr;
    uintptr_t v = *(uintptr_t*)(o + 8);
    return v ? (float*)(v + VIEW_OFF[view]) : nullptr;
}

// Checks that the block is a real camera that works the same way as the main view: rebuilding it from its world
// matrix must give back the same view matrices. (The mirror's block holds a placeholder at startup.)
static bool camConsistent(Block& b) {
    Block t = b;
    blendCamera(t.f, b, b, 1.0f);
    for (int i = 0; i < 0xF0 / 4; i++) {
        if (i >= 0x40 / 4 && i < 0x80 / 4) continue; // the projection is copied, not rebuilt, so skip it
        if (!(fabsf(t.f[i] - b.f[i]) <= 1e-3f * (1.0f + fabsf(b.f[i])))) return false; // written so NaN also fails
    }
    return true;
}

// True if the camera cut or reset between a and b, so it must not be blended across.
static bool camJump(Block& a, Block& b) {
    float* pa = M(a, 0x130); float* pb = M(b, 0x130);
    float dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
    float* fa = M(a, 0x120); float* fb = M(b, 0x120); // axis 2: the direction the camera faces
    float d = fa[0] * fb[0] + fa[1] * fb[1] + fa[2] * fb[2];
    return dx * dx + dy * dy + dz * dz > 30.f * 30.f || d < 0.5f; // moved over 30 m, or turned over 60 degrees, in one step
}

// The camera turned around on the spot (e.g. look back), so there is no earlier view to blend from. Instead, move
// the new camera back along its last motion, to match where the blended cars are drawn.
static void cutCamera(float* out, Block& cur, const float* vel, float steps) {
    Block s = cur;
    for (int i = 0; i < 3; i++) M(s, 0x130)[i] += vel[i] * steps;
    blendCamera(out, s, s, 1.0f);
}

static float* blk[2];  // this frame's camera blocks
static bool blend[2];  // true for each view that applyView overwrote and restoreView must put back

// Called just before Render: records each camera when the simulation steps, then writes the blended camera.
void applyView() {
    blend[0] = blend[1] = false;
    blk[0] = cameraBlock(0); blk[1] = cameraBlock(1);
    if (!blk[0]) return;

    static double camStep = -1;
    bool stepped = camStep != stepCount; // the simulation has stepped since the last frame
    double k = stepCount - camStep;
    uint32_t gap = camStep >= 0 && k <= 5 ? (uint32_t)k : 0;
    camStep = stepCount;
    for (int v = 0; v < 2; v++) {
        Cam& c = cams[v];
        if (stepped) {
            c.prev = c.cur; c.prevOk = c.curOk; c.gap = gap;
            memcpy(&c.cur, blk[v], sizeof(Block));
            c.curOk = v == 0 || camConsistent(c.cur);
            static int rejects;
            if (!c.curOk && rejects < 5 && memcmp(&c.prev, &c.cur, sizeof(Block))) {
                rejects++;
                float* P = M(c.cur, 0x40); float* W = M(c.cur, 0x100);
                logf("view %d not a camera like the main one, not blended: proj %.3f %.3f %.3f %.3f pos %.1f %.1f %.1f fwd %.2f %.2f %.2f",
                     v, P[0], P[5], P[11], P[15], W[12], W[13], W[14], W[8], W[9], W[10]);
            }
            c.snaps++;
            // Update the camera's speed, or flag a turn-around (cut), from how far it moved this step.
            float *a = M(c.prev, 0x130), *b = M(c.cur, 0x130), d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
            c.cut = false;
            if (c.snaps < 2 || !c.gap || !c.prevOk || !c.curOk || d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 30.f * 30.f) c.velOk = false;
            else if (camJump(c.prev, c.cur)) c.cut = c.velOk;
            else { for (int i = 0; i < 3; i++) c.vel[i] = d[i] / c.gap; c.velOk = true; }
        }
        // Blend only with two valid snapshots that differ and aren't separated by a cut (or are a turn-around).
        blend[v] = optCamera && c.snaps >= 2 && c.gap && c.prevOk && c.curOk && (c.cut || !camJump(c.prev, c.cur)) && memcmp(&c.prev, &c.cur, sizeof(Block));
        if (blend[v] && c.cut) cutCamera(blk[v], c.cur, c.vel, alpha + lookahead - 1);
        else if (blend[v]) blendCamera(blk[v], c.prev, c.cur, blendT(c.gap));
    }
}

// Called right after Render: puts the real camera back, so game logic never sees the blended one.
// Only the parts we wrote are restored (column 3 of the world matrix is left alone).
void restoreView() {
    for (int v = 0; v < 2; v++) {
        if (!blend[v]) continue;
        memcpy(blk[v], cams[v].cur.f, 0xF0);
        float* W = blk[v] + 0x100 / 4; float* C = M(cams[v].cur, 0x100);
        for (int r = 0; r < 4; r++) memcpy(W + r * 4, C + r * 4, 12);
    }
}

// ---- Career map camera (SmoothUI setting) ----
// The career map UI element (class 140E2F780) keeps its own camera block at +C10, updated once per step.
// Each 3D item on the map copies rows +C0..+FF of it to item+210 once per step, and Render reads them through
// 1401BF500 (the copy is at 1401BF52C). While drawing, if an item's matrix equals the map camera's latest rows,
// we hand out the blended camera's rows instead.
static const uintptr_t MAP_VTABLE = 0x140E2F780;
static u8* const MAP_ITEM_XFORM = (u8*)0x1401BF52C;
static const u8 MAP_ITEM_XFORM_ORIG[] = {0x0F, 0x10, 0x81, 0x10, 0x02, 0, 0, 0x41, 0x0F, 0x11, 0x00};
static Block mapPrev, mapCur, mapBlend;
static u8* mapElem;
static int mapSnaps;
static bool mapBlendOn;

// Like camJump, but with a much bigger distance: the map camera can fly kilometres per second, so only a real
// cut or reset should count.
static bool mapJump(Block& a, Block& b) {
    float* pa = M(a, 0x130); float* pb = M(b, 0x130);
    float dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
    float* fa = M(a, 0x120); float* fb = M(b, 0x120);
    return dx * dx + dy * dy + dz * dz > 3000.f * 3000.f || fa[0] * fb[0] + fa[1] * fb[1] + fa[2] * fb[2] < 0.5f;
}

void updateMapCamera() {
    static double mapStep = -1;
    static uint32_t mapGap; // steps between mapPrev and mapCur
    mapBlendOn = false;
    if (!uiObj) return;
    if (mapStep != stepCount) { // the simulation (and the game's UI update) has stepped: find the map and record it
        double k = stepCount - mapStep;
        mapGap = mapStep >= 0 && k <= 5 ? (uint32_t)k : 0;
        mapStep = stepCount;
        u8** elems = (u8**)((u8*)uiObj + 0x1B50);
        u8* e = nullptr;
        for (int i = 0; i < 0x400 && !e; i++)
            if (elems[i] && *(uintptr_t*)elems[i] == MAP_VTABLE) e = elems[i];
        if (e != mapElem) { mapElem = e; mapSnaps = 0; }
        if (!e) return;
        mapPrev = mapCur;
        memcpy(&mapCur, e + 0xC10, sizeof(Block));
        mapSnaps++;
    }
    if (!mapElem || mapSnaps < 2 || !mapGap || mapJump(mapPrev, mapCur) || !memcmp(&mapPrev, &mapCur, 0x140)) return;
    mapBlend = mapCur; // the projection (+40), its constants (+F0) and everything else stay from the latest step
    blendCamera(mapBlend.f, mapPrev, mapCur, blendT(mapGap));
    mapBlendOn = true;
}

extern "C" void mapItemTransform(u8* item, const float*, float* out) {
    memcpy(out, item + 0x210, 64);
    if (mapBlendOn && inRender && GetCurrentThreadId() == renderThread && !memcmp(out, M(mapCur, 0xC0), 64))
        memcpy(out, M(mapBlend, 0xC0), 64);
}

void installMapCamera() {
    if (!optUI) return;
    if (memcmp(MAP_ITEM_XFORM, MAP_ITEM_XFORM_ORIG, sizeof MAP_ITEM_XFORM_ORIG) == 0) {
        // A plain jump, not a call: mapItemTransform returns straight to whoever called the game's getter.
        patchJump(MAP_ITEM_XFORM, 5, jumpTo((void*)mapItemTransform));
        logf("map camera patched");
    } else logf("map camera NOT patched (bytes differ)");
}
