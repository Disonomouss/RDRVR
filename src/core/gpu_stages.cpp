// [Stereo] PrepassGpuStages (run 19 item 3; test only, off by default): the walk cut's GPU time by stage (gpu_stages.h,
// research\run19\gpu.md). The stamps are EndQuery timestamps recorded into the game's own direct lists, on the playback
// thread, into a ring of per-frame slots of this module's query heap; a frame's slot closes when the next frame's pass-1
// marker plays, is resolved on the present queue two presents later (the frame's lists all submitted by then: the same
// queue, so the resolve runs after them) and read when its fence passes (never waiting on the GPU). Only the GPU timer's
// pattern (gpu_timer.cpp) is reused: its own brackets around each submission are untouched.
//   What a stage's time means: the GPU's timestamp at a point is when the work before it has finished, so a stage is the
// GPU time between two points of the stream. Points in one list are the GPU's own; points in two lists (an eye's begin
// and end can be) also hold any time the queue waited for the CPU between the submissions (the status says how often an
// eye's begin and end were in one list).

#include "core/gpu_stages.h"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "core/anchors.h"
#include "core/config.h"
#include "core/d3d_hooks.h"
#include "core/log.h"
#include "core/main_instanced.h"
#include "core/prepass_arrays.h"
#include "core/pso.h"
#include "core/state.h"

