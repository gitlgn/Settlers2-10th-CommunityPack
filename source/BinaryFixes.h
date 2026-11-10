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
//  4. Commands that use a player (21 of 46 types) are skipped when their sender no longer resolves to a
//     player (-1, e.g. a player left while commands were queued): the lookup 0x454200 has no range check.
//  5. Memory stream read (0x412CD0, savegames, maps, network): pos+size was compared signed, so a damaged
//     huge size passed the check -> huge copy. Now unsigned with overflow check.
//  6. Memory stream string read (0x6D7E8C): the 16-bit length from the data was never checked against the
//     remaining data (over-read; negative = ~4 GB). The length is clamped to the remaining bytes.
//  7. File string read (0x448B10): length from the file read into a 2 KB stack buffer without a check ->
//     stack buffer overflow with a damaged or crafted map/savegame. Clamped to 0..0x7FF.
//  8. Game load (0x44A440): a C++ exception from a damaged file (e.g. 'vector<T> too long') quit the game.
//     The load now runs in an exception frame and is aborted instead (access violations still crash).
namespace BinaryFixes {
    void init(const char* iniPath, bool supportedExe, const char* logPath, bool trace);   // trace: report commands to NetTrace
    void onDeviceCreated();   // exe unpacked: apply
}
