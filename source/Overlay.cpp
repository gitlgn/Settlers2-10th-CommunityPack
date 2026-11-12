#include "Overlay.h"
#include "GfxPatches.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "third_party/imgui/imgui.h"
#include "third_party/imgui/imgui_impl_dx9.h"


namespace Overlay {

    static std::string g_ini, g_log;
    static bool g_enabled = true, g_ready = false, g_open = false, g_keyDown = false;
    static int g_key = VK_F10;
    static HWND g_wnd = nullptr;
    static IDirect3DDevice9* g_dev = nullptr;
    static WNDPROC g_oldProc = nullptr;
    static POINT g_frozenCursor = {};

    static void logf(const char* fmt, const char* a = "") {
        if (g_log.empty()) return;
        FILE* f = nullptr;
        if (fopen_s(&f, g_log.c_str(), "a") == 0 && f) { fprintf(f, fmt, a); fputc('\n', f); fclose(f); }
    }

    // ------------------------------------------------------------------ settings model
    enum Type { T_BOOL, T_FLOAT, T_CHOICE };
    struct Item {
        const char* tab; const char* section; const char* key; Type type; const char* def;
        float mn, mx; const char* choices;   // T_CHOICE: '|' separated values
        const char* label; const char* help; bool live;
        // state
        bool b; float f; int c; std::string start;
    };

