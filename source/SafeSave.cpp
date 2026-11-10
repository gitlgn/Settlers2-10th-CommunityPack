#include "SafeSave.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace SafeSave {

    // ---- GOG build 11757: import slots of the unpacked exe ----
    static DWORD* const IAT_CREATEFILEA = (DWORD*)0x006541B4;
    static DWORD* const IAT_CLOSEHANDLE = (DWORD*)0x00654048;

    typedef HANDLE(WINAPI* CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    typedef BOOL(WINAPI* CloseHandle_t)(HANDLE);
    static CreateFileA_t g_createFile = nullptr;
    static CloseHandle_t g_closeHandle = nullptr;

    static bool g_pending = false;
    static std::string g_log, g_root;   // g_root: "<documents>\s2\" in lower case
    static CRITICAL_SECTION g_cs;
    struct Pending { HANDLE h; std::string tmp, target; };
    static std::vector<Pending> g_open;
    static volatile LONG g_count = 0;   // number of open redirected handles (fast path for CloseHandle)

    static void logf(const char* fmt, ...) {
        if (g_log.empty()) return;
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        FILE* f = nullptr;
        if (fopen_s(&f, g_log.c_str(), "a") == 0 && f) { fprintf(f, "%s\n", buf); fclose(f); }
    }

    static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

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
        char home[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("USERPROFILE", home, sizeof(home));
        return n && n < sizeof(home) ? std::string(home) + "\\Documents" : std::string();
    }

    // profiles, savegames and other game data below Documents\S2, written from scratch
    static bool protect(LPCSTR name, DWORD access, DWORD disposition) {
        if (!name || !(access & GENERIC_WRITE)) return false;
        if (disposition != CREATE_ALWAYS && disposition != TRUNCATE_EXISTING) return false;
        std::string p = lower(name);
        for (auto& c : p) if (c == '/') c = '\\';
        if (p.compare(0, g_root.size(), g_root) != 0) return false;
        return p.find("\\dumps\\") == std::string::npos;   // logs and crash dumps stay as they are
    }

    static HANDLE WINAPI hookCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                         DWORD disposition, DWORD flags, HANDLE tmpl) {
        if (!protect(name, access, disposition))
            return g_createFile(name, access, share, sa, disposition, flags, tmpl);
        std::string tmp = std::string(name) + ".cptmp";
        HANDLE h = g_createFile(tmp.c_str(), access, share, sa, CREATE_ALWAYS, flags, tmpl);
        if (h == INVALID_HANDLE_VALUE) {
            logf("cannot create %s (error %lu) - writing %s directly", tmp.c_str(), GetLastError(), name);
            return g_createFile(name, access, share, sa, disposition, flags, tmpl);
        }
        EnterCriticalSection(&g_cs);
        g_open.push_back({ h, tmp, name });
        InterlockedIncrement(&g_count);
        LeaveCriticalSection(&g_cs);
        return h;
    }

    static BOOL WINAPI hookCloseHandle(HANDLE h) {
        if (g_count == 0) return g_closeHandle(h);
        Pending p;
        bool found = false;
        EnterCriticalSection(&g_cs);
        for (size_t i = 0; i < g_open.size(); i++) {
            if (g_open[i].h == h) { p = g_open[i]; g_open.erase(g_open.begin() + i); InterlockedDecrement(&g_count); found = true; break; }
        }
        LeaveCriticalSection(&g_cs);
        if (!found) return g_closeHandle(h);

        FlushFileBuffers(h);
        BOOL ok = g_closeHandle(h);
        if (MoveFileExA(p.tmp.c_str(), p.target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            logf("saved %s", p.target.c_str());
        else if (CopyFileA(p.tmp.c_str(), p.target.c_str(), FALSE) && DeleteFileA(p.tmp.c_str()))
            logf("saved %s (copied, rename failed with %lu)", p.target.c_str(), GetLastError());
        else
            logf("could not replace %s (error %lu) - new data kept in %s", p.target.c_str(), GetLastError(), p.tmp.c_str());
        return ok;
    }

    void init(const char* iniPath, bool supportedExe, const char* logPath) {
        g_log = logPath ? logPath : "";
        if (logPath) DeleteFileA(logPath);
        char v[16];
        GetPrivateProfileStringA("Misc", "SafeSave", "on", v, sizeof(v), iniPath);
        bool on = _stricmp(v, "off") != 0 && _stricmp(v, "false") != 0 && strcmp(v, "0") != 0;
        if (!supportedExe || !on) return;
        std::string docs = documentsFolder();
        if (docs.empty()) return;
        g_root = lower(docs + "\\S2\\");
        InitializeCriticalSection(&g_cs);
        g_pending = true;
    }

    static bool redirect(DWORD* slot, const char* name, void* hook, void** original) {
        FARPROC real = GetProcAddress(GetModuleHandleA("kernel32.dll"), name);
        if (!real || *slot != (DWORD)real) {
            logf("%s import not replaced (slot %08lX, kernel32 %p)", name, *slot, real);
            return false;
        }
        *original = (void*)real;
        DWORD old;
        if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) return false;
        *slot = (DWORD)hook;
        VirtualProtect(slot, 4, old, &old);
        return true;
    }

    void onDeviceCreated() {
        if (!g_pending) return;
        g_pending = false;
        // CloseHandle first: a handle opened through the new CreateFileA must always find the matching close
        if (!redirect(IAT_CLOSEHANDLE, "CloseHandle", (void*)&hookCloseHandle, (void**)&g_closeHandle)) return;
        if (!redirect(IAT_CREATEFILEA, "CreateFileA", (void*)&hookCreateFileA, (void**)&g_createFile)) return;
        logf("safe saving active for %s", g_root.c_str());
    }
}
