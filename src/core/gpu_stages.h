#pragma once
// [Stereo] PrepassGpuStages (run 19 item 3, research\run19\gpu.md; test only, off by default, never in a shipped or user
// ini): where the walk cut's extra GPU time goes. GPU timestamps (EndQuery into the mod's own query heap) recorded in the
// game's own direct lists at the stage boundaries the mod already detects:
//   - the passes: op-0x4a markers from dual_pass (pass 1 begins, its scene ends, pass 2 begins, pass 2 ends), each stamped
//     at the next game bind on the playback thread;
//   - each pass's pre-pass: the game's clear of the Shadow Collector Depth Buffer (seen by name, so also in plain two-pass)
//     to the walk's end (prepass_arrays' walk-end marker; stamped after the region's copies when a region ends there);
//   - the region's copies (end_region), split into pass 1's, pass 2's and the cut's handover;
//   - the six pre-pass targets' clears inside each pass's pre-pass (bracketed around the game's clear call).
// The frame's stamps are resolved on the present queue two frames after the frame closes (never waiting on the GPU) and
// averaged over a window: "prepass gpu [on|off|reset]". Off (the default: the switch not set), nothing is installed: the
// call sites check one relaxed flag and return.
#include <d3d12.h>

#include <atomic>
#include <string>
#include <vector>

namespace rdrvr::gpu_stages {

extern std::atomic<bool> g_on_flag;
inline bool on() { return g_on_flag.load(std::memory_order_relaxed); }

void install();  // after prepass::install(): with [Stereo] PrepassGpuStages=1, the bind tap and the frame-end listener

// the render thread (dual_pass): 1 pass 1 begins, 2 pass 1's scene ends (before eye 1's post), 3 pass 2 begins, 4 pass 2 ends
void mark(int point);
// the playback thread, prepass_arrays' walk-end marker played (before anything else it does)
void walk_end_played();
// hk_ClearDSV, before the pre-pass's substitution: a pass's first Shadow Collector Depth Buffer clear starts its pre-pass
void depth_clear_seen(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE game_view);
// around the game's clear call (game_view: the game's own handle, before the substitution): -1, or a token for clear_end
int clear_begin(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE game_view, bool depth);
void clear_end(ID3D12GraphicsCommandList* cl, int token);
// prepass_arrays' end_region: at its entry (slice, and whether it is the cut's handover) and after its last barrier
int copies_begin(ID3D12GraphicsCommandList* cl, int slice, bool handover);
void copies_end(ID3D12GraphicsCommandList* cl, int token);
void copies_none(ID3D12GraphicsCommandList* cl);  // end_region returned before any copy: only the owed walk end

// ---- (GOAL-single-pass item 8b, research\run25\item8.md section 3; test only, off until "prepass gpu walk seg|families")
// the walk census: segment e of each pass (DVL call 1's open to the game's CopyResource into "Depth Resolve") and e1 (its
// open to ForestDraw mode 0, the grass composite) on the GPU, and in "families" the segment split into runs of one VS/PS pair
// (a stamp before each draw whose pair differs from the last draw's), each run's time charged to its pair. Its own area of
// the query heap (kRuns a frame). "walk drop VS PS": pass 2's segment-e draws of that pair dropped (the ablation; eye 2
// knowingly wrong while set).
extern std::atomic<int> g_walk_flag;  // 0 off, 1 seg, 2 families
inline bool walk_on() { return g_walk_flag.load(std::memory_order_relaxed) != 0; }
uintptr_t walk_ret();  // DVL call 1's return address (MainDvlCall + 5), 0 when the anchor is not there
// the render thread (dual_pass): point 1 before DVL call 1, 2 before ForestDraw mode 0 inside it (an op-0x4a marker each,
// stamped before the next game draw on the playback thread)
void walk_mark(int pass, int point);
// the playback thread: before each game draw (true: drop it, the ablation), before each CopyResource (the segment's close)
bool walk_draw(ID3D12GraphicsCommandList* cl);
void walk_copy(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst);
// (GOAL-single-pass item 8e, research\run25\item8e.md 5: 8b's marginal stamps; test only, off until "prepass gpu walk marginal on",
// recorded only in "walk seg" frames, into the walk's own area) main_inst ([Stereo] MainFamily=region) on its dup frames, each listed
// pass-1 draw after the game's: point 0 before the twin, 1 between the twin and R0, 2 after R0 (ord: the pair's ordinal). Per pair the
// twin's and R0's GPU time (twin - R0: view 1's marginal); Σ over the frame of (after R0 - before twin) against pass 1's e1 stage, dup
// frames against the others (8b's interleaving caveat applies). Information, never a gate
extern std::atomic<bool> g_marg_flag;
inline bool marginal_on() { return g_marg_flag.load(std::memory_order_relaxed) && on(); }
void marginal(ID3D12GraphicsCommandList* cl, int point, int ord);
// ---- (item 8c, item8.md section 5; test only, off until "prepass gpu cpu on") the CPU census of DVL call 1 on the render
// thread: per pass, the call's time and its e1 part (its open to ForestDraw mode 0's entry), QueryPerformanceCounter only
extern std::atomic<bool> g_cpu_flag;
inline bool cpu_on() { return g_cpu_flag.load(std::memory_order_relaxed); }
void cpu_open(int pass);   // before DVL call 1
void cpu_e1(int pass);     // ForestDraw mode 0's entry inside it (the first)
void cpu_close(int pass);  // after it
// (item 8c) body.cpp's DrawVisEntity hook, around its whole body: inside e1 of a census'd call, the entity calls' time
// (route F's ceiling: what skipping DrawVisEntity could save at most)
bool cpu_in_e1();             // the render thread: inside e1 of a census'd DVL call 1
void cpu_dve(int64_t ticks);  // one entity call's QueryPerformanceCounter ticks

std::string command(const std::vector<std::string>& t);  // "prepass gpu [on|off|reset]", "prepass gpu walk [...]"

}  // namespace rdrvr::gpu_stages
