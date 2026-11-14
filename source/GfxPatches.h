#pragma once
#include <d3d9.h>

// Engine tweaks applied once the game code is unpacked (at device creation), configured in [Graphics] of d3d9.ini.
namespace GfxPatches {
    void init(const char* iniPath, const char* logPath);
    void onDeviceCreated();

    // render-target diagnostics (only with [Misc] DebugMode = on)
    void onCreateTexture(UINT w, UINT h, DWORD usage, D3DFORMAT fmt, void* tex);
    void onSetTexture(DWORD stage, void* tex);
    void onPresent();

    // depth for the post-processing combine pass: the R32F depth target goes to stage 2 while the scene colour is
    // on stage 1 (the engine never binds it there, so depth of field and SSAO saw no depth)
    void afterSetTexture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* tex);
    void restoreStage2(IDirect3DDevice9* dev);   // before Present
    void beforeReset();
    // transparent objects must not write the depth target (render target 1): returns true when the caller has to call
    // applyWriteMask1 after passing the state on
    bool filterRenderState(IDirect3DDevice9* dev, D3DRENDERSTATETYPE state, DWORD& value);
    void applyWriteMask1(IDirect3DDevice9* dev);
    // [Graphics] DistanceFog: the game's fixed-function distance fog (off by default - with the extended zoom it
    // turns every object pale; Shader Model 3 shaders such as the improved terrain never get it anyway)
    bool distanceFog();
    void setDistanceFog(bool on);   // from the in-game menu (takes effect with the next fog state the game sets)

    // live shader settings (community_live.inc): the effects upload their default values for the registers
    // c28-c31 (pixel) / c249 (vertex) with a marker in .w; these are replaced with the current values here
    const float* liveConstants(bool vertex, unsigned start, const float* data, unsigned count, float* scratch);
    // from the in-game menu: ini key ("FXAA", "SSAOStrength", ...) and its new value (switches: 0 / 1)
    bool setLive(const char* key, float value);
    // comparison shots (bin\compare.trigger): the shader effects as in the original game, settings untouched
    void setVanillaShaders(bool on);
}
