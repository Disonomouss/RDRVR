#include "core/prepass_arrays.h"

#include <windows.h>
#include <d3d12.h>

#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>

#include "core/anchors.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/cut_velocity.h"
#include "core/hooks.h"
#include "core/inst_prev_mirror.h"
#include "core/d3d_hooks.h"
#include "core/dual_pass.h"
#include "core/gpu_stages.h"
#include "core/log.h"
#include "core/prevworld_mirror.h"
#include "core/eye2_collector.h"
#include "core/eye2_light.h"
#include "core/eye2_velocity.h"
#include "core/main_instanced.h"
#include "core/pso.h"
#include "core/rec_state.h"
#include "core/render_settings.h"
#include "core/state.h"

// draw_masks.asm: [Stereo] PrepassWalkCut's switch for the render thread's pass 2 (0 = the stub as before)
extern "C" volatile unsigned char rdrvr_prepass_cut = 0;
extern "C" volatile unsigned long rdrvr_prepass_cut_tid = 0;
extern "C" volatile unsigned long long rdrvr_prepass_cuts = 0;

namespace rdrvr::prepass {
std::atomic<bool> g_cut_pending_flag{false};  // (prepass_arrays.h cut_pending)
std::atomic<bool> g_cut_armed_flag{false};    // (prepass_arrays.h cut_tracking: the cut on at least once; never cleared)
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
    D3D12_CPU_DESCRIPTOR_HANDLE view[3]{};  // slice 0, slice 1, both (the instanced region's)
};
Arr g_arr[kKinds];
struct Retired {
    ID3D12Resource* res;
    uint64_t frame;
};
std::vector<Retired> g_retired;
ID3D12Device* g_dev = nullptr;
ID3D12DescriptorHeap* g_rtv_heap = nullptr;  // 3 per colour kind
ID3D12DescriptorHeap* g_dsv_heap = nullptr;  // 3
UINT g_rtv_inc = 0, g_dsv_inc = 0;

