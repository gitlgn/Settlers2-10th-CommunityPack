#pragma once

#include <windows.h>

// Keeps the memory of d3dx9_29.dll below 2 GB.
//
// The game's D3DX (d3dx9_29, 2006) cannot handle memory above 2 GB: with the large-address-aware
// flag every effect compile fails with E_OUTOFMEMORY as soon as its allocations land above 2 GB
// (reproduced with tools\laafx.exe and [Misc] LAAStressTest). Normal play only gets there late,
// when the lower 2 GB are used up - and then the game cannot build shaders any more.
//
// reserve() sets aside address space below 2 GB at startup, while it is still free: a fixed-size
// Windows heap for small blocks and a page allocator for large ones. patch() redirects the
// msvcrt malloc/calloc/realloc/free/new/delete and kernel32 HeapAlloc/HeapFree imports of one module (D3DX)
// to them. When the reserve is full, allocations fall back to the normal allocator, so the
// behaviour is never worse than without it. Frees are routed by address.
namespace LowHeap {
    struct Stats {
        ULONG_PTR smallBase, smallSize, largeBase, largeSize;
        LONG smallAllocs, largeAllocs, fallbacks, foreignFrees;
        SIZE_T largeUsedPages, largePeakPages;
    };

    // smallMB: fixed heap for blocks below 256 KB; largeMB: page allocator for larger blocks.
    // Returns false if no address space below 2 GB could be reserved (nothing is patched then).
    bool reserve(unsigned smallMB, unsigned largeMB);

    // Redirects the allocator imports of <module>; returns the number of patched import slots.
    int patch(HMODULE module);

    bool active();
    Stats stats();
}

namespace LowHeap {
    // [Misc] LowHeap (default on). Only active when the exe has the large-address-aware flag and runs
    // on 64-bit Windows (only then can memory above 2 GB occur). Must run before LAAStress::install.
    void install(const char* iniPath, const char* logPath);
    // patches d3dx9_29.dll if it was not loaded yet at install time (UPX loads the imports late)
    void onDeviceCreated();
}