namespace rdrvr::gpu_stages {
using anchors::Id;
std::atomic<bool> g_on_flag{false};
std::atomic<int> g_walk_flag{0};
std::atomic<bool> g_cpu_flag{false};
std::atomic<bool> g_marg_flag{false};  // (item 8e) "prepass gpu walk marginal on|off"
namespace {

constexpr int kSlots = 8;  // frames in flight (a slot: recorded, closed, resolved two presents later, read)
constexpr int kQ = 64;     // timestamps a frame: the fixed points, then the clears' pairs
constexpr int kRuns = 8192;  // (item 8b) the walk census's timestamps a frame (both passes' runs), after all slots' fixed areas
constexpr int kPairs = 192;  // (item 8b) VS/PS pairs the census tells apart (more: counted as "other")
constexpr uint16_t kNoPair = 0xffff;
enum : uint8_t { kWRun, kWOpen, kWE1, kWClose, kWM0, kWM1, kWM2 };  // (kWM0-2: item 8e's marginal stamps, pair = the ordinal)
constexpr int kMOrds = 64;  // (item 8e) the ordinals main_inst lists
struct WEnt {  // (no non-zero default: g_went stays in .bss; each entry is written whole by went)
    uint8_t kind, pass;
    uint16_t pair;
    uint32_t draws;
    const void* list;
};
enum : int { kE1B, kE1E, kE2B, kE2E, kP1B, kP1E, kP2B, kP2E, kR1C, kR1D, kR2C, kR2D, kHOC, kHOD, kFixed = 16 };
constexpr int kClearPairs = (kQ - kFixed) / 2;
constexpr uint32_t kPreEnds = (1u << kP1E) | (1u << kP2E);

struct StageDef {
    const char* name;
    int b, e;
};
// eye1/eye2: the pass's scene (its first bind to the first bind after its end marker); post1: eye 1's post between the
// passes; pre1/pre2: the pass's pre-pass (its Shadow Collector Depth Buffer clear to the walk's end, the region's copies
// included); body: the region before its copies; copy: the region's copies (pass 1's slice 0, 4a's pass 2 slice 1);
// hobody/handover: the cut's pass 2 region before the handover, and the handover's copies of slice 1
const StageDef kStages[] = {
    {"frame", kE1B, kE2E}, {"eye1", kE1B, kE1E}, {"post1", kE1E, kE2B}, {"eye2", kE2B, kE2E},  {"pre1", kP1B, kP1E},
    {"pre2", kP2B, kP2E},  {"body1", kP1B, kR1C}, {"copy1", kR1C, kR1D}, {"body2", kP2B, kR2C}, {"copy2", kR2C, kR2D},
    {"hobody", kP2B, kHOC}, {"handover", kHOC, kHOD},
};
constexpr int kStageCount = static_cast<int>(sizeof(kStages) / sizeof(kStages[0]));
const char* const kSix[] = {"Shadow Collector Depth Buffer", "Shadow Collector",        "Velocity RT",
                            "Grass FS Colour",               "Grass FS Light Multiply", "Tree FS Colour Diffuse"};

struct Slot {
    int state = 0;  // 0 free, 1 recording, 2 closed (to resolve), 3 resolving (fenced), 4 retired (never reused: its list may
                    // still be held by an old queue)
    bool drop = false;  // (recording when the queue changed or "prepass gpu off": freed at its close, never resolved)
    uint64_t closed_at = 0, fence = 0, resolved_at = 0, freq = 0;
    uint64_t mask = 0;                   // the timestamps recorded (bit per query)
    const void* list_of[kFixed] = {};    // the list each fixed point went into
    uint8_t clear_pass[kClearPairs] = {};
    int clears = 0;
    int wn = 0, wmode = 0;  // (item 8b) the walk entries recorded (g_went[slot]), the walk mode at the frame's open
    bool wover = false;     // (item 8b) more than kRuns: the frame's walk not read
    bool mgon = false, marg = false;  // (item 8e) the marginal switch at the frame's open; a marginal stamp recorded (a dup frame)
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* resolve = nullptr;
};

std::mutex g_mx;  // the slots, the device objects and the sums (the playback thread's stamps, the present thread's reads)
std::atomic<bool> g_installed{false};
bool g_ready = false, g_failed = false;
ID3D12CommandQueue* g_queue = nullptr;
ID3D12QueryHeap* g_heap = nullptr;
ID3D12Resource* g_readback = nullptr;
const uint64_t* g_mapped = nullptr;
ID3D12Fence* g_fence = nullptr;
uint64_t g_fence_next = 0, g_freq = 0;
ID3D12Device* g_dev = nullptr;        // the queue's device at create (the objects' device)
ID3D12Device* g_state_dev = nullptr;  // state::device at create: a slot opens only while it is still that one
uint64_t g_requeued = 0;
Slot g_slots[kSlots];
int g_cur = -1, g_next = 0;  // the slot recording, the next to open
// the playback thread's own (written only there, by the markers' callbacks and walk_end_played)
std::atomic<DWORD> g_tid{0};
int g_pass = 0;
bool g_span[3] = {};                  // the pass's pre-pass open: its depth clear seen, its walk end not yet
std::atomic<uint32_t> g_pending{0};   // fixed points owed at g_tid's next game bind (bit per point)
std::atomic<uint64_t> g_marks{0}, g_marks_failed{0};

struct Acc {
    double sum = 0;
    uint64_t n = 0;
    void add(double v) {
        sum += v;
        ++n;
    }
};
Acc g_acc[kStageCount], g_clr[2];
uint64_t g_clr_n[2] = {}, g_same[2] = {}, g_eye_n[2] = {};
uint64_t g_frames = 0, g_skipped = 0, g_dropped = 0, g_bad = 0, g_unresolved = 0;
double g_window_ms = 0;

// ---- (item 8b) the walk census. The entries: written by the playback thread into the recording slot (g_went[g_cur]), read
// by the present thread's collect after the slot's fence (the slot's state moves under g_mx).
WEnt g_went[kSlots][kRuns];
uintptr_t g_walk_ret = 0;
struct Pair {
    uint64_t vs = 0, ps = 0;
};
Pair g_pair[kPairs];  // (under g_mx) the pairs seen, by id
int g_npairs = 0;
uint64_t g_pair_full = 0;
// the playback thread's own
int g_wseg = 0;           // the segment open in this pass (1, 2), else 0
uint32_t g_wowed = 0;     // walk points owed at the next draw: bit 1 the open, bit 2 e1
int g_wlast = -1, g_wslot = -1;  // the entry the draws are counted on (in slot g_wslot)
uint64_t g_wvs = 0, g_wps = 0;   // the last draw's pair in the segment
bool g_wknown = false;
std::atomic<uint64_t> g_wdrop_vs{0}, g_wdrop_ps{0};
// (under g_mx) the sums
struct FamAcc {
    double ms[2] = {};
    uint64_t draws[2] = {}, runs[2] = {};
};
FamAcc g_fam[kPairs + 1];  // [kPairs]: other (no pair known, or past the table)
Acc g_wseg_acc[2], g_we1_acc[2], g_wrest_acc[2];
uint64_t g_wframes[2] = {}, g_wfam_frames[2] = {}, g_wbad[2] = {}, g_wlists[2] = {}, g_wnoe1[2] = {};
uint64_t g_wover_n = 0, g_wunclosed = 0, g_wmismatch = 0, g_wnondirect = 0, g_wdropped = 0, g_wmax = 0;
std::atomic<uint64_t> g_wmarks{0}, g_wmarks_failed{0};
// (item 8e, under g_mx) the marginal stamps' sums: per ordinal the twin's and R0's ms and the draws; per dup frame the sum of (after R0 -
// before twin); pass 1's e1 stage on dup frames and on the other frames of the same switch; triples broken, stamps not recorded
double g_mg_twin[kMOrds] = {}, g_mg_r0[kMOrds] = {};
uint64_t g_mg_n[kMOrds] = {};
Acc g_mg_tot, g_mg_e1_dup, g_mg_e1_nd;
uint64_t g_mg_broken = 0, g_mg_refused = 0, g_mg_stamps = 0;

bool mine() { return GetCurrentThreadId() == g_tid.load(std::memory_order_relaxed); }

// (under g_mx, the playback thread) one timestamp into the game's list; the first of each point only
void stamp(ID3D12GraphicsCommandList* cl, int id) {
    if (g_cur < 0 || !g_ready || id < 0 || id >= kQ || !cl || cl->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    Slot& s = g_slots[g_cur];
    const uint64_t bit = 1ull << id;
    if (s.mask & bit) return;
    cl->EndQuery(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(g_cur * kQ + id));
    s.mask |= bit;
    if (id < kFixed) s.list_of[id] = cl;
}

bool create(ID3D12CommandQueue* q);  // (below)

// ---- the markers (op-0x4a callbacks recorded by mark(); they play on the playback thread at their point of the stream)
void cb_begin1(void*) {  // pass 1 begins: the last frame's slot closes, this frame's opens
    std::lock_guard lk(g_mx);
    if (g_cur >= 0) {
        Slot& c = g_slots[g_cur];
        c.state = c.drop ? 0 : 2;  // (dropped: recording across a queue change or "prepass gpu off": never resolved)
        c.drop = false;
        c.closed_at = state::presents.load();
    }
    if (g_pending.exchange(0)) ++g_dropped;  // (points owed past the frame's end: no bind came)
    if (g_wseg) ++g_wunclosed;  // (item 8b: a segment open past its pass: that pass's walk not read, collect's check)
    g_wseg = 0;
    g_wowed = 0;
    g_wlast = g_wslot = -1;
    g_wknown = false;
    g_tid = GetCurrentThreadId();
    g_pass = 1;
    g_span[1] = g_span[2] = false;
    g_cur = -1;
    if (g_failed || !on()) return;
    ID3D12CommandQueue* q = state::present_queue.load();
    if (!q || !state::device.load()) return;
    if (!g_ready) {  // (made at the first double frame, not the first Present: the game re-makes its swapchain on a new queue ~12 s in)
        g_ready = create(q);
        g_failed = !g_ready;
        if (!g_ready) return;
    }
    if (q != g_queue || state::device.load() != g_state_dev) return;  // (the frame end re-points the queue or stands down first)
    int pick = -1;  // the next free slot in ring order (retired ones never come back)
    for (int k = 0; k < kSlots && pick < 0; ++k)
        if (g_slots[(g_next + k) % kSlots].state == 0) pick = (g_next + k) % kSlots;
    if (pick < 0) {  // (the GPU or the reads more than kSlots frames behind: this frame untimed)
        ++g_skipped;
        return;
    }
    Slot& n = g_slots[pick];
    n.state = 1;
    n.mask = 0;
    n.clears = 0;
    n.wn = 0;
    n.wmode = g_walk_flag.load(std::memory_order_relaxed);
    n.wover = false;
    n.mgon = g_marg_flag.load(std::memory_order_relaxed);
    n.marg = false;
    std::memset(n.list_of, 0, sizeof(n.list_of));
    g_cur = pick;
    g_next = (pick + 1) % kSlots;
    g_pending.fetch_or(1u << kE1B);
}
void walk_pass_ends() {  // (item 8b, the playback thread) a segment still open at its pass's end: never closed
    if (g_wseg) ++g_wunclosed;
    g_wseg = 0;
    g_wowed = 0;
    g_wlast = -1;
    g_wknown = false;
}
void cb_end1(void*) {
    std::lock_guard lk(g_mx);
    walk_pass_ends();
    if (g_cur >= 0) g_pending.fetch_or(1u << kE1E);
}
void cb_begin2(void*) {
    std::lock_guard lk(g_mx);
    walk_pass_ends();
    g_pass = 2;
    if (g_cur >= 0) g_pending.fetch_or(1u << kE2B);
}
void cb_end2(void*) {
    std::lock_guard lk(g_mx);
    walk_pass_ends();
    g_pass = 0;
    if (g_cur >= 0) g_pending.fetch_or(1u << kE2E);
}
void (*const kMarks[4])(void*) = {&cb_begin1, &cb_end1, &cb_begin2, &cb_end2};

// (item 8b) the walk's markers: the render thread's pass in the callback, checked against the playback thread's
void walk_marked(int pass, uint32_t bit) {
    if (!mine() || !walk_on()) return;  // (g_wseg, g_wowed: this thread's own)
    std::lock_guard lk(g_mx);           // (the counters)
    if (pass != g_pass) {
        ++g_wmismatch;
        return;
    }
    if (bit == 1) {
        if (g_wseg) ++g_wunclosed;  // (a second open in the pass: the first never closed)
        g_wseg = pass;
        g_wlast = -1;
        g_wknown = false;
    } else if (!g_wseg) {
        return;  // (e1 outside an open segment)
    }
    g_wowed |= bit;
}
void cb_wopen1(void*) { walk_marked(1, 1); }
void cb_wopen2(void*) { walk_marked(2, 1); }
void cb_we1_1(void*) { walk_marked(1, 2); }
void cb_we1_2(void*) { walk_marked(2, 2); }

// (item 8b, the playback thread) one walk entry: a timestamp into the game's list at this point (before the draw or the copy)
void went(ID3D12GraphicsCommandList* cl, uint8_t kind, uint64_t vs, uint64_t ps, bool known) {
    std::lock_guard lk(g_mx);
    g_wlast = -1;
    if (g_cur < 0 || !g_ready || !cl || g_slots[g_cur].wover) return;  // (over: the frame's walk is not read)
    if (cl->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        ++g_wnondirect;
        g_slots[g_cur].wover = true;  // (a stamp missing: the frame's walk not read)
        return;
    }
    Slot& s = g_slots[g_cur];
    if (s.wn >= kRuns) {
        s.wover = true;
        return;
    }
    uint16_t id = kNoPair;
    if (known) {
        for (int k = 0; k < g_npairs && id == kNoPair; ++k)
            if (g_pair[k].vs == vs && g_pair[k].ps == ps) id = static_cast<uint16_t>(k);
        if (id == kNoPair) {
            if (g_npairs < kPairs) {
                g_pair[g_npairs] = Pair{vs, ps};
                id = static_cast<uint16_t>(g_npairs++);
            } else {
                ++g_pair_full;
            }
        }
    }
    const int k = s.wn++;
    g_went[g_cur][k] = WEnt{kind, static_cast<uint8_t>(g_pass), id, 0, cl};
    cl->EndQuery(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(kSlots * kQ + g_cur * kRuns + k));
    g_wlast = k;
    g_wslot = g_cur;
}

// d3d's bind tap (kBindTapGpuStages): the owed points, before the game's bind
void bind_tap(ID3D12GraphicsCommandList* cl, unsigned, const D3D12_CPU_DESCRIPTOR_HANDLE*, int, const D3D12_CPU_DESCRIPTOR_HANDLE*) {
    if (!g_pending.load(std::memory_order_relaxed) || !on() || !mine()) return;
    if (d3d::in_present()) return;  // (the mod's own binds in Present and its frame-end listeners: the points wait for the game's next)
    std::lock_guard lk(g_mx);
    uint32_t p = g_pending.load(std::memory_order_relaxed);
    if (prepass::active()) p &= ~kPreEnds;  // (a region still open ends after this tap: copies_end stamps the walk's end after its copies)
    for (int id = 0; id < kFixed; ++id)
        if (p & (1u << id)) stamp(cl, id);
    g_pending.fetch_and(~p);
}

// ---- the device objects: made under g_mx at the first double frame (the playback thread); used by the present thread
bool create(ID3D12CommandQueue* q) {
    ID3D12Device* dev = nullptr;
    if (FAILED(q->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return false;
    bool ok = true;
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kSlots * (kQ + kRuns), 0};  // (the fixed areas, then the walk's)
    ok = ok && SUCCEEDED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_heap)));
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = static_cast<UINT64>(kSlots) * (kQ + kRuns) * sizeof(uint64_t);
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ok = ok && SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&g_readback)));
    void* mapped = nullptr;
    ok = ok && SUCCEEDED(g_readback->Map(0, nullptr, &mapped)) && mapped;
    g_mapped = ok ? static_cast<const uint64_t*>(mapped) : nullptr;
    ok = ok && SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)));
    for (Slot& s : g_slots) {
        ok = ok && SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.alloc)));
        ok = ok && SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.alloc, nullptr, IID_PPV_ARGS(&s.resolve)));
        if (ok) s.resolve->Close();
    }
    ok = ok && SUCCEEDED(q->GetTimestampFrequency(&g_freq)) && g_freq;
    dev->Release();  // (kept as an identity only: the game holds its device)
    if (ok) {
        g_queue = q;
        g_dev = dev;
        g_state_dev = state::device.load();
        log::info("[gpustages] ready on queue %p (timestamp frequency %llu Hz, %d timestamps a frame, %d frames in flight)",
                  static_cast<void*>(q), static_cast<unsigned long long>(g_freq), kQ, kSlots);
    } else {
        log::error("[gpustages] creation failed: no stage timings");  // (the objects made are left: a test tool, made once)
    }
    return ok;
}

