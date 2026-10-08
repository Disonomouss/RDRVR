// GPU time per frame on the present queue. Each game ExecuteCommandLists there is sent as [begin, game lists, end],
// where begin and end are our one-timestamp command lists (d3d::set_ecl_bracket). Busy time is the sum of end - begin
// over the frame's submissions, so the gaps where the queue waits on fences, the CPU or vsync are left out; span
// is the first begin to the last end. At Present the frame's timestamps are resolved into a readback buffer and
// read a few frames later (never waits on the GPU). Used for the vanilla GPU baseline (Spike S10, DECISIONS D5),
// so it changes nothing the game renders. Also when the frame's GPU work ended against Present (where the mod hands the
// frame to the OpenXR runtime), the GPU clock mapped onto the CPU's by the queue's clock calibration: a frame whose
// eye images are finished late reaches the runtime's compositor late (the DLSS judder study, 2026-10-08).

#include "core/gpu_timer.h"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <vector>

#include "core/d3d_hooks.h"
#include "core/log.h"
#include "core/state.h"

namespace rdrvr::gpu_timer {
namespace {

constexpr int kSlots = 8;         // frames in flight that can be timed
constexpr int kMaxBrackets = 16;  // game submissions on the present queue timed per frame (3 seen on 2026-10-03)
constexpr int kQueriesPerSlot = kMaxBrackets * 2;

struct Slot {
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* ts[kQueriesPerSlot] = {};  // one EndQuery each
    ID3D12GraphicsCommandList* resolve = nullptr;
    bool open = false;      // allocator reset and recording brackets for the current frame
    int used = 0;           // brackets handed out this frame
    bool pending = false;   // resolved and fenced, result not read yet
    int pending_used = 0;
    uint64_t fence_value = 0;
    int64_t present_qpc = 0;  // the CPU clock at this frame's Present (the frame-end listener)
};

std::mutex g_mutex;
bool g_ready = false, g_failed = false;
ID3D12QueryHeap* g_heap = nullptr;
ID3D12Resource* g_readback = nullptr;
uint64_t* g_mapped = nullptr;
ID3D12Fence* g_fence = nullptr;
uint64_t g_fence_next = 0;
uint64_t g_freq = 0;
ID3D12CommandQueue* g_queue = nullptr;
Slot g_slots[kSlots];
uint64_t g_frame = 0;
uint64_t g_skipped_behind = 0, g_skipped_overflow = 0;
std::vector<float> g_busy, g_span;  // last window, for percentiles
std::vector<float> g_done;          // the frame's GPU work ended, ms after its Present (negative: before)
uint64_t g_cal_gpu = 0, g_cal_cpu = 0;  // the queue's clock calibration (GPU timestamp, QPC) of this window
int64_t g_qpc_freq = 0;
std::vector<uint8_t> g_count;
double g_window_start_ms = 0;

bool create(ID3D12CommandQueue* q) {
    ID3D12Device* dev = nullptr;
    if (FAILED(q->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return false;
    bool ok = true;
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kSlots * kQueriesPerSlot, 0};
    ok = ok && SUCCEEDED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_heap)));
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = kSlots * kQueriesPerSlot * sizeof(uint64_t);
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ok = ok && SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&g_readback)));
    ok = ok && SUCCEEDED(g_readback->Map(0, nullptr, reinterpret_cast<void**>(&g_mapped)));
    ok = ok && SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)));
    auto make_list = [&](Slot& s, ID3D12GraphicsCommandList** out) {
        ok = ok && SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.alloc, nullptr, IID_PPV_ARGS(out)));
        if (ok) (*out)->Close();
    };
    for (Slot& s : g_slots) {
        ok = ok && SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.alloc)));
        for (auto*& l : s.ts) make_list(s, &l);
        make_list(s, &s.resolve);
    }
    ok = ok && SUCCEEDED(q->GetTimestampFrequency(&g_freq)) && g_freq;
    if (ok) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpc_freq = f.QuadPart;
        if (FAILED(q->GetClockCalibration(&g_cal_gpu, &g_cal_cpu))) g_cal_gpu = g_cal_cpu = 0;
    }
    dev->Release();
    if (ok) log::info("[gputimer] ready on queue %p (timestamp frequency %llu Hz, up to %d submissions per frame)",
                      static_cast<void*>(q), static_cast<unsigned long long>(g_freq), kMaxBrackets);
    else log::error("[gputimer] creation failed; GPU frame time unavailable");
    return ok;
}

