#pragma once
// DLSS per eye (research\run6\dlss.md plan A, H2-H5; [Render] DlssPerEye, on). With the game's DLSS on in stereo
// (ForceAntiAliasing=3, DlssFirstEye) both eyes share its one DLSS viewport (id 0), so each eye's evaluate reprojects
// the other eye's last output (probe P1: the second eye's flicker +21%). This gives the first eye's post run a viewport
// of its own (id 1, a second NGX instance with its own history):
//  - H2: the deferred append (FUN_140ecc9b0) of the first-eye run's two DLSS callbacks is redirected: the constants'
//    (0x140fced20) to the mod's, with that run's sl::Constants and frame token copied into the callback's payload;
//    the evaluate's (0x140fced10) to the mod's, which runs the game's own evaluate with viewport 1 swapped in;
//  - H3: slSetTag and slEvaluateFeature (their import slots) take viewport 1 while the mod's evaluate runs (playback);
//  - H4: slDLSSSetOptions (the game's cached pointer) also sends the options to viewport 1;
//  - H5: slFreeResources frees viewport 1 before the game's viewport.
// The slots are swapped lazily on the render thread, once each points into sl.interposer.dll (the game's packed
// imports are resolved at run time). Kill switch: the first failed mod call turns it off (back to the shared viewport).

#include <cstddef>
#include <string>

namespace rdrvr::dlss {

bool install();  // startup: the deferred append's hook (only with [Render] DlssPerEye=1)
bool on();       // per-eye engaged (configured, ready, not killed)
std::string command(const std::string& line);  // "dlss": the state and counters; "dlss off": the kill switch by hand
// The jitter (render pixels) of the latest DLSS constants the game's own call set (viewport 0: the second eye's run of
// a double frame, or mono), read on the playback thread; false before any.
bool last_jitter(float out[2]);
void set_mv_scale(float k);  // test aid ("dlss mvscale <k>"): both eyes' mvecScale times k (1 = as the game sets it)

}  // namespace rdrvr::dlss
