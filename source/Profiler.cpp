#include "Profiler.h"

#include <windows.h>
#include <psapi.h>
#include <timeapi.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "winmm.lib")

namespace Profiler {

    static std::string g_log, g_trigger;
    static volatile DWORD g_mainTid = 0;
    static volatile LONG g_running = 0;
    static DWORD g_lastCheck = 0;
    static std::vector<double> g_frames;           // frame times in ms during the recording
    static LARGE_INTEGER g_freq, g_lastPresent;
    static volatile LONG g_recordFrames = 0;

    struct Mod { ULONG_PTR base, end; std::string name; };

    static std::vector<Mod> modules() {
        std::vector<Mod> out;
        HMODULE mods[512];
        DWORD need = 0;
        if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return out;
        for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 512; i++) {
            MODULEINFO mi;
            char name[MAX_PATH];
            if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) continue;
            if (!GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, MAX_PATH)) continue;
            out.push_back({ (ULONG_PTR)mi.lpBaseOfDll, (ULONG_PTR)mi.lpBaseOfDll + mi.SizeOfImage, name });
        }
        return out;
    }

    static const Mod* find(const std::vector<Mod>& m, ULONG_PTR a) {
        for (auto& x : m) if (a >= x.base && a < x.end) return &x;
        return nullptr;
    }

    static std::string where(const std::vector<Mod>& m, ULONG_PTR a) {
        char b[128];
        const Mod* x = find(m, a);
        if (x && _stricmp(x->name.c_str(), "S2DNG.exe") == 0) _snprintf_s(b, sizeof(b), _TRUNCATE, "S2DNG.exe 0x%08IX", a);
        else if (x) _snprintf_s(b, sizeof(b), _TRUNCATE, "%s+0x%IX", x->name.c_str(), a - x->base);
        else _snprintf_s(b, sizeof(b), _TRUNCATE, "?? 0x%08IX", a);
        return b;
    }

    static bool readable(ULONG_PTR p) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
        return (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0 &&
               !(mbi.Protect & PAGE_GUARD);
    }

    static DWORD WINAPI recorder(void*) {
        const DWORD tid = g_mainTid;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!th) { InterlockedExchange(&g_running, 0); return 0; }
        std::vector<Mod> mods = modules();
        std::unordered_map<ULONG_PTR, unsigned> self, incl;     // address (16-byte bucket) -> samples
        std::unordered_map<std::string, unsigned> perModule;
        unsigned samples = 0;
        timeBeginPeriod(1);
        g_frames.clear();
        InterlockedExchange(&g_recordFrames, 1);
        DWORD start = GetTickCount();
        while (GetTickCount() - start < 30000) {
            if (SuspendThread(th) != (DWORD)-1) {
                CONTEXT c = {};
                c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                bool ok = GetThreadContext(th, &c) != 0;
                ULONG_PTR chain[8];
                int n = 0;
                if (ok) {
                    // EBP chain (only where the code keeps frame pointers); stop at anything implausible
                    ULONG_PTR fp = c.Ebp;
                    for (int i = 0; i < 8 && fp && (fp & 3) == 0 && fp >= c.Esp && fp - c.Esp < 0x100000; i++) {
                        if (!readable(fp) || !readable(fp + 4)) break;
                        ULONG_PTR ret = ((ULONG_PTR*)fp)[1], next = ((ULONG_PTR*)fp)[0];
                        if (!find(mods, ret)) break;
                        chain[n++] = ret;
                        if (next <= fp) break;
                        fp = next;
                    }
                }
                ResumeThread(th);
                if (ok) {
                    samples++;
                    self[c.Eip & ~0xFu]++;
                    const Mod* m = find(mods, c.Eip);
                    perModule[m ? m->name : std::string("??")]++;
                    for (int i = 0; i < n; i++) incl[chain[i]]++;
                }
            }
            Sleep(1);
        }
        InterlockedExchange(&g_recordFrames, 0);
        timeEndPeriod(1);
        CloseHandle(th);

        FILE* f = nullptr;
        if (fopen_s(&f, g_log.c_str(), "w") == 0 && f) {
            std::vector<double> ft = g_frames;
            std::sort(ft.begin(), ft.end());
            double sum = 0;
            for (double v : ft) sum += v;
            fprintf(f, "samples: %u (main thread %lu, 30 s)\n", samples, tid);
            if (!ft.empty())
                fprintf(f, "frames: %u, avg %.2f ms (%.1f fps), median %.2f ms, 95%% %.2f ms, 99%% %.2f ms, max %.2f ms\n",
                        (unsigned)ft.size(), sum / ft.size(), 1000.0 * ft.size() / sum, ft[ft.size() / 2],
                        ft[ft.size() * 95 / 100], ft[ft.size() * 99 / 100], ft.back());
            fprintf(f, "\n-- by module (self)\n");
            std::vector<std::pair<std::string, unsigned>> pm(perModule.begin(), perModule.end());
            std::sort(pm.begin(), pm.end(), [](auto& a, auto& b) { return a.second > b.second; });
            for (auto& p : pm) fprintf(f, "%6.2f%%  %s\n", 100.0 * p.second / samples, p.first.c_str());
            auto dump = [&](const char* title, std::unordered_map<ULONG_PTR, unsigned>& m, size_t count) {
                std::vector<std::pair<ULONG_PTR, unsigned>> v(m.begin(), m.end());
                std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
                fprintf(f, "\n-- %s\n", title);
                for (size_t i = 0; i < v.size() && i < count; i++)
                    fprintf(f, "%6.2f%%  %s\n", 100.0 * v[i].second / samples, where(mods, v[i].first).c_str());
            };
            dump("top addresses (self, 16-byte buckets)", self, 80);
            dump("top return addresses on the EBP chain (inclusive, where frame pointers exist)", incl, 80);
            fclose(f);
        }
        InterlockedExchange(&g_running, 0);
        return 0;
    }

    void init(const char* logPath) {
        if (!logPath) return;
        g_log = logPath;
        DeleteFileA(logPath);
        g_trigger = g_log.substr(0, g_log.find_last_of('\\')) + "\\profile.trigger";
        QueryPerformanceFrequency(&g_freq);
    }

    void onPresent() {
        if (g_log.empty()) return;
        g_mainTid = GetCurrentThreadId();
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (g_recordFrames && g_lastPresent.QuadPart)
            g_frames.push_back(1000.0 * (now.QuadPart - g_lastPresent.QuadPart) / g_freq.QuadPart);
        g_lastPresent = now;
        DWORD t = GetTickCount();
        if (t - g_lastCheck < 500) return;
        g_lastCheck = t;
        if (g_running || GetFileAttributesA(g_trigger.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        DeleteFileA(g_trigger.c_str());
        InterlockedExchange(&g_running, 1);
        HANDLE h = CreateThread(nullptr, 0, recorder, nullptr, 0, nullptr);
        if (h) CloseHandle(h); else InterlockedExchange(&g_running, 0);
    }
}