void resolve(int i) {
    Slot& s = g_slots[i];
    if (!s.mask && !s.wn) {
        s.state = 0;
        return;
    }
    // (only a state-2 slot gets here: its allocator's last resolve was collected, its own fence passed on its own queue;
    // a slot whose fence never came is retired, state 4, never reset)
    if (FAILED(s.alloc->Reset()) || FAILED(s.resolve->Reset(s.alloc, nullptr))) {
        ++g_unresolved;
        s.state = 0;
        return;
    }
    for (int a = 0; a < kQ;) {  // each run of recorded timestamps (an unrecorded query is never resolved)
        if (!((s.mask >> a) & 1)) {
            ++a;
            continue;
        }
        int b = a;
        while (b < kQ && ((s.mask >> b) & 1)) ++b;
        const UINT first = static_cast<UINT>(i * kQ + a);
        s.resolve->ResolveQueryData(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, first, static_cast<UINT>(b - a), g_readback,
                                    static_cast<UINT64>(first) * sizeof(uint64_t));
        a = b;
    }
    if (s.wn > 0) {  // (item 8b) the walk's entries: 0 .. wn-1, every one recorded
        const UINT first = static_cast<UINT>(kSlots * kQ + i * kRuns);
        s.resolve->ResolveQueryData(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, first, static_cast<UINT>(s.wn), g_readback,
                                    static_cast<UINT64>(first) * sizeof(uint64_t));
    }
    s.resolve->Close();
    d3d::submit_internal(g_queue, s.resolve);
    g_queue->Signal(g_fence, ++g_fence_next);
    s.fence = g_fence_next;
    s.freq = g_freq;  // (the queue it was resolved on: a later queue change does not change its frequency)
    s.resolved_at = state::presents.load();
    s.state = 3;
}

