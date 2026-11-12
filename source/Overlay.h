#pragma once

#include <d3d9.h>

// In-game settings menu for the community pack ([UI] Menu = on, key [UI] MenuKey, default F10).
// Dear ImGui drawn into the real back buffer right before Present. While the menu is open the game gets no keyboard
// or mouse input (window messages filtered, GetAsyncKeyState / GetCursorPos of the exe redirected).
// Settings are written to bin\d3d9.ini; most of them apply on the next game start, which the menu says.
namespace Overlay {
    void init(const char* iniPath, const char* logPath);
    void onDeviceCreated(IDirect3DDevice9* realDevice, HWND window);
    void beforeReset();
    void afterReset(IDirect3DDevice9* realDevice);
    void onPresent(IDirect3DDevice9* realDevice);   // after the supersampling blit, before the real Present
}
