#include "core/prepass_arrays.h"

#include <windows.h>
#include <d3d12.h>

#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/anchors.h"
#include "core/config.h"
#include "core/hooks.h"
#include "core/d3d_hooks.h"
#include "core/dual_pass.h"
#include "core/log.h"
#include "core/state.h"

namespace rdrvr::prepass {
namespace {

enum Kind { kDepth, kCollector, kVelocity, kGrassColour, kGrassLight, kTree, kKinds };
const char* const kNames[kKinds] = {"Shadow Collector Depth Buffer", "Shadow Collector",        "Velocity RT",
                                    "Grass FS Colour",               "Grass FS Light Multiply", "Tree FS Colour Diffuse"};

std::atomic<bool> g_on{false}, g_active{false}, g_failed{false}, g_hooked{false};
std::atomic<bool> g_views{false}, g_views_write{false};  // [Stereo] PrepassViews: 1 observe, 2 also write eye 2's rows
std::atomic<bool> g_end_pending{false};  // the walk's end marker played back: the next list call ends the region
std::atomic<uint64_t> g_markers{0}, g_marker_ends{0};
std::atomic<uint64_t> g_trace_until{0};  // the trace runs while state::presents is below it
std::atomic<uint64_t> g_regions{0}, g_copies{0}, g_redirects{0}, g_skipped{0};
char g_fail_reason[160] = "";

struct Arr {
    ID3D12Resource* game = nullptr;  // the game's target it stands in for (no reference held)
    UINT64 w = 0;
    UINT h = 0;
    DXGI_FORMAT res_fmt = DXGI_FORMAT_UNKNOWN, view_fmt = DXGI_FORMAT_UNKNOWN;
    ID3D12Resource* arr = nullptr;  // 2 slices
    UINT planes = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE view[2]{};
};
Arr g_arr[kKinds];
struct Retired {
    ID3D12Resource* res;
    uint64_t frame;
};
std::vector<Retired> g_retired;
ID3D12Device* g_dev = nullptr;
ID3D12DescriptorHeap* g_rtv_heap = nullptr;  // 2 per colour kind
ID3D12DescriptorHeap* g_dsv_heap = nullptr;  // 2
UINT g_rtv_inc = 0, g_dsv_inc = 0;

// the open region (the playback thread only)
struct Region {
    ID3D12GraphicsCommandList* cl = nullptr;
    int slice = 0;
    unsigned touched = 0, known = 0;   // bit (kind * 2 + plane): that plane's state known
    D3D12_RESOURCE_STATES state[kKinds][2]{};  // per plane (the depth's two: the game moves them one at a time)
} g_r;
unsigned plane_bits(int k) { return (g_arr[k].planes > 1 ? 3u : 1u) << (k * 2); }
void set_state(int k, int plane, D3D12_RESOURCE_STATES s) {  // plane -1: every plane
    for (int p = 0; p < 2; ++p)
        if (plane < 0 || plane == p) {
            g_r.state[k][p] = s;
            g_r.known |= 1u << (k * 2 + p);
        }
}
uint64_t g_frame_seen = ~0ull;
int g_pass_in_frame = 0;
struct LastBind {  // the list's last bind outside a region (the playback thread)
    ID3D12GraphicsCommandList* cl = nullptr;
    unsigned n = 0;
    bool single = false, has_ds = false;
    D3D12_CPU_DESCRIPTOR_HANDLE rts[8]{}, ds{};
} g_last;

bool is_depth(int k) { return k == kDepth; }
D3D12_RESOURCE_STATES write_state(int k) { return is_depth(k) ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET; }

void fail(const char* why) {
    if (!g_failed.exchange(true)) {
        std::snprintf(g_fail_reason, sizeof(g_fail_reason), "%s", why);
        log::warn("[prepass] stood down: %s (the game's own targets from now on)", why);
    }
}

// The kind of a game view, by its resource's name; -1 when not one of the six.
int kind_of(D3D12_CPU_DESCRIPTOR_HANDLE h, ID3D12Resource** res, DXGI_FORMAT* fmt) {
    int f = 0;
    const void* r = d3d::view_resource(h.ptr, &f);
    *fmt = static_cast<DXGI_FORMAT>(f);
    if (!r) return -1;
    const char* name = d3d::resource_name(r);
    if (!name) return -1;
    for (int k = 0; k < kKinds; ++k)
        if (std::strcmp(name, kNames[k]) == 0) {
            *res = static_cast<ID3D12Resource*>(const_cast<void*>(r));
            return k;
        }
    return -1;
}
int kind_of_resource(const ID3D12Resource* res) {
    for (int k = 0; k < kKinds; ++k)
        if (g_arr[k].game && g_arr[k].game == res) return k;
    return -1;
}

bool heaps() {
    if (g_rtv_heap && g_dsv_heap) return true;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 * kKinds, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtv_heap)))) return false;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 2;
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_dsv_heap)))) return false;
    g_rtv_inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_dsv_inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    return true;
}

