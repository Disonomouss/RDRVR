#include "core/d3d_hooks.h"

#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "core/anchors.h"
#include "core/config.h"
#include "core/frame_probe.h"
#include "core/hooks.h"
#include "core/image_mem.h"
#include "core/gpu_stages.h"
#include "core/cut_velocity.h"
#include "core/main_instanced.h"
#include "core/prepass_arrays.h"
#include "core/render_res.h"
#include "core/pso.h"
#include "core/log.h"
#include "core/ring_probe.h"
#include "core/state.h"

namespace rdrvr::d3d {
namespace {

// ---------------------------------------------------------------------------------------------- originals
using CreateDXGIFactory2_t = HRESULT(WINAPI*)(UINT, REFIID, void**);
using CreateCommandQueue_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
using CreateCommittedResource_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                              const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                              const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreatePlacedResource_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
                                                           D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreateView_t = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const void*, D3D12_CPU_DESCRIPTOR_HANDLE);
using SetName_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Object*, LPCWSTR);
using ECL_t = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using CreateSwapChainForHwnd_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                             IDXGISwapChain1**);
using CreateSwapChain_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffers_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PFN_D3D12_CREATE_DEVICE o_D3D12CreateDevice = nullptr;
CreateDXGIFactory2_t o_CreateDXGIFactory2 = nullptr;
CreateCommandQueue_t o_CreateCommandQueue = nullptr;
// ---- constant-buffer descriptor headroom (2026-10-04: two crashes, the game's own assert "write to 0x3" in
// FUN_140f732b0 when its "ConstantBuffer" descriptor partition (type 6) passed its 40,000 capacity). The shader-visible
// CBV/SRV/UAV heap is sized as the sum of all seven partitions, 32 + 15000 + 32 + 32 + 112 + 16 + 40000 = 55,224
// (type 6 is 60,000 when DAT_142b00ff0 is set: 75,224), but types 4 and 5 (112 + 16) are the sampler heap's, so in
// this heap types 0, 1, 2, 3 and 6 fill 0..55,095, type 6 last (start 15,096), and the last 128 are unused. A constant
// buffer takes one descriptor per dirty bind and the pool grows to the busiest frame; the double scene pass needs
// about twice the flat game's (38,900 in a quiet stereo scene). The heap is created larger by [Render]
// ConstantBufferHeadroom descriptors and, at the first frame end, type 6's capacity (device +0x388) is raised to the
// heap's end: the new descriptors follow type 6 in the same heap.
using CreateDescHeap_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
CreateDescHeap_t o_CreateDescHeap = nullptr;
constexpr UINT kOtherPartitions = 32 + 15000 + 32 + 32 + 112 + 16;
constexpr UINT kSamplerPartitions = 112 + 16;
std::atomic<UINT> g_cb_heap_n{0};    // the game's size of the enlarged heap (0: not enlarged)
std::atomic<UINT> g_cb_headroom{0};  // descriptors added to it
std::atomic<bool> g_cb_cap_raised{false};
// ([Stereo] DescCensus, item 8e; test only) the game's shader-visible heap (the one enlarged above): its CPU start, its increment, its
// partition 1 (the bindless Texture2D table: descriptors 32..15031, kOtherPartitions' second term)
std::atomic<uintptr_t> g_dc_cpu0{0};
std::atomic<UINT> g_dc_inc{0};
constexpr UINT kDcP1First = 32, kDcP1Count = 15000;
std::atomic<bool> g_dc_on{false};
std::atomic<uint64_t> g_dc_writes{0}, g_dc_frames{0}, g_dc_mod{0}, g_dc_in{0};
// (main_instanced, the render thread) from pass 1's DVL call 1 to pass 2's close: the CPU recording window, not the GPU's (a write
// there is seen by both passes' execution; the frame number is the test's measure, matched with the failing frames)
std::atomic<bool> g_dc_phase{false};
uintptr_t g_dc_self0 = 0, g_dc_self1 = 0;  // the mod's own image: its writes counted apart (eye2_images', the slice SRVs)
struct DcSlot {
    std::atomic<uint64_t> frame{0};
    std::atomic<uint32_t> n{0}, in{0}, first{0}, last{0};
};
DcSlot g_dc_ring[64];
void dc_note(D3D12_CPU_DESCRIPTOR_HANDLE h, UINT count, uintptr_t ret) {  // (any thread) count descriptors from h inside partition 1
    const uintptr_t c0 = g_dc_cpu0.load(std::memory_order_relaxed);
    const UINT inc = g_dc_inc.load(std::memory_order_relaxed);
    if (!c0 || !inc || h.ptr < c0 || !count) return;
    const uint64_t i0 = (h.ptr - c0) / inc;
    if (i0 >= kDcP1First + kDcP1Count || i0 + count <= kDcP1First) return;
    const uint32_t a = static_cast<uint32_t>(i0 < kDcP1First ? kDcP1First : i0);
    const uint64_t e = i0 + count < kDcP1First + kDcP1Count ? i0 + count : kDcP1First + kDcP1Count;
    const uint32_t n = static_cast<uint32_t>(e - a);
    if (ret >= g_dc_self0 && ret < g_dc_self1) {
        g_dc_mod.fetch_add(n, std::memory_order_relaxed);
        return;
    }
    const bool in = g_dc_phase.load(std::memory_order_relaxed);
    if (in) g_dc_in.fetch_add(n, std::memory_order_relaxed);
    const uint64_t f = state::presents.load(std::memory_order_relaxed);
    DcSlot& s = g_dc_ring[f & 63];
    uint64_t was = s.frame.load(std::memory_order_relaxed);
    if (was != f && s.frame.compare_exchange_strong(was, f)) {  // (a new frame takes the slot; a racing writer may lose a count)
        s.n.store(0, std::memory_order_relaxed);
        s.in.store(0, std::memory_order_relaxed);
        s.first.store(a, std::memory_order_relaxed);
        g_dc_frames.fetch_add(1, std::memory_order_relaxed);
    }
    s.n.fetch_add(n, std::memory_order_relaxed);
    if (in) s.in.fetch_add(n, std::memory_order_relaxed);
    s.last.store(a + n - 1, std::memory_order_relaxed);
    g_dc_writes.fetch_add(n, std::memory_order_relaxed);
}
using CreateSRV_t = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CopyDescs_t = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using CopyDescsSimple_t = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE,
                                                  D3D12_DESCRIPTOR_HEAP_TYPE);
CreateSRV_t o_CreateSRV = nullptr;
CopyDescs_t o_CopyDescs = nullptr;
CopyDescsSimple_t o_CopyDescsSimple = nullptr;
// ---- (r117 DG4, test only) the depthlog: the next N presents' list calls on the game's depth, logged
std::atomic<int> g_dl_frames{0};
std::atomic<const void*> g_dl_res{nullptr};  // (the depth found by name, cached)
std::atomic<uint64_t> g_dl_lines{0};
uintptr_t g_dl_self0 = 0, g_dl_self1 = 0;  // (the mod's image: its SRV creations flagged)
inline bool dl_on() { return g_dl_frames.load(std::memory_order_relaxed) > 0; }
bool dl_is(const void* r) {
    if (!r) return false;
    if (g_dl_res.load(std::memory_order_relaxed) == r) return true;
    if (!resource_named(r, "Shadow Collector Depth Buffer")) return false;
    g_dl_res.store(r, std::memory_order_relaxed);
    return true;
}
void dl_line(const char* what, bool mod, const void* cl, const char* detail) {
    g_dl_lines.fetch_add(1, std::memory_order_relaxed);
    log::limited("d3d.depthlog", 20000, "[depthlog] present %llu %s%s list %p thread %lu pass %d: %s", static_cast<unsigned long long>(state::presents.load()),
                 mod ? "MOD " : "", what, cl, GetCurrentThreadId(), main_inst::pb_pass(), detail);
}
void dl_barriers(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_RESOURCE_BARRIER* b, bool mod) {
    for (UINT i = 0; i < n && b; ++i) {
        char d[200];
        if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && dl_is(b[i].Transition.pResource)) {
            std::snprintf(d, sizeof(d), "transition sub %d %#x -> %#x flags %#x", static_cast<int>(b[i].Transition.Subresource),
                          static_cast<unsigned>(b[i].Transition.StateBefore), static_cast<unsigned>(b[i].Transition.StateAfter), static_cast<unsigned>(b[i].Flags));
        } else if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV && dl_is(b[i].UAV.pResource)) {
            std::snprintf(d, sizeof(d), "UAV barrier");
        } else if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING && (dl_is(b[i].Aliasing.pResourceBefore) || dl_is(b[i].Aliasing.pResourceAfter))) {
            std::snprintf(d, sizeof(d), "aliasing %p -> %p", static_cast<const void*>(b[i].Aliasing.pResourceBefore),
                          static_cast<const void*>(b[i].Aliasing.pResourceAfter));
        } else {
            continue;
        }
        dl_line("ResourceBarrier", mod, cl, d);
    }
}
void dl_copy(ID3D12GraphicsCommandList* cl, const void* dst, const void* src, int dsub, int ssub, bool mod, const char* what) {
    if (!dl_is(dst) && !dl_is(src)) return;
    char d[160];
    std::snprintf(d, sizeof(d), "%s %p sub %d <- %p sub %d", dl_is(dst) ? "into the depth" : "from the depth", dst, dsub, src, ssub);
    dl_line(what, mod, cl, d);
}
void dl_dsv(ID3D12GraphicsCommandList* cl, const D3D12_CPU_DESCRIPTOR_HANDLE* ds, bool mod, const char* what, const char* extra) {
    if (!ds) return;
    if (!dl_is(view_resource(ds->ptr))) return;
    char d[160];
    std::snprintf(d, sizeof(d), "DSV %#llx%s", static_cast<unsigned long long>(ds->ptr), extra ? extra : "");
    dl_line(what, mod, cl, d);
}
// (r117 DG4's depthlog: "draws A B") the per-list draw ordinals since the list's Reset, the thread's last DSV bind, the draws logged
std::atomic<int> g_dl_da{-1}, g_dl_db{-1};
struct DlList {
    const void* cl = nullptr;
    uint32_t draws = 0, all = 0;  // (Draw / DrawIndexed / ExecuteIndirect; and Dispatch too)
    bool exact = false;           // (its Reset seen while armed)
};
thread_local DlList t_dl[8];
thread_local int t_dl_next = 0;
thread_local const void* t_dl_bind_cl = nullptr;
thread_local uintptr_t t_dl_bind_ds = 0;  // (0: no DSV)
std::mutex g_dsv_flags_mutex;
bool g_dsv_flags_on = false;  // (decided once at the device hooks: [Stereo] MainInstanced=1 or [Debug] DepthLog=1; test only)
std::unordered_map<uintptr_t, uint32_t> g_dsv_flags;  // (under g_dsv_flags_mutex) each DSV's D3D12_DSV_FLAGS
DlList& dl_list(const void* cl) {
    for (DlList& l : t_dl)
        if (l.cl == cl) return l;
    DlList& l = t_dl[t_dl_next++ & 7];
    l = DlList{cl, 0, 0, false};
    return l;
}
void dl_reset_list(const void* cl) {  // (a Reset while armed: the list's ordinals from 0, exact)
    DlList& l = dl_list(cl);
    l = DlList{cl, 0, 0, true};
}
void dl_draw(ID3D12GraphicsCommandList* cl, const char* what, bool draw, bool mod, uintptr_t ret) {
    DlList& l = dl_list(cl);
    const uint32_t o = l.draws, oa = l.all;
    if (draw) ++l.draws;
    ++l.all;
    const int a = g_dl_da.load(std::memory_order_relaxed), b = g_dl_db.load(std::memory_order_relaxed);
    if (!draw || a < 0 || static_cast<int64_t>(o) < a || static_cast<int64_t>(o) > b) return;
    uint64_t vs = 0, ps = 0;
    uint32_t wr = 0;
    const bool game = pso::current(cl, &vs, &ps, &wr);
    const void* raw = pso::raw_current(cl);
    const void* root = nullptr;
    uint64_t tables = 0;
    const bool rn = root_note(cl, &root, &tables);
    const void* dsv_res = nullptr;
    uint32_t dsv_flags = 0;
    bool dsv_known = false;
    if (t_dl_bind_cl == cl && t_dl_bind_ds) {
        dsv_res = view_resource(t_dl_bind_ds);
        std::lock_guard lock(g_dsv_flags_mutex);
        auto it = g_dsv_flags.find(t_dl_bind_ds);
        if (it != g_dsv_flags.end()) {
            dsv_flags = it->second;
            dsv_known = true;
        }
    }
    D3D12_RESOURCE_STATES s0{}, s1{};
    const bool f0 = prepass::kind_followed(0, 0, &s0), f1 = prepass::kind_followed(0, 1, &s1);
    D3D12_RESOURCE_STATES q{};
    const bool fr0 = f0 && prepass::kind_followed_fresh(0, 0, cl, &q), fr1 = f1 && prepass::kind_followed_fresh(0, 1, cl, &q);
    char d[480];
    std::snprintf(d, sizeof(d),
                  "draw ordinal %u (with dispatches %u)%s; %s pipeline %s vs %016llx ps %016llx (raw %p); root signature %s%p (tables %#llx); DSV %s%s "
                  "flags %s%#x; depth followed %d:%d %#x:%#x fresh %d:%d%s",
                  o, oa, l.exact ? "" : " (inexact: the list not reset since the arming)", what, game ? "the game's" : "not noted", static_cast<unsigned long long>(vs),
                  static_cast<unsigned long long>(ps), raw, rn ? "" : "not noted ", root, static_cast<unsigned long long>(tables),
                  t_dl_bind_cl != cl ? "not noted" : !t_dl_bind_ds ? "none" : dl_is(dsv_res) ? "the game's depth" : "another", "",
                  dsv_known ? "" : "unknown ", dsv_flags, f0 ? 1 : 0, f1 ? 1 : 0, static_cast<unsigned>(s0), static_cast<unsigned>(s1), fr0 ? 1 : 0, fr1 ? 1 : 0, "");
    char m[64] = "";
    if (mod) std::snprintf(m, sizeof(m), " (the mod's raw_draw, called from RDRVR+%#llx)", static_cast<unsigned long long>(ret >= g_dl_self0 && ret < g_dl_self1 ? ret - g_dl_self0 : 0));
    char line[560];
    std::snprintf(line, sizeof(line), "%s%s", d, m);
    dl_line("Draw", mod, cl, line);
}
void STDMETHODCALLTYPE hk_CreateSRV(ID3D12Device* dev, ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    o_CreateSRV(dev, r, d, h);
    if (g_dc_on.load(std::memory_order_relaxed)) dc_note(h, 1, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    if (dl_on() && dl_is(r)) {  // (r117 DG4's depthlog: an SRV of the game's depth; its index in the game's shader-visible heap)
        const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const uintptr_t c0 = g_dc_cpu0.load(std::memory_order_relaxed);
        const UINT inc = g_dc_inc.load(std::memory_order_relaxed);
        char t[200];
        if (c0 && inc && h.ptr >= c0) std::snprintf(t, sizeof(t), "an SRV at heap index %llu (format %d)", static_cast<unsigned long long>((h.ptr - c0) / inc),
                                                    d ? static_cast<int>(d->Format) : -1);
        else std::snprintf(t, sizeof(t), "an SRV at CPU %#llx, not the game's heap (format %d)", static_cast<unsigned long long>(h.ptr), d ? static_cast<int>(d->Format) : -1);
        dl_line("CreateShaderResourceView", ret >= g_dl_self0 && ret < g_dl_self1, nullptr, t);
    }
}
void STDMETHODCALLTYPE hk_CopyDescs(ID3D12Device* dev, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* ds, const UINT* dn, UINT ns,
                                    const D3D12_CPU_DESCRIPTOR_HANDLE* ss, const UINT* sn, D3D12_DESCRIPTOR_HEAP_TYPE t) {
    o_CopyDescs(dev, nd, ds, dn, ns, ss, sn, t);
    if (g_dc_on.load(std::memory_order_relaxed) && t == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && ds)
        for (UINT i = 0; i < nd; ++i) dc_note(ds[i], dn ? dn[i] : 1, reinterpret_cast<uintptr_t>(_ReturnAddress()));
}
void STDMETHODCALLTYPE hk_CopyDescsSimple(ID3D12Device* dev, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src,
                                          D3D12_DESCRIPTOR_HEAP_TYPE t) {
    o_CopyDescsSimple(dev, n, dst, src, t);
    if (g_dc_on.load(std::memory_order_relaxed) && t == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) dc_note(dst, n, reinterpret_cast<uintptr_t>(_ReturnAddress()));
}

HRESULT STDMETHODCALLTYPE hk_CreateDescHeap(ID3D12Device* dev, const D3D12_DESCRIPTOR_HEAP_DESC* d, REFIID riid, void** out) {
    if (d) {
        log::info("[d3d] CreateDescriptorHeap type %d, %u descriptors, flags %#x", static_cast<int>(d->Type), d->NumDescriptors,
                  static_cast<unsigned>(d->Flags));
        UINT extra = static_cast<UINT>(config::get_int("Render", "ConstantBufferHeadroom", 40000));
        bool game_heap = d->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && (d->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) &&
                         (d->NumDescriptors == kOtherPartitions + 40000 || d->NumDescriptors == kOtherPartitions + 60000);
        if (extra > 0 && extra <= 400000 && game_heap && g_cb_heap_n.load() == 0) {
            D3D12_DESCRIPTOR_HEAP_DESC big = *d;
            big.NumDescriptors += extra;
            HRESULT hr = o_CreateDescHeap(dev, &big, riid, out);
            if (SUCCEEDED(hr)) {
                g_cb_headroom = extra;
                g_cb_heap_n = d->NumDescriptors;
                if (out && *out && riid == __uuidof(ID3D12DescriptorHeap) && config::get_int("Stereo", "DescCensus", 0) == 1) {  // (DescCensus)
                    g_dc_cpu0 = static_cast<ID3D12DescriptorHeap*>(*out)->GetCPUDescriptorHandleForHeapStart().ptr;
                    g_dc_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                }
                log::info("[d3d] constant-buffer heap enlarged to %u descriptors (+%u for the type-6 partition)", big.NumDescriptors, extra);
                return hr;
            }
            log::warn("[d3d] the enlarged constant-buffer heap failed (%#lx); the game's size is used", static_cast<unsigned long>(hr));
        }
    }
    return o_CreateDescHeap(dev, d, riid, out);
}

// At a frame end: raise type 6's capacity once the heap holds the room (the partition table is filled by DeviceInit).
void raise_cb_capacity() {
    UINT heap_n = g_cb_heap_n.load();
    if (!heap_n || g_cb_cap_raised.load()) return;
    char* devobj = *reinterpret_cast<char**>(anchors::addr(anchors::Id::D3dDeviceSingleton));
    if (!devobj) return;
    auto* cap = reinterpret_cast<uint64_t*>(devobj + 0x208 + 6 * 0x40);
    int start = *reinterpret_cast<int*>(devobj + 0x1f0 + 6 * 0x40);
    if ((*cap == 40000 || *cap == 60000) && start > 0 && start + *cap + kSamplerPartitions == heap_n) {
        uint64_t old = *cap;
        *cap = heap_n + g_cb_headroom.load() - static_cast<uint64_t>(start);
        log::info("[d3d] constant-buffer partition capacity %llu -> %llu", static_cast<unsigned long long>(old),
                  static_cast<unsigned long long>(*cap));
    } else {
        log::warn("[d3d] constant-buffer partition not as expected (start %d, capacity %llu, heap %u): left as is", start,
                  static_cast<unsigned long long>(*cap), heap_n);
    }
    g_cb_cap_raised = true;
}
CreateCommittedResource_t o_CreateCommittedResource = nullptr;
CreatePlacedResource_t o_CreatePlacedResource = nullptr;
CreateView_t o_CreateRTV = nullptr, o_CreateDSV = nullptr;
SetName_t o_ResourceSetName = nullptr;
ECL_t o_ECL = nullptr;
CreateSwapChainForHwnd_t o_CreateSwapChainForHwnd = nullptr;
CreateSwapChain_t o_CreateSwapChain = nullptr;
Present_t o_Present = nullptr;
Present1_t o_Present1 = nullptr;
ResizeBuffers_t o_ResizeBuffers = nullptr;

std::once_flag g_device_once, g_queue_once, g_factory_once, g_swapchain_once;

// ------------------------------------------------------------------------------------------ listeners
constexpr int kMaxListeners = 32;  // 18 frame-end listeners by run 4: at 16 the last two (render_settings' forced AA, pso) were dropped
FrameEndFn g_end_listeners[kMaxListeners];
FrameStartFn g_start_listeners[kMaxListeners];
std::atomic<int> g_end_count{0}, g_start_count{0};
std::atomic<bool> g_frame_started{false};
std::atomic<EclBracketFn> g_bracket{nullptr};
std::atomic<FrameEndFn> g_end_last{nullptr};  // set_frame_end_last: after every listener
thread_local IDXGISwapChain* t_presenting = nullptr;  // presenting_swapchain(): inside hk_Present's / hk_Present1's frame end
std::atomic<CopyTapFn> g_copy_tap[2] = {nullptr, nullptr};  // the frame grabber's, the round in hand's depth (round_draw)

// --------------------------------------------------------------------------------------- per-frame counts
std::atomic<uint32_t> t_ecl_calls{0}, t_ecl_lists{0}, t_ecl_direct{0}, t_ecl_compute{0}, t_ecl_copy{0}, t_ecl_present{0};
thread_local int t_in_present = 0;
thread_local int t_in_ecl = 0;
double g_prev_present_ms = 0.0;

// Queue type cache (the game creates a handful of queues).
struct QueueType {
    std::atomic<ID3D12CommandQueue*> queue{nullptr};
    D3D12_COMMAND_LIST_TYPE type{};
};
QueueType g_queue_types[32];

D3D12_COMMAND_LIST_TYPE queue_type(ID3D12CommandQueue* q) {
    for (QueueType& e : g_queue_types) {
        if (e.queue.load(std::memory_order_acquire) == q) return e.type;
    }
    D3D12_COMMAND_LIST_TYPE t = q->GetDesc().Type;
    for (QueueType& e : g_queue_types) {
        ID3D12CommandQueue* expected = nullptr;
        if (e.queue.compare_exchange_strong(expected, nullptr)) {  // slot empty: claim it
            e.type = t;
            e.queue.store(q, std::memory_order_release);
            break;
        }
    }
    return t;
}

// --------------------------------------------------------------------------------------- names
std::mutex g_names_mutex;
std::unordered_map<const void*, std::string> g_names;            // ID3D12Resource* -> name
std::unordered_map<SIZE_T, const void*> g_rtv;                    // RTV/DSV descriptor -> resource
std::unordered_map<SIZE_T, DXGI_FORMAT> g_view_fmt;               // RTV/DSV descriptor -> its view's format (UNKNOWN: none given)
uint32_t g_names_logged = 0;

std::string narrow(LPCWSTR w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

const char* fmt_name(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "BGRA8_SRGB";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10F";
        case DXGI_FORMAT_R32_FLOAT: return "R32F";
        case DXGI_FORMAT_R16G16_FLOAT: return "RG16F";
        case DXGI_FORMAT_D32_FLOAT: return "D32F";
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32FS8";
        case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TL";
        case DXGI_FORMAT_R32_TYPELESS: return "R32_TL";
        case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TL";
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "RGBA8_TL";
        default: return nullptr;
    }
}

void describe_desc(const D3D12_RESOURCE_DESC* d, char* out, size_t len) {
    const char* f = fmt_name(d->Format);
    char fb[16];
    if (!f) {
        std::snprintf(fb, sizeof(fb), "fmt%d", static_cast<int>(d->Format));
        f = fb;
    }
    std::snprintf(out, len, "%s %llux%u x%u mips%u %s flags%#x", d->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? "buf" : "tex",
                  static_cast<unsigned long long>(d->Width), d->Height, d->DepthOrArraySize, d->MipLevels, f,
                  static_cast<unsigned>(d->Flags));
}

// ---------------------------------------------------------------------------------- frame graph capture
std::atomic<int> g_fg_frames_left{0};
std::atomic<bool> g_fg_armed{false};
std::mutex g_fg_mutex;
std::string g_fg_text;
uint64_t g_fg_frame0 = 0;
int g_fg_file_index = 0;
char g_fg_status[128] = "idle";

struct ListLog {
    std::string text;
    uint32_t draws = 0, dispatches = 0, barriers = 0, queries = 0, indirect = 0;
    std::string targets = "-";
    std::map<std::tuple<uint64_t, uint64_t, uint32_t>, uint32_t> shaders;  // (vs, ps, writes) -> draws in the segment
};
std::unordered_map<const void*, ListLog> g_fg_lists;   // command list -> events since last flush

const char* res_or_hex(const void* res, char* buf, size_t len) {
    auto it = g_names.find(res);
    if (it != g_names.end() && !it->second.empty()) return it->second.c_str();
    std::snprintf(buf, len, "%p", res);
    return buf;
}

void fg_flush_segment(ListLog& l) {
    if (l.draws || l.dispatches || l.barriers || l.queries || l.indirect) {
        char line[512];
        std::snprintf(line, sizeof(line), "    [%s] draws %u dispatch %u indirect %u barriers %u queries %u\n",
                      l.targets.c_str(), l.draws, l.dispatches, l.indirect, l.barriers, l.queries);
        l.text += line;
        for (const auto& [k, n] : l.shaders) {
            std::snprintf(line, sizeof(line), "      vs %016llx ps %016llx writes %03x: %u\n", static_cast<unsigned long long>(std::get<0>(k)),
                          static_cast<unsigned long long>(std::get<1>(k)), std::get<2>(k), n);
            l.text += line;
        }
    }
    l.draws = l.dispatches = l.barriers = l.queries = l.indirect = 0;
    l.shaders.clear();
}

void fg_event(const void* list, const char* fmt, ...) {
    if (!g_fg_armed.load(std::memory_order_relaxed)) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    std::lock_guard lock(g_fg_mutex);
    ListLog& l = g_fg_lists[list];
    fg_flush_segment(l);
    l.text += "    ";
    l.text += line;
    l.text += '\n';
}

template <typename F>
void fg_count(const void* list, F&& bump) {
    if (!g_fg_armed.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_fg_mutex);
    bump(g_fg_lists[list]);
}

// Command list method originals (frame graph).
using OMSetRenderTargets_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                                      const D3D12_CPU_DESCRIPTOR_HANDLE*);
using DrawInstanced_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using DrawIndexedInstanced_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using Dispatch_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using CopyResource_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
using CopyTextureRegion_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT,
                                                     UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