// (item 8b, under g_mx) a frame's walk, per pass: one open first, one close last, at most one e1, the times in order. The
// segment's time is close - open; each entry's interval (to the next entry) is charged to its pair (families mode).
void collect_walk(int i) {
    const Slot& s = g_slots[i];
    g_wmax = std::max(g_wmax, static_cast<uint64_t>(s.wn));
    if (s.wover) {
        ++g_wover_n;
        return;
    }
    const uint64_t* t = g_mapped + static_cast<size_t>(kSlots) * kQ + static_cast<size_t>(i) * kRuns;
    const WEnt* w = g_went[i];
    const double to_ms = 1000.0 / static_cast<double>(s.freq);
    double e1_p1 = -1.0;  // (item 8e: pass 1's e1 stage, for the marginal stamps' check)
    for (int p = 1; p <= 2; ++p) {
        int o = -1, c = -1, e1 = -1, n = 0, prev = -1, lists = 0;
        bool ok = true;
        for (int k = 0; k < s.wn && ok; ++k) {
            if (w[k].pass != p || w[k].kind >= kWM0) continue;  // (item 8e: the marginal stamps are read below, never a run)
            if (n == 0 && w[k].kind != kWOpen) ok = false;
            if (n > 0 && (w[k].kind == kWOpen || c >= 0)) ok = false;  // (a second open, or anything after the close)
            if (w[k].kind == kWOpen) o = k;
            if (w[k].kind == kWClose) c = k;
            if (w[k].kind == kWE1) {
                if (e1 >= 0) ok = false;
                e1 = k;
            }
            if (prev >= 0) {
                if (t[k] < t[prev]) ok = false;
                if (w[k].list != w[prev].list) ++lists;
            }
            prev = k;
            ++n;
        }
        if (n == 0) continue;  // (no segment in this pass: not counted)
        if (!ok || o < 0 || c < 0 || (t[c] - t[o]) * to_ms > 100.0) {
            ++g_wbad[p - 1];
            continue;
        }
        g_wseg_acc[p - 1].add(static_cast<double>(t[c] - t[o]) * to_ms);
        if (e1 >= 0) {
            g_we1_acc[p - 1].add(static_cast<double>(t[e1] - t[o]) * to_ms);
            if (p == 1) e1_p1 = static_cast<double>(t[e1] - t[o]) * to_ms;
            g_wrest_acc[p - 1].add(static_cast<double>(t[c] - t[e1]) * to_ms);
        } else {
            ++g_wnoe1[p - 1];
        }
        ++g_wframes[p - 1];
        g_wlists[p - 1] += static_cast<uint64_t>(lists);
        if (s.wmode != 2) continue;
        ++g_wfam_frames[p - 1];
        int k = o;
        while (k != c) {  // each entry of the pass to the next one of the pass
            int nx = k + 1;
            while (w[nx].pass != p || w[nx].kind >= kWM0) ++nx;  // (c is of this pass: the walk ends there)
            FamAcc& f = g_fam[w[k].pair == kNoPair ? kPairs : w[k].pair];
            f.ms[p - 1] += static_cast<double>(t[nx] - t[k]) * to_ms;
            f.draws[p - 1] += w[k].draws;
            ++f.runs[p - 1];
            k = nx;
        }
    }
    if (!s.mgon) return;
    if (e1_p1 >= 0.0) (s.marg ? g_mg_e1_dup : g_mg_e1_nd).add(e1_p1);  // (item 8e: dup frames against the others of the same switch)
    if (!s.marg) return;
    double tot = 0.0;  // (item 8e) each listed draw's triple: before the twin, between the twin and R0, after R0, in one list, in order
    for (int k = 0; k < s.wn; ++k) {
        if (w[k].kind < kWM0) continue;
        if (w[k].kind == kWM0 && k + 2 < s.wn && w[k + 1].kind == kWM1 && w[k + 2].kind == kWM2 && w[k + 1].pair == w[k].pair &&
            w[k + 2].pair == w[k].pair && w[k + 1].list == w[k].list && w[k + 2].list == w[k].list && t[k + 1] >= t[k] && t[k + 2] >= t[k + 1] &&
            w[k].pair < kMOrds) {
            const int o = w[k].pair;
            g_mg_twin[o] += static_cast<double>(t[k + 1] - t[k]) * to_ms;
            g_mg_r0[o] += static_cast<double>(t[k + 2] - t[k + 1]) * to_ms;
            ++g_mg_n[o];
            tot += static_cast<double>(t[k + 2] - t[k]) * to_ms;
            k += 2;
        } else {
            ++g_mg_broken;
        }
    }
    g_mg_tot.add(tot);
}

void collect(int i) {
    Slot& s = g_slots[i];
    const uint64_t* t = g_mapped + static_cast<size_t>(i) * kQ;
    auto dur = [&](int b, int e, double* ms) {
        if (!((s.mask >> b) & 1) || !((s.mask >> e) & 1)) return false;
        if (t[e] < t[b]) {
            ++g_bad;
            return false;
        }
        *ms = static_cast<double>(t[e] - t[b]) * 1000.0 / static_cast<double>(s.freq);
        if (*ms > 100.0) {  // (a stale or foreign value: never a stage)
            ++g_bad;
            return false;
        }
        return true;
    };
    double ms = 0;
    for (int k = 0; k < kStageCount; ++k)
        if (dur(kStages[k].b, kStages[k].e, &ms)) g_acc[k].add(ms);
    for (int e = 0; e < 2; ++e) {
        const int b = e ? kE2B : kE1B, en = e ? kE2E : kE1E;
        if (((s.mask >> b) & 1) && ((s.mask >> en) & 1)) {
            ++g_eye_n[e];
            if (s.list_of[b] == s.list_of[en]) ++g_same[e];
        }
    }
    double sum[3] = {};
    int cnt[3] = {};
    for (int j = 0; j < s.clears; ++j)
        if (s.clear_pass[j] >= 1 && s.clear_pass[j] <= 2 && dur(kFixed + 2 * j, kFixed + 2 * j + 1, &ms)) {
            sum[s.clear_pass[j]] += ms;
            ++cnt[s.clear_pass[j]];
        }
    for (int p = 1; p <= 2; ++p)
        if (cnt[p]) {
            g_clr[p - 1].add(sum[p]);
            g_clr_n[p - 1] += static_cast<uint64_t>(cnt[p]);
        }
    if (s.wmode && (s.wn > 0 || s.wover)) collect_walk(i);
    ++g_frames;
    s.state = 0;
}

void stand_down(const char* why) {  // (under g_mx) no more stamps or resolves; the objects are left (a test tool)
    g_failed = true;
    g_ready = false;
    g_on_flag = false;
    log::warn("[gpustages] %s: stage timings off", why);
}

void on_frame_end(uint64_t) {
    std::lock_guard lk(g_mx);
    if (g_failed || !g_ready) return;  // (made at the first double frame: cb_begin1)
    ID3D12CommandQueue* q = state::present_queue.load();
    if (!q) return;
    if (state::device.load() != g_state_dev) {
        stand_down("the device changed");
        return;
    }
    if (q != g_queue) {  // a new swapchain on another queue (the game does this ~12 s after launch): the same device, re-pointed
        ID3D12Device* d = nullptr;
        const bool same = SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&d))) && d == g_dev;
        uint64_t f = 0;
        ID3D12Fence* nf = nullptr;  // (the new queue's own fence: the old one may still get the old queue's signals)
        const bool ok = same && SUCCEEDED(q->GetTimestampFrequency(&f)) && f && SUCCEEDED(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&nf)));
        if (d) d->Release();
        if (!ok) {
            stand_down(!same ? "the new present queue is on another device" : "the new present queue has no timestamp frequency or fence");
            return;
        }
        for (Slot& s : g_slots) {  // (frames recorded for the old queue: dropped; those resolving there retired for good)
            if (s.state == 2) s.state = 0;  // (never resolved: its allocator's last resolve was collected, its fence passed)
            else if (s.state == 1) s.drop = true;
            else if (s.state == 3) {
                s.state = 4;
                ++g_unresolved;
            }
        }
        g_fence = nf;  // (the old fence is left: a test tool)
        g_fence_next = 0;
        log::info("[gpustages] the present queue changed %p -> %p (same device): re-pointed", static_cast<void*>(g_queue), static_cast<void*>(q));
        g_queue = q;
        g_freq = f;
        ++g_requeued;
        return;
    }
    const uint64_t now = state::presents.load();
    for (int i = 0; i < kSlots; ++i)
        if (g_slots[i].state == 2 && now >= g_slots[i].closed_at + 2) resolve(i);
    const uint64_t done = g_fence->GetCompletedValue();
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = g_slots[i];
        if (s.state != 3) continue;
        if (done >= s.fence) {
            collect(i);
        } else if (now > s.resolved_at + 240) {  // (its fence never came: retired for good, its allocator never Reset again)
            ++g_unresolved;
            s.state = 4;
        }
    }
}

