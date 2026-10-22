// ---- Crash report (written when Log=1) ----
// Adds the first 4 crashes of a session to TrueDrawFPS.log, with:
//   - the crash address (game addresses as they are, other modules as module+offset)
//   - the thread, frame and simulation step
//   - the CPU registers
//   - values on the stack that point into the game or the plugin (likely return addresses)
//
// This only reports; the game's own error handling runs afterwards as normal. It sees each fault first, so a
// fault the game recovers from on its own is logged too.
// The report is built in a stack buffer and written with WriteFile, because the crash may be inside the heap.
#include "tdfps.h"

thread_local int quietFaults;
static volatile long reports;
static const long MAX_REPORTS = 4;

static bool imageRange(HMODULE m, uintptr_t* lo, uintptr_t* hi) {
    if (!m) return false;
    u8* b = (u8*)m;
    *lo = (uintptr_t)b;
    *hi = *lo + ((IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew))->OptionalHeader.SizeOfImage;
    return true;
}

static void describe(char* out, size_t n, uintptr_t a) {
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
        GetModuleFileNameA(m, name, sizeof name);
        const char* s = strrchr(name, '\\');
        // Game addresses are printed as is, so they match the addresses used in this source code.
        if (m == GetModuleHandleA(nullptr)) { snprintf(out, n, "%llX", (unsigned long long)a); return; }
        snprintf(out, n, "%s+%llX", s ? s + 1 : name, (unsigned long long)(a - (uintptr_t)m));
    } else snprintf(out, n, "%llX", (unsigned long long)a);
}

static LONG CALLBACK onFault(EXCEPTION_POINTERS* x) {
    DWORD code = x->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_IN_PAGE_ERROR) return EXCEPTION_CONTINUE_SEARCH;
    // Skip faults this thread expects, and stop after the first few reports.
    if (quietFaults || !logOn || InterlockedIncrement(&reports) > MAX_REPORTS) return EXCEPTION_CONTINUE_SEARCH;
    char buf[6144], where[MAX_PATH + 32];
    int n = 0;
    auto add = [&](const char* fmt, auto... a) {
        int k = snprintf(buf + n, sizeof buf - n, fmt, a...);
        if (k > 0) n = n + k < (int)sizeof buf ? n + k : (int)sizeof buf - 1;
    };
    const CONTEXT* c = x->ContextRecord;
    describe(where, sizeof where, (uintptr_t)x->ExceptionRecord->ExceptionAddress);
    SYSTEMTIME st; GetLocalTime(&st);
    add("fault %08lX at %s (%02d:%02d:%02d, first-chance: if the game kept running it handled this itself)\n", code, where,
        st.wHour, st.wMinute, st.wSecond);
    if (code == EXCEPTION_ACCESS_VIOLATION && x->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR k = x->ExceptionRecord->ExceptionInformation[0];
        add("  access violation: %s of %llX\n", k == 0 ? "read" : k == 1 ? "write" : k == 8 ? "execute" : "access",
            (unsigned long long)x->ExceptionRecord->ExceptionInformation[1]);
    }
    DWORD tid = GetCurrentThreadId();
    add("  thread %lu (%s), frame %u, step %.0f, fps choice %d\n", tid,
        tid != renderThread ? "not the render thread" : inRender ? "render thread, inside Render" : "render thread, outside Render",
        frameNo, stepCount, targetFps);
    add("  rax %016llX rbx %016llX rcx %016llX rdx %016llX\n", c->Rax, c->Rbx, c->Rcx, c->Rdx);
    add("  rsi %016llX rdi %016llX rbp %016llX rsp %016llX\n", c->Rsi, c->Rdi, c->Rbp, c->Rsp);
    add("  r8  %016llX r9  %016llX r10 %016llX r11 %016llX\n", c->R8, c->R9, c->R10, c->R11);
    add("  r12 %016llX r13 %016llX r14 %016llX r15 %016llX\n", c->R12, c->R13, c->R14, c->R15);
    add("  stack scan (code addresses at rsp+):\n");
    uintptr_t glo = 0, ghi = 0, plo = 0, phi = 0;
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&onFault, &self);
    imageRange(GetModuleHandleA(nullptr), &glo, &ghi);
    imageRange(self, &plo, &phi);
    // Walk up the stack (at most 512 values, stopping at the stack's end) and list up to 24 code addresses.
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    const uintptr_t* sp = (const uintptr_t*)c->Rsp;
    for (int i = 0, found = 0; i < 512 && found < 24 && (uintptr_t)(sp + i + 1) <= (uintptr_t)tib->StackBase; i++) {
        uintptr_t v = sp[i];
        if ((v > glo && v < ghi) || (v > plo && v < phi)) { describe(where, sizeof where, v); add("    +%03X %s\n", i * 8, where); found++; }
    }
    HANDLE f = CreateFileA(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, buf, (DWORD)n, &w, nullptr); CloseHandle(f); }
    return EXCEPTION_CONTINUE_SEARCH;
}

void installCrashGuard() {
    if (AddVectoredExceptionHandler(0, onFault)) logf("crash report on");
}
