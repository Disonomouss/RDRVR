#include "core/lum_check.h"

#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "core/anchors.h"
#include "core/d3d_hooks.h"
#include "core/dual_pass.h"
#include "core/log.h"
#include "core/post_target.h"
#include "core/state.h"

namespace rdrvr::lum_check {
namespace {

// The post output target: the Post FXAA Target under FXAA, FXAATarget otherwise (post_target.h).
const char* const kLumName[2] = {"Adapted Lum A 1x1 RT", "Adapted Lum B 1x1 RT"};
constexpr int kMaxFrames = 30;
constexpr int kMaxSnaps = 3 * kMaxFrames;  // FXAA binds recorded: 2 per double frame, 1 per mono frame
constexpr UINT64 kSlotBytes = 512;         // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT

struct Snap {
    int frame = 0, bind = 0;  // frame index counted from the first UI bind; FXAA bind within that frame (1, 2, ...)
    bool valid[2] = {};
    uint32_t state[2] = {};
};

std::mutex g_mutex;  // one check at a time
HANDLE g_done = nullptr;
std::string g_result;
// 0 idle, 1 armed (set up at the next frame end), 2 waiting for a UI bind to start, 3 recording, 4 recorded (one more
// frame end, then finish), 5 finish
std::atomic<int> g_phase{0};
int g_frames = 8, g_frames_waited = 0;  // presenting thread
ID3D12Resource* g_lum[2] = {};
DXGI_FORMAT g_lum_fmt[2] = {};  // as armed
ID3D12Resource* g_rb = nullptr;
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_fp[2]{};
uint64_t g_rtv[8];
int g_nrtv = 0;
Snap g_snap[kMaxSnaps];
int g_nsnap = 0, g_frame_i = 0, g_binds = 0;  // recording thread
int g_frame_binds[kMaxFrames];
uint64_t g_hist0[4] = {};  // dual_pass's adaptation draws per double frame, at arm
ID3D12Fence* g_fence = nullptr;
uint64_t g_fence_value = 0;
HANDLE g_fence_event = nullptr;

// Recording thread: copy adapted luminance target r into snapshot s's slot, from the state the watch saw in this list.
void copy_one(ID3D12GraphicsCommandList* cl, int s, int r) {
    ID3D12Resource* res = g_lum[r];
    ID3D12GraphicsCommandList* wcl = nullptr;
    uint32_t st = 0;
    if (!d3d::watched_state(res, &wcl, &st) || wcl != cl) return;  // not seen in this list: not measured
    // the slot's footprint is a 1x1 target's: never copied from one the game made again since the arm at another size
    // (live: its barrier is in this list)
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    if (d.Width != 1 || d.Height != 1 || d.Format != g_lum_fmt[r]) return;
    const auto state = static_cast<D3D12_RESOURCE_STATES>(st);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bool transition = state != D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (transition) cl->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
    dl.pResource = g_rb;
    dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dl.PlacedFootprint = g_fp[r];
    dl.PlacedFootprint.Offset = static_cast<UINT64>(s * 2 + r) * kSlotBytes;
    sl.pResource = res;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sl.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    if (transition) {
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cl->ResourceBarrier(1, &b);
    }
    g_snap[s].valid[r] = true;
    g_snap[s].state[r] = st;
}

void tap(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int,
         const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
    int ph = g_phase.load(std::memory_order_acquire);
    if ((ph != 2 && ph != 3) || n == 0 || !rts) return;
    bool target = false;
    for (int i = 0; i < g_nrtv; ++i) target = target || rts[0].ptr == g_rtv[i];
    if (!target) return;
    bool ui = ds != nullptr;
    if (ph == 2) {  // start at a frame boundary
        if (ui) {
            g_frame_i = g_binds = g_nsnap = 0;
            g_phase.store(3, std::memory_order_release);
        }
        return;
    }
    if (!ui) {
        ++g_binds;
        if (g_nsnap < kMaxSnaps) {
            g_snap[g_nsnap] = Snap{};
            g_snap[g_nsnap].frame = g_frame_i;
            g_snap[g_nsnap].bind = g_binds;
            for (int r = 0; r < 2; ++r) copy_one(cl, g_nsnap, r);
            ++g_nsnap;
        }
        return;
    }
    g_frame_binds[g_frame_i] = g_binds;
    g_binds = 0;
    if (++g_frame_i >= g_frames) g_phase.store(4, std::memory_order_release);
}

// Presenting thread: find the targets and their views, watch the two adapted luminance targets, make the readback.
std::string arm() {
    char* postfx = *reinterpret_cast<char**>(anchors::addr(anchors::Id::PostFxSingleton));
    if (!postfx) return "ERROR no PostFx object (NOT MEASURED)";
    const char* tname = post_target::name(postfx);
    void* obj = *reinterpret_cast<void**>(postfx + post_target::field(postfx));
    char where[64] = "?";
    const void* target = d3d::resource_in_object(obj, 0x100, tname, where, sizeof(where));
    if (!target) target = d3d::unique_resource(tname);
    if (!target) return std::string("ERROR no single live resource named \"") + tname + "\" (NOT MEASURED)";
    g_nrtv = d3d::rtv_handles_of(target, g_rtv, 8);
    if (g_nrtv == 0) return std::string("ERROR no render-target view of ") + tname + " seen (NOT MEASURED)";
    for (int r = 0; r < 2; ++r) {
        auto* res = static_cast<ID3D12Resource*>(const_cast<void*>(d3d::unique_resource(kLumName[r])));
        if (!res) return std::string("ERROR no single live resource named \"") + kLumName[r] + "\" (NOT MEASURED)";
        D3D12_RESOURCE_DESC d = res->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width != 1 || d.Height != 1 || d.MipLevels != 1 ||
            d.DepthOrArraySize != 1 || d.SampleDesc.Count != 1)
            return std::string("ERROR \"") + kLumName[r] + "\" is not a 1x1 single-subresource texture (NOT MEASURED)";
        // The chain's pair (PostFx+0x80/+0x88, swapped every run) must hold these two.
        char w0[64], w1[64];
        if (!d3d::resource_in_object(*reinterpret_cast<void**>(postfx + 0x80), 0x100, kLumName[r], w0, sizeof(w0)) &&
            !d3d::resource_in_object(*reinterpret_cast<void**>(postfx + 0x88), 0x100, kLumName[r], w1, sizeof(w1)))
            return std::string("ERROR \"") + kLumName[r] + "\" is not behind PostFx+0x80/+0x88 (NOT MEASURED)";
        UINT rows = 0;
        UINT64 row_bytes = 0, total = 0;
        ID3D12Device* dev = nullptr;
        if (FAILED(res->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return "ERROR no device";
        dev->GetCopyableFootprints(&d, 0, 1, 0, &g_fp[r], &rows, &row_bytes, &total);
        if (r == 0) {
            if (!g_rb) {
                D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
                D3D12_RESOURCE_DESC rd{};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                rd.Width = kMaxSnaps * 2 * kSlotBytes;
                rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
                rd.SampleDesc.Count = 1;
                rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&g_rb));
            }
            if (!g_fence) dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
        }
        dev->Release();
        if (total > kSlotBytes) return "ERROR an adapted luminance footprint is larger than its slot";
        g_lum[r] = res;
        g_lum_fmt[r] = d.Format;
    }
    if (!g_fence_event) g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_rb || !g_fence || !g_fence_event) return "ERROR could not create the readback objects";
    for (ID3D12Resource* res : g_lum) d3d::watch_add(res);
    dual_pass::adapt_histogram(g_hist0);
    log::info("[lumcheck] armed: %s at object%s (%d RTV(s)), %s %p, %s %p, %d frames", tname, where, g_nrtv,
              kLumName[0], static_cast<void*>(g_lum[0]), kLumName[1], static_cast<void*>(g_lum[1]), g_frames);
    return {};
}

