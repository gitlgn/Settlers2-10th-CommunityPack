#include "BinaryFixes.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <climits>
#include <string>

#include "FixDecisions.h"
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
            if (!s || !s->data) return;
            // the decision itself lives in FixDecisions (shared with the headless tests)
            if (FixDecisions::clampStringRead(s->data, s->len, s->pos, nullptr))
                InterlockedIncrement(&g_clamped);
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

    // ---- cube texture (0x51F1F0, S2CE::CCubeTexture::CreateD3DTexture): the result of
    // D3DXCreateCubeTextureFromFileInMemoryEx is not checked before GetLevelDesc is called on the texture, and the
    // engine binds the texture later without a check either (0x51F2B0). A damaged/unsupported cube map (e.g. from a
    // mod, data\enviroment\cube_0.dds) or an out-of-memory error crashes on a NULL texture. Now a small neutral
    // cube texture is used instead (the map loads, only the environment reflection is plain).
    static const DWORD CUBE_AT = 0x0051F233, CUBE_BACK = 0x0051F23E, CUBE_FAIL = 0x0051F295;   // fail: mov al, bl (bl = 0)
    static const BYTE CUBE_BYTES[] = { 0x8B, 0x3F, 0x8D, 0x4C, 0x24, 0x10, 0x51, 0x6A, 0x00, 0x8B, 0xE8 };
    static const BYTE CUBE_FAIL_BYTES[] = { 0x8A, 0xC3, 0x5F, 0x5E, 0x5D, 0x5B };              // mov al,bl; pop x4
    static volatile LONG g_cubeFallbacks = 0;
    static void logCubeFallback();

    // object layout (CCubeTexture): +0x0C device, +0x10 IDirect3DCubeTexture9*
    static void* __stdcall cubeFallback(BYTE* obj) {
        __try {
            void** dev = *(void***)(obj + 0x0C);
            if (!dev) return nullptr;
            typedef HRESULT(__stdcall* CreateCube_t)(void*, UINT, UINT, DWORD, DWORD, DWORD, void**, HANDLE*);
            typedef HRESULT(__stdcall* LockCube_t)(void*, DWORD, UINT, void*, const RECT*, DWORD);
            typedef HRESULT(__stdcall* UnlockCube_t)(void*, DWORD, UINT);
            void* tex = nullptr;
            const UINT N = 4;
            // IDirect3DDevice9::CreateCubeTexture (vtable 25): A8R8G8B8 (21), D3DPOOL_MANAGED (1)
            if (((CreateCube_t)(*(void***)dev)[25])(dev, N, 1, 0, 21, 1, &tex, nullptr) < 0 || !tex) return nullptr;
            struct { INT pitch; void* bits; } lr;
            for (DWORD face = 0; face < 6; face++) {
                // IDirect3DCubeTexture9::LockRect (vtable 19) / UnlockRect (20)
                if (((LockCube_t)(*(void***)tex)[19])(tex, face, 0, &lr, nullptr, 0) < 0) continue;
                for (UINT y = 0; y < N; y++)
                    for (UINT x = 0; x < N; x++)
                        ((DWORD*)((BYTE*)lr.bits + y * lr.pitch))[x] = 0xFF8C9BAA;   // soft grey-blue sky
                ((UnlockCube_t)(*(void***)tex)[20])(tex, face, 0);
            }
            *(void**)(obj + 0x10) = tex;
            InterlockedIncrement(&g_cubeFallbacks);
            logCubeFallback();
            return tex;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    static bool applyCubeTextureCheck() {
        if (memcmp((void*)CUBE_AT, CUBE_BYTES, sizeof(CUBE_BYTES)) != 0 ||
            memcmp((void*)CUBE_FAIL, CUBE_FAIL_BYTES, sizeof(CUBE_FAIL_BYTES)) != 0) return false;
        BYTE* s = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!s) return false;
        BYTE code[45] = {
            0x8B, 0xE8,                                        //  0 mov ebp, eax        (HRESULT, as original)
            0x8B, 0x3F,                                        //  2 mov edi, [edi]      (texture)
            0x85, 0xED,                                        //  4 test ebp, ebp
            0x7C, 0x10,                                        //  6 jl fail
            0x85, 0xFF,                                        //  8 test edi, edi
            0x74, 0x0C,                                        // 10 jz fail
            0x8D, 0x4C, 0x24, 0x10,                            // 12 ok: lea ecx, [esp+0x10]
            0x51,                                              // 16 push ecx
            0x6A, 0x00,                                        // 17 push 0
            0xE9, 0, 0, 0, 0,                                  // 19 jmp CUBE_BACK       (GetLevelDesc call)
            0x56,                                              // 24 fail: push esi      (this)
            0xE8, 0, 0, 0, 0,                                  // 25 call cubeFallback
            0x85, 0xC0,                                        // 30 test eax, eax
            0x74, 0x06,                                        // 32 jz give_up
            0x8B, 0xF8,                                        // 34 mov edi, eax
            0x33, 0xED,                                        // 36 xor ebp, ebp        (S_OK -> returns true)
            0xEB, 0xE4,                                        // 38 jmp ok
            0xE9, 0, 0, 0, 0,                                  // 40 give_up: jmp CUBE_FAIL (return false)
        };
        *(DWORD*)(code + 20) = CUBE_BACK - ((DWORD)s + 24);
        *(DWORD*)(code + 26) = (DWORD)&cubeFallback - ((DWORD)s + 30);
        *(DWORD*)(code + 41) = CUBE_FAIL - ((DWORD)s + 45);
        memcpy(s, code, sizeof(code));
        BYTE jmp[11] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
        *(DWORD*)(jmp + 1) = (DWORD)s - (CUBE_AT + 5);
        DWORD old;
        if (!VirtualProtect((void*)CUBE_AT, sizeof(jmp), PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy((void*)CUBE_AT, jmp, sizeof(jmp));
        VirtualProtect((void*)CUBE_AT, sizeof(jmp), old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)CUBE_AT, sizeof(jmp));
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

    static void logCubeFallback() {
        logf("cube texture could not be loaded (damaged/unsupported file or out of memory): %s", "neutral replacement used");
    }

    // ---- [Misc] SkipIntro: after a startup video ends, 0x4CA441 reads the step counter [esi+0xBC8] (0 -> BB_Logo,
    // 1 -> funaticsAlpha, 2 -> Intro, 3 -> main menu). Reading 3 instead goes from the Ubisoft logo straight to the menu.
    static const DWORD INTRO_AT = 0x004CA441;
    static const BYTE INTRO_BYTES[] = { 0x8B, 0x86, 0xC8, 0x0B, 0x00, 0x00 };   // mov eax, [esi+0BC8h]
    static const BYTE INTRO_SKIP[] = { 0xB8, 0x03, 0x00, 0x00, 0x00, 0x90 };    // mov eax, 3
    static bool g_skipIntro = false;
    static bool write(DWORD addr, const BYTE* bytes, int len);

    void init(const char* iniPath, bool supportedExe, const char* logPath, bool trace) {
        g_log = logPath ? logPath : "";
        if (logPath) DeleteFileA(logPath);
        char v[16];
        GetPrivateProfileStringA("Misc", "GameFixes", "on", v, sizeof(v), iniPath);
        bool on = _stricmp(v, "off") != 0 && _stricmp(v, "false") != 0 && strcmp(v, "0") != 0;
        g_fixes = supportedExe && on;
        g_trace = supportedExe && trace;
        GetPrivateProfileStringA("Misc", "SkipIntro", "off", v, sizeof(v), iniPath);
        g_skipIntro = supportedExe && (!_stricmp(v, "on") || !_stricmp(v, "true") || !strcmp(v, "1"));
        g_pending = g_fixes || g_trace || g_skipIntro;
    }

    static bool write(DWORD addr, const BYTE* bytes, int len) {
        DWORD old;
        if (!VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy((void*)addr, bytes, len);
        VirtualProtect((void*)addr, len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
        return true;
    }

    // ---- multiplayer file transfer (own TFTP, message type 300): a read request from another player carries a
    // file name that is appended to the Documents folder (0x6E2868: Documents + "\" + name) and then opened and
    // sent (0x42B390). The name was never checked -> "..\" or an absolute path let any player in the session
    // read arbitrary files of the host. All transfer sessions start through the thunk 0x42B2B0 -> 0x6E1CE2
    // (path = 3rd argument, std::string). The part after Documents\ must not contain "..", ':' or start with a
    // separator; such a request gets an empty path (the game answers TFTP_ERROR_FILENOTFOUND).
    static const DWORD TFTP_THUNK = 0x0042B2B0;
    static const BYTE TFTP_BYTES[] = { 0xE9, 0x2D, 0x6A, 0x2B, 0x00 };   // jmp 0x6E1CE2
    static const DWORD TFTP_REAL = 0x006E1CE2;
    static std::string g_docs;                                         // "<documents>\" lower case
    static long g_tftpBlocked = 0;

    using GameString = FixDecisions::GameString;

    static std::string documentsFolder() {
        char raw[MAX_PATH] = "", path[MAX_PATH] = "";
        DWORD size = sizeof(raw), type = 0;
        HKEY k;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
                0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
            if (RegQueryValueExA(k, "Personal", nullptr, &type, (BYTE*)raw, &size) != ERROR_SUCCESS) raw[0] = 0;
            RegCloseKey(k);
        }
        raw[sizeof(raw) - 1] = 0;
        if (raw[0] && ExpandEnvironmentStringsA(raw, path, sizeof(path)) && path[0]) return path;
        return std::string();
    }

    // ---- damaged game file (NBase::gDecryptData, 0x593C20): on an integrity error of an encrypted game file (FCC,
    // password CRC, data size, data CRC) the game writes to address 0 on purpose - a crash without explanation,
    // typical for a damaged or wrongly packed mod file. The game has just logged the file name to
    // Documents\S2\dumps\Logfile_*.txt; the four crash sites now show that name and end the game cleanly.
    static const DWORD DECRYPT_FAIL[] = { 0x00593C8B, 0x00593CFC, 0x00593D75, 0x00593DDE };
    static const BYTE DECRYPT_FAIL_BYTES[] = { 0xC6, 0x05, 0x00, 0x00, 0x00, 0x00, 0x0C };   // mov byte ptr [0], 0Ch

    // file name of the file being decrypted: the entry of gDecryptData stores its first parameter (a game std::string)
    static const DWORD DECRYPT_AT = 0x00593C20;
    static const BYTE DECRYPT_ENTRY[] = { 0x83, 0xEC, 0x24, 0xA1, 0xF0, 0x9D, 0x6B, 0x00 };   // sub esp,24h; mov eax,[6B9DF0h]
    static volatile DWORD g_decryptName = 0;

    static void readDecryptName(char* buf, size_t cap) {
        buf[0] = 0;
        __try {
            const BYTE* gs = (const BYTE*)g_decryptName;
            if (gs) {
                DWORD size = *(const DWORD*)(gs + 0x14), c = *(const DWORD*)(gs + 0x18);
                const char* d = c >= 16 ? *(const char* const*)(gs + 4) : (const char*)(gs + 4);
                if (size >= cap) size = (DWORD)cap - 1;
                memcpy(buf, d, size);
                buf[size] = 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = 0; }
    }

    static std::string currentDecryptName() {
        char buf[300];
        readDecryptName(buf, sizeof(buf));
        return buf;
    }

    static bool applyDecryptNameHook() {
        if (memcmp((void*)DECRYPT_AT, DECRYPT_ENTRY, sizeof(DECRYPT_ENTRY)) != 0) return false;
        BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!t) return false;
        BYTE code[] = {
            0x8B, 0x44, 0x24, 0x04,                 // mov eax, [esp+4]      (file name string)
            0xA3, 0, 0, 0, 0,                       // mov [g_decryptName], eax
            0x83, 0xEC, 0x24,                       // sub esp, 24h          (original)
            0xA1, 0xF0, 0x9D, 0x6B, 0x00,           // mov eax, [6B9DF0h]    (original)
            0xE9, 0, 0, 0, 0,                       // jmp 593C28h
        };
        *(DWORD*)(code + 5) = (DWORD)&g_decryptName;
        *(DWORD*)(code + 18) = (DECRYPT_AT + 8) - ((DWORD)t + 22);
        memcpy(t, code, sizeof(code));
        BYTE jmp[8] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
        *(DWORD*)(jmp + 1) = (DWORD)t - (DECRYPT_AT + 5);
        DWORD old;
        if (!VirtualProtect((void*)DECRYPT_AT, 8, PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy((void*)DECRYPT_AT, jmp, 8);
        VirtualProtect((void*)DECRYPT_AT, 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)DECRYPT_AT, 8);
        return true;
    }

    static std::string damagedFileName() {
        std::string direct = currentDecryptName();
        if (!direct.empty()) return direct;
        std::string dir = documentsFolder();
        if (dir.empty()) return std::string();
        dir += "\\S2\\dumps\\";
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((dir + "Logfile_*.txt").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return std::string();
        std::string newest;
        FILETIME best = {};
        do {
            if (CompareFileTime(&fd.ftLastWriteTime, &best) > 0) { best = fd.ftLastWriteTime; newest = fd.cFileName; }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        std::string text, line, found;
        FILE* f = nullptr;
        if (fopen_s(&f, (dir + newest).c_str(), "rb") != 0 || !f) return std::string();
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        fclose(f);
        size_t p = text.rfind("mismatch");
        if (p == std::string::npos) return std::string();
        size_t a = text.rfind("file ", p), b = text.rfind(": ", p);
        if (a == std::string::npos || b == std::string::npos || b <= a + 5) return std::string();
        return text.substr(a + 5, b - a - 5);
    }

    static void __cdecl damagedGameFile() {
        std::string name = damagedFileName();
        if (name.empty()) name = "(see Documents\\S2\\dumps\\Logfile_0.txt)";
        std::string msg =
            "Eine Spieldatei ist beschaedigt oder falsch gepackt:\n    " + name + "\n\n"
            "Stammt sie von einem Mod: Mod neu installieren oder entfernen. Sonst die Spieldateien in GOG Galaxy pruefen lassen.\n\n"
            "A game file is damaged or wrongly packed:\n    " + name + "\n\n"
            "If it comes from a mod, reinstall or remove the mod; otherwise let GOG Galaxy verify the game files.\n\n"
            "Das Spiel wird beendet. / The game will close.";
        HWND w = GetForegroundWindow();
        ShowWindow(w, SW_MINIMIZE);   // fullscreen: let the message box be seen
        MessageBoxA(nullptr, msg.c_str(), "Settlers II - Community Pack", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
        ExitProcess(4);
    }

    static bool applyDamagedFileMessage() {
        applyDecryptNameHook();
        for (DWORD at : DECRYPT_FAIL)
            if (memcmp((void*)at, DECRYPT_FAIL_BYTES, sizeof(DECRYPT_FAIL_BYTES)) != 0) return false;
        for (DWORD at : DECRYPT_FAIL) {
            BYTE call[7] = { 0xE8, 0, 0, 0, 0, 0x90, 0x90 };
            *(DWORD*)(call + 1) = (DWORD)&damagedGameFile - (at + 5);
            DWORD old;
            if (!VirtualProtect((void*)at, 7, PAGE_EXECUTE_READWRITE, &old)) return false;
            memcpy((void*)at, call, 7);
            VirtualProtect((void*)at, 7, old, &old);
            FlushInstructionCache(GetCurrentProcess(), (void*)at, 7);
        }
        return true;
    }

    // ---- damaged unencrypted Granny model (mods ship plain .gr2): granny2.dll parses it without any check and
    // writes past its buffers. The Granny header carries a CRC-32 of the file from (32 + section array offset) to
    // TotalSize (verified on game models); the game's import of GrannyReadEntireFileFromMemory is redirected to a
    // check first. The file name comes from the entry of the game's file loader (0x594810).
    static const DWORD GRANNY_READ_SLOT = 0x006543CC;
    static const DWORD FILELOAD_AT = 0x00594810;
    static const BYTE FILELOAD_ENTRY[] = { 0x53, 0x8B, 0x5C, 0x24, 0x08 };   // push ebx; mov ebx,[esp+8]
    static volatile DWORD g_loadName = 0;
    typedef void*(__stdcall* GrannyRead_t)(int, void*);
    static GrannyRead_t g_grannyRead = nullptr;

    static DWORD crc32(const BYTE* p, size_t n) {
        static DWORD table[256];
        static bool init = false;
        if (!init) {
            for (DWORD i = 0; i < 256; i++) { DWORD c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320 ^ (c >> 1) : c >> 1; table[i] = c; }
            init = true;
        }
        DWORD c = 0xFFFFFFFF;
        for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFF;
    }

    static bool grannyLooksDamaged(int size, const BYTE* m) {
        static const BYTE magic[16] = { 0xB8, 0x67, 0xB0, 0xCA, 0xF8, 0x6D, 0xB1, 0x0F, 0x84, 0x72, 0x8C, 0x7E, 0x5E, 0x19, 0x00, 0x1E };
        if (!m || size < 88 || memcmp(m, magic, 16) != 0) return false;   // other Granny variants: not checked
        DWORD total = *(const DWORD*)(m + 36), crc = *(const DWORD*)(m + 40), secoff = *(const DWORD*)(m + 44);
        if (crc == 0) return false;                                       // written without CRC
        if (total > (DWORD)size || secoff > total || 32 + secoff > total) return true;
        return crc32(m + 32 + secoff, total - 32 - secoff) != crc;
    }

    static bool grannyCheck(int size, void* mem) {
        bool bad = false;
        __try { bad = grannyLooksDamaged(size, (const BYTE*)mem); } __except (EXCEPTION_EXECUTE_HANDLER) { bad = false; }
        return bad;
    }

    static void* __stdcall grannyReadChecked(int size, void* mem) {
        if (grannyCheck(size, mem)) {
            g_decryptName = g_loadName;          // name for the message (same game string layout)
            damagedGameFile();
        }
        return g_grannyRead(size, mem);
    }

    static bool applyGrannyCheck() {
        if (memcmp((void*)FILELOAD_AT, FILELOAD_ENTRY, sizeof(FILELOAD_ENTRY)) != 0) return false;
        HMODULE gr = GetModuleHandleA("granny2.dll");
        void* real = gr ? (void*)GetProcAddress(gr, "_GrannyReadEntireFileFromMemory@8") : nullptr;
        if (!real || *(void**)GRANNY_READ_SLOT != real) return false;
        BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!t) return false;
        BYTE code[] = {
            0x8B, 0x44, 0x24, 0x04,                 // mov eax, [esp+4]      (path string)
            0xA3, 0, 0, 0, 0,                       // mov [g_loadName], eax
            0x53, 0x8B, 0x5C, 0x24, 0x08,           // original: push ebx; mov ebx,[esp+8]
            0xE9, 0, 0, 0, 0,                       // jmp 594815h
        };
        *(DWORD*)(code + 5) = (DWORD)&g_loadName;
        *(DWORD*)(code + 15) = (FILELOAD_AT + 5) - ((DWORD)t + 19);
        memcpy(t, code, sizeof(code));
        BYTE jmp[5] = { 0xE9 };
        *(DWORD*)(jmp + 1) = (DWORD)t - (FILELOAD_AT + 5);
        DWORD old;
        if (!VirtualProtect((void*)FILELOAD_AT, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy((void*)FILELOAD_AT, jmp, 5);
        VirtualProtect((void*)FILELOAD_AT, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)FILELOAD_AT, 5);
        g_grannyRead = (GrannyRead_t)real;
        if (!VirtualProtect((void*)GRANNY_READ_SLOT, 4, PAGE_READWRITE, &old)) return false;
        *(void**)GRANNY_READ_SLOT = (void*)grannyReadChecked;
        VirtualProtect((void*)GRANNY_READ_SLOT, 4, old, &old);
        return true;
    }

    static void __cdecl checkTransferPath(GameString* s) {
        // the request path is snapshotted before the SEH: C++ objects (std::string) may not be
        // destroyed inside __try (C2712) and the decision below rewrites the string in place
        char logPath[4160];
        if (s && g_docs.size() && s->size && s->size <= 4096) {
            memcpy(logPath, FixDecisions::gsData(s), s->size);
            logPath[s->size] = 0;
        }
        __try {
            // decision in FixDecisions (shared with the headless tests): a hostile request is
            // emptied in place there, which the game's own empty-guard turns into FILENOTFOUND
            if (!s || g_docs.empty() || s->size == 0 || s->size > 4096) return;
            if (FixDecisions::checkTransferPath(s, g_docs, &g_tftpBlocked))
                logf("file transfer request BLOCKED: %s", logPath);
            else
                logf("file transfer request: %s", logPath);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void __declspec(naked) tftpHook() {
        __asm {
            pushad
            pushfd
            push dword ptr [esp + 36 + 0x0C]   // 3rd argument: path
            call checkTransferPath
            add esp, 4
            popfd
            popad
            push TFTP_REAL
            ret
        }
    }

    static bool applyTransferPathCheck() {
        if (memcmp((void*)TFTP_THUNK, TFTP_BYTES, sizeof(TFTP_BYTES)) != 0) return false;
        g_docs = documentsFolder();
        if (g_docs.empty()) return false;
        for (auto& c : g_docs) { c = (char)tolower((unsigned char)c); if (c == '/') c = '\\'; }
        if (g_docs.back() != '\\') g_docs += '\\';
        BYTE jmp[5] = { 0xE9 };
        *(DWORD*)(jmp + 1) = (DWORD)tftpHook - (TFTP_THUNK + 5);
        return write(TFTP_THUNK, jmp, 5);
    }

    void onDeviceCreated() {
        if (!g_pending) return;
        g_pending = false;
        if (g_skipIntro) {
            bool ok = memcmp((void*)INTRO_AT, INTRO_BYTES, sizeof(INTRO_BYTES)) == 0 && write(INTRO_AT, INTRO_SKIP, sizeof(INTRO_SKIP));
            logf(ok ? "applied: %s" : "skipped (code differs): %s", "skip intro videos (only the Ubisoft logo, 0x4CA441)");
        }
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
        logf(applyTransferPathCheck() ? "applied: %s" : "skipped (code differs): %s",
            "multiplayer file transfer: requested names limited to the Documents folder (0x42B2B0)");
        logf(applyCubeTextureCheck() ? "applied: %s" : "skipped (code differs): %s",
            "cube texture: a failed load gets a neutral replacement instead of crashing on a NULL texture (0x51F1F0)");
        logf(applyDamagedFileMessage() ? "applied: %s" : "skipped (code differs): %s",
            "damaged game file: message with the file name instead of a deliberate crash (0x593C20)");
        logf(applyGrannyCheck() ? "applied: %s" : "skipped (code differs): %s",
            "unencrypted Granny models: CRC checked before granny2.dll parses them (0x6543CC)");
    }
}
