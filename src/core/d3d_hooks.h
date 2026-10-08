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

#include <cstdint>

struct ID3D12CommandQueue;
struct ID3D12CommandList;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct D3D12_CPU_DESCRIPTOR_HANDLE;

namespace rdrvr::d3d {

bool install_startup_hooks();   // from the bootstrap thread, after MTLX has unloaded

// Called on the presenting thread just before the real Present (frame end) and from the first
// ExecuteCommandLists on the present queue after a Present (frame start). Up to 16 listeners each (a full list logs).
using FrameEndFn = void (*)(uint64_t frame);
using FrameStartFn = void (*)(ID3D12CommandQueue* queue);
void add_frame_end_listener(FrameEndFn fn);
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
// The registered resource called `name` that an engine object points at within its first `bytes` bytes, directly or
// one pointer deep (the live resource behind a render target the game may have recreated); nullptr if none. `where`
// receives "+0x58" or "+0x28->+0x10". Test-command use only (it locks the name table and queries memory).
const void* resource_in_object(const void* object, size_t bytes, const char* name, char* where, size_t where_len);
const void* unique_resource(const char* name);  // the only registered resource with that name, or nullptr
int rtv_handles_of(const void* res, uint64_t* out, int max);  // CPU handles of the render-target views seen for it

// Called on the recording thread just before each game OMSetRenderTargets, in the game's own list. Four slots:
// kBindTapGrab (the eye grab), kBindTapXr (the eye images for the OpenXR swapchains), kBindTapLum (lumcheck),
// kBindTapBurst (the flicker burst), kBindTapAlign (the upscaler's alignment capture).
using BindTapFn = void (*)(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single,
                           const D3D12_CPU_DESCRIPTOR_HANDLE* ds);
constexpr int kBindTapGrab = 0, kBindTapXr = 1, kBindTapLum = 2, kBindTapBurst = 3, kBindTapAlign = 4;
void set_bind_tap(int slot, BindTapFn fn);
// One substitution point, after the taps: may replace the first render-target view of the game's bind (only the CPU
// handle; the caller owns the resource's state). Returns true and sets *out to substitute (ui_layer.h).
using RtvSubstFn = bool (*)(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts,
                            const D3D12_CPU_DESCRIPTOR_HANDLE* ds, D3D12_CPU_DESCRIPTOR_HANDLE* out);
void set_rtv_substitute(RtvSubstFn fn);
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
uint64_t thread_queries();

}  // namespace rdrvr::d3d
