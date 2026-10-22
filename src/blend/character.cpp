// ---- Driver model ----
// Every frame, the game copies the driver's simulated matrix ([rbp+A0]) to the matrix that gets drawn ([rbp+10]).
// We replace that copy with char_stub.asm, which calls blendInstance to write a blended matrix instead.
#include "../tdfps.h"

static u8* const CHAR_COPY = (u8*)0x14017EC5D;
static const u8 CHAR_COPY_ORIG[] = {
    0x0F, 0x10, 0x85, 0xA0, 0x00, 0x00, 0x00, 0x0F, 0x11, 0x45, 0x10, 0x0F, 0x10, 0x8D, 0xB0, 0x00, 0x00, 0x00,
    0x0F, 0x11, 0x4D, 0x20, 0x0F, 0x10, 0x85, 0xC0, 0x00, 0x00, 0x00, 0x0F, 0x11, 0x45, 0x30, 0x0F, 0x10, 0x8D,
    0xD0, 0x00, 0x00, 0x00, 0x0F, 0x11, 0x4D, 0x40};
extern "C" void charStub();
extern "C" uintptr_t charReturn = 0x14017EC89;

void installCharacters() {
    if (!optObjects || memcmp(CHAR_COPY, CHAR_COPY_ORIG, sizeof CHAR_COPY_ORIG) != 0) return;
    patchJump(CHAR_COPY, sizeof CHAR_COPY_ORIG, jumpTo(charStub));
    logf("characters patched");
}
