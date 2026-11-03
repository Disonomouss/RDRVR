#pragma once
// [Stereo] PrepassEye2Velocity (run 25, GOAL-single-pass item 4; research\run25\velocity2.md section 4; test only, off by
// default; needs [Stereo] PrepassEye2Light=1 installed and everything that needs: PrepassEye2Collector, PrepassArrays,
// PrepassInstanced, PrepassViews=2, PrepassWalkCut). Compare only: eye 2's base velocity made in pass 1 by the engine's own
// velocity pass FUN_140873920 into a mod-owned image V2, compared bit for bit with pass 2's own Velocity RT at its call.
// No game target's content is written; nothing reads V2.
//
// Render thread, pass 1, inside eye2_light's (L) (eye2_light::set_light_tail), right after eye 2's lights (the game's depth
// then holds eye 2's depth and light stencil marks; vp2 current), DLSS only (technique 5; the pass's own gate predicted open):
//   marker IN-V; eye 2's NDCToPrevNDC (taa::eye2_ndc, this frame's eye-2 update) into PostFx +0x4b0 and the rdr2_taa variable
//   (+0x3c4, EffectSetVar); PostFx +0xf8 = the main depth (SceneRender's own store, 0x1405c3e5b..6d); the velocity pass
//   through cut_velocity's trampoline (d1 and the callbacks do not see it); eye 1's NDCToPrevNDC back in +0x4b0 and the
//   variable (re-armed: BeginPass unmapped the CB, velocity2.md 1.6); marker OUT-V. eye2_light's snap_put then puts back
//   the render-state cache, the vertex-declaration binding and the scissor.
// Playback thread, at the next list call after each marker (prepass::cut_poll):
//   IN-V   V2 seeded from slice 1 of the pre-pass velocity array (the array RENDER_TARGET at rest: slice 1 to COPY_SOURCE and
//          back; V2 RENDER_TARGET at rest: COPY_DEST and back); a one-shot RTV substitution armed on this list: the first
//          bind after it, when it is the velocity pass's (one target, the game's Velocity RT, a depth bound, the view's format
//          V2's), binds V2's view instead (d3d::set_rtv_substitute2); any first bind disarms it. A capture: V2 (the prior)
//          and the game's depth, both planes from the walk cut's followed states, to readback.
//   OUT-V  disarmed; the draws on this thread since IN-V counted (d3d::thread_draws): substituted and drawn: ok; none and not
//          substituted: the pass's gate closed (counted, no harm); drawn and not substituted: eye 2's velocity went into the
//          game's Velocity RT (eye 1's DLSS input): exposed, the module stands down. A capture: V2 to readback.
// Render thread, the shared hook on 0x873920 (cut_velocity::set_e2v), a capture only:
//   pass 1, after eye 1's call: marker SE1 (eye 1's own output, at the next list call: the negative control)
//   pass 2, before the game's call: marker IN2V (at the velocity pass's own bind: the game's Velocity RT, pass 2's prior,
//          and the depth's planes to readback); after it: marker OUT2V (the next list call: the Velocity RT to readback)
// The compare ("prepass e2vel compare N"): N captured frames on a worker thread: the inputs (the prior, the depth's 24 bits,
// the stencil) and the CPU's (the NDCToPrevNDC bytes, PostFx +0xf8) equal, then eye 2's V2 against pass 2's Velocity RT as
// raw 32-bit texels (NaN and -0 count as bits). Coverage: the texels eye 2's call wrote (V2 after against its prior). The
// negative control: pass 2's against eye 1's own output, which must differ. The positive controls ("control nudge": eye 2's
// NDCToPrevNDC [3][0] + 1e-3; "control eye1": eye 1's matrix for eye 2's call) must make eye 2 differ.
// "prepass e2vel [on|off|compare N|control off|nudge|eye1|reset|kv]". Off at run time: no marker, no call. Ini off: nothing
// installed (the hook pointers stay null). Under FXAA the velocity pass does not run: no eye-2 call, counted (gate closed).
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace rdrvr::prepass {
extern std::atomic<bool> g_eye2_vel_pending_flag;  // (defined in eye2_velocity.cpp) the playback side owes a step
}

namespace rdrvr::eye2_velocity {
void install();                             // bootstrap, after eye2_light::install()
void poll(ID3D12GraphicsCommandList* cl);   // prepass::cut_poll, the list call's thread, when the flag is set
std::string command(const std::vector<std::string>& t);  // t[0] "prepass", t[1] "e2vel"
// [Stereo] Pass2FrontSkip (pass2_skip.cpp, item 7's S2; test only): a compare's frame in flight (pass 1 captured, pass 2 owed)
bool capturing();
// (GOAL-single-pass item 8f, test only: main_instanced's [Stereo] MainRegionE1Compare) V2 (at rest RENDER_TARGET; null when not made)
// on the thread its steps play on (tid), its generation (the makes so far); fresh: this pass 1's OUT-V drew it (set at OUT-V's ok,
// cleared at IN-V, a stand-down, a re-make and v2_spent), not stood down, no step queued; game: the Velocity RT it was made for.
// v2_spent: main_inst's pass-1 region close (a frame whose velocity pass did not run never finds the last frame's V2 fresh)
ID3D12Resource* v2_image(uint64_t* gen, unsigned long* tid, bool* fresh, ID3D12Resource** game = nullptr);
void v2_spent();
}  // namespace rdrvr::eye2_velocity
