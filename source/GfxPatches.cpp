#include "GfxPatches.h"
#include "FixDecisions.h"

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
        float f = FixDecisions::parseIniFloat(v, def);
        return f < lo ? lo : f > hi ? hi : f;
    }

    // FXAA / SSAO switches for the post-processing shader of the "postfx" pack (community_postfx.inc):
    // <game>\data\gfx\fx_20\community_settings.inc, written before the game compiles its shaders
    static void writeShaderSettings(const char* iniPath) {
        std::string dir = iniPath;
        for (int i = 0; i < 2; i++) { size_t p = dir.find_last_of('\\'); if (p != std::string::npos) dir.resize(p); }
        std::string path = dir + "\\data\\gfx\\fx_20\\community_settings.inc";
        char text[1024];
        _snprintf_s(text, sizeof(text), _TRUNCATE,
            "// written by the Community Pack d3d9.dll from [Graphics] in bin\\d3d9.ini - changes here are overwritten\r\n"
            "// shader interface 3: the effects take these settings from live constants (community_live.inc), bloom\r\n"
            "#define CP_FXAA %d\r\n#define CP_SSAO %d\r\n#define CP_SSAO_STRENGTH %.2f\r\n#define CP_SSAO_RADIUS %.2f\r\n"
            "#define CP_DOF_STRENGTH %.2f\r\n#define CP_TERRAIN %d\r\n#define CP_TERRAIN_DETAIL %.2f\r\n#define CP_WATER %d\r\n#define CP_OBJECTS %d\r\n#define CP_UNIT_RIM %.2f\r\n",
            iniOn(iniPath, "FXAA", "on") ? 1 : 0, iniOn(iniPath, "SSAO", "on") ? 1 : 0,
            iniFloat(iniPath, "SSAOStrength", 0.6f, 0.0f, 1.0f), iniFloat(iniPath, "SSAORadius", 1.0f, 0.25f, 4.0f),
            iniFloat(iniPath, "DOFStrength", 0.3f, 0.0f, 1.0f),
            iniOn(iniPath, "ImprovedTerrain", "on") ? 1 : 0, iniFloat(iniPath, "TerrainDetail", 0.5f, 0.0f, 1.0f),
            iniOn(iniPath, "ImprovedWater", "on") ? 1 : 0, iniOn(iniPath, "ImprovedObjects", "on") ? 1 : 0,
            iniFloat(iniPath, "UnitRimLight", 0.5f, 0.0f, 1.0f));
        // rewrite only on change (keeps the file date stable)
        char old[1024] = "";
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "rb") == 0 && f) { size_t n = fread(old, 1, sizeof(old) - 1, f); old[n] = 0; fclose(f); }
        if (strcmp(old, text) == 0) return;
        if (fopen_s(&f, path.c_str(), "wb") == 0 && f) { fputs(text, f); fclose(f); logf("shader settings written: %s", path.c_str()); }
        else { logf("cannot write %s", path.c_str()); return; }
        // the game caches compiled shaders by file name (%LOCALAPPDATA%\S2\postprocess.fxPS_*) and would keep the
        // old settings: drop the cached post-processing shader so it is compiled again with the new include
        char local[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH);
        if (!n || n >= MAX_PATH) return;
        std::string cache = std::string(local) + "\\S2\\";
        // every shader includes community_settings.inc (through shared.inc / community_objects.inc)
        for (const char* name : { "*.fx*" }) {
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA((cache + name).c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (DeleteFileA((cache + fd.cFileName).c_str())) logf("cached shader removed: %s", fd.cFileName);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }

    // The game's post-processing (bloom, depth of field - and the pack's FXAA/SSAO in its combine pass) only runs
    // with the launch parameter -postFX, which neither GOG nor the shortcuts pass. The parameter only sets a flag in
    // the graphics settings object (0x6C4A10 + 0x0C, set at 0x405DC4); the game reads it when a map is loaded.
    static const DWORD SETTINGS_GUARD = 0x006C4A5C, SETTINGS_POSTFX = 0x006C4A1C, SETTINGS_VTABLE = 0x00662688;
    static int g_postfx = -1;     // -1 = leave as the command line set it
    static bool g_fog = false;
    bool distanceFog() { return g_fog; }
    void setDistanceFog(bool on) { g_fog = on; }

    // ---- live shader settings (community_live.inc) ----
    struct LiveSlot { bool vertex; unsigned reg; float marker; const char* keys[3]; float v[3]; };
    static LiveSlot g_live[] = {
        { false, 28, 7771.0f, { "FXAA", "SSAO", "SSAOStrength" },                   { 1.0f, 1.0f, 0.6f } },
        { false, 29, 7772.0f, { "SSAORadius", "DOFStrength", "ImprovedTerrain" },  { 1.0f, 0.3f, 1.0f } },
        { false, 30, 7773.0f, { "TerrainDetail", "ImprovedWater", "ImprovedObjects" }, { 0.5f, 1.0f, 1.0f } },
        { false, 31, 7774.0f, { "UnitRimLight", "Bloom", "BloomThreshold" },        { 0.5f, 0.0f, 0.75f } },
        { true, 249, 7775.0f, { "UnitRimLight", nullptr, nullptr },                 { 0.5f, 0.0f, 0.0f } },
    };

    static void loadLive(const char* iniPath) {
        for (auto& s : g_live)
            for (int i = 0; i < 3; i++) {
                const char* k = s.keys[i];
                if (!k) continue;
                char v[32];
                GetPrivateProfileStringA("Graphics", k, "", v, sizeof(v), iniPath);
                if (!v[0]) continue;
                if (!_stricmp(v, "on") || !_stricmp(v, "true")) s.v[i] = 1.0f;
                else if (!_stricmp(v, "off") || !_stricmp(v, "false")) s.v[i] = 0.0f;
                else s.v[i] = FixDecisions::parseIniFloat(v, s.v[i]);
            }
    }

    // comparison shots: the community shader effects switched to the original game's look (switches off; no depth of
    // field, since the GOG game starts without post-processing), without touching the user's settings
    static bool g_vanilla = false;
    static float vanillaValue(const char* key, float v) {
        if (!key) return v;
        static const char* off[] = { "FXAA", "SSAO", "ImprovedTerrain", "ImprovedWater", "ImprovedObjects", "UnitRimLight", "DOFStrength", "Bloom" };
        for (const char* k : off) if (!_stricmp(k, key)) return 0.0f;
        return v;
    }

    bool setLive(const char* key, float value) {
        bool found = false;
        for (auto& s : g_live)
            for (int i = 0; i < 3; i++)
                if (s.keys[i] && !_stricmp(s.keys[i], key)) { s.v[i] = value; found = true; }
        return found;
    }

    const float* liveConstants(bool vertex, unsigned start, const float* data, unsigned count, float* scratch) {
        if (!data || !count) return data;
        const float* out = data;
        for (auto& s : g_live) {
            if (s.vertex != vertex || s.reg < start || s.reg >= start + count) continue;
            unsigned o = (s.reg - start) * 4;
            if (data[o + 3] != s.marker) continue;                 // another shader's constant: leave it
            if (out == data) {
                if (count > 64) return data;                        // scratch holds 64 registers
                memcpy(scratch, data, count * 16);
                out = scratch;
            }
            for (int i = 0; i < 3; i++) scratch[o + i] = g_vanilla ? vanillaValue(s.keys[i], s.v[i]) : s.v[i];
        }
        return out;
    }

    void setVanillaShaders(bool on) { g_vanilla = on; }

    void init(const char* iniPath, const char* logPath) {
        g_debug = logPath != nullptr;
        {
            char v[16];
            GetPrivateProfileStringA("Graphics", "PostFX", "on", v, sizeof(v), iniPath);
            g_fog = iniOn(iniPath, "DistanceFog", "off");
            g_postfx = (_stricmp(v, "on") == 0 || _stricmp(v, "true") == 0 || strcmp(v, "1") == 0) ? 1
                     : (_stricmp(v, "off") == 0 || _stricmp(v, "false") == 0 || strcmp(v, "0") == 0) ? 0 : -1;
        }
        if (g_debug) { g_log = logPath; DeleteFileA(logPath); }
        writeShaderSettings(iniPath);
        loadLive(iniPath);
        for (auto& p : g_patches) {
            p.scale = GetPrivateProfileIntA("Graphics", p.key, 1, iniPath);
            if (p.scale != 2 && p.scale != 4) p.scale = 1;
        }
        g_pending = true;
    }

    void onDeviceCreated() {
        if (!g_pending) return;
        g_pending = false;
        if (g_postfx >= 0) {
            // settings object constructed (guard bit) and really the graphics settings (vtable)
            if ((*(volatile DWORD*)SETTINGS_GUARD & 1) && *(volatile DWORD*)(SETTINGS_POSTFX - 0x0C) == SETTINGS_VTABLE) {
                BYTE before = *(volatile BYTE*)SETTINGS_POSTFX;
                *(volatile BYTE*)SETTINGS_POSTFX = (BYTE)g_postfx;
                logf("post effects (-postFX): %s (was %s)", g_postfx ? "on" : "off", before ? "on" : "off");
            } else logf("post effects: graphics settings object not found - left unchanged");
        }
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

    // ---- depth for the post-processing combine pass ----
    // The scene writes the linear view depth into a second R32F render target, but the engine binds it only for the
    // bloom pass (stage 1). The final combine pass (blur on stage 0, scene colour on stage 1) samples Texture2, which
    // never held the depth: the game's depth of field blurred the whole picture evenly and SSAO had nothing to work
    // with. When the scene colour target goes to stage 1, the depth target is bound to stage 2 as well.
    struct Size { UINT w, h; };
    static std::map<void*, Size> g_sceneRTs;           // colour render target textures by size
    static IDirect3DTexture9* g_depthTex = nullptr;    // the R32F depth target (no reference held)
    static Size g_depthSize = {};
    static IDirect3DBaseTexture9* g_game2 = nullptr;   // what the game last bound to stage 2
    static bool g_depthBound = false;

    static void trackSceneTarget(UINT w, UINT h, DWORD usage, D3DFORMAT fmt, void* tex) {
        if (!(usage & D3DUSAGE_RENDERTARGET)) return;
        if (fmt == D3DFMT_R32F) {
            // the scene depth has the size of a colour target created before it (the shadow map is R32F on some GPUs)
            bool scene = false;
            for (auto& c : g_sceneRTs) if (c.second.w == w && c.second.h == h) { scene = true; break; }
            if (!scene) return;
            g_depthTex = (IDirect3DTexture9*)tex;
            g_depthSize = { w, h };
            logf("depth target %ux%u: bound to stage 2 for the post-processing combine pass", w, h);
        } else g_sceneRTs[tex] = { w, h };
    }

    // Alpha-blended objects (roads, flags, smoke, decals) would blend their depth into the depth target as well - with
    // the depth itself as alpha, which gives garbage. While blending is on, the second render target is write-protected,
    // so the depth keeps what lies behind them (as transparent objects should).
    static bool g_blend = false;
    static DWORD g_writeMask1 = 0xF;   // what the game set for D3DRS_COLORWRITEENABLE1

    bool filterRenderState(IDirect3DDevice9* dev, D3DRENDERSTATETYPE state, DWORD& value) {
        if (state == D3DRS_COLORWRITEENABLE1) {
            g_writeMask1 = value;
            if (g_blend) value = 0;
            return false;
        }
        if (state == D3DRS_ALPHABLENDENABLE) {
            g_blend = value != FALSE;
            return true;   // the caller applies the write mask after the blend state
        }
        return false;
    }

    void applyWriteMask1(IDirect3DDevice9* dev) {
        dev->SetRenderState(D3DRS_COLORWRITEENABLE1, g_blend ? 0 : g_writeMask1);
    }

    void beforeReset() {
        g_blend = false;
        g_writeMask1 = 0xF;
        g_sceneRTs.clear();
        g_depthTex = nullptr;
        g_game2 = nullptr;
        g_depthBound = false;
    }

    void afterSetTexture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* tex) {
        if (stage == 2) { g_game2 = tex; g_depthBound = false; return; }
        if (stage != 1 || !g_depthTex) return;
        auto it = tex ? g_sceneRTs.find(tex) : g_sceneRTs.end();
        bool combine = it != g_sceneRTs.end() && it->second.w == g_depthSize.w && it->second.h == g_depthSize.h;
        if (combine) {
            dev->SetTexture(2, g_depthTex);
            g_depthBound = true;
        } else if (g_depthBound) {
            dev->SetTexture(2, g_game2);
            g_depthBound = false;
        }
    }

    void restoreStage2(IDirect3DDevice9* dev) {
        if (!g_depthBound) return;
        dev->SetTexture(2, g_game2);
        g_depthBound = false;
    }

    void onCreateTexture(UINT w, UINT h, DWORD usage, D3DFORMAT fmt, void* tex) {
        trackSceneTarget(w, h, usage, fmt, tex);
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
