// Helpers for patching the game's code: writing bytes, allocating memory near the game code for small jump stubs
// ("trampolines"), and redirecting calls, jumps and memory reads to our own code.
#include "tdfps.h"

void write(void* at, const void* data, size_t n) {
    DWORD old;
    VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, data, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
}

// Allocates memory that the game code can reach with a 32-bit relative jump (within +/- 2 GB).
u8* allocNear(uintptr_t target, size_t size) {
    // Every patched site lies between 140000000 and 141400000, so memory in
    // [141400000 - 2 GB, 140000000 + 2 GB] can be reached from all of them.
    // Search outward in both directions: on one user's PC, the whole 2 GB below the game was already in use.
    const uintptr_t lo = 0x141400000 - 0x7FF00000, hi = 0x140000000 + 0x7FF00000, t = target & ~0xFFFFull;
    DWORD err = 0;
    for (uintptr_t d = 0x10000; d < 0x80000000; d += 0x10000)
        for (uintptr_t a : {t - d, t + d})
            if (a >= lo && a + size <= hi) {
                if (void* p = VirtualAlloc((void*)a, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) return (u8*)p;
                err = GetLastError();
            }
    logf("allocNear %llX: no free memory within reach (last error %lu)", (unsigned long long)target, err);
    return nullptr;
}

// Hands out trampoline memory from a single 4 KB page near the game code.
// ponytail: one page is plenty (all hooks together use ~0.6 KB); add a second page if the hooks ever outgrow it.
u8* trampAlloc(size_t n) {
    static u8* page;
    static size_t used;
    if (!page && !(page = allocNear(0x14038BEC0))) return nullptr;
    n = (n + 15) & ~(size_t)15;
    if (used + n > 0x1000) { logf("trampoline page full"); return nullptr; }
    u8* p = page + used;
    used += n;
    return p;
}

u8* jumpTo(const void* target) {
    u8* t = trampAlloc(14);
    t[0] = 0xFF; t[1] = 0x25; *(int32_t*)(t + 2) = 0; *(const void**)(t + 6) = target;
    return t;
}

void patchJump(u8* site, size_t len, const void* to) {
    u8 jmp[64];
    memset(jmp, 0x90, len);
    jmp[0] = 0xE9; *(int32_t*)(jmp + 1) = (int32_t)((const u8*)to - (site + 5));
    write(site, jmp, len);
}

u8* relocate(u8* fn, size_t n) { // the n copied bytes must not contain rip-relative instructions
    u8* o = trampAlloc(n + 14);
    memcpy(o, fn, n);
    o[n] = 0xFF; o[n + 1] = 0x25; *(int32_t*)(o + n + 2) = 0; *(u8**)(o + n + 6) = fn + n;
    return o;
}

// Makes the call instruction at `site` call `hook` instead. Does nothing and returns false unless the site is
// still a call to `expectTarget`.
bool redirectCall(u8* site, uintptr_t expectTarget, void* hook) {
    if (site[0] != 0xE8 || (uintptr_t)(site + 5 + *(int32_t*)(site + 1)) != expectTarget) return false;
    u8* tramp = trampAlloc(12);
    tramp[0] = 0x48; tramp[1] = 0xB8; *(void**)(tramp + 2) = hook; tramp[10] = 0xFF; tramp[11] = 0xE0; // mov rax,hook; jmp rax
    u8 call[5] = {0xE8};
    *(int32_t*)(call + 1) = (int32_t)(tramp - (site + 5));
    write(site, call, 5);
    return true;
}

// Makes the memory read instruction at `at` read from a different offset (newDisp), so drawing code reads our
// blended data instead of the game's. The instruction is `len` bytes and ends in its 32-bit offset.
// The redirect only applies on the render thread while Render runs. Anywhere else (gameplay, physics) the original
// instruction runs unchanged, and the call is counted in offRender[idx].
// The stub (at most 96 bytes) is written at p, and p is moved past it.
void redirectRead(u8*& p, int idx, u8* at, int len, const u8* orig, uint32_t newDisp) {
    u8* stub = p;
    auto b = [&](std::initializer_list<u8> v) { for (u8 x : v) *p++ = x; };
    auto q = [&](const volatile void* v) { memcpy(p, &v, 8); p += 8; };
    auto back = [&] { *p++ = 0xE9; *(int32_t*)p = (int32_t)((at + len) - (p + 4)); p += 4; };
    b({0x50, 0x51, 0x9C});                            // push rax; push rcx; pushfq
    b({0x48, 0xB8}); q(&renderThread);                // mov rax, &renderThread
    b({0x8B, 0x08});                                  // mov ecx, [rax]
    b({0x65, 0x8B, 0x04, 0x25, 0x48, 0, 0, 0});       // mov eax, gs:[48h] (thread id)
    b({0x39, 0xC8, 0x75, 0}); u8* j1 = p - 1;         // cmp eax, ecx; jne other
    b({0x48, 0xB8}); q(&inRender);                    // mov rax, &inRender
    b({0x80, 0x38, 0x00, 0x74, 0}); u8* j2 = p - 1;   // cmp byte [rax], 0; je other
    b({0x9D, 0x59, 0x58});                            // popfq; pop rcx; pop rax
    memcpy(p, orig, len); memcpy(p + len - 4, &newDisp, 4); p += len; back(); // redirected read
    *j1 = (u8)(p - (j1 + 1)); *j2 = (u8)(p - (j2 + 1));
    b({0x48, 0xB8}); q(&offRender[idx]);              // other: mov rax, &offRender[idx]
    b({0xF0, 0x48, 0xFF, 0x00});                      // lock inc qword [rax]
    b({0x9D, 0x59, 0x58});                            // popfq; pop rcx; pop rax
    memcpy(p, orig, len); p += len; back();           // original read
    u8 jmp[15];
    memset(jmp, 0x90, sizeof jmp);
    jmp[0] = 0xE9; *(int32_t*)(jmp + 1) = (int32_t)(stub - (at + 5));
    write(at, jmp, len);
    redirectAt[idx] = at;
}
