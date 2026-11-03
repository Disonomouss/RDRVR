#include "core/eye2_collector.h"

#include <windows.h>
#include <d3d12.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/anchors.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/d3d_hooks.h"
#include "core/dual_pass.h"
#include "core/eye2_light.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/pass2_skip.h"
#include "core/prepass_arrays.h"
#include "core/state.h"

#pragma intrinsic(_ReturnAddress)

namespace rdrvr::prepass {
std::atomic<bool> g_eye2_pending_flag{false};
}

namespace rdrvr::eye2_collector {
namespace {
using anchors::Id;

// FUN_1406e58a0(lighting, viewport, r8, r9b, [rsp+0x28] char, [rsp+0x30], [rsp+0x38] int) (research\run20\p3-textures.md
// sections 1.3 and 5, Ghidra 2026-10-22) [V the disassembly]: r8 is never read (its first use, 0x1406e5923, writes it), the
// 6th argument's slot is written as a local (RBP+0x9238) before any read; r9b is the cloud flag (R12D), the 5th is read
// at RBP+0x9230 (FUN_1406dd4d0 and the state pushes) and the 7th at RBP+0x9240 (the cascade count). All seven are passed
// through as the game gave them: each as the 64-bit slot it arrived in.
using Dir_t = void (*)(void* lighting, void* vp, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7);
// ViewportSetCurrent 0x130940 (vp, push, unused, keep) [V]: ViewportCurrent = vp; with push and keep 0, FUN_140130f00
// (the size normaliser), PushGlobals and the viewport packet; returns the previous current viewport
using SetCurrent_t = uintptr_t (*)(uintptr_t vp, char push, uintptr_t unused, char keep);
Dir_t o_dir = nullptr;
uintptr_t g_ret = 0;  // 0x5cbfd0's call's return address (DirCollectorCall + 5)

std::atomic<bool> g_installed{false}, g_on{false};
using Count = std::atomic<uint64_t>;
inline void bump(Count& c) { c.fetch_add(1, std::memory_order_relaxed); }
inline unsigned long long u(const Count& c) { return static_cast<unsigned long long>(c.load(std::memory_order_relaxed)); }

// the render thread's counters
Count g_p1_calls, g_runs, g_no_vp2, g_not_scene, g_no_stream, g_out_marker_failed, g_fault, g_p2_calls, g_p2_caps, g_p2_marker_failed;
Count g_no_follow, g_not_ready, g_preps, g_p2_other_frame, g_states_restored, g_states_fault, g_prepass_off, g_vd_rebinds;
Count g_no_ctx;  // (run 21) eye 2's runs whose recorder context was not found (the +0x1a0 binding then not put back)
uintptr_t g_bind_vd = 0;  // FUN_140ec67a0(ctx, decl): the recorder context's vertex-declaration binding (+0x1a0 pointer, +0x1a8 id)
Count g_run_ticks;
uint64_t g_pass1_seq = 0, g_cap_seq = ~0ull;  // the render thread: pass-1 calls of the hook, and the captured one's (pass 2 must follow it)
uintptr_t g_rs_cache = 0;                      // the render-state cache DAT_142ac0b60 (ids 0..0x2b), read from RenderStateSet's own code
// ([Stereo] PrepassEye2Decals, run 23, research\run23\decals.md; test only, off by default) eye 2's collector decals
// (DecalMgr vt+0x70 = 0x1405e0dc0: wheel tracks, footprints, gravestone text, blood and bullet decals) drawn into eye 2's
// collector in pass 1 before its directional term, as 0x5cbfd0 draws them for pass 2's eye after the handover
bool g_decals = false;                  // installed (read at install)
std::atomic<bool> g_decals_on{false};   // "prepass e2sc decals on|off"
Count g_dec_calls, g_dec_no_mgr, g_dec_ring, g_dec_states, g_dec_marker_failed, g_dec_no_tls, g_dec_fault;
bool g_dec_drawn = false;  // (item 7's S2, the render thread) the last pass-1 run drew eye 2's decals (the call made, no fault)
constexpr uint32_t kRingMargin = 0x1800000;  // the immediate ring DAT_142c371c8 (0x12e280 executes int3 above 0x2000000): skipped from 24 MB
double g_qpc_us = 0.0;
// the playback thread's
Count g_in1_ok, g_in1_refused, g_out1_ok, g_out1_none, g_out1_fatal, g_in2_ok, g_in2_refused, g_out2_ok, g_out2_refused;
Count g_made, g_make_failed, g_ops_overflow, g_ops_other_thread, g_eye1_exposed, g_not_direct;
Count g_dec_read_refused;  // ([Stereo] PrepassEye2Decals) a capture's kSIn1 not read after eye 2's decals
bool g_cap_dec = false;    // (the playback thread) this capture's IN1 left kSIn1 to kDec1Cap
Count g_c_dec_frames;      // (the worker) compared frames whose kSIn1 was read after eye 2's decals
std::atomic<bool> g_img_ready{false};  // SC2 and the saves made for the current targets (the playback thread; the render thread gates on it)
std::atomic<bool> g_stood{false};      // stood down: "on" refused from then on
std::atomic<uint64_t> g_runs_total{0};  // ([Stereo] PrepassEye2Light: runs(); never reset)
char g_why[200] = "";  // the last refusal (the playback thread writes; the status reads, test only)

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

void refuse(Count& c, const char* why) {
    bump(c);
    std::snprintf(g_why, sizeof(g_why), "%s", why);
    log::limited("e2sc.refused", 10, "[e2sc] a step refused: %s (the frame runs as the game's)", why);
}
void stand_down(const char* why) {
    g_stood = true;
    std::snprintf(g_why, sizeof(g_why), "%s", why);
    if (g_on.exchange(false)) log::error("[e2sc] PrepassEye2Collector stood down: %s", why);
}
// eye 2's call is in the stream but eye 1's targets were not (or will not be) put back: eye 1's collector and depth carry
// eye 2's this frame. Counted, and the module stands down.
void exposed(const char* why) {
    bump(g_eye1_exposed);
    stand_down(why);
}

// ---- the capture (the compare's frames): one in flight at a time
enum Cap : int { kIdle, kArmed, kRec1, kRec2, kReady, kWait };
std::atomic<int> g_cap{kIdle};
std::atomic<int> g_cap_left{0};
std::atomic<uint64_t> g_cap_since{0}, g_ready_frame{0};
std::atomic<bool> g_cap_cut{false};  // the render thread, at the captured frame's pass 2: its walk was cut
Count g_cap_started, g_cap_incomplete, g_cap_done;
// (run 21 item 2, research\run21\d4.md) kSS1: the collector array's slice 1 again at IN2 (pass 2, after the handover):
// against IN1 it says whether slice 1 itself changed between the passes, against IN2 whether the game's collector is
// still slice 1's copy there
// (run 22 item 3, research\run22\d4band.md; "prepass e2sc where2 on", test only, off by default) kSH: the game's collector
// at the first list call after a cut pass 2's region has ended (just after the handover's copies), so the band's write
// is placed either in the handover itself (kSH != slice 1) or between the handover and IN2 (kSH = slice 1, kSH != IN2).
// It is optional: a capture is complete without it (kAllSlots leaves it out), as on a frame that is not cut
enum Slot : int { kSIn1, kSD1, kSOut1, kSIn2, kSD2, kSOut2, kSS1, kSH, kSlots };
constexpr unsigned kAllSlots = (1u << kSH) - 1;
unsigned g_cap_bits = 0;  // the playback thread
bool g_cap_bad = false;

// ---- the images (the playback thread makes and uses them; the worker reads a capture's readback)
struct Img {
    ID3D12Resource* game_sc = nullptr;  // the keys (no reference held: prepass_arrays' own identity)
    ID3D12Resource* game_d = nullptr;
    D3D12_RESOURCE_DESC dsc{}, dd{};
    ID3D12Resource* sc2 = nullptr;      // eye 2's collector after pass 1's call (at rest COPY_DEST)
    ID3D12Resource* save_sc = nullptr;  // the game's collector and depth before IN1 (at rest COPY_DEST)
    ID3D12Resource* save_d = nullptr;
    ID3D12Resource* rb = nullptr;       // readback, kSlots footprints
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[kSlots]{};
    UINT rows[kSlots]{};
    UINT64 rowbytes[kSlots]{};
    UINT64 rb_size = 0;
};
Img g_img;
struct Retired {
    ID3D12Resource* r;
    uint64_t frame;
};
std::mutex g_retire_mutex;
std::vector<Retired> g_retired;
void retire(ID3D12Resource* r) {
    if (!r) return;
    std::lock_guard lock(g_retire_mutex);
    g_retired.push_back({r, state::presents.load()});
}

// the worker's job (written by the playback thread before kReady, read by the worker after kWait)
struct Job {
    ID3D12Resource* rb = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[kSlots]{};
    UINT rows[kSlots]{};
    UINT64 rowbytes[kSlots]{};
    UINT64 size = 0;
    bool cut = false;
    bool dec = false;  // (run 23) kSIn1 read after eye 2's decals ([Stereo] PrepassEye2Decals)
    bool have_h = false;                     // (run 22, where2) kSH taken this capture
    uint64_t h_draws = 0, h_binds = 0;       // draws and collector binds on the H2 thread between kSH and IN2
    uint64_t h_pre_draws = 0, h_pre_binds = 0;  // ... and from the H2 marker's play to kSH (the region's ending call among them)
} g_job;
ID3D12Fence* g_fence = nullptr;
std::atomic<UINT64> g_fence_value{0};
HANDLE g_fence_event = nullptr;
std::mutex g_setup_mutex;  // (the command thread's lazy setup)

// the compare's results (the worker writes)
Count g_c_frames, g_c_cut, g_c_cut_in_eq, g_c_in_eq, g_c_in_sc_diff, g_c_in_d_diff, g_c_out_exact, g_c_out_exact_in_eq, g_c_out_diff_in_eq, g_c_map_failed,
    g_c_timeout, g_c_d_hi_diff;
std::atomic<uint64_t> g_c_out_texels_max{0}, g_c_out_worst{0}, g_c_in_sc_texels_max{0}, g_c_in_d_texels_max{0}, g_c_out_texels_last{0};
// (run 21) kSS1's two compares, and where the last frame's inputs differed (the worker writes; test only)
Count g_c_s1_changed, g_c_game_vs_s1;
std::atomic<uint64_t> g_c_s1_texels_max{0}, g_c_gs1_texels_max{0};
std::atomic<uint32_t> g_w_box[4] = {}, g_w_first[2] = {}, g_w_bytes[3] = {};
// (run 22, where2) kSH's compares: the game's collector after the handover against slice 1 ("the handover's result") and
// against IN2 ("written between the handover and IN2"); the window's draws and collector binds on the H2 thread; collector
// binds on any other thread (the bind tap); the steps (render thread: marks; playback: taken, refused, lost)
std::atomic<bool> g_where2{false};
Count g_c_h_frames, g_c_h_vs_s1, g_c_h_vs_in2, g_h2_marks, g_h2_mark_failed, g_h2_taken, g_h2_refused, g_h2_lost, g_bind_same, g_bind_other;
std::atomic<uint64_t> g_c_h_s1_max{0}, g_c_h_in2_max{0}, g_c_h_draws_max{0}, g_c_h_binds_max{0}, g_c_h_binds_min{~0ull};
std::atomic<uint32_t> g_h_box[4] = {}, g_h_first[2] = {}, g_h_bytes[3] = {};  // the last kSH-vs-IN2 (else kSH-vs-slice 1) difference
std::atomic<DWORD> g_bind_other_tid{0}, g_h2_tid{0};
std::atomic<bool> g_h2_win{false};      // the playback thread: between kSH and IN2 (the tap counts the collector's binds there)
// (the review's must-fix 2, mitigated in these two files) kSH is read at the first list call after the one that ended the
// region, so that ending call itself falls before kSH: the draws and collector binds from the H2 marker's play to kSH are
// counted too, so a write there shows (as a draw or a bind) instead of passing for the handover's own result
std::atomic<bool> g_h2_pre{false};      // the playback thread: from the H2 marker's play to kSH
std::atomic<uint64_t> g_h2_pre_binds{0};
uint64_t g_h2_pre_draws0 = 0, g_cap_h_pre_draws = 0, g_cap_h_pre_binds = 0;
std::atomic<uint64_t> g_c_h_pre_draws_max{0}, g_c_h_pre_binds_max{0};
std::atomic<uint64_t> g_h2_binds{0};    // collector binds in that window (the H2 thread)
uint64_t g_h2_draws0 = 0;               // the H2 thread's draw count at kSH
uint64_t g_cap_h_draws = 0, g_cap_h_binds = 0;  // the playback thread: the window's counts, taken at IN2
int g_h2_defers = 0;                    // the playback thread: polls the pending H2 step waited for the region's end
void store_max(std::atomic<uint64_t>& a, uint64_t v) {
    uint64_t c = a.load(std::memory_order_relaxed);
    while (v > c && !a.compare_exchange_weak(c, v, std::memory_order_relaxed)) {
    }
}

// ---- the op queue: the markers' callbacks (the playback thread, in stream order) and the next list call's poll
// (run 23) kIn1CapD: a capture's IN1 whose kSIn1 is read later, by kDec1Cap after eye 2's decals ([Stereo] PrepassEye2Decals)
enum Op : uint8_t { kIn1, kIn1Cap, kOut1, kOut1Cap, kIn2Cap, kOut2Cap, kPrep, kH2Cap, kIn1CapD, kDec1Cap };
uint8_t g_ops[16];
int g_nops = 0;
DWORD g_ops_tid = 0;
bool only_h2() {  // (run 22, where2) the queue holds nothing but waiting H2 steps
    for (int i = 0; i < g_nops; ++i)
        if (g_ops[i] != kH2Cap) return false;
    return g_nops > 0;
}
void drop_h2();  // (below: counted as lost)
void push(Op o) {
    const DWORD tid = GetCurrentThreadId();
    const bool eye1 = o == kIn1 || o == kIn1Cap || o == kIn1CapD || o == kOut1 || o == kOut1Cap;  // (a dropped IN1 or OUT1 leaves eye 1 exposed)
    if (g_nops > 0 && tid != g_ops_tid && only_h2()) drop_h2();  // (run 22, where2: a waiting H2 step never blocks another step)
    if (g_nops > 0 && tid != g_ops_tid) {
        bump(g_ops_other_thread);  // (two playback threads: not expected; the step dropped)
        if (eye1) exposed("a step's marker played on another thread (dropped)");
        else stand_down("a step's marker played on another thread (dropped)");
        return;
    }
    if (g_nops >= static_cast<int>(sizeof(g_ops))) {
        bump(g_ops_overflow);
        if (eye1) exposed("the step queue full (a step dropped)");
        else stand_down("the step queue full (a step dropped)");
        return;
    }
    g_ops_tid = tid;
    g_ops[g_nops++] = o;
    prepass::g_eye2_pending_flag.store(true, std::memory_order_release);
}
void cb_in1(void*) { push(kIn1); }
void cb_in1_cap(void*) { push(kIn1Cap); }
void cb_in1_capd(void*) { push(kIn1CapD); }
void cb_dec1_cap(void*) { push(kDec1Cap); }
void cb_out1(void*) { push(kOut1); }
void cb_out1_cap(void*) { push(kOut1Cap); }
void cb_in2_cap(void*) { push(kIn2Cap); }
void cb_out2_cap(void*) { push(kOut2Cap); }
void cb_prep(void*) { push(kPrep); }
void h2_marked();  // (below)
void cb_h2_cap(void*) {  // (run 22, where2)
    push(kH2Cap);
    h2_marked();
}

void drop_h2() {  // (run 22, where2) the queue's waiting H2 steps dropped (only_h2() held), counted as lost
    for (int i = 0; i < g_nops; ++i) bump(g_h2_lost);
    g_nops = 0;
    g_h2_defers = 0;
    g_h2_pre.store(false, std::memory_order_relaxed);
    prepass::g_eye2_pending_flag.store(false, std::memory_order_relaxed);
}
void h2_marked() {  // (run 22, where2; the playback thread, at the H2 marker) the pre-window opens
    g_h2_pre_draws0 = d3d::thread_draws();
    g_h2_pre_binds.store(0, std::memory_order_relaxed);
    g_h2_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_h2_pre.store(true, std::memory_order_release);
}

// ---- the targets and their followed states (prepass_arrays: kind 0 the depth, 1 the collector)
constexpr int kKDepth = 0, kKCollector = 1;
struct Targets {
    ID3D12Resource *sc = nullptr, *d = nullptr, *asc = nullptr, *ad = nullptr;
    unsigned planes = 1;
    D3D12_RESOURCE_STATES ssc{}, sd[2]{};
};
bool g_owed = false;  // IN1 overwrote the game's targets: OUT1 must put them back

bool bad_state(D3D12_RESOURCE_STATES s) { return s == D3D12_RESOURCE_STATE_COPY_DEST || s == D3D12_RESOURCE_STATE_COPY_SOURCE; }
// (the review of d6ac220f) cl: the list the targets will be moved on; their followed states must have been set on it (a stale plane
// refused through the callers' own paths, counted per module); null (prep: no barrier) skips that check
const char* gather(Targets* t, ID3D12GraphicsCommandList* cl) {
    if (prepass::active()) return "a pre-pass region open";
    t->sc = prepass::kind_game(kKCollector);
    t->d = prepass::kind_game(kKDepth);
    t->asc = prepass::kind_array(kKCollector);
    t->ad = prepass::kind_array(kKDepth);
    t->planes = prepass::kind_planes(kKDepth);
    if (!t->sc || !t->d || !t->asc || !t->ad) return "the collector's or the depth's array not made";
    if (t->planes < 1 || t->planes > 2 || prepass::kind_planes(kKCollector) != 1) return "an unexpected plane count";
    if (!prepass::kind_followed(kKCollector, 0, &t->ssc)) return "the collector's state not followed (pass 1 not instanced, or another thread)";
    if (cl && !prepass::kind_followed_fresh(kKCollector, 0, cl, &t->ssc)) {
        prepass::follow_stale_note(prepass::kFollowE2sc);
        return "the collector's followed state was set on another list";
    }
    for (unsigned p = 0; p < t->planes; ++p) {
        if (!prepass::kind_followed(kKDepth, p, &t->sd[p])) return "the depth's state not followed";
        if (cl && !prepass::kind_followed_fresh(kKDepth, p, cl, &t->sd[p])) {
            prepass::follow_stale_note(prepass::kFollowE2sc);
            return "the depth's followed state was set on another list";
        }
    }
    if (bad_state(t->ssc) || bad_state(t->sd[0]) || (t->planes > 1 && bad_state(t->sd[1]))) return "a target already in a copy state";
    return nullptr;
}

D3D12_RESOURCE_BARRIER tr(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b, UINT sub) {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    x.Transition.Subresource = sub;
    return x;
}
struct Batch {
    D3D12_RESOURCE_BARRIER b[12];
    UINT n = 0;
    void add(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES s, UINT sub = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        if (a != s && n < 12) b[n++] = tr(r, a, s, sub);
    }
    void flush(ID3D12GraphicsCommandList* cl) {
        if (n) d3d::raw_barrier(cl, n, b);
        n = 0;
    }
};
// the depth's planes, each from its own state (the game moves them one at a time)
void add_depth(Batch& b, const Targets& t, ID3D12Resource* r, bool to, D3D12_RESOURCE_STATES copy) {
    for (unsigned p = 0; p < t.planes; ++p)
        if (to) b.add(r, t.sd[p], copy, t.planes > 1 ? p : D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
        else b.add(r, copy, t.sd[p], t.planes > 1 ? p : D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
}
void copy_sub(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, UINT dsub, ID3D12Resource* src, UINT ssub) {
    D3D12_TEXTURE_COPY_LOCATION d{dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    d.SubresourceIndex = dsub;
    D3D12_TEXTURE_COPY_LOCATION s{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    s.SubresourceIndex = ssub;
    d3d::raw_copy_region(cl, &d, &s);
}
void copy_rb(ID3D12GraphicsCommandList* cl, int slot, ID3D12Resource* src, UINT ssub) {
    D3D12_TEXTURE_COPY_LOCATION d{g_img.rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    d.PlacedFootprint = g_img.fp[slot];
    D3D12_TEXTURE_COPY_LOCATION s{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    s.SubresourceIndex = ssub;
    d3d::raw_copy_region(cl, &d, &s);
    g_cap_bits |= 1u << slot;
}

bool same_desc(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b) {
    return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format && a.DepthOrArraySize == b.DepthOrArraySize &&
           a.MipLevels == b.MipLevels && a.Flags == b.Flags && a.SampleDesc.Count == b.SampleDesc.Count;
}
// SC2, the saves and the readback for these targets (made again, the old ones retired, when the game's change)
const char* ensure(const Targets& t) {
    const D3D12_RESOURCE_DESC dsc = t.sc->GetDesc(), dd = t.d->GetDesc();
    const D3D12_RESOURCE_DESC asc = t.asc->GetDesc(), ad = t.ad->GetDesc();
    if (asc.Width != dsc.Width || asc.Height != dsc.Height || asc.DepthOrArraySize != 2 || ad.Width != dd.Width || ad.Height != dd.Height ||
        ad.DepthOrArraySize != 2 || dsc.MipLevels != 1 || dd.MipLevels != 1 || asc.MipLevels != 1 || ad.MipLevels != 1)
        return "an array's size differs from its game target's";
    if (g_img.sc2 && g_img.game_sc == t.sc && g_img.game_d == t.d && same_desc(g_img.dsc, dsc) && same_desc(g_img.dd, dd)) return nullptr;
    g_img_ready = false;
    if (g_cap.load() != kIdle && g_cap.load() != kArmed) g_cap_bad = true;  // (a capture in flight on the old images)
    retire(g_img.sc2);
    retire(g_img.save_sc);
    retire(g_img.save_d);
    retire(g_img.rb);
    g_img = Img{};
    ID3D12Device* dev = state::device.load();
    if (!dev) return "no device";
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC c = dsc, z = dd;
    c.Alignment = z.Alignment = 0;
    bool ok = SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &c, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_img.sc2)));
    ok = ok && SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &c, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_img.save_sc)));
    ok = ok && SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &z, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_img.save_d)));
    UINT64 off = 0;
    for (int s = 0; ok && s < kSlots; ++s) {
        const bool depth = s == kSD1 || s == kSD2;
        UINT64 total = 0;
        dev->GetCopyableFootprints(depth ? &dd : &dsc, 0, 1, off, &g_img.fp[s], &g_img.rows[s], &g_img.rowbytes[s], &total);
        off = (g_img.fp[s].Offset + total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~static_cast<UINT64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    }
    if (ok) {
        D3D12_HEAP_PROPERTIES rh{};
        rh.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = off;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ok = SUCCEEDED(dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_img.rb)));
        g_img.rb_size = off;
    }
    if (!ok) {
        bump(g_make_failed);
        retire(g_img.sc2);
        retire(g_img.save_sc);
        retire(g_img.save_d);
        retire(g_img.rb);
        g_img = Img{};
        return "SC2, the saves or the readback not made";
    }
    g_img.sc2->SetName(L"RDRVR eye 2 Shadow Collector (PrepassEye2Collector)");
    g_img.game_sc = t.sc;
    g_img.game_d = t.d;
    g_img.dsc = dsc;
    g_img.dd = dd;
    g_img_ready = true;
    bump(g_made);
    log::info("[e2sc] SC2, the saves and the readback made: the collector %llux%u (format %d), the depth %llux%u (format %d, %u planes)",
              static_cast<unsigned long long>(dsc.Width), dsc.Height, static_cast<int>(dsc.Format), static_cast<unsigned long long>(dd.Width), dd.Height,
              static_cast<int>(dd.Format), t.planes);
    return nullptr;
}

