#pragma once

// Small byte patches for bugs in the original S2DNG.exe (GOG 11757), [Misc] GameFixes = on.
// Every patch checks the original bytes first and is skipped on any other build.
//
//  1. Network buffer append (0x412C50): when growing the buffer (realloc) fails, the received data
//     was still copied into the old, smaller buffer -> heap overflow / memory corruption. Now the
//     data is dropped and the buffer counters stay unchanged.
namespace BinaryFixes {
    void init(const char* iniPath, bool supportedExe, const char* logPath);
    void onDeviceCreated();   // exe unpacked: apply
}
