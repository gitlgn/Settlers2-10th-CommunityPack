#pragma once

// Test for the large-address-aware flag ([Misc] LAAStressTest = on):
// reserves (without committing) the free address space below 2 GB at startup, so that the game's
// memory is placed above 2 GB. Code that cannot handle such addresses then fails quickly.
// Logs to <game>\bin\laastress.log.
namespace LAAStress {
    void install(const char* iniPath, const char* logPath);
}
