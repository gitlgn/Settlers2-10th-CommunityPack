#include "UIScale.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <map>
#include <string>

#include "MainPatchASM.h"
#include "uiscale_vs.h"
#include "uiscale_vs_nouv.h"
#include "uiscale_vs_pos.h"

namespace UIScale {

    // ---- GOG build 11757: function that copies the renderer resolution into the HUD globals ----
    //   004F4D3C  8B 08              mov ecx, [eax]
    //   004F4D3E  89 0D 70 F3 6B 00  mov [006BF370], ecx
    //   004F4D44  8B 50 04           mov edx, [eax+4]
    //   004F4D47  89 15 74 F3 6B 00  mov [006BF374], edx
    //   004F4D4D  C3                 ret
    static const DWORD HOOK_ADDR = 0x004F4D3C;
    static const BYTE  HOOK_BYTES[] = { 0x8B, 0x08, 0x89, 0x0D, 0x70, 0xF3, 0x6B, 0x00, 0x8B, 0x50, 0x04 };
    static int* const  UI_WIDTH  = (int*)0x006BF370;
    static int* const  UI_HEIGHT = (int*)0x006BF374;

    static std::string g_log;
    static bool  g_on = false;
    static bool  g_pending = false;
    static bool  g_linear = true;    // [UI] Filter = linear | point
    static float g_scale = 1.0f;

    // real and virtual HUD size
    static volatile LONG g_realW = 0, g_realH = 0, g_virtW = 0, g_virtH = 0;

    // device state tracking
    static IDirect3DVertexDeclaration9* g_curDecl = nullptr;
    static IDirect3DVertexShader9* g_curVS = nullptr;
    static DWORD g_curFVF = 0;
    static bool g_declIsCurrent = false;



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

    bool enabled() { return g_on; }

    // ---------------- virtual HUD size ----------------

    static void __stdcall applySize(const int* wh) {
        int w = wh[0], h = wh[1];
        // never go below the 640x480 the HUD was designed for; one factor for both axes (no distortion)
        float s = g_scale;
        if (s > w / 640.0f) s = w / 640.0f;
        if (s > h / 480.0f) s = h / 480.0f;
        if (s < 1.0f) s = 1.0f;
        int vw = (int)(w / s + 0.5f);
        int vh = (int)(h / s + 0.5f);
        if (vw > w) vw = w;
        if (vh > h) vh = h;
        InterlockedExchange(&g_realW, w);
        InterlockedExchange(&g_realH, h);
        InterlockedExchange(&g_virtW, vw);
        InterlockedExchange(&g_virtH, vh);
        *UI_WIDTH = vw;
        *UI_HEIGHT = vh;
        logf("HUD size: real %dx%d -> virtual %dx%d", w, h, vw, vh);
    }

    // jumped to from 004F4D3C, eax = pointer to {width, height}; finishes the original function (ret)
    static void __declspec(naked) sizeHook() {
        __asm {
            push eax
            call applySize
            ret
        }
    }