// ---- the steps (the playback thread, in the game's list, at the call after their marker)
// SC2 and the saves made ahead (a frame with no eye-2 call: the render thread runs one only once they exist)
void prep() {
    Targets t;
    const char* why = gather(&t, nullptr);
    if (!why) why = ensure(t);
    if (why) std::snprintf(g_why, sizeof(g_why), "prep: %s", why);
}
// IN1 always follows an eye-2 call in the stream: any refusal here leaves eye 1 exposed (MUST-1), so it stands down
void in1(ID3D12GraphicsCommandList* cl, bool cap, bool later_in = false) {
    if (cap) {
        g_cap_dec = later_in;  // (run 23: this capture's kSIn1 read after eye 2's decals)
        g_cap_bits = 0;
        g_cap_bad = false;
        g_cap_h_draws = g_cap_h_binds = g_cap_h_pre_draws = g_cap_h_pre_binds = 0;  // (run 22, where2: this capture's windows)
        g_h2_win.store(false, std::memory_order_relaxed);
        g_h2_pre.store(false, std::memory_order_relaxed);
    }
    if (g_owed) {  // (the last OUT1 never ran: that frame's eye 1 kept eye 2's targets)
        g_owed = false;
        bump(g_in1_refused);
        g_cap_bad = g_cap_bad || cap;
        exposed("IN1 found the last OUT1 not run");
        return;
    }
    if (d3d::thread_predicated()) {
        bump(g_in1_refused);
        g_cap_bad = g_cap_bad || cap;
        exposed("IN1: the list is predicated");
        return;
    }
    Targets t;
    const char* why = gather(&t, cl);
    if (!why) why = ensure(t);
    if (why) {
        bump(g_in1_refused);
        g_cap_bad = g_cap_bad || cap;
        char b[200];
        std::snprintf(b, sizeof(b), "IN1 refused after eye 2's call: %s", why);
        exposed(b);
        return;
    }
    const D3D12_RESOURCE_STATES rest_sc = prepass::kind_rest_state(kKCollector), rest_d = prepass::kind_rest_state(kKDepth);
    const UINT all = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Batch b;
    // the game's targets saved as they are
    b.add(t.sc, t.ssc, D3D12_RESOURCE_STATE_COPY_SOURCE, all);
    add_depth(b, t, t.d, true, D3D12_RESOURCE_STATE_COPY_SOURCE);
    b.flush(cl);
    copy_sub(cl, g_img.save_sc, 0, t.sc, 0);
    for (UINT p = 0; p < t.planes; ++p) copy_sub(cl, g_img.save_d, p, t.d, p);
    // eye 2's inputs: the arrays' slice 1 (mip + slice * mips + plane * mips * slices)
    b.add(t.sc, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST, all);
    for (UINT p = 0; p < t.planes; ++p)
        b.add(t.d, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST, t.planes > 1 ? p : all);
    b.add(t.asc, rest_sc, D3D12_RESOURCE_STATE_COPY_SOURCE, all);  // (the arrays at rest: prepass_arrays' write_state)
    b.add(t.ad, rest_d, D3D12_RESOURCE_STATE_COPY_SOURCE, all);
    b.flush(cl);
    copy_sub(cl, t.sc, 0, t.asc, 1);
    for (UINT p = 0; p < t.planes; ++p) copy_sub(cl, t.d, p, t.ad, 1 + p * 2);
    if (cap && g_img.rb) {
        if (!later_in) copy_rb(cl, kSIn1, t.asc, 1);  // ([Stereo] PrepassEye2Decals: kDec1Cap reads it after eye 2's decals)
        copy_rb(cl, kSD1, t.ad, 1);
    }
    b.add(t.asc, D3D12_RESOURCE_STATE_COPY_SOURCE, rest_sc, all);
    b.add(t.ad, D3D12_RESOURCE_STATE_COPY_SOURCE, rest_d, all);
    b.add(t.sc, D3D12_RESOURCE_STATE_COPY_DEST, t.ssc, all);
    add_depth(b, t, t.d, false, D3D12_RESOURCE_STATE_COPY_DEST);
    b.flush(cl);
    g_owed = true;
    bump(g_in1_ok);
}