void reset_sums() {
    for (Acc& a : g_acc) a = Acc{};
    for (int p = 0; p < 2; ++p) {
        g_clr[p] = Acc{};
        g_clr_n[p] = g_same[p] = g_eye_n[p] = 0;
    }
    g_frames = g_skipped = g_dropped = g_bad = g_unresolved = 0;
    g_marks = 0;
    g_marks_failed = 0;
    for (FamAcc& f : g_fam) f = FamAcc{};  // (item 8b; the pair table is kept)
    for (int p = 0; p < 2; ++p) {
        g_wseg_acc[p] = g_we1_acc[p] = g_wrest_acc[p] = Acc{};
        g_wframes[p] = g_wfam_frames[p] = g_wbad[p] = g_wlists[p] = g_wnoe1[p] = 0;
    }
    g_wover_n = g_wunclosed = g_wmismatch = g_wnondirect = g_wdropped = g_pair_full = g_wmax = 0;
    g_wmarks = 0;
    g_wmarks_failed = 0;
    for (int o = 0; o < kMOrds; ++o) {  // (item 8e)
        g_mg_twin[o] = g_mg_r0[o] = 0.0;
        g_mg_n[o] = 0;
    }
    g_mg_tot = g_mg_e1_dup = g_mg_e1_nd = Acc{};
    g_mg_broken = g_mg_refused = g_mg_stamps = 0;
    g_window_ms = log::now_ms();
}

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

bool named_six(D3D12_CPU_DESCRIPTOR_HANDLE h, bool depth) {
    const void* r = d3d::view_resource(h.ptr);
    if (!r) return false;
    if (depth) return d3d::resource_named(r, kSix[0]);
    for (int k = 1; k < 6; ++k)
        if (d3d::resource_named(r, kSix[k])) return true;
    return false;
}

}  // namespace

void install() {
    if (!config::get_bool("Stereo", "PrepassGpuStages", false)) return;
    d3d::set_bind_tap(d3d::kBindTapGpuStages, &bind_tap);
    d3d::add_frame_end_listener(&on_frame_end);
    {
        std::lock_guard lk(g_mx);
        reset_sums();
    }
    // (item 8b) DVL call 1's return, for the walk census only: a rel32 call of DrawVisList at MainDvlCall (main_inst's check)
    if (!anchors::stand_down() && anchors::exe_matches() && !anchors::relocated() && anchors::present(Id::MainDvlCall)) {
        const uintptr_t site = anchors::addr(Id::MainDvlCall);
        uint8_t op = 0;
        int32_t rel = 0;
        if (rd(site, &op) && op == 0xe8 && rd(site + 1, &rel) && site + 5 + static_cast<intptr_t>(rel) == anchors::addr(Id::DrawVisList))
            g_walk_ret = site + 5;
    }
    g_installed = true;
    g_on_flag = true;
    log::info("[gpustages] [Stereo] PrepassGpuStages on (test only: GPU timestamps at the passes' and the pre-pass's stage boundaries, "
              "\"prepass gpu\")");
    if (!config::get_bool("Stereo", "PrepassInstanced", false))
        log::warn("[gpustages] without [Stereo] PrepassInstanced=1 the walk-end marker is recorded only while \"prepass on\": no pre-pass "
                  "spans in two-pass");
}

void mark(int point) {
    if (point < 1 || point > 4 || !on()) return;
    g_marks.fetch_add(1, std::memory_order_relaxed);
    if (!prepass::stream_marker(kMarks[point - 1])) g_marks_failed.fetch_add(1, std::memory_order_relaxed);
}

uintptr_t walk_ret() { return g_walk_ret; }

// ---- (item 8c) the CPU census: the render thread's own times, summed under g_cmx (the command thread reads)
namespace {
std::mutex g_cmx;
thread_local int64_t t_c_open = 0, t_c_e1 = 0;
thread_local int t_c_pass = 0;
thread_local bool t_c_in_e1 = false;
thread_local int64_t t_c_dve = 0;  // this call's e1 entity ticks
thread_local uint32_t t_c_dve_n = 0;
double g_c_dve[2] = {}, g_c_dve_n[2] = {};
double g_qpc_ms = 0;
double g_c_dvl[2] = {}, g_c_e1[2] = {}, g_c_rest[2] = {};
uint64_t g_c_n[2] = {}, g_c_noe1[2] = {}, g_c_bad = 0;
double g_c_window = 0;
int64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
}  // namespace

void cpu_open(int pass) {
    if (pass != 1 && pass != 2) return;
    t_c_pass = pass;
    t_c_e1 = 0;
    t_c_dve = 0;
    t_c_dve_n = 0;
    t_c_in_e1 = true;
    t_c_open = qpc();
}
void cpu_e1(int pass) {
    if (pass == t_c_pass && t_c_open && !t_c_e1) t_c_e1 = qpc();
    t_c_in_e1 = false;
}
bool cpu_in_e1() { return t_c_in_e1; }
void cpu_dve(int64_t ticks) {
    t_c_dve += ticks;
    ++t_c_dve_n;
}
void cpu_close(int pass) {
    const int64_t now = qpc();
    t_c_in_e1 = false;
    if (pass != t_c_pass || !t_c_open) {
        std::lock_guard lk(g_cmx);
        ++g_c_bad;
        return;
    }
    if (g_qpc_ms == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpc_ms = f.QuadPart ? 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
    }
    const int p = pass - 1;
    std::lock_guard lk(g_cmx);
    g_c_dvl[p] += static_cast<double>(now - t_c_open) * g_qpc_ms;
    if (t_c_e1) {
        g_c_e1[p] += static_cast<double>(t_c_e1 - t_c_open) * g_qpc_ms;
        g_c_rest[p] += static_cast<double>(now - t_c_e1) * g_qpc_ms;
        g_c_dve[p] += static_cast<double>(t_c_dve) * g_qpc_ms;
        g_c_dve_n[p] += static_cast<double>(t_c_dve_n);
    } else {
        ++g_c_noe1[p];
    }
    ++g_c_n[p];
    t_c_open = t_c_e1 = 0;
    t_c_pass = 0;
}

