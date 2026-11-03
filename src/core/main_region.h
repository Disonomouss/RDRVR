#pragma once
// [Stereo] MainRegion (GOAL-single-pass item 8d, research\run25\item8d.md sections 1.3 and 3; test only, off by default, never in a
// shipped or user ini; needs MainInstanced=1, MainUnionCull=1 and InstancedShadersAuto=1): "prepass mr observe", the census of every
// draw of pass 1's e1 (DVL call 1's open to ForestDraw mode 0) by the verdict 8f's hand-out would give it, from pso's region table
// (pso.h region_verdict), the frame latch and the per-draw gates. Observe only: nothing draws a twin, no target is remapped, nothing
// is dropped, the walk cut's decision is not touched; the stream gains op-0x4a markers only (main_inst's region already lends SC2L and
// LB2 and swaps the main InstPrev blocks when those switches are on).
//   Render thread: an open marker after main_inst's (pass 1 and 2 of DVL call 1; it carries the frame's slot in an 8-entry ring of
//     the frame's gates, written before it: the cascades, eye 2's rows, the union cull recorded, p1vp, the control, the PrevWorld
//     gate, the converter's kinds, the fade branch; at pass 2 the walk cut, read at its open, so the frame's verdict is final only
//     there), the segment markers at fcensus's points (E0, P0, E1, P1: [Stereo] RouteFCensus=1 installed; the census and probe may be
//     off), e1's close marker from hk_ForestDraw (mode 0 inside DVL call 1; the main InstPrev mirror closed there too).
//   Playback thread, pass 1's e1: each draw classified, first match wins: empty (count or instances 0: exempt) | pso unknown (the
//     list's pipeline not this list's, or no slot) | bind (not the family's four targets: main_inst's check_bind, cached per bind) |
//     no twin (the slot's state: refused, held by kind, queued, ready, build failed, shape, no rule, a stream creation) | kind wvp |
//     gate e2img (the latch's reason, or not lent on this list) | gate prevworld (the frame, or the draw) | gate instprev (the main
//     mirror not live and why, or the draw not mirrored and why) | twin (plain, pw, e2i, pw+e2i, ip, ip+e2i). An ExecuteIndirect is a
//     single ("indirect"); predication is an attribute counted per verdict. Every non-empty draw of both passes' e1 is keyed (6e's
//     rule, main_instanced.h keys: its own tables, 8,192 slots each) and tagged with its segment (pre, b0 entities, b0 post, b1
//     entities, b1 post), and counted in a pair table (512 VS^PS slots: draws, segments, verdicts, misses, pass 1's repeated keys;
//     8a's expected kind). A draw whose vertex buffer slot 0 lies in the game's dynamic vertex ring (DAT_142b01150's 32 MB buffers,
//     FUN_14012e280's immediate vertices: their address and view size move between the passes) is keyed by its own vertices' bytes
//     (non-indexed, <= 4 KB, read under SEH from the ring's persistent mapping), else by the ring's tag, the stride and its span.
//   A frame is judged when pass 1 and pass 2 each opened and reached e1 on the region's thread in order, no key table was full, no
//     InstPrev bind or PrevWorld write came from another thread inside the walk, and no marker of the frame was lost; it is clean
//     when no frame gate failed and no draw was single. A ring of the last 64 frames that were not clean names each one.
//   Tier 2, the misses' cause (item8d.md section 3.4; "prepass mr misses why on|off", test only, off by default, armed with observe,
//     needs [Stereo] RouteFCensus=1's entity point): body.cpp's DrawVisEntity hook, through fcensus::entity's gates (DVL's entity
//     loop, return 0x6421ae, inside e1 of each pass), calls entity() for buckets 0-1: the render thread writes ring[seq] = {rec,
//     bucket, pass, frame, rec+0xac, the bounding sphere and its margins against the pass's planes} (8,192 entries, sequence-stamped,
//     a seqlock each; static) and appends an op-0x4a marker; the playback thread's cursor (each pass's start carried by its open
//     marker's ring slot) reads the entry at the marker, so each draw after it is that entity's until the next marker or a post's
//     segment marker. Each pass-2 miss of a judged frame is then classified: C1 culled in pass 1 (the entity drew no e1 draw in pass
//     1: split near / side / inside by the sphere against pass 1's planes, unsplit without one, absent without a pass-1 call), C2
//     the entity drew in both (lod: the miss's pipeline among its pass-1 draws; geom: not), C3 a b0 post / b1 post draw; the rest
//     (pre, no entity, a frame whose markers broke, past 64 a frame) are counted unnamed. "prepass mr why": the counts by cause, by
//     pair and kind, and the first 16 misses' details; off: one relaxed load in fcensus::entity (itself only while fcensus is on).
// "prepass mr off|observe|compare|reset|kv|dump|why|control none|nopw|misses why on|off" (control nopw, test only: every prevworld twin
// read as "gate prevworld", M2's control; compare, item 8e: observe plus main_inst's region listing, refused without [Stereo]
// MainFamily=region; its frame verdict pushed to main_inst::mr_verdict at the frame's end). Ini off: nothing installed; each hook site
// is one relaxed load.
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "core/d3d_hooks.h"

