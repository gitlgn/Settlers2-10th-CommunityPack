#include "GameGuard.h"

#include <windows.h>
#include <shlobj.h>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
#pragma comment(lib, "shell32.lib")

namespace GameGuard {

    static std::string g_logPath, g_marker;
    static bool g_debug = false, g_cacheAllowed = true;
    static std::vector<std::string> g_pending;   // messages until we know whether to write a log

    static void logf(const char* fmt, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        g_pending.push_back(buf);
    }
    static void flushLog(bool force) {
        if (g_pending.empty() || (!force && !g_debug)) return;
        FILE* f = nullptr;
        if (fopen_s(&f, g_logPath.c_str(), "a") != 0 || !f) return;
        SYSTEMTIME t;
        GetLocalTime(&t);
        for (auto& m : g_pending)
            fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d  %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, m.c_str());
        fclose(f);
        g_pending.clear();
    }

    static bool readFile(const std::string& path, std::vector<unsigned char>& data) {
        data.clear();
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD size = GetFileSize(h, nullptr), got = 0;
        bool ok = size != INVALID_FILE_SIZE && size < (16u << 20);
        if (ok) {
            data.resize(size);
            ok = size == 0 || (ReadFile(h, data.data(), size, &got, nullptr) && got == size);
        }
        CloseHandle(h);
        return ok;
    }

    static bool sameContent(const std::string& a, const std::string& b) {
        std::vector<unsigned char> x, y;
        return readFile(a, x) && readFile(b, y) && x == y;
    }

    static bool exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

    static bool fileTime(const std::string& p, ULONGLONG& t) {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &a)) return false;
        t = ((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
        return true;
    }

    // ---------------- profile guard ----------------

    // <profile>.cfg: int32 version, signature CE BA A6 88, int32, int32 name length, name (= file name)
    static bool validProfile(const std::string& path, const std::string& stem) {
        std::vector<unsigned char> d;
        if (!readFile(path, d) || d.size() < 16) return false;
        static const unsigned char SIG[4] = { 0xCE, 0xBA, 0xA6, 0x88 };
        if (memcmp(&d[4], SIG, 4) != 0) return false;
        DWORD len = *(const DWORD*)&d[12];
        if (len > 64 || 16 + len > d.size()) return false;
        if (len != stem.size()) return false;
        return _strnicmp((const char*)&d[16], stem.c_str(), len) == 0;
    }

    static void guardProfiles(const std::string& usersDir) {
        std::string backupDir = usersDir + "\\backup";
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((usersDir + "\\*.cfg").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string name = fd.cFileName;                       // e.g. default01.cfg, .cfg
            std::string stem = name.substr(0, name.size() - 4);
            std::string file = usersDir + "\\" + name;
            std::string bak = backupDir + "\\" + name + ".bak";
            std::string old = backupDir + "\\" + name + ".bak.old";

            if (validProfile(file, stem)) {
                if (exists(bak) && sameContent(file, bak)) continue;
                CreateDirectoryA(backupDir.c_str(), nullptr);
                if (exists(bak)) MoveFileExA(bak.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
                if (CopyFileA(file.c_str(), bak.c_str(), FALSE)) logf("profile %s backed up", name.c_str());
                continue;
            }

            // damaged: restore the newest valid backup, keep the damaged file for inspection
            const std::string* src = validProfile(bak, stem) ? &bak : validProfile(old, stem) ? &old : nullptr;
            if (!src) {
                logf("profile %s is damaged and there is no valid backup - left unchanged", name.c_str());
                continue;
            }
            SYSTEMTIME t;
            GetLocalTime(&t);
            char stamp[32];
            _snprintf_s(stamp, sizeof(stamp), _TRUNCATE, "%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
            std::string damaged = backupDir + "\\" + name + ".damaged-" + stamp;
            CreateDirectoryA(backupDir.c_str(), nullptr);
            MoveFileExA(file.c_str(), damaged.c_str(), MOVEFILE_REPLACE_EXISTING);
            if (CopyFileA(src->c_str(), file.c_str(), FALSE))
                logf("profile %s was damaged - restored from %s (damaged copy: %s)", name.c_str(), src->c_str(), damaged.c_str());
            else
                logf("profile %s was damaged - restoring %s FAILED (error %lu)", name.c_str(), src->c_str(), GetLastError());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    // ---------------- mesh cache guard ----------------

    static int clearCache(const std::string& dir) {
        // mesh cache (*.msh) and compiled shader cache (*.fxPS_*); save game infos (*.info) stay
        int n = 0;
        for (const char* pat : { "\\*.msh", "\\*.fxPS_*", "\\*.fxVS_*" }) {
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA((dir + pat).c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && DeleteFileA((dir + "\\" + fd.cFileName).c_str())) n++;
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        return n;
    }

    static void guardCache(const std::string& gameRoot, const std::string& cacheDir, const std::string& dumpsDir, const char* iniPath) {
        char v[16];
        GetPrivateProfileStringA("Misc", "MeshCache", "on", v, sizeof(v), iniPath);
        bool wanted = _stricmp(v, "off") != 0 && _stricmp(v, "false") != 0 && strcmp(v, "0") != 0;

        std::string state = gameRoot + "\\mods\\_state";
        CreateDirectoryA((gameRoot + "\\mods").c_str(), nullptr);
        CreateDirectoryA(state.c_str(), nullptr);
        g_marker = state + "\\session.running";
        std::string lastStart = state + "\\session.started";

        // previous session: still marked as running (killed / crashed without exit), or the game's own
        // crash handler wrote a crash log after that session had started
        bool abnormal = exists(g_marker);
        ULONGLONG tStart = 0, tCrash = 0;
        if (fileTime(lastStart, tStart) && fileTime(dumpsDir + "\\crashlog.txt", tCrash) && tCrash > tStart) abnormal = true;

        if (abnormal && exists(cacheDir)) {
            int n = clearCache(cacheDir);
            logf("previous session ended abnormally - mesh/shader cache cleared (%d files), it is rebuilt on this start", n);
        }

        // session markers
        for (const std::string& p : { g_marker, lastStart }) {
            HANDLE h = CreateFileA(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, "1", 1, &w, nullptr); CloseHandle(h); }
        }

        // the cache folder must be writable, otherwise the engine crashes while writing it
        g_cacheAllowed = wanted;
        if (wanted) {
            CreateDirectoryA(cacheDir.c_str(), nullptr);
            std::string probe = cacheDir + "\\_write_test.tmp";
            HANDLE h = CreateFileA(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                g_cacheAllowed = false;
                logf("mesh cache folder %s is not writable (error %lu) - mesh cache disabled for this session", cacheDir.c_str(), GetLastError());
            } else {
                CloseHandle(h);
            }
        } else {
            logf("mesh cache disabled ([Misc] MeshCache = off)");
        }
    }

    // ---------------- interface ----------------

    void startup(const char* gameRoot, const char* iniPath, bool debug) {
        g_debug = debug;
        g_logPath = std::string(gameRoot) + "\\bin\\gameguard.log";
        if (debug) DeleteFileA(g_logPath.c_str());

        char docs[MAX_PATH] = "", local[MAX_PATH] = "";
        SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs);
        SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local);

        size_t before = 0;
        if (docs[0]) guardProfiles(std::string(docs) + "\\S2\\users");
        bool repaired = false;
        for (auto& m : g_pending) if (m.find("damaged") != std::string::npos) repaired = true;
        before = g_pending.size();
        if (local[0]) guardCache(gameRoot, std::string(local) + "\\S2", std::string(docs) + "\\S2\\dumps", iniPath);
        for (size_t i = before; i < g_pending.size(); i++) repaired = true;   // every cache message is an event
        flushLog(repaired);
    }

    bool meshCacheAllowed() { return g_cacheAllowed; }

    void onCleanExit() {
        if (!g_marker.empty()) DeleteFileA(g_marker.c_str());
    }
}
