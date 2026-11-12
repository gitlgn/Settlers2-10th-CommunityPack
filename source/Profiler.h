#pragma once

// Developer aid ([Misc] DebugMode): sampling profiler for the game's render/main thread.
// Creating <game>\bin\profile.trigger starts a 30 s recording: every ~1 ms the thread that calls Present is
// suspended briefly and its instruction pointer (plus a few frames of its EBP chain) is counted; frame times are
// recorded too. The result goes to bin\profile.log (top addresses as module+offset, inclusive caller counts,
// frame time statistics).
namespace Profiler {
    void init(const char* logPath);     // nullptr = off
    void onPresent();
}
