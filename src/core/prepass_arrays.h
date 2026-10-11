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
void before_draw(ID3D12GraphicsCommandList* cl);  // a draw in a region: the walk's end copied first
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
void views_size(bool grown);  // the test's: the two declared sizes grown or as the game's (from the next pass 1)
// A trace of the frames after "prepass on" (or "prepass trace"): every bind, clear and barrier of the six targets
// logged with the list's draws so far ([prepass] trace lines), in a region or not. Test only.
bool tracing();
void trace_start(int frames);
void trace_bind(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single,
                const D3D12_CPU_DESCRIPTOR_HANDLE* ds);
void trace_barrier(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_RESOURCE_BARRIER* b);
void trace_clear(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE h, bool depth);
}  // namespace rdrvr::prepass