    static std::vector<Item> g_items = {
        { "Graphics", "Graphics", "Supersampling", T_BOOL, "off", 0, 0, nullptr, "Supersampling (2x)",
          "Renders the scene at twice the resolution and shrinks it: smooth edges, finer textures. Needs a strong GPU.", false },
        { "Graphics", "Graphics", "SupersamplingMSAA", T_CHOICE, "0", 0, 0, "0|2|4|8", "Supersampling MSAA",
          "Extra multisampling of the large image (only without post-processing).", false },
        { "Graphics", "Graphics", "ShadowMapScale", T_CHOICE, "2", 0, 0, "1|2|4", "Shadow resolution",
          "1 = original 2048, 2 = 4096, 4 = 8192 (at the highest shadow setting).", false },
        { "Graphics", "Graphics", "ReflectionMapScale", T_CHOICE, "2", 0, 0, "1|2|4", "Water reflection resolution",
          "1 = original 2048x1024, 2 = 4096x2048, 4 = 8192x4096.", false },
        { "Graphics", "Graphics", "DistanceFog", T_BOOL, "off", 0, 0, nullptr, "Distance fog",
          "The game's fog. With the extended zoom it turns objects pale when zoomed far out.", true },

        { "Effects", "Graphics", "PostFX", T_BOOL, "on", 0, 0, nullptr, "Post-processing",
          "The game's bloom and depth of field (normally only with -postFX). FXAA and SSAO need it.", false },
        { "Effects", "Graphics", "DOFStrength", T_FLOAT, "0.3", 0, 1, nullptr, "Depth of field haze", "0 = off, 1 = original game.", false },
        { "Effects", "Graphics", "FXAA", T_BOOL, "on", 0, 0, nullptr, "FXAA edge smoothing", "", false },
        { "Effects", "Graphics", "SSAO", T_BOOL, "on", 0, 0, nullptr, "Ambient occlusion (SSAO)", "Contact shadows under trees, houses and in corners.", false },
        { "Effects", "Graphics", "SSAOStrength", T_FLOAT, "0.6", 0, 1, nullptr, "SSAO strength", "", false },
        { "Effects", "Graphics", "SSAORadius", T_FLOAT, "1.0", 0.25f, 4, nullptr, "SSAO radius", "", false },
        { "Effects", "Graphics", "ImprovedTerrain", T_BOOL, "on", 0, 0, nullptr, "Improved terrain", "Sky/ground light and a detail layer at close zoom.", false },
        { "Effects", "Graphics", "TerrainDetail", T_FLOAT, "0.5", 0, 1, nullptr, "Terrain detail", "", false },
        { "Effects", "Graphics", "ImprovedWater", T_BOOL, "on", 0, 0, nullptr, "Improved water", "Fresnel reflection, sun glints, shore colour and foam.", false },
        { "Effects", "Graphics", "ImprovedObjects", T_BOOL, "on", 0, 0, nullptr, "Improved object light", "Houses, trees, rocks with sky/ground light.", false },
        { "Effects", "Graphics", "UnitRimLight", T_FLOAT, "0.5", 0, 1, nullptr, "Rim light on settlers", "0 = off.", false },

        { "Packs", "Mods", "HDTextures", T_BOOL, "on", 0, 0, nullptr, "HD textures (4K)", "S2-modders 4K remakes.", false },
        { "Packs", "Mods", "HDInterface", T_BOOL, "on", 0, 0, nullptr, "HD interface", "2x upscaled interface textures and fonts.", false },
        { "Packs", "Mods", "LODFix", T_BOOL, "on", 0, 0, nullptr, "View distance fix", "Less pop-in when zoomed out.", false },
        { "Packs", "Mods", "SharpShadows", T_BOOL, "on", 0, 0, nullptr, "Precise shadows (32-bit)", "", false },
        { "Packs", "Mods", "SmoothShadows", T_BOOL, "on", 0, 0, nullptr, "Soft shadows",
          "Also carries the improved terrain, water and object shaders.", false },
        { "Packs", "Mods", "ImprovedPostFX", T_BOOL, "on", 0, 0, nullptr, "Softer post-processing", "Gaussian blur; needed for FXAA/SSAO.", false },

        { "Interface", "UI", "Scale", T_FLOAT, "1.5", 1, 3, nullptr, "HUD scale", "1.0 = off (original size).", false },
        { "Interface", "UI", "Filter", T_CHOICE, "linear", 0, 0, "linear|point", "HUD filter", "linear = smooth, point = crisp pixels.", false },

        { "Fixes", "Misc", "GameFixes", T_BOOL, "on", 0, 0, nullptr, "Game bug fixes",
          "Multiplayer robustness, damaged savegames/maps, file transfer, cube maps.", false },
        { "Fixes", "Misc", "TimeFix", T_BOOL, "on", 0, 0, nullptr, "Clock fix", "Smooth animations with long PC uptime.", false },
        { "Fixes", "Misc", "SafeSave", T_BOOL, "on", 0, 0, nullptr, "Safe saving", "No damaged files after a crash while saving.", false },
        { "Fixes", "Misc", "MeshCache", T_BOOL, "on", 0, 0, nullptr, "Mesh cache", "Faster loading; cleared after a crash.", false },
        { "Fixes", "Misc", "LowHeap", T_BOOL, "on", 0, 0, nullptr, "Shader compiler memory below 2 GB", "Needed with the 4 GB flag.", false },
        { "Fixes", "Misc", "DebugMode", T_BOOL, "off", 0, 0, nullptr, "Debug logs", "Writes diagnostic logs to bin\\.", false },
    };

    static std::string readIni(const Item& it) {
        char v[64];
        GetPrivateProfileStringA(it.section, it.key, it.def, v, sizeof(v), g_ini.c_str());
        std::string s = v;
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
        size_t p = s.find_first_not_of(" \t");
        return p == std::string::npos ? std::string() : s.substr(p);
    }

    static bool isOn(const std::string& v) { return !_stricmp(v.c_str(), "on") || !_stricmp(v.c_str(), "true") || v == "1"; }

    static std::vector<std::string> split(const char* list) {
        std::vector<std::string> out;
        std::string s = list ? list : "";
        size_t a = 0;
        while (a <= s.size()) { size_t b = s.find('|', a); if (b == std::string::npos) b = s.size(); out.push_back(s.substr(a, b - a)); a = b + 1; }
        return out;
    }