// The array standing in for this game target (made, or made again when the game re-made its own).
bool ensure(int k, ID3D12Resource* res, DXGI_FORMAT view_fmt) {
    Arr& a = g_arr[k];
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    if (a.arr && a.game == res && a.w == d.Width && a.h == d.Height && a.res_fmt == d.Format) return true;
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.DepthOrArraySize != 1 || d.MipLevels != 1 || d.SampleDesc.Count != 1) {
        fail("a pre-pass target is not a single 2D texture");
        return false;
    }
    if (!g_dev && FAILED(res->GetDevice(IID_PPV_ARGS(&g_dev)))) return false;
    if (!heaps()) {
        fail("no descriptor heaps");
        return false;
    }
    if (a.arr) g_retired.push_back({a.arr, state::presents.load()});  // the GPU may still use it: released later
    a = Arr{};
    D3D12_RESOURCE_DESC ad = d;
    ad.DepthOrArraySize = 2;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    const DXGI_FORMAT vf = view_fmt != DXGI_FORMAT_UNKNOWN ? view_fmt : d.Format;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = vf;
    cv.DepthStencil = {1.0f, 0};
    ID3D12Resource* arr = nullptr;
    const HRESULT hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ad, write_state(k), is_depth(k) ? &cv : nullptr,
                                                      IID_PPV_ARGS(&arr));
    if (FAILED(hr) || !arr) {
        char why[96];
        std::snprintf(why, sizeof(why), "the %s array not made (%#lx)", kNames[k], static_cast<unsigned long>(hr));
        fail(why);
        return false;
    }
    D3D12_FEATURE_DATA_FORMAT_INFO fi{d.Format, 0};
    a.planes = SUCCEEDED(g_dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &fi, sizeof(fi))) && fi.PlaneCount ? fi.PlaneCount : 1;
    for (int s = 0; s < 2; ++s) {
        if (is_depth(k)) {
            D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
            dv.Format = vf;
            dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            dv.Texture2DArray = {0, static_cast<UINT>(s), 1};
            a.view[s] = g_dsv_heap->GetCPUDescriptorHandleForHeapStart();
            a.view[s].ptr += static_cast<SIZE_T>(s) * g_dsv_inc;
            g_dev->CreateDepthStencilView(arr, &dv, a.view[s]);
        } else {
            D3D12_RENDER_TARGET_VIEW_DESC rv{};
            rv.Format = vf;
            rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rv.Texture2DArray = {0, static_cast<UINT>(s), 1, 0};
            a.view[s] = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
            a.view[s].ptr += static_cast<SIZE_T>(k * 2 + s) * g_rtv_inc;
            g_dev->CreateRenderTargetView(arr, &rv, a.view[s]);
        }
    }
    a.game = res;
    a.w = d.Width;
    a.h = d.Height;
    a.res_fmt = d.Format;
    a.view_fmt = vf;
    a.arr = arr;
    wchar_t nm[96];
    std::swprintf(nm, 96, L"RDRVR prepass %S", kNames[k]);
    arr->SetName(nm);
    log::info("[prepass] %s: a 2-slice array %llux%u (format %d, view %d, %u plane(s))", kNames[k], static_cast<unsigned long long>(d.Width),
              d.Height, static_cast<int>(d.Format), static_cast<int>(vf), a.planes);
    return true;
}

void release_retired() {
    const uint64_t now = state::presents.load();
    for (size_t i = 0; i < g_retired.size();)
        if (now > g_retired[i].frame + 4) {
            g_retired[i].res->Release();
            g_retired[i] = g_retired.back();
            g_retired.pop_back();
        } else {
            ++i;
        }
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    return b;
}

