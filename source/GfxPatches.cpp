#include "GfxPatches.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>

namespace GfxPatches {

    static std::string g_log;
    static bool g_debug = false;
    static bool g_pending = false;

    static void logf(const char* fmt, ...) {
        if (g_log.empty()) return;
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        HANDLE h = CreateFileA(g_log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD w;
        WriteFile(h, buf, (DWORD)n, &w, nullptr);
        WriteFile(h, "\r\n", 2, &w, nullptr);
        CloseHandle(h);
    }

    // GOG 11757: both render targets are sized "base << quality level" (level 0..4 from the graphics settings),
    // clamped to the device's maximum texture size. The base is an immediate in "mov esi, 0x80 / shl esi, cl".
    //   00550477  shadow map        (square, R32F or R16F with lowresDepthMap)  -> 2048x2048 at maximum
    //   0055D3CC  reflection target (A8R8G8B8, height = size / 2, water)        -> 2048x1024 at maximum
    struct BasePatch { DWORD addr; const char* key; int scale; };
    static BasePatch g_patches[] = {
        { 0x00550477, "ShadowMapScale",     1 },
        { 0x0055D3CC, "ReflectionMapScale", 1 },
    };
    static const BYTE BASE_BYTES[] = { 0xBE, 0x80, 0x00, 0x00, 0x00, 0xD3, 0xE6 };
    static bool iniOn(const char* iniPath, const char* key, const char* def) {
        char v[16];
        GetPrivateProfileStringA("Graphics", key, def, v, sizeof(v), iniPath);
        return _stricmp(v, "on") == 0 || _stricmp(v, "true") == 0 || strcmp(v, "1") == 0;
    }
    static float iniFloat(const char* iniPath, const char* key, float def, float lo, float hi) {
        char v[16], d[16];
        _snprintf_s(d, sizeof(d), _TRUNCATE, "%.2f", def);
        GetPrivateProfileStringA("Graphics", key, d, v, sizeof(v), iniPath);
        float f = (float)atof(v);
        return f < lo ? lo : f > hi ? hi : f;
    }

    // FXAA / SSAO switches for the post-processing shader of the "postfx" pack (community_postfx.inc):
    // <game>\data\gfx\fx_20\community_settings.inc, written before the game compiles its shaders
    static void writeShaderSettings(const char* iniPath) {
        std::string dir = iniPath;
        for (int i = 0; i < 2; i++) { size_t p = dir.find_last_of('\\'); if (p != std::string::npos) dir.resize(p); }
        std::string path = dir + "\\data\\gfx\\fx_20\\community_settings.inc";
        char text[512];
        _snprintf_s(text, sizeof(text), _TRUNCATE,
            "// written by the Community Pack d3d9.dll from [Graphics] in bin\\d3d9.ini - changes here are overwritten\r\n"
            "#define CP_FXAA %d\r\n#define CP_SSAO %d\r\n#define CP_SSAO_STRENGTH %.2f\r\n#define CP_SSAO_RADIUS %.2f\r\n",
            iniOn(iniPath, "FXAA", "on") ? 1 : 0, iniOn(iniPath, "SSAO", "on") ? 1 : 0,
            iniFloat(iniPath, "SSAOStrength", 0.6f, 0.0f, 1.0f), iniFloat(iniPath, "SSAORadius", 1.0f, 0.25f, 4.0f));
        // rewrite only on change (keeps the file date stable)
        char old[512] = "";
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "rb") == 0 && f) { size_t n = fread(old, 1, sizeof(old) - 1, f); old[n] = 0; fclose(f); }
        if (strcmp(old, text) == 0) return;
        if (fopen_s(&f, path.c_str(), "wb") == 0 && f) { fputs(text, f); fclose(f); logf("shader settings written: %s", path.c_str()); }
        else logf("cannot write %s", path.c_str());
    }

    void init(const char* iniPath, const char* logPath) {
        g_debug = logPath != nullptr;
        if (g_debug) { g_log = logPath; DeleteFileA(logPath); }
        writeShaderSettings(iniPath);
        for (auto& p : g_patches) {
            p.scale = GetPrivateProfileIntA("Graphics", p.key, 1, iniPath);
            if (p.scale != 2 && p.scale != 4) p.scale = 1;
        }
        g_pending = true;
    }

    void onDeviceCreated() {
        if (!g_pending) return;
        g_pending = false;
        for (auto& p : g_patches) {
            if (p.scale == 1) continue;
            if (memcmp((void*)p.addr, BASE_BYTES, sizeof(BASE_BYTES)) != 0) {
                logf("%s: code not found (unsupported exe build)", p.key);
                continue;
            }
            DWORD base = 0x80 * p.scale, old;
            if (VirtualProtect((void*)(p.addr + 1), 4, PAGE_EXECUTE_READWRITE, &old)) {
                *(DWORD*)(p.addr + 1) = base;
                VirtualProtect((void*)(p.addr + 1), 4, old, &old);
                FlushInstructionCache(GetCurrentProcess(), (void*)p.addr, 5);
                logf("%s %d: base size 0x80 -> 0x%lX", p.key, p.scale, base);
            }
        }
    }
    // ---- diagnostics: which render target is bound as shadow map (sampler 5)? ----
    struct RT { UINT w, h; D3DFORMAT fmt; };
    static std::map<void*, RT> g_rts;
    static std::set<void*> g_seenOnStage5;
    static unsigned g_frames = 0;

    void onCreateTexture(UINT w, UINT h, DWORD usage, D3DFORMAT fmt, void* tex) {
        if (!g_debug || !(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) return;
        g_rts[tex] = { w, h, fmt };
        logf("render target texture %p: %ux%u fmt %d usage %lX", tex, w, h, (int)fmt, usage);
    }

    void onSetTexture(DWORD stage, void* tex) {
        if (!g_debug || stage != 5 || !tex) return;
        if (g_seenOnStage5.insert(tex).second) {
            auto it = g_rts.find(tex);
            if (it != g_rts.end())
                logf("shadow sampler (stage 5) uses render target %p: %ux%u fmt %d", tex, it->second.w, it->second.h, (int)it->second.fmt);
            else
                logf("shadow sampler (stage 5) uses texture %p (not a render target)", tex);
        }
    }

    void onPresent() { g_frames++; }
}
