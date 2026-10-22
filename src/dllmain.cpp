// True Draw FPS Unlock for NFS Hot Pursuit Remastered
// Target: NFS11Remastered.exe, Steam build, loaded at image base 0x140000000.
//
// How it works:
//   - The game updates its world (the "simulation") in fixed 1/60 s steps.
//   - pacer.cpp lets the game draw frames in between those steps.
//   - fpsoption.cpp caps those frames at the Max frame rate picked in the game's settings.
//   - blend/ draws everything at a position blended between the last two steps, and puts the real values back
//     afterwards, so the game's own logic never sees blended values.
//
// Every patch first checks that the game's original bytes are what we expect. On any other game build,
// nothing is patched.
#include "tdfps.h"

static DWORD WINAPI init(void*) {
    char ini[MAX_PATH];
    HMODULE self; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&init, &self);
    GetModuleFileNameA(self, ini, MAX_PATH);
    strcpy(strrchr(ini, '.'), ".ini");
    strcpy(logPath, ini);
    strcpy(strrchr(logPath, '.'), ".log");
    loadConfig(ini);
    if (logOn) if (FILE* f = fopen(logPath, "w")) fclose(f); // start a fresh log on every launch
    {
        char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
        u8* base = (u8*)GetModuleHandleA(nullptr);
        logf("TrueDrawFPS " TDFPS_VERSION " loading into %s (base %p, PE timestamp %08X; supported: Steam build 613F368C at 140000000)", exe, base,
             ((IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew))->FileHeader.TimeDateStamp);
    }

    // The game's code is encrypted on disk (EA's .ooa protection) and only readable once unpacked,
    // so wait up to 2 minutes for it.
    for (int i = 0; i < 1200 && !gameCodeReady(); i++) Sleep(100);
    if (!gameCodeReady()) { logf("unsupported game build (scheduler bytes differ after 120 s), nothing patched"); return 0; }
    installFrameLatency();
    if (!trampAlloc(0)) { logf("no memory near the game code for trampolines, nothing patched"); return 0; }
    if (!hookScheduler()) { logf("scheduler call NOT patched (bytes differ, another mod?), nothing patched"); return 0; }
    if (!hookRender()) { logf("render call NOT patched (bytes differ, another mod?), only the scheduler hook is in"); return 0; }
    installGates(gateMask);
    installReplay();
    installUI();
    installObjects();
    installEffects();
    installFpsOption();
    installWheels();
    installCharacters();
    installProps();
    installReadouts();
    installMapCamera();
    installCrashGuard();
    unlockSteps();
    if (optProbe) installProbes();
    logf("TrueDrawFPS " TDFPS_VERSION " started: camera=%d objects=%d wheels=%d ui=%d particles=%d lookahead=%.2f", optCamera, optObjects, optWheels, optUI, optParticles, lookahead);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) CloseHandle(CreateThread(nullptr, 0, init, nullptr, 0, nullptr));
    return TRUE;
}