// The region's end: each touched target's slice copied into the game's target, from the states seen (per plane).
void end_region(ID3D12GraphicsCommandList* cl) {
    g_active = false;
    g_end_pending = false;
    unsigned need = 0;
    for (int k = 0; k < kKinds; ++k)
        if (g_r.touched & (1u << k)) need |= plane_bits(k);
    if ((g_r.known & need) != need) {
        fail("a pre-pass target's state not known at the region's end");
        return;
    }
    D3D12_RESOURCE_BARRIER pre[3 * kKinds], post[3 * kKinds];
    UINT n = 0;
    for (int k = 0; k < kKinds; ++k) {
        if (!(g_r.touched & (1u << k))) continue;
        const Arr& a = g_arr[k];
        pre[n] = transition(a.arr, write_state(k), D3D12_RESOURCE_STATE_COPY_SOURCE);
        post[n] = transition(a.arr, D3D12_RESOURCE_STATE_COPY_SOURCE, write_state(k));
        ++n;
        for (UINT p = 0; p < a.planes && p < 2; ++p) {
            const D3D12_RESOURCE_STATES st = g_r.state[k][p];
            if (st == D3D12_RESOURCE_STATE_COPY_DEST) {
                fail("a pre-pass target already in COPY_DEST");
                return;
            }
            pre[n] = transition(a.game, st, D3D12_RESOURCE_STATE_COPY_DEST);
            post[n] = transition(a.game, D3D12_RESOURCE_STATE_COPY_DEST, st);
            if (a.planes > 1) pre[n].Transition.Subresource = post[n].Transition.Subresource = p;  // (one mip, one slice)
            ++n;
        }
    }
    if (!n) return;
    d3d::raw_barrier(cl, n, pre);
    for (int k = 0; k < kKinds; ++k) {
        if (!(g_r.touched & (1u << k))) continue;
        const Arr& a = g_arr[k];
        for (UINT p = 0; p < a.planes; ++p) {
            D3D12_TEXTURE_COPY_LOCATION dst{a.game, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            dst.SubresourceIndex = p;
            D3D12_TEXTURE_COPY_LOCATION src{a.arr, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            src.SubresourceIndex = static_cast<UINT>(g_r.slice) + p * 2;  // mip + slice * mips + plane * mips * slices
            d3d::raw_copy_region(cl, &dst, &src);
        }
        g_copies.fetch_add(1, std::memory_order_relaxed);
    }
    d3d::raw_barrier(cl, n, post);
    g_marker_ends.fetch_add(1, std::memory_order_relaxed);
}

// ---- the walk's end, recorded on the render thread: FUN_1405cbfd0 calls the letterbox clear right after the walk
// (its other callers return elsewhere). An op-0x4a callback there (DeferredAppend, as the DLSS work records its own)
// runs on the playback thread at that point of the stream.
using Letterbox_t = void (*)(void* renderer);
Letterbox_t o_letterbox = nullptr;
using Append_t = void (*)(void* recorder, void* fn, const void* data, uint64_t size);

template <class T>
bool rd(uintptr_t a, T* out) {
    if (!a) return false;
    __try {
        *out = *reinterpret_cast<const T*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void walk_end_cb(void*) { g_end_pending.store(true, std::memory_order_release); }

// ---- [Stereo] PrepassViews, step 1 (observe): the render thread only
struct Rows {
    float wv[16], vi[16], pvp[16], vport[4];  // gWorldView, gViewInverse (b1), gPrevViewProj (rage_prev_proj), gViewport
    bool ok = false;
};
Rows g_seen[2];          // after the walk, pass 1 and pass 2 (the game's own)
float g_e2_wv[16], g_e2_vi[16], g_e2_pvp[16];  // pass 1's prediction of eye 2's
bool g_e2_ok = false, g_e2_pvp_ok = false;
uint64_t g_e2_frame = ~0ull;
struct Stats {
    uint64_t frames = 0, refused = 0;
    float wv = 0, vi = 0, eyes_wv = 0, eyes_vi = 0, pvp = 0, vport = 0, e2_pvp = 0;  // the largest differences
    uint64_t pvp_frames = 0, bad_wv = 0, bad_vi = 0, bad_pvp = 0, bad_first = 0;
} g_vs;
std::atomic<uint64_t> g_vs_frames{0};
char g_vs_text[700] = "no frame compared yet";

bool read_raw(uintptr_t a, void* out, size_t n) {
    if (!a) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// A shader global's 16 x n bytes from its constant buffer's storage (cb +0x00), by its handle's table entry.
bool global_rows(anchors::Id handle, float* out, size_t bytes, uintptr_t* cb_out = nullptr) {
    int h = 0;
    uintptr_t cb = 0, storage = 0;
    int off = -1;
    const uintptr_t table = anchors::addr(anchors::Id::ShaderGlobalTable);
    if (!anchors::present(handle) || !rd(anchors::addr(handle), &h) || h <= 0 || h > 4096 || !rd(table + static_cast<uintptr_t>(h) * 16, &cb) ||
        !rd(table + static_cast<uintptr_t>(h) * 16 + 8, &off) || off < 0 || off > 0x1000 || !rd(cb, &storage))
        return false;
    if (cb_out) *cb_out = cb;
    return read_raw(storage + static_cast<uintptr_t>(off), out, bytes);
}
// ---- [Stereo] PrepassViews=2, step 2: eye 2's rows written past the declared ends of rage_matrices (b1) and
// rage_prev_proj (stageVI-prepass.md, milestone 3): each buffer's storage swapped once for a larger block from the
// game's own allocator (ucrtbase's malloc, so the game's free stays valid; the old block never freed), then its declared
// size (layout +0x14) grown so the game's own upload carries the rows. The upload's rounded size (256) does not change.
template <class T>
bool wr(uintptr_t a, T v) {
    if (!a) return false;
    __try {
        *reinterpret_cast<T*>(a) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool write_raw(uintptr_t a, const void* src, size_t n) {
    if (!a) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(a), src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool global_cb(anchors::Id handle, uintptr_t* cb, int* off) {
    int h = 0;
    const uintptr_t table = anchors::addr(anchors::Id::ShaderGlobalTable);
    return anchors::present(handle) && rd(anchors::addr(handle), &h) && h > 0 && h <= 4096 &&
           rd(table + static_cast<uintptr_t>(h) * 16, cb) && *cb && rd(table + static_cast<uintptr_t>(h) * 16 + 8, off);
}
struct Grown {
    uintptr_t cb = 0, block = 0;
    uint32_t declared = 0, grown = 0;
};
Grown g_b1, g_pp;           // rage_matrices, rage_prev_proj
int g_grow_state = 0;       // 0 not tried, 1 grown, -1 refused
char g_grow_why[160] = "";
uint64_t g_written = 0, g_kept = 0, g_lost = 0;
float g_written_wv[16], g_written_vi[16], g_written_pp[16];

bool grow(Grown& g, uintptr_t cb, uint32_t declared, uint32_t grown, uint32_t alloc) {
    using Malloc_t = void* (*)(size_t);
    static const auto game_malloc = reinterpret_cast<Malloc_t>(GetProcAddress(GetModuleHandleW(L"ucrtbase.dll"), "malloc"));
    uintptr_t layout = 0, old = 0, mapped = 0;
    uint32_t size = 0;
    if (!game_malloc || !rd(cb + 0x08, &layout) || !rd(layout + 0x14, &size) || size != declared || !rd(cb, &old) || !old || !rd(cb + 0x18, &mapped))
        return false;
    if (((declared + 0xffu) & ~0xffu) != ((grown + 0xffu) & ~0xffu)) return false;  // the upload's size must not change
    void* blk = game_malloc(alloc);
    if (!blk) return false;
    std::memset(blk, 0, alloc);
    if (!read_raw(old, blk, declared)) return false;  // (the block leaks: the game never frees it either way)
    if (!wr<uintptr_t>(cb, reinterpret_cast<uintptr_t>(blk))) return false;
    if (mapped == old) wr<uintptr_t>(cb + 0x18, reinterpret_cast<uintptr_t>(blk));
    if (!wr<uint32_t>(layout + 0x14, grown)) {
        wr<uintptr_t>(cb, old);  // back as it was
        if (mapped == old) wr<uintptr_t>(cb + 0x18, old);
        return false;
    }
    g = Grown{cb, reinterpret_cast<uintptr_t>(blk), declared, grown};
    return true;
}
void refuse_grow(const char* why) {
    g_grow_state = -1;
    std::snprintf(g_grow_why, sizeof(g_grow_why), "%s", why);
    log::warn("[prepass] views: eye 2's rows not written: %s", why);
}
bool ensure_grown() {
    if (g_grow_state) return g_grow_state > 0;
    uintptr_t cb_wv = 0, cb_vi = 0, cb_pp = 0;
    int off_wv = -1, off_vi = -1, off_pp = -1;
    if (!global_cb(anchors::Id::HandleWorldView, &cb_wv, &off_wv) || !global_cb(anchors::Id::HandleViewInverse, &cb_vi, &off_vi) ||
        !global_cb(anchors::Id::HandlePrevViewProj, &cb_pp, &off_pp)) {
        refuse_grow("a shader global not found");
        return false;
    }
    if (cb_wv != cb_vi || off_wv != 0x40 || off_vi != 0xc0 || off_pp != 0) {
        char why[160];
        std::snprintf(why, sizeof(why), "not the expected layout (b1 %#llx/%#llx, gWorldView +%#x, gViewInverse +%#x, gPrevViewProj +%#x)",
                      static_cast<unsigned long long>(cb_wv), static_cast<unsigned long long>(cb_vi), off_wv, off_vi, off_pp);
        refuse_grow(why);
        return false;
    }
    if (!grow(g_b1, cb_wv, 0x110, 0x1d0, 0x200)) {
        refuse_grow("rage_matrices not grown (its size is not 0x110, or a read failed)");
        return false;
    }
    if (!grow(g_pp, cb_pp, 0x40, 0x80, 0x100)) {
        refuse_grow("rage_prev_proj not grown (its size is not 0x40, or a read failed)");
        return false;
    }
    g_grow_state = 1;
    log::info("[prepass] views: rage_matrices %#llx grown 0x110 -> 0x1d0 (storage %#llx), rage_prev_proj %#llx 0x40 -> 0x80 (storage %#llx)",
              static_cast<unsigned long long>(g_b1.cb), static_cast<unsigned long long>(g_b1.block), static_cast<unsigned long long>(g_pp.cb),
              static_cast<unsigned long long>(g_pp.block));
    return true;
}
std::atomic<int> g_size_want{-1};  // the test's "prepass views size on|off": applied on the render thread (1 grown, 0 declared)
void apply_size_want() {
    const int w = g_size_want.exchange(-1);
    if (w < 0 || g_grow_state <= 0) return;
    for (Grown* g : {&g_b1, &g_pp}) {
        uintptr_t layout = 0;
        if (rd(g->cb + 0x08, &layout)) wr<uint32_t>(layout + 0x14, w ? g->grown : g->declared);
    }
    log::info("[prepass] views: the declared sizes %s", w ? "grown (0x1d0, 0x80)" : "as the game's (0x110, 0x40)");
}
bool still_grown(const Grown& g) {
    uintptr_t storage = 0, layout = 0;
    uint32_t size = 0;
    return rd(g.cb, &storage) && storage == g.block && rd(g.cb + 0x08, &layout) && rd(layout + 0x14, &size) && size == g.grown;
}
// pass 1, before the pre-pass: rows 17-20 (gWorldView) and 25-28 (gViewInverse) of b1, rows 4-7 of rage_prev_proj
void write_eye2(const float* wv, const float* vi, const float* pvp) {
    if (!ensure_grown()) return;
    apply_size_want();
    {  // the test's sizes back as the game's: the rows are not uploaded then, nothing to check
        uintptr_t layout = 0;
        uint32_t size = 0;
        if (rd(g_b1.cb + 0x08, &layout) && rd(layout + 0x14, &size) && size == g_b1.declared) return;
    }
    if (!still_grown(g_b1) || !still_grown(g_pp)) {
        refuse_grow("a grown buffer was made again by the game (its storage or size changed)");
        return;
    }
    float pp[16];
    if (pvp) std::memcpy(pp, pvp, sizeof(pp));
    else if (!read_raw(g_pp.block, pp, sizeof(pp))) return;  // no jitter: both eyes' gPrevViewProj are the same
    if (!write_raw(g_b1.block + 0x110, wv, 64) || !write_raw(g_b1.block + 0x190, vi, 64) || !write_raw(g_pp.block + 0x40, pp, 64)) return;
    wr<int>(g_b1.cb + 0x24, -1);  // (re-uploaded at its next commit)
    wr<int>(g_pp.cb + 0x24, -1);
    std::memcpy(g_written_wv, wv, 64);
    std::memcpy(g_written_vi, vi, 64);
    std::memcpy(g_written_pp, pp, 64);
    ++g_written;
}
// after pass 1's walk: the rows still as written (nothing else writes past the declared ends)
void check_written() {
    if (g_grow_state <= 0) return;
    {
        uintptr_t layout = 0;
        uint32_t size = 0;
        if (rd(g_b1.cb + 0x08, &layout) && rd(layout + 0x14, &size) && size == g_b1.declared) return;  // (the test's sizes off)
    }
    float a[16], b[16], c[16];
    if (still_grown(g_b1) && still_grown(g_pp) && read_raw(g_b1.block + 0x110, a, 64) && read_raw(g_b1.block + 0x190, b, 64) &&
        read_raw(g_pp.block + 0x40, c, 64) && !std::memcmp(a, g_written_wv, 64) && !std::memcmp(b, g_written_vi, 64) &&
        !std::memcmp(c, g_written_pp, 64))
        ++g_kept;
    else
        ++g_lost;
}

float max_diff(const float* a, const float* b, int n) {
    float m = 0;
    for (int i = 0; i < n; ++i) m = std::fmax(m, std::fabs(a[i] - b[i]));
    return m;
}
void observe_after_walk() {
    const int pass = dual_pass::pass();  // 1, 2 (0 mono)
    if (pass < 1 || pass > 2) return;
    if (pass == 1 && g_views_write.load(std::memory_order_relaxed)) check_written();
    Rows& r = g_seen[pass - 1];
    r.ok = global_rows(anchors::Id::HandleWorldView, r.wv, 64) && global_rows(anchors::Id::HandleViewInverse, r.vi, 64) &&
           global_rows(anchors::Id::HandlePrevViewProj, r.pvp, 64) && global_rows(anchors::Id::HandleViewport, r.vport, 16);
    if (pass != 2) return;
    const uint64_t f = state::presents.load();
    if (!g_seen[0].ok || !r.ok || !g_e2_ok || g_e2_frame != f) {
        ++g_vs.refused;
        return;
    }
    ++g_vs.frames;
    const float dwv = max_diff(g_e2_wv, r.wv, 16), dvi = max_diff(g_e2_vi, r.vi, 16);
    g_vs.wv = std::fmax(g_vs.wv, dwv);
    g_vs.vi = std::fmax(g_vs.vi, dvi);
    if (dwv > 1e-4f) ++g_vs.bad_wv;
    if (dvi > 1e-4f) ++g_vs.bad_vi;
    if ((dwv > 1e-4f || dvi > 1e-4f) && !g_vs.bad_first) {
        g_vs.bad_first = g_vs.frames;
        log::info("[prepass] views: the first mismatch, frame %llu of the comparison: gWorldView %.6f gViewInverse %.6f; predicted wv row 0 "
                  "(%.4f %.4f %.4f %.4f) row 3 (%.4f %.4f %.4f %.4f), the game's (%.4f %.4f %.4f %.4f) (%.4f %.4f %.4f %.4f); vi row 3 "
                  "predicted (%.3f %.3f %.3f) the game's (%.3f %.3f %.3f) pass 1's (%.3f %.3f %.3f)",
                  static_cast<unsigned long long>(g_vs.frames), dwv, dvi, g_e2_wv[0], g_e2_wv[1], g_e2_wv[2], g_e2_wv[3], g_e2_wv[12], g_e2_wv[13],
                  g_e2_wv[14], g_e2_wv[15], r.wv[0], r.wv[1], r.wv[2], r.wv[3], r.wv[12], r.wv[13], r.wv[14], r.wv[15], g_e2_vi[12], g_e2_vi[13],
                  g_e2_vi[14], r.vi[12], r.vi[13], r.vi[14], g_seen[0].vi[12], g_seen[0].vi[13], g_seen[0].vi[14]);
    }
    g_vs.eyes_wv = std::fmax(g_vs.eyes_wv, max_diff(g_seen[0].wv, r.wv, 16));
    g_vs.eyes_vi = std::fmax(g_vs.eyes_vi, max_diff(g_seen[0].vi, r.vi, 16));
    g_vs.pvp = std::fmax(g_vs.pvp, max_diff(g_seen[0].pvp, r.pvp, 16));
    g_vs.vport = std::fmax(g_vs.vport, max_diff(g_seen[0].vport, r.vport, 4));
    if (g_e2_pvp_ok) {
        ++g_vs.pvp_frames;
        const float dp = max_diff(g_e2_pvp, r.pvp, 16);
        g_vs.e2_pvp = std::fmax(g_vs.e2_pvp, dp);
        if (dp > 1e-4f) ++g_vs.bad_pvp;
    }
    g_e2_ok = false;
    if (g_vs_frames.fetch_add(1, std::memory_order_relaxed) % 600 == 0 || g_vs.frames < 3) {
        std::snprintf(g_vs_text, sizeof(g_vs_text),
                      "%llu frames compared (%llu not): eye 2 predicted in pass 1 against the game's pass 2, largest differences: "
                      "gWorldView %.6f, gViewInverse %.6f | the eyes against each other: gWorldView %.6f, gViewInverse %.6f | "
                      "pass 1 against 2: gPrevViewProj %.6f, gViewport %.6f | eye 2's gPrevViewProj predicted (%llu frames): %.6f | "
                      "frames off by more than 1e-4: gWorldView %llu, gViewInverse %llu, gPrevViewProj %llu",
                      static_cast<unsigned long long>(g_vs.frames), static_cast<unsigned long long>(g_vs.refused), g_vs.wv, g_vs.vi,
                      g_vs.eyes_wv, g_vs.eyes_vi, g_vs.pvp, g_vs.vport, static_cast<unsigned long long>(g_vs.pvp_frames), g_vs.e2_pvp,
                      static_cast<unsigned long long>(g_vs.bad_wv), static_cast<unsigned long long>(g_vs.bad_vi), static_cast<unsigned long long>(g_vs.bad_pvp));
        log::info("[prepass] views: %s", g_vs_text);
    }
}

void hk_letterbox(void* renderer) {
    if (g_views.load(std::memory_order_relaxed) &&
        reinterpret_cast<uintptr_t>(_ReturnAddress()) == anchors::addr(anchors::Id::PrepassLetterboxCall) + 5)
        observe_after_walk();
    if (g_on.load(std::memory_order_relaxed) && !g_failed.load(std::memory_order_relaxed) &&
        reinterpret_cast<uintptr_t>(_ReturnAddress()) == anchors::addr(anchors::Id::PrepassLetterboxCall) + 5) {
        uintptr_t dev = 0, hrec = 0, rec = 0, vt = 0;
        if (rd(anchors::addr(anchors::Id::D3dDeviceSingleton), &dev) && rd(dev + 0x58, &hrec) && rd(hrec + 0x128, &rec) && rd(rec, &vt) &&
            vt == anchors::addr(anchors::Id::RecorderVtbl)) {
            reinterpret_cast<Append_t>(anchors::addr(anchors::Id::DeferredAppend))(reinterpret_cast<void*>(rec),
                                                                                    reinterpret_cast<void*>(&walk_end_cb), nullptr, 0);
            g_markers.fetch_add(1, std::memory_order_relaxed);
        } else {
            log::limited("prepass.norec", 5, "[prepass] the walk's end: the recorder not found (no marker)");
        }
    }
    o_letterbox(renderer);
}

}  // namespace

void init() {
    g_on = false;  // on only once install() has its hook
}
void install() {
    const bool arrays = config::get_bool("Stereo", "PrepassArrays", false), views = config::get_int("Stereo", "PrepassViews", 0) >= 1;
    if ((!arrays && !views) || anchors::stand_down()) return;
    using anchors::Id;
    if (!anchors::present(Id::LetterboxClear) || !anchors::present(Id::PrepassLetterboxCall) || !anchors::addr(Id::DeferredAppend)) {
        log::warn("[prepass] not installed: this build of RDR.exe has no LetterboxClear/PrepassLetterboxCall");
        return;
    }
    if (!hooks::install("RDR letterbox clear (the pre-pass walk's end, PrepassArrays)", reinterpret_cast<void*>(anchors::addr(Id::LetterboxClear)),
                        reinterpret_cast<void*>(&hk_letterbox), reinterpret_cast<void**>(&o_letterbox)))
        return;
    g_hooked = true;
    g_on = arrays;
    if (arrays) log::info("[prepass] [Stereo] PrepassArrays on (test only: each eye's pre-pass drawn into 2-slice arrays, copied back at the walk's end)");
    if (views && anchors::present(Id::HandleWorldView) && anchors::present(Id::HandleViewInverse) && anchors::present(Id::HandlePrevViewProj) &&
        anchors::present(Id::HandleViewport) && anchors::present(Id::HandlePrevWorldVP)) {
        g_views = true;
        g_views_write = config::get_int("Stereo", "PrepassViews", 0) >= 2;
        log::info("[prepass] [Stereo] PrepassViews on (test only: eye 2's rows predicted in pass 1, compared with the game's pass 2%s)",
                  g_views_write.load() ? "; and written past rage_matrices' and rage_prev_proj's declared ends" : "");
    }
}
bool on() { return g_on.load(std::memory_order_relaxed) && !g_failed.load(std::memory_order_relaxed); }
bool active() { return g_active.load(std::memory_order_relaxed); }
void set_on(bool want) {
    if (want && !g_hooked.load()) {
        log::warn("[prepass] not on: start with [Stereo] PrepassArrays=1 (the walk's end hook)");
        return;
    }
    g_on = want;
    if (want) trace_start(3);
    log::info("[prepass] %s", want ? "on (from the next pre-pass)" : "off (from the next pre-pass)");
}

D3D12_CPU_DESCRIPTOR_HANDLE clear_dsv(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    ID3D12Resource* res = nullptr;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    if (kind_of(h, &res, &fmt) != kDepth) return h;
    const uint64_t f = state::presents.load();
    if (g_active.load()) {  // the depth cleared again in a region: the last one never ended
        g_active = false;
        fail("a pre-pass region did not end before the next pre-pass");
        return h;
    }
    if (f != g_frame_seen) {
        g_frame_seen = f;
        g_pass_in_frame = 0;
        release_retired();
    }
    const int slice = g_pass_in_frame++;
    if (slice > 1) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return h;
    }
    if (!ensure(kDepth, res, fmt)) return h;
    g_r = Region{};
    g_r.cl = cl;
    g_r.slice = slice;
    g_r.touched = 1u << kDepth;
    set_state(kDepth, -1, D3D12_RESOURCE_STATE_DEPTH_WRITE);  // (a depth clear needs it)
    g_end_pending = false;
    g_active = true;
    g_regions.fetch_add(1, std::memory_order_relaxed);
    if (g_last.cl == cl) {  // the targets bound before this clear: bound again, the six as the slices
        D3D12_CPU_DESCRIPTOR_HANDLE r2[8], d2{};
        if (bind(cl, g_last.n, g_last.rts, g_last.single, g_last.has_ds ? &g_last.ds : nullptr, r2, &d2))
            d3d::raw_bind(cl, g_last.n, r2, g_last.single, g_last.has_ds ? &d2 : nullptr);
    }
    return g_arr[kDepth].view[slice];
}

void note_bind(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single,
               const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
    if (n > 8 || (n && !rts)) {
        g_last.cl = nullptr;
        return;
    }
    g_last.cl = cl;
    g_last.n = n;
    g_last.single = single != 0;
    for (unsigned i = 0; i < (single ? (n ? 1u : 0u) : n); ++i) g_last.rts[i] = rts[i];
    g_last.has_ds = ds != nullptr;
    if (ds) g_last.ds = *ds;
}

D3D12_CPU_DESCRIPTOR_HANDLE clear_rtv(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    if (cl != g_r.cl) return h;
    ID3D12Resource* res = nullptr;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    const int k = kind_of(h, &res, &fmt);
    if (g_end_pending.load(std::memory_order_acquire)) {
        end_region(cl);
        return h;
    }
    if (k <= kDepth || !ensure(k, res, fmt)) return h;
    g_r.touched |= 1u << k;
    set_state(k, -1, D3D12_RESOURCE_STATE_RENDER_TARGET);  // (a clear needs it)
    return g_arr[k].view[g_r.slice];
}

bool bind(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single,
          const D3D12_CPU_DESCRIPTOR_HANDLE* ds, D3D12_CPU_DESCRIPTOR_HANDLE* out_rts, D3D12_CPU_DESCRIPTOR_HANDLE* out_ds) {
    if (cl != g_r.cl || n > 8) return false;
    if (g_end_pending.load(std::memory_order_acquire)) {
        end_region(cl);
        return false;
    }
    bool any = false;
    if (single && n >= 1 && rts) {  // a contiguous range: one handle for all n (not substituted one by one)
        ID3D12Resource* res = nullptr;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        if (kind_of(rts[0], &res, &fmt) > kDepth) {
            end_region(cl);
            fail("a pre-pass target bound as a range");
        }
        return false;
    }
    for (unsigned i = 0; i < n && rts; ++i) {
        out_rts[i] = rts[i];
        ID3D12Resource* res = nullptr;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        const int k = kind_of(rts[i], &res, &fmt);
        if (k <= kDepth || !ensure(k, res, fmt)) continue;
        if (!(g_r.known & (1u << (k * 2)))) set_state(k, -1, D3D12_RESOURCE_STATE_RENDER_TARGET);  // (its barrier before the region)
        g_r.touched |= 1u << k;
        out_rts[i] = g_arr[k].view[g_r.slice];
        any = true;
    }
    if (ds) {
        *out_ds = *ds;
        ID3D12Resource* res = nullptr;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        if (kind_of(*ds, &res, &fmt) == kDepth && res == g_arr[kDepth].game) {
            *out_ds = g_arr[kDepth].view[g_r.slice];
            any = true;
        }
    }
    if (any) g_redirects.fetch_add(1, std::memory_order_relaxed);
    return any;
}

void barrier(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_RESOURCE_BARRIER* b) {
    if (cl != g_r.cl || !b) return;
    if (g_end_pending.load(std::memory_order_acquire)) {  // the walk ended: copied before this batch
        end_region(cl);
        return;
    }
    for (unsigned i = 0; i < n; ++i) {  // the six's transitions in the region: their states followed, per plane
        if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
        const int k = kind_of_resource(b[i].Transition.pResource);
        if (k < 0) continue;
        const UINT sub = b[i].Transition.Subresource;
        const int plane = sub == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? -1 : static_cast<int>(sub);
        if ((plane >= 0 && plane >= static_cast<int>(g_arr[k].planes)) || b[i].Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE) {
            g_r.known &= ~plane_bits(k);  // a split barrier or another subresource: not followed
            continue;
        }
        const int p0 = plane < 0 ? 0 : plane;
        if ((g_r.known & (1u << (k * 2 + p0))) && b[i].Transition.StateBefore != g_r.state[k][p0])
            log::limited("prepass.mismatch", 20, "[prepass] %s plane %d: the game's barrier from %#x, the region had %#x", kNames[k], p0,
                         static_cast<unsigned>(b[i].Transition.StateBefore), static_cast<unsigned>(g_r.state[k][p0]));
        set_state(k, plane, b[i].Transition.StateAfter);
    }
}

void before_draw(ID3D12GraphicsCommandList* cl) {
    if (cl == g_r.cl && g_end_pending.load(std::memory_order_acquire)) end_region(cl);
}

bool views_wanted() { return g_views.load(std::memory_order_relaxed); }
void set_eye2(const void* vp2, const void* prev_vp) {
    const char* v = static_cast<const char*>(vp2);
    g_e2_ok = read_raw(reinterpret_cast<uintptr_t>(v + 0xc0), g_e2_wv, 64) && read_raw(reinterpret_cast<uintptr_t>(v + 0x140), g_e2_vi, 64);
    g_e2_pvp_ok = prev_vp && read_raw(reinterpret_cast<uintptr_t>(prev_vp), g_e2_pvp, 64);
    g_e2_frame = state::presents.load();
    if (g_e2_ok && g_views_write.load(std::memory_order_relaxed) && g_grow_state >= 0) write_eye2(g_e2_wv, g_e2_vi, g_e2_pvp_ok ? g_e2_pvp : nullptr);
}
void views_size(bool grown) { g_size_want = grown ? 1 : 0; }
std::string views_status() {
    char w[260];
    std::snprintf(w, sizeof(w), " | written: %s, %llu frames written, after the walk kept %llu, lost %llu%s%s",
                  g_grow_state > 0 ? "grown" : g_grow_state < 0 ? "refused" : "not yet", static_cast<unsigned long long>(g_written),
                  static_cast<unsigned long long>(g_kept), static_cast<unsigned long long>(g_lost), g_grow_state < 0 ? ": " : "", g_grow_why);
    return std::string(g_views.load() ? "views on: " : "views off: ") + g_vs_text + (g_views_write.load() ? w : "");
}

bool tracing() { return state::presents.load(std::memory_order_relaxed) < g_trace_until.load(std::memory_order_relaxed); }
void trace_start(int frames) {
    g_trace_until = state::presents.load() + 1 + static_cast<uint64_t>(frames);
    log::info("[prepass] trace: the next %d frame(s)", frames);
}
namespace {
const char* name_of_view(D3D12_CPU_DESCRIPTOR_HANDLE h) {
    const void* r = d3d::view_resource(h.ptr, nullptr);
    const char* n = r ? d3d::resource_name(r) : nullptr;
    return n ? n : "?";
}
bool six_name(const char* n) {
    for (const char* k : kNames)
        if (n && std::strcmp(n, k) == 0) return true;
    return false;
}
}  // namespace
void trace_bind(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single,
                const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
    std::string t;
    bool any = false;
    for (unsigned i = 0; i < n && rts && i < 8; ++i) {
        const char* nm = name_of_view(rts[single ? 0 : i]);
        any |= six_name(nm);
        t += (i ? ", " : "") + std::string(nm);
        if (single) break;
    }
    const char* dn = ds ? name_of_view(*ds) : "-";
    any |= six_name(dn);
    if (!any) return;
    log::info("[prepass] trace frame %llu draws %llu: bind [%s | DS %s]%s%s", static_cast<unsigned long long>(state::presents.load()),
              static_cast<unsigned long long>(d3d::thread_draws()), t.c_str(), dn, single ? " (range)" : "", g_active.load() ? " in a region" : "");
    (void)cl;
}
void trace_barrier(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_RESOURCE_BARRIER* b) {
    for (unsigned i = 0; i < n && b; ++i) {
        if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
        const char* nm = d3d::resource_name(b[i].Transition.pResource);
        if (!six_name(nm)) continue;
        log::info("[prepass] trace frame %llu draws %llu: barrier %s sub %#x: %#x -> %#x flags %u%s",
                  static_cast<unsigned long long>(state::presents.load()), static_cast<unsigned long long>(d3d::thread_draws()), nm,
                  b[i].Transition.Subresource, static_cast<unsigned>(b[i].Transition.StateBefore),
                  static_cast<unsigned>(b[i].Transition.StateAfter), static_cast<unsigned>(b[i].Flags), g_active.load() ? " in a region" : "");
    }
    (void)cl;
}
void trace_clear(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE h, bool depth) {
    const char* nm = name_of_view(h);
    if (!six_name(nm)) return;
    log::info("[prepass] trace frame %llu draws %llu: clear %s %s%s", static_cast<unsigned long long>(state::presents.load()),
              static_cast<unsigned long long>(d3d::thread_draws()), depth ? "DSV" : "RTV", nm, g_active.load() ? " in a region" : "");
    (void)cl;
}

std::string status() {
    char b[520];
    std::snprintf(b, sizeof(b), "prepass %s%s%s: regions %llu, walk-end markers %llu, ended by them %llu, copies %llu, redirected binds %llu, passes past two %llu%s%s",
                  g_on.load() ? "on" : "off", g_failed.load() ? " (stood down)" : "", g_active.load() ? " (in a region)" : "",
                  static_cast<unsigned long long>(g_regions.load()), static_cast<unsigned long long>(g_markers.load()),
                  static_cast<unsigned long long>(g_marker_ends.load()), static_cast<unsigned long long>(g_copies.load()),
                  static_cast<unsigned long long>(g_redirects.load()), static_cast<unsigned long long>(g_skipped.load()),
                  g_failed.load() ? ": " : "", g_failed.load() ? g_fail_reason : "");
    return b;
}

}  // namespace rdrvr::prepass