double to_ms(uint64_t ticks) { return static_cast<double>(ticks) * 1000.0 / static_cast<double>(g_freq); }

void collect(Slot& s, int index) {
    if (!s.pending || g_fence->GetCompletedValue() < s.fence_value) return;
    s.pending = false;
    const uint64_t* t = g_mapped + index * kQueriesPerSlot;
    uint64_t busy = 0;
    for (int k = 0; k < s.pending_used; ++k)
        if (t[2 * k + 1] > t[2 * k]) busy += t[2 * k + 1] - t[2 * k];
    uint64_t first = t[0], last = t[2 * s.pending_used - 1];
    double busy_ms = to_ms(busy), span_ms = last > first ? to_ms(last - first) : 0.0;
    double ema = state::gpu_frame_ms.load();
    state::gpu_frame_ms = ema == 0 ? busy_ms : ema * 0.95 + busy_ms * 0.05;
    ema = state::gpu_span_ms.load();
    state::gpu_span_ms = ema == 0 ? span_ms : ema * 0.95 + span_ms * 0.05;
    state::gpu_brackets = static_cast<uint32_t>(s.pending_used);
    g_busy.push_back(static_cast<float>(busy_ms));
    g_span.push_back(static_cast<float>(span_ms));
    g_count.push_back(static_cast<uint8_t>(s.pending_used));
    if (g_cal_cpu && g_qpc_freq && s.present_qpc) {  // the last game submission's end on the CPU clock, against Present
        const double end_qpc = static_cast<double>(g_cal_cpu) +
                               (static_cast<double>(last) - static_cast<double>(g_cal_gpu)) * static_cast<double>(g_qpc_freq) / static_cast<double>(g_freq);
        g_done.push_back(static_cast<float>((end_qpc - static_cast<double>(s.present_qpc)) * 1000.0 / static_cast<double>(g_qpc_freq)));
    }
}

void report_window() {
    double now = log::now_ms();
    if (g_window_start_ms == 0) g_window_start_ms = now;
    if (g_busy.size() < 600) return;
    std::vector<float> b = g_busy, sp = g_span;
    std::sort(b.begin(), b.end());
    std::sort(sp.begin(), sp.end());
    double sum = 0;
    for (float f : b) sum += f;
    uint8_t cmin = *std::min_element(g_count.begin(), g_count.end()), cmax = *std::max_element(g_count.begin(), g_count.end());
    std::vector<float> dn = g_done;
    std::sort(dn.begin(), dn.end());
    char done[160] = " | GPU done vs Present: n/a";
    if (!dn.empty())
        std::snprintf(done, sizeof(done), " | GPU done vs Present: p5 %+.2f p50 %+.2f p95 %+.2f max %+.2f ms", dn[dn.size() * 5 / 100],
                      dn[dn.size() / 2], dn[dn.size() * 95 / 100], dn.back());
    log::info("[gputimer] %zu frames over %.1f s: GPU busy mean %.2f ms  p50 %.2f  p95 %.2f  max %.2f | span p50 %.2f p95 %.2f"
              " | submissions/frame %u..%u | swap %ux%u  cpu %.2f ms | skipped %llu behind %llu overflow%s",
              b.size(), (now - g_window_start_ms) / 1000.0, sum / b.size(), b[b.size() / 2], b[b.size() * 95 / 100], b.back(),
              sp[sp.size() / 2], sp[sp.size() * 95 / 100], cmin, cmax, state::swap_width.load(), state::swap_height.load(),
              state::cpu_frame_ms.load(), static_cast<unsigned long long>(g_skipped_behind),
              static_cast<unsigned long long>(g_skipped_overflow), done);
    g_busy.clear();
    g_span.clear();
    g_count.clear();
    g_done.clear();
    if (g_queue && FAILED(g_queue->GetClockCalibration(&g_cal_gpu, &g_cal_cpu))) g_cal_gpu = g_cal_cpu = 0;  // no drift across windows
    g_window_start_ms = now;
}

