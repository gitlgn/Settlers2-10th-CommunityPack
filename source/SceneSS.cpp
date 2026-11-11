#include "SceneSS.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <string>

namespace SceneSS {
    static void readIni(const char* iniPath);
    static void trackRT(DWORD i, IDirect3DSurface9* rt);

    static std::string g_log, g_trigger;
    static bool g_tracing = false;
    static DWORD g_lastCheck = 0;
    static int g_draws = 0;
    static std::map<void*, std::string> g_names;     // created render targets / depth surfaces / textures

    static void logf(const char* fmt, ...) {
        if (g_log.empty()) return;
        FILE* f = nullptr;
        if (fopen_s(&f, g_log.c_str(), "a") != 0 || !f) return;
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }

    static std::string describe(IDirect3DSurface9* s) {
        if (!s) return "NULL";
        char b[160];
        D3DSURFACE_DESC d;
        if (FAILED(s->GetDesc(&d))) { _snprintf_s(b, sizeof(b), _TRUNCATE, "%p", s); return b; }
        IDirect3DTexture9* parent = nullptr;
        void* key = s;
        if (SUCCEEDED(s->GetContainer(__uuidof(IDirect3DTexture9), (void**)&parent)) && parent) { key = parent; parent->Release(); }
        auto it = g_names.find(key);
        _snprintf_s(b, sizeof(b), _TRUNCATE, "%p %ux%u fmt %d usage %lX%s%s", s, d.Width, d.Height, (int)d.Format, d.Usage,
                    key != s ? " (texture " : "", key != s ? (it != g_names.end() ? (it->second + ")").c_str() : "?)") : "");
        return b;
    }

    void init(const char* iniPath, const char* logPath) {
        readIni(iniPath);
        if (!logPath) return;
        g_log = logPath;
        DeleteFileA(logPath);
        g_trigger = g_log.substr(0, g_log.find_last_of('\\')) + "\\d3dtrace.trigger";
    }