// ([Stereo] PrepassEye2Decals, run 23) a capture's kSIn1: the game's collector after eye 2's decals (slice 1 and eye 2's
// decals, the input eye 2's term reads), at the first list call after their marker, before eye 2's term's own draws
void read_dec1(ID3D12GraphicsCommandList* cl) {
    const char* why = !g_owed ? "IN1 did not run" : d3d::thread_predicated() ? "the list is predicated" : nullptr;
    Targets t;
    if (!why) why = gather(&t, cl);
    if (!why && (!g_img.rb || g_img.game_sc != t.sc)) why = "the images are not this capture's";
    if (why) {
        bump(g_dec_read_refused);
        g_cap_bad = true;
        log::limited("e2sc.dec1", 10, "[e2sc] decals: the capture's input after eye 2's decals not read: %s", why);
        return;
    }
    const UINT all = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Batch b;
    b.add(t.sc, t.ssc, D3D12_RESOURCE_STATE_COPY_SOURCE, all);
    b.flush(cl);
    copy_rb(cl, kSIn1, t.sc, 0);
    b.add(t.sc, D3D12_RESOURCE_STATE_COPY_SOURCE, t.ssc, all);
    b.flush(cl);
}

void out1(ID3D12GraphicsCommandList* cl, bool cap) {
    if (!g_owed) {
        bump(g_out1_none);  // (IN1 refused: the game's targets untouched)
        if (cap) g_cap_bad = true;
        return;
    }
    g_owed = false;
    Targets t;
    const char* why = gather(&t, cl);  // (no guessed StateBefore: the follow lost since IN1 stands down without a copy)
    if (!why && (!g_img.save_sc || g_img.game_sc != t.sc || g_img.game_d != t.d)) why = "the images changed since IN1";
    if (!why && d3d::thread_predicated()) why = "the list is predicated";
    if (why) {
        bump(g_out1_fatal);
        if (cap) g_cap_bad = true;
        char b[200];
        std::snprintf(b, sizeof(b), "OUT1 could not put eye 1's collector and depth back: %s", why);
        exposed(b);
        return;
    }
    // ([Stereo] PrepassEye2Light, test only) eye 2's directional term and its depth (the interior branch's stencil marks)
    // copied into eye2_light's SC2L and D2 from the followed states and left in them; returns at once when it is off
    eye2_light::from_collector(cl, t.sc, t.d, t.planes, t.ssc, t.sd);
    const UINT all = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Batch b;
    b.add(t.sc, t.ssc, D3D12_RESOURCE_STATE_COPY_SOURCE, all);
    add_depth(b, t, t.d, true, D3D12_RESOURCE_STATE_COPY_DEST);
    b.add(g_img.save_sc, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE, all);
    b.add(g_img.save_d, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE, all);
    b.flush(cl);
    copy_sub(cl, g_img.sc2, 0, t.sc, 0);  // eye 2's directional term, kept
    if (cap && g_img.rb) copy_rb(cl, kSOut1, t.sc, 0);
    b.add(t.sc, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST, all);
    b.flush(cl);
    copy_sub(cl, t.sc, 0, g_img.save_sc, 0);  // eye 1's back
    for (UINT p = 0; p < t.planes; ++p) copy_sub(cl, t.d, p, g_img.save_d, p);
    b.add(t.sc, D3D12_RESOURCE_STATE_COPY_DEST, t.ssc, all);
    add_depth(b, t, t.d, false, D3D12_RESOURCE_STATE_COPY_DEST);
    b.add(g_img.save_sc, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST, all);
    b.add(g_img.save_d, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST, all);
    b.flush(cl);
    bump(g_out1_ok);
}

// (run 22, where2) the game's collector to readback at the first list call after a cut pass 2's region ended (the H2
// step waits in the queue while the region is open: poll); the window to IN2 opens here
bool read_h2(ID3D12GraphicsCommandList* cl) {
    if (g_h2_pre.exchange(false, std::memory_order_acq_rel)) {  // (the pre-window closes: the marker's play to here)
        g_cap_h_pre_draws = d3d::thread_draws() - g_h2_pre_draws0;
        g_cap_h_pre_binds = g_h2_pre_binds.load(std::memory_order_relaxed);
    }
    if (d3d::thread_predicated()) {
        bump(g_h2_refused);
        return false;
    }
    Targets t;
    const char* why = gather(&t, cl);
    if (!why && (!g_img.rb || g_img.game_sc != t.sc || g_img.game_d != t.d)) why = "the images are not this capture's";
    if (why) {
        bump(g_h2_refused);
        log::limited("e2sc.h2", 10, "[e2sc] where2: the step after the handover refused: %s", why);
        return false;
    }
    const UINT all = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Batch b;
    b.add(t.sc, t.ssc, D3D12_RESOURCE_STATE_COPY_SOURCE, all);
    b.flush(cl);
    copy_rb(cl, kSH, t.sc, 0);
    b.add(t.sc, D3D12_RESOURCE_STATE_COPY_SOURCE, t.ssc, all);
    b.flush(cl);
    g_h2_draws0 = d3d::thread_draws();
    g_h2_binds.store(0, std::memory_order_relaxed);
    g_h2_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_h2_win.store(true, std::memory_order_release);
    bump(g_h2_taken);
    return true;
}

// pass 2: the game's collector (and depth plane 0) to readback, as they are
bool read_pass2(ID3D12GraphicsCommandList* cl, bool with_depth, Count& refused) {
    if (with_depth && g_h2_win.exchange(false, std::memory_order_acq_rel)) {  // (run 22, where2) the window kSH..IN2 closes
        g_cap_h_draws = d3d::thread_draws() - g_h2_draws0;
        g_cap_h_binds = g_h2_binds.load(std::memory_order_relaxed);
    }
    if (d3d::thread_predicated()) {
        refuse(refused, "pass 2: the list is predicated");
        return false;
    }
    Targets t;
    const char* why = gather(&t, cl);
    if (!why && (!g_img.rb || g_img.game_sc != t.sc || g_img.game_d != t.d)) why = "pass 2: the images are not this capture's";
    if (why) {
        refuse(refused, why);
        return false;
    }
    const UINT all = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    const D3D12_RESOURCE_STATES rest_sc = prepass::kind_rest_state(kKCollector);  // (the array at rest: prepass_arrays' write_state)
    Batch b;
    b.add(t.sc, t.ssc, D3D12_RESOURCE_STATE_COPY_SOURCE, all);
    if (with_depth) b.add(t.d, t.sd[0], D3D12_RESOURCE_STATE_COPY_SOURCE, t.planes > 1 ? 0 : all);
    if (with_depth) b.add(t.asc, rest_sc, D3D12_RESOURCE_STATE_COPY_SOURCE, all);  // (run 21: kSS1)
    b.flush(cl);
    if (with_depth) {
        copy_rb(cl, kSIn2, t.sc, 0);
        copy_rb(cl, kSD2, t.d, 0);
        copy_rb(cl, kSS1, t.asc, 1);  // (slice 1, mip 0)
    } else {
        copy_rb(cl, kSOut2, t.sc, 0);
    }
    b.add(t.sc, D3D12_RESOURCE_STATE_COPY_SOURCE, t.ssc, all);
    if (with_depth) b.add(t.d, D3D12_RESOURCE_STATE_COPY_SOURCE, t.sd[0], t.planes > 1 ? 0 : all);
    if (with_depth) b.add(t.asc, D3D12_RESOURCE_STATE_COPY_SOURCE, rest_sc, all);
    b.flush(cl);
    return true;
}

