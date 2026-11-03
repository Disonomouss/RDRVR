#pragma once
// D3D12/DXGI hook chain, installed at startup before the game creates its device (DESIGN §3.3 H8; GOAL rule:
// D3D12 detours at startup or not at all):
//   d3d12!D3D12CreateDevice  -> (arm DRED) -> ID3D12Device::CreateCommandQueue / CreateCommittedResource /
//                               CreatePlacedResource / CreateRenderTargetView / CreateDepthStencilView,
//                               ID3D12Object::SetName (resources), ID3D12GraphicsCommandList methods (frame graph)
//   CreateCommandQueue       -> ID3D12CommandQueue::ExecuteCommandLists
//   dxgi!CreateDXGIFactory2  -> IDXGIFactory2::CreateSwapChainForHwnd / CreateSwapChain
//   CreateSwapChainForHwnd   -> IDXGISwapChain::Present / Present1 / ResizeBuffers
// Each vtable method is hooked once, at its implementation, so every object of that class is covered.

#include <cstddef>
#include <cstdint>
#include <string>

struct ID3D12CommandQueue;
struct IDXGISwapChain;
struct ID3D12CommandList;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct D3D12_CPU_DESCRIPTOR_HANDLE;
struct D3D12_RESOURCE_BARRIER;
struct D3D12_TEXTURE_COPY_LOCATION;
struct D3D12_BOX;