using ResourceBarrier_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using ClearRTV_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT[4], UINT,
                                            const D3D12_RECT*);
using ClearDSV_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT,
                                            UINT8, UINT, const D3D12_RECT*);
using BeginEvent_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const void*, UINT);
using EndQuery_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT);
using BeginQuery_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT);
using ExecuteIndirect_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64,
                                                   ID3D12Resource*, UINT64);
using SetPredication_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64, D3D12_PREDICATION_OP);
using Close_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using IASetPrimitiveTopology_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_PRIMITIVE_TOPOLOGY);  // ([Stereo] MainRegionHandover)
using RSSetViewports_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
using RSSetScissorRects_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
IASetPrimitiveTopology_t o_IASetPrimitiveTopology = nullptr;
RSSetViewports_t o_RSSetViewports = nullptr;
RSSetScissorRects_t o_RSSetScissorRects = nullptr;
std::atomic<bool> g_rs_hooked{false};
thread_local RasterNote t_rs;
// (item 8g, the review's SF1) the graphics root signature and the descriptor tables set since it, per thread's list
using SetGraphicsRootSignature_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using SetGraphicsRootDescriptorTable_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
SetGraphicsRootSignature_t o_SetGraphicsRootSignature = nullptr;
SetGraphicsRootDescriptorTable_t o_SetGraphicsRootDescriptorTable = nullptr;
std::atomic<bool> g_rn_hooked{false};
struct RootNote {
    const void* cl = nullptr;
    const void* root = nullptr;
    uint64_t tables = 0;
    uint64_t gpu[64] = {};  // (the delta review: each table's handle as last given)
};
thread_local RootNote t_rn;
void STDMETHODCALLTYPE hk_SetGraphicsRootSignature(ID3D12GraphicsCommandList* cl, ID3D12RootSignature* r) {
    if (nested(kNRaster)) return o_SetGraphicsRootSignature(cl, r);
    t_rn = RootNote{};  // (a set makes every root argument undefined: the tables noted again from here)
    t_rn.cl = cl;
    t_rn.root = r;
    InOriginal in(kNRaster);
    o_SetGraphicsRootSignature(cl, r);
}
void STDMETHODCALLTYPE hk_SetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList* cl, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    if (nested(kNRaster)) return o_SetGraphicsRootDescriptorTable(cl, i, h);
    if (t_rn.cl == cl && i < 64) {
        t_rn.tables |= 1ull << i;
        t_rn.gpu[i] = h.ptr;
    }
    InOriginal in(kNRaster);
    o_SetGraphicsRootDescriptorTable(cl, i, h);
}
void rs_take(ID3D12GraphicsCommandList* cl) {  // (this thread's note is this list's from here)
    if (t_rs.cl != cl) {
        t_rs = RasterNote{};
        t_rs.cl = cl;
    }
}
void STDMETHODCALLTYPE hk_IASetPrimitiveTopology(ID3D12GraphicsCommandList* cl, D3D12_PRIMITIVE_TOPOLOGY t) {
    if (nested(kNRaster)) return o_IASetPrimitiveTopology(cl, t);
    rs_take(cl);
    t_rs.topo = static_cast<int>(t);
    t_rs.has_topo = true;
    InOriginal in(kNRaster);
    o_IASetPrimitiveTopology(cl, t);
}
void STDMETHODCALLTYPE hk_RSSetViewports(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_VIEWPORT* v) {
    if (nested(kNRaster)) return o_RSSetViewports(cl, n, v);
    rs_take(cl);
    t_rs.has_vp = n >= 1 && n <= 16 && v;
    t_rs.nvp = t_rs.has_vp ? n : 0;
    static_assert(sizeof(D3D12_VIEWPORT) == sizeof(t_rs.vp[0]) && sizeof(D3D12_RECT) == sizeof(t_rs.sc[0]), "the note's layouts");
    for (UINT i = 0; i < t_rs.nvp; ++i) std::memcpy(t_rs.vp[i], &v[i], sizeof(t_rs.vp[0]));
    InOriginal in(kNRaster);
    o_RSSetViewports(cl, n, v);
}
void STDMETHODCALLTYPE hk_RSSetScissorRects(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_RECT* r) {
    if (nested(kNRaster)) return o_RSSetScissorRects(cl, n, r);
    rs_take(cl);
    t_rs.has_sc = n >= 1 && n <= 16 && r;
    t_rs.nsc = t_rs.has_sc ? n : 0;
    for (UINT i = 0; i < t_rs.nsc; ++i) std::memcpy(t_rs.sc[i], &r[i], sizeof(t_rs.sc[0]));
    InOriginal in(kNRaster);
    o_RSSetScissorRects(cl, n, r);
}
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);

OMSetRenderTargets_t o_OMSetRenderTargets = nullptr;
DrawInstanced_t o_DrawInstanced = nullptr;
DrawIndexedInstanced_t o_DrawIndexedInstanced = nullptr;
Dispatch_t o_Dispatch = nullptr;
CopyResource_t o_CopyResource = nullptr;
CopyTextureRegion_t o_CopyTextureRegion = nullptr;
ResourceBarrier_t o_ResourceBarrier = nullptr;
ClearRTV_t o_ClearRTV = nullptr;
ClearDSV_t o_ClearDSV = nullptr;
BeginEvent_t o_BeginEvent = nullptr;
EndQuery_t o_EndQuery = nullptr;
BeginQuery_t o_BeginQuery = nullptr;  // ([Stereo] MainSlice only)
ExecuteIndirect_t o_ExecuteIndirect = nullptr;
SetPredication_t o_SetPredication = nullptr;
Close_t o_Close = nullptr;
Reset_t o_Reset = nullptr;

// ---- re-entrancy (research\run17\sps.md (B); the guard itself: d3d_hooks.h, Nest / InOriginal / nested())
std::atomic<uint64_t> g_nested[kNestKinds] = {};

const void* rtv_resource(SIZE_T ptr) {
    auto it = g_rtv.find(ptr);
    return it == g_rtv.end() ? nullptr : it->second;
}

