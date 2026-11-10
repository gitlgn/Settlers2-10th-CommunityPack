#pragma once

// Small byte patches for bugs in the original S2DNG.exe (GOG 11757), [Misc] GameFixes = on.
// Every patch checks the original bytes first and is skipped on any other build.
//
//  1. Network buffer append (0x412C50): when growing the buffer (realloc) fails, the received data
//     was still copied into the old, smaller buffer -> heap overflow / memory corruption. Now the
//     data is dropped and the buffer counters stay unchanged.
//  2. Game command 0x2002F (0x610390) wrote a value from the command to building+0x3C8+index*4 with an
//     unchecked index (table of 5) -> memory outside the object could be overwritten. Now 0..4 only.
//  3. Game command 0x20022 (0x60F6F0) used obj+0x14C+index*8 with an unchecked index (6 entries).
//     Out-of-range commands are ignored - identical on every PC running the fix.
namespace BinaryFixes {
    void init(const char* iniPath, bool supportedExe, const char* logPath);
    void onDeviceCreated();   // exe unpacked: apply
}
