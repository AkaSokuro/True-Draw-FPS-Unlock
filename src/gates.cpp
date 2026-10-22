// ---- Update gates (UpdateGates setting) ----
// The game's Update function (14038C0B0) calls ~30 subsystems once per drawn frame. At 60 FPS that is once per
// simulation step, but at 240 FPS it would be 4 times per step.
//
// Each "gate" wraps one of those calls: on frames with no simulation step, a gated call returns straight away
// (if its bit in the mask is on). Skipping the whole Update function broke visuals, so calls that set up frame
// data, and calls whose return value is used, are left alone.
#include "tdfps.h"

struct GateSite { uintptr_t site, target; }; // call instruction address, and the function it calls
static const GateSite GATE_SITES[] = {
    {0x14038C126, 0x1402AF9E0}, {0x14038C142, 0x140453920}, {0x14038C157, 0x14014F800}, {0x14038C171, 0x14044F100},
    {0x14038C186, 0x1403DD390}, {0x14038C197, 0x140521540}, {0x14038C1AC, 0x1403F3340}, {0x14038C1C8, 0x14050DC10},
    {0x14038C1DD, 0x1404D45E0}, {0x14038C1F2, 0x140176B10}, {0x14038C207, 0x140507DB0}, {0x14038C24F, 0x1401D6B20},
    {0x14038C281, 0x1402045D0}, {0x14038C296, 0x1401E7830}, {0x14038C2AB, 0x14017F480}, {0x14038C2C0, 0x1404FF300},
    {0x14038C2D5, 0x14011D4F0}, {0x14038C2EA, 0x1401ACD20}, {0x14038C332, 0x140514180}, {0x14038C342, 0x140514180},
    {0x14038C349, 0x140514140},                                                         // calls before the step loop
    {0x14038C90F, 0x140453ED0}, {0x14038C950, 0x14030F240}, {0x14038C965, 0x140508320}, {0x14038C971, 0x14030F460},
    {0x14038C98C, 0x140514DC0}, {0x14038C998, 0x14044F410},                             // calls after it
};
static const int NGATES = sizeof GATE_SITES / sizeof GATE_SITES[0];
// Memory shared with the stubs:
//   byte 0           - 1 when this frame runs no simulation step
//   byte 1 + i       - 1 when gate i is on
//   byte 64 + 32 * i - the stub code for gate i
u8* gatePage;

uint32_t gatesOn() {
    uint32_t mask = 0;
    for (int i = 0; gatePage && i < NGATES; i++) mask |= (uint32_t)(gatePage[1 + i] & 1) << i;
    return mask;
}

// Writes the 31-byte stub for gate i. If this frame has no step and the gate is on, it returns 0 at once;
// otherwise it jumps to the original function.
static u8* buildGateStub(u8* page, int i, uintptr_t target) {
    u8* s = page + 64 + 32 * i;
    auto rel = [&](int at, u8* to) { *(int32_t*)(s + at) = (int32_t)(to - (s + at + 4)); };
    s[0] = 0x8A; s[1] = 0x05; rel(2, page);                // mov al, [zero-step flag]
    s[6] = 0x22; s[7] = 0x05; rel(8, page + 1 + i);        // and al, [gate i]
    s[12] = 0x75; s[13] = 0x0E;                             // jnz skip
    s[14] = 0xFF; s[15] = 0x25; *(int32_t*)(s + 16) = 0;    // jmp [rip+0] -> original
    *(uintptr_t*)(s + 20) = target;
    s[28] = 0x31; s[29] = 0xC0; s[30] = 0xC3;               // skip: xor eax, eax; ret
    return s;
}

int installGates(uint32_t mask) {
    if (!(gatePage = allocNear(0x14038C0B0))) return 0;
    int n = 0;
    for (int i = 0; i < NGATES; i++) {
        u8* site = (u8*)GATE_SITES[i].site;
        // Skip this site unless it is still the expected call (another mod may have changed it).
        if (site[0] != 0xE8 || (uintptr_t)(site + 5 + *(int32_t*)(site + 1)) != GATE_SITES[i].target) continue;
        gatePage[1 + i] = (mask >> i) & 1;
        u8* s = buildGateStub(gatePage, i, GATE_SITES[i].target);
        u8 call[5] = {0xE8};
        *(int32_t*)(call + 1) = (int32_t)(s - (site + 5));
        write(site, call, 5);
        n++;
    }
    logf("update gates: %d of %d installed, mask %08X", n, NGATES, mask);
    return n;
}
