#pragma once

#include "pch.h"

namespace DecryptPatch {
    bool injectFileLoad(DWORD* fileloadFktAddr);
    bool injectFileStore(DWORD* fileloadEndFktAddr);
    const char* lastLoadedFile();   // file name the calling thread loaded last ("" if none)
    void clearLastLoadedFile();
}

namespace FogPatch {
    bool injectJumperFog(DWORD* fogStart, DWORD* fogEnd);
}
