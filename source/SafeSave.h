#pragma once

// Game bug: profiles and savegames are written in place (CreateFileA with CREATE_ALWAYS truncates the
// old file first). A crash, a forced quit or a power loss while writing leaves an empty or half-written
// file - the reported "all settings and campaign progress lost" (damaged profile).
// Fix ([Misc] SafeSave): files below Documents\S2 that the game opens for writing are written to
// "<name>.cptmp" and only replace the real file after CloseHandle, so the old version survives any
// interruption. Only the exe's own CreateFileA/CloseHandle imports are redirected.
namespace SafeSave {
    void init(const char* iniPath, bool supportedExe, const char* logPath);   // DllMain
    void onDeviceCreated();                                                   // exe unpacked: install
}
