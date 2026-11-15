#include "Overlay.h"
#include "FixDecisions.h"
#include "GfxPatches.h"

#include <windows.h>
#include <cstdarg>
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
        { "Effects", "Graphics", "DOFStrength", T_FLOAT, "0.3", 0, 1, nullptr, "Depth of field haze", "0 = off, 1 = original game.", true },
        { "Effects", "Graphics", "FXAA", T_BOOL, "on", 0, 0, nullptr, "FXAA edge smoothing", "", true },
        { "Effects", "Graphics", "SSAO", T_BOOL, "on", 0, 0, nullptr, "Ambient occlusion (SSAO)", "Contact shadows under trees, houses and in corners.", true },
        { "Effects", "Graphics", "SSAOStrength", T_FLOAT, "0.6", 0, 1, nullptr, "SSAO strength", "", true },
        { "Effects", "Graphics", "SSAORadius", T_FLOAT, "1.0", 0.25f, 4, nullptr, "SSAO radius", "", true },
        { "Effects", "Graphics", "ImprovedTerrain", T_BOOL, "on", 0, 0, nullptr, "Improved terrain", "Sky/ground light and a detail layer at close zoom.", true },
        { "Effects", "Graphics", "TerrainDetail", T_FLOAT, "0.5", 0, 1, nullptr, "Terrain detail", "", true },
        { "Effects", "Graphics", "ImprovedWater", T_BOOL, "on", 0, 0, nullptr, "Improved water", "Fresnel reflection, sun glints, shore colour and foam.", true },
        { "Effects", "Graphics", "ImprovedObjects", T_BOOL, "on", 0, 0, nullptr, "Improved object light", "Houses, trees, rocks with sky/ground light.", true },
        { "Effects", "Graphics", "UnitRimLight", T_FLOAT, "0.5", 0, 1, nullptr, "Rim light on settlers", "0 = off.", true },
        { "Effects", "Graphics", "Bloom", T_FLOAT, "0", 0, 1, nullptr, "Bloom", "Soft glow around bright areas (sunlit water, white walls). 0 = off. Needs post-processing.", true },
        { "Effects", "Graphics", "BloomThreshold", T_FLOAT, "0.75", 0.5f, 0.95f, nullptr, "Bloom threshold", "How bright an area must be to glow.", true },

        { "Packs", "Mods", "HDTextures", T_BOOL, "on", 0, 0, nullptr, "HD textures (4K)", "S2-modders 4K remakes.", false },
        { "Packs", "Mods", "HDInterface", T_BOOL, "on", 0, 0, nullptr, "HD interface", "2x upscaled interface textures and fonts.", false },
        { "Packs", "Mods", "LODFix", T_BOOL, "on", 0, 0, nullptr, "View distance fix", "Less pop-in when zoomed out.", false },
        { "Packs", "Mods", "SharpShadows", T_BOOL, "on", 0, 0, nullptr, "Precise shadows (32-bit)", "", false },
        { "Packs", "Mods", "SmoothShadows", T_BOOL, "on", 0, 0, nullptr, "Soft shadows",
          "Also carries the improved terrain, water and object shaders.", false },
        { "Packs", "Mods", "ImprovedPostFX", T_BOOL, "on", 0, 0, nullptr, "Softer post-processing", "Gaussian blur; needed for FXAA/SSAO.", false },

        { "Interface", "UI", "Scale", T_FLOAT, "1.5", 1, 3, nullptr, "HUD scale", "1.0 = off (original size).", false },
        { "Interface", "Misc", "SkipIntro", T_BOOL, "off", 0, 0, nullptr, "Skip intro videos",
          "No logo videos and no intro film: straight to the main menu.", false },
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
            it.f = FixDecisions::parseIniFloat(v.c_str(), FixDecisions::parseIniFloat(it.def, 0.0f));
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
        else if (it.live) GfxPatches::setLive(it.key, it.type == T_BOOL ? (it.b ? 1.0f : 0.0f) : it.f);
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
        logf("menu ready (build " __DATE__ " " __TIME__ "), game input imports redirected: %s", n);
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

    // the visible game window of this process, subclassed for the menu key / wheel / input filter. The largest visible
    // top-level window is taken (not simply the foreground one: on some PCs a small 160x28 helper window of the game
    // is in front during the first frames, which left the menu without mouse and drawing area)
    static HWND g_hooked = nullptr;
    struct WndSearch { HWND best; LONG area; };
    static BOOL CALLBACK findWindowCb(HWND w, LPARAM lp) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(w)) return TRUE;
        RECT r;
        if (!GetClientRect(w, &r)) return TRUE;
        auto* s = (WndSearch*)lp;
        LONG a = r.right * r.bottom;
        if (a > s->area) { s->area = a; s->best = w; }
        return TRUE;
    }
    static void attachWindow() {
        static unsigned frame = 0;
        if (g_hooked && (frame++ % 120)) return;            // look again every 120 frames (window recreated?)
        WndSearch s = { nullptr, 0 };
        EnumWindows(findWindowCb, (LPARAM)&s);
        HWND fg = s.best;
        if (!fg || fg == g_hooked || s.area < 320 * 240) return;
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
        HRESULT hc = g_dev ? g_dev->CreateTexture(t->Width, t->Height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tx, nullptr) : E_POINTER;
        static int nlog = 0;
        if (nlog++ < 20) {
            char b[160]; _snprintf_s(b, sizeof(b), _TRUNCATE, "menu: font atlas %dx%d format %d -> texture %p (0x%08lX)", t->Width, t->Height, (int)t->Format, tx, hc);
            logf("%s", b);
        }
        if (FAILED(hc)) return;
        t->SetTexID((ImTextureID)(intptr_t)tx);
        if (old) old->Release();
        D3DLOCKED_RECT lr;
        HRESULT hl = tx->LockRect(0, &lr, nullptr, 0);
        if (FAILED(hl) && nlog < 20) { char b[80]; _snprintf_s(b, sizeof(b), _TRUNCATE, "menu: atlas lock failed (0x%08lX)", hl); logf("%s", b); }
        if (SUCCEEDED(hl)) {
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
            // diagnostics: read the texture back - does it hold what was written? (pixels with alpha, white ones)
            if (nlog < 20 && SUCCEEDED(tx->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) {
                unsigned withAlpha = 0, white = 0, srcAlpha = 0;
                for (int y = 0; y < t->Height; y++) {
                    const DWORD* p = (const DWORD*)((const BYTE*)lr.pBits + y * lr.Pitch);
                    const ImU32* src = (const ImU32*)t->GetPixelsAt(0, y);
                    for (int x = 0; x < t->Width; x++) {
                        if (p[x] >> 24) withAlpha++;
                        if ((p[x] & 0xFFFFFF) == 0xFFFFFF) white++;
                        if (src[x] >> 24) srcAlpha++;
                    }
                }
                tx->UnlockRect(0);
                char b[160];
                _snprintf_s(b, sizeof(b), _TRUNCATE, "menu: atlas readback: %u pixels with alpha (source %u), %u white, pitch %d",
                            withAlpha, srcAlpha, white, lr.Pitch);
                logf("%s", b);
            }
        }
        t->SetStatus(ImTextureStatus_OK);
    }

    // diagnostics: the back buffer right after the overlay draw, saved once as bin\menu_<what>.bmp (MSAA resolved)
    // diagnostics: four 64x64 test fields at the top left, each one step further down the drawing pipeline
    // (1 red: Clear, 2 green: shader draw from memory, 3 blue: shader draw from a vertex buffer, 4: font texture)
    static void drawTestFields(IDirect3DDevice9* dev, IDirect3DTexture9* tex) {
        struct V { float x, y, z, w; DWORD c; float u, v; };
        auto quad = [](V* q, float x0, DWORD c, float u0, float v0, float u1, float v1) {
            float y0 = 0.0f, x1 = x0 + 64.0f, y1 = 64.0f;
            q[0] = { x0 - 0.5f, y0 - 0.5f, 0, 1, c, u0, v0 }; q[1] = { x1 - 0.5f, y0 - 0.5f, 0, 1, c, u1, v0 }; q[2] = { x1 - 0.5f, y1 - 0.5f, 0, 1, c, u1, v1 };
            q[3] = { x0 - 0.5f, y0 - 0.5f, 0, 1, c, u0, v0 }; q[4] = { x1 - 0.5f, y1 - 0.5f, 0, 1, c, u1, v1 }; q[5] = { x0 - 0.5f, y1 - 0.5f, 0, 1, c, u0, v1 };
        };
        HRESULT h[4] = { E_FAIL, E_FAIL, E_FAIL, E_FAIL };
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        D3DRECT r = { 0, 0, 64, 64 };
        h[0] = dev->Clear(1, &r, D3DCLEAR_TARGET, 0xFFFF0000, 1.0f, 0);
        // white texel of the font atlas: ImGui keeps it at the very first pixels; a full-quad sample of the atlas instead
        // shows the glyphs themselves (step 4)
        IDirect3DTexture9* white = nullptr;
        if (SUCCEEDED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &white, nullptr))) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(white->LockRect(0, &lr, nullptr, 0))) { *(DWORD*)lr.pBits = 0xFFFFFFFF; white->UnlockRect(0); }
        }
        V q[6];
        dev->SetTexture(0, white);
        quad(q, 64, 0xFF00FF00, 0, 0, 1, 1);
        h[1] = dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, q, sizeof(V));
        IDirect3DVertexBuffer9* vbuf = nullptr;
        if (SUCCEEDED(dev->CreateVertexBuffer(sizeof(q), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vbuf, nullptr))) {
            void* p = nullptr;
            quad(q, 128, 0xFF0000FF, 0, 0, 1, 1);
            if (SUCCEEDED(vbuf->Lock(0, 0, &p, 0))) { memcpy(p, q, sizeof(q)); vbuf->Unlock(); }
            dev->SetStreamSource(0, vbuf, 0, sizeof(V));
            h[2] = dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
            dev->SetStreamSource(0, nullptr, 0, 0);
            vbuf->Release();
        }
        dev->SetTexture(0, tex);
        quad(q, 192, 0xFFFFFFFF, 0, 0, 1, 1);
        h[3] = dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, q, sizeof(V));
        if (white) white->Release();
        char b[200];
        _snprintf_s(b, sizeof(b), _TRUNCATE, "menu: test fields: clear 0x%08lX, draw from memory 0x%08lX, vertex buffer 0x%08lX, font texture 0x%08lX (texture %p)",
                    h[0], h[1], h[2], h[3], (void*)tex);
        logf("%s", b);
        if (tex) {
            D3DSURFACE_DESC td = {};
            tex->GetLevelDesc(0, &td);
            _snprintf_s(b, sizeof(b), _TRUNCATE, "menu: font texture: %ux%u format %d pool %d usage %lX levels %lu",
                        td.Width, td.Height, (int)td.Format, (int)td.Pool, td.Usage, tex->GetLevelCount());
            logf("%s", b);
        }

        // second row of fields (x 256..511): white textures made in different ways, all must come out white
        auto fill = [](IDirect3DTexture9* t, DWORD c) {
            D3DLOCKED_RECT lr; D3DSURFACE_DESC d = {};
            if (!t || FAILED(t->GetLevelDesc(0, &d)) || FAILED(t->LockRect(0, &lr, nullptr, 0))) return E_FAIL;
            for (UINT y = 0; y < d.Height; y++) { DWORD* p = (DWORD*)((BYTE*)lr.pBits + y * lr.Pitch); for (UINT x = 0; x < d.Width; x++) p[x] = c; }
            return t->UnlockRect(0);
        };
        IDirect3DTexture9 *t5 = nullptr, *t6 = nullptr, *t7 = nullptr, *t7s = nullptr, *t8 = nullptr;
        HRESULT c5 = dev->CreateTexture(512, 128, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t5, nullptr);
        HRESULT f5 = fill(t5, 0xFFFFFFFF);
        HRESULT c6 = dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t6, nullptr);
        HRESULT f6 = fill(t6, 0xFFFFFFFF);
        HRESULT c7 = dev->CreateTexture(512, 128, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t7, nullptr);
        HRESULT c7s = dev->CreateTexture(512, 128, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &t7s, nullptr);
        HRESULT f7 = fill(t7s, 0xFFFFFFFF);
        HRESULT u7 = (t7 && t7s) ? dev->UpdateTexture(t7s, t7) : E_FAIL;
        // 8: the atlas content copied into a brand-new managed texture of the same size
        HRESULT c8 = E_FAIL, f8 = E_FAIL;
        if (tex) {
            D3DSURFACE_DESC td = {};
            tex->GetLevelDesc(0, &td);
            c8 = dev->CreateTexture(td.Width, td.Height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t8, nullptr);
            D3DLOCKED_RECT src, dst;
            if (SUCCEEDED(c8) && SUCCEEDED(tex->LockRect(0, &src, nullptr, D3DLOCK_READONLY))) {
                if (SUCCEEDED(t8->LockRect(0, &dst, nullptr, 0))) {
                    for (UINT y = 0; y < td.Height; y++) memcpy((BYTE*)dst.pBits + y * dst.Pitch, (BYTE*)src.pBits + y * src.Pitch, td.Width * 4);
                    f8 = t8->UnlockRect(0);
                }
                tex->UnlockRect(0);
            }
        }
        IDirect3DTexture9* ts[4] = { t5, t6, t7, t8 };
        HRESULT d[4] = { E_FAIL, E_FAIL, E_FAIL, E_FAIL };
        for (int i = 0; i < 4; i++) {
            if (!ts[i]) continue;
            dev->SetTexture(0, ts[i]);
            quad(q, 256.0f + 64.0f * i, 0xFFFFFFFF, 0, 0, 1, 1);
            d[i] = dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, q, sizeof(V));
        }
        dev->SetTexture(0, nullptr);
        for (auto* t : { t5, t6, t7, t7s, t8 }) if (t) t->Release();
        _snprintf_s(b, sizeof(b), _TRUNCATE,
            "menu: texture fields: 5 managed 512x128 0x%08lX/0x%08lX/0x%08lX, 6 managed 64x64 0x%08lX/0x%08lX/0x%08lX, 7 default+UpdateTexture 0x%08lX/0x%08lX/0x%08lX/0x%08lX/0x%08lX",
            c5, f5, d[0], c6, f6, d[1], c7, c7s, f7, u7, d[2]);
        logf("%s", b);
        _snprintf_s(b, sizeof(b), _TRUNCATE, "menu: texture field 8 (atlas copy) 0x%08lX/0x%08lX/0x%08lX", c8, f8, d[3]);
        logf("%s", b);
    }

    static void captureBackBuffer(IDirect3DDevice9* dev, IDirect3DSurface9* bb, const char* what) {
        D3DSURFACE_DESC d = {};
        bb->GetDesc(&d);
        IDirect3DSurface9 *rt = nullptr, *sys = nullptr;
        HRESULT h1 = dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr);
        HRESULT h2 = SUCCEEDED(h1) ? dev->StretchRect(bb, nullptr, rt, nullptr, D3DTEXF_NONE) : h1;
        HRESULT h3 = SUCCEEDED(h2) ? dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr) : h2;
        HRESULT h4 = SUCCEEDED(h3) ? dev->GetRenderTargetData(rt, sys) : h3;
        D3DLOCKED_RECT lr;
        HRESULT h5 = SUCCEEDED(h4) ? sys->LockRect(&lr, nullptr, D3DLOCK_READONLY) : h4;
        std::string path = g_log.substr(0, g_log.find_last_of('\\') + 1) + what + ".bmp";
        if (SUCCEEDED(h5)) {
            FILE* f = nullptr;
            if (fopen_s(&f, path.c_str(), "wb") == 0 && f) {
                DWORD rowBytes = (d.Width * 3 + 3) & ~3u, img = rowBytes * d.Height;
                BYTE hdr[54] = { 'B', 'M' };
                *(DWORD*)(hdr + 2) = 54 + img; *(DWORD*)(hdr + 10) = 54; *(DWORD*)(hdr + 14) = 40;
                *(LONG*)(hdr + 18) = (LONG)d.Width; *(LONG*)(hdr + 22) = -(LONG)d.Height;
                *(WORD*)(hdr + 26) = 1; *(WORD*)(hdr + 28) = 24; *(DWORD*)(hdr + 34) = img;
                fwrite(hdr, 1, 54, f);
                std::vector<BYTE> row(rowBytes, 0);
                for (UINT y = 0; y < d.Height; y++) {
                    const DWORD* p = (const DWORD*)((const BYTE*)lr.pBits + y * lr.Pitch);
                    for (UINT x = 0; x < d.Width; x++) { row[x * 3] = (BYTE)p[x]; row[x * 3 + 1] = (BYTE)(p[x] >> 8); row[x * 3 + 2] = (BYTE)(p[x] >> 16); }
                    fwrite(row.data(), 1, rowBytes, f);
                }
                fclose(f);
            }
            sys->UnlockRect();
        }
        char b[220];
        _snprintf_s(b, sizeof(b), _TRUNCATE, "menu: back buffer capture %s: format %d %ux%u ms %d -> %s (0x%08lX 0x%08lX 0x%08lX 0x%08lX 0x%08lX)",
                    what, (int)d.Format, d.Width, d.Height, (int)d.MultiSampleType, path.c_str(), h1, h2, h3, h4, h5);
        logf("%s", b);
        if (sys) sys->Release();
        if (rt) rt->Release();
    }

    // diagnostics: device state at the overlay draw (to compare PCs), written once for the hint and once for the menu
    static const char* g_dumpPending = nullptr;
    static void dumpState(IDirect3DDevice9* d, ImDrawData* dd, const char* what) {
        std::string o = std::string("menu: state dump (") + what + ")\n";
        char b[200];
        auto add = [&](const char* fmt, ...) { va_list ap; va_start(ap, fmt); _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, ap); va_end(ap); o += b; };
        add("  rs:");
        for (DWORD r = 7; r <= 209; r++) { DWORD v = 0; if (SUCCEEDED(d->GetRenderState((D3DRENDERSTATETYPE)r, &v))) add(" %lu=%lX", r, v); }
        add("\n");
        for (DWORD st = 0; st < 3; st++) {
            add("  tss%lu:", st);
            for (DWORD t = 1; t <= 32; t++) { DWORD v = 0; if (SUCCEEDED(d->GetTextureStageState(st, (D3DTEXTURESTAGESTATETYPE)t, &v))) add(" %lu=%lX", t, v); }
            add("\n  samp%lu:", st);
            for (DWORD t = 1; t <= 13; t++) { DWORD v = 0; if (SUCCEEDED(d->GetSamplerState(st, (D3DSAMPLERSTATETYPE)t, &v))) add(" %lu=%lX", t, v); }
            IDirect3DBaseTexture9* tx = nullptr; d->GetTexture(st, &tx);
            add(" tex=%p", (void*)tx); if (tx) tx->Release();
            add("\n");
        }
        DWORD fvf = 0; d->GetFVF(&fvf);
        IDirect3DVertexDeclaration9* decl = nullptr; d->GetVertexDeclaration(&decl);
        IDirect3DVertexShader9* vs = nullptr; d->GetVertexShader(&vs);
        IDirect3DPixelShader9* ps = nullptr; d->GetPixelShader(&ps);
        UINT f0 = 0, f1 = 0; d->GetStreamSourceFreq(0, &f0); d->GetStreamSourceFreq(1, &f1);
        RECT sc = {}; d->GetScissorRect(&sc);
        D3DVIEWPORT9 v = {}; d->GetViewport(&v);
        add("  fvf=%lX decl=%p vs=%p ps=%p freq0=%X freq1=%X scissor=%ld,%ld-%ld,%ld viewport=%lu,%lu %lux%lu z %.2f-%.2f\n",
            fvf, (void*)decl, (void*)vs, (void*)ps, f0, f1, sc.left, sc.top, sc.right, sc.bottom, v.X, v.Y, v.Width, v.Height, v.MinZ, v.MaxZ);
        if (decl) decl->Release();
        if (vs) vs->Release();
        if (ps) ps->Release();
        IDirect3DSurface9* rts[4] = {};
        for (DWORD i = 0; i < 4; i++) d->GetRenderTarget(i, &rts[i]);
        add("  rt0=%p rt1=%p rt2=%p rt3=%p\n", (void*)rts[0], (void*)rts[1], (void*)rts[2], (void*)rts[3]);
        for (auto* r : rts) if (r) r->Release();
        if (dd && dd->CmdListsCount > 0) {
            const ImDrawList* cl = dd->CmdLists[0];
            for (int i = 0; i < cl->VtxBuffer.Size && i < 4; i++) {
                const ImDrawVert& w = cl->VtxBuffer[i];
                add("  vtx%d pos %.1f,%.1f uv %.4f,%.4f col %08X\n", i, w.pos.x, w.pos.y, w.uv.x, w.uv.y, w.col);
            }
            for (int i = 0; i < cl->CmdBuffer.Size && i < 4; i++) {
                const ImDrawCmd& c = cl->CmdBuffer[i];
                add("  cmd%d clip %.0f,%.0f-%.0f,%.0f tex %p elems %u vtxoff %u idxoff %u\n", i, c.ClipRect.x, c.ClipRect.y, c.ClipRect.z, c.ClipRect.w,
                    (void*)(intptr_t)c.GetTexID(), c.ElemCount, c.VtxOffset, c.IdxOffset);
            }
        }
        if (!o.empty() && o.back() == '\n') o.pop_back();
        logf("%s", o.c_str());
    }

    // Shader path: on an NVIDIA PC the pre-transformed (XYZRHW) fixed-function draw came out offset with black glyph
    // boxes, although every device state was identical to a PC where it works (state dumps compared). The game's own
    // HUD draws with vertex/pixel shaders and works everywhere, so the menu does the same: assembled at runtime with
    // the game's d3dx9_29 (no extra files); the fixed-function path remains as fallback.
    struct ID3DXBufferMin : IUnknown { virtual LPVOID STDMETHODCALLTYPE GetBufferPointer() = 0; virtual DWORD STDMETHODCALLTYPE GetBufferSize() = 0; };
    typedef HRESULT (WINAPI* AssembleShader_t)(LPCSTR, UINT, const void*, void*, DWORD, ID3DXBufferMin**, ID3DXBufferMin**);
    static IDirect3DVertexShader9* g_vs = nullptr;
    static IDirect3DPixelShader9* g_ps = nullptr;
    static IDirect3DVertexDeclaration9* g_decl = nullptr;
    static bool g_shaderTried = false;

    static void* assemble(AssembleShader_t asmf, const char* src) {
        ID3DXBufferMin *code = nullptr, *err = nullptr;
        HRESULT h = asmf(src, (UINT)strlen(src), nullptr, nullptr, 0, &code, &err);
        if (FAILED(h) || !code) {
            logf("menu: shader assembly failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
            if (err) err->Release();
            return nullptr;
        }
        if (err) err->Release();
        return code;
    }

    static void createShaders(IDirect3DDevice9* dev) {
        g_shaderTried = true;
        HMODULE dx = GetModuleHandleA("d3dx9_29.dll");
        auto asmf = dx ? (AssembleShader_t)GetProcAddress(dx, "D3DXAssembleShader") : nullptr;
        if (!asmf) { logf("menu: D3DXAssembleShader not found - fixed-function drawing"); return; }
        // c0 = (2/width, -2/height, -1, 1): screen position -> clip space
        const char* vsSrc =
            "vs_2_0\n dcl_position v0\n dcl_color v1\n dcl_texcoord v2\n"
            " mad r0.xy, v0.xy, c0.xy, c0.zw\n mov r0.zw, c1.xy\n mov oPos, r0\n mov oD0, v1\n mov oT0.xy, v2.xy\n";
        const char* psSrc =
            "ps_2_0\n dcl t0.xy\n dcl v0\n dcl_2d s0\n texld r0, t0, s0\n mul r0, r0, v0\n mov oC0, r0\n";
        auto* vb = (ID3DXBufferMin*)assemble(asmf, vsSrc);
        auto* pb = (ID3DXBufferMin*)assemble(asmf, psSrc);
        HRESULT h1 = E_FAIL, h2 = E_FAIL, h3 = E_FAIL;
        if (vb) { h1 = dev->CreateVertexShader((const DWORD*)vb->GetBufferPointer(), &g_vs); vb->Release(); }
        if (pb) { h2 = dev->CreatePixelShader((const DWORD*)pb->GetBufferPointer(), &g_ps); pb->Release(); }
        const D3DVERTEXELEMENT9 el[] = {
            { 0, 0,  D3DDECLTYPE_FLOAT4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
            { 0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
            { 0, 20, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
            D3DDECL_END() };
        h3 = dev->CreateVertexDeclaration(el, &g_decl);
        char b[128];
        _snprintf_s(b, sizeof(b), _TRUNCATE, "menu: shader drawing %s (vs 0x%08lX, ps 0x%08lX, decl 0x%08lX)",
                    SUCCEEDED(h1) && SUCCEEDED(h2) && SUCCEEDED(h3) ? "on" : "off - fixed-function", h1, h2, h3);
        logf("%s", b);
        if (FAILED(h1) || FAILED(h2) || FAILED(h3)) {
            if (g_vs) { g_vs->Release(); g_vs = nullptr; }
            if (g_ps) { g_ps->Release(); g_ps = nullptr; }
            if (g_decl) { g_decl->Release(); g_decl = nullptr; }
        }
    }

    // Own ImGui renderer: draws straight from memory (DrawIndexedPrimitiveUP); the DX9 backend is only used for its
    // texture management.
    static void renderDrawData(IDirect3DDevice9* dev, ImDrawData* dd) {
        if (!dd || dd->DisplaySize.x <= 0 || dd->DisplaySize.y <= 0) return;
        if (dd->Textures)
            for (ImTextureData* t : *dd->Textures)
                if (t->Status != ImTextureStatus_OK) updateTexture(t);
        struct V { float x, y, z, w; DWORD c; float u, v; };
        static std::vector<V> vb;
        if (!g_shaderTried) createShaders(dev);
        if (g_vs && g_ps && g_decl) {
            const float c[8] = { 2.0f / dd->DisplaySize.x, -2.0f / dd->DisplaySize.y, -1.0f, 1.0f,   0.0f, 1.0f, 0.0f, 0.0f };
            dev->SetVertexDeclaration(g_decl);
            dev->SetVertexShader(g_vs);
            dev->SetPixelShader(g_ps);
            dev->SetVertexShaderConstantF(0, c, 2);
        } else {
            dev->SetVertexShader(nullptr);
            dev->SetPixelShader(nullptr);
            dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        }
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
        if (g_dumpPending) { dumpState(dev, dd, g_dumpPending); g_dumpPending = nullptr; }
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
                HRESULT hd = dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)(cl->VtxBuffer.Size - (int)cmd.VtxOffset), cmd.ElemCount / 3,
                                            cl->IdxBuffer.Data + cmd.IdxOffset, sizeof(ImDrawIdx) == 2 ? D3DFMT_INDEX16 : D3DFMT_INDEX32,
                                            vb.data() + cmd.VtxOffset, sizeof(V));
                static int failLog = 0;
                if (FAILED(hd) && failLog < 5) { failLog++; char b[80]; _snprintf_s(b, sizeof(b), _TRUNCATE, "menu: draw call failed (0x%08lX)", hd); logf("%s", b); }
            }
        }
    }

    // comparison shots: bin\compare.trigger -> bin\compare_<n>_off.bmp (shader effects and distance fog as in the
    // original game) and compare_<n>_on.bmp (the user's settings) a few frames apart, taken before the menu is drawn
    static void compareShots(IDirect3DDevice9* dev) {
        static int state = 0, wait = 0, n = 0, frame = 0;
        static bool fog = false;
        if (state == 0) {
            if (++frame % 30) return;
            std::string trig = g_log.substr(0, g_log.find_last_of('\\') + 1) + "compare.trigger";
            if (g_log.empty() || GetFileAttributesA(trig.c_str()) == INVALID_FILE_ATTRIBUTES) return;
            DeleteFileA(trig.c_str());
            fog = GfxPatches::distanceFog();
            GfxPatches::setVanillaShaders(true);
            GfxPatches::setDistanceFog(true);
            n++; state = 1; wait = 4;
            return;
        }
        if (--wait > 0) return;
        IDirect3DSurface9* bb = nullptr;
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
        char name[32];
        _snprintf_s(name, sizeof(name), _TRUNCATE, "compare_%d_%s", n, state == 1 ? "off" : "on");
        captureBackBuffer(dev, bb, name);
        bb->Release();
        if (state == 1) {
            GfxPatches::setVanillaShaders(false);
            GfxPatches::setDistanceFog(fog);
            state = 2; wait = 4;
        } else state = 0;
    }

    // The menu keeps the device it was set up with. If the game presents through a different device (some PCs create a
    // second one), textures and shaders made on the first device cannot be used there: switch to the presenting
    // device and create them again on it.
    static void followDevice(IDirect3DDevice9* dev) {
        if (!dev || dev == g_dev) return;
        char b[160];
        _snprintf_s(b, sizeof(b), _TRUNCATE, "menu: presenting device %p differs from the menu's device %p - switching, textures and shaders recreated",
                    (void*)dev, (void*)g_dev);
        logf("%s", b);
        g_dev = dev;
        for (ImTextureData* t : ImGui::GetPlatformIO().Textures) {
            if (auto* tx = (IDirect3DTexture9*)(intptr_t)t->GetTexID()) tx->Release();
            t->SetTexID(ImTextureID_Invalid);
            if (t->Status != ImTextureStatus_Destroyed) t->SetStatus(ImTextureStatus_WantCreate);
        }
        if (g_vs) { g_vs->Release(); g_vs = nullptr; }
        if (g_ps) { g_ps->Release(); g_ps = nullptr; }
        if (g_decl) { g_decl->Release(); g_decl = nullptr; }
        g_shaderTried = false;
    }

    void onPresent(IDirect3DDevice9* dev) {
        if (!g_ready) return;
        followDevice(dev);
        compareShots(dev);
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
            static int dlog = 0;
            if (dlog < 3 || (g_open && dlog < 6)) {
                D3DSURFACE_DESC bd = {}; bb->GetDesc(&bd);
                D3DVIEWPORT9 v2 = {}; dev->GetViewport(&v2);
                ImDrawData* dd = ImGui::GetDrawData();
                char b[256]; _snprintf_s(b, sizeof(b), _TRUNCATE,
                    "menu: draw %s back buffer %ux%u ms %d, viewport %lu,%lu %lux%lu, display %.0fx%.0f, lists %d, vertices %d, game viewport %lu,%lu %lux%lu",
                    g_open ? "menu" : "hint", bd.Width, bd.Height, (int)bd.MultiSampleType, v2.X, v2.Y, v2.Width, v2.Height,
                    dd ? dd->DisplaySize.x : 0.f, dd ? dd->DisplaySize.y : 0.f, dd ? dd->CmdListsCount : 0, dd ? dd->TotalVtxCount : 0,
                    vp.X, vp.Y, vp.Width, vp.Height);
                logf("%s", b);
                dlog++;
            }
            static bool dumpedHint = false, dumpedMenu = false;
            bool& dumped = g_open ? dumpedMenu : dumpedHint;
            if (!dumped) { dumped = true; g_dumpPending = g_open ? "menu" : "hint"; }
            hbs = dev->BeginScene();
            if (SUCCEEDED(hbs)) {
                renderDrawData(dev, ImGui::GetDrawData());
                dev->EndScene();
                static bool capHint = false, capMenu = false;
                bool& cap = g_open ? capMenu : capHint;
                static int hintFrames = 0;
                // the hint after a few frames (atlas uploaded), the menu on its third frame
                if (!cap && (g_open ? ++hintFrames > 2 : ++hintFrames > 30)) {
                    cap = true; hintFrames = 0;
                    if (!g_open && SUCCEEDED(dev->BeginScene())) {   // test fields once, in the hint capture
                        ImTextureData* td = ImGui::GetIO().Fonts->TexData;
                        drawTestFields(dev, td ? (IDirect3DTexture9*)(intptr_t)td->GetTexID() : nullptr);
                        dev->EndScene();
                    }
                    captureBackBuffer(dev, bb, g_open ? "menu_menu" : "menu_hint");
                }
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
