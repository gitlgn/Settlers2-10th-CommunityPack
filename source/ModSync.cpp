#include "ModSync.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace ModSync {

    static std::string g_log;

    static void logf(const char* fmt, ...) {
        if (g_log.empty()) return;
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        std::string line = std::string(buf) + "\r\n";
        HANDLE h = CreateFileA(g_log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD w;
        WriteFile(h, line.data(), (DWORD)line.size(), &w, nullptr);
        CloseHandle(h);
    }

    static bool exists(const std::string& p) {
        return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    static void makeDirs(const std::string& path) {
        // create every directory component of "path" (a directory path)
        for (size_t i = 0; i < path.size(); i++) {
            if (path[i] == '\\' && i > 2) {
                CreateDirectoryA(path.substr(0, i).c_str(), nullptr);
            }
        }
        CreateDirectoryA(path.c_str(), nullptr);
    }

    static void makeParentDirs(const std::string& file) {
        size_t p = file.find_last_of('\\');
        if (p != std::string::npos) makeDirs(file.substr(0, p));
    }

    static void listFiles(const std::string& root, const std::string& rel, std::vector<std::string>& out) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((root + "\\" + rel + (rel.empty() ? "*" : "\\*")).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            std::string name = fd.cFileName;
            if (name == "." || name == "..") continue;
            std::string r = rel.empty() ? name : rel + "\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                listFiles(root, r, out);
            else
                out.push_back(r);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    static bool copyFile(const std::string& from, const std::string& to) {
        makeParentDirs(to);
        SetFileAttributesA(to.c_str(), FILE_ATTRIBUTE_NORMAL);
        return CopyFileA(from.c_str(), to.c_str(), FALSE) != 0;
    }

    static bool iniOn(const char* ini, const char* key, bool def) {
        char v[32];
        GetPrivateProfileStringA("Mods", key, def ? "on" : "off", v, sizeof(v), ini);
        std::string s = v;
        for (auto& c : s) c = (char)tolower(c);
        return s == "on" || s == "true" || s == "1" || s == "yes";
    }

    static std::string readAll(const std::string& p) {
        std::string r;
        HANDLE h = CreateFileA(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return r;
        char buf[4096];
        DWORD n;
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n) r.append(buf, n);
        CloseHandle(h);
        return r;
    }

    static void writeAll(const std::string& p, const std::string& data) {
        makeParentDirs(p);
        HANDLE h = CreateFileA(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD w;
        WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr);
        CloseHandle(h);
    }

    // signature of a pack folder (file count, total size, newest write time) - a changed pack is re-applied
    static std::string packSignature(const std::string& modDir, const std::vector<std::string>& files) {
        unsigned long long size = 0, newest = 0;
        for (auto& rel : files) {
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (!GetFileAttributesExA((modDir + "\\" + rel).c_str(), GetFileExInfoStandard, &a)) continue;
            size += ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
            unsigned long long t = ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
            if (t > newest) newest = t;
        }
        char buf[96];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "#sig %u %llu %llu", (unsigned)files.size(), size, newest);
        return buf;
    }

    // state file: first line "#sig ...", then one line per touched file "<0|1>|<relative path>"
    // (1 = an original existed and is backed up in mods\_backup\<pack>)
    static bool restorePack(const std::string& game, const std::string& name, const std::string& state) {
        std::string backupDir = game + "\\mods\\_backup\\" + name;
        size_t pos = 0;
        unsigned n = 0, failed = 0;
        while (pos < state.size()) {
            size_t e = state.find('\n', pos);
            if (e == std::string::npos) e = state.size();
            std::string line = state.substr(pos, e - pos);
            pos = e + 1;
            if (line.size() < 3 || line[0] == '#') continue;
            bool had = line[0] == '1';
            std::string rel = line.substr(2);
            std::string dest = game + "\\data\\" + rel;
            if (had) {
                if (!copyFile(backupDir + "\\" + rel, dest)) {
                    logf("[%s] restore failed: %s", name.c_str(), rel.c_str());
                    failed++;
                    continue;
                }
            }
            else {
                DeleteFileA(dest.c_str());
            }
            n++;
        }
        logf("[%s] disabled: %u restored, %u failed", name.c_str(), n, failed);
        return failed == 0;
    }

    static void applyMod(const std::string& game, const std::string& name, bool wanted) {
        std::string modDir = game + "\\mods\\" + name;
        std::string stateFile = game + "\\mods\\_state\\" + name + ".applied";
        std::string backupDir = game + "\\mods\\_backup\\" + name;
        bool hasPack = exists(modDir);
        bool applied = exists(stateFile);
        if (!hasPack && !applied) return;

        std::vector<std::string> files;
        std::string sig;
        if (hasPack) {
            listFiles(modDir, "", files);
            sig = packSignature(modDir, files);
        }

        // switched off, pack folder deleted or pack content changed -> put the originals back first
        if (applied) {
            std::string state = readAll(stateFile);
            std::string oldSig = state.substr(0, state.find('\n'));
            if (!wanted || !hasPack || oldSig != sig) {
                if (!restorePack(game, name, state)) return;   // keep the state file, retry next start
                DeleteFileA(stateFile.c_str());
                applied = false;
            }
        }
        if (!wanted || !hasPack || applied) return;

        logf("[%s] enabling: %u files", name.c_str(), (unsigned)files.size());
        std::string state = sig + "\n";
        unsigned failed = 0;
        for (auto& rel : files) {
            std::string dest = game + "\\data\\" + rel;
            bool had = exists(dest);
            if (had) {
                std::string bk = backupDir + "\\" + rel;
                if (!exists(bk) && !copyFile(dest, bk)) {
                    logf("[%s] backup failed, skipping: %s", name.c_str(), rel.c_str());
                    failed++;
                    continue;
                }
            }
            if (!copyFile(modDir + "\\" + rel, dest)) {
                logf("[%s] copy failed: %s (err %lu)", name.c_str(), rel.c_str(), GetLastError());
                failed++;
                continue;
            }
            state += std::string(had ? "1|" : "0|") + rel + "\n";
        }
        writeAll(stateFile, state);
        logf("[%s] enabled, %u failed", name.c_str(), failed);
    }
    void apply(const char* gameDir, const char* iniPath, const char* logPath) {
        g_log = logPath ? logPath : "";   // nullptr = no log file
        std::string game = gameDir;
        while (!game.empty() && game.back() == '\\') game.pop_back();

        struct { const char* name; const char* key; bool def; } mods[] = {
            { "hd_textures",   "HDTextures",   true },
            { "ui_hd",         "HDInterface",  true },
            { "lod_fix",       "LODFix",       true },
            { "sharp_shadows", "SharpShadows", true },
            { "smooth_shadows", "SmoothShadows", true },
            { "postfx",        "ImprovedPostFX", true },
        };
        DWORD t0 = GetTickCount();
        for (auto& m : mods) {
            applyMod(game, m.name, iniOn(iniPath, m.key, m.def));
        }
        DWORD dt = GetTickCount() - t0;
        if (dt > 500) logf("mod sync took %lu ms", dt);
    }
}
