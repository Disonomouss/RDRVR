#pragma once
// [Stereo] PrepassEye2Collector (run 20 item 5, research\run20\p3-textures.md section 5; test only, off by default; needs
// PrepassArrays=1, PrepassInstanced=1, PrepassViews=2 and PrepassWalkCut=1, whose follow gives the game targets' states
// after pass 1's region). Observe only: P3's first step, eye 2's final directional Shadow Collector term made in pass 1 by
// the engine's own FUN_1406e58a0 on the private eye-2 viewport, kept in a mod-owned 1-slice image (SC2) and compared bit
// for bit with pass 2's own collector at the same point. No shader, descriptor or CB change; nothing reads SC2 yet.
//
// A hook on FUN_1406e58a0 (optional anchors DirCollector, DirCollectorCall: 0x5cbfd0's call at 0x1405cc54d, return
// 0x5cc552; the other caller 0x5cbad0 is passed through), the render thread:
//   pass 1  op-0x4a marker IN1; ViewportSetCurrent(vp2, 1, 0, 0) (normalised to the locked collector's size, as pass 2's
//           walk or d4 leaves eye 2's viewport); o_6e58a0(lighting, vp2, the call's own r8, r9b, and stack arguments);
//           ViewportSetCurrent(the saved current, 1, 0, 0); DAT_142ad1cdc (the stencil reference the call zeroes) put back;
//           marker OUT1; then the game's own call for eye 1.
//   pass 2  (a capture only) marker IN2, the game's call, marker OUT2.
// The playback thread, at the next list call after each marker (prepass::cut_poll, the flag prepass::g_eye2_pending_flag):
//   IN1   the game's collector and depth saved (SCsave, Dsave), then the arrays' slice 1 (collector; depth, both planes)
//         copied into them: eye 2's inputs. A capture also copies slice 1's collector and depth plane 0 to readback.
//   OUT1  the game's collector copied into SC2 (a capture: to readback too), then SCsave and Dsave copied back: eye 1's
//         targets as they were.
//   IN2   (a capture) the game's collector and depth plane 0 to readback: pass 2's inputs.
//   OUT2  (a capture) the game's collector to readback: pass 2's own result.
// Every barrier is from the states prepass_arrays' walk-cut follow saw, and back. The render thread runs eye 2's call only
// when IN1 is expected to succeed: the follow armed, pass 1's walk instanced and ready this frame, SC2 and the saves made
// (a "prep" step makes them first). IN1 or OUT1 refused after the call was recorded (a state not known, a region open, a
// predicated list, a dropped step) leaves eye 1 exposed: counted, and the module stands down for the session. The
// render-state cache (DAT_142ac0b60, ids 0..0x2b) is saved before eye 2's call and each entry that differs after it is
// set back through RenderStateSet. The footprints (0x193060, after 0x6e58a0) are outside both compare points.
// The compare ("prepass e2sc compare N"): N captured frames, each read back two frame ends after its last copy and
// compared on a worker thread: pass 2's inputs against pass 1's (collector, depth's 24 bits), and pass 1's eye-2 result
// against pass 2's; the outputs are bit-exact when the inputs are.
// "prepass e2sc [on|off|compare N|reset|kv]". Off at run time: no marker, no copy, no call (the hook passes through).
// With the ini switch off nothing is installed. On another RDR.exe build (relocated, or an anchor absent) it is off.
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct ID3D12GraphicsCommandList;

namespace rdrvr::prepass {
extern std::atomic<bool> g_eye2_pending_flag;  // (defined in eye2_collector.cpp) the playback side owes a step
}

namespace rdrvr::eye2_collector {
void install();                             // bootstrap, after prepass::install()
void poll(ID3D12GraphicsCommandList* cl);   // prepass::cut_poll, the list call's thread, when the flag is set
std::string command(const std::vector<std::string>& t);  // t[0] "prepass", t[1] "e2sc"
// [Stereo] PrepassEye2Light (eye2_light.cpp, test only): runs() the eye-2 directional calls made so far (monotonic: "e2sc
// reset" leaves it; the render thread's count, pass 1); usable() installed, on and not stood down
uint64_t runs();
bool usable();
// [Stereo] Pass2FrontSkip (pass2_skip.cpp, item 7's S2; test only; the render thread): decals_installed() [Stereo]
// PrepassEye2Decals installed; last_run_decals() the last pass-1 eye-2 run drew eye 2's decals (DecalMgr vt+0x70 called, not
// skipped for the manager, the ring or the TLS value, no fault); capturing() a compare's frame in flight (its pass 1 captured,
// its pass 2 owed: IN2/OUT2 read pass 2's own collector, which then must be drawn)
bool decals_installed();
bool last_run_decals();
bool capturing();
}  // namespace rdrvr::eye2_collector

// [Stereo] PrepassEye2Images (eye2_images.cpp, test only): SC2 and its generation, read on the playback thread
struct ID3D12Resource;
namespace rdrvr::eye2_collector {
ID3D12Resource* sc2_image(uint64_t* gen, unsigned long* tid);  // tid: the thread its steps play on (ensure's)
}  // namespace rdrvr::eye2_collector
