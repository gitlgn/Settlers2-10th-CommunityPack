#pragma once
#include <d3d9.h>

// Engine tweaks applied once the game code is unpacked (at device creation), configured in [Graphics] of d3d9.ini.
namespace GfxPatches {
    void init(const char* iniPath, const char* logPath);
    void onDeviceCreated();

    // render-target diagnostics (only with [Misc] DebugMode = on)
    void onCreateTexture(UINT w, UINT h, DWORD usage, D3DFORMAT fmt, void* tex);
    void onSetTexture(DWORD stage, void* tex);
    void onPresent();
}
