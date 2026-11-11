#include "LowHeap.h"

#include <cstring>

namespace LowHeap {

    static const ULONG_PTR LIMIT = 0x80000000;    // 2 GB
    static const SIZE_T PAGE = 0x1000;
    static const SIZE_T LARGE_MIN = 0x40000;      // 256 KB: from here on blocks go to the page allocator

    static HANDLE g_heap = nullptr;                // fixed-size heap (small blocks)
    static ULONG_PTR g_smallBase = 0, g_smallEnd = 0;
    static ULONG_PTR g_largeBase = 0, g_largeEnd = 0;
    static SIZE_T g_pages = 0;                     // pages in the large region
    static unsigned short* g_runs = nullptr;       // per page: run length of an allocation starting there, else 0
    static unsigned char* g_used = nullptr;        // per page: 1 = in use
    static SIZE_T g_hint = 0, g_usedPages = 0, g_peakPages = 0;
    static SRWLOCK g_lock = SRWLOCK_INIT;
    static volatile LONG g_smallAllocs = 0, g_largeAllocs = 0, g_fallbacks = 0, g_foreignFrees = 0;

    typedef void* (__cdecl* PMalloc)(size_t);
    typedef void* (__cdecl* PCalloc)(size_t, size_t);
    typedef void* (__cdecl* PRealloc)(void*, size_t);
    typedef void(__cdecl* PFree)(void*);
    typedef LPVOID(WINAPI* PHeapAlloc)(HANDLE, DWORD, SIZE_T);
    typedef BOOL(WINAPI* PHeapFree)(HANDLE, DWORD, LPVOID);
    static PMalloc o_malloc;
    static PCalloc o_calloc;
    static PRealloc o_realloc;
    static PFree o_free;
    static PMalloc o_new;
    static PFree o_delete;
    static PHeapAlloc o_HeapAlloc;
    static PHeapFree o_HeapFree;
    static HANDLE g_processHeap;

    static bool inSmall(const void* p) { return (ULONG_PTR)p >= g_smallBase && (ULONG_PTR)p < g_smallEnd; }
    static bool inLarge(const void* p) { return (ULONG_PTR)p >= g_largeBase && (ULONG_PTR)p < g_largeEnd; }

