#include "GameGuard.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace GameGuard {

    static std::string g_logPath, g_marker;
    static bool g_debug = false, g_cacheAllowed = true;
    static std::vector<std::string> g_pending;   // messages until we know whether to write a log

    static bool g_repaired = false;   // something was actually repaired: the log is written without DebugMode

    static void vlog(const char* fmt, va_list ap) {
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, ap);
        g_pending.push_back(buf);
    }
    // information (written with DebugMode only)
    static void logf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vlog(fmt, ap); va_end(ap); }
    // a repair or a problem the player should be able to find later
    static void repair(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vlog(fmt, ap); va_end(ap); g_repaired = true; }

    static void flushLog(bool force) {
        if (g_pending.empty() || (!force && !g_debug)) return;
        WIN32_FILE_ATTRIBUTE_DATA a;   // keep the log small: start over above 64 KB
        if (GetFileAttributesExA(g_logPath.c_str(), GetFileExInfoStandard, &a) && a.nFileSizeLow > 64 * 1024)
            DeleteFileA(g_logPath.c_str());
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
        // the stored name need not equal the file name; it only has to look like a name
        for (DWORD i = 0; i < len; i++) if (d[16 + i] < 0x20) return false;
        (void)stem;
        return true;
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
                repair("profile %s is damaged and there is no valid backup - left unchanged", name.c_str());
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
                repair("profile %s was damaged - restored from %s (damaged copy: %s)", name.c_str(), src->c_str(), damaged.c_str());
            else
                repair("profile %s was damaged - restoring %s FAILED (error %lu)", name.c_str(), src->c_str(), GetLastError());
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
            repair("previous session ended abnormally - mesh/shader cache cleared (%d files), it is rebuilt on this start", n);
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
                repair("mesh cache folder %s is not writable (error %lu) - mesh cache disabled for this session", cacheDir.c_str(), GetLastError());
            } else {
                CloseHandle(h);
            }
        } else {
            logf("mesh cache disabled ([Misc] MeshCache = off)");
        }
    }

    // ---------------- folders ----------------

    static std::string envVar(const char* name) {
        char buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
        return n && n < sizeof(buf) ? std::string(buf) : std::string();
    }

    // HKCU\...\User Shell Folders\Personal (REG_EXPAND_SZ, follows redirected / OneDrive folders)
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
        std::string home = envVar("USERPROFILE");
        return home.empty() ? home : home + "\\Documents";
    }

    // ---------------- interface ----------------

    void startup(const char* gameRoot, const char* iniPath, bool debug) {
        g_debug = debug;
        g_logPath = std::string(gameRoot) + "\\bin\\gameguard.log";
        if (debug) DeleteFileA(g_logPath.c_str());

        // runs inside DllMain (loader lock): no shell API (SHGetFolderPath may go through COM), only the
        // registry value Explorer itself keeps for the Documents folder and environment variables
        std::string docs = documentsFolder(), local = envVar("LOCALAPPDATA");

        if (!docs.empty()) guardProfiles(docs + "\\S2\\users");
        if (!local.empty()) guardCache(gameRoot, local + "\\S2", docs + "\\S2\\dumps", iniPath);
        flushLog(g_repaired);
    }

    bool meshCacheAllowed() { return g_cacheAllowed; }

    static void writeMarker() {
        if (g_marker.empty()) return;
        HANDLE h = CreateFileA(g_marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, "1", 1, &w, nullptr); CloseHandle(h); }
    }

    void onDeviceCreated() { writeMarker(); }

    void onCleanExit() {
        if (!g_marker.empty()) DeleteFileA(g_marker.c_str());
    }
}
