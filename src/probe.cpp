// ---- Probe mode (Probe=1, debug only) ----
// Counts where each candidate read of an object's position runs. For every site there are 3 counters:
//   0 = render thread, outside Render (gameplay / update code)
//   1 = render thread, inside Render
//   2 = any other thread
// A site that only ever runs inside Render is safe to redirect to the blended matrix.
#include "tdfps.h"

struct ProbeSite { u8* at; int len; u8 orig[15]; const char* text; };
static const ProbeSite PROBES[] = {
#include "probe_sites.inc"
};
static const int NPROBES = sizeof PROBES / sizeof PROBES[0];
static uint64_t probeCounts[NPROBES][3];
bool probing;

void installProbes() {
    u8* page = allocNear(0x14038BEC0, NPROBES * 96); // each stub is at most 96 bytes
    if (!page) return;
    u8* p = page;
    for (int i = 0; i < NPROBES; i++) {
        const ProbeSite& s = PROBES[i];
        if (memcmp(s.at, s.orig, s.len) != 0) { logf("probe %d: bytes differ, skipped", i); continue; }
        // Stub: work out which counter applies (0, 1 or 2), add 1 to it, run the original instruction, jump back.
        u8* stub = p;
        auto b = [&](std::initializer_list<u8> v) { for (u8 x : v) *p++ = x; };
        auto q = [&](const void* v) { memcpy(p, &v, 8); p += 8; };
        b({0x50, 0x51, 0x9C});                                   // push rax; push rcx; pushfq
        b({0x48, 0xB8}); q(&renderThread);                       // mov rax, &renderThread
        b({0x8B, 0x08});                                         // mov ecx, [rax]
        b({0x65, 0x8B, 0x04, 0x25, 0x48, 0, 0, 0});              // mov eax, gs:[48h] (thread id)
        b({0x39, 0xC8});                                         // cmp eax, ecx
        b({0xB9, 2, 0, 0, 0});                                   // mov ecx, 2
        b({0x75, 0x0D});                                         // jne +13
        b({0x48, 0xB8}); q(&inRender);                           // mov rax, &inRender
        b({0x0F, 0xB6, 0x08});                                   // movzx ecx, byte [rax]
        b({0x48, 0xB8}); q(probeCounts[i]);                      // mov rax, &probeCounts[i]
        b({0x48, 0xFF, 0x04, 0xC8});                             // inc qword [rax+rcx*8]
        b({0x9D, 0x59, 0x58});                                   // popfq; pop rcx; pop rax
        memcpy(p, s.orig, s.len); p += s.len;                    // original instruction (no rip-relative)
        *p++ = 0xE9; *(int32_t*)p = (int32_t)((s.at + s.len) - (p + 4)); p += 4; // jmp back
        u8 jmp[15];
        memset(jmp, 0x90, sizeof jmp);
        jmp[0] = 0xE9; *(int32_t*)(jmp + 1) = (int32_t)(stub - (s.at + 5));
        write(s.at, jmp, s.len);
    }
    probing = true;
    logf("probes installed: %d sites", NPROBES);
}

void logProbes() {
    for (int i = 0; i < NPROBES; i++) {
        uint64_t* c = probeCounts[i];
        logf("  probe %2d %p update=%llu render=%llu other=%llu  %s", i, PROBES[i].at, c[0], c[1], c[2], PROBES[i].text);
        c[0] = c[1] = c[2] = 0;
    }
}