std::atomic<BindTapFn> g_bind_tap[7];
std::atomic<RtvSubstFn> g_rtv_subst{nullptr};
std::atomic<RtvSubstFn> g_rtv_subst2{nullptr};  // ([Stereo] PrepassEye2Velocity, test only: null unless installed)
std::atomic<FrameSizeFn> g_frame_size{nullptr};
std::atomic<bool> g_present_unsynced{false};
std::atomic<uint64_t> g_unsynced_presents{0};
// 2026-11-05 (the user: "cutscenes have been low fps"; the other PC's 3D cutscenes held at 60.1 Hz with Present itself
// blocking 9.4 ms a frame, gameplay at 90 Hz with 0.14 ms): what the game asks of Present, for the cutscene log; and
// [XR] PacingTearing (off; the simulator: the game asks for vsync on every frame, in play too, and in October's logs the
// other PC held 90 Hz in play with the interval taken off and no flag, so this may not be its cause; the log decides):
// with the game's sync interval taken off, the tearing flag added, so a windowed swap chain is not
// held to the desktop monitor's refresh. Only the game's swap chain (state::swapchain: an overlay's own Presents are
// left alone), made with ALLOW_TEARING and not in full screen (checked once per swap chain and after each
// ResizeBuffers); a Present that refuses it is made again without, and the flag not added again until ResizeBuffers.
std::atomic<uint32_t> g_game_present{0xffffffffu};  // the game's last Present: sync interval << 16 | its flags' low 16 bits
std::atomic<uint64_t> g_tearing_added{0};
std::atomic<int> g_tearing_ok{-1};  // -1 not checked, 0 no, 1 yes
std::atomic<bool> g_tearing_cfg{false};
IDXGISwapChain* g_tearing_sc = nullptr;  // (the presenting thread's)

