#include "NetTrace.h"

#include <climits>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace NetTrace {

    // ---- GOG build 11757 ----
    static const DWORD RECV_ADDR = 0x00421560;   // void __thiscall cb_Received_Data(uint, uint, ushort, ushort, void*, int)
    static const BYTE RECV_BYTES[] = { 0x6A, 0xFF, 0x68, 0x78, 0x3E, 0x62, 0x00 };   // push -1; push 0x623E78

    enum Kind : BYTE { K_NET = 1, K_CLICK = 2, K_CMD = 3 };

    static volatile LONG g_cmdSeen = 0;   // commands reported by the dispatcher hook (BinaryFixes)
    struct Event {
        DWORD ms;
        BYTE kind;
        BYTE button;      // click: 1 = left, 2 = right, 3 = middle
        WORD channel, type;
        DWORD conn, user; // click: x, y in window pixels
        int size;
        BYTE head[16];
    };
    static const LONG RING = 512;
    static Event g_ring[RING];
    static volatile LONG g_next = 0;

    struct Stat { unsigned count; unsigned long long bytes; int maxSize; };
    static std::map<DWORD, Stat> g_stats;     // (channel << 16) | type
    static CRITICAL_SECTION g_cs;

    static bool g_enabled = false, g_pending = false, g_hooked = false;
    static std::string g_log;
    static HWND g_window = nullptr;
    static BYTE* g_tramp = nullptr;
    static DWORD g_t0 = 0;

    static Event& slot() {
        LONG i = InterlockedIncrement(&g_next) - 1;
        Event& e = g_ring[i % RING];
        memset(&e, 0, sizeof(e));
        e.ms = GetTickCount() - g_t0;
        return e;
    }

    // called with a pointer to the six stack arguments of cb_Received_Data
    static void __cdecl onReceive(const DWORD* a) {
        Event& e = slot();
        e.kind = K_NET;
        e.conn = a[0];
        e.user = a[1];
        e.channel = (WORD)a[2];
        e.type = (WORD)a[3];
        e.size = (int)a[5];
        const BYTE* data = (const BYTE*)a[4];
        if (data && e.size > 0) {
            __try { memcpy(e.head, data, e.size < 16 ? e.size : 16); }
            __except (EXCEPTION_EXECUTE_HANDLER) { e.head[0] = 0xEE; }
        }
        EnterCriticalSection(&g_cs);
        Stat& s = g_stats[((DWORD)e.channel << 16) | e.type];
        s.count++;
        s.bytes += e.size > 0 ? (unsigned)e.size : 0;
        if (e.size > s.maxSize) s.maxSize = e.size;
        LeaveCriticalSection(&g_cs);
    }

    // command from the dispatcher hook in BinaryFixes: type, sender slot (+0x0C), resolved player (or INT_MIN
    // when the command does not use one), whether it was skipped, and the first three parameters
    void onCommand(DWORD type, const BYTE* cmd, int player, bool skipped) {
        if (!g_enabled) return;
        InterlockedIncrement(&g_cmdSeen);
        Event& e = slot();
        e.kind = K_CMD;
        e.type = (WORD)(type - 0x20000);
        e.channel = (WORD)(short)(player == INT_MIN ? 0x7FFF : player);
        e.button = skipped ? 1 : 0;
        __try {
            e.size = *(const int*)(cmd + 0x0C);   // sender slot
            e.conn = *(const DWORD*)(cmd + 0x10);
            e.user = *(const DWORD*)(cmd + 0x14);
            memcpy(e.head, cmd + 0x18, 4);
        } __except (EXCEPTION_EXECUTE_HANDLER) { e.head[0] = 0xEE; }
        EnterCriticalSection(&g_cs);
        Stat& s = g_stats[0xFFFF0000u | e.type];
        s.count++;
        if (skipped) s.bytes++;                   // commands skipped because of an invalid player
        if (player != INT_MIN && player < 0) s.maxSize = player;
        LeaveCriticalSection(&g_cs);
    }
    static void __declspec(naked) recvHook() {
        __asm {
            pushad
            pushfd
            lea eax, [esp + 36 + 4]     // first argument (8 dwords of pushad + flags + return address)
            push eax
            call onReceive
            add esp, 4
            popfd
            popad
            jmp g_tramp
        }
    }

    void init(bool enabled, const char* logPath) {
        g_enabled = enabled && logPath;
        if (!g_enabled) return;
        g_log = logPath;
        DeleteFileA(logPath);
        InitializeCriticalSection(&g_cs);
        g_t0 = GetTickCount();
        g_pending = true;
    }

    void onDeviceCreated(HWND window) {
        if (!g_enabled) return;
        if (window) g_window = window;
        if (!g_pending) return;
        g_pending = false;
        if (memcmp((void*)RECV_ADDR, RECV_BYTES, sizeof(RECV_BYTES)) != 0) return;
        BYTE* t = (BYTE*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!t) return;
        memcpy(t, RECV_BYTES, sizeof(RECV_BYTES));
        t[7] = 0xE9;
        *(DWORD*)(t + 8) = (RECV_ADDR + 7) - ((DWORD)t + 12);
        g_tramp = t;
        DWORD old;
        if (!VirtualProtect((void*)RECV_ADDR, 7, PAGE_EXECUTE_READWRITE, &old)) return;
        BYTE* p = (BYTE*)RECV_ADDR;
        p[0] = 0xE9;
        *(DWORD*)(p + 1) = (DWORD)recvHook - (RECV_ADDR + 5);
        p[5] = 0x90;
        p[6] = 0x90;
        VirtualProtect((void*)RECV_ADDR, 7, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)RECV_ADDR, 7);
        g_hooked = true;

    }

    void onPresent() {
        if (!g_enabled || !g_window || GetForegroundWindow() != g_window) return;
        static bool down[3] = {};
        static const int vk[3] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON };
        for (int b = 0; b < 3; b++) {
            bool now = (GetAsyncKeyState(vk[b]) & 0x8000) != 0;
            if (now && !down[b]) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(g_window, &pt);
                Event& e = slot();
                e.kind = K_CLICK;
                e.button = (BYTE)(b + 1);
                e.conn = (DWORD)pt.x;
                e.user = (DWORD)pt.y;
            }
            down[b] = now;
        }
    }

    void dump(const char* reason) {
        if (!g_enabled) return;
        FILE* f = nullptr;
        if (fopen_s(&f, g_log.c_str(), "w") != 0 || !f) return;
        RECT rc = {};
        if (g_window) GetClientRect(g_window, &rc);
        fprintf(f, "nettrace: %s  (receive hook %s, command hook %s, window %ldx%ld, %ld events)\n\n", reason,
            g_hooked ? "active" : "NOT active", g_cmdSeen ? "active" : "no commands seen", rc.right, rc.bottom, g_next);

        fprintf(f, "statistics: network channel/type: count, bytes, max size | commands 65535/(type-0x20000): count, skipped (invalid player), lowest negative player\n");
        if (TryEnterCriticalSection(&g_cs)) {   // never block inside a crash handler
            for (auto& s : g_stats)
                fprintf(f, "  %5u/%-5u %8u %12llu %8d\n", s.first >> 16, s.first & 0xFFFF, s.second.count, s.second.bytes, s.second.maxSize);
            LeaveCriticalSection(&g_cs);
        }

        fprintf(f, "\nlast events (oldest first; ms since start)\n");
        LONG n = g_next, first = n > RING ? n - RING : 0;
        for (LONG i = first; i < n; i++) {
            const Event& e = g_ring[i % RING];
            if (e.kind == K_CLICK) {
                fprintf(f, "%9lu  click   button %u at %ld,%ld\n", e.ms, e.button, (long)e.conn, (long)e.user);
            } else if (e.kind == K_CMD) {
                char pl[16];
                if (e.channel == 0x7FFF) strcpy_s(pl, "-"); else sprintf_s(pl, "%d", (int)(short)e.channel);
                fprintf(f, "%9lu  command 0x%05X  slot %d  player %s%s  args %08lX %08lX %02X%02X%02X%02X\n", e.ms, 0x20000 + e.type, e.size,
                    pl, e.button ? "  SKIPPED" : "", e.conn, e.user, e.head[3], e.head[2], e.head[1], e.head[0]);
            } else if (e.kind == K_NET) {
                char hex[64] = "";
                int m = e.size < 16 ? (e.size > 0 ? e.size : 0) : 16;
                for (int k = 0; k < m; k++) sprintf_s(hex + k * 3, sizeof(hex) - k * 3, "%02X ", e.head[k]);
                fprintf(f, "%9lu  net     conn %lu user %lu  ch %u type %u  size %d  %s\n", e.ms, e.conn, e.user, e.channel, e.type, e.size, hex);
            }
        }
        fclose(f);
    }
}