// the open region (the playback thread only)
struct Region {
    ID3D12GraphicsCommandList* cl = nullptr;
    int slice = 0;
    bool inst = false, drop = false;  // [Stereo] PrepassInstanced: pass 1's region drawing both slices, pass 2's dropping
    bool all_six = false;             // the last bind's targets all among the six (the twins' rule, both passes)
    bool started = false;             // the walk-start marker played back: twins (pass 1) and drops (pass 2) only after it
    bool no_twins = false;            // pass 1's walk not ready for instancing this frame (its marker said so)
    ID3D12PipelineState* game_pso = nullptr;  // the game's last pipeline in the region, and the one set on the list
    ID3D12PipelineState* set_pso = nullptr;
    ID3D12PipelineState* game_twin = nullptr;  // game_pso's twin (looked up once, at the game's set)
    bool twin_ip = false;  // ([Stereo] PrepassInstPrevMirror) game_twin is an instprev VS's: drawn only where the draw was mirrored
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

void on_fail();  // (below: the instanced region's state)
void fail(const char* why) {
    on_fail();
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
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3 * kKinds, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtv_heap)))) return false;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 3;
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
    for (int s = 0; s < 3; ++s) {  // (s 2: both slices)
        const UINT first = s < 2 ? static_cast<UINT>(s) : 0u, count = s < 2 ? 1u : 2u;
        if (is_depth(k)) {
            D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
            dv.Format = vf;
            dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            dv.Texture2DArray = {0, first, count};
            a.view[s] = g_dsv_heap->GetCPUDescriptorHandleForHeapStart();
            a.view[s].ptr += static_cast<SIZE_T>(s) * g_dsv_inc;
            g_dev->CreateDepthStencilView(arr, &dv, a.view[s]);
        } else {
            D3D12_RENDER_TARGET_VIEW_DESC rv{};
            rv.Format = vf;
            rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rv.Texture2DArray = {0, first, count, 0};
            a.view[s] = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
            a.view[s].ptr += static_cast<SIZE_T>(k * 3 + s) * g_rtv_inc;
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

// ---- [Stereo] PrepassInstanced (the playback thread)
std::atomic<bool> g_inst_hooked{false}, g_inst_on{false}, g_pred_off{false};
bool g_frame_inst = false;  // this frame's pass 1 region was instanced (pass 2's drops follow it)
unsigned g_frame_cleared = 0;  // the kinds pass 1's instanced region cleared on both slices (pass 2 skips only those)
thread_local bool t_skip_clear = false;

// ---- [Stereo] PrepassWalkCut (2026-10-15, run 13; test only, off; after optimumbox's ViSkipWalk with the fork review's
// fixes, research\sps\fork-port-plan.md P1). In pass 2 of a frame whose pass 1 drew the pre-pass for both eyes, the
// pre-pass routine (FUN_140706050, its pass+0x7f = 1 call) returns at entry (draw_masks.asm), so eye 2's pre-pass is
// neither walked nor recorded: 4b's CPU saving. Eye 2's images are pass 1's slice 1: at pass 2's walk end (its marker
// is recorded outside the cut) each kind pass 1's region touched is copied from slice 1 into the game's target, as pass
// 2's own region would have done, from the game targets' states followed since pass 1's region (their barriers; nothing
// else moves them). A pass-2 region the game opens before the walk (its clears are the caller's) ends the same way, the
// kinds it did not touch added. The cut is held off for kCutHold frames after any pass 1 single draw (a pipeline
// without a twin, whatever the reason: pass 2 no longer draws it for eye 2) or any handover problem.
constexpr uint64_t kCutHold = 600;
std::atomic<bool> g_cut_on{false}, g_cut_force{false};
bool g_cut_ready = false;  // the render thread: this frame's pass 1 walk start was ready (hk_prewalk)
uint64_t g_cut_decided_frame = ~0ull;  // ([Stereo] PrepassInstPrevMirror) the frame whose pass 2 walk the decision cut (stub or record skip)
uint64_t g_cut_seq = 0;  // (item 7's S2, [Stereo] Pass2FrontSkip) the render thread: walk_cut_decide's calls (one per double frame's between_passes)
uint64_t g_cut_hold_until = 0, g_cut_seen_single = 0, g_cut_seen_bad = 0;  // the render thread
unsigned long long g_cuts_at_decide = 0;  // the render thread: the stub's count when this frame's cut was decided
std::atomic<uint64_t> g_cut_frames{0}, g_cut_holds{0}, g_cut_handovers{0}, g_cut_region_ends{0}, g_cut_bad{0}, g_trk_mismatch{0};
// (run 19 item 2, velocity.md 8) decide_cut's declines, one counter per return path, the hold by the cause of its last
// extension (1 started singles, 2 not-started singles, 3 problems); and the cut frames whose pass 2 cut more than one
// +0x7f = 1 call of 0x706050 (the stub's count against the decision's), with the calls past the first
std::atomic<uint64_t> g_cutd_calls{0}, g_cutd_off{0}, g_cutd_state{0}, g_cutd_unready{0}, g_cutd_notwins{0}, g_cutd_held_started{0},
    g_cutd_held_unstarted{0}, g_cutd_held_bad{0}, g_cutd_instprev{0}, g_cutd_nomarker{0}, g_cut_multi{0}, g_cut_extra{0};
int g_cut_hold_cause = 0;  // the render thread
// [Stereo] PrepassCutHoldStarted (velocity.md 8.4; test only, off): the hold extended only by pass 1's started singles (walk
// draws without a twin, which the cut would lose for eye 2) or problems, not by not-started ones (a mono frame's walk, or
// 0x5cbfd0's draws before the walk-start marker, which pass 2 makes itself under the cut). Off: as before (any single)
std::atomic<bool> g_cut_hold_started{false};
uint64_t g_cut_seen_started = 0, g_cut_seen_unstarted = 0;  // the render thread
// [Stereo] PrepassCutHoldPending (research\run25\item8d.md 11; test only, off; read at install with PrepassWalkCut): a pass 1 single
// whose pipeline's twin is pending in auto mode (pso's region table: kRsQueued, a shader still converting, or kRsReadyNotBuilt, to
// the builder or built and not yet installed) holds the cut only until that twin is installed plus kHoldPendMargin presents (the
// playback's single counts lag the install), not kCutHold. A single of any other pipeline (refused, held, shape, no rule, build
// failed, a stream creation, not listed, or a twin installed more than the margin ago: single for another reason) and a single of no
// known pipeline keep the kCutHold hold. The render thread reads "prepass singles"' table (the playback thread's bounded set of
// pipelines: no lock, no allocation per draw) once a decision against its own snapshot, and looks the moved pipelines up lock-free
// (pso::twin_state). A pending entry whose pipeline drew no new single for kCutHold presents goes (the hold without the switch would
// have ended). Off at start: nothing of it runs; off at run time ("prepass cut holdpending off"): a pending hold left becomes the
// kCutHold one, and the decision is as before.
// (the review's must-fix) the short hold only where the install ends the singles: a pipeline whose singles were ever single for a
// reason no twin cures (kSnNotAllSix, kSnInstPrev, kSnNoTwins since the last "prepass singles reset") is permanent; a twin installed
// that twin_of would not hand to the pre-pass now (pso::twin_given_now: prevworld or eye2img kinds, wvp or instprev with their
// mirror off, a main-set pipeline under the main withhold) is permanent at once (the kCutHold hold from that decision, never the
// margin), at the install (hp_hold) as when first seen installed (hp_scan)
constexpr uint64_t kHoldPendMargin = 8;
constexpr uint64_t kHoldPendQuiet = 4;  // (and at least this many presents past such a pipeline's last single seen: hp_quiet)
constexpr int kHoldPendSlots = 32;
bool g_hp_armed = false;            // [Stereo] PrepassCutHoldPending=1 at install (read-only after)
std::atomic<bool> g_hp_on{false};   // the session's switch (armed only)
struct HpWait {
    const void* game;
    uint32_t gen;         // the region table's creation number (an address made again is another pipeline)
    uint64_t since, last;  // the present of the decision that first saw it pending, of the last one that saw a new single of it
};
HpWait g_hp_wait[kHoldPendSlots];  // the render thread: the pending pipelines holding the cut
int g_hp_n = 0;                    // the render thread
uint64_t g_hp_until = 0;           // the render thread: the pending hold's end (an installed twin's present + the margin)
// holds begun by kind (pending: no hold of either kind before; permanent: the kCutHold hold begun by singles under the switch),
// pending pipelines listed (and those first seen already installed within the margin), cleared by their install, turned permanent
// (refused or failed while pending, made again at their address, or installed with a twin not handed to the pre-pass), expired,
// the list full; permanent pipelines seen, singles of no known pipeline (before the region's first bind, or the table's overflow);
// frames held by the pending hold alone; the longest pending wait (first seen to the install, presents); (the must-fix) of the
// permanent ones, installed twins twin_of would not hand out (at the install or first seen installed) and pipelines single for a
// reason no twin cures (pending or installed, permanent by g_sn_why)
std::atomic<uint64_t> g_hp_pend_holds{0}, g_hp_perm_holds{0}, g_hp_pend_added{0}, g_hp_made_seen{0}, g_hp_cleared{0}, g_hp_to_perm{0},
    g_hp_expired{0}, g_hp_full{0}, g_hp_perm_pipes{0}, g_hp_unknown{0}, g_hp_held{0}, g_hp_wait_max{0}, g_hp_not_given{0}, g_hp_why_perm{0};
int hp_scan(uint64_t f, bool started_only, bool on);  // (after the singles' table) bit 0 permanent, 1 pending, 2 permanent with started singles
bool hp_hold(uint64_t f, bool on);                    // the pending list resolved: true while the pending hold holds
// (the cut-hold review) the walk-start markers that failed to append (hk_prewalk: pass 1's walk then not ready), and the
// cut frames handed over with no pass 2 region (cut_poll: pass 2's draws before its walk went into the game's targets and
// the handover overwrites them; with PrepassCutHoldStarted on each one also counts as a problem for the hold)
std::atomic<uint64_t> g_start_marker_failed{0}, g_cut_noregion{0}, g_cut_noregion_held{0};
std::atomic<bool> g_cut_frame{false};  // the playback thread: this frame's pass 2 cut (its marker played, between the passes)
bool g_cut_region_end = false;         // the playback thread: the open region is a cut frame's pass 2
DWORD g_cut_pending_tid = 0;           // the thread the walk-end marker played on (the handover's)
unsigned g_frame_touched = 0;          // the kinds pass 1's instanced region touched this frame
D3D12_RESOURCE_STATES g_trk[kKinds][2]{};  // those game targets' states since pass 1's region (per plane)
unsigned g_trk_known = 0;              // bit (kind * 2 + plane)
DWORD g_trk_tid = 0;                   // (the playback thread: the game's barriers followed there only)
ID3D12GraphicsCommandList* g_trk_cl[kKinds][2]{};  // (r117 DG4) the list whose transition last set each plane's followed state
thread_local ID3D12GraphicsCommandList* t_poll_cl = nullptr;  // (the list of the cut_poll in progress on this thread)
std::atomic<uint64_t> g_follow_stale_reads{0};
std::atomic<uint64_t> g_follow_stale_user[kFollowUsers];
bool g_seeded = false;                 // pass 1's instanced region ended and seeded the follow since the last handover
                                       // (the review: the proof a cut frame's slice 1 is this frame's, not an old one)
void cut_frame_cb(void*) { g_cut_frame.store(true, std::memory_order_release); }
// the game's barriers on the followed targets (outside a region, and in one for the kinds it does not track)
void track(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_RESOURCE_BARRIER* b) {
    if (!b || GetCurrentThreadId() != g_trk_tid || !g_frame_touched) return;
    for (unsigned i = 0; i < n; ++i) {
        if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
        const int k = kind_of_resource(b[i].Transition.pResource);
        if (k < 0 || !(g_frame_touched & (1u << k))) continue;
        const UINT sub = b[i].Transition.Subresource;
        const int planes = g_arr[k].planes > 1 ? 2 : 1;
        if ((sub != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && static_cast<int>(sub) >= planes) ||
            b[i].Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE) {
            g_trk_known &= ~plane_bits(k);  // a split barrier or another subresource: not followed
            g_trk_mismatch.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        for (int p = 0; p < planes; ++p) {
            if (sub != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && static_cast<int>(sub) != p) continue;
            const unsigned bit = 1u << (k * 2 + p);
            if ((g_trk_known & bit) && b[i].Transition.StateBefore != g_trk[k][p]) {
                g_trk_mismatch.fetch_add(1, std::memory_order_relaxed);
                log::limited("prepass.trk", 10, "[prepass] walk cut: %s plane %d: the game's barrier from %#x, followed %#x", kNames[k], p,
                             static_cast<unsigned>(b[i].Transition.StateBefore), static_cast<unsigned>(g_trk[k][p]));
            }
            g_trk[k][p] = b[i].Transition.StateAfter;
            g_trk_known |= bit;
            g_trk_cl[k][p] = cl;  // (r117 DG4: this list's state from here)
        }
    }
}
// the kinds pass 1 touched and this region did not, with their followed states: false when one is not known
bool add_followed(unsigned* missing) {
    *missing = 0;
    for (int k = 0; k < kKinds; ++k) {
        if (!(g_frame_touched & (1u << k)) || (g_r.touched & (1u << k))) continue;
        if ((g_trk_known & plane_bits(k)) != plane_bits(k)) {
            *missing |= 1u << k;
            continue;
        }
        g_r.touched |= 1u << k;
        for (int p = 0; p < 2; ++p) g_r.state[k][p] = g_trk[k][p];
        g_r.known |= plane_bits(k);
    }
    return *missing == 0;
}
std::atomic<uint64_t> g_starts_ok{0}, g_starts_plain{0};
void on_fail() {  // a stand-down: the instancing off, the open region ended at its list's next call
    g_inst_on = false;
    g_pred_off = false;
    g_frame_inst = false;
    if (g_active.load()) g_end_pending.store(true, std::memory_order_release);
}
// the walk-start markers (op-0x4a callbacks recorded by hk_prewalk): pass 1 ready for instancing (the walk widened to
// both eyes, eye 2's rows written this frame) or not; pass 2's plain one marks where its drops may begin
// pass 1's singles (the playback thread; always counted): started (after the walk-start marker in a ready walk: the ones
// the cut would lose for eye 2), not started (before the marker, or in a region without one), started in a walk not ready
// (no twins and no cut that frame); and of the not-started, those before the marker in a region that got one (a double
// frame's pre-walk draws; the rest are regions without a marker: mono frames, or a failed append)
std::atomic<uint64_t> g_single_started{0}, g_single_unstarted{0}, g_single_unready{0}, g_single_prewalk{0};
uint64_t g_unstarted_open = 0;  // the playback thread: g_single_unstarted at pass 1's region open
// [Stereo] PrepassCutEmptyDraws (run 20 item 2, research/run20/nocb.md section 4; test only, off): a draw in a region with
// no instances or no vertices (the "nocb" instanced draws: the fill accepted no record, DrawIndexedInstanced(n, 0, ...))
// makes nothing in either eye, whatever is bound, so it is neither twinned nor single: no pipeline swap, no single count
// (so no hold), counted apart (pass 1 by started, pass 2). The game's no-op call runs. Off: as before
std::atomic<bool> g_empty_on{false};
std::atomic<uint64_t> g_empty_started{0}, g_empty_unstarted{0}, g_empty_p2{0};
void note_prewalk() {  // (pass 1's walk-start marker played: the region's not-started singles so far were before it)
    if (g_r.inst && !g_r.started)
        g_single_prewalk.fetch_add(g_single_unstarted.load(std::memory_order_relaxed) - g_unstarted_open, std::memory_order_relaxed);
}
void walk_start_ok_cb(void*) {
    if (!g_active.load()) return;
    note_prewalk();
    g_r.started = true;
    g_starts_ok.fetch_add(1, std::memory_order_relaxed);
}
void walk_start_plain_cb(void*) {
    if (!g_active.load()) return;
    note_prewalk();
    g_r.started = true;
    if (g_r.inst) {  // pass 1 not ready: no twins this frame, and pass 2 drops nothing
        g_r.no_twins = true;
        g_frame_inst = false;
    }
    g_starts_plain.fetch_add(1, std::memory_order_relaxed);
}
std::atomic<uint64_t> g_inst_regions{0}, g_drop_regions{0}, g_twin_draws{0}, g_single_draws{0}, g_dropped{0}, g_drawn{0},
    g_skipped_clears{0}, g_pso_restores{0};
int view_index() { return g_r.inst ? 2 : g_r.slice; }
void restore_pso(ID3D12GraphicsCommandList* cl) {  // the game's pipeline back on the list if a twin is set
    if (g_r.game_pso && g_r.set_pso && g_r.set_pso != g_r.game_pso) {
        pso::set_raw(cl, g_r.game_pso);
        g_pso_restores.fetch_add(1, std::memory_order_relaxed);
    }
    g_r.set_pso = g_r.game_pso;
}

// ---- run 21 item 4 (research/run19/gpu.md section 7; test only, each off by default): two cheaper reductions of
// end_region's GPU work (its copies at pass 1's region end, a cut pass 2 region's end and cut_poll's handover)
// [Stereo] PrepassSliceBarriers: the arrays' transitions to COPY_SOURCE and back name only the copied slice's subresources
// (slice + plane x 2: one mip, two slices), not ALL_SUBRESOURCES, so the other slice stays in its write state. Each pair is
// symmetric, so outside end_region every subresource of an array is still in write_state (its rest state): the instanced
// region's both-slices views, kind_rest_state's readers (eye2_collector's whole-array transitions) and ensure()'s initial
// state all still hold. The game targets' transitions, g_r.state, g_trk and the follow are not touched by it.
// [Stereo] PrepassSkipVelocityCopy: the Velocity RT kind's copy and its transitions skipped while nothing reads the game's
// Velocity RT (Ghidra, read only, gpu.md 7.2): the readers are the AA slot 0x873e60 (FSR 3/4, DLSS 5), the mod's TAA (2),
// the improved motion blur 0x8748a0 (PostFx +0x5a4: samples +0x3f0) and frame generation (the post chain hands +0x3f0's
// resource to the DLSS object's vt+0x90 every run); so: PostFx +0x868 is 0 or 1, both motion blur flags (+0x5a4, +0xe1c)
// are 0, for kSkipVelSettle presents in a row (the render thread may record a frame ahead of this playback), and [Render]
// ForceAntiAliasing is set (the mod's AA setter forces frame generation off). Any read failing: copied, as without it.
std::atomic<bool> g_sb_armed{false}, g_sb_on{false}, g_sv_armed{false}, g_sv_on{false};
std::atomic<uint64_t> g_sb_ends{0}, g_sv_skipped{0}, g_sv_tech{0}, g_sv_blur{0}, g_sv_settle{0}, g_sv_unread{0}, g_sv_nohook{0};
std::atomic<uint64_t> g_sv_since{~0ull};  // the present from which the condition held (~0: not holding)
std::atomic<int> g_sv_last_tech{-1};
constexpr uint64_t kSkipVelSettle = 8;
bool peek_raw(uintptr_t a, void* out, size_t n) {
    if (!a) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool skip_velocity_copy() {  // (the playback thread, end_region; true: the Velocity RT's copy not needed)
    if (!render_settings::aa_hook_installed()) {  // (the review) frame generation forced off only through the AA setter hook
        g_sv_since.store(~0ull, std::memory_order_relaxed);
        g_sv_nohook.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    uintptr_t pfx = 0;
    int tech = -1;
    unsigned char blur2 = 1, blur1 = 1;
    if (!peek_raw(anchors::addr(anchors::Id::PostFxSingleton), &pfx, sizeof(pfx)) || !pfx || !peek_raw(pfx + 0x868, &tech, sizeof(tech)) ||
        !peek_raw(pfx + 0x5a4, &blur2, 1) || !peek_raw(pfx + 0xe1c, &blur1, 1)) {
        g_sv_since.store(~0ull, std::memory_order_relaxed);
        g_sv_unread.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_sv_last_tech.store(tech, std::memory_order_relaxed);
    if (tech != 0 && tech != 1) {
        g_sv_since.store(~0ull, std::memory_order_relaxed);
        g_sv_tech.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (blur2 || blur1) {
        g_sv_since.store(~0ull, std::memory_order_relaxed);
        g_sv_blur.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const uint64_t now = state::presents.load(std::memory_order_relaxed);
    uint64_t since = g_sv_since.load(std::memory_order_relaxed);
    if (since == ~0ull || since > now) {
        since = now;
        g_sv_since.store(now, std::memory_order_relaxed);
    }
    if (now - since < kSkipVelSettle) {
        g_sv_settle.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

// The region's end: each touched target's slice copied into the game's target, from the states seen (per plane).
void end_region(ID3D12GraphicsCommandList* cl) {
    // ([Stereo] PrepassGpuStages, test only: the region's body ends; cut_poll's region is never active, as a cut pass 2's is flagged)
    const int gs = gpu_stages::on() ? gpu_stages::copies_begin(cl, g_r.slice, (g_cut_region_end && g_r.slice == 1) || !g_active.load()) : -1;
    g_active = false;
    g_end_pending = false;
    if (g_r.inst) restore_pso(cl);
    if (g_cut_region_end) {  // [Stereo] PrepassWalkCut: pass 2's region with its walk cut: pass 1's other kinds too
        g_cut_region_end = false;
        unsigned missing = 0;
        const bool seeded = g_seeded;
        g_seeded = false;
        if (g_r.slice == 1 && g_frame_inst && seeded && add_followed(&missing)) {
            g_cut_region_ends.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_cut_bad.fetch_add(1, std::memory_order_relaxed);
            log::limited("prepass.cutbad", 10, "[prepass] walk cut: pass 2's region (slice %d, pass 1 %s%s), followed states missing for kinds %#x: "
                         "eye 2 keeps eye 1's there this frame", g_r.slice, g_frame_inst ? "instanced" : "not instanced",
                         seeded ? "" : ", not this frame's", missing);
        }
    }
    unsigned need = 0;
    for (int k = 0; k < kKinds; ++k)
        if (g_r.touched & (1u << k)) need |= plane_bits(k);
    if ((g_r.known & need) != need) {
        fail("a pre-pass target's state not known at the region's end");
        if (gs >= 0) gpu_stages::copies_none(cl);  // ([Stereo] PrepassGpuStages: the owed walk end, no copies)
        return;
    }
    // (run 21 item 4, test only) decided once, so the transitions there and back agree
    const bool slice_bar = g_sb_on.load(std::memory_order_relaxed);
    const bool skip_vel = (g_r.touched & (1u << kVelocity)) && g_sv_on.load(std::memory_order_relaxed) && skip_velocity_copy();
    unsigned copied = g_r.touched;
    if (skip_vel) {
        copied &= ~(1u << kVelocity);
        g_sv_skipped.fetch_add(1, std::memory_order_relaxed);
    }
    D3D12_RESOURCE_BARRIER pre[4 * kKinds], post[4 * kKinds];
    UINT n = 0;
    for (int k = 0; k < kKinds; ++k) {
        if (!(copied & (1u << k))) continue;
        const Arr& a = g_arr[k];
        if (slice_bar && a.planes <= 2) {  // [Stereo] PrepassSliceBarriers: the copied slice's subresources only
            for (UINT p = 0; p < a.planes; ++p) {
                pre[n] = transition(a.arr, write_state(k), D3D12_RESOURCE_STATE_COPY_SOURCE);
                post[n] = transition(a.arr, D3D12_RESOURCE_STATE_COPY_SOURCE, write_state(k));
                pre[n].Transition.Subresource = post[n].Transition.Subresource = static_cast<UINT>(g_r.slice) + p * 2;
                ++n;
            }
        } else {
            pre[n] = transition(a.arr, write_state(k), D3D12_RESOURCE_STATE_COPY_SOURCE);
            post[n] = transition(a.arr, D3D12_RESOURCE_STATE_COPY_SOURCE, write_state(k));
            ++n;
        }
        for (UINT p = 0; p < a.planes && p < 2; ++p) {
            const D3D12_RESOURCE_STATES st = g_r.state[k][p];
            if (st == D3D12_RESOURCE_STATE_COPY_DEST) {
                fail("a pre-pass target already in COPY_DEST");
                if (gs >= 0) gpu_stages::copies_none(cl);
                return;
            }
            pre[n] = transition(a.game, st, D3D12_RESOURCE_STATE_COPY_DEST);
            post[n] = transition(a.game, D3D12_RESOURCE_STATE_COPY_DEST, st);
            if (a.planes > 1) pre[n].Transition.Subresource = post[n].Transition.Subresource = p;  // (one mip, one slice)
            ++n;
        }
    }
    if (!n && !skip_vel) {
        if (gs >= 0) gpu_stages::copies_none(cl);
        return;
    }
    if (n) {  // (n 0 with skip_vel: the Velocity RT was the only kind; nothing to copy, the follow below still seeded)
        if (slice_bar) g_sb_ends.fetch_add(1, std::memory_order_relaxed);
        d3d::raw_barrier(cl, n, pre);
        for (int k = 0; k < kKinds; ++k) {
            if (!(copied & (1u << k))) continue;
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
        if (gs >= 0) gpu_stages::copies_end(cl, gs);
    } else if (gs >= 0) {
        gpu_stages::copies_none(cl);
    }
    g_marker_ends.fetch_add(1, std::memory_order_relaxed);
    if (g_cut_armed_flag.load(std::memory_order_relaxed) && g_r.inst && g_r.slice == 0) {  // [Stereo] PrepassWalkCut: follow from here
        g_seeded = true;
        g_frame_touched = g_r.touched;
        g_trk_known = 0;
        for (int k = 0; k < kKinds; ++k)
            if (g_r.touched & (1u << k)) {
                for (int p = 0; p < 2; ++p) {
                    g_trk[k][p] = g_r.state[k][p];
                    g_trk_cl[k][p] = cl;  // (r117 DG4: the region's states, its own list's)
                }
                g_trk_known |= plane_bits(k);
            }
        g_trk_tid = GetCurrentThreadId();
    }
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

void walk_end_cb(void*) {
    if (gpu_stages::on()) gpu_stages::walk_end_played();  // ([Stereo] PrepassGpuStages, test only: this pass's pre-pass ends)
    // [Stereo] PrepassWalkCut: the first walk end after the cut frame's marker is pass 2's
    if (g_cut_frame.exchange(false, std::memory_order_acq_rel)) {
        if (!g_active.load()) {  // no region (the cut walk opened none): the handover at this thread's next list call
            g_cut_pending_tid = GetCurrentThreadId();
            g_cut_pending_flag.store(true, std::memory_order_release);
            return;
        }
        g_cut_region_end = true;
    }
    g_end_pending.store(true, std::memory_order_release);
}

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
// (item 8e 14, observe only) the same comparison bit for bit: frames whose prediction is pass 2's own exactly, the largest
// distance in ulps, and the last frames where gWorldView or gViewInverse was not exact (to set against the region's failing frames)
struct Exact {
    uint64_t eq = 0, ne_wv = 0, ne_vi = 0, pvp_eq = 0, ne_pvp = 0;
    uint32_t ulp_wv = 0, ulp_vi = 0, ulp_pvp = 0;
    uint64_t ring[16] = {};
    uint32_t ring_n = 0;
} g_vx;
uint32_t max_ulp(const float* a, const float* b, int n) {
    uint32_t m = 0;
    for (int i = 0; i < n; ++i) {
        int32_t x, y;
        std::memcpy(&x, a + i, 4);
        std::memcpy(&y, b + i, 4);
        const int64_t ox = x < 0 ? static_cast<int64_t>(INT32_MIN) - x : x, oy = y < 0 ? static_cast<int64_t>(INT32_MIN) - y : y;
        const int64_t d = ox > oy ? ox - oy : oy - ox;
        m = std::max(m, static_cast<uint32_t>(std::min<int64_t>(d, UINT32_MAX)));
    }
    return m;
}

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
uint64_t g_written_frame = ~0ull;  // the frame eye 2's rows were last written (write_eye2)
int g_written_pp_src = 0;  // ([Stereo] MainInstanced's rows diagnostic) that write's gPrevViewProj: 1 the TAA update's, 2 rows 0-3 copied (no jitter)
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
    g_written_frame = state::presents.load();
    g_written_pp_src = pvp ? 1 : 2;
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
    {
        const uint32_t uw = max_ulp(g_e2_wv, r.wv, 16), uv = max_ulp(g_e2_vi, r.vi, 16);
        g_vx.ulp_wv = std::max(g_vx.ulp_wv, uw);
        g_vx.ulp_vi = std::max(g_vx.ulp_vi, uv);
        const bool ew = !std::memcmp(g_e2_wv, r.wv, 64), ev = !std::memcmp(g_e2_vi, r.vi, 64);  // (exact by the bits: -0 is not +0; the ulps the size only)
        if (!ew) ++g_vx.ne_wv;
        if (!ev) ++g_vx.ne_vi;
        if (ew && ev) ++g_vx.eq;
        else g_vx.ring[g_vx.ring_n++ % 16] = f;
    }
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
        const uint32_t up = max_ulp(g_e2_pvp, r.pvp, 16);
        g_vx.ulp_pvp = std::max(g_vx.ulp_pvp, up);
        ++(std::memcmp(g_e2_pvp, r.pvp, 64) ? g_vx.ne_pvp : g_vx.pvp_eq);
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

// ---- [Stereo] PrepassUnionCull (2026-10-14, the render thread; stageVI-prepass.md milestone 4): the draw-time culls in
// the eye's pre-pass walk read the current viewport's side planes: grmModel's per-model culler (0x142cb0 calls
// DAT_142272378, statically 0x142b00: the model's box against +0x2c0..+0x2fc, which SetWorldMtx 0x1348e0 makes from the
// side-plane matrix at +0x3f0..+0x42f) and IsSphereVisible 0x130030 (types 1 and 2: the near plane +0x3d0, the side
// planes +0x390..+0x3cf). ViewportCullPlanes (from Perspective) writes both blocks; SetCurrent does not touch them. So
// in pass 1, from the walk's start (FUN_14065a990's return to 0x5cc2b3) to its end (the letterbox clear's call), those
// two blocks are the union viewport's (camera_lever's, the centre eye with both eyes' tangents plus 3 deg); the near and
// far planes stay the eye's. Wider culling only adds draws the GPU clips.
using PreWalk_t = void (*)(void* obj);
PreWalk_t o_prewalk = nullptr;
std::atomic<bool> g_cull_hooked{false}, g_cull_on{false};
uintptr_t g_cull_vp = 0;  // the viewport whose side planes are the union's now (0: none)
uint64_t g_cull_frame = 0;
alignas(16) unsigned char g_cull_saved[2][0x40];
std::atomic<uint64_t> g_cull_begins{0}, g_cull_ends{0}, g_cull_nounion{0}, g_cull_stale{0};
float g_cull_widen = 0.0f;  // the last swap: the largest change of a side plane's normal (for the status)

void cull_end(bool at_walk_end) {
    if (!g_cull_vp) return;
    uintptr_t cur = 0;
    if (at_walk_end && dual_pass::pass() == 1 && g_cull_frame == state::presents.load() &&
        rd(anchors::addr(anchors::Id::ViewportCurrent), &cur) && cur == g_cull_vp) {
        write_raw(g_cull_vp + 0x390, g_cull_saved[0], 0x40);
        write_raw(g_cull_vp + 0x3f0, g_cull_saved[1], 0x40);
        g_cull_ends.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_cull_stale.fetch_add(1, std::memory_order_relaxed);  // (pass 2 starts from the saved viewport anyway)
    }
    g_cull_vp = 0;
}
void cull_begin() {
    cull_end(false);  // (a walk that never ended: dropped, nothing written back)
    uintptr_t vp = 0;
    const uintptr_t u = reinterpret_cast<uintptr_t>(camera_lever::shadow_union_vp());
    if (!rd(anchors::addr(anchors::Id::ViewportCurrent), &vp) || !vp) return;
    if (!u || u == vp) {
        g_cull_nounion.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!read_raw(vp + 0x390, g_cull_saved[0], 0x40) || !read_raw(vp + 0x3f0, g_cull_saved[1], 0x40)) return;
    float un[16];
    if (!read_raw(u + 0x390, un, sizeof(un))) return;
    float w = 0.0f;
    const float* mine = reinterpret_cast<const float*>(g_cull_saved[0]);
    for (int p = 0; p < 4; ++p) {
        float d = 0.0f;
        for (int k = 0; k < 3; ++k) d += un[p * 4 + k] * mine[p * 4 + k];
        w = std::fmax(w, std::acos(std::fmax(-1.0f, std::fmin(1.0f, d))) * 57.29578f);
    }
    if (!write_raw(vp + 0x390, reinterpret_cast<const void*>(u + 0x390), 0x40) || !write_raw(vp + 0x3f0, reinterpret_cast<const void*>(u + 0x3f0), 0x40)) {
        write_raw(vp + 0x390, g_cull_saved[0], 0x40);
        write_raw(vp + 0x3f0, g_cull_saved[1], 0x40);
        return;
    }
    g_cull_widen = w;
    g_cull_vp = vp;
    g_cull_frame = state::presents.load();
    g_cull_begins.fetch_add(1, std::memory_order_relaxed);
}
// the recorder append_marker records into (its vtable checked): 0 when not found
uintptr_t marker_recorder() {
    uintptr_t dev = 0, hrec = 0, rec = 0, vt = 0;
    if (!rd(anchors::addr(anchors::Id::D3dDeviceSingleton), &dev) || !rd(dev + 0x58, &hrec) || !rd(hrec + 0x128, &rec) || !rd(rec, &vt) ||
        vt != anchors::addr(anchors::Id::RecorderVtbl))
        return 0;
    return rec;
}
// an op-0x4a callback at this point of the render thread's stream; (run 29 item 2) with a payload the append copies into
// the stream (data, size; dlss.cpp's ShapePayload way), handed to fn at playback
bool append_marker(void (*fn)(void*), const void* data = nullptr, uint32_t size = 0) {
    const uintptr_t rec = marker_recorder();
    if (!rec) return false;
    reinterpret_cast<Append_t>(anchors::addr(anchors::Id::DeferredAppend))(reinterpret_cast<void*>(rec), reinterpret_cast<void*>(fn), data, size);
    return true;
}
bool rows_ready() {  // eye 2's rows written this frame into the grown buffers, uploaded at their grown sizes
    uintptr_t layout = 0;
    uint32_t size = 0;
    return g_written_frame == state::presents.load() && still_grown(g_b1) && still_grown(g_pp) && rd(g_b1.cb + 0x08, &layout) &&
           rd(layout + 0x14, &size) && size == g_b1.grown;
}

// ---- [Stereo] PrepassWvpMirror (run 17, research\run17\sps.md (A) option 1; test only, off; needs PrepassViews=2 and
// the walk's start hook): VS_TransformUnlitDepthOnly (vs 2f2dce823554d19a, its pair's twin listed "wvp" in twins.txt)
// reads gWorldViewProj, per draw (viewport +0x100 = (World +0x80 x View +0x180) x Proj +0x1c0, Regenerate 0x1310f0,
// pushed by PushGlobals 0x130e60); its twin reads eye 2's at b1 rows 21-24 (+0x150 of the grown block), which nothing
// else writes. So at each push in pass 1's pre-pass walk (camera_lever's PushGlobals hook, after the game's), eye 2's
// rows are made from the pushed World and eye 2's View and Proj (set_eye2) in Regenerate's own order and written there;
// the game's SetShaderGlobal in the same push has the buffer mapped, so they ride along with the draw's upload.
// The compare (the cut off, so pass 2 still walks): pass 2's own pushes matched to pass 1's mirrored rows by World.
// All on the render thread (g_wvp_tid); every game read and write under SEH; nothing allocated or locked per push.
std::atomic<bool> g_wvp_hooked{false}, g_wvp_on{false};
std::atomic<int> g_wvp_walk{0};       // 1 pass 1's walk open (eye 2's rows ready), 2 pass 2's (the compare), 0 none
std::atomic<DWORD> g_wvp_tid{0};      // the open walk's thread
alignas(16) float g_e2_view[16], g_e2_proj[16];  // eye 2's View (+0x180) and Proj (+0x1c0), set_eye2 (the render thread)
uint64_t g_e2_vp_frame = ~0ull;
alignas(16) float g_wvp_last1[16];    // pass 1: the last eye-1 WVP mirrored this walk (rows 21-24 hold its mirror)
alignas(16) float g_wvp_last2[16];    // ... and that mirror
bool g_wvp_last_ok = false;
bool g_wvp_rec = false;               // pass 1's walk records its pushes for the compare (the cut off at the walk's start)
uint64_t g_wvp_rec_frame = ~0ull;
uint32_t g_wvp_gen = 0;               // the compare table's generation (one per recorded walk: no clearing)
constexpr unsigned kWvpSlots = 4096, kWvpProbe = 32;
struct WvpSlot {
    uint32_t gen;
    float world[16], wvp2[16];
};
WvpSlot g_wvp_tab[kWvpSlots];         // (static: 0.55 MB of zeroes, never allocated)
// single writer (the render thread): load + store, no locked add; read by the status from another thread
using Count = std::atomic<uint64_t>;
inline void bump(Count& c) { c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }
Count g_wvp_walks{0}, g_wvp_unready{0}, g_wvp_mirrored{0}, g_wvp_same{0}, g_wvp_seeded{0}, g_wvp_zeroed{0};
Count g_wvp_notpushed[2], g_wvp_othervp[2], g_wvp_failed[2];  // per walk: [0] pass 1's (the mirror), [1] pass 2's (the compare)
std::atomic<bool> g_wvp_live_pending{false};  // "prepass wvp on": the "wvp" twins handed out from the next ready pass-1 walk
Count g_wvp_cmp_walks{0}, g_wvp_cmp_pushes{0}, g_wvp_matched{0}, g_wvp_unmatched{0}, g_wvp_exact{0}, g_wvp_bad{0}, g_wvp_full{0},
    g_wvp_inconsistent{0};
std::atomic<float> g_wvp_worst{0.0f}, g_wvp_worst_rel{0.0f};

// out = a x b (row vectors, rows of 4), as Regenerate's SSE does it: row i = ((a_i0 b0 + a_i1 b1) + a_i2 b2) + a_i3 b3
inline void mul4(const float* a, const float* b, float* out) {
    const __m128 b0 = _mm_loadu_ps(b), b1 = _mm_loadu_ps(b + 4), b2 = _mm_loadu_ps(b + 8), b3 = _mm_loadu_ps(b + 12);
    for (int i = 0; i < 4; ++i) {
        __m128 r = _mm_mul_ps(_mm_set1_ps(a[i * 4]), b0);
        r = _mm_add_ps(r, _mm_mul_ps(_mm_set1_ps(a[i * 4 + 1]), b1));
        r = _mm_add_ps(r, _mm_mul_ps(_mm_set1_ps(a[i * 4 + 2]), b2));
        r = _mm_add_ps(r, _mm_mul_ps(_mm_set1_ps(a[i * 4 + 3]), b3));
        _mm_storeu_ps(out + i * 4, r);
    }
}
inline uint32_t wvp_hash(const float* world) {
    uint64_t q[8], h = 0;
    std::memcpy(q, world, sizeof(q));
    for (uint64_t v : q) h = (h ^ v) * 0x9E3779B97F4A7C15ull;
    return static_cast<uint32_t>(h ^ (h >> 32));
}
void wvp_record(const float* world, const float* wvp2) {
    uint32_t i = wvp_hash(world);
    for (unsigned p = 0; p < kWvpProbe; ++p, ++i) {
        WvpSlot& s = g_wvp_tab[i & (kWvpSlots - 1)];
        if (s.gen == g_wvp_gen && std::memcmp(s.world, world, 64) != 0) continue;
        s.gen = g_wvp_gen;
        std::memcpy(s.world, world, 64);
        std::memcpy(s.wvp2, wvp2, 64);
        return;
    }
    bump(g_wvp_full);
}
const WvpSlot* wvp_find(const float* world) {
    uint32_t i = wvp_hash(world);
    for (unsigned p = 0; p < kWvpProbe; ++p, ++i) {
        const WvpSlot& s = g_wvp_tab[i & (kWvpSlots - 1)];
        if (s.gen != g_wvp_gen) return nullptr;
        if (!std::memcmp(s.world, world, 64)) return &s;
    }
    return nullptr;
}
void fmax_store(std::atomic<float>& a, float v) {
    if (v > a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed);
}
// A push in an open walk (seed: the walk's start, the scene viewport's current push state)
void wvp_push(void* vp, void* scene, bool seed) {
    const int walk = g_wvp_walk.load(std::memory_order_relaxed);
    if (!walk || GetCurrentThreadId() != g_wvp_tid.load(std::memory_order_relaxed)) return;
    const int k = walk - 1;
    if (!vp || vp != scene) {
        bump(g_wvp_othervp[k]);
        return;
    }
    const uintptr_t v = reinterpret_cast<uintptr_t>(vp);
    alignas(16) float world[16], wvp1[16], stored[16];
    uintptr_t storage = 0;
    if (!rd(g_b1.cb, &storage) || storage != g_b1.block || !read_raw(v + 0x80, world, 64) || !read_raw(v + 0x100, wvp1, 64) ||
        !read_raw(storage + 0x80, stored, 64)) {
        bump(g_wvp_failed[k]);
        return;
    }
    if (std::memcmp(stored, wvp1, 64) != 0) {  // rows 8-11 not this viewport's +0x100: no push (the push gate) or another writer
        bump(g_wvp_notpushed[k]);
        return;
    }
    if (walk == 1) {
        alignas(16) float wvp2[16];
        if (g_wvp_last_ok && !std::memcmp(wvp1, g_wvp_last1, 64)) {
            // +0x100 not regenerated since the last push (a SetCurrent's re-push after SetWorld: World new, WVP stale):
            // rows 21-24 already hold its mirror, stale the same way, as pass 2's push would be
            std::memcpy(wvp2, g_wvp_last2, 64);
            bump(g_wvp_same);
        } else {
            alignas(16) float wv2[16];
            mul4(world, g_e2_view, wv2);  // Regenerate: (World x View) x Proj
            mul4(wv2, g_e2_proj, wvp2);
            if (!write_raw(g_b1.block + 0x150, wvp2, 64)) {
                g_wvp_last_ok = false;
                bump(g_wvp_failed[0]);
                return;
            }
            wr<int>(g_b1.cb + 0x24, -1);  // b1 dirty (the seed has no game push of its own to upload the rows)
            std::memcpy(g_wvp_last1, wvp1, 64);
            std::memcpy(g_wvp_last2, wvp2, 64);
            g_wvp_last_ok = true;
            bump(seed ? g_wvp_seeded : g_wvp_mirrored);
            alignas(16) float view1[16], proj1[16], wv1[16], again[16];
            if (g_wvp_rec && read_raw(v + 0x180, view1, 64) && read_raw(v + 0x1c0, proj1, 64)) {
                // (the compare only) eye 1's +0x100 made from this World? If not, it came from an older one (Regenerate while
                // not current, then SetWorld): the mirror's World is not the one eye 1 draws with
                mul4(world, view1, wv1);
                mul4(wv1, proj1, again);
                if (std::memcmp(again, wvp1, 64) != 0) bump(g_wvp_inconsistent);
            }
        }
        if (g_wvp_rec) wvp_record(world, wvp2);
        return;
    }
    // walk 2: the game's own eye-2 push against pass 1's mirror of the same World
    bump(g_wvp_cmp_pushes);
    const WvpSlot* s = wvp_find(world);
    if (!s) {
        bump(g_wvp_unmatched);
        return;
    }
    bump(g_wvp_matched);
    if (!std::memcmp(s->wvp2, wvp1, 64)) {
        bump(g_wvp_exact);
        return;
    }
    float d = 0.0f, m = 1.0f;
    for (int i = 0; i < 16; ++i) {
        d = std::fmax(d, std::fabs(s->wvp2[i] - wvp1[i]));
        m = std::fmax(m, std::fabs(wvp1[i]));
    }
    fmax_store(g_wvp_worst, d);
    fmax_store(g_wvp_worst_rel, d / m);
    if (d / m > 1e-4f) bump(g_wvp_bad);
}
void wvp_after_push(void* vp, void* scene) { wvp_push(vp, scene, false); }
// hk_prewalk, the walk's start (pass 1 or 2)
void wvp_walk_open(int pass) {
    g_wvp_walk.store(0, std::memory_order_relaxed);
    g_wvp_last_ok = false;
    const uint64_t f = state::presents.load();
    if (pass == 1) {
        // eye 2's rows 21-24 zeroed first: a twin drawn before the walk's first mirrored push, or on a frame not ready, gets
        // a degenerate WVP (nothing rasterised: eye 2 misses those stencil marks) instead of a stale or memset one
        if (g_grow_state > 0 && still_grown(g_b1)) {
            static const float kZero[16] = {};
            if (write_raw(g_b1.block + 0x150, kZero, 64)) {
                wr<int>(g_b1.cb + 0x24, -1);
                bump(g_wvp_zeroed);
            }
        }
        if (g_e2_vp_frame != f || !rows_ready()) {
            bump(g_wvp_unready);
            g_wvp_rec_frame = ~0ull;
            return;
        }
        if (g_wvp_live_pending.exchange(false, std::memory_order_relaxed)) pso::set_wvp_live(true);  // (after a mid-session "on")
        g_wvp_rec = !g_cut_on.load(std::memory_order_relaxed);  // (the compare needs pass 2's walk)
        if (g_wvp_rec) {
            if (++g_wvp_gen == 0) g_wvp_gen = 1;  // (0: the table's empty slots)
            g_wvp_rec_frame = f;
        } else {
            g_wvp_rec_frame = ~0ull;
        }
        g_wvp_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
        g_wvp_walk.store(1, std::memory_order_relaxed);
        bump(g_wvp_walks);
        void* scene = camera_lever::scene_viewport();
        if (scene) wvp_push(scene, scene, true);  // the rows before the walk's first push: the push state it starts from
        return;
    }
    if (pass == 2 && g_wvp_rec_frame == f) {
        g_wvp_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
        g_wvp_walk.store(2, std::memory_order_relaxed);
        bump(g_wvp_cmp_walks);
    }
}

std::atomic<bool> g_inst_markers{false};  // [Stereo] PrepassInstanced at start: the walk-start markers recorded
// ([Stereo] PrepassPrevProjRefresh, run 25 item 4, velocity2.md 3.2; test only, off by default; the render thread) eye 2's
// rage_prev_proj rows 4-7 copied again from rows 0-3 at pass 1's walk start: after SceneRender's push of this frame's rows
// 0-3 (0x1405c3d8c), before the pre-pass twins commit them. Only on the frames whose eye-2 rows were a copy (FXAA)
std::atomic<bool> g_ppr_hooked{false}, g_ppr_on{false};
Count g_ppr_calls{0}, g_ppr_changed{0}, g_ppr_equal{0}, g_ppr_failed{0}, g_ppr_not_copy{0};
void pp_refresh_prewalk() {
    bump(g_ppr_calls);
    if (g_written_frame != state::presents.load() || g_written_pp_src != 2) {  // (not written this frame, or the TAA update's rows)
        bump(g_ppr_not_copy);
        return;
    }
    const int r = eye2_pp_refresh();
    bump(r < 0 ? g_ppr_failed : r == 0 ? g_ppr_equal : g_ppr_changed);
}
void hk_prewalk(void* obj) {
    o_prewalk(obj);
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != anchors::addr(anchors::Id::PrepassPreWalkCall) + 5) return;
    const int pass = dual_pass::pass();
    if (pass == 1 && g_ppr_on.load(std::memory_order_relaxed)) pp_refresh_prewalk();  // ([Stereo] PrepassPrevProjRefresh, test only)
    if (pass == 1 && g_cull_on.load(std::memory_order_relaxed)) cull_begin();
    if (g_wvp_on.load(std::memory_order_relaxed)) wvp_walk_open(pass);  // ([Stereo] PrepassWvpMirror: pass 1 mirrors, pass 2 compares)
    if ((pass == 1 || pass == 2) && inst_prev::installed())  // ([Stereo] PrepassInstPrevMirror; cut: pass 1 the cut on, pass 2 this frame's decided)
        inst_prev::walk_open(pass, pass == 1 && rows_ready(),
                             pass == 1 ? g_cut_on.load(std::memory_order_relaxed) : g_cut_decided_frame == state::presents.load());
    if (g_inst_markers.load(std::memory_order_relaxed) && (pass == 1 || pass == 2)) {
        const bool ok = pass == 1 && g_cull_vp && g_cull_frame == state::presents.load() && rows_ready();
        const bool appended = append_marker(ok ? &walk_start_ok_cb : &walk_start_plain_cb);
        if (!appended) {  // (pass 1: its walk's draws then all not-started singles, so no cut this frame)
            g_start_marker_failed.fetch_add(1, std::memory_order_relaxed);
            log::limited("prepass.startmarker", 10, "[prepass] pass %d's walk-start marker not recorded: no twins (pass 1) or drops (pass 2) "
                         "this walk", pass);
        }
        if (pass == 1) g_cut_ready = ok && appended;  // ([Stereo] PrepassWalkCut: pass 2 may be cut)
    }
}

void hk_letterbox(void* renderer) {
    if (g_wvp_walk.load(std::memory_order_relaxed) && reinterpret_cast<uintptr_t>(_ReturnAddress()) == anchors::addr(anchors::Id::PrepassLetterboxCall) + 5)
        g_wvp_walk.store(0, std::memory_order_relaxed);  // ([Stereo] PrepassWvpMirror: the walk's end)
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == anchors::addr(anchors::Id::PrepassLetterboxCall) + 5) inst_prev::walk_close();  // ([Stereo] PrepassInstPrevMirror)
    if (g_cull_vp && reinterpret_cast<uintptr_t>(_ReturnAddress()) == anchors::addr(anchors::Id::PrepassLetterboxCall) + 5) cull_end(true);
    if (g_views.load(std::memory_order_relaxed) &&
        reinterpret_cast<uintptr_t>(_ReturnAddress()) == anchors::addr(anchors::Id::PrepassLetterboxCall) + 5)
        observe_after_walk();
    // (2026-10-14, the review: recorded whenever the arrays may run, not only while on and not stood down: a region the
    // playback thread opened before "prepass off" or a stand-down still gets its end)
    if (g_hooked.load(std::memory_order_relaxed) && (g_on.load(std::memory_order_relaxed) || g_failed.load(std::memory_order_relaxed) ||
                                                     g_active.load(std::memory_order_relaxed) || g_inst_markers.load(std::memory_order_relaxed)) &&
        reinterpret_cast<uintptr_t>(_ReturnAddress()) == anchors::addr(anchors::Id::PrepassLetterboxCall) + 5) {
        if (append_marker(&walk_end_cb)) g_markers.fetch_add(1, std::memory_order_relaxed);
        else log::limited("prepass.norec", 5, "[prepass] the walk's end: the recorder not found (no marker)");
    }
    // [Stereo] RecorderReemit's late variant (R0, run 19 item 5): a recorded pass-2 walk's re-emit, after the walk-end marker
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == anchors::addr(anchors::Id::PrepassLetterboxCall) + 5 && dual_pass::pass() == 2 && !walk_was_cut())
        rec_state::walk_end();
    // [Stereo] PrepassCutVelocity's d3: the record side re-emitted at a cut walk's end, after the walk-end marker (its packets
    // then play after the handover: the region's first list call ends it, so the re-emitted binds are the game's targets)
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == anchors::addr(anchors::Id::PrepassLetterboxCall) + 5 && dual_pass::pass() == 2 && walk_was_cut())
        cut_velocity::cut_walk_end();
    // (run 25 item 6g, test only: "prepass cutvel p1d4 on"; returns at once unless on, in d4) pass 1's walk end alike
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == anchors::addr(anchors::Id::PrepassLetterboxCall) + 5 && dual_pass::pass() == 1)
        cut_velocity::pass1_walk_end();
    o_letterbox(renderer);
}

}  // namespace

void init() {
    g_on = false;  // on only once install() has its hook
}
bool grow_cb(uintptr_t cb, uint32_t declared, uint32_t grown, uint32_t alloc, uintptr_t* block) {  // ([Stereo] PrepassPrevWorldMirror)
    Grown g;
    if (!block || grown > alloc || !grow(g, cb, declared, grown, alloc)) return false;
    *block = g.block;
    return true;
}
bool cb_still_grown(uintptr_t cb, uintptr_t block, uint32_t grown) { return still_grown(Grown{cb, block, 0, grown}); }
// (SF-1) the calling thread's recorder context, as the game reads it (PassBind 0x14015157b: TLS slot 0's block +0x28, its
// +0x38 byte the block's lazy init), is the one append_marker records into (*(dev +0x58)): a marker then lands in the
// same stream as this thread's draws. Read only; an uninitialised TLS block is "no" (never initialised here).
bool thread_stream_ok() {
    uintptr_t dev = 0, hctx = 0, blk = 0, ctx = 0;
    unsigned char init = 0;
    const uintptr_t tls = static_cast<uintptr_t>(__readgsqword(0x58));
    if (!rd(anchors::addr(anchors::Id::D3dDeviceSingleton), &dev) || !rd(dev + 0x58, &hctx) || !hctx) return false;
    if (!rd(tls, &blk) || !blk || !rd(blk + 0x38, &init) || !init || !rd(blk + 0x28, &ctx)) return false;
    return ctx == hctx;
}
bool stream_marker(void (*fn)(void*)) { return thread_stream_ok() && append_marker(fn); }
bool stream_ready() { return thread_stream_ok() && marker_recorder() != 0; }
uintptr_t stream_recorder() { return thread_stream_ok() ? marker_recorder() : 0; }  // (run 29 item 2: both glow markers decided together)
bool stream_marker_data(void (*fn)(void*), const void* data, uint32_t size) {
    return data && size && size <= kMarkerPayloadMax && thread_stream_ok() && append_marker(fn, data, size);
}

// ---- [Stereo] MainInstanced (main_instanced.cpp, run 25 item 3; test only): the render thread
bool eye2_rows_ready() { return g_views_write.load(std::memory_order_relaxed) && g_grow_state > 0 && rows_ready(); }
bool main_rows(int pass, float* pp, float* vi, float* wv, int* src) {  // (MainInstanced's rows diagnostic: read only)
    if ((pass != 1 && pass != 2) || g_grow_state <= 0 || !still_grown(g_b1) || !still_grown(g_pp)) return false;
    *src = g_written_frame == state::presents.load() ? g_written_pp_src : 0;
    const bool e2 = pass == 1;  // (pass 1: eye 2's rows past the declared ends, the twin's; pass 2: the game's own)
    return read_raw(g_pp.block + (e2 ? 0x40 : 0x00), pp, 64) && read_raw(g_b1.block + (e2 ? 0x190 : 0xc0), vi, 64) &&
           read_raw(g_b1.block + (e2 ? 0x110 : 0x40), wv, 64);
}
int eye2_pp_refresh() {  // (the experiment: rows 4-7 := rows 0-3 as the storage holds them now)
    if (!eye2_rows_ready()) return -1;
    float pp[16], old[16];
    if (!read_raw(g_pp.block, pp, 64) || !read_raw(g_pp.block + 0x40, old, 64)) return -1;
    if (std::memcmp(pp, old, 64) == 0) return 0;  // (the review: counted apart, so the diagnostic shows whether it changed anything)
    if (!write_raw(g_pp.block + 0x40, pp, 64)) return -1;
    std::memcpy(g_written_pp, pp, 64);  // (the micro-review: check_written then counts the refresh as written, not as lost)
    wr<int>(g_pp.cb + 0x24, -1);  // (re-uploaded at its next commit)
    return 1;
}
std::string pp_refresh_command(const std::string& w) {  // ([Stereo] PrepassPrevProjRefresh, test only)
    if (!g_ppr_hooked.load()) return "pprefresh: not installed (start with [Stereo] PrepassPrevProjRefresh=1, PrepassViews=2 and the walk's start hook)";
    if (w == "on" || w == "off") {
        g_ppr_on = w == "on";
        log::info("[prepass] pprefresh %s (the session)", w.c_str());
    } else if (w == "reset") {
        for (Count* c : {&g_ppr_calls, &g_ppr_changed, &g_ppr_equal, &g_ppr_failed, &g_ppr_not_copy}) c->store(0);
    } else if (!w.empty() && w != "kv") {
        return "ERROR prepass pprefresh [on|off|reset|kv]";
    }
    const auto u = [](const Count& c) { return static_cast<unsigned long long>(c.load(std::memory_order_relaxed)); };
    char b[400];
    std::snprintf(b, sizeof(b),
                  "pprefresh %s: pass 1 walk starts %llu, rows 4-7 refreshed from rows 0-3: changed %llu, already equal %llu, failed %llu; not a "
                  "copy frame (the TAA update's rows, or not written this frame) %llu | kv: ppr_on=%d ppr_calls=%llu ppr_changed=%llu ppr_equal=%llu "
                  "ppr_failed=%llu ppr_not_copy=%llu",
                  g_ppr_on.load() ? "on" : "off", u(g_ppr_calls), u(g_ppr_changed), u(g_ppr_equal), u(g_ppr_failed), u(g_ppr_not_copy),
                  g_ppr_on.load() ? 1 : 0, u(g_ppr_calls), u(g_ppr_changed), u(g_ppr_equal), u(g_ppr_failed), u(g_ppr_not_copy));
    return b;
}
namespace {
float g_ctl_saved[32];  // eye 2's gWorldView (b1 17-20) and gViewInverse (25-28) before the control wrote them
bool g_ctl_held = false;
}  // namespace
bool eye2_rows_control(int mode) {
    if (mode == 0) {
        if (!g_ctl_held) return true;
        g_ctl_held = false;
        if (g_grow_state <= 0 || !still_grown(g_b1)) return false;  // (the game made b1 again: nothing of ours to put back)
        if (!write_raw(g_b1.block + 0x110, g_ctl_saved, 64) || !write_raw(g_b1.block + 0x190, g_ctl_saved + 16, 64)) return false;
        wr<int>(g_b1.cb + 0x24, -1);
        return true;
    }
    if (g_ctl_held || (mode != 1 && mode != 2) || !eye2_rows_ready()) return false;
    if (!read_raw(g_b1.block + 0x110, g_ctl_saved, 64) || !read_raw(g_b1.block + 0x190, g_ctl_saved + 16, 64)) return false;
    float wv[16], vi[16];
    if (mode == 1) {
        std::memcpy(wv, g_ctl_saved, 64);
        std::memcpy(vi, g_ctl_saved + 16, 64);
        wv[12] += 1e-3f;  // (row 3: the translation)
        wv[13] += 1e-3f;
    } else if (!read_raw(g_b1.block + 0x40, wv, 64) || !read_raw(g_b1.block + 0xc0, vi, 64)) {
        return false;
    }
    g_ctl_held = true;  // (from here a partial write is put back by mode 0)
    if (!write_raw(g_b1.block + 0x110, wv, 64) || !write_raw(g_b1.block + 0x190, vi, 64)) {
        eye2_rows_control(0);
        return false;
    }
    wr<int>(g_b1.cb + 0x24, -1);  // (re-uploaded at its next commit)
    return true;
}

// ---- [Stereo] PrepassWalkCut: the render thread decides between the passes; the playback thread hands over
// (skip: [Stereo] PrepassRecordSkip, rec_state.cpp: the cut frame's walk runs with the recorder's writers skipped instead
// of the stub's cut at entry; its marker and the handover the same)
static bool decide_cut(bool ready, bool skip) {
    g_cutd_calls.fetch_add(1, std::memory_order_relaxed);
    if (!g_cut_on.load(std::memory_order_relaxed)) {
        g_cutd_off.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!g_on.load(std::memory_order_relaxed) || g_failed.load() || !g_inst_on.load(std::memory_order_relaxed)) {
        g_cutd_state.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!ready) {
        g_cutd_unready.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (pso::twins_made() == 0) {
        g_cutd_notwins.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const uint64_t f = state::presents.load();
    const uint64_t single = g_single_draws.load(std::memory_order_relaxed);
    const uint64_t started = g_single_started.load(std::memory_order_relaxed), unstarted = g_single_unstarted.load(std::memory_order_relaxed);
    const uint64_t bad = g_cut_bad.load(std::memory_order_relaxed) + g_trk_mismatch.load(std::memory_order_relaxed) +
                         g_cut_noregion_held.load(std::memory_order_relaxed);
    // (the playback thread's counts: a frame or two behind). Off: any single or problem extends the hold, as before; on
    // ([Stereo] PrepassCutHoldStarted): started singles or problems only
    const bool ch_bad = bad != g_cut_seen_bad, ch_started = started != g_cut_seen_started;
    // ([Stereo] PrepassCutHoldPending, test only) on: the singles' part by their pipelines (the table, started ones only with
    // PrepassCutHoldStarted): a permanent one extends the kCutHold hold, a pending one the pending hold (hp_hold below). The
    // snapshot is kept whenever armed (so "holdpending on" mid-session classifies only what is new)
    const bool hp = g_hp_armed && g_hp_on.load(std::memory_order_relaxed);
    const int hp_new = g_hp_armed ? hp_scan(f, g_cut_hold_started.load(std::memory_order_relaxed), hp) : 0;
    const bool extend = hp ? (hp_new & 1) != 0 || ch_bad
                           : g_cut_hold_started.load(std::memory_order_relaxed) ? ch_started || ch_bad : single != g_cut_seen_single || ch_bad;
    if (extend) {
        if (f >= g_cut_hold_until) {
            g_cut_holds.fetch_add(1, std::memory_order_relaxed);
            if (hp && (hp_new & 1)) g_hp_perm_holds.fetch_add(1, std::memory_order_relaxed);
            log::limited("prepass.cuthold", 20, "[prepass] walk cut held for %llu frames: %llu single draws in pass 1 (%llu started, %llu not), "
                         "%llu handover problems so far", static_cast<unsigned long long>(kCutHold), static_cast<unsigned long long>(single),
                         static_cast<unsigned long long>(started), static_cast<unsigned long long>(unstarted), static_cast<unsigned long long>(bad));
        }
        g_cut_hold_until = f + kCutHold;
        g_cut_hold_cause = ch_bad ? 3 : hp ? ((hp_new & 4) ? 1 : 2) : ch_started ? 1 : 2;
    }
    g_cut_seen_single = single;
    g_cut_seen_bad = bad;
    g_cut_seen_started = started;
    g_cut_seen_unstarted = unstarted;
    const bool hp_held = g_hp_armed && hp_hold(f, hp);  // (first: a pending twin refused extends the kCutHold hold)
    if (f < g_cut_hold_until && !g_cut_force.load(std::memory_order_relaxed)) {
        (g_cut_hold_cause == 1 ? g_cutd_held_started : g_cut_hold_cause == 3 ? g_cutd_held_bad : g_cutd_held_unstarted)
            .fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (hp_held && !g_cut_force.load(std::memory_order_relaxed)) {  // ([Stereo] PrepassCutHoldPending: a twin still pending, or just installed)
        g_hp_held.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // ([Stereo] PrepassInstPrevMirror, SF-3) an instanced draw of this frame's pass 1 not mirrored (drawn single, its twin
    // not drawn): pass 2 must draw it, so no unforced cut this frame
    if (!g_cut_force.load(std::memory_order_relaxed) && inst_prev::frame_refused()) {
        g_cutd_instprev.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!append_marker(&cut_frame_cb)) {
        g_cutd_nomarker.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_cut_frames.fetch_add(1, std::memory_order_relaxed);
    if (skip) return true;  // (no stub cut: rec_state skips the walk's writers)
    g_cuts_at_decide = rdrvr_prepass_cuts;
    rdrvr_prepass_cut_tid = GetCurrentThreadId();
    rdrvr_prepass_cut = 1;
    return true;
}
bool walk_cut_decide() {
    const bool ready = g_cut_ready;
    g_cut_ready = false;
    ++g_cut_seq;  // (item 7's S2: it names the pass 2 that follows)
    const bool skip = rec_state::skip_wanted();
    const bool cut = decide_cut(ready, skip);
    g_cut_decided_frame = cut ? state::presents.load() : ~0ull;  // ([Stereo] PrepassInstPrevMirror: pass 2's walk not opened when cut)
    rec_state::arm(cut && skip);  // ([Stereo] RecorderReemit: the wrapper for pass 2's walk, armed each frame; off, nothing)
    return cut && !skip;  // (a record-skipped walk runs: its grass shifts the gust, which dual_pass puts back as without a cut)
}
ID3D12Resource* velocity_array() { return g_arr[kVelocity].arr; }
ID3D12Resource* kind_game(int k) { return k >= 0 && k < kKinds ? g_arr[k].game : nullptr; }
ID3D12Resource* kind_array(int k) { return k >= 0 && k < kKinds ? g_arr[k].arr : nullptr; }
D3D12_RESOURCE_STATES kind_rest_state(int k) { return k >= 0 && k < kKinds ? write_state(k) : D3D12_RESOURCE_STATE_COMMON; }
unsigned kind_planes(int k) { return k >= 0 && k < kKinds ? g_arr[k].planes : 0u; }
bool kind_followed(int k, unsigned p, D3D12_RESOURCE_STATES* st) {  // ([Stereo] PrepassEye2Collector, the follow's thread)
    if (k < 0 || k >= kKinds || p > 1 || !st || g_active.load() || GetCurrentThreadId() != g_trk_tid || !(g_frame_touched & (1u << k)) ||
        !(g_trk_known & (1u << (k * 2 + p))))
        return false;
    *st = g_trk[k][p];
    if (t_poll_cl && g_trk_cl[k][p] != t_poll_cl) g_follow_stale_reads.fetch_add(1, std::memory_order_relaxed);  // (r117 DG4: counted only)
    return true;
}
bool kind_followed_fresh(int k, unsigned p, ID3D12GraphicsCommandList* cl, D3D12_RESOURCE_STATES* st) {  // (r117 DG4)
    D3D12_RESOURCE_STATES s{};
    if (!cl || !kind_followed(k, p, &s) || g_trk_cl[k][p] != cl) return false;
    *st = s;
    return true;
}
void follow_list_ends(ID3D12GraphicsCommandList* cl) {  // (a list's Reset or Close, on the follow's thread: its marks dropped)
    if (!cl || GetCurrentThreadId() != g_trk_tid) return;
    for (auto& k : g_trk_cl)
        for (auto& c : k)
            if (c == cl) c = nullptr;
}
const void* follow_setter(int k, unsigned p) { return k >= 0 && k < kKinds && p < 2 ? g_trk_cl[k][p] : nullptr; }
uint64_t follow_stale_reads() { return g_follow_stale_reads.load(std::memory_order_relaxed); }
void follow_stale_note(int user) {
    if (user >= 0 && user < kFollowUsers) g_follow_stale_user[user].fetch_add(1, std::memory_order_relaxed);
}
uint64_t follow_stale(int user) { return user >= 0 && user < kFollowUsers ? g_follow_stale_user[user].load(std::memory_order_relaxed) : 0; }
bool walk_cut_decided(uint64_t* seq) {  // (item 7's S2, [Stereo] Pass2FrontSkip; the render thread)
    if (seq) *seq = g_cut_seq;
    return g_cut_decided_frame != ~0ull;  // (set or cleared by every decision: the last one's, the pass 2 that seq names)
}
bool pass1_walk_ready() { return g_cut_ready; }  // ([Stereo] PrepassEye2Collector, the render thread, pass 1 after hk_prewalk)
bool instanced_on() { return g_inst_on.load(std::memory_order_relaxed); }  // ([Stereo] PrepassEye2Collector)
bool walk_was_cut() {
    return rdrvr_prepass_cut && rdrvr_prepass_cut_tid == GetCurrentThreadId() && rdrvr_prepass_cuts != g_cuts_at_decide;
}
void walk_cut_end() {
    inst_prev::walk_close();  // ([Stereo] PrepassInstPrevMirror, SF-5: no walk left open past pass 2)
    if (rdrvr_prepass_cut && rdrvr_prepass_cuts == g_cuts_at_decide) {  // decided, but the stub cut nothing: its gust shifted twice
        g_cut_bad.fetch_add(1, std::memory_order_relaxed);
        log::limited("prepass.nocut", 10, "[prepass] walk cut decided but pass 2's pre-pass was not cut (its gust kept anyway)");
    }
    if (rdrvr_prepass_cut && rdrvr_prepass_cuts > g_cuts_at_decide + 1) {  // (more than one +0x7f = 1 call cut in this pass 2)
        g_cut_multi.fetch_add(1, std::memory_order_relaxed);
        g_cut_extra.fetch_add(rdrvr_prepass_cuts - g_cuts_at_decide - 1, std::memory_order_relaxed);
    }
    rdrvr_prepass_cut = 0;
    if (!rec_state::disarm()) {  // [Stereo] PrepassRecordSkip decided, but no walk skipped (refused at its entry): recorded whole
        g_cut_bad.fetch_add(1, std::memory_order_relaxed);
        log::limited("prepass.noskip", 10, "[prepass] record skip decided but pass 2's pre-pass walk was recorded (rec_state refused it at entry)");
    }
}
void cut_track(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_RESOURCE_BARRIER* b) {
    if (cut_pending()) cut_poll(cl);  // (the handover before this batch: the states it follows are before it)
    track(cl, n, b);
}
void cut_poll(ID3D12GraphicsCommandList* cl) {
    struct PollCl {  // (r117 DG4: the older users' kind_followed answers inside this poll checked against cl, counted only)
        ID3D12GraphicsCommandList* was;
        explicit PollCl(ID3D12GraphicsCommandList* c) : was(t_poll_cl) { t_poll_cl = c; }
        ~PollCl() { t_poll_cl = was; }
    } poll_cl(cl);
    if (g_eye2_pending_flag.load(std::memory_order_relaxed)) eye2_collector::poll(cl);  // ([Stereo] PrepassEye2Collector, test only)
    if (g_eye2_light_pending_flag.load(std::memory_order_relaxed)) eye2_light::poll(cl);  // ([Stereo] PrepassEye2Light, test only)
    if (g_main_pending_flag.load(std::memory_order_relaxed)) main_inst::poll(cl);  // ([Stereo] MainInstanced, test only)
    if (g_eye2_vel_pending_flag.load(std::memory_order_relaxed)) eye2_velocity::poll(cl);  // ([Stereo] PrepassEye2Velocity, test only)
    if (GetCurrentThreadId() != g_cut_pending_tid || !cl || cl->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    if (!g_cut_pending_flag.exchange(false, std::memory_order_acq_rel)) return;
    // pass 2's walk cut and no region: pass 1's slice 1 copied into the game's targets here, as pass 2's region's end would
    const bool seeded = g_seeded;
    g_seeded = false;
    if (g_active.load() || !g_frame_inst || !g_frame_touched || !seeded || g_failed.load()) {
        g_cut_bad.fetch_add(1, std::memory_order_relaxed);
        log::limited("prepass.cutbad", 10, "[prepass] walk cut: no handover (%s): eye 2 keeps eye 1's pre-pass images this frame",
                     g_active.load() ? "a region open" : !g_frame_inst ? "pass 1 not instanced" : !g_frame_touched ? "pass 1 touched nothing"
                     : !seeded ? "pass 1's region did not end this frame" : "stood down");
        return;
    }
    // (the cut-hold review) no pass 2 region: its draws before the walk (0x5cbfd0's, outside 0x706050) went into the game's
    // targets, and this copy overwrites them with slice 1, which has only pass 1's twins of them. With the hold on any
    // single that is no loss; with PrepassCutHoldStarted pass 1 may have had not-started ones, so each such frame is a
    // problem that holds the cut (conservative: counted whether or not that frame had any). The decision is made before
    // pass 2 records, so the cut cannot be skipped for it
    g_cut_noregion.fetch_add(1, std::memory_order_relaxed);
    if (g_cut_hold_started.load(std::memory_order_relaxed)) {
        g_cut_noregion_held.fetch_add(1, std::memory_order_relaxed);
        log::limited("prepass.cutnoregion", 10, "[prepass] walk cut handed over with no pass 2 region (PrepassCutHoldStarted): pass 2's "
                     "draws before the walk overwritten; the cut held");
    }
    g_r = Region{};
    g_r.cl = cl;
    g_r.slice = 1;
    g_r.drop = true;
    unsigned missing = 0;
    if (!add_followed(&missing)) {
        g_cut_bad.fetch_add(1, std::memory_order_relaxed);
        log::limited("prepass.cutbad", 10, "[prepass] walk cut: the followed states of kinds %#x not known: those keep eye 1's this frame", missing);
        if (!g_r.touched) return;
    }
    g_pred_off = true;  // (as pass 2's region: its queries were never issued)
    end_region(cl);
    g_cut_handovers.fetch_add(1, std::memory_order_relaxed);
}
void set_cut_hold_started(bool on) {  // "prepass cuthold started|all" ([Stereo] PrepassCutHoldStarted at run time)
    g_cut_hold_started = on;
    log::info("[prepass] walk cut hold: %s (the session)", on ? "started singles and problems only" : "any single or problem");
}
bool set_cut_hold_pending(bool on) {  // "prepass cut holdpending on|off" ([Stereo] PrepassCutHoldPending at run time; armed only)
    if (!g_hp_armed) {
        log::warn("[prepass] cut holdpending: not armed (start with [Stereo] PrepassCutHoldPending=1 and PrepassWalkCut's switches)");
        return false;
    }
    g_hp_on = on;  // (the render thread's next decision: off converts a pending hold left into the kCutHold one)
    log::info("[prepass] walk cut hold pending %s (the session)", on ? "on: a pending twin's singles hold only until its install" : "off: as before");
    return true;
}
bool cut_hold_pending_wanted() {  // (prepass_arrays.h: the ini side of install's arming below, for pso's region table)
    return config::get_bool("Stereo", "PrepassCutHoldPending", false) && config::get_bool("Stereo", "PrepassWalkCut", false) &&
           config::get_bool("Stereo", "PrepassInstanced", false) && config::get_bool("Stereo", "PrepassArrays", false) &&
           config::get_int("Stereo", "PrepassViews", 0) >= 2 && config::get_bool("Compat", "DrawMaskInit", true);
}
void set_walk_cut(const std::string& how) {  // on, off, or force (on, the hold ignored)
    if (how != "on" && how != "off" && how != "force") return;
    const bool want = how != "off";
    if (want && !(g_inst_hooked.load() && dual_pass::masks_hooked())) {
        log::warn("[prepass] walk cut not on: start with [Stereo] PrepassInstanced's switches and [Compat] DrawMaskInit=1");
        return;
    }
    g_cut_force = how == "force";
    if (want != g_cut_on.load()) log::info("[prepass] walk cut %s (the session)", how.c_str());
    if (want) g_cut_armed_flag = true;  // (followed from now on, so a cut in flight when it goes off still hands over right)
    g_cut_on = want;
}
void install() {
    const bool arrays = config::get_bool("Stereo", "PrepassArrays", false), views = config::get_int("Stereo", "PrepassViews", 0) >= 1;
    const bool ucull = config::get_bool("Stereo", "PrepassUnionCull", false);
    const bool inst = config::get_bool("Stereo", "PrepassInstanced", false) && arrays && config::get_int("Stereo", "PrepassViews", 0) >= 2;
    if (!arrays && !views && !ucull && (config::get_bool("Stereo", "RecorderReemit", false) || config::get_bool("Stereo", "PrepassRecordSkip", false)))
        log::warn("[prepass] RecorderReemit/PrepassRecordSkip ignored: PrepassArrays, PrepassViews and PrepassUnionCull are all off");
    if ((!arrays && !views && !ucull) || anchors::stand_down()) return;
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
    if ((ucull || inst) && anchors::present(Id::PrepassPreWalk) && anchors::present(Id::PrepassPreWalkCall) &&
        hooks::install("RDR pre-pass walk's start (FUN_14065a990, PrepassUnionCull/Instanced)", reinterpret_cast<void*>(anchors::addr(Id::PrepassPreWalk)),
                       reinterpret_cast<void*>(&hk_prewalk), reinterpret_cast<void**>(&o_prewalk))) {
        g_cull_hooked = true;
        g_cull_on = ucull;
        if (ucull) log::info("[prepass] [Stereo] PrepassUnionCull on (test only: pass 1's pre-pass walk culls against both eyes' union)");
    } else if (ucull || inst) {
        log::warn("[prepass] the walk's start hook not installed (no PrepassPreWalk anchors on this build, or the hook failed): no union cull, no instancing");
    }
    if (config::get_bool("Stereo", "PrepassInstanced", false)) {
        if (inst && g_cull_hooked.load()) {
            g_inst_hooked = true;
            g_inst_on = true;
            g_inst_markers = true;
            log::info("[prepass] [Stereo] PrepassInstanced on (test only: pass 1's pre-pass drawn for both eyes by the twins, pass 2 dropping them)");
            if (config::get_bool("Stereo", "PrepassCutEmptyDraws", false)) {
                g_empty_on = true;
                log::info("[prepass] [Stereo] PrepassCutEmptyDraws on (test only: a region's draws with no instances or no vertices neither "
                          "twinned nor single, so they hold no cut)");
            }
        } else {
            log::warn("[prepass] PrepassInstanced needs PrepassArrays=1, PrepassViews=2 and the walk's start hook: off");
        }
    }
    if (config::get_bool("Stereo", "PrepassWalkCut", false)) {
        if (g_inst_hooked.load() && dual_pass::masks_hooked()) {
            g_cut_on = true;
            g_cut_armed_flag = true;
            log::info("[prepass] [Stereo] PrepassWalkCut on (test only: pass 2's pre-pass walk cut after an instanced pass 1, eye 2's "
                      "slices handed over at its walk end)");
            if (config::get_bool("Stereo", "PrepassCutHoldStarted", false)) {
                g_cut_hold_started = true;
                log::info("[prepass] [Stereo] PrepassCutHoldStarted on (test only: the cut held by started singles and problems only)");
            }
            if (config::get_bool("Stereo", "PrepassCutHoldPending", false)) {  // (item8d.md 11; pso writes the region table for it)
                g_hp_armed = true;
                g_hp_on = true;
                log::info("[prepass] [Stereo] PrepassCutHoldPending on (test only: a single whose twin is pending holds the cut until its "
                          "install + %llu presents; other singles %llu as before; needs InstancedShadersAuto, else every single holds as before)",
                          static_cast<unsigned long long>(kHoldPendMargin), static_cast<unsigned long long>(kCutHold));
            }
        } else {
            log::warn("[prepass] PrepassWalkCut needs PrepassInstanced (and its switches) and [Compat] DrawMaskInit=1: off%s",
                      cut_hold_pending_wanted() ? " (PrepassCutHoldPending not armed: pso's region table, if written for it, read by nothing)" : "");
        }
    }
    rec_state::install(g_cut_on.load());  // [Stereo] RecorderReemit / PrepassRecordSkip (run 18 P2, test only; both off: nothing)
    if (arrays) log::info("[prepass] [Stereo] PrepassArrays on (test only: each eye's pre-pass drawn into 2-slice arrays, copied back at the walk's end)");
    // (run 21 item 4, research/run19/gpu.md section 7; test only, off by default) end_region's two GPU reductions
    if (config::get_bool("Stereo", "PrepassSliceBarriers", false)) {
        if (arrays) {
            g_sb_armed = true;  // (the review's must-fix: armed, off until "prepass slicebarriers on")
            log::info("[prepass] [Stereo] PrepassSliceBarriers on (test only, armed: off until \"prepass slicebarriers on\"; the arrays' copy transitions name the copied slice only)");
        } else {
            log::warn("[prepass] PrepassSliceBarriers needs PrepassArrays=1: off");
        }
    }
    if (config::get_bool("Stereo", "PrepassSkipVelocityCopy", false)) {
        if (!arrays) {
            log::warn("[prepass] PrepassSkipVelocityCopy needs PrepassArrays=1: off");
        } else if (!anchors::present(Id::PostFxSingleton) || !anchors::present(Id::AaSetter)) {
            log::warn("[prepass] PrepassSkipVelocityCopy: no PostFxSingleton/AaSetter anchor on this build: off");
        } else if (config::get_int("Render", "ForceAntiAliasing", -1) < 0) {
            log::warn("[prepass] PrepassSkipVelocityCopy needs [Render] ForceAntiAliasing (the mod's AA setter forces frame generation off; "
                      "frame generation reads the Velocity RT): off");
        } else {
            g_sv_armed = true;  // (the review's must-fix: armed, off until "prepass skipvel on")
            log::info("[prepass] [Stereo] PrepassSkipVelocityCopy on (test only, armed: off until \"prepass skipvel on\"; the Velocity RT's slice copies skipped while the AA is Off or "
                      "FXAA and motion blur is off, %llu presents in a row)", static_cast<unsigned long long>(kSkipVelSettle));
        }
    }
    if (views && anchors::present(Id::HandleWorldView) && anchors::present(Id::HandleViewInverse) && anchors::present(Id::HandlePrevViewProj) &&
        anchors::present(Id::HandleViewport) && anchors::present(Id::HandlePrevWorldVP)) {
        g_views = true;
        g_views_write = config::get_int("Stereo", "PrepassViews", 0) >= 2;
        log::info("[prepass] [Stereo] PrepassViews on (test only: eye 2's rows predicted in pass 1, compared with the game's pass 2%s)",
                  g_views_write.load() ? "; and written past rage_matrices' and rage_prev_proj's declared ends" : "");
    }
    if (config::get_int("Stereo", "PrepassPrevProjRefresh", 0) == 1) {  // (run 25 item 4, test only)
        if (g_views_write.load() && g_cull_hooked.load()) {
            g_ppr_hooked = true;
            g_ppr_on = true;
            log::info("[prepass] [Stereo] PrepassPrevProjRefresh on (test only: eye 2's rage_prev_proj rows 4-7 copied again from rows 0-3 at "
                      "pass 1's walk start, on the frames they were a copy: FXAA)");
        } else {
            log::warn("[prepass] PrepassPrevProjRefresh needs PrepassViews=2 and the walk's start hook (PrepassUnionCull or PrepassInstanced): off");
        }
    }
    if (config::get_bool("Stereo", "PrepassWvpMirror", false)) {
        if (g_views_write.load() && g_cull_hooked.load() && camera_lever::push_hook_installed()) {
            g_wvp_hooked = true;
            g_wvp_on = true;
            camera_lever::set_after_push(&wvp_after_push);  // (camera_lever's PushGlobals hook, installed before this)
            pso::set_wvp_live(true);
            log::info("[prepass] [Stereo] PrepassWvpMirror on (test only: eye 2's gWorldViewProj, b1 rows 21-24, mirrored at each push in "
                      "pass 1's pre-pass walk; twins.txt's \"wvp\" pairs drawn)");
        } else {
            log::warn("[prepass] PrepassWvpMirror needs PrepassViews=2, the walk's start hook (PrepassUnionCull or PrepassInstanced) and "
                      "camera_lever's PushGlobals hook installed: off");
        }
    }
    // [Stereo] PrepassPrevWorldMirror (run 19 item 6, test only; off: nothing), before PrepassCutVelocity: when both are on,
    // the one detour on 0x1322b0 is prevworld's and cut_velocity's d1 sampling runs inside it
    prevworld::install(g_views_write.load());
    cut_velocity::install(g_inst_hooked.load() && dual_pass::masks_hooked());  // [Stereo] PrepassCutVelocity (run 19, test only; off: nothing)
    inst_prev::install(g_views_write.load() && g_cull_hooked.load());  // [Stereo] PrepassInstPrevMirror (run 19, test only; off: nothing)
}
// (run 21 item 4, test only) "prepass slicebarriers on|off", "prepass skipvel on|off": the session (the ini switch at start)
std::string set_slice_barriers(bool on) {
    if (on && !g_sb_armed.load()) return "ERROR slice barriers not armed: start with [Stereo] PrepassSliceBarriers=1 (and PrepassArrays=1)";
    g_sb_on = on;
    log::info("[prepass] slice barriers %s (the session, from the next region end)", on ? "on" : "off");
    return reductions_status();
}
std::string set_skip_velocity(bool on) {
    if (on && !g_sv_armed.load()) return "ERROR skip velocity not armed: start with [Stereo] PrepassSkipVelocityCopy=1 (and PrepassArrays=1, ForceAntiAliasing)";
    if (on && !render_settings::aa_hook_installed()) return "ERROR skip velocity: the AA setter hook is not installed (frame generation not forced off)";
    g_sv_on = on;
    g_sv_since.store(~0ull, std::memory_order_relaxed);  // (the settle counted again)
    log::info("[prepass] skip velocity copy %s (the session, from the next region end)", on ? "on" : "off");
    return reductions_status();
}
std::string reductions_status() {
    const auto u = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load(std::memory_order_relaxed)); };
    char b[640];
    std::snprintf(b, sizeof(b),
                  "reductions: slice barriers %s%s: region ends with per-slice barriers %llu | skip velocity %s%s: velocity copies skipped "
                  "%llu, kept for technique %llu, motion blur %llu, settling %llu, PostFx not read %llu; last technique %d, AA hook missing %llu | copies %llu",
                  g_sb_on.load() ? "on" : "off", g_sb_armed.load() ? "" : " (not armed)", u(g_sb_ends), g_sv_on.load() ? "on" : "off",
                  g_sv_armed.load() ? "" : " (not armed)", u(g_sv_skipped), u(g_sv_tech), u(g_sv_blur), u(g_sv_settle), u(g_sv_unread),
                  g_sv_last_tech.load(std::memory_order_relaxed), u(g_sv_nohook), u(g_copies));
    return b;
}
void set_wvp(bool on) {
    if (on && !g_wvp_hooked.load()) {
        log::warn("[prepass] wvp mirror not on: start with [Stereo] PrepassWvpMirror=1 (and PrepassViews=2, the walk's start hook)");
        return;
    }
    g_wvp_on = on;
    if (!on) {
        g_wvp_walk = 0;
        g_wvp_live_pending = false;
        pso::set_wvp_live(false);  // (the "wvp" pairs' twins not handed out: those draws single, in each pass)
    } else {
        g_wvp_live_pending = true;  // (handed out from the next ready pass-1 walk, its rows zeroed or mirrored by then)
    }
    log::info("[prepass] wvp mirror %s (from the next pass 1's walk)", on ? "on" : "off");
}
std::string wvp_status() {
    const auto u = [](const Count& c) { return static_cast<unsigned long long>(c.load(std::memory_order_relaxed)); };
    char b[1100];
    std::snprintf(b, sizeof(b),
                  "wvp mirror %s%s%s: pass 1 walks %llu (rows 21-24 zeroed at the start %llu; eye 2's rows not ready %llu); pushes mirrored %llu, "
                  "WVP unchanged (kept) %llu, seeded at the walk's start %llu; skipped in pass 1's walk: not pushed %llu, other viewports %llu, "
                  "failed %llu | compare (the cut off): pass 2 walks %llu, pushes %llu, matched by World %llu (bit-exact %llu), unmatched %llu, "
                  "worst diff %.6g (relative %.3g), over 1e-4 relative %llu; skipped in pass 2's walk: not pushed %llu, other viewports %llu, "
                  "failed %llu; eye 1's WVP not from its pushed World %llu, table full %llu",
                  g_wvp_on.load() ? "on" : "off", g_wvp_hooked.load() ? "" : " (not hooked)",
                  g_wvp_live_pending.load() ? " (twins from the next ready walk)" : "", u(g_wvp_walks), u(g_wvp_zeroed), u(g_wvp_unready),
                  u(g_wvp_mirrored), u(g_wvp_same), u(g_wvp_seeded), u(g_wvp_notpushed[0]), u(g_wvp_othervp[0]), u(g_wvp_failed[0]),
                  u(g_wvp_cmp_walks), u(g_wvp_cmp_pushes), u(g_wvp_matched), u(g_wvp_exact), u(g_wvp_unmatched),
                  static_cast<double>(g_wvp_worst.load()), static_cast<double>(g_wvp_worst_rel.load()), u(g_wvp_bad), u(g_wvp_notpushed[1]),
                  u(g_wvp_othervp[1]), u(g_wvp_failed[1]), u(g_wvp_inconsistent), u(g_wvp_full));
    return b;
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
        if (g_r.inst && g_r.cl == cl) restore_pso(cl);
        g_pred_off = false;
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
    pso::prepass_open(slice);  // (GOAL-single-pass item 6c: slice 0 is the frame latch's first stage; twin_of's eye2img answer for both)
    if (slice == 0) {  // [Stereo] PrepassInstanced: pass 1 draws both slices (eye 2's rows written, twins made)
        g_r.inst = g_inst_on.load(std::memory_order_relaxed) && g_views_write.load(std::memory_order_relaxed) && g_grow_state > 0 &&
                   pso::twins_made() > 0;
        g_frame_inst = g_r.inst;
        g_frame_cleared = g_r.inst ? 1u << kDepth : 0u;
        g_frame_touched = 0;  // ([Stereo] PrepassWalkCut: pass 1's end begins the follow)
        g_trk_known = 0;
        g_unstarted_open = g_single_unstarted.load(std::memory_order_relaxed);  // (the singles' split: before the marker)
        g_seeded = false;
        g_cut_frame.store(false, std::memory_order_relaxed);  // (a pass 2 with no walk end must not carry it into pass 1)
        g_pred_off = false;
        pso::latch_main_withhold();  // ([Stereo] MainInstanced: the family's twins withheld or not, for this frame's two regions)
        if (g_r.inst) {
            ID3D12GraphicsCommandList1* c1 = nullptr;
            if (SUCCEEDED(cl->QueryInterface(IID_PPV_ARGS(&c1))) && c1) {
                c1->SetViewInstanceMask(3);
                if (main_inst::noting()) d3d::note_view_instance_mask(cl, 3);  // ([Stereo] MainInstanced puts it back after its twin; off: nothing)
                c1->Release();
                g_inst_regions.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_r.inst = g_frame_inst = false;
            }
        }
    } else if (g_frame_inst) {  // pass 2 after an instanced pass 1: its slice is already cleared and holds the twins' draws
        g_r.drop = true;
        g_pred_off = true;
        g_drop_regions.fetch_add(1, std::memory_order_relaxed);
    }
    set_state(kDepth, -1, D3D12_RESOURCE_STATE_DEPTH_WRITE);  // (a depth clear needs it)
    g_end_pending = false;
    g_active = true;
    g_regions.fetch_add(1, std::memory_order_relaxed);
    if (g_last.cl == cl) {  // the targets bound before this clear: bound again, the six as the slices
        D3D12_CPU_DESCRIPTOR_HANDLE r2[8], d2{};
        if (bind(cl, g_last.n, g_last.rts, g_last.single, g_last.has_ds ? &g_last.ds : nullptr, r2, &d2))
            d3d::raw_bind(cl, g_last.n, r2, g_last.single, g_last.has_ds ? &d2 : nullptr);
    }
    if (g_r.drop && (g_frame_cleared & (1u << kDepth))) {
        t_skip_clear = true;
        g_skipped_clears.fetch_add(1, std::memory_order_relaxed);
    }
    return g_arr[kDepth].view[view_index()];
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
    if (g_r.inst) g_frame_cleared |= 1u << k;
    if (g_r.drop && (g_frame_cleared & (1u << k))) {
        t_skip_clear = true;
        g_skipped_clears.fetch_add(1, std::memory_order_relaxed);
    }
    return g_arr[k].view[view_index()];
}

bool bind(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single,
          const D3D12_CPU_DESCRIPTOR_HANDLE* ds, D3D12_CPU_DESCRIPTOR_HANDLE* out_rts, D3D12_CPU_DESCRIPTOR_HANDLE* out_ds) {
    if (cl != g_r.cl || n > 8) return false;
    if (g_end_pending.load(std::memory_order_acquire)) {
        end_region(cl);
        return false;
    }
    bool any = false;
    g_r.all_six = false;
    if (single && n >= 1 && rts) {  // a contiguous range: one handle for all n (not substituted one by one)
        ID3D12Resource* res = nullptr;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        if (kind_of(rts[0], &res, &fmt) > kDepth) {
            end_region(cl);
            fail("a pre-pass target bound as a range");
        }
        return false;
    }
    bool six = n > 0 || ds;  // every bound target one of the six (the twins' rule)
    for (unsigned i = 0; i < n && rts; ++i) {
        out_rts[i] = rts[i];
        ID3D12Resource* res = nullptr;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        const int k = kind_of(rts[i], &res, &fmt);
        if (k <= kDepth || !ensure(k, res, fmt)) {
            six = false;
            continue;
        }
        if (!(g_r.known & (1u << (k * 2)))) set_state(k, -1, D3D12_RESOURCE_STATE_RENDER_TARGET);  // (its barrier before the region)
        g_r.touched |= 1u << k;
        out_rts[i] = g_arr[k].view[view_index()];
        any = true;
    }
    if (ds) {
        *out_ds = *ds;
        ID3D12Resource* res = nullptr;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        if (kind_of(*ds, &res, &fmt) == kDepth && res == g_arr[kDepth].game) {
            *out_ds = g_arr[kDepth].view[view_index()];
            any = true;
        } else {
            six = false;
        }
    }
    g_r.all_six = six && any;
    if (any) g_redirects.fetch_add(1, std::memory_order_relaxed);
    return any;
}

void barrier(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_RESOURCE_BARRIER* b) {
    // ([Stereo] PrepassWalkCut: the game's batches followed for the kinds a region does not track; a batch that ends the
    // region after its copies, which come before it)
    const bool follow = g_cut_armed_flag.load(std::memory_order_relaxed);
    if (cl != g_r.cl || !b) {
        if (follow) track(cl, n, b);
        return;
    }
    if (g_end_pending.load(std::memory_order_acquire)) {  // the walk ended: copied before this batch
        end_region(cl);
        if (follow) track(cl, n, b);
        return;
    }
    if (follow) track(cl, n, b);
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

// "prepass singles" (run 18, read only): pass 1's single (not twinned) draws by the game pipeline and why, so the
// leftovers that hold the walk cut can be named. Written on the recording thread only, and only on a single draw (rare
// with P0 and the WVP mirror); the status reads it with relaxed atomics.
namespace {
constexpr int kSingleSlots = 64;
enum : uint32_t { kSnNotStarted = 1, kSnNoTwins = 2, kSnNotAllSix = 4, kSnNoTwin = 8, kSnInstPrev = 16 };
std::atomic<uintptr_t> g_sn_key[kSingleSlots];
std::atomic<uint64_t> g_sn_count[kSingleSlots];
std::atomic<uint32_t> g_sn_why[kSingleSlots];
std::atomic<uint64_t> g_sn_overflow{0};
std::atomic<uint64_t> g_sn_nopso{0};  // single draws before the region's first pipeline bind (the pipeline inherited: unknown here)
// ([Stereo] PrepassCutHoldPending) of each slot's singles, the started ones (as g_single_started: after the walk-start marker in a
// ready walk); and of the singles of no slot (no pipeline bound yet, or the table full), the started ones
std::atomic<uint64_t> g_sn_started[kSingleSlots];
std::atomic<uint64_t> g_sn_unknown_started{0};
void note_single(const void* pso, uint32_t why, bool started) {
    const uintptr_t k = reinterpret_cast<uintptr_t>(pso);
    if (k == 0) {  // (the review: key 0 marks an empty slot; such draws counted apart)
        g_sn_nopso.store(g_sn_nopso.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        if (started) g_sn_unknown_started.store(g_sn_unknown_started.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        return;
    }
    size_t h = (k >> 4) % kSingleSlots;
    for (int i = 0; i < kSingleSlots; ++i, h = (h + 1) % kSingleSlots) {
        const uintptr_t cur = g_sn_key[h].load(std::memory_order_relaxed);
        if (cur == 0) g_sn_key[h].store(k, std::memory_order_relaxed);
        else if (cur != k) continue;
        // (PrepassCutHoldPending's must-fix) the reasons first, the counts after with release: hp_scan, seeing a count move
        // (acquire), reads the reasons of that single too
        g_sn_why[h].store(g_sn_why[h].load(std::memory_order_relaxed) | why, std::memory_order_relaxed);
        if (started) g_sn_started[h].store(g_sn_started[h].load(std::memory_order_relaxed) + 1, std::memory_order_release);
        g_sn_count[h].store(g_sn_count[h].load(std::memory_order_relaxed) + 1, std::memory_order_release);
        return;
    }
    g_sn_overflow.store(g_sn_overflow.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    if (started) g_sn_unknown_started.store(g_sn_unknown_started.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}
// ---- [Stereo] PrepassCutHoldPending (the render thread, decide_cut): the table above against the snapshot of the last decision
uintptr_t g_hp_key[kSingleSlots];
uint64_t g_hp_cnt[kSingleSlots], g_hp_cst[kSingleSlots];  // each slot's count and started count at the last decision
uint64_t g_hp_seen_unknown = 0, g_hp_seen_unknown_st = 0;
bool hp_pending_state(uint32_t st) { return st == pso::region::kRsQueued || st == pso::region::kRsReadyNotBuilt; }
uint64_t hp_moved(uint64_t now, uint64_t* seen) {  // (a count lower than seen: "prepass singles reset" since; all of it is new)
    const uint64_t d = now < *seen ? now : now - *seen;
    *seen = now;
    return d;
}
// (the review of the margin) a pending or just-installed pipeline's single seen: the pending hold lasts kHoldPendQuiet presents past
// it too, so a twin installed but still not drawn by the pre-pass for a reason twin_given_now did not see (singles go on) is seen
// as permanent (its next singles, past the margin) before the pending hold ends, with no cut frame between (the kinds twin_of
// withholds are permanent at once: hp_scan, hp_hold)
void hp_quiet(uint64_t f) {
    if (f + kHoldPendQuiet > g_hp_until) g_hp_until = f + kHoldPendQuiet;
}
int hp_scan(uint64_t f, bool started_only, bool on) {
    const bool idle = f >= g_cut_hold_until && g_hp_n == 0 && f >= g_hp_until;  // (no hold of either kind before this decision)
    int r = 0;
    for (int i = 0; i < kSingleSlots; ++i) {
        const uintptr_t k = g_sn_key[i].load(std::memory_order_relaxed);
        if (k != g_hp_key[i]) {  // (another pipeline in the slot since: a reset; its counts are all new)
            g_hp_key[i] = k;
            g_hp_cnt[i] = g_hp_cst[i] = 0;
        }
        const uint64_t d_all = hp_moved(g_sn_count[i].load(std::memory_order_acquire), &g_hp_cnt[i]);
        const uint64_t d_st = hp_moved(g_sn_started[i].load(std::memory_order_acquire), &g_hp_cst[i]);
        if (!on || !k || !(started_only ? d_st : d_all)) continue;
        const void* p = reinterpret_cast<const void*>(k);
        // (the must-fix) single for a reason no twin cures (not all six targets, an instprev draw not mirrored, twins off for the
        // region), ever since the last reset: permanent whatever its twin's state (the reasons read after the counts: note_single).
        // (the delta review's should-fix) In "all" mode a not-started single is counted, and no twin cures it either
        if (g_sn_why[i].load(std::memory_order_relaxed) & (kSnNotAllSix | kSnInstPrev | kSnNoTwins | (started_only ? 0u : kSnNotStarted))) {
            g_hp_why_perm.fetch_add(1, std::memory_order_relaxed);
            g_hp_perm_pipes.fetch_add(1, std::memory_order_relaxed);
            r |= d_st ? 5 : 1;
            continue;
        }
        pso::RegionInfo ri;
        const bool got = pso::twin_state(p, &ri);
        const uint32_t st = got ? ri.st & 0xff : pso::region::kRsUnknown;
        if (got && hp_pending_state(st)) {  // pending: listed once a pipeline and creation, its last single noted
            hp_quiet(f);
            int j = 0;
            while (j < g_hp_n && !(g_hp_wait[j].game == p && g_hp_wait[j].gen == ri.gen)) ++j;
            if (j < g_hp_n) {
                g_hp_wait[j].last = f;
                r |= 2;
            } else if (g_hp_n < kHoldPendSlots) {
                g_hp_wait[g_hp_n++] = HpWait{p, ri.gen, f, f};
                g_hp_pend_added.fetch_add(1, std::memory_order_relaxed);
                r |= 2;
            } else {
                g_hp_full.fetch_add(1, std::memory_order_relaxed);
                r |= d_st ? 5 : 1;
            }
            continue;
        }
        if (got && st == pso::region::kRsMade && f < ri.made_at + kHoldPendMargin) {  // installed since its singles were drawn
            if (pso::twin_given_now(p, ri.st)) {
                if (ri.made_at + kHoldPendMargin > g_hp_until) g_hp_until = ri.made_at + kHoldPendMargin;
                hp_quiet(f);
                g_hp_made_seen.fetch_add(1, std::memory_order_relaxed);
                r |= 2;
                continue;
            }
            g_hp_not_given.fetch_add(1, std::memory_order_relaxed);  // (the must-fix: a twin the pre-pass is not given: permanent at once)
        }
        g_hp_perm_pipes.fetch_add(1, std::memory_order_relaxed);  // (never a twin, one this single did not use, or one not given)
        r |= d_st ? 5 : 1;
    }
    const uint64_t u_all = hp_moved(g_sn_nopso.load(std::memory_order_relaxed) + g_sn_overflow.load(std::memory_order_relaxed), &g_hp_seen_unknown);
    const uint64_t u_st = hp_moved(g_sn_unknown_started.load(std::memory_order_relaxed), &g_hp_seen_unknown_st);
    if (on && (started_only ? u_st : u_all)) {
        g_hp_unknown.fetch_add(started_only ? u_st : u_all, std::memory_order_relaxed);
        r |= u_st ? 5 : 1;
    }
    if (idle && (r & 2) && !(r & 1)) g_hp_pend_holds.fetch_add(1, std::memory_order_relaxed);
    return r;
}
bool hp_hold(uint64_t f, bool on) {
    if (!on) {  // (switched off at run time: a pending hold left becomes the kCutHold one, as the decision without the switch holds)
        if (g_hp_n || f < g_hp_until) {
            if (g_cut_hold_until < f + kCutHold) g_cut_hold_until = f + kCutHold;
            g_cut_hold_cause = g_cut_hold_started.load(std::memory_order_relaxed) ? 1 : 2;
        }
        g_hp_n = 0;
        g_hp_until = 0;
        return false;
    }
    for (int j = 0; j < g_hp_n;) {
        HpWait& w = g_hp_wait[j];
        pso::RegionInfo ri;
        if (!pso::twin_state(w.game, &ri)) {  // (its slot being written through every retry: again at the next decision)
            ++j;
            continue;
        }
        const uint32_t st = ri.st & 0xff;
        if (ri.gen == w.gen && hp_pending_state(st)) {
            if (f < w.last + kCutHold) {
                ++j;
                continue;
            }
            g_hp_expired.fetch_add(1, std::memory_order_relaxed);  // (no single of it for kCutHold: the hold without the switch had ended)
        } else if (ri.gen == w.gen && st == pso::region::kRsMade && pso::twin_given_now(w.game, ri.st)) {
            if (ri.made_at + kHoldPendMargin > g_hp_until) g_hp_until = ri.made_at + kHoldPendMargin;
            const uint64_t wait = ri.made_at > w.since ? ri.made_at - w.since : 0;
            if (wait > g_hp_wait_max.load(std::memory_order_relaxed)) g_hp_wait_max.store(wait, std::memory_order_relaxed);
            g_hp_cleared.fetch_add(1, std::memory_order_relaxed);
        } else {  // refused, failed, held, another pipeline at its address, or (the must-fix) installed with a twin the pre-pass is
                  // not given now (pso::twin_given_now): the kCutHold hold from now, never the margin
            if (ri.gen == w.gen && st == pso::region::kRsMade) g_hp_not_given.fetch_add(1, std::memory_order_relaxed);
            if (g_cut_hold_until < f + kCutHold) {
                if (f >= g_cut_hold_until) g_hp_perm_holds.fetch_add(1, std::memory_order_relaxed);
                g_cut_hold_until = f + kCutHold;
            }
            g_cut_hold_cause = g_cut_hold_started.load(std::memory_order_relaxed) ? 1 : 2;
            g_hp_to_perm.fetch_add(1, std::memory_order_relaxed);
        }
        w = g_hp_wait[--g_hp_n];  // (the last entry in its place: looked at next)
    }
    return g_hp_n > 0 || f < g_hp_until;
}
// a listed pipeline's twin state for "prepass singles" (PrepassCutHoldPending armed)
const char* hp_state_name(const void* p, uint64_t f, char* buf, size_t n) {
    static const char* const kSt[] = {"unknown(stream)", "pending(queued)", "pending(ready)", "made", "vs_refused", "ps_refused",
                                      "held", "build_failed", "shape", "norule"};
    static_assert(sizeof(kSt) / sizeof(kSt[0]) == pso::region::kRsStates, "a name for each region state");
    pso::RegionInfo ri;
    if (!pso::twin_states_on()) return "no-table";
    if (!pso::twin_state(p, &ri)) return "not-listed";
    const uint32_t st = ri.st & 0xff;
    if (st == pso::region::kRsMade) {
        std::snprintf(buf, n, "made(%llu presents ago)", static_cast<unsigned long long>(f >= ri.made_at ? f - ri.made_at : 0));
        return buf;
    }
    return st < pso::region::kRsStates ? kSt[st] : "bad-state";
}
// ([Stereo] PrepassCutEmptyDraws) pass 1's empty draws by the game pipeline (nocb.md open question 1), as note_single
constexpr int kEmptySlots = 16;
std::atomic<uintptr_t> g_em_key[kEmptySlots];
std::atomic<uint64_t> g_em_count[kEmptySlots];
std::atomic<uint64_t> g_em_overflow{0};
void note_empty(const void* pso) {
    const uintptr_t k = reinterpret_cast<uintptr_t>(pso);
    size_t h = (k >> 4) % kEmptySlots;
    for (int i = 0; k && i < kEmptySlots; ++i, h = (h + 1) % kEmptySlots) {
        const uintptr_t cur = g_em_key[h].load(std::memory_order_relaxed);
        if (cur == 0) g_em_key[h].store(k, std::memory_order_relaxed);
        else if (cur != k) continue;
        g_em_count[h].store(g_em_count[h].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        return;
    }
    g_em_overflow.store(g_em_overflow.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);  // (or no pipeline known)
}
}  // namespace

bool before_draw(ID3D12GraphicsCommandList* cl, unsigned count, unsigned instances) {
    if (cl != g_r.cl) return false;
    if (g_end_pending.load(std::memory_order_acquire)) {
        end_region(cl);
        return false;
    }
    if (!g_r.inst && !g_r.drop) return false;
    if ((count == 0 || instances == 0) && g_empty_on.load(std::memory_order_relaxed)) {  // [Stereo] PrepassCutEmptyDraws (nocb.md 4)
        if (g_r.drop) {
            g_empty_p2.fetch_add(1, std::memory_order_relaxed);
        } else {
            (g_r.started ? g_empty_started : g_empty_unstarted).fetch_add(1, std::memory_order_relaxed);
            note_empty(g_r.game_pso);
            // (run 20 review) a twin left bound by an earlier draw while the targets are no longer all six: the game's
            // pipeline back first, so a view-instanced pipeline never meets non-array views (it would draw nothing, but
            // the combination is new for drivers and the debug layer)
            if (g_r.set_pso != g_r.game_pso && !g_r.all_six) restore_pso(cl);
        }
        return false;  // (the game's no-op runs on the bound pipeline, the game's or its twin as the last draw left it: g_r.set_pso still names it)
    }
    ID3D12PipelineState* twin = g_r.started && !g_r.no_twins && g_r.all_six ? g_r.game_twin : nullptr;
    // [Stereo] PrepassInstPrevMirror: an instprev twin only where this draw's eye-2 rows were written (pass 1: its marker;
    // pass 2: pass 1 mirrored the same draw). Otherwise pass 1 draws it single and pass 2 draws it, not dropped
    bool ip_single = false;
    if (twin && g_r.twin_ip && !inst_prev::draw_ok()) {
        twin = nullptr;
        ip_single = true;
    }
    pso::prepass_draw(g_r.slice, g_r.game_pso, twin);  // (GOAL-single-pass item 6c's check: off, one load)
    if (g_r.drop) {
        (twin ? g_dropped : g_drawn).fetch_add(1, std::memory_order_relaxed);
        return twin != nullptr;
    }
    ID3D12PipelineState* want = twin ? twin : g_r.game_pso;
    if (want && want != g_r.set_pso) {
        pso::set_raw(cl, want);
        g_r.set_pso = want;
    }
    (twin ? g_twin_draws : g_single_draws).fetch_add(1, std::memory_order_relaxed);
    if (!twin)  // (run 19: the hold's split)
        (g_r.started ? (g_r.no_twins ? g_single_unready : g_single_started) : g_single_unstarted).fetch_add(1, std::memory_order_relaxed);
    if (!twin)
        note_single(g_r.game_pso, (g_r.started ? 0u : kSnNotStarted) | (g_r.no_twins ? kSnNoTwins : 0u) | (g_r.all_six ? 0u : kSnNotAllSix) |
                                      (g_r.game_twin ? 0u : kSnNoTwin) | (ip_single ? kSnInstPrev : 0u),
                    g_r.started && !g_r.no_twins);  // (started: as g_single_started, PrepassCutHoldPending's split)
    return false;
}

void singles_reset() {  // "prepass singles reset" (a test: counts from now; the writer racing it loses a count at most)
    for (int i = 0; i < kSingleSlots; ++i) {
        g_sn_count[i].store(0, std::memory_order_relaxed);
        g_sn_why[i].store(0, std::memory_order_relaxed);
        g_sn_started[i].store(0, std::memory_order_relaxed);
        g_sn_key[i].store(0, std::memory_order_relaxed);
    }
    g_sn_overflow.store(0, std::memory_order_relaxed);
    g_sn_nopso.store(0, std::memory_order_relaxed);
    g_sn_unknown_started.store(0, std::memory_order_relaxed);  // (PrepassCutHoldPending's snapshot reads a lower count as all new)
    for (int i = 0; i < kEmptySlots; ++i) {  // ([Stereo] PrepassCutEmptyDraws' table too)
        g_em_count[i].store(0, std::memory_order_relaxed);
        g_em_key[i].store(0, std::memory_order_relaxed);
    }
    g_em_overflow.store(0, std::memory_order_relaxed);
}

std::string singles_status() {
    struct Row {
        uintptr_t k;
        uint64_t n;
        uint32_t why;
    };
    Row rows[kSingleSlots];
    int nr = 0;
    for (int i = 0; i < kSingleSlots; ++i) {
        const uintptr_t k = g_sn_key[i].load(std::memory_order_relaxed);
        if (k) rows[nr++] = {k, g_sn_count[i].load(std::memory_order_relaxed), g_sn_why[i].load(std::memory_order_relaxed)};
    }
    std::sort(rows, rows + nr, [](const Row& a, const Row& b) { return a.n > b.n; });
    std::string out = "pass 1's single draws by pipeline (" + std::to_string(nr) + " pipelines, overflow " +
                      std::to_string(g_sn_overflow.load(std::memory_order_relaxed)) + ", before the region's first pipeline bind " +
                      std::to_string(g_sn_nopso.load(std::memory_order_relaxed)) + "; indirect draws not counted):";
    const uint64_t f = state::presents.load();
    for (int i = 0; i < nr && i < 12; ++i) {
        uint64_t vs = 0, ps = 0;
        pso::describe(reinterpret_cast<const void*>(rows[i].k), &vs, &ps);
        char b[200];
        std::snprintf(b, sizeof(b), " | vs %016llx ps %016llx: %llu%s%s%s%s%s", static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps),
                      static_cast<unsigned long long>(rows[i].n), (rows[i].why & kSnNoTwin) ? " no-twin" : "",
                      (rows[i].why & kSnNotStarted) ? " not-started" : "", (rows[i].why & kSnNoTwins) ? " twins-off" : "",
                      (rows[i].why & kSnNotAllSix) ? " not-all-six" : "", (rows[i].why & kSnInstPrev) ? " instprev-not-mirrored" : "");
        out += b;
        if (g_hp_armed) {  // ([Stereo] PrepassCutHoldPending) the pipeline's twin state now, as the hold reads it (the region table)
            char sb[48];
            out += std::string(" twin=") + hp_state_name(reinterpret_cast<const void*>(rows[i].k), f, sb, sizeof(sb));
        }
    }
    if (g_empty_on.load(std::memory_order_relaxed)) {  // ([Stereo] PrepassCutEmptyDraws) pass 1's empty draws, not singles
        out += " || pass 1's empty draws by pipeline (overflow or no pipeline " + std::to_string(g_em_overflow.load(std::memory_order_relaxed)) + "):";
        for (int i = 0; i < kEmptySlots; ++i) {
            const uintptr_t k = g_em_key[i].load(std::memory_order_relaxed);
            if (!k) continue;
            uint64_t vs = 0, ps = 0;
            pso::describe(reinterpret_cast<const void*>(k), &vs, &ps);
            char b[96];
            std::snprintf(b, sizeof(b), " | vs %016llx ps %016llx: %llu", static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps),
                          static_cast<unsigned long long>(g_em_count[i].load(std::memory_order_relaxed)));
            out += b;
        }
    }
    return out;
}

void note_pipeline(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* ps) {
    if (cl != g_r.cl) return;
    g_r.game_pso = ps;
    g_r.set_pso = ps;  // (the hook sets it next)
    g_r.game_twin = g_r.inst || g_r.drop ? pso::twin_of(ps) : nullptr;
    g_r.twin_ip = g_r.game_twin && pso::instprev_twin(g_r.game_twin);  // ([Stereo] PrepassInstPrevMirror; false at once without its twins)
}
void before_indirect(ID3D12GraphicsCommandList* cl) {  // an indirect draw in a region: never with a twin, never dropped
    if (cl == g_r.cl && g_r.inst && g_r.set_pso != g_r.game_pso) restore_pso(cl);
}
bool take_skip_clear() {
    const bool s = t_skip_clear;
    t_skip_clear = false;
    return s;
}
// (round 136, a test: "prepass pred off|game") the game's occlusion predicates dropped in pass 1 too. The game leaves a
// draw out of the picture when its occlusion query (a box, where the GAME has the thing) counted no pixel, while the
// depth pass before it draws everything: a guess at the duster's dark outline. The user with it on as an option
// ([Render] DrawOccluded, round 138): "Nothing changed": not it; the option taken out again (round 139), the test
// command kept
std::atomic<bool> g_pred_force_off{false};
std::atomic<uint64_t> g_pred_dropped{0};
bool predication_off() {
    if (g_pred_off.load(std::memory_order_relaxed)) return true;
    if (!g_pred_force_off.load(std::memory_order_relaxed)) return false;
    g_pred_dropped.fetch_add(1, std::memory_order_relaxed);
    return true;
}
std::string predication_force(int set) {
    if (set >= 0) g_pred_force_off = set == 1;
    char b[120];
    std::snprintf(b, sizeof(b), "predication in pass 1: %s (dropped by the switch %llu)", g_pred_force_off.load() ? "off" : "the game's",
                  static_cast<unsigned long long>(g_pred_dropped.load()));
    return b;
}
void set_instanced(bool on) {
    if (on && !g_inst_hooked.load()) {
        log::warn("[prepass] instanced not on: start with [Stereo] PrepassInstanced=1, PrepassArrays=1, PrepassViews=2 and InstancedShaders");
        return;
    }
    g_inst_on = on;
    log::info("[prepass] instanced %s (from the next pass 1)", on ? "on" : "off");
}

bool views_wanted() { return g_views.load(std::memory_order_relaxed); }
static float g_e2_rows01[8];
static bool g_e2_levelled = false;
static uint64_t g_e2_levels = 0;
void eye2_level(const float* level8) {
    g_e2_levelled = false;
    if (!g_views_write.load(std::memory_order_relaxed) || g_grow_state <= 0 || dual_pass::pass() != 1 || !still_grown(g_b1)) return;
    if (!read_raw(g_b1.block + 0x190, g_e2_rows01, sizeof(g_e2_rows01))) return;
    float l[8];
    std::memcpy(l, level8, sizeof(l));
    l[3] = g_e2_rows01[3];  // (each eye's own w)
    l[7] = g_e2_rows01[7];
    if (!write_raw(g_b1.block + 0x190, l, sizeof(l))) return;
    wr<int>(g_b1.cb + 0x24, -1);
    g_e2_levelled = true;
    ++g_e2_levels;
}
void eye2_unlevel() {
    if (!g_e2_levelled) return;
    g_e2_levelled = false;
    if (still_grown(g_b1) && write_raw(g_b1.block + 0x190, g_e2_rows01, sizeof(g_e2_rows01))) wr<int>(g_b1.cb + 0x24, -1);
}
bool union_cull_wanted() { return g_cull_on.load(std::memory_order_relaxed); }
void set_union_cull(bool on) {
    if (on && !g_cull_hooked.load()) {
        log::warn("[prepass] union cull not on: start with [Stereo] PrepassUnionCull=1 (the walk's start hook)");
        return;
    }
    g_cull_on = on;
    log::info("[prepass] union cull %s (from the next pass 1)", on ? "on" : "off");
}
std::string cull_status() {
    char b[320];
    std::snprintf(b, sizeof(b), "union cull %s%s: walks widened %llu, put back at the walk's end %llu, dropped %llu, no union viewport %llu; last widening %.2f deg",
                  g_cull_on.load() ? "on" : "off", g_cull_hooked.load() ? "" : " (not hooked)", static_cast<unsigned long long>(g_cull_begins.load()),
                  static_cast<unsigned long long>(g_cull_ends.load()), static_cast<unsigned long long>(g_cull_stale.load()),
                  static_cast<unsigned long long>(g_cull_nounion.load()), g_cull_widen);
    return b;
}
void set_eye2(const void* vp2, const void* prev_vp) {
    const char* v = static_cast<const char*>(vp2);
    g_e2_ok = read_raw(reinterpret_cast<uintptr_t>(v + 0xc0), g_e2_wv, 64) && read_raw(reinterpret_cast<uintptr_t>(v + 0x140), g_e2_vi, 64);
    g_e2_pvp_ok = prev_vp && read_raw(reinterpret_cast<uintptr_t>(prev_vp), g_e2_pvp, 64);
    g_e2_frame = state::presents.load();
    inst_prev::note_eye2(vp2, prev_vp != nullptr);  // ([Stereo] PrepassInstPrevMirror: the last call wins; taa.cpp's: vp2 +0x200 eye 2's)
    prevworld::note_eye2(vp2, prev_vp != nullptr);  // ([Stereo] PrepassPrevWorldMirror: the grow, vp2's frame)
    if (g_e2_ok && g_views_write.load(std::memory_order_relaxed) && g_grow_state >= 0) write_eye2(g_e2_wv, g_e2_vi, g_e2_pvp_ok ? g_e2_pvp : nullptr);
    // [Stereo] PrepassWvpMirror: eye 2's View and Proj for the per-push mirror (the last call before the walk wins: the
    // TAA update's, its Proj jittered as pass 2's will be)
    if (g_wvp_on.load(std::memory_order_relaxed))
        g_e2_vp_frame = read_raw(reinterpret_cast<uintptr_t>(v + 0x180), g_e2_view, 64) && read_raw(reinterpret_cast<uintptr_t>(v + 0x1c0), g_e2_proj, 64)
                            ? g_e2_frame
                            : ~0ull;
}
void views_size(bool grown) { g_size_want = grown ? 1 : 0; }
std::string views_status() {
    char w[360];
    std::snprintf(w, sizeof(w), " | written: %s, %llu frames written, after the walk kept %llu, lost %llu, grass levelled for eye 2 %llu%s%s",
                  g_grow_state > 0 ? "grown" : g_grow_state < 0 ? "refused" : "not yet", static_cast<unsigned long long>(g_written),
                  static_cast<unsigned long long>(g_kept), static_cast<unsigned long long>(g_lost), static_cast<unsigned long long>(g_e2_levels),
                  g_grow_state < 0 ? ": " : "", g_grow_why);
    char x[400];  // (item 8e 14: the bit-exact count, read on the command thread; the render thread's plain counters, a diagnostic)
    int k = std::snprintf(x, sizeof(x), " | exact: frames %llu of %llu (gWorldView not %llu, gViewInverse not %llu; ulps at most %u, %u), gPrevViewProj exact %llu, not %llu (ulps %u); not exact at",
                          static_cast<unsigned long long>(g_vx.eq), static_cast<unsigned long long>(g_vs.frames), static_cast<unsigned long long>(g_vx.ne_wv),
                          static_cast<unsigned long long>(g_vx.ne_vi), g_vx.ulp_wv, g_vx.ulp_vi, static_cast<unsigned long long>(g_vx.pvp_eq),
                          static_cast<unsigned long long>(g_vx.ne_pvp), g_vx.ulp_pvp);
    const uint32_t rn = g_vx.ring_n;
    for (uint32_t i = rn > 16 ? rn - 16 : 0; i < rn && k > 0 && k < static_cast<int>(sizeof(x)) - 24; ++i)
        k += std::snprintf(x + k, sizeof(x) - k, " %llu", static_cast<unsigned long long>(g_vx.ring[i % 16]));
    if (!rn && k > 0 && k < static_cast<int>(sizeof(x)) - 4) std::snprintf(x + k, sizeof(x) - k, " -");
    return std::string(g_views.load() ? "views on: " : "views off: ") + g_vs_text + (g_views_write.load() ? w : "") + x;
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
    char ib[360] = "";
    if (g_inst_hooked.load())
        std::snprintf(ib, sizeof(ib),
                      " | instanced %s: pass 1 regions %llu (twin draws %llu, single %llu), pass 2 regions %llu (dropped %llu, drawn %llu), clears "
                      "skipped %llu, pipelines put back %llu, predication off %d, walk starts ready %llu not %llu",
                      g_inst_on.load() ? "on" : "off", static_cast<unsigned long long>(g_inst_regions.load()),
                      static_cast<unsigned long long>(g_twin_draws.load()), static_cast<unsigned long long>(g_single_draws.load()),
                      static_cast<unsigned long long>(g_drop_regions.load()), static_cast<unsigned long long>(g_dropped.load()),
                      static_cast<unsigned long long>(g_drawn.load()), static_cast<unsigned long long>(g_skipped_clears.load()),
                      static_cast<unsigned long long>(g_pso_restores.load()), g_pred_off.load() ? 1 : 0,
                      static_cast<unsigned long long>(g_starts_ok.load()), static_cast<unsigned long long>(g_starts_plain.load()));
    char cb[2048] = "";  // (run 26: 1100 cut the cut-more-than-once count off the end since run 25's holds and declines)
    const uint64_t mm = g_trk_mismatch.load();
    if (g_cut_on.load() || g_cut_frames.load())
        std::snprintf(cb, sizeof(cb),
                      " | walk cut %s%s: frames %llu, cuts %llu, handovers %llu, pass 2 regions ended with it %llu, problems %llu, followed "
                      "mismatches %llu, holds %llu (until %llu), gust kept %llu",
                      g_cut_on.load() ? "on" : "off", g_cut_force.load() ? " (forced)" : "", static_cast<unsigned long long>(g_cut_frames.load()),
                      static_cast<unsigned long long>(rdrvr_prepass_cuts), static_cast<unsigned long long>(g_cut_handovers.load()),
                      static_cast<unsigned long long>(g_cut_region_ends.load()), static_cast<unsigned long long>(g_cut_bad.load()),
                      static_cast<unsigned long long>(mm), static_cast<unsigned long long>(g_cut_holds.load()),
                      static_cast<unsigned long long>(g_cut_hold_until), static_cast<unsigned long long>(dual_pass::gust_kept()));
    if (g_cut_on.load() || g_cut_frames.load() || g_cut_hold_started.load()) {
        auto v = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load(std::memory_order_relaxed)); };
        const size_t used = std::strlen(cb);
        std::snprintf(cb + used, sizeof(cb) - used,
                      " | cut declines (hold by %s): decisions %llu, off %llu, arrays off, failed or instancing off %llu, not ready %llu, no twins %llu, held "
                      "by started singles %llu, by other singles %llu, by problems %llu, instprev refused %llu, marker failed %llu | pass 1 "
                      "singles started %llu, not started %llu (before the walk-start marker %llu), started in a walk not ready %llu | "
                      "walk-start markers failed %llu, cut handovers with no pass 2 region %llu (held %llu) | cut frames with more than "
                      "one 0x706050 cut %llu (calls past the first %llu)",
                      g_cut_hold_started.load() ? "started singles and problems" : "any single or problem", v(g_cutd_calls), v(g_cutd_off),
                      v(g_cutd_state), v(g_cutd_unready), v(g_cutd_notwins), v(g_cutd_held_started), v(g_cutd_held_unstarted), v(g_cutd_held_bad),
                      v(g_cutd_instprev), v(g_cutd_nomarker), v(g_single_started), v(g_single_unstarted), v(g_single_prewalk),
                      v(g_single_unready), v(g_start_marker_failed), v(g_cut_noregion), v(g_cut_noregion_held), v(g_cut_multi),
                      v(g_cut_extra));
    }
    char hb[2048] = "";
    if (g_hp_armed) {  // ([Stereo] PrepassCutHoldPending, test only) the holds by kind and the pending list (the render thread's, read racily)
        auto v = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load(std::memory_order_relaxed)); };
        const unsigned long long held_perm = v(g_cutd_held_started) + v(g_cutd_held_unstarted);
        std::snprintf(hb, sizeof(hb),
                      " | cut hold pending %s (region table %s): holds begun pending %llu, permanent %llu; pending pipelines listed %llu (installed "
                      "when first seen %llu), cleared by their install %llu, turned permanent %llu, expired %llu, list full %llu; permanent pipelines "
                      "%llu, singles of no known pipeline %llu; installed twins not given to the pre-pass %llu, pipelines single for a reason no twin "
                      "cures %llu; held frames by the pending hold %llu, by singles %llu, by problems %llu; the "
                      "longest pending wait %llu presents; waiting now %d (pending hold until %llu, permanent until %llu) | kv: hp_on=%d hp_table=%d "
                      "hp_pend_holds=%llu hp_perm_holds=%llu hp_pend_added=%llu hp_made_seen=%llu hp_cleared=%llu hp_to_perm=%llu hp_expired=%llu "
                      "hp_full=%llu hp_perm_pipes=%llu hp_unknown=%llu hp_held_pend=%llu hp_held_perm=%llu hp_held_bad=%llu hp_wait_max=%llu "
                      "hp_waiting=%d hp_cut_frames=%llu hp_decisions=%llu hp_holds=%llu hp_not_given=%llu hp_why_perm=%llu",
                      g_hp_on.load() ? "on" : "off", pso::twin_states_on() ? "written" : "NOT written: every single permanent", v(g_hp_pend_holds),
                      v(g_hp_perm_holds), v(g_hp_pend_added), v(g_hp_made_seen), v(g_hp_cleared), v(g_hp_to_perm), v(g_hp_expired), v(g_hp_full),
                      v(g_hp_perm_pipes), v(g_hp_unknown), v(g_hp_not_given), v(g_hp_why_perm), v(g_hp_held), held_perm, v(g_cutd_held_bad),
                      v(g_hp_wait_max), g_hp_n, static_cast<unsigned long long>(g_hp_until), static_cast<unsigned long long>(g_cut_hold_until),
                      g_hp_on.load() ? 1 : 0, pso::twin_states_on() ? 1 : 0, v(g_hp_pend_holds), v(g_hp_perm_holds), v(g_hp_pend_added), v(g_hp_made_seen), v(g_hp_cleared),
                      v(g_hp_to_perm), v(g_hp_expired), v(g_hp_full), v(g_hp_perm_pipes), v(g_hp_unknown), v(g_hp_held), held_perm,
                      v(g_cutd_held_bad), v(g_hp_wait_max), g_hp_n, v(g_cut_frames), v(g_cutd_calls), v(g_cut_holds), v(g_hp_not_given),
                      v(g_hp_why_perm));
    }
    char b[900];
    std::snprintf(b, sizeof(b), "prepass %s%s%s: regions %llu, walk-end markers %llu, ended by them %llu, copies %llu, redirected binds %llu, passes past two %llu%s%s",
                  g_on.load() ? "on" : "off", g_failed.load() ? " (stood down)" : "", g_active.load() ? " (in a region)" : "",
                  static_cast<unsigned long long>(g_regions.load()), static_cast<unsigned long long>(g_markers.load()),
                  static_cast<unsigned long long>(g_marker_ends.load()), static_cast<unsigned long long>(g_copies.load()),
                  static_cast<unsigned long long>(g_redirects.load()), static_cast<unsigned long long>(g_skipped.load()),
                  g_failed.load() ? ": " : "", g_failed.load() ? g_fail_reason : "");
    char eb[200] = "";
    if (g_empty_on.load())  // ([Stereo] PrepassCutEmptyDraws: neither twinned nor single)
        std::snprintf(eb, sizeof(eb), " | empty draws (no instances or no vertices): pass 1 started %llu, not started %llu, pass 2 %llu",
                      static_cast<unsigned long long>(g_empty_started.load()), static_cast<unsigned long long>(g_empty_unstarted.load()),
                      static_cast<unsigned long long>(g_empty_p2.load()));
    char nb[768];
    d3d::nested_status(nb, sizeof(nb));  // (the list hooks' debug-layer forwards: 0 without the layer)
    return std::string(b) + ib + cb + hb + eb + " | " + nb;
}

}  // namespace rdrvr::prepass

// [Stereo] PrepassCutVelocity (run 19, test only): its own file, compiled in this translation unit while CMakeLists.txt is
// held by step C's uncommitted change. Move it into rdrvr_core's source list (and drop this line) when that is committed.
#include "core/cut_velocity.cpp"

// [Stereo] PrepassInstPrevMirror (run 19 item 2, test only): its own file, compiled in this translation unit as
// cut_velocity.cpp is (CMakeLists.txt held by step C's uncommitted change). Move both into rdrvr_core's source list later.
#include "core/inst_prev_mirror.cpp"

// [Stereo] PrepassPrevWorldMirror (run 19 item 6, test only): its own file, compiled in this translation unit as the two
// above (CMakeLists.txt held by uncommitted changes). Move the three into rdrvr_core's source list later.
#include "core/prevworld_mirror.cpp"
