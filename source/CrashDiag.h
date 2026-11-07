#pragma once
#include <windows.h>

// Diagnostic helper: records exceptions and process exits of the game in
// <gamedir>\bin\crashdiag.log (and a minidump on the first fatal exception).
namespace CrashDiag {
    void install(const char* logPath);
    void onProcessDetach(LPVOID lpReserved);
}