    static std::string current(const Item& it) {
        char b[32];
        switch (it.type) {
        case T_BOOL: return it.b ? "on" : "off";
        case T_FLOAT: _snprintf_s(b, sizeof(b), _TRUNCATE, "%.2f", it.f); return b;
        default: return split(it.choices)[it.c];
        }
    }

    static void load() {
        for (auto& it : g_items) {
            std::string v = readIni(it);
            it.b = isOn(v);
            it.f = (float)atof(v.c_str());
            if (it.f < it.mn) it.f = it.mn;
            if (it.mx > it.mn && it.f > it.mx) it.f = it.mx;
            it.c = 0;
            if (it.type == T_CHOICE) { auto ch = split(it.choices); for (size_t i = 0; i < ch.size(); i++) if (!_stricmp(ch[i].c_str(), v.c_str())) it.c = (int)i; }
            it.start = current(it);
        }
    }

    static void save(Item& it) {
        std::string v = current(it);
        WritePrivateProfileStringA(it.section, it.key, (" " + v).c_str(), g_ini.c_str());
        if (it.live && !strcmp(it.key, "DistanceFog")) GfxPatches::setDistanceFog(it.b);
    }

    // ------------------------------------------------------------------ input blocking
    static bool isInputMsg(UINT m) {
        return (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || (m >= WM_KEYFIRST && m <= WM_KEYLAST) || m == WM_INPUT ||
               m == WM_NCMOUSEMOVE || m == WM_SETCURSOR;
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        // the menu key itself never reaches the game (F10 would start the window's menu loop)
        if ((m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) && (int)w == g_key) return 0;
        if (g_open && g_ready) {
            if (m == WM_MOUSEWHEEL) ImGui::GetIO().AddMouseWheelEvent(0, (float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA);
            if (isInputMsg(m)) return m == WM_SETCURSOR ? TRUE : 0;
        }
        return CallWindowProcA(g_oldProc, h, m, w, l);
    }

    typedef SHORT(WINAPI* GetAsyncKeyState_t)(int);
    typedef BOOL(WINAPI* GetCursorPos_t)(LPPOINT);
    typedef BOOL(WINAPI* SetCursorPos_t)(int, int);
    static GetAsyncKeyState_t o_GetAsyncKeyState;
    static GetCursorPos_t o_GetCursorPos;
    static SetCursorPos_t o_SetCursorPos;

    static SHORT WINAPI h_GetAsyncKeyState(int k) { return g_open ? 0 : o_GetAsyncKeyState(k); }
    static BOOL WINAPI h_GetCursorPos(LPPOINT p) {
        if (g_open && p) { *p = g_frozenCursor; return TRUE; }
        return o_GetCursorPos(p);
    }
    static BOOL WINAPI h_SetCursorPos(int x, int y) { return g_open ? TRUE : o_SetCursorPos(x, y); }

    static int patchImports(HMODULE mod) {
        BYTE* b = (BYTE*)mod;
        auto nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
        auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) return 0;
        HMODULE user = GetModuleHandleA("user32.dll");
        void* fa = (void*)GetProcAddress(user, "GetAsyncKeyState");
        void* fc = (void*)GetProcAddress(user, "GetCursorPos");
        void* fs = (void*)GetProcAddress(user, "SetCursorPos");
        o_GetAsyncKeyState = (GetAsyncKeyState_t)fa; o_GetCursorPos = (GetCursorPos_t)fc; o_SetCursorPos = (SetCursorPos_t)fs;
        int n = 0;
        // GOG 11757: UPX fills the import table at these slots without import descriptors; patch only a slot that
        // really holds the expected function
        struct { DWORD slot; void* fn; void* hook; } known[] = {
            { 0x00654244, fa, (void*)h_GetAsyncKeyState }, { 0x00654238, fc, (void*)h_GetCursorPos }, { 0x00654234, fs, (void*)h_SetCursorPos },
        };
        for (auto& k : known) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((void*)k.slot, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) continue;
            if (*(void**)k.slot != k.fn) continue;
            DWORD old;
            if (VirtualProtect((void*)k.slot, sizeof(void*), PAGE_READWRITE, &old)) {
                *(void**)k.slot = k.hook;
                VirtualProtect((void*)k.slot, sizeof(void*), old, &old);
                n++;
            }
        }
        if (n) return n;
        for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(b + dir.VirtualAddress); imp->Name; imp++) {
            auto slot = (IMAGE_THUNK_DATA*)(b + imp->FirstThunk);
            for (; slot->u1.Function; slot++) {
                void* cur = (void*)slot->u1.Function;
                void* hook = cur == fa ? (void*)h_GetAsyncKeyState : cur == fc ? (void*)h_GetCursorPos : cur == fs ? (void*)h_SetCursorPos : nullptr;
                if (!hook) continue;
                DWORD old;
                if (VirtualProtect(&slot->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                    slot->u1.Function = (ULONG_PTR)hook;
                    VirtualProtect(&slot->u1.Function, sizeof(void*), old, &old);
                    n++;
                }
            }
        }
        return n;
    }