void out2_done() {
    if ((g_cap_bits & kAllSlots) == kAllSlots && !g_cap_bad && g_img.rb) {  // (run 22: kSH, optional, outside kAllSlots)
        g_job.rb = g_img.rb;
        std::memcpy(g_job.fp, g_img.fp, sizeof(g_job.fp));
        std::memcpy(g_job.rows, g_img.rows, sizeof(g_job.rows));
        std::memcpy(g_job.rowbytes, g_img.rowbytes, sizeof(g_job.rowbytes));
        g_job.size = g_img.rb_size;
        g_job.cut = g_cap_cut.load();
        g_job.dec = g_cap_dec;
        g_job.have_h = (g_cap_bits & (1u << kSH)) != 0;  // (run 22, where2)
        g_job.h_draws = g_cap_h_draws;
        g_job.h_binds = g_cap_h_binds;
        g_job.h_pre_draws = g_cap_h_pre_draws;
        g_job.h_pre_binds = g_cap_h_pre_binds;
        g_ready_frame.store(state::presents.load());
        int s = kRec2;
        if (!g_cap.compare_exchange_strong(s, kReady)) bump(g_cap_incomplete);
        return;
    }
    bump(g_cap_incomplete);
    int s = kRec2;
    g_cap.compare_exchange_strong(s, kArmed);
}

// ---- the compare (the worker thread)
struct Diff {
    uint64_t texels = 0, worst = 0, hi = 0;
    // (run 21) where: the bounding box of the differing texels and the first one's bytes in both images
    UINT x0 = ~0u, y0 = ~0u, x1 = 0, y1 = 0, fx = 0, fy = 0;
    uint8_t fa[4] = {}, fb[4] = {};
    void at(UINT x, UINT y, const uint8_t* ta, const uint8_t* tb, UINT bpt) {
        if (!texels) {
            fx = x;
            fy = y;
            for (UINT k = 0; k < bpt && k < 4; ++k) {
                fa[k] = ta[k];
                fb[k] = tb[k];
            }
        }
        if (x < x0) x0 = x;
        if (y < y0) y0 = y;
        if (x > x1) x1 = x;
        if (y > y1) y1 = y;
    }
};
Diff diff(const uint8_t* base, const Job& j, int a, int b, bool depth) {
    Diff d;
    const UINT w = j.fp[a].Footprint.Width, h = j.rows[a];
    const UINT64 rb = j.rowbytes[a];
    const UINT bpt = w ? static_cast<UINT>(rb / w) : 0;
    if (!bpt || j.fp[a].Footprint.Width != j.fp[b].Footprint.Width || j.rows[a] != j.rows[b]) {
        d.texels = ~0ull;
        return d;
    }
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* pa = base + j.fp[a].Offset + static_cast<UINT64>(y) * j.fp[a].Footprint.RowPitch;
        const uint8_t* pb = base + j.fp[b].Offset + static_cast<UINT64>(y) * j.fp[b].Footprint.RowPitch;
        if (!std::memcmp(pa, pb, static_cast<size_t>(rb))) continue;
        for (UINT x = 0; x < w; ++x) {
            const uint8_t* ta = pa + static_cast<size_t>(x) * bpt;
            const uint8_t* tb = pb + static_cast<size_t>(x) * bpt;
            if (!std::memcmp(ta, tb, bpt)) continue;
            if (depth && bpt == 4) {  // (plane 0 of D24S8: depth in the low 24 bits, the high byte undefined)
                uint32_t va = 0, vb = 0;
                std::memcpy(&va, ta, 4);
                std::memcpy(&vb, tb, 4);
                if ((va & 0x00ffffffu) == (vb & 0x00ffffffu)) {
                    ++d.hi;
                    continue;
                }
                const uint32_t da = va & 0x00ffffffu, db = vb & 0x00ffffffu;
                const uint64_t dd = da > db ? da - db : db - da;
                if (dd > d.worst) d.worst = dd;
                d.at(x, y, ta, tb, bpt);
                ++d.texels;
                continue;
            }
            d.at(x, y, ta, tb, bpt);
            ++d.texels;
            for (UINT k = 0; k < bpt; ++k) {
                const uint64_t dd = ta[k] > tb[k] ? ta[k] - tb[k] : tb[k] - ta[k];
                if (dd > d.worst) d.worst = dd;
            }
        }
    }
    return d;
}
void compare_job() {
    const Job& j = g_job;
    uint8_t* p = nullptr;
    D3D12_RANGE r{0, static_cast<SIZE_T>(j.size)};
    if (!j.rb || FAILED(j.rb->Map(0, &r, reinterpret_cast<void**>(&p))) || !p) {
        bump(g_c_map_failed);
        return;
    }
    const Diff in_sc = diff(p, j, kSIn1, kSIn2, false), in_d = diff(p, j, kSD1, kSD2, true), out = diff(p, j, kSOut1, kSOut2, false);
    // (run 21) slice 1 at IN2 against IN1 (did slice 1 itself change between the passes?) and against IN2 (is the game's
    // collector slice 1's copy at IN2?); the first IN difference's texel in all three images
    const Diff s1 = diff(p, j, kSIn1, kSS1, false), gs1 = diff(p, j, kSS1, kSIn2, false);
    // (run 22, where2) kSH, the game's collector just after the handover: against slice 1 and against IN2
    Diff hs, hi;
    uint32_t h_in2_at = 0, h_s1_at = 0;
    if (j.have_h) {
        hs = diff(p, j, kSH, kSS1, false);
        hi = diff(p, j, kSH, kSIn2, false);
        const Diff& w = hi.texels && hi.texels != ~0ull ? hi : hs;
        auto texel = [&](int slot, uint32_t* dst) {  // (the shown difference's first texel in another collector image)
            const UINT ww = j.fp[slot].Footprint.Width, bpt = ww ? static_cast<UINT>(j.rowbytes[slot] / ww) : 0;
            if (w.texels && w.texels != ~0ull && bpt == 4 && w.fx < ww && w.fy < j.rows[slot])
                std::memcpy(dst, p + j.fp[slot].Offset + static_cast<UINT64>(w.fy) * j.fp[slot].Footprint.RowPitch + static_cast<UINT64>(w.fx) * 4, 4);
        };
        texel(kSIn2, &h_in2_at);
        texel(kSS1, &h_s1_at);
    }
    uint32_t s1_at = 0;
    if (in_sc.texels && in_sc.texels != ~0ull) {
        const UINT w = j.fp[kSS1].Footprint.Width, bpt = w ? static_cast<UINT>(j.rowbytes[kSS1] / w) : 0;
        if (bpt == 4 && in_sc.fx < w && in_sc.fy < j.rows[kSS1])
            std::memcpy(&s1_at, p + j.fp[kSS1].Offset + static_cast<UINT64>(in_sc.fy) * j.fp[kSS1].Footprint.RowPitch + static_cast<UINT64>(in_sc.fx) * 4, 4);
    }
    D3D12_RANGE none{0, 0};
    j.rb->Unmap(0, &none);
    if (j.have_h) {  // (run 22, where2)
        bump(g_c_h_frames);
        if (hs.texels) bump(g_c_h_vs_s1);
        if (hi.texels) bump(g_c_h_vs_in2);
        store_max(g_c_h_s1_max, hs.texels);
        store_max(g_c_h_in2_max, hi.texels);
        store_max(g_c_h_draws_max, j.h_draws);
        store_max(g_c_h_binds_max, j.h_binds);
        store_max(g_c_h_pre_draws_max, j.h_pre_draws);
        store_max(g_c_h_pre_binds_max, j.h_pre_binds);
        uint64_t mn = g_c_h_binds_min.load(std::memory_order_relaxed);
        while (j.h_binds < mn && !g_c_h_binds_min.compare_exchange_weak(mn, j.h_binds, std::memory_order_relaxed)) {
        }
        const Diff& w = hi.texels && hi.texels != ~0ull ? hi : hs;
        uint32_t a = 0;  // (w is kSH against IN2, or else kSH against slice 1: its fa is kSH's bytes)
        if (w.texels && w.texels != ~0ull) {
            std::memcpy(&a, w.fa, 4);
            g_h_box[0] = w.x0;
            g_h_box[1] = w.y0;
            g_h_box[2] = w.x1;
            g_h_box[3] = w.y1;
            g_h_first[0] = w.fx;
            g_h_first[1] = w.fy;
            g_h_bytes[0] = a;         // after the handover (kSH)
            g_h_bytes[1] = h_in2_at;  // IN2
            g_h_bytes[2] = h_s1_at;   // slice 1 at IN2
        }
        log::limited("e2sc.where2", 20, "[e2sc] where2 (%s frame): the game's collector after the handover against slice 1 %llu texels, against "
                     "IN2 %llu texels; before it (from its marker) draws %llu, collector binds %llu; the window to IN2: draws %llu, collector binds "
                     "%llu (this thread); %s at (%u,%u): after the handover %08x, IN2 %08x, slice 1 %08x", j.cut ? "a cut" : "a not cut",
                     static_cast<unsigned long long>(hs.texels), static_cast<unsigned long long>(hi.texels),
                     static_cast<unsigned long long>(j.h_pre_draws), static_cast<unsigned long long>(j.h_pre_binds),
                     static_cast<unsigned long long>(j.h_draws), static_cast<unsigned long long>(j.h_binds),
                     hi.texels && hi.texels != ~0ull ? "changed since the handover" : hs.texels ? "not slice 1 already" : "no difference",
                     w.fx, w.fy, a, h_in2_at, h_s1_at);
    }
    if (s1.texels) bump(g_c_s1_changed);
    if (gs1.texels) bump(g_c_game_vs_s1);
    store_max(g_c_s1_texels_max, s1.texels);
    store_max(g_c_gs1_texels_max, gs1.texels);
    if (in_sc.texels && in_sc.texels != ~0ull) {
        g_w_box[0] = in_sc.x0;
        g_w_box[1] = in_sc.y0;
        g_w_box[2] = in_sc.x1;
        g_w_box[3] = in_sc.y1;
        g_w_first[0] = in_sc.fx;
        g_w_first[1] = in_sc.fy;
        uint32_t a = 0, b = 0;
        std::memcpy(&a, in_sc.fa, 4);
        std::memcpy(&b, in_sc.fb, 4);
        g_w_bytes[0] = a;  // IN1 (pass 1's slice 1)
        g_w_bytes[1] = b;  // IN2 (pass 2's collector)
        g_w_bytes[2] = s1_at;  // slice 1 at IN2
        log::limited("e2sc.where", 20, "[e2sc] where (%s frame): inputs differ in the box (%u,%u)-(%u,%u); the first at (%u,%u): IN1 %08x, IN2 %08x, "
                     "slice 1 at IN2 %08x (BGRA bytes, little-endian) | slice 1 changed since IN1 %llu texels, the game's collector against slice 1 "
                     "at IN2 %llu texels", j.cut ? "a cut" : "a not cut", in_sc.x0, in_sc.y0, in_sc.x1, in_sc.y1, in_sc.fx, in_sc.fy, a, b, s1_at,
                     static_cast<unsigned long long>(s1.texels), static_cast<unsigned long long>(gs1.texels));
    }
    bump(g_c_frames);
    if (j.cut) bump(g_c_cut);
    if (j.dec) bump(g_c_dec_frames);  // (run 23: "slice 1 changed since IN1" then measures eye 2's decals)
    const bool in_eq = in_sc.texels == 0 && in_d.texels == 0;
    if (in_eq) bump(g_c_in_eq);
    if (in_eq && j.cut) bump(g_c_cut_in_eq);
    if (in_sc.texels) bump(g_c_in_sc_diff);
    if (in_d.texels) bump(g_c_in_d_diff);
    if (in_d.hi) bump(g_c_d_hi_diff);
    if (out.texels == 0) bump(g_c_out_exact);
    if (out.texels == 0 && in_eq) bump(g_c_out_exact_in_eq);
    if (out.texels != 0 && in_eq) bump(g_c_out_diff_in_eq);
    store_max(g_c_out_texels_max, out.texels);
    store_max(g_c_out_worst, out.worst);
    store_max(g_c_in_sc_texels_max, in_sc.texels);
    store_max(g_c_in_d_texels_max, in_d.texels);
    g_c_out_texels_last.store(out.texels);
    log::limited("e2sc.cmp", 40, "[e2sc] compared (%s frame): inputs collector %llu texels differ, depth %llu (worst %llu); eye 2's term %llu texels "
                 "differ (worst %llu)", j.cut ? "a cut" : "a not cut", static_cast<unsigned long long>(in_sc.texels),
                 static_cast<unsigned long long>(in_d.texels), static_cast<unsigned long long>(in_d.worst), static_cast<unsigned long long>(out.texels),
                 static_cast<unsigned long long>(out.worst));
}
DWORD WINAPI worker(void*) {
    int slow = 0;
    for (;;) {
        const bool signalled = WaitForSingleObject(g_fence_event, 5000) == WAIT_OBJECT_0;
        if (g_cap.load() != kWait) continue;
        // (the event may have fired before kWait was seen, or been missed: the fence's value decides, not the wake-up)
        if (!g_fence || g_fence->GetCompletedValue() < g_fence_value.load()) {
            int s = kWait;
            if (!signalled && ++slow >= 6 && g_cap.compare_exchange_strong(s, kArmed)) {
                bump(g_c_timeout);  // (30 s: re-armed, the readback never came)
                slow = 0;
            }
            continue;
        }
        slow = 0;
        compare_job();
        bump(g_cap_done);
        const int left = g_cap_left.fetch_sub(1) - 1;
        g_cap.store(left > 0 ? kArmed : kIdle);
    }
}

