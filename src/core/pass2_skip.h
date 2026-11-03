#pragma once
// [Stereo] Pass2FrontSkip (GOAL-single-pass item 7, stage S2: research\run25\item7.md; test only, off by default; needs
// [Stereo] PrepassEye2Collector=1, PrepassEye2Decals=1, PrepassEye2Light=1, PrepassEye2LightDepth=1, RecorderReemit=1 and the
// walk cut's hooks (PrepassWalkCut=1, [Compat] DrawMaskInit=1), else it logs why and stays off). Route B, the record-level
// skip: in pass 2 of a frame whose walk was cut and whose pass 1 already made eye 2's front half (e2sc's decals and term, eye
// 2's footprints, e2lb's lights, D2L), pass 2's own four front-half calls still run on the CPU, each in a record-skipped
// stretch (rec_state's stretch API: the recorder's 22 state and draw writers skipped, then the re-emit), so every CPU side
// effect is the game's and only their GPU work is dropped:
//   D  DecalMgr vt+0x70 = 0x1405e0dc0 (DecalDraw), its call at 0x1405cc3dc (return 0x5cc3df): this module's detour, filtered
//      by the return address (e2sc's own eye-2 call in pass 1 passes through)
//   S  DirCollector 0x1406e58a0 (return 0x5cc552): eye2_collector's detour
//   F  Footprints 0x140193060 (return 0x5cc5e4): eye2_light's detour
//   L  LightFrontHalf 0x1406e0450 (return 0x5c3e4d): eye2_light's detour; after its stretch, P2IN's marker (eye2_light's op
//      queue, before the save marker): on the playback thread SC2L, D2L and LB2 copied into the game's collector, depth and
//      Light Buffer, so every reader after L sees what pass 2 would have made.
// The decision, latched at pass 2's first wrapped call (keyed by the walk cut's decision sequence) and re-checked at each call:
// on and not stood down, the stretches ready, this frame's walk cut, this frame's pass 1 ran eye 2's lights with D2L and eye
// 2's decals, no e2sc/e2lb/e2vel capture in flight, and the fade branch at 0x1405cc4a4 cannot draw (item7.md 7, design
// change 2: FadeValue 0, read from its globals, with the front object's pass +0x87 != 0; conservative: its other conditions
// are not read). A refused stretch_begin runs the call normally (counted); a latched pass 2 always gets its P2IN.
// "prepass p2skip on|off|kv|reset|control none|nocopy lb|sc|d|all|stale". Ini off: nothing installed, and each wrapped call
// is one relaxed load.
#include <atomic>
#include <string>
#include <vector>

namespace rdrvr::p2skip {
enum Call : int { kDecals, kDir, kFoot, kLight, kCalls };
// "control nocopy ...": the copies P2IN leaves out (eye2_light's p2in_step)
inline constexpr unsigned kNoCopyLb = 1, kNoCopySc = 2, kNoCopyD = 4;
extern std::atomic<bool> g_installed_flag;
inline bool installed() { return g_installed_flag.load(std::memory_order_relaxed); }
void install();  // bootstrap, after eye2_velocity::install()
// the render thread, around the game's call c (enter just before it, leave just after it): enter true = a stretch begun;
// leave ends it and, after kLight, appends P2IN's marker when this pass 2 was latched to skip. Nothing outside pass 2
bool enter(Call c);
void leave(Call c, bool began);
template <class F>
void around(Call c, F&& call) {
    if (!installed()) return call();
    const bool began = enter(c);
    call();
    leave(c, began);
}
// eye2_light's (L), pass 1, after eye 1's call: eye 2's lights ran this pass 1 (e2sc's term then ran, and eye 2's footprints
// where the game drew them), D2L made (d2l), the game drew footprints in this pass 1 (fp)
void pass1_lights(bool eye2_ran, bool d2l, bool fp);
// any thread (P2IN's playback side): pass 2's front half was skipped and eye 2 keeps pass 1's eye-1 images this frame:
// counted (exposed2), and the module stands down for the session
void exposed2(const char* why);
// ---- (GOAL-single-pass item 8c, research\run25\item8.md section 5; test only, simulator only) the record-skip probe of
// pass 2's e1 ("prepass p2skip probe off|r0|recskip|reset"): r0, R0 at e1's end (rec_state::reemit_now with nothing skipped,
// which must change nothing); recskip, route W's probe: pass 2's DVL call 1 recorded with the writers skipped from its open to
// ForestDraw mode 0 (e1's end), then the re-emit, so eye 2 has no e1 (knowingly wrong while on). The render thread, from
// dual_pass's DVL call 1 hook: probe_open before the call, probe_e1 at the call's first ForestDraw mode 0, probe_close after
// the call (a stretch still open is ended there, counted). Nothing outside pass 2.
extern std::atomic<int> g_probe_flag;  // 0 off, 1 r0, 2 recskip
inline bool probe_on() { return g_probe_flag.load(std::memory_order_relaxed) != 0; }
void probe_open();
void probe_e1();
void probe_close();
std::string command(const std::vector<std::string>& t);  // t[0] "prepass", t[1] "p2skip"
// (GOAL-single-pass item 8d's frame gate, main_region.cpp; test only; the render thread) the fade branch's rule as decide() reads it,
// without its counters: 0 it cannot draw, 1 it can, -1 not read (taken as "can"), -2 not installed (its globals never read)
int fade_branch();
}  // namespace rdrvr::p2skip
