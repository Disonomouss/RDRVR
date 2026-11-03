#pragma once
// [Stereo] PrepassPrevWorldMirror (run 19 item 6, research\run19\prevworld.md section 3; test only, off by default; needs
// [Stereo] PrepassViews=2). The observe-and-compare half of P3's first prerequisite: eye 2's gPrevWorldViewProjSingle.
// FUN_1401322b0(ViewportCurrent, W_prev), called before each entity draw, writes rage_prev_world rows 0-3 =
// (W_prev - (vp+0x70 & mask), 1 - cam.w&mask.w) x vp+0x200 through the CB's mapped pointer (gated on vp+0x371,
// DAT_142ac09f0 == 0 and the handle 0x2c371f4) [V]. A post-hook on it (one detour: cut_velocity's d1 sampling runs inside
// it, through set_d1, when both are on) in pass 1, on the scene viewport, after the game's write:
//   rows 5-8 (+0x50 of the CB, its declared size grown once 0x50 -> 0x90 by prepass's grow, the 0x100 CBV unchanged) =
//   FUN_140132640(vp2, tmp, W_prev), tmp prefilled with eye 1's rows; vp2 the private eye-2 viewport (after taa.cpp's
//   eye-2 update when TAA runs). 0x132640 does 0x1322b0's arithmetic in the same order [V], so it is the game's own value.
//   With vp2 missing this frame, rows 5-8 = eye 1's rows (counted "not ready").
// (run 26) Read only by the converter's prevworld twins ([Stereo] PrepassPrevWorldTwins=1, vi_auto's --prevworld), which
// pso.cpp holds unless rows_written below; (item 6c) main_twin_of hands them out only under its frame latch and the per-draw
// gate below (no listed family has one yet: nothing draws them).
//   compare  pass 1's eye-2 rows kept by W_prev's 12 read floats (a static table, a generation per frame); pass 2's own
//            rows 0-3 after each of its writes looked up: matched, bit-exact, unmatched, worst. And once a frame vp2's
//            +0x70..+0x7f and +0x200..+0x23f against pass 2's scene viewport at its first write. Works with the cut on
//            (pass 2's main walk still writes; only its pre-pass calls are gone).
// [Stereo] PrepassPrevWorldMirror=1 the mirror, 2 the mirror and the compare, 3 observe only (the calls counted, nothing
// written, no grow). "prepass prevworld [on|off|observe|compare [on|off]|reset|kv|gate|gate skip N]" (item 6c: gate, the
// per-draw gate's kv; gate skip N, its negative control: every Nth mirrored write gets eye 1's rows, 0 off, the default).
// On another RDR.exe build (relocated, not the analysed one, an anchor absent, 0x1322b0's entry bytes not as analysed) it is off; the mod never stands down for
// it. Nothing allocated, locked or logged per call.
#include <cstdint>
#include <string>
#include <vector>

namespace rdrvr::prevworld {
using Original_t = void (*)(void* vp, const float* prev_world);
// cut_velocity's d1 sampling, run in place of the original (it calls `call` once, where it would call the original)
using D1_t = void (*)(void* vp, const float* prev_world, Original_t call);

void install(bool prereq);   // prepass::install, before cut_velocity::install (prereq: PrepassViews=2)
uintptr_t storage();
// (run 25 item 6g, the p1vp review) the storage only while the CB is still the grown one (else 0), and the CB's upload re-armed
// (+0x24 = -1: the next draw's commit uploads it again), as the held writes do; the render thread
uintptr_t storage_live();
bool set_nudge_ndc(float d);  // (item 8e's region pw control: clip.x + d * clip.w, a shift of d in NDC at every distance; 0 off)
bool set_nudge(float d);     // (the family compare's pw-nudge control: "prepass mi control pw"; 0 off) true: the mirror is live
bool rearm();          // (run 25 item 6g, a diagnostic) rage_prev_world's CPU storage once grown (the render thread; 0 before)
bool installed();            // the detour on 0x1322b0 is this module's (cut_velocity then registers its d1 through set_d1)
void set_d1(D1_t fn);        // cut_velocity::install (nullptr: none)
void note_eye2(const void* vp2, bool taa);  // prepass::set_eye2 (the render thread, pass 1's scene wait): the grow, vp2's frame
std::string command(const std::vector<std::string>& t);  // t[0] "prepass", t[1] "prevworld"
// (run 26, GOAL-single-pass item 6a) pso.cpp's hold of the converter's prevworld twins (they read rows 5-8). Both are false
// unless the mirror is installed, writing (=1 or 2, not observing or "off") and grown. frame: state::presents during the frame.
// rows_written: pass 1 of `frame` wrote eye 2's own rows (vp2's) at least once, and none of its game writes of rows 0-3 fell
//   back (rows 5-8 left at eye 1's rows, or not written: no vp2, vp2's +0x371 clear, not grown, a fault, the game writing on
//   another viewport, a CB not confirmed), and no game write of rows 0-3 outside pass 1/2 happened in `frame`. Read at the
//   frame's end (pso's safe point, latched there for both passes of the next frame).
bool rows_written(uint64_t frame);

// (GOAL-single-pass item 6c, test only: armed by pso with "prepass mi allowtwins on"; off, nothing below runs) The per-draw gate
// of the prevworld twins, replacing the frame-level hold above for main_twin_of (6a: the game writes rows 0-3 outside the passes
// and on other viewports, so no frame is ever "written"). The CB the twin's draw reads is rage_prev_world (one object, g_cb);
// 0x1322b0 writes its rows 0-3 through the mapped storage on the render thread, and the entity's draws that follow commit it
// (their upload taken at record time) until the next write. So the unit is the write: a generation per CB object, wgen = the
// game writes of rows 0-3 seen (any thread, any pass; a write whose CB is not the grown one counts too), mgen = the wgen of the
// last write the mirror completed with eye 2's own rows (pass 1, the scene viewport, vp2 ready, grown, written). "Last write
// mirrored" is mgen == wgen. The playback thread draws long after the render thread recorded, so the render thread carries the
// verdict into the stream: inside pass 1's main walk (walk_open .. walk_close, main_inst's DVL call 1 markers) an op-0x4a marker
// (mirrored / not) after each write whose verdict differs from the last one sent, and one at the walk's open (after main_inst's
// open marker, whose playback resets the verdict to "not": pb_walk_reset). draw_mirrored(): the verdict the playback thread last
// played, so it holds for the draw being played. A "not" marker that cannot be appended breaks the gate for the session (every
// prevworld twin refused); a "mirrored" one is retried at the next write. A write on another thread inside the walk is counted
// (foreign_walk): its draws up to the render thread's next write cannot be marked, so a compare must exclude such a frame.
void set_gate(bool on);         // pso (the command thread)
bool gate_live();               // armed, not broken, the mirror installed, writing and grown
void walk_open();               // the render thread, pass 1's DVL call 1 open (after main_inst's open marker)
void walk_close();              // the render thread, its close
void pb_walk_reset();           // the playback thread, pass 1's main region open: the verdict "not" until a marker plays
bool draw_mirrored();           // the playback thread: gate_live() and the last played verdict "mirrored" (this thread's)
std::string gate_kv();          // the gate's counters
uint64_t foreign_walks();       // (item 8d's frame rule) the writes on another thread inside the walk so far (gate_foreign_walk)
}  // namespace rdrvr::prevworld
