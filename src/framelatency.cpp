// ---- GPU frame queue (FrameLatency setting) ----
// Limits how many frames the GPU driver may queue up ahead (the driver default is 3). Fewer queued frames means
// less input delay when the GPU is the bottleneck.
// A background thread checks the game's Direct3D device pointer (at 14139C8F8) and applies the limit to each new
// device it finds.
#include "tdfps.h"
#include <d3d11.h>

static ID3D11Device** const GAME_DEVICE = (ID3D11Device**)0x14139C8F8;

static DWORD WINAPI limitQueue(void*) {
    for (ID3D11Device* last = nullptr;; Sleep(1000)) {
        ID3D11Device* dev = *GAME_DEVICE;
        // Only trust the pointer once it points to a real d3d11.dll device. d3d11.dll may load after this thread
        // starts, so it is looked up every time.
        HMODULE owner = nullptr;
        if (!dev || dev == last || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                       *(LPCSTR*)dev, &owner) || owner != GetModuleHandleA("d3d11.dll")) continue;
        last = dev;
        IDXGIDevice1* d = nullptr;
        if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&d))) { logf("frame latency: no IDXGIDevice1"); continue; }
        HRESULT hr = d->SetMaximumFrameLatency(frameLatency);
        d->Release();
        logf("frame latency %d %s", frameLatency, SUCCEEDED(hr) ? "set" : "NOT set");
    }
}

void installFrameLatency() {
    if (frameLatency > 0) CloseHandle(CreateThread(nullptr, 0, limitQueue, nullptr, 0, nullptr));
}
