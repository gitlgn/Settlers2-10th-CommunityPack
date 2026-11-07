#pragma once
#include <d3d9.h>

// HUD scaling for high resolutions ([UI] Scale in d3d9.ini, 1.0 = off).
// The game lays out its HUD from two globals (screen width/height). We hand it a smaller virtual
// screen (real / scale), scale the HUD draw calls back up with a tiny vertex shader and convert
// mouse coordinates from real to virtual pixels.
namespace UIScale {
    void init(const char* iniPath, const char* logPath);   // DllMain: read config, patch the size setter
    void onDeviceCreated(IDirect3DDevice9* dev, HWND hwnd);  // after CreateDevice
    bool enabled();

    // device wrapper hooks
    void trackDecl(IDirect3DVertexDeclaration9* decl);
    void trackFVF(DWORD fvf);
    void trackVS(IDirect3DVertexShader9* vs);
    void trackVSConst(UINT start, const float* data, UINT count);
    void trackSampler(DWORD stage, D3DSAMPLERSTATETYPE type, DWORD value);
    void trackTexture(DWORD stage, void* tex);
    void onCreateTexture(UINT w, UINT h, DWORD usage, D3DPOOL pool, void* tex);   // recognises upscaled HUD textures (mods\ui_hd.sizes.txt)
    void trackStream(UINT n, IDirect3DVertexBuffer9* vb, UINT offset, UINT stride);

    // vertices of the draw call (for user-pointer draws upData is set, otherwise stream 0 is read)
    struct DrawRange { const void* upData; UINT upStride; UINT first; UINT count; };
    UINT vertexCount(D3DPRIMITIVETYPE type, UINT primitives);

    // called around every draw; returns true if the HUD state was swapped in (call end() after drawing)
    bool begin(IDirect3DDevice9* dev, const DrawRange& range);
    void end(IDirect3DDevice9* dev);
}