std::string cpu_command(const std::vector<std::string>& t) {
    if (!g_walk_ret) return "prepass gpu cpu: refused (MainDvlCall is not a call of DrawVisList on this build)";
    if (t.size() > 4 || (t.size() == 4 && t[3] != "on" && t[3] != "off" && t[3] != "reset")) return "ERROR usage: prepass gpu cpu [on|off|reset]";
    std::lock_guard lk(g_cmx);
    if (t.size() == 4 && t[3] == "on") g_cpu_flag = true;
    if (t.size() == 4 && t[3] == "off") g_cpu_flag = false;
    if (t.size() == 4 && t[3] == "reset") {
        for (int p = 0; p < 2; ++p) {
            g_c_dvl[p] = g_c_e1[p] = g_c_rest[p] = g_c_dve[p] = g_c_dve_n[p] = 0;
            g_c_n[p] = g_c_noe1[p] = 0;
        }
        g_c_bad = 0;
        g_c_window = log::now_ms();
    }
    auto m = [](double s, uint64_t n) { return n ? s / static_cast<double>(n) : 0.0; };
    const uint64_t e1n[2] = {g_c_n[0] - g_c_noe1[0], g_c_n[1] - g_c_noe1[1]};
    char b[700];
    std::snprintf(b, sizeof(b),
                  "prepass gpu cpu %s: over %.1f s, calls p1 %llu p2 %llu (no e1 p1 %llu p2 %llu, bad %llu) | dvl1 p1 %.4f ms p2 %.4f ms | "
                  "e1 p1 %.4f ms p2 %.4f ms | rest p1 %.4f ms p2 %.4f ms | dve in e1 p1 %.4f ms (%.1f calls) p2 %.4f ms (%.1f calls)",
                  cpu_on() ? "on" : "off", (log::now_ms() - g_c_window) / 1000.0, static_cast<unsigned long long>(g_c_n[0]),
                  static_cast<unsigned long long>(g_c_n[1]), static_cast<unsigned long long>(g_c_noe1[0]),
                  static_cast<unsigned long long>(g_c_noe1[1]), static_cast<unsigned long long>(g_c_bad), m(g_c_dvl[0], g_c_n[0]),
                  m(g_c_dvl[1], g_c_n[1]), m(g_c_e1[0], e1n[0]), m(g_c_e1[1], e1n[1]), m(g_c_rest[0], e1n[0]), m(g_c_rest[1], e1n[1]),
                  m(g_c_dve[0], e1n[0]), m(g_c_dve_n[0], e1n[0]), m(g_c_dve[1], e1n[1]), m(g_c_dve_n[1], e1n[1]));
    return b;
}

void walk_mark(int pass, int point) {
    if ((pass != 1 && pass != 2) || (point != 1 && point != 2) || !on() || !walk_on()) return;
    static void (*const kW[2][2])(void*) = {{&cb_wopen1, &cb_we1_1}, {&cb_wopen2, &cb_we1_2}};
    g_wmarks.fetch_add(1, std::memory_order_relaxed);
    if (!prepass::stream_marker(kW[pass - 1][point - 1])) g_wmarks_failed.fetch_add(1, std::memory_order_relaxed);
}

bool walk_draw(ID3D12GraphicsCommandList* cl) {
    if (!mine() || !g_wseg || !on()) return false;  // (g_wseg: written and read on this thread only)
    uint64_t vs = 0, ps = 0;
    uint32_t wr = 0;
    const bool known = pso::current(cl, &vs, &ps, &wr);
    const uint64_t dvs = g_wdrop_vs.load(std::memory_order_relaxed);
    if (dvs && g_wseg == 2 && known && vs == dvs && ps == g_wdrop_ps.load(std::memory_order_relaxed) && !main_inst::listening()) {
        std::lock_guard lk(g_mx);  // (the ablation: pass 2's draw of the pair dropped; not a run of the walk)
        ++g_wdropped;
        return true;
    }
    const int mode = g_cur >= 0 ? g_slots[g_cur].wmode : 0;  // (the frame's mode, set at its open on this thread)
    uint8_t kind = 0xff;
    if (g_wowed & 1) kind = kWOpen;
    else if (g_wowed & 2) kind = kWE1;
    else if (mode == 2 && g_wlast >= 0 && (known != g_wknown || vs != g_wvs || ps != g_wps)) kind = kWRun;
    if (kind == kWOpen && (g_wowed & 2)) {  // (e1 owed with the open: no draw between them; e1 here too, after the open)
        went(cl, kWOpen, vs, ps, known);
        kind = kWE1;
    }
    if (kind != 0xff) {
        went(cl, kind, vs, ps, known);
        g_wowed = 0;
    }
    g_wvs = vs;
    g_wps = ps;
    g_wknown = known;
    if (g_wlast >= 0 && g_wslot == g_cur && g_cur >= 0) ++g_went[g_wslot][g_wlast].draws;  // (g_cur: written on this thread only)
    return false;
}

void walk_copy(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst) {
    if (!mine() || !g_wseg || !on() || !dst || !d3d::resource_named(dst, "Depth Resolve")) return;
    if (g_wowed & 1) went(cl, kWOpen, 0, 0, false);  // (owed with no draw since: stamped here, at the close's point)
    if (g_wowed & 2) went(cl, kWE1, 0, 0, false);
    went(cl, kWClose, 0, 0, false);
    std::lock_guard lk(g_mx);
    g_wseg = 0;
    g_wowed = 0;
    g_wlast = -1;
    g_wknown = false;
}

void marginal(ID3D12GraphicsCommandList* cl, int point, int ord) {  // (item 8e, the playback thread: main_inst's dup frames)
    if (point < 0 || point > 2 || ord < 0 || ord >= kMOrds || !mine() || !on() || !cl) return;
    std::lock_guard lk(g_mx);
    if (g_cur < 0 || !g_ready) return;
    Slot& s = g_slots[g_cur];
    if (s.wmode != 1 || !s.mgon || s.wover) {  // (only "walk seg" frames of the switch; a frame over its area is not read)
        ++g_mg_refused;
        return;
    }
    if (cl->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        ++g_wnondirect;
        s.wover = true;
        return;
    }
    if (s.wn >= kRuns) {
        s.wover = true;
        return;
    }
    const int k = s.wn++;
    g_went[g_cur][k] = WEnt{static_cast<uint8_t>(kWM0 + point), static_cast<uint8_t>(g_pass), static_cast<uint16_t>(ord), 0, cl};
    cl->EndQuery(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(kSlots * kQ + g_cur * kRuns + k));
    s.marg = true;
    ++g_mg_stamps;
}

void walk_end_played() {
    if (!on() || !mine()) return;
    std::lock_guard lk(g_mx);
    if (g_cur < 0 || (g_pass != 1 && g_pass != 2) || !g_span[g_pass]) return;
    g_span[g_pass] = false;
    g_pending.fetch_or(1u << (g_pass == 1 ? kP1E : kP2E));
}