    static bool patchSetter() {
        if (memcmp((void*)HOOK_ADDR, HOOK_BYTES, sizeof(HOOK_BYTES)) != 0) {
            const BYTE* p = (const BYTE*)HOOK_ADDR;
            logf("size setter not found (bytes %02X %02X %02X %02X %02X %02X %02X %02X) - HUD scaling disabled",
                p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
            return false;
        }
        DWORD old;
        if (!VirtualProtect((void*)HOOK_ADDR, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
        BYTE* p = (BYTE*)HOOK_ADDR;
        p[0] = 0xE9;
        *(DWORD*)(p + 1) = (DWORD)sizeHook - (HOOK_ADDR + 5);
        VirtualProtect((void*)HOOK_ADDR, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)HOOK_ADDR, 5);
        return true;
    }

    // ---------------- mouse ----------------
    // No conversion needed: the game normalises GetCursorPos by the real client size (GetClientRect,
    // code at 004F0F40) and multiplies with the HUD size afterwards, so the cursor follows the virtual HUD.

    // real / virtual HUD pixels
    static float sx() { LONG v = g_virtW; return v ? (float)g_realW / (float)v : 1.0f; }
    static float sy() { LONG v = g_virtH; return v ? (float)g_realH / (float)v : 1.0f; }

    // ---------------- upscaled HUD textures ----------------
    // <game>\mods\ui_hd.sizes.txt lists "relative\path.dds width height" of the original files. The game builds
    // HUD texture coordinates from pixel rectangles divided by the REAL texture size, so for an upscaled texture
    // the HUD vertex shader multiplies them by real/original (c253).
    struct Factor { float x, y; };
    static std::map<std::string, std::pair<UINT, UINT>> g_hdOrig;   // lower-case path below data\ -> original size
    static std::map<void*, Factor> g_hdTex;                          // texture -> uv factor
    static void* g_curTex0 = nullptr;
    static bool g_scaleOn = false;

    static std::string normPath(const char* p) {
        std::string s = p ? p : "";
        for (auto& c : s) { c = (char)tolower((unsigned char)c); if (c == '/') c = '\\'; }
        // the game builds some paths with doubled separators ("data\\gfx\\uiframe.dds")
        s.erase(std::unique(s.begin(), s.end(), [](char a, char b) { return a == '\\' && b == '\\'; }), s.end());
        if (s.compare(0, 5, "data\\") == 0) s = s.substr(5);
        return s;
    }

    static void loadManifest(const std::string& path) {
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "r") != 0 || !f) return;
        char name[MAX_PATH];
        unsigned w, h;
        while (fscanf_s(f, "%259s %u %u", name, (unsigned)sizeof(name), &w, &h) == 3)
            g_hdOrig[normPath(name)] = { w, h };
        fclose(f);
        logf("upscaled HUD textures listed: %u", (unsigned)g_hdOrig.size());
    }

