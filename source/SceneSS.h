#pragma once

#include <d3d9.h>

// Supersampling of the 3D scene ([Graphics] Supersampling) and a one-frame D3D9 call trace for diagnostics:
// with DebugMode, creating <game>\bin\d3dtrace.trigger writes the render target / viewport / copy calls of the
// next frame to bin\d3dtrace.log.
namespace SceneSS {
    void init(const char* iniPath, const char* logPath);
    void onPresent();
    void onSetRenderTarget(DWORD index, IDirect3DSurface9* rt);
    void onSetDepthStencil(IDirect3DSurface9* ds);
    void onSetViewport(const D3DVIEWPORT9* vp);
    void onClear(DWORD count, DWORD flags);
    void onStretchRect(IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr);
    void onDraw();
    void onSetTexture(DWORD stage, IDirect3DBaseTexture9* tex);
    void onCreate(const char* what, UINT w, UINT h, DWORD usage, D3DFORMAT fmt, void* obj);
    void onGetRenderTarget(DWORD index, IDirect3DSurface9* rt);
}

namespace SceneSS {
    // supersampling: an own render target + depth buffer of 2x the back buffer stand in for them and are
    // shrunk into the back buffer before Present (windowed and fullscreen)
    void onDeviceReady(IDirect3DDevice9* dev);      // after CreateDevice / Reset
    void release();                                 // before Reset (D3DPOOL_DEFAULT surfaces)
    IDirect3DSurface9* renderTarget(DWORD index, IDirect3DSurface9* rt);   // back buffer -> own target
    IDirect3DSurface9* depthStencil(IDirect3DSurface9* ds);                // device depth -> own depth
    void beforePresent(IDirect3DDevice9* dev);
    // game pixels -> render target pixels while the own target is bound
    const D3DVIEWPORT9* viewport(const D3DVIEWPORT9* in, D3DVIEWPORT9& tmp);
    void viewportToGame(D3DVIEWPORT9* vp);
    const RECT* rect(const RECT* in, RECT& tmp);
    const D3DRECT* clearRects(DWORD count, const D3DRECT* in);
    bool active();
    // with post effects: enlarges the game's screen-sized render targets / depth surfaces (returns true if changed)
    bool scaleCreate(UINT& w, UINT& h, DWORD usage);
}