namespace rdrvr::d3d {

bool install_startup_hooks();   // from the bootstrap thread, after MTLX has unloaded

// Called on the presenting thread just before the real Present (frame end) and from the first
// ExecuteCommandLists on the present queue after a Present (frame start). Up to 16 listeners each (a full list logs).
using FrameEndFn = void (*)(uint64_t frame);
using FrameStartFn = void (*)(ID3D12CommandQueue* queue);
void add_frame_end_listener(FrameEndFn fn);
// One function called after every frame-end listener, just before the real Present (2026-10-29: the frame grabber's
// "grab <path> late", the back buffer as it is presented). Set once at startup.
void set_frame_end_last(FrameEndFn fn);
// The swapchain the presenting thread is presenting now (inside its Present's frame end; nullptr elsewhere, and on
// other threads): the one whose back buffer the frame-end listeners draw into (2026-10-29).
IDXGISwapChain* presenting_swapchain();
void add_frame_start_listener(FrameStartFn fn);

// Lets one module wrap each game ExecuteCommandLists on the present queue with two command lists of its own, sent
// in the same call (the GPU timer's per-submission timestamps). Return false to leave a submission alone.
using EclBracketFn = bool (*)(ID3D12CommandQueue* queue, ID3D12CommandList** before, ID3D12CommandList** after);
void set_ecl_bracket(EclBracketFn fn);

// Frame-graph capture: record command-list events for `frames` frames into RDRVR_framegraph_<n>.txt.
void framegraph_arm(int frames);
const char* framegraph_status();

// Submits one of our own command lists without it being counted as game work.
void submit_internal(ID3D12CommandQueue* queue, ID3D12CommandList* list);
// While one lives on this thread, ExecuteCommandLists calls are the mod's own (a library's uploads, ImGui's).
struct InternalSubmitScope {
    InternalSubmitScope();
    ~InternalSubmitScope();
};

// Name of a resource (from SetName or the game's CreateRenderTarget), or nullptr.
const char* resource_name(const void* resource);
void set_resource_name(const void* resource, const char* name);
const void* find_resource(const char* name);  // a resource with that name, or nullptr
bool resource_named(const void* resource, const char* name);  // that resource has that name (compared under the lock)

// Called right after each game CopyResource, in the game's own command list, while dst and src are in COPY_DEST and
// COPY_SOURCE: the frame grabber (and the round in hand's depth) append their own copy of src there, with no barrier
// on the game's resources. Up to two taps, each added once.
using CopyTapFn = void (*)(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, ID3D12Resource* src);
void set_copy_tap(CopyTapFn fn);

// Barrier watch: the last transition the game recorded for each watched resource (up to 4, reference-counted), so the
// mod can record a copy of that resource into the game's own command list from a state it has seen rather than
// guessed. A record holds only while that list stays open (Close and Reset end it), and only the thread that recorded
// the transition may use it.
void watch_add(const void* res);
void watch_remove(const void* res);
bool watched_state(const void* res, ID3D12GraphicsCommandList** cl, uint32_t* state);
// the game's transitions of a watched resource into a state that can write it (render target, UAV, depth write, copy or
// resolve dest), counted across lists and threads since it was first watched; 0 if not watched. A write with no
// transition (the resource left in such a state) is not seen
uint64_t watched_writes(const void* res);
// The registered resource called `name` that an engine object points at within its first `bytes` bytes, directly or
// one pointer deep (the live resource behind a render target the game may have recreated); nullptr if none. `where`
// receives "+0x58" or "+0x28->+0x10". Test-command use only (it locks the name table and queries memory).
const void* resource_in_object(const void* object, size_t bytes, const char* name, char* where, size_t where_len);
const void* unique_resource(const char* name);  // the only registered resource with that name, or nullptr
int rtv_handles_of(const void* res, uint64_t* out, int max);  // CPU handles of the render-target views seen for it

// Called on the recording thread just before each game OMSetRenderTargets, in the game's own list. Seven slots:
// kBindTapGrab (the eye grab), kBindTapXr (the eye images for the OpenXR swapchains), kBindTapLum (lumcheck),
// kBindTapBurst (the flicker burst), kBindTapAlign (the upscaler's alignment capture), kBindTapCutVel (the walk cut's
// velocity probe, cut_velocity.cpp: test only).
using BindTapFn = void (*)(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single,
                           const D3D12_CPU_DESCRIPTOR_HANDLE* ds);
constexpr int kBindTapGrab = 0, kBindTapXr = 1, kBindTapLum = 2, kBindTapBurst = 3, kBindTapAlign = 4, kBindTapCutVel = 5;
constexpr int kBindTapGpuStages = 6;  // ([Stereo] PrepassGpuStages' owed timestamps, gpu_stages.h: test only)
void set_bind_tap(int slot, BindTapFn fn);
// One substitution point, after the taps: may replace the first render-target view of the game's bind (only the CPU
// handle; the caller owns the resource's state). Returns true and sets *out to substitute (ui_layer.h).
using RtvSubstFn = bool (*)(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts,
                            const D3D12_CPU_DESCRIPTOR_HANDLE* ds, D3D12_CPU_DESCRIPTOR_HANDLE* out);
void set_rtv_substitute(RtvSubstFn fn);
// ([Stereo] PrepassEye2Velocity, eye2_velocity.h; test only) a second substitution slot, asked before the first at every
// game bind of at most 8 targets (n may be 0, rts may be null; the single-handle form included): it may replace only a
// one-target bind's view (n == 1). Null (the default): the bind is as before.
void set_rtv_substitute2(RtvSubstFn fn);
// The resource an RTV/DSV CPU handle was made for (the device's view hooks) and the view's format (UNKNOWN when the
// game gave no view desc); nullptr if the handle was never seen.
const void* view_resource(uintptr_t handle, int* format = nullptr);  // (format: a DXGI_FORMAT)
// A barrier and a whole-subresource copy into the game's list, past the mod's own list hooks (the mod's own work).
void raw_barrier(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_RESOURCE_BARRIER* b);
// (r120 HG3, test only) a DSV handle's D3D12_DSV_FLAGS as last created there (the map kept with [Stereo] MainInstanced=1 or DepthLog); -1 unknown
int dsv_flags(uintptr_t h);
void raw_bind(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single,
              const D3D12_CPU_DESCRIPTOR_HANDLE* ds);
void raw_copy_region(ID3D12GraphicsCommandList* cl, const D3D12_TEXTURE_COPY_LOCATION* dst, const D3D12_TEXTURE_COPY_LOCATION* src,
                     const D3D12_BOX* box = nullptr);  // (item 8e 14: a box, the region trace's)
// ([Stereo] MainInstanced, main_instanced.h; test only) a game draw's arguments, drawn again past the list hooks (the
// originals, as the raw helpers above): DrawInstanced(a vertices, b instances, c first vertex, e first instance) or
// DrawIndexedInstanced(a indices, b instances, c first index, d base vertex, e first instance); and a clear of a view.
struct DrawArgs {
    bool indexed;
    unsigned a, b, c;
    int d;
    unsigned e;
};
void raw_draw(ID3D12GraphicsCommandList* cl, const DrawArgs& x);
void raw_clear_rtv(ID3D12GraphicsCommandList* cl, const D3D12_CPU_DESCRIPTOR_HANDLE* h, const float rgba[4]);
// ([Stereo] MainSlice, run 25 item 5; test only) a clear of a depth-stencil view's both planes (the original, past the hook)
void raw_clear_dsv(ID3D12GraphicsCommandList* cl, const D3D12_CPU_DESCRIPTOR_HANDLE* h, float depth, unsigned char stencil);
// The view-instance mask the mod set on a list (the game never sets one; SetViewInstanceMask is not hooked): noted by the
// setter (prepass_arrays' instanced region), forgotten at the list's Reset and Close. known_...: false when the mod has
// set none on that list since its Reset (the list's initial mask then). Up to 16 lists; a full table notes nothing.
void note_view_instance_mask(ID3D12GraphicsCommandList* cl, unsigned mask);
bool known_view_instance_mask(ID3D12GraphicsCommandList* cl, unsigned* mask);
// ---- the list hooks' re-entrancy (research\run17\sps.md (B)). Under the D3D12 debug layer the hooks sit on
// D3D12SDKLayers.dll's functions, and the layer's function forwards the call to itself (or through the vtable to the
// same function) on the inner object at [cl+0x148]: the hook ran twice per game call. t_in_original[m] counts this
// thread's calls in method m's original (a hook's own call, or a raw helper's: raw_*, pso::set_raw); a hook entered
// while it is > 0 is that forward and passes straight to the original (counted, nothing else). Per method, so only a
// function calling itself is caught; without the layer no original calls itself and the count is 0 at every hook's
// entry: the hooks run as before. Raised around the original's call only, so the mod's own vtable calls in a hook's
// body (outside the original) are hooked as before. Every list hook: `if (d3d::nested(m)) return o_X(...);` first, and
// `d3d::InOriginal in(m);` just before its o_X call.
enum Nest : int {
    kNBarrier, kNBind, kNCopyRegion, kNIndirect, kNDraw, kNDrawIndexed, kNDispatch, kNCopy, kNClose, kNClearDsv, kNClearRtv,
    kNReset, kNEndQuery, kNPredication, kNBeginEvent,
    kNPipeline, kNVertexBuffers,             // pso.cpp
    kNGraphicsRootCbv, kNComputeRootCbv,     // ring_probe.cpp
    kNBeginQuery,                            // ([Stereo] MainSlice)
    kNRaster,                                // ([Stereo] MainRegionHandover: viewports, scissors, topology)
    kNestKinds
};
extern thread_local int t_in_original[kNestKinds];
void count_nested(Nest m);  // (only for a nested call: the debug layer)
struct InOriginal {  // around an original's call
    Nest m;
    explicit InOriginal(Nest x) : m(x) { ++t_in_original[m]; }
    ~InOriginal() { --t_in_original[m]; }
    InOriginal(const InOriginal&) = delete;
    InOriginal& operator=(const InOriginal&) = delete;
};
// at a hook's entry: true when this is the original's forward to itself (counted)
inline bool nested(Nest m) {
    if (t_in_original[m] == 0) return false;
    count_nested(m);
    return true;
}
// The calls passed so far, per method ("nested: barriers N, binds N, ..."; 0 without the layer).
void nested_status(char* out, size_t len);
// R5 pacing: while set, the game's Present runs with sync interval 0, so vsync does not hold the game to the desktop's
// refresh and xrWaitFrame paces it (the game's own frame limiter still applies).
void set_present_unsynced(bool on);

// The game making its frame again (its Graphics menu's resolution; research\run7\eye-shape.md, the resize row: the
// render class's targets are made again and named, then ResizeBuffers, then the "Main Backbuffer" RTs): called with
// the size of each "Post FXAA Target" or "Main Backbuffer" named (not FXAATarget: EyeShape re-makes it at the eye's
// size; SetName: the game's main or render thread), and around each ResizeBuffers ("ResizeBuffers" before it, with its arguments, 0 meaning the window's;
// "ResizeBuffers done" after it, with the buffers' size). One listener: xr's live-resize guard. It must be quick.
using FrameSizeFn = void (*)(const char* what, uint32_t w, uint32_t h);
void set_frame_size_listener(FrameSizeFn fn);

// Draw and query calls the calling thread has recorded so far (frame-graph hooks), for per-call attribution in tests.
uint64_t thread_draws();
// This recording thread's list has a predication set by its last SetPredication (after the mod's own null substitution),
// cleared only at the list's Reset (hk_Reset; a list starts unpredicated) (cut_velocity.cpp's d1: is the velocity pass
// predicated at playback?).
bool thread_predicated();
// ([Stereo] MainPredication, GOAL-single-pass item 6d; test only) with tracking set (once, at main_inst's install), hk_SetPredication
// keeps per thread the list and the predicate it passed on (after the null substitution above), hk_Reset makes it that list's
// "none", hk_Close forgets it. thread_predicate: kPredSet (*out the buffer, offset and op) or kPredNone for cl; kPredUnknown when
// not tracking, the record is another list's, or the list's Reset was not seen on this thread (never guessed). raw_set_predication: the original, past the hook (the record
// and thread_predicated() unchanged: the caller puts the recorded predicate back)
struct Predicate {
    ID3D12Resource* res = nullptr;
    uint64_t off = 0;
    int op = 0;  // (a D3D12_PREDICATION_OP)
};
enum : int { kPredUnknown = -1, kPredNone = 0, kPredSet = 1 };
void set_predicate_tracking(bool on);
int thread_predicate(ID3D12GraphicsCommandList* cl, Predicate* out);
void raw_set_predication(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, uint64_t off, int op);
// ([Stereo] MainSlice, test only) this recording thread's list has this many queries open that count draws (BeginQuery of any
// type but a timestamp, not yet ended), from its Reset; -1 when BeginQuery is not hooked (hooked only with MainSlice=1 and
// MainInstanced=1 at the list hooks' install)
int thread_open_queries(ID3D12GraphicsCommandList* cl);
// ([Stereo] MainRegionHandover, item 8g; test only, hooked only with it and MainInstanced) this thread's list's last viewports, scissor
// rects and primitive topology as the game set them (dropped at the list's Reset and Close, and when another list sets one on the thread).
// raster_note: false when not hooked or the note is another list's; has_*: set since the list's Reset. raw_set_raster: the originals,
// past the hooks (the note unchanged): n 0 leaves that state alone, topo 0 too. (vp: D3D12_VIEWPORT's six floats; sc: D3D12_RECT's four)
struct RasterNote {
    const void* cl = nullptr;
    unsigned nvp = 0, nsc = 0;
    float vp[16][6]{};
    int32_t sc[16][4]{};
    int topo = 0;
    bool has_vp = false, has_sc = false, has_topo = false;
};
bool raster_note(ID3D12GraphicsCommandList* cl, RasterNote* out);
// (item 8g, the review's SF1; hooked only with MainRegionHandover) this thread's list's graphics root signature as last set, and the root
// parameters (bit i: parameter i < 64) given a descriptor table since that set (SetGraphicsRootDescriptorTable); false: not hooked, the
// note another list's, or no root signature set since the list's Reset
bool root_note(ID3D12GraphicsCommandList* cl, const void** root, uint64_t* tables);
// (r117 DG4, test only; "prepass diag depthlog N", off by default) for the next N presents every list call that touches the game's
// "Shadow Collector Depth Buffer" is logged ([depthlog] lines): the game's and the mod's raw barriers, copies, depth clears and DSV
// binds, with the list, the thread, the region's pass, the subresource and the states; with [Debug] DepthLog=1 at start also each
// CreateShaderResourceView of it (its shader-visible heap index, the mod's own flagged). depthlog_status: the state
void depthlog_arm(int frames);
// "prepass diag depthlog N draws A B": also every draw with a per-list ordinal in [A, B] logged (the ordinal: the list's Draw,
// DrawIndexed and ExecuteIndirect calls since its Reset, the game's and the mod's raw ones, from 0; a second ordinal also counts
// Dispatch; a list not reset since the arming is marked inexact)
void depthlog_draws(int a, int b);
std::string depthlog_status();
// ([Debug] PixCapturer=1, test only; off: nothing loaded) the newest C:\Program Files\Microsoft PIX\<version>\WinPixGpuCapturer.dll
// loaded at the startup hooks, before d3d12.dll is hooked or loaded by the mod. pixcap: its PIXGpuCaptureNextFrames(file, frames)
bool pix_loaded();
std::string pixcap(const std::string& file, unsigned frames);
// (the delta review) the GPU handle the table parameter `param` (< 64) was last given since the root signature's set; false: not set
bool root_table_gpu(ID3D12GraphicsCommandList* cl, unsigned param, uint64_t* gpu);
void raw_set_raster(ID3D12GraphicsCommandList* cl, unsigned nvp, const float (*vp)[6], unsigned nsc, const int32_t (*sc)[4], int topo);
uint64_t thread_queries();
// This thread is inside the Present hook (its frame-end listeners included): the mod's own work there ([Stereo] PrepassGpuStages)
bool in_present();
// ([Stereo] DescCensus, GOAL-single-pass item 8e, research\run25\item8e.md 14; test only, off by default) the game's writes into its
// shader-visible CBV/SRV/UAV heap's partition 1 (the bindless Texture2D table: CreateShaderResourceView, CopyDescriptorsSimple,
// CopyDescriptors with a destination there), counted per frame (state::presents at the write) in a ring of the last 64 frames that had
// any: " dc_on=.. dc_writes=.. dc_in=.. dc_mod=.. dc_frames=.. dcf=frame:writes:within:first:last,..." (empty without the switch);
// "within": made while desc_phase is set (main_instanced: pass 1's DVL call 1 to pass 2's close); the mod's own writes apart (dc_mod)
// ([Debug] D3D12DebugLayer, item 8e) each debug-layer message id's count since the start, uncapped: " dbg_ids=id:count,..." (empty off)
std::string debug_msg_kv();
void desc_phase(bool in);
std::string desc_census_kv();

}  // namespace rdrvr::d3d
