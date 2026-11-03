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
#include <atomic>
#include <cstdint>
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
// count / instances: the draw's vertex or index count per instance and its instance count ([Stereo] PrepassCutEmptyDraws:
// a draw with either 0 makes nothing in either eye, so it is neither twinned nor single; research/run20/nocb.md)
bool before_draw(ID3D12GraphicsCommandList* cl, unsigned count = 1, unsigned instances = 1);
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
// (run 21 item 4, research\run19\gpu.md section 7; test only, off by default) end_region's GPU reductions, each armed by its
// own ini switch and toggled for the session: [Stereo] PrepassSliceBarriers ("prepass slicebarriers on|off": the arrays'
// copy transitions name only the copied slice's subresources) and [Stereo] PrepassSkipVelocityCopy ("prepass skipvel
// on|off": the Velocity RT's slice copies skipped while nothing reads it: AA Off or FXAA, motion blur off, ForceAntiAliasing
// set). Each returns reductions_status(), or an ERROR line when its switch was not set at start.
std::string set_slice_barriers(bool on);
std::string set_skip_velocity(bool on);
std::string reductions_status();
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
// [Stereo] PrepassPrevWorldMirror (prevworld_mirror.cpp): PrepassViews' grow (as rage_matrices' and rage_prev_proj's) on
// another shader global's CB object: its storage swapped once for a zeroed block of `alloc` bytes from the game's malloc
// (the old contents copied, the old block never freed), its declared size (layout +0x14) `declared` -> `grown`; refused
// unless the size is `declared` and both round up to the same 256. The render thread. cb_still_grown: storage and size
// still the grow's (the game did not make the CB again).
bool grow_cb(uintptr_t cb, uint32_t declared, uint32_t grown, uint32_t alloc, uintptr_t* block);
bool cb_still_grown(uintptr_t cb, uintptr_t block, uint32_t grown);
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
// [Stereo] PrepassWalkCut (run 13, 2026-10-15; test only, off; research\sps\fork-port-plan.md P1, after optimumbox's
// ViSkipWalk): pass 2's pre-pass walk (FUN_140706050, pass+0x7f = 1) cut at entry after an instanced pass 1; eye 2's
// pre-pass images handed over from pass 1's slice 1 at pass 2's walk end. Off, none of these does anything.
bool walk_cut_decide();  // the render thread, between the passes: true = pass 2 cut (its gust kept: dual_pass)
void walk_cut_end();     // the render thread, after pass 2 (and a mono frame)
ID3D12Resource* velocity_array();  // the Velocity RT's 2-slice array (nullptr before its first region; at rest RENDER_TARGET)
// [Stereo] PrepassEye2Collector (eye2_collector.cpp, test only): kind 0 the depth, 1 the collector (the pre-pass kinds).
// kind_game: the game's target the kind stands in for; kind_array: its 2-slice array; kind_rest_state: the array's state at
// rest (write_state: DEPTH_WRITE or RENDER_TARGET); kind_planes: 2 for the depth. kind_followed: the game target's state per
// plane as the walk cut's follow saw it, on the follow's own thread only, while no region is open and pass 1's instanced
// region touched the kind this frame; false otherwise. pass1_walk_ready: the render thread, in pass 1 after its walk's
// start: that walk was ready for instancing and its walk-start marker recorded (g_cut_ready, reset between the passes).
// instanced_on: [Stereo] PrepassInstanced's run-time switch ("prepass inst on|off", g_inst_on).
ID3D12Resource* kind_game(int kind);
ID3D12Resource* kind_array(int kind);
D3D12_RESOURCE_STATES kind_rest_state(int kind);
unsigned kind_planes(int kind);
bool kind_followed(int kind, unsigned plane, D3D12_RESOURCE_STATES* st);
// (r117 DG4, the e1f path after the game's pause: GBV's 1358, the depth left in DEPTH_WRITE where the game had it read-only) a list's state
// of a subresource is known at a point only from a transition recorded earlier in that same list (another list, or another thread's, runs
// before or after it as a whole and is not followed). kind_followed_fresh: kind_followed, and the plane's state last set on cl (a
// followed barrier on cl, or pass 1's region end on cl), cl not reset or closed since; follow_list_ends: a list's Reset or Close (its
// marks dropped). follow_stale_reads: kind_followed answers given inside a list call's poll (the older users: eye2_collector,
// eye2_light, eye2_velocity) whose plane was last set on another list (counted only, their answers unchanged)
bool kind_followed_fresh(int kind, unsigned plane, ID3D12GraphicsCommandList* cl, D3D12_RESOURCE_STATES* st);
void follow_list_ends(ID3D12GraphicsCommandList* cl);
const void* follow_setter(int kind, unsigned plane);  // (the diagnostic: the list that last set the plane's followed state)
uint64_t follow_stale_reads();
// (the review of d6ac220f) the older users' refusals of a plane last set on another list, per module (0 e2sc, 1 e2lb, 2 e2vel)
enum FollowUser : int { kFollowE2sc, kFollowE2lb, kFollowE2vel, kFollowUsers };
void follow_stale_note(int user);
uint64_t follow_stale(int user);
bool pass1_walk_ready();
bool instanced_on();
bool walk_was_cut();     // the render thread, in pass 2: this frame's pre-pass walk was cut by the stub (decided and done)
// (item 7's S2, research\run25\item7.md 3; [Stereo] Pass2FrontSkip, pass2_skip.cpp; test only; the render thread) the last
// walk-cut decision (walk_cut_decide, between the passes): true when it cut pass 2's walk (the stub's cut or PrepassRecordSkip's
// record skip; g_cut_decided_frame set), so slice 1 is complete. seq: the decisions made so far (one per double frame's
// between_passes): in pass 2 it names this pass 2's decision, in pass 1 the last frame's (pass 2 then sees it + 1)
bool walk_cut_decided(uint64_t* seq);
// an op-0x4a callback at this point of the render thread's stream (it runs on the playback thread there); false: the
// recorder not found ([Stereo] PrepassInstPrevMirror's per-draw markers)
bool stream_marker(void (*fn)(void*));
// the calling thread's recorder context (TLS slot 0 +0x28, as PassBind reads it) is the one stream_marker records into
// (*(dev +0x58)); stream_marker checks it itself (false: no marker)
bool thread_stream_ok();
// (run 29 item 2, [Holsters] InteractionHighlights=parts) stream_ready: thread_stream_ok and the recorder found (its vtable
// checked), so a marker appended now lands: the part glow checks it before its first marker, so its two markers and the
// draw between them are recorded together or not at all. stream_marker_data: stream_marker with a payload of at most
// kMarkerPayloadMax bytes, copied into the stream by the game's append (as dlss.cpp's) and handed to fn at playback
constexpr uint32_t kMarkerPayloadMax = 512;  // (the DLSS constants' payload, 464 bytes, is recorded so every frame)
bool stream_ready();
bool stream_marker_data(void (*fn)(void*), const void* data, uint32_t size);
uintptr_t stream_recorder();  // the recorder those markers go to (0: none; the part glow's extra draw is recorded only into it)
extern std::atomic<bool> g_cut_armed_flag;
inline bool cut_tracking() { return g_cut_armed_flag.load(std::memory_order_relaxed); }  // followed (also outside a region)
void cut_track(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_RESOURCE_BARRIER* b);  // outside a region
extern std::atomic<bool> g_cut_pending_flag;
extern std::atomic<bool> g_eye2_pending_flag;  // [Stereo] PrepassEye2Collector's steps owed (eye2_collector.cpp defines it; test only)
extern std::atomic<bool> g_eye2_light_pending_flag;  // [Stereo] PrepassEye2Light's steps owed (eye2_light.cpp defines it; test only)
extern std::atomic<bool> g_main_pending_flag;  // [Stereo] MainInstanced's readback owed (main_instanced.cpp defines it; test only)
extern std::atomic<bool> g_eye2_vel_pending_flag;  // [Stereo] PrepassEye2Velocity's steps owed (eye2_velocity.cpp defines it; test only)
inline bool cut_pending() {  // (a list call's first check)
    return g_cut_pending_flag.load(std::memory_order_relaxed) || g_eye2_pending_flag.load(std::memory_order_relaxed) ||
           g_eye2_light_pending_flag.load(std::memory_order_relaxed) || g_main_pending_flag.load(std::memory_order_relaxed) ||
           g_eye2_vel_pending_flag.load(std::memory_order_relaxed);
}
// ([Stereo] MainInstanced, main_instanced.h; test only; the render thread) eye2_rows_ready: eye 2's rows were written this
// frame into the grown buffers and are uploaded at the grown sizes (the twins' rows). eye2_rows_control, the compare's
// positive control, in pass 1 between DVL call 1's open and close: mode 1 nudges eye 2's gWorldView (b1 row 20, the
// translation row: x and y + 1e-3), mode 2 writes eye 1's gWorldView and gViewInverse rows (b1 4-7, 12-15) over eye 2's
// (17-20, 25-28); mode 0 puts back what mode 1 or 2 saved. True when written (or put back). The rows are uploaded with
// the next commits of b1 (marked dirty), so only draws recorded in between read them.
bool eye2_rows_ready();
bool eye2_rows_control(int mode);
// ([Stereo] MainInstanced's rows diagnostic, run 25; test only; the render thread, read only) the rows as the CPU storage
// holds them now (each CB is uploaded from it at its draws' commits): pass 1 eye 2's (rage_prev_proj rows 4-7, b1 rows
// 25-28 gViewInverse and 17-20 gWorldView: what the twin reads), pass 2 the game's (rows 0-3, 12-15, 4-7). *src: how this
// frame's write_eye2 made eye 2's gPrevViewProj: 1 the TAA update's (DLSS, TAA), 2 rows 0-3 copied at the write (no jitter:
// FXAA), 0 not written this frame. eye2_pp_refresh ("prepass mi ppfix", on by default with MainInstanced): eye 2's rows 4-7 copied
// again from rows 0-3 at pass 1's DVL call 1 (only twins read rows 4-7; the pre-pass's are drawn by then).
bool main_rows(int pass, float* pp, float* vi, float* wv, int* src);
int eye2_pp_refresh();  // (-1 failed, 0 rows 4-7 already equal to rows 0-3, 1 changed)
// [Stereo] PrepassPrevProjRefresh (run 25 item 4, research\run25\velocity2.md 3.2; test only, off by default; needs PrepassViews=2
// and the walk's start hook): eye2_pp_refresh() in hk_prewalk in pass 1, after SceneRender's gPrevViewProj push at 0x1405c3d8c,
// on the frames whose eye-2 gPrevViewProj was a copy of rows 0-3 (no TAA update: FXAA); the pre-pass twins then read this
// frame's rows. Under DLSS or TAA (the update's rows) it does nothing (counted). "prepass pprefresh [on|off|reset]"
std::string pp_refresh_command(const std::string& w);
void cut_poll(ID3D12GraphicsCommandList* cl);  // the handover, on the walk-end marker's thread
void set_walk_cut(const std::string& how);      // "prepass cut on|off|force" (force: the hold ignored)
void set_cut_hold_started(bool on);             // "prepass cuthold started|all": [Stereo] PrepassCutHoldStarted at run time (test only)
// "prepass cut holdpending on|off": [Stereo] PrepassCutHoldPending at run time (test only; research\run25\item8d.md 11): a single
// whose twin is pending holds the walk cut only until the twin's install + 8 presents (a twin the pre-pass is not given then, or a
// pipeline single for a reason no twin cures: the 600 hold). False: not armed (the ini switch at start)
bool set_cut_hold_pending(bool on);
// the ini switches the hold arms by (PrepassCutHoldPending, PrepassWalkCut, PrepassInstanced, PrepassArrays, PrepassViews=2,
// [Compat] DrawMaskInit): config only, any time; pso writes its region table for the hold only with them (at the first device's
// hooks, before install below: the walk's hooks are known only there)
bool cut_hold_pending_wanted();
void set_union_cull(bool on);
std::string cull_status();  // the test's: the two declared sizes grown or as the game's (from the next pass 1)
// [Stereo] PrepassWvpMirror (run 17, research\run17\sps.md (A) option 1; test only, off; needs PrepassViews=2 and the
// walk's start hook): eye 2's gWorldViewProj (b1 rows 21-24, read by VS_TransformUnlitDepthOnly's twin) written at each
// PushGlobals of the scene viewport in pass 1's pre-pass walk = (the pushed World x eye 2's View) x eye 2's Proj, through
// camera_lever's PushGlobals hook; twins.txt's "wvp" pairs drawn only while on (pso::set_wvp_live). With the cut off, pass
// 2's own pushes are matched to pass 1's mirror by World ("prepass wvp": the counts and the worst difference).
void set_wvp(bool on);  // "prepass wvp on|off" (the session; needs the ini switch at start)
std::string singles_status();  // "prepass singles" (run 18, read only): pass 1's single draws by game pipeline (VS/PS hashes) and why
void singles_reset();          // "prepass singles reset": the counts from now
std::string wvp_status();  // "prepass wvp": the mirror's counts and the compare's (matched, bit-exact, worst difference)
// A trace of the frames after "prepass on" (or "prepass trace"): every bind, clear and barrier of the six targets
// logged with the list's draws so far ([prepass] trace lines), in a region or not. Test only.
bool tracing();
void trace_start(int frames);
void trace_bind(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single,
                const D3D12_CPU_DESCRIPTOR_HANDLE* ds);
void trace_barrier(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_RESOURCE_BARRIER* b);
void trace_clear(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE h, bool depth);
}  // namespace rdrvr::prepass
