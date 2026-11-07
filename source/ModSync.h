#pragma once

// Data-pack switcher: <game>\mods\<name>\ mirrors <game>\data\.
// Packs are switched on/off through the [Mods] section of d3d9.ini; the originals
// are backed up to <game>\mods\_backup\<name>\ and restored when a pack is switched off.
namespace ModSync {
    void apply(const char* gameDir, const char* iniPath, const char* logPath);
}