void depth_clear_seen(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE game_view) {
    if (!mine() || g_cur < 0 || (g_pass != 1 && g_pass != 2)) return;  // (g_cur and g_pass: written on this thread only)
    const int id = g_pass == 1 ? kP1B : kP2B;
    if ((g_slots[g_cur].mask >> id) & 1) return;  // (the pass's first only; the mask is written on this thread only)
    if (!named_six(game_view, true)) return;  // (d3d's name lock, never under g_mx)
    std::lock_guard lk(g_mx);
    if (g_cur < 0) return;
    stamp(cl, id);
    g_span[g_pass] = true;
}

int clear_begin(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE game_view, bool depth) {
    if (!mine() || g_cur < 0 || (g_pass != 1 && g_pass != 2) || !g_span[g_pass]) return -1;
    if (!named_six(game_view, depth)) return -1;
    std::lock_guard lk(g_mx);
    if (g_cur < 0) return -1;
    Slot& s = g_slots[g_cur];
    if (s.clears >= kClearPairs) return -1;
    const int j = s.clears++;
    s.clear_pass[j] = static_cast<uint8_t>(g_pass);
    stamp(cl, kFixed + 2 * j);
    return j;
}

void clear_end(ID3D12GraphicsCommandList* cl, int token) {
    if (token < 0 || token >= kClearPairs || !mine()) return;
    std::lock_guard lk(g_mx);
    stamp(cl, kFixed + 2 * token + 1);
}

int copies_begin(ID3D12GraphicsCommandList* cl, int slice, bool handover) {
    if (!mine()) return -1;
    std::lock_guard lk(g_mx);
    if (g_cur < 0) return -1;
    const int id = handover ? kHOC : slice == 0 ? kR1C : kR2C;
    stamp(cl, id);
    return id;
}

void copies_none(ID3D12GraphicsCommandList* cl) {
    if (!mine()) return;
    std::lock_guard lk(g_mx);
    const uint32_t p = g_pending.load(std::memory_order_relaxed) & kPreEnds;  // the walk's end owed: here, no copies came
    for (int id : {kP1E, kP2E})
        if (p & (1u << id)) stamp(cl, id);
    g_pending.fetch_and(~p);
}

void copies_end(ID3D12GraphicsCommandList* cl, int token) {
    if ((token != kR1C && token != kR2C && token != kHOC) || !mine()) return;
    std::lock_guard lk(g_mx);
    stamp(cl, token + 1);
    const uint32_t p = g_pending.load(std::memory_order_relaxed) & kPreEnds;  // the walk's end owed: after the copies
    for (int id : {kP1E, kP2E})
        if (p & (1u << id)) stamp(cl, id);
    g_pending.fetch_and(~p);
}

namespace {
// (item 8b) "prepass gpu walk [off|seg|families|reset|drop <vs> <ps>|nodrop]": the walk census and its status
std::string walk_command(const std::vector<std::string>& t) {
    const char* const usage = "ERROR usage: prepass gpu walk [off|seg|families|reset|drop <vs 16 hex> <ps 16 hex>|nodrop|marginal on|off]";
    if (!g_walk_ret) return "prepass gpu walk: refused (MainDvlCall is not a call of DrawVisList on this build)";
    if (t.size() > 3) {
        const std::string& a = t[3];
        if (a == "off" || a == "seg" || a == "families") {
            if (t.size() != 4) return usage;
            if (a != "off") pso::arm_census();  // (the draws' pairs: pso::current; armed for good, as the frame graph does)
            g_walk_flag = a == "off" ? 0 : a == "seg" ? 1 : 2;
        } else if (a == "reset") {
            if (t.size() != 4) return usage;
            std::lock_guard lk(g_mx);
            reset_sums();
        } else if (a == "drop") {
            if (t.size() != 6 || t[4].size() != 16 || t[5].size() != 16) return usage;
            for (int k = 4; k <= 5; ++k)
                for (char ch : t[k])
                    if (!std::isxdigit(static_cast<unsigned char>(ch))) return usage;
            const uint64_t vs = std::strtoull(t[4].c_str(), nullptr, 16), ps = std::strtoull(t[5].c_str(), nullptr, 16);
            if (!vs) return usage;  // (vs set is the drop's flag; ps 0 = no pixel shader)
            g_wdrop_ps = ps;
            g_wdrop_vs = vs;
        } else if (a == "nodrop") {
            if (t.size() != 4) return usage;
            g_wdrop_vs = 0;
            g_wdrop_ps = 0;
        } else if (a == "marginal") {  // (item 8e, test only: main_inst's dup frames, "walk seg" frames only)
            if (t.size() != 5 || (t[4] != "on" && t[4] != "off")) return usage;
            g_marg_flag = t[4] == "on";
        } else {
            return usage;
        }
    }
    std::lock_guard lk(g_mx);
    const int mode = g_walk_flag.load();
    auto mean = [](const Acc& a) { return a.n ? a.sum / static_cast<double>(a.n) : 0.0; };
    std::string r;
    char b[640];
    std::snprintf(b, sizeof(b),
                  "prepass gpu walk %s%s: frames p1 %llu p2 %llu (families p1 %llu p2 %llu; bad p1 %llu p2 %llu; no e1 p1 %llu p2 %llu; over %llu, "
                  "unclosed %llu, pass mismatch %llu, non-direct %llu; markers %llu, failed %llu; list changes a frame p1 %.2f p2 %.2f; drop %016llx/%016llx, "
                  "dropped %llu; max entries %llu) | e p1 %.4f ms p2 %.4f ms | e1 p1 %.4f ms p2 %.4f ms | rest p1 %.4f ms p2 %.4f ms | pairs %d (full %llu)",
                  mode == 0 ? "off" : mode == 1 ? "seg" : "families", on() ? "" : " (stages off)",
                  static_cast<unsigned long long>(g_wframes[0]), static_cast<unsigned long long>(g_wframes[1]),
                  static_cast<unsigned long long>(g_wfam_frames[0]), static_cast<unsigned long long>(g_wfam_frames[1]),
                  static_cast<unsigned long long>(g_wbad[0]), static_cast<unsigned long long>(g_wbad[1]),
                  static_cast<unsigned long long>(g_wnoe1[0]), static_cast<unsigned long long>(g_wnoe1[1]),
                  static_cast<unsigned long long>(g_wover_n), static_cast<unsigned long long>(g_wunclosed),
                  static_cast<unsigned long long>(g_wmismatch), static_cast<unsigned long long>(g_wnondirect),
                  static_cast<unsigned long long>(g_wmarks.load()), static_cast<unsigned long long>(g_wmarks_failed.load()),
                  g_wframes[0] ? static_cast<double>(g_wlists[0]) / static_cast<double>(g_wframes[0]) : 0.0,
                  g_wframes[1] ? static_cast<double>(g_wlists[1]) / static_cast<double>(g_wframes[1]) : 0.0,
                  static_cast<unsigned long long>(g_wdrop_vs.load()), static_cast<unsigned long long>(g_wdrop_ps.load()),
                  static_cast<unsigned long long>(g_wdropped), static_cast<unsigned long long>(g_wmax), mean(g_wseg_acc[0]), mean(g_wseg_acc[1]), mean(g_we1_acc[0]),
                  mean(g_we1_acc[1]), mean(g_wrest_acc[0]), mean(g_wrest_acc[1]), g_npairs, static_cast<unsigned long long>(g_pair_full));
    r = b;
    // per pair, by pass 2's time a frame: "fam <vs> <ps> p1 <ms> <draws> <runs> p2 <ms> <draws> <runs>" (means a frame)
    int order[kPairs + 1];
    int n = 0;
    for (int k = 0; k <= kPairs; ++k)
        if (g_fam[k].runs[0] || g_fam[k].runs[1]) order[n++] = k;
    std::sort(order, order + n, [](int x, int y) { return g_fam[x].ms[1] > g_fam[y].ms[1]; });
    const double f1 = g_wfam_frames[0] ? static_cast<double>(g_wfam_frames[0]) : 1.0, f2 = g_wfam_frames[1] ? static_cast<double>(g_wfam_frames[1]) : 1.0;
    for (int j = 0; j < n; ++j) {
        const int k = order[j];
        const FamAcc& f = g_fam[k];
        if (k == kPairs) std::snprintf(b, sizeof(b), " | fam other");
        else std::snprintf(b, sizeof(b), " | fam %016llx %016llx", static_cast<unsigned long long>(g_pair[k].vs), static_cast<unsigned long long>(g_pair[k].ps));
        r += b;
        std::snprintf(b, sizeof(b), " p1 %.4f %.1f %.1f p2 %.4f %.1f %.1f", f.ms[0] / f1, static_cast<double>(f.draws[0]) / f1,
                      static_cast<double>(f.runs[0]) / f1, f.ms[1] / f2, static_cast<double>(f.draws[1]) / f2, static_cast<double>(f.runs[1]) / f2);
        r += b;
    }
    // (item 8e) the marginal stamps: "mg <ordinal> <twin ms> <R0 ms> <twin - R0 ms> <draws>", each a dup frame's mean
    std::snprintf(b, sizeof(b),
                  " | marginal %s%s: dup frames %llu, (after R0 - before twin) summed %.4f ms a dup frame; e1 p1 dup %.4f ms (%llu) other %.4f ms (%llu): "
                  "%+.4f; stamps %llu, refused %llu, triples broken %llu",
                  g_marg_flag.load() ? "on" : "off", mode == 1 ? "" : " (recorded in \"walk seg\" frames only)",
                  static_cast<unsigned long long>(g_mg_tot.n), mean(g_mg_tot), mean(g_mg_e1_dup), static_cast<unsigned long long>(g_mg_e1_dup.n),
                  mean(g_mg_e1_nd), static_cast<unsigned long long>(g_mg_e1_nd.n), mean(g_mg_e1_dup) - mean(g_mg_e1_nd),
                  static_cast<unsigned long long>(g_mg_stamps), static_cast<unsigned long long>(g_mg_refused), static_cast<unsigned long long>(g_mg_broken));
    r += b;
    const double fm = g_mg_tot.n ? static_cast<double>(g_mg_tot.n) : 1.0;
    for (int o = 0; o < kMOrds; ++o) {
        if (!g_mg_n[o]) continue;
        std::snprintf(b, sizeof(b), " | mg %d %.4f %.4f %.4f %.1f", o, g_mg_twin[o] / fm, g_mg_r0[o] / fm, (g_mg_twin[o] - g_mg_r0[o]) / fm,
                      static_cast<double>(g_mg_n[o]) / fm);
        r += b;
    }
    return r;
}
}  // namespace

