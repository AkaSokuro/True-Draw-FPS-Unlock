// Adds choices above 60 to the game's "Max frame rate" option. Physics still runs at 60 Hz.
//
// Patched in the game:
//   - the number of choices (at 1404330A0)
//   - the table of label text IDs (lea at 140336907); our new IDs are answered by a hooked string lookup
//   - the frame tick's 30/60 FPS mode check (14038BDDC, je -> jae), so every choice of 60 or more uses 60 FPS mode
#include "tdfps.h"

static const int FPS_CHOICES[] = {30, 60, 120, 144, 165, 240, 0}; // 0 = unlimited
static const wchar_t* const FPS_LABELS[] = {nullptr, nullptr, L"120", L"144", L"165", L"240", L"UNLIMITED"};
static const int NCHOICES = sizeof FPS_CHOICES / sizeof FPS_CHOICES[0];
static const uint32_t FPS_LABEL_IDS[2] = {0x12C1B8, 0x12C1B9};  // the game's own text IDs for "30" and "60"
static const uint32_t CUSTOM_ID = 0x7FFF0000;                   // our text IDs are CUSTOM_ID + choice index
static u8* const FPS_ITEM_OFFSET = (u8*)0x13F500;
static const uintptr_t FPS_ITEM_VTABLE = 0x140F12848;
static u8* const FPS_COUNT = (u8*)0x1404330A0;
static u8* const FPS_LABEL_LEA = (u8*)0x140336907;
static const u8 FPS_LABEL_LEA_ORIG[] = {0x48, 0x8D, 0x0D, 0x32, 0x3F, 0xB0, 0x00};
static void** const STRING_LOOKUP_SLOT = (void**)0x140F118C8;
static const uintptr_t FN_STRING_LOOKUP = 0x1403EEC00;
static u8* const FPS_MODE_JE = (u8*)0x14038BDDC;

struct LocString { const wchar_t* text; uint64_t length; }; // length includes the terminating zero
static LocString customStrings[NCHOICES];
typedef const LocString*(__fastcall* LookupFn)(void* table, uint32_t id);
static LookupFn lookupOrig = (LookupFn)FN_STRING_LOOKUP;
static bool fpsMenu;   // true once the option is patched and has choices above 60

// Returns our label text for our own IDs, and passes every other ID to the game.
extern "C" const LocString* stringLookupHook(void* table, uint32_t id) {
    uint32_t i = id - CUSTOM_ID;
    if (i < (uint32_t)NCHOICES && customStrings[i].text) return &customStrings[i];
    return lookupOrig(table, id);
}

static u8* gameSettings() { // follows the pointer chain [[[14129C498]+8]+C80]
    u8* owner = *(u8**)0x14129C498;
    u8* root = owner ? *(u8**)(owner + 8) : nullptr;
    return root ? *(u8**)(root + 0xC80) : nullptr;
}

// Called every frame: keeps the menu item's range correct, and reads the current choice into targetFps.
void updateFpsChoice(u8* game) {
    if (fpsMenu) { // the game sets the menu item's maximum once at startup, from the original 2 choices, so fix it
        u8* item = game + (uintptr_t)FPS_ITEM_OFFSET;
        if (*(uintptr_t*)item == FPS_ITEM_VTABLE && *(int*)(item + 0x1C) != NCHOICES - 1) *(int*)(item + 0x1C) = NCHOICES - 1;
    }
    if (u8* st = gameSettings()) {
        int c = *(int*)(st + 0x20C);
        targetFps = FPS_CHOICES[c < 0 ? 0 : c >= NCHOICES ? NCHOICES - 1 : c];
    }
}

void installFpsOption() {
    if (FPS_COUNT[0] == 0xB8 && *(int32_t*)(FPS_COUNT + 1) == 2 && FPS_MODE_JE[0] == 0x74 &&
        memcmp(FPS_LABEL_LEA, FPS_LABEL_LEA_ORIG, sizeof FPS_LABEL_LEA_ORIG) == 0 && *STRING_LOOKUP_SLOT == (void*)FN_STRING_LOOKUP) {
        // Our own label ID table, placed close enough for the game's lea instruction to reach it.
        uint32_t* labels = (uint32_t*)trampAlloc(4 * NCHOICES);
        for (int i = 0; i < NCHOICES; i++) {
            labels[i] = i < 2 ? FPS_LABEL_IDS[i] : CUSTOM_ID + i;
            if (FPS_LABELS[i]) customStrings[i] = {FPS_LABELS[i], wcslen(FPS_LABELS[i]) + 1};
        }
        int32_t disp = (int32_t)((u8*)labels - (FPS_LABEL_LEA + sizeof FPS_LABEL_LEA_ORIG));
        write(FPS_LABEL_LEA + 3, &disp, 4);
        void* hook = (void*)stringLookupHook;
        write(STRING_LOOKUP_SLOT, &hook, 8);
        int32_t n = NCHOICES;
        write(FPS_COUNT + 1, &n, 4);
        u8 jae = 0x73;
        write(FPS_MODE_JE, &jae, 1);
        fpsMenu = true;
        logf("in-game frame rate option patched (%d choices)", NCHOICES);
    } else logf("in-game frame rate option NOT patched (bytes differ)");
}
