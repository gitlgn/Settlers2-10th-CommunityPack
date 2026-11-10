#pragma once
#include <windows.h>

// Diagnostics for multiplayer problems ([Misc] DebugMode = on, GOG 11757 only):
// a ring buffer of the last received network messages (NComm::TinCatNetwork::cb_Received_Data, 0x421560:
// connection, user, channel, message type, size, first bytes) and of mouse clicks (HUD window position),
// plus per-type statistics. Written to <game>\bin\nettrace.log on a crash and at the end of the session.
// Read-only: the game's behaviour is not changed.
namespace NetTrace {
    void init(bool enabled, const char* logPath);
    void onDeviceCreated(HWND window);
    void onPresent();                     // samples mouse buttons once per frame
    void dump(const char* reason);        // crash handler / process exit
}