std::string command(const std::vector<std::string>& t) {
    if (!g_installed.load()) return "prepass gpu: not installed (start with [Stereo] PrepassGpuStages=1; test only)";
    if (t.size() >= 3 && t[2] == "walk") return walk_command(t);
    if (t.size() >= 3 && t[2] == "cpu") return cpu_command(t);
    if (t.size() > 3 || (t.size() == 3 && t[2] != "on" && t[2] != "off" && t[2] != "reset")) return "ERROR usage: prepass gpu [on|off|reset]";
    std::lock_guard lk(g_mx);
    if (t.size() == 3 && t[2] == "on") g_on_flag = !g_failed;
    if (t.size() == 3 && t[2] == "off") {
        g_on_flag = false;
        if (g_cur >= 0) g_slots[g_cur].drop = true;  // (the open frame: freed at its close, never resolved; cb_begin1 runs when on again)
        g_pending = 0;
    }
    if (t.size() == 3 && t[2] == "reset") reset_sums();
    char b[2600];
    int n = std::snprintf(b, sizeof(b),
                          "prepass gpu %s%s: frames %llu over %.1f s (untimed: busy %llu, owed points dropped %llu, bad %llu, unresolved %llu; "
                          "markers %llu, failed %llu; queue re-pointed %llu)",
                          on() ? "on" : "off", g_failed ? " (stood down)" : g_ready ? "" : " (not ready)",
                          static_cast<unsigned long long>(g_frames), (log::now_ms() - g_window_ms) / 1000.0,
                          static_cast<unsigned long long>(g_skipped), static_cast<unsigned long long>(g_dropped),
                          static_cast<unsigned long long>(g_bad), static_cast<unsigned long long>(g_unresolved),
                          static_cast<unsigned long long>(g_marks.load()), static_cast<unsigned long long>(g_marks_failed.load()),
                          static_cast<unsigned long long>(g_requeued));
    for (int k = 0; k < kStageCount && n > 0 && n < static_cast<int>(sizeof(b)); ++k)
        n += std::snprintf(b + n, sizeof(b) - static_cast<size_t>(n), " | %s %.4f ms (%llu)", kStages[k].name,
                           g_acc[k].n ? g_acc[k].sum / static_cast<double>(g_acc[k].n) : 0.0, static_cast<unsigned long long>(g_acc[k].n));
    for (int p = 0; p < 2 && n > 0 && n < static_cast<int>(sizeof(b)); ++p)
        n += std::snprintf(b + n, sizeof(b) - static_cast<size_t>(n), " | clears%d %.4f ms (%llu; %.2f clears a frame)", p + 1,
                           g_clr[p].n ? g_clr[p].sum / static_cast<double>(g_clr[p].n) : 0.0, static_cast<unsigned long long>(g_clr[p].n),
                           g_clr[p].n ? static_cast<double>(g_clr_n[p]) / static_cast<double>(g_clr[p].n) : 0.0);
    if (n > 0 && n < static_cast<int>(sizeof(b)))
        std::snprintf(b + n, sizeof(b) - static_cast<size_t>(n), " | one list: eye1 %llu of %llu, eye2 %llu of %llu",
                      static_cast<unsigned long long>(g_same[0]), static_cast<unsigned long long>(g_eye_n[0]),
                      static_cast<unsigned long long>(g_same[1]), static_cast<unsigned long long>(g_eye_n[1]));
    return b;
}

}  // namespace rdrvr::gpu_stages
