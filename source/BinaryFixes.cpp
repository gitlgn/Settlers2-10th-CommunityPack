#include "BinaryFixes.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <climits>
#include <string>

#include "NetTrace.h"

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
        {
            "game command 0x2002F: index of the per-building value table checked (0..4) before writing (0x610390)",
            {
                // 00610402  int3 x12  ->  mov ecx,[esi+1C] ; mov [eax+edx*4+3C8],ecx ; jmp 0x6103F0
                { "write stub", 0x00610402, { 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC }, { 0x8B, 0x4E, 0x1C, 0x89, 0x8C, 0x90, 0xC8, 0x03 }, 8 },
                { "write stub 2", 0x0061040A, { 0xCC, 0xCC, 0xCC, 0xCC }, { 0x00, 0x00, 0xEB, 0xE2 }, 4 },
                // 006103E3  mov edx,[esi+18] ; mov ecx,[esi+1C] ; mov [eax+edx*4+3C8],ecx
                //       ->  mov edx,[esi+18] ; cmp edx,4 ; ja 0x6103F0 (skip) ; jmp 0x610402 (write stub)
                { "check", 0x006103E3, { 0x8B, 0x56, 0x18, 0x8B, 0x4E, 0x1C, 0x89, 0x8C }, { 0x8B, 0x56, 0x18, 0x83, 0xFA, 0x04, 0x77, 0x05 }, 8 },
                { "check 2", 0x006103EB, { 0x90, 0xC8, 0x03, 0x00, 0x00 }, { 0xEB, 0x15, 0xCC, 0xCC, 0xCC }, 5 },
            },
            4
        },
        {
            "stream read: size check without signed overflow (0x412CD0, savegames/maps/network)",
            {
                // 00412D14  int3 x12  ->  mov ecx,edi ; add ecx,eax ; jc fail ; cmp ecx,esi ; ja fail ; jmp 0x412CE5
                { "check stub", 0x00412D14, { 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC }, { 0x8B, 0xCF, 0x03, 0xC8, 0x72, 0xF3, 0x3B, 0xCE }, 8 },
                { "check stub 2", 0x00412D1C, { 0xCC, 0xCC, 0xCC, 0xCC }, { 0x77, 0xEF, 0xEB, 0xC5 }, 4 },
                // 00412CDE  lea ecx,[edi+eax] ; cmp ecx,esi ; jg fail   (pos+size signed: a huge size passed)
                //       ->  jmp 0x412D14 (check stub)
                { "check", 0x00412CDE, { 0x8D, 0x0C, 0x07, 0x3B, 0xCE, 0x7F, 0x28 }, { 0xEB, 0x34, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC }, 7 },
            },
            3
        },
    };

    // ---- stream string read (thunk 0x412D80 -> 0x6D7E8C): 16-bit length from the data, signed, never checked
    // against the remaining data -> reads past the buffer; a negative length becomes ~4 GB. The length in the
    // stream is clamped to 0 .. remaining bytes before the original runs.
    struct MemStream { void* vtbl; BYTE* data; int len; int pos; };
    static const DWORD STRREAD_THUNK = 0x00412D80;
    static const BYTE STRREAD_BYTES[] = { 0xE9, 0x07, 0x51, 0x2C, 0x00 };   // jmp 0x6D7E8C
    static const DWORD STRREAD_REAL = 0x006D7E8C;
    static volatile LONG g_clamped = 0;

    static void __cdecl clampStringLength(MemStream* s) {
        __try {
            if (!s || !s->data || s->pos < 0 || s->pos + 2 > s->len) return;   // original reads length 0 then
            short* p = (short*)(s->data + s->pos);
            int rest = s->len - s->pos - 2;
            if (*p < 0) { *p = 0; InterlockedIncrement(&g_clamped); }
            else if (*p > rest) { *p = (short)rest; InterlockedIncrement(&g_clamped); }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void __declspec(naked) stringReadHook() {
        __asm {
            pushad
            pushfd
            push ecx
            call clampStringLength
            add esp, 4
            popfd
            popad
            push STRREAD_REAL
            ret
        }
    }

    // ---- file stream string read (0x448B10, vtable of the file class): char buffer[0x800] on the stack,
    // length read from the file (vcall +0x30) and passed unchecked to read (ReadFile) and buffer[len] = 0
    // -> stack buffer overflow with a damaged/crafted map or savegame. Length clamped to 0..0x7FF.
    static const DWORD FSTR_AT = 0x00448B39;                   // mov eax,[esp+4] ; mov edx,[esi]  (6 bytes)
    static const BYTE FSTR_BYTES[] = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x16 };
    static const DWORD FSTR_BACK = 0x00448B3F;                 // push eax ; lea ecx,[esp+0C] ; ...

    static bool applyFileStringClamp() {
        if (memcmp((void*)FSTR_AT, FSTR_BYTES, sizeof(FSTR_BYTES)) != 0) return false;
        BYTE* s = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!s) return false;
        BYTE code[27] = {
            0x8B, 0x44, 0x24, 0x04,                            // mov eax, [esp+4]      (length)
            0x3D, 0xFF, 0x07, 0x00, 0x00,                      // cmp eax, 0x7FF
            0x76, 0x09,                                        // jbe ok (unsigned: negatives are too large)
            0xB8, 0xFF, 0x07, 0x00, 0x00,                      // mov eax, 0x7FF
            0x89, 0x44, 0x24, 0x04,                            // mov [esp+4], eax      (used for buffer[len] = 0)
            0x8B, 0x16,                                        // ok: mov edx, [esi]
            0xE9, 0, 0, 0, 0,                                  // jmp FSTR_BACK
        };
        *(DWORD*)(code + 23) = FSTR_BACK - ((DWORD)s + 27);
        memcpy(s, code, sizeof(code));
        BYTE jmp[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
        *(DWORD*)(jmp + 1) = (DWORD)s - (FSTR_AT + 5);
        DWORD old;
        if (!VirtualProtect((void*)FSTR_AT, 6, PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy((void*)FSTR_AT, jmp, 6);
        VirtualProtect((void*)FSTR_AT, 6, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)FSTR_AT, 6);
        return true;
    }

    // ---- game load (0x44A440, called by nGame::File::load): a damaged savegame/map makes the loader read
    // garbage counts and throw a C++ exception ("vector<T> too long"), which nobody catches -> the game quits.
    // The load runs inside an exception frame. The loader has already torn down the previous game state when it
    // fails, so continuing would crash on the half-loaded state (tested): a clear message is shown and the game
    // ends cleanly instead. Access violations are NOT caught.
    static const DWORD LOAD_AT = 0x0044A440;                    // push -1 ; push 0x6264B8
    static const BYTE LOAD_BYTES[] = { 0x6A, 0xFF, 0x68, 0xB8, 0x64, 0x62, 0x00 };
    typedef char(__cdecl* Load_t)(DWORD, DWORD, char);
    static Load_t g_loadOriginal = nullptr;
    static volatile LONG g_loadAborted = 0;

    static int loadFilter(DWORD code) { return code == 0xE06D7363 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH; }

    static char __cdecl safeLoad(DWORD type, DWORD path, char flag) {
        __try {
            return g_loadOriginal(type, path, flag);
        } __except (loadFilter(GetExceptionCode())) {
            InterlockedIncrement(&g_loadAborted);
            HWND w = GetForegroundWindow();
            ShowWindow(w, SW_MINIMIZE);   // fullscreen: let the message box be seen
            MessageBoxA(nullptr,
                "Der Spielstand bzw. die Karte ist beschaedigt und kann nicht geladen werden.\n"
                "The savegame or map is damaged and cannot be loaded.\n\n"
                "Das Spiel wird beendet. / The game will close.",
                "Settlers II - Community Pack", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
            ExitProcess(3);
            return 0;
        }
    }

    static bool applySafeLoad() {
        if (memcmp((void*)LOAD_AT, LOAD_BYTES, sizeof(LOAD_BYTES)) != 0) return false;
        BYTE* t = (BYTE*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!t) return false;
        memcpy(t, LOAD_BYTES, sizeof(LOAD_BYTES));
        t[7] = 0xE9;
        *(DWORD*)(t + 8) = (LOAD_AT + 7) - ((DWORD)t + 12);
        g_loadOriginal = (Load_t)t;
        BYTE jmp[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
        *(DWORD*)(jmp + 1) = (DWORD)&safeLoad - (LOAD_AT + 5);
        DWORD old;
        if (!VirtualProtect((void*)LOAD_AT, 7, PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy((void*)LOAD_AT, jmp, 7);
        VirtualProtect((void*)LOAD_AT, 7, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)LOAD_AT, 7);
        return true;
    }

    static bool applyStringClamp() {
        if (memcmp((void*)STRREAD_THUNK, STRREAD_BYTES, sizeof(STRREAD_BYTES)) != 0) return false;
        BYTE jmp[5] = { 0xE9 };
        *(DWORD*)(jmp + 1) = (DWORD)stringReadHook - (STRREAD_THUNK + 5);
        DWORD old;
        if (!VirtualProtect((void*)STRREAD_THUNK, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy((void*)STRREAD_THUNK, jmp, 5);
        VirtualProtect((void*)STRREAD_THUNK, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)STRREAD_THUNK, 5);
        return true;
    }

    // ---- fixes that need a stub outside the exe (no padding space at the patch site) ----
    // game command 0x20022 (0x60F6F0): FUN_00451530(obj + 0x14C + index * 8) with an unchecked index (6 entries)
    static const DWORD CMD22_AT = 0x0060F767;                 // lea edx,[ecx+esi*8+14C] (7 bytes)
    static const BYTE CMD22_BYTES[] = { 0x8D, 0x94, 0xF1, 0x4C, 0x01, 0x00, 0x00 };
    static const DWORD CMD22_CONTINUE = 0x0060F76E;           // push edx ; add ecx,118 ; call
    static const DWORD CMD22_SKIP = 0x0060F77A;               // after the call

    static bool applyCmd22() {
        if (memcmp((void*)CMD22_AT, CMD22_BYTES, sizeof(CMD22_BYTES)) != 0) return false;
        BYTE* s = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!s) return false;
        BYTE code[22] = {
            0x83, 0xFE, 0x05,                                  // cmp esi, 5
            0x77, 0x0C,                                        // ja skip
            0x8D, 0x94, 0xF1, 0x4C, 0x01, 0x00, 0x00,          // lea edx, [ecx+esi*8+14C]
            0xE9, 0, 0, 0, 0,                                  // jmp CMD22_CONTINUE
            0xE9, 0, 0, 0, 0,                                  // skip: jmp CMD22_SKIP
        };
        *(DWORD*)(code + 13) = CMD22_CONTINUE - ((DWORD)s + 17);
        *(DWORD*)(code + 18) = CMD22_SKIP - ((DWORD)s + 22);
        memcpy(s, code, sizeof(code));
        BYTE jmp[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
        *(DWORD*)(jmp + 1) = (DWORD)s - (CMD22_AT + 5);
        DWORD old;
        if (!VirtualProtect((void*)CMD22_AT, 7, PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy((void*)CMD22_AT, jmp, 7);
        VirtualProtect((void*)CMD22_AT, 7, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)CMD22_AT, 7);
        return true;
    }

    // ---- game command dispatcher nGame::System (0x611620) ----
    // Commands of these types look up their player with an unchecked index (0x454200). The player comes
    // from the command's sender slot (0x410430 -> 0x6D6B34 -> 0x6D7189) and is -1 when the slot no longer
    // exists (player left / reconnected while commands were queued) -> access before the player table.
    // Such commands are skipped; with the trace on, every command is also reported to NetTrace.
    static const DWORD DISPATCH_AT = 0x00611634;               // add eax, 0xFFFDFFFE (command type in eax, command in esi)
    static const BYTE DISPATCH_BYTES[] = { 0x05, 0xFE, 0xFF, 0xFD, 0xFF };
    static const DWORD DISPATCH_BACK = 0x00611639;
    static const DWORD DISPATCH_SKIP = 0x006118E6;             // pop edi ; pop esi ; ret 4
    static DWORD* const NET_MANAGER = (DWORD*)0x006BD4E8;      // NComm::Manager (0 = no network session)
    static const unsigned long long PLAYER_COMMANDS =          // bit = type - 0x20000
        (1ull << 0x02) | (1ull << 0x04) | (1ull << 0x06) | (1ull << 0x07) | (1ull << 0x08) | (1ull << 0x15) |
        (1ull << 0x18) | (1ull << 0x19) | (1ull << 0x1A) | (1ull << 0x1B) | (1ull << 0x1C) | (1ull << 0x1D) |
        (1ull << 0x1E) | (1ull << 0x26) | (1ull << 0x27) | (1ull << 0x28) | (1ull << 0x29) | (1ull << 0x2A) |
        (1ull << 0x2B) | (1ull << 0x2C) | (1ull << 0x2D);
    typedef int(__fastcall* ResolvePlayer_t)(const void* command, void* unused);   // thiscall without arguments
    static const ResolvePlayer_t resolvePlayer = (ResolvePlayer_t)0x00410430;

    static bool g_checkPlayers = false, g_trace = false;
    static DWORD g_skip = 0;
    static volatile LONG g_skipped = 0;

    static void __cdecl onDispatch(DWORD type, const BYTE* cmd) {
        int player = INT_MIN;
        bool skip = false;
        DWORD idx = type - 0x20000;
        if (idx < 64 && ((PLAYER_COMMANDS >> idx) & 1) && *NET_MANAGER) {
            player = resolvePlayer(cmd, nullptr);
            if (g_checkPlayers && player < 0) { skip = true; InterlockedIncrement(&g_skipped); }
        }
        g_skip = skip ? 1 : 0;
        if (g_trace) NetTrace::onCommand(type, cmd, player, skip);
    }

    static void __declspec(naked) dispatchHook() {
        __asm {
            pushad
            pushfd
            push esi
            push eax
            call onDispatch
            add esp, 8
            popfd
            popad
            cmp g_skip, 0
            jne skip_cmd
            add eax, 0xFFFDFFFE
            push DISPATCH_BACK
            ret
        skip_cmd:
            push DISPATCH_SKIP
            ret
        }
    }

    static bool hookDispatcher() {
        if (memcmp((void*)DISPATCH_AT, DISPATCH_BYTES, sizeof(DISPATCH_BYTES)) != 0) return false;
        BYTE jmp[5] = { 0xE9 };
        *(DWORD*)(jmp + 1) = (DWORD)dispatchHook - (DISPATCH_AT + 5);
        DWORD old;
        if (!VirtualProtect((void*)DISPATCH_AT, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy((void*)DISPATCH_AT, jmp, 5);
        VirtualProtect((void*)DISPATCH_AT, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)DISPATCH_AT, 5);
        return true;
    }

    static bool g_pending = false, g_fixes = false;
    static std::string g_log;

    static void logf(const char* fmt, const char* a, const char* b = "") {
        if (g_log.empty()) return;
        FILE* f = nullptr;
        if (fopen_s(&f, g_log.c_str(), "a") == 0 && f) { fprintf(f, fmt, a, b); fputc('\n', f); fclose(f); }
    }

    void init(const char* iniPath, bool supportedExe, const char* logPath, bool trace) {
        g_log = logPath ? logPath : "";
        if (logPath) DeleteFileA(logPath);
        char v[16];
        GetPrivateProfileStringA("Misc", "GameFixes", "on", v, sizeof(v), iniPath);
        bool on = _stricmp(v, "off") != 0 && _stricmp(v, "false") != 0 && strcmp(v, "0") != 0;
        g_fixes = supportedExe && on;
        g_trace = supportedExe && trace;
        g_pending = g_fixes || g_trace;
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
        g_checkPlayers = g_fixes;
        bool hooked = hookDispatcher();
        logf(hooked ? "command dispatcher hooked: %s" : "command dispatcher NOT hooked (code differs): %s",
            g_fixes ? "commands with an invalid player are skipped" : "trace only");
        if (!g_fixes) return;
        for (const Fix& fx : FIXES) {
            bool match = true;
            for (int i = 0; i < fx.count; i++)
                if (memcmp((void*)fx.parts[i].addr, fx.parts[i].before, fx.parts[i].len) != 0) { match = false; break; }
            if (!match) { logf("skipped (code differs): %s", fx.name); continue; }
            bool ok = true;
            for (int i = 0; i < fx.count; i++) ok = write(fx.parts[i].addr, fx.parts[i].after, fx.parts[i].len) && ok;
            logf(ok ? "applied: %s" : "FAILED: %s", fx.name);
        }
        logf(applyCmd22() ? "applied: %s" : "skipped (code differs): %s",
            "game command 0x20022: slot index checked (0..5) before use (0x60F6F0)");
        logf(applyStringClamp() ? "applied: %s" : "skipped (code differs): %s",
            "stream string read: length limited to the remaining data (0x412D80/0x6D7E8C, savegames/maps/network)");
        logf(applyFileStringClamp() ? "applied: %s" : "skipped (code differs): %s",
            "file string read: length limited to the 2 KB stack buffer (0x448B10, savegame/map headers)");
        logf(applySafeLoad() ? "applied: %s" : "skipped (code differs): %s",
            "game load: a C++ exception from a damaged file aborts the load instead of quitting (0x44A440)");
    }
}