struct ID3D12GraphicsCommandList;

namespace rdrvr::main_region {
extern std::atomic<bool> g_armed;      // installed and "observe": the render thread records the markers
extern std::atomic<bool> g_observing;  // (the playback thread) an observed region pass is open: main_inst's draws come here
inline bool armed() { return g_armed.load(std::memory_order_relaxed); }
inline bool observing() { return g_observing.load(std::memory_order_relaxed); }
extern std::atomic<bool> g_why_armed;  // (tier 2) installed, observe and "misses why on": the render thread records the entity markers
inline bool why_on() { return g_why_armed.load(std::memory_order_relaxed); }
void install();  // bootstrap, after main_inst::install() and fcensus::install()
// the render thread: main_inst's DVL call 1 open (after its open marker was recorded; mi: what that open saw) and close
enum MiBits : uint32_t { kMiCascades = 1, kMiRows = 2, kMiUc = 4, kMiP1vp = 8, kMiCtl = 16 };
void dvl_open(int pass, uint32_t mi);
void dvl_close(int pass);
void e1_end(int pass);  // dual_pass hk_ForestDraw, mode 0 inside DVL call 1 (e1's end): the main InstPrev mirror closed, e1's marker
void segment(int seg);  // fcensus's points inside e1: 1 E0, 2 P0, 3 E1, 4 P1
// (tier 2, while why_on()) fcensus::entity, after its points: an entity-loop DrawVisEntity call of bucket 0 or 1 inside e1 (pass 1|2)
void entity(uintptr_t rec, uint32_t bucket, int pass);
// the playback thread (main_inst, while observing): the region's close, its draws (pso: the list's game pipeline, or null) and indirects
void pb_close(int pass);
// (GOAL-single-pass item 8e; test only) "prepass mr compare": observe's markers, classification and keys, plus main_inst's listing
// ([Stereo] MainFamily=region). draw's out (null in observe): in_e1 (the region pass's e1, its thread), empty (count or instances 0),
// ord (the pair's ordinal 0..63, given at the pair's first non-empty pass-1 e1 draw this session, kept across "mr reset"; -1 none: not
// given yet, past 63, or no pipeline known), flags (the slot's main_inst_flags), fmt (the slot's pso.h region::pack_formats word)
extern std::atomic<bool> g_comparing;
inline bool comparing() { return g_comparing.load(std::memory_order_relaxed); }
struct DrawInfo {
    bool in_e1 = false, empty = false;
    int ord = -1;
    uint32_t flags = 0, fmt = 0;
    bool miss = false;  // (item 8g, pass 2) this draw's key not found among pass 1's (counted out by number: a key drawn more is a miss)
};
void draw(ID3D12GraphicsCommandList* cl, const d3d::DrawArgs& args, const void* pso, DrawInfo* out = nullptr);
int ord_find(uint32_t vs32, uint32_t ps32);       // (any thread) the ordinal of the pair whose hashes' top 32 bits these are; -1 none
bool ord_pair(int ord, uint64_t* vs, uint64_t* ps);  // (any thread) the pair at an ordinal; false none yet
int ord_count();                                     // (any thread) the ordinals given so far (at most 64)
uint32_t ord_state(int ord);                         // (any thread) the slot state (pso.h region: state | kinds) last seen at its pass-1 draws
void indirect(ID3D12GraphicsCommandList* cl);
// (item 8g, test only; the playback thread, inside pass 2's e1) pass 1's e1 as 8d's verdict will judge it, as far as known now: judgeable,
// no failed gate but the control, no single, no draw without an ordinal, no key table full; why: the first reason (else null)
bool pass1_clean(const char** why);
std::string command(const std::vector<std::string>& t);  // t[0] "prepass", t[1] "mr"
}  // namespace rdrvr::main_region
