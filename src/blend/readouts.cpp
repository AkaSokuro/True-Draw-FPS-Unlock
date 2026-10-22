// ---- Overhead tags (the name / health labels above other cars) ----
// The game places each tag once per step, at the car's simulated position. We replace the copy in the tag's
// position getter (at 1402D69A4): while drawing, the tag is moved by the same amount the nearest car was moved by
// blending, so it stays on the car.
#include "../tdfps.h"

static u8* const LAYER_XFORM = (u8*)0x1402D69A4;
static const u8 LAYER_XFORM_ORIG[] = {0x0F, 0x10, 0x81, 0x68, 0x05, 0, 0, 0x0F, 0x10, 0x89, 0x78, 0x05, 0, 0};
extern "C" float* layerTransform(u8* obj, float* out) {
    memcpy(out, obj + 0x568, 64);
    float d[3];
    if (inRender && GetCurrentThreadId() == renderThread && carShiftNear(out + 12, 1, d))
        for (int c = 0; c < 3; c++) out[12 + c] += d[c];
    return out;
}

void installReadouts() {
    if (!optObjects) return;
    if (memcmp(LAYER_XFORM, LAYER_XFORM_ORIG, sizeof LAYER_XFORM_ORIG) == 0) {
        // A plain jump, not a call: layerTransform returns straight to whoever called the game's getter.
        patchJump(LAYER_XFORM, 5, jumpTo((void*)layerTransform));
        logf("overhead tags patched");
    } else logf("overhead tags NOT patched (bytes differ)");
}