    // ---------------------------------------------------------------- large blocks: page allocator
    static void* largeAlloc(SIZE_T bytes) {
        SIZE_T n = (bytes + PAGE - 1) / PAGE;
        if (!g_pages || n == 0 || n > 0xFFFF || n > g_pages) return nullptr;
        void* result = nullptr;
        AcquireSRWLockExclusive(&g_lock);
        for (int pass = 0; pass < 2 && !result; pass++) {     // first fit from the hint, then from the start
            SIZE_T i = pass == 0 ? g_hint : 0, stop = pass == 0 ? g_pages : g_hint;
            while (i + n <= g_pages && i < stop + n) {
                SIZE_T j = 0;
                while (j < n && !g_used[i + j]) j++;
                if (j == n) {
                    void* p = (void*)(g_largeBase + i * PAGE);
                    if (!VirtualAlloc(p, n * PAGE, MEM_COMMIT, PAGE_READWRITE)) break;
                    memset(g_used + i, 1, n);
                    g_runs[i] = (unsigned short)n;
                    g_usedPages += n;
                    if (g_usedPages > g_peakPages) g_peakPages = g_usedPages;
                    g_hint = i + n;
                    result = p;
                    break;
                }
                i += j + 1;
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
        return result;
    }

    static SIZE_T largeSize(const void* p) {
        SIZE_T i = ((ULONG_PTR)p - g_largeBase) / PAGE;
        return (SIZE_T)g_runs[i] * PAGE;
    }

    static void largeFree(void* p) {
        if (((ULONG_PTR)p - g_largeBase) % PAGE) return;      // not a block start: ignore like the CRT would crash
        SIZE_T i = ((ULONG_PTR)p - g_largeBase) / PAGE;
        AcquireSRWLockExclusive(&g_lock);
        SIZE_T n = g_runs[i];
        if (n) {
            VirtualFree(p, n * PAGE, MEM_DECOMMIT);
            memset(g_used + i, 0, n);
            g_runs[i] = 0;
            g_usedPages -= n;
            if (i < g_hint) g_hint = i;
        }
        ReleaseSRWLockExclusive(&g_lock);
    }

    // ---------------------------------------------------------------- common entry points
    static void* lowAlloc(SIZE_T bytes, bool zero) {
        if (bytes == 0) bytes = 1;
        void* p = nullptr;
        if (bytes < LARGE_MIN) {
            p = HeapAlloc(g_heap, zero ? HEAP_ZERO_MEMORY : 0, bytes);
            if (p) InterlockedIncrement(&g_smallAllocs);
        }
        if (!p) {
            p = largeAlloc(bytes);                 // committed pages are zero
            if (p) InterlockedIncrement(&g_largeAllocs);
        }
        return p;
    }

    static void lowFree(void* p) {
        if (inSmall(p)) HeapFree(g_heap, 0, p);
        else largeFree(p);
    }

    static SIZE_T lowSize(void* p) {
        return inSmall(p) ? HeapSize(g_heap, 0, p) : largeSize(p);
    }

    static void* __cdecl h_malloc(size_t n) {
        void* p = lowAlloc(n, false);
        if (p) return p;
        InterlockedIncrement(&g_fallbacks);
        return o_malloc(n);
    }

    static void* __cdecl h_calloc(size_t c, size_t n) {
        if (n && c > ((size_t)-1) / n) return nullptr;
        void* p = lowAlloc(c * n, true);
        if (p) return p;
        InterlockedIncrement(&g_fallbacks);
        return o_calloc(c, n);
    }

    static void __cdecl h_free(void* p) {
        if (!p) return;
        if (inSmall(p) || inLarge(p)) { lowFree(p); return; }
        InterlockedIncrement(&g_foreignFrees);
        o_free(p);
    }

    static void* __cdecl h_new(size_t n) {
        void* p = lowAlloc(n, false);
        if (p) return p;
        InterlockedIncrement(&g_fallbacks);
        return o_new(n);
    }

    static void __cdecl h_delete(void* p) {
        if (!p) return;
        if (inSmall(p) || inLarge(p)) { lowFree(p); return; }
        InterlockedIncrement(&g_foreignFrees);
        o_delete(p);
    }

    static void* __cdecl h_realloc(void* p, size_t n) {
        if (!p) return h_malloc(n);
        if (n == 0) { h_free(p); return nullptr; }
        if (!inSmall(p) && !inLarge(p)) return o_realloc(p, n);    // block from before the patch
        if (inSmall(p) && n < LARGE_MIN) {
            void* q = HeapReAlloc(g_heap, 0, p, n);
            if (q) return q;
        }
        SIZE_T old = lowSize(p);
        if (inLarge(p) && n <= old && n >= old / 2) return p;
        void* q = h_malloc(n);
        if (!q) return nullptr;
        memcpy(q, p, old < n ? old : n);
        lowFree(p);
        return q;
    }

    static LPVOID WINAPI h_HeapAlloc(HANDLE h, DWORD flags, SIZE_T n) {
        if (h == g_processHeap) {
            void* p = lowAlloc(n, (flags & HEAP_ZERO_MEMORY) != 0);
            if (p) return p;
            InterlockedIncrement(&g_fallbacks);
        }
        return o_HeapAlloc(h, flags, n);
    }

    static BOOL WINAPI h_HeapFree(HANDLE h, DWORD flags, LPVOID p) {
        if (p && (inSmall(p) || inLarge(p))) { lowFree(p); return TRUE; }
        return o_HeapFree(h, flags, p);
    }

    // ---------------------------------------------------------------- setup
    bool reserve(unsigned smallMB, unsigned largeMB) {
        if (g_heap) return true;
        // fixed-size heap: its whole maximum is reserved now, bottom-up, i.e. in the low free space
        HANDLE heap = HeapCreate(0, 0x100000, (SIZE_T)smallMB << 20);
        if (!heap) return false;
        void* probe = HeapAlloc(heap, 0, 16);
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!probe || !VirtualQuery(probe, &mbi, sizeof(mbi))) { HeapDestroy(heap); return false; }
        ULONG_PTR base = (ULONG_PTR)mbi.AllocationBase, end = base;
        for (ULONG_PTR a = base; VirtualQuery((void*)a, &mbi, sizeof(mbi)) && (ULONG_PTR)mbi.AllocationBase == base;
             a += mbi.RegionSize)
            end = a + mbi.RegionSize;
        HeapFree(heap, 0, probe);
        if (end > LIMIT) { HeapDestroy(heap); return false; }

        SIZE_T largeBytes = (SIZE_T)largeMB << 20;
        void* large = largeBytes ? VirtualAlloc(nullptr, largeBytes, MEM_RESERVE, PAGE_NOACCESS) : nullptr;
        if (large && (ULONG_PTR)large + largeBytes > LIMIT) { VirtualFree(large, 0, MEM_RELEASE); large = nullptr; }
        if (large) {
            SIZE_T pages = largeBytes / PAGE;
            g_runs = (unsigned short*)VirtualAlloc(nullptr, pages * 3, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!g_runs) { VirtualFree(large, 0, MEM_RELEASE); large = nullptr; }
            else {
                g_used = (unsigned char*)(g_runs + pages);
                g_pages = pages;
                g_largeBase = (ULONG_PTR)large;
                g_largeEnd = g_largeBase + largeBytes;
            }
        }
        g_heap = heap;
        g_smallBase = base;
        g_smallEnd = end;
        g_processHeap = GetProcessHeap();
        return true;
    }

    static bool patchSlot(void** slot, void* hook, void** orig) {
        DWORD old;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
        if (!*orig) *orig = *slot;
        *slot = hook;
        VirtualProtect(slot, sizeof(void*), old, &old);
        return true;
    }

    int patch(HMODULE mod) {
        if (!g_heap || !mod) return 0;
        BYTE* b = (BYTE*)mod;
        auto dos = (IMAGE_DOS_HEADER*)b;
        auto nt = (IMAGE_NT_HEADERS*)(b + dos->e_lfanew);
        auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) return 0;
        struct { const char* dll; const char* name; void* hook; void** orig; } table[] = {
            { "msvcrt.dll", "malloc", (void*)h_malloc, (void**)&o_malloc },
            { "msvcrt.dll", "calloc", (void*)h_calloc, (void**)&o_calloc },
            { "msvcrt.dll", "realloc", (void*)h_realloc, (void**)&o_realloc },
            { "msvcrt.dll", "free", (void*)h_free, (void**)&o_free },
            { "msvcrt.dll", "??2@YAPAXI@Z", (void*)h_new, (void**)&o_new },       // operator new
            { "msvcrt.dll", "??3@YAXPAX@Z", (void*)h_delete, (void**)&o_delete }, // operator delete
            { "kernel32.dll", "HeapAlloc", (void*)h_HeapAlloc, (void**)&o_HeapAlloc },
            { "kernel32.dll", "HeapFree", (void*)h_HeapFree, (void**)&o_HeapFree },
        };
        // resolve all originals first: a half-patched module would mix allocators
        for (auto& t : table) {
            HMODULE m = GetModuleHandleA(t.dll);
            void* f = m ? (void*)GetProcAddress(m, t.name) : nullptr;
            if (!f) return 0;
        }
        int patched = 0;
        for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(b + dir.VirtualAddress); imp->Name; imp++) {
            const char* dll = (const char*)(b + imp->Name);
            if (!imp->OriginalFirstThunk) continue;
            auto names = (IMAGE_THUNK_DATA*)(b + imp->OriginalFirstThunk);
            auto slots = (IMAGE_THUNK_DATA*)(b + imp->FirstThunk);
            for (; names->u1.AddressOfData; names++, slots++) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                const char* fn = (const char*)((IMAGE_IMPORT_BY_NAME*)(b + names->u1.AddressOfData))->Name;
                for (auto& t : table)
                    if (!_stricmp(dll, t.dll) && !strcmp(fn, t.name) &&
                        patchSlot((void**)&slots->u1.Function, t.hook, t.orig))
                        patched++;
            }
        }
        return patched;
    }

    bool active() { return g_heap != nullptr; }

    Stats stats() {
        Stats s = { g_smallBase, g_smallEnd - g_smallBase, g_largeBase, g_largeEnd - g_largeBase,
                    g_smallAllocs, g_largeAllocs, g_fallbacks, g_foreignFrees, g_usedPages, g_peakPages };
        return s;
    }
}