bool tearing_allowed(IDXGISwapChain* sc) {
    if (!g_tearing_cfg.load(std::memory_order_relaxed)) return false;
    int ok = g_tearing_ok.load(std::memory_order_relaxed);
    if (ok >= 0 && sc == g_tearing_sc) return ok == 1;
    g_tearing_sc = sc;
    DXGI_SWAP_CHAIN_DESC d{};
    BOOL full = FALSE;
    const bool flag = SUCCEEDED(sc->GetDesc(&d)) && (d.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
    const bool windowed = SUCCEEDED(sc->GetFullscreenState(&full, nullptr)) && !full;
    ok = flag && windowed ? 1 : 0;
    g_tearing_ok.store(ok, std::memory_order_relaxed);
    log::info("[d3d] the tearing flag with the game's vsync taken off: %s (the swap chain %s ALLOW_TEARING, %s)", ok ? "yes" : "no",
              flag ? "made with" : "without", windowed ? "windowed" : "full screen");
    return ok == 1;
}

// The game's sync interval taken off ([XR] Pacing) and the tearing flag added; true when the flag was added
bool unsync(IDXGISwapChain* sc, UINT& sync, UINT& flags) {
    const bool game = sc == state::swapchain.load(std::memory_order_relaxed);
    if (game) g_game_present.store(sync << 16 | (flags & 0xffffu), std::memory_order_relaxed);
    if (!sync || !g_present_unsynced.load(std::memory_order_relaxed)) return false;
    sync = 0;
    g_unsynced_presents.fetch_add(1, std::memory_order_relaxed);
    if (!game || (flags & DXGI_PRESENT_ALLOW_TEARING) || !tearing_allowed(sc)) return false;
    flags |= DXGI_PRESENT_ALLOW_TEARING;
    g_tearing_added.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void tearing_refused(HRESULT hr) {
    g_tearing_ok.store(0, std::memory_order_relaxed);  // (until ResizeBuffers checks again: a switch to full screen)
    log::limited("d3d.tearing.refused", 8, "[d3d] Present refused the tearing flag (%#lx): made again without it, not added again until "
                 "the next ResizeBuffers", static_cast<unsigned long>(hr));
}

void STDMETHODCALLTYPE hk_OMSetRenderTargets(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts,
                                             BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
    if (nested(kNBind)) return o_OMSetRenderTargets(cl, n, rts, single, ds);  // (the debug layer's forward)
    if (prepass::cut_pending()) prepass::cut_poll(cl);  // ([Stereo] PrepassWalkCut: eye 2's handover first)
    if (dl_on()) {
        dl_dsv(cl, ds, false, "OMSetRenderTargets", "");  // (r117 DG4's depthlog: the DSV bound)
        t_dl_bind_cl = cl;
        t_dl_bind_ds = ds ? ds->ptr : 0;
    }
    for (auto& slot : g_bind_tap)
        if (BindTapFn tap = slot.load(std::memory_order_relaxed)) tap(cl, n, rts, single, ds);
    if (g_fg_armed.load(std::memory_order_relaxed)) {
        std::string t;
        {
            std::lock_guard lock(g_names_mutex);
            char buf[32];
            for (UINT i = 0; i < n && rts; ++i) {
                SIZE_T p = single ? rts[0].ptr + 0 : rts[i].ptr;  // single-handle form: contiguous range (approximate)
                if (i) t += ", ";
                t += res_or_hex(rtv_resource(p), buf, sizeof(buf));
            }
            if (ds) {
                t += n ? " | DS " : "DS ";
                t += res_or_hex(rtv_resource(ds->ptr), buf, sizeof(buf));
            }
        }
        std::lock_guard lock(g_fg_mutex);
        ListLog& l = g_fg_lists[cl];
        fg_flush_segment(l);
        l.targets = t.empty() ? "none" : t;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE pp_rts[8], pp_ds{};
    if (prepass::tracing()) prepass::trace_bind(cl, n, rts, single, ds);
    if (prepass::on() && !prepass::active()) prepass::note_bind(cl, n, rts, single, ds);  // (bound before the pre-pass's clear)
    if (prepass::active() && prepass::bind(cl, n, rts, single, ds, pp_rts, &pp_ds)) {  // the pre-pass's targets: its arrays
        rts = pp_rts;
        if (ds) ds = &pp_ds;
    }
    // ([Stereo] PrepassEye2Velocity, test only) the second slot sees every bind of at most 8 targets; it substitutes only n == 1
    if (RtvSubstFn sub2 = g_rtv_subst2.load(std::memory_order_relaxed); sub2 && n <= 8) {
        D3D12_CPU_DESCRIPTOR_HANDLE repl1[1];
        if (sub2(cl, n, rts, ds, &repl1[0]) && n == 1 && rts) {
            if (main_inst::noting()) main_inst::note_bind(cl, n, repl1, single, ds);  // ([Stereo] MainInstanced: the bind as made)
            InOriginal in(kNBind);
            o_OMSetRenderTargets(cl, n, repl1, single, ds);
            return;
        }
    }
    if (RtvSubstFn sub = g_rtv_subst.load(std::memory_order_relaxed); sub && n >= 1 && n <= 8 && rts && !single) {
        D3D12_CPU_DESCRIPTOR_HANDLE repl[8];
        if (sub(cl, n, rts, ds, &repl[0])) {
            for (UINT i = 1; i < n; ++i) repl[i] = rts[i];
            if (main_inst::noting()) main_inst::note_bind(cl, n, repl, single, ds);  // ([Stereo] MainInstanced: the bind as made)
            InOriginal in(kNBind);
            o_OMSetRenderTargets(cl, n, repl, single, ds);
            return;
        }
    }
    if (main_inst::noting()) main_inst::note_bind(cl, n, rts, single, ds);  // ([Stereo] MainInstanced, test only: the list's last bind)
    InOriginal in(kNBind);
    o_OMSetRenderTargets(cl, n, rts, single, ds);
}

thread_local uint64_t t_draws = 0, t_queries = 0;
thread_local bool t_predicated = false;  // (thread_predicated: the last SetPredication on this thread's list had a buffer)
thread_local int t_open_queries = 0;     // ([Stereo] MainSlice: thread_open_queries; 0 at its list's Reset)
thread_local ID3D12GraphicsCommandList* t_q_cl = nullptr;  // (the delta review) the list the open count belongs to
std::atomic<bool> g_bq_hooked{false};
// ([Stereo] MainPredication, GOAL-single-pass item 6d; test only) the predicate this thread last passed on for its list (after
// the mod's own null substitution): kept only while g_pd_track (set once at main_inst's install). cl null: unknown
std::atomic<bool> g_pd_track{false};
struct PdRec {
    ID3D12GraphicsCommandList* cl;
    ID3D12Resource* res;
    UINT64 off;
    D3D12_PREDICATION_OP op;
    bool here;  // (the 6d/6e review's should-fix 1) the list's Reset was seen on this thread: else never trusted (another thread's)
};
thread_local PdRec t_pd{nullptr, nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO, false};

void fg_draw(ID3D12GraphicsCommandList* cl) {
    if (!g_fg_armed.load(std::memory_order_relaxed)) return;
    uint64_t vs = 0, ps = 0;
    uint32_t w = 0;
    const bool known = pso::current(cl, &vs, &ps, &w);
    fg_count(cl, [&](ListLog& l) {
        ++l.draws;
        ++l.shaders[{known ? vs : 0, known ? ps : 0, known ? w : 0xfffu}];
    });
}

void STDMETHODCALLTYPE hk_DrawInstanced(ID3D12GraphicsCommandList* cl, UINT a, UINT b, UINT c, UINT d) {
    if (nested(kNDraw)) return o_DrawInstanced(cl, a, b, c, d);
    if (prepass::cut_pending()) prepass::cut_poll(cl);  // ([Stereo] PrepassWalkCut: eye 2's handover first)
    if (pso::glow::t_on && pso::glow::foreign(pso::glow::kDropNonIndexed)) return;  // ([Holsters] InteractionHighlights=parts: never the extra draw)
    if (prepass::active() && prepass::before_draw(cl, a, b)) return;  // ([Stereo] PrepassInstanced: drawn in pass 1 for both eyes)
    if (pso::on_draw(cl)) return;
    if (gpu_stages::walk_on() && gpu_stages::walk_draw(cl)) return;  // ([Stereo] PrepassGpuStages' walk census, item 8b, test only)
    ++t_draws;
    fg_draw(cl);
    pso::on_draw_args(cl, c, a);
    // ([Stereo] MainInstanced, test only) a listed draw in a main region: pass 1's seeds before the game's draw, the
    // duplicates into the mod's own arrays after it (the game's draw itself unchanged). ([Stereo] MainSlice, run 25 item 5)
    // pass 2's listed draws of a handed-over frame: merged at the first, then dropped (t_draws counted above, as in pass 1)
    const int mi = main_inst::listening() ? main_inst::before_draw(cl, DrawArgs{false, a, b, c, 0, d}) : main_inst::kNone;
    if (mi == main_inst::kDrop) return;
    if (dl_on()) dl_draw(cl, "DrawInstanced (the game's)", true, false, 0);  // (r117 DG4's depthlog: a draw that reaches the original)
    {
        InOriginal in(kNDraw);
        o_DrawInstanced(cl, a, b, c, d);
    }
    if (mi == main_inst::kDuplicate) main_inst::after_draw(cl, DrawArgs{false, a, b, c, 0, d});
}
void STDMETHODCALLTYPE hk_DrawIndexedInstanced(ID3D12GraphicsCommandList* cl, UINT a, UINT b, UINT c, INT d, UINT e) {
    if (nested(kNDrawIndexed)) return o_DrawIndexedInstanced(cl, a, b, c, d, e);
    if (prepass::cut_pending()) prepass::cut_poll(cl);  // ([Stereo] PrepassWalkCut: eye 2's handover first)
    // ([Holsters] InteractionHighlights=parts, run 29 item 2) between the part glow's markers: the part's runs drawn with the
    // current pipeline's glow twin, anything else of the extra draw dropped (pso.h glow); none of the pre-pass's or the
    // main region's work sees these draws. Off: one thread-local read
    if (pso::glow::t_on && pso::glow::draw_indexed(cl, a, b, c, d, e, o_DrawIndexedInstanced)) return;
    if (prepass::active() && prepass::before_draw(cl, a, b)) return;
    if (pso::on_draw(cl)) return;
    if (gpu_stages::walk_on() && gpu_stages::walk_draw(cl)) return;  // ([Stereo] PrepassGpuStages' walk census, item 8b, test only)
    ++t_draws;
    fg_draw(cl);
    pso::on_draw_args(cl, d, a);
    // ([Stereo] MainInstanced / MainSlice, test only: as hk_DrawInstanced)
    const int mi = main_inst::listening() ? main_inst::before_draw(cl, DrawArgs{true, a, b, c, d, e}) : main_inst::kNone;
    if (mi == main_inst::kDrop) return;
    if (dl_on()) dl_draw(cl, "DrawIndexedInstanced (the game's)", true, false, 0);  // (r117 DG4's depthlog: a draw that reaches the original)
    {
        InOriginal in(kNDrawIndexed);
        o_DrawIndexedInstanced(cl, a, b, c, d, e);
    }
    if (mi == main_inst::kDuplicate) main_inst::after_draw(cl, DrawArgs{true, a, b, c, d, e});
}
void STDMETHODCALLTYPE hk_Dispatch(ID3D12GraphicsCommandList* cl, UINT x, UINT y, UINT z) {
    if (nested(kNDispatch)) return o_Dispatch(cl, x, y, z);
    if (prepass::cut_pending()) prepass::cut_poll(cl);  // ([Stereo] PrepassWalkCut: eye 2's handover first)
    if (dl_on()) dl_draw(cl, "Dispatch", false, false, 0);  // (r117 DG4's depthlog: counted in the second ordinal only)
    fg_count(cl, [](ListLog& l) { ++l.dispatches; });
    InOriginal in(kNDispatch);
    o_Dispatch(cl, x, y, z);
}
void STDMETHODCALLTYPE hk_ExecuteIndirect(ID3D12GraphicsCommandList* cl, ID3D12CommandSignature* s, UINT n, ID3D12Resource* a,
                                          UINT64 b, ID3D12Resource* c, UINT64 d) {
    if (nested(kNIndirect)) return o_ExecuteIndirect(cl, s, n, a, b, c, d);
    if (prepass::cut_pending()) prepass::cut_poll(cl);  // ([Stereo] PrepassWalkCut: eye 2's handover first)
    if (pso::glow::t_on && pso::glow::foreign(pso::glow::kDropIndirect)) return;  // ([Holsters] InteractionHighlights=parts: never the extra draw)
    if (dl_on()) dl_draw(cl, "ExecuteIndirect (the game's)", true, false, 0);  // (r117 DG4's depthlog)
    fg_count(cl, [](ListLog& l) { ++l.indirect; });
    if (prepass::active()) prepass::before_indirect(cl);
    if (main_inst::listening()) main_inst::indirect(cl);  // ([Stereo] MainRegion, item 8d: an indirect in e1 is a single; off: one load)
    InOriginal in(kNIndirect);
    o_ExecuteIndirect(cl, s, n, a, b, c, d);
}
// ---- barrier watch (d3d::watch_add)
constexpr int kWatchSlots = 4;
std::mutex g_watch_mutex;
std::atomic<const void*> g_watch_res[kWatchSlots];
std::atomic<int> g_watch_count{0};
struct Watch {
    ID3D12GraphicsCommandList* cl = nullptr;
    uint32_t state = 0;
    DWORD tid = 0;
    bool valid = false;
    int refs = 0;
    uint64_t writes = 0;  // the game's transitions into a state that can write it, any list or thread (watched_writes)
};
Watch g_watch[kWatchSlots];
constexpr uint32_t kWriteStates = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
                                  D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST;

void watch_barriers(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_RESOURCE_BARRIER* b) {
    if (g_watch_count.load(std::memory_order_relaxed) == 0) return;
    for (UINT i = 0; i < n; ++i) {
        if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
        for (int w = 0; w < kWatchSlots; ++w) {
            if (b[i].Transition.pResource != g_watch_res[w].load(std::memory_order_relaxed)) continue;
            std::lock_guard lock(g_watch_mutex);
            Watch& x = g_watch[w];
            x.cl = cl;
            x.state = static_cast<uint32_t>(b[i].Transition.StateAfter);
            x.tid = GetCurrentThreadId();
            if (x.state & kWriteStates) ++x.writes;
            // A split barrier, or one subresource of several, leaves the whole resource's state unknown.
            x.valid = b[i].Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE &&
                      (b[i].Transition.Subresource == 0 ||
                       b[i].Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
        }
    }
}

void watch_list_ends(ID3D12GraphicsCommandList* cl) {
    if (g_watch_count.load(std::memory_order_relaxed) == 0) return;
    std::lock_guard lock(g_watch_mutex);
    for (Watch& x : g_watch)
        if (x.cl == cl) x.valid = false;
}

void STDMETHODCALLTYPE hk_ResourceBarrier(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_RESOURCE_BARRIER* b) {
    if (nested(kNBarrier)) return o_ResourceBarrier(cl, n, b);  // (the debug layer's forward: followed once, at the outer call)
    if (dl_on()) dl_barriers(cl, n, b, false);  // (r117 DG4's depthlog, test only)
    if (main_inst::slice_installed() && main_inst::listening()) main_inst::pre_barrier(cl, n, b);  // ([Stereo] MainRegionE1Compare's remap, test only)
    if (prepass::tracing()) prepass::trace_barrier(cl, n, b);
    if (prepass::active()) prepass::barrier(cl, n, b);  // (before the game's batch: the pre-pass copies)
    else if (prepass::cut_tracking()) prepass::cut_track(cl, n, b);  // ([Stereo] PrepassWalkCut: the handover, then followed)
    fg_count(cl, [n](ListLog& l) { l.barriers += n; });
    watch_barriers(cl, n, b);
    if (main_inst::slice_installed() && main_inst::listening()) main_inst::barrier(cl, n, b);  // ([Stereo] MainSlice, test only: the family's targets inside a handed-over run)
    InOriginal in(kNBarrier);
    o_ResourceBarrier(cl, n, b);
}
// ---- the mod's view-instance masks per list (d3d_hooks.h note_view_instance_mask; [Stereo] PrepassInstanced and MainInstanced)
std::mutex g_vim_mutex;
std::atomic<int> g_vim_count{0};
struct Vim {
    ID3D12GraphicsCommandList* cl;
    unsigned mask;
};
Vim g_vim[16] = {};
void vim_list_ends(ID3D12GraphicsCommandList* cl) {
    if (g_vim_count.load(std::memory_order_relaxed) == 0) return;  // (off: one load)
    std::lock_guard lock(g_vim_mutex);
    for (Vim& v : g_vim)
        if (v.cl == cl) {
            v.cl = nullptr;
            g_vim_count.fetch_sub(1, std::memory_order_relaxed);
        }
}
HRESULT STDMETHODCALLTYPE hk_Close(ID3D12GraphicsCommandList* cl) {
    if (nested(kNClose)) return o_Close(cl);
    watch_list_ends(cl);
    vim_list_ends(cl);
    if (t_rs.cl == cl) t_rs = RasterNote{};  // ([Stereo] MainRegionHandover: the list's raster note dropped)
    if (t_rn.cl == cl) t_rn = RootNote{};  // (and its root note)
    prepass::follow_list_ends(cl);  // (r117 DG4: the follow's states set on this list no longer this list's)
    if (main_inst::noting()) {
        main_inst::list_closing(cl);  // (GOAL-single-pass item 6c: SC2L and LB2 lent on this list given back before it closes)
        main_inst::list_ends(cl);     // ([Stereo] MainInstanced: the list's notes dropped)
    }
    // ([Stereo] MainPredication: no longer known here; after list_closing, whose steps may record on this list and read its predicate)
    if (g_pd_track.load(std::memory_order_relaxed) && t_pd.cl == cl) t_pd.cl = nullptr;
    InOriginal in(kNClose);
    return o_Close(cl);
}
HRESULT STDMETHODCALLTYPE hk_Reset(ID3D12GraphicsCommandList* cl, ID3D12CommandAllocator* a, ID3D12PipelineState* ps) {
    if (nested(kNReset)) return o_Reset(cl, a, ps);
    t_predicated = false;
    if (g_pd_track.load(std::memory_order_relaxed)) t_pd = PdRec{cl, nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO, true};  // (6d: a reset list is unpredicated)
    if (cl == t_q_cl || t_open_queries == 0) {  // (the delta review: another list's Reset on this thread keeps the count)
        t_open_queries = 0;
        t_q_cl = nullptr;
    }
    watch_list_ends(cl);
    vim_list_ends(cl);
    if (t_rs.cl == cl) t_rs = RasterNote{};  // ([Stereo] MainRegionHandover: the list's raster note dropped)
    if (t_rn.cl == cl) t_rn = RootNote{};  // (and its root note)
    prepass::follow_list_ends(cl);  // (r117 DG4: the follow's states set on this list no longer this list's)
    if (dl_on()) dl_reset_list(cl);  // (r117 DG4's depthlog: the list's draw ordinals from 0)
    if (main_inst::noting()) main_inst::list_ends(cl);  // ([Stereo] MainInstanced: the list's notes dropped)
    pso::glow::list_reset(cl, ps);  // ([Holsters] InteractionHighlights=parts: the list's pipeline the glow puts back)
    InOriginal in(kNReset);
    return o_Reset(cl, a, ps);
}
// ([Stereo] MainSlice, test only; hooked only with MainSlice=1 and MainInstanced=1) the queries that count draws, open on this
// thread's list: a draw dropped inside one would change the game's result unseen (main_instanced's handover refuses then)
void STDMETHODCALLTYPE hk_BeginQuery(ID3D12GraphicsCommandList* cl, ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT i) {
    if (nested(kNBeginQuery)) return o_BeginQuery(cl, h, t, i);
    if (t != D3D12_QUERY_TYPE_TIMESTAMP) {
        if (t_open_queries > 0 && t_q_cl != cl) t_q_cl = nullptr;  // (two lists' queries open on one thread: unknown, refused)
        else t_q_cl = cl;
        ++t_open_queries;
        if (main_inst::slice_installed() && main_inst::listening()) main_inst::query(cl, true);
    }
    InOriginal in(kNBeginQuery);
    o_BeginQuery(cl, h, t, i);
}
void STDMETHODCALLTYPE hk_EndQuery(ID3D12GraphicsCommandList* cl, ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT i) {
    if (nested(kNEndQuery)) return o_EndQuery(cl, h, t, i);
    if (t != D3D12_QUERY_TYPE_TIMESTAMP && g_bq_hooked.load(std::memory_order_relaxed)) {  // ([Stereo] MainSlice only)
        if (t_open_queries > 0) --t_open_queries;
        if (main_inst::slice_installed() && main_inst::listening()) main_inst::query(cl, false);
    }
    ++t_queries;
    fg_count(cl, [](ListLog& l) { ++l.queries; });
    InOriginal in(kNEndQuery);
    o_EndQuery(cl, h, t, i);
}
void STDMETHODCALLTYPE hk_CopyResource(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, ID3D12Resource* src) {
    if (nested(kNCopy)) return o_CopyResource(cl, dst, src);  // (no copy taps: the outer call runs them)
    if (prepass::cut_pending()) prepass::cut_poll(cl);  // ([Stereo] PrepassWalkCut: eye 2's handover first)
    if (g_fg_armed.load(std::memory_order_relaxed)) {
        char a[32], b[32];
        std::string d, s;
        {
            std::lock_guard lock(g_names_mutex);
            d = res_or_hex(dst, a, sizeof(a));
            s = res_or_hex(src, b, sizeof(b));
        }
        fg_event(cl, "CopyResource %s <- %s", d.c_str(), s.c_str());
    }
    if (gpu_stages::walk_on()) gpu_stages::walk_copy(cl, dst);    // ([Stereo] PrepassGpuStages' walk census, item 8b: segment e's close)
    if (dl_on()) dl_copy(cl, dst, src, -1, -1, false, "CopyResource");  // (r117 DG4's depthlog)
    if (main_inst::slice_installed() && main_inst::listening()) main_inst::copy_write(cl, dst, src);  // ([Stereo] MainRegionE1Compare, test only: T to C2)
    if (main_inst::listening()) main_inst::before_copy(cl, dst);  // ([Stereo] MainInstanced, test only: the Depth Resolve close)
    if (cut_velocity::fsc_listening()) cut_velocity::fsc_before_copy(cl, dst);  // (item 8f0, test only: the FSC probe's slot)
    {
        InOriginal in(kNCopy);
        o_CopyResource(cl, dst, src);
    }
    for (auto& t : g_copy_tap)
        if (CopyTapFn tap = t.load(std::memory_order_relaxed)) tap(cl, dst, src);
}
void STDMETHODCALLTYPE hk_CopyTextureRegion(ID3D12GraphicsCommandList* cl, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y,
                                            UINT z, const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    if (nested(kNCopyRegion)) return o_CopyTextureRegion(cl, dst, x, y, z, src, box);
    if (prepass::cut_pending()) prepass::cut_poll(cl);  // ([Stereo] PrepassWalkCut: eye 2's handover first)
    if (g_fg_armed.load(std::memory_order_relaxed)) {
        char a[32], b[32];
        std::string d, s;
        {
            std::lock_guard lock(g_names_mutex);
            d = res_or_hex(dst->pResource, a, sizeof(a));
            s = res_or_hex(src->pResource, b, sizeof(b));
        }
        fg_event(cl, "CopyTextureRegion %s <- %s", d.c_str(), s.c_str());
    }
    if (dl_on() && dst && src) dl_copy(cl, dst->pResource, src->pResource, static_cast<int>(dst->SubresourceIndex), static_cast<int>(src->SubresourceIndex), false, "CopyTextureRegion");
    if (main_inst::slice_installed() && main_inst::listening() && dst) main_inst::copy_write(cl, dst->pResource, src ? src->pResource : nullptr);  // ([Stereo] MainRegionE1Compare, test only)
    InOriginal in(kNCopyRegion);
    o_CopyTextureRegion(cl, dst, x, y, z, src, box);
}
void STDMETHODCALLTYPE hk_ClearRTV(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT c[4], UINT n,
                                   const D3D12_RECT* r) {
    if (nested(kNClearRtv)) return o_ClearRTV(cl, h, c, n, r);
    if (prepass::cut_pending()) prepass::cut_poll(cl);  // ([Stereo] PrepassWalkCut: eye 2's handover first)
    const D3D12_CPU_DESCRIPTOR_HANDLE h_game = h;       // ([Stereo] PrepassGpuStages: the game's view, before the substitution)
    if (main_inst::slice_installed() && main_inst::listening()) main_inst::clear(cl, h_game.ptr);  // ([Stereo] MainRegionE1Compare, test only: T to C2)
    if (g_fg_armed.load(std::memory_order_relaxed)) {
        char a[32];
        std::string name;
        {
            std::lock_guard lock(g_names_mutex);
            name = res_or_hex(rtv_resource(h.ptr), a, sizeof(a));
        }
        fg_event(cl, "ClearRTV %s (%.2f %.2f %.2f %.2f)", name.c_str(), c[0], c[1], c[2], c[3]);
    }
    if (prepass::tracing()) prepass::trace_clear(cl, h, false);
    if (prepass::active()) {
        h = prepass::clear_rtv(cl, h);
        if (prepass::take_skip_clear()) return;  // (pass 2's, instanced: its slice was cleared and drawn in pass 1)
    }
    const int gs = gpu_stages::on() ? gpu_stages::clear_begin(cl, h_game, false) : -1;  // ([Stereo] PrepassGpuStages, test only)
    InOriginal in(kNClearRtv);
    o_ClearRTV(cl, h, c, n, r);
    if (gs >= 0) gpu_stages::clear_end(cl, gs);
}
void STDMETHODCALLTYPE hk_ClearDSV(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE h, D3D12_CLEAR_FLAGS f, FLOAT d,
                                   UINT8 s, UINT n, const D3D12_RECT* r) {
    if (nested(kNClearDsv)) return o_ClearDSV(cl, h, f, d, s, n, r);
    if (prepass::cut_pending()) prepass::cut_poll(cl);  // ([Stereo] PrepassWalkCut: eye 2's handover first)
    if (dl_on()) dl_dsv(cl, &h, false, "ClearDepthStencilView", "");  // (r117 DG4's depthlog)
    const D3D12_CPU_DESCRIPTOR_HANDLE h_game = h;       // ([Stereo] PrepassGpuStages: a pass's pre-pass starts at this clear)
    if (main_inst::slice_installed() && main_inst::listening()) main_inst::clear(cl, h_game.ptr);  // ([Stereo] MainRegionE1Compare, test only: T to C2)
    if (gpu_stages::on()) gpu_stages::depth_clear_seen(cl, h_game);
    if (g_fg_armed.load(std::memory_order_relaxed)) {
        char a[32];
        std::string name;
        {
            std::lock_guard lock(g_names_mutex);
            name = res_or_hex(rtv_resource(h.ptr), a, sizeof(a));
        }
        fg_event(cl, "ClearDSV %s depth %.3f", name.c_str(), d);
    }
    if (prepass::tracing()) prepass::trace_clear(cl, h, true);
    if (prepass::on()) {
        h = prepass::clear_dsv(cl, h);  // (the pre-pass's start: its depth's clear)
        if (prepass::take_skip_clear()) return;
    }
    const int gs = gpu_stages::on() ? gpu_stages::clear_begin(cl, h_game, true) : -1;  // ([Stereo] PrepassGpuStages, test only)
    InOriginal in(kNClearDsv);
    o_ClearDSV(cl, h, f, d, s, n, r);
    if (gs >= 0) gpu_stages::clear_end(cl, gs);
}
void STDMETHODCALLTYPE hk_BeginEvent(ID3D12GraphicsCommandList* cl, UINT meta, const void* data, UINT size) {
    if (nested(kNBeginEvent)) return o_BeginEvent(cl, meta, data, size);
    if (g_fg_armed.load(std::memory_order_relaxed) && data && size) {
        // PIX metadata 0 = Unicode string, 1 = ANSI string; PIX3 events are encoded (logged as size only).
        if (meta == 1) fg_event(cl, "BeginEvent \"%.*s\"", static_cast<int>(size), static_cast<const char*>(data));
        else if (meta == 0) fg_event(cl, "BeginEvent \"%ls\"", static_cast<const wchar_t*>(data));
        else fg_event(cl, "BeginEvent meta%u size%u", meta, size);
    }
    InOriginal in(kNBeginEvent);
    o_BeginEvent(cl, meta, data, size);
}
void STDMETHODCALLTYPE hk_SetPredication(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, UINT64 off, D3D12_PREDICATION_OP op) {
    if (nested(kNPredication)) return o_SetPredication(cl, r, off, op);
    fg_count(cl, [](ListLog& l) { ++l.queries; });
    if (r && prepass::predication_off()) {  // ([Stereo] PrepassInstanced: pass 2's queries count the dropped draws as nothing)
        r = nullptr;
        off = 0;
    }
    t_predicated = r != nullptr;
    if (g_pd_track.load(std::memory_order_relaxed))  // ([Stereo] MainPredication: what the list now has; trusted only after its Reset here)
        t_pd = PdRec{cl, r, off, op, t_pd.cl == cl && t_pd.here};
    if (main_inst::slice_installed()) main_inst::note_predication(cl, r, off, static_cast<int>(op));  // ([Stereo] MainRegionE1Compare's remap, test only)
    InOriginal in(kNPredication);
    o_SetPredication(cl, r, off, op);
}

void fg_on_execute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (!g_fg_armed.load(std::memory_order_relaxed)) return;
    D3D12_COMMAND_LIST_TYPE t = queue_type(q);
    std::lock_guard lock(g_fg_mutex);
    char hdr[160];
    std::snprintf(hdr, sizeof(hdr), "  ECL queue %p (%s) %u list(s)\n", static_cast<void*>(q),
                  t == D3D12_COMMAND_LIST_TYPE_DIRECT ? "direct" : t == D3D12_COMMAND_LIST_TYPE_COMPUTE ? "compute"
                                                                 : t == D3D12_COMMAND_LIST_TYPE_COPY  ? "copy"
                                                                                                      : "other",
                  n);
    g_fg_text += hdr;
    for (UINT i = 0; i < n; ++i) {
        auto it = g_fg_lists.find(lists[i]);
        char lh[64];
        std::snprintf(lh, sizeof(lh), "   list %p\n", static_cast<const void*>(lists[i]));
        g_fg_text += lh;
        if (it != g_fg_lists.end()) {
            fg_flush_segment(it->second);
            g_fg_text += it->second.text;
            it->second.text.clear();
        }
    }
}

void fg_on_frame_end(uint64_t frame) {
    if (!g_fg_armed.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_fg_mutex);
    char line[64];
    std::snprintf(line, sizeof(line), "==== Present (frame %llu)\n", static_cast<unsigned long long>(frame));
    g_fg_text += line;
    if (--g_fg_frames_left <= 0) {
        g_fg_armed.store(false);
        wchar_t name[64], path[MAX_PATH];
        std::swprintf(name, 64, L"RDRVR_framegraph_%d.txt", g_fg_file_index++);
        log::path_in_game_dir(name, path, MAX_PATH);
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"wb") == 0 && f) {
            std::fwrite(g_fg_text.data(), 1, g_fg_text.size(), f);
            std::fclose(f);
        }
        std::snprintf(g_fg_status, sizeof(g_fg_status), "wrote %ls (%zu bytes, frames from %llu)", name, g_fg_text.size(),
                      static_cast<unsigned long long>(g_fg_frame0));
        log::info("[fg] %s", g_fg_status);
        g_fg_text.clear();
        g_fg_lists.clear();
    }
}

// ---------------------------------------------------------------------------------- device & resources
HRESULT STDMETHODCALLTYPE hk_CreateCommittedResource(ID3D12Device* dev, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
                                                     const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES st,
                                                     const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out) {
    HRESULT hr = o_CreateCommittedResource(dev, hp, hf, d, st, cv, riid, out);
    if (SUCCEEDED(hr) && out && *out && hp && hp->Type == D3D12_HEAP_TYPE_UPLOAD && ring_probe::tracking()) {
        ID3D12Resource* r = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&r))) && r) {
            ring_probe::on_upload_buffer(r, d);
            r->Release();
        }
    }
    if (SUCCEEDED(hr) && d && (d->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))) {
        char desc[160];
        describe_desc(d, desc, sizeof(desc));
        log::limited("d3d.committed.rt", 400, "[res] committed %p %s", out ? *out : nullptr, desc);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreatePlacedResource(ID3D12Device* dev, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC* d,
                                                  D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out) {
    HRESULT hr = o_CreatePlacedResource(dev, heap, off, d, st, cv, riid, out);
    if (SUCCEEDED(hr) && out && *out && heap && ring_probe::tracking() && heap->GetDesc().Properties.Type == D3D12_HEAP_TYPE_UPLOAD) {
        ID3D12Resource* r = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&r))) && r) {
            ring_probe::on_upload_buffer(r, d);
            r->Release();
        }
    }
    if (SUCCEEDED(hr) && d && (d->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))) {
        char desc[160];
        describe_desc(d, desc, sizeof(desc));
        log::limited("d3d.placed.rt", 400, "[res] placed %p heap %p+%#llx %s", out ? *out : nullptr, static_cast<void*>(heap),
                     static_cast<unsigned long long>(off), desc);
    }
    return hr;
}