// ---- the frame end (the presenting thread)
void frame_end(uint64_t f) {
    {
        std::lock_guard lock(g_retire_mutex);
        const int cs = g_cap.load();
        if (cs == kIdle || cs == kArmed)  // (never while a capture's readback may still be read)
            for (size_t i = 0; i < g_retired.size();)
                if (g_retired[i].frame + 8 < f) {
                    g_retired[i].r->Release();
                    g_retired.erase(g_retired.begin() + static_cast<ptrdiff_t>(i));
                } else {
                    ++i;
                }
    }
    int s = g_cap.load();
    if ((s == kRec1 || s == kRec2) && f > g_cap_since.load() + 8) {
        if (g_cap.compare_exchange_strong(s, kArmed)) bump(g_cap_incomplete);  // (a pass 2 that never came: a mono frame)
        return;
    }
    if (s == kReady && f >= g_ready_frame.load() + 2) {
        ID3D12CommandQueue* q = state::present_queue.load();
        if (!q || !g_fence || !g_fence_event) return;
        const UINT64 v = g_fence_value.load() + 1;
        g_fence_value.store(v);
        g_cap.store(kWait);  // (before the event can fire: the worker checks the state first)
        q->Signal(g_fence, v);
        g_fence->SetEventOnCompletion(v, g_fence_event);
    }
}

// ---- the hook (the render thread)
constexpr int kRsIds = 0x2c;  // RenderStateSet's ids (its cmp ecx, 0x2b at 0x140147127)
using RsSet_t = void (*)(int id, uint32_t value);
using BindVd_t = void (*)(uintptr_t ctx, uintptr_t decl);
// FUN_140ec67a0's first 40 bytes [V]: test rdx; id = decl ? [rdx+0x10] : 0; cmp [rcx+0x1a8]; [rcx+0x1a8] = id; [rcx+0x1a0] =
// decl; byte [rcx+0x124] = 1 (then the recorder's vt+0xb8): the offsets it binds, proved by the bytes
constexpr uintptr_t kRvaBindVd = 0xec67a0;
const uint8_t kBindVdEntry[40] = {0x48, 0x85, 0xd2, 0x74, 0x05, 0x8b, 0x42, 0x10, 0xeb, 0x02, 0x33, 0xc0, 0x39, 0x81, 0xa8, 0x01, 0x00, 0x00, 0x74, 0x25,
                                  0x89, 0x81, 0xa8, 0x01, 0x00, 0x00, 0x48, 0x89, 0x91, 0xa0, 0x01, 0x00, 0x00, 0xc6, 0x81, 0x24, 0x01, 0x00, 0x00, 0x01};
