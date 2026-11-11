#pragma once

// Developer aid: when the game runs under RenderDoc (renderdoc.dll loaded), creating the file
// <game>\bin\rdcapture.trigger makes RenderDoc capture the next frame (in-application API, TriggerCapture).
// Captures go to <game>\bin\captures\s2dng_*.rdc. Does nothing without RenderDoc.
namespace RDTrigger {
    void onPresent();
}