void STDMETHODCALLTYPE hk_CreateRTV(ID3D12Device* dev, ID3D12Resource* res, const void* desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    {
        std::lock_guard lock(g_names_mutex);
        if (g_rtv.size() < 2000000) {
            g_rtv[h.ptr] = res;
            g_view_fmt[h.ptr] = desc ? *static_cast<const DXGI_FORMAT*>(desc) : DXGI_FORMAT_UNKNOWN;  // (Format is the first member)
        }
    }
    o_CreateRTV(dev, res, desc, h);
}

void STDMETHODCALLTYPE hk_CreateDSV(ID3D12Device* dev, ID3D12Resource* res, const void* desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    {
        std::lock_guard lock(g_names_mutex);
        if (g_rtv.size() < 2000000) {
            g_rtv[h.ptr] = res;
            g_view_fmt[h.ptr] = desc ? *static_cast<const DXGI_FORMAT*>(desc) : DXGI_FORMAT_UNKNOWN;
        }
    }
    if (g_dsv_flags_on) {  // (r117 DG4's depthlog: the view's read-only flags; test inis only)
        std::lock_guard lock(g_dsv_flags_mutex);
        if (g_dsv_flags.size() < 2000000) g_dsv_flags[h.ptr] = desc ? static_cast<uint32_t>(static_cast<const D3D12_DEPTH_STENCIL_VIEW_DESC*>(desc)->Flags) : 0u;
    }
    o_CreateDSV(dev, res, desc, h);
}

HRESULT STDMETHODCALLTYPE hk_ResourceSetName(ID3D12Object* obj, LPCWSTR name) {
    std::string n = narrow(name);
    // Describe it, so the log ties names to sizes and formats. Render targets, depth buffers and UAV targets are
    // always logged (Spike S3 needs all of them); streamed textures and buffers only for the first 3000 names.
    ID3D12Resource* r = nullptr;
    char desc[160] = "";
    bool target = false;
    if (SUCCEEDED(obj->QueryInterface(IID_PPV_ARGS(&r))) && r) {
        D3D12_RESOURCE_DESC d = r->GetDesc();
        describe_desc(&d, desc, sizeof(desc));
        target = (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) != 0;
        r->Release();
        // the frame's own targets: the live-resize guard's first sign (the game names them before its ResizeBuffers).
        // Not FXAATarget: [XR] EyeShape's sizes make the game re-make it at the eye's size (a false resize in the
        // simulator, 2026-10-08); a real resize re-makes the Post FXAA Target and the back buffer too
        if (FrameSizeFn fs = g_frame_size.load(std::memory_order_acquire);
            fs && d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && (n == "Post FXAA Target" || n == "Main Backbuffer"))
            fs(n.c_str(), static_cast<uint32_t>(d.Width), d.Height);
    }
    bool log_it = false;
    {
        std::lock_guard lock(g_names_mutex);
        g_names[obj] = n;
        log_it = target || ++g_names_logged <= 3000;
    }
    if (log_it) log::info("[res] SetName %p \"%s\" %s", static_cast<void*>(obj), n.c_str(), desc);
    return o_ResourceSetName(obj, name);
}


// ------------------------------------------------------------------------------------------- queues
void STDMETHODCALLTYPE hk_ECL(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (t_in_ecl) {  // our own submissions (GPU timer) pass straight through
        o_ECL(q, n, lists);
        return;
    }
    ++t_in_ecl;
    D3D12_COMMAND_LIST_TYPE t = queue_type(q);
    t_ecl_calls.fetch_add(1, std::memory_order_relaxed);
    t_ecl_lists.fetch_add(n, std::memory_order_relaxed);
    if (t == D3D12_COMMAND_LIST_TYPE_DIRECT) t_ecl_direct.fetch_add(n, std::memory_order_relaxed);
    else if (t == D3D12_COMMAND_LIST_TYPE_COMPUTE) t_ecl_compute.fetch_add(n, std::memory_order_relaxed);
    else if (t == D3D12_COMMAND_LIST_TYPE_COPY) t_ecl_copy.fetch_add(n, std::memory_order_relaxed);
    bool present_queue = q == state::present_queue.load(std::memory_order_relaxed);
    if (present_queue) {
        t_ecl_present.fetch_add(1, std::memory_order_relaxed);
        if (!g_frame_started.exchange(true)) {
            int c = g_start_count.load();
            for (int i = 0; i < c; ++i) g_start_listeners[i](q);
        }
    }
    fg_on_execute(q, n, lists);
    ID3D12CommandList* before = nullptr;
    ID3D12CommandList* after = nullptr;
    EclBracketFn bracket = g_bracket.load();
    if (present_queue && bracket && bracket(q, &before, &after)) {
        // One submission: [before, the game's lists..., after], so nothing else on the queue lands in between.
        ID3D12CommandList* all[66];
        if (n + 2 <= 66) {
            all[0] = before;
            for (UINT i = 0; i < n; ++i) all[i + 1] = lists[i];
            all[n + 1] = after;
            o_ECL(q, n + 2, all);
        } else {
            o_ECL(q, 1, &before);
            o_ECL(q, n, lists);
            o_ECL(q, 1, &after);
        }
    } else {
        o_ECL(q, n, lists);
    }
    --t_in_ecl;
}

HRESULT STDMETHODCALLTYPE hk_CreateCommandQueue(ID3D12Device* dev, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid, void** out) {
    HRESULT hr = o_CreateCommandQueue(dev, desc, riid, out);
    if (SUCCEEDED(hr) && out && *out && desc) {
        if (desc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT) state::queues_direct++;
        else if (desc->Type == D3D12_COMMAND_LIST_TYPE_COMPUTE) state::queues_compute++;
        else if (desc->Type == D3D12_COMMAND_LIST_TYPE_COPY) state::queues_copy++;
        log::info("[d3d] CreateCommandQueue %p type %d priority %d flags %#x", *out, static_cast<int>(desc->Type),
                  desc->Priority, static_cast<unsigned>(desc->Flags));
        ID3D12CommandQueue* q = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&q))) && q) {
            std::call_once(g_queue_once, [q] {
                hooks::install("ID3D12CommandQueue::ExecuteCommandLists", hooks::vtable_entry(q, 10), hk_ECL, &o_ECL);
            });
            q->Release();
        }
    }
    return hr;
}

void hook_command_list_methods(ID3D12Device* dev) {
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl)))) {
        log::error("[d3d] frame graph: could not create a probe command list");
        if (alloc) alloc->Release();
        return;
    }
    ring_probe::install_list_hooks(cl);
    hooks::install("CL::Close", hooks::vtable_entry(cl, 9), hk_Close, &o_Close);
    hooks::install("CL::Reset", hooks::vtable_entry(cl, 10), hk_Reset, &o_Reset);
    hooks::install("CL::ResourceBarrier", hooks::vtable_entry(cl, 26), hk_ResourceBarrier, &o_ResourceBarrier);
    hooks::install("CL::OMSetRenderTargets", hooks::vtable_entry(cl, 46), hk_OMSetRenderTargets, &o_OMSetRenderTargets);
    if (!config::get_bool("Debug", "FrameGraph", true)) {
        cl->Close();
        cl->Release();
        alloc->Release();
        return;
    }
    pso::install_list_hooks(cl);
    hooks::install("CL::DrawInstanced", hooks::vtable_entry(cl, 12), hk_DrawInstanced, &o_DrawInstanced);
    hooks::install("CL::DrawIndexedInstanced", hooks::vtable_entry(cl, 13), hk_DrawIndexedInstanced, &o_DrawIndexedInstanced);
    hooks::install("CL::Dispatch", hooks::vtable_entry(cl, 14), hk_Dispatch, &o_Dispatch);
    hooks::install("CL::CopyTextureRegion", hooks::vtable_entry(cl, 16), hk_CopyTextureRegion, &o_CopyTextureRegion);
    hooks::install("CL::CopyResource", hooks::vtable_entry(cl, 17), hk_CopyResource, &o_CopyResource);
    hooks::install("CL::ClearDepthStencilView", hooks::vtable_entry(cl, 47), hk_ClearDSV, &o_ClearDSV);
    hooks::install("CL::ClearRenderTargetView", hooks::vtable_entry(cl, 48), hk_ClearRTV, &o_ClearRTV);
    hooks::install("CL::EndQuery", hooks::vtable_entry(cl, 53), hk_EndQuery, &o_EndQuery);
    // ([Stereo] MainSlice, test only; [Stereo] MainRegionHandover, item 8g: the queries open at its decision too)
    const bool ho_hooks = config::get_int("Stereo", "MainRegionHandover", 0) == 1 && config::get_int("Stereo", "MainInstanced", 0) == 1;
    if (ho_hooks) {  // (item 8g, test only: note only, so the merge can put the game's viewport, scissor and topology back)
        hooks::install("CL::IASetPrimitiveTopology", hooks::vtable_entry(cl, 20), hk_IASetPrimitiveTopology, &o_IASetPrimitiveTopology);
        hooks::install("CL::RSSetViewports", hooks::vtable_entry(cl, 21), hk_RSSetViewports, &o_RSSetViewports);
        hooks::install("CL::RSSetScissorRects", hooks::vtable_entry(cl, 22), hk_RSSetScissorRects, &o_RSSetScissorRects);
        g_rs_hooked = o_IASetPrimitiveTopology && o_RSSetViewports && o_RSSetScissorRects;
        // (the review's SF1: the merge's bindless table proven set on the list since its root signature)
        hooks::install("CL::SetGraphicsRootSignature", hooks::vtable_entry(cl, 30), hk_SetGraphicsRootSignature, &o_SetGraphicsRootSignature);
        hooks::install("CL::SetGraphicsRootDescriptorTable", hooks::vtable_entry(cl, 32), hk_SetGraphicsRootDescriptorTable, &o_SetGraphicsRootDescriptorTable);
        g_rn_hooked = o_SetGraphicsRootSignature && o_SetGraphicsRootDescriptorTable;
    }
    if ((config::get_int("Stereo", "MainSlice", 0) == 1 && config::get_int("Stereo", "MainInstanced", 0) == 1) || ho_hooks) {
        hooks::install("CL::BeginQuery", hooks::vtable_entry(cl, 52), hk_BeginQuery, &o_BeginQuery);
        g_bq_hooked = o_BeginQuery != nullptr;
    }
    hooks::install("CL::SetPredication", hooks::vtable_entry(cl, 55), hk_SetPredication, &o_SetPredication);
    hooks::install("CL::BeginEvent", hooks::vtable_entry(cl, 57), hk_BeginEvent, &o_BeginEvent);
    hooks::install("CL::ExecuteIndirect", hooks::vtable_entry(cl, 59), hk_ExecuteIndirect, &o_ExecuteIndirect);
    cl->Close();
    cl->Release();
    alloc->Release();
}

// ([Debug] PixCapturer, test only) PIX's wrapper vtables may fold identical methods into one function: a detour on it would take every
// slot's calls with the wrong method's arguments. So before any D3D12 vtable hook, every slot any module may patch (a superset: the
// conditional ones too) is resolved on probe objects (the device, Device2, a list, a resource, a queue) and compared with each other and
// with the hooks installed already (DXGI's factory); any shared target: no D3D12 vtable hook (nor the swapchain's) this session
std::atomic<bool> g_pix_collide{false};
struct PixSlot {
    const char* name;
    void* target;
};
void pix_report(const std::vector<PixSlot>& v) {
    std::string col;
    for (size_t i = 0; i < v.size(); ++i) {
        for (size_t j = i + 1; j < v.size(); ++j)
            if (v[i].target == v[j].target) col += std::string(" ") + v[i].name + "=" + v[j].name;
        if (const char* h = hooks::hooked_name(v[i].target)) col += std::string(" ") + v[i].name + "=" + h + "(installed)";
    }
    if (col.empty()) {
        log::info("[pix] %zu hook targets resolved: no two share a function", v.size());
        return;
    }
    g_pix_collide = true;
    log::error("[pix] hook targets collide:%s: D3D12 hooks not installed (the PIX session is unusable)", col.c_str());
}
bool pix_prescan(ID3D12Device* dev) {
    std::vector<PixSlot> v;
    const auto add = [&v](const char* n, void* obj, int slot) { v.push_back({n, hooks::vtable_entry(obj, slot)}); };
    static const struct {
        const char* n;
        int s;
    } kDev[] = {{"Device::CreateCommandQueue", 8},       {"Device::CreateGraphicsPipelineState", 10}, {"Device::CreateDescriptorHeap", 14},
                {"Device::CreateRootSignature", 16},     {"Device::CreateConstantBufferView", 17},    {"Device::CreateShaderResourceView", 18},
                {"Device::CreateRenderTargetView", 20},  {"Device::CreateDepthStencilView", 21},      {"Device::CopyDescriptors", 23},
                {"Device::CopyDescriptorsSimple", 24},   {"Device::CreateCommittedResource", 27},     {"Device::CreatePlacedResource", 29}},
      kCl[] = {{"CL::Close", 9},
               {"CL::Reset", 10},
               {"CL::DrawInstanced", 12},
               {"CL::DrawIndexedInstanced", 13},
               {"CL::Dispatch", 14},
               {"CL::CopyTextureRegion", 16},
               {"CL::CopyResource", 17},
               {"CL::IASetPrimitiveTopology", 20},
               {"CL::RSSetViewports", 21},
               {"CL::RSSetScissorRects", 22},
               {"CL::SetPipelineState", 25},
               {"CL::ResourceBarrier", 26},
               {"CL::SetGraphicsRootSignature", 30},
               {"CL::SetGraphicsRootDescriptorTable", 32},
               {"CL::SetComputeRootConstantBufferView", 37},
               {"CL::SetGraphicsRootConstantBufferView", 38},
               {"CL::IASetVertexBuffers", 44},
               {"CL::OMSetRenderTargets", 46},
               {"CL::ClearDepthStencilView", 47},
               {"CL::ClearRenderTargetView", 48},
               {"CL::BeginQuery", 52},
               {"CL::EndQuery", 53},
               {"CL::SetPredication", 55},
               {"CL::BeginEvent", 57},
               {"CL::ExecuteIndirect", 59}};
    for (const auto& e : kDev) add(e.n, dev, e.s);
    ID3D12Device2* d2 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d2))) && d2) {
        add("Device2::CreatePipelineState", d2, 47);
        d2->Release();
    }
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl)))) {
        if (alloc) alloc->Release();
        g_pix_collide = true;
        log::error("[pix] the pre-scan's probe list not made: D3D12 hooks not installed (the PIX session is unusable)");
        return false;
    }
    for (const auto& e : kCl) add(e.n, cl, e.s);
    cl->Close();
    cl->Release();
    alloc->Release();
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 256;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* probe = nullptr;
    ID3D12CommandQueue* q = nullptr;
    const D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&probe))) ||
        !probe || FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) || !q) {
        if (probe) probe->Release();
        g_pix_collide = true;
        log::error("[pix] the pre-scan's probe resource or queue not made: D3D12 hooks not installed (the PIX session is unusable)");
        return false;
    }
    add("Resource::SetName", probe, 6);
    add("Queue::ExecuteCommandLists", q, 10);
    probe->Release();
    q->Release();
    char where[160] = "?";
    hooks::describe_target(v[0].target, where, sizeof(where));
    log::info("[pix] the pre-scan: Device::CreateCommandQueue at %s", where);
    pix_report(v);
    return !g_pix_collide.load();
}