    // ------------------------------------------------------------------ setup
    void init(const char* iniPath, const char* logPath) {
        g_ini = iniPath ? iniPath : "";
        g_log = logPath ? logPath : "";
        if (logPath) DeleteFileA(logPath);
        char v[32];
        GetPrivateProfileStringA("UI", "Menu", "on", v, sizeof(v), iniPath);
        g_enabled = isOn(v);
        GetPrivateProfileStringA("UI", "MenuKey", "F10", v, sizeof(v), iniPath);
        if ((v[0] == 'F' || v[0] == 'f') && atoi(v + 1) >= 1 && atoi(v + 1) <= 24) g_key = VK_F1 + atoi(v + 1) - 1;
        else if (!_stricmp(v, "insert")) g_key = VK_INSERT;
        else if (!_stricmp(v, "home")) g_key = VK_HOME;
    }

    void onDeviceCreated(IDirect3DDevice9* dev, HWND wnd) {
        if (!g_enabled || g_ready || !dev || !wnd) return;
        g_wnd = wnd;
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        RECT rc;
        GetClientRect(wnd, &rc);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        float scale = (screenH > 0 ? screenH : rc.bottom) / 1080.0f;
        if (scale < 1.0f) scale = 1.0f;
        char win[MAX_PATH];
        GetWindowsDirectoryA(win, MAX_PATH);
        std::string font = std::string(win) + "\\Fonts\\segoeui.ttf";
        if (GetFileAttributesA(font.c_str()) != INVALID_FILE_ATTRIBUTES) io.Fonts->AddFontFromFileTTF(font.c_str(), 18.0f * scale);
        else io.FontGlobalScale = scale;
        ImGui::StyleColorsDark();
        ImGui::GetStyle().ScaleAllSizes(scale);
        ImGui::GetStyle().WindowRounding = 6.0f * scale;
        // the device window can be a hidden helper window: display size, mouse and messages come from the visible
        // game window, which is looked up on the first frames (see attachWindow)
        // own renderer and texture handling (see renderDrawData / updateTexture)
        g_dev = dev;
        if (!ImGui_ImplDX9_Init(dev)) { logf("menu: ImGui init failed"); return; }
        io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
        char n[16];
        _snprintf_s(n, sizeof(n), _TRUNCATE, "%d", patchImports(GetModuleHandleA(nullptr)));
        logf("menu ready, game input imports redirected: %s", n);
        load();
        g_ready = true;
    }

    void beforeReset() {}                                  // managed textures survive a Reset
    void afterReset(IDirect3DDevice9* dev) { if (dev) g_dev = dev; }

