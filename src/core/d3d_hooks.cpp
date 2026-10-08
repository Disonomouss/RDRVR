#include "core/d3d_hooks.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/anchors.h"
#include "core/config.h"
#include "core/hooks.h"
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
    }
    l.draws = l.dispatches = l.barriers = l.queries = l.indirect = 0;
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
using ExecuteIndirect_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64,
                                                   ID3D12Resource*, UINT64);
using SetPredication_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64, D3D12_PREDICATION_OP);
using Close_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
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
ExecuteIndirect_t o_ExecuteIndirect = nullptr;
SetPredication_t o_SetPredication = nullptr;
Close_t o_Close = nullptr;
Reset_t o_Reset = nullptr;

const void* rtv_resource(SIZE_T ptr) {
    auto it = g_rtv.find(ptr);
    return it == g_rtv.end() ? nullptr : it->second;
}

std::atomic<BindTapFn> g_bind_tap[5];
std::atomic<RtvSubstFn> g_rtv_subst{nullptr};
std::atomic<FrameSizeFn> g_frame_size{nullptr};
std::atomic<bool> g_present_unsynced{false};
std::atomic<uint64_t> g_unsynced_presents{0};

void STDMETHODCALLTYPE hk_OMSetRenderTargets(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts,
                                             BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
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
    if (RtvSubstFn sub = g_rtv_subst.load(std::memory_order_relaxed); sub && n >= 1 && n <= 8 && rts && !single) {
        D3D12_CPU_DESCRIPTOR_HANDLE repl[8];
        if (sub(cl, n, rts, ds, &repl[0])) {
            for (UINT i = 1; i < n; ++i) repl[i] = rts[i];
            o_OMSetRenderTargets(cl, n, repl, single, ds);
            return;
        }
    }
    o_OMSetRenderTargets(cl, n, rts, single, ds);
}

thread_local uint64_t t_draws = 0, t_queries = 0;