void hook_device_methods(ID3D12Device* dev) {
    std::call_once(g_device_once, [dev] {
        if (pix_loaded() && !pix_prescan(dev)) return;  // ([Debug] PixCapturer: shared targets, no D3D12 vtable hook)
        hooks::install("ID3D12Device::CreateCommandQueue", hooks::vtable_entry(dev, 8), hk_CreateCommandQueue,
                       &o_CreateCommandQueue);
        hooks::install("ID3D12Device::CreateDescriptorHeap", hooks::vtable_entry(dev, 14), hk_CreateDescHeap, &o_CreateDescHeap);
        ring_probe::install_device_hooks(dev);
        if (config::get_int("Stereo", "DescCensus", 0) == 1) {  // (item 8e, test only, off by default)
            HMODULE self = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(&dc_note), &self) && self) {
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(self) + dos->e_lfanew);
                g_dc_self0 = reinterpret_cast<uintptr_t>(self);
                g_dc_self1 = g_dc_self0 + nt->OptionalHeader.SizeOfImage;
            }
            hooks::install("ID3D12Device::CreateShaderResourceView", hooks::vtable_entry(dev, 18), hk_CreateSRV, &o_CreateSRV);
            hooks::install("ID3D12Device::CopyDescriptors", hooks::vtable_entry(dev, 23), hk_CopyDescs, &o_CopyDescs);
            hooks::install("ID3D12Device::CopyDescriptorsSimple", hooks::vtable_entry(dev, 24), hk_CopyDescsSimple, &o_CopyDescsSimple);
            g_dc_on = o_CreateSRV && o_CopyDescs && o_CopyDescsSimple && g_dc_self1;
            log::info("[d3d] [Stereo] DescCensus %s (test only, item 8e): the game's writes into its bindless texture partition, per frame",
                      g_dc_on.load() ? "on" : "NOT installed");
        }
        if (config::get_bool("Debug", "DepthLog", false) && !o_CreateSRV) {  // (r117 DG4, test only: the depthlog's SRV creations)
            HMODULE self = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(&dl_line), &self) && self) {
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(self) + dos->e_lfanew);
                g_dl_self0 = reinterpret_cast<uintptr_t>(self);
                g_dl_self1 = g_dl_self0 + nt->OptionalHeader.SizeOfImage;
            }
            hooks::install("ID3D12Device::CreateShaderResourceView", hooks::vtable_entry(dev, 18), hk_CreateSRV, &o_CreateSRV);
            log::info("[d3d] [Debug] DepthLog: CreateShaderResourceView hooked (test only: the depthlog's SRVs of the game's depth)");
        } else if (config::get_bool("Debug", "DepthLog", false)) {
            HMODULE self = nullptr;  // (DescCensus hooked it already: its module range taken for the flag too)
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(&dl_line), &self) && self) {
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(self) + dos->e_lfanew);
                g_dl_self0 = reinterpret_cast<uintptr_t>(self);
                g_dl_self1 = g_dl_self0 + nt->OptionalHeader.SizeOfImage;
            }
        }
        if (config::get_bool("Debug", "FrameGraph", true)) pso::install_device_hooks(dev);
        bool log_resources = config::get_bool("Debug", "LogResources", true);
        if (!log_resources && !ring_probe::tracking()) return;
        hooks::install("ID3D12Device::CreateRenderTargetView", hooks::vtable_entry(dev, 20), hk_CreateRTV, &o_CreateRTV);
        g_dsv_flags_on = config::get_int("Stereo", "MainInstanced", 0) == 1 || config::get_bool("Debug", "DepthLog", false);  // (before the hook)
        hooks::install("ID3D12Device::CreateDepthStencilView", hooks::vtable_entry(dev, 21), hk_CreateDSV, &o_CreateDSV);
        hooks::install("ID3D12Device::CreateCommittedResource", hooks::vtable_entry(dev, 27), hk_CreateCommittedResource,
                       &o_CreateCommittedResource);
        hooks::install("ID3D12Device::CreatePlacedResource", hooks::vtable_entry(dev, 29), hk_CreatePlacedResource,
                       &o_CreatePlacedResource);
        // A probe buffer gives the resource vtable, so SetName is hooked for every resource.
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 256;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* probe = nullptr;
        if (SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&probe))) &&
            probe) {
            hooks::install("ID3D12Resource::SetName", hooks::vtable_entry(probe, 6), hk_ResourceSetName, &o_ResourceSetName);
            probe->Release();
        }
        if (config::get_bool("Debug", "FrameGraph", true) || ring_probe::enabled()) hook_command_list_methods(dev);
    });
}

void enable_dred() {
    using GetDebugInterface_t = HRESULT(WINAPI*)(REFIID, void**);
    auto gdi = reinterpret_cast<GetDebugInterface_t>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12GetDebugInterface"));
    ID3D12DeviceRemovedExtendedDataSettings1* s = nullptr;
    if (!gdi || FAILED(gdi(IID_PPV_ARGS(&s))) || !s) {
        log::warn("[dred] settings interface unavailable; DRED not armed");
        return;
    }
    s->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    s->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    s->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    s->Release();
    state::dred_enabled = true;
    log::info("[dred] auto-breadcrumbs, breadcrumb context and page-fault reporting forced on");
}

// The devices made through the hook (run 8 item 3: a swapchain on any other device means the hooks came after the
// game made it: no device, list or ECL hooks, so no eye images)
std::mutex g_made_mutex;
ID3D12Device* g_made[16] = {};
int g_n_made = 0;
std::atomic<uint64_t> g_devices_made{0};
bool made_here(ID3D12Device* d) {
    std::lock_guard lock(g_made_mutex);
    for (int i = 0; i < g_n_made; ++i)
        if (g_made[i] == d) return true;
    return false;
}

// [Debug] D3D12DebugLayer=1 (test only, single-pass stereo's safety net: the mod's barriers and copies checked in the
// simulator before they could fault the GPU): the debug layer on before the game's devices are made, and its
// warnings and errors in the log ([d3d12] lines, at most 400 of each message id).
std::atomic<bool> g_debug_layer{false};
constexpr int kMsgIds = 2048;  // (item 8e: each message id's count, uncapped, for a test's windows: debug_msg_kv)
std::atomic<uint64_t> g_msg_n[kMsgIds];
void CALLBACK debug_message(D3D12_MESSAGE_CATEGORY cat, D3D12_MESSAGE_SEVERITY sev, D3D12_MESSAGE_ID id, LPCSTR text, void*) {
    if (sev > D3D12_MESSAGE_SEVERITY_WARNING) return;
    if (static_cast<int>(id) >= 0 && static_cast<int>(id) < kMsgIds) g_msg_n[static_cast<int>(id)].fetch_add(1, std::memory_order_relaxed);
    // (r117 DG4) log::limited keeps the key's pointer: one stable string per id (a stack buffer made every id share the first's count)
    static char keys[kMsgIds + 1][20];
    const int slot = static_cast<int>(id) >= 0 && static_cast<int>(id) < kMsgIds ? static_cast<int>(id) : kMsgIds;
    char* key = keys[slot];
    if (!key[0]) std::snprintf(key, sizeof(keys[0]), "d3d12.msg.%d", slot == kMsgIds ? -1 : slot);
    log::limited(key, 400, "[d3d12] %s %d (category %d): %s", sev <= D3D12_MESSAGE_SEVERITY_ERROR ? "layer error" : "layer warning",
                 static_cast<int>(id), static_cast<int>(cat), text ? text : "");
}
void enable_debug_layer() {
    using GetDebugInterface_t = HRESULT(WINAPI*)(REFIID, void**);
    auto gdi = reinterpret_cast<GetDebugInterface_t>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12GetDebugInterface"));
    ID3D12Debug* d = nullptr;
    if (!gdi || FAILED(gdi(IID_PPV_ARGS(&d))) || !d) {
        log::warn("[d3d12] the debug layer is not available (Graphics Tools not installed?)");
        return;
    }
    d->EnableDebugLayer();
    if (config::get_bool("Debug", "D3D12GpuValidation", false)) {  // (item 8g, test only: GPU-based validation with the layer)
        ID3D12Debug1* d1 = nullptr;
        if (SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&d1))) && d1) {
            d1->SetEnableGPUBasedValidation(TRUE);
            d1->Release();
            log::info("[d3d12] GPU-based validation on ([Debug] D3D12GpuValidation)");
        } else {
            log::warn("[d3d12] GPU-based validation not available (no ID3D12Debug1)");
        }
    }
    d->Release();
    g_debug_layer = true;
    log::info("[d3d12] the debug layer on ([Debug] D3D12DebugLayer)");
}
void hook_debug_messages(ID3D12Device* dev) {
    if (!g_debug_layer.load()) return;
    ID3D12InfoQueue1* q = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&q))) || !q) {
        log::warn("[d3d12] no ID3D12InfoQueue1: the debug layer's messages are not logged");
        return;
    }
    DWORD cookie = 0;
    const HRESULT hr = q->RegisterMessageCallback(debug_message, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
    log::info("[d3d12] message callback on device %p -> %#lx", static_cast<void*>(dev), static_cast<unsigned long>(hr));
    q->Release();  // the callback stays registered with the device
}

HRESULT WINAPI hk_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void** out) {
    if (out && !state::dred_enabled && config::get_bool("Debug", "Dred", true)) enable_dred();
    if (out && !g_debug_layer.load() && config::get_bool("Debug", "D3D12DebugLayer", false)) enable_debug_layer();
    HRESULT hr = o_D3D12CreateDevice(adapter, fl, riid, out);
    log::info("[d3d] D3D12CreateDevice(adapter %p, fl %#x, %s) -> %#lx", static_cast<void*>(adapter), static_cast<unsigned>(fl),
              out ? "create" : "probe", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && out && *out) {
        // Hooks only. The game creates and destroys a first device (with a 120x61 swapchain) before its real one
        // (ENGINE-NOTES 3.1), so the device the core uses is taken, with a reference, from the swapchain's queue.
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&dev))) && dev) {
            log::info("[d3d] device %p created", static_cast<void*>(dev));
            hook_debug_messages(dev);
            {  // single-pass stereo, view instancing (research\sps): what the device offers (two read-only queries)
                D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3{};
                const bool o3_ok = SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3)));
                D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_7};
                HRESULT smr = E_FAIL;
                for (D3D_SHADER_MODEL m : {D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4,
                                           D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2, D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0}) {
                    sm.HighestShaderModel = m;
                    if (SUCCEEDED(smr = dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) break;
                }
                log::info("[d3d] view instancing tier %d%s, highest shader model %#x%s", o3_ok ? static_cast<int>(o3.ViewInstancingTier) : -1,
                          o3_ok ? "" : " (OPTIONS3 not answered)", SUCCEEDED(smr) ? static_cast<unsigned>(sm.HighestShaderModel) : 0u,
                          SUCCEEDED(smr) ? "" : " (not answered)");
            }
            {
                std::lock_guard lock(g_made_mutex);
                if (g_n_made < 16) g_made[g_n_made++] = dev;  // the pointer only, for made_here
            }
            g_devices_made.fetch_add(1, std::memory_order_relaxed);
            hook_device_methods(dev);
            dev->Release();
        }
    }
    return hr;
}

// ------------------------------------------------------------------------------------------- present
void on_frame_end() {
    raise_cb_capacity();
    state::frame_ecl_calls = t_ecl_calls.exchange(0, std::memory_order_relaxed);
    state::frame_ecl_lists = t_ecl_lists.exchange(0, std::memory_order_relaxed);
    state::frame_ecl_direct = t_ecl_direct.exchange(0, std::memory_order_relaxed);
    state::frame_ecl_compute = t_ecl_compute.exchange(0, std::memory_order_relaxed);
    state::frame_ecl_copy = t_ecl_copy.exchange(0, std::memory_order_relaxed);
    state::frame_ecl_present = t_ecl_present.exchange(0, std::memory_order_relaxed);
    uint64_t frame = state::presents.load() + 1;
    if (dl_on() && g_dl_frames.fetch_sub(1, std::memory_order_relaxed) == 1) log::info("[depthlog] done (%llu lines)", static_cast<unsigned long long>(g_dl_lines.load()));
    int c = g_end_count.load();
    for (int i = 0; i < c; ++i) g_end_listeners[i](frame);
    if (FrameEndFn last = g_end_last.load(std::memory_order_relaxed)) last(frame);
    fg_on_frame_end(frame);
    g_frame_started = false;
}

void after_present(HRESULT hr) {
    uint64_t n = ++state::presents;
    double now = log::now_ms();
    if (g_prev_present_ms > 0) {
        double dt = now - g_prev_present_ms;
        double ema = state::cpu_frame_ms.load();
        state::cpu_frame_ms = ema == 0 ? dt : ema * 0.95 + dt * 0.05;
    }
    g_prev_present_ms = now;
    state::last_present_ms = now;
    if (n == 1 || n == 60 || n % 3600 == 0) {
        log::info("[frame] present %llu hr %#lx  cpu %.2f ms  gpu busy %.2f span %.2f ms  ECL/frame %u (present queue %u;"
                  " lists %u: direct %u compute %u copy %u)",
                  static_cast<unsigned long long>(n), static_cast<unsigned long>(hr), state::cpu_frame_ms.load(),
                  state::gpu_frame_ms.load(), state::gpu_span_ms.load(), state::frame_ecl_calls.load(),
                  state::frame_ecl_present.load(), state::frame_ecl_lists.load(),
                  state::frame_ecl_direct.load(), state::frame_ecl_compute.load(), state::frame_ecl_copy.load());
    }
    if (FAILED(hr)) log::limited("d3d.present.fail", 16, "[d3d] Present failed %#lx", static_cast<unsigned long>(hr));
}

// The single-pass probe (frame_probe.h): the frame end's and Present's times on the playback thread
void probe_present(double t0, double t1) {
    frame_probe::add(frame_probe::kFrameEnd, t1 - t0);
    frame_probe::add(frame_probe::kPresent, log::now_ms() - t1);
    frame_probe::playback_frame(t_draws);
}

HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (t_in_present || (flags & DXGI_PRESENT_TEST)) return o_Present(sc, sync, flags);
    ++t_in_present;
    const bool probe = frame_probe::on();
    const double t0 = probe ? log::now_ms() : 0;
    t_presenting = sc;
    on_frame_end();
    t_presenting = nullptr;
    const bool tear = unsync(sc, sync, flags);
    const double t1 = probe ? log::now_ms() : 0;
    HRESULT hr = o_Present(sc, sync, flags);
    if (tear && hr == DXGI_ERROR_INVALID_CALL) {
        tearing_refused(hr);
        hr = o_Present(sc, sync, flags & ~DXGI_PRESENT_ALLOW_TEARING);
    }
    if (probe) probe_present(t0, t1);
    after_present(hr);
    --t_in_present;
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (t_in_present || (flags & DXGI_PRESENT_TEST)) return o_Present1(sc, sync, flags, p);
    ++t_in_present;
    const bool probe = frame_probe::on();
    const double t0 = probe ? log::now_ms() : 0;
    t_presenting = sc;
    on_frame_end();
    t_presenting = nullptr;
    const bool tear = unsync(sc, sync, flags);
    const double t1 = probe ? log::now_ms() : 0;
    HRESULT hr = o_Present1(sc, sync, flags, p);
    if (tear && hr == DXGI_ERROR_INVALID_CALL) {
        tearing_refused(hr);
        hr = o_Present1(sc, sync, flags & ~DXGI_PRESENT_ALLOW_TEARING, p);
    }
    if (probe) probe_present(t0, t1);
    after_present(hr);
    --t_in_present;
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT f, UINT flags) {
    log::info("[dxgi] ResizeBuffers(count %u, %ux%u, fmt %d, flags %#x)", count, w, h, static_cast<int>(f), flags);
    {  // a relaunch's put-back size: the buffers at the armed size as the targets (render_res::relaunch_swap_size)
        uint32_t rw = 0, rh = 0;
        if (sc == state::swapchain && render_res::relaunch_swap_size(&rw, &rh) && (w != rw || h != rh)) {
            log::info("[renderres] the relaunch's ResizeBuffers %ux%u made %ux%u (the targets' size)", w, h, rw, rh);
            w = rw;
            h = rh;
        }
    }
    if (FrameSizeFn fs = g_frame_size.load(std::memory_order_acquire)) fs("ResizeBuffers", w, h);
    HRESULT hr = o_ResizeBuffers(sc, count, w, h, f, flags);
    if (SUCCEEDED(hr)) {
        g_tearing_ok.store(-1, std::memory_order_relaxed);  // ([XR] PacingTearing) checked again: full screen or windowed, the flags
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(sc->GetDesc(&d))) {
            state::swap_width = d.BufferDesc.Width;
            state::swap_height = d.BufferDesc.Height;
            state::swap_format = d.BufferDesc.Format;
            state::swap_buffers = d.BufferCount;
            if (FrameSizeFn fs = g_frame_size.load(std::memory_order_acquire)) fs("ResizeBuffers done", d.BufferDesc.Width, d.BufferDesc.Height);
        }
    }
    return hr;
}