void reset_objects() {
    // A new swapchain on a new queue: rebuild there. The old objects are dropped without waiting; they belong to an
    // earlier device and are never submitted again.
    g_ready = false;
    g_frame = 0;
    for (Slot& s : g_slots) s = Slot{};
    g_heap = nullptr;
    g_readback = nullptr;
    g_mapped = nullptr;
    g_fence = nullptr;
    g_fence_next = 0;
}

// Called from the ExecuteCommandLists hook for each game submission on the present queue.
bool bracket(ID3D12CommandQueue* q, ID3D12CommandList** before, ID3D12CommandList** after) {
    std::lock_guard lock(g_mutex);
    if (g_failed) return false;
    if (g_ready && q != g_queue) {
        log::info("[gputimer] present queue changed %p -> %p; rebuilding", static_cast<void*>(g_queue), static_cast<void*>(q));
        reset_objects();
    }
    if (!g_ready) {
        g_queue = q;
        g_ready = create(q);
        g_failed = !g_ready;
        if (!g_ready) return false;
    }
    int i = static_cast<int>(g_frame % kSlots);
    Slot& s = g_slots[i];
    if (!s.open) {
        collect(s, i);
        if (s.pending) {  // GPU more than kSlots frames behind: leave this frame untimed
            ++g_skipped_behind;
            return false;
        }
        if (FAILED(s.alloc->Reset())) return false;
        s.open = true;
        s.used = 0;
    }
    if (s.used == kMaxBrackets) {
        ++g_skipped_overflow;
        return false;
    }
    int k = s.used;
    UINT base = static_cast<UINT>(i * kQueriesPerSlot + 2 * k);
    ID3D12GraphicsCommandList* b = s.ts[2 * k];
    ID3D12GraphicsCommandList* e = s.ts[2 * k + 1];
    if (FAILED(b->Reset(s.alloc, nullptr))) return false;
    b->EndQuery(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, base);
    b->Close();
    if (FAILED(e->Reset(s.alloc, nullptr))) return false;
    e->EndQuery(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, base + 1);
    e->Close();
    s.used = k + 1;
    *before = b;
    *after = e;
    return true;
}

void on_frame_end(uint64_t) {
    std::lock_guard lock(g_mutex);
    if (!g_ready) return;
    int i = static_cast<int>(g_frame % kSlots);
    Slot& s = g_slots[i];
    if (!s.open) return;  // no game submission timed since the last Present
    if (s.used > 0 && SUCCEEDED(s.resolve->Reset(s.alloc, nullptr))) {
        UINT base = static_cast<UINT>(i * kQueriesPerSlot);
        s.resolve->ResolveQueryData(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, base, static_cast<UINT>(2 * s.used), g_readback,
                                    base * sizeof(uint64_t));
        s.resolve->Close();
        d3d::submit_internal(g_queue, s.resolve);
        g_queue->Signal(g_fence, ++g_fence_next);
        s.fence_value = g_fence_next;
        s.pending = true;
        s.pending_used = s.used;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        s.present_qpc = now.QuadPart;
    }
    s.open = false;
    ++g_frame;
    for (int k = 0; k < kSlots; ++k) collect(g_slots[k], k);
    report_window();
}

}  // namespace

void init() {
    d3d::set_ecl_bracket(bracket);
    d3d::add_frame_end_listener(on_frame_end);
}

}  // namespace rdrvr::gpu_timer