// ------------------------------------------------------------------------------------ integration
#include <cstdio>
#include <cstdarg>
#include <string>

namespace LowHeap {

    static std::string g_log;
    static bool g_patched = false;

    static void logf(const char* fmt, ...) {
        if (g_log.empty()) return;
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

    static SIZE_T smallCommitted() {
        SIZE_T c = 0;
        MEMORY_BASIC_INFORMATION mbi;
        for (ULONG_PTR a = g_smallBase; a < g_smallEnd && VirtualQuery((void*)a, &mbi, sizeof(mbi)); a += mbi.RegionSize)
            if (mbi.State == MEM_COMMIT) c += mbi.RegionSize;
        return c;
    }

    static void logStats(const char* when) {
        Stats s = stats();
        logf("%s: small heap %lu KB committed, %ld blocks; large %lu KB in use (peak %lu KB), %ld blocks; "
             "fallbacks %ld, frees of older blocks %ld", when, (unsigned long)(smallCommitted() >> 10), s.smallAllocs,
             (unsigned long)(s.largeUsedPages * 4), (unsigned long)(s.largePeakPages * 4), s.largeAllocs, s.fallbacks,
             s.foreignFrees);
    }

    static DWORD WINAPI statsThread(void*) {
        for (;;) {
            Sleep(60 * 1000);
            logStats("status");
        }
    }

    static bool tryPatch() {
        if (g_patched) return true;
        HMODULE dx = GetModuleHandleA("d3dx9_29.dll");
        if (!dx) return false;
        int n = patch(dx);
        g_patched = n > 0;
        logf("d3dx9_29.dll: %d allocator imports redirected", n);
        return g_patched;
    }

    void install(const char* iniPath, const char* logPath) {
        g_log = logPath ? logPath : "";
        if (logPath) DeleteFileA(logPath);
        char v[16];
        GetPrivateProfileStringA("Misc", "LowHeap", "on", v, sizeof(v), iniPath);
        if (!_stricmp(v, "off") || !_stricmp(v, "false") || !strcmp(v, "0")) { logf("off in d3d9.ini"); return; }

        BYTE* exe = (BYTE*)GetModuleHandleA(nullptr);
        auto nt = (IMAGE_NT_HEADERS*)(exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
        BOOL wow = FALSE;
        IsWow64Process(GetCurrentProcess(), &wow);
        if (!(nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) || !wow) {
            logf("not needed: exe without large-address-aware flag or 32-bit Windows (no memory above 2 GB)");
            return;
        }
        // measured: compiling all shaders of an empty cache needs 7 MB small + 1 MB large
        if (!reserve(32, 64)) { logf("could not reserve address space below 2 GB - not active"); return; }
        Stats s = stats();
        logf("reserved below 2 GB for d3dx9_29: small heap %08lX (%lu MB), large blocks %08lX (%lu MB)",
             (unsigned long)s.smallBase, (unsigned long)(s.smallSize >> 20), (unsigned long)s.largeBase,
             (unsigned long)(s.largeSize >> 20));
        if (!tryPatch()) logf("d3dx9_29.dll not loaded yet - patched at device creation");
        if (!g_log.empty()) CloseHandle(CreateThread(nullptr, 0, statsThread, nullptr, 0, nullptr));
    }

    void onDeviceCreated() {
        if (!active()) return;
        if (!g_patched && !tryPatch()) logf("d3dx9_29.dll not found - not active");
        logStats("device created");
    }
}