void STDMETHODCALLTYPE hk_DrawInstanced(ID3D12GraphicsCommandList* cl, UINT a, UINT b, UINT c, UINT d) {
    if (pso::on_draw(cl)) return;
    ++t_draws;
    fg_count(cl, [](ListLog& l) { ++l.draws; });
    pso::on_draw_args(cl, c, a);
    o_DrawInstanced(cl, a, b, c, d);
}
void STDMETHODCALLTYPE hk_DrawIndexedInstanced(ID3D12GraphicsCommandList* cl, UINT a, UINT b, UINT c, INT d, UINT e) {
    if (pso::on_draw(cl)) return;
    ++t_draws;
    fg_count(cl, [](ListLog& l) { ++l.draws; });
    pso::on_draw_args(cl, d, a);
    o_DrawIndexedInstanced(cl, a, b, c, d, e);
}
void STDMETHODCALLTYPE hk_Dispatch(ID3D12GraphicsCommandList* cl, UINT x, UINT y, UINT z) {
    fg_count(cl, [](ListLog& l) { ++l.dispatches; });
    o_Dispatch(cl, x, y, z);
}
void STDMETHODCALLTYPE hk_ExecuteIndirect(ID3D12GraphicsCommandList* cl, ID3D12CommandSignature* s, UINT n, ID3D12Resource* a,
                                          UINT64 b, ID3D12Resource* c, UINT64 d) {
    fg_count(cl, [](ListLog& l) { ++l.indirect; });
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
};
Watch g_watch[kWatchSlots];

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
    fg_count(cl, [n](ListLog& l) { l.barriers += n; });
    watch_barriers(cl, n, b);
    o_ResourceBarrier(cl, n, b);
}
HRESULT STDMETHODCALLTYPE hk_Close(ID3D12GraphicsCommandList* cl) {
    watch_list_ends(cl);
    return o_Close(cl);
}
HRESULT STDMETHODCALLTYPE hk_Reset(ID3D12GraphicsCommandList* cl, ID3D12CommandAllocator* a, ID3D12PipelineState* ps) {
    watch_list_ends(cl);
    return o_Reset(cl, a, ps);
}
void STDMETHODCALLTYPE hk_EndQuery(ID3D12GraphicsCommandList* cl, ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT i) {
    ++t_queries;
    fg_count(cl, [](ListLog& l) { ++l.queries; });
    o_EndQuery(cl, h, t, i);
}
void STDMETHODCALLTYPE hk_CopyResource(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, ID3D12Resource* src) {
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
    o_CopyResource(cl, dst, src);
    for (auto& t : g_copy_tap)
        if (CopyTapFn tap = t.load(std::memory_order_relaxed)) tap(cl, dst, src);
}
void STDMETHODCALLTYPE hk_CopyTextureRegion(ID3D12GraphicsCommandList* cl, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y,
                                            UINT z, const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
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
    o_CopyTextureRegion(cl, dst, x, y, z, src, box);
}
void STDMETHODCALLTYPE hk_ClearRTV(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT c[4], UINT n,
                                   const D3D12_RECT* r) {
    if (g_fg_armed.load(std::memory_order_relaxed)) {
        char a[32];
        std::string name;
        {
            std::lock_guard lock(g_names_mutex);
            name = res_or_hex(rtv_resource(h.ptr), a, sizeof(a));
        }
        fg_event(cl, "ClearRTV %s (%.2f %.2f %.2f %.2f)", name.c_str(), c[0], c[1], c[2], c[3]);
    }
    o_ClearRTV(cl, h, c, n, r);
}
void STDMETHODCALLTYPE hk_ClearDSV(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE h, D3D12_CLEAR_FLAGS f, FLOAT d,
                                   UINT8 s, UINT n, const D3D12_RECT* r) {
    if (g_fg_armed.load(std::memory_order_relaxed)) {
        char a[32];
        std::string name;
        {
            std::lock_guard lock(g_names_mutex);
            name = res_or_hex(rtv_resource(h.ptr), a, sizeof(a));
        }
        fg_event(cl, "ClearDSV %s depth %.3f", name.c_str(), d);
    }
    o_ClearDSV(cl, h, f, d, s, n, r);
}
void STDMETHODCALLTYPE hk_BeginEvent(ID3D12GraphicsCommandList* cl, UINT meta, const void* data, UINT size) {
    if (g_fg_armed.load(std::memory_order_relaxed) && data && size) {
        // PIX metadata 0 = Unicode string, 1 = ANSI string; PIX3 events are encoded (logged as size only).
        if (meta == 1) fg_event(cl, "BeginEvent \"%.*s\"", static_cast<int>(size), static_cast<const char*>(data));
        else if (meta == 0) fg_event(cl, "BeginEvent \"%ls\"", static_cast<const wchar_t*>(data));
        else fg_event(cl, "BeginEvent meta%u size%u", meta, size);
    }
    o_BeginEvent(cl, meta, data, size);
}
void STDMETHODCALLTYPE hk_SetPredication(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, UINT64 off, D3D12_PREDICATION_OP op) {
    fg_count(cl, [](ListLog& l) { ++l.queries; });
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
    if (SUCCEEDED(hr) && out && *out && hp && hp->Type == D3D12_HEAP_TYPE_UPLOAD && ring_probe::enabled()) {
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
    if (SUCCEEDED(hr) && out && *out && heap && ring_probe::enabled() && heap->GetDesc().Properties.Type == D3D12_HEAP_TYPE_UPLOAD) {
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
        if (g_rtv.size() < 2000000) g_rtv[h.ptr] = res;
    }
    o_CreateRTV(dev, res, desc, h);
}

void STDMETHODCALLTYPE hk_CreateDSV(ID3D12Device* dev, ID3D12Resource* res, const void* desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    {
        std::lock_guard lock(g_names_mutex);
        if (g_rtv.size() < 2000000) g_rtv[h.ptr] = res;
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
    hooks::install("CL::SetPredication", hooks::vtable_entry(cl, 55), hk_SetPredication, &o_SetPredication);
    hooks::install("CL::BeginEvent", hooks::vtable_entry(cl, 57), hk_BeginEvent, &o_BeginEvent);
    hooks::install("CL::ExecuteIndirect", hooks::vtable_entry(cl, 59), hk_ExecuteIndirect, &o_ExecuteIndirect);
    cl->Close();
    cl->Release();
    alloc->Release();
}

void hook_device_methods(ID3D12Device* dev) {
    std::call_once(g_device_once, [dev] {
        hooks::install("ID3D12Device::CreateCommandQueue", hooks::vtable_entry(dev, 8), hk_CreateCommandQueue,
                       &o_CreateCommandQueue);
        hooks::install("ID3D12Device::CreateDescriptorHeap", hooks::vtable_entry(dev, 14), hk_CreateDescHeap, &o_CreateDescHeap);
        ring_probe::install_device_hooks(dev);
        if (config::get_bool("Debug", "FrameGraph", true)) pso::install_device_hooks(dev);
        bool log_resources = config::get_bool("Debug", "LogResources", true);
        if (!log_resources && !ring_probe::enabled()) return;
        hooks::install("ID3D12Device::CreateRenderTargetView", hooks::vtable_entry(dev, 20), hk_CreateRTV, &o_CreateRTV);
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

HRESULT WINAPI hk_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void** out) {
    if (out && !state::dred_enabled && config::get_bool("Debug", "Dred", true)) enable_dred();
    HRESULT hr = o_D3D12CreateDevice(adapter, fl, riid, out);
    log::info("[d3d] D3D12CreateDevice(adapter %p, fl %#x, %s) -> %#lx", static_cast<void*>(adapter), static_cast<unsigned>(fl),
              out ? "create" : "probe", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && out && *out) {
        // Hooks only. The game creates and destroys a first device (with a 120x61 swapchain) before its real one
        // (ENGINE-NOTES 3.1), so the device the core uses is taken, with a reference, from the swapchain's queue.
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&dev))) && dev) {
            log::info("[d3d] device %p created", static_cast<void*>(dev));
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
    int c = g_end_count.load();
    for (int i = 0; i < c; ++i) g_end_listeners[i](frame);
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

HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (t_in_present || (flags & DXGI_PRESENT_TEST)) return o_Present(sc, sync, flags);
    ++t_in_present;
    on_frame_end();
    if (sync && g_present_unsynced.load(std::memory_order_relaxed)) {
        sync = 0;
        g_unsynced_presents.fetch_add(1, std::memory_order_relaxed);
    }
    HRESULT hr = o_Present(sc, sync, flags);
    after_present(hr);
    --t_in_present;
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (t_in_present || (flags & DXGI_PRESENT_TEST)) return o_Present1(sc, sync, flags, p);
    ++t_in_present;
    on_frame_end();
    if (sync && g_present_unsynced.load(std::memory_order_relaxed)) {
        sync = 0;
        g_unsynced_presents.fetch_add(1, std::memory_order_relaxed);
    }
    HRESULT hr = o_Present1(sc, sync, flags, p);
    after_present(hr);
    --t_in_present;
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT f, UINT flags) {
    log::info("[dxgi] ResizeBuffers(count %u, %ux%u, fmt %d, flags %#x)", count, w, h, static_cast<int>(f), flags);
    if (FrameSizeFn fs = g_frame_size.load(std::memory_order_acquire)) fs("ResizeBuffers", w, h);
    HRESULT hr = o_ResizeBuffers(sc, count, w, h, f, flags);
    if (SUCCEEDED(hr)) {
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
    HRESULT hr = o_CreateSwapChainForHwnd(f, dev, hwnd, desc, fs, out, sc);
    log::info("[dxgi] CreateSwapChainForHwnd(factory %p, queue %p, hwnd %p) -> %#lx", static_cast<void*>(f),
              static_cast<void*>(dev), static_cast<void*>(hwnd), static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && sc && *sc) on_swapchain(dev, hwnd, *sc);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** sc) {
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

bool install_startup_hooks() {
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
bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT) return false;
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

void set_present_unsynced(bool on) {
    if (g_present_unsynced.exchange(on) != on)
        log::info("[d3d] Present sync interval: %s", on ? "0 (paced by the XR frame loop)" : "the game's");
}

void set_bind_tap(int slot, BindTapFn fn) {
    if (slot >= 0 && slot < 5) g_bind_tap[slot] = fn;
}

void set_frame_size_listener(FrameSizeFn fn) { g_frame_size.store(fn, std::memory_order_release); }



uint64_t thread_draws() { return t_draws; }
uint64_t thread_queries() { return t_queries; }

}  // namespace rdrvr::d3d
