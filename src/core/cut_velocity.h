#pragma once
// [Stereo] PrepassCutVelocity (run 19 item 2, research\run19\velocity.md section 4; test only, off by default; needs
// [Stereo] PrepassWalkCut's hooks). Under DLSS a cut frame's pass 2 writes the Velocity RT with the previous position
// pinned at the target's origin (v = 0.5 (X, Y) render pixels): the velocity pass FUN_140873920 (GenerateVelocityBuffer on
// rdr2_taa, PostFx +0x128) reads a degenerate NDCToPrevNDC on the GPU, though the CPU's is right. A hook on 0x873920
// (optional anchor VelocityPass; a .pdata function, called once a pass from SceneRender 0x1405c3e74, the render thread):
//   d1  reads only, every pass: PostFx +0x4b0 (NDCToPrevNDC, TaaUpdate's), the effect's own value of the variable
//       (handle PostFx +0x3c4), rdr2_taa's constant buffer (effect +0x38: +0x00 storage, +0x10 mapped size, +0x18 mapped
//       pointer, +0x24 upload stamp, +0x28 GPU address of its last upload), the variable's bytes in that storage, and the
//       recorder's stamp DAT_142893480 (optional anchor RecorderStamp): kept per class, pass 1 / pass 2 not cut / pass 2
//       cut, for "prepass cutvel". Also the depth texture variable (+0x3cc): before the call the effect's RT, its +0x2c
//       and PostFx +0xf8 (whether the pass sets it again), after it the effect's RT and the CB's bindless index and sampler;
//       and TexelSize (+0x3c8, set inside the call) as in the CB after it. And op-0x4a markers around the pass (no
//       payload: one callback per class and probe bit), for the playback thread: its draws, a predication set on the
//       list, a pre-pass region open. "prepass cutvel probe <prefix> [cut|nocut|pass1]" (one-shot; refused in mode 0): a
//       crop of the game's Velocity RT at the pass's own bind and at the next bind after it, and slice 1 of the mod's
//       velocity array at the first, written as <prefix>-pre/-post/-slice1.rdb (velocity.md 5.4).
//   d2  d1, then in a cut frame's pass 2 (techniques 2-5 only) before the original: the CB re-mapped exactly as
//       FUN_140ec8bd0(cb, -1, 0) does (+0x18 = storage, +0x10 = the layout's declared size, +0x24 = -1, byte +0x20 = 0; its
//       next commit FUN_140ecc150 uploads again), then the variable set again from PostFx +0x4b0 through EffectSetVar
//       FUN_140151020 (as taa.cpp's set_vec).
//   d3  d1, then at a cut walk's end (hk_letterbox, return 0x5cc334, pass 2 of a cut frame) rec_state's re-emit once
//       (needs [Stereo] RecorderReemit=1 at start; "prepass reemit off" keeps the walks' own re-emits off).
//   d4  d1, then at a cut walk's end (hk_letterbox, pass 2 of a cut frame) ViewportSetCurrent(ViewportCurrent, 1, 0, 0)
//       once, as 0x706050 calls it at entry and exit (round 3, velocity.md 7): FUN_140130f00 sets the scene viewport's
//       screen size +0x330/+0x334 to the bound target's and rebuilds its screen matrix, which SceneRender's gViewport
//       (rage_prev_world) for the colour walk is made from. d1 also reads that size against the target's at the
//       velocity pass, and the probe also takes the RT at the 2nd-4th binds that include it and at the post's first.
//   d5  (run 21 item 2, research\run21\d4.md) d4's size without its push and viewport packet, and after the collector:
//       at the cut walk's end FUN_140130f00 runs on a copy of ViewportCurrent (d8/d0 the locked collector's size, as d4
//       takes it); at the velocity pass's entry of that pass 2 the three ranges it writes (+0x280..+0x29f, +0x2b0..+0x2bf,
//       +0x330..+0x337) are put into ViewportCurrent, when it is the same viewport with the same +0x300..+0x313. Nothing
//       of it reaches the game before 0x5cbfd0 has returned.
//   d6  (run 21, the discriminator) FUN_140130f00(ViewportCurrent) alone at the cut walk's end: d4 without the push and
//       the packet. d5 and d6 call FUN_140130f00 by RVA (the analysed build, its entry bytes checked; else refused).
// [Stereo] PrepassCutVelocity=1..6 starts in d1..d6; "prepass cutvel on|off|d1|d2|d3|d4|d5|d6|reset". On another RDR.exe build
// (relocated, or VelocityPass absent) it is off; the mod never stands down for it. Nothing allocated, locked or logged
// per call (limited logs only).
#include <string>
#include <vector>

namespace rdrvr::cut_velocity {
void install(bool cut_hooked);  // prepass::install, after the walk cut (cut_hooked: its hooks installed)
void pass1_walk_end();          // (run 25 item 6g, test only) the render thread, at pass 1's walk end (hk_letterbox): "p1d4"
void cut_walk_end();            // the render thread, at a cut walk's end in pass 2 (hk_letterbox): D3
std::string command(const std::vector<std::string>& t);  // t[0] "prepass", t[1] "cutvel"
// (item 8f0, test only) the FSC sentinel probe's Depth Resolve slot: d3d's CopyResource hook calls it before the copy while
// fsc_listening() (one relaxed load otherwise)
bool fsc_listening();
void fsc_before_copy(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst);
// [Stereo] PrepassEye2Velocity (eye2_velocity.h, test only): the one detour on 0x873920 is shared. It is installed when
// PrepassCutVelocity > 0 or PrepassEye2Velocity = 1 (the latter alone: mode 0, the original and the callbacks only).
// set_e2v: callbacks before (after = false) and after (true) the game's call, around d1-d6 (the render thread; null: none).
// call_velocity: the original through the trampoline (eye 2's own call in pass 1: neither d1 nor the callbacks see it).
using VelHook_t = void (*)(void* postfx, bool after);
bool velocity_hooked();
void set_e2v(VelHook_t fn);
void call_velocity(void* postfx);
}  // namespace rdrvr::cut_velocity
