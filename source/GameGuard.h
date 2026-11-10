#pragma once

// Robustness fixes for known problems of the game (community reports after the last official patch):
//  - profile guard: a damaged Documents\S2\users\<profile>.cfg resets all settings and the campaign
//    progress. Valid profiles are backed up at every start; a damaged one is restored from the backup.
//  - mesh cache guard: crashes caused by a damaged or unwritable mesh/shader cache (%LOCALAPPDATA%\S2).
//    After an abnormal end of the previous session the cache is cleared; an unwritable cache folder
//    or [Misc] MeshCache = off disables it (engine.ini useMeshCache).
namespace GameGuard {
    // call in DllMain before the game reads any file; logPath may be nullptr (events are still logged
    // to <game>\bin\gameguard.log when something was repaired)
    void startup(const char* gameRoot, const char* iniPath, bool debug);
    // value for engine.ini useMeshCache
    bool meshCacheAllowed();
    // DLL_PROCESS_DETACH on process exit: the session ended normally
    void onCleanExit();
}
