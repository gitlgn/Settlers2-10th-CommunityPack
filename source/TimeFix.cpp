#include "TimeFix.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace TimeFix {

    // ---- GOG build 11757 ----
    static DWORD* const IAT_GETTICKCOUNT = (DWORD*)0x006541E0;   // KERNEL32!GetTickCount import slot
    static void** const GAME_CLOCK_FN    = (void**)0x006BD760;   // ms clock selected at startup
    static DWORD* const GAME_CLOCK_PREV  = (DWORD*)0x006BD778;   // previous frame time (0 = none yet)
    static const DWORD QPC_CLOCK = 0x004473A0;                   // QueryPerformanceCounter / (freq / 1000)
    static const DWORD TICK_CLOCK = 0x00447390;                  // jmp [GetTickCount]
    static const BYTE QPC_CLOCK_BYTES[] = { 0x83, 0xEC, 0x08, 0x8D, 0x04, 0x24, 0x50, 0xFF, 0x15, 0xCC, 0x41, 0x65, 0x00 };

    static const DWORD START_MS = 1000;   // the clocks start at 1 s (0 means "not set" for the frame clock)

    static bool g_enabled = false, g_pending = false;
    static DWORD g_tick0 = 0;
    static LONGLONG g_qpc0 = 0, g_qpcPerMs = 0;
    static std::string g_log;

    static void logf(const char* fmt, ...) {
        if (g_log.empty()) return;
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        FILE* f = nullptr;
        if (fopen_s(&f, g_log.c_str(), "a") == 0 && f) { fprintf(f, "%s\n", buf); fclose(f); }
    }

    // replacement for the exe's GetTickCount import
    static DWORD WINAPI relativeTickCount() {
        return GetTickCount() - g_tick0 + START_MS;
    }

    // replacement for the frame clock (no arguments, result in eax)
    static DWORD __cdecl relativeGameMs() {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return (DWORD)((now.QuadPart - g_qpc0) / g_qpcPerMs) + START_MS;
    }

    void init(const char* iniPath, bool supportedExe, const char* logPath) {
        g_log = logPath ? logPath : "";
        if (logPath) DeleteFileA(logPath);
        char v[16];
        GetPrivateProfileStringA("Misc", "TimeFix", "on", v, sizeof(v), iniPath);
        g_enabled = supportedExe && _stricmp(v, "off") != 0 && _stricmp(v, "false") != 0 && strcmp(v, "0") != 0;
        if (!g_enabled) return;
        g_tick0 = GetTickCount();
        LARGE_INTEGER f, c;
        if (QueryPerformanceFrequency(&f) && f.QuadPart >= 1000 && QueryPerformanceCounter(&c)) {
            g_qpcPerMs = f.QuadPart / 1000;
            g_qpc0 = c.QuadPart;
        }
        g_pending = true;
    }

    static bool writeDword(void* at, DWORD value) {
        DWORD old;
        if (!VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
        *(DWORD*)at = value;
        VirtualProtect(at, 4, old, &old);
        return true;
    }

    void onDeviceCreated() {
        if (!g_pending) return;
        g_pending = false;

        // GetTickCount import: only if it still points to the real function
        FARPROC real = GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetTickCount");
        if (*IAT_GETTICKCOUNT == (DWORD)real && writeDword(IAT_GETTICKCOUNT, (DWORD)&relativeTickCount))
            logf("GetTickCount import: counts from game start");
        else
            logf("GetTickCount import not replaced (slot %08lX, kernel32 %p)", *IAT_GETTICKCOUNT, real);

        // frame clock: the QPC variant (normal case) is replaced; the GetTickCount variant is covered above
        void* clock = *GAME_CLOCK_FN;
        DWORD prev = *GAME_CLOCK_PREV;
        if (clock == (void*)QPC_CLOCK && g_qpcPerMs && memcmp((void*)QPC_CLOCK, QPC_CLOCK_BYTES, sizeof(QPC_CLOCK_BYTES)) == 0) {
            *GAME_CLOCK_FN = (void*)&relativeGameMs;
            logf("frame clock: counts from game start");
        } else if (clock == (void*)TICK_CLOCK) {
            logf("frame clock uses GetTickCount (covered by the import)");
        } else {
            logf("frame clock not recognised (%p) - left unchanged", clock);
        }
        // a frame time taken with the old clock would give one huge step: start the frame timing over
        if (prev) {
            *GAME_CLOCK_PREV = 0;
            logf("frame timing restarted (previous value %lu ms)", prev);
        }
    }
}