void on_swapchain(IUnknown* device_or_queue, HWND hwnd, IDXGISwapChain* sc) {
    // The core keeps (and never releases) a reference to the queue and device of every swapchain it sees, so a
    // device the game throws away cannot be used after it is freed (the 2026-10-03 watchdog crash). The latest
    // swapchain's queue and device are the current ones.
    ID3D12CommandQueue* q = nullptr;
    if (device_or_queue && SUCCEEDED(device_or_queue->QueryInterface(IID_PPV_ARGS(&q))) && q) {
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&dev))) && dev) {
            state::device = dev;
            log::info("[d3d] current device %p (from the swapchain's queue)", static_cast<void*>(dev));
            if (!made_here(dev))
                log::error("[d3d] LATE: the swapchain's device %p was made before the startup hooks (no device, list or ECL hooks: no "
                           "eye images). A game executable or wrapper that loads the mod late?", static_cast<void*>(dev));
        }
        state::present_queue = q;
    }
    state::swapchain = sc;
    state::swapchain_frame = state::presents.load();
    state::game_hwnd = hwnd;
    DXGI_SWAP_CHAIN_DESC d{};
    if (SUCCEEDED(sc->GetDesc(&d))) {
        state::swap_width = d.BufferDesc.Width;
        state::swap_height = d.BufferDesc.Height;
        state::swap_format = d.BufferDesc.Format;
        state::swap_buffers = d.BufferCount;
        // the usage too: whether the buffers take a shader view (the cinema's resample copies them first either way)
        log::info("[dxgi] swapchain %p on queue %p hwnd %p: %ux%u fmt %d buffers %u swap effect %d flags %#x windowed %d usage %#x",
                  static_cast<void*>(sc), static_cast<void*>(q), static_cast<void*>(hwnd), d.BufferDesc.Width,
                  d.BufferDesc.Height, static_cast<int>(d.BufferDesc.Format), d.BufferCount, static_cast<int>(d.SwapEffect),
                  d.Flags, d.Windowed, static_cast<unsigned>(d.BufferUsage));
    }
    std::call_once(g_swapchain_once, [sc] {
        if (pix_loaded()) {  // ([Debug] PixCapturer: the swapchain's slots checked as the device's)
            if (g_pix_collide.load()) {
                log::error("[pix] the swapchain's hooks not installed either (the D3D12 hooks' targets collided)");
                return;
            }
            std::vector<PixSlot> v{{"SwapChain::Present", hooks::vtable_entry(sc, 8)}, {"SwapChain::ResizeBuffers", hooks::vtable_entry(sc, 13)}};
            IDXGISwapChain1* s1 = nullptr;
            if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&s1))) && s1) {
                v.push_back({"SwapChain1::Present1", hooks::vtable_entry(s1, 22)});
                s1->Release();
            }
            pix_report(v);
            if (g_pix_collide.load()) return;
        }
        hooks::install("IDXGISwapChain::Present", hooks::vtable_entry(sc, 8), hk_Present, &o_Present);
        hooks::install("IDXGISwapChain::ResizeBuffers", hooks::vtable_entry(sc, 13), hk_ResizeBuffers, &o_ResizeBuffers);
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
            hooks::install("IDXGISwapChain1::Present1", hooks::vtable_entry(sc1, 22), hk_Present1, &o_Present1);
            sc1->Release();
        }
    });
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out,
                                                    IDXGISwapChain1** sc) {
    DXGI_SWAP_CHAIN_DESC1 d1{};
    {  // a relaunch's put-back size: the game's swapchain made at the armed size (not the 120x61 probe's)
        uint32_t rw = 0, rh = 0;
        if (desc && desc->Width >= 640 && desc->Height >= 480 && render_res::relaunch_swap_size(&rw, &rh) && (desc->Width != rw || desc->Height != rh)) {
            d1 = *desc;
            log::info("[renderres] the relaunch's swapchain %ux%u made %ux%u (the targets' size)", d1.Width, d1.Height, rw, rh);
            d1.Width = rw;
            d1.Height = rh;
            desc = &d1;
        }
    }
    HRESULT hr = o_CreateSwapChainForHwnd(f, dev, hwnd, desc, fs, out, sc);
    log::info("[dxgi] CreateSwapChainForHwnd(factory %p, queue %p, hwnd %p) -> %#lx", static_cast<void*>(f),
              static_cast<void*>(dev), static_cast<void*>(hwnd), static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && sc && *sc) on_swapchain(dev, hwnd, *sc);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** sc) {
    DXGI_SWAP_CHAIN_DESC d0{};
    {  // (as CreateSwapChainForHwnd's: a relaunch's put-back size)
        uint32_t rw = 0, rh = 0;
        if (desc && desc->BufferDesc.Width >= 640 && desc->BufferDesc.Height >= 480 && render_res::relaunch_swap_size(&rw, &rh) &&
            (desc->BufferDesc.Width != rw || desc->BufferDesc.Height != rh)) {
            d0 = *desc;
            log::info("[renderres] the relaunch's swapchain %ux%u made %ux%u (the targets' size)", d0.BufferDesc.Width, d0.BufferDesc.Height, rw, rh);
            d0.BufferDesc.Width = rw;
            d0.BufferDesc.Height = rh;
            desc = &d0;
        }
    }
    HRESULT hr = o_CreateSwapChain(f, dev, desc, sc);
    log::info("[dxgi] CreateSwapChain(factory %p, queue %p) -> %#lx", static_cast<void*>(f), static_cast<void*>(dev),
              static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && sc && *sc && desc) on_swapchain(dev, desc->OutputWindow, *sc);
    return hr;
}

HRESULT WINAPI hk_CreateDXGIFactory2(UINT flags, REFIID riid, void** out) {
    HRESULT hr = o_CreateDXGIFactory2(flags, riid, out);
    log::limited("dxgi.factory", 8, "[dxgi] CreateDXGIFactory2(flags %#x) -> %#lx", flags, static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && out && *out) {
        IDXGIFactory2* f = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&f))) && f) {
            std::call_once(g_factory_once, [f] {
                hooks::install("IDXGIFactory2::CreateSwapChainForHwnd", hooks::vtable_entry(f, 15), hk_CreateSwapChainForHwnd,
                               &o_CreateSwapChainForHwnd);
                hooks::install("IDXGIFactory::CreateSwapChain", hooks::vtable_entry(f, 10), hk_CreateSwapChain, &o_CreateSwapChain);
            });
            f->Release();
        }
    }
    return hr;
}

}  // namespace

namespace {
HMODULE g_pix = nullptr;  // ([Debug] PixCapturer) set once at the startup hooks, before the device
// the version directory names compared as dotted numbers ("2603.25" > "2509.9"); a name that is not one sorts first
bool pix_newer(const wchar_t* a, const wchar_t* b) {
    for (;;) {
        wchar_t* ea = nullptr;
        wchar_t* eb = nullptr;
        const unsigned long va = std::wcstoul(a, &ea, 10), vb = std::wcstoul(b, &eb, 10);
        if (ea == a || eb == b) return ea != a && eb == b;
        if (va != vb) return va > vb;
        if (*ea != L'.' || *eb != L'.') return *ea == L'.' && *eb != L'.';
        a = ea + 1;
        b = eb + 1;
    }
}
void pix_load() {
    const std::wstring root = L"C:\\Program Files\\Microsoft PIX\\";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"*").c_str(), &fd);
    std::wstring best;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            const std::wstring dll = root + fd.cFileName + L"\\WinPixGpuCapturer.dll";
            if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            if (best.empty() || pix_newer(fd.cFileName, best.c_str())) best = fd.cFileName;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (best.empty()) {
        log::error("[pix] [Debug] PixCapturer: no WinPixGpuCapturer.dll under C:\\Program Files\\Microsoft PIX\\<version>; continuing without it");
        return;
    }
    const std::wstring path = root + best + L"\\WinPixGpuCapturer.dll";
    char p8[MAX_PATH * 2] = "?";
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, p8, sizeof(p8), nullptr, nullptr);
    const bool d3d12_before = GetModuleHandleW(L"d3d12.dll") != nullptr;
    g_pix = LoadLibraryW(path.c_str());
    if (!g_pix) {
        log::error("[pix] LoadLibraryW %s failed (error %lu); continuing without it", p8, GetLastError());
        return;
    }
    // (PIX's wrappers: each D3D12 object the game gets may be PIX's; the mod's vtable hooks then land in PIX's code: the device's
    // pre-scan, pix_prescan; a target hooked twice still refused as a backstop, hooks::set_refuse_shared)
    hooks::set_refuse_shared(true);
    log::info("[pix] loaded %s at %p (d3d12.dll %s loaded before it; CaptureNextFrame %s)", p8, static_cast<void*>(g_pix),
              d3d12_before ? "already" : "not yet", GetProcAddress(g_pix, "CaptureNextFrame") || GetProcAddress(g_pix, "PIXGpuCaptureNextFrames") ? "exported" : "NOT exported");
}
}  // namespace

bool pix_loaded() { return g_pix != nullptr; }

std::string pixcap(const std::string& file, unsigned frames) {
    if (!g_pix) return "ERROR pixcap: the PIX capturer is not loaded ([Debug] PixCapturer=1 at startup)";
    using Fn = HRESULT(WINAPI*)(PCWSTR, UINT32);
    // (pix3_win.h's PIXGpuCaptureNextFrames: the capturer's "CaptureNextFrame" export, (PCWSTR fileName, UINT32 numFrames); the other name kept)
    auto fn = reinterpret_cast<Fn>(GetProcAddress(g_pix, "CaptureNextFrame"));
    if (!fn) fn = reinterpret_cast<Fn>(GetProcAddress(g_pix, "PIXGpuCaptureNextFrames"));
    if (!fn) return "ERROR pixcap: neither CaptureNextFrame nor PIXGpuCaptureNextFrames exported by the loaded capturer";
    if (file.empty() || file.size() >= MAX_PATH || frames < 1 || frames > 16) return "ERROR pixcap <file.wpix> [frames=1..16]";
    // (the review: PIX may read the name when it writes, frames later: a static buffer, not rewritten while a capture may be pending:
    // until the asked frames plus 30 presents have passed since the call; one caller, the test channel's thread)
    static wchar_t w[MAX_PATH] = {};
    static std::atomic<uint64_t> pending_until{0};
    const uint64_t now = state::presents.load(std::memory_order_relaxed);
    if (now < pending_until.load()) return "ERROR pixcap: a capture may still be pending (wait for its frames plus 30 presents)";
    if (!MultiByteToWideChar(CP_UTF8, 0, file.c_str(), -1, w, MAX_PATH)) return "ERROR pixcap: the file name is not convertible";
    const HRESULT hr = fn(w, frames);
    if (SUCCEEDED(hr)) pending_until = now + frames + 30;
    log::info("[pix] CaptureNextFrame(%s, %u) = 0x%08lx", file.c_str(), frames, static_cast<unsigned long>(hr));
    char b[160];
    std::snprintf(b, sizeof(b), "pixcap %s frames=%u hr=0x%08lx%s", file.c_str(), frames, static_cast<unsigned long>(hr), SUCCEEDED(hr) ? "" : " (failed)");
    return b;
}

bool install_startup_hooks() {
    if (config::get_bool("Debug", "PixCapturer", false)) pix_load();  // ([Debug] PixCapturer, test only: before d3d12.dll is hooked)
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (!d3d12) d3d12 = LoadLibraryW(L"d3d12.dll");
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!dxgi) dxgi = LoadLibraryW(L"dxgi.dll");
    if (!d3d12 || !dxgi) {
        log::error("[d3d] d3d12.dll or dxgi.dll not loadable; no D3D12 hooks");
        return false;
    }
    bool ok = hooks::install("d3d12!D3D12CreateDevice", reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice")),
                             hk_D3D12CreateDevice, &o_D3D12CreateDevice);
    ok = hooks::install("dxgi!CreateDXGIFactory2", reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory2")),
                        hk_CreateDXGIFactory2, &o_CreateDXGIFactory2) &&
         ok;
    return ok;
}

// Registration happens at startup, from one thread. A full list is an error, never a silent drop (cycle 23: a ninth
// frame-end listener pushed the AA forcing out, so technique 2 could not be selected).
void add_frame_end_listener(FrameEndFn fn) {
    int i = g_end_count.load();
    if (i >= kMaxListeners) {
        log::error("[d3d] frame-end listener list full (%d): %p not registered", kMaxListeners, reinterpret_cast<void*>(fn));
        return;
    }
    g_end_listeners[i] = fn;
    g_end_count = i + 1;
}

void set_frame_end_last(FrameEndFn fn) { g_end_last.store(fn); }

IDXGISwapChain* presenting_swapchain() { return t_presenting; }

void add_frame_start_listener(FrameStartFn fn) {
    int i = g_start_count.load();
    if (i >= kMaxListeners) {
        log::error("[d3d] frame-start listener list full (%d): %p not registered", kMaxListeners, reinterpret_cast<void*>(fn));
        return;
    }
    g_start_listeners[i] = fn;
    g_start_count = i + 1;
}

void set_ecl_bracket(EclBracketFn fn) { g_bracket = fn; }

void framegraph_arm(int frames) {
    std::lock_guard lock(g_fg_mutex);
    g_fg_text.clear();
    g_fg_lists.clear();
    g_fg_frame0 = state::presents.load() + 1;
    g_fg_frames_left = frames;
    if (frames > 0) pso::arm_census();  // the segments' shaders
    std::snprintf(g_fg_status, sizeof(g_fg_status), "armed for %d frame(s) from %llu", frames,
                  static_cast<unsigned long long>(g_fg_frame0));
    g_fg_armed = frames > 0;
}

const char* framegraph_status() { return g_fg_status; }

InternalSubmitScope::InternalSubmitScope() { ++t_in_ecl; }
InternalSubmitScope::~InternalSubmitScope() { --t_in_ecl; }

void submit_internal(ID3D12CommandQueue* queue, ID3D12CommandList* list) {
    ++t_in_ecl;  // bypasses counting, frame start and the frame graph
    queue->ExecuteCommandLists(1, &list);
    --t_in_ecl;
}

const void* view_resource(uintptr_t handle, int* format) {
    std::lock_guard lock(g_names_mutex);
    auto it = g_rtv.find(handle);
    if (it == g_rtv.end()) return nullptr;
    if (format) {
        auto f = g_view_fmt.find(handle);
        *format = static_cast<int>(f == g_view_fmt.end() ? DXGI_FORMAT_UNKNOWN : f->second);
    }
    return it->second;
}

void raw_bind(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single,
              const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
    if (dl_on()) {
        dl_dsv(cl, ds, true, "OMSetRenderTargets (raw)", "");  // (r117 DG4's depthlog: the mod's own binds of the game's DSV)
        t_dl_bind_cl = cl;
        t_dl_bind_ds = ds ? ds->ptr : 0;
    }
    if (o_OMSetRenderTargets) {
        InOriginal in(kNBind);  // (a debug layer's forward inside it passes straight through the hook)
        o_OMSetRenderTargets(cl, n, rts, single, ds);
    } else {
        cl->OMSetRenderTargets(n, rts, single, ds);
    }
}

int dsv_flags(uintptr_t h) {
    if (!g_dsv_flags_on) return -1;
    std::lock_guard lock(g_dsv_flags_mutex);
    const auto it = g_dsv_flags.find(h);
    return it == g_dsv_flags.end() ? -1 : static_cast<int>(it->second);
}

void raw_barrier(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_RESOURCE_BARRIER* b) {
    if (dl_on()) dl_barriers(cl, n, b, true);  // (r117 DG4's depthlog: the mod's own)
    if (o_ResourceBarrier) {
        InOriginal in(kNBarrier);
        o_ResourceBarrier(cl, n, b);
    } else {
        cl->ResourceBarrier(n, b);
    }
}

void raw_copy_region(ID3D12GraphicsCommandList* cl, const D3D12_TEXTURE_COPY_LOCATION* dst, const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    if (dl_on() && dst && src)  // (r117 DG4's depthlog: the mod's own)
        dl_copy(cl, dst->pResource, src->pResource, dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ? static_cast<int>(dst->SubresourceIndex) : -2,
                src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ? static_cast<int>(src->SubresourceIndex) : -2, true, "CopyTextureRegion (raw)");
    if (o_CopyTextureRegion) {
        InOriginal in(kNCopyRegion);
        o_CopyTextureRegion(cl, dst, 0, 0, 0, src, box);
    } else {
        cl->CopyTextureRegion(dst, 0, 0, 0, src, box);
    }
}

