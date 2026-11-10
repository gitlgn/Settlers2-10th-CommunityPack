#include "BinaryFixes.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace BinaryFixes {

    struct Patch {
        const char* name;
        DWORD addr;
        BYTE before[8];
        BYTE after[8];
        int len;
    };

    // all patches of one fix are applied together, or none of them
    struct Fix {
        const char* name;
        Patch parts[4];
        int count;
    };

    static const Fix FIXES[] = {
        {
            "network buffer append: drop data when realloc fails (0x412C50)",
            {
                // 00412CCA  int3 x4  ->  xor ebp, ebp ; jmp 0x412CB3 (epilogue: counters += 0, pop, ret 8)
                { "exit stub", 0x00412CCA, { 0xCC, 0xCC, 0xCC, 0xCC }, { 0x31, 0xED, 0xEB, 0xE5 }, 4 },
                // 00412C8F  je 0x412C97 (copy anyway)  ->  je 0x412CCA (exit stub)
                { "branch", 0x00412C8F, { 0x74, 0x06 }, { 0x74, 0x39 }, 2 },
            },
            2
        },
    };

    static bool g_pending = false;
    static std::string g_log;

    static void logf(const char* fmt, const char* a, const char* b = "") {
        if (g_log.empty()) return;
        FILE* f = nullptr;
        if (fopen_s(&f, g_log.c_str(), "a") == 0 && f) { fprintf(f, fmt, a, b); fputc('\n', f); fclose(f); }
    }

    void init(const char* iniPath, bool supportedExe, const char* logPath) {
        g_log = logPath ? logPath : "";
        if (logPath) DeleteFileA(logPath);
        char v[16];
        GetPrivateProfileStringA("Misc", "GameFixes", "on", v, sizeof(v), iniPath);
        bool on = _stricmp(v, "off") != 0 && _stricmp(v, "false") != 0 && strcmp(v, "0") != 0;
        g_pending = supportedExe && on;
    }

    static bool write(DWORD addr, const BYTE* bytes, int len) {
        DWORD old;
        if (!VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy((void*)addr, bytes, len);
        VirtualProtect((void*)addr, len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
        return true;
    }

    void onDeviceCreated() {
        if (!g_pending) return;
        g_pending = false;
        for (const Fix& fx : FIXES) {
            bool match = true;
            for (int i = 0; i < fx.count; i++)
                if (memcmp((void*)fx.parts[i].addr, fx.parts[i].before, fx.parts[i].len) != 0) { match = false; break; }
            if (!match) { logf("skipped (code differs): %s", fx.name); continue; }
            bool ok = true;
            for (int i = 0; i < fx.count; i++) ok = write(fx.parts[i].addr, fx.parts[i].after, fx.parts[i].len) && ok;
            logf(ok ? "applied: %s" : "FAILED: %s", fx.name);
        }
    }
}