std::string finish() {
    ID3D12CommandQueue* q = state::present_queue.load();
    if (!q || !g_fence) return "ERROR no present queue";
    q->Signal(g_fence, ++g_fence_value);
    g_fence->SetEventOnCompletion(g_fence_value, g_fence_event);
    if (WaitForSingleObject(g_fence_event, 2000) != WAIT_OBJECT_0) return "ERROR the copies timed out (NOT MEASURED)";
    uint8_t* p = nullptr;
    D3D12_RANGE whole{0, static_cast<SIZE_T>(kMaxSnaps * 2 * kSlotBytes)};
    if (FAILED(g_rb->Map(0, &whole, reinterpret_cast<void**>(&p))) || !p) return "ERROR Map failed (NOT MEASURED)";
    uint32_t bits[kMaxSnaps][2] = {};
    for (int s = 0; s < g_nsnap; ++s)
        for (int r = 0; r < 2; ++r)
            if (g_snap[s].valid[r]) std::memcpy(&bits[s][r], p + (s * 2 + r) * kSlotBytes, 4);
    D3D12_RANGE none{0, 0};
    g_rb->Unmap(0, &none);

    // Per frame: the snapshots of its FXAA binds. A double frame's pair is compared on every target valid in both.
    int doubles = 0, monos = 0, other = 0, identical = 0, differing = 0, unmeasured = 0;
    for (int f = 0, s = 0; f < g_frames; ++f) {
        int first = s;
        while (s < g_nsnap && g_snap[s].frame == f) ++s;
        int binds = g_frame_binds[f];
        char line[200];
        int k = std::snprintf(line, sizeof(line), " f%d:", f);
        for (int i = first; i < s && k < static_cast<int>(sizeof(line)); ++i)
            for (int r = 0; r < 2 && k < static_cast<int>(sizeof(line)); ++r) {
                float v = 0;
                std::memcpy(&v, &bits[i][r], 4);
                k += g_snap[i].valid[r]
                         ? std::snprintf(line + k, sizeof(line) - k, " %d%c=%08x(%.6g,s%u)", g_snap[i].bind, "AB"[r],
                                         bits[i][r], v, g_snap[i].state[r])
                         : std::snprintf(line + k, sizeof(line) - k, " %d%c=-", g_snap[i].bind, "AB"[r]);
            }
        log::info("[lumcheck]%s", line);  // per frame: bind, target, bits (value, state it was copied from)
        if (binds == 1) {
            ++monos;
            continue;
        }
        if (binds != 2 || s - first != 2) {
            ++other;
            continue;
        }
        ++doubles;
        int compared = 0, diff = 0;
        for (int r = 0; r < 2; ++r) {
            if (!g_snap[first].valid[r] || !g_snap[first + 1].valid[r]) continue;
            ++compared;
            diff += bits[first][r] != bits[first + 1][r];
        }
        if (!compared) ++unmeasured;
        else if (diff) ++differing;
        else ++identical;
    }
    uint64_t hist[4];
    dual_pass::adapt_histogram(hist);
    char head[400];
    std::snprintf(head, sizeof(head),
                  "%d frames: %d double, %d mono, %d other; double-frame pairs: %d identical, %d differing, %d NOT "
                  "MEASURED | adaptation draws per double frame since arm: 0:%llu 1:%llu 2:%llu 3+:%llu",
                  g_frames, doubles, monos, other, identical, differing, unmeasured,
                  static_cast<unsigned long long>(hist[0] - g_hist0[0]), static_cast<unsigned long long>(hist[1] - g_hist0[1]),
                  static_cast<unsigned long long>(hist[2] - g_hist0[2]), static_cast<unsigned long long>(hist[3] - g_hist0[3]));
    return head;
}

