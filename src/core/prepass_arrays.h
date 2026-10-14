#pragma once
// [Stereo] PrepassArrays (single-pass stereo, research\sps\stageVI-prepass.md milestone 2; test only, off by default):
// each eye's depth pre-pass drawn into its slice of mod-owned 2-slice arrays instead of the game's six pre-pass targets
// (the Shadow Collector, its depth, the velocity, the grass and tree screen targets), then copied into the game's own
// before the game reads them. No instancing yet: the image must be the game's, bit for bit; the copies' cost measured.
//
// On the playback thread, in the game's list: the game's clear of the Shadow Collector Depth Buffer starts a region (the
// frame's first pre-pass slice 0, its second slice 1); in it the six targets' binds and clears go to the slice; the
// game's first barrier taking that depth out of DEPTH_WRITE ends it: first each touched target's slice is copied into
// the game's target, from the states the region saw (a bind or a clear: RENDER_TARGET or DEPTH_WRITE; a game
// barrier: its StateAfter). Any state not known, any mismatch or a region that does not end: logged, nothing copied
// from then on, and the arrays stood down.
#include <d3d12.h>
#include <string>

namespace rdrvr::prepass {
void init();
void install();              // after anchors::verify(): the walk's end hook (with [Stereo] PrepassArrays=1), then on
bool on();                   // the switch (a region may start)
bool active();               // a region is open (the binds, clears and barriers go through the module)
void set_on(bool want);      // "prepass on|off" (from the next region)
D3D12_CPU_DESCRIPTOR_HANDLE clear_dsv(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE h);
D3D12_CPU_DESCRIPTOR_HANDLE clear_rtv(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE h);
// The game's bind in a region: true with the substituted handles (out_rts[0..n), *out_ds when ds was given).
bool bind(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single,
          const D3D12_CPU_DESCRIPTOR_HANDLE* ds, D3D12_CPU_DESCRIPTOR_HANDLE* out_rts, D3D12_CPU_DESCRIPTOR_HANDLE* out_ds);
void barrier(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_RESOURCE_BARRIER* b);  // before the game's batch
// A draw in a region: the walk's end copied first. True: drop it ([Stereo] PrepassInstanced, pass 2: drawn in pass 1).
bool before_draw(ID3D12GraphicsCommandList* cl);
// [Stereo] PrepassInstanced (milestone 4a steps 2-4, 2026-10-14; test only, off; needs PrepassArrays=1, PrepassViews=2
// and the twins, pso.h): pass 1's region binds and clears the six as both slices, sets the view mask to both views and
// draws a pipeline's twin (pso::twin_of) while every bound target is one of the six; pass 2's region drops those draws
// (the same rule), skips the six's clears, and from it on to the next pass 1 no draw is predicated (the dropped draws'
// queries count nothing). The game's pipeline is put back at the region's end. "prepass inst on|off".
void note_pipeline(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* ps);  // the game's SetPipelineState in a region
void before_indirect(ID3D12GraphicsCommandList* cl);  // an ExecuteIndirect in a region: the game's pipeline, not a twin
bool take_skip_clear();  // after clear_dsv/clear_rtv: true = skip that clear (consumed)
bool predication_off();
void set_instanced(bool on);
// Every game bind while on and outside a region: the game binds the pre-pass's targets before it clears them, so the
// region's start binds them again with the slices.
void note_bind(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single,
               const D3D12_CPU_DESCRIPTOR_HANDLE* ds);
std::string status();
// [Stereo] PrepassViews (milestone 3, test only; stageVI-prepass.md): eye 2's per-view constants for pass 1.
// Step 1, observe only (nothing written to the game): camera_lever builds a private eye-2 viewport in pass 1 and hands
// its rows here; after each pass's walk the game's own rows are read from the constant buffers' storage, and pass 2's
// (the game's eye 2) are compared with pass 1's prediction ("prepass views" reports the differences).
bool views_wanted();
// the render thread, pass 1: vp2's +0xc0 (rotation-only VP) and +0x140 (view inverse); prev_vp: eye 2's gPrevViewProj
// (the TAA update's +0x4f0) or nullptr (no jitter)
void set_eye2(const void* vp2, const void* prev_vp);
std::string views_status();
void views_size(bool grown);
// [Stereo] PrepassUnionCull (2026-10-14, milestone 4's culling; test only, off): pass 1's pre-pass walk culls against
// both eyes (the shadow passes' union viewport's four side planes swapped into the current viewport from the walk's start
// to its end), so a pre-pass drawn once for both eyes misses nothing eye 2 sees. Extra draws are clipped on the GPU:
// eye 1's image must not change. "prepass cull on|off" (the session; the hook needs the ini switch at start).
// [Stereo] PrepassViews=2 and the mod's upright grass (dual_pass hk_ForestDraw, mode 1, pass 1): eye 2's gViewInverse
// rows 0-1 (b1 rows 25-26) levelled as eye 1's are for the grass draw (the heading's right and up; each eye's own w), and
// put back after it, so the instanced grass expands its quads the same way in both eyes (2026-10-14, milestone 4).
void eye2_level(const float* level8);
void eye2_unlevel();
bool union_cull_wanted();
void set_union_cull(bool on);
std::string cull_status();  // the test's: the two declared sizes grown or as the game's (from the next pass 1)
// A trace of the frames after "prepass on" (or "prepass trace"): every bind, clear and barrier of the six targets
// logged with the list's draws so far ([prepass] trace lines), in a region or not. Test only.
bool tracing();
void trace_start(int frames);
void trace_bind(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single,
                const D3D12_CPU_DESCRIPTOR_HANDLE* ds);
void trace_barrier(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_RESOURCE_BARRIER* b);
void trace_clear(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE h, bool depth);
}  // namespace rdrvr::prepass
