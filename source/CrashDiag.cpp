#include "CrashDiag.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace CrashDiag {

    static char g_logPath[MAX_PATH];
    static char g_dumpPath[MAX_PATH];
    static HMODULE g_exe = nullptr;
    static volatile LONG g_dumped = 0;
    static volatile LONG g_count = 0;

    typedef BOOL(WINAPI* MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);

    static void logf(const char* fmt, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
        va_end(ap);
        if (n < 0) return;

        SYSTEMTIME st;
        GetLocalTime(&st);
        char line[1100];
        int m = _snprintf_s(line, sizeof(line), _TRUNCATE, "%02d:%02d:%02d.%03d [T%lu] %s\r\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId(), buf);
        if (m <= 0) return;

        HANDLE h = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD w;
        WriteFile(h, line, (DWORD)m, &w, nullptr);
        CloseHandle(h);
    }

    // "name+offset" for an address, e.g. "S2DNG.exe+0017C20C"
    static void describe(void* addr, char* out, size_t outSize) {
        HMODULE mod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)addr, &mod) && mod) {
            char name[MAX_PATH];
            GetModuleFileNameA(mod, name, MAX_PATH);
            const char* base = strrchr(name, '\\');
            _snprintf_s(out, outSize, _TRUNCATE, "%s+%08X (va %p)", base ? base + 1 : name,
                (unsigned)((BYTE*)addr - (BYTE*)mod), addr);
        }
        else {
            _snprintf_s(out, outSize, _TRUNCATE, "?? (%p)", addr);
        }
    }

    static bool isInteresting(DWORD code) {
        switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case 0xC0000374: // heap corruption
        case 0xC0000409: // stack buffer overrun
            return true;
        }
        return false;
    }

    static void writeDump(EXCEPTION_POINTERS* ep) {
        if (InterlockedCompareExchange(&g_dumped, 1, 0) != 0) return;
        HMODULE dbg = LoadLibraryA("dbghelp.dll");
        if (!dbg) return;
        auto fn = (MiniDumpWriteDump_t)GetProcAddress(dbg, "MiniDumpWriteDump");
        if (!fn) return;
        HANDLE f = CreateFileA(g_dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        struct { DWORD tid; EXCEPTION_POINTERS* ep; BOOL cl; } info = { GetCurrentThreadId(), ep, FALSE };
        // MiniDumpWithIndirectlyReferencedMemory(0x40) | WithDataSegs(0x1) | WithThreadInfo(0x1000)
        fn(GetCurrentProcess(), GetCurrentProcessId(), f, 0x1 | 0x40 | 0x1000, &info, nullptr, nullptr);
        CloseHandle(f);
        logf("minidump written: %s", g_dumpPath);
    }

    static void dumpStackWords(CONTEXT* c) {
        // return-address candidates: stack dwords that point into the game exe's code range
        DWORD* sp = (DWORD*)c->Esp;
        BYTE* lo = (BYTE*)g_exe + 0x1000;
        BYTE* hi = (BYTE*)g_exe + 0x280000;
        int found = 0;
        __try {
            for (int i = 0; i < 1024 && found < 16; i++) {
                BYTE* v = (BYTE*)sp[i];
                if (v >= lo && v < hi) {
                    char d[200];
                    describe(v, d, sizeof(d));
                    logf("    stack[+%03X] -> %s", i * 4, d);
                    found++;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static LONG CALLBACK vectoredHandler(EXCEPTION_POINTERS* ep) {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (!isInteresting(code)) return EXCEPTION_CONTINUE_SEARCH;
        if (InterlockedIncrement(&g_count) > 200) return EXCEPTION_CONTINUE_SEARCH;

        CONTEXT* c = ep->ContextRecord;
        char where[300];
        describe(ep->ExceptionRecord->ExceptionAddress, where, sizeof(where));
        logf("EXCEPTION 0x%08X at %s", code, where);
        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
            logf("    %s address %p", ep->ExceptionRecord->ExceptionInformation[0] ? "write to" : "read from",
                (void*)ep->ExceptionRecord->ExceptionInformation[1]);
        }
        logf("    eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X",
            c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
        dumpStackWords(c);
        writeDump(ep);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void install(const char* logPath) {
        strncpy_s(g_logPath, logPath, _TRUNCATE);
        strncpy_s(g_dumpPath, logPath, _TRUNCATE);
        char* dot = strrchr(g_dumpPath, '.');
        if (dot) strcpy_s(dot, g_dumpPath + MAX_PATH - dot, ".dmp");
        g_exe = GetModuleHandleA(nullptr);

        DeleteFileA(g_logPath);
        logf("crashdiag installed (exe base %p)", g_exe);
        AddVectoredExceptionHandler(1, vectoredHandler);
    }

    void onProcessDetach(LPVOID lpReserved) {
        if (!g_logPath[0]) return;
        // lpReserved != NULL means the process is terminating (ExitProcess), not an explicit FreeLibrary
        logf("DLL_PROCESS_DETACH (%s)", lpReserved ? "process exit" : "FreeLibrary");
        void* frames[40];
        USHORT n = CaptureStackBackTrace(0, 40, frames, nullptr);
        for (USHORT i = 0; i < n; i++) {
            char d[300];
            describe(frames[i], d, sizeof(d));
            logf("    exit-frame %02u: %s", i, d);
        }
    }
}
