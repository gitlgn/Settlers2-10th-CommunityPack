#include "RDTrigger.h"

#include <windows.h>
#include <string>

namespace RDTrigger {

    // the few entries of RENDERDOC_API_1_1_2 that are used (layout from renderdoc_app.h, MIT licence)
    struct Api {
        void* GetAPIVersion;
        void* SetCaptureOptionU32;
        void* SetCaptureOptionF32;
        void* GetCaptureOptionU32;
        void* GetCaptureOptionF32;
        void* SetFocusToggleKeys;
        void* SetCaptureKeys;
        void* GetOverlayBits;
        void* MaskOverlayBits;
        void* RemoveHooks;
        void* UnloadCrashHandler;
        void(__cdecl* SetCaptureFilePathTemplate)(const char*);
        void* GetCaptureFilePathTemplate;
        void* GetNumCaptures;
        void* GetCapture;
        void(__cdecl* TriggerCapture)();
    };
    typedef int(__cdecl* GetApi_t)(int version, void** out);

    static Api* g_api = nullptr;
    static bool g_checked = false;
    static std::string g_trigger;
    static DWORD g_lastCheck = 0;

    void onPresent() {
        if (!g_checked) {
            g_checked = true;
            HMODULE rd = GetModuleHandleA("renderdoc.dll");
            if (!rd) return;
            auto get = (GetApi_t)GetProcAddress(rd, "RENDERDOC_GetAPI");
            void* api = nullptr;
            if (!get || !get(10102, &api) || !api) return;
            g_api = (Api*)api;
            char exe[MAX_PATH];
            GetModuleFileNameA(nullptr, exe, MAX_PATH);
            std::string dir = exe;
            dir = dir.substr(0, dir.find_last_of('\\'));
            g_trigger = dir + "\\rdcapture.trigger";
            CreateDirectoryA((dir + "\\captures").c_str(), nullptr);
            g_api->SetCaptureFilePathTemplate((dir + "\\captures\\s2dng").c_str());
        }
        if (!g_api) return;
        DWORD now = GetTickCount();
        if (now - g_lastCheck < 250) return;
        g_lastCheck = now;
        if (GetFileAttributesA(g_trigger.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        DeleteFileA(g_trigger.c_str());
        g_api->TriggerCapture();
    }
}