bool raw_read(uintptr_t a, void* out, size_t n) {
    if (!a) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// ([Stereo] PrepassEye2Decals, run 23, research\run23\decals.md section 10) eye 2's collector decals with vp2 current, as
// 0x5cbfd0 draws them (0x1405cc3cd..0x1405cc403): RenderStateSet(6, 0), DecalMgr vt+0x70, state 6 back; then any other
// state the decals moved set back to the hook's (expected none). Under the caller's SEH; no C++ objects. Skipped (counted)
// when the manager or its vt+0x70 is not as analysed, or the immediate ring is near its end
// (the review's should-fix 1) 0x1405e0dc0 enters the manager's critical section (mgr +0x3040) and leaves it only on its
// normal exit: a fault inside the lists would keep it on this thread and block the build (vt+0x58) and the main thread's
// adds for good, so the call has its own SEH and a fault leaves the section while this thread owns it (then stand-down)
// (the delta review) as eye2_images' call_alloc: this thread's recursion count noted before the call, and after a fault the
// section left only down to it (the levels taken during the call), never a hold an outer frame of this thread had
bool decal_call(uintptr_t fn, uintptr_t mgr) {
    auto* cs = reinterpret_cast<CRITICAL_SECTION*>(mgr + 0x3040);
    const uintptr_t self = static_cast<uintptr_t>(GetCurrentThreadId());
    LONG before = 0;
    __try {
        if (reinterpret_cast<uintptr_t>(cs->OwningThread) == self) before = cs->RecursionCount;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        bump(g_dec_fault);
        return false;  // (the manager not readable: not called)
    }
    __try {
        reinterpret_cast<void (*)(uintptr_t)>(fn)(mgr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        __try {
            for (int i = 0; i < 64 && reinterpret_cast<uintptr_t>(cs->OwningThread) == self && cs->RecursionCount > before; ++i)
                LeaveCriticalSection(cs);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        bump(g_dec_fault);
        return false;
    }
}
bool decals_eye2(const uint32_t* saved, bool have_rs, uint64_t* restored) {  // (false: a fault in the decals' call)
    uintptr_t mgr = 0, vt = 0, fn = 0;
    if (!rd(anchors::addr(Id::DecalMgr), &mgr) || !mgr || !rd(mgr, &vt) || !vt || !rd(vt + 0x70, &fn) || fn != anchors::addr(Id::DecalDraw)) {
        bump(g_dec_no_mgr);
        return true;
    }
    uint32_t ring = 0;
    if (!rd(anchors::addr(Id::ImmRingCursor), &ring) || ring >= kRingMargin) {
        bump(g_dec_ring);
        return true;
    }
    const RsSet_t rs_set = reinterpret_cast<RsSet_t>(anchors::addr(Id::RenderStateSet));
    uint32_t s6 = 0;
    const bool have6 = rd(g_rs_cache + 6 * 4, &s6);
    rs_set(6, 0);
    const bool called = decal_call(fn, mgr);
    g_dec_drawn = called;  // (item 7's S2: not dec_no_mgr, dec_ring or dec_no_tls skipped, no fault)
    if (have6) rs_set(6, s6);
    if (have_rs) {
        for (int id = 0; id < kRsIds; ++id) {
            uint32_t now = 0;
            if (!rd(g_rs_cache + static_cast<uintptr_t>(id) * 4, &now) || now == saved[id]) continue;
            rs_set(id, saved[id]);
            ++*restored;
            bump(g_dec_states);
        }
    }
    bump(g_dec_calls);
    return called;
}
// eye 2's call between two SetCurrent calls, under SEH (no C++ objects in this frame). dec: eye 2's decals first; cap: their
// capture marker (kDec1Cap) after them, before eye 2's term
bool run_eye2(void* lighting, uintptr_t vp2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7, uint64_t* restored, bool dec,
              bool cap) {
    uintptr_t cur = 0;
    if (!rd(anchors::addr(Id::ViewportCurrent), &cur) || !cur) return false;
    uint32_t ref = 0;
    const uintptr_t ref_at = anchors::addr(Id::LightStencilRef);
    const bool have_ref = rd(ref_at, &ref);
    const SetCurrent_t set_current = reinterpret_cast<SetCurrent_t>(anchors::addr(Id::ViewportSetCurrent));
    // (should-fix 2) the render-state cache before eye 2's call: 0x6e58a0 pops what it pushed but sets states 0x14-0x1e to their
    // defaults at its end; each entry that differs after it is set back through RenderStateSet (the cache and its effect)
    uint32_t saved[kRsIds] = {};
    const bool have_rs = raw_read(g_rs_cache, saved, sizeof(saved));
    // (the review's optional item) the recorder context (TLS slot 0 +0x28)'s binding at +0x1a0 (pointer) / +0x1a8 (its id),
    // which 0x6e58a0's draws set through FUN_140ec67a0 (0x6dd4d0 binds DAT_142c443e0's): put back through the same setter
    // (run 21, a must-fix found in run 20's EC log: two first-chance reads of 0x28 here) the context as the game and
    // prepass::thread_stream_ok read it: TEB +0x58 is the TLS array, its slot 0 the block (+0x38 its lazy-init byte), the
    // block +0x28 the context. The read was one level too deep (the block's first qword taken as the block), so the
    // "context" was not the recorder's: a wrong one whose +0x1a0 changed would have been written by FUN_140ec67a0
    uintptr_t block = 0, ctx = 0, vd = 0;
    unsigned char init = 0;
    const uintptr_t tls = static_cast<uintptr_t>(__readgsqword(0x58));
    const bool have_vd = g_bind_vd && rd(tls, &block) && block && rd(block + 0x38, &init) && init && rd(block + 0x28, &ctx) && ctx &&
                         rd(ctx + 0x1a0, &vd);
    if (g_bind_vd && !have_vd) bump(g_no_ctx);
    // ([Stereo] PrepassEye2Decals) the TLS block's +0x174 (the gravestone text's per-thread value, which nothing puts back):
    // put back after eye 2's term, so eye 1's call and pass 2 run with the game's
    uint32_t tls174 = 0;
    uintptr_t dblock = 0;
    unsigned char dinit = 0;
    const bool have_174 = dec && rd(tls, &dblock) && dblock && rd(dblock + 0x38, &dinit) && dinit && rd(dblock + 0x174, &tls174);
    if (dec && !have_174) {
        bump(g_dec_no_tls);
        dec = false;  // (not drawn: nothing to put back)
    }
    bool ok = true;
    __try {
        set_current(vp2, 1, 0, 0);
        if (dec && !decals_eye2(saved, have_rs, restored)) ok = false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    // (the review's should-fix 3) the capture's marker outside the SEH blocks; then eye 2's term, skipped after a fault (IN1
    // is in the stream and OUT1 follows whatever happened; the caller stands down on !ok)
    if (cap && !prepass::stream_marker(&cb_dec1_cap)) bump(g_dec_marker_failed);  // (the capture then lacks kSIn1: incomplete)
    __try {
        if (ok) o_dir(lighting, reinterpret_cast<void*>(vp2), a3, a4, a5, a6, a7);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (have_174 && !wr(dblock + 0x174, tls174)) ok = false;
    __try {
        set_current(cur, 1, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (have_ref) wr(ref_at, ref);  // (redundant: 0x6da060 zeroes it before the branch reads it; kept, it costs nothing)
    if (have_rs) {
        const RsSet_t rs_set = reinterpret_cast<RsSet_t>(anchors::addr(Id::RenderStateSet));
        for (int id = 0; id < kRsIds; ++id) {
            uint32_t now = 0;
            if (!rd(g_rs_cache + static_cast<uintptr_t>(id) * 4, &now) || now == saved[id]) continue;
            __try {
                rs_set(id, saved[id]);
                ++*restored;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
        }
    } else {
        ok = false;  // (the cache not read: eye 1 would start from eye 2's states)
    }
    uintptr_t vd_now = 0;
    if (have_vd && rd(ctx + 0x1a0, &vd_now) && vd_now != vd) {
        __try {
            reinterpret_cast<BindVd_t>(g_bind_vd)(ctx, vd);  // (it compares the ids and re-emits only on a change)
            ++*restored;
            bump(g_vd_rebinds);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    return ok;
}

void pass1(void* lighting, void* vp, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7) {
    bump(g_p1_calls);
    ++g_pass1_seq;
    g_dec_drawn = false;  // (item 7's S2: set again by this run's decals only)
    void* vp2 = camera_lever::eye2_viewport();
    if (!vp2) {
        bump(g_no_vp2);
        return;
    }
    if (vp != camera_lever::scene_viewport()) {
        bump(g_not_scene);
        return;
    }
    if (!prepass::thread_stream_ok()) {
        bump(g_no_stream);
        return;
    }
    // (MUST-1) what predicts IN1's success, before anything is recorded: the walk cut's follow armed, pass 1's walk
    // instanced and ready this frame (its walk-start marker said so), SC2 and the saves made
    if (!prepass::cut_tracking()) {
        bump(g_no_follow);
        return;
    }
    if (!prepass::on() || !prepass::instanced_on()) {  // ("prepass off", "prepass inst off", or prepass_arrays stood down)
        bump(g_prepass_off);
        return;
    }
    if (!prepass::pass1_walk_ready()) {
        bump(g_not_ready);
        return;
    }
    if (!g_img_ready.load()) {
        if (prepass::stream_marker(&cb_prep)) bump(g_preps);  // (made at playback; eye 2's call from a later frame)
        return;
    }
    int s = kArmed;
    const bool cap = g_cap.compare_exchange_strong(s, kRec1);
    if (cap) {
        g_cap_since.store(state::presents.load());
        g_cap_seq = g_pass1_seq;
        bump(g_cap_started);
    }
    const bool dec = g_decals && g_decals_on.load(std::memory_order_relaxed);  // ([Stereo] PrepassEye2Decals)
    if (!prepass::stream_marker(cap ? (dec ? &cb_in1_capd : &cb_in1_cap) : &cb_in1)) {
        bump(g_no_stream);
        if (cap) g_cap.store(kArmed);
        return;
    }
    LARGE_INTEGER t0{}, t1{};
    QueryPerformanceCounter(&t0);
    uint64_t restored = 0;
    const bool ok = run_eye2(lighting, reinterpret_cast<uintptr_t>(vp2), a3, a4, a5, a6, a7, &restored, dec, cap && dec);
    g_states_restored.fetch_add(restored, std::memory_order_relaxed);
    // (OUT1 whatever happened: IN1 is in the stream, so eye 1's targets must be put back)
    const bool out = prepass::stream_marker(cap ? &cb_out1_cap : &cb_out1);
    QueryPerformanceCounter(&t1);
    g_run_ticks.fetch_add(static_cast<uint64_t>(t1.QuadPart - t0.QuadPart), std::memory_order_relaxed);
    bump(g_runs);
    g_runs_total.fetch_add(1, std::memory_order_relaxed);
    if (!out) {
        bump(g_out_marker_failed);
        exposed("OUT1's marker not appended after eye 2's call");
    }
    if (!ok) {
        bump(g_fault);
        stand_down("a fault in eye 2's call (ViewportSetCurrent, FUN_1406e58a0 on vp2 or the render states put back)");
    }
}

void hk(void* lighting, void* vp, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7) {
    if (!g_on.load(std::memory_order_relaxed) || reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_ret)
        return o_dir(lighting, vp, a3, a4, a5, a6, a7);
    const int pass = dual_pass::pass();
    if (pass == 1) {
        pass1(lighting, vp, a3, a4, a5, a6, a7);
        return o_dir(lighting, vp, a3, a4, a5, a6, a7);
    }
    if (pass == 2) {
        bump(g_p2_calls);
        int s = kRec1;
        if (g_cap.load() == kRec1 && g_cap_seq != g_pass1_seq) {  // (the capture's pass 1 was an earlier frame's: no pass 2 came)
            if (g_cap.compare_exchange_strong(s, kArmed)) {
                bump(g_p2_other_frame);
                bump(g_cap_incomplete);
            }
            return o_dir(lighting, vp, a3, a4, a5, a6, a7);
        }
        s = kRec1;
        if (!g_cap.compare_exchange_strong(s, kRec2))  // (item 7's S2, [Stereo] Pass2FrontSkip: in a record-skipped stretch when this pass 2 skips)
            return p2skip::around(p2skip::kDir, [&] { o_dir(lighting, vp, a3, a4, a5, a6, a7); });
        g_cap_seq = ~0ull;  // (one pass 2 a capture)
        bump(g_p2_caps);
        g_cap_cut.store(prepass::walk_was_cut());
        const bool m1 = prepass::stream_marker(&cb_in2_cap);
        o_dir(lighting, vp, a3, a4, a5, a6, a7);
        const bool m2 = m1 && prepass::stream_marker(&cb_out2_cap);
        if (!m1 || !m2) {  // (the playback side sees an incomplete capture; the next pass 1 starts again)
            bump(g_p2_marker_failed);
            s = kRec2;
            g_cap.compare_exchange_strong(s, kArmed);
        }
        return;
    }
    o_dir(lighting, vp, a3, a4, a5, a6, a7);
}

// (run 22, where2) the bind tap (d3d's kBindTapCutVel slot, shared with cut_velocity's probe, which is refused while
// where2 is on): every OMSetRenderTargets on any thread whose render targets include the game's collector. On the H2
// thread inside the window kSH..IN2 it is counted for the capture (the lock's own bind is expected); on any other
// thread it is counted for the session with the last such thread. Reads only (view_resource, prepass::kind_game).
void h2_tap(ID3D12GraphicsCommandList*, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single, const D3D12_CPU_DESCRIPTOR_HANDLE*) {
    if (!g_where2.load(std::memory_order_relaxed) || !rts || !n) return;
    const void* sc = prepass::kind_game(kKCollector);
    if (!sc) return;
    bool hit = false;
    for (unsigned i = 0; i < (single ? 1u : n) && !hit; ++i) hit = d3d::view_resource(rts[i].ptr) == sc;
    if (!hit) return;
    const DWORD tid = GetCurrentThreadId(), h2 = g_h2_tid.load(std::memory_order_relaxed);
    if (!h2) return;  // (no H2 step taken yet: the follow's thread not known here)
    if (tid == h2) {
        bump(g_bind_same);
        if (g_h2_win.load(std::memory_order_acquire)) g_h2_binds.fetch_add(1, std::memory_order_relaxed);
        if (g_h2_pre.load(std::memory_order_acquire)) g_h2_pre_binds.fetch_add(1, std::memory_order_relaxed);
    } else {
        bump(g_bind_other);
        g_bind_other_tid.store(tid, std::memory_order_relaxed);
    }
}

bool setup_compare() {  // (the command thread) the fence, its event and the worker, once
    std::lock_guard lock(g_setup_mutex);
    if (g_fence && g_fence_event) return true;
    ID3D12Device* dev = state::device.load();
    if (!dev) return false;
    if (!g_fence && FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
    if (!g_fence_event) g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_fence_event) return false;
    HANDLE th = CreateThread(nullptr, 0, &worker, nullptr, 0, nullptr);
    if (!th) return false;
    CloseHandle(th);
    return true;
}

std::string kv() {
    const double us = u(g_runs) ? static_cast<double>(u(g_run_ticks)) * g_qpc_us / static_cast<double>(u(g_runs)) : 0.0;
    char b[3200];
    std::snprintf(b, sizeof(b),
                  "on=%d cap=%d left=%d p1_calls=%llu runs=%llu us_per_run=%.1f no_vp2=%llu not_scene=%llu no_stream=%llu out_marker_failed=%llu "
                  "fault=%llu p2_calls=%llu p2_caps=%llu p2_marker_failed=%llu in1_ok=%llu in1_refused=%llu out1_ok=%llu out1_none=%llu "
                  "out1_fatal=%llu in2_ok=%llu in2_refused=%llu out2_ok=%llu out2_refused=%llu made=%llu make_failed=%llu "
                  "ops_overflow=%llu ops_other_thread=%llu caps_started=%llu caps_done=%llu caps_incomplete=%llu frames=%llu cut_frames=%llu "
                  "in_equal=%llu in_sc_diff=%llu in_d_diff=%llu d_hi_only=%llu out_exact=%llu out_exact_in_equal=%llu out_diff_in_equal=%llu "
                  "out_texels_max=%llu out_worst=%llu in_sc_texels_max=%llu in_d_texels_max=%llu map_failed=%llu timeouts=%llu "
                  "no_follow=%llu not_ready=%llu preps=%llu p2_other_frame=%llu states_restored=%llu eye1_exposed=%llu not_direct=%llu "
                  "cut_in_equal=%llu stood=%d prepass_off=%llu vd_rebinds=%llu",
                  g_on.load() ? 1 : 0, g_cap.load(), g_cap_left.load(), u(g_p1_calls), u(g_runs), us, u(g_no_vp2), u(g_not_scene), u(g_no_stream),
                  u(g_out_marker_failed), u(g_fault), u(g_p2_calls), u(g_p2_caps), u(g_p2_marker_failed), u(g_in1_ok), u(g_in1_refused), u(g_out1_ok),
                  u(g_out1_none), u(g_out1_fatal), u(g_in2_ok), u(g_in2_refused), u(g_out2_ok), u(g_out2_refused), u(g_made),
                  u(g_make_failed), u(g_ops_overflow), u(g_ops_other_thread), u(g_cap_started), u(g_cap_done), u(g_cap_incomplete), u(g_c_frames),
                  u(g_c_cut), u(g_c_in_eq), u(g_c_in_sc_diff), u(g_c_in_d_diff), u(g_c_d_hi_diff), u(g_c_out_exact), u(g_c_out_exact_in_eq),
                  u(g_c_out_diff_in_eq), static_cast<unsigned long long>(g_c_out_texels_max.load()),
                  static_cast<unsigned long long>(g_c_out_worst.load()), static_cast<unsigned long long>(g_c_in_sc_texels_max.load()),
                  static_cast<unsigned long long>(g_c_in_d_texels_max.load()), u(g_c_map_failed), u(g_c_timeout), u(g_no_follow), u(g_not_ready),
                  u(g_preps), u(g_p2_other_frame), u(g_states_restored), u(g_eye1_exposed), u(g_not_direct), u(g_c_cut_in_eq),
                  g_stood.load() ? 1 : 0, u(g_prepass_off), u(g_vd_rebinds));
    std::string out = b;
    std::snprintf(b, sizeof(b),
                  " s1_changed=%llu s1_changed_texels_max=%llu game_vs_s1=%llu game_vs_s1_texels_max=%llu where_x0=%u where_y0=%u where_x1=%u "
                  "where_y1=%u first_x=%u first_y=%u first_in1=%u first_in2=%u first_s1=%u no_ctx=%llu",
                  u(g_c_s1_changed), static_cast<unsigned long long>(g_c_s1_texels_max.load()), u(g_c_game_vs_s1),
                  static_cast<unsigned long long>(g_c_gs1_texels_max.load()), g_w_box[0].load(), g_w_box[1].load(), g_w_box[2].load(),
                  g_w_box[3].load(), g_w_first[0].load(), g_w_first[1].load(), g_w_bytes[0].load(), g_w_bytes[1].load(), g_w_bytes[2].load(),
                  u(g_no_ctx));
    out += b;
    const uint64_t bmin = g_c_h_binds_min.load();
    std::snprintf(b, sizeof(b),  // (run 22, where2)
                  " where2=%d h_marks=%llu h_mark_failed=%llu h_taken=%llu h_refused=%llu h_lost=%llu h_frames=%llu h_vs_s1=%llu "
                  "h_vs_s1_texels_max=%llu h_vs_in2=%llu h_vs_in2_texels_max=%llu h_draws_max=%llu h_binds_min=%llu h_binds_max=%llu "
                  "h_x0=%u h_y0=%u h_x1=%u h_y1=%u h_first_x=%u h_first_y=%u h_first_h=%u h_first_in2=%u h_first_s1=%u bind_same=%llu "
                  "bind_other=%llu bind_other_tid=%lu h2_tid=%lu h_pre_draws_max=%llu h_pre_binds_max=%llu",
                  g_where2.load() ? 1 : 0, u(g_h2_marks), u(g_h2_mark_failed), u(g_h2_taken), u(g_h2_refused), u(g_h2_lost), u(g_c_h_frames),
                  u(g_c_h_vs_s1), static_cast<unsigned long long>(g_c_h_s1_max.load()), u(g_c_h_vs_in2),
                  static_cast<unsigned long long>(g_c_h_in2_max.load()), static_cast<unsigned long long>(g_c_h_draws_max.load()),
                  static_cast<unsigned long long>(bmin == ~0ull ? 0 : bmin), static_cast<unsigned long long>(g_c_h_binds_max.load()),
                  g_h_box[0].load(), g_h_box[1].load(), g_h_box[2].load(), g_h_box[3].load(), g_h_first[0].load(), g_h_first[1].load(),
                  g_h_bytes[0].load(), g_h_bytes[1].load(), g_h_bytes[2].load(), u(g_bind_same), u(g_bind_other),
                  static_cast<unsigned long>(g_bind_other_tid.load()), static_cast<unsigned long>(g_h2_tid.load()),
                  static_cast<unsigned long long>(g_c_h_pre_draws_max.load()), static_cast<unsigned long long>(g_c_h_pre_binds_max.load()));
    out += b;
    std::snprintf(b, sizeof(b),  // (run 23, [Stereo] PrepassEye2Decals)
                  " decals=%d decals_on=%d dec_calls=%llu dec_no_mgr=%llu dec_ring=%llu dec_states=%llu dec_marker_failed=%llu dec_no_tls=%llu "
                  "dec_read_refused=%llu dec_fault=%llu dec_frames=%llu",
                  g_decals ? 1 : 0, g_decals_on.load() ? 1 : 0, u(g_dec_calls), u(g_dec_no_mgr), u(g_dec_ring), u(g_dec_states),
                  u(g_dec_marker_failed), u(g_dec_no_tls), u(g_dec_read_refused), u(g_dec_fault), u(g_c_dec_frames));
    return out + b;
}

void reset() {
    for (Count* c : {&g_p1_calls, &g_runs, &g_no_vp2, &g_not_scene, &g_no_stream, &g_p2_calls, &g_p2_caps, &g_p2_marker_failed, &g_run_ticks, &g_in1_ok,
                     &g_in1_refused, &g_out1_ok, &g_out1_none, &g_in2_ok, &g_in2_refused, &g_out2_ok, &g_out2_refused,
                     &g_cap_started, &g_cap_done, &g_cap_incomplete, &g_c_frames, &g_c_cut, &g_c_in_eq, &g_c_in_sc_diff, &g_c_in_d_diff,
                     &g_c_out_exact, &g_c_out_exact_in_eq, &g_c_out_diff_in_eq, &g_c_map_failed, &g_c_timeout, &g_c_d_hi_diff, &g_no_follow,
                     &g_not_ready, &g_preps, &g_p2_other_frame, &g_states_restored, &g_c_cut_in_eq, &g_prepass_off, &g_vd_rebinds,
                     &g_c_s1_changed, &g_c_game_vs_s1, &g_no_ctx, &g_dec_calls, &g_dec_no_mgr, &g_dec_ring, &g_dec_states,
                     &g_dec_marker_failed, &g_dec_no_tls, &g_dec_read_refused, &g_dec_fault, &g_c_dec_frames})
        c->store(0);
    for (auto* a : {&g_c_out_texels_max, &g_c_out_worst, &g_c_in_sc_texels_max, &g_c_in_d_texels_max, &g_c_out_texels_last, &g_c_s1_texels_max,
                    &g_c_gs1_texels_max})
        a->store(0);
    for (auto& a : g_w_box) a.store(0);
    for (auto& a : g_w_first) a.store(0);
    for (auto& a : g_w_bytes) a.store(0);
    // (run 22, where2)
    for (Count* c : {&g_c_h_frames, &g_c_h_vs_s1, &g_c_h_vs_in2, &g_h2_marks, &g_h2_mark_failed, &g_h2_taken, &g_h2_refused, &g_h2_lost, &g_bind_same,
                     &g_bind_other})
        c->store(0);
    for (auto* a : {&g_c_h_s1_max, &g_c_h_in2_max, &g_c_h_draws_max, &g_c_h_binds_max, &g_c_h_pre_draws_max, &g_c_h_pre_binds_max}) a->store(0);
    g_c_h_binds_min.store(~0ull);
    for (auto& a : g_h_box) a.store(0);
    for (auto& a : g_h_first) a.store(0);
    for (auto& a : g_h_bytes) a.store(0);
    g_bind_other_tid.store(0);
    // (fault, out_marker_failed, out1_fatal, make_failed, ops_*, eye1_exposed, not_direct: kept, they mark a stand-down)
}

}  // namespace

void poll(ID3D12GraphicsCommandList* cl) {
    if (!cl || GetCurrentThreadId() != g_ops_tid || g_nops == 0) return;
    if (cl->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {  // (refused here; the steps kept for the next direct list call)
        if (only_h2()) {  // (run 22, where2: a waiting H2 step alone is dropped, never a stand-down)
            drop_h2();
            return;
        }
        bump(g_not_direct);
        exposed("a step's list call on a non-DIRECT list");  // (the steps kept, but eye 1's frame may already be wrong)
        return;
    }
    prepass::g_eye2_pending_flag.store(false, std::memory_order_relaxed);
    const int n = g_nops;
    g_nops = 0;
    for (int i = 0; i < n; ++i) {
        if (g_ops[i] == kH2Cap) {  // (run 22, where2) after the handover: wait while the cut pass 2's region is open, or its
            // handover without a region is still owed (cut_poll runs this poll before that handover)
            if (!prepass::active() && !prepass::g_cut_pending_flag.load(std::memory_order_acquire)) {
                g_h2_defers = 0;
                read_h2(cl);  // (a refusal leaves the capture without kSH: it stays complete)
                continue;
            }
            // (the review's must-fix 1) it waits only alone: any step behind it (IN2, OUT2, an eye-1 step) runs as before,
            // at its own call, and this H2 is dropped; and not for long
            bool later_ok = true;
            for (int k = i + 1; k < n; ++k) later_ok = later_ok && g_ops[k] == kH2Cap;
            if (later_ok && ++g_h2_defers <= 4096) {
                std::memmove(g_ops, g_ops + i, static_cast<size_t>(n - i));
                g_nops = n - i;
                prepass::g_eye2_pending_flag.store(true, std::memory_order_release);
                return;
            }
            g_h2_defers = 0;
            g_h2_pre.store(false, std::memory_order_relaxed);
            bump(g_h2_lost);
            continue;
        }
        switch (g_ops[i]) {
            case kIn1: in1(cl, false); break;
            case kIn1Cap: in1(cl, true); break;
            case kIn1CapD: in1(cl, true, true); break;
            case kDec1Cap: read_dec1(cl); break;
            case kOut1: out1(cl, false); break;
            case kOut1Cap: out1(cl, true); break;
            case kIn2Cap:
                if (read_pass2(cl, true, g_in2_refused)) bump(g_in2_ok);
                else g_cap_bad = true;
                break;
            case kOut2Cap:
                if (read_pass2(cl, false, g_out2_refused)) bump(g_out2_ok);
                else g_cap_bad = true;
                out2_done();
                break;
            case kPrep: prep(); break;
            case kH2Cap: break;  // (handled above)
        }
    }
}

void install() {
    const int want = config::get_int("Stereo", "PrepassEye2Collector", 0);
    if (want <= 0) return;
    if (want != 1) {
        log::warn("[e2sc] PrepassEye2Collector=%d is not 1: off", want);
        return;
    }
    if (!config::get_bool("Stereo", "PrepassArrays", false) || !config::get_bool("Stereo", "PrepassInstanced", false) ||
        config::get_int("Stereo", "PrepassViews", 0) < 2 || !config::get_bool("Stereo", "PrepassWalkCut", false)) {
        log::warn("[e2sc] PrepassEye2Collector needs [Stereo] PrepassArrays=1, PrepassInstanced=1, PrepassViews=2 and PrepassWalkCut=1 "
                  "(the slices and the walk cut's follow of the targets' states): off");
        return;
    }
    if (anchors::stand_down() || !anchors::exe_matches() || anchors::relocated() || !anchors::present(Id::DirCollector) ||
        !anchors::present(Id::DirCollectorCall) || !anchors::present(Id::LightStencilRef)) {
        log::warn("[e2sc] PrepassEye2Collector runs on the analysed RDR.exe build only (DirCollector, DirCollectorCall, LightStencilRef): off");
        return;
    }
    // the call site is a rel32 call of DirCollector (the return address the hook keys on)
    const uintptr_t site = anchors::addr(Id::DirCollectorCall);
    uint8_t op = 0;
    int32_t rel = 0;
    if (!rd(site, &op) || op != 0xe8 || !rd(site + 1, &rel) || site + 5 + static_cast<intptr_t>(rel) != anchors::addr(Id::DirCollector)) {
        log::warn("[e2sc] the call at DirCollectorCall is not a call of DirCollector: off");
        return;
    }
    g_ret = site + 5;
    if (!prepass::cut_tracking()) {  // (MUST-1: prepass::install arms the walk cut's follow with PrepassWalkCut)
        log::warn("[e2sc] the walk cut's follow of the targets' states is not armed (PrepassWalkCut not installed?): off");
        return;
    }
    // RenderStateSet 0x147110 [V]: 48 83 ec 28 | 48 63 c1 | 4c 8d 05 <rel32: the image base> | 41 89 94 80 <disp32: the
    // cache's RVA> | 83 f9 2b: (&DAT_142ac0b60)[id] = value for ids 0..0x2b
    {
        const uintptr_t f = anchors::addr(Id::RenderStateSet);
        uint8_t code[28] = {};
        uint32_t disp = 0;
        int32_t lea = 0;
        const uint8_t lea_op[3] = {0x4c, 0x8d, 0x05}, mov_op[4] = {0x41, 0x89, 0x94, 0x80}, cmp_op[3] = {0x83, 0xf9, 0x2b};
        if (!raw_read(f, code, sizeof(code)) || std::memcmp(code + 7, lea_op, 3) || std::memcmp(code + 14, mov_op, 4) ||
            std::memcmp(code + 22, cmp_op, 3)) {
            log::warn("[e2sc] RenderStateSet's code is not as analysed (the render-state cache not found): off");
            return;
        }
        std::memcpy(&lea, code + 10, 4);
        std::memcpy(&disp, code + 18, 4);
        if (f + 14 + static_cast<intptr_t>(lea) != anchors::base()) {
            log::warn("[e2sc] RenderStateSet's base is not the image's: off");
            return;
        }
        g_rs_cache = anchors::base() + disp;
    }
    {  // (the analysed build only, checked above: no anchor, the entry bytes compared; absent, the binding is not put back)
        uint8_t now[sizeof(kBindVdEntry)] = {};
        const uintptr_t f = anchors::base() + kRvaBindVd;
        if (raw_read(f, now, sizeof(now)) && !std::memcmp(now, kBindVdEntry, sizeof(now))) g_bind_vd = f;
        else log::warn("[e2sc] FUN_140ec67a0's entry is not as analysed: the recorder's +0x1a0 binding is not put back after eye 2's call");
    }
    LARGE_INTEGER fr{};
    QueryPerformanceFrequency(&fr);
    g_qpc_us = fr.QuadPart ? 1e6 / static_cast<double>(fr.QuadPart) : 0.0;
    if (!hooks::install("RDR directional collector term (FUN_1406e58a0, PrepassEye2Collector)", reinterpret_cast<void*>(anchors::addr(Id::DirCollector)),
                        &hk, &o_dir))
        return;
    d3d::add_frame_end_listener(&frame_end);
    // ([Stereo] PrepassEye2Decals, run 23, test only) eye 2's collector decals drawn in pass 1 before its term
    if (config::get_int("Stereo", "PrepassEye2Decals", 0) == 1) {
        if (anchors::present(Id::DecalMgr) && anchors::present(Id::DecalDraw) && anchors::present(Id::ImmRingCursor) && g_rs_cache) {
            g_decals = true;
            g_decals_on = true;
            log::info("[e2sc] [Stereo] PrepassEye2Decals on (test only): eye 2's collector decals (DecalMgr vt+0x70) drawn in pass 1 on "
                      "the private eye-2 viewport before its directional term; \"prepass e2sc decals off\" leaves them out");
        } else {
            log::warn("[e2sc] PrepassEye2Decals needs DecalMgr, DecalDraw and ImmRingCursor (the analysed RDR.exe): off");
        }
    }
    g_installed = true;
    g_on = true;
    log::info("[e2sc] [Stereo] PrepassEye2Collector on (test only): eye 2's directional collector term made in pass 1 on the private eye-2 "
              "viewport into SC2, eye 1's collector and depth put back; \"prepass e2sc compare N\" compares it with pass 2's");
}

std::string command(const std::vector<std::string>& t) {
    if (!g_installed.load())
        return "e2sc: not installed (start with [Stereo] PrepassEye2Collector=1, PrepassArrays=1, PrepassInstanced=1, PrepassViews=2 and "
               "PrepassWalkCut=1, on the analysed RDR.exe)";
    if (t.size() >= 3) {
        const std::string& w = t[2];
        if (w == "kv") return kv();
        if (w == "on" || w == "off") {
            if (w == "on" && g_stood.load()) return "ERROR stood down (" + std::string(g_why) + "): restart the game";
            g_on = w == "on";
        } else if (w == "reset") {
            reset();
        } else if (w == "decals") {  // (run 23, [Stereo] PrepassEye2Decals; test only) eye 2's collector decals in pass 1
            if (!g_decals) return "ERROR decals: not installed (start with [Stereo] PrepassEye2Decals=1, on the analysed RDR.exe)";
            if (t.size() < 4 || (t[3] != "on" && t[3] != "off")) return "ERROR prepass e2sc decals on|off";
            g_decals_on = t[3] == "on";
            log::info("[e2sc] decals %s (eye 2's collector decals drawn in pass 1 before its term)", t[3].c_str());
        } else if (w == "where2") {  // (run 22, research\run22\d4band.md; test only) the step after the handover and the bind tap
            if (t.size() < 4 || (t[3] != "on" && t[3] != "off")) return "ERROR prepass e2sc where2 on|off";
            const bool on = t[3] == "on";
            if (on && !g_where2.load()) d3d::set_bind_tap(d3d::kBindTapCutVel, &h2_tap);  // (cut_velocity's probe refused meanwhile)
            g_where2 = on;
            if (!on) d3d::set_bind_tap(d3d::kBindTapCutVel, nullptr);
            log::info("[e2sc] where2 %s (the game's collector read after each captured cut frame's handover; collector binds watched)", on ? "on" : "off");
        } else if (w == "compare") {
            const int n = t.size() >= 4 ? std::atoi(t[3].c_str()) : 10;
            if (n <= 0 || n > 1000) return "ERROR compare N: 1..1000";
            if (!setup_compare()) return "ERROR the fence, its event or the worker not made";
            const int s = g_cap.load();
            if (s != kIdle && s != kArmed) return "ERROR a capture is in flight (cap=" + std::to_string(s) + ")";
            g_cap_left.store(n);
            g_cap.store(kArmed);
        } else {
            return "ERROR prepass e2sc [on|off|compare N|reset|kv|where2 on|off|decals on|off]";
        }
    }
    const double us = u(g_runs) ? static_cast<double>(u(g_run_ticks)) * g_qpc_us / static_cast<double>(u(g_runs)) : 0.0;
    char b[2000];
    std::snprintf(b, sizeof(b),
                  "e2sc %s: pass 1 calls %llu, eye-2 runs %llu (%.1f us each on the render thread; no vp2 %llu, not the scene viewport %llu, no "
                  "stream %llu, not ready %llu, no follow %llu, preps %llu; render states put back %llu) | eye 1 exposed %llu | IN1 %llu (refused %llu), OUT1 %llu (nothing owed %llu, fatal %llu) | captures: started %llu, "
                  "done %llu, incomplete %llu, left %d (state %d); pass 2 IN2 %llu (refused %llu), OUT2 %llu (refused %llu) | compared %llu "
                  "frames (cut %llu): inputs equal %llu (collector differs %llu, depth %llu), eye 2's term bit-exact %llu (with equal inputs %llu; "
                  "differs with equal inputs %llu; most texels %llu, worst %llu) | last refusal: %s",
                  g_on.load() ? "on" : g_stood.load() ? "stood down" : "off", u(g_p1_calls), u(g_runs), us, u(g_no_vp2), u(g_not_scene),
                  u(g_no_stream), u(g_not_ready), u(g_no_follow), u(g_preps), u(g_states_restored), u(g_eye1_exposed), u(g_in1_ok),
                  u(g_in1_refused), u(g_out1_ok), u(g_out1_none), u(g_out1_fatal), u(g_cap_started), u(g_cap_done),
                  u(g_cap_incomplete), g_cap_left.load(), g_cap.load(), u(g_in2_ok), u(g_in2_refused), u(g_out2_ok), u(g_out2_refused),
                  u(g_c_frames), u(g_c_cut), u(g_c_in_eq), u(g_c_in_sc_diff), u(g_c_in_d_diff), u(g_c_out_exact), u(g_c_out_exact_in_eq),
                  u(g_c_out_diff_in_eq), static_cast<unsigned long long>(g_c_out_texels_max.load()),
                  static_cast<unsigned long long>(g_c_out_worst.load()), g_why[0] ? g_why : "none");
    std::string out = b;
    std::snprintf(b, sizeof(b),
                  " | slice 1 at IN2: changed since IN1 in %llu frames (most texels %llu), the game's collector not its copy in %llu frames (most "
                  "texels %llu); the last input difference in (%u,%u)-(%u,%u), first (%u,%u) IN1 %08x IN2 %08x slice 1 %08x | no recorder context %llu",
                  u(g_c_s1_changed), static_cast<unsigned long long>(g_c_s1_texels_max.load()), u(g_c_game_vs_s1),
                  static_cast<unsigned long long>(g_c_gs1_texels_max.load()), g_w_box[0].load(), g_w_box[1].load(), g_w_box[2].load(), g_w_box[3].load(),
                  g_w_first[0].load(), g_w_first[1].load(), g_w_bytes[0].load(), g_w_bytes[1].load(), g_w_bytes[2].load(), u(g_no_ctx));
    out += b;
    if (g_where2.load() || u(g_c_h_frames)) {  // (run 22, where2)
        const uint64_t bmin = g_c_h_binds_min.load();
        std::snprintf(b, sizeof(b),
                      " | where2 %s: marks %llu (failed %llu), taken %llu (refused %llu, lost %llu); compared %llu: after the handover != slice 1 in "
                      "%llu (most texels %llu), changed by IN2 in %llu (most texels %llu); before kSH draws at most %llu, collector binds at most "
                      "%llu; the window: draws at most %llu, collector binds %llu..%llu; collector binds on this thread %llu, on another %llu (last "
                      "thread %lu)",
                      g_where2.load() ? "on" : "off", u(g_h2_marks), u(g_h2_mark_failed), u(g_h2_taken), u(g_h2_refused), u(g_h2_lost), u(g_c_h_frames),
                      u(g_c_h_vs_s1), static_cast<unsigned long long>(g_c_h_s1_max.load()), u(g_c_h_vs_in2),
                      static_cast<unsigned long long>(g_c_h_in2_max.load()), static_cast<unsigned long long>(g_c_h_pre_draws_max.load()),
                      static_cast<unsigned long long>(g_c_h_pre_binds_max.load()), static_cast<unsigned long long>(g_c_h_draws_max.load()),
                      static_cast<unsigned long long>(bmin == ~0ull ? 0 : bmin),
                      static_cast<unsigned long long>(g_c_h_binds_max.load()), u(g_bind_same), u(g_bind_other),
                      static_cast<unsigned long>(g_bind_other_tid.load()));
        out += b;
    }
    return out;
}

// (run 22, research\run22\d4band.md; test only) the render thread, at a cut pass 2's walk end (cut_velocity::cut_walk_end,
// from prepass_arrays' letterbox hook, after the walk-end marker, whether or not PrepassCutVelocity is installed): the H2
// step's marker, for a capture whose pass 1 was this frame's
void cut_walk_end_mark() {
    if (!g_installed.load(std::memory_order_relaxed) || !g_where2.load(std::memory_order_relaxed) || !g_on.load(std::memory_order_relaxed)) return;
    if (g_cap.load() != kRec1 || g_cap_seq != g_pass1_seq || dual_pass::pass() != 2) return;
    if (prepass::stream_marker(&cb_h2_cap)) bump(g_h2_marks);
    else bump(g_h2_mark_failed);
}
bool where2_on() { return g_where2.load(std::memory_order_relaxed); }  // (cut_velocity's probe: the bind tap slot is shared)

uint64_t runs() { return g_runs_total.load(std::memory_order_relaxed); }  // ([Stereo] PrepassEye2Light)
bool usable() { return g_installed.load() && g_on.load() && !g_stood.load(); }  // ([Stereo] PrepassEye2Light)
// ([Stereo] Pass2FrontSkip, item 7's S2; the render thread)
bool decals_installed() { return g_decals; }
bool last_run_decals() { return g_dec_drawn; }
bool capturing() {
    const int s = g_cap.load();
    return s == kRec1 || s == kRec2;
}

}  // namespace rdrvr::eye2_collector

namespace rdrvr::eye2_collector {
// ([Stereo] PrepassEye2Images, eye2_images.cpp, test only) the playback thread, where ensure() makes and retires SC2: SC2 (null
// when not made), its generation (the makes so far: it moves at each new SC2) and the thread that plays its steps (0 none yet)
ID3D12Resource* sc2_image(uint64_t* gen, unsigned long* tid) {
    if (gen) *gen = g_made.load(std::memory_order_relaxed);
    if (tid) *tid = g_ops_tid;
    return g_img.sc2;
}
}  // namespace rdrvr::eye2_collector