    // ------------------------------------------------------------------ drawing
    static void drawMenu() {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.y * 0.62f, io.DisplaySize.y * 0.62f), ImGuiCond_Appearing);
        bool open = true;
        if (!ImGui::Begin("Community Pack", &open, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
        if (!open) g_open = false;
        if (ImGui::BeginTabBar("tabs")) {
            for (const char* tab : { "Graphics", "Effects", "Packs", "Interface", "Fixes" }) {
                if (!ImGui::BeginTabItem(tab)) continue;
                ImGui::Spacing();
                for (auto& it : g_items) {
                    if (strcmp(it.tab, tab)) continue;
                    ImGui::PushID(it.key);
                    bool changed = false;
                    if (it.type == T_BOOL) changed = ImGui::Checkbox(it.label, &it.b);
                    else if (it.type == T_FLOAT) {
                        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
                        changed = ImGui::SliderFloat(it.label, &it.f, it.mn, it.mx, "%.2f");
                    } else {
                        auto ch = split(it.choices);
                        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
                        if (ImGui::BeginCombo(it.label, ch[it.c].c_str())) {
                            for (int i = 0; i < (int)ch.size(); i++)
                                if (ImGui::Selectable(ch[i].c_str(), i == it.c)) { it.c = i; changed = true; }
                            ImGui::EndCombo();
                        }
                    }
                    if (it.help && *it.help) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.65f, 0.65f, 1));
                        ImGui::TextWrapped("%s", it.help);
                        ImGui::PopStyleColor();
                    }
                    if (changed) save(it);
                    ImGui::Spacing();
                    ImGui::PopID();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        std::string pending;
        for (auto& it : g_items)
            if (!it.live && current(it) != it.start) pending += (pending.empty() ? "" : ", ") + std::string(it.label);
        ImGui::Separator();
        if (!pending.empty()) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1), "Restart the game to apply: %s", pending.c_str());
        else ImGui::TextDisabled("Settings are saved to bin\\d3d9.ini. F10 or Esc closes this menu.");
        ImGui::End();
    }

    // the visible game window of this process (foreground), subclassed once for the menu key / wheel / input filter
    static HWND g_hooked = nullptr;
    static void attachWindow() {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (!fg || fg == g_hooked) return;
        GetWindowThreadProcessId(fg, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(fg)) return;
        if (g_hooked && g_oldProc) SetWindowLongPtrA(g_hooked, GWLP_WNDPROC, (LONG_PTR)g_oldProc);
        g_oldProc = (WNDPROC)SetWindowLongPtrA(fg, GWLP_WNDPROC, (LONG_PTR)wndProc);
        g_hooked = fg;
        RECT r; GetClientRect(fg, &r);
        char b[96]; _snprintf_s(b, sizeof(b), _TRUNCATE, "game window %p client %ldx%ld", fg, r.right, r.bottom);
        logf("%s", b);
    }

    static void feedInput(float w, float h) {
        ImGuiIO& io = ImGui::GetIO();
        static LARGE_INTEGER f = {}, last = {};
        LARGE_INTEGER now;
        if (!f.QuadPart) QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&now);
        io.DeltaTime = last.QuadPart ? (float)((double)(now.QuadPart - last.QuadPart) / f.QuadPart) : 1.0f / 60.0f;
        if (io.DeltaTime <= 0.0f) io.DeltaTime = 1.0f / 1000.0f;
        last = now;
        io.DisplaySize = ImVec2(w, h);
        POINT p;
        if (g_hooked && o_GetCursorPos && o_GetCursorPos(&p) && ScreenToClient(g_hooked, &p)) {
            RECT r; GetClientRect(g_hooked, &r);
            float sx = r.right > 0 ? w / r.right : 1.0f, sy = r.bottom > 0 ? h / r.bottom : 1.0f;
            io.AddMousePosEvent(p.x * sx, p.y * sy);
        }
        io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
        io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
    }

    // ImGui 1.92+ builds its font atlas incrementally. Partial uploads into a dynamic D3DPOOL_DEFAULT texture lose
    // earlier glyphs under DXVK, so the atlas lives in a managed A8R8G8B8 texture and is uploaded completely.
    static void updateTexture(ImTextureData* t) {
        if (t->Status == ImTextureStatus_WantDestroy) {
            if (auto* tx = (IDirect3DTexture9*)(intptr_t)t->TexID) tx->Release();
            t->SetTexID(ImTextureID_Invalid);
            t->SetStatus(ImTextureStatus_Destroyed);
            return;
        }
        // always a fresh texture: locking the one that is still in use for drawing deadlocks under DXVK
        IDirect3DTexture9* old = (IDirect3DTexture9*)(intptr_t)t->TexID;
        IDirect3DTexture9* tx = nullptr;
        if (!g_dev || FAILED(g_dev->CreateTexture(t->Width, t->Height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tx, nullptr))) return;
        t->SetTexID((ImTextureID)(intptr_t)tx);
        if (old) old->Release();
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(tx->LockRect(0, &lr, nullptr, 0))) {
            for (int y = 0; y < t->Height; y++) {
                const ImU32* src = (const ImU32*)t->GetPixelsAt(0, y);
                DWORD* dst = (DWORD*)((BYTE*)lr.pBits + y * lr.Pitch);
                for (int x = 0; x < t->Width; x++) {
                    if (t->Format == ImTextureFormat_Alpha8) { dst[x] = ((DWORD)((const unsigned char*)src)[x] << 24) | 0xFFFFFF; continue; }
                    ImU32 c = src[x];   // RGBA (ABGR in memory order) -> A8R8G8B8
                    dst[x] = (c & 0xFF00FF00) | ((c & 0xFF0000) >> 16) | ((c & 0xFF) << 16);
                }
            }
            tx->UnlockRect(0);
        }
        t->SetStatus(ImTextureStatus_OK);
    }

    // Own ImGui renderer: draws straight from memory with pre-transformed vertices (DrawIndexedPrimitiveUP), the
    // path that works in this game; the DX9 backend is only used for its texture management.
    static void renderDrawData(IDirect3DDevice9* dev, ImDrawData* dd) {
        if (!dd || dd->DisplaySize.x <= 0 || dd->DisplaySize.y <= 0) return;
        if (dd->Textures)
            for (ImTextureData* t : *dd->Textures)
                if (t->Status != ImTextureStatus_OK) updateTexture(t);
        struct V { float x, y, z, w; DWORD c; float u, v; };
        static std::vector<V> vb;
        dev->SetVertexShader(nullptr);
        dev->SetPixelShader(nullptr);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        const ImVec2 off = dd->DisplayPos;
        for (int n = 0; n < dd->CmdListsCount; n++) {
            const ImDrawList* cl = dd->CmdLists[n];
            vb.resize(cl->VtxBuffer.Size);
            for (int i = 0; i < cl->VtxBuffer.Size; i++) {
                const ImDrawVert& s0 = cl->VtxBuffer[i];
                ImU32 c = s0.col;   // ImGui: ABGR -> D3D: ARGB
                vb[i] = { s0.pos.x - off.x - 0.5f, s0.pos.y - off.y - 0.5f, 0.0f, 1.0f,
                          (c & 0xFF00FF00) | ((c & 0xFF0000) >> 16) | ((c & 0xFF) << 16), s0.uv.x, s0.uv.y };
            }
            for (const ImDrawCmd& cmd : cl->CmdBuffer) {
                if (cmd.UserCallback) { if (cmd.UserCallback != ImDrawCallback_ResetRenderState) cmd.UserCallback(cl, &cmd); continue; }
                RECT r = { (LONG)(cmd.ClipRect.x - off.x), (LONG)(cmd.ClipRect.y - off.y), (LONG)(cmd.ClipRect.z - off.x), (LONG)(cmd.ClipRect.w - off.y) };
                if (r.right <= r.left || r.bottom <= r.top) continue;
                dev->SetScissorRect(&r);
                dev->SetTexture(0, (IDirect3DTexture9*)(intptr_t)cmd.GetTexID());
                dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)(cl->VtxBuffer.Size - (int)cmd.VtxOffset), cmd.ElemCount / 3,
                                            cl->IdxBuffer.Data + cmd.IdxOffset, sizeof(ImDrawIdx) == 2 ? D3DFMT_INDEX16 : D3DFMT_INDEX32,
                                            vb.data() + cmd.VtxOffset, sizeof(V));
            }
        }
    }

    void onPresent(IDirect3DDevice9* dev) {
        if (!g_ready) return;
        attachWindow();
        bool down = (GetAsyncKeyState(g_key) & 0x8000) != 0;
        if (down && !g_keyDown) {
            g_open = !g_open;
            if (g_open) { o_GetCursorPos(&g_frozenCursor); load(); GetAsyncKeyState(VK_ESCAPE); }
        }
        g_keyDown = down;
        if (g_open && (GetAsyncKeyState(VK_ESCAPE) & 1)) g_open = false;
        static DWORD hintUntil = 0;
        if (!hintUntil) hintUntil = GetTickCount() + 60000;
        bool hint = !g_open && GetTickCount() < hintUntil;
        if (!g_open && !hint) return;

        ImGui::GetIO().MouseDrawCursor = g_open;
        {
            IDirect3DSurface9* b0 = nullptr;
            D3DSURFACE_DESC d0 = {};
            if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &b0)) && b0) { b0->GetDesc(&d0); b0->Release(); }
            if (!d0.Width) return;
            feedInput((float)d0.Width, (float)d0.Height);
        }
        ImGui_ImplDX9_NewFrame();
        ImGui::NewFrame();
        if (g_open) drawMenu();
        else {
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
            ImGui::SetNextWindowBgAlpha(0.6f);
            ImGui::Begin("hint", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
            char k[16];
            if (g_key >= VK_F1 && g_key <= VK_F24) _snprintf_s(k, sizeof(k), _TRUNCATE, "F%d", g_key - VK_F1 + 1);
            else strcpy_s(k, g_key == VK_INSERT ? "Insert" : "Home");
            ImGui::Text("%s - Community Pack settings", k);
            ImGui::End();
        }
        ImGui::EndFrame();
        ImGui::Render();

        // draw into the real back buffer (after the supersampling blit), then restore the game's targets
        IDirect3DSurface9 *bb = nullptr, *rt = nullptr, *ds = nullptr;
        D3DVIEWPORT9 vp;
        dev->GetViewport(&vp);
        dev->GetRenderTarget(0, &rt);
        dev->GetDepthStencilSurface(&ds);
        HRESULT hbb = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
        HRESULT hbs = E_FAIL;
        IDirect3DStateBlock9* sb = nullptr;
        if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) sb = nullptr;
        if (SUCCEEDED(hbb) && bb) {
            dev->SetRenderTarget(0, bb);
            for (DWORD i = 1; i < 4; i++) dev->SetRenderTarget(i, nullptr);
            dev->SetDepthStencilSurface(nullptr);
            // states the ImGui backend does not set but the game may have changed
            dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
            dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
            dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
            dev->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
            dev->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
            dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
            dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFF);
            dev->SetRenderState(D3DRS_COLORVERTEX, TRUE);
            // the game's fixed-function techniques leave texture coordinate transforms / indices behind
            for (DWORD st = 0; st < 2; st++) {
                dev->SetTextureStageState(st, D3DTSS_TEXCOORDINDEX, st);
                dev->SetTextureStageState(st, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
                dev->SetTextureStageState(st, D3DTSS_RESULTARG, D3DTA_CURRENT);
            }
            dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
            dev->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
            dev->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, 0);
            dev->SetVertexShader(nullptr);
            dev->SetPixelShader(nullptr);
            hbs = dev->BeginScene();
            if (SUCCEEDED(hbs)) {
                renderDrawData(dev, ImGui::GetDrawData());
                dev->EndScene();
            }
        }
        if (bb) bb->Release();
        if (sb) { sb->Apply(); sb->Release(); }
        if (rt) { dev->SetRenderTarget(0, rt); rt->Release(); }
        dev->SetDepthStencilSurface(ds);
        if (ds) ds->Release();
        dev->SetViewport(&vp);
    }
}