    void onCreateTexture(UINT w, UINT h, DWORD usage, D3DPOOL pool, void* tex) {
        if (g_hdOrig.empty() || !tex) return;
        g_hdTex.erase(tex);   // the address may belong to a released texture
        // file textures only: no render targets / dynamic textures, no system-memory staging copies
        if ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC)) || pool == D3DPOOL_SYSTEMMEM) return;
        std::string file = normPath(DecryptPatch::lastLoadedFile());
        DecryptPatch::clearLastLoadedFile();   // a file name is used for one texture only
        auto it = g_hdOrig.find(file);
        if (it == g_hdOrig.end()) return;
        if (w == it->second.first && h == it->second.second) return;
        g_hdTex[tex] = { (float)w / it->second.first, (float)h / it->second.second };
        ((IUnknown*)tex)->AddRef();   // keep it alive so the address can never be reused by another texture
        logf("upscaled HUD texture %s: %ux%u (original %ux%u)", it->first.c_str(), w, h, it->second.first, it->second.second);
    }

    void trackTexture(DWORD stage, void* tex) { if (stage == 0) g_curTex0 = tex; }

    static IDirect3DVertexBuffer9* g_vb = nullptr;
    static UINT g_vbOffset = 0, g_vbStride = 0;
    void trackStream(UINT n, IDirect3DVertexBuffer9* vb, UINT offset, UINT stride) {
        if (n == 0) { g_vb = vb; g_vbOffset = offset; g_vbStride = stride; }
    }

    UINT vertexCount(D3DPRIMITIVETYPE type, UINT n) {
        switch (type) {
        case D3DPT_POINTLIST: return n;
        case D3DPT_LINELIST: return n * 2;
        case D3DPT_LINESTRIP: return n + 1;
        case D3DPT_TRIANGLELIST: return n * 3;
        case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return n + 2;
        default: return 0;
        }
    }

    // extents of the draw: largest x/y position (bytes 0/4) and, for layout V_UV, largest texture coordinate (byte 20)
    struct Bounds { float maxX, maxY, maxUV; bool valid; };
    static Bounds drawBounds(const DrawRange& r, bool hasUV) {
        const UINT UV = 20;
        Bounds b = { -1e30f, -1e30f, -1.0f, false };
        UINT count = r.count > 4096 ? 4096 : r.count;
        const BYTE* p = nullptr;
        UINT stride = 0;
        bool locked = false;
        if (r.upData) {
            p = (const BYTE*)r.upData + r.first * r.upStride;
            stride = r.upStride;
        } else if (g_vb && g_vbStride >= 16) {
            void* data = nullptr;
            stride = g_vbStride;
            if (SUCCEEDED(g_vb->Lock(g_vbOffset + r.first * stride, count * stride, &data, D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) && data) {
                p = (const BYTE*)data;
                locked = true;
            }
        }
        if (p && stride >= 16 && count) {
            for (UINT i = 0; i < count; i++) {
                const float* v = (const float*)(p + i * stride);
                if (v[0] > b.maxX) b.maxX = v[0];
                if (v[1] > b.maxY) b.maxY = v[1];
                if (hasUV && stride >= UV + 8) {
                    const float* uv = (const float*)(p + i * stride + UV);
                    if (uv[0] > b.maxUV) b.maxUV = uv[0];
                    if (uv[1] > b.maxUV) b.maxUV = uv[1];
                }
            }
            b.valid = true;
        }
        if (locked) g_vb->Unlock();
        return b;
    }

    // ---------------- init ----------------

    void init(const char* iniPath, const char* logPath) {
        g_log = logPath ? logPath : "";   // nullptr = no log file
        if (logPath) DeleteFileA(logPath);

        char v[32];
        GetPrivateProfileStringA("UI", "Scale", "1.0", v, sizeof(v), iniPath);
        g_scale = (float)atof(v);
        g_scaleOn = g_scale > 1.01f && g_scale <= 3.0f;
        if (!g_scaleOn) g_scale = 1.0f;
        GetPrivateProfileStringA("UI", "Filter", "linear", v, sizeof(v), iniPath);
        g_linear = _stricmp(v, "point") != 0;

        GetPrivateProfileStringA("Mods", "HDInterface", "on", v, sizeof(v), iniPath);
        if (_stricmp(v, "on") == 0 || _stricmp(v, "true") == 0 || strcmp(v, "1") == 0) {
            std::string dir = iniPath;                       // <game>\bin\d3d9.ini -> <game>\mods\ui_hd.sizes.txt
            for (int i = 0; i < 2; i++) { size_t p = dir.find_last_of('\\'); if (p != std::string::npos) dir.resize(p); }
            loadManifest(dir + "\\mods\\ui_hd.sizes.txt");
        }

        if (!g_scaleOn && g_hdOrig.empty()) return;
        // the game code is still UPX-packed while DllMain runs; the patches follow in onDeviceCreated()
        g_pending = true;
        logf("HUD scaling %.2f, filter %s", g_scale, g_linear ? "linear" : "point");
    }

    // ---- world -> screen projection (GOG 11757: 0x509E60, thiscall bool(vec3* out, const vec3* in)) ----
    // D3DXVec3Project with the camera's REAL viewport. Its callers (building labels, markers, screen-distance
    // tests against the mouse) all work in HUD coordinates, which are virtual while scaling is on.
    static const DWORD PROJECT_ADDR = 0x00509E60;
    static const BYTE  PROJECT_BYTES[] = { 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x8B, 0xF1 };
    static BYTE* g_projectTramp = nullptr;   // original first 6 bytes + jmp back

    static void __cdecl projectToVirtual(float* out) {
        LONG vw = g_virtW, vh = g_virtH;
        if (!vw || !vh) return;
        out[0] *= (float)vw / (float)g_realW;
        out[1] *= (float)vh / (float)g_realH;
    }

    static void __declspec(naked) projectHook() {
        __asm {
            push dword ptr [esp + 8]      // in
            push dword ptr [esp + 8]      // out
            call g_projectTramp           // original (ecx = this), ret 8
            test al, al
            jz done
            push eax
            push dword ptr [esp + 8]      // out
            call projectToVirtual
            add esp, 4
            pop eax
        done:
            ret 8
        }
    }

    static bool patchProjection() {
        if (memcmp((void*)PROJECT_ADDR, PROJECT_BYTES, sizeof(PROJECT_BYTES)) != 0) {
            logf("world projection not found - building labels keep real-pixel positions");
            return false;
        }
        BYTE* t = (BYTE*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!t) return false;
        memcpy(t, PROJECT_BYTES, 6);                                   // push esi; push edi; mov edi,[esp+0Ch]
        t[6] = 0xE9;
        *(DWORD*)(t + 7) = (PROJECT_ADDR + 6) - ((DWORD)t + 11);
        g_projectTramp = t;
        DWORD old;
        if (!VirtualProtect((void*)PROJECT_ADDR, 6, PAGE_EXECUTE_READWRITE, &old)) return false;
        BYTE* p = (BYTE*)PROJECT_ADDR;
        p[0] = 0xE9;
        *(DWORD*)(p + 1) = (DWORD)projectHook - (PROJECT_ADDR + 5);
        p[5] = 0x90;
        VirtualProtect((void*)PROJECT_ADDR, 6, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)PROJECT_ADDR, 6);
        logf("world projection hooked (labels in HUD coordinates)");
        return true;
    }

    static void installHooks() {
        if (g_scaleOn && !patchSetter()) g_scaleOn = false;
        if (g_scaleOn) patchProjection();
        g_on = g_scaleOn || !g_hdOrig.empty();
        if (g_on) logf("HUD hooks enabled (scaling %s, upscaled textures %u)", g_scaleOn ? "on" : "off", (unsigned)g_hdOrig.size());
    }

    void onDeviceCreated(IDirect3DDevice9* dev, HWND hwnd) {
        if (g_pending) {
            g_pending = false;
            installHooks();
        }
        if (!g_on) return;
        // the size setter may have run before we patched it
        if (g_scaleOn && *UI_WIDTH > 0 && !g_virtW) {
            int wh[2] = { *UI_WIDTH, *UI_HEIGHT };
            applySize(wh);
        }
    }

    // ---------------- draw interception ----------------

    void trackDecl(IDirect3DVertexDeclaration9* decl) { g_curDecl = decl; g_declIsCurrent = true; }
    void trackFVF(DWORD fvf) { g_curFVF = fvf; g_declIsCurrent = false; }
    void trackVS(IDirect3DVertexShader9* vs) { g_curVS = vs; }

    // the game's values of the registers the HUD shader uses (c253..c255), restored after every HUD draw
    static float g_vsConst[3][4] = {};
    void trackVSConst(UINT start, const float* data, UINT count) {
        if (!data) return;
        for (UINT i = 0; i < count; i++) {
            UINT reg = start + i;
            if (reg >= 253 && reg <= 255) memcpy(g_vsConst[reg - 253], data + i * 4, sizeof(float) * 4);
        }
    }

    // last Min/Mag filter the game set for stages 0 and 1 (pure device: no GetSamplerState)
    static DWORD g_filter[2][2] = { { D3DTEXF_POINT, D3DTEXF_POINT }, { D3DTEXF_POINT, D3DTEXF_POINT } };
    void trackSampler(DWORD stage, D3DSAMPLERSTATETYPE type, DWORD value) {
        if (stage > 1) return;
        if (type == D3DSAMP_MAGFILTER) g_filter[stage][0] = value;
        else if (type == D3DSAMP_MINFILTER) g_filter[stage][1] = value;
    }

    // HUD vertex layouts (all pre-transformed, stream 0):
    //   V_UV   POSITIONT f4 @0, COLOR0 @16, TEXCOORD0 f2 @20  (stride 28, textured elements and fonts)
    //   V_COL  POSITIONT f4 @0, COLOR0 @16                    (stride 20)
    //   V_POS  POSITIONT f4 @0                                (stride 16, lines/frames, FVF XYZRHW)
    enum Variant { V_NONE = -1, V_UV = 0, V_COL = 1, V_POS = 2 };
    struct Repl { IDirect3DVertexDeclaration9* decl; int variant; };

    static Variant classify(const D3DVERTEXELEMENT9* el, UINT n) {
        // n includes D3DDECL_END
        if (n < 2 || el[0].Stream != 0 || el[0].Offset != 0 || el[0].Type != D3DDECLTYPE_FLOAT4 || el[0].Usage != D3DDECLUSAGE_POSITIONT)
            return V_NONE;
        bool col = n >= 3 && el[1].Stream == 0 && el[1].Offset == 16 && el[1].Type == D3DDECLTYPE_D3DCOLOR &&
                   el[1].Usage == D3DDECLUSAGE_COLOR && el[1].UsageIndex == 0;
        bool uv = col && n >= 4 && el[2].Stream == 0 && el[2].Offset == 20 && el[2].Type == D3DDECLTYPE_FLOAT2 &&
                  el[2].Usage == D3DDECLUSAGE_TEXCOORD && el[2].UsageIndex == 0;
        if (n == 2) return V_POS;
        if (n == 3 && col) return V_COL;
        if (n == 4 && uv) return V_UV;
        return V_NONE;
    }

    static Repl makeRepl(IDirect3DDevice9* dev, D3DVERTEXELEMENT9* el, UINT n, const char* what) {
        Repl r = { nullptr, classify(el, n) };
        if (r.variant == V_NONE) {
            if (n >= 2 && el[0].Usage == D3DDECLUSAGE_POSITIONT)
                logf("pre-transformed layout not scaled (%s, %u elements)", what, n - 1);
            return r;
        }
        el[0].Usage = D3DDECLUSAGE_POSITION;
        if (FAILED(dev->CreateVertexDeclaration(el, &r.decl))) { r.decl = nullptr; r.variant = V_NONE; }
        logf("HUD layout %d found (%s)", r.variant, what);
        return r;
    }

    static std::map<IDirect3DVertexDeclaration9*, Repl> g_declRepl;
    static std::map<DWORD, Repl> g_fvfRepl;

    static Repl replacementForDecl(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* decl) {
        auto it = g_declRepl.find(decl);
        if (it != g_declRepl.end()) return it->second;
        D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
        UINT n = MAXD3DDECLLENGTH + 1;
        Repl r = { nullptr, V_NONE };
        if (SUCCEEDED(decl->GetDeclaration(el, &n))) r = makeRepl(dev, el, n, "declaration");
        decl->AddRef();   // cached by address: keep it alive so the address cannot be reused
        g_declRepl[decl] = r;
        return r;
    }

    static Repl replacementForFVF(IDirect3DDevice9* dev, DWORD fvf) {
        auto it = g_fvfRepl.find(fvf);
        if (it != g_fvfRepl.end()) return it->second;
        Repl r = { nullptr, V_NONE };
        if ((fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW) {
            D3DVERTEXELEMENT9 el[4];
            UINT n = 0;
            el[n++] = { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0 };
            if (fvf == (D3DFVF_XYZRHW | D3DFVF_DIFFUSE) || fvf == (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1))
                el[n++] = { 0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 };
            if (fvf == (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1))
                el[n++] = { 0, 20, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 };
            el[n++] = D3DDECL_END();
            char what[32];
            _snprintf_s(what, sizeof(what), _TRUNCATE, "FVF %08lX", fvf);
            bool known = fvf == D3DFVF_XYZRHW || fvf == (D3DFVF_XYZRHW | D3DFVF_DIFFUSE) ||
                         fvf == (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
            if (known) r = makeRepl(dev, el, n, what);
            else logf("pre-transformed layout not scaled (%s)", what);
        }
        g_fvfRepl[fvf] = r;
        return r;
    }

    static IDirect3DVertexShader9* g_vsVariant[3] = { nullptr, nullptr, nullptr };
    static bool g_viaFVF = false;

    bool begin(IDirect3DDevice9* dev, const DrawRange& range) {
        if (!g_on || g_curVS) return false;
        auto hd = g_hdTex.find(g_curTex0);
        bool isHD = hd != g_hdTex.end();
        if (!g_scaleOn) {
            if (!isHD) return false;
            g_realW = g_virtW = *UI_WIDTH;          // no virtual HUD: draw 1:1
            g_realH = g_virtH = *UI_HEIGHT;
        }
        if (!g_virtW) return false;

        Repl r = { nullptr, V_NONE };
        if (g_declIsCurrent) {
            if (!g_curDecl) return false;
            r = replacementForDecl(dev, g_curDecl);
        } else {
            r = replacementForFVF(dev, g_curFVF);
        }
        if (r.variant == V_NONE || !r.decl) return false;

        IDirect3DVertexShader9*& vs = g_vsVariant[r.variant];
        if (!vs) {
            const BYTE* code = r.variant == V_UV ? g_UIScaleVS : r.variant == V_COL ? g_UIScaleVSNoUV : g_UIScaleVSPos;
            if (FAILED(dev->CreateVertexShader((const DWORD*)code, &vs))) {
                logf("CreateVertexShader (layout %d) failed - HUD scaling disabled", r.variant);
                g_on = false;
                vs = nullptr;
                return false;
            }
            logf("HUD vertex shader %d created", r.variant);
        }

        Bounds bd = drawBounds(range, r.variant == V_UV);

        // The game frame is positioned with the REAL resolution and its pieces end exactly at the right/bottom
        // screen edge -> draw those 1:1. Other elements beyond the virtual HUD (dialogs sliding in, partly
        // off-screen windows) stay virtual and are simply clipped.
        float px = sx(), py = sy();
        if (g_scaleOn && bd.valid && (bd.maxX > g_virtW + 2.0f || bd.maxY > g_virtH + 2.0f) &&
            (fabsf(bd.maxX - (float)g_realW) <= 2.0f || fabsf(bd.maxY - (float)g_realH) <= 2.0f)) { px = 1.0f; py = 1.0f; }
        if (!g_log.empty()) {
            // debug: one line per distinct HUD element (texture, layout, extent)
            static std::map<std::string, bool> seen;
            char key[128];
            _snprintf_s(key, sizeof(key), _TRUNCATE, "%p/%d/%d/%d", g_curTex0, r.variant, (int)(bd.maxX / 16), (int)(bd.maxY / 16));
            if (seen.emplace(key, true).second && seen.size() < 2000)
                logf("draw tex=%p hd=%d layout=%d n=%u max=(%.0f,%.0f) uv=%.3f -> %s", g_curTex0, isHD ? 1 : 0, r.variant,
                    range.count, bd.maxX, bd.maxY, bd.maxUV, (px == 1.0f && py == 1.0f) ? "real pixels" : "virtual");
        }
        if (!isHD && px == 1.0f && py == 1.0f) return false;   // nothing to change for this draw

        // pixel rectangles were divided by the real (upscaled) size: an atlas cut-out stays within 1/factor,
        // a tiled border (pixels / size, wrapping) goes beyond 1 -> both need the correction.
        // Whole-image draws use 0..1 and must stay as they are.
        float fx = 1.0f, fy = 1.0f;
        if (isHD && r.variant == V_UV && bd.valid) {
            float m = bd.maxUV;
            float limit = 1.0f / (hd->second.x > hd->second.y ? hd->second.x : hd->second.y) + 0.002f;
            bool wholeImage = m > limit && m <= 1.02f;
            if (m >= 0.0f && !wholeImage) { fx = hd->second.x; fy = hd->second.y; }
        }
        if (px == 1.0f && py == 1.0f && fx == 1.0f && fy == 1.0f) return false;
        float W = (float)g_realW, H = (float)g_realH;
        float c[12] = { fx, fy, 0.0f, 0.0f,   // c253: uv factor
                       1.0f, 1.0f, 1.0f, 1.0f,   // c254: colour for layouts without vertex colour (unlit default white)
                       2.0f * px / W, -2.0f * py / H, 1.0f / W - 1.0f, 1.0f - 1.0f / H };  // c255
        g_viaFVF = !g_declIsCurrent;
        dev->SetVertexDeclaration(r.decl);
        dev->SetVertexShader(vs);
        dev->SetVertexShaderConstantF(253, c, 3);
        if (g_linear && r.variant == V_UV) {
            for (DWORD s = 0; s < 2; s++) {
                dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            }
        }
        return true;
    }

    void end(IDirect3DDevice9* dev) {
        dev->SetVertexShader(nullptr);
        dev->SetVertexShaderConstantF(253, &g_vsConst[0][0], 3);
        if (g_viaFVF) dev->SetFVF(g_curFVF);
        else dev->SetVertexDeclaration(g_curDecl);
        if (g_linear) {
            for (DWORD s = 0; s < 2; s++) {
                dev->SetSamplerState(s, D3DSAMP_MAGFILTER, g_filter[s][0]);
                dev->SetSamplerState(s, D3DSAMP_MINFILTER, g_filter[s][1]);
            }
        }
    }}
