#pragma once
// [Stereo] PrepassInstPrevMirror (run 19 item 2, research\run19\instprev.md section 4; test only, off by default; needs
// [Stereo] PrepassViews=2 and the pre-pass walk's start hook, as PrepassWvpMirror). The 48 vertex shaders that read
// rage_instancemtxprev (the foliage pair VS 5d2d5cd6 among them) take each instance's previous world-view-projection from
// a per-draw constant buffer the game builds in rmcInstDrawModel (FUN_14087d950 / 0x87df60): n entries of 64 bytes, each
// FUN_140132640(sm_Current, out, W_i), W_i the same draw's rage_instancemtx entry. Their twins (the converter's
// `--instprev K`, twins.txt "instprev<K>") read eye 2's entry i at row 4 (K + i) + r. So at the pass bind of each instanced
// draw in pass 1's pre-pass walk (a hook on PassBind FUN_1401514d0's entry, before the original, for its two instanced
// callers only: return 0x14087de13 / 0x14087e3ed; DAT_142ac0c38 set: the draw's previous block published, not yet
// committed), the block's storage is swapped for a mod block of (K + n) x 64 bytes:
//   rows [0, n)      eye 1's entries (the game's, copied);
//   rows [K, K + n)  eye 2's: eye 1's entry first, then FUN_140132640(vp2, row, W_i) with vp2 the private eye-2 viewport
//                    after taa.cpp's eye-2 TAA update (+0x200 eye 2's previous VP, +0x371 set);
// and the block's declared size (its layout +0x14, per draw) set to (K + n) x 64, so the game's own upload at the draw's
// commit carries it. Refused, counted by reason and left as the game's: any check of the game's object failing, n over K
// or K + n over 1024 (64 KB), the walk's mirrored bytes over a cap (the upload ring has no capacity check).
// The per-draw gate (pso.cpp: an instprev twin is handed out only while the mirror runs): before each such bind an
// op-0x4a marker in the render thread's stream tells prepass's playback whether this draw's eye-2 rows were all written.
// Pass 1 draws the twin only after an "ok" marker (else single); pass 2 drops its own copy only when pass 1 mirrored the
// same draw (matched by its W block and n), else draws it. A walk that is not ready (no eye-2 TAA update this frame: open
// question 5; eye 2's rows not written) marks every draw single, unless "notaa on" lets the mirror run without the update.
//   observe  read only: n per instanced draw in pass 1's walk (and anywhere), nothing written (open question 1, K)
//   compare  with the cut off (pass 2 walks): pass 1's draws' eye-2 entries made the mirror's way are kept by W_i, and
//            pass 2's own entries for the same W_i compared with them, bit for bit (open question 3)
// [Stereo] PrepassInstPrevMirror=1 the mirror, 2 observe only, 3 observe and compare (read only); [Stereo]
// PrepassInstPrevK (default 512, 1..768) must be the K the twins were made with (pso.cpp holds other K's lines).
// "prepass instprev [on|off|observe|compare on|compare off|notaa on|notaa off|reset|kv]". On another RDR.exe build
// (relocated, or an anchor absent) it is off; the mod never stands down for it. Nothing allocated, locked or logged per draw.
#include <string>
#include <vector>

namespace rdrvr::inst_prev {
void install(bool prereq);                               // prepass::install (prereq: PrepassViews=2 and the walk's start hook)
bool installed();                                        // the hook in (the ini switch on at start)
void walk_open(int pass, bool rows_ready, bool cut);     // hk_prewalk (the render thread), pass 1 or 2 (cut: pass 1 the walk cut on, pass 2 this frame cut)
void walk_close();                                       // hk_letterbox at the walk's end
void note_eye2(const void* vp2, bool taa);               // prepass::set_eye2 (taa: from taa.cpp's eye-2 update, after TaaUpdate on vp2)
bool draw_ok();                                          // the playback thread: the last marker said "this draw mirrored"
// (GOAL-single-pass item 8e, test only) the playback thread, main_inst ([Stereo] MainFamily=region) right after it drew an instprev
// twin: the "ok" marker spent (draw_ok() false, draw_no_why() kNoMark), so a second draw before the next marker is refused
void consume_draw_ok();
bool frame_refused();                                    // the render thread, between the passes: an instanced draw of this frame's pass 1 not mirrored (no unforced cut)
std::string command(const std::vector<std::string>& t);  // t[0] "prepass", t[1] "instprev"
// ---- [Stereo] PrepassInstPrevMain (GOAL-single-pass item 8d, research\run25\item8d.md 2; test only, off by default; read with
// PrepassInstPrevMirror=1 at start): the mirror opened over the main walk's DVL call 1 as well (the treerock VS d420020b's near
// trees draw in bucket 1's post through the same two PassBind callers), for 8d's observe and 8f's hand-out. Its own state: its own
// counters (kv main_*), its own byte cap (1 MB a walk; the ring of blocks is shared), its own compare table, no pass-2 key (8d drops
// nothing), and never the walk cut's refusal (g_refused_frame): a main-walk refusal must not change this frame's cut. Its markers
// tell the playback why a draw is not mirrored; a "no" marker that cannot be appended breaks the main mirror for the session (every
// later main_live() false), as prevworld's gate does. "prepass instprev main on|off|compare on|off" (off at start).
//   main_open   the render thread, main_inst's DVL call 1 open (after prevworld's walk open): pass 1 opens the main walk (this
//               frame's eye-2 state, its live latch carried to the playback by the open's marker); pass 2 compares when on and pass
//               1 recorded this frame
//   main_close  e1's end (main_region's e1 marker, hk_ForestDraw) and again at DVL call 1's close (fallback: true)
//   main_live   the playback thread: the main walk's open marker said live (eye 2's viewport, its TAA update or notaa, the rows),
//               and the session's main mirror not broken; main_why: why not (0 live, 1 no eye-2 viewport, 2 no TAA update, 3 rows
//               not ready, 4 broken, 5 off or not installed)
//   draw_no_why the playback thread: why the last main marker said "not mirrored" (kNo* below; 0 when it said "mirrored")
//   main_other  PassBinds of the two instanced callers on another thread during a main walk (8d's frame rule: not judged)
enum MainNo : int { kNoNone = 0, kNoMark, kNoBad, kNoOverK, kNoCap, kNoFault, kNoAsEye1, kNoNocb, kNoCtx, kNoWhys };
void main_open(int pass);
void main_close(bool fallback);
bool main_live();
int main_why();
int draw_no_why();
uint64_t main_other();
std::string main_kv();  // the main walk's counters (main_*), for main_region's kv
// (GOAL-single-pass item 8e, research\run25\item8e.md 4.1; test only, off at start) the ip control: "prepass instprev main control
// nudge|off" (main_nudge_wanted: the command's switch). main_inst ([Stereo] MainFamily=region) applies it as its control kind 5:
// set_main_nudge(true) at pass 1's DVL call 1 open of a control frame (before main_open), set_main_nudge(false) at its close; pass 1's
// main_open latches it for the main walk, so each mirrored eye-2 row K + i gets +0.01 on every element after FUN_140132640 wrote it
// (in the mod's own ring block; the main compare records the value before the nudge); the latch cleared at main_close. False: the
// main walk not installed
bool main_nudge_wanted();
bool set_main_nudge(bool on);
}  // namespace rdrvr::inst_prev
