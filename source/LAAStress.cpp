#include "LAAStress.h"

#include <windows.h>
#include <psapi.h>
#include <cstdarg>
#include <cstdio>
#include <string>
#pragma comment(lib, "psapi.lib")

namespace LAAStress {

    static std::string g_log;
    static int g_keepMB = 0, g_delay = 120;

    static void logf(const char* fmt, ...) {
        FILE* f = nullptr;
        if (fopen_s(&f, g_log.c_str(), "a") != 0 || !f) return;
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "%02d:%02d:%02d  ", t.wHour, t.wMinute, t.wSecond);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }

    static const ULONG_PTR LIMIT = 0x80000000;   // 2 GB

    // committed memory below / above 2 GB and the highest committed address
    static void usage(SIZE_T& low, SIZE_T& high, ULONG_PTR& top) {
        low = high = 0;
        top = 0;
        MEMORY_BASIC_INFORMATION mbi;
        ULONG_PTR a = 0x10000;
        while (VirtualQuery((void*)a, &mbi, sizeof(mbi)) == sizeof(mbi)) {
            ULONG_PTR base = (ULONG_PTR)mbi.BaseAddress, end = base + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT) {
                if (base >= LIMIT) high += mbi.RegionSize;
                else if (end <= LIMIT) low += mbi.RegionSize;
                else { low += LIMIT - base; high += end - LIMIT; }
                if (end > top) top = end;
            }
            if (end <= a) break;
            a = end;
        }
    }

    // Reserve (without committing) the free address space below 2 GB, except the lowest <g_keepMB>.
    static void reserveLow() {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        const ULONG_PTR gran = si.dwAllocationGranularity;
        const ULONG_PTR keep = (ULONG_PTR)g_keepMB << 20;
        SIZE_T reserved = 0, blocks = 0, keptFree = 0;
        MEMORY_BASIC_INFORMATION mbi;
        ULONG_PTR a = 0x10000;
        while (a < LIMIT && VirtualQuery((void*)a, &mbi, sizeof(mbi)) == sizeof(mbi)) {
            ULONG_PTR base = (ULONG_PTR)mbi.BaseAddress, end = base + mbi.RegionSize;
            if (end > LIMIT) end = LIMIT;
            if (mbi.State == MEM_FREE) {
                ULONG_PTR start = (base + gran - 1) & ~(gran - 1);
                if (keptFree < keep && end > start) {
                    ULONG_PTR take = (keep - keptFree + gran - 1) & ~(gran - 1);
                    if (take > end - start) take = end - start;
                    keptFree += take;
                    start += take;
                }
                if (end > start && VirtualAlloc((void*)start, end - start, MEM_RESERVE, PAGE_NOACCESS)) {
                    reserved += end - start;
                    blocks++;
                }
            }
            if ((ULONG_PTR)mbi.BaseAddress + mbi.RegionSize <= a) break;
            a = (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
        }
        logf("blocked %u MB below 2 GB in %u blocks (%u MB left free) - new allocations now go above 2 GB",
            (unsigned)(reserved >> 20), (unsigned)blocks, (unsigned)(keptFree >> 20));
    }

    static DWORD WINAPI monitor(LPVOID) {
        // The game compiles its shaders at startup with d3dx9_29, which fails (E_OUTOFMEMORY) when its
        // memory lies above 2 GB. So the low address space is blocked only once the player is in a game.
        logf("waiting %d s before blocking the address space below 2 GB - load a savegame now", g_delay);
        Sleep((DWORD)g_delay * 1000);
        reserveLow();
        for (;;) {
            SIZE_T low, high;
            ULONG_PTR top;
            usage(low, high, top);
            PROCESS_MEMORY_COUNTERS pmc = { sizeof(pmc) };
            GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
            logf("committed: %u MB below 2 GB, %u MB above 2 GB (highest address 0x%08IX), private %u MB",
                (unsigned)(low >> 20), (unsigned)(high >> 20), top, (unsigned)(pmc.PagefileUsage >> 20));
            Sleep(30000);
        }
    }

    void install(const char* iniPath, const char* logPath) {
        char v[16];
        GetPrivateProfileStringA("Misc", "LAAStressTest", "off", v, sizeof(v), iniPath);
        if (_stricmp(v, "on") != 0 && _stricmp(v, "true") != 0 && strcmp(v, "1") != 0) return;

        g_log = logPath;
        DeleteFileA(logPath);

        SYSTEM_INFO si;
        GetSystemInfo(&si);
        logf("LAA stress test: highest user address 0x%08IX", (ULONG_PTR)si.lpMaximumApplicationAddress);
        if ((ULONG_PTR)si.lpMaximumApplicationAddress < LIMIT) {
            logf("S2DNG.exe is not large address aware - test skipped (run install.cmd)");
            return;
        }
        g_keepMB = GetPrivateProfileIntA("Misc", "LAAStressFreeMB", 0, iniPath);
        g_delay = GetPrivateProfileIntA("Misc", "LAAStressDelay", 120, iniPath);
        HANDLE th = CreateThread(nullptr, 0, monitor, nullptr, 0, nullptr);
        if (th) CloseHandle(th); else logf("monitor thread could not be started");
    }
}
