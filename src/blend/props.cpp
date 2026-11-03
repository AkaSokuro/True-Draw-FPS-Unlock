// ---- Props (cones, signs, debris, ...) ----
// Four places in the draw code copy a prop's world matrix ([reg+20] to [reg+5F]) into a draw packet every frame.
// The register is rbx for the first two sites and rdi for the other two.
// Each 43-byte copy is replaced by prop_stub.asm, which calls blendProp(reg+20, packet) to write a blended matrix.
#include "../tdfps.h"

static u8* const PROP_COPIES[] = {(u8*)0x140205421, (u8*)0x1402055AC, (u8*)0x140205E97, (u8*)0x140206267};
static const int PROP_COPY_LEN = 43;
// Builds the original bytes expected at a copy site, so we can check them before patching.
static void propCopyOrig(u8* site, bool rdi, u8* out) {
    static const u8 t[] = {0x0F, 0x10, 0x43, 0x20, 0x48, 0x8D, 0x0D, 0, 0, 0, 0, 0xBA, 0x08, 0x00, 0x00, 0x00, 0x0F, 0x11,
        0x00, 0x0F, 0x10, 0x4B, 0x30, 0x0F, 0x11, 0x48, 0x10, 0x0F, 0x10, 0x43, 0x40, 0x0F, 0x11, 0x40, 0x20, 0x0F,
        0x10, 0x4B, 0x50, 0x0F, 0x11, 0x48, 0x30};
    memcpy(out, t, sizeof t);
    *(int32_t*)(out + 7) = (int32_t)(0x1412DBA30 - (uintptr_t)(site + 11));
    if (rdi) for (int i : {2, 21, 29, 37}) out[i] += 4; // same instructions, but reading from rdi instead of rbx
}
extern "C" void propStub0(); extern "C" void propStub1(); extern "C" void propStub2(); extern "C" void propStub3();
static void (*const PROP_STUBS[])() = {propStub0, propStub1, propStub2, propStub3};
extern "C" uintptr_t propReturns[4] = {0x14020544C, 0x1402055D7, 0x140205EC2, 0x140206292};

static const int PROP_SLOTS = 65536; // bigger table: lots of static scenery passes through the same draw code
static Hist props[PROP_SLOTS];
int propsThisFrame, propsMax; // counters for the log: props blended per frame

// Writes the prop's matrix into the draw packet: blended if the prop is moving, copied as is otherwise.
extern "C" void blendProp(const float* sim, float* packet) {
    if (inRender && GetCurrentThreadId() == renderThread) {
        uint64_t step = (uint64_t)stepCount;
        if (Hist* e = histFor<PROP_SLOTS>(props, (void*)sim, step)) {
            track(e, sim, step);
            if (memcmp(e->prev, e->cur, 64) != 0) { // only blend props that moved; still ones are copied below
                blendHist(e, sim, packet);
                propsThisFrame++;
                return;
            }
        }
    }
    memcpy(packet, sim, 64);
}

// Props with several models (the police helicopter: body and rotors) are also put on a model list, which is what
// draws them: 14020C690(obj, list) calls 140518A70(list, world, models, count, id) at 14020C6C0 with the prop's
// matrix (obj+50, the same matrix the copies above read). It copies the matrix once, so a blended copy on the stack
// is enough.
typedef void(__fastcall* ModelListFn)(void* list, const float* world, void* models, uint32_t count, uint64_t id);
static u8* const CALL_MODEL_LIST = (u8*)0x14020C6C0;
static const uintptr_t FN_MODEL_LIST = 0x140518A70;
static void __fastcall modelListHook(void* list, const float* world, void* models, uint32_t count, uint64_t id) {
    alignas(16) float m[16];
    blendProp(world, m);
    ((ModelListFn)FN_MODEL_LIST)(list, m, models, count, id);
}

void installProps() {
    if (!optObjects) return;
    logf("prop model list %s", redirectCall(CALL_MODEL_LIST, FN_MODEL_LIST, (void*)modelListHook) ? "patched" : "NOT patched");
    int n = 0;
    for (int i = 0; i < 4; i++) {
        u8 orig[PROP_COPY_LEN];
        propCopyOrig(PROP_COPIES[i], i >= 2, orig);
        if (memcmp(PROP_COPIES[i], orig, PROP_COPY_LEN) != 0) continue;
        patchJump(PROP_COPIES[i], PROP_COPY_LEN, jumpTo(PROP_STUBS[i]));
        n++;
    }
    logf("props patched (%d of 4 sites)", n);
}
