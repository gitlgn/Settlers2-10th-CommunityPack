#include "GfxPatches.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
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
    void init(const char* iniPath, const char* logPath) {
        g_debug = logPath != nullptr;
        if (g_debug) { g_log = logPath; DeleteFileA(logPath); }
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