void raw_draw(ID3D12GraphicsCommandList* cl, const DrawArgs& x) {  // ([Stereo] MainInstanced, test only)
    if (dl_on()) dl_draw(cl, x.indexed ? "DrawIndexedInstanced (raw)" : "DrawInstanced (raw)", true, true, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    if (x.indexed) {
        if (o_DrawIndexedInstanced) {
            InOriginal in(kNDrawIndexed);
            o_DrawIndexedInstanced(cl, x.a, x.b, x.c, x.d, x.e);
        } else {
            cl->DrawIndexedInstanced(x.a, x.b, x.c, x.d, x.e);
        }
    } else if (o_DrawInstanced) {
        InOriginal in(kNDraw);
        o_DrawInstanced(cl, x.a, x.b, x.c, x.e);
    } else {
        cl->DrawInstanced(x.a, x.b, x.c, x.e);
    }
}

void note_view_instance_mask(ID3D12GraphicsCommandList* cl, unsigned mask) {
    std::lock_guard lock(g_vim_mutex);
    Vim* free_slot = nullptr;
    for (Vim& v : g_vim) {
        if (v.cl == cl) {
            v.mask = mask;
            return;
        }
        if (!v.cl && !free_slot) free_slot = &v;
    }
    if (!free_slot) return;
    *free_slot = {cl, mask};
    g_vim_count.fetch_add(1, std::memory_order_relaxed);
}
bool known_view_instance_mask(ID3D12GraphicsCommandList* cl, unsigned* mask) {
    if (g_vim_count.load(std::memory_order_relaxed) == 0) return false;
    std::lock_guard lock(g_vim_mutex);
    for (const Vim& v : g_vim)
        if (v.cl == cl) {
            *mask = v.mask;
            return true;
        }
    return false;
}
void raw_clear_rtv(ID3D12GraphicsCommandList* cl, const D3D12_CPU_DESCRIPTOR_HANDLE* h, const float rgba[4]) {  // (MainInstanced)
    if (o_ClearRTV) {
        InOriginal in(kNClearRtv);
        o_ClearRTV(cl, *h, rgba, 0, nullptr);
    } else {
        cl->ClearRenderTargetView(*h, rgba, 0, nullptr);
    }
}
void raw_clear_dsv(ID3D12GraphicsCommandList* cl, const D3D12_CPU_DESCRIPTOR_HANDLE* h, float depth, unsigned char stencil) {  // (MainSlice)
    if (dl_on()) dl_dsv(cl, h, true, "ClearDepthStencilView (raw)", "");  // (r117 DG4's depthlog)
    const D3D12_CLEAR_FLAGS f = D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL;
    if (o_ClearDSV) {
        InOriginal in(kNClearDsv);
        o_ClearDSV(cl, *h, f, depth, stencil, 0, nullptr);
    } else {
        cl->ClearDepthStencilView(*h, f, depth, stencil, 0, nullptr);
    }
}

thread_local int t_in_original[kNestKinds] = {};

void count_nested(Nest m) { g_nested[m].fetch_add(1, std::memory_order_relaxed); }


void nested_status(char* out, size_t len) {
    auto g = [](Nest m) { return static_cast<unsigned long long>(g_nested[m].load(std::memory_order_relaxed)); };
    std::snprintf(out, len,
                  "nested: barriers %llu, binds %llu, copy regions %llu, indirect %llu, draws %llu, indexed draws %llu, dispatches %llu, "
                  "copies %llu, closes %llu, resets %llu, depth clears %llu, target clears %llu, queries %llu, predications %llu, events %llu, "
                  "pipelines %llu, vertex buffers %llu, graphics root cbvs %llu, compute root cbvs %llu",
                  g(kNBarrier), g(kNBind), g(kNCopyRegion), g(kNIndirect), g(kNDraw), g(kNDrawIndexed), g(kNDispatch), g(kNCopy),
                  g(kNClose), g(kNReset), g(kNClearDsv), g(kNClearRtv), g(kNEndQuery), g(kNPredication), g(kNBeginEvent),
                  g(kNPipeline), g(kNVertexBuffers), g(kNGraphicsRootCbv), g(kNComputeRootCbv));
}

const char* resource_name(const void* resource) {
    std::lock_guard lock(g_names_mutex);
    auto it = g_names.find(resource);
    return it == g_names.end() ? nullptr : it->second.c_str();
}

void set_resource_name(const void* resource, const char* name) {
    std::lock_guard lock(g_names_mutex);
    g_names[resource] = name;
}

const void* find_resource(const char* name) {
    std::lock_guard lock(g_names_mutex);
    const void* found = nullptr;
    for (const auto& [res, n] : g_names)
        if (n == name) found = res;  // the newest of several with the same name is not knowable here; any live one works
    return found;
}

bool resource_named(const void* resource, const char* name) {
    std::lock_guard lock(g_names_mutex);
    auto it = g_names.find(resource);
    return it != g_names.end() && it->second == name;
}

void set_copy_tap(CopyTapFn fn) {
    for (auto& t : g_copy_tap) {
        CopyTapFn none = nullptr;
        if (t.load() == fn || t.compare_exchange_strong(none, fn)) return;
    }
    log::error("[d3d] copy tap list full");
}

void watch_add(const void* res) {
    if (!res) return;
    std::lock_guard lock(g_watch_mutex);
    for (int w = 0; w < kWatchSlots; ++w)
        if (g_watch_res[w].load() == res) {
            ++g_watch[w].refs;
            return;
        }
    for (int w = 0; w < kWatchSlots; ++w)
        if (!g_watch_res[w].load()) {
            g_watch[w] = Watch{};
            g_watch[w].refs = 1;
            g_watch_res[w] = res;
            ++g_watch_count;
            return;
        }
    log::error("[d3d] barrier watch full: %p not watched", res);
}

void watch_remove(const void* res) {
    if (!res) return;
    std::lock_guard lock(g_watch_mutex);
    for (int w = 0; w < kWatchSlots; ++w)
        if (g_watch_res[w].load() == res && --g_watch[w].refs <= 0) {
            g_watch_res[w] = nullptr;
            g_watch[w] = Watch{};
            --g_watch_count;
        }
}

uint64_t watched_writes(const void* res) {
    if (!res) return 0;
    std::lock_guard lock(g_watch_mutex);
    for (int w = 0; w < kWatchSlots; ++w)
        if (g_watch_res[w].load() == res) return g_watch[w].writes;
    return 0;
}

bool watched_state(const void* res, ID3D12GraphicsCommandList** cl, uint32_t* state) {
    if (!res) return false;
    std::lock_guard lock(g_watch_mutex);
    for (int w = 0; w < kWatchSlots; ++w) {
        if (g_watch_res[w].load() != res) continue;
        const Watch& x = g_watch[w];
        if (!x.valid || x.tid != GetCurrentThreadId()) return false;
        *cl = x.cl;
        *state = x.state;
        return true;
    }
    return false;
}

namespace {
// (2026-10-29) inside RDR.exe's image the working set's page check first, as body.cpp's checked reads (image_mem.h)
bool readable(const void* p, size_t n) {
    if (image_mem::fast_readable(reinterpret_cast<uintptr_t>(p), n, image_mem::kReadAny)) return true;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || !image_mem::query(reinterpret_cast<uintptr_t>(p), &mbi, image_mem::kD3d) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                     PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & ok)) return false;
    return static_cast<const char*>(p) + n <= static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
}
}  // namespace

const void* resource_in_object(const void* object, size_t bytes, const char* name, char* where, size_t where_len) {
    if (!readable(object, bytes)) return nullptr;
    const void* const* q = static_cast<const void* const*>(object);
    std::lock_guard lock(g_names_mutex);
    // A field may hold the resource itself or the CPU handle of one of its render-target views.
    auto match = [&](const void* p) -> const void* {
        auto it = g_names.find(p);
        if (it != g_names.end() && it->second == name) return p;
        auto v = g_rtv.find(reinterpret_cast<SIZE_T>(p));
        if (v != g_rtv.end()) {
            auto n = g_names.find(v->second);
            if (n != g_names.end() && n->second == name) return v->second;
        }
        return nullptr;
    };
    for (size_t i = 0; i < bytes / 8; ++i) {
        if (const void* r = match(q[i])) {
            std::snprintf(where, where_len, "+%#zx", i * 8);
            return r;
        }
    }
    for (size_t i = 0; i < bytes / 8; ++i) {
        if (!readable(q[i], 0x100)) continue;
        const void* const* r = static_cast<const void* const*>(q[i]);
        for (size_t j = 0; j < 0x100 / 8; ++j) {
            if (const void* res = match(r[j])) {
                std::snprintf(where, where_len, "+%#zx->+%#zx", i * 8, j * 8);
                return res;
            }
        }
    }
    return nullptr;
}

const void* unique_resource(const char* name) {
    std::lock_guard lock(g_names_mutex);
    const void* found = nullptr;
    for (const auto& [res, n] : g_names) {
        if (n != name) continue;
        if (found) return nullptr;  // two with that name: one may be gone, so neither is safe
        found = res;
    }
    return found;
}

int rtv_handles_of(const void* res, uint64_t* out, int max) {
    std::lock_guard lock(g_names_mutex);
    int n = 0;
    for (const auto& [h, r] : g_rtv)
        if (r == res && n < max) out[n++] = h;
    return n;
}

void set_rtv_substitute(RtvSubstFn fn) { g_rtv_subst = fn; }
void set_rtv_substitute2(RtvSubstFn fn) { g_rtv_subst2 = fn; }

void set_present_unsynced(bool on) {
    if (on) g_tearing_cfg = config::get_bool("XR", "PacingTearing", false);
    if (g_present_unsynced.exchange(on) != on)
        log::info("[d3d] Present sync interval: %s", on ? "0 (paced by the XR frame loop)" : "the game's");
}

void game_present_state(uint32_t* last, uint64_t* unsynced, uint64_t* tearing_added) {
    *last = g_game_present.load(std::memory_order_relaxed);
    *unsynced = g_unsynced_presents.load(std::memory_order_relaxed);
    *tearing_added = g_tearing_added.load(std::memory_order_relaxed);
}

void set_bind_tap(int slot, BindTapFn fn) {
    if (slot >= 0 && slot < 7) g_bind_tap[slot] = fn;
}

void set_frame_size_listener(FrameSizeFn fn) { g_frame_size.store(fn, std::memory_order_release); }



uint64_t thread_draws() { return t_draws; }
bool thread_predicated() { return t_predicated; }
void set_predicate_tracking(bool on) { g_pd_track.store(on, std::memory_order_relaxed); }
int thread_predicate(ID3D12GraphicsCommandList* cl, Predicate* out) {  // ([Stereo] MainPredication, test only)
    if (!g_pd_track.load(std::memory_order_relaxed) || !cl || t_pd.cl != cl || !t_pd.here) return kPredUnknown;
    out->res = t_pd.res;
    out->off = t_pd.off;
    out->op = static_cast<int>(t_pd.op);
    return t_pd.res ? kPredSet : kPredNone;
}
void raw_set_predication(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, uint64_t off, int op) {  // (MainPredication)
    const D3D12_PREDICATION_OP o = static_cast<D3D12_PREDICATION_OP>(op);
    if (o_SetPredication) {
        InOriginal in(kNPredication);  // (a debug layer's forward inside it passes straight through the hook)
        o_SetPredication(cl, r, off, o);
    } else {
        cl->SetPredication(r, off, o);
    }
}
void depthlog_draws(int a, int b) {  // (r117 DG4, test only: the draws' ordinal range; a < 0 none)
    g_dl_da.store(a, std::memory_order_relaxed);
    g_dl_db.store(b, std::memory_order_relaxed);
}
void depthlog_arm(int frames) {  // (r117 DG4, test only)
    g_dl_lines.store(0, std::memory_order_relaxed);
    g_dl_frames.store(frames > 0 ? frames : 0, std::memory_order_relaxed);
    log::info("[depthlog] armed for %d presents (the game's depth %p as last found by name)", frames, g_dl_res.load());
}
std::string depthlog_status() {
    char b[200];
    std::snprintf(b, sizeof(b), "depthlog: %d presents left, %llu lines, the depth %p, SRV creations %s", g_dl_frames.load(),
                  static_cast<unsigned long long>(g_dl_lines.load()), g_dl_res.load(), o_CreateSRV ? "hooked" : "not hooked ([Debug] DepthLog=1 at start)");
    return b;
}
bool root_note(ID3D12GraphicsCommandList* cl, const void** root, uint64_t* tables) {  // (item 8g, the review's SF1)
    if (!g_rn_hooked.load(std::memory_order_relaxed) || !cl || t_rn.cl != cl || !t_rn.root) return false;
    *root = t_rn.root;
    *tables = t_rn.tables;
    return true;
}
bool root_table_gpu(ID3D12GraphicsCommandList* cl, unsigned param, uint64_t* gpu) {  // (item 8g, the delta review)
    if (!g_rn_hooked.load(std::memory_order_relaxed) || !cl || t_rn.cl != cl || param >= 64 || !(t_rn.tables >> param & 1)) return false;
    *gpu = t_rn.gpu[param];
    return true;
}
bool raster_note(ID3D12GraphicsCommandList* cl, RasterNote* out) {  // ([Stereo] MainRegionHandover, item 8g)
    if (!g_rs_hooked.load(std::memory_order_relaxed) || !cl || t_rs.cl != cl) return false;
    *out = t_rs;
    return true;
}
void raw_set_raster(ID3D12GraphicsCommandList* cl, unsigned nvp, const float (*vp)[6], unsigned nsc, const int32_t (*sc)[4], int topo) {
    if (!g_rs_hooked.load(std::memory_order_relaxed)) return;
    InOriginal in(kNRaster);  // (a debug layer's forward passes straight through the hooks: the note unchanged)
    if (nvp && vp) o_RSSetViewports(cl, nvp, reinterpret_cast<const D3D12_VIEWPORT*>(vp));
    if (nsc && sc) o_RSSetScissorRects(cl, nsc, reinterpret_cast<const D3D12_RECT*>(sc));
    if (topo) o_IASetPrimitiveTopology(cl, static_cast<D3D12_PRIMITIVE_TOPOLOGY>(topo));
}
int thread_open_queries(ID3D12GraphicsCommandList* cl) {  // (-1: not hooked, or the open queries not this list's alone)
    if (!g_bq_hooked.load(std::memory_order_relaxed)) return -1;
    if (t_open_queries == 0) return 0;
    return t_q_cl == cl ? t_open_queries : -1;
}
uint64_t thread_queries() { return t_queries; }
bool in_present() { return t_in_present != 0; }
std::string debug_msg_kv() {  // ([Debug] D3D12DebugLayer) " dbg_ids=id:count,..." (empty without the layer)
    if (!g_debug_layer.load()) return "";
    std::string out = " dbg_ids=";
    char b[32];
    int m = 0;
    for (int i = 0; i < kMsgIds; ++i) {
        const uint64_t n = g_msg_n[i].load(std::memory_order_relaxed);
        if (!n) continue;
        std::snprintf(b, sizeof(b), "%s%d:%llu", m++ ? "," : "", i, static_cast<unsigned long long>(n));
        out += b;
    }
    if (!m) out += "-";
    return out;
}
void desc_phase(bool in) { g_dc_phase.store(in, std::memory_order_relaxed); }
std::string desc_census_kv() {  // ([Stereo] DescCensus) the ring's frames with writes, oldest first
    if (!g_dc_on.load()) return "";
    struct F {
        uint64_t frame;
        uint32_t n, in, first, last;
    } f[64];
    int m = 0;
    for (DcSlot& s : g_dc_ring) {
        const uint32_t n = s.n.load(std::memory_order_relaxed);
        if (n)
            f[m++] = F{s.frame.load(std::memory_order_relaxed), n, s.in.load(std::memory_order_relaxed), s.first.load(std::memory_order_relaxed),
                       s.last.load(std::memory_order_relaxed)};
    }
    std::sort(f, f + m, [](const F& a, const F& b) { return a.frame < b.frame; });
    char b[96];
    std::snprintf(b, sizeof(b), " dc_on=1 dc_writes=%llu dc_in=%llu dc_mod=%llu dc_frames=%llu dcf=", static_cast<unsigned long long>(g_dc_writes.load()),
                  static_cast<unsigned long long>(g_dc_in.load()), static_cast<unsigned long long>(g_dc_mod.load()),
                  static_cast<unsigned long long>(g_dc_frames.load()));
    std::string out = b;
    for (int i = m > 32 ? m - 32 : 0; i < m; ++i) {  // (the last 32: frame:writes:within the passes:first:last)
        std::snprintf(b, sizeof(b), "%s%llu:%u:%u:%u:%u", i > (m > 32 ? m - 32 : 0) ? "," : "", static_cast<unsigned long long>(f[i].frame), f[i].n,
                      f[i].in, f[i].first, f[i].last);
        out += b;
    }
    if (!m) out += "-";
    return out;
}

}  // namespace rdrvr::d3d
