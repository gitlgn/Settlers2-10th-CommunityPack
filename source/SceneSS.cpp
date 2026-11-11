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
    static UINT g_w = 0, g_h = 0;                  // game resolution
    static bool g_onSS = true;
    static std::vector<D3DRECT> g_rects;

    static void readIni(const char* iniPath) {
        char v[16];
        GetPrivateProfileStringA("Graphics", "Supersampling", "off", v, sizeof(v), iniPath);
        g_want = _stricmp(v, "on") == 0 || _stricmp(v, "true") == 0 || strcmp(v, "1") == 0 || strcmp(v, "2") == 0;
    }

    bool active() { return g_factor > 1; }

    void release() {
        if (g_rt) { g_rt->Release(); g_rt = nullptr; }
        if (g_ds) { g_ds->Release(); g_ds = nullptr; }
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
        HRESULT h1 = dev->CreateRenderTarget(w, h, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_rt, nullptr);
        HRESULT h2 = g_autoDS ? dev->CreateDepthStencilSurface(w, h, dd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_ds, nullptr) : S_OK;
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
        logf("supersampling on: game %ux%u (format %d), render target %ux%u, depth format %d", g_w, g_h, (int)d.Format, w, h, (int)dd.Format);
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
        dev->StretchRect(g_rt, nullptr, g_bb, nullptr, D3DTEXF_LINEAR);
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
