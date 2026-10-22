// Debug helper, not part of the mod: loads RenderDoc into the game at startup (the game is started through
// launchers, so RenderDoc cannot launch it itself). The ASI loader runs before the D3D11 device exists, so
// RenderDoc hooks it. Capture with F12 / Print Screen; captures go to %TEMP%\RenderDoc. Delete after use.
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) LoadLibraryA("C:\\Program Files\\RenderDoc\\renderdoc.dll");
    return TRUE;
}
