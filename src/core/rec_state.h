#pragma once
// The record side's cached state (run 18 item 4, P2: research\run18\p2.md; test only, off; nothing of optimumbox's).
// Every ctx setter writes its "last bound" field first and then the recorder's writer, so a stretch whose writers are
// skipped leaves the ctx claiming state the stream never got, and the next draws' packets are suppressed by those caches.
//
// [Stereo] RecorderReemit=1: after each of pass 2's pre-pass walks (FUN_140706050, pass +0x7f = 1, not cut; the
// DrawMasksFn stub's wrapper, draw_masks.asm) the ctx's state is written into the stream again (p2.md section 3): the
// CB tables invalidated (no stamp bump), the blend and rasterizer written, FUN_140ec7090 (targets, depth-stencil,
// viewport, scissor, program, IB, PSO, IA), ctx +0x124 = 1. Only between pass brackets, checked every time; refused
// (counted, by reason) otherwise. R0's probe: on correct state it must change nothing.
// [Stereo] PrepassRecordSkip=1 (needs RecorderReemit and PrepassWalkCut's gates): on the cut's frames pass 2's walk runs
// instead of being cut, with the recorder's 22 state and draw writers skipped (rec_skip.asm's thunks, patched into the
// recorder vtable once at start, gated on the render thread's recorder and thread), then the re-emit. R1's harness for
// P4: the stream it leaves must equal the cut's. A drift census (reads only) counts what the walk moved in the ctx.
// Off (both switches), nothing is installed, no vtable slot is touched, and no anchor is needed.
#include <cstdint>
#include <string>
#include <vector>

namespace rdrvr::rec_state {
void install(bool cut_gates);  // prepass::install, after PrepassWalkCut (cut_gates: its gates held at start)
bool skip_wanted();            // PrepassRecordSkip installed and on: a cut frame's walk is record-skipped, not cut
void arm(bool skip);           // the render thread, between the passes: the wrapper for pass 2 (skip: this frame's walk)
bool disarm();                 // the render thread, after pass 2 or a mono frame: false when a skip was armed and none ran
// [Stereo] PrepassCutVelocity's D3 (cut_velocity.cpp): the re-emit once, now, on the render thread at a cut walk's end
// (the walk not run, so no wrapper): the same preconditions and re-emit as after a walk, counted with them. False when
// RecorderReemit is not installed or the preconditions refused it (counted by reason). Independent of "prepass reemit on|off".
bool installed();
bool reemit_now();
// R0's variants (run 19 item 5, research\run18\p2.md section 10; "prepass reemit variant full|cbonly|late|nobind", full the
// default and run 18's): the re-emit after a recorded walk as it was (full), the CB invalidation alone (cbonly), full at the
// walk's end after the walk-end marker (late: walk_end below), or full without the 0x25 and 9 (nobind). Record-skipped
// walks and reemit_now always get full.
// walk_end: hk_letterbox at the walk's end (return 0x5cc334), pass 2, the walk not cut, after the walk-end marker: the late
// variant's re-emit (the same preconditions, refusals counted with the others); nothing unless a walk of this pass 2 left it.
void walk_end();
// (item 7's S2, research\run25\item7.md 1.4 and 5; [Stereo] Pass2FrontSkip, pass2_skip.cpp; test only) record-skipped
// stretches around single game calls, usable without PrepassRecordSkip's walk skip and without R0's re-emit after uncut walks:
//   stretch_install: bootstrap (pass2_skip's install, its gates held): RecorderReemit installed; the recorder vtable's 22 slots
//     patched once (if PrepassRecordSkip has not already); R0's re-emit then starts off unless PrepassRecordSkip is installed
//     ("prepass reemit on" turns it on). False: off (logged).
//   stretch_ready: installed and not stood down (the stretches' own sticky flag, not the walk skip's).
//   stretch_begin: the render thread, just before the call: check() (p2.md section 3's preconditions) passes, then the 22
//     writers are skipped for the render thread's recorder; false (counted by reason, nothing skipped): the call records as
//     the game's. Refused while another skip is on or a walk is open.
//   stretch_end: just after it: the skip off, then the full re-emit; its preconditions failing there (or another ctx):
//     after_skip's repair and the stretches stand down for the session; false then. skipped: the writer calls skipped.
//   stretch_kv / stretch_reset: the counters (st_*=), a test's reset (the stand-down kept).
bool stretch_install();
bool stretch_ready();
bool stretch_begin();
bool stretch_end(uint64_t* skipped);
std::string stretch_kv();
void stretch_reset();
std::string command(const std::vector<std::string>& t);  // "prepass reemit [on|off|skip on|skip off|reset|variant V]": the status
}  // namespace rdrvr::rec_state
