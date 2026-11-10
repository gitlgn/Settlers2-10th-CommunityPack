#pragma once

// Game bug: the engine's clocks count from Windows start (GetTickCount, QueryPerformanceCounter in ms)
// and are converted to 32-bit floats for animation and timestamps. A float has 24 bits of precision,
// so the longer the PC runs (days with sleep instead of shutdown), the coarser these times get:
// 2 ms steps after 4.7 h, 64 ms after 10 days -> stuttering animations. GetTickCount() << 6 also
// wraps every 18.6 h. The fix makes the game's clocks count from game start instead ([Misc] TimeFix).
// Only the exe's own import and clock pointer are changed; DLLs and drivers keep the real clock.
namespace TimeFix {
    void init(const char* iniPath, bool supportedExe, const char* logPath);   // DllMain: read switch, remember the start time
    void onDeviceCreated();                               // exe is unpacked now: install
}