    void onPresent() {
        if (g_tracing) { logf("---- Present (draws since last: %d)", g_draws); g_tracing = false; logf("==== trace end"); }
        if (g_log.empty()) return;
        DWORD now = GetTickCount();
        if (now - g_lastCheck < 250) return;
        g_lastCheck = now;
        if (GetFileAttributesA(g_trigger.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        DeleteFileA(g_trigger.c_str());
        g_tracing = true;
        g_draws = 0;
        logf("==== trace of one frame");
    }

    static void flushDraws() {
        if (g_draws) { logf("   draws: %d", g_draws); g_draws = 0; }
    }

    void onSetRenderTarget(DWORD i, IDirect3DSurface9* rt) {
        trackRT(i, rt);
        if (!g_tracing) return;
        flushDraws();
        logf("SetRenderTarget(%lu, %s)", i, describe(rt).c_str());
    }
    void onSetDepthStencil(IDirect3DSurface9* ds) {
        if (!g_tracing) return;
        flushDraws();
        logf("SetDepthStencilSurface(%s)", describe(ds).c_str());
    }
    void onSetViewport(const D3DVIEWPORT9* v) {
        if (!g_tracing || !v) return;
        logf("SetViewport(%lu,%lu %lux%lu z %.2f-%.2f)", v->X, v->Y, v->Width, v->Height, v->MinZ, v->MaxZ);
    }
    void onClear(DWORD count, DWORD flags) {
        if (!g_tracing) return;
        flushDraws();
        logf("Clear(rects %lu, flags %lX)", count, flags);
    }
    void onStretchRect(IDirect3DSurface9* s, const RECT* sr, IDirect3DSurface9* d, const RECT* dr) {
        if (!g_tracing) return;
        flushDraws();
        logf("StretchRect(%s -> %s, srcRect %s, dstRect %s)", describe(s).c_str(), describe(d).c_str(), sr ? "set" : "full", dr ? "set" : "full");
    }
    void onGetRenderTarget(DWORD i, IDirect3DSurface9* rt) {
        if (!g_tracing) return;
        logf("GetRenderTarget(%lu) -> %s", i, describe(rt).c_str());
    }
    void onDraw() { if (g_tracing) g_draws++; }
    void onSetTexture(DWORD stage, IDirect3DBaseTexture9* tex) {
        if (!g_tracing || !tex) return;
        auto it = g_names.find(tex);
        if (it != g_names.end()) { flushDraws(); logf("SetTexture(%lu, %s)", stage, it->second.c_str()); }
    }
    void onCreate(const char* what, UINT w, UINT h, DWORD usage, D3DFORMAT fmt, void* obj) {
        if (g_log.empty() || !(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) && strcmp(what, "CreateTexture") == 0) return;
        char b[96];
        _snprintf_s(b, sizeof(b), _TRUNCATE, "%s %ux%u fmt %d usage %lX", what, w, h, (int)fmt, usage);
        g_names[obj] = b;
        logf("%s -> %p", b, obj);
    }
}

// ------------------------------------------------------------------------------- supersampling
// The game draws the 3D scene and the HUD straight into the back buffer. With supersampling an own render
// target (and depth buffer) of twice the size stands in for the back buffer and the device's own depth
// buffer; before Present it is shrunk into the real back buffer (linear filter = 2x2 box). Works in
// windowed and exclusive fullscreen mode (the game resets the device to fullscreen).
#include <vector>
#include "UIScale.h"

namespace SceneSS {

    static bool g_want = false;          // [Graphics] Supersampling
    static int g_factor = 1;             // active factor
    static IDirect3DSurface9* g_bb = nullptr;      // real back buffer (no reference held)
    static IDirect3DSurface9* g_autoDS = nullptr;  // device's own depth buffer (no reference held)
    static IDirect3DSurface9* g_rt = nullptr;      // our 2x colour target (own reference)
    static IDirect3DSurface9* g_ds = nullptr;      // our 2x depth buffer (own reference)
    static IDirect3DSurface9* g_resolve = nullptr; // 2x without MSAA (only with SupersamplingMSAA)
    static int g_msaaWant = 0, g_msaa = 0;
    static UINT g_w = 0, g_h = 0;                  // game resolution
    static bool g_onSS = true;
    static std::vector<D3DRECT> g_rects;
    // with the game's post-processing (-postFX) the scene goes into the game's own screen-sized targets and only the
    // combine pass writes the back buffer: then those targets are enlarged instead (texture mode)
    static bool g_texMode = false;
    static UINT g_bbW = 0, g_bbH = 0;
    static const DWORD POSTFX_FLAG = 0x006C4A1C, SETTINGS_GUARD = 0x006C4A5C, SETTINGS_VTABLE = 0x00662688;
    static bool postfxOn() {
        __try {
            return (*(volatile DWORD*)SETTINGS_GUARD & 1) && *(volatile DWORD*)(POSTFX_FLAG - 0x0C) == SETTINGS_VTABLE &&
                   *(volatile BYTE*)POSTFX_FLAG != 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static void readIni(const char* iniPath) {
        char v[16];
        GetPrivateProfileStringA("Graphics", "Supersampling", "off", v, sizeof(v), iniPath);
        g_want = _stricmp(v, "on") == 0 || _stricmp(v, "true") == 0 || strcmp(v, "1") == 0 || strcmp(v, "2") == 0;
        g_msaaWant = GetPrivateProfileIntA("Graphics", "SupersamplingMSAA", 0, iniPath);
        if (g_msaaWant != 2 && g_msaaWant != 4 && g_msaaWant != 8) g_msaaWant = 0;
    }

    bool active() { return g_factor > 1; }

    // texture mode: screen-sized render targets / depth surfaces of the post-processing get twice the size
    bool scaleCreate(UINT& w, UINT& h, DWORD usage) {
        if (!g_texMode || !g_want || w != g_bbW || h != g_bbH || !(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) return false;
        w *= 2; h *= 2;
        static int n = 0;
        if (n++ < 16) logf("scene target %ux%u usage %lX created at %ux%u", w / 2, h / 2, usage, w, h);
        return true;
    }

    void release() {
        if (g_rt) { g_rt->Release(); g_rt = nullptr; }
        if (g_ds) { g_ds->Release(); g_ds = nullptr; }
        if (g_resolve) { g_resolve->Release(); g_resolve = nullptr; }
        g_msaa = 0;
        g_bb = g_autoDS = nullptr;
        g_factor = 1;
    }

    void onDeviceReady(IDirect3DDevice9* dev) {
        release();
        if (!g_want || !dev) { UIScale::setSupersample(1); return; }
        IDirect3DSurface9* bb = nullptr;
        D3DSURFACE_DESC d;
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb || FAILED(bb->GetDesc(&d))) {
            if (bb) bb->Release();
            logf("supersampling: no back buffer");
            UIScale::setSupersample(1);
            return;
        }
        g_bb = bb; bb->Release();
        g_w = d.Width; g_h = d.Height;
        g_bbW = d.Width; g_bbH = d.Height;
        g_texMode = false;
        if (postfxOn()) {
            if (d.Width * 2 > 8192 || d.Height * 2 > 8192) { logf("supersampling off: %ux%u too large", d.Width * 2, d.Height * 2); UIScale::setSupersample(1); return; }
            g_texMode = true;
            UIScale::setSupersample(1);
            logf("supersampling on (post effects): the game's %ux%u scene targets are created at 2x", d.Width, d.Height);
            return;
        }
        UINT w = d.Width * 2, h = d.Height * 2;
        D3DCAPS9 caps;
        if (w > 8192 || h > 8192 || (SUCCEEDED(dev->GetDeviceCaps(&caps)) && (w > caps.MaxTextureWidth || h > caps.MaxTextureHeight))) {
            logf("supersampling off: %ux%u too large", w, h);
            UIScale::setSupersample(1);
            return;
        }
        IDirect3DSurface9* ds = nullptr;
        D3DSURFACE_DESC dd = {};
        if (SUCCEEDED(dev->GetDepthStencilSurface(&ds)) && ds) { ds->GetDesc(&dd); g_autoDS = ds; ds->Release(); }
        // multisampling of the large target (the back buffer's own MSAA does not reach it)
        g_msaa = 0;
        if (g_msaaWant) {
            IDirect3D9* d3d = nullptr;
            if (SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d) {
                D3DMULTISAMPLE_TYPE t = (D3DMULTISAMPLE_TYPE)g_msaaWant;
                if (SUCCEEDED(d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, d.Format, TRUE, t, nullptr)) &&
                    (!g_autoDS || SUCCEEDED(d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dd.Format, TRUE, t, nullptr))))
                    g_msaa = g_msaaWant;
                else logf("supersampling: MSAA %dx not supported - without MSAA", g_msaaWant);
                d3d->Release();
            }
        }
        D3DMULTISAMPLE_TYPE ms = (D3DMULTISAMPLE_TYPE)g_msaa;
        HRESULT h1 = dev->CreateRenderTarget(w, h, d.Format, ms, 0, FALSE, &g_rt, nullptr);
        HRESULT h2 = g_autoDS ? dev->CreateDepthStencilSurface(w, h, dd.Format, ms, 0, FALSE, &g_ds, nullptr) : S_OK;
        if (SUCCEEDED(h1) && SUCCEEDED(h2) && g_msaa) {
            HRESULT h3 = dev->CreateRenderTarget(w, h, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_resolve, nullptr);
            if (FAILED(h3)) { logf("supersampling: resolve target failed (0x%08lX)", h3); h1 = h3; }
        }
        if (FAILED(h1) || FAILED(h2)) {
            logf("supersampling off: render target %ux%u could not be created (0x%08lX / 0x%08lX)", w, h, h1, h2);
            release();
            UIScale::setSupersample(1);
            return;
        }
        g_factor = 2;
        dev->SetRenderTarget(0, g_rt);
        if (g_ds) dev->SetDepthStencilSurface(g_ds);
        g_onSS = true;
        UIScale::setSupersample(2);
        logf("supersampling on: game %ux%u (format %d), render target %ux%u, depth format %d, MSAA %dx", g_w, g_h, (int)d.Format, w, h, (int)dd.Format, g_msaa);
    }

    IDirect3DSurface9* renderTarget(DWORD i, IDirect3DSurface9* rt) {
        if (g_factor > 1 && i == 0 && rt == g_bb) rt = g_rt;
        if (i == 0) g_onSS = g_factor > 1 && rt == g_rt;
        return rt;
    }

    IDirect3DSurface9* depthStencil(IDirect3DSurface9* ds) {
        return (g_factor > 1 && ds && ds == g_autoDS && g_ds) ? g_ds : ds;
    }

    void beforePresent(IDirect3DDevice9* dev) {
        if (g_factor == 1 || !g_rt || !g_bb) return;
        if (g_resolve) {
            dev->StretchRect(g_rt, nullptr, g_resolve, nullptr, D3DTEXF_NONE);     // MSAA resolve, same size
            dev->StretchRect(g_resolve, nullptr, g_bb, nullptr, D3DTEXF_LINEAR);
        } else dev->StretchRect(g_rt, nullptr, g_bb, nullptr, D3DTEXF_LINEAR);
    }

    static void trackRT(DWORD, IDirect3DSurface9*) {}

    const D3DVIEWPORT9* viewport(const D3DVIEWPORT9* in, D3DVIEWPORT9& t) {
        if (g_factor == 1 || !g_onSS || !in) return in;
        t = *in;
        t.X *= g_factor; t.Y *= g_factor; t.Width *= g_factor; t.Height *= g_factor;
        return &t;
    }

    void viewportToGame(D3DVIEWPORT9* vp) {
        if (g_factor == 1 || !g_onSS || !vp) return;
        vp->X /= g_factor; vp->Y /= g_factor; vp->Width /= g_factor; vp->Height /= g_factor;
    }

    const RECT* rect(const RECT* in, RECT& t) {
        if (g_factor == 1 || !g_onSS || !in) return in;
        t.left = in->left * g_factor; t.top = in->top * g_factor; t.right = in->right * g_factor; t.bottom = in->bottom * g_factor;
        return &t;
    }

    const D3DRECT* clearRects(DWORD n, const D3DRECT* in) {
        if (g_factor == 1 || !g_onSS || !in || !n) return in;
        g_rects.assign(in, in + n);
        for (auto& r : g_rects) { r.x1 *= g_factor; r.y1 *= g_factor; r.x2 *= g_factor; r.y2 *= g_factor; }
        return g_rects.data();
    }
}