void stop(const std::string& result) {
    d3d::set_bind_tap(d3d::kBindTapLum, nullptr);
    for (ID3D12Resource*& res : g_lum) {
        d3d::watch_remove(res);
        res = nullptr;
    }
    g_result = result;
    g_phase = 0;
    SetEvent(g_done);
}

void on_frame_end(uint64_t) {
    int ph = g_phase.load(std::memory_order_acquire);
    if (ph == 1) {
        std::string err = arm();
        if (!err.empty()) {
            stop(err);
            return;
        }
        g_frames_waited = 0;
        d3d::set_bind_tap(d3d::kBindTapLum, tap);
        g_phase.store(2, std::memory_order_release);
    } else if (ph == 2 || ph == 3) {
        if (++g_frames_waited > g_frames + 12)
            stop("ERROR the bind sequence of the post output target was not seen in time (NOT MEASURED)");
    } else if (ph == 4) {
        g_phase = 5;  // the last frame's list is submitted before the next Present
    } else if (ph == 5) {
        d3d::set_bind_tap(d3d::kBindTapLum, nullptr);
        stop(finish());
    }
}

}  // namespace

void init() {
    g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    d3d::add_frame_end_listener(on_frame_end);
}

std::string run(int frames, unsigned timeout_ms) {
    std::lock_guard lock(g_mutex);
    g_frames = frames < 1 ? 1 : frames > kMaxFrames ? kMaxFrames : frames;
    ResetEvent(g_done);
    g_phase.store(1, std::memory_order_release);
    if (WaitForSingleObject(g_done, timeout_ms) != WAIT_OBJECT_0) {
        int expected = 1;
        g_phase.compare_exchange_strong(expected, 0);  // never armed: disarm (an armed check finishes on its own)
        return "ERROR no frame within the timeout (NOT MEASURED)";
    }
    log::info("[lumcheck] %s", g_result.c_str());
    return g_result;
}

}  // namespace rdrvr::lum_check
