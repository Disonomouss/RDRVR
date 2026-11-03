#include "core/main_instanced.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <intrin.h>
#include <xmmintrin.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
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
#include "core/eye2_images.h"
#include "core/eye2_light.h"
#include "core/eye2_velocity.h"
#include "core/gpu_stages.h"
#include "core/hooks.h"
#include "core/inst_prev_mirror.h"
#include "core/log.h"
#include "core/main_region.h"
#include "core/prepass_arrays.h"
#include "core/prevworld_mirror.h"
#include "core/pso.h"
#include "core/state.h"

namespace rdrvr::prepass {
std::atomic<bool> g_main_pending_flag{false};  // (prepass_arrays.h cut_pending)
}

namespace rdrvr::main_inst {
std::atomic<bool> g_armed{false}, g_listen{false}, g_noting{false};
std::atomic<bool> g_slice{false};  // ([Stereo] MainSlice or MainRegionE1Compare installed: the list hooks' barrier and query calls)
namespace {
using anchors::Id;
using Count = std::atomic<uint64_t>;
inline void bump(Count& c) { c.fetch_add(1, std::memory_order_relaxed); }
inline unsigned long long u(const Count& c) { return static_cast<unsigned long long>(c.load(std::memory_order_relaxed)); }
void store_max(std::atomic<uint64_t>& a, uint64_t v) {
    uint64_t c = a.load(std::memory_order_relaxed);
    while (v > c && !a.compare_exchange_weak(c, v, std::memory_order_relaxed)) {
    }
}
void store_min(std::atomic<uint64_t>& a, uint64_t v) {
    uint64_t c = a.load(std::memory_order_relaxed);
    while (v < c && !a.compare_exchange_weak(c, v, std::memory_order_relaxed)) {
    }
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
bool rd_raw(uintptr_t a, void* out, size_t n) {
    if (!a) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::atomic<bool> g_installed{false}, g_on{false}, g_stood{false};
uintptr_t g_ret = 0;                // DVL call 1's return address
thread_local int t_ctl_mode = 0;  // (the render thread) the control mode this pass 1's DVL call 1 applied
std::atomic<int> g_control{0};      // the positive control: 0 off, 1 nudge eye 2's rows, 2 eye 1's rows as eye 2's
char g_why[200] = "";               // the last refusal (test only: the status reads it)
void set_armed() { g_armed.store(g_installed.load() && g_on.load() && !g_stood.load(), std::memory_order_relaxed); }
void stand_down(const char* why) {
    std::snprintf(g_why, sizeof(g_why), "%s", why);
    if (!g_stood.exchange(true)) log::error("[mi] MainInstanced stood down: %s", why);
    g_on = false;
    set_armed();
}
void refuse(const char* why) {
    std::snprintf(g_why, sizeof(g_why), "%s", why);
    log::limited("mi.refused", 12, "[mi] a frame excluded: %s", why);
}

// ---- the four targets of the family's draws (the frame graph's bind, billboard.md section 3.2)
enum Kind : int { kFsc, kVel, kReact, kDepth, kKinds };
const char* const kNames[kKinds] = {"FullScreenCopy", "Velocity RT", "Reactivity RT", "Shadow Collector Depth Buffer"};
enum Set : int { kS, kR, kSets };  // the side arrays (the twin's) and the reference arrays (the game's pipeline's)

struct Arr {
    ID3D12Resource* game = nullptr;  // (no reference held: the identity only)
    UINT64 w = 0;
    UINT h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN, vf = DXGI_FORMAT_UNKNOWN;
    ID3D12Resource* arr = nullptr;  // 2 slices, at rest RENDER_TARGET (colour) or DEPTH_WRITE
    UINT planes = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE view[3]{};  // slice 0, slice 1, both
    // ([Stereo] MainRegionE1Compare installed, the depth kind; the reviews of a6830cc9 and 1588431b) the same three DSVs read-only:
    // [0] the depth plane only, [1] the stencil plane only, [2] both
    D3D12_CPU_DESCRIPTOR_HANDLE ro[3][3]{};
    bool ro_ok = false;
};
Arr g_a[kSets][kKinds];  // (the playback thread)
uint64_t g_gen = 0;      // the arrays' generation (moves at each re-make)
ID3D12DescriptorHeap* g_rtv_heap = nullptr;  // kSets x 3 colour kinds x 3 views
ID3D12DescriptorHeap* g_dsv_heap = nullptr;  // kSets x 3
ID3D12DescriptorHeap* g_dsv_ro_heap = nullptr;  // (item 8f, made only with MainRegionE1Compare installed) kSets x 3 views x 3 read-only kinds
UINT g_rtv_inc = 0, g_dsv_inc = 0;
D3D12_RESOURCE_STATES rest(int k) { return k == kDepth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET; }

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

// ---- [Stereo] MainSlice (run 25 item 5, research\run25\slice.md; test only): the switch, the SRVs, the merge pipelines, C
std::atomic<bool> g_sl_installed{false}, g_sl_stood{false};
std::atomic<int> g_sl_mode{0};  // run time: 0 off, 1 on, 2 nomerge (the control: dropped, not merged), 3 always (the merge's DepthFunc ALWAYS)
const char* const kSlName[5] = {"off", "on", "nomerge", "always", "identity"};  // (4: the identity control: no merge, no drop)
char g_sl_why[200] = "";  // the last handover refusal or the stand-down (test only: the status reads it)
using Compile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                   ID3DBlob**);
Compile_t g_compile = nullptr;  // d3dcompiler_47's D3DCompile (install)
void sl_stand_down(const char* why) {
    std::snprintf(g_sl_why, sizeof(g_sl_why), "%s", why);
    if (!g_sl_stood.exchange(true)) log::error("[mi] MainSlice stood down: %s (no more drops: the game draws its own billboards)", why);
}
void sl_refuse(const char* why) {
    std::snprintf(g_sl_why, sizeof(g_sl_why), "%s", why);
    log::limited("mi.slice", 12, "[mi] MainSlice: no handover this frame: %s", why);
}
// the four SRVs of S slice 1 (slice.md 2.2), partition-1 records of the game's DescAlloc, kept for the session (the playback thread)
enum SrvK : int { kSrvFsc, kSrvVel, kSrvDepth, kSrvStencil, kSrvs };
const char* const kSrvName[kSrvs] = {"RDRVR main slice FullScreenCopy", "RDRVR main slice Velocity", "RDRVR main slice depth", "RDRVR main slice stencil"};
const char* const kSrvShort[kSrvs] = {"the slice's colour SRV", "the slice's velocity SRV", "the slice's depth SRV", "the slice's stencil SRV"};
eye2_images::DescRec g_srv_rec[kSrvs]{};
bool g_srv_have[kSrvs] = {};
uint64_t g_srv_gen = 0;       // the arrays' generation the four SRVs view (0: never written)
uint64_t g_remake_frame = 0;  // the present of the arrays' last re-make after the SRVs were first written (the drop held 8 presents)
constexpr uint64_t kHold = 8;  // (the frames in flight: the retire ring's count)
std::atomic<uint32_t> g_srv_idx[kSrvs];  // the bindless indices (written before any merge worker starts; fixed for the session)
// the merge pipelines, one per listed pipeline (by its listing's serial), made on a worker (pso::OwnCreation)
struct Merge {
    ID3D12PipelineState* game = nullptr;  // (the playback thread's key, with serial)
    uint32_t serial = 0;
    std::atomic<int> state{0};  // 0 free, 1 being made, 2 ready, -1 refused (for the session)
    ID3D12PipelineState* merge = nullptr;   // the family's depth-stencil state verbatim
    ID3D12PipelineState* always = nullptr;  // the same with DepthFunc ALWAYS (the control)
    char why[200] = "";
};
constexpr int kMerges = 8;
Merge g_mg[kMerges];
std::mutex g_compile_mutex;
ID3DBlob* g_vs = nullptr;  // (under g_compile_mutex, once: the slots' indices are the session's)
ID3DBlob* g_ps = nullptr;
bool g_compile_tried = false;
// C (compare frames): the game's four targets right after the merge; one slice, the game's descs, at rest COPY_DEST
Arr g_c[kKinds];
// counters (the playback thread unless noted)
Count g_sl_frames, g_sl_handed, g_sl_refused, g_sl_dropped, g_sl_extra, g_sl_short, g_sl_merges, g_sl_nomerge, g_sl_barrier, g_sl_cmp,
    g_sl_srv_allocs, g_sl_srv_writes, g_sl_held, g_sl_seed_far, g_sl_good, g_sl_ref_skipped, g_sl_query_in_run, g_sl_identity;
Count g_sl_pso_made, g_sl_pso_refused;  // (the worker)
// (the review: kHrPso pass 2's first listed pipeline not pass 1's; kHrQuery a query that counts draws open at the drop point)
enum Hr : int { kHrEarly, kHrStood, kHrWhole, kHrHeld, kHrSrv, kHrMerge, kHrPso, kHrQuery, kHrs };  // the handover's refusals by reason
Count g_sl_hr[kHrs];
std::atomic<uint64_t> g_sl_drop_min{~0ull}, g_sl_drop_max{0}, g_sl_last_dropped{0};

// ---- [Stereo] MainPredication (GOAL-single-pass item 6d, research\run25\families.md 1.4 and 3.1.3; test only): a listed draw on a
// predicated list keeps its own predicate; the duplicates after it are drawn under SetPredication(null) and the list's predicate
// (d3d_hooks' per-list record) is put back after the last one. Any disagreement between the record and the list stands it down
std::atomic<bool> g_pd_installed{false}, g_pd_stood{false};
// (run 25, the family compare's diagnostic; test only, off) the references R0 and R1 depth-tested against the game's own bound
// depth view instead of R's seeded copy: "prepass mi refdepth game|own"; never for the billboards (they write depth)
// (run 25 item 6g, a diagnostic: read only) at DVL call 1's open and close, each pass: the current viewport's size
// (+0x330/+0x334) and gViewport (rage_prev_world +0x40, the CPU storage); pass 2's against pass 1's of the same frame
float g_gv[3][2][4];  // [pass][0 open, 1 close][xyzw] (the render thread)
int g_gvsz[3][2][2];
Count g_gv_eq[2], g_gv_ne[2], g_gv_walk_changed[3], g_gv_unread, g_gv_unpaired;
uint64_t g_gv_stamp[3][2];  // (the review) the frame (state::presents + 1) of each slot's last whole read; 0 never
std::atomic<int> g_gvsz_last[3][2][2];
std::atomic<uint32_t> g_gv_last[3][2][4];  // (the bits, for kv: pass 1 and 2 at open)
void gv_sample(int pass, int at) {
    const uintptr_t st = prevworld::storage();
    uintptr_t cur = 0;
    g_gv_stamp[pass][at] = 0;
    if (!st || !rd(anchors::addr(Id::ViewportCurrent), &cur) || !cur || !rd_raw(st + 0x40, g_gv[pass][at], 16) || !rd_raw(cur + 0x330, g_gvsz[pass][at], 8)) {
        bump(g_gv_unread);
        return;
    }
    const uint64_t f = state::presents.load(std::memory_order_relaxed) + 1;
    g_gv_stamp[pass][at] = f;
    for (int i = 0; i < 2; ++i) g_gvsz_last[pass][at][i].store(g_gvsz[pass][at][i], std::memory_order_relaxed);
    for (int i = 0; i < 4; ++i) {
        uint32_t b = 0;
        std::memcpy(&b, &g_gv[pass][at][i], 4);
        g_gv_last[pass][at][i].store(b, std::memory_order_relaxed);
    }
    if (at == 1 && g_gv_stamp[pass][0] == f && (std::memcmp(g_gv[pass][0], g_gv[pass][1], 16) != 0 || std::memcmp(g_gvsz[pass][0], g_gvsz[pass][1], 8) != 0))
        bump(g_gv_walk_changed[pass]);
    if (pass == 2) {
        if (g_gv_stamp[1][at] != f) bump(g_gv_unpaired);  // (pass 1's slot not read whole this frame)
        else bump(std::memcmp(g_gv[1][at], g_gv[2][at], 16) == 0 && std::memcmp(g_gvsz[1][at], g_gvsz[2][at], 8) == 0 ? g_gv_eq[at] : g_gv_ne[at]);
    }
}
// (run 25 item 6g; test only, off: "prepass mi p1vp on|off") pass 1's DVL call 1 gets pass 2's gViewport (the previous frame's,
// read at its open: under d4 the render size, pass 1's the output size, r77): written at rage_prev_world +0x40 at pass 1's open
// and pass 1's own put back at its close (only where the storage still holds ours)
bool wr_raw(uintptr_t a, const void* src, size_t n);  // (below: the union cull's SEH write)
std::atomic<bool> g_p1vp{false};
Count g_p1vp_set, g_p1vp_back, g_p1vp_kept, g_p1vp_none, g_p1vp_failed, g_p1vp_equal;
float g_p1vp_saved[4], g_p1vp_put[4];
uintptr_t g_p1vp_at = 0;  // (the render thread) the storage written this pass 1; 0 none
int g_p1vp_res = 0;  // ([Stereo] MainRegion's frame gate, item 8d; the render thread) this pass 1's gViewport is pass 2's: 1 set, 2 already equal
void p1vp_open(uint64_t f) {
    g_p1vp_at = 0;
    g_p1vp_res = 0;
    const bool whole = g_gv_stamp[2][0] + 1 == f && g_gv_stamp[1][0] == f;  // (pass 2's of the previous frame, pass 1's of this one, whole)
    if (!g_p1vp.load(std::memory_order_relaxed)) {
        // (MainRegion only, read only) without p1vp the gate holds where pass 1's gViewport is already pass 2's (no d4)
        if (main_region::armed() && whole && !std::memcmp(g_gv[1][0], g_gv[2][0], 16)) g_p1vp_res = 2;
        return;
    }
    const uintptr_t st = prevworld::storage_live();  // (the review: the CB still the grown one)
    if (!st || !whole) {
        bump(g_p1vp_none);
        return;
    }
    if (!std::memcmp(g_gv[1][0], g_gv[2][0], 16)) {  // (already equal: nothing to do)
        bump(g_p1vp_equal);
        g_p1vp_res = 2;
        return;
    }
    std::memcpy(g_p1vp_saved, g_gv[1][0], 16);
    std::memcpy(g_p1vp_put, g_gv[2][0], 16);
    if (!wr_raw(st + 0x40, g_p1vp_put, 16)) {
        bump(g_p1vp_failed);
        return;
    }
    if (!prevworld::rearm()) bump(g_p1vp_failed);  // (the review: the next draw's commit uploads the CB again)
    g_p1vp_at = st;
    g_p1vp_res = 1;
    bump(g_p1vp_set);
}
void p1vp_close() {
    if (!g_p1vp_at) return;
    const uintptr_t st = g_p1vp_at;
    g_p1vp_at = 0;
    float now[4];
    if (!rd_raw(st + 0x40, now, 16) || std::memcmp(now, g_p1vp_put, 16) != 0) {  // (the game wrote it inside the walk: kept)
        bump(g_p1vp_kept);
        return;
    }
    if (!wr_raw(st + 0x40, g_p1vp_saved, 16)) {
        bump(g_p1vp_failed);
        return;
    }
    if (!prevworld::rearm()) bump(g_p1vp_failed);
    bump(g_p1vp_back);
}
std::atomic<bool> g_refdepth_game{false};
Count g_refdepth_draws, g_refdepth_writes;
bool g_draw_writes = false;  // (the playback thread) this listed draw's pipeline writes depth or stencil: never the game's view
std::atomic<int> g_pd_mode{0};  // run time: 0 off (a predicated listed draw excludes its frame, as item 3), 1 on, 2 refs (on; pass 1 without
                                // a twin still draws R0, the frame excluded: the null and the restore exercised on a family not yet twinned)
const char* const kPdName[3] = {"off", "on", "refs"};
char g_pd_why[200] = "";
struct PdNote {  // (the playback thread) the predicate held from before_draw to after_draw of one listed draw
    bool active = false;
    ID3D12GraphicsCommandList* cl = nullptr;
    d3d::Predicate p;
};
PdNote g_pd_note;
Count g_pd_held[3], g_pd_unknown, g_pd_slice, g_pd_nulled, g_pd_restored, g_pd_dup_twin, g_pd_dup_ref, g_pd_skipped, g_pd_refonly, g_pd_after_bad,
    g_pd_unpred_set, g_pd_seed_nulled, g_p_seed_read, g_fam_judged, g_fam_within, g_fam_rep;
std::atomic<uint32_t> g_p_seed_read_state{0};  // (run 25 r60: the read-only depth state a non-writing family's seed went from)
void pd_stand_down(const char* why) {
    std::snprintf(g_pd_why, sizeof(g_pd_why), "%s", why);
    if (!g_pd_stood.exchange(true)) log::error("[mi] MainPredication stood down: %s (a predicated listed draw excludes its frame again)", why);
}

// ---- [Stereo] MainUnionCull (GOAL-single-pass item 6e; test only): pass 1's DVL call 1 culls against both eyes' union, as
// PrepassUnionCull does for the pre-pass walk: the current viewport's side planes (+0x390..+0x3cf, IsSphereVisible 0x130030) and
// side-plane matrix (+0x3f0..+0x42f; SetWorldMtx 0x1348e0 makes the per-world planes +0x2c0..+0x2ff from it, which grmModel's culler
// 0x142b00 reads) are the union viewport's (camera_lever::shadow_union_vp) from DVL call 1's start to its return, then the eye's
// again. +0x2c0 is made again with the swapped planes (when it held World x planes before) and with the eye's after. Pass 2 is
// never touched: it culls against eye 2's own planes. The render thread swaps; the playback thread checks by draw keys
// (pipeline, arguments, vertex buffer) that pass 2's listed draws are a sub-multiset of pass 1's
std::atomic<bool> g_uc_installed{false}, g_uc_on{false};
uintptr_t g_uc_vp = 0;  // (the render thread) the viewport whose planes are the union's now (0: none)
// (item 8e, r110 E1m's end check) "prepass mi reset" zeroes uc_begins / uc_ends on the command thread, which may land between a
// begin and its end (ends one more than begins); the render thread zeroes them again at its next begin, before it counts
std::atomic<bool> g_uc_reset_req{false};
// ---- [Stereo] MainUnionBox (GOAL-single-pass item 8e, research\run25\item8e.md 14; test only, off; needs MainUnionCull and the
// private eye-2 viewport, PrepassViews=2): grmModel's box culler 0x142b00 (reached only through the pointer 0x142272378, by the
// game's indirect calls, so a plain detour) tests a model-space box, [rcx] its min and [rcx+0x10] its max, against the current
// viewport's four side planes at +0x2c0 (SoA rows x, y, z, d: World x +0x3f0, as SetWorldMtx makes them), each plane alone: 0
// outside, 1 crossing, 2 inside (the caller tests each geometry unless 2). One plane at a time, the union's planes (the centre camera
// widened 3 degrees) do not contain an eye's behind the camera: a box there passes eye 2's and fails the union's (r111 M1, tier 2's
// c1_inside and c2_geom at the near place). Inside pass 1's union swap, on the render thread, the answer is the most of the game's
// (the union's planes) and the same test on World x eye 1's side matrix and on World x eye 2's (the private eye-2 viewport's, as
// pass 2 makes it; checked against pass 2's at its open). So pass 1 keeps every model and geometry either eye keeps. Everywhere
// else, and whenever +0x2c0 is not World x the union's or the test's copy differs from the game's answer, the game's answer as is
constexpr uintptr_t kRvaBoxCull = 0x142b00, kRvaBoxCullPtr = 0x2272378;
using BoxCull_t = int (*)(const float* box);
BoxCull_t o_box = nullptr;
std::atomic<bool> g_ub_installed{false}, g_ub_on{false};
Count g_ub_frames, g_ub_noeye2, g_ub_calls, g_ub_notvp, g_ub_odd, g_ub_emu_ne, g_ub_up0, g_ub_up1, g_ub_e2_eq, g_ub_e2_ne;
thread_local bool t_ub = false;  // (the render thread) inside pass 1's union swap with MainUnionBox on
// (the render thread) this swap's side matrices (0 the union's, 1 eye 1's, 2 eye 2's), the planes World x each for the cached World
alignas(16) float g_ub_side[3][16], g_ub_world[16], g_ub_pl[3][16];
bool g_ub_cached = false, g_ub_e2_pending = false;
alignas(16) float g_uc_saved[2][16], g_uc_union[2][16];
Count g_uc_begins, g_uc_ends, g_uc_nounion, g_uc_failed, g_uc_rewritten, g_uc_2c0_set, g_uc_2c0_odd, g_uc_2c0_back, g_uc_2c0_kept, g_uc_marks,
    g_uc_mark_failed, g_uc_mark_lost;
std::atomic<uint32_t> g_uc_widen_mdeg{0};  // the last swap's largest side-plane normal change (millidegrees)
// (the playback thread) the frame's draw keys: [0] with the vertex buffer, [1] without; generation-stamped, no clearing (item 8d:
// keys::, the same tables and rule, made general so main_region keeps its own set)
constexpr unsigned kUcSlots = 4096;
keys::Slot g_uc_tab[2][kUcSlots];
keys::Tables g_uc_keys{{g_uc_tab[0], g_uc_tab[1]}, kUcSlots, 0};
struct UcFrame : keys::Frame {
    bool uc1 = false;     // pass 1's region played the union marker
    uint64_t draws1 = 0;  // pass 1's region draws (all pipelines)
} g_ucf;
// per category (0: pass 1 not union-culled, the control; 1: union-culled), over judged frames
Count g_uc_frames[2], g_uc_subset[2], g_uc_subset_nvb[2], g_uc_miss[2], g_uc_miss_nvb[2], g_uc_listed1[2], g_uc_listed2[2], g_uc_draws1[2],
    g_uc_draws2[2], g_uc_over, g_uc_unjudged;
std::atomic<uint64_t> g_uc_last_sum2{0}, g_uc_last_n2{0}, g_uc_last_n1{0}, g_uc_last_miss{0}, g_uc_last_cat{0};

// ---- [Stereo] MainFamily=region (GOAL-single-pass item 8e, research\run25\item8e.md; test only): e1's draws listed from main_region's
// classifier on dup frames, their twins from pso::region_twin_of, 8d's verdict as the frame rule, joint / solo / subset, the swap
// (M1) g_rg_want: read from the ini at main_inst's install (before the device: pso's own switch, g_main_region, is read only at the first
// device's hooks): [Stereo] MainFamily=region with MainRegion=1, MainUnionCull=1, MainPredication=1 and InstancedShadersAuto=1, as pso
// checks them (read-only after). g_rg_installed: the machinery installed on it. rg_on(): installed and pso's own switch on, read lazily
// (pso falling back to bb at the device refuses the region path, so it never pairs with a non-empty bb set)
bool g_rg_want = false;
std::atomic<bool> g_rg_installed{false};
inline bool rg_on() { return g_rg_installed.load(std::memory_order_relaxed) && pso::main_family_region(); }
enum RgMode : int { kRgNone, kRgJoint, kRgSolo, kRgSubset, kRgModes };
const char* const kRgModeName[kRgModes] = {"none", "joint", "solo", "subset"};
// run time ("prepass mi region all|solo|subset"), latched at pass 1's open: the mode | (the solo ordinal + 1) << 8 in one word (never
// torn), and the subset's mask (stored before the word: a latch that reads a subset word reads its mask)
std::atomic<int> g_rg_sel{kRgJoint};
std::atomic<uint64_t> g_rg_subset{~0ull};
std::atomic<int> g_rg_swap{0};                 // 0 off, 1 auto (joint, information)
std::atomic<int> g_rg_every{8};                // a dup frame 1 in N while a capture is armed (N >= 8 except solo)
std::atomic<bool> g_rg_ip{true};               // (S3) "prepass mi region ip on|off": instprev (treerock) twins handed out, latched at pass 1
uint64_t g_rg_dupc = 0;                        // (the playback thread) armed pass 1 opens
uint64_t g_rg_last_drew = 0;                   // (the playback thread) the last judged frame's pass-1 pairs (by ordinal)
int g_rg_swap_last = -1;                       // (the playback thread) the last pair swapped
constexpr int kOrds = 64;
// the control kinds: 0 none, 1 nudge, 2 eye1, 3 e2img, 4 pw, 5 ip (the main InstPrev nudge); (item 8f, playback only, e1f frames) 6 seed
// (S1's stencil from D2L, the lights' marks, for D2), 7 single (one pair's pass-1 draws by the game's pipeline into S0 only). The render thread's marked kinds
// are 0..kCkMarked-1; per-pair records and the 8e loops that readers parse by position keep those
// (item 8f's remap, playback only) 8 rmdrop: on a remap frame that pair's listed draws neither issued nor twinned (eye 1's positive control)
// 9 rmpoison: at the remap's close S0's colour cleared to (1, 0, 1, 1) before the copy-back (eye 1 must show it)
constexpr int kCk = 10, kCkMarked = 6, kCkSeed = 6, kCkSingle = 7, kCkRmDrop = 8, kCkRmPoison = 9;
const char* const kCkName[kCk] = {"none", "nudge", "eye1", "e2img", "pw", "ip", "seed", "single", "rmdrop", "rmpoison"};
// the playback thread's
Count g_rv[pso::region::kVerdicts];  // region_twin_of's answers by verdict
// (item 8e, research\run25\item8e.md 14) the draws of entities body.cpp edits per pass (the player's sets, held items, the copy) stay
// out of the region: body.cpp's entity hook brackets them with stream markers while armed (MainFamily=region installed and "prepass mi
// region own on", the default); on the playback thread g_own_pb is set between the two, and the listing leaves those draws to each pass
std::atomic<bool> g_own_armed{false}, g_own_on{true};
std::atomic<bool> g_rg_seed_early_on{false};  // ("prepass mi region seed early|listed": rg_seed_early, below)
// (item 8f0b, test only: "prepass mi region fscseed <r> <g> <b> <a>|off") S's and R's FullScreenCopy seeded with this colour instead of
// 0, both alike (J unchanged): on the pause, S1 under two seeds shows which texels depend on FSC's seed (uncovered, or blended over it)
std::atomic<uint32_t> g_rg_fsc_seed[8] = {0, 0, 0, 0, 0, 0, 0, 0};  // (float bits: S's [0..3], R's [4..7]; all 0: the clear to 0, as before)
// ---- [Stereo] MainRegionE1Compare (GOAL-single-pass item 8f, research\run25\item8f.md "8f: the plan"; test only, off by default; needs
// MainFamily=region installed and PrepassEye2LightDepth=1; "prepass mi region e1f on|off", off by default): on joint dup frames slice 1 of
// S and R is seeded from eye 2's own images (depth and stencil D2L; velocity V2 under DLSS, a clear under FXAA; reactivity 0; FSC the 8f0b
// sentinel), R gets no duplicates (R0 <- T, pass 2's four targets at its e1's first non-empty draw; R1 keeps the seed), the player's own
// draws of pass 2 also go into S1, and C <- C2 (the game's four at pass 2's e1 end). The worker: G0 = R1 against T (the seeds), G1 = S1
// against C2 (compare_e1f). With the switch or the command off nothing of this runs (no C, no copy, no barrier)
int g_e1f_want = 0;                       // (the ini's, read at main_inst's install)
std::atomic<bool> g_e1f_installed{false}, g_e1f_on{false};
std::atomic<bool> g_e1f_live{true};       // ("prepass mi region e1f pred live|null") pass 1's twins under the list's own predicate, or nulled
std::atomic<int> g_e1f_ctl{0};            // ("prepass mi control seed|single|rmdrop <pair>") kCkSeed, kCkSingle, kCkRmDrop or 0 (rg_latch)
std::atomic<int> g_e1f_single{-1};        // (single's ordinal)
// (GOAL-single-pass-2, E2/E3/E5, test only; off by default) "region e1f dupall on|off": with the handover off (no mode latched) and the
// remap off, every joint e1f frame a dup frame (pass 1's seed and twins into S), compare frames still 1 in N (rcmp: only they are read
// back); ignored (counted) while a handover mode is set or the remap is on. "dupall twins on|off" (on by default): off, a dupall frame
// that is no compare frame seeds S and R but draws no twin (its listed draws the game's alone, as on a non-dup frame; an instprev twin's
// "ok" marker spent as a twin's would be). E5: each twin draw (any dup frame) on a list with a query open (or the open queries unknown)
std::atomic<bool> g_e1f_dupall{false}, g_e1f_dupall_twins{true};
// ("dupall mask 3|1|2", 3 by default) a dupall frame's twins (never a compare frame's) drawn with this view-instance mask: 1 view 0 alone
// (into S's slice 0), 2 view 1 alone (slice 1). S is seeded again at each frame's first listed draw and read back on compare frames only
std::atomic<int> g_e1f_dupall_mask{3};
Count g_e1f_dupall_mask_twins;  // (twins drawn with a mask other than 3)
// ("twins mask 3|2", 3 by default) every e1f frame's twins (rg_after's listed path: handover frames, G1 / H1 / G1' compare frames, dupall
// frames) drawn with view-instance mask 2 (view 1 alone, into S's slice 1: what the handover and G1 read), overriding "dupall mask". S's
// slice 0 then keeps the seed: 8f's negative control (S1 vs S0) is not measured (counted), the pair trace refused (it starts on mask 3
// only). Never on a remap frame (S0 stands in for eye 1 there; rm_draw's twins keep 3): such frames counted, their twins as before
std::atomic<int> g_e1f_twins_mask{3};
Count g_e1f_twins_mask2, g_e1f_twins_mask_remap, g_e1f_neg_unmeasured;  // (twins drawn with mask 2; remap frames left at 3; compares)
Count g_e1f_dupall_frames, g_e1f_dupall_notwin, g_e1f_dupall_ignored, g_e1f_notwin_draws, g_e1f_twin_open_query, g_e1f_twin_query_unknown;
// (GOAL-single-pass-2, test only) "region e1f c2pred null|live" (live by default): null, on a compare (rcmp) e1f frame each pass-2 e1 draw
// the game issues (the draws that make C2, and G1''s redraw into R1 after them; a player's own draw on a predicated list is refused
// before, so none goes again into S1 unpredicated) recorded under SetPredication(null), the
// list's own predicate (its record known) put back after it and the record checked, as "e1f pred null" does for pass 1's twins. A
// predicated draw whose record is not known excludes the compare (kXPred), never guessed. kDrop's in-place R1 draw keeps the live one
std::atomic<bool> g_e1f_c2null{false};
Count g_e1f_c2pred_nulled, g_e1f_c2pred_unknown, g_e1f_c2pred_bad;
const float kE1fSentinel[4] = {1.0009765625f, 2.001953125f, 0.50048828125f, 1.0f};  // (8f0b: half-exact, RGBA16F)
constexpr uint64_t kE1fSentinel64 = 0x3C00380140013C01ull;  // (the same as an RGBA16F texel's 8 bytes, little-endian)
// the seed's refusals by reason (kXE1fSeed), T's (kXE1fT) and C2's (kXE1fC2)
enum E1fSeedWhy : int { kEsTech, kEsFmt, kEsC, kEsD2l, kEsD2lStale, kEsD2lThread, kEsD2lDesc, kEsD2, kEsV2, kEsV2Stale, kEsV2Thread, kEsV2Desc, kEss };
const char* const kEsName[kEss] = {"tech", "fmt", "c", "d2l", "d2l_stale", "d2l_thread", "d2l_desc", "d2", "v2", "v2_stale", "v2_thread", "v2_desc"};
enum E1fTWhy : int { kEtNotOk, kEtBind, kEtSeed, kEtDepth, kEtPred, kEts };
const char* const kEtName[kEts] = {"not_ok", "bind", "seed", "depth", "pred"};
enum E1fCWhy : int { kEcNoT, kEcList, kEcBind, kEcBarrier, kEcC, kEcDepth, kEcPred, kEcLate, kEcStale, kEcClear, kEcWrite, kEcColour, kEcSplit, kEcs };
const char* const kEcName[kEcs] = {"no_t", "list", "bind", "barrier", "c", "depth", "pred", "late", "stale", "clear", "write", "colour_state", "split"};
Count g_e1f_sx[kEss], g_e1f_tx[kEts], g_e1f_cx[kEcs];
// (the playback thread's) frames latched, seeds made, V2 copies, T and C2 taken, own draws into S1 and refused, twins under the live
// predicate, barriers on the four between T and C2, e1 end markers (and stale ones), the controls' draws, the exclusions by bit
Count g_e1f_latched, g_e1f_seeds, g_e1f_v2_copies, g_e1f_t, g_e1f_c2, g_e1f_own_draws, g_e1f_own_refused, g_e1f_pred_live, g_e1f_barrier,
    g_e1f_e1end, g_e1f_e1end_stale, g_e1f_seed_ctl, g_e1f_single_draws, g_e1f_x_seed, g_e1f_x_t, g_e1f_x_c2, g_e1f_clears, g_e1f_writes, g_e1f_splits, g_e1f_planes_differ,
    g_e1f_uav_barriers, g_e1f_transitions[2];  // (transitions of the four between T and C2: [0] colour, [1] depth; information)
char g_e1f_why[200] = "";  // (the last refusal: the status reads it)
// (item 8f, r116 G2) the game depth's two planes met in different followed states (seed, T or C2: each plane's own barriers), the last pair
std::atomic<uint32_t> g_e1f_pst[2] = {0, 0};
// (r117 DG4) the follow's users here refuse a plane whose followed state was not set on the use's own list (prepass::kind_followed_fresh):
// the seed (8e families, 8e / 8f / 8g region frames), T, C2, the handover's decision, the remap's close; counted per user (x_follow_stale)
enum FsUser : int { kFsSeed, kFsT, kFsC2, kFsHandover, kFsRemap, kFsUsers };
const char* const kFsName[kFsUsers] = {"seed", "t", "c2", "handover", "remap"};
Count g_fs_stale[kFsUsers];
// (the diagnostic, test only) the first kFsDiag e1f frames after a mono -> stereo switch (pass 1's opens more than 2 presents apart): the
// planes' followed states, the lists that set them and every barrier issued on the game's depth, logged
constexpr int kFsDiag = 3;
int g_fs_diag_left = 0;     // (the playback thread)
uint64_t g_fs_last_open = 0;
bool fs_followed(ID3D12GraphicsCommandList* cl, unsigned p, D3D12_RESOURCE_STATES* st, int user);  // (below)
void fs_log(const char* where, ID3D12GraphicsCommandList* cl, const D3D12_RESOURCE_STATES from[2], const char* what);
// ---- (item 8f's remap, research\run25\item8.md 2.3; test only: "prepass mi region e1f remap on|off", off by default; e1f frames only)
// pass 1's S slice 0 stands in for the game's four targets over e1. At the seed S0 <- the four (in the slice-1 seeds' batch); each listed
// e1 draw's game draw is not issued (the hook returns before it) and its twin draws both views into S (no twin, or the single control:
// the game's pipeline into S0 only, the frame refused); a non-listed e1 draw (the player's own, a pair without an ordinal) is issued and
// drawn again into S0; at pass 1's e1 end S0 -> the four (rm_end), so eye 1 goes on as two-pass. While S0 stands in, the game's four
// must not be read, written or moved out of RENDER_TARGET / DEPTH_WRITE / DEPTH_READ: any such event (a clear or copy touching them, a
// barrier taking one into another state, a split, UAV or aliasing barrier on them, an indirect draw, a draw that holds any of them and
// cannot be drawn into S0, a listed draw refused, an event on another list, the list's Close, the region's close) closes the remap at
// once, before it, and the frame goes on as an e1f frame. A close that cannot be made safely (a state unknown or contradicted, the
// predicate unknown) is not made: eye 1 lacks the draws skipped so far that frame (counted: lost; the frame excluded, kXE1fRemap)
std::atomic<bool> g_rm_want{false};
enum RmWhy : int { kRmEnd, kRmRefused, kRmBind, kRmState, kRmSplit, kRmUav, kRmAlias, kRmClear, kRmCopy, kRmIndirect, kRmList, kRmClose, kRmRegion,
                   kRmOutside, kRms };
const char* const kRmName[kRms] = {"end", "refused", "bind", "state", "split", "uav", "alias", "clear", "copy", "indirect", "list", "close", "region",
                                   "outside"};
enum RoWhy : int { kRoPred, kRoTrack, kRoType, kRoDepth, kRoPredRec, kRos };  // (the remap refused at the open: nothing skipped)
const char* const kRoName[kRos] = {"pred", "track", "type", "depth", "pred_record"};
Count g_rm_ro_closes, g_rm_ro_frames;  // (r116 G3) closes that left a read-only depth alone; remaps opened read-only
Count g_rm_frames, g_rm_skipped, g_rm_own_s0, g_rm_single, g_rm_lost_n, g_rm_closed[kRms], g_rm_lost[kRms], g_rm_refused[kRos];
// (the playback thread) S0 standing in (on), the close owed by pass 1's e1 end (taken at the next list call, poll), a non-listed draw to
// draw again into S0 (before_draw to after_draw); the list and thread of the remap
bool g_rm_on = false, g_rm_owed = false, g_rm_dup = false;
ID3D12GraphicsCommandList* g_rm_cl = nullptr;
DWORD g_rm_tid = 0;
// (the review's SF3) the remap list's predicate: from its record at the start (known, or no remap), then each SetPredication on it
// (note_predication, d3d_hooks' tap, after any substitution): the close knows it even at a call on another list of the thread
d3d::Predicate g_rm_pred;
// (r116 G3) the depth's planes at the remap's open: read-only (DEPTH_READ, no write state; e.g. with shader-resource bits: e1's draws then
// write neither the game's depth nor S's, and may read the game's, which equals S0's) or DEPTH_WRITE / plain DEPTH_READ. Read-only: any
// transition of the depth into a write state closes the remap first, and the close leaves the depth alone (unchanged). The states at the
// open and at the last depth refusal, for kv
bool g_rm_ro = false;
std::atomic<uint32_t> g_rm_open_st[2] = {0, 0}, g_rm_ref_st[2] = {0, 0};
constexpr D3D12_RESOURCE_STATES kRmWrites = D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
                                            D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST | D3D12_RESOURCE_STATE_STREAM_OUT;
inline bool rm_read_only(D3D12_RESOURCE_STATES st) { return (st & D3D12_RESOURCE_STATE_DEPTH_READ) && !(st & kRmWrites); }
// (draws with depth or stencil writes under a read-only remap; e1f draws into S on a read-only DSV: depth only, stencil only, both)
Count g_rm_ro_write_draws, g_e1f_ro_dsv_draws[3];
// (the rmdrop control: the pair's draws dropped on remap frames; its listed draws on e1f frames without the remap; rmpoison's closes)
Count g_rm_dropped, g_rm_drop_noremap, g_rm_poisoned;
// (r116 G3's diagnosis, test only) the last barrier that closed a remap by state, split, UAV or aliasing: the kind (0..3), its type,
// StateBefore, StateAfter, Subresource; per frame (published at pass 1's close) the draws skipped while it stood and pass 1's non-empty e1
// draws after its close (drawn by the game), the last frame's and summed
std::atomic<uint32_t> g_rm_last_tr[5] = {0, 0, 0, 0, 0};
std::atomic<uint64_t> g_rm_skip_last{0}, g_rm_after_last{0};
Count g_rm_after_sum;
// (r116 G3's diagnosis, test only) the rmpoison control's check that the copy-back landed: after the copy-back on a poison frame, the
// centre texel of each of the game's three colour targets copied to a readback (its first 8 bytes each), read 8 presents later by kv
// (state 0 free, 1 recorded at frame g_pz_frame, then read into g_pz_text and free again)
ID3D12Resource* g_pz_rb = nullptr;
ID3D12Device* g_pz_dev = nullptr;
std::atomic<int> g_pz_state{0};
std::atomic<uint64_t> g_pz_frame{0};
std::mutex g_pz_mutex;
std::string g_pz_text = "-";
std::string rm_poison_text();  // (below)
// ---- [Stereo] MainRegionHandover (GOAL-single-pass item 8g, research\run25\item8f.md "8g: the plan" and its review; test only, off by
// default, never in a shipped or user ini; needs MainRegionE1Compare installed; "prepass mi region e1f handover on|off|nohandover|identity",
// off by default). Every joint frame with "region e1f on" and the handover is a handover frame; its pass 1 is 8f's (the seed, the twins
// into S both views). At pass 2's first non-empty e1 draw, before the game's, the decision (ho_decide): pass 1 clean as 8d will judge it,
// the frame not excluded, the four bound with the arrays' formats, the depth's planes followed, unpredicated, no counting query open,
// the list's viewport, scissor and topology noted, the merge pipeline for the bound pipeline's root signature made and S1's FSC SRV
// written. Handed over: S1 -> the four (depth and stencil per plane from the follow; velocity and reactivity under DLSS by copy; FSC by
// the merge, which keeps the game's own where S1 is the 8f0b sentinel), then each pass-2 e1 draw whose key pass 1 drew (by count; the
// player's own bracket checked first) is dropped, the rest (the player's own, misses, no ordinal, indirect) drawn by the game on top.
// Compare frames (1 in N while a capture is armed): T -> R1 before it, each pass-2 e1 draw also into R1 by the game's pipeline (R's
// read-only DSVs from the follow), C at e1's end: G1' = C against R1 (compare_handover). A refusal leaves the frame two-pass (an 8f
// compare frame when it is one); nothing refuses after the decision. (GOAL-single-pass-2 item 4, test only) "handover ref h1" puts G1
// and H1 in G1''s place, alternating per compare frame; "handover trim on" copies only the planes pass 1's e1 wrote (below)
int g_ho_want = 0;  // (the ini's, read at main_inst's install)
std::atomic<bool> g_ho_installed{false};
std::atomic<int> g_ho_mode{0};  // 0 off, 1 on, 2 nohandover (decided, dropped, nothing copied: the control), 3 identity (decided, nothing dropped or copied)
std::atomic<bool> g_ho_strict{false};  // ("handover strict on") pass 1's own draws refuse the decision; pass 2's own draws or misses exclude the compare
// (4 copyonly, H2m's diagnosis: decided, S1 handed over at the first e1 draw, nothing dropped: the game draws its own e1 over it)
const char* const kHoModeName[5] = {"off", "on", "nohandover", "identity", "copyonly"};
std::atomic<bool> g_ho_sparse{false};  // ("handover sparse on|off", H2m's diagnosis) the handover only on compare frames
// (H2m's diagnosis) the four's transitions into a shader-resource state inside e1 (pass 1 / pass 2, by kind): an e1 draw reads them
Count g_ho_sparse_skipped, g_ho_copyonly, g_hc_copy_frames, g_hc_copy_exact, g_e1_srv[3][4];
// ("handover velreact always|tech", H2m's diagnosis) always: S1's velocity and reactivity handed over under FXAA too (tech: DLSS only)
std::atomic<bool> g_ho_vr_always{false};
Count g_ho_vr_fxaa;  // (handovers under FXAA that copied them: always)
// ("handover dropmask <hex>|all", H2m's diagnosis) only listed draws of pairs whose ordinal's bit is set dropped (bit o = 1 << o: main_region's
// session ordinals, given in order of each pair's first non-empty pass-1 e1 draw, at most 64; the subset's space); the others the game's own
std::atomic<uint64_t> g_ho_dropmask{~0ull};
Count g_ho_masked_kept;   // (listed draws kept by the mask: drawn by the game over S1)
Count g_ho_pair_n[64];    // (pass 2's non-empty e1 draws by ordinal on decided handover frames)
// (GOAL-single-pass-2 item 4, test only) "handover ref r1|h1" (r1 by default: G1' against R1, as before). h1: the handover's compare
// frames alternate, the 1st, 3rd, ... G1 frames (the decision refused, kHwG1: an 8f compare frame, S1 against C2 by compare_e1f), the
// 2nd, 4th, ... H1 frames (handed over; T -> R1 as before but no draw into R1; H, the game's four right after the handover, into R slice
// 0; compare_h1). "handover trim on|off" (off by default): a depth plane no listed pass-1 e1 draw's pipeline writes (pso's kDepthWrite,
// kStencilWrite: depth or stencil enabled with a write mask) is not copied; FSC, velocity and reactivity as before
std::atomic<bool> g_ho_ref_h1{false}, g_ho_trim{false};
uint64_t g_ho_alt = 0;  // (the playback thread) handover compare frames latched under ref h1: even G1, odd H1
bool g_ho_alt_ref = false;  // (the playback thread) the switch as the last handover frame latched it
Count g_ho_g1alt, g_ho_h, g_ho_trim_depth, g_ho_trim_stencil;  // (G1 frames refused, H copies, the planes trim did not copy)
Count g_ho_g1_refused, g_ho_h1_refused;  // (G1 and H1 frames the decision refused for another reason: two-pass, 8f compare frames)
// (r120 HG3, test only, compare frames) r1sync: a full flush (a null aliasing barrier and a null UAV barrier) after T -> R1, before R1's
// first draw; r1dsv game: R1's draws bind exactly the DSV descriptor the game bound for that draw (t_bind's, whatever its flags: the game
// binds a writable DSV over a read state) when both planes are in read states now (the follow); else the compare refused
std::atomic<bool> g_ho_r1sync{false}, g_ho_r1dsv_game{false};
Count g_ho_r1sync_n, g_ho_r1dsv_draws, g_ho_r1dsv_refused, g_ho_r1_other_list, g_ho_dec_n[64], g_ho_dec_noord;
Count g_ho_r1_dsw, g_ho_r1_dsw_n[64], g_ho_r1dsv_kept_r;  // (kept_r: r1dsv game's writing draws kept on R's read-only views)  // (r120 HG3) compare frames' R1 draws whose pipeline writes depth or stencil, and by ordinal
enum HoState : int { kHsNone, kHsRefused, kHsHanded };
enum HoWhy : int { kHwExcluded, kHwNotClean, kHwWhole, kHwStrict, kHwThread, kHwPred, kHwQuery, kHwBind, kHwFmt, kHwDepth, kHwView, kHwRoot,
                   kHwMerge, kHwSrv, kHwHeld, kHwRemap, kHwTable, kHwQueryHold, kHwTableHandle, kHwG1, kHws };
const char* const kHwName[kHws] = {"excluded", "not_clean", "whole", "strict", "thread", "pred", "query", "bind", "fmt", "depth", "view", "root",
                                   "merge", "srv", "held", "remap", "table", "query_hold", "table_handle", "g1frame"};
// (the review's NIT) a query begun or ended inside a dropped run holds the handover off for this many presents after it (counted)
constexpr uint64_t kHoQueryHold = 120;
std::atomic<uint64_t> g_ho_query_until{0};
// (the review's NIT) pass 1's own draws' keys (keys::make, with the vertex buffer): a pass-2 draw with one of them is never dropped (S1
// lacks it); more than kHoOwnKeys own draws in pass 1 refuse the decision
constexpr int kHoOwnKeys = 512;
Count g_ho_x[kHws];
// frames latched, decided (by mode), drops, pass 2's own draws, misses and no-ordinal draws drawn by the game, indirect draws in pass 2,
// predicated drops, frames whose pass 1 twinned under a live predicate and pass 2 dropped (an upper bound of eye 1's occlusion predicate
// hiding eye 2's geometry), queries in the run, strict exclusions, merges, velocity and reactivity handed over, FXAA's not, R1's draws
Count g_ho_frames, g_ho_handed, g_ho_nohand, g_ho_ident, g_ho_dropped, g_ho_own2, g_ho_miss2, g_ho_noord2, g_ho_indirect, g_ho_pred_drops,
    g_ho_pred_risk, g_ho_query_in_run, g_ho_strict_excl, g_ho_merges, g_ho_velreact, g_ho_fxaa_skip, g_ho_r1_draws, g_ho_t, g_ho_r1_skipped;
std::atomic<uint64_t> g_ho_drop_last{0};
char g_ho_why[200] = "";
uint64_t g_ho_own_keys[kHoOwnKeys];  // (the playback thread; this frame's pass 1)
// (H2m's diagnosis, test only: "prepass mi region e1f handover save <dir>|off") one-shot: the first compared handover frame whose G1' differs
// saves C's, R1's and S1's FullScreenCopy and the player's own pass-2 draws' mask (R0's FullScreenCopy: on compare frames while armed, R0's
// colour is cleared at T and each own pass-2 draw is drawn again into it, depth-tested against R1's depth through a read-only view), with
// a .txt of the differing texels (at most 400); the split of FSC's differing texels: S1 covered / the sentinel / under the own mask
std::atomic<bool> g_ho_sv_want{false};
std::mutex g_ho_sv_mutex;
std::string g_ho_sv_dir, g_ho_sv_last = "none";
Count g_ho_mask_draws, g_hc_fsc_cov_diff, g_hc_fsc_unc_diff, g_hc_fsc_own_diff;
Count g_ho_own_keep;  // (pass-2 draws found among pass 1's keys but one of pass 1's own: drawn by the game)
// the merge pipelines, one per root signature (made on a worker, pso::OwnCreation)
struct HoMerge {
    ID3D12RootSignature* root = nullptr;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    std::atomic<int> state{0};  // 0 free, 1 being made, 2 ready, -1 refused (for the session)
    ID3D12PipelineState* pso = nullptr;
    char why[160] = "";
};
constexpr int kHoMerges = 8;
HoMerge g_hm[kHoMerges];
std::mutex g_ho_compile_mutex;
ID3DBlob* g_ho_vs = nullptr;  // (under g_ho_compile_mutex, once)
ID3DBlob* g_ho_ps = nullptr;
bool g_ho_compile_tried = false;
// S1's FSC SRV: one partition-1 record of the game's DescAlloc (MainSlice's route), kept for the session, written again kHold presents
// after an arrays' re-make; a fault or a bad record stands the handover down for the session
eye2_images::DescRec g_ho_rec{};
bool g_ho_rec_have = false;
std::atomic<bool> g_ho_stood{false};
std::atomic<uint32_t> g_ho_idx{0};
uint64_t g_ho_srv_gen = 0, g_ho_remake = 0;
bool rm_end(ID3D12GraphicsCommandList* now, int reason, unsigned n = 0, const D3D12_RESOURCE_BARRIER* b = nullptr);  // (below)
void rm_lost(int reason, const char* why);
bool rm_open_ok(ID3D12GraphicsCommandList* cl);
void ho_r1_draw(ID3D12GraphicsCommandList* cl, const d3d::DrawArgs& args);  // ([Stereo] MainRegionHandover, below)
// (the playback thread) C2's guards: pass 2's last e1 draw's list and bind generation; C2 owed by pass 2's e1 end marker (taken at the
// next list call on that thread, poll)
ID3D12GraphicsCommandList* g_e1f_cl = nullptr;
uint64_t g_e1f_bgen = 0;
bool g_e1f_owed = false;
DWORD g_e1f_owed_tid = 0;
// (item 8e 14, test only) the region trace's state (rg_trace and tr_copy, below)
constexpr int kTrSlots = 64;
constexpr uint64_t kTrWait = 8;
enum : int { kTrOff, kTrArmed, kTrRec, kTrDone, kTrSetup };  // (Setup: the command thread writing the fields)
std::atomic<int> g_tr_st{kTrOff};
std::atomic<int> g_tr_ord{-1};
UINT g_tr_x0 = 0, g_tr_y0 = 0, g_tr_w = 0, g_tr_h = 0, g_tr_pitch = 0;  // (written only in kTrSetup)
UINT g_tr_bx0 = 0, g_tr_by0 = 0, g_tr_bw = 0, g_tr_bh = 0;  // (the playback thread's snapshot, taken at Armed -> Rec)
ID3D12Device* g_tr_dev = nullptr;  // (the device the readback was made on)
std::atomic<uint32_t> g_tr_why{0};  // (the traced frame's g_f.why at pass 2's close, and bit 31: g_r.ok false)
std::mutex g_tr_mutex;
std::string g_tr_dir, g_tr_last;
ID3D12Resource* g_tr_rb = nullptr;  // (the playback thread makes it; the command thread reads it once kTrDone and waited)
uint64_t g_tr_slot_bytes = 0;
struct TrSlot {
    int pass, n;
    d3d::DrawArgs a;
};
TrSlot g_tr_slot[kTrSlots];
std::atomic<int> g_tr_n{0};
int g_tr_pn[3] = {};
std::atomic<uint64_t> g_tr_frame{0}, g_tr_done{0};
Count g_tr_lost, g_tr_refused;
// (item 8e 14) with each traced draw's box, the images it sampled (eye2_light::trace_images: pass 1 SC2L and LB2, pass 2 the game's
// collector and Light Buffer), for the first kTrImgPass traced draws of each pass; each image slot at most g_tr_img_cap bytes
constexpr int kTrImgPass = 8, kTrImgs = 2 * kTrImgPass;
struct TrImg {
    int slot;  // (the RT slot it goes with)
    D3D12_SUBRESOURCE_FOOTPRINT fp[2];
};
ID3D12Resource* g_tr_rbi = nullptr;  // (made with g_tr_rb, on its device)
uint64_t g_tr_img_cap = 0;
TrImg g_tr_img[kTrImgs];
std::atomic<int> g_tr_ni{0};
int g_tr_pi[3] = {};
Count g_tr_img_refused;
std::atomic<const char*> g_tr_img_why{""};  // (the last refusal's reason: a literal)
// (GOAL-single-pass-2, test only) "prepass mi region e1f g1trace <absolute dir> <x0> <y0> <w> <h>" | "g1trace off" | "g1trace": one 8f
// compare frame's per-draw history of a FullScreenCopy box (rg_trace's pattern, every draw). Armed, the next e1f compare frame not to be
// handed over (no handover latched, or ref h1's G1 frame) records: stream 1, S's slice 1 after each pass-1 twin; stream 2, the game's own
// FSC (the four's, as bound) at pass 2's first e1 draw before it (T) and after each pass-2 e1 draw the game issues; stream 3, S's slice 1
// after each player's own draw drawn again into it in pass 2. Each copy on the region's list, RENDER_TARGET -> COPY_SOURCE -> back, never
// under a predicate (a live one nulled around the copy and put back; a predicated list whose record is not known: that slot refused). A
// slot not copied (the four not bound, a split barrier begun on them, the box or the format) is kept, marked, so the orders stay. At
// pass 2's close the trace waits kTrWait presents, then "g1trace" writes the streams and a manifest
constexpr int kGtStreams = 3, kGtCapMax = 2048;
constexpr UINT kGtBoxMax = 64;
constexpr uint64_t kGtBytes = 96ull << 20;  // (the readback's cap: the slots per stream are min(2048, this / (3 * a slot's bytes)))
struct GtSlot {
    bool ok = false, t = false, own = false, noord = false, miss = false;
    int ord = 0;  // (each slot's set from its draw: -1 none; 0 here keeps the table zero-initialised)
    d3d::DrawArgs a{};
};
std::atomic<int> g_gt_st{kTrOff};
UINT g_gt_x0 = 0, g_gt_y0 = 0, g_gt_w = 0, g_gt_h = 0;      // (written only in kTrSetup)
UINT g_gt_bx0 = 0, g_gt_by0 = 0, g_gt_bw = 0, g_gt_bh = 0;  // (the playback thread's snapshot at Armed -> Rec)
std::mutex g_gt_mutex;
std::string g_gt_dir, g_gt_last;
ID3D12Resource* g_gt_rb = nullptr;  // (made at the arming, the command thread; never in use then: armed only from off)
ID3D12Device* g_gt_dev = nullptr;
UINT g_gt_pitch = 0, g_gt_rbw = 0, g_gt_rbh = 0;
uint64_t g_gt_slot_bytes = 0;
int g_gt_cap = 0;
GtSlot g_gt_slot[kGtStreams][kGtCapMax];
std::atomic<int> g_gt_n[kGtStreams] = {};
std::atomic<uint64_t> g_gt_frame{0}, g_gt_done{0};
std::atomic<uint32_t> g_gt_why{0};  // (the frame's g_f.why at pass 2's close; bit 31 g_r.ok false; bit 30 pass 2 never closed)
Count g_gt_lost[kGtStreams], g_gt_refused, g_gt_pred_unknown, g_gt_frames;
std::atomic<const char*> g_gt_rwhy{""};  // (the last refused slot's reason: a literal)
// (the playback thread) the brackets open (a depth: a nested entity's), the completed opens per region pass, and a bracket left open at
// a region's close; a frame whose passes differ in either is excluded (kXRgOwn: S1 and R1 would hold different draws)
int g_own_pb = 0;
int g_own_n[3] = {};
bool g_own_bad = false;
Count g_rg_own, g_own_marks, g_own_mark_failed, g_rg_x_own, g_rg_seed_early;
void cb_own1(void*);
void cb_own0(void*) {
    if (g_own_pb > 0) --g_own_pb;
    else g_own_bad = true;
}
Count g_rg_frames, g_rg_dup, g_rg_nondup, g_rg_noord, g_rg_twin, g_rg_swap_draws, g_rg_swap_frames, g_rg_qi, g_rg_qi_hit, g_rg_qi_failed,
    g_rg_mrv, g_rg_mrv_stale, g_rg_mrv_twice, g_rg_x_verdict, g_rg_x_unj, g_rg_x_gate, g_rg_x_single, g_rg_x_miss, g_rg_x_noord, g_rg_good[kRgModes],
    g_rg_ctl_frames[kCk], g_rg_marg_frames, g_rg_rep, g_rg_unjb[8], g_rg_gateb[16];
// (S1, S3, S5) bind formats unlike the arrays' views, the slot's formats or NumRenderTargets unlike them; ip twins drawn (each one's
// "ok" marker then spent); main_region's frame number pushed outside its region, a verdict whose frame is not the latched one
Count g_rg_x_vf, g_rg_x_fmt, g_rg_ip_twins, g_rg_mro, g_rg_mro_stale, g_rg_mrv_frame;
Count g_rg_tsc, g_rg_tsc_n;  // (E5) rdtsc around the listed path (before and after the game's draw), dup frames' draws
std::atomic<uint64_t> g_rg_tsc0{0};
std::atomic<int64_t> g_rg_qpc0{0};
// the worker's, by mode (kRgJoint .. kRgSubset) and control kind; per pair by ordinal (solo frames, joint frames it drew in, swaps)
Count g_rc_frames[kRgModes], g_rc_e2[kRgModes], g_rc_e1[kRgModes], g_rc_neg[kRgModes], g_rc_cov[kRgModes], g_rc_react0[kRgModes], g_rc_pass[kRgModes],
    g_rc_ck_frames[kRgModes][kCk], g_rc_ck_e2[kRgModes][kCk], g_rc_ck_e1[kRgModes][kCk], g_rc_swap_frames, g_rc_swap_differ, g_rc_layout,
    g_rc_rep[kRgModes], g_rc_rep_e2[kRgModes];  // (the compared frames with 8d's rep, and those eye-2 exact: bisect step 0)
std::atomic<uint64_t> g_rc_cov_min[kRgModes] = {~0ull, ~0ull, ~0ull, ~0ull}, g_rc_cov_max[kRgModes], g_rc_react_max[kRgModes],
    g_rc_ck_min[kCk] = {~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull};
struct PairRes {
    Count frames, e2, e1, neg, cov, react0, joint, joint_exact, swap, swap_differ;
    Count ck_frames[kCk], ck_e2[kCk], ck_e1[kCk];
    Count ck_cov[kCk], ck_cov_e2[kCk];  // (V15's amendment) the control frames the solo pair covered (R1 written), and of them eye 2 differing
    std::atomic<uint64_t> cov_min{~0ull}, cov_max{0}, swap_min{~0ull};
};
PairRes g_pr[kOrds];
// (the playback thread) the list's ID3D12GraphicsCommandList1, one QueryInterface per list per region (its reference held to the
// region's close or the list's Reset / Close; only ever used on the same list and thread)
struct C1Cache {
    ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12GraphicsCommandList1* c1 = nullptr;
};
thread_local C1Cache t_c1;
void c1_drop() {
    if (t_c1.c1) t_c1.c1->Release();
    t_c1 = C1Cache{};
}
ID3D12GraphicsCommandList1* c1_of(ID3D12GraphicsCommandList* cl) {
    if (t_c1.cl == cl && t_c1.c1) {
        bump(g_rg_qi_hit);
        return t_c1.c1;
    }
    c1_drop();
    ID3D12GraphicsCommandList1* c1 = nullptr;
    if (FAILED(cl->QueryInterface(IID_PPV_ARGS(&c1))) || !c1) {
        bump(g_rg_qi_failed);
        return nullptr;
    }
    bump(g_rg_qi);
    t_c1.cl = cl;
    t_c1.c1 = c1;
    return c1;
}

// ---- the capture (one in flight), the readback and the worker's job
enum Cap : int { kIdle, kArmed, kReady, kWait };
std::atomic<int> g_cap{kIdle};
std::atomic<int> g_cap_left{0};
std::atomic<uint64_t> g_ready_frame{0};
constexpr int kPlanes = 5;  // FullScreenCopy, Velocity, Reactivity, depth (plane 0), stencil (plane 1)
constexpr int kSlots = kSets * 2 * kPlanes + kPlanes;  // (+ C's five planes, [Stereo] MainSlice)
int slot(int set, int slice, int plane) { return (set * 2 + slice) * kPlanes + plane; }
int slot_c(int plane) { return kSets * 2 * kPlanes + plane; }
struct Rb {
    ID3D12Resource* rb = nullptr;
    uint64_t gen = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[kSlots]{};
    UINT rows[kSlots]{};
    UINT64 rowbytes[kSlots]{};
    UINT64 size = 0;
    bool has_c = false;  // (C's slots laid out: item 8f makes C after the arrays, at an e1f frame's first seed)
};
Rb g_rb;  // (the playback thread)
struct Job {
    ID3D12Resource* rb = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[kSlots]{};
    UINT rows[kSlots]{};
    UINT64 rowbytes[kSlots]{};
    UINT64 size = 0;
    bool ctl = false;
    int listed = 0;
    int slice = 0;  // ([Stereo] MainSlice) the frame's latched mode: C and R1 hold the handover's (G1)
    // ([Stereo] MainFamily=region, item 8e) the frame's mode (0: not a region frame), its solo ordinal, its swapped ordinal (-1 none),
    // its control kind (0 none) and the pairs that drew in its pass 1 (by ordinal)
    int rmode = 0, solo = -1, swap = -1, ck = 0;
    uint64_t drew = 0;
    bool rep = false;  // (8d's rep: a pass-1 key drawn twice in the frame; bisect step 0)
    int listed2 = 0;     // (item 8e's diagnosis) pass 2's listed draws (region frames)
    uint64_t frame = 0;  // (state::presents at the readback)
    // ([Stereo] MainRegionE1Compare, item 8f) an e1f frame (compare_e1f: R0 is T, R1 the seed, C is C2), under DLSS (velocity and
    // reactivity compared), the single control's ordinal
    bool e1f = false, dlss = false;
    int single = -1;
    bool ho = false;  // ([Stereo] MainRegionHandover, item 8g: compare_handover, C against R1) and its mode (1 on, 2 nohandover, 3 identity)
    int ho_mode = 0;
    bool ho_mask = false;  // (H2m's diagnosis: R0's FSC holds the own pass-2 draws' mask)
    bool h1 = false;       // (GOAL-single-pass-2 item 4: compare_h1, R0 is H, R1 is T) and the planes the decision copies (bit per plane)
    uint32_t h1_cp = 0;
    unsigned twin_mask = 3;  // (the frame's twins' view-instance mask: not 3, S0 holds no twin and the negative is not measured)
    int alt = 0;  // (an e1f compare of ref h1's: 1 a G1 frame, 2 a G1 frame refused for another reason, 3 an H1 frame refused)
} g_job;
ID3D12Fence* g_fence = nullptr;
std::atomic<UINT64> g_fence_value{0};
HANDLE g_fence_event = nullptr;
std::mutex g_setup_mutex;

// ---- counters
// the render thread
Count g_r_opens[3], g_r_open_failed, g_r_closes, g_r_close_failed, g_r_gate_bad[3], g_r_unready, g_r_ctl, g_r_ctl_failed, g_r_ctl_restore_failed;
Count g_dg_equal, g_dg_differ, g_dg_unread;
// the playback thread
Count g_p_opens[3], g_p_unclosed, g_p_order, g_p_close_copy, g_p_close_marker, g_p_listed[3], g_p_predicated, g_p_refused, g_p_seeds,
    g_p_seed_e2_array, g_p_seed_followed, g_p_twin, g_p_ref[3], g_p_no_twin, g_p_no_cl1, g_p_made, g_p_make_failed, g_p_other_thread;
Count g_f_good, g_f_bad, g_f_x_gate, g_f_x_pred, g_f_x_refused, g_f_x_counts, g_f_x_contig, g_f_x_seed, g_f_x_order, g_f_x_none, g_f_first_same;
Count g_f_x_hand, g_f_x_barrier, g_f_x_extra, g_f_x_query;  // ([Stereo] MainSlice)
Count g_cap_started, g_cap_incomplete, g_cap_done, g_rb_made, g_rb_failed, g_pending_late, g_rb_deferred, g_mask_known, g_mask_unknown;
std::atomic<uint64_t> g_last_listed[3], g_last_first[3];
// the worker
Count g_c_frames, g_c_pass, g_c_e2_exact, g_c_e1_exact, g_c_neg_differ, g_c_react_zero, g_c_cov_ok, g_c_ctl, g_c_ctl_e2_differ, g_c_ctl_e1_exact,
    g_c_map_failed, g_c_timeout, g_c_layout;
std::atomic<uint64_t> g_c_e2_max[kPlanes], g_c_e1_max[kPlanes], g_c_e2_worst{0}, g_c_cov_min{~0ull}, g_c_cov_max{0}, g_c_neg_min{~0ull},
    g_c_react_max{0}, g_c_ctl_e2_min{~0ull};
std::atomic<uint32_t> g_c_where[6] = {};  // the last eye-2 difference: plane, x0, y0, x1, y1, its texels (low 32 bits)
constexpr uint64_t kCovMin = 1000;
// ([Stereo] MainSlice) the worker's: G1 (C against R1), the coverage splits, the controls (0 the rows: nudge or eye1; 1 nomerge;
// 2 always): their frames, the frames with their case, those failing G1, eye 1 exact
Count g_c_id_frames, g_c_id_exact, g_c_id_e1;  // (the identity control: C taken at the run's end, no drop, must equal R1)
Count g_c_sl_frames, g_c_sl_pass, g_c_g1_exact, g_c_sl_marked, g_c_pass_frames, g_c_fail_frames, g_c_sc_frames[3], g_c_sc_case[3], g_c_sc_fail[3],
    g_c_sc_e1[3];
std::atomic<uint64_t> g_c_g1_max[kPlanes], g_c_g1_worst{0}, g_c_marked_min{~0ull}, g_c_marked_max{0}, g_c_pass_tex{0}, g_c_fail_tex{0},
    g_c_sc_min[3] = {~0ull, ~0ull, ~0ull};
std::atomic<uint32_t> g_c_g1_where[6] = {};

// ---- the render thread: the markers (op-0x4a callbacks) and the positive control
thread_local bool t_open[3] = {};  // pass p's open marker recorded: its close owed
thread_local bool t_ctl = false;   // eye 2's rows changed for pass 1's DVL call 1 (put back at its close)
void cb_open1(void*);
void cb_open1_ctl(void*);
void cb_open1_x(void*);
void cb_open2(void*);
void cb_open2_x(void*);
void cb_close(void*);

// the shadow-constant diagnostic (billboard.md 3.5 / 8.5.5): gCameraMatrix (rage_shadowmap_common rows 0-3), Shadowmap_PixelSize0
// (row 9) and global_textures uints 14 and 38, as the render thread's storage holds them at DVL call 1's end of each pass
struct Diag {
    float cam[16], px[4];
    uint32_t tex[2];
    bool ok;
};
Diag g_dg1{};  // (the render thread) pass 1's
bool global_bytes(Id id_anchor, void* out, size_t n) {
    int id = 0;
    uintptr_t cb = 0, storage = 0;
    int off = -1;
    const uintptr_t table = anchors::addr(Id::ShaderGlobalTable);
    if (!anchors::present(id_anchor) || !rd(anchors::addr(id_anchor), &id) || id <= 0 || id > 4096 ||
        !rd(table + static_cast<uintptr_t>(id) * 16, &cb) || !cb || !rd(table + static_cast<uintptr_t>(id) * 16 + 8, &off) || off < 0 ||
        off > 0x1000 || !rd(cb, &storage))
        return false;
    return rd_raw(storage + static_cast<uintptr_t>(off), out, n);
}
Diag read_diag() {
    Diag d{};
    uintptr_t gt = 0, storage = 0;
    d.ok = global_bytes(Id::ShadowCamMtxId, d.cam, sizeof(d.cam)) && global_bytes(Id::ShadowPixelSizeId, d.px, sizeof(d.px)) &&
           anchors::present(Id::GlobalTexturesCb) && rd(anchors::addr(Id::GlobalTexturesCb), &gt) && gt && rd(gt, &storage) &&
           rd(storage + 14 * 4, &d.tex[0]) && rd(storage + 38 * 4, &d.tex[1]);
    return d;
}

// ---- (run 25, the FXAA velocity finding) the rows diagnostic, the render thread, read only: the rows the twin reads for
// eye 2 in pass 1 (rage_prev_proj rows 4-7 gPrevViewProj, b1 rows 25-28 gViewInverse, 17-20 gWorldView) against the rows
// pass 2's own draws read (rows 0-3, 12-15, 4-7), as the CPU storage holds them at DVL call 1's open (each CB is uploaded
// from its storage at its draws' commits); gPrevViewProj also at the close (changed inside DVL call 1: "unstable"). [I]
// gWorldView is per draw (World x View): at the open it is the last draw's, so its row is information, not a verdict.
// "prepass mi ppfix on|off" (on by default with the step, run 25): eye 2's rows 4-7 copied again from rows 0-3 at pass 1's DVL call 1 open
// when this frame's eye-2 gPrevViewProj was a copy (no TAA update: FXAA), before the twin's draws record.
enum RowQ : int { kQPp, kQVi, kQWv, kQs };
const char* const kQName[kQs] = {"gPrevViewProj", "gViewInverse", "gWorldView"};
struct Rows {
    float q[kQs][16];
    float pp_close[16];
    int src = 0;
    bool ok = false, ctl = false, closed = false;
    uint64_t frame = 0;
};
Rows g_rw1;  // (the render thread) pass 1's
// (run 25, r52: under FXAA, eye 2's rows 4-7 are camera_lever's copy made before pass 1, one frame stale in motion: pass 2's
// rows 0-3 differed in 286 of 481 frames and eye 2's velocity by 1 LSB; refreshed at DVL call 1, equal 503/503 and eye 2
// bit-exact. On by default with the step; "prepass mi ppfix off" leaves them as camera_lever wrote them)
std::atomic<bool> g_ppfix{true};
Count g_rw_frames, g_rw_unread, g_rw_ctl, g_rw_src[3], g_rw_eq[kQs], g_rw_ne[kQs], g_rw_ne_src[3], g_rw_unstable, g_rw_fixed, g_rw_fix_failed;
Count g_rw_fix_changed;  // (the review) ppfix frames whose rows 4-7 differed from rows 0-3 before the copy
std::atomic<uint64_t> g_rw_ulp[kQs];
std::atomic<uint32_t> g_rw_first[kQs];  // the first differing element of the last differing frame: row * 4 + column (+1; 0 none)
uint64_t ulp(float a, float b) {
    int32_t x = 0, y = 0;
    std::memcpy(&x, &a, 4);
    std::memcpy(&y, &b, 4);
    const int64_t ox = x < 0 ? static_cast<int64_t>(INT32_MIN) - x : x, oy = y < 0 ? static_cast<int64_t>(INT32_MIN) - y : y;
    return static_cast<uint64_t>(ox > oy ? ox - oy : oy - ox);
}
void rows_open(int pass) {
    Rows r;
    float pp[16], vi[16], wv[16];
    r.ok = prepass::main_rows(pass, pp, vi, wv, &r.src);
    if (r.ok) {
        std::memcpy(r.q[kQPp], pp, 64);
        std::memcpy(r.q[kQVi], vi, 64);
        std::memcpy(r.q[kQWv], wv, 64);
    }
    r.frame = state::presents.load();
    if (pass == 1) {
        g_rw1 = r;
        return;
    }
    if (!r.ok || !g_rw1.ok || g_rw1.frame != r.frame || !g_rw1.closed) {
        bump(g_rw_unread);
        g_rw1.ok = false;
        return;
    }
    if (g_rw1.ctl) {  // (the positive control's rows: not this diagnostic's)
        bump(g_rw_ctl);
        g_rw1.ok = false;
        return;
    }
    bump(g_rw_frames);
    const int src = g_rw1.src >= 0 && g_rw1.src <= 2 ? g_rw1.src : 0;
    bump(g_rw_src[src]);
    char line[640];
    int n = std::snprintf(line, sizeof(line), "[mi] rows (eye 2's in pass 1 against pass 2's, gPrevViewProj %s):",
                          src == 1 ? "from the TAA update" : src == 2 ? "copied from rows 0-3 (no jitter)" : "not written this frame");
    bool any = false;
    for (int q = 0; q < kQs; ++q) {
        uint64_t worst = 0;
        int first = -1;
        for (int i = 0; i < 16; ++i) {
            if (!std::memcmp(&g_rw1.q[q][i], &r.q[q][i], 4)) continue;
            if (first < 0) first = i;
            const uint64_t d = ulp(g_rw1.q[q][i], r.q[q][i]);
            if (d > worst) worst = d;
        }
        if (first < 0) {
            bump(g_rw_eq[q]);
            n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " %s equal;", kQName[q]);
        } else {
            bump(g_rw_ne[q]);
            if (q == kQPp) bump(g_rw_ne_src[src]);
            store_max(g_rw_ulp[q], worst);
            g_rw_first[q].store(static_cast<uint32_t>(first + 1), std::memory_order_relaxed);
            uint32_t a = 0, b = 0;
            std::memcpy(&a, &g_rw1.q[q][first], 4);
            std::memcpy(&b, &r.q[q][first], 4);
            n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " %s differs (max %llu ULP; first row %d col %d: %08x / %08x);",
                               kQName[q], static_cast<unsigned long long>(worst), first / 4, first % 4, a, b);
            any = true;
        }
        if (n >= static_cast<int>(sizeof(line))) n = static_cast<int>(sizeof(line)) - 1;
    }
    const bool unstable = std::memcmp(g_rw1.q[kQPp], g_rw1.pp_close, 64) != 0;
    if (unstable) bump(g_rw_unstable);
    const int cs = g_cap.load();
    (void)any;  // (the review: gWorldView differs every frame, per draw; logged only while a capture is armed)
    if (cs != kIdle) log::limited("mi.rows", 60, "%s pass 1's gPrevViewProj %s inside DVL call 1", line, unstable ? "changed" : "unchanged");
    g_rw1.ok = false;
}
void rows_close(int pass) {  // (pass 1: gPrevViewProj's eye-2 rows again, after DVL call 1's draws were recorded)
    if (pass != 1 || !g_rw1.ok) return;
    float vi[16], wv[16];
    int src = 0;
    g_rw1.closed = prepass::main_rows(1, g_rw1.pp_close, vi, wv, &src);
}

// ---- the playback thread: the region and the frame
struct Region {
    bool open = false;
    int pass = 0;
    bool ok = false;   // duplicates drawn in this region (the gate held and nothing refused so far)
    bool ctl = false;  // pass 1 with the positive control
    DWORD tid = 0;
    uint64_t d0 = 0;   // the thread's draws at the open
    int listed = 0;
    uint64_t first = 0, last = 0;
    bool contig = true;
    int twins = 0;        // pass 1: twin draws made in this region
    int would = 0;        // (item 6c's check) pass 2: listed draws a drop would take (pso::main_would_drop)
    bool handed = false;  // ([Stereo] MainSlice) pass 2: merged at the first listed draw; listed draws up to listed1 dropped
    bool cmp = false;     // (a handed-over compare frame: T in R slice 1, C taken, the references drawn into R slice 1)
    bool in_run = false;  // (between the first and the last dropped draw: a barrier on the targets excludes the frame)
    int dropped = 0;
    const void* pso0 = nullptr;  // the first listed draw's pipeline; mixed: another one listed later in the region
    bool mixed = false;
    bool ident = false;  // ([Stereo] MainSlice, the identity control) pass 2: T taken at the first listed draw, C at listed1's, no drop
    bool uc = false;     // ([Stereo] MainUnionCull) pass 1: the render thread's union marker played in this region
    int cur_ord = -1;    // ([Stereo] MainFamily=region) the listed draw's ordinal, before_draw to after_draw
    // ([Stereo] MainRegionE1Compare, item 8f) pass 2: a barrier on the four watched (T taken, C2 not yet); this draw is a player's own
    // draw to draw again into S1 (before_draw to after_draw)
    bool e1f_watch = false, e1f_own = false;
    bool ho_r1 = false;  // ([Stereo] MainRegionHandover, item 8g) a compare frame's pass-2 draw drawn by the game: again into R1 after it
    bool ho_own = false;  // (H2m's diagnosis) that draw is one of the player's own: also into the mask (R0's colour)
    bool e1_done = false;  // (H2m's diagnosis) this pass's e1 end marker played (mr_e1_end)
    bool ho_r1_w = false;  // (r120 HG3) the current R1 draw's pipeline writes depth or stencil (ho_before2, for ho_r1_draw)
    bool gt_after = false;  // ("e1f g1trace") a traced pass-2 draw: its boxes copied in after_draw (gt_meta its slot's fields)
    GtSlot gt_meta;
    bool c2_nulled = false;  // ("e1f c2pred null") this pass-2 draw's predicate nulled before the game's, c2_pd put back in after_draw
    d3d::Predicate c2_pd;
} g_r;
void cb_own1(void*) {
    ++g_own_pb;
    if (g_r.open && g_r.pass >= 1 && g_r.pass <= 2) ++g_own_n[g_r.pass];
}
enum : uint32_t {
    kXGate = 1, kXPred = 2, kXRefused = 4, kXCounts = 8, kXContig = 16, kXSeed = 32, kXOrder = 64, kXNone = 128,
    kXHand = 256, kXBarrier = 512, kXExtra = 1024,  // ([Stereo] MainSlice) not handed over; a barrier in the run; listed draws past listed1
    kXQuery = 2048,  // ([Stereo] MainSlice) a query begun or ended inside the dropped run
    // ([Stereo] MainFamily=region, item 8e) no verdict of 8d's for the frame; not judged; a failed gate; a single (joint: anywhere in
    // e1; solo / subset: in a listed pair); a miss (not within); (joint) a draw of a pair without an ordinal (never listed)
    kXRgVerdict = 4096, kXRgUnj = 8192, kXRgGate = 16384, kXRgSingle = 32768, kXRgMiss = 65536, kXRgNoOrd = 131072,
    kXRgOwn = 262144,  // (item 8e 14) the passes' own-entity brackets differ (a marker failed, one left open, or counted differently)
    // ([Stereo] MainRegionE1Compare, item 8f) slice 1's seed refused (D2L or V2 not fresh, another thread, a desc, the technique); T not
    // taken at pass 2's e1 start; C2 not taken at its e1 end
    kXE1fSeed = 524288, kXE1fT = 1048576, kXE1fC2 = 2097152,
    kXE1fRemap = 4194304,  // (item 8f's remap) a close not made: eye 1 lacked the skipped draws
    kXHo = 8388608  // (item 8g) the compare's exclusion: a predicated drop, a query in the run, a strict own draw or miss, R1 not drawable
};
struct Frame {
    int state = 0;  // 0 none, 1 pass 1 open, 2 pass 1 closed, 3 pass 2 open
    uint32_t why = 0;
    bool ctl = false, seeded = false;
    uint64_t gen = 0;
    int listed1 = 0;
    uint64_t first1 = 0;
    bool contig1 = false;
    int slice = 0;  // ([Stereo] MainSlice) the mode latched at pass 1's open (0: an item-3 frame)
    int twins1 = 0;
    bool mixed1 = false;  // (pass 1's listed draws used more than one family pipeline: the merge has one pipeline's state, no handover)
    const void* pso1 = nullptr;  // (pass 1's listed pipeline: pass 2's first must be it)
    bool handed = false, cmp = false, hr_counted = false;
    bool c_taken = false;  // (C holds this frame's merged targets, or the identity control's at the run's end)
    // ([Stereo] MainFamily=region, item 8e) latched at pass 1's open: a region frame ("mr compare"), a dup frame, the marginal stamps,
    // the mode, the subset, the solo and swapped ordinals, the control kind; 8d's verdict pushed at pass 2's e1 (mr_verdict)
    bool rg = false, dup = false, marg = false, mr_have = false;
    bool c2null = false;  // ("e1f c2pred null") latched: a compare e1f frame whose pass-2 e1 draws go unpredicated
    bool gt = false, gt_t = false;  // ("e1f g1trace") the traced frame; T's box taken (pass 2's first e1 draw)
    unsigned dupall_mask = 3;  // ("dupall mask": the twins' view-instance mask on a dupall frame; 3 on any other)
    unsigned twin_mask = 3;    // (the mask rg_after's twins draw with: "twins mask 2" on an e1f frame, else dupall_mask)
    bool twin_mask_sw = false;  // (twin_mask from "twins mask 2")
    bool dupall = false, notwin = false;  // (E2/E3) a dup frame by "e1f dupall" only (no compare frame); its twins not drawn ("dupall twins off")
    bool ip = true;          // (S3) instprev twins handed out this frame ("prepass mi region ip")
    bool mr_fr_have = false;  // (S5) main_region's frame number for this frame (mr_open, cleared by rg_latch with the rest of g_f)
    uint64_t mr_frame = 0;
    int rmode = 0, solo = -1, swap = -1, ck = 0;
    uint64_t subset = 0, drew = 0;
    MrVerdict mr;
    int listed2 = 0;  // (item 8e's diagnosis) pass 2's listed draws, at its close
    // ([Stereo] MainRegionE1Compare, item 8f) latched at pass 1's open (rg_latch): an e1f frame, DLSS (else FXAA: the technique read
    // there), the twins under the live predicate, the single control's ordinal; T taken (tried: at pass 2's first non-empty e1 draw),
    // a barrier on the four between T and C2 (C2 is c_taken)
    bool e1f = false, e1f_dlss = false, e1f_tech_ok = false, e1f_live = true, e1f_t = false, e1f_t_tried = false, e1f_barrier = false,
         e1f_clear = false, e1f_write = false, e1f_split = false;  // (a clear, a copy into, or a split barrier begun on one of the four, T to C2)
    bool e1f_remap = false, e1f_rm_try = false, e1f_rm_ready = false, e1f_rm_started = false;  // (item 8f's remap: latched, tried, S0 seeded, on)
    bool e1f_rm_ro = false;  // (the depth's planes read-only beyond plain DEPTH_READ at the seed: g_rm_ro at the start)
    uint32_t e1f_rm_skip = 0, e1f_rm_after = 0;
    bool fs_diag = false;  // (r117 DG4's diagnostic: one of the first e1f frames after a mono -> stereo switch)  // (r116 G3's diagnosis: draws skipped while the remap stood; e1 draws after its close)
    bool e1f_cmoved[3] = {};
    // ([Stereo] MainRegionHandover, item 8g) latched in rg_latch: a region compare frame (rcmp: a capture armed, 1 in N; dup is rcmp or
    // ho), a handover frame and its mode and strictness; pass 2's decision state; pass 1's draws without an ordinal and its indirect draws
    // (the decision's rule), a twin under a live predicate (the risk count); pass 2's drops
    uint64_t ho_dropmask = ~0ull;
    bool ho_r1sync = false, ho_r1dsv_game = false;
    ID3D12GraphicsCommandList* ho_cl = nullptr;  // (r120 HG3) the decision's list (T -> R1 recorded on it)
    bool rcmp = false, ho = false, ho_strict = false, ho_vr_always = false, ho_ind1 = false, ho_pred1 = false, ho_mask = false;
    int ho_mode = 0, ho_state = 0;
    uint32_t noord1 = 0, ho_drops = 0;
    uint32_t ho_nown = 0;  // (pass 1's own draws' keys noted; past kHoOwnKeys the decision refused)  // (item 8f) a colour target transitioned since pass 2's last e1 draw (its last state after: e1f_cst)
    // (GOAL-single-pass-2 item 4) latched: a G1 or an H1 frame of ref h1's alternation, trim; pass 1's listed e1 draws' kDepthWrite and
    // kStencilWrite (or-ed); the planes the decision copies (bit per plane, whatever the mode: H1's expected)
    bool ho_g1 = false, ho_h1 = false, ho_trim = false, ho_g1_done = false;  // (g1_done: refused as a G1 frame, every check held)
    uint32_t ho_wr1 = 0, ho_cp = 0;
    uint32_t e1f_cst[3] = {};
    int e1f_single = -1;
} g_f;
bool g_pending = false;  // pass 2's region closed by its marker on a good captured frame: the readback at the next list call
DWORD g_pending_tid = 0;


// the list's last pipeline and bind on this thread (noted while armed or listening: the family's bind comes before the
// region opens, at segment e's MRT bind)
thread_local ID3D12GraphicsCommandList* t_pso_cl = nullptr;
thread_local ID3D12PipelineState* t_pso = nullptr;
struct Bind {
    ID3D12GraphicsCommandList* cl = nullptr;
    unsigned n = 0;
    bool single = false, has_ds = false;
    D3D12_CPU_DESCRIPTOR_HANDLE rts[8]{}, ds{};
};
thread_local Bind t_bind;
// ([Stereo] MainRegion, item 8d) check_bind's answer per note_bind: the generation moves at each bind (and when the list's notes go)
thread_local uint64_t t_bind_gen = 0, t_bind_ok_gen = ~0ull;
thread_local const ID3D12GraphicsCommandList* t_bind_ok_cl = nullptr;
thread_local bool t_bind_ok = false;

// ---- ([Stereo] MainUnionCull, item 6e) the playback thread's draw keys
inline uint64_t mix64(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    return h ^ (h >> 29);
}
uint64_t fin64(uint64_t h) {  // (a digest's term: a keyed key spread over 64 bits)
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    return h ^ (h >> 33);
}
// pass 1: counted in; pass 2: counted out (false: not found, a miss); full: the probe ran out (the frame not judged)
constexpr unsigned kKeyProbe = 64;
bool key_tab(keys::Tables& ts, int t, uint64_t key, bool in, bool* full, bool* rep = nullptr) {  // (rep: pass 1's key seen earlier this frame)
    const uint64_t k = key ? key : 1;
    uint32_t i = static_cast<uint32_t>(fin64(k));
    for (unsigned p = 0; p < kKeyProbe; ++p, ++i) {
        keys::Slot& s = ts.tab[t][i & (ts.slots - 1)];
        if (s.gen != ts.gen) {  // (an empty slot: the key is not in the table)
            if (!in) return false;
            s = keys::Slot{ts.gen, 1, k};
            return true;
        }
        if (s.key != k) continue;
        if (in) {
            ++s.n;
            if (rep) *rep = true;
            return true;
        }
        if (!s.n) return false;
        --s.n;
        return true;
    }
    *full = true;
    return in;
}
void uc_note(ID3D12GraphicsCommandList* cl, const d3d::DrawArgs& a) {  // a listed draw (either pass)
    uint64_t kv = 0, k = 0;
    keys::make(cl, t_pso, a, &kv, &k);
    keys::count(g_uc_keys, g_ucf, g_r.pass, kv, k);
}
struct Bound {
    ID3D12Resource* res[kKinds]{};
    DXGI_FORMAT vf[kKinds]{};
};

D3D12_RESOURCE_BARRIER tr(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    return x;
}
struct Batch {
    D3D12_RESOURCE_BARRIER b[24];
    UINT n = 0;
    void add(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES s) {
        if (r && a != s && n < 24) b[n++] = tr(r, a, s);
    }
    void add_sub(ID3D12Resource* r, UINT sub, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES s) {  // ([Stereo] MainSlice) one subresource
        if (!r || a == s || n >= 24) return;
        b[n] = tr(r, a, s);
        b[n++].Transition.Subresource = sub;
    }
    void flush(ID3D12GraphicsCommandList* cl) {
        if (n) d3d::raw_barrier(cl, n, b);
        n = 0;
    }
};
void copy_sub(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, UINT dsub, ID3D12Resource* src, UINT ssub) {
    D3D12_TEXTURE_COPY_LOCATION d{dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    d.SubresourceIndex = dsub;
    D3D12_TEXTURE_COPY_LOCATION s{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    s.SubresourceIndex = ssub;
    d3d::raw_copy_region(cl, &d, &s);
}

// the bound targets are the family's four (by name; their views' resources and formats)
const char* check_bind(ID3D12GraphicsCommandList* cl, Bound* b) {
    const Bind& x = t_bind;
    if (x.cl != cl) return "the list's last bind not seen";
    if (x.single || x.n != 3 || !x.has_ds) return "not three render targets and a depth";
    for (int k = 0; k < kKinds; ++k) {
        int f = 0;
        const void* r = d3d::view_resource(k < kDepth ? x.rts[k].ptr : x.ds.ptr, &f);
        if (!r || !d3d::resource_named(r, kNames[k])) return "a bound target is not the family's (FullScreenCopy, Velocity RT, Reactivity RT, the depth)";
        b->res[k] = static_cast<ID3D12Resource*>(const_cast<void*>(r));
        b->vf[k] = static_cast<DXGI_FORMAT>(f);
    }
    return nullptr;
}

bool heaps(ID3D12Device* dev) {
    if (g_rtv_heap && g_dsv_heap) return true;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kSets * 3 * 3, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (!g_rtv_heap && FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtv_heap)))) return false;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = kSets * 3;
    if (!g_dsv_heap && FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_dsv_heap)))) return false;
    g_rtv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_dsv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    return true;
}
void drop_arrays() {
    for (auto& set : g_a)
        for (Arr& a : set) {
            retire(a.arr);
            a = Arr{};
        }
    for (Arr& a : g_c) {  // ([Stereo] MainSlice)
        retire(a.arr);
        a = Arr{};
    }
}
// C ([Stereo] MainSlice's merged targets; item 8f's C2): the game's descs (one slice), at rest COPY_DEST; only ever a copy's destination
// and source. A failure retires the C's made so far (ensure's caller then drops the arrays too)
const char* make_c(const Bound& b, UINT planes) {
    ID3D12Device* dev = state::device.load();
    if (!dev) return "no device";
    for (int k = 0; k < kKinds; ++k) {
        Arr& a = g_c[k];
        const D3D12_RESOURCE_DESC gd = b.res[k]->GetDesc();
        D3D12_RESOURCE_DESC ad = gd;
        ad.Alignment = 0;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = b.vf[k] != DXGI_FORMAT_UNKNOWN ? b.vf[k] : gd.Format;
        cv.DepthStencil = {1.0f, 0};
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ad, D3D12_RESOURCE_STATE_COPY_DEST, k == kDepth ? &cv : nullptr,
                                               IID_PPV_ARGS(&a.arr))) ||
            !a.arr) {
            a.arr = nullptr;
            for (Arr& c : g_c) {
                retire(c.arr);
                c = Arr{};
            }
            bump(g_p_make_failed);
            return "a compare array (C) not made";
        }
        a.game = b.res[k];
        a.w = gd.Width;
        a.h = gd.Height;
        a.fmt = gd.Format;
        a.vf = cv.Format;
        a.planes = k == kDepth ? planes : 1;
        wchar_t nm[96];
        std::swprintf(nm, 96, L"RDRVR main slice merged %S", kNames[k]);
        a.arr->SetName(nm);
    }
    return nullptr;
}
// (item 8f) C for these targets on an e1f frame, made at its first seed (never with the switch or the command off); S and R made first
const char* ensure_c(const Bound& b) {
    bool same = true;
    for (int k = 0; k < kKinds; ++k) same = same && g_c[k].arr && g_c[k].game == b.res[k];
    if (same) return nullptr;
    for (Arr& c : g_c) {
        retire(c.arr);
        c = Arr{};
    }
    return make_c(b, g_a[kS][kDepth].planes);
}
// S and R for these targets (made again, the old ones retired, when the game's change)
const char* ensure(const Bound& b) {
    D3D12_RESOURCE_DESC d[kKinds];
    bool same = g_gen != 0;
    for (int k = 0; k < kKinds; ++k) {
        d[k] = b.res[k]->GetDesc();
        const DXGI_FORMAT vf = b.vf[k] != DXGI_FORMAT_UNKNOWN ? b.vf[k] : d[k].Format;
        for (int s = 0; s < kSets; ++s) {
            const Arr& a = g_a[s][k];
            same = same && a.arr && a.game == b.res[k] && a.w == d[k].Width && a.h == d[k].Height && a.fmt == d[k].Format && a.vf == vf;
        }
    }
    if (same) return nullptr;
    for (int k = 0; k < kKinds; ++k)
        if (d[k].Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d[k].DepthOrArraySize != 1 || d[k].MipLevels != 1 || d[k].SampleDesc.Count != 1 ||
            d[k].Width != d[0].Width || d[k].Height != d[0].Height)
            return "a target is not a single 2D texture of the others' size";
    ID3D12Device* dev = state::device.load();
    if (!dev) return "no device";
    if (!heaps(dev)) return "the descriptor heaps not made";
    D3D12_FEATURE_DATA_FORMAT_INFO fi{d[kDepth].Format, 0};
    const UINT planes = SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &fi, sizeof(fi))) && fi.PlaneCount ? fi.PlaneCount : 1;
    if (planes != 2) return "the depth has no stencil plane";
    drop_arrays();
    ++g_gen;
    if (g_srv_gen) g_remake_frame = state::presents.load();  // ([Stereo] MainSlice: the SRVs view the old arrays: the drop held, then rewritten)
    if (g_ho_srv_gen) g_ho_remake = state::presents.load();  // ([Stereo] MainRegionHandover: likewise its FSC SRV)
    // ([Stereo] MainSlice, installed) S's depth typeless (R24G8, its DSV the game's 45) so S slice 1's planes can be read; S's
    // resources never deny shader resources
    const bool sl = g_sl_installed.load(std::memory_order_relaxed) && d[kDepth].Format == DXGI_FORMAT_D24_UNORM_S8_UINT;
    for (int s = 0; s < kSets; ++s)
        for (int k = 0; k < kKinds; ++k) {
            Arr& a = g_a[s][k];
            D3D12_RESOURCE_DESC ad = d[k];
            ad.DepthOrArraySize = 2;
            ad.Alignment = 0;
            if (sl && s == kS) {
                ad.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
                if (k == kDepth) ad.Format = DXGI_FORMAT_R24G8_TYPELESS;
            }
            const DXGI_FORMAT vf = b.vf[k] != DXGI_FORMAT_UNKNOWN ? b.vf[k] : d[k].Format;
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_CLEAR_VALUE cv{};
            cv.Format = vf;
            cv.DepthStencil = {1.0f, 0};
            if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ad, rest(k), k == kDepth ? &cv : nullptr, IID_PPV_ARGS(&a.arr))) ||
                !a.arr) {
                a.arr = nullptr;
                drop_arrays();
                bump(g_p_make_failed);
                return "an array not made";
            }
            a.game = b.res[k];
            a.w = d[k].Width;
            a.h = d[k].Height;
            a.fmt = d[k].Format;
            a.vf = vf;
            a.planes = k == kDepth ? planes : 1;
            for (int v = 0; v < 3; ++v) {
                const UINT first = v < 2 ? static_cast<UINT>(v) : 0u, count = v < 2 ? 1u : 2u;
                if (k == kDepth) {
                    D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
                    dv.Format = vf;
                    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                    dv.Texture2DArray = {0, first, count};
                    a.view[v] = g_dsv_heap->GetCPUDescriptorHandleForHeapStart();
                    a.view[v].ptr += static_cast<SIZE_T>(s * 3 + v) * g_dsv_inc;
                    dev->CreateDepthStencilView(a.arr, &dv, a.view[v]);
                    // (item 8f, the review of a6830cc9) the read-only twins of the view: bound for every draw into S while the game's depth
                    // is read-only, so a pipeline with writes enabled writes S no more than the game's read-only DSV lets it write the game's
                    if (g_e1f_installed.load(std::memory_order_relaxed)) {
                        if (!g_dsv_ro_heap) {
                            D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kSets * 3 * 3, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
                            if (FAILED(dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_dsv_ro_heap)))) g_dsv_ro_heap = nullptr;
                        }
                        if (g_dsv_ro_heap) {
                            const D3D12_DSV_FLAGS kRoFlags[3] = {D3D12_DSV_FLAG_READ_ONLY_DEPTH, D3D12_DSV_FLAG_READ_ONLY_STENCIL,
                                                                 D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL};
                            for (int r = 0; r < 3; ++r) {
                                D3D12_DEPTH_STENCIL_VIEW_DESC rv = dv;
                                rv.Flags = kRoFlags[r];
                                a.ro[r][v] = g_dsv_ro_heap->GetCPUDescriptorHandleForHeapStart();
                                a.ro[r][v].ptr += static_cast<SIZE_T>((s * 3 + v) * 3 + r) * g_dsv_inc;
                                dev->CreateDepthStencilView(a.arr, &rv, a.ro[r][v]);
                            }
                            a.ro_ok = v == 2 ? true : a.ro_ok;
                        }
                    }
                } else {
                    D3D12_RENDER_TARGET_VIEW_DESC rv{};
                    rv.Format = vf;
                    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                    rv.Texture2DArray = {0, first, count, 0};
                    a.view[v] = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
                    a.view[v].ptr += static_cast<SIZE_T>((s * 3 + k) * 3 + v) * g_rtv_inc;
                    dev->CreateRenderTargetView(a.arr, &rv, a.view[v]);
                }
            }
            wchar_t nm[96];
            std::swprintf(nm, 96, L"RDRVR main %s %S", s == kS ? L"side" : L"reference", kNames[k]);
            a.arr->SetName(nm);
        }
    if (sl)  // ([Stereo] MainSlice) C: the game's descs (one slice), at rest COPY_DEST; only ever a copy's destination and source
        if (const char* why = make_c(b, planes)) {
            drop_arrays();
            return why;
        }
    bump(g_p_made);
    log::info("[mi] the side and reference arrays made (generation %llu): %llux%u, formats %d %d %d %d (views %d %d %d %d)",
              static_cast<unsigned long long>(g_gen), static_cast<unsigned long long>(d[0].Width), d[0].Height, static_cast<int>(d[0].Format),
              static_cast<int>(d[1].Format), static_cast<int>(d[2].Format), static_cast<int>(d[3].Format), static_cast<int>(g_a[0][0].vf),
              static_cast<int>(g_a[0][1].vf), static_cast<int>(g_a[0][2].vf), static_cast<int>(g_a[0][3].vf));
    return nullptr;
}

// a depth plane's followed state for a barrier on cl: false (counted for the user) when not followed, or followed but last set on another list
bool fs_followed(ID3D12GraphicsCommandList* cl, unsigned p, D3D12_RESOURCE_STATES* st, int user) {
    D3D12_RESOURCE_STATES s{};
    if (!prepass::kind_followed(0, p, &s)) return false;
    if (!prepass::kind_followed_fresh(0, p, cl, &s)) {
        bump(g_fs_stale[user]);
        return false;
    }
    *st = s;
    return true;
}
void fs_log(const char* where, ID3D12GraphicsCommandList* cl, const D3D12_RESOURCE_STATES from[2], const char* what) {
    if (!g_f.fs_diag) return;
    D3D12_RESOURCE_STATES s0{}, s1{};
    const bool f0 = prepass::kind_followed(0, 0, &s0), f1 = prepass::kind_followed(0, 1, &s1);
    log::limited("mi.fsdiag", 80, "[mi] follow diag (%s, frame %llu): depth planes followed %d:%d %#x:%#x, set on lists %p:%p, this list %p; %s %#x:%#x",
                 where, static_cast<unsigned long long>(state::presents.load()), f0 ? 1 : 0, f1 ? 1 : 0, static_cast<unsigned>(s0), static_cast<unsigned>(s1),
                 prepass::follow_setter(0, 0), prepass::follow_setter(0, 1), static_cast<void*>(cl), what,
                 from ? static_cast<unsigned>(from[0]) : 0u, from ? static_cast<unsigned>(from[1]) : 0u);
}
// ([Stereo] MainRegionE1Compare, item 8f) an e1f frame's slice-1 inputs, checked before the seed records anything: D2L (both slices 1's
// depth plane), D2 (their stencil plane; r116's G0 smoke: pass 2's stencil at e1's start is D2's, without the lights' marks D2L holds,
// while the depth is D2L's = D2's), V2 under DLSS (both velocity slices 1; FXAA: the clear), each this pass 1's (fresh: D2L's have2 also
// covers D2, its IN-L found this frame's OUT1 copy), on this thread, of the bound target and its desc; FSC RGBA16F (the sentinel's
// format); C made. The seed control: S1's stencil from D2L instead (the lights' marks: G1 must fail). A refusal excludes the frame
// (kXE1fSeed, counted by reason) and touches nothing
struct E1fSeed {
    ID3D12Resource* dl = nullptr;  // D2L (at rest COPY_DEST): both slices 1's depth plane
    ID3D12Resource* d2 = nullptr;  // D2 (at rest COPY_DEST)
    ID3D12Resource* st[2] = {};    // the stencil plane's source, S1's [kS] and R1's [kR]: D2 (S1 D2L under the seed control)
    ID3D12Resource* v2 = nullptr;  // V2 (at rest RENDER_TARGET); null under FXAA
};
bool same_2d(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& g) {  // (seed's desc check of the pre-pass array, for one slice)
    return a.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && a.Width == g.Width && a.Height == g.Height && a.Format == g.Format &&
           a.DepthOrArraySize == 1 && a.MipLevels == 1 && a.SampleDesc.Count == 1;
}
const char* e1f_no_seed(int r, const char* why) {
    g_f.why |= kXE1fSeed;
    bump(g_e1f_sx[r]);
    std::snprintf(g_e1f_why, sizeof(g_e1f_why), "seed: %s", why);
    return why;
}
const char* e1f_inputs(const Bound& b, E1fSeed* o) {
    const DWORD tid = GetCurrentThreadId();
    if (!g_f.e1f_tech_ok) return e1f_no_seed(kEsTech, "the post technique (PostFx +0x868) neither DLSS (5) nor FXAA or none (1, 0), or not read");
    if (g_a[kS][kFsc].vf != DXGI_FORMAT_R16G16B16A16_FLOAT || g_a[kR][kFsc].vf != DXGI_FORMAT_R16G16B16A16_FLOAT)
        return e1f_no_seed(kEsFmt, "FullScreenCopy's view is not RGBA16F (the sentinel's format)");
    if (const char* why = ensure_c(b)) return e1f_no_seed(kEsC, why);
    unsigned long t = 0;
    bool fresh = false;
    ID3D12Resource* game = nullptr;
    const D3D12_RESOURCE_DESC gd = b.res[kDepth]->GetDesc();
    // (each accessor reads its module's state only on that module's thread: the thread checked first)
    ID3D12Resource* dl = eye2_light::d2l_image(nullptr, &t, &fresh, &game);
    if (t != tid) return e1f_no_seed(kEsD2lThread, "D2L's steps play on another thread");
    if (!dl) return e1f_no_seed(kEsD2l, "D2L not made ([Stereo] PrepassEye2LightDepth)");
    if (!fresh) return e1f_no_seed(kEsD2lStale, "D2L not this pass 1's (no OUT-L wrote it, a step still queued, or stood down)");
    if (game != b.res[kDepth] || !same_2d(dl->GetDesc(), gd)) return e1f_no_seed(kEsD2lDesc, "D2L is not the bound depth's (its game target or desc)");
    ID3D12Resource* d2 = eye2_light::d2_image(nullptr, &t, &game);
    if (!d2 || t != tid || game != b.res[kDepth] || !same_2d(d2->GetDesc(), gd))
        return e1f_no_seed(kEsD2, "D2 (the stencil's seed) not made, on another thread, or not the bound depth's");
    o->dl = dl;
    o->d2 = d2;
    o->st[kS] = o->st[kR] = d2;
    if (g_f.ck == kCkSeed) o->st[kS] = dl;  // (the control: S1's stencil with the lights' marks; R1 keeps D2's)
    o->v2 = nullptr;
    if (g_f.e1f_dlss) {
        ID3D12Resource* v2 = eye2_velocity::v2_image(nullptr, &t, &fresh, &game);
        if (t != tid) return e1f_no_seed(kEsV2Thread, "DLSS: V2's steps play on another thread");
        if (!v2) return e1f_no_seed(kEsV2, "DLSS: V2 not made ([Stereo] PrepassEye2Velocity)");
        if (!fresh) return e1f_no_seed(kEsV2Stale, "DLSS: V2 not this pass 1's (no OUT-V drew it, a step still queued, or stood down)");
        if (game != b.res[kVel] || !same_2d(v2->GetDesc(), b.res[kVel]->GetDesc()))
            return e1f_no_seed(kEsV2Desc, "DLSS: V2 is not the bound Velocity RT's (its game target or desc)");
        o->v2 = v2;
    }
    return nullptr;
}
// pass 1's first listed draw, before the game's: S and R cleared (colour) and seeded (both depth planes)
const char* seed(ID3D12GraphicsCommandList* cl, uint32_t flags, const Bound& b) {
    // the game's depth: a writable DSV bound with a depth- and stencil-writing pipeline, so both planes are in DEPTH_WRITE
    // at this draw; where the walk cut's follow has the state, it must agree
    // (items 6d/6e, research\run25\families.md 3.1.4) the three families write neither (007, EQUAL): for a family other than the
    // billboards, the depth's state may instead come from the walk cut's follow (both planes the same: DEPTH_WRITE, or read-only below)
    const bool writes = (flags & pso::main_inst_flags::kDepthWrite) && (flags & pso::main_inst_flags::kStencilWrite);
    const bool bb = std::strcmp(pso::main_family_key(), "bb") == 0;
    if (!writes && bb) return "the family's pipeline writes no depth or no stencil (the depth's state not implied)";
    bool followed = false;
    int planes_followed = 0;
    // (run 25, r60 E1: CliffWall's draws meet the depth in a read state) for a family writing neither, the followed state of both
    // planes, the same, DEPTH_WRITE or read-only with DEPTH_READ; the barriers below go from it and back to it
    D3D12_RESOURCE_STATES dst = D3D12_RESOURCE_STATE_DEPTH_WRITE, st0{};
    // ([Stereo] MainRegionE1Compare, item 8f; r116 G2: under FXAA e1's first listed draw meets the planes in different followed states) an
    // e1f frame takes each plane's own state, each held to the rule below, and its barriers go per plane (stp)
    D3D12_RESOURCE_STATES stp[2] = {D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_WRITE};
    constexpr D3D12_RESOURCE_STATES kWrites = D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_RENDER_TARGET |
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_COPY_DEST |
                                              D3D12_RESOURCE_STATE_RESOLVE_DEST | D3D12_RESOURCE_STATE_STREAM_OUT;
    static char why_st[160];
    if (prepass::kind_game(0) == b.res[kDepth])
        for (unsigned p = 0; p < 2; ++p) {
            D3D12_RESOURCE_STATES st{};
            if (!fs_followed(cl, p, &st, kFsSeed)) continue;  // (r117 DG4: set on this list)
            followed = true;
            stp[p] = st;
            if (planes_followed++ == 0) st0 = st;
            else if (st != st0 && !g_f.e1f) return "the walk cut's follow has the depth's two planes in different states at the family's draw";
            const bool read_only = !writes && (st & D3D12_RESOURCE_STATE_DEPTH_READ) && !(st & kWrites);
            if (st != D3D12_RESOURCE_STATE_DEPTH_WRITE && !read_only) {
                std::snprintf(why_st, sizeof(why_st), "the walk cut's follow has the depth in state 0x%x at the family's draw (not DEPTH_WRITE%s)",
                              static_cast<unsigned>(st), writes ? "" : " or read-only DEPTH_READ");
                return why_st;
            }
            dst = st;
        }
    if (!writes && planes_followed != 2)
        return "the family's pipeline writes no depth or no stencil, and the walk cut's follow has not both planes' state (the depth's state not implied)";
    // ([Stereo] MainFamily=region, item 8e) always the followed rule: both planes' state from the walk cut's follow, whatever e1's first
    // listed pipeline writes
    if (g_f.rg && planes_followed != 2) return "a region frame: the walk cut's follow has not both depth planes' state at e1's first listed draw";
    const bool per_plane = g_f.e1f && planes_followed == 2 && stp[0] != stp[1];  // (item 8f: each plane from and back to its own state)
    if (g_f.e1f_rm_try) {  // (item 8f's remap: both planes DEPTH_WRITE or plain DEPTH_READ, or (r116 G3) both read-only: then unchanged by e1)
        const auto plain = [](D3D12_RESOURCE_STATES st) { return st == D3D12_RESOURCE_STATE_DEPTH_WRITE || st == D3D12_RESOURCE_STATE_DEPTH_READ; };
        const bool ok_plain = planes_followed == 2 && plain(stp[0]) && plain(stp[1]);
        const bool ok_ro = planes_followed == 2 && !ok_plain && rm_read_only(stp[0]) && rm_read_only(stp[1]) && g_a[kS][kDepth].ro_ok;
        if (!ok_plain && !ok_ro) {
            g_f.e1f_rm_try = false;
            bump(g_rm_refused[kRoDepth]);
            g_rm_ref_st[0].store(static_cast<uint32_t>(stp[0]), std::memory_order_relaxed);
            g_rm_ref_st[1].store(static_cast<uint32_t>(stp[1]), std::memory_order_relaxed);
        } else {
            g_f.e1f_rm_ro = ok_ro;
            g_rm_open_st[0].store(static_cast<uint32_t>(stp[0]), std::memory_order_relaxed);
            g_rm_open_st[1].store(static_cast<uint32_t>(stp[1]), std::memory_order_relaxed);
        }
    }
    if (per_plane) {
        bump(g_e1f_planes_differ);
        g_e1f_pst[0].store(static_cast<uint32_t>(stp[0]), std::memory_order_relaxed);
        g_e1f_pst[1].store(static_cast<uint32_t>(stp[1]), std::memory_order_relaxed);
    }
    if (const char* why = ensure(b)) return why;
    // ([Stereo] MainSlice) a slice frame: S slice 1 is seeded far below instead (slice.md 1.3), never from eye 2's depth; it
    // needs the typeless side depth (made only for a D24S8 game depth): otherwise the frame is refused before any copy
    const bool far1 = g_f.slice != 0 && g_a[kS][kDepth].arr && g_a[kS][kDepth].arr->GetDesc().Format == DXGI_FORMAT_R24G8_TYPELESS;
    if (g_f.slice && !far1) return "a slice frame without the typeless side depth (the game's depth is not D24S8)";
    // ([Stereo] MainRegionE1Compare, item 8f) an e1f frame: slice 1's inputs checked first (refused: nothing recorded)
    E1fSeed es;
    if (g_f.e1f)
        if (const char* why = e1f_inputs(b, &es)) return why;
    // eye 2's depth seed: the pre-pass depth array's slice 1 (eye 2's pre-pass) when PrepassInstanced has it, at rest (never an e1f frame's)
    ID3D12Resource* e2 = nullptr;
    if (!g_f.e1f && prepass::instanced_on() && !prepass::active() && prepass::kind_game(0) == b.res[kDepth] && prepass::kind_array(0) &&
        prepass::kind_planes(0) == 2 && prepass::kind_rest_state(0) == D3D12_RESOURCE_STATE_DEPTH_WRITE) {
        const D3D12_RESOURCE_DESC ad = prepass::kind_array(0)->GetDesc(), gd = b.res[kDepth]->GetDesc();
        if (ad.Width == gd.Width && ad.Height == gd.Height && ad.Format == gd.Format && ad.DepthOrArraySize == 2 && ad.MipLevels == 1)
            e2 = prepass::kind_array(0);
    }
    const float zero[4] = {};
    float fsc_seed[kSets][4];  // (kS then kR: "fscseed A [B]": R's B, or A when none given)
    for (int s = 0; s < kSets; ++s)
        for (int c = 0; c < 4; ++c) {
            const uint32_t u = g_rg_fsc_seed[(s == kR ? 4 : 0) + c].load(std::memory_order_relaxed);
            std::memcpy(&fsc_seed[s][c], &u, 4);
        }
    for (int s = 0; s < kSets; ++s)
        for (int k = 0; k < kDepth; ++k)
            d3d::raw_clear_rtv(cl, &g_a[s][k].view[2], k == kFsc && g_f.rg && !g_f.slice ? fsc_seed[s] : zero);  // (never a slice frame: MainSlice's merge reads S)
    if (g_f.e1f)  // (item 8f) slice 1's FullScreenCopy the 8f0b sentinel in S and R (its reactivity 0 and, under FXAA, its velocity 0 above)
        for (int s = 0; s < kSets; ++s) d3d::raw_clear_rtv(cl, &g_a[s][kFsc].view[1], kE1fSentinel);
    constexpr D3D12_RESOURCE_STATES kCd = D3D12_RESOURCE_STATE_COPY_DEST, kCs = D3D12_RESOURCE_STATE_COPY_SOURCE, kRt = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Batch bt;
    if (per_plane)  // (item 8f: the game depth's subresource p is its plane p)
        for (UINT p = 0; p < 2; ++p) bt.add_sub(b.res[kDepth], p, stp[p], D3D12_RESOURCE_STATE_COPY_SOURCE);
    else
        bt.add(b.res[kDepth], dst, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (int s = 0; s < kSets; ++s) bt.add(g_a[s][kDepth].arr, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (e2) bt.add(e2, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    // (item 8f) D2L (and the seed control's D2) from their rest COPY_DEST; V2 and both velocity arrays' slice 1 from their rest RENDER_TARGET
    bt.add(es.dl, kCd, kCs);
    bt.add(es.d2, kCd, kCs);
    if (es.v2) {
        bt.add(es.v2, kRt, kCs);
        for (int s = 0; s < kSets; ++s) bt.add_sub(g_a[s][kVel].arr, 1, kRt, kCd);
    }
    const bool rm = g_f.e1f && g_f.e1f_rm_try;  // (item 8f's remap: S0's colour <- the four's, bound at this draw: RENDER_TARGET, check_bind)
    if (rm)
        for (int k = 0; k < kDepth; ++k) {
            bt.add(b.res[k], kRt, kCs);
            bt.add_sub(g_a[kS][k].arr, 0, kRt, kCd);
        }
    bt.flush(cl);
    if (rm)
        for (int k = 0; k < kDepth; ++k) copy_sub(cl, g_a[kS][k].arr, 0, b.res[k], 0);
    for (int s = 0; s < kSets; ++s)
        for (UINT p = 0; p < 2; ++p) {  // (subresource: slice + plane x 2 slices)
            copy_sub(cl, g_a[s][kDepth].arr, 0 + p * 2, b.res[kDepth], p);
            if (far1 && s == kS) continue;
            if (es.dl) copy_sub(cl, g_a[s][kDepth].arr, 1 + p * 2, p == 0 ? es.dl : es.st[s], p);  // (item 8f: the depth D2L's, the stencil D2's)
            else if (e2) copy_sub(cl, g_a[s][kDepth].arr, 1 + p * 2, e2, 1 + p * 2);
            else copy_sub(cl, g_a[s][kDepth].arr, 1 + p * 2, b.res[kDepth], p);
        }
    if (es.v2)
        for (int s = 0; s < kSets; ++s) copy_sub(cl, g_a[s][kVel].arr, 1, es.v2, 0);
    if (per_plane)
        for (UINT p = 0; p < 2; ++p) bt.add_sub(b.res[kDepth], p, D3D12_RESOURCE_STATE_COPY_SOURCE, stp[p]);
    else
        bt.add(b.res[kDepth], D3D12_RESOURCE_STATE_COPY_SOURCE, dst);
    for (int s = 0; s < kSets; ++s) bt.add(g_a[s][kDepth].arr, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (e2) bt.add(e2, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    bt.add(es.dl, kCs, kCd);
    bt.add(es.d2, kCs, kCd);
    if (es.v2) {
        bt.add(es.v2, kCs, kRt);
        for (int s = 0; s < kSets; ++s) bt.add_sub(g_a[s][kVel].arr, 1, kCd, kRt);
    }
    if (rm)
        for (int k = 0; k < kDepth; ++k) {
            bt.add(b.res[k], kCs, kRt);
            bt.add_sub(g_a[kS][k].arr, 0, kCd, kRt);
        }
    bt.flush(cl);
    if (rm) g_f.e1f_rm_ready = true;
    {
        const D3D12_RESOURCE_STATES fr[2] = {per_plane ? stp[0] : dst, per_plane ? stp[1] : dst};
        fs_log("seed", cl, fr, "the game's depth -> COPY_SOURCE and back from");
    }
    if (g_f.e1f) {
        bump(g_e1f_seeds);
        if (es.v2) bump(g_e1f_v2_copies);
        if (es.st[kS] != es.st[kR]) bump(g_e1f_seed_ctl);
    }
    if (far1) {  // depth 1.0 (the far value: standard Z, billboard.md 8.2), stencil 0 (S1's marker: 0x87 where a fragment is kept)
        d3d::raw_clear_dsv(cl, &g_a[kS][kDepth].view[1], 1.0f, 0);
        bump(g_sl_seed_far);
    }
    bump(g_p_seeds);
    if (e2) bump(g_p_seed_e2_array);
    if (followed) bump(g_p_seed_followed);
    if (dst != D3D12_RESOURCE_STATE_DEPTH_WRITE) {  // (the depth met read-only: the last state seen kept for the status)
        bump(g_p_seed_read);
        g_p_seed_read_state.store(static_cast<uint32_t>(dst), std::memory_order_relaxed);
    }
    return nullptr;
}

// the readback for the current arrays (made on the first capture, again when the arrays are re-made)
const char* ensure_rb() {
    if (g_rb.rb && g_rb.gen == g_gen && g_rb.has_c == (g_c[0].arr != nullptr)) return nullptr;
    retire(g_rb.rb);
    g_rb = Rb{};
    ID3D12Device* dev = state::device.load();
    if (!dev || !g_gen) return "no device or no arrays";
    UINT64 off = 0;
    for (int set = 0; set < kSets; ++set)
        for (int sl = 0; sl < 2; ++sl)
            for (int pl = 0; pl < kPlanes; ++pl) {
                const int k = pl < kDepth ? pl : kDepth;
                const UINT sub = pl < kDepth ? static_cast<UINT>(sl) : static_cast<UINT>(sl + (pl - kDepth) * 2);
                const D3D12_RESOURCE_DESC ad = g_a[set][k].arr->GetDesc();
                const int x = slot(set, sl, pl);
                UINT64 total = 0;
                dev->GetCopyableFootprints(&ad, sub, 1, off, &g_rb.fp[x], &g_rb.rows[x], &g_rb.rowbytes[x], &total);
                off = (g_rb.fp[x].Offset + total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                      ~static_cast<UINT64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
            }
    if (g_c[0].arr)  // ([Stereo] MainSlice) C's five planes (one slice: the depth's planes are subresources 0 and 1)
        for (int pl = 0; pl < kPlanes; ++pl) {
            const int k = pl < kDepth ? pl : kDepth;
            const UINT sub = pl < kDepth ? 0u : static_cast<UINT>(pl - kDepth);
            const D3D12_RESOURCE_DESC ad = g_c[k].arr->GetDesc();
            const int x = slot_c(pl);
            UINT64 total = 0;
            dev->GetCopyableFootprints(&ad, sub, 1, off, &g_rb.fp[x], &g_rb.rows[x], &g_rb.rowbytes[x], &total);
            off = (g_rb.fp[x].Offset + total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~static_cast<UINT64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
        }
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
    if (FAILED(dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_rb.rb))) || !g_rb.rb) {
        g_rb = Rb{};
        bump(g_rb_failed);
        return "the readback not made";
    }
    g_rb.rb->SetName(L"RDRVR main compare readback (MainInstanced)");
    g_rb.gen = g_gen;
    g_rb.has_c = g_c[0].arr != nullptr;
    g_rb.size = off;
    bump(g_rb_made);
    log::info("[mi] the compare's readback made: %llu MB", static_cast<unsigned long long>(off >> 20));
    return nullptr;
}
// pass 2's close of a good captured frame: S and R, both slices, every plane, to readback
void readback(ID3D12GraphicsCommandList* cl) {
    int s = g_cap.load();
    if (s != kArmed) return;
    if (cl->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        bump(g_cap_incomplete);
        return;
    }
    if (d3d::thread_predicated()) {  // (the review's should-fix 3: never copied under a predicate; at the next list call)
        bump(g_rb_deferred);
        g_pending = true;
        g_pending_tid = GetCurrentThreadId();
        prepass::g_main_pending_flag.store(true, std::memory_order_release);
        return;
    }
    if (const char* why = ensure_rb()) {  // (the review's should-fix 4: the capture disarmed, never retried every good frame)
        bump(g_cap_incomplete);
        g_cap_left.store(0);
        g_cap.store(kIdle);
        stand_down(why);
        return;
    }
    // ([Stereo] MainSlice: C holds this frame's merged targets; item 8f: an e1f frame's C2)
    const bool with_c = ((g_f.slice && g_f.cmp) || g_f.e1f) && g_f.c_taken && g_c[0].arr && g_rb.has_c;
    if (g_f.e1f && !with_c) {  // (an e1f frame is compared only with its C2: R1 is a seed, never 8e's reference; rg_close excludes it first)
        bump(g_cap_incomplete);
        return;
    }
    Batch bt;
    for (int set = 0; set < kSets; ++set)
        for (int k = 0; k < kKinds; ++k) bt.add(g_a[set][k].arr, rest(k), D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (with_c)
        for (int k = 0; k < kKinds; ++k) bt.add(g_c[k].arr, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    bt.flush(cl);
    for (int set = 0; set < kSets; ++set)
        for (int sl = 0; sl < 2; ++sl)
            for (int pl = 0; pl < kPlanes; ++pl) {
                const int k = pl < kDepth ? pl : kDepth;
                const UINT sub = pl < kDepth ? static_cast<UINT>(sl) : static_cast<UINT>(sl + (pl - kDepth) * 2);
                const int x = slot(set, sl, pl);
                D3D12_TEXTURE_COPY_LOCATION d{g_rb.rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
                d.PlacedFootprint = g_rb.fp[x];
                D3D12_TEXTURE_COPY_LOCATION src{g_a[set][k].arr, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
                src.SubresourceIndex = sub;
                d3d::raw_copy_region(cl, &d, &src);
            }
    if (with_c)
        for (int pl = 0; pl < kPlanes; ++pl) {
            const int k = pl < kDepth ? pl : kDepth;
            D3D12_TEXTURE_COPY_LOCATION d{g_rb.rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
            d.PlacedFootprint = g_rb.fp[slot_c(pl)];
            D3D12_TEXTURE_COPY_LOCATION src{g_c[k].arr, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            src.SubresourceIndex = pl < kDepth ? 0u : static_cast<UINT>(pl - kDepth);
            d3d::raw_copy_region(cl, &d, &src);
        }
    for (int set = 0; set < kSets; ++set)
        for (int k = 0; k < kKinds; ++k) bt.add(g_a[set][k].arr, D3D12_RESOURCE_STATE_COPY_SOURCE, rest(k));
    if (with_c)
        for (int k = 0; k < kKinds; ++k) bt.add(g_c[k].arr, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    bt.flush(cl);
    g_job.rb = g_rb.rb;
    std::memcpy(g_job.fp, g_rb.fp, sizeof(g_job.fp));
    std::memcpy(g_job.rows, g_rb.rows, sizeof(g_job.rows));
    std::memcpy(g_job.rowbytes, g_rb.rowbytes, sizeof(g_job.rowbytes));
    g_job.size = g_rb.size;
    g_job.ctl = g_f.ctl;
    g_job.listed = g_f.listed1;
    g_job.slice = with_c ? g_f.slice : 0;
    g_job.rmode = g_f.rg ? g_f.rmode : 0;  // ([Stereo] MainFamily=region, item 8e; 0: the compare as before)
    g_job.solo = g_f.solo;
    g_job.swap = g_f.swap;
    g_job.ck = g_f.ck;
    g_job.drew = g_f.drew;
    g_job.rep = g_f.mr.rep;
    g_job.listed2 = g_f.listed2;
    g_job.ho = g_f.rg && g_f.ho && g_f.ho_state == kHsHanded && with_c;  // ([Stereo] MainRegionHandover, item 8g: compare_handover)
    g_job.ho_mode = g_f.ho_mode;
    g_job.ho_mask = g_f.ho_mask;
    g_job.h1 = g_job.ho && g_f.ho_h1;  // (GOAL-single-pass-2 item 4: compare_h1 in compare_handover's place)
    g_job.h1_cp = g_f.ho_cp;
    g_job.twin_mask = g_f.twin_mask;
    g_job.alt = g_f.ho_g1_done ? 1 : g_f.ho_g1 && g_f.ho_state == kHsRefused ? 2 : g_f.ho_h1 && g_f.ho_state == kHsRefused ? 3 : 0;
    g_job.e1f = g_f.rg && g_f.e1f && with_c && !g_job.ho;  // ([Stereo] MainRegionE1Compare, item 8f: compare_e1f)
    g_job.dlss = g_f.e1f_dlss;
    g_job.single = g_f.e1f_single;
    g_job.frame = state::presents.load();
    g_ready_frame.store(g_job.frame);
    bump(g_cap_started);
    if (!g_cap.compare_exchange_strong(s, kReady)) bump(g_cap_incomplete);
}

// ([Stereo] MainUnionCull) pass 2's close: the frame judged (pass 1 then pass 2, in order, no table full), by pass 1's category
void uc_judge() {
    if (g_ucf.over || (g_f.why & kXOrder)) {
        bump(g_ucf.over ? g_uc_over : g_uc_unjudged);
        return;
    }
    const int c = g_ucf.uc1 ? 1 : 0;
    bump(g_uc_frames[c]);
    if (!g_ucf.miss[0]) bump(g_uc_subset[c]);
    if (!g_ucf.miss[1]) bump(g_uc_subset_nvb[c]);
    g_uc_miss[c].fetch_add(g_ucf.miss[0], std::memory_order_relaxed);
    g_uc_miss_nvb[c].fetch_add(g_ucf.miss[1], std::memory_order_relaxed);
    g_uc_listed1[c].fetch_add(g_ucf.n1, std::memory_order_relaxed);
    g_uc_listed2[c].fetch_add(g_ucf.n2, std::memory_order_relaxed);
    g_uc_draws1[c].fetch_add(g_ucf.draws1, std::memory_order_relaxed);
    g_uc_draws2[c].fetch_add(d3d::thread_draws() - g_r.d0, std::memory_order_relaxed);
    g_uc_last_sum2.store(g_ucf.sum2, std::memory_order_relaxed);
    g_uc_last_n2.store(g_ucf.n2, std::memory_order_relaxed);
    g_uc_last_n1.store(g_ucf.n1, std::memory_order_relaxed);
    g_uc_last_miss.store(g_ucf.miss[0], std::memory_order_relaxed);
    g_uc_last_cat.store(static_cast<uint64_t>(c), std::memory_order_relaxed);
}

// ([Stereo] MainFamily=region, item 8e) pass 2's close of a region frame: 8d's verdict for this frame (judged, no failed gate, no single:
// anywhere in e1 for joint, in a listed pair for solo and subset; within), main_inst's own (seeded, the arrays' generation, nothing
// predicated or refused, the order), a dup frame; the counts and the contiguity do not apply (the union and the subset)
void rg_close(ID3D12GraphicsCommandList* cl) {
    if (!g_f.dup) return;  // (not a dup frame: nothing listed; counted at the latch)
    g_f.listed2 = g_r.listed;  // (item 8e's diagnosis: pass 2's region, still in g_r at its close)
    uint32_t why = g_f.why;
    const MrVerdict& v = g_f.mr;
    if (!g_f.mr_have) {
        why |= kXRgVerdict;
    } else if (!v.judged) {
        why |= kXRgUnj;
    } else {
        if (v.gates) why |= kXRgGate;
        if (g_f.rmode == kRgJoint ? v.singles != 0 : (v.single_ords & g_f.subset) != 0) why |= kXRgSingle;
        if (!v.within) why |= kXRgMiss;
        if (g_f.rmode == kRgJoint && v.noord) why |= kXRgNoOrd;
    }
    if (g_f.listed1 == 0) why |= kXNone;  // (pass 2 may list none of a solo pair pass 1 drew: still compared, R1 then holds none)
    if (!g_f.seeded || g_f.gen != g_gen) why |= kXSeed;
    // ([Stereo] MainRegionE1Compare, item 8f) R0 is T and C is C2 only when both were taken (the seed's own bit set at it); (item 8g) on a
    // compare frame only: a handover frame that is none is not read back
    if (g_f.e1f && g_f.rcmp) {
        if (!g_f.e1f_t) why |= kXE1fT;
        if (!g_f.c_taken) why |= kXE1fC2;
        if (why & kXE1fSeed) bump(g_e1f_x_seed);
        if (why & kXE1fT) bump(g_e1f_x_t);
        if (why & kXE1fC2) bump(g_e1f_x_c2);
        if (why & kXE1fRemap) bump(g_rm_lost_n);
        if (why & kXHo) bump(g_ho_strict_excl);
    }
    if (why) {
        bump(g_f_bad);
        if (why & kXGate) bump(g_f_x_gate);
        if (why & kXPred) bump(g_f_x_pred);
        if (why & kXRefused) bump(g_f_x_refused);
        if (why & kXSeed) bump(g_f_x_seed);
        if (why & kXOrder) bump(g_f_x_order);
        if (why & kXNone) bump(g_f_x_none);
        if (why & kXRgVerdict) bump(g_rg_x_verdict);
        if (why & kXRgUnj) bump(g_rg_x_unj);
        if (why & kXRgGate) bump(g_rg_x_gate);
        if (why & kXRgSingle) bump(g_rg_x_single);
        if (why & kXRgMiss) bump(g_rg_x_miss);
        if (why & kXRgNoOrd) bump(g_rg_x_noord);
        return;
    }
    bump(g_f_good);
    bump(g_rg_good[g_f.rmode >= 0 && g_f.rmode < kRgModes ? g_f.rmode : 0]);
    if (v.rep) bump(g_rg_rep);  // (8d's rep: a pass-1 key drawn twice; bisect step 0 reads it)
    if (g_f.ck > 0 && g_f.ck < kCk) bump(g_rg_ctl_frames[g_f.ck]);
    if (g_f.swap >= 0) bump(g_rg_swap_frames);
    if (g_f.marg) bump(g_rg_marg_frames);
    if (g_f.ho && g_f.ho_state == kHsHanded) g_ho_drop_last.store(g_f.ho_drops, std::memory_order_relaxed);  // (item 8g)
    if (g_cap.load() != kArmed || !g_f.rcmp) return;  // (item 8g: a handover frame is read back only as a compare frame; else rcmp is dup)
    if (cl) {
        readback(cl);
    } else {  // (the fallback marker has no list: at this thread's next list call)
        g_pending = true;
        g_pending_tid = GetCurrentThreadId();
        prepass::g_main_pending_flag.store(true, std::memory_order_release);
    }
}

void close_region(ID3D12GraphicsCommandList* cl, bool by_copy) {
    if (!g_r.open) return;
    g_r.open = false;
    g_listen.store(false, std::memory_order_relaxed);
    bump(by_copy ? g_p_close_copy : g_p_close_marker);
    const int pass = g_r.pass;
    if (pass == 2 && g_tr_st.load(std::memory_order_relaxed) == kTrRec) {  // (item 8e 14: the trace recorded; read after kTrWait)
        g_tr_why.store(g_f.why | (g_r.ok ? 0u : 0x80000000u));
        g_tr_done.store(state::presents.load());
        g_tr_st.store(kTrDone, std::memory_order_release);
    }
    if (pass == 2 && g_f.gt && g_gt_st.load(std::memory_order_relaxed) == kTrRec) {  // ("e1f g1trace": read after kTrWait)
        g_gt_why.store(g_f.why | (g_r.ok ? 0u : 0x80000000u));
        g_gt_done.store(state::presents.load());
        g_gt_st.store(kTrDone, std::memory_order_release);
    }
    if (g_own_pb != 0) g_own_bad = true;  // (item 8e 14: a bracket open at the close)
    g_own_pb = 0;
    g_r.e1f_watch = false;  // ([Stereo] MainRegionE1Compare, item 8f) the watch ends with the region; a C2 still owed is never taken
    if (g_e1f_owed) {
        g_e1f_owed = false;
        bump(g_e1f_cx[kEcLate]);
        if (!g_pending) prepass::g_main_pending_flag.store(false, std::memory_order_relaxed);
    }
    if (pass == 1 && g_e1f_installed.load(std::memory_order_relaxed)) eye2_velocity::v2_spent();  // (item 8f: V2 is this pass 1's only)
    if (pass == 1 && g_f.e1f_rm_started) {  // (r116 G3's diagnosis)
        g_rm_skip_last.store(g_f.e1f_rm_skip, std::memory_order_relaxed);
        g_rm_after_last.store(g_f.e1f_rm_after, std::memory_order_relaxed);
        g_rm_after_sum.fetch_add(g_f.e1f_rm_after, std::memory_order_relaxed);
    }
    if (g_rm_on) {  // (item 8f's remap: e1's end never came) closed now on the copy's list call, else owed to this thread's next list call
        if (cl) {
            rm_end(cl, kRmRegion);
        } else {
            g_rm_on = false;
            g_rm_owed = true;
            prepass::g_main_pending_flag.store(true, std::memory_order_release);
        }
    }
    if (pass == 2 && g_f.rg && (g_own_bad || g_own_n[1] != g_own_n[2])) {
        g_f.why |= kXRgOwn;
        bump(g_rg_x_own);
    }
    if (main_region::observing()) main_region::pb_close(pass);  // ([Stereo] MainRegion, item 8d: its pass ends with the region)
    pso::walk_close(cl, pass);  // (GOAL-single-pass item 6c: pass 1 gives SC2L and LB2 back; pass 2 compares the latch's answers)
    if (g_f.rg) c1_drop();  // ([Stereo] MainFamily=region: the cached ID3D12GraphicsCommandList1 released at each region close)
    g_last_listed[pass].store(static_cast<uint64_t>(g_r.listed), std::memory_order_relaxed);
    g_last_first[pass].store(g_r.first, std::memory_order_relaxed);
    if (pass == 1) {
        if (g_uc_installed.load(std::memory_order_relaxed)) {  // ([Stereo] MainUnionCull) pass 1's side of the judgement
            g_ucf.uc1 = g_r.uc;
            g_ucf.draws1 = d3d::thread_draws() - g_r.d0;
        }
        g_f.state = 2;
        g_f.listed1 = g_r.listed;
        g_f.first1 = g_r.first;
        g_f.contig1 = g_r.contig;
        g_f.twins1 = g_r.twins;
        g_f.mixed1 = g_r.mixed;
        g_f.pso1 = g_r.pso0;
        return;
    }
    g_f.state = 0;
    if (g_f.rg) {  // ([Stereo] MainFamily=region, item 8e: 8d's verdict is the frame rule; its draws are keyed by main_region only)
        rg_close(cl);
        return;
    }
    if (g_uc_installed.load(std::memory_order_relaxed)) uc_judge();  // ([Stereo] MainUnionCull) pass 2's listed draws against pass 1's
    uint32_t why = g_f.why;
    if (std::strcmp(pso::main_family_key(), "bb") != 0 && !g_f.slice) {  // (the review: a slice frame keeps the handover's count and run rules)
        // (GOAL-single-pass item 6, the family compare; families.md 3.1.5 and 3.3 option A) a family's draws are neither one run
        // nor the same count per pass: the frame is good when pass 1 culled against the union and pass 2's listed draws are
        // within pass 1's by key (uc_judge, the vertex buffer in the key); else excluded as unequal counts
        bump(g_fam_judged);
        if (!g_uc_installed.load(std::memory_order_relaxed) || !g_ucf.uc1 || g_ucf.over || g_ucf.miss[0]) why |= kXCounts;
        else bump(g_fam_within);  // (within, not yet good: the other exclusions below still apply)
        if (g_ucf.rep) bump(g_fam_rep);
    } else {
        if (g_r.listed != g_f.listed1) why |= kXCounts;
        if (!g_r.contig || !g_f.contig1) why |= kXContig;
    }
    if (g_r.listed == 0 || g_f.listed1 == 0) why |= kXNone;
    if (!g_f.seeded || g_f.gen != g_gen) why |= kXSeed;
    if (g_f.slice) {  // ([Stereo] MainSlice) the handover's counts
        if (!g_r.handed && !g_r.ident) {
            why |= kXHand;
            bump(g_sl_refused);
            if (!g_f.hr_counted) bump(g_sl_hr[kHrEarly]);  // (refused before the decision: the gate, a predicate, the bind, pass 1's region)
        } else if (g_r.handed) {
            if (g_r.listed < g_f.listed1) bump(g_sl_short);
            const uint64_t n = static_cast<uint64_t>(g_r.dropped);
            g_sl_last_dropped.store(n, std::memory_order_relaxed);
            store_min(g_sl_drop_min, n);
            store_max(g_sl_drop_max, n);
        }
    }
    if (!why && g_r.first == g_f.first1) bump(g_f_first_same);
    if (why) {
        bump(g_f_bad);
        if (why & kXGate) bump(g_f_x_gate);
        if (why & kXPred) bump(g_f_x_pred);
        if (why & kXRefused) bump(g_f_x_refused);
        if (why & kXCounts) bump(g_f_x_counts);
        if (why & kXContig) bump(g_f_x_contig);
        if (why & kXSeed) bump(g_f_x_seed);
        if (why & kXOrder) bump(g_f_x_order);
        if (why & kXNone) bump(g_f_x_none);
        if (why & kXHand) bump(g_f_x_hand);
        if (why & kXBarrier) bump(g_f_x_barrier);
        if (why & kXExtra) bump(g_f_x_extra);
        if (why & kXQuery) bump(g_f_x_query);
        return;
    }
    bump(g_f_good);
    if (g_f.slice) bump(g_sl_good);
    if (!g_f.slice && pso::allow_latched() && g_r.listed == g_f.listed1)  // (the family review: index-keyed, so equal lists only)
        pso::main_drop_compare(g_f.twins1, g_r.would);  // (item 6c: pass 1's twins = pass 2's would-drops)
    if (g_cap.load() != kArmed) return;
    if (g_f.slice && (!g_f.cmp || !g_f.c_taken)) return;  // ([Stereo] MainSlice) a slice frame is compared only when its drop point took T and C
    if (cl) {
        readback(cl);
    } else {  // (the fallback marker has no list: at this thread's next list call)
        g_pending = true;
        g_pending_tid = GetCurrentThreadId();
        prepass::g_main_pending_flag.store(true, std::memory_order_release);
    }
}

// ([Stereo] MainFamily=region, item 8e) pass 1's open, the playback thread: the frame is a region frame while "mr compare" runs; its
// mode, subset and solo pair as the commands set them; a dup frame while a capture is armed, 1 in N; the swapped pair (joint, swap
// auto, not a control frame): the next ordinal after the last one swapped among the pairs the last judged frame drew in pass 1
void rg_latch(int ck) {
    if (g_gt_st.load(std::memory_order_relaxed) == kTrRec) {  // ("e1f g1trace") the traced frame's pass 2 never closed: written as it stands
        g_gt_why.store(0x40000000u);
        g_gt_done.store(state::presents.load());
        g_gt_st.store(kTrDone, std::memory_order_release);
    }
    g_f.mr_fr_have = false;  // (S5: the frame number main_region pushes next, at its own pass-1 open, is this frame's)
    g_f.rg = rg_on() && main_region::comparing();  // (M1: pso's own switch too, read here)
    if (!g_f.rg) return;
    g_f.ip = g_rg_ip.load(std::memory_order_relaxed);
    bump(g_rg_frames);
    const int sel = g_rg_sel.load();
    g_f.rmode = sel & 0xff;
    g_f.solo = g_f.rmode == kRgSolo ? (sel >> 8) - 1 : -1;
    g_f.subset = g_f.rmode == kRgJoint ? ~0ull : g_f.rmode == kRgSolo ? (g_f.solo >= 0 && g_f.solo < kOrds ? 1ull << g_f.solo : 0ull) : g_rg_subset.load();
    g_f.ck = g_f.ctl && ck > 0 && ck < kCk ? ck : 0;
    int n = g_rg_every.load(std::memory_order_relaxed);
    if (n < 8 && g_f.rmode != kRgSolo) n = 8;
    if (n < 1) n = 1;
    g_f.rcmp = g_cap.load() == kArmed && ++g_rg_dupc % static_cast<uint64_t>(n) == 0;
    // ([Stereo] MainRegionHandover, item 8g) a handover frame: a joint frame with "region e1f on" and the handover, never with the remap
    const int hm = g_ho_installed.load(std::memory_order_relaxed) && !g_ho_stood.load(std::memory_order_relaxed) ? g_ho_mode.load(std::memory_order_relaxed) : 0;
    g_f.ho = hm != 0 && g_f.rmode == kRgJoint && !g_f.slice && g_e1f_on.load(std::memory_order_relaxed) && !g_rm_want.load(std::memory_order_relaxed);
    if (g_f.ho && !g_f.rcmp && g_ho_sparse.load(std::memory_order_relaxed)) {  // (sparse: the other joint frames two-pass, counted)
        g_f.ho = false;
        bump(g_ho_sparse_skipped);
    }
    if (hm && g_rm_want.load(std::memory_order_relaxed)) bump(g_ho_x[kHwRemap]);
    if (g_f.ho) {
        g_f.ho_mode = hm;
        g_f.ho_strict = g_ho_strict.load(std::memory_order_relaxed);
        g_f.ho_vr_always = g_ho_vr_always.load(std::memory_order_relaxed);
        g_f.ho_dropmask = g_ho_dropmask.load(std::memory_order_relaxed);
        g_f.ho_r1sync = g_ho_r1sync.load(std::memory_order_relaxed);
        g_f.ho_r1dsv_game = g_ho_r1dsv_game.load(std::memory_order_relaxed);
        g_f.ho_cl = nullptr;
        const bool ref_h1 = g_ho_ref_h1.load(std::memory_order_relaxed);
        if (ref_h1 != g_ho_alt_ref) {  // (the alternation starts again with G1 at each change of the switch)
            g_ho_alt_ref = ref_h1;
            g_ho_alt = 0;
        }
        if (g_f.rcmp && ref_h1) {  // (GOAL-single-pass-2 item 4: the 1st, 3rd, ... G1, the 2nd, 4th, ... H1)
            g_f.ho_g1 = g_ho_alt % 2 == 0;
            g_f.ho_h1 = !g_f.ho_g1;
            ++g_ho_alt;
        }
        g_f.ho_trim = g_ho_trim.load(std::memory_order_relaxed);
        // (H2m's diagnosis: the own mask on compare frames while armed; never on an H1 frame, whose R0 holds H)
        g_f.ho_mask = g_f.rcmp && !g_f.ho_h1 && g_ho_sv_want.load(std::memory_order_relaxed);
        bump(g_ho_frames);
    }
    g_f.dup = g_f.rcmp || g_f.ho;  // (no handover: dup is rcmp, as before)
    if (!g_f.dup && g_e1f_dupall.load(std::memory_order_relaxed) && g_f.rmode == kRgJoint && !g_f.slice &&
        g_e1f_installed.load(std::memory_order_relaxed) && g_e1f_on.load(std::memory_order_relaxed)) {  // (E2/E3, test only)
        if (hm != 0 || g_rm_want.load(std::memory_order_relaxed)) {
            bump(g_e1f_dupall_ignored);  // (a handover mode set, sparse or not, or the remap on: the frame as before)
        } else {
            g_f.dup = g_f.dupall = true;
            g_f.notwin = !g_e1f_dupall_twins.load(std::memory_order_relaxed);
            const int dm = g_e1f_dupall_mask.load(std::memory_order_relaxed);
            g_f.dupall_mask = dm == 1 || dm == 2 ? static_cast<unsigned>(dm) : 3u;
            bump(g_e1f_dupall_frames);
            if (g_f.notwin) bump(g_e1f_dupall_notwin);
        }
    }
    bump(g_f.dup ? g_rg_dup : g_rg_nondup);
    g_f.swap = -1;
    if (g_f.dup && g_f.rmode == kRgJoint && !g_f.ck && g_rg_swap.load(std::memory_order_relaxed) == 1 && g_rg_last_drew) {
        for (int k = 1; k <= kOrds && g_f.swap < 0; ++k) {
            const int o = (g_rg_swap_last + k + kOrds) % kOrds;
            if (g_rg_last_drew >> o & 1) g_f.swap = o;
        }
        g_rg_swap_last = g_f.swap;
    }
    // ([Stereo] MainRegionE1Compare, item 8f) an e1f frame: a joint dup frame with "region e1f on"; the technique read here (PostFx +0x868,
    // under SEH: 5 DLSS, 0 or 1 FXAA or none; another value or no read refuses the seed); no swap (R gets no duplicates); a playback-only
    // control ("control seed|single") on a frame the render thread's control did not mark
    g_f.e1f = g_f.dup && !g_f.slice && g_f.rmode == kRgJoint && g_e1f_installed.load(std::memory_order_relaxed) && g_e1f_on.load(std::memory_order_relaxed);
    if (g_f.e1f && g_fs_diag_left > 0) {  // (r117 DG4's diagnostic: one of the first e1f frames after the switch)
        --g_fs_diag_left;
        g_f.fs_diag = true;
    }
    if (g_f.e1f) {
        bump(g_e1f_latched);
        g_f.swap = -1;
        uintptr_t pfx = 0;
        int tech = -1;
        const bool read = anchors::present(Id::PostFxSingleton) && rd(anchors::addr(Id::PostFxSingleton), &pfx) && pfx && rd(pfx + 0x868, &tech);
        g_f.e1f_dlss = read && tech == 5;
        g_f.e1f_tech_ok = read && (tech == 5 || tech == 0 || tech == 1);
        g_f.e1f_live = g_e1f_live.load(std::memory_order_relaxed);
        g_f.c2null = g_f.rcmp && g_e1f_c2null.load(std::memory_order_relaxed);  // ("e1f c2pred null": compare frames only)
        g_f.e1f_remap = g_rm_want.load(std::memory_order_relaxed);  // (item 8f's remap)
        g_f.twin_mask = g_f.dupall_mask;  // ("twins mask": 2 over every e1f frame's twins but a remap frame's)
        if (g_e1f_twins_mask.load(std::memory_order_relaxed) == 2) {
            if (g_f.e1f_remap) {
                bump(g_e1f_twins_mask_remap);
            } else {
                g_f.twin_mask = 2;
                g_f.twin_mask_sw = true;
            }
        }
        const int pc = g_e1f_ctl.load(std::memory_order_acquire);  // (acquire: the single ordinal stored before it is seen)
        if (!g_f.ctl && (pc == kCkSeed || pc == kCkSingle || pc == kCkRmDrop || pc == kCkRmPoison)) {
            g_f.ctl = true;
            g_f.ck = pc;
            g_f.e1f_single = pc == kCkSingle || pc == kCkRmDrop ? g_e1f_single.load(std::memory_order_relaxed) : -1;
        }
    }
    // ("e1f g1trace", test only) an armed trace records this frame: an e1f compare frame not to be handed over (no handover latched, or a
    // G1 frame of ref h1's: its decision refuses)
    if (g_f.e1f && g_f.rcmp && (!g_f.ho || g_f.ho_g1) && g_gt_st.load(std::memory_order_acquire) == kTrArmed) {
        int want = kTrArmed;
        if (g_gt_st.compare_exchange_strong(want, kTrRec, std::memory_order_acq_rel)) {
            g_gt_bx0 = g_gt_x0;  // (the snapshot: the command thread writes the fields only in Setup)
            g_gt_by0 = g_gt_y0;
            g_gt_bw = g_gt_w;
            g_gt_bh = g_gt_h;
            for (auto& c : g_gt_n) c.store(0, std::memory_order_relaxed);
            g_gt_frame.store(state::presents.load());
            g_f.gt = true;
            bump(g_gt_frames);
        }
    }
    // (item 8b's open decision 3: gpu_stages' marginal stamps around each listed pass-1 draw's duplicates; a dup frame without a swap)
    g_f.marg =g_f.dup && g_f.swap < 0 && gpu_stages::marginal_on();
}

void open_region(int pass, bool ok, bool ctl, int slice = 0, int ck = 0) {
    g_own_pb = 0;  // (item 8e: a marked entity never spans a region's open)
    if (g_rm_on || g_rm_owed) rm_lost(kRmRegion, "a region opened before the remap's close");  // (item 8f's remap: never expected)
    if (g_e1f_owed) {  // ([Stereo] MainRegionE1Compare, item 8f: a C2 owed by another region is never taken; the close clears it first)
        g_e1f_owed = false;
        bump(g_e1f_cx[kEcLate]);
        if (!g_pending) prepass::g_main_pending_flag.store(false, std::memory_order_relaxed);
    }
    if (pass == 1) {
        g_own_n[1] = g_own_n[2] = 0;
        g_own_bad = false;
    }
    if (g_r.open) {  // (the last region never closed: closed here, its frame excluded)
        bump(g_p_unclosed);
        g_f.why |= kXOrder;
        close_region(nullptr, false);
    }
    if (g_pending) {  // (a readback owed and not yet taken: dropped)
        g_pending = false;
        prepass::g_main_pending_flag.store(false, std::memory_order_relaxed);
        bump(g_pending_late);
        bump(g_cap_incomplete);
    }
    if (pass == 1) {
        g_f = Frame{};
        g_f.state = 1;
        {  // (r117 DG4's diagnostic) pass 1's opens more than 2 presents apart: a mono stretch (the pause) ended
            const uint64_t now = state::presents.load(std::memory_order_relaxed);
            if (g_fs_last_open && now > g_fs_last_open + 2) g_fs_diag_left = kFsDiag;
            g_fs_last_open = now;
        }
        if (g_uc_installed.load(std::memory_order_relaxed)) {  // ([Stereo] MainUnionCull) the frame's key tables, fresh
            keys::begin(g_uc_keys);  // (the generation; wrapped: the tables cleared once)
            g_ucf = UcFrame{};
        }
        g_f.ctl = ctl;
        g_f.slice = slice;  // ([Stereo] MainSlice: latched by the render thread's marker; pass 2 drops only on such a frame)
        if (slice) bump(g_sl_frames);
        if (!ok) g_f.why |= kXGate;
        if (g_rg_installed.load(std::memory_order_relaxed)) rg_latch(ck);  // ([Stereo] MainFamily=region, item 8e; rg_latch checks pso's switch)
    } else {
        if (g_f.state != 2) {
            bump(g_p_order);
            g_f.why |= kXOrder;
        }
        g_f.state = 3;
        if (!ok) g_f.why |= kXGate;
    }
    g_r = Region{};
    g_r.open = true;
    g_r.pass = pass;
    g_r.ctl = ctl;
    g_r.ok = g_f.why == 0 && (pass == 1 || (g_f.seeded && g_f.gen == g_gen));
    g_r.tid = GetCurrentThreadId();
    g_r.d0 = d3d::thread_draws();
    bump(g_p_opens[pass]);
    pso::walk_open(pass);  // (GOAL-single-pass item 6c: the frame latch; pass 1 resets the prevworld verdict)
    g_listen.store(true, std::memory_order_release);
}
void cb_open1(void*) { open_region(1, true, false); }
void cb_open1_ctl(void*) { open_region(1, true, true); }
void cb_open1_x(void*) { open_region(1, false, false); }
// ([Stereo] MainSlice) pass 1's open with the slice mode latched (1 on, 2 nomerge, 3 always), with or without the rows' control
template <bool Ctl, int Slice>
void cb_open1_sl(void*) {
    open_region(1, true, Ctl, Slice);
}
using MarkFn = void (*)(void*);
constexpr MarkFn kOpen1Sl[2][4] = {{&cb_open1_sl<false, 1>, &cb_open1_sl<false, 2>, &cb_open1_sl<false, 3>, &cb_open1_sl<false, 4>},
                                   {&cb_open1_sl<true, 1>, &cb_open1_sl<true, 2>, &cb_open1_sl<true, 3>, &cb_open1_sl<true, 4>}};
// ([Stereo] MainFamily=region, item 8e) pass 1's open of a control frame with its kind (1 nudge .. 5 ip), for the per-kind results
template <int K>
void cb_open1_ck(void*) {
    open_region(1, true, true, 0, K);
}
constexpr MarkFn kOpen1Ck[kCkMarked] = {  // (the render thread marks these kinds only; seed and single are the playback's)
    &cb_open1_ctl, &cb_open1_ck<1>, &cb_open1_ck<2>, &cb_open1_ck<3>, &cb_open1_ck<4>, &cb_open1_ck<5>};
void cb_open2(void*) { open_region(2, true, false); }
void cb_open2_x(void*) { open_region(2, false, false); }
void cb_close(void*) {
    if (g_r.open && GetCurrentThreadId() != g_r.tid) bump(g_p_other_thread);
    close_region(nullptr, false);
}
void cb_uc(void*) {  // ([Stereo] MainUnionCull) after pass 1's open marker: DVL call 1's walk culls against the union this frame
    if (g_r.open && g_r.pass == 1) g_r.uc = true;
    else bump(g_uc_mark_lost);
}

// ---- the compare (the worker thread)
struct Diff {
    uint64_t texels = 0, worst = 0;
    UINT x0 = ~0u, y0 = ~0u, x1 = 0, y1 = 0;
    double wabs = 0.0;  // (item 8e's diagnosis) the largest difference in the plane's own units: bytes (mode 0), 24-bit depth (1), half floats (2)
};
float half_to_float(uint16_t h);
// mode 0 every byte, 1 the depth's low 24 bits (plane 0 of D24S8, the high byte undefined), 2 every byte compared as mode 0, and
// wabs taken over the texel's half floats (RGBA16F, RG16F; a non-finite half on either side counts as infinite)
Diff diff(const uint8_t* base, const Job& j, int a, int b, int mode) {
    Diff d;
    const UINT w = j.fp[a].Footprint.Width, h = j.rows[a];
    const UINT64 rb = j.rowbytes[a];
    const UINT bpt = w ? static_cast<UINT>(rb / w) : 0;
    if (!bpt || w != j.fp[b].Footprint.Width || h != j.rows[b] || rb != j.rowbytes[b]) {
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
            uint64_t dd = 0;
            if (mode == 1 && bpt == 4) {
                uint32_t va = 0, vb = 0;
                std::memcpy(&va, ta, 4);
                std::memcpy(&vb, tb, 4);
                va &= 0x00ffffffu;
                vb &= 0x00ffffffu;
                if (va == vb) continue;
                dd = va > vb ? va - vb : vb - va;
            } else {
                for (UINT k = 0; k < bpt; ++k) {
                    const uint64_t e = ta[k] > tb[k] ? ta[k] - tb[k] : tb[k] - ta[k];
                    if (e > dd) dd = e;
                }
            }
            if (mode == 2 && bpt % 2 == 0) {
                for (UINT k = 0; k + 1 < bpt; k += 2) {
                    uint16_t ha = 0, hb = 0;
                    std::memcpy(&ha, ta + k, 2);
                    std::memcpy(&hb, tb + k, 2);
                    if (ha == hb) continue;
                    const float fa = half_to_float(ha), fb = half_to_float(hb);
                    double e = std::fabs(static_cast<double>(fa) - static_cast<double>(fb));
                    if (((ha >> 10) & 0x1f) == 31 || ((hb >> 10) & 0x1f) == 31 || e != e) e = HUGE_VAL;
                    if (e > d.wabs) d.wabs = e;
                }
            } else if (static_cast<double>(dd) > d.wabs) {
                d.wabs = static_cast<double>(dd);
            }
            if (dd > d.worst) d.worst = dd;
            if (x < d.x0) d.x0 = x;
            if (y < d.y0) d.y0 = y;
            if (x > d.x1) d.x1 = x;
            if (y > d.y1) d.y1 = y;
            ++d.texels;
        }
    }
    return d;
}
// texels with any byte set in slot a or slot b (b < 0: a only); ~0 on a layout mismatch
uint64_t nonzero(const uint8_t* base, const Job& j, int a, int b) {
    const UINT w = j.fp[a].Footprint.Width, h = j.rows[a];
    const UINT bpa = w ? static_cast<UINT>(j.rowbytes[a] / w) : 0;
    const UINT bpb = b >= 0 && w ? static_cast<UINT>(j.rowbytes[b] / w) : 0;
    if (!bpa || (b >= 0 && (!bpb || j.fp[b].Footprint.Width != w || j.rows[b] != h))) return ~0ull;
    uint64_t n = 0;
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* pa = base + j.fp[a].Offset + static_cast<UINT64>(y) * j.fp[a].Footprint.RowPitch;
        const uint8_t* pb = b >= 0 ? base + j.fp[b].Offset + static_cast<UINT64>(y) * j.fp[b].Footprint.RowPitch : nullptr;
        for (UINT x = 0; x < w; ++x) {
            bool any = false;
            for (UINT k = 0; k < bpa && !any; ++k) any = pa[static_cast<size_t>(x) * bpa + k] != 0;
            for (UINT k = 0; pb && k < bpb && !any; ++k) any = pb[static_cast<size_t>(x) * bpb + k] != 0;
            if (any) ++n;
        }
    }
    return n;
}
// (run 25 r61/r62, the family compare: CliffWall's duplicates wrote nothing) a depth plane's spread: texels at 0, at the far
// value (0xffffff), and the others' least and most 24-bit values; the last compared frame's, per R slice (g_c_dz)
std::atomic<uint64_t> g_c_dz[2][4];  // [slice][zero, far, min, max]
void depth_spread(const uint8_t* base, const Job& j, int a, int sl) {
    const UINT w = j.fp[a].Footprint.Width, h = j.rows[a];
    if (!w || j.rowbytes[a] / w != 4) return;
    uint64_t zero = 0, at_far = 0, lo = ~0ull, hi = 0;
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* pa = base + j.fp[a].Offset + static_cast<UINT64>(y) * j.fp[a].Footprint.RowPitch;
        for (UINT x = 0; x < w; ++x) {
            uint32_t v = 0;
            std::memcpy(&v, pa + static_cast<size_t>(x) * 4, 4);
            v &= 0x00ffffffu;
            if (v == 0) ++zero;
            else if (v == 0x00ffffffu) ++at_far;
            else {
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
        }
    }
    g_c_dz[sl][0].store(zero, std::memory_order_relaxed);
    g_c_dz[sl][1].store(at_far, std::memory_order_relaxed);
    g_c_dz[sl][2].store(lo == ~0ull ? 0 : lo, std::memory_order_relaxed);
    g_c_dz[sl][3].store(hi, std::memory_order_relaxed);
}
// (run 25, the family compare: eye 2's velocity differs on every texel) the velocity plane's mean (x, y) over the texels where
// R1's colour or velocity is written, for S1 and R1 (RG16F); the last compared frame's: g_c_vm[0] S1, [1] R1, [2] the texels
std::atomic<double> g_c_vm[3][2], g_c_vm0[3][2];  // (g_c_vm0: the same for S0 and R0, eye 1)
float half_to_float(uint16_t h) {
    const uint32_t sgn = (h & 0x8000u) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ffu;
    uint32_t f;
    if (e == 0) {
        if (!m) {
            f = sgn;
        } else {  // (subnormal)
            int ee = -1;
            uint32_t mm = m;
            do {
                ++ee;
                mm <<= 1;
            } while (!(mm & 0x400u));
            f = sgn | static_cast<uint32_t>(127 - 15 - ee) << 23 | (mm & 0x3ffu) << 13;
        }
    } else if (e == 31) {
        f = sgn | 0x7f800000u | m << 13;
    } else {
        f = sgn | (e + 127 - 15) << 23 | m << 13;
    }
    float out;
    std::memcpy(&out, &f, 4);
    return out;
}
void vel_mean(const uint8_t* base, const Job& j, int sv, int rv, int rc, std::atomic<double> (&out)[3][2]) {
    const UINT w = j.fp[sv].Footprint.Width, h = j.rows[sv];
    if (!w || j.rowbytes[sv] / w != 4 || j.rowbytes[rv] / w != 4 || j.fp[rv].Footprint.Width != w || j.fp[rc].Footprint.Width != w ||
        j.rows[rv] != h || j.rows[rc] != h || !j.rowbytes[rc])
        return;
    const UINT bc = static_cast<UINT>(j.rowbytes[rc] / w);
    double acc[2][2] = {}, n = 0, bad = 0;  // (bad: a texel with an inf or NaN half, the review's must-fix: left out, counted)
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* ps = base + j.fp[sv].Offset + static_cast<UINT64>(y) * j.fp[sv].Footprint.RowPitch;
        const uint8_t* pr = base + j.fp[rv].Offset + static_cast<UINT64>(y) * j.fp[rv].Footprint.RowPitch;
        const uint8_t* pc = base + j.fp[rc].Offset + static_cast<UINT64>(y) * j.fp[rc].Footprint.RowPitch;
        for (UINT x = 0; x < w; ++x) {
            uint16_t a[2], b[2];
            std::memcpy(a, ps + static_cast<size_t>(x) * 4, 4);
            std::memcpy(b, pr + static_cast<size_t>(x) * 4, 4);
            bool any = b[0] || b[1];
            for (UINT k = 0; k < bc && !any; ++k) any = pc[static_cast<size_t>(x) * bc + k] != 0;
            if (!any) continue;
            if (((a[0] >> 10) & 0x1f) == 31 || ((a[1] >> 10) & 0x1f) == 31 || ((b[0] >> 10) & 0x1f) == 31 || ((b[1] >> 10) & 0x1f) == 31) {
                bad += 1;
                continue;
            }
            n += 1;
            for (int c = 0; c < 2; ++c) {
                acc[0][c] += half_to_float(a[c]);
                acc[1][c] += half_to_float(b[c]);
            }
        }
    }
    for (int s = 0; s < 2; ++s)
        for (int c = 0; c < 2; ++c) out[s][c].store(n ? acc[s][c] / n : 0.0, std::memory_order_relaxed);
    out[2][0].store(n, std::memory_order_relaxed);
    out[2][1].store(bad, std::memory_order_relaxed);
}
// ([Stereo] MainSlice) S1's marked texels (its stencil, seeded 0, nonzero) and their split: passing where R1's depth (the
// game's own draws at the drop point) equals S1's (the kept fragment won: slice.md 1.2), failing otherwise (an occluded texel)
struct Split {
    uint64_t marked = 0, passing = 0, failing = 0;
    bool bad = false;
};
Split splits(const uint8_t* base, const Job& j, int st, int ds, int dr) {
    Split s;
    const UINT w = j.fp[st].Footprint.Width, h = j.rows[st];
    const UINT bs = w ? static_cast<UINT>(j.rowbytes[st] / w) : 0, bd = w ? static_cast<UINT>(j.rowbytes[ds] / w) : 0;
    if (!bs || bd != 4 || j.fp[ds].Footprint.Width != w || j.rows[ds] != h || j.fp[dr].Footprint.Width != w || j.rows[dr] != h ||
        j.rowbytes[dr] != j.rowbytes[ds]) {
        s.bad = true;
        return s;
    }
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* ps = base + j.fp[st].Offset + static_cast<UINT64>(y) * j.fp[st].Footprint.RowPitch;
        const uint8_t* pd = base + j.fp[ds].Offset + static_cast<UINT64>(y) * j.fp[ds].Footprint.RowPitch;
        const uint8_t* pr = base + j.fp[dr].Offset + static_cast<UINT64>(y) * j.fp[dr].Footprint.RowPitch;
        for (UINT x = 0; x < w; ++x) {
            bool m = false;
            for (UINT k = 0; k < bs && !m; ++k) m = ps[static_cast<size_t>(x) * bs + k] != 0;
            if (!m) continue;
            ++s.marked;
            uint32_t a = 0, b = 0;
            std::memcpy(&a, pd + static_cast<size_t>(x) * 4, 4);
            std::memcpy(&b, pr + static_cast<size_t>(x) * 4, 4);
            if ((a & 0x00ffffffu) == (b & 0x00ffffffu)) ++s.passing;
            else ++s.failing;
        }
    }
    return s;
}
// ([Stereo] MainSlice) a handed-over compare frame: G1 (C, the merged targets, against R1, the game's own draws there), eye 1
// (S0 against R0), the negative control (C against R0's colour), Reactivity zero in S0, S1, R0 (R1 and C hold the game's)
void compare_slice(const uint8_t* p, const Job& j) {
    Diff g1[kPlanes], e1[kPlanes];
    for (int pl = 0; pl < kPlanes; ++pl) {
        const int mode = pl == 3 ? 1 : 0;
        g1[pl] = diff(p, j, slot_c(pl), slot(kR, 1, pl), mode);
        e1[pl] = diff(p, j, slot(kS, 0, pl), slot(kR, 0, pl), mode);
    }
    const Diff neg = diff(p, j, slot_c(0), slot(kR, 0, 0), 0);
    const Split sp = splits(p, j, slot(kS, 1, 4), slot(kS, 1, 3), slot(kR, 1, 3));
    uint64_t react = 0;
    for (const int x : {slot(kS, 0, 2), slot(kS, 1, 2), slot(kR, 0, 2)}) {
        const uint64_t n = nonzero(p, j, x, -1);
        react = n == ~0ull || react == ~0ull ? ~0ull : react + n;
    }
    bool layout = sp.bad || react == ~0ull || neg.texels == ~0ull;
    bool exact_g1 = true, exact1 = true;
    uint64_t t = 0;
    for (int pl = 0; pl < kPlanes; ++pl) {
        layout = layout || g1[pl].texels == ~0ull || e1[pl].texels == ~0ull;
        exact_g1 = exact_g1 && g1[pl].texels == 0;
        exact1 = exact1 && e1[pl].texels == 0;
        if (g1[pl].texels != ~0ull) t += g1[pl].texels;
    }
    bump(g_c_frames);  // (as item 3: every compared frame, the control's too)
    if (layout) {
        bump(g_c_layout);
        return;
    }
    if (!j.ctl && j.slice == 4) {  // (the identity control: G1 must hold with nothing dropped: R1 is the game's real result)
        bump(g_c_id_frames);
        if (exact_g1) bump(g_c_id_exact);
        if (exact1) bump(g_c_id_e1);
        log::limited("mi.slid", 20, "[mi] slice compared an identity frame: C (the game's targets after its own listed draws) against R1: %llu texels differ "
                     "(colour %llu, velocity %llu, reactivity %llu, depth %llu, stencil %llu); eye 1 %s", static_cast<unsigned long long>(t),
                     static_cast<unsigned long long>(g1[0].texels), static_cast<unsigned long long>(g1[1].texels),
                     static_cast<unsigned long long>(g1[2].texels), static_cast<unsigned long long>(g1[3].texels),
                     static_cast<unsigned long long>(g1[4].texels), exact1 ? "bit-exact" : "differs");
        return;
    }
    const int ci = j.ctl ? 0 : j.slice == 2 ? 1 : j.slice == 3 ? 2 : -1;  // (a control frame: the rows, nomerge, always)
    if (ci >= 0) {  // (counted apart, never in the pass: G1 must fail where the control's case occurs)
        bump(g_c_ctl);
        bump(g_c_sc_frames[ci]);
        if (!exact_g1) bump(g_c_ctl_e2_differ);
        if (exact1) {
            bump(g_c_ctl_e1_exact);
            bump(g_c_sc_e1[ci]);
        }
        const bool has_case = ci == 0 ? sp.marked > 0 : ci == 1 ? sp.passing > 0 : sp.failing > 0;
        if (has_case) {
            bump(g_c_sc_case[ci]);
            if (!exact_g1) bump(g_c_sc_fail[ci]);
        }
        store_min(g_c_ctl_e2_min, t);
        store_min(g_c_sc_min[ci], t);
        log::limited("mi.slctl", 30, "[mi] slice compared a control frame (%s): G1 differs in %llu texels (colour %llu, velocity %llu, reactivity %llu, depth %llu, "
                     "stencil %llu), eye 1 %s; marked %llu (passing %llu, failing %llu)", ci == 0 ? "rows" : ci == 1 ? "nomerge" : "always",
                     static_cast<unsigned long long>(t), static_cast<unsigned long long>(g1[0].texels), static_cast<unsigned long long>(g1[1].texels),
                     static_cast<unsigned long long>(g1[2].texels), static_cast<unsigned long long>(g1[3].texels),
                     static_cast<unsigned long long>(g1[4].texels), exact1 ? "bit-exact" : "differs", static_cast<unsigned long long>(sp.marked),
                     static_cast<unsigned long long>(sp.passing), static_cast<unsigned long long>(sp.failing));
        return;
    }
    bump(g_c_sl_frames);
    if (exact_g1) bump(g_c_g1_exact);
    if (exact1) bump(g_c_e1_exact);
    if (neg.texels) bump(g_c_neg_differ);
    if (react == 0) bump(g_c_react_zero);
    if (sp.marked) bump(g_c_sl_marked);
    if (sp.passing) bump(g_c_pass_frames);
    if (sp.failing) bump(g_c_fail_frames);
    g_c_pass_tex.fetch_add(sp.passing, std::memory_order_relaxed);
    g_c_fail_tex.fetch_add(sp.failing, std::memory_order_relaxed);
    store_min(g_c_marked_min, sp.marked);
    store_max(g_c_marked_max, sp.marked);
    if (exact_g1 && exact1 && neg.texels && react == 0 && sp.marked) bump(g_c_sl_pass);
    for (int pl = 0; pl < kPlanes; ++pl) {
        store_max(g_c_g1_max[pl], g1[pl].texels);
        store_max(g_c_e1_max[pl], e1[pl].texels);
        store_max(g_c_g1_worst, g1[pl].worst);
        if (g1[pl].texels) {
            g_c_g1_where[0] = static_cast<uint32_t>(pl);
            g_c_g1_where[1] = g1[pl].x0;
            g_c_g1_where[2] = g1[pl].y0;
            g_c_g1_where[3] = g1[pl].x1;
            g_c_g1_where[4] = g1[pl].y1;
            g_c_g1_where[5] = static_cast<uint32_t>(g1[pl].texels);
        }
    }
    store_min(g_c_neg_min, neg.texels);
    store_max(g_c_react_max, react);
    log::limited("mi.slcmp", 40, "[mi] slice compared (%s): G1 (C vs R1) colour %llu velocity %llu reactivity %llu depth %llu stencil %llu texels differ (worst %llu); "
                 "eye 1 (S0 vs R0) %llu %llu %llu %llu %llu; negative control (C vs R0, colour) %llu; reactivity written %llu; marked %llu "
                 "(passing %llu, failing %llu); listed %d", kSlName[j.slice >= 0 && j.slice <= 4 ? j.slice : 0], static_cast<unsigned long long>(g1[0].texels),
                 static_cast<unsigned long long>(g1[1].texels), static_cast<unsigned long long>(g1[2].texels), static_cast<unsigned long long>(g1[3].texels),
                 static_cast<unsigned long long>(g1[4].texels), static_cast<unsigned long long>(g1[0].worst > g1[1].worst ? g1[0].worst : g1[1].worst),
                 static_cast<unsigned long long>(e1[0].texels), static_cast<unsigned long long>(e1[1].texels),
                 static_cast<unsigned long long>(e1[2].texels), static_cast<unsigned long long>(e1[3].texels),
                 static_cast<unsigned long long>(e1[4].texels), static_cast<unsigned long long>(neg.texels), static_cast<unsigned long long>(react),
                 static_cast<unsigned long long>(sp.marked), static_cast<unsigned long long>(sp.passing), static_cast<unsigned long long>(sp.failing),
                 j.listed);
}
// ---- ([Stereo] MainFamily=region, item 8e's diagnosis: solo F not exact where J is; test only, the worker's, the mod's readback only)
// per compare kind (joint, solo, subset, swap) and control kind (0 none): per plane and eye (0 eye 2 S1 / R1, 1 eye 1 S0 / R0) the frames
// with a difference, the texels summed, the most in one frame, and the largest difference in the plane's units (half floats for the
// colour and velocity, bytes for reactivity and stencil, 24-bit depth)
constexpr int kPm = 5;  // [0] unused, kRgJoint, kRgSolo, kRgSubset, [4] a swap frame
const char* const kPmName[kPm] = {"none", "joint", "solo", "subset", "swap"};
const char* const kPlName[kPlanes] = {"fsc", "vel", "react", "depth", "stencil"};
struct PlaneAcc {
    Count frames;
    Count nf[2][kPlanes], tx[2][kPlanes];
    std::atomic<uint64_t> mt[2][kPlanes];
    std::atomic<double> wa[2][kPlanes];
};
PlaneAcc g_pa[kPm][kCk];
void store_maxd(std::atomic<double>& a, double v) {  // (the worker is the only writer)
    if (v > a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed);
}
// eye 2's texels by who wrote them (S1, R1 colour or velocity non-zero: both are cleared at the seed): S1 only, R1 only, both and the
// colour or velocity differs, both and equal; and the texels where neither was written but the depth or stencil differs
struct Split2 {
    uint64_t s_only = 0, r_only = 0, both_diff = 0, both_eq = 0, ds_only = 0;
    bool bad = true;
};
Split2 split2(const uint8_t* base, const Job& j) {
    Split2 s;
    const int sc = slot(kS, 1, 0), sv = slot(kS, 1, 1), rc = slot(kR, 1, 0), rv = slot(kR, 1, 1), sd = slot(kS, 1, 3), rd = slot(kR, 1, 3),
              ss = slot(kS, 1, 4), rs = slot(kR, 1, 4);
    const UINT w = j.fp[sc].Footprint.Width, h = j.rows[sc];
    for (const int x : {sv, rc, rv, sd, rd, ss, rs})
        if (j.fp[x].Footprint.Width != w || j.rows[x] != h) return s;
    const UINT bc = w ? static_cast<UINT>(j.rowbytes[sc] / w) : 0, bv = w ? static_cast<UINT>(j.rowbytes[sv] / w) : 0,
               bd = w ? static_cast<UINT>(j.rowbytes[sd] / w) : 0, bs = w ? static_cast<UINT>(j.rowbytes[ss] / w) : 0;
    if (!bc || !bv || bd != 4 || !bs || j.rowbytes[rc] != j.rowbytes[sc] || j.rowbytes[rv] != j.rowbytes[sv] || j.rowbytes[rd] != j.rowbytes[sd] ||
        j.rowbytes[rs] != j.rowbytes[ss])
        return s;
    s.bad = false;
    auto row = [&](int x, UINT y) { return base + j.fp[x].Offset + static_cast<UINT64>(y) * j.fp[x].Footprint.RowPitch; };
    auto any = [](const uint8_t* q, UINT n) {
        for (UINT k = 0; k < n; ++k)
            if (q[k]) return true;
        return false;
    };
    for (UINT y = 0; y < h; ++y) {
        const uint8_t *p_sc = row(sc, y), *p_sv = row(sv, y), *p_rc = row(rc, y), *p_rv = row(rv, y), *p_sd = row(sd, y), *p_rd = row(rd, y),
                      *p_ss = row(ss, y), *p_rs = row(rs, y);
        for (UINT x = 0; x < w; ++x) {
            const uint8_t *a = p_sc + static_cast<size_t>(x) * bc, *b = p_rc + static_cast<size_t>(x) * bc;
            const uint8_t *va = p_sv + static_cast<size_t>(x) * bv, *vb = p_rv + static_cast<size_t>(x) * bv;
            const bool ws = any(a, bc) || any(va, bv), wr = any(b, bc) || any(vb, bv);
            if (ws && wr) {
                if (std::memcmp(a, b, bc) || std::memcmp(va, vb, bv)) ++s.both_diff;
                else ++s.both_eq;
            } else if (ws) {
                ++s.s_only;
            } else if (wr) {
                ++s.r_only;
            } else {
                uint32_t da = 0, db = 0;
                std::memcpy(&da, p_sd + static_cast<size_t>(x) * 4, 4);
                std::memcpy(&db, p_rd + static_cast<size_t>(x) * 4, 4);
                if ((da & 0x00ffffffu) != (db & 0x00ffffffu) || std::memcmp(p_ss + static_cast<size_t>(x) * bs, p_rs + static_cast<size_t>(x) * bs, bs))
                    ++s.ds_only;
            }
        }
    }
    return s;
}
// the last failing frame (a compare or subset frame not exact in either eye; a control frame with eye 2 exact or eye 1 not), its numbers
struct LastFail {
    int pm = 0, ck = 0, solo = -1, swap = -1, listed1 = 0, listed2 = 0;
    uint64_t frame = 0, cov = 0, neg = 0;
    uint64_t tx[2][kPlanes]{};
    double wa[2][kPlanes]{};
    UINT box[kPlanes][4]{};  // (eye 2: x0, y0, x1, y1 of the differing texels)
    Split2 sp;
};
std::mutex g_lf_mutex;
LastFail g_lf;
Count g_lf_n;
// (item 8e's bisect step 0, under g_lf_mutex) the last kLff failing frames that are no control or swap frame (lf is the last one only),
// for matching with main_region's tier-2 frames
constexpr unsigned kLff = 16;
uint64_t g_lff[kLff];
unsigned g_lff_n = 0;
// "prepass mi region save <dir> [any|fail]": one-shot, the next compared frame of the selection latched at the command (the mode and its
// solo ordinal), any kind (any) or the next one not exact in either eye that is no control or swap frame (fail): S1's and R1's five planes
// written as RDE1 files (eye_grab's header: "RDE1", width, height, the footprint's DXGI format, eye 1 = eye 2, the frame's low 32 bits, the
// bytes per row, 0; then the rows, top first) and a .txt of the frame's numbers; on the worker thread, from the mapped readback
std::mutex g_sv_mutex;
std::string g_sv_dir, g_sv_last = "none";
std::atomic<int> g_sv_want{0};  // 0 off, 1 any, 2 fail
int g_sv_sel = 0;               // (under g_sv_mutex) g_rg_sel at the command
Count g_sv_done, g_sv_failed;
std::string sv_plane(const std::string& path, const uint8_t* base, const Job& j, int x, uint64_t frame) {
    std::wstring wp(path.begin(), path.end()), tmp = wp + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return "cannot open " + path;
    const uint32_t hdr[8] = {0x31454452u /* RDE1 */, j.fp[x].Footprint.Width, j.rows[x], static_cast<uint32_t>(j.fp[x].Footprint.Format), 1u,
                             static_cast<uint32_t>(frame), static_cast<uint32_t>(j.rowbytes[x]), 0u};
    bool ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    for (UINT y = 0; ok && y < j.rows[x]; ++y)
        ok = fwrite(base + j.fp[x].Offset + static_cast<UINT64>(y) * j.fp[x].Footprint.RowPitch, 1, static_cast<size_t>(j.rowbytes[x]), f) ==
             static_cast<size_t>(j.rowbytes[x]);
    ok = fclose(f) == 0 && ok && MoveFileExW(tmp.c_str(), wp.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(tmp.c_str());
    return ok ? std::string() : "cannot write " + path;
}
void sv_frame(const uint8_t* base, const Job& j, const LastFail& r, bool exact2, bool exact1) {
    std::string dir;
    {
        std::lock_guard lock(g_sv_mutex);
        dir = g_sv_dir;
    }
    char tag[64];
    std::snprintf(tag, sizeof(tag), "region_%llu_%s_%d", static_cast<unsigned long long>(r.frame), kPmName[r.pm], r.solo);
    std::string err;
    int files = 0;
    for (int set = 0; set < kSets && err.empty(); ++set)
        for (int pl = 0; pl < kPlanes && err.empty(); ++pl) {
            err = sv_plane(dir + "\\" + tag + (set == kS ? "_S1_" : "_R1_") + kPlName[pl] + ".rde", base, j, slot(set, 1, pl), r.frame);
            if (err.empty()) ++files;
        }
    if (err.empty()) {
        const std::string mp = dir + "\\" + tag + ".txt";
        FILE* f = nullptr;
        if (fopen_s(&f, mp.c_str(), "wb") != 0 || !f) {
            err = "cannot open " + mp;
        } else {
            std::fprintf(f, "frame=%llu mode=%s ck=%s solo=%d swap=%d listed1=%d listed2=%d exact2=%d exact1=%d cov=%llu neg=%llu\n",
                         static_cast<unsigned long long>(r.frame), kPmName[r.pm], kCkName[r.ck], r.solo, r.swap, r.listed1, r.listed2, exact2 ? 1 : 0,
                         exact1 ? 1 : 0, static_cast<unsigned long long>(r.cov), static_cast<unsigned long long>(r.neg));
            for (int pl = 0; pl < kPlanes; ++pl)
                std::fprintf(f, "%s e2_texels=%llu e2_maxabs=%.9g e2_box=%u,%u,%u,%u e1_texels=%llu e1_maxabs=%.9g\n", kPlName[pl],
                             static_cast<unsigned long long>(r.tx[0][pl]), r.wa[0][pl], r.box[pl][0], r.box[pl][1], r.box[pl][2], r.box[pl][3],
                             static_cast<unsigned long long>(r.tx[1][pl]), r.wa[1][pl]);
            std::fprintf(f, "split s_only=%llu r_only=%llu both_diff=%llu both_eq=%llu ds_only=%llu bad=%d\n",
                         static_cast<unsigned long long>(r.sp.s_only), static_cast<unsigned long long>(r.sp.r_only),
                         static_cast<unsigned long long>(r.sp.both_diff), static_cast<unsigned long long>(r.sp.both_eq),
                         static_cast<unsigned long long>(r.sp.ds_only), r.sp.bad ? 1 : 0);
            std::fprintf(f, "files: <tag>_S1_<plane>.rde and <tag>_R1_<plane>.rde, planes fsc vel react depth stencil (RDE1, eye 1 = eye 2)\n");
            if (fclose(f) != 0) err = "cannot write " + mp;
            else ++files;
        }
    }
    char msg[512];
    std::snprintf(msg, sizeof(msg), "%s %s\\%s (%d files; %s, %s, solo ordinal %d, eye 2 %s, eye 1 %s)%s%s", err.empty() ? "saved" : "FAILED", dir.c_str(),
                  tag, files, kPmName[r.pm], kCkName[r.ck], r.solo, exact2 ? "exact" : "differs", exact1 ? "exact" : "differs", err.empty() ? "" : ": ",
                  err.c_str());
    bump(err.empty() ? g_sv_done : g_sv_failed);
    {
        std::lock_guard lock(g_sv_mutex);
        g_sv_last = msg;
    }
    log::info("[mi] region save: %s", msg);
}
// the per-plane counts, the last failing frame and the one-shot save, for a compared region frame (layout checked)
void rg_planes(const uint8_t* p, const Job& j, const Diff (&e2)[kPlanes], const Diff (&e1)[kPlanes], bool exact2, bool exact1, uint64_t cov,
               uint64_t neg) {
    const int m = j.rmode > 0 && j.rmode < kRgModes ? j.rmode : kRgJoint;
    const int ck = j.ctl && j.ck > 0 && j.ck < kCk ? j.ck : 0;
    const bool swap = !j.ctl && j.swap >= 0 && j.swap < kOrds;
    const int pm = swap ? 4 : m;
    PlaneAcc& a = g_pa[pm][ck];
    bump(a.frames);
    for (int e = 0; e < 2; ++e)
        for (int pl = 0; pl < kPlanes; ++pl) {
            const Diff& d = e ? e1[pl] : e2[pl];
            if (!d.texels) continue;
            bump(a.nf[e][pl]);
            a.tx[e][pl].fetch_add(d.texels, std::memory_order_relaxed);
            store_max(a.mt[e][pl], d.texels);
            store_maxd(a.wa[e][pl], d.wabs);
        }
    const bool failing = j.ctl ? (exact2 || !exact1) : !swap && (!exact2 || !exact1);
    const int want = g_sv_want.load(std::memory_order_relaxed);
    bool save = false;
    if (want) {
        int sel = 0;
        {
            std::lock_guard lock(g_sv_mutex);
            sel = g_sv_sel;
        }
        const int smode = sel & 0xff, ssolo = smode == kRgSolo ? (sel >> 8) - 1 : -1;
        save = j.rmode == smode && (smode != kRgSolo || j.solo == ssolo) && (want == 1 || (!j.ctl && !swap && (!exact2 || !exact1)));
        if (save) {
            int w = want;
            save = g_sv_want.compare_exchange_strong(w, 0);  // (one-shot)
        }
    }
    if (!failing && !save) return;
    LastFail r;
    r.pm = pm;
    r.ck = ck;
    r.solo = m == kRgSolo ? j.solo : -1;
    r.swap = j.swap;
    r.listed1 = j.listed;
    r.listed2 = j.listed2;
    r.frame = j.frame;
    r.cov = cov;
    r.neg = neg;
    for (int pl = 0; pl < kPlanes; ++pl) {
        r.tx[0][pl] = e2[pl].texels;
        r.tx[1][pl] = e1[pl].texels;
        r.wa[0][pl] = e2[pl].wabs;
        r.wa[1][pl] = e1[pl].wabs;
        if (e2[pl].texels) {
            r.box[pl][0] = e2[pl].x0;
            r.box[pl][1] = e2[pl].y0;
            r.box[pl][2] = e2[pl].x1;
            r.box[pl][3] = e2[pl].y1;
        }
    }
    r.sp = split2(p, j);
    if (failing) {
        bump(g_lf_n);
        std::lock_guard lock(g_lf_mutex);
        g_lf = r;
        if (!j.ctl && !swap) g_lff[g_lff_n++ % kLff] = r.frame;
    }
    if (save) sv_frame(p, j, r, exact2, exact1);
}
// ([Stereo] MainFamily=region, item 8e) a region frame: eye 2 (S1 / R1) and eye 1 (S0 / R0) on the five planes, the negative control
// (S1 / R0's colour), coverage (R1's written texels), reactivity written (information in joint frames; its plane is in the exactness);
// no depth_spread or vel_mean (the worker's speed). Counted by mode and control kind, and per pair by ordinal: a solo frame is its pair's,
// a joint frame counts for every pair that drew in its pass 1, a swap frame for the swapped pair (its joint visibility: eye 2 differs)
void compare_region(const uint8_t* p, const Job& j) {
    Diff e2[kPlanes], e1[kPlanes];
    for (int pl = 0; pl < kPlanes; ++pl) {
        const int mode = pl == 3 ? 1 : pl < 2 ? 2 : 0;  // (2: the colour's and velocity's largest difference as half floats; texels as before)
        e2[pl] = diff(p, j, slot(kS, 1, pl), slot(kR, 1, pl), mode);
        e1[pl] = diff(p, j, slot(kS, 0, pl), slot(kR, 0, pl), mode);
    }
    const Diff neg = diff(p, j, slot(kS, 1, 0), slot(kR, 0, 0), 0);
    const uint64_t cov = nonzero(p, j, slot(kR, 1, 0), slot(kR, 1, 1));
    uint64_t react = 0;
    for (int set = 0; set < kSets; ++set)
        for (int sl = 0; sl < 2; ++sl) {
            const uint64_t n = nonzero(p, j, slot(set, sl, 2), -1);
            react = n == ~0ull || react == ~0ull ? ~0ull : react + n;
        }
    bool layout = cov == ~0ull || react == ~0ull || neg.texels == ~0ull;
    bool exact2 = true, exact1 = true;
    uint64_t t2 = 0;
    for (int pl = 0; pl < kPlanes; ++pl) {
        layout = layout || e2[pl].texels == ~0ull || e1[pl].texels == ~0ull;
        exact2 = exact2 && e2[pl].texels == 0;
        exact1 = exact1 && e1[pl].texels == 0;
        if (e2[pl].texels != ~0ull) t2 += e2[pl].texels;
    }
    bump(g_c_frames);
    if (layout) {
        bump(g_c_layout);
        bump(g_rc_layout);
        return;
    }
    rg_planes(p, j, e2, e1, exact2, exact1, cov, neg.texels);  // (item 8e's diagnosis: per-plane counts, the last failing frame, the save)
    const int m = j.rmode > 0 && j.rmode < kRgModes ? j.rmode : kRgJoint;
    const int ck = j.ck > 0 && j.ck < kCk ? j.ck : 0;
    PairRes* solo = m == kRgSolo && j.solo >= 0 && j.solo < kOrds ? &g_pr[j.solo] : nullptr;
    if (j.ctl) {  // (a control frame: eye 2 must differ, eye 1 stay exact; never in the pass)
        bump(g_c_ctl);
        if (!exact2) bump(g_c_ctl_e2_differ);
        if (exact1) bump(g_c_ctl_e1_exact);
        store_min(g_c_ctl_e2_min, t2);
        bump(g_rc_ck_frames[m][ck]);
        if (!exact2) bump(g_rc_ck_e2[m][ck]);
        if (exact1) bump(g_rc_ck_e1[m][ck]);
        store_min(g_rc_ck_min[ck], t2);
        if (solo) {
            bump(solo->ck_frames[ck]);
            if (!exact2) bump(solo->ck_e2[ck]);
            if (exact1) bump(solo->ck_e1[ck]);
            if (cov > 0) {  // (a frame the pair wrote no texel in cannot show a control: counted apart)
                bump(solo->ck_cov[ck]);
                if (!exact2) bump(solo->ck_cov_e2[ck]);
            }
        }
        log::limited("mi.rgctl", 30, "[mi] region compared a control frame (%s, %s, solo ordinal %d): eye 2 differs in %llu texels (colour %llu, velocity %llu, "
                     "reactivity %llu, depth %llu, stencil %llu), eye 1 %s, coverage %llu, listed %d", kRgModeName[m], kCkName[ck],
                     solo ? j.solo : -1, static_cast<unsigned long long>(t2), static_cast<unsigned long long>(e2[0].texels),
                     static_cast<unsigned long long>(e2[1].texels), static_cast<unsigned long long>(e2[2].texels),
                     static_cast<unsigned long long>(e2[3].texels), static_cast<unsigned long long>(e2[4].texels), exact1 ? "bit-exact" : "differs",
                     static_cast<unsigned long long>(cov), j.listed);
        return;
    }
    if (j.swap >= 0 && j.swap < kOrds) {  // (a swap frame: the pair's joint visibility, information; never in the pass)
        PairRes& r = g_pr[j.swap];
        bump(g_rc_swap_frames);
        bump(r.swap);
        if (!exact2) {
            bump(g_rc_swap_differ);
            bump(r.swap_differ);
        }
        store_min(r.swap_min, t2);
        log::limited("mi.rgswap", 30, "[mi] region compared a swap frame (ordinal %d drawn by the game's pipeline into S0 and S1): eye 2 differs in "
                     "%llu texels, eye 1 %s", j.swap, static_cast<unsigned long long>(t2), exact1 ? "bit-exact" : "differs");
        return;
    }
    bump(g_rc_frames[m]);
    if (j.rep) {
        bump(g_rc_rep[m]);
        if (exact2) bump(g_rc_rep_e2[m]);
    }
    if (exact2) bump(g_rc_e2[m]);
    if (exact1) bump(g_rc_e1[m]);
    if (neg.texels) bump(g_rc_neg[m]);
    if (cov) bump(g_rc_cov[m]);
    if (react == 0) bump(g_rc_react0[m]);
    if (exact2 && exact1 && neg.texels && cov) bump(g_rc_pass[m]);
    store_min(g_rc_cov_min[m], cov);
    store_max(g_rc_cov_max[m], cov);
    store_max(g_rc_react_max[m], react);
    // (the generic maxima and the last difference's place, as item 3's kv shows them)
    if (exact2) bump(g_c_e2_exact);
    if (exact1) bump(g_c_e1_exact);
    if (neg.texels) bump(g_c_neg_differ);
    for (int pl = 0; pl < kPlanes; ++pl) {
        store_max(g_c_e2_max[pl], e2[pl].texels);
        store_max(g_c_e1_max[pl], e1[pl].texels);
        store_max(g_c_e2_worst, e2[pl].worst);
        if (e2[pl].texels) {
            g_c_where[0] = static_cast<uint32_t>(pl);
            g_c_where[1] = e2[pl].x0;
            g_c_where[2] = e2[pl].y0;
            g_c_where[3] = e2[pl].x1;
            g_c_where[4] = e2[pl].y1;
            g_c_where[5] = static_cast<uint32_t>(e2[pl].texels);
        }
    }
    store_min(g_c_neg_min, neg.texels);
    if (solo) {
        bump(solo->frames);
        if (exact2) bump(solo->e2);
        if (exact1) bump(solo->e1);
        if (neg.texels) bump(solo->neg);
        if (cov) bump(solo->cov);
        if (react == 0) bump(solo->react0);
        store_min(solo->cov_min, cov);
        store_max(solo->cov_max, cov);
    }
    if (m == kRgJoint)
        for (int o = 0; o < kOrds; ++o)
            if (j.drew >> o & 1) {
                bump(g_pr[o].joint);
                if (exact2 && exact1) bump(g_pr[o].joint_exact);
            }
    log::limited("mi.rgcmp", 40, "[mi] region compared (%s, solo ordinal %d): eye 2 (S1 vs R1) colour %llu velocity %llu reactivity %llu depth %llu stencil %llu "
                 "texels differ (worst %llu); eye 1 (S0 vs R0) %llu %llu %llu %llu %llu; negative control (S1 vs R0, colour) %llu; reactivity "
                 "written %llu; coverage %llu; listed %d", kRgModeName[m], solo ? j.solo : -1,
                 static_cast<unsigned long long>(e2[0].texels), static_cast<unsigned long long>(e2[1].texels),
                 static_cast<unsigned long long>(e2[2].texels), static_cast<unsigned long long>(e2[3].texels),
                 static_cast<unsigned long long>(e2[4].texels), static_cast<unsigned long long>(e2[0].worst > e2[1].worst ? e2[0].worst : e2[1].worst),
                 static_cast<unsigned long long>(e1[0].texels), static_cast<unsigned long long>(e1[1].texels),
                 static_cast<unsigned long long>(e1[2].texels), static_cast<unsigned long long>(e1[3].texels),
                 static_cast<unsigned long long>(e1[4].texels), static_cast<unsigned long long>(neg.texels), static_cast<unsigned long long>(react),
                 static_cast<unsigned long long>(cov), j.listed);
}
// ---- ([Stereo] MainRegionE1Compare, item 8f) the worker's e1f compare. R0 holds T (pass 2's four targets at its e1's first non-empty
// draw), R1 the seed (never drawn), S1 the twins plus pass 2's own draws, C pass 2's four at its e1 end (C2). Per plane (fsc, vel, react,
// depth, stencil): G0 = R1 against T (depth 24 bits, stencil; velocity and reactivity under DLSS only; FSC: R1 all the sentinel); G1 = S1
// against C2 (the same planes; FSC: where S1 is not the sentinel S1 = C2, where it is C2 = T); the negative control S1 against S0 (FSC
// where S1 is covered); coverage: S1's texels not the sentinel. Control frames are counted apart by kind (single: also over the frames its
// pair drew in pass 1)
Count g_e1f_cmp, g_e1f_cmp_ctl, g_e1f_g0_exact, g_e1f_g1_exact, g_e1f_pass, g_e1f_neg_differ, g_e1f_layout, g_e1f_dlss_frames, g_e1f_fxaa_frames;
Count g_e1f_g0_nf[kPlanes], g_e1f_g1_nf[kPlanes], g_e1f_g0_tx[kPlanes], g_e1f_g1_tx[kPlanes], g_e1f_unc_sum, g_e1f_ck_frames[kCk], g_e1f_ck_g1[kCk], g_e1f_ck_drew[kCk], g_e1f_ck_drew_g1[kCk];
// (GOAL-single-pass-2 item 4) e1f compares of ref h1's frames (layout, control and plain alike): [1] G1 frames, [2] G1 frames refused for
// another reason, [3] H1 frames refused ([0] unused)
Count g_e1f_alt[4];
std::atomic<uint64_t> g_e1f_g0_max[kPlanes], g_e1f_g1_max[kPlanes], g_e1f_cov_min{~0ull}, g_e1f_cov_max{0}, g_e1f_unc_max{0}, g_e1f_g1_cov_max{0},
    g_e1f_g1_unc_max{0}, g_e1f_neg_min{~0ull};
struct E1fFsc {
    uint64_t cov = 0, unc = 0;      // S1's texels not the sentinel (covered), and the sentinel
    uint64_t r1_not = 0;            // (G0) R1's texels not the sentinel
    uint64_t g1_cov = 0, g1_unc = 0;  // (G1) covered and S1 != C2; uncovered and C2 != T
    uint64_t neg = 0;               // covered and S1 != S0
    bool bad = true;
};
E1fFsc e1f_fsc(const uint8_t* base, const Job& j) {
    E1fFsc f;
    const int s1 = slot(kS, 1, 0), s0 = slot(kS, 0, 0), r1 = slot(kR, 1, 0), r0 = slot(kR, 0, 0), c = slot_c(0);
    const UINT w = j.fp[s1].Footprint.Width, h = j.rows[s1];
    for (const int x : {s0, r1, r0, c})
        if (j.fp[x].Footprint.Width != w || j.rows[x] != h || j.rowbytes[x] != j.rowbytes[s1]) return f;
    if (!w || j.rowbytes[s1] != static_cast<UINT64>(w) * 8) return f;  // (RGBA16F: the sentinel's 8 bytes)
    f.bad = false;
    auto row = [&](int x, UINT y) { return base + j.fp[x].Offset + static_cast<UINT64>(y) * j.fp[x].Footprint.RowPitch; };
    for (UINT y = 0; y < h; ++y) {
        const uint8_t *ps1 = row(s1, y), *ps0 = row(s0, y), *pr1 = row(r1, y), *pr0 = row(r0, y), *pc = row(c, y);
        for (UINT x = 0; x < w; ++x) {
            uint64_t a = 0, z = 0, r = 0, t = 0, cv = 0;
            std::memcpy(&a, ps1 + static_cast<size_t>(x) * 8, 8);
            std::memcpy(&z, ps0 + static_cast<size_t>(x) * 8, 8);
            std::memcpy(&r, pr1 + static_cast<size_t>(x) * 8, 8);
            std::memcpy(&t, pr0 + static_cast<size_t>(x) * 8, 8);
            std::memcpy(&cv, pc + static_cast<size_t>(x) * 8, 8);
            if (r != kE1fSentinel64) ++f.r1_not;
            if (a != kE1fSentinel64) {
                ++f.cov;
                if (a != cv) ++f.g1_cov;
                if (a != z) ++f.neg;
            } else {
                ++f.unc;
                if (cv != t) ++f.g1_unc;
            }
        }
    }
    return f;
}
// (GOAL-single-pass-2, test only) "handover save <dir>" also fires on the first 8f compare frame (no control) whose G1 FSC differs, the
// handover installed or not: S1's, C2's and T's FullScreenCopy (e1f_<frame>_S1_fsc.rde, _C2_fsc.rde, _T_fsc.rde) and a .txt of the
// differing texels (at most 400): x y, covered (S1 not the sentinel: S1 vs C2) or the sentinel (C2 vs T), each channel's halves as floats
// (S1, C2, T) and the compared pair's distance in ULPs (the halves' order, -0 = +0)
int half_ulps(uint16_t a, uint16_t b) {
    const int ia = (a & 0x8000) ? -static_cast<int>(a & 0x7fff) : static_cast<int>(a);
    const int ib = (b & 0x8000) ? -static_cast<int>(b & 0x7fff) : static_cast<int>(b);
    return ia > ib ? ia - ib : ib - ia;
}
void e1f_fsc_save(const uint8_t* p, const Job& j, const E1fFsc& f) {
    bool t = true;
    if (!g_ho_sv_want.compare_exchange_strong(t, false)) return;  // (one-shot: this frame takes it, or another already did)
    std::string dir;
    {
        std::lock_guard lock(g_ho_sv_mutex);
        dir = g_ho_sv_dir;
    }
    const int s1 = slot(kS, 1, 0), c = slot_c(0), r0 = slot(kR, 0, 0);
    char tag[64];
    std::snprintf(tag, sizeof(tag), "e1f_%llu", static_cast<unsigned long long>(j.frame));
    std::string err;
    const char* const names[3] = {"_S1_fsc.rde", "_C2_fsc.rde", "_T_fsc.rde"};
    const int slots[3] = {s1, c, r0};
    for (int i = 0; i < 3 && err.empty(); ++i) err = sv_plane(dir + "\\" + tag + names[i], p, j, slots[i], j.frame);
    uint64_t listed = 0;
    if (err.empty()) {  // (e1f_fsc checked the layout: the same width, rows and row bytes, 8 bytes a texel)
        const std::string mp = dir + "\\" + tag + ".txt";
        FILE* fp = nullptr;
        if (fopen_s(&fp, mp.c_str(), "wb") != 0 || !fp) {
            err = "cannot open " + mp;
        } else {
            std::fprintf(fp, "frame=%llu g1_fsc_covered=%llu g1_fsc_uncovered=%llu coverage=%llu %s (x y kind, then per channel r g b a: S1 C2 T "
                         "as floats and the compared pair's ULPs)\n", static_cast<unsigned long long>(j.frame), static_cast<unsigned long long>(f.g1_cov),
                         static_cast<unsigned long long>(f.g1_unc), static_cast<unsigned long long>(f.cov), j.dlss ? "DLSS" : "FXAA");
            const UINT w = j.fp[s1].Footprint.Width, h = j.rows[s1];
            auto row = [&](int x, UINT y) { return p + j.fp[x].Offset + static_cast<UINT64>(y) * j.fp[x].Footprint.RowPitch; };
            for (UINT y = 0; y < h && listed < 400; ++y) {
                const uint8_t *ps = row(s1, y), *pc = row(c, y), *pt = row(r0, y);
                for (UINT x = 0; x < w && listed < 400; ++x) {
                    uint64_t a = 0, cv = 0, tv = 0;
                    std::memcpy(&a, ps + static_cast<size_t>(x) * 8, 8);
                    std::memcpy(&cv, pc + static_cast<size_t>(x) * 8, 8);
                    std::memcpy(&tv, pt + static_cast<size_t>(x) * 8, 8);
                    const bool covered = a != kE1fSentinel64;
                    if (covered ? a == cv : cv == tv) continue;
                    std::fprintf(fp, "%u %u %s", x, y, covered ? "covered" : "sentinel");
                    for (int ch = 0; ch < 4; ++ch) {
                        const uint16_t hs = static_cast<uint16_t>(a >> (16 * ch)), hc = static_cast<uint16_t>(cv >> (16 * ch)),
                                       ht = static_cast<uint16_t>(tv >> (16 * ch));
                        std::fprintf(fp, " %c: %.9g %.9g %.9g %d", "rgba"[ch], half_to_float(hs), half_to_float(hc), half_to_float(ht),
                                     half_ulps(covered ? hs : ht, hc));
                    }
                    std::fputc('\n', fp);
                    ++listed;
                }
            }
            if (fclose(fp) != 0) err = "cannot write " + mp;
        }
    }
    char msg[400];
    std::snprintf(msg, sizeof(msg), "%s %s\\%s (8f G1 fsc differ %llu: covered %llu, sentinel %llu; listed %llu)%s%s", err.empty() ? "saved" : "FAILED",
                  dir.c_str(), tag, static_cast<unsigned long long>(f.g1_cov + f.g1_unc), static_cast<unsigned long long>(f.g1_cov),
                  static_cast<unsigned long long>(f.g1_unc), static_cast<unsigned long long>(listed), err.empty() ? "" : ": ", err.c_str());
    {
        std::lock_guard lock(g_ho_sv_mutex);
        g_ho_sv_last = msg;
    }
    log::info("[mi] e1f save: %s", msg);
}
void compare_e1f(const uint8_t* p, const Job& j) {
    Diff g0[kPlanes], g1[kPlanes];
    const bool dlss = j.dlss;
    for (int pl = 1; pl < kPlanes; ++pl) {  // (FSC by its own rule below)
        if (!dlss && (pl == 1 || pl == 2)) continue;  // (FXAA: velocity and reactivity out of G0 and G1, item8.md 2.5 row 6)
        const int mode = pl == 3 ? 1 : 0;
        g0[pl] = diff(p, j, slot(kR, 1, pl), slot(kR, 0, pl), mode);
        g1[pl] = diff(p, j, slot(kS, 1, pl), slot_c(pl), mode);
    }
    const E1fFsc f = e1f_fsc(p, j);
    g0[0].texels = f.bad ? ~0ull : f.r1_not;
    g1[0].texels = f.bad ? ~0ull : f.g1_cov + f.g1_unc;
    bool layout = f.bad, exact0 = true, exact1 = true;
    uint64_t t1 = 0;
    for (int pl = 0; pl < kPlanes; ++pl) {
        layout = layout || g0[pl].texels == ~0ull || g1[pl].texels == ~0ull;
        exact0 = exact0 && g0[pl].texels == 0;
        exact1 = exact1 && g1[pl].texels == 0;
        if (g1[pl].texels != ~0ull) t1 += g1[pl].texels;
    }
    bump(g_c_frames);
    if (j.alt >= 1 && j.alt <= 3) bump(g_e1f_alt[j.alt]);  // (GOAL-single-pass-2 item 4: before the layout's return, as e1f_layout)
    if (layout) {
        bump(g_c_layout);
        bump(g_e1f_layout);
        return;
    }
    const int ck = j.ctl && j.ck > 0 && j.ck < kCk ? j.ck : 0;
    if (j.ctl) {  // (a control frame: G1 must differ; never in the pass)
        bump(g_e1f_cmp_ctl);
        bump(g_e1f_ck_frames[ck]);
        if (!exact1) bump(g_e1f_ck_g1[ck]);
        if (ck == kCkSingle && j.single >= 0 && j.single < kOrds && (j.drew >> j.single & 1)) {  // (the frames its pair drew in pass 1)
            bump(g_e1f_ck_drew[ck]);
            if (!exact1) bump(g_e1f_ck_drew_g1[ck]);
        }
        log::limited("mi.e1fctl", 30, "[mi] e1f compared a control frame (%s%s): G1 (S1 vs C2) differs in %llu texels (fsc %llu+%llu, vel %llu, react %llu, "
                     "depth %llu, stencil %llu), G0 %s, coverage %llu", kCkName[ck], dlss ? ", DLSS" : ", FXAA", static_cast<unsigned long long>(t1),
                     static_cast<unsigned long long>(f.g1_cov), static_cast<unsigned long long>(f.g1_unc), static_cast<unsigned long long>(g1[1].texels),
                     static_cast<unsigned long long>(g1[2].texels), static_cast<unsigned long long>(g1[3].texels),
                     static_cast<unsigned long long>(g1[4].texels), exact0 ? "exact" : "differs", static_cast<unsigned long long>(f.cov));
        return;
    }
    bump(g_e1f_cmp);
    bump(dlss ? g_e1f_dlss_frames : g_e1f_fxaa_frames);
    if (g1[0].texels && g_ho_sv_want.load()) e1f_fsc_save(p, j, f);  // (the one-shot save: the first such frame after "handover save <dir>")
    if (exact0) bump(g_e1f_g0_exact);
    if (exact1) bump(g_e1f_g1_exact);
    const bool neg = j.twin_mask == 3;  // ("twins mask 2" / "dupall mask": S0 holds no twin, the negative not measured)
    if (!neg) bump(g_e1f_neg_unmeasured);
    if (neg && f.neg) bump(g_e1f_neg_differ);
    if (exact0 && exact1 && f.cov) bump(g_e1f_pass);  // (the review: the negative is information only; pass 2's own draws alone make S1 differ from S0)
    for (int pl = 0; pl < kPlanes; ++pl) {
        if (g0[pl].texels) bump(g_e1f_g0_nf[pl]);
        if (g1[pl].texels) bump(g_e1f_g1_nf[pl]);
        g_e1f_g0_tx[pl].fetch_add(g0[pl].texels, std::memory_order_relaxed);
        g_e1f_g1_tx[pl].fetch_add(g1[pl].texels, std::memory_order_relaxed);
        store_max(g_e1f_g0_max[pl], g0[pl].texels);
        store_max(g_e1f_g1_max[pl], g1[pl].texels);
    }
    store_min(g_e1f_cov_min, f.cov);
    store_max(g_e1f_cov_max, f.cov);
    g_e1f_unc_sum.fetch_add(f.unc, std::memory_order_relaxed);
    store_max(g_e1f_unc_max, f.unc);
    store_max(g_e1f_g1_cov_max, f.g1_cov);
    store_max(g_e1f_g1_unc_max, f.g1_unc);
    if (neg) store_min(g_e1f_neg_min, f.neg);
    log::limited("mi.e1fcmp", 40, "[mi] e1f compared (%s): G0 (R1 vs T) fsc %llu vel %llu react %llu depth %llu stencil %llu; G1 (S1 vs C2) fsc %llu "
                 "(covered %llu, uncovered %llu) vel %llu react %llu depth %llu stencil %llu; negative (S1 vs S0, covered fsc) %llu%s; coverage %llu, "
                 "uncovered %llu; listed %d / %d", dlss ? "DLSS" : "FXAA: velocity and reactivity not compared", static_cast<unsigned long long>(g0[0].texels),
                 static_cast<unsigned long long>(g0[1].texels), static_cast<unsigned long long>(g0[2].texels), static_cast<unsigned long long>(g0[3].texels),
                 static_cast<unsigned long long>(g0[4].texels), static_cast<unsigned long long>(g1[0].texels), static_cast<unsigned long long>(f.g1_cov),
                 static_cast<unsigned long long>(f.g1_unc), static_cast<unsigned long long>(g1[1].texels), static_cast<unsigned long long>(g1[2].texels),
                 static_cast<unsigned long long>(g1[3].texels), static_cast<unsigned long long>(g1[4].texels), static_cast<unsigned long long>(f.neg),
                 neg ? "" : " (NOT MEASURED: the twins' mask is not 3)", static_cast<unsigned long long>(f.cov), static_cast<unsigned long long>(f.unc),
                 j.listed, j.listed2);
}
// ---- ([Stereo] MainRegionHandover, item 8g) the worker's G1': C (the game's four at pass 2's e1 end, after the handover and the game's
// own draws on top) against R1 (T plus pass 2's own e1 by the game's pipeline: two-pass), per plane (depth 24 bits, stencil, FSC whole;
// velocity and reactivity under DLSS only); coverage: S1's FSC texels not the sentinel (what the merge wrote). nohandover and identity
// frames and control frames counted apart
Count g_hc_frames, g_hc_exact, g_hc_layout, g_hc_dlss, g_hc_fxaa, g_hc_nf[kPlanes], g_hc_tx[kPlanes], g_hc_ctl_frames[kCk], g_hc_ctl_differ[kCk],
    g_hc_nohand_frames, g_hc_nohand_differ, g_hc_ident_frames, g_hc_ident_exact;
std::atomic<uint64_t> g_hc_max[kPlanes], g_hc_cov_min{~0ull}, g_hc_cov_max{0};
// (H2m's diagnosis) FSC's differing texels (C against R1) by S1 covered / the sentinel / under the own mask (R0's FSC non-zero, when
// latched), and on the first such frame after "handover save <dir>": C's, R1's, S1's FSC and the mask (RDE1) and a .txt of the texels
void ho_fsc_split(const uint8_t* p, const Job& j) {
    const int c = slot_c(0), r1 = slot(kR, 1, 0), s1 = slot(kS, 1, 0), m = slot(kR, 0, 0);
    const UINT w = j.fp[c].Footprint.Width, h = j.rows[c];
    for (const int x : {r1, s1, m})
        if (j.fp[x].Footprint.Width != w || j.rows[x] != h || j.rowbytes[x] != j.rowbytes[c]) return;
    if (!w || j.rowbytes[c] != static_cast<UINT64>(w) * 8) return;
    bool save = false;
    std::string dir;
    if (g_ho_sv_want.load() && j.ho_mask) {
        bool t = true;
        save = g_ho_sv_want.compare_exchange_strong(t, false);
        std::lock_guard lock(g_ho_sv_mutex);
        dir = g_ho_sv_dir;
    }
    std::string txt;
    uint64_t cov = 0, unc = 0, own = 0, listed = 0;
    auto row = [&](int x, UINT y) { return p + j.fp[x].Offset + static_cast<UINT64>(y) * j.fp[x].Footprint.RowPitch; };
    for (UINT y = 0; y < h; ++y) {
        const uint8_t *pc = row(c, y), *pr = row(r1, y), *ps = row(s1, y), *pm = row(m, y);
        for (UINT x = 0; x < w; ++x) {
            const size_t o = static_cast<size_t>(x) * 8;
            if (!std::memcmp(pc + o, pr + o, 8)) continue;
            uint64_t sv = 0, mv = 0, cv = 0, rv = 0;
            std::memcpy(&sv, ps + o, 8);
            std::memcpy(&mv, pm + o, 8);
            std::memcpy(&cv, pc + o, 8);
            std::memcpy(&rv, pr + o, 8);
            const bool covered = sv != kE1fSentinel64, under = j.ho_mask && mv != 0;
            ++(covered ? cov : unc);
            if (under) ++own;
            if (save && listed < 400) {
                char b[160];
                std::snprintf(b, sizeof(b), "%u %u C=%016llx R1=%016llx S1=%016llx %s own=%d\n", x, y, static_cast<unsigned long long>(cv),
                              static_cast<unsigned long long>(rv), static_cast<unsigned long long>(sv), covered ? "covered" : "sentinel", under ? 1 : 0);
                txt += b;
                ++listed;
            }
        }
    }
    g_hc_fsc_cov_diff.fetch_add(cov, std::memory_order_relaxed);
    g_hc_fsc_unc_diff.fetch_add(unc, std::memory_order_relaxed);
    g_hc_fsc_own_diff.fetch_add(own, std::memory_order_relaxed);
    if (!save) return;
    char tag[64];
    std::snprintf(tag, sizeof(tag), "handover_%llu", static_cast<unsigned long long>(j.frame));
    std::string err;
    const char* const names[4] = {"_C_fsc.rde", "_R1_fsc.rde", "_S1_fsc.rde", "_own_mask.rde"};
    const int slots[4] = {c, r1, s1, m};
    for (int i = 0; i < 4 && err.empty(); ++i) err = sv_plane(dir + "\\" + tag + names[i], p, j, slots[i], j.frame);
    if (err.empty()) {
        const std::string mp = dir + "\\" + tag + ".txt";
        FILE* f = nullptr;
        if (fopen_s(&f, mp.c_str(), "wb") != 0 || !f) {
            err = "cannot open " + mp;
        } else {
            std::fprintf(f, "frame=%llu fsc_differ=%llu covered=%llu sentinel=%llu under_own_mask=%llu listed=%llu (x y C R1 S1 as RGBA16F's 8 bytes)\n",
                         static_cast<unsigned long long>(j.frame), static_cast<unsigned long long>(cov + unc), static_cast<unsigned long long>(cov),
                         static_cast<unsigned long long>(unc), static_cast<unsigned long long>(own), static_cast<unsigned long long>(listed));
            std::fputs(txt.c_str(), f);
            if (fclose(f) != 0) err = "cannot write " + mp;
        }
    }
    char msg[400];
    std::snprintf(msg, sizeof(msg), "%s %s\\%s (fsc differ %llu: covered %llu, sentinel %llu, under the own mask %llu)%s%s", err.empty() ? "saved" : "FAILED",
                  dir.c_str(), tag, static_cast<unsigned long long>(cov + unc), static_cast<unsigned long long>(cov), static_cast<unsigned long long>(unc),
                  static_cast<unsigned long long>(own), err.empty() ? "" : ": ", err.c_str());
    {
        std::lock_guard lock(g_ho_sv_mutex);
        g_ho_sv_last = msg;
    }
    log::info("[mi] handover save: %s", msg);
}
void compare_handover(const uint8_t* p, const Job& j) {
    Diff g[kPlanes];
    for (int pl = 0; pl < kPlanes; ++pl) {
        if (!j.dlss && (pl == 1 || pl == 2)) continue;  // (FXAA: velocity and reactivity unread, not handed over [V item8a.md])
        g[pl] = diff(p, j, slot_c(pl), slot(kR, 1, pl), pl == 3 ? 1 : 0);
    }
    uint64_t cov = ~0ull;
    {
        const int s1 = slot(kS, 1, 0);
        const UINT w = j.fp[s1].Footprint.Width, h = j.rows[s1];
        if (w && j.rowbytes[s1] == static_cast<UINT64>(w) * 8) {
            cov = 0;
            for (UINT y = 0; y < h; ++y) {
                const uint8_t* row = p + j.fp[s1].Offset + static_cast<UINT64>(y) * j.fp[s1].Footprint.RowPitch;
                for (UINT x = 0; x < w; ++x) {
                    uint64_t t = 0;
                    std::memcpy(&t, row + static_cast<size_t>(x) * 8, 8);
                    if (t != kE1fSentinel64) ++cov;
                }
            }
        }
    }
    bool layout = cov == ~0ull, exact = true;
    uint64_t t = 0;
    for (int pl = 0; pl < kPlanes; ++pl) {
        layout = layout || g[pl].texels == ~0ull;
        exact = exact && g[pl].texels == 0;
        if (g[pl].texels != ~0ull) t += g[pl].texels;
    }
    bump(g_c_frames);
    if (layout) {
        bump(g_c_layout);
        bump(g_hc_layout);
        return;
    }
    const char* kind = j.ho_mode == 4 ? "copyonly" : j.ho_mode == 3 ? "identity" : j.ho_mode == 2 ? "nohandover" : j.ctl ? kCkName[j.ck > 0 && j.ck < kCk ? j.ck : 0] : "handover";
    if (j.ho_mode == 4) {  // (copyonly: handed over, nothing dropped: the game's own e1 drawn over S1; G1' counted apart)
        bump(g_hc_copy_frames);
        if (exact) bump(g_hc_copy_exact);
    } else if (j.ho_mode == 3) {  // (decided, nothing dropped or copied: C must equal R1)
        bump(g_hc_ident_frames);
        if (exact) bump(g_hc_ident_exact);
    } else if (j.ho_mode == 2) {  // (dropped, nothing copied: G1' must fail)
        bump(g_hc_nohand_frames);
        if (!exact) bump(g_hc_nohand_differ);
    } else if (j.ctl) {  // (a control frame: G1' must fail)
        const int ck = j.ck > 0 && j.ck < kCk ? j.ck : 0;
        bump(g_hc_ctl_frames[ck]);
        if (!exact) bump(g_hc_ctl_differ[ck]);
    } else {
        bump(g_hc_frames);
        bump(j.dlss ? g_hc_dlss : g_hc_fxaa);
        if (exact) bump(g_hc_exact);
        if (g[0].texels) ho_fsc_split(p, j);  // (H2m's diagnosis: the split, and the one-shot save)
        for (int pl = 0; pl < kPlanes; ++pl) {
            if (g[pl].texels) bump(g_hc_nf[pl]);
            g_hc_tx[pl].fetch_add(g[pl].texels, std::memory_order_relaxed);
            store_max(g_hc_max[pl], g[pl].texels);
        }
        store_min(g_hc_cov_min, cov);
        store_max(g_hc_cov_max, cov);
    }
    log::limited("mi.hocmp", 40, "[mi] handover compared (%s, %s): G1' (C vs R1) fsc %llu vel %llu react %llu depth %llu stencil %llu texels differ "
                 "(%llu in all); coverage %llu; listed %d", kind, j.dlss ? "DLSS" : "FXAA: velocity and reactivity not compared",
                 static_cast<unsigned long long>(g[0].texels), static_cast<unsigned long long>(g[1].texels), static_cast<unsigned long long>(g[2].texels),
                 static_cast<unsigned long long>(g[3].texels), static_cast<unsigned long long>(g[4].texels), static_cast<unsigned long long>(t),
                 static_cast<unsigned long long>(cov), j.listed);
}
// ---- (GOAL-single-pass-2 item 4, "handover ref h1") the worker's H1: H (R slice 0, the game's four right after the handover) against
// the expected, bit-exact per plane (depth 24 bits): FSC S1 where S1 is not the 8f0b sentinel, T (R slice 1) where it is; another plane
// the decision copies (j.h1_cp, whatever the mode) S1, one it does not T (velocity and reactivity under FXAA, a plane trim left); identity:
// T for every plane (nothing copied). nohandover (nothing copied: must fail), identity and control frames counted apart (a control
// changes S1, which the expected follows: H1 holds on it; the G1 frames between see it)
Count g_h1_frames, g_h1_exact, g_h1_layout, g_h1_dlss, g_h1_fxaa, g_h1_tx[kPlanes], g_h1_nf[kPlanes], g_h1_nohand_frames, g_h1_nohand_differ,
    g_h1_ident_frames, g_h1_ident_exact, g_h1_ctl_frames[kCk], g_h1_ctl_differ[kCk], g_h1_copy_frames, g_h1_copy_exact, g_h1_trim_frames,
    g_h1_trim_s1t_frames, g_h1_trim_s1t_tx;  // (trim: H1 frames with a plane left; of them S1 != T there, and its texels)
uint64_t h1_fsc(const uint8_t* p, const Job& j) {  // (FSC's texels H != the expected; ~0: the layout)
    const int h = slot(kR, 0, 0), t = slot(kR, 1, 0), s1 = slot(kS, 1, 0);
    const UINT w = j.fp[h].Footprint.Width, rows = j.rows[h];
    for (const int x : {t, s1})
        if (j.fp[x].Footprint.Width != w || j.rows[x] != rows || j.rowbytes[x] != j.rowbytes[h]) return ~0ull;
    if (!w || j.rowbytes[h] != static_cast<UINT64>(w) * 8) return ~0ull;  // (RGBA16F: the sentinel's 8 bytes)
    uint64_t n = 0;
    auto row = [&](int x, UINT y) { return p + j.fp[x].Offset + static_cast<UINT64>(y) * j.fp[x].Footprint.RowPitch; };
    for (UINT y = 0; y < rows; ++y) {
        const uint8_t *ph = row(h, y), *pt = row(t, y), *ps = row(s1, y);
        for (UINT x = 0; x < w; ++x) {
            uint64_t hv = 0, tv = 0, sv = 0;
            std::memcpy(&hv, ph + static_cast<size_t>(x) * 8, 8);
            std::memcpy(&tv, pt + static_cast<size_t>(x) * 8, 8);
            std::memcpy(&sv, ps + static_cast<size_t>(x) * 8, 8);
            if (hv != (sv != kE1fSentinel64 ? sv : tv)) ++n;
        }
    }
    return n;
}
void compare_h1(const uint8_t* p, const Job& j) {
    const bool ident = j.ho_mode == 3;
    Diff g[kPlanes];
    for (int pl = 1; pl < kPlanes; ++pl) {
        const bool s = !ident && (j.h1_cp >> pl & 1);
        g[pl] = diff(p, j, slot(kR, 0, pl), s ? slot(kS, 1, pl) : slot(kR, 1, pl), pl == 3 ? 1 : 0);
    }
    if (ident || !(j.h1_cp & 1)) g[0] = diff(p, j, slot(kR, 0, 0), slot(kR, 1, 0), 0);
    else g[0].texels = h1_fsc(p, j);
    bool layout = false, exact = true;
    uint64_t t = 0;
    for (int pl = 0; pl < kPlanes; ++pl) {
        layout = layout || g[pl].texels == ~0ull;
        exact = exact && g[pl].texels == 0;
        if (g[pl].texels != ~0ull) t += g[pl].texels;
    }
    bump(g_c_frames);
    if (layout) {
        bump(g_c_layout);
        bump(g_h1_layout);
        return;
    }
    const int ck = j.ctl && j.ck > 0 && j.ck < kCk ? j.ck : 0;
    const char* kind = j.ho_mode == 4 ? "copyonly" : ident ? "identity" : j.ho_mode == 2 ? "nohandover" : j.ctl ? kCkName[ck] : "handover";
    if (ident) {  // (decided, nothing dropped or copied: H must equal T)
        bump(g_h1_ident_frames);
        if (exact) bump(g_h1_ident_exact);
    } else if (j.ho_mode == 2) {  // (decided, nothing copied: H is T, the expected S1's: must fail)
        bump(g_h1_nohand_frames);
        if (!exact) bump(g_h1_nohand_differ);
    } else if (j.ctl) {
        bump(g_h1_ctl_frames[ck]);
        if (!exact) bump(g_h1_ctl_differ[ck]);
    } else if (j.ho_mode == 4) {  // (copyonly: handed over, nothing dropped; H taken as on's, counted apart)
        bump(g_h1_copy_frames);
        if (exact) bump(g_h1_copy_exact);
    } else {  // (on: H is taken right after the copies, before the game's own e1 on top)
        bump(g_h1_frames);
        bump(j.dlss ? g_h1_dlss : g_h1_fxaa);
        if (exact) bump(g_h1_exact);
        for (int pl = 0; pl < kPlanes; ++pl) {
            if (g[pl].texels) bump(g_h1_nf[pl]);
            g_h1_tx[pl].fetch_add(g[pl].texels, std::memory_order_relaxed);
        }
    }
    // (trim's check: a plane trim left, S1 against T; nonzero: an e1 draw wrote what trim found unwritten, or the seed differs from the
    // game's own there. Not on control frames: the seed control seeds S1 otherwise)
    uint64_t st = 0;
    if (const uint32_t tr = j.ctl ? 0u : ~j.h1_cp & 24u) {
        bump(g_h1_trim_frames);
        for (const int pl : {3, 4})
            if (tr >> pl & 1) {
                const Diff d = diff(p, j, slot(kS, 1, pl), slot(kR, 1, pl), pl == 3 ? 1 : 0);
                if (d.texels == ~0ull) continue;  // (the layout: H1's diff of the same slots said so already)
                st += d.texels;
            }
        if (st) bump(g_h1_trim_s1t_frames);
        g_h1_trim_s1t_tx.fetch_add(st, std::memory_order_relaxed);
    }
    log::limited("mi.h1cmp", 40, "[mi] handover compared (H1, %s, %s): H vs the expected fsc %llu vel %llu react %llu depth %llu stencil %llu texels "
                 "differ (%llu in all); copied fsc %d vel %d react %d depth %d stencil %d (trim: S1 vs T on the planes left %llu); listed %d", kind, j.dlss ? "DLSS" : "FXAA",
                 static_cast<unsigned long long>(g[0].texels), static_cast<unsigned long long>(g[1].texels), static_cast<unsigned long long>(g[2].texels),
                 static_cast<unsigned long long>(g[3].texels), static_cast<unsigned long long>(g[4].texels), static_cast<unsigned long long>(t),
                 static_cast<int>(j.h1_cp & 1), static_cast<int>(j.h1_cp >> 1 & 1), static_cast<int>(j.h1_cp >> 2 & 1), static_cast<int>(j.h1_cp >> 3 & 1),
                 static_cast<int>(j.h1_cp >> 4 & 1), static_cast<unsigned long long>(st), j.listed);
}
void compare_job() {
    const Job& j = g_job;
    uint8_t* p = nullptr;
    D3D12_RANGE r{0, static_cast<SIZE_T>(j.size)};
    if (!j.rb || FAILED(j.rb->Map(0, &r, reinterpret_cast<void**>(&p))) || !p) {
        bump(g_c_map_failed);
        return;
    }
    if (j.ho) {  // ([Stereo] MainRegionHandover, item 8g: G1'; GOAL-single-pass-2 item 4: H1 on ref h1's H1 frames)
        if (j.h1) compare_h1(p, j);
        else compare_handover(p, j);
        D3D12_RANGE none{0, 0};
        j.rb->Unmap(0, &none);
        return;
    }
    if (j.e1f) {  // ([Stereo] MainRegionE1Compare, item 8f: G0 and G1)
        compare_e1f(p, j);
        D3D12_RANGE none{0, 0};
        j.rb->Unmap(0, &none);
        return;
    }
    if (j.rmode) {  // ([Stereo] MainFamily=region, item 8e)
        compare_region(p, j);
        D3D12_RANGE none{0, 0};
        j.rb->Unmap(0, &none);
        return;
    }
    if (j.slice) {  // ([Stereo] MainSlice)
        compare_slice(p, j);
        D3D12_RANGE none{0, 0};
        j.rb->Unmap(0, &none);
        return;
    }
    Diff e2[kPlanes], e1[kPlanes];
    for (int pl = 0; pl < kPlanes; ++pl) {
        const int mode = pl == 3 ? 1 : 0;
        e2[pl] = diff(p, j, slot(kS, 1, pl), slot(kR, 1, pl), mode);
        e1[pl] = diff(p, j, slot(kS, 0, pl), slot(kR, 0, pl), mode);
    }
    const Diff neg = diff(p, j, slot(kS, 1, 0), slot(kR, 0, 0), 0);
    const uint64_t cov = nonzero(p, j, slot(kR, 1, 0), slot(kR, 1, 1));
    depth_spread(p, j, slot(kR, 0, 3), 0);  // (the family compare's diagnostic: R's seeded depth, each slice)
    depth_spread(p, j, slot(kR, 1, 3), 1);
    vel_mean(p, j, slot(kS, 1, 1), slot(kR, 1, 1), slot(kR, 1, 0), g_c_vm);   // (the family compare's diagnostic: eye 2's velocity, S1 and R1)
    vel_mean(p, j, slot(kS, 0, 1), slot(kR, 0, 1), slot(kR, 0, 0), g_c_vm0);  // (eye 1: S0 and R0)
    uint64_t react = 0;
    for (int set = 0; set < kSets; ++set)
        for (int sl = 0; sl < 2; ++sl) {
            const uint64_t n = nonzero(p, j, slot(set, sl, 2), -1);
            react = n == ~0ull || react == ~0ull ? ~0ull : react + n;
        }
    D3D12_RANGE none{0, 0};
    j.rb->Unmap(0, &none);
    bool layout = cov == ~0ull || react == ~0ull || neg.texels == ~0ull;
    bool exact2 = true, exact1 = true;
    for (int pl = 0; pl < kPlanes; ++pl) {
        layout = layout || e2[pl].texels == ~0ull || e1[pl].texels == ~0ull;
        exact2 = exact2 && e2[pl].texels == 0;
        exact1 = exact1 && e1[pl].texels == 0;
    }
    bump(g_c_frames);
    if (layout) {
        bump(g_c_layout);
        return;
    }
    if (j.ctl) {  // (the positive control's frames: eye 2 must differ; counted apart, never in the pass)
        bump(g_c_ctl);
        if (!exact2) bump(g_c_ctl_e2_differ);
        if (exact1) bump(g_c_ctl_e1_exact);
        uint64_t t = 0;
        for (const Diff& d : e2) t += d.texels;
        store_min(g_c_ctl_e2_min, t);
        store_max(g_c_cov_max, cov);
        store_min(g_c_cov_min, cov);
        log::limited("mi.ctl", 20, "[mi] compared a control frame: eye 2 differs in %llu texels (colour %llu, velocity %llu, depth %llu, stencil "
                     "%llu), eye 1 %s, coverage %llu", static_cast<unsigned long long>(t), static_cast<unsigned long long>(e2[0].texels),
                     static_cast<unsigned long long>(e2[1].texels), static_cast<unsigned long long>(e2[3].texels),
                     static_cast<unsigned long long>(e2[4].texels), exact1 ? "bit-exact" : "differs", static_cast<unsigned long long>(cov));
        return;
    }
    if (exact2) bump(g_c_e2_exact);
    if (exact1) bump(g_c_e1_exact);
    if (neg.texels) bump(g_c_neg_differ);
    if (react == 0) bump(g_c_react_zero);
    if (cov >= kCovMin) bump(g_c_cov_ok);
    if (exact2 && exact1 && react == 0 && cov >= kCovMin && neg.texels) bump(g_c_pass);
    for (int pl = 0; pl < kPlanes; ++pl) {
        store_max(g_c_e2_max[pl], e2[pl].texels);
        store_max(g_c_e1_max[pl], e1[pl].texels);
        store_max(g_c_e2_worst, e2[pl].worst);
        if (e2[pl].texels) {
            g_c_where[0] = static_cast<uint32_t>(pl);
            g_c_where[1] = e2[pl].x0;
            g_c_where[2] = e2[pl].y0;
            g_c_where[3] = e2[pl].x1;
            g_c_where[4] = e2[pl].y1;
            g_c_where[5] = static_cast<uint32_t>(e2[pl].texels);
        }
    }
    store_min(g_c_cov_min, cov);
    store_max(g_c_cov_max, cov);
    store_min(g_c_neg_min, neg.texels);
    store_max(g_c_react_max, react);
    log::limited("mi.cmp", 40, "[mi] compared: eye 2 (S1 vs R1) colour %llu velocity %llu reactivity %llu depth %llu stencil %llu texels differ (worst %llu); "
                 "eye 1 (S0 vs R0) %llu %llu %llu %llu %llu; negative control (S1 vs R0, colour) %llu; reactivity written %llu; coverage %llu; listed %d",
                 static_cast<unsigned long long>(e2[0].texels), static_cast<unsigned long long>(e2[1].texels),
                 static_cast<unsigned long long>(e2[2].texels), static_cast<unsigned long long>(e2[3].texels),
                 static_cast<unsigned long long>(e2[4].texels), static_cast<unsigned long long>(e2[0].worst > e2[1].worst ? e2[0].worst : e2[1].worst),
                 static_cast<unsigned long long>(e1[0].texels), static_cast<unsigned long long>(e1[1].texels),
                 static_cast<unsigned long long>(e1[2].texels), static_cast<unsigned long long>(e1[3].texels),
                 static_cast<unsigned long long>(e1[4].texels), static_cast<unsigned long long>(neg.texels), static_cast<unsigned long long>(react),
                 static_cast<unsigned long long>(cov), j.listed);
}
DWORD WINAPI worker(void*) {
    int slow = 0;
    for (;;) {
        const bool signalled = WaitForSingleObject(g_fence_event, 5000) == WAIT_OBJECT_0;
        if (g_cap.load() != kWait) continue;
        if (!g_fence || g_fence->GetCompletedValue() < g_fence_value.load()) {
            int s = kWait;
            if (!signalled && ++slow >= 6 && g_cap.compare_exchange_strong(s, kArmed)) {
                bump(g_c_timeout);
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
    if (g_cap.load() == kReady && f >= g_ready_frame.load() + 2) {
        ID3D12CommandQueue* q = state::present_queue.load();
        if (!q || !g_fence || !g_fence_event) return;
        const UINT64 v = g_fence_value.load() + 1;
        g_fence_value.store(v);
        g_cap.store(kWait);  // (before the event can fire: the worker checks the state first)
        q->Signal(g_fence, v);
        g_fence->SetEventOnCompletion(v, g_fence_event);
    }
}

double gvf(int pass, int at, int i) {  // (item 6g's diagnostic: the last gViewport sampled, for kv)
    const uint32_t b = g_gv_last[pass][at][i].load(std::memory_order_relaxed);
    float f;
    std::memcpy(&f, &b, 4);
    return static_cast<double>(f);
}
std::string kv() {
    auto mx = [](const std::atomic<uint64_t>& a) {
        const uint64_t v = a.load();
        return static_cast<unsigned long long>(v == ~0ull ? 0 : v);
    };
    char b[8192];  // (the review: 4000 cut the slice block mid-key; each block is checked, and the reply ends with kv_end=1)
    bool cut = false;
    const int n0 = std::snprintf(b, sizeof(b),
                  "on=%d stood=%d cap=%d left=%d control=%d r_opens1=%llu r_opens2=%llu r_open_failed=%llu r_closes=%llu r_close_failed=%llu "
                  "r_gate_bad1=%llu r_gate_bad2=%llu r_unready=%llu r_ctl=%llu r_ctl_failed=%llu r_ctl_restore_failed=%llu diag_equal=%llu "
                  "diag_differ=%llu diag_unread=%llu p_opens1=%llu p_opens2=%llu p_unclosed=%llu p_order=%llu close_copy=%llu close_marker=%llu "
                  "listed1=%llu listed2=%llu predicated=%llu refused=%llu seeds=%llu seed_e2_array=%llu seed_followed=%llu twin_draws=%llu "
                  "ref1_draws=%llu ref2_draws=%llu no_twin=%llu no_cl1=%llu made=%llu make_failed=%llu other_thread=%llu frames_good=%llu "
                  "frames_bad=%llu x_gate=%llu x_pred=%llu x_refused=%llu x_counts=%llu x_contig=%llu x_seed=%llu x_order=%llu x_none=%llu "
                  "first_same=%llu last_listed1=%llu last_listed2=%llu last_first1=%llu last_first2=%llu caps_started=%llu caps_done=%llu "
                  "caps_incomplete=%llu rb_made=%llu rb_failed=%llu pending_late=%llu frames=%llu pass=%llu e2_exact=%llu e1_exact=%llu "
                  "neg_differ=%llu react_zero=%llu cov_ok=%llu ctl_frames=%llu ctl_e2_differ=%llu ctl_e1_exact=%llu ctl_e2_min=%llu "
                  "map_failed=%llu timeouts=%llu layout=%llu cov_min=%llu cov_max=%llu neg_min=%llu react_max=%llu e2_worst=%llu rb_deferred=%llu "
                  "mask_known=%llu mask_unknown=%llu",
                  g_on.load() ? 1 : 0, g_stood.load() ? 1 : 0, g_cap.load(), g_cap_left.load(), g_control.load(), u(g_r_opens[1]), u(g_r_opens[2]),
                  u(g_r_open_failed), u(g_r_closes), u(g_r_close_failed), u(g_r_gate_bad[1]), u(g_r_gate_bad[2]), u(g_r_unready), u(g_r_ctl),
                  u(g_r_ctl_failed), u(g_r_ctl_restore_failed), u(g_dg_equal), u(g_dg_differ), u(g_dg_unread), u(g_p_opens[1]), u(g_p_opens[2]),
                  u(g_p_unclosed), u(g_p_order), u(g_p_close_copy), u(g_p_close_marker), u(g_p_listed[1]), u(g_p_listed[2]), u(g_p_predicated),
                  u(g_p_refused), u(g_p_seeds), u(g_p_seed_e2_array), u(g_p_seed_followed), u(g_p_twin), u(g_p_ref[1]), u(g_p_ref[2]),
                  u(g_p_no_twin), u(g_p_no_cl1), u(g_p_made), u(g_p_make_failed), u(g_p_other_thread), u(g_f_good), u(g_f_bad), u(g_f_x_gate),
                  u(g_f_x_pred), u(g_f_x_refused), u(g_f_x_counts), u(g_f_x_contig), u(g_f_x_seed), u(g_f_x_order), u(g_f_x_none),
                  u(g_f_first_same), static_cast<unsigned long long>(g_last_listed[1].load()), static_cast<unsigned long long>(g_last_listed[2].load()),
                  static_cast<unsigned long long>(g_last_first[1].load()), static_cast<unsigned long long>(g_last_first[2].load()),
                  u(g_cap_started), u(g_cap_done), u(g_cap_incomplete), u(g_rb_made), u(g_rb_failed), u(g_pending_late), u(g_c_frames),
                  u(g_c_pass), u(g_c_e2_exact), u(g_c_e1_exact), u(g_c_neg_differ), u(g_c_react_zero), u(g_c_cov_ok), u(g_c_ctl),
                  u(g_c_ctl_e2_differ), u(g_c_ctl_e1_exact), mx(g_c_ctl_e2_min), u(g_c_map_failed), u(g_c_timeout), u(g_c_layout),
                  mx(g_c_cov_min), mx(g_c_cov_max), mx(g_c_neg_min), mx(g_c_react_max), mx(g_c_e2_worst), u(g_rb_deferred), u(g_mask_known),
                  u(g_mask_unknown));
    cut = cut || n0 < 0 || n0 >= static_cast<int>(sizeof(b));
    std::string out = b;
    const int n1 = std::snprintf(b, sizeof(b),
                  " e2_fsc_max=%llu e2_vel_max=%llu e2_react_max=%llu e2_depth_max=%llu e2_stencil_max=%llu e1_fsc_max=%llu e1_vel_max=%llu "
                  "e1_react_max=%llu e1_depth_max=%llu e1_stencil_max=%llu where_plane=%u where_x0=%u where_y0=%u where_x1=%u where_y1=%u where_texels=%u",
                  mx(g_c_e2_max[0]), mx(g_c_e2_max[1]), mx(g_c_e2_max[2]), mx(g_c_e2_max[3]), mx(g_c_e2_max[4]), mx(g_c_e1_max[0]), mx(g_c_e1_max[1]),
                  mx(g_c_e1_max[2]), mx(g_c_e1_max[3]), mx(g_c_e1_max[4]), g_c_where[0].load(), g_c_where[1].load(), g_c_where[2].load(),
                  g_c_where[3].load(), g_c_where[4].load(), g_c_where[5].load());
    cut = cut || n1 < 0 || n1 >= static_cast<int>(sizeof(b));
    out += b;
    const int n2 = std::snprintf(b, sizeof(b),  // (run 25: the rows diagnostic; pp gPrevViewProj, vi gViewInverse, wv gWorldView; first = row * 4 + col + 1)
                  " ppfix=%d rows_frames=%llu rows_unread=%llu rows_ctl=%llu pp_src_none=%llu pp_src_taa=%llu pp_src_copy=%llu pp_equal=%llu "
                  "pp_differ=%llu pp_ulp_max=%llu pp_first=%u pp_differ_taa=%llu pp_differ_copy=%llu pp_unstable=%llu vi_equal=%llu vi_differ=%llu "
                  "vi_ulp_max=%llu vi_first=%u wv_equal=%llu wv_differ=%llu wv_ulp_max=%llu wv_first=%u ppfix_applied=%llu ppfix_failed=%llu ppfix_changed=%llu",
                  g_ppfix.load() ? 1 : 0, u(g_rw_frames), u(g_rw_unread), u(g_rw_ctl), u(g_rw_src[0]), u(g_rw_src[1]), u(g_rw_src[2]),
                  u(g_rw_eq[kQPp]), u(g_rw_ne[kQPp]), static_cast<unsigned long long>(g_rw_ulp[kQPp].load()), g_rw_first[kQPp].load(),
                  u(g_rw_ne_src[1]), u(g_rw_ne_src[2]), u(g_rw_unstable), u(g_rw_eq[kQVi]), u(g_rw_ne[kQVi]),
                  static_cast<unsigned long long>(g_rw_ulp[kQVi].load()), g_rw_first[kQVi].load(), u(g_rw_eq[kQWv]), u(g_rw_ne[kQWv]),
                  static_cast<unsigned long long>(g_rw_ulp[kQWv].load()), g_rw_first[kQWv].load(), u(g_rw_fixed), u(g_rw_fix_failed), u(g_rw_fix_changed));
    cut = cut || n2 < 0 || n2 >= static_cast<int>(sizeof(b));
    out += b;
    int ready = 0, building = 0, refused = 0;
    for (const Merge& m : g_mg) {
        const int s = m.state.load();
        ready += s == 2;
        building += s == 1;
        refused += s == -1;
    }
    const int n3 = std::snprintf(b, sizeof(b),  // ([Stereo] MainSlice, run 25 item 5)
                  " sl_installed=%d sl_mode=%d sl_stood=%d sl_frames=%llu sl_handed=%llu sl_refused=%llu hr_early=%llu hr_stood=%llu hr_whole=%llu "
                  "hr_held=%llu hr_srv=%llu hr_merge=%llu hr_pso=%llu hr_query=%llu sl_query_in_run=%llu x_query=%llu sl_identity=%llu id_frames=%llu "
                  "id_exact=%llu id_e1=%llu sl_dropped=%llu sl_drop_min=%llu sl_drop_max=%llu sl_last_dropped=%llu sl_extra=%llu "
                  "sl_short=%llu sl_merges=%llu sl_nomerge=%llu sl_barrier=%llu sl_cmp=%llu sl_good=%llu sl_seed_far=%llu sl_ref_skipped=%llu "
                  "srv_allocs=%llu srv_writes=%llu srv_held=%llu srv_idx0=%u srv_idx1=%u srv_idx2=%u srv_idx3=%u merge_ready=%d merge_building=%d "
                  "merge_refused=%d pso_made=%llu pso_refused=%llu x_hand=%llu x_barrier=%llu x_extra=%llu sl_cmp_frames=%llu sl_pass=%llu "
                  "g1_exact=%llu g1_fsc_max=%llu g1_vel_max=%llu g1_react_max=%llu g1_depth_max=%llu g1_stencil_max=%llu g1_worst=%llu "
                  "g1_where_plane=%u g1_where_x0=%u g1_where_y0=%u g1_where_x1=%u g1_where_y1=%u g1_where_texels=%u marked_frames=%llu "
                  "marked_min=%llu marked_max=%llu passing_frames=%llu failing_frames=%llu passing_texels=%llu failing_texels=%llu "
                  "sc_rows_frames=%llu sc_rows_case=%llu sc_rows_fail=%llu sc_rows_e1=%llu sc_nomerge_frames=%llu sc_nomerge_case=%llu "
                  "sc_nomerge_fail=%llu sc_nomerge_e1=%llu sc_always_frames=%llu sc_always_case=%llu sc_always_fail=%llu sc_always_e1=%llu",
                  g_sl_installed.load() ? 1 : 0, g_sl_mode.load(), g_sl_stood.load() ? 1 : 0, u(g_sl_frames), u(g_sl_handed), u(g_sl_refused),
                  u(g_sl_hr[kHrEarly]), u(g_sl_hr[kHrStood]), u(g_sl_hr[kHrWhole]), u(g_sl_hr[kHrHeld]), u(g_sl_hr[kHrSrv]), u(g_sl_hr[kHrMerge]),
                  u(g_sl_hr[kHrPso]), u(g_sl_hr[kHrQuery]), u(g_sl_query_in_run), u(g_f_x_query), u(g_sl_identity), u(g_c_id_frames), u(g_c_id_exact),
                  u(g_c_id_e1), u(g_sl_dropped), mx(g_sl_drop_min), mx(g_sl_drop_max), mx(g_sl_last_dropped), u(g_sl_extra), u(g_sl_short), u(g_sl_merges),
                  u(g_sl_nomerge), u(g_sl_barrier), u(g_sl_cmp), u(g_sl_good), u(g_sl_seed_far), u(g_sl_ref_skipped), u(g_sl_srv_allocs),
                  u(g_sl_srv_writes), u(g_sl_held), g_srv_idx[0].load(), g_srv_idx[1].load(), g_srv_idx[2].load(), g_srv_idx[3].load(), ready, building,
                  refused, u(g_sl_pso_made), u(g_sl_pso_refused), u(g_f_x_hand), u(g_f_x_barrier), u(g_f_x_extra), u(g_c_sl_frames), u(g_c_sl_pass),
                  u(g_c_g1_exact), mx(g_c_g1_max[0]), mx(g_c_g1_max[1]), mx(g_c_g1_max[2]), mx(g_c_g1_max[3]), mx(g_c_g1_max[4]), mx(g_c_g1_worst),
                  g_c_g1_where[0].load(), g_c_g1_where[1].load(), g_c_g1_where[2].load(), g_c_g1_where[3].load(), g_c_g1_where[4].load(),
                  g_c_g1_where[5].load(), u(g_c_sl_marked), mx(g_c_marked_min), mx(g_c_marked_max), u(g_c_pass_frames), u(g_c_fail_frames),
                  mx(g_c_pass_tex), mx(g_c_fail_tex), u(g_c_sc_frames[0]), u(g_c_sc_case[0]), u(g_c_sc_fail[0]), u(g_c_sc_e1[0]), u(g_c_sc_frames[1]),
                  u(g_c_sc_case[1]), u(g_c_sc_fail[1]), u(g_c_sc_e1[1]), u(g_c_sc_frames[2]), u(g_c_sc_case[2]), u(g_c_sc_fail[2]), u(g_c_sc_e1[2]));
    cut = cut || n3 < 0 || n3 >= static_cast<int>(sizeof(b));
    out += b;
    const int n4 = std::snprintf(b, sizeof(b),  // (GOAL-single-pass items 6d MainPredication and 6e MainUnionCull; uc_*0 the control, uc_*1 union-culled)
                  " family=%s pd_installed=%d pd_mode=%d pd_stood=%d pd_held1=%llu pd_held2=%llu pd_unknown=%llu pd_slice=%llu pd_nulled=%llu "
                  "pd_restored=%llu pd_dup_twin=%llu pd_dup_ref=%llu pd_skipped=%llu pd_refonly=%llu pd_after_bad=%llu pd_unpred_set=%llu pd_seed_nulled=%llu seed_read=%llu seed_read_state=%u fam_judged=%llu fam_within=%llu fam_rep=%llu refdepth=%d refdepth_draws=%llu refdepth_writes=%llu gv_eq_open=%llu gv_ne_open=%llu gv_eq_close=%llu gv_ne_close=%llu gv_walk1=%llu gv_walk2=%llu gv_unread=%llu gv_unpaired=%llu p1vp=%d p1vp_set=%llu p1vp_back=%llu p1vp_kept=%llu p1vp_none=%llu p1vp_failed=%llu p1vp_equal=%llu gvsz1=%dx%d gvsz2=%dx%d gvsz1c=%dx%d gvsz2c=%dx%d gv1=%.6g,%.6g,%.6g,%.6g gv2=%.6g,%.6g,%.6g,%.6g gv1c=%.6g,%.6g,%.6g,%.6g gv2c=%.6g,%.6g,%.6g,%.6g vm_s1x=%.6g vm_s1y=%.6g vm_r1x=%.6g vm_r1y=%.6g vm_n=%.0f vm_nonfinite=%.0f vm_s0x=%.6g vm_s0y=%.6g vm_r0x=%.6g vm_r0y=%.6g vm0_n=%.0f r0_dz=%llu r0_dfar=%llu r0_dmin=%llu r0_dmax=%llu r1_dz=%llu r1_dfar=%llu r1_dmin=%llu r1_dmax=%llu "
                  "uc_installed=%d uc_on=%d uc_begins=%llu uc_ends=%llu uc_nounion=%llu uc_failed=%llu uc_rewritten=%llu uc_2c0_set=%llu "
                  "uc_2c0_odd=%llu uc_2c0_back=%llu uc_2c0_kept=%llu uc_marks=%llu uc_mark_failed=%llu uc_mark_lost=%llu uc_widen_mdeg=%u "
                  "uc_frames0=%llu uc_frames1=%llu uc_subset0=%llu uc_subset1=%llu uc_subset_nvb0=%llu uc_subset_nvb1=%llu uc_miss0=%llu "
                  "uc_miss1=%llu uc_miss_nvb0=%llu uc_miss_nvb1=%llu uc_listed1_0=%llu uc_listed1_1=%llu uc_listed2_0=%llu uc_listed2_1=%llu "
                  "uc_draws1_0=%llu uc_draws1_1=%llu uc_draws2_0=%llu uc_draws2_1=%llu uc_over=%llu uc_unjudged=%llu uc_last_cat=%llu "
                  "uc_last_n1=%llu uc_last_n2=%llu uc_last_miss=%llu uc_last_sum2=%llu",
                  pso::main_family_key(), g_pd_installed.load() ? 1 : 0, g_pd_mode.load(), g_pd_stood.load() ? 1 : 0, u(g_pd_held[1]), u(g_pd_held[2]),
                  u(g_pd_unknown), u(g_pd_slice), u(g_pd_nulled), u(g_pd_restored), u(g_pd_dup_twin), u(g_pd_dup_ref), u(g_pd_skipped), u(g_pd_refonly),
                  u(g_pd_after_bad), u(g_pd_unpred_set), u(g_pd_seed_nulled), u(g_p_seed_read), g_p_seed_read_state.load(), u(g_fam_judged), u(g_fam_within), u(g_fam_rep), g_refdepth_game.load() ? 1 : 0, u(g_refdepth_draws), u(g_refdepth_writes), u(g_gv_eq[0]), u(g_gv_ne[0]), u(g_gv_eq[1]), u(g_gv_ne[1]), u(g_gv_walk_changed[1]), u(g_gv_walk_changed[2]), u(g_gv_unread), u(g_gv_unpaired), g_p1vp.load() ? 1 : 0, u(g_p1vp_set), u(g_p1vp_back), u(g_p1vp_kept), u(g_p1vp_none), u(g_p1vp_failed), u(g_p1vp_equal), g_gvsz_last[1][0][0].load(), g_gvsz_last[1][0][1].load(), g_gvsz_last[2][0][0].load(), g_gvsz_last[2][0][1].load(), g_gvsz_last[1][1][0].load(), g_gvsz_last[1][1][1].load(), g_gvsz_last[2][1][0].load(), g_gvsz_last[2][1][1].load(), gvf(1, 0, 0), gvf(1, 0, 1), gvf(1, 0, 2), gvf(1, 0, 3), gvf(2, 0, 0), gvf(2, 0, 1), gvf(2, 0, 2), gvf(2, 0, 3), gvf(1, 1, 0), gvf(1, 1, 1), gvf(1, 1, 2), gvf(1, 1, 3), gvf(2, 1, 0), gvf(2, 1, 1), gvf(2, 1, 2), gvf(2, 1, 3), g_c_vm[0][0].load(), g_c_vm[0][1].load(), g_c_vm[1][0].load(), g_c_vm[1][1].load(), g_c_vm[2][0].load(), g_c_vm[2][1].load(), g_c_vm0[0][0].load(), g_c_vm0[0][1].load(), g_c_vm0[1][0].load(), g_c_vm0[1][1].load(), g_c_vm0[2][0].load(), u(g_c_dz[0][0]), u(g_c_dz[0][1]), u(g_c_dz[0][2]), u(g_c_dz[0][3]), u(g_c_dz[1][0]), u(g_c_dz[1][1]), u(g_c_dz[1][2]), u(g_c_dz[1][3]), g_uc_installed.load() ? 1 : 0, g_uc_on.load() ? 1 : 0, u(g_uc_begins), u(g_uc_ends),
                  u(g_uc_nounion), u(g_uc_failed), u(g_uc_rewritten), u(g_uc_2c0_set), u(g_uc_2c0_odd), u(g_uc_2c0_back), u(g_uc_2c0_kept), u(g_uc_marks),
                  u(g_uc_mark_failed), u(g_uc_mark_lost), g_uc_widen_mdeg.load(), u(g_uc_frames[0]), u(g_uc_frames[1]), u(g_uc_subset[0]),
                  u(g_uc_subset[1]), u(g_uc_subset_nvb[0]), u(g_uc_subset_nvb[1]), u(g_uc_miss[0]), u(g_uc_miss[1]), u(g_uc_miss_nvb[0]),
                  u(g_uc_miss_nvb[1]), u(g_uc_listed1[0]), u(g_uc_listed1[1]), u(g_uc_listed2[0]), u(g_uc_listed2[1]), u(g_uc_draws1[0]),
                  u(g_uc_draws1[1]), u(g_uc_draws2[0]), u(g_uc_draws2[1]), u(g_uc_over), u(g_uc_unjudged),
                  static_cast<unsigned long long>(g_uc_last_cat.load()), static_cast<unsigned long long>(g_uc_last_n1.load()),
                  static_cast<unsigned long long>(g_uc_last_n2.load()), static_cast<unsigned long long>(g_uc_last_miss.load()),
                  static_cast<unsigned long long>(g_uc_last_sum2.load()));
    cut = cut || n4 < 0 || n4 >= static_cast<int>(sizeof(b));
    out += b;
    const int n5 = std::snprintf(b, sizeof(b),  // ([Stereo] MainUnionBox, item 8e)
                  " ub_installed=%d ub_on=%d ub_frames=%llu ub_noeye2=%llu ub_calls=%llu ub_notvp=%llu ub_odd=%llu ub_emu_ne=%llu ub_up0=%llu ub_up1=%llu "
                  "ub_e2_eq=%llu ub_e2_ne=%llu",
                  g_ub_installed.load() ? 1 : 0, g_ub_on.load() ? 1 : 0, u(g_ub_frames), u(g_ub_noeye2), u(g_ub_calls), u(g_ub_notvp), u(g_ub_odd), u(g_ub_emu_ne),
                  u(g_ub_up0), u(g_ub_up1), u(g_ub_e2_eq), u(g_ub_e2_ne));
    cut = cut || n5 < 0 || n5 >= static_cast<int>(sizeof(b));
    out += b;
    out += d3d::desc_census_kv();  // ([Stereo] DescCensus, item 8e; empty without it)
    out += d3d::debug_msg_kv();  // ([Debug] D3D12DebugLayer, item 8e; empty without it)
    if (cut) out += " kv_truncated=1";
    return out + " kv_end=1";  // (the test checks this last key)
}

// ---- ([Stereo] MainFamily=region, item 8e) "prepass mi region kv [first]": the region's counters, then per pair from ordinal first
// "pNN=vs:ps:name:st:frames:e2_exact:e1_exact:neg_differ:cov_frames:cov_min:cov_max:react0:joint:joint_exact:swap:swap_differ:swap_min"
// (st: the slot state last seen, hex: bits 0-7 the state, 8-12 the kinds plain pw e2i ip wvp)
// then per control kind 1..5 (nudge eye1 e2img pw ip) ":frames:e2_differ:e1_exact" (8a's name, spaces as '_'; "-" not in 8a's census);
// a reply past about 9 KB ends its pairs early with kv_next=<the next ordinal>
const char* const kRgUnjName[6] = {"no_e1", "order", "other_thread", "over", "no_p2", "marker"};  // (main_region's bits, in its order)
const char* const kRgGateName[12] = {"cascades", "rows", "ucull", "p1vp", "control", "allow", "auto_e2i", "auto_pw", "pw_live", "ip_live", "fade", "cut"};
const char* const kRvName[pso::region::kVerdicts] = {"none", "unknown_pso", "no_twin", "kind_wvp", "gate_e2i", "gate_pw", "gate_ip", "twin", "latch", "format",
                                                     "ip_off"};
void rg_cost(double* cyc_per_draw, double* ms_per_dup) {  // (E5) the listed path's rdtsc, calibrated against QPC since the install
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    const double secs = f.QuadPart ? static_cast<double>(q.QuadPart - g_rg_qpc0.load()) / static_cast<double>(f.QuadPart) : 0.0;
    const double hz = secs > 0.0 ? static_cast<double>(__rdtsc() - g_rg_tsc0.load()) / secs : 0.0;
    const double n = static_cast<double>(g_rg_tsc_n.load()), dups = static_cast<double>(g_rg_dup.load());
    *cyc_per_draw = n > 0.0 ? static_cast<double>(g_rg_tsc.load()) / n : 0.0;
    *ms_per_dup = hz > 0.0 && dups > 0.0 ? 1000.0 * static_cast<double>(g_rg_tsc.load()) / hz / dups : 0.0;
}
void rg_add(std::string& out, bool& cut, const char* fmt, ...) {
    char b[4000];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= static_cast<int>(sizeof(b))) cut = true;
    out += b;
}
std::string rg_kv(int first) {
    auto mx = [](const std::atomic<uint64_t>& a) {
        const uint64_t v = a.load();
        return static_cast<unsigned long long>(v == ~0ull ? 0 : v);
    };
    std::string out;
    out.reserve(10000);
    bool cut = false;
    const int sel = g_rg_sel.load();
    double cyc = 0, ms = 0;
    rg_cost(&cyc, &ms);
    rg_add(out, cut,
           "rg_installed=%d rg_on=%d rg_mode=%d rg_solo=%d rg_swap=%d rg_every=%d rg_subset_hex=%016llx ords=%d rg_frames=%llu rg_dup=%llu rg_nondup=%llu "
           "rg_noord=%llu rg_twin=%llu rg_swap_draws=%llu rg_swap_frames=%llu qi=%llu qi_hit=%llu qi_failed=%llu mrv=%llu mrv_stale=%llu "
           "mrv_twice=%llu rx_verdict=%llu rx_unj=%llu rx_gate=%llu rx_single=%llu rx_miss=%llu rx_noord=%llu good_joint=%llu good_solo=%llu "
           "good_subset=%llu rg_rep=%llu marg_frames=%llu ad_cyc_per_draw=%.0f ad_ms_per_dup_frame=%.4f ad_draws=%llu frames_good=%llu frames_bad=%llu "
           "x_pred=%llu x_refused=%llu x_seed=%llu x_order=%llu x_none=%llu no_twin=%llu no_cl1=%llu refused=%llu predicated=%llu cap=%d left=%d "
           "c_frames=%llu c_layout=%llu rc_layout=%llu rc_swap_frames=%llu rc_swap_differ=%llu",
           g_rg_installed.load() ? 1 : 0, main_region::comparing() ? 1 : 0, sel & 0xff, (sel >> 8) - 1, g_rg_swap.load(), g_rg_every.load(),
           static_cast<unsigned long long>(g_rg_subset.load()), main_region::ord_count(), u(g_rg_frames), u(g_rg_dup), u(g_rg_nondup), u(g_rg_noord),
           u(g_rg_twin), u(g_rg_swap_draws), u(g_rg_swap_frames), u(g_rg_qi), u(g_rg_qi_hit), u(g_rg_qi_failed), u(g_rg_mrv), u(g_rg_mrv_stale),
           u(g_rg_mrv_twice), u(g_rg_x_verdict), u(g_rg_x_unj), u(g_rg_x_gate), u(g_rg_x_single), u(g_rg_x_miss), u(g_rg_x_noord),
           u(g_rg_good[kRgJoint]), u(g_rg_good[kRgSolo]), u(g_rg_good[kRgSubset]), u(g_rg_rep), u(g_rg_marg_frames), cyc, ms, u(g_rg_tsc_n), u(g_f_good),
           u(g_f_bad), u(g_f_x_pred), u(g_f_x_refused), u(g_f_x_seed), u(g_f_x_order), u(g_f_x_none), u(g_p_no_twin), u(g_p_no_cl1), u(g_p_refused),
           u(g_p_predicated), g_cap.load(), g_cap_left.load(), u(g_c_frames), u(g_c_layout), u(g_rc_layout), u(g_rc_swap_frames), u(g_rc_swap_differ));
    rg_add(out, cut, " rg_ip=%d rg_pso=%d x_vf=%llu x_fmt=%llu ip_twins=%llu mro=%llu mro_stale=%llu mrv_frame=%llu", g_rg_ip.load() ? 1 : 0,
           pso::main_family_region() ? 1 : 0, u(g_rg_x_vf), u(g_rg_x_fmt), u(g_rg_ip_twins), u(g_rg_mro), u(g_rg_mro_stale), u(g_rg_mrv_frame));
    for (int k = 1; k < kCkMarked; ++k) rg_add(out, cut, " ctl_frames_%s=%llu", kCkName[k], u(g_rg_ctl_frames[k]));
    for (int v = 1; v < pso::region::kVerdicts; ++v) rg_add(out, cut, " rv_%s=%llu", kRvName[v], u(g_rv[v]));
    for (int b = 0; b < 6; ++b) rg_add(out, cut, " unj_%s=%llu", kRgUnjName[b], u(g_rg_unjb[b]));
    for (int b = 0; b < 12; ++b) rg_add(out, cut, " gate_%s=%llu", kRgGateName[b], u(g_rg_gateb[b]));
    for (int m = kRgJoint; m < kRgModes; ++m) {
        const char* n = kRgModeName[m];
        rg_add(out, cut, " rc_%s_frames=%llu rc_%s_e2=%llu rc_%s_e1=%llu rc_%s_neg=%llu rc_%s_cov=%llu rc_%s_react0=%llu rc_%s_pass=%llu rc_%s_cov_min=%llu "
                         "rc_%s_cov_max=%llu rc_%s_react_max=%llu rc_%s_rep=%llu rc_%s_rep_e2=%llu",
               n, u(g_rc_frames[m]), n, u(g_rc_e2[m]), n, u(g_rc_e1[m]), n, u(g_rc_neg[m]), n, u(g_rc_cov[m]), n, u(g_rc_react0[m]), n, u(g_rc_pass[m]),
               n, mx(g_rc_cov_min[m]), n, mx(g_rc_cov_max[m]), n, mx(g_rc_react_max[m]), n, u(g_rc_rep[m]), n, u(g_rc_rep_e2[m]));
        for (int k = 1; k < kCkMarked; ++k)
            rg_add(out, cut, " ck_%s_%s=%llu:%llu:%llu", n, kCkName[k], u(g_rc_ck_frames[m][k]), u(g_rc_ck_e2[m][k]), u(g_rc_ck_e1[m][k]));
    }
    for (int k = 1; k < kCkMarked; ++k) rg_add(out, cut, " ck_min_%s=%llu", kCkName[k], mx(g_rc_ck_min[k]));
    // (item 8e's diagnosis) per compare kind and control kind with frames: pp_<kind>_<control>=<frames>|e2=<per plane fsc, vel, react, depth,
    // stencil: frames differing/texels summed/most in a frame/largest difference>|e1=<the same>; the last failing frame; the save
    for (int pm = 1; pm < kPm && first <= 0; ++pm)  // (page 0 only: the review's should-fix, so a page always has room for pairs)
        for (int k = 0; k < kCk; ++k) {
            const PlaneAcc& a = g_pa[pm][k];
            if (!a.frames.load()) continue;
            rg_add(out, cut, " pp_%s_%s=%llu", kPmName[pm], kCkName[k], u(a.frames));
            for (int e = 0; e < 2; ++e)
                for (int pl = 0; pl < kPlanes; ++pl)
                    rg_add(out, cut, "%s%llu/%llu/%llu/%.6g", pl ? "," : e ? "|e1=" : "|e2=", u(a.nf[e][pl]), u(a.tx[e][pl]), mx(a.mt[e][pl]),
                           a.wa[e][pl].load());
        }
    if (first <= 0) rg_add(out, cut, " lf_n=%llu", u(g_lf_n));
    if (first <= 0 && g_lf_n.load()) {
        LastFail r;
        {
            std::lock_guard lock(g_lf_mutex);
            r = g_lf;
        }
        rg_add(out, cut, " lf=%s:%s:%d:%d:%llu:%d:%d:%llu:%llu", kPmName[r.pm >= 0 && r.pm < kPm ? r.pm : 0], kCkName[r.ck >= 0 && r.ck < kCk ? r.ck : 0],
               r.solo, r.swap, static_cast<unsigned long long>(r.frame), r.listed1, r.listed2, static_cast<unsigned long long>(r.cov),
               static_cast<unsigned long long>(r.neg));
        for (int pl = 0; pl < kPlanes; ++pl)
            rg_add(out, cut, "%s%llu/%.6g/%u/%u/%u/%u", pl ? "," : "|e2=", static_cast<unsigned long long>(r.tx[0][pl]), r.wa[0][pl], r.box[pl][0],
                   r.box[pl][1], r.box[pl][2], r.box[pl][3]);
        for (int pl = 0; pl < kPlanes; ++pl)
            rg_add(out, cut, "%s%llu/%.6g", pl ? "," : "|e1=", static_cast<unsigned long long>(r.tx[1][pl]), r.wa[1][pl]);
        rg_add(out, cut, "|split=%llu/%llu/%llu/%llu/%llu/%d", static_cast<unsigned long long>(r.sp.s_only), static_cast<unsigned long long>(r.sp.r_only),
               static_cast<unsigned long long>(r.sp.both_diff), static_cast<unsigned long long>(r.sp.both_eq),
               static_cast<unsigned long long>(r.sp.ds_only), r.sp.bad ? 1 : 0);
    }
    if (first <= 0) {  // (the last kLff failing frames, oldest first)
        uint64_t f[kLff];
        unsigned n = 0, k0 = 0;
        {
            std::lock_guard lock(g_lf_mutex);
            n = g_lff_n < kLff ? g_lff_n : kLff;
            k0 = g_lff_n - n;
            for (unsigned i = 0; i < n; ++i) f[i] = g_lff[(k0 + i) % kLff];
        }
        rg_add(out, cut, " lff=");
        for (unsigned i = 0; i < n; ++i) rg_add(out, cut, "%s%llu", i ? "," : "", static_cast<unsigned long long>(f[i]));
        if (!n) rg_add(out, cut, "-");
    }
    rg_add(out, cut, " own_armed=%d own_on=%d rg_own=%llu own_marks=%llu own_mark_failed=%llu rx_own=%llu seed_rule=%s seed_early=%llu", g_own_armed.load() ? 1 : 0,
           g_own_on.load() ? 1 : 0, u(g_rg_own), u(g_own_marks), u(g_own_mark_failed), u(g_rg_x_own), g_rg_seed_early_on.load() ? "early" : "listed",
           u(g_rg_seed_early));
    uint32_t any_seed = 0;
    for (const auto& c : g_rg_fsc_seed) any_seed |= c.load();
    rg_add(out, cut, " fscseed=%d", any_seed ? 1 : 0);  // (item 8f0b: set, the coverage counts invalid)
    if (first <= 0 && g_e1f_installed.load()) {  // ([Stereo] MainRegionE1Compare, item 8f; page 0 only, so the pages keep room for the pairs)
        rg_add(out, cut,
               " e1f_installed=1 e1f_on=%d e1f_pred_live=%d e1f_ctl=%s e1f_single=%d e1f_latched=%llu e1f_seeds=%llu e1f_v2_copies=%llu e1f_seed_ctl=%llu "
               "e1f_t=%llu e1f_c2=%llu e1f_own_draws=%llu e1f_own_refused=%llu e1f_pred_live_twins=%llu e1f_barrier=%llu e1f_clears=%llu e1f_writes=%llu e1f_splits=%llu e1f_planes_differ=%llu e1f_plane_st=%x:%x e1f_uav_barriers=%llu "
               "e1f_transitions_colour=%llu e1f_transitions_depth=%llu e1f_e1end=%llu "
               "e1f_e1end_stale=%llu e1f_single_draws=%llu e1f_x_seed=%llu e1f_x_t=%llu e1f_x_c2=%llu e1f_frames=%llu e1f_ctl_frames=%llu "
               "e1f_dlss_frames=%llu e1f_fxaa_frames=%llu e1f_layout=%llu e1f_g0_exact=%llu e1f_g1_exact=%llu e1f_pass=%llu e1f_neg_differ=%llu "
               "e1f_neg_min=%llu e1f_cov_min=%llu e1f_cov_max=%llu e1f_fsc_uncovered=%llu e1f_fsc_uncovered_max=%llu e1f_g1_fsc_cov_max=%llu "
               "e1f_g1_fsc_unc_max=%llu",
               g_e1f_on.load() ? 1 : 0, g_e1f_live.load() ? 1 : 0, kCkName[g_e1f_ctl.load() >= 0 && g_e1f_ctl.load() < kCk ? g_e1f_ctl.load() : 0],
               g_e1f_single.load(), u(g_e1f_latched), u(g_e1f_seeds), u(g_e1f_v2_copies), u(g_e1f_seed_ctl), u(g_e1f_t), u(g_e1f_c2), u(g_e1f_own_draws),
               u(g_e1f_own_refused), u(g_e1f_pred_live), u(g_e1f_barrier), u(g_e1f_clears), u(g_e1f_writes), u(g_e1f_splits), u(g_e1f_planes_differ), g_e1f_pst[0].load(), g_e1f_pst[1].load(), u(g_e1f_uav_barriers),
               u(g_e1f_transitions[0]), u(g_e1f_transitions[1]), u(g_e1f_e1end), u(g_e1f_e1end_stale), u(g_e1f_single_draws), u(g_e1f_x_seed),
               u(g_e1f_x_t), u(g_e1f_x_c2), u(g_e1f_cmp), u(g_e1f_cmp_ctl), u(g_e1f_dlss_frames), u(g_e1f_fxaa_frames), u(g_e1f_layout),
               u(g_e1f_g0_exact), u(g_e1f_g1_exact), u(g_e1f_pass), u(g_e1f_neg_differ), mx(g_e1f_neg_min), mx(g_e1f_cov_min), mx(g_e1f_cov_max),
               u(g_e1f_unc_sum), mx(g_e1f_unc_max), mx(g_e1f_g1_cov_max), mx(g_e1f_g1_unc_max));
        for (int pl = 0; pl < kPlanes; ++pl)  // (frames differing, texels differing summed, the most in one frame; G0's fsc: R1 not the sentinel)
            rg_add(out, cut, " e1f_g0_%s_frames=%llu e1f_g0_%s_texels=%llu e1f_g0_%s_max=%llu e1f_g1_%s_frames=%llu e1f_g1_%s_texels=%llu e1f_g1_%s_max=%llu",
                   kPlName[pl], u(g_e1f_g0_nf[pl]), kPlName[pl], u(g_e1f_g0_tx[pl]), kPlName[pl], mx(g_e1f_g0_max[pl]), kPlName[pl], u(g_e1f_g1_nf[pl]),
                   kPlName[pl], u(g_e1f_g1_tx[pl]), kPlName[pl], mx(g_e1f_g1_max[pl]));
        for (int r = 0; r < kEss; ++r) rg_add(out, cut, " e1f_sx_%s=%llu", kEsName[r], u(g_e1f_sx[r]));
        for (int r = 0; r < kEts; ++r) rg_add(out, cut, " e1f_tx_%s=%llu", kEtName[r], u(g_e1f_tx[r]));
        for (int r = 0; r < kEcs; ++r) rg_add(out, cut, " e1f_cx_%s=%llu", kEcName[r], u(g_e1f_cx[r]));
        for (int r = 0; r < kFsUsers; ++r) rg_add(out, cut, " x_follow_stale_%s=%llu", kFsName[r], u(g_fs_stale[r]));  // (r117 DG4)
        rg_add(out, cut, " follow_stale_reads=%llu follow_stale_e2sc=%llu follow_stale_e2lb=%llu follow_stale_e2vel=%llu",
               static_cast<unsigned long long>(prepass::follow_stale_reads()), static_cast<unsigned long long>(prepass::follow_stale(prepass::kFollowE2sc)),
               static_cast<unsigned long long>(prepass::follow_stale(prepass::kFollowE2lb)), static_cast<unsigned long long>(prepass::follow_stale(prepass::kFollowE2vel)));
        uint64_t closes = 0;  // (item 8f's remap)
        for (const Count& c : g_rm_closed) closes += c.load();
        rg_add(out, cut, " e1f_remap=%d e1f_remap_frames=%llu e1f_remap_skipped_draws=%llu e1f_remap_own_s0=%llu e1f_remap_close=%llu e1f_remap_closes=%llu "
                         "e1f_remap_single=%llu e1f_remap_lost=%llu",
               g_rm_want.load() ? 1 : 0, u(g_rm_frames), u(g_rm_skipped), u(g_rm_own_s0), u(g_rm_closed[kRmEnd]), static_cast<unsigned long long>(closes),
               u(g_rm_single), u(g_rm_lost_n));
        for (int r = 0; r < kRms; ++r) rg_add(out, cut, " e1f_remap_closed_%s=%llu e1f_remap_lost_%s=%llu", kRmName[r], u(g_rm_closed[r]), kRmName[r], u(g_rm_lost[r]));
        for (int r = 0; r < kRos; ++r) rg_add(out, cut, " e1f_remap_refused_%s=%llu", kRoName[r], u(g_rm_refused[r]));
        rg_add(out, cut, " e1f_remap_refused_depth_st=%x:%x e1f_remap_depth_st=%x:%x e1f_remap_ro_frames=%llu e1f_remap_ro_closes=%llu e1f_remap_ro_write_draws=%llu "
               "e1f_ro_dsv_depth=%llu e1f_ro_dsv_stencil=%llu e1f_ro_dsv_both=%llu e1f_remap_dropped_draws=%llu e1f_remap_drop_noremap=%llu e1f_remap_poisoned=%llu "
               "e1f_remap_last_tr=%u:%x:%x:%x:%u e1f_remap_skip_last=%llu e1f_remap_after_last=%llu e1f_remap_after_sum=%llu",
               g_rm_ref_st[0].load(), g_rm_ref_st[1].load(), g_rm_open_st[0].load(),
               g_rm_open_st[1].load(), u(g_rm_ro_frames), u(g_rm_ro_closes), u(g_rm_ro_write_draws), u(g_e1f_ro_dsv_draws[0]), u(g_e1f_ro_dsv_draws[1]),
               u(g_e1f_ro_dsv_draws[2]), u(g_rm_dropped), u(g_rm_drop_noremap), u(g_rm_poisoned),
               g_rm_last_tr[0].load(), g_rm_last_tr[1].load(), g_rm_last_tr[2].load(), g_rm_last_tr[3].load(), g_rm_last_tr[4].load(),
               static_cast<unsigned long long>(g_rm_skip_last.load()), static_cast<unsigned long long>(g_rm_after_last.load()), u(g_rm_after_sum));
        rg_add(out, cut, " e1f_remap_poison_rb=%s", rm_poison_text().c_str());
        rg_add(out, cut, " e1f_dupall_mask=%d e1f_dupall_mask_twins=%llu", g_e1f_dupall_mask.load(), u(g_e1f_dupall_mask_twins));
        rg_add(out, cut, " e1f_twins_mask=%d e1f_twins_mask2_twins=%llu e1f_twins_mask_remap_frames=%llu e1f_neg_unmeasured=%llu", g_e1f_twins_mask.load(),
               u(g_e1f_twins_mask2), u(g_e1f_twins_mask_remap), u(g_e1f_neg_unmeasured));
        rg_add(out, cut, " e1f_c2pred=%s e1f_c2pred_nulled=%llu e1f_c2pred_unknown=%llu e1f_c2pred_bad=%llu", g_e1f_c2null.load() ? "null" : "live",
               u(g_e1f_c2pred_nulled), u(g_e1f_c2pred_unknown), u(g_e1f_c2pred_bad));
        rg_add(out, cut, " e1f_dupall=%d e1f_dupall_twins=%d e1f_dupall_frames=%llu e1f_dupall_notwin_frames=%llu e1f_dupall_ignored=%llu "
               "e1f_notwin_draws=%llu e1f_twin_open_query=%llu e1f_twin_query_unknown=%llu", g_e1f_dupall.load() ? 1 : 0,
               g_e1f_dupall_twins.load() ? 1 : 0, u(g_e1f_dupall_frames), u(g_e1f_dupall_notwin), u(g_e1f_dupall_ignored), u(g_e1f_notwin_draws),
               u(g_e1f_twin_open_query), u(g_e1f_twin_query_unknown));  // (E2/E3/E5, test only)
        if (g_ho_installed.load()) {  // ([Stereo] MainRegionHandover, item 8g)
            int ready = 0, building = 0, refused = 0;
            for (const HoMerge& m : g_hm) {
                const int st = m.state.load();
                ready += st == 2;
                building += st == 1;
                refused += st == -1;
            }
            const int hm = g_ho_mode.load();
            rg_add(out, cut,
                   " ho_installed=1 ho_mode=%s ho_strict=%d ho_stood=%d ho_frames=%llu ho_handed=%llu ho_nohandover=%llu ho_identity=%llu ho_dropped=%llu "
                   "ho_drop_last=%llu ho_own2=%llu ho_miss2=%llu ho_noord2=%llu ho_indirect2=%llu ho_pred_drops=%llu ho_pred_risk_frames=%llu "
                   "ho_query_in_run=%llu ho_excluded=%llu ho_merges=%llu ho_velreact=%llu ho_fxaa_skip=%llu ho_t=%llu ho_r1_draws=%llu ho_r1_skipped=%llu "
                   "ho_merge_ready=%d ho_merge_building=%d ho_merge_refused=%d ho_srv_idx=%u hc_frames=%llu hc_exact=%llu hc_dlss=%llu hc_fxaa=%llu "
                   "hc_layout=%llu hc_cov_min=%llu hc_cov_max=%llu hc_nohandover=%llu:%llu hc_identity=%llu:%llu ho_own_keep=%llu ho_query_hold=%llu "
                   "hc_fsc_cov_diff=%llu hc_fsc_unc_diff=%llu hc_fsc_own_diff=%llu ho_mask_draws=%llu ho_save=%d ho_sparse=%d ho_sparse_skipped=%llu "
                   "ho_copyonly=%llu hc_copyonly=%llu:%llu e1_srv_p1=%llu:%llu:%llu:%llu e1_srv_p2=%llu:%llu:%llu:%llu "
                   "ho_velreact_always=%d ho_vr_fxaa=%llu ho_dropmask=%016llx ho_masked_kept=%llu ho_ords=%d ho_r1sync=%d ho_r1sync_n=%llu ho_r1dsv=%s "
                   "ho_r1dsv_draws=%llu ho_r1dsv_refused=%llu ho_r1_other_list=%llu ho_dec_noord=%llu",
                   kHoModeName[hm >= 0 && hm < 5 ? hm : 0], g_ho_strict.load() ? 1 : 0, g_ho_stood.load() ? 1 : 0, u(g_ho_frames), u(g_ho_handed),
                   u(g_ho_nohand), u(g_ho_ident), u(g_ho_dropped), static_cast<unsigned long long>(g_ho_drop_last.load()), u(g_ho_own2), u(g_ho_miss2),
                   u(g_ho_noord2), u(g_ho_indirect), u(g_ho_pred_drops), u(g_ho_pred_risk), u(g_ho_query_in_run), u(g_ho_strict_excl), u(g_ho_merges),
                   u(g_ho_velreact), u(g_ho_fxaa_skip), u(g_ho_t), u(g_ho_r1_draws), u(g_ho_r1_skipped), ready, building, refused, g_ho_idx.load(),
                   u(g_hc_frames), u(g_hc_exact), u(g_hc_dlss), u(g_hc_fxaa), u(g_hc_layout), mx(g_hc_cov_min), mx(g_hc_cov_max), u(g_hc_nohand_frames),
                   u(g_hc_nohand_differ), u(g_hc_ident_frames), u(g_hc_ident_exact), u(g_ho_own_keep), static_cast<unsigned long long>(kHoQueryHold),
                   u(g_hc_fsc_cov_diff), u(g_hc_fsc_unc_diff), u(g_hc_fsc_own_diff), u(g_ho_mask_draws), g_ho_sv_want.load() ? 1 : 0,
                   g_ho_sparse.load() ? 1 : 0, u(g_ho_sparse_skipped), u(g_ho_copyonly), u(g_hc_copy_frames), u(g_hc_copy_exact), u(g_e1_srv[1][0]),
                   u(g_e1_srv[1][1]), u(g_e1_srv[1][2]), u(g_e1_srv[1][3]), u(g_e1_srv[2][0]), u(g_e1_srv[2][1]), u(g_e1_srv[2][2]), u(g_e1_srv[2][3]),
                   g_ho_vr_always.load() ? 1 : 0, u(g_ho_vr_fxaa), static_cast<unsigned long long>(g_ho_dropmask.load()), u(g_ho_masked_kept),
                   main_region::ord_count(), g_ho_r1sync.load() ? 1 : 0, u(g_ho_r1sync_n), g_ho_r1dsv_game.load() ? "game" : "r", u(g_ho_r1dsv_draws),
                   u(g_ho_r1dsv_refused), u(g_ho_r1_other_list), u(g_ho_dec_noord));
            {  // (the ordinals drawn in pass 2's e1 on handover frames, with their draws: ho_pairs=ord:n,...; ho_npairs their number)
                std::string ps;
                int np = 0;
                for (int o = 0; o < 64; ++o)
                    if (const uint64_t n = u(g_ho_pair_n[o])) {
                        char e[48];
                        std::snprintf(e, sizeof(e), "%s%d:%llu", np ? "," : "", o, static_cast<unsigned long long>(n));
                        ps += e;
                        ++np;
                    }
                rg_add(out, cut, " ho_npairs=%d ho_pairs=%s", np, np ? ps.c_str() : "none");
                std::string ds_;
                int nd = 0;
                for (int o = 0; o < 64; ++o)
                    if (const uint64_t n = u(g_ho_dec_n[o])) {
                        char e[48];
                        std::snprintf(e, sizeof(e), "%s%d:%llu", nd ? "," : "", o, static_cast<unsigned long long>(n));
                        ds_ += e;
                        ++nd;
                    }
                rg_add(out, cut, " ho_dec=%s", nd ? ds_.c_str() : "none");  // (the decision draws' ordinals: ord:frames)
                std::string ws;
                int nw = 0;
                for (int o = 0; o < 64; ++o)
                    if (const uint64_t n = u(g_ho_r1_dsw_n[o])) {
                        char e[48];
                        std::snprintf(e, sizeof(e), "%s%d:%llu", nw ? "," : "", o, static_cast<unsigned long long>(n));
                        ws += e;
                        ++nw;
                    }
                rg_add(out, cut, " ho_r1_dswrite_draws=%llu ho_r1_dswrite_pairs=%s ho_r1dsv_kept_r=%llu", u(g_ho_r1_dsw), nw ? ws.c_str() : "none",
                       u(g_ho_r1dsv_kept_r));
            }
            for (int pl = 0; pl < kPlanes; ++pl)
                rg_add(out, cut, " hc_%s_frames=%llu hc_%s_texels=%llu hc_%s_max=%llu", kPlName[pl], u(g_hc_nf[pl]), kPlName[pl], u(g_hc_tx[pl]), kPlName[pl],
                       mx(g_hc_max[pl]));
            for (int r = 0; r < kHws; ++r) rg_add(out, cut, " ho_x_%s=%llu", kHwName[r], u(g_ho_x[r]));
            for (int k = 1; k < kCk; ++k) rg_add(out, cut, " hc_ck_%s=%llu:%llu", kCkName[k], u(g_hc_ctl_frames[k]), u(g_hc_ctl_differ[k]));
            // (GOAL-single-pass-2 item 4) ref h1's G1 and H1 frames (h1_<plane>: texels differ, summed over the H1 frames), trim's planes left.
            // e1f_frames + e1f_ctl_frames + e1f_layout = g1alt_cmp + g1_refused_e1f + h1_refused_e1f + the e1f compares of frames latched
            // neither (none under ref h1 while the handover is on, not stood down, the remap off: every e1f frame is then a handover frame)
            rg_add(out, cut,
                   " ho_ref=%s ho_trim=%d g1alt_frames=%llu ho_h=%llu h1_frames=%llu h1_exact=%llu h1_dlss=%llu h1_fxaa=%llu h1_layout=%llu "
                   "h1_fsc=%llu h1_vel=%llu h1_react=%llu h1_depth=%llu h1_stencil=%llu h1_nohandover=%llu:%llu h1_identity=%llu:%llu "
                   "ho_trim_depth=%llu ho_trim_stencil=%llu h1_copyonly=%llu:%llu h1_trim_frames=%llu h1_trim_s1t=%llu:%llu g1_refused=%llu h1_refused=%llu "
                   "g1alt_cmp=%llu g1_refused_e1f=%llu h1_refused_e1f=%llu",
                   g_ho_ref_h1.load() ? "h1" : "r1", g_ho_trim.load() ? 1 : 0, u(g_ho_g1alt), u(g_ho_h), u(g_h1_frames), u(g_h1_exact), u(g_h1_dlss),
                   u(g_h1_fxaa), u(g_h1_layout), u(g_h1_tx[0]), u(g_h1_tx[1]), u(g_h1_tx[2]), u(g_h1_tx[3]), u(g_h1_tx[4]), u(g_h1_nohand_frames),
                   u(g_h1_nohand_differ), u(g_h1_ident_frames), u(g_h1_ident_exact), u(g_ho_trim_depth), u(g_ho_trim_stencil),
                   u(g_h1_copy_frames), u(g_h1_copy_exact), u(g_h1_trim_frames), u(g_h1_trim_s1t_frames), u(g_h1_trim_s1t_tx), u(g_ho_g1_refused),
                   u(g_ho_h1_refused), u(g_e1f_alt[1]), u(g_e1f_alt[2]), u(g_e1f_alt[3]));
            for (int pl = 0; pl < kPlanes; ++pl) rg_add(out, cut, " h1_%s_frames=%llu", kPlName[pl], u(g_h1_nf[pl]));
            for (int k = 1; k < kCk; ++k) rg_add(out, cut, " h1_ck_%s=%llu:%llu", kCkName[k], u(g_h1_ctl_frames[k]), u(g_h1_ctl_differ[k]));
        }
        for (int k = 1; k < kCk; ++k)  // (frames : G1 differs : the frames its pair drew (single) : of them G1 differs)
            rg_add(out, cut, " e1f_ck_%s=%llu:%llu:%llu:%llu", kCkName[k], u(g_e1f_ck_frames[k]), u(g_e1f_ck_g1[k]), u(g_e1f_ck_drew[k]),
                   u(g_e1f_ck_drew_g1[k]));
    }
    rg_add(out, cut, " sv_want=%d sv_done=%llu sv_failed=%llu", g_sv_want.load(), u(g_sv_done), u(g_sv_failed));
    const pso::E1Pair* e1 = nullptr;
    const int ne1 = pso::e1_pairs(&e1);
    const int n = main_region::ord_count();
    int o = first < 0 ? 0 : first;
    for (; o < n && o < kOrds; ++o) {
        if (o > (first < 0 ? 0 : first) && out.size() > 9000) break;  // (at least one pair a page: never a page that repeats itself)
        uint64_t vs = 0, ps = 0;
        if (!main_region::ord_pair(o, &vs, &ps)) continue;
        const int x = pso::e1_pair(vs, ps);
        char nm[48];
        std::snprintf(nm, sizeof(nm), "%s", x >= 0 && x < ne1 ? e1[x].name : "-");
        for (char* c = nm; *c; ++c)
            if (*c == ' ' || *c == '=') *c = '_';
        const PairRes& r = g_pr[o];
        rg_add(out, cut, " p%02d=%08x:%08x:%s:%x:%llu:%llu:%llu:%llu:%llu:%llu:%llu:%llu:%llu:%llu:%llu:%llu:%llu", o, static_cast<unsigned>(vs >> 32),
               static_cast<unsigned>(ps >> 32), nm, main_region::ord_state(o), u(r.frames), u(r.e2), u(r.e1), u(r.neg), u(r.cov), mx(r.cov_min), mx(r.cov_max), u(r.react0),
               u(r.joint), u(r.joint_exact), u(r.swap), u(r.swap_differ), mx(r.swap_min));
        for (int k = 1; k < kCkMarked; ++k) rg_add(out, cut, ":%llu:%llu:%llu", u(r.ck_frames[k]), u(r.ck_e2[k]), u(r.ck_e1[k]));
        for (int k = 1; k < kCkMarked; ++k) rg_add(out, cut, ":%llu:%llu", u(r.ck_cov[k]), u(r.ck_cov_e2[k]));  // (after the triplets: older readers unchanged)
    }
    if (o < n && o < kOrds) rg_add(out, cut, " kv_next=%d", o);
    if (cut) out += " kv_truncated=1";
    return out + " kv_end=1";
}
std::string rg_status() {
    const int sel = g_rg_sel.load();
    const int m = (sel & 0xff) < kRgModes ? (sel & 0xff) : 0;
    char b[2400];
    std::snprintf(b, sizeof(b),
                  "mi region %s: mode %s (solo ordinal %d, subset %016llx), swap %s, ip twins %s, a dup frame 1 in %d while armed; ordinals %d | frames %llu (dup %llu, "
                  "not %llu), twins %llu, swap draws %llu, QueryInterface %llu (cached %llu, failed %llu) | 8d's verdicts %llu (stale %llu, twice %llu) | "
                  "dup frames good: joint %llu, solo %llu, subset %llu; excluded: no verdict %llu, unjudged %llu, gated %llu, single %llu, miss %llu, "
                  "no ordinal %llu, refused %llu (no twin %llu), predicated %llu, seed %llu | compared joint %llu (eye 2 exact %llu), solo %llu (%llu), "
                  "subset %llu (%llu), swap %llu (differ %llu) | last refusal: %s",
                  main_region::comparing() ? "on (mr compare)" : "off (\"prepass mr compare\")", kRgModeName[m], (sel >> 8) - 1,
                  static_cast<unsigned long long>(g_rg_subset.load()), g_rg_swap.load() ? "auto" : "off", g_rg_ip.load() ? "on" : "off (refused)",
                  g_rg_every.load(), main_region::ord_count(),
                  u(g_rg_frames), u(g_rg_dup), u(g_rg_nondup), u(g_rg_twin), u(g_rg_swap_draws), u(g_rg_qi), u(g_rg_qi_hit), u(g_rg_qi_failed),
                  u(g_rg_mrv), u(g_rg_mrv_stale), u(g_rg_mrv_twice), u(g_rg_good[kRgJoint]), u(g_rg_good[kRgSolo]), u(g_rg_good[kRgSubset]),
                  u(g_rg_x_verdict), u(g_rg_x_unj), u(g_rg_x_gate), u(g_rg_x_single), u(g_rg_x_miss), u(g_rg_x_noord), u(g_p_refused), u(g_p_no_twin),
                  u(g_p_predicated), u(g_f_x_seed), u(g_rc_frames[kRgJoint]), u(g_rc_e2[kRgJoint]), u(g_rc_frames[kRgSolo]), u(g_rc_e2[kRgSolo]),
                  u(g_rc_frames[kRgSubset]), u(g_rc_e2[kRgSubset]), u(g_rc_swap_frames), u(g_rc_swap_differ), g_why[0] ? g_why : "none");
    return b;
}
// ---- ("e1f g1trace", test only) the copies (the playback thread) and the command (the command thread)
// one box of src's subresource sub (RENDER_TARGET now: a bound colour target after a draw, or S at rest) into stream s's next slot; m
// its fields. The caller has the list unpredicated (a live predicate nulled). A slot not copied is kept, ok false, its reason noted
void gt_slot_refused(int s, GtSlot m, const char* why) {
    const int n = g_gt_n[s].load(std::memory_order_relaxed);
    bump(g_gt_refused);
    g_gt_rwhy.store(why, std::memory_order_relaxed);
    if (n >= g_gt_cap) {
        bump(g_gt_lost[s]);
        return;
    }
    m.ok = false;
    g_gt_slot[s][n] = m;
    g_gt_n[s].store(n + 1, std::memory_order_release);
}
void gt_copy(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, UINT sub, int s, GtSlot m) {
    const int n = g_gt_n[s].load(std::memory_order_relaxed);
    if (n >= g_gt_cap) {
        bump(g_gt_lost[s]);
        return;
    }
    if (!src || !g_gt_rb || g_gt_dev != state::device.load()) return gt_slot_refused(s, m, "no readback for this device, or no target");
    const D3D12_RESOURCE_DESC d = src->GetDesc();
    if (d.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && d.Format != DXGI_FORMAT_R16G16B16A16_TYPELESS)
        return gt_slot_refused(s, m, "the target is not RGBA16F");
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || g_gt_bx0 + g_gt_bw > d.Width || g_gt_by0 + g_gt_bh > d.Height ||
        sub >= static_cast<UINT>(d.DepthOrArraySize) * d.MipLevels)
        return gt_slot_refused(s, m, "the box or the subresource is outside the target");
    Batch bt;
    bt.add(src, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    bt.flush(cl);
    D3D12_TEXTURE_COPY_LOCATION dst{g_gt_rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    dst.PlacedFootprint.Offset = g_gt_slot_bytes * (static_cast<uint64_t>(s) * static_cast<uint64_t>(g_gt_cap) + static_cast<uint64_t>(n));
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    dst.PlacedFootprint.Footprint.Width = g_gt_bw;
    dst.PlacedFootprint.Footprint.Height = g_gt_bh;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = g_gt_pitch;
    D3D12_TEXTURE_COPY_LOCATION sl{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    sl.SubresourceIndex = sub;
    const D3D12_BOX box{g_gt_bx0, g_gt_by0, 0, g_gt_bx0 + g_gt_bw, g_gt_by0 + g_gt_bh, 1};
    d3d::raw_copy_region(cl, &dst, &sl, &box);
    bt.add(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    bt.flush(cl);
    m.ok = true;
    g_gt_slot[s][n] = m;
    g_gt_n[s].store(n + 1, std::memory_order_release);
}
// gt_copy with the list's own predicate (if any) nulled around it: from the list's record; null_now: the caller has it nulled already
void gt_copy_unpred(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, UINT sub, int s, const GtSlot& m, bool null_now) {
    if (null_now || !d3d::thread_predicated()) return gt_copy(cl, src, sub, s, m);
    d3d::Predicate p;
    if (d3d::thread_predicate(cl, &p) != d3d::kPredSet || !p.res) {
        bump(g_gt_pred_unknown);
        return gt_slot_refused(s, m, "the list is predicated and its record unknown");
    }
    d3d::raw_set_predication(cl, nullptr, 0, 0);
    gt_copy(cl, src, sub, s, m);
    d3d::raw_set_predication(cl, p.res, p.off, p.op);
}
// pass 2: the game's own FSC as bound (the four, no split barrier begun on them since T) into stream 2
void gt_game(ID3D12GraphicsCommandList* cl, const GtSlot& m, bool null_now) {
    Bound b;
    if (check_bind(cl, &b) || b.res[kFsc] != g_a[kS][kFsc].game) return gt_slot_refused(1, m, "the four not bound (the game's FSC not known)");
    if (g_f.e1f_split) return gt_slot_refused(1, m, "a split barrier begun on the four since T");
    gt_copy_unpred(cl, b.res[kFsc], 0, 1, m, null_now);
}
std::string gt_command(const std::vector<std::string>& t) {  // ("prepass mi region e1f g1trace ...")
    char b[1400];
    const char* const usage = "ERROR prepass mi region e1f g1trace <absolute dir> <x0> <y0> <w> <h>|off";
    if (t.size() == 6 && t[5] == "off") {
        int armed = kTrArmed;  // (only an armed trace is dropped; a recording finishes, a recorded one is written)
        g_gt_st.compare_exchange_strong(armed, kTrOff);
    } else if (t.size() == 10) {
        const int x0 = std::atoi(t[6].c_str()), y0 = std::atoi(t[7].c_str()), w = std::atoi(t[8].c_str()), h = std::atoi(t[9].c_str());
        if (x0 < 0 || y0 < 0 || w < 1 || h < 1 || w > static_cast<int>(kGtBoxMax) || h > static_cast<int>(kGtBoxMax))
            return "ERROR mi region e1f g1trace: the box (w and h 1..64)";
        const DWORD at = GetFileAttributesA(t[5].c_str());
        if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) return "ERROR mi region e1f g1trace: " + t[5] + " is not a directory";
        ID3D12Device* dev = state::device.load();
        if (!dev) return "ERROR mi region e1f g1trace: no device yet";
        int off = kTrOff;  // (armed only from off: the readback and the fields written in Setup, then Armed released)
        if (!g_gt_st.compare_exchange_strong(off, kTrSetup)) return "ERROR mi region e1f g1trace: a trace is armed, recording or not yet written";
        if (g_gt_rb && (g_gt_dev != dev || g_gt_rbw != static_cast<UINT>(w) || g_gt_rbh != static_cast<UINT>(h))) {
            g_gt_rb->Release();  // (off: no copy into it in flight, the last one written kTrWait presents after its frame)
            g_gt_rb = nullptr;
        }
        if (!g_gt_rb) {
            const UINT pitch = (static_cast<UINT>(w) * 8 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
            const uint64_t sb = (static_cast<uint64_t>(pitch) * static_cast<uint64_t>(h) + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                                ~static_cast<uint64_t>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
            uint64_t cap = kGtBytes / (kGtStreams * sb);
            if (cap > static_cast<uint64_t>(kGtCapMax)) cap = kGtCapMax;
            D3D12_HEAP_PROPERTIES rh{};
            rh.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = sb * cap * kGtStreams;
            rd.Height = 1;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource* r = nullptr;
            if (cap < 1 || FAILED(dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r))) || !r) {
                g_gt_st.store(kTrOff);
                return "ERROR mi region e1f g1trace: the readback not made";
            }
            r->SetName(L"RDRVR e1f g1trace readback (test only)");
            g_gt_rb = r;
            g_gt_dev = dev;
            g_gt_pitch = pitch;
            g_gt_slot_bytes = sb;
            g_gt_cap = static_cast<int>(cap);
            g_gt_rbw = static_cast<UINT>(w);
            g_gt_rbh = static_cast<UINT>(h);
        }
        {
            std::lock_guard lock(g_gt_mutex);
            g_gt_dir = t[5];
        }
        g_gt_x0 = static_cast<UINT>(x0);
        g_gt_y0 = static_cast<UINT>(y0);
        g_gt_w = static_cast<UINT>(w);
        g_gt_h = static_cast<UINT>(h);
        for (auto& n : g_gt_n) n.store(0);
        g_gt_why.store(0);
        g_gt_rwhy.store("", std::memory_order_relaxed);
        g_gt_st.store(kTrArmed, std::memory_order_release);
        log::info("[mi] region e1f g1trace armed (test only): box %d,%d %dx%d, %d slots a stream, to %s (the next e1f compare frame not handed over)", x0, y0, w,
                  h, g_gt_cap, t[5].c_str());
    } else if (t.size() != 5) {
        return usage;
    }
    if (g_gt_st.load(std::memory_order_acquire) == kTrDone && state::presents.load() >= g_gt_done.load() + kTrWait && g_gt_rb) {
        std::string dir;
        {
            std::lock_guard lock(g_gt_mutex);
            dir = g_gt_dir;
        }
        const unsigned long long fr = static_cast<unsigned long long>(g_gt_frame.load());
        int nn[kGtStreams] = {};
        for (int s = 0; s < kGtStreams; ++s) nn[s] = g_gt_n[s].load(std::memory_order_acquire);
        void* map = nullptr;
        const D3D12_RANGE rr{0, static_cast<SIZE_T>(g_gt_slot_bytes * static_cast<uint64_t>(g_gt_cap) * kGtStreams)};
        std::string err;
        if (SUCCEEDED(g_gt_rb->Map(0, &rr, &map)) && map) {
            char name[600];
            // the streams: g1trace_<frame>_s<1..3>.gtb, a header of 8 uint32 (GTB1, w, h, slots, stream, frame, row bytes w * 8, 0) then the
            // slots' boxes tightly packed (RGBA16F rows); a slot not copied (ok=0 in the manifest) holds zeros
            for (int s = 0; s < kGtStreams && err.empty(); ++s) {
                std::snprintf(name, sizeof(name), "%s\\g1trace_%llu_s%d.gtb", dir.c_str(), fr, s + 1);
                FILE* f = nullptr;
                if (fopen_s(&f, name, "wb") || !f) {
                    err = std::string("cannot open ") + name;
                    break;
                }
                const uint32_t hdr[8] = {0x31425447u, g_gt_bw, g_gt_bh, static_cast<uint32_t>(nn[s]), static_cast<uint32_t>(s + 1), static_cast<uint32_t>(fr),
                                         g_gt_bw * 8, 0u};
                fwrite(hdr, sizeof(hdr), 1, f);
                std::vector<uint8_t> zero(static_cast<size_t>(g_gt_bw) * 8, 0);
                for (int i = 0; i < nn[s]; ++i) {
                    const uint8_t* base = static_cast<const uint8_t*>(map) + g_gt_slot_bytes * (static_cast<uint64_t>(s) * static_cast<uint64_t>(g_gt_cap) + static_cast<uint64_t>(i));
                    for (UINT y = 0; y < g_gt_bh; ++y)
                        fwrite(g_gt_slot[s][i].ok ? base + static_cast<uint64_t>(y) * g_gt_pitch : zero.data(), static_cast<size_t>(g_gt_bw) * 8, 1, f);
                }
                if (fclose(f) != 0) err = std::string("cannot write ") + name;
            }
            if (err.empty()) {  // the manifest: g1trace_<frame>.txt, a header line, then a line per slot
                std::snprintf(name, sizeof(name), "%s\\g1trace_%llu.txt", dir.c_str(), fr);
                FILE* mf = nullptr;
                if (fopen_s(&mf, name, "wb") || !mf) {
                    err = std::string("cannot open ") + name;
                } else {
                    std::fprintf(mf, "frame=%llu box=%u,%u,%u,%u s1=%d s2=%d s3=%d cap=%d lost=%llu,%llu,%llu refused=%llu pred_unknown=%llu why=%#x "
                                 "(s1 S1 after each pass-1 twin; s2 the game's FSC: T, then after each pass-2 e1 draw; s3 S1 after each own draw's redraw)\n",
                                 fr, g_gt_bx0, g_gt_by0, g_gt_bw, g_gt_bh, nn[0], nn[1], nn[2], g_gt_cap, u(g_gt_lost[0]), u(g_gt_lost[1]), u(g_gt_lost[2]),
                                 u(g_gt_refused), u(g_gt_pred_unknown), g_gt_why.load());
                    for (int s = 0; s < kGtStreams; ++s)
                        for (int i = 0; i < nn[s]; ++i) {
                            const GtSlot& m = g_gt_slot[s][i];
                            uint64_t vs = 0, ps = 0;
                            const bool kp = m.ord >= 0 && main_region::ord_pair(m.ord, &vs, &ps);
                            std::fprintf(mf, "stream=%d n=%d ok=%d t=%d own=%d noord=%d miss=%d ord=%d pair=%08x:%08x indexed=%d a=%u b=%u c=%u d=%d e=%u\n",
                                         s + 1, i, m.ok ? 1 : 0, m.t ? 1 : 0, m.own ? 1 : 0, m.noord ? 1 : 0, m.miss ? 1 : 0, m.ord,
                                         kp ? static_cast<unsigned>(vs >> 32) : 0u, kp ? static_cast<unsigned>(ps >> 32) : 0u, m.a.indexed ? 1 : 0, m.a.a,
                                         m.a.b, m.a.c, m.a.d, m.a.e);
                        }
                    if (fclose(mf) != 0) err = std::string("cannot write ") + name;
                }
            }
            const D3D12_RANGE none{0, 0};
            g_gt_rb->Unmap(0, &none);
        } else {
            err = "the readback did not map";
        }
        std::snprintf(b, sizeof(b), "%s frame %llu (slots %d, %d, %d) to %s%s%s", err.empty() ? "written" : "FAILED", fr, nn[0], nn[1], nn[2], dir.c_str(),
                      err.empty() ? "" : ": ", err.c_str());
        {
            std::lock_guard lock(g_gt_mutex);
            g_gt_last = b;
        }
        log::info("[mi] region e1f g1trace: %s", b);
        g_gt_st.store(kTrOff);
    }
    std::string last;
    {
        std::lock_guard lock(g_gt_mutex);
        last = g_gt_last;
    }
    std::snprintf(b, sizeof(b), "mi region e1f g1trace: state %d (0 off, 1 armed, 2 recording, 3 recorded), slots %d, %d, %d (cap %d), frame %llu, frames "
                  "traced %llu, lost %llu, %llu, %llu, refused %llu (last: %s), pred unknown %llu; last: %s", g_gt_st.load(), g_gt_n[0].load(), g_gt_n[1].load(),
                  g_gt_n[2].load(), g_gt_cap, static_cast<unsigned long long>(g_gt_frame.load()), u(g_gt_frames), u(g_gt_lost[0]), u(g_gt_lost[1]),
                  u(g_gt_lost[2]), u(g_gt_refused), g_gt_rwhy.load(std::memory_order_relaxed), u(g_gt_pred_unknown), last.empty() ? "none" : last.c_str());
    return b;
}
// ([Stereo] MainRegionE1Compare, item 8f) "prepass mi region e1f": its status line
std::string e1f_status() {
    char b[1600];
    const int ck = g_e1f_ctl.load();
    std::snprintf(b, sizeof(b),
                  "mi region e1f %s, twins' predicate %s, control %s (single ordinal %d) | frames latched %llu, seeds %llu (V2 copied %llu, the seed "
                  "control %llu), T %llu, C2 %llu, own draws into S1 %llu (refused %llu), twins under the live predicate %llu, barriers on the four "
                  "after T %llu, e1 ends %llu (stale %llu) | excluded: seed %llu, T %llu, C2 %llu | compared %llu (DLSS %llu, FXAA %llu; control "
                  "frames %llu): G0 exact %llu, G1 exact %llu, pass %llu, negative differs %llu, coverage %llu..%llu | remap %s: frames %llu, "
                  "skipped draws %llu, own into S0 %llu, closed at e1's end %llu, lost %llu | last refusal: %s",
                  !g_e1f_installed.load() ? "not installed ([Stereo] MainRegionE1Compare=1, MainFamily=region, PrepassEye2LightDepth=1; test only)"
                  : g_e1f_on.load()       ? "on (joint dup frames)"
                                          : "off",
                  g_e1f_live.load() ? "live" : "nulled", kCkName[ck >= 0 && ck < kCk ? ck : 0], g_e1f_single.load(), u(g_e1f_latched), u(g_e1f_seeds),
                  u(g_e1f_v2_copies), u(g_e1f_seed_ctl), u(g_e1f_t), u(g_e1f_c2), u(g_e1f_own_draws), u(g_e1f_own_refused), u(g_e1f_pred_live),
                  u(g_e1f_barrier), u(g_e1f_e1end), u(g_e1f_e1end_stale), u(g_e1f_x_seed), u(g_e1f_x_t), u(g_e1f_x_c2), u(g_e1f_cmp),
                  u(g_e1f_dlss_frames), u(g_e1f_fxaa_frames), u(g_e1f_cmp_ctl), u(g_e1f_g0_exact), u(g_e1f_g1_exact), u(g_e1f_pass),
                  u(g_e1f_neg_differ), static_cast<unsigned long long>(g_e1f_cov_min.load() == ~0ull ? 0 : g_e1f_cov_min.load()),
                  static_cast<unsigned long long>(g_e1f_cov_max.load()), g_rm_want.load() ? "on" : "off", u(g_rm_frames), u(g_rm_skipped),
                  u(g_rm_own_s0), u(g_rm_closed[kRmEnd]), u(g_rm_lost_n), g_e1f_why[0] ? g_e1f_why : "none");
    if (!g_ho_installed.load()) return b;
    char h[1400];  // ([Stereo] MainRegionHandover, item 8g)
    const int hm = g_ho_mode.load();
    uint64_t h1c = 0;  // (GOAL-single-pass-2 item 4: H1's control frames, all kinds)
    for (int k = 1; k < kCk; ++k) h1c += u(g_h1_ctl_frames[k]);
    std::snprintf(h, sizeof(h),
                  " | handover %s%s%s: frames %llu, handed over %llu (nohandover %llu, identity %llu), dropped %llu (last frame %llu), drawn by the "
                  "game: own %llu, misses %llu, no ordinal %llu, indirect %llu | G1' compared %llu: exact %llu | ref %s: G1 frames %llu (refused "
                  "otherwise %llu), H1 refused %llu, H1 compared %llu: exact %llu (DLSS %llu, FXAA %llu; copyonly %llu, exact %llu; nohandover %llu, "
                  "differ %llu; identity %llu, exact %llu; control frames %llu) | trim %s: "
                  "depth not copied %llu, stencil not copied %llu | last refusal: %s",
                  kHoModeName[hm >= 0 && hm < 5 ? hm : 0], g_ho_strict.load() ? " (strict)" : "", g_ho_stood.load() ? " (stood down)" : "",
                  u(g_ho_frames), u(g_ho_handed), u(g_ho_nohand), u(g_ho_ident), u(g_ho_dropped), static_cast<unsigned long long>(g_ho_drop_last.load()),
                  u(g_ho_own2), u(g_ho_miss2), u(g_ho_noord2), u(g_ho_indirect), u(g_hc_frames), u(g_hc_exact),
                  g_ho_ref_h1.load() ? "h1 (G1 and H1 alternate)" : "r1 (G1' against R1)", u(g_ho_g1alt), u(g_ho_g1_refused), u(g_ho_h1_refused),
                  u(g_h1_frames), u(g_h1_exact), u(g_h1_dlss), u(g_h1_fxaa), u(g_h1_copy_frames), u(g_h1_copy_exact), u(g_h1_nohand_frames), u(g_h1_nohand_differ), u(g_h1_ident_frames), u(g_h1_ident_exact),
                  static_cast<unsigned long long>(h1c), g_ho_trim.load() ? "on" : "off", u(g_ho_trim_depth), u(g_ho_trim_stencil),
                  g_ho_why[0] ? g_ho_why : "none");
    return std::string(b) + h;
}
// "prepass mi region e1f [on|off|pred live|null]" (the next pass 1 latches each; off by default, the predicate live by default)
std::string e1f_command(const std::vector<std::string>& t) {
    if (!g_e1f_installed.load()) return e1f_status();
    if (t.size() >= 5 && t[4] == "g1trace") return gt_command(t);  // (test only: its own status line)
    if (t.size() == 5 && (t[4] == "on" || t[4] == "off")) {
        g_e1f_on = t[4] == "on";
        log::info("[mi] region e1f %s (test only: joint dup frames from the next pass 1)", t[4].c_str());
    } else if (t.size() == 7 && t[4] == "handover" && t[5] == "r1sync" && (t[6] == "on" || t[6] == "off")) {  // (r120 HG3, test only)
        g_ho_r1sync = t[6] == "on";
        log::info("[mi] region e1f handover r1sync %s (a full flush after T -> R1)", t[6].c_str());
    } else if (t.size() == 7 && t[4] == "handover" && t[5] == "r1dsv" && (t[6] == "game" || t[6] == "r")) {  // (r120 HG3, test only)
        g_ho_r1dsv_game = t[6] == "game";
        log::info("[mi] region e1f handover r1dsv %s (R1's draws bind %s)", t[6].c_str(), t[6] == "game" ? "the game's own DSV" : "R's depth views");
    } else if (t.size() == 7 && t[4] == "handover" && t[5] == "dropmask") {  // (H2m's diagnosis, test only)
        uint64_t m = ~0ull;
        if (t[6] != "all") {
            if (t[6].empty() || t[6].size() > 16) return "ERROR prepass mi region e1f handover dropmask <hex (1-16 digits)>|all";
            for (char ch : t[6])
                if (!std::isxdigit(static_cast<unsigned char>(ch))) return "ERROR prepass mi region e1f handover dropmask <hex (1-16 digits)>|all";
            m = std::strtoull(t[6].c_str(), nullptr, 16);
        }
        g_ho_dropmask = m;
        log::info("[mi] region e1f handover dropmask %016llx (bit o: the pair of session ordinal o dropped)", static_cast<unsigned long long>(m));
    } else if (t.size() == 7 && t[4] == "handover" && t[5] == "velreact" && (t[6] == "always" || t[6] == "tech")) {  // (H2m's diagnosis, test only)
        g_ho_vr_always = t[6] == "always";
        log::info("[mi] region e1f handover velreact %s (S1's velocity and reactivity handed over %s)", t[6].c_str(),
                  t[6] == "always" ? "under FXAA too" : "under DLSS only");
    } else if (t.size() == 7 && t[4] == "handover" && t[5] == "ref" && (t[6] == "r1" || t[6] == "h1")) {  // (GOAL-single-pass-2 item 4, test only)
        g_ho_ref_h1 = t[6] == "h1";
        log::info("[mi] region e1f handover ref %s (%s)", t[6].c_str(),
                  t[6] == "h1" ? "compare frames alternate: G1, S1 against C2, not handed over; H1, H right after the handover against S1 and T"
                               : "G1', C against R1");
    } else if (t.size() == 7 && t[4] == "handover" && t[5] == "trim" && (t[6] == "on" || t[6] == "off")) {  // (GOAL-single-pass-2 item 4, test only)
        g_ho_trim = t[6] == "on";
        log::info("[mi] region e1f handover trim %s (%s)", t[6].c_str(),
                  t[6] == "on" ? "a depth plane no listed pass-1 e1 draw writes is not copied" : "both depth planes copied");
    } else if (t.size() == 7 && t[4] == "handover" && t[5] == "sparse" && (t[6] == "on" || t[6] == "off")) {  // (H2m's diagnosis, test only)
        g_ho_sparse = t[6] == "on";
        log::info("[mi] region e1f handover sparse %s (the handover only on compare frames)", t[6].c_str());
    } else if (t.size() == 6 && t[4] == "handover" && (t[5] == "on" || t[5] == "off" || t[5] == "nohandover" || t[5] == "identity" || t[5] == "copyonly")) {
        if (!g_ho_installed.load()) return "mi region e1f handover: not installed (start with [Stereo] MainRegionHandover=1; test only)";
        g_ho_mode = t[5] == "on" ? 1 : t[5] == "nohandover" ? 2 : t[5] == "identity" ? 3 : t[5] == "copyonly" ? 4 : 0;  // (item 8g, test only)
        log::info("[mi] region e1f handover %s (test only: from the next pass 1)", t[5].c_str());
    } else if ((t.size() == 7 && t[4] == "handover" && t[5] == "save") || (t.size() == 6 && t[4] == "save")) {  // (H2m's diagnosis, test only: one-shot)
        // ("e1f save <dir>|off", the same one-shot: the first compared handover frame whose G1' differs, or 8f compare frame whose G1 FSC does)
        const std::string& arg = t.back();
        if (arg == "off") {
            g_ho_sv_want = false;
        } else {
            const DWORD at = GetFileAttributesA(arg.c_str());
            if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) return "ERROR mi region e1f handover save: " + arg + " is not a directory";
            {
                std::lock_guard lock(g_ho_sv_mutex);
                g_ho_sv_dir = arg;
            }
            g_ho_sv_want = true;
        }
        std::string last;
        {
            std::lock_guard lock(g_ho_sv_mutex);
            last = g_ho_sv_last;
        }
        return std::string("mi region e1f handover save ") + (g_ho_sv_want.load() ? "armed" : "off") + "; last: " + last;
    } else if (t.size() == 7 && t[4] == "handover" && t[5] == "strict" && (t[6] == "on" || t[6] == "off")) {
        g_ho_strict = t[6] == "on";
        log::info("[mi] region e1f handover strict %s", t[6].c_str());
    } else if (t.size() == 6 && t[4] == "c2pred" && (t[5] == "null" || t[5] == "live")) {  // (test only)
        g_e1f_c2null = t[5] == "null";
        log::info("[mi] region e1f c2pred %s (compare frames' pass-2 e1 draws %s)", t[5].c_str(),
                  t[5] == "null" ? "under SetPredication(null), the list's own put back after each" : "under the list's own predicate");
    } else if (t.size() == 6 && t[4] == "dupall" && (t[5] == "on" || t[5] == "off")) {  // (E2, test only)
        g_e1f_dupall = t[5] == "on";
        log::info("[mi] region e1f dupall %s (%s)", t[5].c_str(), t[5] == "on" ? "every joint e1f frame a dup frame while no handover mode is set and the "
                  "remap is off; compare frames 1 in N as before" : "dup frames are the compare frames, as before");
    } else if (t.size() == 7 && t[4] == "twins" && t[5] == "mask" && (t[6] == "3" || t[6] == "2")) {  // (test only)
        g_e1f_twins_mask = t[6][0] - '0';
        log::info("[mi] region e1f twins mask %s (%s)", t[6].c_str(), t[6] == "2" ? "every e1f frame's twins view 1 only, into S slice 1 (not on a remap "
                  "frame); S0 keeps the seed: the negative not measured, the pair trace refused" : "the twins both views, or dupall mask's");
    } else if (t.size() == 7 && t[4] == "dupall" && t[5] == "mask" && (t[6] == "3" || t[6] == "1" || t[6] == "2")) {  // (test only)
        g_e1f_dupall_mask = t[6][0] - '0';
        log::info("[mi] region e1f dupall mask %s (dupall frames' twins %s; compare frames' 3)", t[6].c_str(),
                  t[6] == "3" ? "both views" : t[6] == "1" ? "view 0 only" : "view 1 only");
    } else if (t.size() == 7 && t[4] == "dupall" && t[5] == "twins" && (t[6] == "on" || t[6] == "off")) {  // (E3, test only)
        g_e1f_dupall_twins = t[6] == "on";
        log::info("[mi] region e1f dupall twins %s (%s)", t[6].c_str(), t[6] == "on" ? "dupall frames draw the twins into S"
                  : "dupall frames that are no compare frames seed S and R, draw no twin");
    } else if (t.size() == 6 && t[4] == "remap" && (t[5] == "on" || t[5] == "off")) {  // (item 8f's remap, test only)
        g_rm_want = t[5] == "on";
        log::info("[mi] region e1f remap %s (test only: pass 1's S slice 0 stands in for the game's four over e1 from the next e1f frame)", t[5].c_str());
    } else if (t.size() == 6 && t[4] == "pred" && (t[5] == "live" || t[5] == "null")) {
        g_e1f_live = t[5] == "live";
        log::info("[mi] region e1f pred %s (pass 1's twins %s)", t[5].c_str(), t[5] == "live" ? "under the list's own predicate" : "under SetPredication(null)");
    } else if (t.size() != 4) {
        return "ERROR prepass mi region e1f [on|off|pred live|null|remap on|off|handover on|off|nohandover|identity|copyonly|handover strict|sparse on|off|handover velreact always|tech|handover dropmask <hex>|all|handover r1sync on|off|handover r1dsv game|r|"
               "handover save <dir>|off|handover ref r1|h1|handover trim on|off|dupall on|off|dupall twins on|off|dupall mask 3|1|2|twins mask 3|2|g1trace <dir> <x0> <y0> <w> <h>|off|c2pred null|live|save <dir>|off]";
    }
    return e1f_status();
}
// (item 8e's diagnosis) "prepass mi region save": its state; "save off"; "save <absolute dir> [any|fail]" (fail by default): one-shot,
// the next compared frame of the selection now set (the mode, the solo ordinal), written by the worker (rg_planes, sv_frame)
// ---- (item 8e 14, test only) "prepass mi region trace <absolute dir> <x0> <y0> <w> <h> <vs8:ps8>" | "trace off" | "trace": one region
// frame's per-draw history of one pair. On the next dup frame (a capture armed), after each pass-1 twin of that pair the box of S's slice
// 1 colour (FSC) is copied out, and after each pass-2 duplicate the box of R's slice 1; at pass 2's close the trace waits kTrWait
// presents (the frames in flight), then "trace" writes each slot as an RDE1 file and a manifest (the slot's pass, its order in the pass,
// the draw's arguments). Offline: the first draw after which S1 and R1 differ at a texel is the draw that makes the difference. The
// copies are the arrays' own (RENDER_TARGET -> COPY_SOURCE -> RENDER_TARGET), on the region's list, never under a predicate (the
// listed path nulls a held one before the duplicates)
void tr_copy(ID3D12GraphicsCommandList* cl, int set, const d3d::DrawArgs& args, bool pred_nulled) {  // (the playback thread, rg_after)
    const int pass = g_r.pass;
    const Arr& a = g_a[set][kFsc];
    if (!a.arr) return;
    const D3D12_RESOURCE_DESC ad = a.arr->GetDesc();
    if (ad.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && ad.Format != DXGI_FORMAT_R16G16B16A16_TYPELESS) {
        bump(g_tr_refused);
        return;
    }
    if (g_tr_bx0 + g_tr_bw > ad.Width || g_tr_by0 + g_tr_bh > ad.Height || ad.DepthOrArraySize < 2) {
        bump(g_tr_refused);
        return;
    }
    if (g_tr_rb && g_tr_dev != state::device.load()) {  // (the device re-made: never copied with another device's resource)
        bump(g_tr_refused);
        return;
    }
    if (!g_tr_rb) {
        ID3D12Device* dev = state::device.load();
        if (!dev) return;
        g_tr_dev = dev;
        g_tr_pitch = (g_tr_bw * 8 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        g_tr_slot_bytes = (static_cast<uint64_t>(g_tr_pitch) * g_tr_bh + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                          ~static_cast<uint64_t>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
        D3D12_HEAP_PROPERTIES rh{};
        rh.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = g_tr_slot_bytes * kTrSlots;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_tr_rb))) || !g_tr_rb) {
            g_tr_rb = nullptr;
            bump(g_tr_refused);
            g_tr_st.store(kTrOff);
            return;
        }
        g_tr_rb->SetName(L"RDRVR region trace readback (test only)");
        g_tr_img_cap = (static_cast<uint64_t>((g_tr_bw * 8 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)) * g_tr_bh +
                        D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~static_cast<uint64_t>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);  // (8 B a texel at most)
        rd.Width = g_tr_img_cap * 2 * kTrImgs;
        if (FAILED(dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_tr_rbi))) || !g_tr_rbi)
            g_tr_rbi = nullptr;  // (the RT trace goes on without the images)
        else
            g_tr_rbi->SetName(L"RDRVR region trace image readback (test only)");
    }
    const int n = g_tr_n.load(std::memory_order_relaxed);
    if (n >= kTrSlots) {
        bump(g_tr_lost);
        return;
    }
    Batch bt;
    bt.add(a.arr, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    bt.flush(cl);
    D3D12_TEXTURE_COPY_LOCATION dst{g_tr_rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    dst.PlacedFootprint.Offset = g_tr_slot_bytes * static_cast<uint64_t>(n);
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    dst.PlacedFootprint.Footprint.Width = g_tr_bw;
    dst.PlacedFootprint.Footprint.Height = g_tr_bh;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = g_tr_pitch;
    D3D12_TEXTURE_COPY_LOCATION src{a.arr, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    src.SubresourceIndex = 1;  // (slice 1, mip 0)
    const D3D12_BOX box{g_tr_bx0, g_tr_by0, 0, g_tr_bx0 + g_tr_bw, g_tr_by0 + g_tr_bh, 1};
    d3d::raw_copy_region(cl, &dst, &src, &box);
    Batch back;
    back.add(a.arr, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    back.flush(cl);
    g_tr_slot[n] = TrSlot{pass, g_tr_pn[pass]++, args};
    const int ni = g_tr_ni.load(std::memory_order_relaxed);
    if (g_tr_rbi && ni < kTrImgs && g_tr_pi[pass] < kTrImgPass) {
        ++g_tr_pi[pass];
        const uint64_t off[2] = {g_tr_img_cap * (2 * ni), g_tr_img_cap * (2 * ni + 1)};
        TrImg& im = g_tr_img[ni];
        const char* w = nullptr;
        if (eye2_light::trace_images(cl, pass, g_tr_rbi, off, g_tr_img_cap, im.fp, box, pred_nulled, &w)) {
            im.slot = n;
            g_tr_ni.store(ni + 1, std::memory_order_release);
        } else {
            bump(g_tr_img_refused);
            g_tr_img_why.store(w ? w : "", std::memory_order_relaxed);
        }
    }
    g_tr_n.store(n + 1, std::memory_order_release);
}
std::string rg_trace(const std::vector<std::string>& t) {  // (the command thread)
    char b[1200];
    if (t.size() == 5 && t[4] == "off") {
        int armed = kTrArmed;  // (only an armed trace is dropped; a recording finishes, a recorded one is written)
        g_tr_st.compare_exchange_strong(armed, kTrOff);
    } else if (t.size() == 10) {
        unsigned vs = 0, ps = 0;
        char tail = 0;
        if (t[9].size() != 17 || t[9][8] != ':' || sscanf_s(t[9].c_str(), "%8x:%8x%c", &vs, &ps, &tail, 1) != 2)
            return "ERROR prepass mi region trace <absolute dir> <x0> <y0> <w> <h> <vs8:ps8>|off";
        const int o = main_region::ord_find(vs, ps);
        if (o < 0) return "ERROR mi region trace: that pair has no ordinal";
        const int x0 = std::atoi(t[5].c_str()), y0 = std::atoi(t[6].c_str()), w = std::atoi(t[7].c_str()), h = std::atoi(t[8].c_str());
        if (x0 < 0 || y0 < 0 || w < 1 || h < 1 || w > 512 || h > 512) return "ERROR mi region trace: the box (w and h 1..512)";
        const DWORD at = GetFileAttributesA(t[4].c_str());
        if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) return "ERROR mi region trace: " + t[4] + " is not a directory";
        if (g_tr_rb && (static_cast<UINT>(w) != g_tr_bw || static_cast<UINT>(h) != g_tr_bh)) return "ERROR mi region trace: the box's size is fixed for the session";
        int off = kTrOff;  // (armed only from off: the fields written in Setup, then Armed released)
        if (!g_tr_st.compare_exchange_strong(off, kTrSetup)) return "ERROR mi region trace: a trace is armed, recording or not yet written";
        {
            std::lock_guard lock(g_tr_mutex);
            g_tr_dir = t[4];
        }
        g_tr_x0 = static_cast<UINT>(x0);
        g_tr_y0 = static_cast<UINT>(y0);
        g_tr_w = static_cast<UINT>(w);
        g_tr_h = static_cast<UINT>(h);
        g_tr_ord.store(o);
        g_tr_n.store(0);
        g_tr_why.store(0);
        g_tr_img_why.store("", std::memory_order_relaxed);
        g_tr_st.store(kTrArmed, std::memory_order_release);
        log::info("[mi] region trace armed (test only): pair %s (ordinal %d), box %d,%d %dx%d, to %s", t[9].c_str(), o, x0, y0, w, h, t[4].c_str());
    } else if (t.size() != 4) {
        return "ERROR prepass mi region trace <absolute dir> <x0> <y0> <w> <h> <vs8:ps8>|off";
    }
    if (g_tr_st.load(std::memory_order_acquire) == kTrDone && state::presents.load() >= g_tr_done.load() + kTrWait && g_tr_rb) {
        std::string dir;
        {
            std::lock_guard lock(g_tr_mutex);
            dir = g_tr_dir;
        }
        const int n = g_tr_n.load(std::memory_order_acquire);
        void* map = nullptr;
        const D3D12_RANGE rr{0, static_cast<SIZE_T>(g_tr_slot_bytes * kTrSlots)};
        int written = 0;
        if (SUCCEEDED(g_tr_rb->Map(0, &rr, &map)) && map) {
            char name[512];
            std::snprintf(name, sizeof(name), "%s\\trace_%llu.txt", dir.c_str(), static_cast<unsigned long long>(g_tr_frame.load()));
            FILE* mf = nullptr;
            fopen_s(&mf, name, "wb");
            for (int i = 0; i < n; ++i) {
                const TrSlot& s = g_tr_slot[i];
                std::snprintf(name, sizeof(name), "%s\\trace_%llu_%02d_p%d_%02d.rde", dir.c_str(), static_cast<unsigned long long>(g_tr_frame.load()), i, s.pass, s.n);
                FILE* f = nullptr;
                if (fopen_s(&f, name, "wb") || !f) continue;
                const uint32_t hdr[8] = {0x31454452u, g_tr_bw, g_tr_bh, 10u, 1u, static_cast<uint32_t>(g_tr_frame.load()), g_tr_bw * 8, 0u};
                fwrite(hdr, sizeof(hdr), 1, f);
                const uint8_t* base = static_cast<const uint8_t*>(map) + g_tr_slot_bytes * static_cast<uint64_t>(i);
                for (UINT y = 0; y < g_tr_bh; ++y) fwrite(base + static_cast<uint64_t>(y) * g_tr_pitch, g_tr_bw * 8, 1, f);
                fclose(f);
                ++written;
                if (mf)
                    std::fprintf(mf, "slot=%d pass=%d n=%d indexed=%d a=%u b=%u c=%u d=%d e=%u\n", i, s.pass, s.n, s.a.indexed ? 1 : 0, s.a.a, s.a.b, s.a.c, s.a.d, s.a.e);
            }
            if (mf) {
                std::fprintf(mf, "frame=%llu box=%u,%u,%u,%u slots=%d lost=%llu why=%#x\n", static_cast<unsigned long long>(g_tr_frame.load()), g_tr_bx0, g_tr_by0, g_tr_bw,
                             g_tr_bh, n, static_cast<unsigned long long>(g_tr_lost.load()), g_tr_why.load());
                fclose(mf);
            }
            const D3D12_RANGE none{0, 0};
            g_tr_rb->Unmap(0, &none);
        }
        const int ni = g_tr_ni.load(std::memory_order_acquire);  // (the images: RDI1, the rows at their footprint's pitch)
        void* imap = nullptr;
        const D3D12_RANGE ir{0, static_cast<SIZE_T>(g_tr_img_cap * 2 * kTrImgs)};
        if (ni > 0 && g_tr_rbi && SUCCEEDED(g_tr_rbi->Map(0, &ir, &imap)) && imap) {
            for (int i = 0; i < ni; ++i) {
                const TrImg& im = g_tr_img[i];
                if (im.slot < 0 || im.slot >= n) continue;
                const TrSlot& s = g_tr_slot[im.slot];
                for (int k = 0; k < 2; ++k) {
                    const D3D12_SUBRESOURCE_FOOTPRINT& fp = im.fp[k];
                    if (static_cast<uint64_t>(fp.RowPitch) * fp.Height > g_tr_img_cap) continue;
                    char name[512];
                    std::snprintf(name, sizeof(name), "%s\\trace_%llu_%02d_p%d_%02d_img%d.rdi", dir.c_str(), static_cast<unsigned long long>(g_tr_frame.load()), im.slot,
                                  s.pass, s.n, k);
                    FILE* f = nullptr;
                    if (fopen_s(&f, name, "wb") || !f) continue;
                    const uint32_t hdr[8] = {0x31494452u, fp.Width, fp.Height, static_cast<uint32_t>(fp.Format), fp.RowPitch,
                                             static_cast<uint32_t>(g_tr_frame.load()), static_cast<uint32_t>(s.pass), static_cast<uint32_t>(k)};
                    fwrite(hdr, sizeof(hdr), 1, f);
                    fwrite(static_cast<const uint8_t*>(imap) + g_tr_img_cap * static_cast<uint64_t>(2 * i + k), static_cast<size_t>(fp.RowPitch) * fp.Height, 1, f);
                    fclose(f);
                }
            }
            const D3D12_RANGE none{0, 0};
            g_tr_rbi->Unmap(0, &none);
        }
        std::snprintf(b, sizeof(b), "written %d slots of frame %llu to %s", written, static_cast<unsigned long long>(g_tr_frame.load()), dir.c_str());
        {
            std::lock_guard lock(g_tr_mutex);
            g_tr_last = b;
        }
        g_tr_st.store(kTrOff);
    }
    std::string last;
    {
        std::lock_guard lock(g_tr_mutex);
        last = g_tr_last;
    }
    std::snprintf(b, sizeof(b), "mi region trace: state %d (0 off, 1 armed, 2 recording, 3 recorded), ordinal %d, slots %d, frame %llu, lost %llu, refused %llu, images %d (readback %s, refused %llu: %s); last: %s",
                  g_tr_st.load(), g_tr_ord.load(), g_tr_n.load(), static_cast<unsigned long long>(g_tr_frame.load()), static_cast<unsigned long long>(g_tr_lost.load()),
                  static_cast<unsigned long long>(g_tr_refused.load()), g_tr_ni.load(), g_tr_rbi ? "made" : "none", static_cast<unsigned long long>(g_tr_img_refused.load()),
                  g_tr_img_why.load(std::memory_order_relaxed), last.empty() ? "none" : last.c_str());
    return b;
}
std::string rg_save(const std::vector<std::string>& t) {
    if (t.size() == 5 && t[4] == "off") {
        g_sv_want = 0;
    } else if (t.size() == 5 || t.size() == 6) {
        const std::string opt = t.size() == 6 ? t[5] : "fail";
        if (opt != "any" && opt != "fail") return "ERROR prepass mi region save <absolute dir> [any|fail]|off";
        std::string dir = t[4];
        while (dir.size() > 3 && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
        bool ok = dir.size() >= 3 && dir.size() <= 200 && std::isalpha(static_cast<unsigned char>(dir[0])) && dir[1] == ':' &&
                  (dir[2] == '\\' || dir[2] == '/');
        for (const char ch : dir)
            if (ch < 0x21 || ch > 0x7e || ch == '"' || ch == '<' || ch == '>' || ch == '|' || ch == '?' || ch == '*') ok = false;
        if (!ok) return "ERROR mi region save: an absolute directory of printable ASCII, no spaces, at most 200 characters";
        const DWORD at = GetFileAttributesA(dir.c_str());
        if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) return "ERROR mi region save: " + dir + " is not a directory";
        {
            std::lock_guard lock(g_sv_mutex);
            g_sv_dir = dir;
            g_sv_sel = g_rg_sel.load();
        }
        g_sv_want = opt == "any" ? 1 : 2;
        log::info("[mi] region save armed: the next %s compared frame of the selection (%d) to %s (test only)", opt == "any" ? "" : "failing",
                  g_rg_sel.load(), dir.c_str());
    } else if (t.size() != 4) {
        return "ERROR prepass mi region save <absolute dir> [any|fail]|off";
    }
    const int w = g_sv_want.load();
    std::string dir, last;
    int sel = 0;
    {
        std::lock_guard lock(g_sv_mutex);
        dir = g_sv_dir;
        last = g_sv_last;
        sel = g_sv_sel;
    }
    char b[900];
    std::snprintf(b, sizeof(b), "mi region save %s%s (selection: mode %s, solo ordinal %d); saved %llu, failed %llu; last: %s",
                  w == 0 ? "off" : w == 1 ? "armed (any) to " : "armed (fail) to ", w ? dir.c_str() : "",
                  kRgModeName[(sel & 0xff) < kRgModes ? (sel & 0xff) : 0], (sel & 0xff) == kRgSolo ? (sel >> 8) - 1 : -1,
                  u(g_sv_done), u(g_sv_failed), last.c_str());
    return b;
}
// "prepass mi region [all|solo <vs8>:<ps8>|subset <hex>|swap off|auto|ip on|off|every N|kv [first]|save ...]" (the next pass 1 latches each)
std::string region_command(const std::vector<std::string>& t) {
    const char* const usage =
        "ERROR prepass mi region [all|solo <vs 8 hex>:<ps 8 hex>|subset <64-bit hex>|swap off|auto|ip on|off|every N|kv [first]|save [<dir> [any|fail]|off]|"
        "e1f [on|off|pred live|null]]";
    if (!g_rg_installed.load())
        return "mi region: not installed (start with [Stereo] MainFamily=region, MainRegion=1, MainUnionCull=1, MainPredication=1 and "
               "InstancedShadersAuto=1; test only)";
    if (!rg_on())  // (M1: pso refused the switch at the device, or no device yet)
        return "mi region: not installed (pso has not taken [Stereo] MainFamily=region: no device yet, or [Stereo] MainInstanced's family "
               "fell back to bb; test only)";
    if (t.size() >= 4) {
        const std::string& a = t[3];
        if (a == "kv") return rg_kv(t.size() >= 5 ? std::atoi(t[4].c_str()) : 0);
        if (a == "save") return rg_save(t);  // (item 8e's diagnosis, test only)
        if (a == "trace") return rg_trace(t);  // (item 8e 14, test only)
        if (a == "e1f") return e1f_command(t);  // ([Stereo] MainRegionE1Compare, item 8f, test only)
        if (a == "all" && t.size() == 4) {
            g_rg_sel = kRgJoint;
        } else if (a == "solo" && t.size() == 5) {
            unsigned vs = 0, ps = 0;
            char tail = 0;
            if (t[4].size() != 17 || t[4][8] != ':' || sscanf_s(t[4].c_str(), "%8x:%8x%c", &vs, &ps, &tail, 1) != 2) return usage;
            const int o = main_region::ord_find(vs, ps);
            if (o < 0) return "ERROR mi region solo " + t[4] + ": that pair has no ordinal (not drawn in e1 since the session's start, or past 63)";
            g_rg_sel = kRgSolo | (o + 1) << 8;
        } else if (a == "subset" && t.size() == 5) {
            if (t[4].empty() || t[4].size() > 16) return usage;
            for (char ch : t[4])
                if (!std::isxdigit(static_cast<unsigned char>(ch))) return usage;
            const uint64_t m = std::strtoull(t[4].c_str(), nullptr, 16);
            if (!m) return usage;
            g_rg_subset = m;  // (before the word: a latch that reads the subset word reads this mask)
            g_rg_sel = kRgSubset;
        } else if (a == "swap" && t.size() == 5 && (t[4] == "off" || t[4] == "auto")) {
            g_rg_swap = t[4] == "auto" ? 1 : 0;
        } else if (a == "seed" && t.size() == 5 && (t[4] == "early" || t[4] == "listed")) {  // (item 8e 14: solo / subset's seed point)
            g_rg_seed_early_on = t[4] == "early";
        } else if (a == "fscseed" && (t.size() == 5 || t.size() == 8 || t.size() == 12)) {
            // (item 8f0b, test only: S's FSC seed colour, and R's if given (else S's); off: 0. Two colours: J differs where the seed shows)
            float v[8] = {};
            if (t.size() >= 8) {
                for (int c = 0; c < static_cast<int>(t.size()) - 4; ++c) {
                    char* end = nullptr;
                    v[c] = std::strtof(t[4 + c].c_str(), &end);
                    if (!end || *end || !std::isfinite(v[c])) return "ERROR prepass mi region fscseed <r> <g> <b> <a> [<r> <g> <b> <a>]|off (finite floats)";
                }
                if (t.size() == 8)
                    for (int c = 0; c < 4; ++c) v[4 + c] = v[c];
            } else if (t[4] != "off") {
                return "ERROR prepass mi region fscseed <r> <g> <b> <a> [<r> <g> <b> <a>]|off";
            }
            for (int c = 0; c < 8; ++c) {
                uint32_t u;
                std::memcpy(&u, &v[c], 4);
                g_rg_fsc_seed[c].store(u, std::memory_order_relaxed);
            }
        } else if (a == "own" && t.size() == 5 && (t[4] == "on" || t[4] == "off")) {  // (item 8e 14: off, the control)
            g_own_on = t[4] == "on";
        } else if (a == "ip" && t.size() == 5 && (t[4] == "on" || t[4] == "off")) {  // (S3: the instprev twins handed out, or kept refused)
            g_rg_ip = t[4] == "on";
        } else if (a == "every" && t.size() == 5) {
            const int n = std::atoi(t[4].c_str());
            if (n < 1 || n > 1000) return "ERROR mi region every N: 1..1000 (joint and subset take at least 8)";
            g_rg_every = n;
        } else {
            return usage;
        }
        log::info("[mi] region %s%s%s (test only: from the next pass 1)", a.c_str(), t.size() >= 5 ? " " : "", t.size() >= 5 ? t[4].c_str() : "");
    }
    return rg_status();
}
void rg_reset() {  // ("prepass mi reset": the region's counts; the ordinals, the mode and the latch's own state kept)
    for (Count* c : {&g_rg_own, &g_own_marks, &g_own_mark_failed, &g_rg_x_own, &g_rg_seed_early, &g_rg_frames, &g_rg_dup, &g_rg_nondup, &g_rg_noord, &g_rg_twin, &g_rg_swap_draws, &g_rg_swap_frames, &g_rg_qi, &g_rg_qi_hit,
                     &g_rg_qi_failed, &g_rg_mrv, &g_rg_mrv_stale, &g_rg_mrv_twice, &g_rg_x_verdict, &g_rg_x_unj, &g_rg_x_gate, &g_rg_x_single,
                     &g_rg_x_miss, &g_rg_x_noord, &g_rg_rep, &g_rg_marg_frames, &g_rg_tsc, &g_rg_tsc_n, &g_rc_swap_frames, &g_rc_swap_differ, &g_rc_layout,
                     &g_rg_x_vf, &g_rg_x_fmt, &g_rg_ip_twins, &g_rg_mro, &g_rg_mro_stale, &g_rg_mrv_frame})
        c->store(0);
    for (Count& c : g_rv) c.store(0);
    for (Count& c : g_rg_good) c.store(0);
    for (Count& c : g_rg_ctl_frames) c.store(0);
    for (Count& c : g_rg_unjb) c.store(0);
    for (Count& c : g_rg_gateb) c.store(0);
    for (int m = 0; m < kRgModes; ++m) {
        for (Count* c : {&g_rc_frames[m], &g_rc_e2[m], &g_rc_e1[m], &g_rc_neg[m], &g_rc_cov[m], &g_rc_react0[m], &g_rc_pass[m], &g_rc_rep[m], &g_rc_rep_e2[m]})
            c->store(0);
        for (int k = 0; k < kCk; ++k) {
            g_rc_ck_frames[m][k].store(0);
            g_rc_ck_e2[m][k].store(0);
            g_rc_ck_e1[m][k].store(0);
        }
        g_rc_cov_min[m].store(~0ull);
        g_rc_cov_max[m].store(0);
        g_rc_react_max[m].store(0);
    }
    for (auto& a : g_rc_ck_min) a.store(~0ull);
    for (auto& row : g_pa)  // (item 8e's diagnosis; the save's state is kept)
        for (PlaneAcc& a : row) {
            a.frames.store(0);
            for (int e = 0; e < 2; ++e)
                for (int pl = 0; pl < kPlanes; ++pl) {
                    a.nf[e][pl].store(0);
                    a.tx[e][pl].store(0);
                    a.mt[e][pl].store(0);
                    a.wa[e][pl].store(0.0);
                }
        }
    g_lf_n.store(0);
    {
        std::lock_guard lock(g_lf_mutex);
        g_lf = LastFail{};
        g_lff_n = 0;
    }
    for (PairRes& r : g_pr) {
        for (Count* c : {&r.frames, &r.e2, &r.e1, &r.neg, &r.cov, &r.react0, &r.joint, &r.joint_exact, &r.swap, &r.swap_differ}) c->store(0);
        for (int k = 0; k < kCk; ++k) {
            r.ck_frames[k].store(0);
            r.ck_e2[k].store(0);
            r.ck_e1[k].store(0);
            r.ck_cov[k].store(0);
            r.ck_cov_e2[k].store(0);
        }
        r.cov_min.store(~0ull);
        r.cov_max.store(0);
        r.swap_min.store(~0ull);
    }
    // ([Stereo] MainRegionE1Compare, item 8f) its counts (the switches, the control and its ordinal kept)
    for (Count* c : {&g_e1f_latched, &g_e1f_seeds, &g_e1f_v2_copies, &g_e1f_t, &g_e1f_c2, &g_e1f_own_draws, &g_e1f_own_refused, &g_e1f_pred_live,
                     &g_e1f_barrier, &g_e1f_clears, &g_e1f_writes, &g_e1f_splits, &g_e1f_planes_differ, &g_e1f_uav_barriers, &g_e1f_transitions[0], &g_e1f_transitions[1], &g_e1f_e1end, &g_e1f_e1end_stale, &g_e1f_seed_ctl, &g_e1f_single_draws, &g_e1f_x_seed, &g_e1f_x_t, &g_e1f_x_c2,
                     &g_e1f_cmp, &g_e1f_cmp_ctl, &g_e1f_g0_exact, &g_e1f_g1_exact, &g_e1f_pass, &g_e1f_neg_differ, &g_e1f_layout, &g_e1f_dlss_frames,
                     &g_e1f_fxaa_frames, &g_e1f_unc_sum, &g_rm_frames, &g_rm_skipped, &g_rm_own_s0, &g_rm_single, &g_rm_lost_n,
                     &g_rm_ro_frames, &g_rm_ro_closes, &g_rm_ro_write_draws, &g_e1f_ro_dsv_draws[0], &g_e1f_ro_dsv_draws[1],
                     &g_e1f_ro_dsv_draws[2], &g_rm_dropped, &g_rm_drop_noremap, &g_rm_poisoned, &g_rm_after_sum, &g_e1f_dupall_frames,
                     &g_e1f_dupall_notwin, &g_e1f_dupall_ignored, &g_e1f_notwin_draws, &g_e1f_twin_open_query, &g_e1f_twin_query_unknown, &g_e1f_c2pred_nulled,
                     &g_e1f_c2pred_unknown, &g_e1f_c2pred_bad, &g_e1f_dupall_mask_twins,
                     &g_e1f_twins_mask2, &g_e1f_twins_mask_remap, &g_e1f_neg_unmeasured, &g_gt_frames, &g_gt_refused, &g_gt_pred_unknown,
                     &g_gt_lost[0], &g_gt_lost[1], &g_gt_lost[2]})
        c->store(0);
    for (Count& c : g_rm_closed) c.store(0);
    for (Count* c : {&g_ho_frames, &g_ho_handed, &g_ho_nohand, &g_ho_ident, &g_ho_dropped, &g_ho_own2, &g_ho_miss2, &g_ho_noord2, &g_ho_indirect,
                     &g_ho_pred_drops, &g_ho_pred_risk, &g_ho_query_in_run, &g_ho_strict_excl, &g_ho_merges, &g_ho_velreact, &g_ho_fxaa_skip, &g_ho_r1_draws,
                     &g_ho_t, &g_ho_r1_skipped, &g_hc_frames, &g_hc_exact, &g_hc_layout, &g_hc_dlss, &g_hc_fxaa, &g_hc_nohand_frames, &g_hc_nohand_differ,
                     &g_hc_ident_frames, &g_hc_ident_exact, &g_ho_own_keep, &g_hc_fsc_cov_diff, &g_hc_fsc_unc_diff, &g_hc_fsc_own_diff,
                     &g_ho_mask_draws, &g_ho_sparse_skipped, &g_ho_copyonly, &g_hc_copy_frames, &g_hc_copy_exact, &g_ho_vr_fxaa, &g_ho_masked_kept, &g_ho_r1sync_n, &g_ho_r1dsv_draws,
                     &g_ho_r1dsv_refused, &g_ho_r1_other_list, &g_ho_dec_noord, &g_ho_r1_dsw, &g_ho_r1dsv_kept_r, &g_ho_g1alt, &g_ho_h,
                     &g_ho_trim_depth, &g_ho_trim_stencil, &g_h1_frames, &g_h1_exact, &g_h1_layout, &g_h1_dlss, &g_h1_fxaa, &g_h1_nohand_frames,
                     &g_h1_nohand_differ, &g_h1_ident_frames, &g_h1_ident_exact, &g_h1_copy_frames, &g_h1_copy_exact, &g_h1_trim_frames,
                     &g_h1_trim_s1t_frames, &g_h1_trim_s1t_tx, &g_ho_g1_refused, &g_ho_h1_refused, &g_e1f_alt[1], &g_e1f_alt[2], &g_e1f_alt[3]})
        c->store(0);  // ([Stereo] MainRegionHandover, item 8g; GOAL-single-pass-2 item 4)
    for (int pl = 0; pl < kPlanes; ++pl) {
        g_h1_tx[pl].store(0);
        g_h1_nf[pl].store(0);
    }
    for (int k = 0; k < kCk; ++k) {
        g_h1_ctl_frames[k].store(0);
        g_h1_ctl_differ[k].store(0);
    }
    for (Count& c : g_ho_x) c.store(0);
    for (auto& row : g_e1_srv)
        for (Count& c : row) c.store(0);
    for (Count& c : g_ho_pair_n) c.store(0);
    for (Count& c : g_ho_dec_n) c.store(0);
    for (Count& c : g_ho_r1_dsw_n) c.store(0);
    for (int pl = 0; pl < kPlanes; ++pl) {
        g_hc_nf[pl].store(0);
        g_hc_tx[pl].store(0);
        g_hc_max[pl].store(0);
    }
    for (int k = 0; k < kCk; ++k) {
        g_hc_ctl_frames[k].store(0);
        g_hc_ctl_differ[k].store(0);
    }
    g_hc_cov_min.store(~0ull);
    g_hc_cov_max.store(0);
    g_ho_drop_last.store(0);
    for (Count& c : g_rm_lost) c.store(0);
    for (Count& c : g_rm_refused) c.store(0);
    for (Count& c : g_e1f_sx) c.store(0);
    for (Count& c : g_fs_stale) c.store(0);
    for (Count& c : g_e1f_tx) c.store(0);
    for (Count& c : g_e1f_cx) c.store(0);
    for (int pl = 0; pl < kPlanes; ++pl) {
        g_e1f_g0_nf[pl].store(0);
        g_e1f_g1_nf[pl].store(0);
        g_e1f_g0_tx[pl].store(0);
        g_e1f_g1_tx[pl].store(0);
        g_e1f_g0_max[pl].store(0);
        g_e1f_g1_max[pl].store(0);
    }
    for (int k = 0; k < kCk; ++k) {
        g_e1f_ck_frames[k].store(0);
        g_e1f_ck_g1[k].store(0);
        g_e1f_ck_drew[k].store(0);
        g_e1f_ck_drew_g1[k].store(0);
    }
    for (auto* a : {&g_e1f_cov_max, &g_e1f_unc_max, &g_e1f_g1_cov_max, &g_e1f_g1_unc_max}) a->store(0);
    for (auto* a : {&g_e1f_cov_min, &g_e1f_neg_min}) a->store(~0ull);
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    g_rg_qpc0.store(q.QuadPart);
    g_rg_tsc0.store(__rdtsc());
}

void reset() {
    for (Count* c : {&g_r_opens[1], &g_r_opens[2], &g_r_open_failed, &g_r_closes, &g_r_close_failed, &g_r_gate_bad[1], &g_r_gate_bad[2], &g_r_unready,
                     &g_r_ctl, &g_r_ctl_failed, &g_dg_equal, &g_dg_differ, &g_dg_unread, &g_p_opens[1], &g_p_opens[2], &g_p_unclosed, &g_p_order,
                     &g_p_close_copy, &g_p_close_marker, &g_p_listed[1], &g_p_listed[2], &g_p_predicated, &g_p_refused, &g_p_seeds,
                     &g_p_seed_e2_array, &g_p_seed_followed, &g_p_twin, &g_p_ref[1], &g_p_ref[2], &g_p_no_twin, &g_p_no_cl1, &g_p_other_thread,
                     &g_f_good, &g_f_bad, &g_f_x_gate, &g_f_x_pred, &g_f_x_refused, &g_f_x_counts, &g_f_x_contig, &g_f_x_seed, &g_f_x_order,
                     &g_f_x_none, &g_f_first_same, &g_cap_started, &g_cap_done, &g_cap_incomplete, &g_pending_late, &g_c_frames, &g_c_pass,
                     &g_c_e2_exact, &g_c_e1_exact, &g_c_neg_differ, &g_c_react_zero, &g_c_cov_ok, &g_c_ctl, &g_c_ctl_e2_differ, &g_c_ctl_e1_exact,
                     &g_c_map_failed, &g_c_timeout, &g_c_layout})
        c->store(0);
    g_p_seed_read_state.store(0);
    for (auto& a : g_c_e2_max) a.store(0);
    for (auto& a : g_c_e1_max) a.store(0);
    for (auto* a : {&g_c_e2_worst, &g_c_cov_max, &g_c_react_max}) a->store(0);
    for (auto* a : {&g_c_cov_min, &g_c_neg_min, &g_c_ctl_e2_min}) a->store(~0ull);
    for (auto& a : g_c_where) a.store(0);
    for (Count* c : {&g_rw_frames, &g_rw_unread, &g_rw_ctl, &g_rw_src[0], &g_rw_src[1], &g_rw_src[2], &g_rw_eq[0], &g_rw_eq[1], &g_rw_eq[2],
                     &g_rw_ne[0], &g_rw_ne[1], &g_rw_ne[2], &g_rw_ne_src[0], &g_rw_ne_src[1], &g_rw_ne_src[2], &g_rw_unstable, &g_rw_fix_changed, &g_rw_fixed,
                     &g_rw_fix_failed})
        c->store(0);
    for (auto& a : g_rw_ulp) a.store(0);
    for (auto& a : g_rw_first) a.store(0);
    // (made, make_failed, rb_made, rb_failed, r_ctl_restore_failed: kept)
    for (Count* c : {&g_sl_frames, &g_sl_handed, &g_sl_refused, &g_sl_dropped, &g_sl_extra, &g_sl_short, &g_sl_merges, &g_sl_nomerge, &g_sl_barrier,
                     &g_sl_cmp, &g_sl_held, &g_sl_seed_far, &g_sl_good, &g_sl_ref_skipped, &g_sl_query_in_run, &g_sl_identity, &g_f_x_query,
                     &g_c_id_frames, &g_c_id_exact, &g_c_id_e1, &g_f_x_hand, &g_f_x_barrier, &g_f_x_extra, &g_c_sl_frames,
                     &g_c_sl_pass, &g_c_g1_exact, &g_c_sl_marked, &g_c_pass_frames, &g_c_fail_frames})
        c->store(0);
    for (Count& c : g_sl_hr) c.store(0);
    for (int i = 0; i < 3; ++i) {
        g_c_sc_frames[i].store(0);
        g_c_sc_case[i].store(0);
        g_c_sc_fail[i].store(0);
        g_c_sc_e1[i].store(0);
        g_c_sc_min[i].store(~0ull);
    }
    for (auto& a : g_c_g1_max) a.store(0);
    for (auto* a : {&g_c_g1_worst, &g_c_marked_max, &g_c_pass_tex, &g_c_fail_tex, &g_sl_drop_max, &g_sl_last_dropped}) a->store(0);
    for (auto* a : {&g_c_marked_min, &g_sl_drop_min}) a->store(~0ull);
    for (auto& a : g_c_g1_where) a.store(0);
    // (srv_allocs, srv_writes, pso_made, pso_refused: kept)
    for (Count* c : {&g_pd_held[1], &g_pd_held[2], &g_pd_unknown, &g_pd_slice, &g_pd_nulled, &g_pd_restored, &g_pd_dup_twin, &g_pd_dup_ref,
                     &g_pd_skipped, &g_pd_refonly, &g_pd_after_bad, &g_pd_unpred_set, &g_pd_seed_nulled, &g_p_seed_read, &g_fam_judged, &g_fam_within, &g_fam_rep, &g_refdepth_draws, &g_refdepth_writes, &g_gv_eq[0], &g_gv_eq[1], &g_gv_ne[0], &g_gv_ne[1],
                     &g_gv_walk_changed[1], &g_gv_walk_changed[2], &g_gv_unread, &g_gv_unpaired, &g_p1vp_set, &g_p1vp_back, &g_p1vp_kept, &g_p1vp_none, &g_p1vp_failed, &g_p1vp_equal, &g_uc_begins, &g_uc_ends, &g_uc_nounion, &g_uc_failed,
                     &g_uc_rewritten, &g_uc_2c0_set, &g_uc_2c0_odd, &g_uc_2c0_back, &g_uc_2c0_kept, &g_uc_marks, &g_uc_mark_failed, &g_uc_mark_lost,
                     &g_uc_over, &g_uc_unjudged, &g_ub_frames, &g_ub_noeye2, &g_ub_calls, &g_ub_notvp, &g_ub_odd, &g_ub_emu_ne, &g_ub_up0, &g_ub_up1,
                     &g_ub_e2_eq, &g_ub_e2_ne})
        c->store(0);
    for (int i = 0; i < 2; ++i)
        for (Count* c : {&g_uc_frames[i], &g_uc_subset[i], &g_uc_subset_nvb[i], &g_uc_miss[i], &g_uc_miss_nvb[i], &g_uc_listed1[i], &g_uc_listed2[i],
                         &g_uc_draws1[i], &g_uc_draws2[i]})
            c->store(0);
    g_uc_reset_req.store(true, std::memory_order_relaxed);  // (uc_begins / uc_ends zeroed again at the next begin)
    rg_reset();  // ([Stereo] MainFamily=region, item 8e)
}

// ---- [Stereo] MainSlice (run 25 item 5, research\run25\slice.md 2.2-2.4): the SRVs, the merge pipelines, the handover
// The merge's shaders (2.3), compiled once the four records exist: their bindless indices are the registers, in t-space 2 (the
// game's bindless Texture2D table, partition 1's: 0x140f7fc20 sets it to the partition's GPU base). Step 0 (run 25): this
// source compiled offline with the system's d3dcompiler_47 (vs_5_1 / ps_5_1) at t5266..t5269 and at t14996..t14999.
const char kMergeHlsl[] =
    "Texture2DArray<float4> Sc : register(t%u, space2);\n"  // S slice 1's FullScreenCopy
    "Texture2DArray<float4> Sv : register(t%u, space2);\n"  // S slice 1's velocity (.xy)
    "Texture2DArray<float>  Sd : register(t%u, space2);\n"  // S slice 1's depth plane
    "Texture2DArray<uint2>  Ss : register(t%u, space2);\n"  // S slice 1's stencil plane (.y)
    "float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0.5, 1); }\n"
    "struct O { float4 c : SV_Target0; float4 v : SV_Target1; float d : SV_Depth; };\n"
    "O ps(float4 p : SV_Position) {\n"
    "    int4 l = int4(p.xy, 0, 0);\n"
    "    if (Ss.Load(l).y == 0) discard;\n"
    "    O o;\n"
    "    o.c = Sc.Load(l);\n"
    "    o.v = Sv.Load(l);\n"
    "    o.d = Sd.Load(l);\n"
    "    return o;\n"
    "}\n";
// (the worker, under g_compile_mutex) the two blobs, once
bool compile_merge(char* why, size_t n) {
    if (g_vs && g_ps) return true;
    if (g_compile_tried) {
        std::snprintf(why, n, "the merge shaders did not compile (the log has the compiler's message)");
        return false;
    }
    g_compile_tried = true;
    if (!g_compile) {
        std::snprintf(why, n, "no D3DCompile");
        return false;
    }
    char src[sizeof(kMergeHlsl) + 64];
    const int len = std::snprintf(src, sizeof(src), kMergeHlsl, g_srv_idx[kSrvFsc].load(), g_srv_idx[kSrvVel].load(), g_srv_idx[kSrvDepth].load(),
                                  g_srv_idx[kSrvStencil].load());
    if (len <= 0 || len >= static_cast<int>(sizeof(src))) {
        std::snprintf(why, n, "the merge source not formatted");
        return false;
    }
    ID3DBlob* out[2] = {};
    const char* const entry[2] = {"vs", "ps"};
    const char* const target[2] = {"vs_5_1", "ps_5_1"};
    for (int i = 0; i < 2; ++i) {
        ID3DBlob* err = nullptr;
        const HRESULT hr = g_compile(src, static_cast<SIZE_T>(len), "rdrvr_main_slice", nullptr, nullptr, entry[i], target[i],
                                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out[i], &err);
        if (FAILED(hr) || !out[i]) {
            log::error("[mi] MainSlice: the merge's %s does not compile (%#lx): %s", entry[i], static_cast<unsigned long>(hr),
                       err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            if (err) err->Release();
            if (out[0]) out[0]->Release();
            if (out[1]) out[1]->Release();
            std::snprintf(why, n, "the merge's %s does not compile", entry[i]);
            return false;
        }
        if (err) err->Release();
    }
    g_vs = out[0];
    g_ps = out[1];
    log::info("[mi] MainSlice: the merge's shaders compiled (vs_5_1 %zu B, ps_5_1 %zu B; t%u t%u t%u t%u, space 2)", g_vs->GetBufferSize(), g_ps->GetBufferSize(),
              g_srv_idx[kSrvFsc].load(), g_srv_idx[kSrvVel].load(), g_srv_idx[kSrvDepth].load(), g_srv_idx[kSrvStencil].load());
    return true;
}
// the family pipeline's desc as the merge needs it (slice.md 2.1's per-pipeline checks): nullptr, or why that pipeline is not sliced
const char* desc_why(const pso::MainDesc& d) {
    const D3D12_DEPTH_STENCIL_DESC& s = d.ds;
    if (!d.root) return "the family pipeline has no root signature";
    if (d.stream_out) return "the family pipeline has stream output";
    if (!s.DepthEnable || (s.DepthFunc != D3D12_COMPARISON_FUNC_LESS_EQUAL && s.DepthFunc != D3D12_COMPARISON_FUNC_LESS) ||
        s.DepthWriteMask != D3D12_DEPTH_WRITE_MASK_ALL)
        return "the family's depth is not LESS_EQUAL (or LESS) with writes";
    if (!s.StencilEnable) return "the family's stencil is off";
    if (std::memcmp(&s.FrontFace, &s.BackFace, sizeof(s.FrontFace)) != 0) return "the family's stencil faces differ";
    if (s.FrontFace.StencilFunc != D3D12_COMPARISON_FUNC_ALWAYS || s.FrontFace.StencilPassOp != D3D12_STENCIL_OP_REPLACE ||
        (s.FrontFace.StencilDepthFailOp != D3D12_STENCIL_OP_KEEP && s.FrontFace.StencilDepthFailOp != D3D12_STENCIL_OP_REPLACE))
        return "the family's stencil is not ALWAYS / REPLACE / depth-fail KEEP or REPLACE (slice.md 1.2)";
    if (d.blend.IndependentBlendEnable || d.blend.AlphaToCoverageEnable || d.blend.RenderTarget[0].BlendEnable || d.blend.RenderTarget[0].LogicOpEnable)
        return "the family's blend is not off";
    if (d.topo != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE) return "the family's topology type is not triangles";
    if (d.sample.Count != 1) return "the family pipeline is multisampled";
    if (d.num_rt != 3 || d.dsv != DXGI_FORMAT_D24_UNORM_S8_UINT) return "the family pipeline has not 3 render targets and a D24S8 depth";
    return nullptr;
}
// (the worker) the two merge pipelines of one family pipeline
const char* build_merge(Merge& m) {
    pso::MainDesc d;
    if (!pso::main_desc(m.game, m.serial, &d)) return "the family pipeline's desc not kept (MainSlice not read at the device's creation, or re-made)";
    if (const char* why = desc_why(d)) return why;
    ID3D12Device* dev = state::device.load();
    if (!dev) return "no device";
    ID3DBlob *vs = nullptr, *ps = nullptr;
    {
        std::lock_guard lock(g_compile_mutex);
        if (!compile_merge(m.why, sizeof(m.why))) return m.why;
        vs = g_vs;
        ps = g_ps;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = d.root;  // (the family's own: the playback never sets another, the tables stay bound)
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.BlendState = d.blend;  // (verbatim: off, RT0's write mask for all three)
    pd.SampleMask = d.sample_mask;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;  // (no bias: SV_Depth carries the kept fragment's stored depth)
    pd.DepthStencilState = d.ds;  // (verbatim: the one test against pass 2's depth, the same stencil ops)
    pd.InputLayout = {nullptr, 0};
    pd.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = d.num_rt;
    for (int i = 0; i < 8; ++i) pd.RTVFormats[i] = d.rtv[i];
    pd.DSVFormat = d.dsv;
    pd.SampleDesc = d.sample;
    pd.NodeMask = d.node_mask;
    HRESULT h1 = E_FAIL, h2 = E_FAIL;
    {
        const pso::OwnCreation own;  // ([Stereo] InstancedShadersAuto leaves the mod's own pipelines alone)
        h1 = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m.merge));
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;  // (the control "slice always")
        h2 = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m.always));
    }
    if (FAILED(h1) || FAILED(h2) || !m.merge || !m.always) {
        if (m.merge) m.merge->Release();
        if (m.always) m.always->Release();
        m.merge = m.always = nullptr;
        std::snprintf(m.why, sizeof(m.why), "CreateGraphicsPipelineState refused the merge (%#lx, ALWAYS %#lx): the root signature's t-space 2 not "
                      "visible to the pixel shader, or another desc mismatch", static_cast<unsigned long>(h1), static_cast<unsigned long>(h2));
        return m.why;
    }
    m.merge->SetName(L"RDRVR main slice merge");
    m.always->SetName(L"RDRVR main slice merge (ALWAYS, the control)");
    return nullptr;
}
DWORD WINAPI merge_worker(void* p) {
    Merge& m = *static_cast<Merge*>(p);
    const char* why = build_merge(m);
    if (why) {
        if (why != m.why) std::snprintf(m.why, sizeof(m.why), "%s", why);
        bump(g_sl_pso_refused);
        log::warn("[mi] MainSlice: no merge pipeline for the family pipeline %p: %s (its draws are drawn by pass 2 as before)", static_cast<void*>(m.game), m.why);
        m.state.store(-1, std::memory_order_release);
    } else {
        bump(g_sl_pso_made);
        log::info("[mi] MainSlice: the merge pipelines made for the family pipeline %p (serial %u): its root signature, blend and depth-stencil "
                  "state", static_cast<void*>(m.game), m.serial);
        m.state.store(2, std::memory_order_release);
    }
    return 0;
}
// the merge of the listed pipeline `game` (its listing's serial): nullptr with *out when ready; else why (the worker started
// once for it, the SRVs' indices fixed first)
const char* merge_for(ID3D12PipelineState* game, uint32_t serial, Merge** out) {
    Merge* free_slot = nullptr;
    for (Merge& m : g_mg) {
        if (m.game == game && m.serial == serial) {
            const int s = m.state.load(std::memory_order_acquire);
            if (s == 2) {
                *out = &m;
                return nullptr;
            }
            return s == 1 ? "the merge pipeline not made yet (its worker)" : "the merge pipeline refused for this pipeline";
        }
        if (!m.game && !free_slot) free_slot = &m;
    }
    if (!free_slot) return "the merge table is full";
    for (bool h : g_srv_have)
        if (!h) return "the SRVs not allocated (the merge's registers)";
    free_slot->game = game;
    free_slot->serial = serial;
    free_slot->state.store(1, std::memory_order_relaxed);
    HANDLE th = CreateThread(nullptr, 0, &merge_worker, free_slot, 0, nullptr);
    if (!th) {
        std::snprintf(free_slot->why, sizeof(free_slot->why), "the merge's worker not started");
        free_slot->state.store(-1, std::memory_order_relaxed);
        return "the merge's worker not started";
    }
    CloseHandle(th);
    return "the merge pipeline not made yet (its worker started)";
}
bool typeless_or_unknown(DXGI_FORMAT f) {  // (a view format the SRV cannot take)
    switch (f) {
        case DXGI_FORMAT_UNKNOWN:
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
            return true;
        default:
            return false;
    }
}
// the four records (once) and their SRVs of S slice 1 (again after a re-make, past the hold); a fault or a bad record stands
// the slice down; a refusal (headroom) is tried again on the next slice frame
void srv_step() {
    if (g_sl_stood.load() || g_srv_gen == g_gen) return;
    if (g_srv_gen && state::presents.load() < g_remake_frame + kHold) {
        bump(g_sl_held);
        return;
    }
    for (int i = 0; i < kSrvs; ++i) {
        if (g_srv_have[i]) continue;
        char why[200];
        bool read = false;
        uint64_t used = 0, cap = 0;
        const eye2_images::Alloc a = eye2_images::alloc_p1(kSrvName[i], kSrvShort[i], &g_srv_rec[i], why, sizeof(why), &read, &used, &cap);
        if (a == eye2_images::Alloc::kRefused) {
            sl_refuse(why);
            return;
        }
        if (a != eye2_images::Alloc::kOk) {
            sl_stand_down(why);
            return;
        }
        g_srv_have[i] = true;
        g_srv_idx[i].store(static_cast<uint32_t>(g_srv_rec[i].index), std::memory_order_relaxed);
        bump(g_sl_srv_allocs);
        log::info("[mi] MainSlice: %s: partition 1 index %d (heap index %d, CPU %#llx); partition 1 used %llu of %llu", kSrvShort[i], g_srv_rec[i].index,
                  g_srv_rec[i].heap_index, static_cast<unsigned long long>(g_srv_rec[i].cpu), static_cast<unsigned long long>(used + 1),
                  static_cast<unsigned long long>(cap));
    }
    const Arr& fc = g_a[kS][kFsc];
    const Arr& ve = g_a[kS][kVel];
    const Arr& de = g_a[kS][kDepth];
    if (!fc.arr || !ve.arr || !de.arr || de.arr->GetDesc().Format != DXGI_FORMAT_R24G8_TYPELESS) return;  // (no slice-mode arrays)
    if (typeless_or_unknown(fc.vf) || typeless_or_unknown(ve.vf)) {
        sl_stand_down("the side colour or velocity has no typed view format");
        return;
    }
    for (int i = 0; i < kSrvs; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Texture2DArray.MostDetailedMip = 0;
        s.Texture2DArray.MipLevels = 1;
        s.Texture2DArray.FirstArraySlice = 1;  // (slice 1: eye 2's)
        s.Texture2DArray.ArraySize = 1;
        s.Texture2DArray.PlaneSlice = i == kSrvStencil ? 1u : 0u;
        s.Texture2DArray.ResourceMinLODClamp = 0.0f;
        ID3D12Resource* r = i == kSrvFsc ? fc.arr : i == kSrvVel ? ve.arr : de.arr;
        s.Format = i == kSrvFsc ? fc.vf : i == kSrvVel ? ve.vf : i == kSrvDepth ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        if (!eye2_images::create_srv_at(r, &s, g_srv_rec[i].cpu)) {
            sl_stand_down("a fault in CreateShaderResourceView (the slice's SRVs)");
            return;
        }
    }
    g_srv_gen = g_gen;
    bump(g_sl_srv_writes);
    log::limited("mi.srv", 6, "[mi] MainSlice: S slice 1's four SRVs written (arrays generation %llu; indices %u %u %u %u)",
                 static_cast<unsigned long long>(g_gen), g_srv_idx[0].load(), g_srv_idx[1].load(), g_srv_idx[2].load(), g_srv_idx[3].load());
}
// pass 2: the game's own pipeline (current) into R slice 1 with this draw's arguments, the game's bind put back
void ref_r1(ID3D12GraphicsCommandList* cl, const d3d::DrawArgs& args) {
    D3D12_CPU_DESCRIPTOR_HANDLE rt[3], ds{};
    for (int k = 0; k < kDepth; ++k) rt[k] = g_a[kR][k].view[1];
    ds = g_a[kR][kDepth].view[1];
    d3d::raw_bind(cl, 3, rt, FALSE, &ds);
    d3d::raw_draw(cl, args);
    bump(g_p_ref[2]);
    d3d::raw_bind(cl, t_bind.n, t_bind.rts, FALSE, &t_bind.ds);
}
// C: the game's four targets (bound, at RENDER_TARGET / ds: DEPTH_WRITE for MainSlice, the followed state for item 8f's C2; ds1 the
// stencil plane's when it differs, item 8f, then each plane's own barriers) copied into C (at rest COPY_DEST)
void take_c(ID3D12GraphicsCommandList* cl, ID3D12Resource* const res[kKinds], D3D12_RESOURCE_STATES ds = D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATES ds1 = static_cast<D3D12_RESOURCE_STATES>(-1)) {
    const D3D12_RESOURCE_STATES dsp[2] = {ds, ds1 == static_cast<D3D12_RESOURCE_STATES>(-1) ? ds : ds1};
    const bool per_plane = dsp[0] != dsp[1];
    Batch bt;
    for (int k = 0; k < kDepth; ++k) bt.add(res[k], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (per_plane)
        for (UINT p = 0; p < 2; ++p) bt.add_sub(res[kDepth], p, dsp[p], D3D12_RESOURCE_STATE_COPY_SOURCE);
    else
        bt.add(res[kDepth], ds, D3D12_RESOURCE_STATE_COPY_SOURCE);
    bt.flush(cl);
    for (int k = 0; k < kDepth; ++k) copy_sub(cl, g_c[k].arr, 0, res[k], 0);
    copy_sub(cl, g_c[kDepth].arr, 0, res[kDepth], 0);
    copy_sub(cl, g_c[kDepth].arr, 1, res[kDepth], 1);
    for (int k = 0; k < kDepth; ++k) bt.add(res[k], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (per_plane)
        for (UINT p = 0; p < 2; ++p) bt.add_sub(res[kDepth], p, D3D12_RESOURCE_STATE_COPY_SOURCE, dsp[p]);
    else
        bt.add(res[kDepth], D3D12_RESOURCE_STATE_COPY_SOURCE, ds);
    bt.flush(cl);
    g_f.c_taken = true;
}
// ---- ([Stereo] MainRegionE1Compare, item 8f) T and C2 (the playback thread, pass 2's region of an e1f frame)
// the game depth's planes' states at T and C2, from the walk cut's follow: both planes followed, each DEPTH_WRITE or read-only (DEPTH_READ
// and no write state; read-only only where the draw's pipeline does not write that plane: wr[0] depth, wr[1] stencil, both false at C2).
// They may differ (r116 G2, FXAA): out[p] is plane p's, and the barriers go per plane then
bool e1f_depth_state(ID3D12GraphicsCommandList* cl, int user, ID3D12Resource* d, const bool wr[2], D3D12_RESOURCE_STATES out[2]) {
    constexpr D3D12_RESOURCE_STATES kWrites = D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_RENDER_TARGET |
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_COPY_DEST |
                                              D3D12_RESOURCE_STATE_RESOLVE_DEST | D3D12_RESOURCE_STATE_STREAM_OUT;
    if (!d || prepass::kind_game(0) != d) return false;
    D3D12_RESOURCE_STATES st[2]{};
    for (unsigned p = 0; p < 2; ++p)
        if (!fs_followed(cl, p, &st[p], user)) return false;  // (r117 DG4: set on this list)
    for (unsigned p = 0; p < 2; ++p) {
        const bool read_only = !wr[p] && (st[p] & D3D12_RESOURCE_STATE_DEPTH_READ) && !(st[p] & kWrites);
        if (st[p] != D3D12_RESOURCE_STATE_DEPTH_WRITE && !read_only) return false;
    }
    if (st[0] != st[1]) {
        bump(g_e1f_planes_differ);
        g_e1f_pst[0].store(static_cast<uint32_t>(st[0]), std::memory_order_relaxed);
        g_e1f_pst[1].store(static_cast<uint32_t>(st[1]), std::memory_order_relaxed);
    }
    out[0] = st[0];
    out[1] = st[1];
    return true;
}
// a copy never under a predicate: the list's (its record known) nulled around it and put back (rg_seed_early's rule); false: the record
// unknown, nothing recorded
bool e1f_pred_off(ID3D12GraphicsCommandList* cl, d3d::Predicate* p, bool* nulled) {
    *nulled = false;
    if (!d3d::thread_predicated()) return true;
    if (d3d::thread_predicate(cl, p) != d3d::kPredSet || !p->res) return false;
    d3d::raw_set_predication(cl, nullptr, 0, 0);
    *nulled = true;
    return true;
}
void e1f_pred_back(ID3D12GraphicsCommandList* cl, const d3d::Predicate& p, bool nulled) {
    if (nulled) d3d::raw_set_predication(cl, p.res, p.off, p.op);
}
void e1f_no_t(int r, const char* why) {
    bump(g_e1f_tx[r]);
    std::snprintf(g_e1f_why, sizeof(g_e1f_why), "T: %s", why);
}
// T at pass 2's e1's first non-empty draw, before the game's (before the own-draw return): the game's four into R slice 0 (handover's T
// copy, slice 0), the colour bound (check_bind: RENDER_TARGET), the depth from the follow; R's slice 0 from its rest state and back. Not
// taken (counted by reason): the frame excluded at the close (kXE1fT). From T to C2 a barrier on the four is watched (barrier)
void e1f_take_t(ID3D12GraphicsCommandList* cl, const main_region::DrawInfo& di) {
    g_f.e1f_t_tried = true;
    if (!g_r.ok) return e1f_no_t(kEtNotOk, "the frame excluded already");
    Bound b;
    if (const char* why = check_bind(cl, &b)) return e1f_no_t(kEtBind, why);
    for (int k = 0; k < kKinds; ++k)
        if (g_a[kS][k].game != b.res[k] || !g_a[kR][k].arr) return e1f_no_t(kEtBind, "a bound target is not the arrays' game target");
    if (!g_f.seeded || g_f.gen != g_gen) return e1f_no_t(kEtSeed, "not seeded this frame (or the arrays re-made since)");
    D3D12_RESOURCE_STATES dst[2]{};
    const bool wr[2] = {(di.flags & pso::main_inst_flags::kDepthWrite) != 0, (di.flags & pso::main_inst_flags::kStencilWrite) != 0};
    if (!e1f_depth_state(cl, kFsT, b.res[kDepth], wr, dst))
        return e1f_no_t(kEtDepth, "the walk cut's follow has not both depth planes, each DEPTH_WRITE (or read-only), at e1's first draw");
    d3d::Predicate pr;
    bool nulled = false;
    if (!e1f_pred_off(cl, &pr, &nulled)) return e1f_no_t(kEtPred, "the list is predicated and its record unknown");
    fs_log("T", cl, dst, "the game's depth -> COPY_SOURCE and back from");
    const D3D12_RESOURCE_STATES kRt = D3D12_RESOURCE_STATE_RENDER_TARGET, kDw = D3D12_RESOURCE_STATE_DEPTH_WRITE, kCs = D3D12_RESOURCE_STATE_COPY_SOURCE,
                                kCd = D3D12_RESOURCE_STATE_COPY_DEST;
    ID3D12Resource* rd_ = g_a[kR][kDepth].arr;
    Batch bt;
    for (int k = 0; k < kDepth; ++k) {
        bt.add(b.res[k], kRt, kCs);
        bt.add_sub(g_a[kR][k].arr, 0, kRt, kCd);
    }
    for (UINT p = 0; p < 2; ++p) bt.add_sub(b.res[kDepth], p, dst[p], kCs);  // (each plane from its own followed state)
    bt.add_sub(rd_, 0, kDw, kCd);  // (slice 0's planes: subresources 0 and 2)
    bt.add_sub(rd_, 2, kDw, kCd);
    bt.flush(cl);
    for (int k = 0; k < kDepth; ++k) copy_sub(cl, g_a[kR][k].arr, 0, b.res[k], 0);
    copy_sub(cl, rd_, 0, b.res[kDepth], 0);
    copy_sub(cl, rd_, 2, b.res[kDepth], 1);
    for (int k = 0; k < kDepth; ++k) {
        bt.add(b.res[k], kCs, kRt);
        bt.add_sub(g_a[kR][k].arr, 0, kCd, kRt);
    }
    for (UINT p = 0; p < 2; ++p) bt.add_sub(b.res[kDepth], p, kCs, dst[p]);
    bt.add_sub(rd_, 0, kCd, kDw);
    bt.add_sub(rd_, 2, kCd, kDw);
    bt.flush(cl);
    e1f_pred_back(cl, pr, nulled);
    g_f.e1f_t = true;
    g_f.e1f_barrier = false;
    g_f.e1f_clear = false;
    g_f.e1f_write = false;
    g_f.e1f_split = false;
    for (int k = 0; k < kDepth; ++k) g_f.e1f_cmoved[k] = false;
    g_r.e1f_watch = true;
    bump(g_e1f_t);
}
void e1f_no_c2(int r, const char* why) {
    bump(g_e1f_cx[r]);
    std::snprintf(g_e1f_why, sizeof(g_e1f_why), "C2: %s", why);
}
// C2 at the first list call after pass 2's e1 end marker (poll): the game's four into C, on the list and thread of pass 2's last e1 draw,
// no bind since it (its bind generation), the four still bound, no barrier on them since T (the colour still RENDER_TARGET as T proved),
// the depth from the follow. Not taken (counted by reason): the frame excluded at the close (kXE1fC2)
void e1f_take_c2(ID3D12GraphicsCommandList* cl) {
    if (!g_r.open || g_r.pass != 2 || !g_f.e1f || g_f.state != 3 || GetCurrentThreadId() != g_r.tid || g_f.c_taken)
        return e1f_no_c2(kEcStale, "not inside an e1f frame's pass-2 region");
    if (!g_f.e1f_t) return e1f_no_c2(kEcNoT, "T not taken");
    if (cl != g_e1f_cl) return e1f_no_c2(kEcList, "not the list of pass 2's last e1 draw");
    if (t_bind.cl != cl || t_bind_gen != g_e1f_bgen) return e1f_no_c2(kEcBind, "a bind since pass 2's last e1 draw");
    if (g_f.e1f_clear) return e1f_no_c2(kEcClear, "a clear of the four targets since T");
    if (g_f.e1f_write) return e1f_no_c2(kEcWrite, "a copy into the four targets since T");
    if (g_f.e1f_split) return e1f_no_c2(kEcSplit, "a split barrier begun on the four targets since T");
    for (int k = 0; k < kDepth; ++k)  // (the colour is RENDER_TARGET at C2: as the last e1 draw left it, or transitioned back to it since)
        if (g_f.e1f_cmoved[k] && g_f.e1f_cst[k] != static_cast<uint32_t>(D3D12_RESOURCE_STATE_RENDER_TARGET))
            return e1f_no_c2(kEcColour, "a colour target transitioned out of RENDER_TARGET since pass 2's last e1 draw");
    if (g_f.e1f_barrier || !g_r.e1f_watch) return e1f_no_c2(kEcBarrier, "an aliasing barrier on the four, or a transition into a write state, since T");
    Bound b;
    if (check_bind(cl, &b)) return e1f_no_c2(kEcBind, "the four targets not bound");
    ID3D12Resource* res[kKinds];
    for (int k = 0; k < kKinds; ++k) {
        res[k] = b.res[k];
        if (g_a[kS][k].game != res[k] || !g_c[k].arr || g_c[k].game != res[k]) return e1f_no_c2(kEcC, "C not made for the four bound targets");
    }
    if (!g_f.seeded || g_f.gen != g_gen) return e1f_no_c2(kEcC, "the arrays re-made since the seed");
    D3D12_RESOURCE_STATES dst[2]{};
    const bool none[2] = {false, false};
    if (!e1f_depth_state(cl, kFsC2, res[kDepth], none, dst))
        return e1f_no_c2(kEcDepth, "the walk cut's follow has not both depth planes, each DEPTH_WRITE (or read-only), at e1's end");
    d3d::Predicate pr;
    bool nulled = false;
    if (!e1f_pred_off(cl, &pr, &nulled)) return e1f_no_c2(kEcPred, "the list is predicated and its record unknown");
    fs_log("C2", cl, dst, "the game's depth -> COPY_SOURCE and back from");
    take_c(cl, res, dst[0], dst[1]);
    e1f_pred_back(cl, pr, nulled);
    g_r.e1f_watch = false;
    bump(g_e1f_c2);
}
// ---- (item 8f's remap) the close: S0 -> the four
// (r116 G3's diagnosis) the centre texel of each colour target (RENDER_TARGET after the copy-back) to the readback, at most one in flight
void rm_poison_rb(ID3D12GraphicsCommandList* cl, ID3D12Resource* const res[kKinds]) {
    int free_ = 0;
    if (!g_pz_state.compare_exchange_strong(free_, 3)) return;  // (3: being recorded)
    ID3D12Device* dev = state::device.load();
    if (g_pz_rb && g_pz_dev != dev) {  // (another device: never used with it)
        g_pz_state.store(0);
        return;
    }
    if (!g_pz_rb && dev) {
        D3D12_HEAP_PROPERTIES rh{};
        rh.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 512 * kDepth;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_pz_rb)))) g_pz_rb = nullptr;
        else g_pz_rb->SetName(L"RDRVR remap poison check readback (test only)");
        g_pz_dev = dev;
    }
    if (!g_pz_rb) {
        g_pz_state.store(0);
        return;
    }
    Batch bt;
    for (int k = 0; k < kDepth; ++k) bt.add(res[k], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    bt.flush(cl);
    for (int k = 0; k < kDepth; ++k) {
        const D3D12_RESOURCE_DESC d = res[k]->GetDesc();
        const UINT x = static_cast<UINT>(d.Width / 2), y = d.Height / 2;
        D3D12_TEXTURE_COPY_LOCATION dst{g_pz_rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        dst.PlacedFootprint.Offset = 512ull * static_cast<UINT64>(k);
        dst.PlacedFootprint.Footprint = {d.Format, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
        D3D12_TEXTURE_COPY_LOCATION src{res[k], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        src.SubresourceIndex = 0;
        const D3D12_BOX box{x, y, 0, x + 1, y + 1, 1};
        d3d::raw_copy_region(cl, &dst, &src, &box);
    }
    for (int k = 0; k < kDepth; ++k) bt.add(res[k], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    bt.flush(cl);
    g_pz_frame.store(state::presents.load());
    g_pz_state.store(1, std::memory_order_release);
}
std::string rm_poison_text() {  // (the command thread, kv) the readback 8 presents after its frame: per target "k:" its first 8 bytes in hex
    if (g_pz_state.load(std::memory_order_acquire) == 1 && state::presents.load() >= g_pz_frame.load() + kHold && g_pz_rb) {
        void* map = nullptr;
        const D3D12_RANGE rr{0, 512 * kDepth};
        if (SUCCEEDED(g_pz_rb->Map(0, &rr, &map)) && map) {
            std::string t;
            for (int k = 0; k < kDepth; ++k) {
                char b[48];
                const uint8_t* q = static_cast<const uint8_t*>(map) + 512 * k;
                std::snprintf(b, sizeof(b), "%s%d:%02x%02x%02x%02x%02x%02x%02x%02x", k ? "," : "", k, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
                t += b;
            }
            const D3D12_RANGE none{0, 0};
            g_pz_rb->Unmap(0, &none);
            std::lock_guard lock(g_pz_mutex);
            g_pz_text = t;
        }
        g_pz_state.store(0, std::memory_order_release);
    }
    std::lock_guard lock(g_pz_mutex);
    return g_pz_text;
}
void rm_lost(int reason, const char* why) {
    g_rm_on = g_rm_owed = g_rm_dup = false;
    bump(g_rm_lost[reason]);
    g_f.why |= kXE1fRemap;
    std::snprintf(g_e1f_why, sizeof(g_e1f_why), "remap close not made (%s): %s", kRmName[reason], why);
    log::limited("mi.rmlost", 12, "[mi] e1f remap: the close not made (%s): %s (eye 1 lacks this frame's skipped e1 draws)", kRmName[reason], why);
}
// on the remap's list (still open: its Close closes first) and thread, before the event that ends it (b: a barrier batch about to run, not
// yet followed). The four from their states now: the colour RENDER_TARGET (no barrier took it out while S0 stood in; the batch's own
// transitions of it must start there), the depth's planes from the walk cut's follow (the batch's must start there); S's slice 0 from its
// rest state and back; the remap list's predicate (its record known) nulled around it. False: not made (rm_lost)
bool rm_end(ID3D12GraphicsCommandList* now, int reason, unsigned n, const D3D12_RESOURCE_BARRIER* b) {
    if (!g_rm_on && !g_rm_owed) return true;
    ID3D12GraphicsCommandList* cl = g_rm_cl;
    const char* why = nullptr;
    if (!cl || GetCurrentThreadId() != g_rm_tid) why = "not the remap's thread";
    if (!why && (!g_f.seeded || g_f.gen != g_gen)) why = "the arrays re-made since the seed";
    ID3D12Resource* res[kKinds] = {};
    for (int k = 0; k < kKinds && !why; ++k) {
        res[k] = g_a[kS][k].game;
        if (!res[k] || !g_a[kS][k].arr) why = "the arrays not made";
    }
    D3D12_RESOURCE_STATES ds[2]{};
    // (a read-only close leaves the depth alone: its states not needed)
    if (!why && !g_rm_ro && prepass::kind_game(0) != res[kDepth]) why = "the walk cut's follow is not the four's depth";
    for (unsigned p = 0; p < 2 && !why && !g_rm_ro; ++p)
        if (!fs_followed(cl, p, &ds[p], kFsRemap)) why = "the depth's planes not followed on the remap's list";
    for (unsigned i = 0; i < n && b && !why; ++i) {  // (the batch about to run starts where this close leaves the four)
        if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
        const D3D12_RESOURCE_TRANSITION_BARRIER& t = b[i].Transition;
        for (int k = 0; k < kKinds && !why; ++k) {
            if (t.pResource != res[k] || (k == kDepth && g_rm_ro)) continue;
            if (k != kDepth) {
                if (t.StateBefore != D3D12_RESOURCE_STATE_RENDER_TARGET) why = "a colour target's transition does not start at RENDER_TARGET";
            } else if (t.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
                if (t.StateBefore != ds[0] || t.StateBefore != ds[1]) why = "the depth's transition does not start at its followed states";
            } else if (t.Subresource > 1 || t.StateBefore != ds[t.Subresource]) {
                why = "the depth plane's transition does not start at its followed state";
            }
        }
    }
    d3d::Predicate pr;
    bool nulled = false;
    if (!why) {
        int st = d3d::thread_predicate(cl, &pr);
        if (st == d3d::kPredUnknown) {  // (the thread's record is another list's now: the remap's own note of its list)
            pr = g_rm_pred;
            st = pr.res ? d3d::kPredSet : d3d::kPredNone;
        } else if ((st == d3d::kPredSet) != (g_rm_pred.res != nullptr) || (st == d3d::kPredSet && (pr.res != g_rm_pred.res || pr.off != g_rm_pred.off ||
                                                                                                   pr.op != g_rm_pred.op))) {
            why = "the remap list's predicate record and the remap's note disagree";
        }
        if (!why && st == d3d::kPredSet && pr.res) {
            d3d::raw_set_predication(cl, nullptr, 0, 0);
            nulled = true;
        }
    }
    if (why) {
        rm_lost(reason, why);
        return false;
    }
    g_rm_on = g_rm_owed = g_rm_dup = false;
    // (r116 G3) a read-only open: the depth stayed read-only (a write state would have closed the remap first), so neither e1 nor the twins
    // wrote it: the game's depth equals S0's and is left alone (no transition of a depth shaders may be reading)
    const bool depth = !g_rm_ro;
    if (!depth) bump(g_rm_ro_closes);
    fs_log("remap close", cl, ds, depth ? "the game's depth -> COPY_DEST and back from" : "the depth left alone (read-only), followed");
    const D3D12_RESOURCE_STATES kRt = D3D12_RESOURCE_STATE_RENDER_TARGET, kDw = D3D12_RESOURCE_STATE_DEPTH_WRITE, kCs = D3D12_RESOURCE_STATE_COPY_SOURCE,
                                kCd = D3D12_RESOURCE_STATE_COPY_DEST;
    ID3D12Resource* sd = depth ? g_a[kS][kDepth].arr : nullptr;
    if (g_f.ck == kCkRmPoison) {  // (the rmpoison control: S0's colour (at rest RENDER_TARGET) cleared before the copy-back, the predicate nulled)
        const float poison[4] = {1.0f, 0.0f, 1.0f, 1.0f};
        for (int k = 0; k < kDepth; ++k) d3d::raw_clear_rtv(cl, &g_a[kS][k].view[0], poison);
        bump(g_rm_poisoned);
    }
    Batch bt;
    for (int k = 0; k < kDepth; ++k) {
        bt.add_sub(g_a[kS][k].arr, 0, kRt, kCs);
        bt.add(res[k], kRt, kCd);
    }
    if (depth) {
        bt.add_sub(sd, 0, kDw, kCs);  // (slice 0's planes: subresources 0 and 2)
        bt.add_sub(sd, 2, kDw, kCs);
        for (UINT p = 0; p < 2; ++p) bt.add_sub(res[kDepth], p, ds[p], kCd);
    }
    bt.flush(cl);
    for (int k = 0; k < kDepth; ++k) copy_sub(cl, res[k], 0, g_a[kS][k].arr, 0);
    if (depth) {
        copy_sub(cl, res[kDepth], 0, sd, 0);
        copy_sub(cl, res[kDepth], 1, sd, 2);
    }
    for (int k = 0; k < kDepth; ++k) {
        bt.add_sub(g_a[kS][k].arr, 0, kCs, kRt);
        bt.add(res[k], kCd, kRt);
    }
    if (depth) {
        bt.add_sub(sd, 0, kCs, kDw);
        bt.add_sub(sd, 2, kCs, kDw);
        for (UINT p = 0; p < 2; ++p) bt.add_sub(res[kDepth], p, kCd, ds[p]);
    }
    bt.flush(cl);
    if (g_f.ck == kCkRmPoison) rm_poison_rb(cl, res);  // (r116 G3's diagnosis: the copy-back landed? the predicate still nulled)
    if (nulled) d3d::raw_set_predication(cl, pr.res, pr.off, pr.op);
    bump(g_rm_closed[reason]);
    return true;
}
// pass 2's first listed draw of a slice frame (the drop point): decided before anything is dropped (slice.md 2.4); kDrop: merged
// (or, "nomerge", not), the draw to be dropped; kNone: refused, the game draws its own; kDuplicate: the identity control (T
// taken, the game draws its own, each also into R slice 1, C after the last)
int handover(ID3D12GraphicsCommandList* cl, const Bound& b, const d3d::DrawArgs& args, uint32_t flags, uint32_t serial) {
    const char* why = nullptr;
    int hr = kHrEarly;
    Merge* m = nullptr;
    if (g_sl_stood.load()) {
        why = "stood down";
        hr = kHrStood;
    } else if (g_f.listed1 <= 0 || !g_f.contig1 || g_f.twins1 != g_f.listed1 || g_f.mixed1 || !(flags & pso::main_inst_flags::kDepthWrite) ||
               !(flags & pso::main_inst_flags::kStencilWrite)) {
        why = "pass 1's region not whole (a twin for each listed draw, one run, one pipeline), or the pipeline writes no depth or stencil";
        hr = kHrWhole;
    } else if (g_srv_gen != g_gen) {
        const bool held = g_srv_gen && state::presents.load() < g_remake_frame + kHold;
        why = held ? "held: 8 presents after an array re-make" : "the SRVs not written for these arrays";
        hr = held ? kHrHeld : kHrSrv;
    } else if (t_pso != g_f.pso1) {  // (the review: the merge carries pass 1's pipeline's state; S1 is its twin's; checked first)
        why = "pass 2's first listed pipeline is not pass 1's";
        hr = kHrPso;
    } else if ((why = merge_for(t_pso, serial, &m)) != nullptr) {
        hr = kHrMerge;
    } else if (const int q = d3d::thread_open_queries(cl); q != 0) {  // (the review: a dropped draw would change the game's query unseen)
        why = q < 0 ? "BeginQuery not hooked (the open queries unknown)" : "a query that counts draws is open on the list at the drop point";
        hr = kHrQuery;
    } else if (d3d::thread_predicated()) {  // (checked by the caller already: never touched)
        why = "predicated";
    }
    for (int k = 0; !why && k < kKinds; ++k)
        if (!g_a[kS][k].arr || !g_a[kR][k].arr) why = "the arrays not made";
    if (why) {
        bump(g_sl_hr[hr]);
        g_f.hr_counted = true;
        g_f.why |= kXHand;
        g_r.ok = false;
        sl_refuse(why);
        return kNone;
    }
    const int mode = g_f.slice;
    const bool cmp = g_cap.load() == kArmed && g_c[0].arr && g_c[kDepth].arr;
    const D3D12_RESOURCE_STATES kRt = D3D12_RESOURCE_STATE_RENDER_TARGET, kDw = D3D12_RESOURCE_STATE_DEPTH_WRITE, kCs = D3D12_RESOURCE_STATE_COPY_SOURCE,
                                kCd = D3D12_RESOURCE_STATE_COPY_DEST, kPs = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                kDr = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* rd_ = g_a[kR][kDepth].arr;
    ID3D12Resource* sd = g_a[kS][kDepth].arr;
    Batch bt;
    if (cmp) {  // T (the game's targets at the drop point) into R slice 1: the two-pass reference's start
        for (int k = 0; k < kDepth; ++k) {
            bt.add(b.res[k], kRt, kCs);
            bt.add_sub(g_a[kR][k].arr, 1, kRt, kCd);
        }
        bt.add(b.res[kDepth], kDw, kCs);
        bt.add_sub(rd_, 1, kDw, kCd);
        bt.add_sub(rd_, 3, kDw, kCd);
        bt.flush(cl);
        for (int k = 0; k < kDepth; ++k) copy_sub(cl, g_a[kR][k].arr, 1, b.res[k], 0);
        copy_sub(cl, rd_, 1, b.res[kDepth], 0);
        copy_sub(cl, rd_, 3, b.res[kDepth], 1);
        for (int k = 0; k < kDepth; ++k) {
            bt.add(b.res[k], kCs, kRt);
            bt.add_sub(g_a[kR][k].arr, 1, kCd, kRt);
        }
        bt.add(b.res[kDepth], kCs, kDw);
        bt.add_sub(rd_, 1, kCd, kDw);
        bt.add_sub(rd_, 3, kCd, kDw);
    }
    if (mode == 4) {  // (the identity control: nothing merged or dropped; the references and C as after_draw goes)
        bt.flush(cl);
        g_r.ident = true;
        g_r.cmp = g_f.cmp = cmp;
        bump(g_sl_identity);
        return kDuplicate;
    }
    // S slice 1 (the twin's eye 2, seeded far) to the merge's shader-resource states
    bt.add_sub(g_a[kS][kFsc].arr, 1, kRt, kPs);
    bt.add_sub(g_a[kS][kVel].arr, 1, kRt, kPs);
    bt.add_sub(sd, 1, kDw, kDr);
    bt.add_sub(sd, 3, kDw, kDr);
    bt.flush(cl);
    if (mode == 2) {  // (the control "nomerge": dropped without merging)
        bump(g_sl_nomerge);
    } else {  // the merge: one covering triangle (a list or a strip alike), only the pipeline changed and put back
        pso::set_raw(cl, mode == 3 ? m->always : m->merge);
        d3d::raw_draw(cl, d3d::DrawArgs{false, 3, 1, 0, 0, 0});
        pso::set_raw(cl, t_pso);
        bump(g_sl_merges);
    }
    bt.add_sub(g_a[kS][kFsc].arr, 1, kPs, kRt);
    bt.add_sub(g_a[kS][kVel].arr, 1, kPs, kRt);
    bt.add_sub(sd, 1, kDr, kDw);
    bt.add_sub(sd, 3, kDr, kDw);
    bt.flush(cl);
    if (cmp) {
        take_c(cl, b.res);  // the game's targets right after the merge
        ref_r1(cl, args);   // (the game's own first draw into R slice 1)
        bump(g_sl_cmp);
    }
    g_r.handed = g_f.handed = true;
    g_r.cmp = g_f.cmp = cmp;
    g_r.in_run = g_f.listed1 > 1;
    g_r.dropped = 1;
    bump(g_sl_handed);
    bump(g_sl_dropped);
    return kDrop;
}
// pass 2's listed draws 2.. of a handed-over frame: dropped up to listed1 ([compare] the reference into R slice 1 first);
// past it drawn by the game (after the merge: the order kept)
int dropped(ID3D12GraphicsCommandList* cl, const d3d::DrawArgs& args) {
    if (g_r.listed > g_f.listed1) {
        bump(g_sl_extra);
        g_f.why |= kXExtra;
        g_r.in_run = false;
        return kNone;
    }
    if (d3d::thread_predicated()) {  // (none expected: the frame excluded, the predicate untouched; still dropped, S1 has it)
        bump(g_p_predicated);
        g_f.why |= kXPred;
    }
    if (g_r.mixed) g_f.why |= kXRefused;  // (another family pipeline than pass 1's one: S1 has its fragments, the merge had the first's state)
    if (g_r.cmp) {
        Bound b;
        bool same = check_bind(cl, &b) == nullptr;
        for (int k = 0; same && k < kKinds; ++k) same = g_a[kS][k].game == b.res[k];
        if (same) {
            ref_r1(cl, args);
        } else {  // (the targets changed inside the run: no reference, the frame excluded)
            bump(g_sl_ref_skipped);
            g_f.why |= kXRefused;
        }
    }
    if (g_r.listed == g_f.listed1) g_r.in_run = false;
    ++g_r.dropped;
    bump(g_sl_dropped);
    return kDrop;
}
int slice_latch() {  // (the render thread, pass 1's open) the mode this frame runs, 0 when not installed, stood down or off
    if (!g_sl_installed.load(std::memory_order_relaxed) || g_sl_stood.load(std::memory_order_relaxed)) return 0;
    const int m = g_sl_mode.load(std::memory_order_relaxed);
    return m >= 1 && m <= 4 ? m : 0;
}


// ---- ([Stereo] MainUnionCull, item 6e) the render thread's swap around DVL call 1 in pass 1
bool wr_raw(uintptr_t a, const void* src, size_t n) {
    if (!a) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(a), src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// out = w x p (rows of 4), as SetWorldMtx 0x1348e0 makes +0x2c0 from +0x80 and +0x3f0: row i = ((w_i0 p0 + w_i1 p1) + w_i2 p2) + w_i3 p3
void mul4(const float* w, const float* p, float* out) {
    const __m128 p0 = _mm_loadu_ps(p), p1 = _mm_loadu_ps(p + 4), p2 = _mm_loadu_ps(p + 8), p3 = _mm_loadu_ps(p + 12);
    for (int i = 0; i < 4; ++i) {
        __m128 r = _mm_mul_ps(_mm_set1_ps(w[i * 4]), p0);
        r = _mm_add_ps(r, _mm_mul_ps(_mm_set1_ps(w[i * 4 + 1]), p1));
        r = _mm_add_ps(r, _mm_mul_ps(_mm_set1_ps(w[i * 4 + 2]), p2));
        r = _mm_add_ps(r, _mm_mul_ps(_mm_set1_ps(w[i * 4 + 3]), p3));
        _mm_storeu_ps(out + i * 4, r);
    }
}
void uc_end() {  // DVL call 1's return in pass 1 (or a swap left over): the eye's planes back where the union's still are
    t_ub = false;  // ([Stereo] MainUnionBox: the game's own answers from here)
    if (!g_uc_vp) return;
    const uintptr_t vp = g_uc_vp;
    g_uc_vp = 0;
    alignas(16) float cur[2][16];
    bool left[2] = {false, false};
    for (int b = 0; b < 2; ++b) {
        const uintptr_t at = vp + (b ? 0x3f0 : 0x390);
        if (!rd_raw(at, cur[b], 64)) {
            bump(g_uc_failed);
            return;
        }
        if (std::memcmp(cur[b], g_uc_union[b], 64) != 0) {  // (the game wrote that block inside the walk: its value kept)
            left[b] = true;
            continue;
        }
        if (!wr_raw(at, g_uc_saved[b], 64)) bump(g_uc_failed);
    }
    if (left[0] || left[1]) bump(g_uc_rewritten);
    // +0x2c0: made again with the eye's matrix when it holds World x the union's (the last SetWorldMtx inside the walk, or uc_begin's)
    // and +0x3f0 was put back (the 6d/6e review's should-fix 5: +0x390 alone rewritten by the game no longer leaves it World x union)
    alignas(16) float world[16], pl[16], u2[16], e2[16];
    if (!left[1] && rd_raw(vp + 0x80, world, 64) && rd_raw(vp + 0x2c0, pl, 64)) {
        mul4(world, g_uc_union[1], u2);
        if (!std::memcmp(u2, pl, 64)) {
            mul4(world, g_uc_saved[1], e2);
            if (wr_raw(vp + 0x2c0, e2, 64)) bump(g_uc_2c0_back);
            else bump(g_uc_failed);
        } else {
            bump(g_uc_2c0_kept);
        }
    }
    bump(g_uc_ends);
}
bool uc_begin() {  // pass 1's DVL call 1, after the open marker: the current viewport's side planes the union viewport's
    uc_end();  // (none expected: dvl_close ends each)
    if (g_uc_reset_req.exchange(false, std::memory_order_relaxed)) {
        g_uc_begins.store(0);
        g_uc_ends.store(0);
    }
    uintptr_t vp = 0;
    const uintptr_t u = reinterpret_cast<uintptr_t>(camera_lever::shadow_union_vp());
    if (!anchors::present(Id::ViewportCurrent) || !rd(anchors::addr(Id::ViewportCurrent), &vp) || !vp) {
        bump(g_uc_failed);
        return false;
    }
    if (!u || u == vp) {
        bump(g_uc_nounion);
        return false;
    }
    alignas(16) float sv[2][16], un[2][16], world[16], pl[16], chk[16];
    if (!rd_raw(vp + 0x390, sv[0], 64) || !rd_raw(vp + 0x3f0, sv[1], 64) || !rd_raw(u + 0x390, un[0], 64) || !rd_raw(u + 0x3f0, un[1], 64) ||
        !rd_raw(vp + 0x80, world, 64) || !rd_raw(vp + 0x2c0, pl, 64)) {
        bump(g_uc_failed);
        return false;
    }
    if (!std::memcmp(sv, un, sizeof(sv))) {  // (the planes already the union's: PrepassUnionCull's swap not ended? never saved as the eye's)
        bump(g_uc_nounion);
        return false;
    }
    mul4(world, sv[1], chk);
    const bool consistent = !std::memcmp(chk, pl, 64);  // (+0x2c0 = World x the eye's matrix, as SetWorldMtx left it)
    if (!wr_raw(vp + 0x390, un[0], 64) || !wr_raw(vp + 0x3f0, un[1], 64)) {
        wr_raw(vp + 0x390, sv[0], 64);
        wr_raw(vp + 0x3f0, sv[1], 64);
        bump(g_uc_failed);
        return false;
    }
    std::memcpy(g_uc_saved, sv, sizeof(sv));
    std::memcpy(g_uc_union, un, sizeof(un));
    g_uc_vp = vp;
    if (consistent) {
        mul4(world, un[1], chk);
        if (wr_raw(vp + 0x2c0, chk, 64)) bump(g_uc_2c0_set);
    } else {
        bump(g_uc_2c0_odd);  // (left as the game had it: the walk's first SetWorldMtx makes it again)
    }
    float w = 0.0f;
    for (int p = 0; p < 4; ++p) {
        float d = 0.0f;
        for (int k = 0; k < 3; ++k) d += un[0][p * 4 + k] * sv[0][p * 4 + k];
        d = d > 1.0f ? 1.0f : d < -1.0f ? -1.0f : d;
        const float a = std::acos(d) * 57.29578f;
        if (a > w) w = a;
    }
    g_uc_widen_mdeg.store(static_cast<uint32_t>(w * 1000.0f), std::memory_order_relaxed);
    if (g_ub_installed.load(std::memory_order_relaxed) && g_ub_on.load(std::memory_order_relaxed)) {  // ([Stereo] MainUnionBox)
        const uintptr_t e2 = reinterpret_cast<uintptr_t>(camera_lever::eye2_viewport());
        if (e2 && rd_raw(e2 + 0x3f0, g_ub_side[2], 64)) {
            std::memcpy(g_ub_side[0], un[1], 64);
            std::memcpy(g_ub_side[1], sv[1], 64);
            g_ub_cached = false;
            g_ub_e2_pending = true;
            t_ub = true;
            bump(g_ub_frames);
        } else {
            bump(g_ub_noeye2);  // (no eye-2 viewport built this frame: the game's answers)
        }
    }
    bump(g_uc_begins);
    return true;
}
// ([Stereo] MainUnionBox) 0x142b00's test on the planes pl (SoA rows x, y, z, d), its operations in its order: per plane the box's
// nearest corner along the normal (p: the min where a coefficient is negative, else the max) and its farthest (n), each
// ((z cz + d) + y cy) + x cx; 2 when every n is not negative, plus 1 when every p is not
int box_test(const float* box, const float* pl) {
    const __m128 mn = _mm_loadu_ps(box), mx = _mm_loadu_ps(box + 4), zero = _mm_setzero_ps();
    const __m128 cx = _mm_loadu_ps(pl), cy = _mm_loadu_ps(pl + 4), cz = _mm_loadu_ps(pl + 8), cd = _mm_loadu_ps(pl + 12);
    const __m128 mnx = _mm_shuffle_ps(mn, mn, 0x00), mny = _mm_shuffle_ps(mn, mn, 0x55), mnz = _mm_shuffle_ps(mn, mn, 0xaa);
    const __m128 mxx = _mm_shuffle_ps(mx, mx, 0x00), mxy = _mm_shuffle_ps(mx, mx, 0x55), mxz = _mm_shuffle_ps(mx, mx, 0xaa);
    const __m128 kx = _mm_cmplt_ps(cx, zero), ky = _mm_cmplt_ps(cy, zero), kz = _mm_cmplt_ps(cz, zero);
    const auto pick = [](__m128 k, __m128 a, __m128 b) { return _mm_or_ps(_mm_and_ps(k, a), _mm_andnot_ps(k, b)); };  // k ? a : b
    __m128 p = _mm_add_ps(_mm_mul_ps(pick(kz, mnz, mxz), cz), cd);
    p = _mm_add_ps(p, _mm_mul_ps(pick(ky, mny, mxy), cy));
    p = _mm_add_ps(p, _mm_mul_ps(pick(kx, mnx, mxx), cx));
    __m128 n = _mm_add_ps(_mm_mul_ps(pick(kz, mxz, mnz), cz), cd);
    n = _mm_add_ps(n, _mm_mul_ps(pick(ky, mxy, mny), cy));
    n = _mm_add_ps(n, _mm_mul_ps(pick(kx, mxx, mnx), cx));
    return (_mm_movemask_ps(_mm_cmplt_ps(n, zero)) == 0 ? 1 : 0) + (_mm_movemask_ps(_mm_cmplt_ps(p, zero)) == 0 ? 1 : 0);
}
int hk_box(const float* box) {
    const int r = o_box(box);
    if (!t_ub || r == 2) return r;  // (2: inside for the union's planes already)
    bump(g_ub_calls);
    // (the render thread inside its own swap: the game's culler has just read these same fields)
    const uintptr_t vp = *reinterpret_cast<const uintptr_t*>(anchors::addr(Id::ViewportCurrent));
    if (!vp || vp != g_uc_vp) {  // (another viewport current inside the walk: its planes are not the swap's)
        bump(g_ub_notvp);
        return r;
    }
    const float* world = reinterpret_cast<const float*>(vp + 0x80);
    if (!g_ub_cached || std::memcmp(world, g_ub_world, 64) != 0) {
        std::memcpy(g_ub_world, world, 64);
        for (int k = 0; k < 3; ++k) mul4(g_ub_world, g_ub_side[k], g_ub_pl[k]);
        g_ub_cached = true;
    }
    const float* cur = reinterpret_cast<const float*>(vp + 0x2c0);
    if (std::memcmp(cur, g_ub_pl[0], 64) != 0) {  // (+0x2c0 not World x the union's: made from another matrix)
        bump(g_ub_odd);
        return r;
    }
    if (box_test(box, cur) != r) {  // (the copy is not the game's test: its answer only)
        bump(g_ub_emu_ne);
        return r;
    }
    int m = r;
    for (int k = 1; k < 3; ++k) {
        const int e = box_test(box, g_ub_pl[k]);
        if (e > m) m = e;
    }
    if (m > r) bump(r == 0 ? g_ub_up0 : g_ub_up1);
    return m;
}
bool ub_install() {  // (after MainUnionCull's) the culler's code and its pointer checked against the analysed build, then the detour
    const uintptr_t base = anchors::base(), f = base + kRvaBoxCull;
    // mov r11, rsp; sub rsp, 0xa8; mov rax, [rip + disp32] (the current viewport)
    static const uint8_t kHead[13] = {0x4c, 0x8b, 0xdc, 0x48, 0x81, 0xec, 0xa8, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x05};
    uint8_t head[13] = {};
    int32_t disp = 0;
    uintptr_t slot = 0;
    DWORD64 image = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(static_cast<DWORD64>(f), &image, nullptr);
    const char* why = nullptr;
    if (!base || !anchors::present(Id::ViewportCurrent)) why = "no module base or ViewportCurrent anchor";
    else if (!fn || !image || image + fn->BeginAddress != f) why = "0x142b00 is not a .pdata function start";
    else if (!rd_raw(f, head, sizeof(head)) || std::memcmp(head, kHead, sizeof(head)) != 0) why = "0x142b00's code is not the analysed culler's";
    else if (!rd(f + 13, &disp) || f + 17 + static_cast<intptr_t>(disp) != anchors::addr(Id::ViewportCurrent))
        why = "0x142b00 does not read the current viewport";
    else if (!rd(base + kRvaBoxCullPtr, &slot) || slot != f) why = "the culler's pointer 0x142272378 does not hold 0x142b00";
    if (why) {
        log::warn("[mi] MainUnionBox: %s: off", why);
        return false;
    }
    if (!hooks::install("RDR grmModel's box culler (FUN_140142b00, MainUnionBox)", reinterpret_cast<void*>(f), &hk_box, &o_box)) {
        log::warn("[mi] MainUnionBox: the culler's hook not installed: off");
        return false;
    }
    return true;
}

}  // namespace

uintptr_t dvl_ret() { return g_ret; }

// ---- the render thread
void dvl_open(int pass) {
    if (pass != 1 && pass != 2) return;
    if (pass == 1) d3d::desc_phase(true);  // ([Stereo] DescCensus, item 8e: its writes within the passes from here)
    if (!g_on.load(std::memory_order_relaxed) || g_stood.load(std::memory_order_relaxed)) return;
    // the frame's gate: the cascades (gCameraMatrix, PixelSize0, the tree-shadow map) the same in both passes
    // (billboard.md 8.3): pass 1 fitted to the union; pass 2 skipped (Split_shadows) or the union's again (Split_shadowunion)
    const int act = dual_pass::cascades_act();
    bool ok = pass == 1 ? act == 2 : (act == 3 || act == 2);
    if (!ok) bump(g_r_gate_bad[pass]);
    bool ctl = false;
    if (pass == 1 && ok && g_ppfix.load(std::memory_order_relaxed)) {  // (the experiment: before the diagnostic reads them)
        float pp[16], vi[16], wv[16];
        int src = 0;
        if (prepass::main_rows(1, pp, vi, wv, &src) && src == 2) {
            const int r = prepass::eye2_pp_refresh();
            bump(r >= 0 ? g_rw_fixed : g_rw_fix_failed);
            if (r == 1) bump(g_rw_fix_changed);
        }
    }
    if (pass == 2 && g_ub_e2_pending) {  // ([Stereo] MainUnionBox) eye 2's side matrix of pass 1's swap against pass 2's own
        g_ub_e2_pending = false;
        uintptr_t vp = 0;
        float s2[16];
        const bool eq = rd(anchors::addr(Id::ViewportCurrent), &vp) && vp && rd_raw(vp + 0x3f0, s2, 64) && !std::memcmp(s2, g_ub_side[2], 64);
        bump(eq ? g_ub_e2_eq : g_ub_e2_ne);
    }
    rows_open(pass);  // (the rows diagnostic: read only)
    gv_sample(pass, 0);  // (item 6g's diagnostic: read only)
    if (pass == 1) p1vp_open(state::presents.load(std::memory_order_relaxed) + 1);  // ("prepass mi p1vp on", test only)
    uint32_t mr = ok ? main_region::kMiCascades : 0u;  // ([Stereo] MainRegion, item 8d: the frame's gates as this open saw them)
    if (pass == 1) {
        if (!prepass::eye2_rows_ready()) {  // (the twin reads eye 2's rows: written this frame, at the grown sizes)
            if (ok) bump(g_r_unready);
            ok = false;
        } else {
            mr |= main_region::kMiRows;
        }
        if (g_p1vp_res) mr |= main_region::kMiP1vp;
        int c = g_control.load(std::memory_order_relaxed);
        // ([Stereo] MainFamily=region, item 8e) the ip control ("prepass instprev main control nudge"), kind 5, when no other one is on
        if (!c && rg_on() && inst_prev::main_nudge_wanted()) c = 5;
        if (ok && c) {
            // (3 e2img: uint 56 zeroed for the walk, the latch kept; 4 pw: the PrevWorld rows 5-8 nudged; families.md 3.4; 5 ip: the main
            // InstPrev mirror's eye-2 rows nudged, latched by inst_prev::main_open below)
            const bool done = c == 3   ? eye2_images::guard_zero(true)
                              : c == 4 ? (rg_on() ? prevworld::set_nudge_ndc(0.001f) : prevworld::set_nudge(0.01f))
                              : c == 5 ? inst_prev::set_main_nudge(true)
                                       : prepass::eye2_rows_control(c);
            if (done) {
                t_ctl_mode = c;
                t_ctl = ctl = true;
                g_rw1.ctl = true;
                bump(g_r_ctl);
                mr |= main_region::kMiCtl;
            } else {
                if (c == 4) {  // (the review: a nudge stored but not latched would outlive the frame; both cleared)
                    prevworld::set_nudge(0.0f);
                    prevworld::set_nudge_ndc(0.0f);
                }
                bump(g_r_ctl_failed);
                ok = false;
            }
        }
    }
    // ([Stereo] MainSlice) the slice mode latched into pass 1's marker: the playback seeds S slice 1 far and pass 2 drops on that frame only
    const int sl = pass == 1 && ok ? slice_latch() : 0;
    // ([Stereo] MainFamily=region, item 8e) a control frame's marker carries its kind (the per-kind results); otherwise as before
    const MarkFn ctl_fn = rg_on() && t_ctl_mode > 0 && t_ctl_mode < kCkMarked ? kOpen1Ck[t_ctl_mode] : &cb_open1_ctl;
    void (*fn)(void*) = pass == 1 ? (ok ? (sl ? kOpen1Sl[ctl ? 1 : 0][sl - 1] : ctl ? ctl_fn : &cb_open1) : &cb_open1_x)
                                  : (ok ? &cb_open2 : &cb_open2_x);
    if (prepass::stream_marker(fn)) {
        t_open[pass] = true;
        bump(g_r_opens[pass]);
        if (pass == 1) prevworld::walk_open();  // (GOAL-single-pass item 6c, armed only: the walk's prevworld verdict, after the open marker)
        // ([Stereo] PrepassInstPrevMain, item 8d; installed only: the InstPrev mirror over this DVL call 1 (pass 1), its compare (pass 2);
        // closed at e1's end, or at this call's close below)
        inst_prev::main_open(pass);
        // ([Stereo] MainUnionCull, item 6e) pass 1's walk culls against the union; the playback told by a marker after the open's
        // (no marker recorded: the swap undone at once, so a frame is never union-culled unseen). Pass 2 is never touched
        if (pass == 1 && g_uc_installed.load(std::memory_order_relaxed) && g_uc_on.load(std::memory_order_relaxed) && uc_begin()) {
            if (prepass::stream_marker(&cb_uc)) {
                bump(g_uc_marks);
                mr |= main_region::kMiUc;
            } else {
                bump(g_uc_mark_failed);
                uc_end();
            }
        }
        // ([Stereo] MainRegion, item 8d, observe only; off: one relaxed load) its own open marker, after the others: the frame's key
        if (main_region::armed()) main_region::dvl_open(pass, mr);
    } else {
        bump(g_r_open_failed);
    }
}
void dvl_close(int pass) {
    if (pass != 1 && pass != 2) return;
    if (pass == 2) d3d::desc_phase(false);  // ([Stereo] DescCensus: to pass 2's close)
    inst_prev::main_close(true);  // ([Stereo] PrepassInstPrevMain, item 8d: the fallback close; returns at once without a main walk)
    if (pass == 1) uc_end();  // ([Stereo] MainUnionCull: the eye's planes back at DVL call 1's return; returns at once without a swap)
    if (pass == 1) prevworld::walk_close();  // (item 6c: no verdict marker after the walk; returns at once unless its walk is open)
    rows_close(pass);  // (before the control's rows are put back: a control frame is excluded anyway)
    if (pass == 1) p1vp_close();  // (pass 1's own gViewport back before the close sample and anything after the walk)
    if (g_on.load(std::memory_order_relaxed) && !g_stood.load(std::memory_order_relaxed)) gv_sample(pass, 1);  // (item 6g's diagnostic)
    if (t_ctl) {  // (the control's rows put back whatever else happens)
        t_ctl = false;
        const bool back = t_ctl_mode == 3   ? eye2_images::guard_zero(false)
                          : t_ctl_mode == 4 ? (prevworld::set_nudge(0.0f), prevworld::set_nudge_ndc(0.0f), true)
                          : t_ctl_mode == 5 ? inst_prev::set_main_nudge(false)  // (item 8e: the walk's latch already cleared at main_close)
                                            : prepass::eye2_rows_control(0);
        if (!back) bump(g_r_ctl_restore_failed);
    }
    if (anchors::present(Id::ShadowCamMtxId) && g_on.load(std::memory_order_relaxed)) {  // (the diagnostic: read only)
        const Diag d = read_diag();
        if (pass == 1) {
            g_dg1 = d;
        } else if (!d.ok || !g_dg1.ok) {
            bump(g_dg_unread);
        } else {
            bump(!std::memcmp(d.cam, g_dg1.cam, sizeof(d.cam)) && !std::memcmp(d.px, g_dg1.px, sizeof(d.px)) && !std::memcmp(d.tex, g_dg1.tex, sizeof(d.tex))
                     ? g_dg_equal
                     : g_dg_differ);
        }
        if (pass == 2) g_dg1.ok = false;
    }
    main_region::dvl_close(pass);  // ([Stereo] MainRegion: the render side's markers end with the call; returns at once when none)
    if (!t_open[pass]) return;
    t_open[pass] = false;
    if (prepass::stream_marker(&cb_close)) bump(g_r_closes);
    else bump(g_r_close_failed);
}

// ---- the playback thread
void note_pipeline(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* ps) {
    t_pso_cl = cl;
    t_pso = ps;
}
void note_bind(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, int single,
               const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
    Bind& x = t_bind;
    ++t_bind_gen;  // ([Stereo] MainRegion's cached check_bind)
    if (n > 8 || (n && !rts)) {
        x.cl = nullptr;
        return;
    }
    x.cl = cl;
    x.n = n;
    x.single = single != 0;
    for (unsigned i = 0; i < (single ? (n ? 1u : 0u) : n); ++i) x.rts[i] = rts[i];
    x.has_ds = ds != nullptr;
    if (ds) x.ds = *ds;
}

namespace {
// ([Stereo] MainPredication) a listed draw on a predicated list: true when its predicate is held for after_draw (g_pd_note);
// false: the frame is excluded as before (off, stood down, a slice frame, the record unknown, or a contradiction: stood down)
bool pd_hold(ID3D12GraphicsCommandList* cl) {
    if (!g_pd_installed.load(std::memory_order_relaxed) || g_pd_stood.load(std::memory_order_relaxed) || g_pd_mode.load(std::memory_order_relaxed) == 0)
        return false;
    if (g_f.slice) {  // (MainSlice's handover and drop keep their own rule: never under a held predicate)
        bump(g_pd_slice);
        return false;
    }
    d3d::Predicate p;
    const int st = d3d::thread_predicate(cl, &p);
    if (st == d3d::kPredUnknown) {  // (the record is another list's, or none yet on this thread: never guessed)
        bump(g_pd_unknown);
        return false;
    }
    if (st != d3d::kPredSet || !p.res) {
        pd_stand_down("thread_predicated() says predicated where the list's record holds none");
        return false;
    }
    g_pd_note.active = true;
    g_pd_note.cl = cl;
    g_pd_note.p = p;
    bump(g_pd_held[g_r.pass]);
    return true;
}

// ---- ([Stereo] MainFamily=region, item 8e) the listed path of a region frame (the playback thread, the region's thread)
const char* const kRvWhy[pso::region::kVerdicts] = {
    "region_twin_of: ?", "region_twin_of: no slot (pso unknown)", "region_twin_of: no twin (the slot's state)", "region_twin_of: a wvp twin",
    "region_twin_of: gate e2img", "region_twin_of: gate prevworld", "region_twin_of: gate instprev", "region_twin_of: twin",
    "region_twin_of: the frame latch (allow off, or not pass 1's walk)",
    "region_twin_of: the slot's formats, sample count or NumRenderTargets not the side arrays' views'",
    "region_twin_of: an instprev twin while \"prepass mi region ip off\""};
uint32_t rg_formats() {  // the side arrays' view formats, as region_twin_of compares them with the slot's (single-sampled: ensure's rule)
    return pso::region::pack_formats(static_cast<uint32_t>(g_a[kS][kFsc].vf), static_cast<uint32_t>(g_a[kS][kVel].vf),
                                     static_cast<uint32_t>(g_a[kS][kReact].vf), static_cast<uint32_t>(g_a[kS][kDepth].vf), true);
}
// a listed draw (in e1, non-empty, its pair in the subset, a dup frame): before_draw's listed path with the region's rules (no 6c
// would-drop, no 6e key: main_region keys e1; no withhold check: the set is empty); kDuplicate: after_draw draws its duplicates
int rg_list(ID3D12GraphicsCommandList* cl, const main_region::DrawInfo& di) {
    const uint64_t idx = d3d::thread_draws() - g_r.d0;  // (this draw's: counted before the call)
    if (g_r.listed && idx != g_r.last + 1) g_r.contig = false;
    if (!g_r.listed) g_r.first = idx;
    g_r.last = idx;
    ++g_r.listed;
    g_draw_writes = (di.flags & (pso::main_inst_flags::kDepthWrite | pso::main_inst_flags::kStencilWrite)) != 0;
    // (GOAL-single-pass-2 item 4's trim) pass 1's listed draws' depth and stencil writes: each is twinned into S with its pipeline's
    // depth-stencil state, or the frame is refused (a superset of the twins': a listed draw that refused counts too); a pipeline whose
    // desc was not read (a stream creation: no formats, no flags) counts as writing both
    if (g_r.pass == 1 && g_f.ho) {
        constexpr uint32_t kW = pso::main_inst_flags::kDepthWrite | pso::main_inst_flags::kStencilWrite;
        g_f.ho_wr1 |= di.fmt ? (di.flags & kW) : kW;
    }
    bump(g_p_listed[g_r.pass]);
    if (!g_r.pso0) g_r.pso0 = t_pso;
    else if (t_pso != g_r.pso0) g_r.mixed = true;
    g_r.cur_ord = di.ord;
    g_pd_note.active = false;
    if (d3d::thread_predicated()) {
        // ([Stereo] MainPredication) the list's predicate held for after_draw: the duplicates nulled, then it put back
        if (!pd_hold(cl)) {
            bump(g_p_predicated);
            g_f.why |= kXPred;
            g_r.ok = false;
            return kNone;
        }
    } else if (g_pd_installed.load(std::memory_order_relaxed) && !g_pd_stood.load(std::memory_order_relaxed)) {
        d3d::Predicate p;  // (the converse: the record says predicated where the hook's flag does not)
        if (d3d::thread_predicate(cl, &p) == d3d::kPredSet) {
            bump(g_pd_unpred_set);
            pd_stand_down("the list's record holds a predicate where thread_predicated() says none");
        }
    }
    if (!g_r.ok) return kNone;
    Bound b;
    const char* why = check_bind(cl, &b);
    if (!why && g_r.pass == 1 && !g_f.seeded) {
        if (g_f.e1f && g_f.e1f_remap && !g_f.e1f_rm_started) g_f.e1f_rm_try = rm_open_ok(cl);  // (item 8f's remap: S0 <- the four in the seed)
        // (the 6d/6e review's must-fix) a held predicate nulled across the seed, put back after (the record unchanged)
        const bool pd_seed = g_pd_note.active && g_pd_note.cl == cl;
        if (pd_seed) d3d::raw_set_predication(cl, nullptr, 0, 0);
        why = seed(cl, di.flags, b);
        if (pd_seed) {
            d3d::raw_set_predication(cl, g_pd_note.p.res, g_pd_note.p.off, g_pd_note.p.op);
            if (!why) bump(g_pd_seed_nulled);
        }
        if (!why) {
            g_f.seeded = true;
            g_f.gen = g_gen;
        } else if (g_p_make_failed.load()) {
            stand_down(why);  // (an allocation failed: not tried again this session)
        }
    }
    if (!why && (!g_f.seeded || g_f.gen != g_gen)) why = "not seeded this frame (or the arrays re-made since)";
    if (!why)
        for (int k = 0; k < kKinds && !why; ++k)
            if (g_a[kS][k].game != b.res[k]) why = "a bound target is not the arrays' game target";
    // (S1, both passes) the game's bound views in the arrays' view formats (R's draws use the game's pipeline through the arrays' views;
    // a view made without a format reads as its resource's, ensure's rule: the arrays' game target is this resource, so its fmt), and
    // the game's pipeline made for those formats, single-sampled, with three render targets (the twin is made from its desc)
    if (!why)
        for (int k = 0; k < kKinds && !why; ++k)
            if ((b.vf[k] != DXGI_FORMAT_UNKNOWN ? b.vf[k] : g_a[kS][k].fmt) != g_a[kS][k].vf) {
                bump(g_rg_x_vf);
                why = "a bound view's format is not the arrays' view format";
            }
    if (!why) {
        const uint32_t fmt = rg_formats();
        if (((di.flags >> 16) & 0xf) != 3 || !(fmt & pso::region::kFmtSingle) || di.fmt != fmt) {
            bump(g_rg_x_fmt);
            why = "the game pipeline's formats, sample count or NumRenderTargets are not the arrays' views'";
        }
    }
    if (why) {
        bump(g_p_refused);
        g_f.why |= kXRefused;
        g_r.ok = false;
        refuse(why);
        return kNone;
    }
    if (g_f.notwin && g_r.pass == 1) {  // (E3, "dupall twins off": seeded, no twin; the game's draw alone, as on a non-dup frame)
        g_pd_note.active = false;  // (the held predicate only noted: nothing to put back)
        uint32_t v = 0;  // (an instprev twin's "ok" marker spent as the twin's draw would spend it: rm_draw's rmdrop rule)
        ID3D12PipelineState* tw = pso::region_twin_of(t_pso, cl, rg_formats(), g_f.ip, &v);
        if (tw && ((v >> 8) & pso::region::kKIp) != 0) inst_prev::consume_draw_ok();
        bump(g_e1f_notwin_draws);
        return kNone;
    }
    return kDuplicate;
}
// (item 8e 14) solo and subset frames: S and R seeded at e1's first non-empty draw of pass 1, as joint frames are. A selection's own first
// draw may meet the depth's two planes in different states (the character pairs', r113 E2m: every solo frame refused at the seed, so
// those families were never measured). The draw itself is not listed; a refused early seed leaves the listed draw to try, as before.
// Off by default ("prepass mi region seed early|listed"): it compares the selection against e1's starting depth, without the depth
// earlier families wrote, so its results are not the listed rule's and are reported apart
void rg_seed_early(ID3D12GraphicsCommandList* cl, const main_region::DrawInfo& di) {
    if (!g_rg_seed_early_on.load(std::memory_order_relaxed) || g_r.pass != 1 || g_f.seeded || !g_r.ok || g_f.rmode == kRgJoint) return;
    d3d::Predicate p;
    const bool pred = d3d::thread_predicated();
    if (pred && (d3d::thread_predicate(cl, &p) != d3d::kPredSet || !p.res)) return;  // (the list's record unknown: never guessed)
    Bound b;
    const char* why = check_bind(cl, &b);
    if (!why) {
        if (pred) d3d::raw_set_predication(cl, nullptr, 0, 0);  // (the seed's copies and clears never predicated; the record unchanged)
        why = seed(cl, di.flags, b);
        if (pred) d3d::raw_set_predication(cl, p.res, p.off, p.op);
    }
    if (!why) {
        g_f.seeded = true;
        g_f.gen = g_gen;
        bump(g_rg_seed_early);
    } else if (g_p_make_failed.load()) {
        stand_down(why);
    }
}
// ([Stereo] MainRegionE1Compare, item 8f) pass 2's non-empty e1 draw of an e1f frame: nothing listed (R gets no duplicates; R1 keeps the
// seed). Its list and bind generation noted (C2's guards); T at the first, before the own-draw return; a player's own draw (body.cpp's
// brackets) is drawn again after the game's by the game's pipeline into S's slice-1 view (rg_after), so S1 and C2 hold the same draws.
// Its rules are rg_list's (the four bound, the arrays' targets, the views' and the pipeline's formats; the frame seeded); an own draw that
// cannot be drawn into S1 (or a predicated one: pass 2 is not expected to be) excludes the frame, never guessed
int e1f_before2(ID3D12GraphicsCommandList* cl, const main_region::DrawInfo& di) {
    g_e1f_cl = cl;
    g_e1f_bgen = t_bind_gen;
    for (int k = 0; k < kDepth; ++k) g_f.e1f_cmoved[k] = false;  // (a draw: its bound colour targets are RENDER_TARGET, as the game's draw needs)
    if (!g_f.e1f_t_tried) e1f_take_t(cl, di);
    if (g_own_pb <= 0) return kNone;
    bump(g_rg_own);
    if (!g_r.ok || !g_f.e1f_t) return kNone;  // (the frame excluded already: nothing more drawn)
    const char* why = nullptr;
    Bound b;
    if (d3d::thread_predicated()) why = "a player's own draw on a predicated list in pass 2";
    if (!why) why = check_bind(cl, &b);
    if (!why && (!g_f.seeded || g_f.gen != g_gen)) why = "not seeded this frame (or the arrays re-made since)";
    for (int k = 0; k < kKinds && !why; ++k)
        if (g_a[kS][k].game != b.res[k]) why = "a bound target is not the arrays' game target";
    for (int k = 0; k < kKinds && !why; ++k)
        if ((b.vf[k] != DXGI_FORMAT_UNKNOWN ? b.vf[k] : g_a[kS][k].fmt) != g_a[kS][k].vf) {
            bump(g_rg_x_vf);
            why = "a bound view's format is not the arrays' view format";
        }
    if (!why) {
        const uint32_t fmt = rg_formats();
        if (((di.flags >> 16) & 0xf) != 3 || !(fmt & pso::region::kFmtSingle) || di.fmt != fmt) {
            bump(g_rg_x_fmt);
            why = "the own draw's pipeline: its formats, sample count or NumRenderTargets are not the arrays' views'";
        }
    }
    if (why) {
        bump(g_e1f_own_refused);
        bump(g_p_refused);
        g_f.why |= kXRefused;
        g_r.ok = false;
        std::snprintf(g_e1f_why, sizeof(g_e1f_why), "own draw: %s", why);
        refuse(why);
        return kNone;
    }
    g_r.e1f_own = true;
    return kDuplicate;
}
// (item 8f, the review of a6830cc9) S's depth view v for a draw into S: read-only while the remap opened read-only stands (ro), or on an e1f
// frame while the game's depth is read-only now (the follow, both planes), the writable one otherwise
// (the review of 1588431b) per plane: bit 0 the depth plane read-only now, bit 1 the stencil plane (a plane not followed: writable, as before)
int e1f_depth_ro_now() {
    if (prepass::kind_game(0) != g_a[kS][kDepth].game) return 0;
    int m = 0;
    for (unsigned p = 0; p < 2; ++p) {
        D3D12_RESOURCE_STATES st{};
        if (prepass::kind_followed(0, p, &st) && rm_read_only(st)) m |= 1 << p;
    }
    return m;
}
D3D12_CPU_DESCRIPTOR_HANDLE s_dsv(int v, int ro) {
    const Arr& a = g_a[kS][kDepth];
    if (ro >= 1 && ro <= 3 && a.ro_ok) {
        bump(g_e1f_ro_dsv_draws[ro - 1]);
        return a.ro[ro - 1][v];
    }
    return a.view[v];
}
// ---- ([Stereo] MainRegionHandover, item 8g) the merge, S1's FSC SRV, the decision, the handover, the drops
const char kHoHlsl[] =
    "Texture2DArray<float4> Sc : register(t%u, space2);\n"  // S slice 1's FullScreenCopy (one slice: index 0)
    "float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0.5, 1); }\n"
    "float4 ps(float4 p : SV_Position) : SV_Target0 {\n"
    "    float4 c = Sc.Load(int4(p.xy, 0, 0));\n"
    "    if (all(c == float4(1.0009765625, 2.001953125, 0.50048828125, 1.0))) discard;\n"  // (the 8f0b sentinel: the game's own kept)
    "    return c;\n"
    "}\n";
bool ho_compile(char* why, size_t n) {  // (the worker, under g_ho_compile_mutex) once: the record's index is the register
    if (g_ho_vs && g_ho_ps) return true;
    if (g_ho_compile_tried) {
        std::snprintf(why, n, "the handover merge's shaders did not compile (the log has the compiler's message)");
        return false;
    }
    g_ho_compile_tried = true;
    if (!g_compile) {
        std::snprintf(why, n, "no D3DCompile");
        return false;
    }
    char src[sizeof(kHoHlsl) + 32];
    const int len = std::snprintf(src, sizeof(src), kHoHlsl, g_ho_idx.load());
    if (len <= 0 || len >= static_cast<int>(sizeof(src))) {
        std::snprintf(why, n, "the merge source not formatted");
        return false;
    }
    ID3DBlob* out[2] = {};
    const char* const entry[2] = {"vs", "ps"};
    const char* const target[2] = {"vs_5_1", "ps_5_1"};
    for (int i = 0; i < 2; ++i) {
        ID3DBlob* err = nullptr;
        const HRESULT hr = g_compile(src, static_cast<SIZE_T>(len), "rdrvr_region_handover", nullptr, nullptr, entry[i], target[i],
                                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out[i], &err);
        if (FAILED(hr) || !out[i]) {
            log::error("[mi] MainRegionHandover: the merge's %s does not compile (%#lx): %s", entry[i], static_cast<unsigned long>(hr),
                       err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            if (err) err->Release();
            if (out[0]) out[0]->Release();
            if (out[1]) out[1]->Release();
            std::snprintf(why, n, "the merge's %s does not compile", entry[i]);
            return false;
        }
        if (err) err->Release();
    }
    g_ho_vs = out[0];
    g_ho_ps = out[1];
    log::info("[mi] MainRegionHandover: the merge's shaders compiled (t%u, space 2)", g_ho_idx.load());
    return true;
}
const char* ho_build(HoMerge& m) {  // (the worker) one RT (FSC's view format), no depth, blend off, the bound pipelines' own root signature
    ID3D12Device* dev = state::device.load();
    if (!dev) return "no device";
    ID3DBlob *vs = nullptr, *ps = nullptr;
    {
        std::lock_guard lock(g_ho_compile_mutex);
        if (!ho_compile(m.why, sizeof(m.why))) return m.why;
        vs = g_ho_vs;
        ps = g_ho_ps;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m.root;
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.InputLayout = {nullptr, 0};
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = m.fmt;
    pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pd.SampleDesc.Count = 1;
    HRESULT h = E_FAIL;
    {
        const pso::OwnCreation own;  // ([Stereo] InstancedShadersAuto leaves the mod's own pipelines alone)
        h = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m.pso));
    }
    if (FAILED(h) || !m.pso) {
        m.pso = nullptr;
        std::snprintf(m.why, sizeof(m.why), "CreateGraphicsPipelineState refused the merge (%#lx): the root signature's t-space 2 not visible to the "
                      "pixel shader, or another desc mismatch", static_cast<unsigned long>(h));
        return m.why;
    }
    m.pso->SetName(L"RDRVR region handover merge (test only)");
    return nullptr;
}
DWORD WINAPI ho_worker(void* p) {
    HoMerge& m = *static_cast<HoMerge*>(p);
    const char* why = ho_build(m);
    if (why) {
        if (why != m.why) std::snprintf(m.why, sizeof(m.why), "%s", why);
        log::warn("[mi] MainRegionHandover: no merge pipeline for the root signature %p: %s", static_cast<void*>(m.root), m.why);
        m.state.store(-1, std::memory_order_release);
    } else {
        log::info("[mi] MainRegionHandover: the merge pipeline made for the root signature %p", static_cast<void*>(m.root));
        m.state.store(2, std::memory_order_release);
    }
    return 0;
}
// the merge for this root signature and format: nullptr with *out when ready; else why (its worker started once)
const char* ho_merge_for(ID3D12RootSignature* root, DXGI_FORMAT fmt, HoMerge** out) {
    HoMerge* free_slot = nullptr;
    for (HoMerge& m : g_hm) {
        if (m.root == root && m.fmt == fmt) {
            const int st = m.state.load(std::memory_order_acquire);
            if (st == 2) {
                *out = &m;
                return nullptr;
            }
            return st == 1 ? "the merge pipeline not made yet (its worker)" : "the merge pipeline refused for this root signature";
        }
        if (!m.root && !free_slot) free_slot = &m;
    }
    if (!free_slot) return "the merge table is full";
    free_slot->root = root;
    free_slot->fmt = fmt;
    free_slot->state.store(1, std::memory_order_relaxed);
    HANDLE th = CreateThread(nullptr, 0, &ho_worker, free_slot, 0, nullptr);
    if (!th) {
        free_slot->state.store(-1, std::memory_order_relaxed);
        return "the merge's worker not started";
    }
    CloseHandle(th);
    return "the merge pipeline not made yet (its worker started)";
}
// S1's FSC SRV for the current arrays: -1 written (now or before); else the refusal's reason (*why set)
int ho_srv_step(const char** why) {
    if (g_ho_srv_gen == g_gen) return -1;
    if (g_ho_srv_gen && state::presents.load() < g_ho_remake + kHold) {
        *why = "held: 8 presents after an arrays' re-make";
        return kHwHeld;
    }
    static char w[200];
    if (!g_ho_rec_have) {
        bool read = false;
        uint64_t used = 0, cap = 0;
        const eye2_images::Alloc a = eye2_images::alloc_p1("RDRVR region handover FullScreenCopy", "the handover's FSC SRV", &g_ho_rec, w, sizeof(w), &read, &used, &cap);
        if (a != eye2_images::Alloc::kOk) {
            if (a != eye2_images::Alloc::kRefused) {
                g_ho_stood = true;
                log::error("[mi] MainRegionHandover stood down: %s (no more handovers)", w);
            }
            *why = w;
            return kHwSrv;
        }
        g_ho_rec_have = true;
        g_ho_idx.store(static_cast<uint32_t>(g_ho_rec.index), std::memory_order_relaxed);
        log::info("[mi] MainRegionHandover: the FSC SRV at partition 1 index %d (used %llu of %llu)", g_ho_rec.index, static_cast<unsigned long long>(used + 1),
                  static_cast<unsigned long long>(cap));
    }
    const Arr& fc = g_a[kS][kFsc];
    if (!fc.arr || typeless_or_unknown(fc.vf)) {
        *why = "the side FullScreenCopy not made, or no typed view format";
        return kHwSrv;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Format = fc.vf;
    sv.Texture2DArray.MostDetailedMip = 0;
    sv.Texture2DArray.MipLevels = 1;
    sv.Texture2DArray.FirstArraySlice = 1;  // (slice 1: eye 2's)
    sv.Texture2DArray.ArraySize = 1;
    if (!eye2_images::create_srv_at(fc.arr, &sv, g_ho_rec.cpu)) {
        g_ho_stood = true;
        log::error("[mi] MainRegionHandover stood down: a fault in CreateShaderResourceView (its FSC SRV)");
        *why = "a fault in CreateShaderResourceView";
        return kHwSrv;
    }
    g_ho_srv_gen = g_gen;
    return -1;
}
// R1's duplicate of a compare frame's pass-2 draw: the game's pipeline into R's slice-1 views, R's depth view read-only as the game's now
void ho_r1_draw(ID3D12GraphicsCommandList* cl, const d3d::DrawArgs& args) {
    D3D12_CPU_DESCRIPTOR_HANDLE rt[3], ds{};
    for (int k = 0; k < kDepth; ++k) rt[k] = g_a[kR][k].view[1];
    const int ro = e1f_depth_ro_now();
    const Arr& rd_ = g_a[kR][kDepth];
    ds = ro >= 1 && ro <= 3 && rd_.ro_ok ? rd_.ro[ro - 1][1] : rd_.view[1];
    if (g_f.ho_cl && cl != g_f.ho_cl) bump(g_ho_r1_other_list);
    if (g_f.ho_r1dsv_game && g_r.ho_r1_w) {  // (the review: a writing pipeline would write the game's depth through its DSV: R's views)
        bump(g_ho_r1dsv_kept_r);
    } else if (g_f.ho_r1dsv_game) {  // (r120 HG3, test only: the game's own DSV as bound for this draw; the four's size, array size 1 as R1's RTVs)
        if (ro == 3 && t_bind.has_ds && t_bind.ds.ptr) {
            ds = t_bind.ds;
            bump(g_ho_r1dsv_draws);
        } else {
            if (!(g_f.why & kXHo)) bump(g_ho_r1dsv_refused);
            g_f.why |= kXHo;  // (a plane not followed in a read state, or no DSV bound: no compare this frame)
        }
    }
    d3d::raw_bind(cl, 3, rt, FALSE, &ds);
    d3d::raw_draw(cl, args);
    d3d::raw_bind(cl, t_bind.n, t_bind.rts, FALSE, &t_bind.ds);  // the game's bind back (check_bind proved it is the four)
    bump(g_ho_r1_draws);
}
// R1 can take this draw: the four bound with the arrays' formats, the pipeline's formats the arrays' (rg_list's rules)
bool ho_r1_ok(ID3D12GraphicsCommandList* cl, const main_region::DrawInfo& di) {
    Bound b;
    if (check_bind(cl, &b)) return false;
    for (int k = 0; k < kKinds; ++k)
        if (g_a[kS][k].game != b.res[k] || (b.vf[k] != DXGI_FORMAT_UNKNOWN ? b.vf[k] : g_a[kS][k].fmt) != g_a[kS][k].vf) return false;
    const uint32_t fmt = rg_formats();
    return ((di.flags >> 16) & 0xf) == 3 && (fmt & pso::region::kFmtSingle) && di.fmt == fmt;
}
bool ho_refuse(int r, const char* why) {
    bump(g_ho_x[r]);
    if (g_f.ho_g1 || g_f.ho_h1) bump(g_f.ho_h1 ? g_ho_h1_refused : g_ho_g1_refused);  // (GOAL-single-pass-2 item 4: latched, refused for this reason)
    g_f.ho_state = kHsRefused;
    std::snprintf(g_ho_why, sizeof(g_ho_why), "%s: %s", kHwName[r], why ? why : "?");
    log::limited("mi.ho", 12, "[mi] handover refused (%s): %s (the frame stays two-pass)", kHwName[r], why ? why : "?");
    return false;
}
// T -> R1: the game's four at the decision into R's slice 1 (the colour bound: RENDER_TARGET; the depth from the follow, per plane)
void ho_take_t(ID3D12GraphicsCommandList* cl, const Bound& b, const D3D12_RESOURCE_STATES dst[2]) {
    const D3D12_RESOURCE_STATES kRt = D3D12_RESOURCE_STATE_RENDER_TARGET, kDw = D3D12_RESOURCE_STATE_DEPTH_WRITE, kCs = D3D12_RESOURCE_STATE_COPY_SOURCE,
                                kCd = D3D12_RESOURCE_STATE_COPY_DEST;
    ID3D12Resource* rd_ = g_a[kR][kDepth].arr;
    Batch bt;
    for (int k = 0; k < kDepth; ++k) {
        bt.add(b.res[k], kRt, kCs);
        bt.add_sub(g_a[kR][k].arr, 1, kRt, kCd);
    }
    for (UINT p = 0; p < 2; ++p) bt.add_sub(b.res[kDepth], p, dst[p], kCs);
    bt.add_sub(rd_, 1, kDw, kCd);  // (slice 1's planes: subresources 1 and 3)
    bt.add_sub(rd_, 3, kDw, kCd);
    bt.flush(cl);
    for (int k = 0; k < kDepth; ++k) copy_sub(cl, g_a[kR][k].arr, 1, b.res[k], 0);
    copy_sub(cl, rd_, 1, b.res[kDepth], 0);
    copy_sub(cl, rd_, 3, b.res[kDepth], 1);
    for (int k = 0; k < kDepth; ++k) {
        bt.add(b.res[k], kCs, kRt);
        bt.add_sub(g_a[kR][k].arr, 1, kCd, kRt);
    }
    for (UINT p = 0; p < 2; ++p) bt.add_sub(b.res[kDepth], p, kCs, dst[p]);
    bt.add_sub(rd_, 1, kCd, kDw);
    bt.add_sub(rd_, 3, kCd, kDw);
    bt.flush(cl);
    g_f.e1f_t = true;  // (C at e1's end: 8f's C2 path and its guards)
    g_f.e1f_t_tried = true;
    g_f.e1f_barrier = g_f.e1f_clear = g_f.e1f_write = g_f.e1f_split = false;
    for (int k = 0; k < kDepth; ++k) g_f.e1f_cmoved[k] = false;
    g_r.e1f_watch = true;
    bump(g_ho_t);
    if (g_f.ho_mask) {  // (H2m's diagnosis: R0's colour, the own draws' mask, from 0; R's colour at rest RENDER_TARGET, unpredicated here)
        const float zero[4] = {};
        for (int k = 0; k < kDepth; ++k) d3d::raw_clear_rtv(cl, &g_a[kR][k].view[0], zero);
    }
    if (g_f.ho_r1sync) {  // (r120 HG3, test only: a full flush before R1's first draw)
        D3D12_RESOURCE_BARRIER fb[2]{};
        fb[0].Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
        fb[0].Aliasing.pResourceBefore = nullptr;
        fb[0].Aliasing.pResourceAfter = nullptr;
        fb[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        fb[1].UAV.pResource = nullptr;
        d3d::raw_barrier(cl, 2, fb);
        bump(g_ho_r1sync_n);
    }
}
// (GOAL-single-pass-2 item 4) H: an H1 frame's game four right after the handover (its copies and merge done; before any pass-2 e1 draw
// the game draws on top) into R's slice 0, as ho_take_t's: the colour RENDER_TARGET (the merge's target, the copies' put back), the depth
// per plane from the follow (the handover put each back); R slice 0 from its rest state and back (an H1 frame has no own mask in R0)
void ho_take_h(ID3D12GraphicsCommandList* cl, const Bound& b, const D3D12_RESOURCE_STATES dst[2]) {
    const D3D12_RESOURCE_STATES kRt = D3D12_RESOURCE_STATE_RENDER_TARGET, kDw = D3D12_RESOURCE_STATE_DEPTH_WRITE, kCs = D3D12_RESOURCE_STATE_COPY_SOURCE,
                                kCd = D3D12_RESOURCE_STATE_COPY_DEST;
    ID3D12Resource* rd_ = g_a[kR][kDepth].arr;
    Batch bt;
    for (int k = 0; k < kDepth; ++k) {
        bt.add(b.res[k], kRt, kCs);
        bt.add_sub(g_a[kR][k].arr, 0, kRt, kCd);
    }
    for (UINT p = 0; p < 2; ++p) bt.add_sub(b.res[kDepth], p, dst[p], kCs);
    bt.add_sub(rd_, 0, kDw, kCd);  // (slice 0's planes: subresources 0 and 2)
    bt.add_sub(rd_, 2, kDw, kCd);
    bt.flush(cl);
    for (int k = 0; k < kDepth; ++k) copy_sub(cl, g_a[kR][k].arr, 0, b.res[k], 0);
    copy_sub(cl, rd_, 0, b.res[kDepth], 0);
    copy_sub(cl, rd_, 2, b.res[kDepth], 1);
    for (int k = 0; k < kDepth; ++k) {
        bt.add(b.res[k], kCs, kRt);
        bt.add_sub(g_a[kR][k].arr, 0, kCd, kRt);
    }
    for (UINT p = 0; p < 2; ++p) bt.add_sub(b.res[kDepth], p, kCs, dst[p]);
    bt.add_sub(rd_, 0, kCd, kDw);
    bt.add_sub(rd_, 2, kCd, kDw);
    bt.flush(cl);
    bump(g_ho_h);
}
// the handover: S1 -> the four, unpredicated (the decision's rule); the game's pipeline, bind, viewport, scissor and topology put back
void ho_handover(ID3D12GraphicsCommandList* cl, const Bound& b, const D3D12_RESOURCE_STATES dst[2], HoMerge* m, const d3d::RasterNote& rn) {
    const D3D12_RESOURCE_STATES kRt = D3D12_RESOURCE_STATE_RENDER_TARGET, kDw = D3D12_RESOURCE_STATE_DEPTH_WRITE, kCs = D3D12_RESOURCE_STATE_COPY_SOURCE,
                                kCd = D3D12_RESOURCE_STATE_COPY_DEST, kPs = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* sd = g_a[kS][kDepth].arr;
    // (FXAA: velocity and reactivity are unread [V item8a.md; gpu.md 7.2's readers], so S1's are not handed over; DLSS: both)
    const bool vr = g_f.e1f_dlss || g_f.ho_vr_always;  // (velreact always: under FXAA too, H2m's diagnosis)
    // (GOAL-single-pass-2 item 4's trim: the decision's ho_cp; trim off, both planes, as before)
    const bool cp[2] = {(g_f.ho_cp >> 3 & 1) != 0, (g_f.ho_cp >> 4 & 1) != 0};
    if (!cp[0]) bump(g_ho_trim_depth);
    if (!cp[1]) bump(g_ho_trim_stencil);
    Batch bt;
    if (cp[0]) bt.add_sub(sd, 1, kDw, kCs);  // (S1's planes: subresources 1 and 3)
    if (cp[1]) bt.add_sub(sd, 3, kDw, kCs);
    for (UINT p = 0; p < 2; ++p)
        if (cp[p]) bt.add_sub(b.res[kDepth], p, dst[p], kCd);
    if (vr)
        for (const int k : {static_cast<int>(kVel), static_cast<int>(kReact)}) {
            bt.add_sub(g_a[kS][k].arr, 1, kRt, kCs);
            bt.add(b.res[k], kRt, kCd);
        }
    bt.add_sub(g_a[kS][kFsc].arr, 1, kRt, kPs);  // (the merge's SRV)
    bt.flush(cl);
    if (cp[0]) copy_sub(cl, b.res[kDepth], 0, sd, 1);
    if (cp[1]) copy_sub(cl, b.res[kDepth], 1, sd, 3);
    if (vr)
        for (const int k : {static_cast<int>(kVel), static_cast<int>(kReact)}) copy_sub(cl, b.res[k], 0, g_a[kS][k].arr, 1);
    if (cp[0]) bt.add_sub(sd, 1, kCs, kDw);
    if (cp[1]) bt.add_sub(sd, 3, kCs, kDw);
    for (UINT p = 0; p < 2; ++p)
        if (cp[p]) bt.add_sub(b.res[kDepth], p, kCd, dst[p]);
    if (vr)
        for (const int k : {static_cast<int>(kVel), static_cast<int>(kReact)}) {
            bt.add_sub(g_a[kS][k].arr, 1, kCs, kRt);
            bt.add(b.res[k], kCd, kRt);
        }
    bt.flush(cl);
    bump(vr ? g_ho_velreact : g_ho_fxaa_skip);
    if (vr && !g_f.e1f_dlss) bump(g_ho_vr_fxaa);
    // FSC: the merge, a covering triangle into the game's own FSC view alone (no depth), the full target's viewport and scissor
    const D3D12_RESOURCE_DESC fd = b.res[kFsc]->GetDesc();
    const float vp[1][6] = {{0.0f, 0.0f, static_cast<float>(fd.Width), static_cast<float>(fd.Height), 0.0f, 1.0f}};
    const int32_t sc[1][4] = {{0, 0, static_cast<int32_t>(fd.Width), static_cast<int32_t>(fd.Height)}};
    const D3D12_CPU_DESCRIPTOR_HANDLE rt0 = t_bind.rts[kFsc];
    pso::set_raw(cl, m->pso);
    d3d::raw_bind(cl, 1, &rt0, FALSE, nullptr);
    d3d::raw_set_raster(cl, 1, vp, 1, sc, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3d::raw_draw(cl, d3d::DrawArgs{false, 3, 1, 0, 0, 0});
    d3d::raw_set_raster(cl, rn.nvp, rn.vp, rn.nsc, rn.sc, rn.topo);
    pso::set_raw(cl, t_pso);
    d3d::raw_bind(cl, t_bind.n, t_bind.rts, FALSE, &t_bind.ds);
    bt.add_sub(g_a[kS][kFsc].arr, 1, kPs, kRt);
    bt.flush(cl);
    bump(g_ho_merges);
}
// pass 2's first non-empty e1 draw of a handover frame, before the game's: decided (true: handed over in its mode) or refused (false,
// counted by reason: the frame two-pass). Nothing is dropped or copied before every check has held
bool ho_decide(ID3D12GraphicsCommandList* cl, const main_region::DrawInfo& di) {
    const char* w = nullptr;
    const int mode = g_f.ho_mode;
    if (!g_r.ok || g_f.why) return ho_refuse(kHwExcluded, "the frame excluded already");
    if (!main_region::pass1_clean(&w)) return ho_refuse(kHwNotClean, w);
    if (state::presents.load() < g_ho_query_until.load(std::memory_order_relaxed))
        return ho_refuse(kHwQueryHold, "held: a query was begun or ended inside a recent dropped run");
    if (g_own_bad || g_f.ho_ind1 || g_f.noord1 || g_f.listed1 <= 0 || g_f.twins1 != g_f.listed1 || !g_f.seeded || g_f.gen != g_gen ||
        g_f.ho_nown > static_cast<uint32_t>(kHoOwnKeys))
        return ho_refuse(kHwWhole, "pass 1's region not whole (a twin for each listed draw, seeded, no draw without an ordinal or indirect)");
    if (g_f.ho_strict && g_own_n[1] > 0) return ho_refuse(kHwStrict, "strict: pass 1 drew the player's own entities");
    if (GetCurrentThreadId() != g_r.tid || cl->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || cl != t_pso_cl || !t_pso)
        return ho_refuse(kHwThread, "not the region's thread, a DIRECT list, or its pipeline not noted");
    if (d3d::thread_predicated()) return ho_refuse(kHwPred, "the list is predicated at the decision");
    {
        d3d::Predicate pr;  // (the review's NIT: the list's predicate record known, unset)
        if (d3d::thread_predicate(cl, &pr) != d3d::kPredNone) return ho_refuse(kHwPred, "the list's predicate record not known (or set)");
    }
    if (const int q = d3d::thread_open_queries(cl); q != 0) return ho_refuse(kHwQuery, q < 0 ? "BeginQuery not hooked (the open queries unknown)" : "a counting query open");
    Bound b;
    if (const char* why = check_bind(cl, &b)) return ho_refuse(kHwBind, why);
    for (int k = 0; k < kKinds; ++k)
        if (g_a[kS][k].game != b.res[k] || (b.vf[k] != DXGI_FORMAT_UNKNOWN ? b.vf[k] : g_a[kS][k].fmt) != g_a[kS][k].vf)
            return ho_refuse(kHwFmt, "a bound target or its view's format is not the arrays'");
    D3D12_RESOURCE_STATES dst[2]{};
    const bool wr[2] = {(di.flags & pso::main_inst_flags::kDepthWrite) != 0, (di.flags & pso::main_inst_flags::kStencilWrite) != 0};
    if (!e1f_depth_state(cl, kFsHandover, b.res[kDepth], wr, dst))
        return ho_refuse(kHwDepth, "the depth's planes not followed on this list, or not DEPTH_WRITE / read-only");
    d3d::RasterNote rn;
    if (!d3d::raster_note(cl, &rn) || !rn.has_vp || !rn.has_sc || !rn.has_topo) return ho_refuse(kHwView, "the list's viewport, scissor or topology not noted");
    HoMerge* m = nullptr;
    const bool copies = mode == 1 || mode == 4;  // (on, and copyonly: the handover's copies and merge)
    if (copies) {
        ID3D12RootSignature* root = pso::root_of(t_pso);
        if (!root) return ho_refuse(kHwRoot, "the bound pipeline's root signature not kept");
        const char* sw = nullptr;
        if (const int r = ho_srv_step(&sw); r >= 0) return ho_refuse(r, sw);
        // (the review's SF1) the merge reads the bindless t-space 2 table: its root parameter (from the root signature's desc) given a table
        // on this list since the list's root signature, the bound pipeline's, was last set
        const void* bound = nullptr;
        uint64_t tables = 0;
        const int bp = pso::bindless_param(root, g_ho_idx.load(std::memory_order_relaxed));
        if (bp < 0 || bp >= 64 || !d3d::root_note(cl, &bound, &tables) || bound != root || !(tables >> bp & 1))
            return ho_refuse(kHwTable, bp < 0 ? "the root signature has no pixel-visible SRV table in space 2 over the merge's register"
                                              : "the list's root signature not the pipeline's, or its bindless table not set since");
        // (the delta review) and where it points: partition 1's GPU base (the game's convention, 0x140f7fc20 [V]), from the record (its GPU
        // handle is the base plus its index times the increment, alloc_p1's analysed record)
        uint64_t h = 0;
        const uint64_t base = g_ho_rec.gpu - static_cast<uint64_t>(g_ho_rec.index) * static_cast<uint64_t>(g_ho_rec.inc);
        if (!d3d::root_table_gpu(cl, static_cast<unsigned>(bp), &h) || !g_ho_rec.gpu || g_ho_rec.index <= 0 || g_ho_rec.inc <= 0 || h != base)
            return ho_refuse(kHwTableHandle, "the bindless table's handle is not partition 1's GPU base");
        if (const char* why = ho_merge_for(root, g_a[kS][kFsc].vf, &m)) return ho_refuse(kHwMerge, why);
    }
    // (GOAL-single-pass-2 item 4) ref h1's G1 frame: every check held (it would have been handed over), refused here: an 8f compare frame;
    // counted, neither logged nor the last refusal
    if (g_f.ho_g1) {
        bump(g_ho_x[kHwG1]);
        bump(g_ho_g1alt);
        g_f.ho_g1_done = true;
        g_f.ho_state = kHsRefused;
        return false;
    }
    {  // (GOAL-single-pass-2 item 4) the planes copied (bit per plane): FSC; velocity and reactivity under DLSS (or velreact always); the
       // depth's planes unless trim found no listed pass-1 draw writing one. Whatever the mode: H1's expected
        const uint32_t wr1 = g_f.ho_trim ? g_f.ho_wr1 : pso::main_inst_flags::kDepthWrite | pso::main_inst_flags::kStencilWrite;
        g_f.ho_cp = 1u | (g_f.e1f_dlss || g_f.ho_vr_always ? 6u : 0u) | (wr1 & pso::main_inst_flags::kDepthWrite ? 8u : 0u) |
                    (wr1 & pso::main_inst_flags::kStencilWrite ? 16u : 0u);
    }
    const bool trimmed = (g_f.ho_cp & 24u) != 24u;
    // decided: T first on a compare frame (the four still hold it), then the handover (on), nothing (nohandover, identity)
    fs_log("handover", cl, dst, !copies ? "T only from"
                                : trimmed ? "the game's depth -> COPY_SOURCE (T) / COPY_DEST (the handover; trim: a plane not copied left alone) and back from"
                                          : "the game's depth -> COPY_SOURCE (T) / COPY_DEST (the handover) and back from");
    g_f.ho_cl = cl;
    if (di.ord >= 0 && di.ord < 64) {  // (r120 HG3: the decision draw's pair, logged once per ordinal and counted)
        if (g_ho_dec_n[di.ord].fetch_add(1, std::memory_order_relaxed) == 0) {
            uint64_t vs = 0, ps = 0;
            main_region::ord_pair(di.ord, &vs, &ps);
            log::info("[mi] handover decision draw: ordinal %d pair %08x:%08x (first time)", di.ord, static_cast<unsigned>(vs >> 32),
                      static_cast<unsigned>(ps >> 32));
        }
    } else {
        bump(g_ho_dec_noord);
    }
    if (g_f.rcmp) ho_take_t(cl, b, dst);
    if (copies) ho_handover(cl, b, dst, m, rn);
    if (g_f.ho_h1) ho_take_h(cl, b, dst);  // (item 4: H right after the handover; nohandover and identity: the four as T)
    g_f.ho_state = kHsHanded;
    bump(mode == 1 ? g_ho_handed : mode == 2 ? g_ho_nohand : mode == 4 ? g_ho_copyonly : g_ho_ident);
    return true;
}
// each pass-2 non-empty e1 draw of a handover frame. Dropped (kDrop: the game's not issued) when not the player's own (its bracket checked
// first), with an ordinal, and its key found among pass 1's by count (main_region's keys: a key drawn more in pass 2 is a miss); else the
// game's own (on top of the handover). Identity: nothing dropped. A compare frame also draws each into R1 (the dropped in place of the game's)
int ho_before2(ID3D12GraphicsCommandList* cl, const main_region::DrawInfo& di, const d3d::DrawArgs& args) {
    if (g_f.ho_state == kHsNone && !ho_decide(cl, di)) return g_f.rcmp ? e1f_before2(cl, di) : kNone;
    if (g_f.rcmp) {  // (C's guards at e1's end, as e1f_before2's)
        g_e1f_cl = cl;
        g_e1f_bgen = t_bind_gen;
        for (int k = 0; k < kDepth; ++k) g_f.e1f_cmoved[k] = false;
    }
    const bool own = g_own_pb > 0;
    if (di.ord >= 0 && di.ord < 64) bump(g_ho_pair_n[di.ord]);
    // (dropmask: a listed draw of a pair outside the mask is kept, the game's own over S1, as copyonly; not counted as own / miss)
    const bool masked = di.ord >= 0 && di.ord < 64 && !(g_f.ho_dropmask >> di.ord & 1) && g_f.ho_mode != 3 && g_f.ho_mode != 4 && !own && !di.miss;
    if (masked) bump(g_ho_masked_kept);
    bool drop = g_f.ho_mode != 3 && g_f.ho_mode != 4 && !masked && !own && di.ord >= 0 && !di.miss && region_bind_ok(cl);  // (the review's SF2: only on the four's bind)
    if (drop && g_f.ho_nown) {  // (the review's NIT: a key one of pass 1's own draws had is not in S1)
        uint64_t kv_ = 0, k_ = 0;
        keys::make(cl, t_pso, args, &kv_, &k_);
        for (uint32_t i = 0; i < g_f.ho_nown && i < static_cast<uint32_t>(kHoOwnKeys) && drop; ++i)
            if (g_ho_own_keys[i] == kv_) {
                drop = false;
                bump(g_ho_own_keep);
            }
    }
    if (!drop && !masked && g_f.ho_mode != 3 && g_f.ho_mode != 4) {
        bump(own ? g_ho_own2 : di.ord < 0 ? g_ho_noord2 : g_ho_miss2);
        if (g_f.ho_strict) g_f.why |= kXHo;  // (strict: a frame with the player's own draws or a miss in pass 2 is not compared)
    }
    const bool r1 = g_f.rcmp && !g_f.ho_h1 && ho_r1_ok(cl, di);  // (GOAL-single-pass-2 item 4: an H1 frame draws nothing into R1, T stays)
    g_r.ho_r1_w = (di.flags & (pso::main_inst_flags::kDepthWrite | pso::main_inst_flags::kStencilWrite)) != 0;
    if (r1 && g_r.ho_r1_w) {  // (r120 HG3)
        bump(g_ho_r1_dsw);
        if (di.ord >= 0 && di.ord < 64) bump(g_ho_r1_dsw_n[di.ord]);
    }
    if (g_f.rcmp && !g_f.ho_h1 && !r1) {
        bump(g_ho_r1_skipped);
        g_f.why |= kXHo;  // (R1 cannot take it: no compare this frame)
    }
    if (!drop) {
        if (r1) {
            g_r.ho_r1 = true;
            g_r.ho_own = own && g_f.ho_mask;
            return kDuplicate;
        }
        return kNone;
    }
    if (d3d::thread_predicated()) {  // (pass 2 is not expected to be: S1 holds its twin under pass 1's predicate; the compare excluded)
        bump(g_ho_pred_drops);
        g_f.why |= kXHo;
    }
    if (r1) ho_r1_draw(cl, args);  // (in place of the game's: R1 = two-pass)
    if (g_f.ho_pred1 && !g_f.ho_drops) bump(g_ho_pred_risk);
    ++g_f.ho_drops;
    bump(g_ho_dropped);
    return kDrop;
}
// (item 8f's remap) the remap may start at pass 1's first listed draw of an e1f frame, before the seed: the twins' predicate live (a skipped
// draw's twin keeps the game's predicate, as a drop), the walk cut tracking the barriers (the depth's planes followed; the barrier hook
// sees each batch before the follow), no pre-pass region open, a DIRECT list. The depth's planes are checked by seed()
bool rm_open_ok(ID3D12GraphicsCommandList* cl) {
    const int r = !g_f.e1f_live ? kRoPred : (!prepass::cut_tracking() || prepass::active()) ? kRoTrack : cl->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ? kRoType : -1;
    if (r >= 0) bump(g_rm_refused[r]);
    return r < 0;
}
// a listed draw while S0 stands in: the game's draw not issued (kDrop); its twin both views into S, or (no twin, or the single control) the
// game's pipeline into S0 only (no twin: the frame refused); the list's live predicate kept; the pipeline, bind and mask put back
int rm_draw(ID3D12GraphicsCommandList* cl, const d3d::DrawArgs& args) {
    g_pd_note.active = false;  // (a held predicate stays set: the twin draws under it)
    const int ord = g_r.cur_ord;
    // (the rmdrop control: nothing into the game's targets, S0 or S1; "rmdrop all", the ordinal -2: every listed draw while the remap stands)
    ++g_f.e1f_rm_skip;
    if (g_f.ck == kCkRmDrop && (g_f.e1f_single == -2 || (ord >= 0 && ord == g_f.e1f_single))) {
        // (r116 G3: a dropped draw has the twin path's side effects without its draw: an instprev twin's "ok" marker spent, as rm_draw
        // spends it after drawing one, so the next draw before the next marker is refused alike)
        uint32_t v = 0;
        ID3D12PipelineState* tw = pso::region_twin_of(t_pso, cl, rg_formats(), g_f.ip, &v);
        if (tw && ((v >> 8) & pso::region::kKIp) != 0) inst_prev::consume_draw_ok();
        bump(g_rm_dropped);
        bump(g_rm_skipped);
        return kDrop;
    }
    const bool single = g_f.ck == kCkSingle && ord >= 0 && ord == g_f.e1f_single;
    ID3D12GraphicsCommandList1* c1 = nullptr;
    ID3D12PipelineState* twin = nullptr;
    bool ip_twin = false;
    if (!single) {
        uint32_t v = 0;
        twin = pso::region_twin_of(t_pso, cl, rg_formats(), g_f.ip, &v);
        ip_twin = twin && ((v >> 8) & pso::region::kKIp) != 0;
        const uint32_t code = (v & 0xff) < pso::region::kVerdicts ? (v & 0xff) : 0;
        bump(g_rv[code]);
        const char* why = nullptr;
        if (!twin) {
            bump(g_p_no_twin);
            why = kRvWhy[code];
        } else if ((c1 = c1_of(cl)) == nullptr) {
            bump(g_p_no_cl1);
            twin = nullptr;
            why = "no ID3D12GraphicsCommandList1 (SetViewInstanceMask)";
        }
        if (!twin) {  // (the remap: this draw by the game's pipeline into S0 only)
            g_f.why |= kXRefused;
            g_r.ok = false;
            refuse(why);
            bump(g_rm_single);
        }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rt[3], ds{};
    unsigned mask = 0;
    if (twin) bump(d3d::known_view_instance_mask(cl, &mask) ? g_mask_known : g_mask_unknown);
    if (g_rm_ro && g_draw_writes) bump(g_rm_ro_write_draws);  // (rg_list's: the pipeline writes depth or stencil)
    if (twin) {
        for (int k = 0; k < kDepth; ++k) rt[k] = g_a[kS][k].view[2];
        ds = s_dsv(2, g_rm_ro ? 3 : e1f_depth_ro_now());  // (the game's depth's planes at this draw)
        pso::set_raw(cl, twin);
        d3d::raw_bind(cl, 3, rt, FALSE, &ds);
        c1->SetViewInstanceMask(3);
        d3d::raw_draw(cl, args);
        c1->SetViewInstanceMask(mask);
        pso::set_raw(cl, t_pso);
        if (ip_twin) {
            inst_prev::consume_draw_ok();
            bump(g_rg_ip_twins);
        }
        bump(g_p_twin);
        bump(g_rg_twin);
        ++g_r.twins;
    } else {
        for (int k = 0; k < kDepth; ++k) rt[k] = g_a[kS][k].view[0];
        ds = s_dsv(0, g_rm_ro ? 3 : e1f_depth_ro_now());
        d3d::raw_bind(cl, 3, rt, FALSE, &ds);
        d3d::raw_draw(cl, args);
        if (single) bump(g_e1f_single_draws);
    }
    d3d::raw_bind(cl, t_bind.n, t_bind.rts, FALSE, &t_bind.ds);  // the game's bind back (check_bind proved it is the four)
    bump(g_rm_skipped);
    return kDrop;
}
// a non-listed non-empty e1 draw while S0 stands in (the player's own, a pair without an ordinal): the four bound with the arrays' formats
// and the pipeline's: drawn again into S0 after the game's (rg_after); a bind holding none of the four (its views known): nothing; any
// other (one of the four held, a view not known, the formats not the arrays'): the remap closed first, the draw then the game's alone
int rm_other(ID3D12GraphicsCommandList* cl, const main_region::DrawInfo& di) {
    Bound b;
    if (!check_bind(cl, &b)) {
        bool ok = true;  // (under the game's own live predicate, as its draw)
        for (int k = 0; k < kKinds && ok; ++k)
            ok = g_a[kS][k].game == b.res[k] && (b.vf[k] != DXGI_FORMAT_UNKNOWN ? b.vf[k] : g_a[kS][k].fmt) == g_a[kS][k].vf;
        const uint32_t fmt = rg_formats();
        ok = ok && ((di.flags >> 16) & 0xf) == 3 && (fmt & pso::region::kFmtSingle) && di.fmt == fmt;
        if (ok) {
            if (g_rm_ro && (di.flags & (pso::main_inst_flags::kDepthWrite | pso::main_inst_flags::kStencilWrite))) bump(g_rm_ro_write_draws);
            g_rm_dup = true;
            return kDuplicate;
        }
        rm_end(cl, kRmBind);
        return kNone;
    }
    const Bind& x = t_bind;
    bool touch = x.cl != cl || x.single;  // (a bind not seen, or one descriptor range: not known)
    auto four = [](const void* r) {
        for (int k = 0; k < kKinds; ++k)
            if (r == g_a[kS][k].game) return true;
        return false;
    };
    for (unsigned i = 0; i < x.n && !touch; ++i) {
        const void* r = d3d::view_resource(x.rts[i].ptr);
        touch = !r || four(r);
    }
    if (!touch && x.has_ds) {
        const void* r = d3d::view_resource(x.ds.ptr);
        touch = !r || four(r);
    }
    if (touch) rm_end(cl, kRmBind);
    return kNone;
}
int rg_before(ID3D12GraphicsCommandList* cl, const d3d::DrawArgs& args) {
    main_region::DrawInfo di;
    main_region::draw(cl, args, cl == t_pso_cl ? t_pso : nullptr, &di);  // (8d's classification and keys, every frame)
    if (!di.in_e1 || di.empty) {
        if (g_rm_on && !di.in_e1) rm_end(cl, kRmOutside);  // (the review's SF1: pass 1's e1 end lost; a later draw may write the four)
        return kNone;
    }
    if (g_r.pass == 1 && g_f.e1f_rm_started && !g_rm_on && !g_rm_owed) ++g_f.e1f_rm_after;  // (r116 G3's diagnosis: drawn by the game)
    if (g_f.e1f && g_r.pass == 2) {  // ([Stereo] MainRegionE1Compare, item 8f: T, the own draws into S1; item 8g: the handover)
        int r = kNone;
        if (g_f.ho && g_f.ho_state != kHsRefused) r = ho_before2(cl, di, args);
        else r = g_f.rcmp ? e1f_before2(cl, di) : kNone;  // (a refused handover frame that is no compare frame: two-pass)
        // ("e1f c2pred null") after T's and the handover's own copies (their predicate rules their own): the game's draw unpredicated
        if (g_f.c2null && r != kDrop && d3d::thread_predicated()) {
            d3d::Predicate pr;
            if (d3d::thread_predicate(cl, &pr) != d3d::kPredSet || !pr.res) {
                bump(g_e1f_c2pred_unknown);
                g_f.why |= kXPred;  // (the record not known: left live, the compare excluded)
            } else {
                d3d::raw_set_predication(cl, nullptr, 0, 0);
                g_r.c2_nulled = true;
                g_r.c2_pd = pr;
                bump(g_e1f_c2pred_nulled);
                r = kDuplicate;  // (after_draw puts it back; rg_after only when the branch asked for its draws)
            }
        }
        if (g_f.gt && r != kDrop && g_gt_st.load(std::memory_order_relaxed) == kTrRec) {  // ("e1f g1trace") stream 2 (and 3)
            GtSlot m;
            m.ord = di.ord;
            m.a = args;
            m.own = g_own_pb > 0;
            m.noord = di.ord < 0;
            m.miss = di.miss;
            if (!g_f.gt_t) {  // (T: the game's FSC before pass 2's first e1 draw; the branch's own copies leave it as it was)
                g_f.gt_t = true;
                GtSlot tm = m;
                tm.t = true;
                gt_game(cl, tm, g_r.c2_nulled);
            }
            g_r.gt_after = true;  // (after_draw: the game's FSC after its draw; rg_after only when the branch asked for its draws)
            g_r.gt_meta = m;
            r = kDuplicate;
        }
        return r;
    }
    if (g_rm_on) {  // (item 8f's remap: S0 stands in)
        if (cl != g_rm_cl || GetCurrentThreadId() != g_rm_tid) {
            rm_end(cl, kRmList);
        } else if (g_own_pb > 0 || di.ord < 0) {
            bump(g_own_pb > 0 ? g_rg_own : g_rg_noord);
            return rm_other(cl, di);
        }
    }
    if (g_own_pb > 0) {  // (item 8e: an entity body.cpp edits per pass: each pass draws its own)
        bump(g_rg_own);
        if (g_f.ho && g_r.pass == 1) {  // (item 8g, the review's NIT: pass 1's own keys, so pass 2 never drops a draw that matches one)
            uint64_t kv_ = 0, k_ = 0;
            keys::make(cl, t_pso, args, &kv_, &k_);
            if (g_f.ho_nown < static_cast<uint32_t>(kHoOwnKeys)) g_ho_own_keys[g_f.ho_nown] = kv_;
            ++g_f.ho_nown;
        }
        return kNone;
    }
    if (di.ord < 0) {  // (a pair past 63, or no pipeline known: never listed; joint frames with one are excluded by the verdict)
        bump(g_rg_noord);
        if (g_r.pass == 1) ++g_f.noord1;  // (item 8g: the decision's rule)
        return kNone;
    }
    if (g_f.dup) rg_seed_early(cl, di);  // (solo and subset: the seed at e1's first non-empty draw)
    if (!g_f.dup || !(g_f.subset >> di.ord & 1)) return kNone;  // (not a dup frame: nothing more; not in the subset: the game's alone)
    const uint64_t t0 = __rdtsc();
    const int r = rg_list(cl, di);
    g_rg_tsc.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
    if (g_f.e1f && g_r.pass == 1) {  // (item 8f's remap)
        d3d::Predicate pr0;
        const int pst = r == kDuplicate && !g_rm_on && !g_rm_owed && g_f.e1f_rm_ready && !g_f.e1f_rm_started ? d3d::thread_predicate(cl, &pr0) : d3d::kPredNone;
        if (r == kDuplicate && !g_rm_on && !g_rm_owed && g_f.e1f_rm_ready && !g_f.e1f_rm_started && pst == d3d::kPredUnknown) {
            g_f.e1f_rm_started = true;  // (the list's predicate record not known: no remap this frame, nothing skipped)
            bump(g_rm_refused[kRoPredRec]);
        }
        if (r == kDuplicate && !g_rm_on && !g_rm_owed && g_f.e1f_rm_ready && !g_f.e1f_rm_started) {  // (its start: S0 seeded at this draw)
            g_rm_pred = pst == d3d::kPredSet ? pr0 : d3d::Predicate{};
            g_rm_ro = g_f.e1f_rm_ro;
            if (g_rm_ro) bump(g_rm_ro_frames);
            g_rm_on = true;
            g_rm_cl = cl;
            g_rm_tid = GetCurrentThreadId();
            g_f.e1f_rm_started = true;
            bump(g_rm_frames);
        }
        if (g_rm_on) {
            if (r == kDuplicate) return rm_draw(cl, args);
            rm_end(cl, kRmRefused);  // (refused: the game draws its own, after the close)
        }
    }
    return r;
}
// a listed draw after the game's: pass 1 the twin (region_twin_of; view mask 3) into S's both slices, or on a swap frame the swapped
// pair's game pipeline into S0 then S1; then the game's pipeline into R0 (pass 2: R1); the pipeline, the bind, the view-instance mask
// and the predicate back as before
void rg_after(ID3D12GraphicsCommandList* cl, const d3d::DrawArgs& args) {
    D3D12_CPU_DESCRIPTOR_HANDLE rt[3], ds{};
    if (g_r.ho_r1) {  // (item 8g) a compare frame's pass-2 draw drawn by the game: again into R1 (two-pass's reference)
        g_r.ho_r1 = false;
        ho_r1_draw(cl, args);
        if (g_r.ho_own) {  // (H2m's diagnosis: the own draw again into R0's colour, depth-tested against R1's depth, read-only: no write there)
            g_r.ho_own = false;
            const Arr& rd_ = g_a[kR][kDepth];
            if (rd_.ro_ok) {
                for (int k = 0; k < kDepth; ++k) rt[k] = g_a[kR][k].view[0];
                ds = rd_.ro[2][1];
                d3d::raw_bind(cl, 3, rt, FALSE, &ds);
                d3d::raw_draw(cl, args);
                d3d::raw_bind(cl, t_bind.n, t_bind.rts, FALSE, &t_bind.ds);
                bump(g_ho_mask_draws);
            }
        }
        return;
    }
    if (g_rm_dup) {  // (item 8f's remap) a non-listed e1 draw of pass 1: the game's pipeline again into S's slice-0 view (S0 stands in)
        g_rm_dup = false;
        for (int k = 0; k < kDepth; ++k) rt[k] = g_a[kS][k].view[0];
        ds = s_dsv(0, g_rm_ro ? 3 : e1f_depth_ro_now());
        d3d::raw_bind(cl, 3, rt, FALSE, &ds);
        d3d::raw_draw(cl, args);
        d3d::raw_bind(cl, t_bind.n, t_bind.rts, FALSE, &t_bind.ds);  // the game's bind back (check_bind proved it is the four)
        bump(g_rm_own_s0);
        return;
    }
    if (g_r.e1f_own) {  // ([Stereo] MainRegionE1Compare, item 8f) pass 2's own draw: the game's pipeline again into S's slice-1 view (the swap's pattern)
        g_r.e1f_own = false;
        for (int k = 0; k < kDepth; ++k) rt[k] = g_a[kS][k].view[1];
        ds = s_dsv(1, e1f_depth_ro_now());  // (S1 as read-only as the game's depth now)
        d3d::raw_bind(cl, 3, rt, FALSE, &ds);
        d3d::raw_draw(cl, args);
        d3d::raw_bind(cl, t_bind.n, t_bind.rts, FALSE, &t_bind.ds);  // the game's bind back (check_bind proved it is the four, as noted)
        bump(g_e1f_own_draws);
        return;
    }
    const uint64_t t0 = __rdtsc();
    const bool p1 = g_r.pass == 1;
    const PdNote pd = g_pd_note;  // ([Stereo] MainPredication) this draw's held predicate, if any
    g_pd_note.active = false;
    const int ord = g_r.cur_ord;
    const bool swap = p1 && ord >= 0 && ord == g_f.swap;
    // ([Stereo] MainRegionE1Compare, item 8f) an e1f frame: R gets no duplicates; the single control's pair by the game's pipeline into S0
    // only (no twin, no QueryInterface, no mask); the twins under the list's own live predicate unless "region e1f pred null"
    const bool e1f = g_f.e1f;
    const bool single = p1 && e1f && g_f.ck == kCkSingle && ord >= 0 && ord == g_f.e1f_single;
    if (p1 && e1f && g_f.ck == kCkRmDrop && (g_f.e1f_single == -2 || (ord >= 0 && ord == g_f.e1f_single))) bump(g_rm_drop_noremap);  // (rmdrop without the remap: nothing)
    ID3D12GraphicsCommandList1* c1 = nullptr;
    ID3D12PipelineState* twin = nullptr;
    bool ip_twin = false;  // (S3) an instprev twin: its draw's "ok" marker spent once drawn
    if (p1 && !swap && !single) {
        uint32_t v = 0;
        twin = pso::region_twin_of(t_pso, cl, rg_formats(), g_f.ip, &v);
        ip_twin = twin && ((v >> 8) & pso::region::kKIp) != 0;
        const uint32_t code = (v & 0xff) < pso::region::kVerdicts ? (v & 0xff) : 0;
        bump(g_rv[code]);
        if (!twin) {
            bump(g_p_no_twin);
            if (pd.active) bump(g_pd_skipped);
            g_f.why |= kXRefused;
            g_r.ok = false;
            refuse(kRvWhy[code]);
            return;
        }
        if ((c1 = c1_of(cl)) == nullptr) {
            bump(g_p_no_cl1);
            if (pd.active) bump(g_pd_skipped);
            g_f.why |= kXRefused;
            g_r.ok = false;
            refuse("no ID3D12GraphicsCommandList1 (SetViewInstanceMask)");
            return;
        }
    }
    bool nulled = false;
    if (pd.active) {
        if (pd.cl != cl) {  // (never expected: the note is this draw's)
            pd_stand_down("the held predicate is another list's at its duplicates");
            g_f.why |= kXPred;
        } else if (e1f && g_f.e1f_live) {  // (item 8f: the twin draws under the game's draw's own predicate, as a drop would; item8.md 2.5 row 5)
            bump(g_e1f_pred_live);
            if (g_f.ho) g_f.ho_pred1 = true;  // (item 8g's risk count)
        } else {
            d3d::raw_set_predication(cl, nullptr, 0, 0);
            nulled = true;
            bump(g_pd_nulled);
        }
    }
    unsigned mask = 0;  // (the list's view-instance mask before the twin, put back after it: as after_draw's)
    if (twin) bump(d3d::known_view_instance_mask(cl, &mask) ? g_mask_known : g_mask_unknown);
    const bool marg = g_f.marg && twin;  // (gpu_stages' marginal stamps: before the twin, between it and R0, after R0)
    if (marg) gpu_stages::marginal(cl, 0, ord);
    if (twin) {  // the twin, both views, into S's both-slice views
        if (const int q = d3d::thread_open_queries(cl); q != 0) {  // (E5, every dup frame: a query open over the twin, or not known)
            bump(g_e1f_twin_open_query);
            if (q < 0) bump(g_e1f_twin_query_unknown);
        }
        for (int k = 0; k < kDepth; ++k) rt[k] = g_a[kS][k].view[2];
        ds = e1f ? s_dsv(2, e1f_depth_ro_now()) : g_a[kS][kDepth].view[2];  // (item 8f: S as read-only as the game's depth now)
        pso::set_raw(cl, twin);
        d3d::raw_bind(cl, 3, rt, FALSE, &ds);
        c1->SetViewInstanceMask(g_f.twin_mask);  // ("dupall mask": 1 or 2 on a dupall frame; "twins mask 2": 2 on an e1f frame; 3 otherwise)
        if (g_f.twin_mask != 3) bump(g_f.twin_mask_sw ? g_e1f_twins_mask2 : g_e1f_dupall_mask_twins);
        d3d::raw_draw(cl, args);
        c1->SetViewInstanceMask(mask);
        pso::set_raw(cl, t_pso);  // (the game's pipeline back before any single-slice view meets it)
        if (ip_twin) {  // (S3: the "ok" marker played before this draw's bind is this draw's only; a second draw on it is refused)
            inst_prev::consume_draw_ok();
            bump(g_rg_ip_twins);
        }
        bump(g_p_twin);
        bump(g_rg_twin);
        ++g_r.twins;
        // (the trace copies never under a predicate: none on an e1f frame whose twins keep a live one; never a dupall frame drawn
        // with a mask other than 3: its S slice 1 would not hold the pair; nor any twin "twins mask 2" drew)
        if (g_tr_st.load(std::memory_order_relaxed) >= kTrArmed && g_f.dup && g_f.twin_mask == 3 && ord == g_tr_ord.load(std::memory_order_relaxed) &&
            (!e1f || !pd.active || nulled)) {
            int want = kTrArmed;  // (the first traced draw of a dup frame starts the trace: its frame)
            if (g_tr_st.compare_exchange_strong(want, kTrRec, std::memory_order_acq_rel)) {
                g_tr_bx0 = g_tr_x0;  // (the snapshot: the command thread writes the fields only in Setup)
                g_tr_by0 = g_tr_y0;
                g_tr_bw = g_tr_w;
                g_tr_bh = g_tr_h;
                g_tr_frame.store(state::presents.load());
                g_tr_pn[1] = g_tr_pn[2] = 0;
                g_tr_pi[1] = g_tr_pi[2] = 0;
                g_tr_ni.store(0, std::memory_order_relaxed);
            }
            if (g_tr_st.load(std::memory_order_relaxed) == kTrRec) tr_copy(cl, kS, args, nulled);
        }
        if (g_f.gt && p1 && g_gt_st.load(std::memory_order_relaxed) == kTrRec) {  // ("e1f g1trace") stream 1: S1's box after this twin
            GtSlot m;
            m.ord = ord;
            m.a = args;
            if (pd.active && !nulled) {  // (the twin under the list's live predicate: nulled around the copy, then put back)
                d3d::raw_set_predication(cl, nullptr, 0, 0);
                gt_copy(cl, g_a[kS][kFsc].arr, 1, 0, m);
                d3d::raw_set_predication(cl, pd.p.res, pd.p.off, pd.p.op);
            } else {  // (none held, or held and nulled already)
                gt_copy(cl, g_a[kS][kFsc].arr, 1, 0, m);
            }
        }
        if (nulled) bump(g_pd_dup_twin);
    }
    if (swap) {  // (the swap: no twin, no QueryInterface, no mask; the game's pipeline into S0 then S1, R's pattern)
        for (int s = 0; s < 2; ++s) {
            for (int k = 0; k < kDepth; ++k) rt[k] = g_a[kS][k].view[s];
            ds = g_a[kS][kDepth].view[s];
            d3d::raw_bind(cl, 3, rt, FALSE, &ds);
            d3d::raw_draw(cl, args);
        }
        bump(g_rg_swap_draws);
    }
    if (single) {  // (item 8f, the single control: the pair's draw by the game's pipeline into S0 only; S1 lacks it)
        for (int k = 0; k < kDepth; ++k) rt[k] = g_a[kS][k].view[0];
        ds = s_dsv(0, e1f_depth_ro_now());
        d3d::raw_bind(cl, 3, rt, FALSE, &ds);
        d3d::raw_draw(cl, args);
        bump(g_e1f_single_draws);
    }
    if (marg) gpu_stages::marginal(cl, 1, ord);
    if (!e1f) {  // (item 8f: an e1f frame's R0 is T and its R1 the seed, so R draws nothing)
        const int sl = p1 ? 0 : 1;  // the game's own pipeline into R's slice for this pass
        for (int k = 0; k < kDepth; ++k) rt[k] = g_a[kR][k].view[sl];
        ds = g_a[kR][kDepth].view[sl];
        if (g_refdepth_game.load(std::memory_order_relaxed) && t_bind.has_ds) {  // (the diagnostic: the game's depth view, no write)
            if (g_draw_writes) {
                bump(g_refdepth_writes);
            } else {
                ds = t_bind.ds;
                bump(g_refdepth_draws);
            }
        }
        d3d::raw_bind(cl, 3, rt, FALSE, &ds);
        d3d::raw_draw(cl, args);
        bump(g_p_ref[g_r.pass]);
        if (!p1 && g_tr_st.load(std::memory_order_relaxed) == kTrRec && ord == g_tr_ord.load(std::memory_order_relaxed)) tr_copy(cl, kR, args, nulled);
        if (nulled) bump(g_pd_dup_ref);
    }
    if (marg) gpu_stages::marginal(cl, 2, ord);
    d3d::raw_bind(cl, t_bind.n, t_bind.rts, FALSE, &t_bind.ds);  // the game's bind back (check_bind proved it is the four, as noted)
    if (nulled) {  // ([Stereo] MainPredication) the list's own predicate back, then the record checked against what was held
        d3d::raw_set_predication(cl, pd.p.res, pd.p.off, pd.p.op);
        bump(g_pd_restored);
        d3d::Predicate q;
        if (d3d::thread_predicate(cl, &q) != d3d::kPredSet || q.res != pd.p.res || q.off != pd.p.off || q.op != pd.p.op || !d3d::thread_predicated()) {
            bump(g_pd_after_bad);
            pd_stand_down("the list's record moved across the duplicates");
            g_f.why |= kXPred;
        }
    }
    g_rg_tsc.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
    bump(g_rg_tsc_n);
}
}  // namespace

int before_draw(ID3D12GraphicsCommandList* cl, const d3d::DrawArgs& args) {
    if (!g_r.open || GetCurrentThreadId() != g_r.tid) return kNone;
    pso::walk_draw(cl, g_r.pass);  // (GOAL-single-pass item 6c: the latch's second stage at pass 1's first draw; off: one load)
    // ([Stereo] MainRegion, item 8d, test only; off: one relaxed load) observe: every draw of the region classified (pass 1's e1) or
    // keyed and tagged (pass 2's), and nothing else: no seed, no duplicate, whatever MainFamily lists
    // (item 8f's remap) a draw the region's listing would not see while S0 stands in (never expected inside the region): the remap closed first
    if (g_rm_on && !(main_region::observing() && g_f.rg)) rm_end(cl, kRmBind);
    if (main_region::observing()) {
        if (g_f.rg) return rg_before(cl, args);  // ([Stereo] MainFamily=region, item 8e: "mr compare" lists e1's draws on dup frames)
        main_region::draw(cl, args, cl == t_pso_cl ? t_pso : nullptr);
        return kNone;
    }
    uint32_t flags = 0, serial = 0;
    if (!t_pso || cl != t_pso_cl || !pso::main_listed(t_pso, &flags, &serial)) return kNone;
    const uint64_t idx = d3d::thread_draws() - g_r.d0;  // (this draw's: counted before the call)
    if (g_r.listed && idx != g_r.last + 1) g_r.contig = false;
    if (!g_r.listed) g_r.first = idx;
    g_r.last = idx;
    ++g_r.listed;
    g_draw_writes = (flags & (pso::main_inst_flags::kDepthWrite | pso::main_inst_flags::kStencilWrite)) != 0;
    bump(g_p_listed[g_r.pass]);
    if (!g_r.pso0) g_r.pso0 = t_pso;
    else if (t_pso != g_r.pso0) g_r.mixed = true;
    if (g_r.pass == 2 && pso::allow_latched() && pso::main_would_drop(t_pso, g_r.listed - 1)) ++g_r.would;  // (item 6c's check)
    if (g_uc_installed.load(std::memory_order_relaxed)) uc_note(cl, args);  // ([Stereo] MainUnionCull: the draw's key, whatever the frame)
    g_pd_note.active = false;
    if (g_r.handed) return dropped(cl, args);  // ([Stereo] MainSlice) merged at the first: dropped up to listed1
    if (d3d::thread_predicated()) {
        // ([Stereo] MainPredication, item 6d) the list's predicate held for after_draw: the duplicates nulled, then it put back
        if (!pd_hold(cl)) {  // (billboard.md 8.4: none expected for the billboards; the frame excluded, the predicate untouched)
            bump(g_p_predicated);
            g_f.why |= kXPred;
            g_r.ok = false;
            return kNone;
        }
    } else if (g_pd_installed.load(std::memory_order_relaxed) && !g_pd_stood.load(std::memory_order_relaxed)) {
        d3d::Predicate p;  // (the converse: the record says predicated where the hook's flag does not)
        if (d3d::thread_predicate(cl, &p) == d3d::kPredSet) {
            bump(g_pd_unpred_set);
            pd_stand_down("the list's record holds a predicate where thread_predicated() says none");
        }
    }
    if (!g_r.ok) return kNone;
    Bound b;
    const char* why = check_bind(cl, &b);
    // (a main twin never shared with the pre-pass: this frame's pre-pass regions latched the withholding; twin_of serves only
    // them. The delta review: checked before the seed, so a refused pass 1 touches neither the arrays nor the game's depth)
    if (!why && g_r.pass == 1 && prepass::on() && !pso::main_withheld()) why = "the family's twins not withheld from this frame's pre-pass yet";
    if (!why && g_r.pass == 1 && !g_f.seeded) {
        // (the 6d/6e review's must-fix) a held predicate nulled across the seed: its clears and copies are predicated operations,
        // skipped where the entity's query reads 0 (S and R then the last frame's); put back after (the record unchanged)
        const bool pd_seed = g_pd_note.active && g_pd_note.cl == cl;
        if (pd_seed) d3d::raw_set_predication(cl, nullptr, 0, 0);
        why = seed(cl, flags, b);
        if (pd_seed) {
            d3d::raw_set_predication(cl, g_pd_note.p.res, g_pd_note.p.off, g_pd_note.p.op);
            if (!why) bump(g_pd_seed_nulled);  // (the delta review: counted as g_p_seeds is, a seed made)
        }
        if (!why) {
            g_f.seeded = true;
            g_f.gen = g_gen;
        } else if (g_p_make_failed.load()) {
            stand_down(why);  // (an allocation failed: not tried again this session)
        }
    }
    if (!why && (!g_f.seeded || g_f.gen != g_gen)) why = "not seeded this frame (or the arrays re-made since)";
    if (!why)
        for (int k = 0; k < kKinds && !why; ++k)
            if (g_a[kS][k].game != b.res[k]) why = "a bound target is not the arrays' game target";
    if (why) {
        bump(g_p_refused);
        g_f.why |= kXRefused;
        g_r.ok = false;
        refuse(why);
        return kNone;
    }
    if (g_f.slice && g_r.listed == 1) {  // ([Stereo] MainSlice)
        // pass 1: S slice 1's SRVs (after the seed made the arrays) and the merge's worker started, so pass 2 (this frame's or a later one's) finds them
        if (g_r.pass == 1) {
            srv_step();
            Merge* m = nullptr;
            if (g_srv_gen == g_gen && !g_sl_stood.load()) (void)merge_for(t_pso, serial, &m);
        } else {
            // pass 2, the drop point: merged and dropped, or (refused) drawn by the game as before, this frame excluded
            return handover(cl, b, args, flags, serial);
        }
    }
    return kDuplicate;
}

void after_draw(ID3D12GraphicsCommandList* cl, const d3d::DrawArgs& args) {
    if (g_f.rg) {  // ([Stereo] MainFamily=region, item 8e: only its listed path returns kDuplicate on a region frame)
        const bool c2 = g_r.c2_nulled;  // ("e1f c2pred null": rg_after only for the draws its branch asked for, R1's or S1's)
        const bool gt = g_r.gt_after, own_s1 = g_r.e1f_own;  // ("e1f g1trace": likewise; own_s1: rg_after draws it again into S1)
        if ((!c2 && !gt) || g_r.ho_r1 || g_r.e1f_own) rg_after(cl, args);
        if (gt) {  // (before c2pred's put-back: a nulled list stays nulled across the copies)
            g_r.gt_after = false;
            if (g_gt_st.load(std::memory_order_relaxed) == kTrRec) {
                gt_game(cl, g_r.gt_meta, c2);
                if (own_s1) gt_copy_unpred(cl, g_a[kS][kFsc].arr, 1, 2, g_r.gt_meta, c2);
            }
        }
        if (c2) {  // (the list's own predicate back, then the record checked against what was noted: rg_after's rule for the twins)
            g_r.c2_nulled = false;
            const d3d::Predicate& p = g_r.c2_pd;
            d3d::raw_set_predication(cl, p.res, p.off, p.op);
            d3d::Predicate q;
            if (d3d::thread_predicate(cl, &q) != d3d::kPredSet || q.res != p.res || q.off != p.off || q.op != p.op || !d3d::thread_predicated()) {
                bump(g_e1f_c2pred_bad);
                pd_stand_down("the list's record moved across a c2pred-nulled draw");
                g_f.why |= kXPred;
            }
        }
        return;
    }
    ID3D12GraphicsCommandList1* c1 = nullptr;
    ID3D12PipelineState* twin = nullptr;
    const bool p1 = g_r.pass == 1;
    const PdNote pd = g_pd_note;  // ([Stereo] MainPredication) this draw's held predicate, if any
    g_pd_note.active = false;
    const bool refs = pd.active && g_pd_mode.load(std::memory_order_relaxed) == 2;  // (the "refs" mode: R0 without a twin)
    if (p1) {
        twin = pso::main_twin_of(t_pso, cl, g_r.listed - 1);
        if (!twin && refs) {  // (the frame excluded; this region goes on: the references still drawn, the predicate nulled and put back)
            bump(g_p_no_twin);
            bump(g_pd_refonly);
            g_f.why |= kXRefused;
            refuse("the family's pipeline has no twin (MainPredication refs: R0 drawn alone)");
        } else if (!twin) {
            bump(g_p_no_twin);
            if (pd.active) bump(g_pd_skipped);
            g_f.why |= kXRefused;
            g_r.ok = false;
            refuse("the family's pipeline has no twin");
            return;
        } else if (FAILED(cl->QueryInterface(IID_PPV_ARGS(&c1))) || !c1) {
            bump(g_p_no_cl1);
            if (pd.active) bump(g_pd_skipped);
            g_f.why |= kXRefused;
            g_r.ok = false;
            refuse("no ID3D12GraphicsCommandList1 (SetViewInstanceMask)");
            return;
        }
    }
    // ([Stereo] MainPredication, item 6d) the duplicates unpredicated: the game's own draw above kept its predicate; it is put back
    // below, after the last duplicate (the record unchanged: the raw call passes the hook by)
    bool nulled = false;
    if (pd.active) {
        if (pd.cl != cl) {  // (never expected: the note is this draw's)
            pd_stand_down("the held predicate is another list's at its duplicates");
            g_f.why |= kXPred;
        } else {
            d3d::raw_set_predication(cl, nullptr, 0, 0);
            nulled = true;
            bump(g_pd_nulled);
        }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rt[3], ds{};
    // (the review's should-fix 5) the list's view-instance mask before the twin, put back after it: the one the mod set on
    // this list (prepass_arrays' instanced region leaves 3), else 0. [I] 0 is the initial mask of a reset list: the game
    // never sets one, and only view-instanced pipelines with masking read it (the twins, each region setting its own first)
    unsigned mask = 0;
    if (p1 && twin) bump(d3d::known_view_instance_mask(cl, &mask) ? g_mask_known : g_mask_unknown);
    if (p1 && twin) {  // the twin, both views, into S's both-slice views
        for (int k = 0; k < kDepth; ++k) rt[k] = g_a[kS][k].view[2];
        ds = g_a[kS][kDepth].view[2];
        pso::set_raw(cl, twin);
        d3d::raw_bind(cl, 3, rt, FALSE, &ds);
        c1->SetViewInstanceMask(3);
        d3d::raw_draw(cl, args);
        c1->SetViewInstanceMask(mask);
        pso::set_raw(cl, t_pso);  // (the game's pipeline back before any single-slice view meets it)
        c1->Release();
        bump(g_p_twin);
        ++g_r.twins;
        if (nulled) bump(g_pd_dup_twin);
    }
    const int sl = p1 ? 0 : 1;  // the game's own pipeline into R's slice for this pass
    for (int k = 0; k < kDepth; ++k) rt[k] = g_a[kR][k].view[sl];
    ds = g_a[kR][kDepth].view[sl];
    if (g_refdepth_game.load(std::memory_order_relaxed) && t_bind.has_ds) {  // (the diagnostic: the game's depth view, no write)
        if (g_draw_writes) {
            bump(g_refdepth_writes);
        } else {
            ds = t_bind.ds;
            bump(g_refdepth_draws);
        }
    }
    d3d::raw_bind(cl, 3, rt, FALSE, &ds);
    d3d::raw_draw(cl, args);
    bump(g_p_ref[g_r.pass]);
    if (nulled) bump(g_pd_dup_ref);
    // the game's bind back (check_bind proved it is the four, as noted)
    d3d::raw_bind(cl, t_bind.n, t_bind.rts, FALSE, &t_bind.ds);
    if (nulled) {  // ([Stereo] MainPredication) the list's own predicate back, then the record checked against what was held
        d3d::raw_set_predication(cl, pd.p.res, pd.p.off, pd.p.op);
        bump(g_pd_restored);
        d3d::Predicate q;
        if (d3d::thread_predicate(cl, &q) != d3d::kPredSet || q.res != pd.p.res || q.off != pd.p.off || q.op != pd.p.op || !d3d::thread_predicated()) {
            bump(g_pd_after_bad);
            pd_stand_down("the list's record moved across the duplicates");
            g_f.why |= kXPred;
        }
    }
    if (!p1 && g_r.ident && g_r.cmp && g_r.listed == g_f.listed1) {  // ([Stereo] MainSlice, the identity control) C at the run's end
        ID3D12Resource* res[kKinds];
        for (int k = 0; k < kKinds; ++k) res[k] = static_cast<ID3D12Resource*>(g_a[kS][k].game);
        take_c(cl, res);
    }
}

void query(ID3D12GraphicsCommandList* cl, bool begin) {  // ([Stereo] MainSlice, the review: a query inside the dropped run)
    (void)cl;
    (void)begin;
    if (g_r.open && g_r.pass == 2 && g_f.ho && g_f.ho_state == kHsHanded && GetCurrentThreadId() == g_r.tid) {  // (item 8g: in the dropped run)
        bump(g_ho_query_in_run);
        g_f.why |= kXHo;
        g_ho_query_until.store(state::presents.load() + kHoQueryHold, std::memory_order_relaxed);  // (the review's NIT: the next frames refused)
    }
    if (!g_r.open || !g_r.in_run || GetCurrentThreadId() != g_r.tid) return;
    bump(g_sl_query_in_run);
    g_f.why |= kXQuery;
}
// (item 8f's remap) a barrier batch while S0 stands in, before the walk cut follows it and before it runs: an owed close first (the poll),
// then a batch that would end the remap (one of the four into a state other than DEPTH_WRITE / DEPTH_READ for the depth, any transition of a
// colour target, a split, UAV or aliasing barrier on them, or another list) closes it before it runs
void pre_barrier(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_RESOURCE_BARRIER* b) {
    if ((!g_rm_on && !g_rm_owed) || !b || GetCurrentThreadId() != g_rm_tid) return;
    if (prepass::cut_pending()) prepass::cut_poll(cl);  // (as the walk cut's track polls before following this batch)
    if (!g_rm_on) return;
    if (cl != g_rm_cl) {
        rm_end(cl, kRmList);
        return;
    }
    int reason = -1;
    for (unsigned i = 0; i < n && reason < 0; ++i) {
        const D3D12_RESOURCE_BARRIER& x = b[i];
        const ID3D12Resource* r = x.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ? x.Transition.pResource
                                  : x.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV      ? x.UAV.pResource
                                                                                   : x.Aliasing.pResourceAfter;
        const ID3D12Resource* r0 = x.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING ? x.Aliasing.pResourceBefore : nullptr;
        int k = -1;
        for (int j = 0; j < kKinds && k < 0; ++j)
            if ((r && r == g_a[kS][j].game) || (r0 && r0 == g_a[kS][j].game)) k = j;
        if (k < 0) continue;
        if (x.Flags & (D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY | D3D12_RESOURCE_BARRIER_FLAG_END_ONLY)) reason = kRmSplit;
        else if (x.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) reason = kRmUav;
        else if (x.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) reason = kRmAlias;
        else if (k != kDepth || (g_rm_ro ? !rm_read_only(x.Transition.StateAfter)  // (read-only open: into any write state closes it)
                                         : x.Transition.StateAfter != D3D12_RESOURCE_STATE_DEPTH_WRITE && x.Transition.StateAfter != D3D12_RESOURCE_STATE_DEPTH_READ))
            reason = kRmState;
        if (reason >= 0) {  // (r116 G3's diagnosis: the closing barrier)
            g_rm_last_tr[0].store(static_cast<uint32_t>(k), std::memory_order_relaxed);
            g_rm_last_tr[1].store(static_cast<uint32_t>(x.Type) | (static_cast<uint32_t>(x.Flags) << 8), std::memory_order_relaxed);
            const bool t = x.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            g_rm_last_tr[2].store(t ? static_cast<uint32_t>(x.Transition.StateBefore) : 0u, std::memory_order_relaxed);
            g_rm_last_tr[3].store(t ? static_cast<uint32_t>(x.Transition.StateAfter) : 0u, std::memory_order_relaxed);
            g_rm_last_tr[4].store(t ? x.Transition.Subresource : 0u, std::memory_order_relaxed);
        }
    }
    if (reason >= 0) rm_end(cl, reason, n, b);
}
// ([Stereo] MainSlice; [Stereo] MainRegionE1Compare, item 8f: between T and C2 the four's barriers watched, e1f_watch_barrier)
void e1f_watch_barrier(const D3D12_RESOURCE_BARRIER& x);
void barrier(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_RESOURCE_BARRIER* b) {
    (void)cl;
    // (H2m's diagnosis) inside e1 of a region frame (the e1f machinery installed): the four moved into a shader-resource state, by pass and kind
    if (b && g_e1f_installed.load(std::memory_order_relaxed) && g_r.open && g_f.rg && !g_r.e1_done && g_r.pass >= 1 && g_r.pass <= 2 &&
        GetCurrentThreadId() == g_r.tid)
        for (unsigned i = 0; i < n; ++i) {
            if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
                !(b[i].Transition.StateAfter & (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)))
                continue;
            for (int k = 0; k < kKinds; ++k)
                if (b[i].Transition.pResource == g_a[kS][k].game) bump(g_e1_srv[g_r.pass][k]);
        }
    if (!g_r.open || (!g_r.in_run && !g_r.e1f_watch) || !b || GetCurrentThreadId() != g_r.tid) return;
    for (unsigned i = 0; i < n; ++i) {
        const ID3D12Resource* r = b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ? b[i].Transition.pResource
                                  : b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV      ? b[i].UAV.pResource
                                                                                      : b[i].Aliasing.pResourceAfter;
        if (g_r.e1f_watch) e1f_watch_barrier(b[i]);  // (item 8f)
        for (int k = 0; k < kKinds; ++k) {
            const bool hit = r && r == g_a[kS][k].game;
            if (hit && g_r.in_run) {  // (the merged state may be changed under the dropped draws: the frame is not compared)
                bump(g_sl_barrier);
                g_f.why |= kXBarrier;
                return;
            }
        }
    }
}

// ([Stereo] MainRegionE1Compare, item 8f; the smoke cycle's change) one barrier between T and C2. A plain transition changes no content:
// counted (colour or depth), and a colour target's last state since pass 2's last e1 draw kept (C2 needs it RENDER_TARGET); a transition
// of one of the four into a write state other than its own (UNORDERED_ACCESS, COPY_DEST, RESOLVE_DEST, STREAM_OUT: a compute, copy or
// resolve may write it, Dispatch and ResolveSubresource not being watched by resource) marks the frame. UAV barriers: counted. An
// aliasing barrier whose resource before or after is one of the four marks it (C2 refused, "barrier")
void e1f_watch_barrier(const D3D12_RESOURCE_BARRIER& x) {
    constexpr D3D12_RESOURCE_STATES kOther = D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST |
                                             D3D12_RESOURCE_STATE_STREAM_OUT;
    auto kind = [](const ID3D12Resource* r) {
        for (int k = 0; r && k < kKinds; ++k)
            if (r == g_a[kS][k].game) return k;
        return -1;
    };
    // (a split barrier's BEGIN_ONLY half on one of the four: mid-transition a copy from it is invalid, so C2 is refused, "split"; its
    // END_ONLY half then needs no case of its own)
    if (x.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) {
        const ID3D12Resource* r = x.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ? x.Transition.pResource
                                  : x.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV      ? x.UAV.pResource
                                                                                   : x.Aliasing.pResourceAfter;
        if (kind(r) >= 0 || (x.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING && kind(x.Aliasing.pResourceBefore) >= 0)) {
            g_f.e1f_split = true;
            g_r.e1f_watch = false;
            bump(g_e1f_splits);
            return;
        }
    }
    bool mark = false;
    if (x.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
        const int k = kind(x.Transition.pResource);
        if (k < 0) return;
        bump(g_e1f_transitions[k == kDepth ? 1 : 0]);
        if (x.Transition.StateAfter & kOther) mark = true;
        if (k != kDepth) {
            g_f.e1f_cmoved[k] = true;
            g_f.e1f_cst[k] = static_cast<uint32_t>(x.Transition.StateAfter);
        }
    } else if (x.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
        if (kind(x.UAV.pResource) >= 0) bump(g_e1f_uav_barriers);
    } else {
        mark = kind(x.Aliasing.pResourceBefore) >= 0 || kind(x.Aliasing.pResourceAfter) >= 0;
    }
    if (!mark) return;
    g_f.e1f_barrier = true;
    g_r.e1f_watch = false;
    bump(g_e1f_barrier);
}
// (item 8f, the smoke cycle's change) a CopyResource or CopyTextureRegion while listening, after the hook's poll: its destination one
// of the four between T and C2 marks the frame (C2 refused, "write"). ResolveSubresource, DiscardResource and ExecuteBundle are not
// hooked (a resolve or a compute write needs a transition into a write state first: e1f_watch_barrier)
void copy_write(ID3D12GraphicsCommandList* cl, const ID3D12Resource* dst, const ID3D12Resource* src) {
    if (g_rm_on && GetCurrentThreadId() == g_rm_tid) {  // (item 8f's remap: a copy reading or writing one of the four, or another list's)
        bool four = cl != g_rm_cl;
        for (int k = 0; k < kKinds && !four; ++k) four = (dst && dst == g_a[kS][k].game) || (src && src == g_a[kS][k].game);
        if (four) rm_end(cl, cl != g_rm_cl ? kRmList : kRmCopy);
    }
    if (!g_r.open || !g_r.e1f_watch || !dst || GetCurrentThreadId() != g_r.tid) return;
    for (int k = 0; k < kKinds; ++k)
        if (dst == g_a[kS][k].game) {
            g_f.e1f_write = true;
            g_r.e1f_watch = false;
            bump(g_e1f_writes);
            return;
        }
}
// ([Stereo] MainRegionE1Compare, item 8f; the review's SF2) a ClearRenderTargetView / ClearDepthStencilView while listening, after the
// hook's poll (an owed C2 is taken before it): between T and C2 a clear of one of the four, or of a view whose resource is not known,
// marks the frame (C2 then refused, "clear"). DiscardResource and ExecuteBundle are not hooked (a bundle cannot clear or transition)
void clear(ID3D12GraphicsCommandList* cl, uintptr_t view) {
    if (g_rm_on && GetCurrentThreadId() == g_rm_tid) {  // (item 8f's remap: a clear of one of the four, of a view not known, or another list's)
        const void* q = d3d::view_resource(view);
        bool four = cl != g_rm_cl || !q;
        for (int k = 0; k < kKinds && !four; ++k) four = q == g_a[kS][k].game;
        if (four) rm_end(cl, cl != g_rm_cl ? kRmList : kRmClear);
    }
    if (!g_r.open || !g_r.e1f_watch || GetCurrentThreadId() != g_r.tid) return;
    const void* r = d3d::view_resource(view);
    bool hit = !r;
    for (int k = 0; k < kKinds && !hit; ++k) hit = r == g_a[kS][k].game;
    if (!hit) return;
    g_f.e1f_clear = true;
    g_r.e1f_watch = false;
    bump(g_e1f_clears);
}

void before_copy(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst) {
    if (!g_r.open || GetCurrentThreadId() != g_r.tid || !dst || !d3d::resource_named(dst, "Depth Resolve")) return;
    close_region(cl, true);
}

// (the review's SF3) a SetPredication (what the list now has, after any substitution): the remap list's kept for its close
void note_predication(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, uint64_t off, int op) {
    if ((!g_rm_on && !g_rm_owed) || cl != g_rm_cl) return;
    if (GetCurrentThreadId() != g_rm_tid) {  // (another thread recording the remap's list: never expected)
        rm_lost(kRmList, "a SetPredication on the remap's list from another thread");
        return;
    }
    g_rm_pred = d3d::Predicate{r, off, op};
}
void list_closing(ID3D12GraphicsCommandList* cl) {
    if (eye2_light::lent_any()) eye2_light::list_closing(cl);  // (item 6c: SC2L and LB2 lent on this list given back before it closes)
    if ((g_rm_on || g_rm_owed) && cl == g_rm_cl) rm_end(cl, kRmClose);  // (item 8f's remap: closed on its list before the list closes)
}
void list_ends(ID3D12GraphicsCommandList* cl) {
    if ((g_rm_on || g_rm_owed) && cl == g_rm_cl) rm_lost(kRmClose, "the remap's list reset before a close");  // (never expected)
    if (eye2_light::lent_any()) eye2_light::list_reset(cl);  // (item 6c: lent on a list reset with no Close seen: never expected)
    if (t_bind.cl == cl) {
        t_bind.cl = nullptr;
        ++t_bind_gen;
    }
    if (t_c1.cl == cl) c1_drop();  // ([Stereo] MainFamily=region: the cached ID3D12GraphicsCommandList1 released at the list's Reset or Close)
    if (t_pso_cl == cl) {
        t_pso_cl = nullptr;
        t_pso = nullptr;
    }
}
bool withholding() { return g_installed.load(std::memory_order_relaxed) && g_on.load(std::memory_order_relaxed) && !g_stood.load(std::memory_order_relaxed); }

// ---- (GOAL-single-pass item 8d) [Stereo] MainRegion's reads of the region (main_region.h; the playback thread)
void indirect(ID3D12GraphicsCommandList* cl) {
    if (g_rm_on && GetCurrentThreadId() == g_rm_tid) rm_end(cl, kRmIndirect);  // (item 8f's remap: its draws would land in the four, unseen)
    if (g_f.ho && g_r.open && GetCurrentThreadId() == g_r.tid) {  // (item 8g: pass 1's refuses the decision; pass 2's are drawn by the game)
        if (g_r.pass == 1) g_f.ho_ind1 = true;
        else bump(g_ho_indirect);
    }
    if (!g_r.open || GetCurrentThreadId() != g_r.tid) return;
    if (main_region::observing()) main_region::indirect(cl);
}
bool ucull_installed() { return g_uc_installed.load(std::memory_order_relaxed); }
bool pb_union_culled() { return g_r.open && g_r.pass == 1 && g_r.uc; }
bool uc_eye_planes(uintptr_t vp, float out[16]) {  // (the render thread: g_uc_vp and g_uc_saved are its own)
    if (!g_uc_vp || g_uc_vp != vp) return false;
    std::memcpy(out, g_uc_saved[0], 64);
    return true;
}
bool pb_frame_order() { return (g_f.why & kXOrder) != 0; }
// ---- (GOAL-single-pass item 8e) [Stereo] MainFamily=region (main_instanced.h)
bool region_family() { return rg_on(); }
int pb_pass() { return g_r.open && GetCurrentThreadId() == g_r.tid ? g_r.pass : 0; }  // (M1: installed on the ini, and pso's own switch on at the device)
bool own_armed() {  // (the render thread: inside a main pass's DVL call 1 only, where the stream takes markers)
    return g_own_armed.load(std::memory_order_relaxed) && g_own_on.load(std::memory_order_relaxed) && (t_open[1] || t_open[2]);
}
void own_mark(bool on) { bump(prepass::stream_marker(on ? &cb_own1 : &cb_own0) ? g_own_marks : g_own_mark_failed); }
void mr_open(uint64_t frame) {  // (the playback thread: main_region's pass-1 open, just after main_inst's open marker)
    if (!g_f.rg || g_f.state != 1 || !g_r.open || g_r.pass != 1 || g_f.mr_fr_have) {
        bump(g_rg_mro_stale);
        return;
    }
    bump(g_rg_mro);
    g_f.mr_fr_have = true;
    g_f.mr_frame = frame;
}
void mr_verdict(const MrVerdict& v) {  // (the playback thread: main_region's finalize)
    if (!g_f.rg || g_f.state != 3 || g_r.pass != 2) {  // (only inside this frame's pass-2 region: an unfinished frame's is not this one's)
        bump(g_rg_mrv_stale);
        return;
    }
    if (!g_f.mr_fr_have || v.frame != g_f.mr_frame) {  // (S5: another frame's verdict; this frame then has none: kXRgVerdict)
        bump(g_rg_mrv_frame);
        return;
    }
    if (g_f.mr_have) bump(g_rg_mrv_twice);
    bump(g_rg_mrv);
    g_f.mr_have = true;
    g_f.mr = v;
    g_f.mr.draws = nullptr;  // (the call's array only)
    uint64_t drew = 0;
    for (int o = 0; v.draws && o < kOrds; ++o)
        if (v.draws[o]) drew |= 1ull << o;
    g_f.drew = drew;
    if (v.judged) g_rg_last_drew = drew;  // (swap auto: the next pair drawn in the last judged frame)
    for (int b = 0; b < 8; ++b)
        if (v.unj & (1u << b)) bump(g_rg_unjb[b]);
    for (int b = 0; b < 16; ++b)
        if (v.gates & (1u << b)) bump(g_rg_gateb[b]);
}
bool region_bind_ok(ID3D12GraphicsCommandList* cl) {
    if (t_bind_ok_gen != t_bind_gen || t_bind_ok_cl != cl) {  // (a new bind since the last answer: the four lookups once)
        Bound b;
        t_bind_ok = check_bind(cl, &b) == nullptr;
        t_bind_ok_gen = t_bind_gen;
        t_bind_ok_cl = cl;
    }
    return t_bind_ok;
}

// ([Stereo] MainRegionE1Compare, item 8f) main_region's pb_e1 (the playback thread, e1's end marker): on an e1f frame's pass 2, inside its
// region with T taken, C2 is owed: taken at this thread's next list call (poll), before that call's own work
void mr_e1_end(int pass) {
    if (g_r.open && g_r.pass == pass && GetCurrentThreadId() == g_r.tid) g_r.e1_done = true;  // (H2m's diagnosis: e1's SRV reads counted to here)
    if (pass == 1 && g_rm_on) {  // (item 8f's remap: pass 1's e1 end: S0 -> the four at this thread's next list call)
        if (GetCurrentThreadId() != g_rm_tid) {
            rm_lost(kRmEnd, "pass 1's e1 end on another thread");
        } else {
            g_rm_on = false;
            g_rm_owed = true;
            prepass::g_main_pending_flag.store(true, std::memory_order_release);
        }
        return;
    }
    if (pass != 2 || !g_f.e1f) return;
    if (!g_r.open || g_r.pass != 2 || g_f.state != 3 || GetCurrentThreadId() != g_r.tid || g_f.c_taken || g_e1f_owed) {
        bump(g_e1f_e1end_stale);
        return;
    }
    bump(g_e1f_e1end);
    if (!g_f.e1f_t) return;  // (no T: the frame is excluded at the close, C2 not taken)
    g_e1f_owed = true;
    g_e1f_owed_tid = GetCurrentThreadId();
    prepass::g_main_pending_flag.store(true, std::memory_order_release);
}
void poll(ID3D12GraphicsCommandList* cl) {
    if (!cl) return;
    if (g_rm_owed && GetCurrentThreadId() == g_rm_tid) {  // (item 8f's remap: S0 -> the four, before this list call's own work)
        rm_end(cl, kRmEnd);
        if (!g_pending && !g_e1f_owed) prepass::g_main_pending_flag.store(false, std::memory_order_relaxed);
    }
    if (g_e1f_owed && GetCurrentThreadId() == g_e1f_owed_tid) {  // (item 8f: C2, before a readback owed; one try)
        g_e1f_owed = false;
        e1f_take_c2(cl);
        if (!g_pending) prepass::g_main_pending_flag.store(false, std::memory_order_relaxed);
    }
    if (!g_pending || GetCurrentThreadId() != g_pending_tid) return;
    g_pending = false;
    prepass::g_main_pending_flag.store(false, std::memory_order_relaxed);
    readback(cl);
}

namespace {
// ([Stereo] MainSlice, run 25 item 5) after MainInstanced's install: the allocator's anchors and D3DCompile; nothing made yet
void install_slice() {
    const int want = config::get_int("Stereo", "MainSlice", 0);
    if (want <= 0) return;
    if (want != 1) {
        log::warn("[mi] MainSlice=%d is not 1: off", want);
        return;
    }
    if (g_rg_want) {  // (GOAL-single-pass item 8e: the merge refuses non-writing families; never with the region's listing; M1: the ini's)
        log::warn("[mi] MainSlice=1 with [Stereo] MainFamily=region: refused (off)");
        return;
    }
    if (!anchors::present(Id::DescAlloc) || !anchors::present(Id::D3dDeviceSingleton)) {
        log::warn("[mi] MainSlice needs the anchors DescAlloc and D3dDeviceSingleton (the SRVs from the game's allocator): off");
        return;
    }
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    g_compile = dc ? reinterpret_cast<Compile_t>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    if (!g_compile) {
        log::warn("[mi] MainSlice needs d3dcompiler_47.dll's D3DCompile (the merge's shaders): off");
        return;
    }
    g_sl_installed = true;
    g_slice = true;
    g_sl_mode = 1;
    log::info("[mi] [Stereo] MainSlice on (test only): pass 2 drops the billboard family's listed draws; eye 2's come from pass 1's twin (S slice "
              "1 seeded far) through a depth-tested merge at the drop point (the family's root signature and depth-stencil state, the SRVs from the "
              "game's allocator); \"prepass mi slice on|off|nomerge|always\"; %s",
              pso::main_slice_kept() ? "the family's descs kept" : "the family's descs not read yet (pso reads the switch at the device's creation)");
}
// (GOAL-single-pass item 8e, test only) after MainPredication's and MainUnionCull's: [Stereo] MainFamily=region (g_rg_want, read from
// the ini at this install, before the device; pso's own switch read at the first device's hooks and checked lazily, rg_on); off until
// "prepass mr compare" (main_region's mode)
void install_region();
// (GOAL-single-pass items 6d and 6e, test only) after MainInstanced's install
void install_pred_ucull() {
    const int pd = config::get_int("Stereo", "MainPredication", 0);
    if (pd > 0) {
        if (pd != 1) {
            log::warn("[mi] MainPredication=%d is not 1: off", pd);
        } else {
            d3d::set_predicate_tracking(true);
            g_pd_installed = true;
            g_pd_mode = 1;
            log::info("[mi] [Stereo] MainPredication on (test only, item 6d): a listed draw on a predicated list keeps its predicate; its "
                      "duplicates are drawn under SetPredication(null) and the list's predicate (kept per list by the SetPredication hook) "
                      "is put back after them; \"prepass mi pred on|off|refs\"");
        }
    }
    const int uc = config::get_int("Stereo", "MainUnionCull", 0);
    if (uc > 0) {
        if (uc != 1) {
            log::warn("[mi] MainUnionCull=%d is not 1: off", uc);
        } else if (!anchors::present(Id::ViewportCurrent)) {
            log::warn("[mi] MainUnionCull needs the ViewportCurrent anchor: off");
        } else {
            pso::set_vb_note(true);
            g_uc_installed = true;
            g_uc_on = true;
            log::info("[mi] [Stereo] MainUnionCull on (test only, item 6e): pass 1's DVL call 1 culls against both eyes' union (the viewport's "
                      "side planes swapped, as PrepassUnionCull's walk), pass 2 against eye 2's own; pass 2's listed draws checked against "
                      "pass 1's by key; \"prepass mi ucull on|off\"");
            // (the 6d/6e review's should-fix 2) the union viewport is built only for PrepassUnionCull or the dual-pass shadow union
            // (camera_lever): without them each swap is refused and counted (uc_nounion), never guessed
            if (config::get_int("Stereo", "PrepassUnionCull", 0) <= 0)
                log::warn("[mi] MainUnionCull: the union viewport is built only with [Stereo] PrepassUnionCull=1 (or the dual-pass shadow "
                          "union); without either every swap is refused (uc_nounion)");
        }
    }
    const int ub = config::get_int("Stereo", "MainUnionBox", 0);  // (item 8e, test only, off by default)
    if (ub > 0) {
        if (ub != 1) {
            log::warn("[mi] MainUnionBox=%d is not 1: off", ub);
        } else if (!g_uc_installed.load()) {
            log::warn("[mi] MainUnionBox needs [Stereo] MainUnionCull=1 installed: off");
        } else if (config::get_int("Stereo", "PrepassViews", 0) < 2) {
            log::warn("[mi] MainUnionBox needs [Stereo] PrepassViews=2 (the private eye-2 viewport): off");
        } else if (ub_install()) {
            g_ub_installed = true;
            g_ub_on = true;
            log::info("[mi] [Stereo] MainUnionBox on (test only, item 8e): inside pass 1's union swap, grmModel's box culler answers the most of "
                      "the union's planes, eye 1's and eye 2's (each World x that eye's side matrix), so pass 1 keeps every model and geometry "
                      "either eye keeps; \"prepass mi ubox on|off\"");
        }
    }
}
void install_region() {
    if (!g_rg_want) return;
    if (!g_uc_installed.load() || !g_pd_installed.load()) {
        log::warn("[mi] [Stereo] MainFamily=region needs MainUnionCull and MainPredication installed: off (nothing listed: the main set is empty)");
        return;
    }
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    g_rg_qpc0.store(q.QuadPart);
    g_rg_tsc0.store(__rdtsc());
    g_rg_installed = true;
    g_own_armed = true;  // (item 8e 14: body.cpp marks the entities it edits per pass)
    if (g_e1f_want > 0) {  // ([Stereo] MainRegionE1Compare, item 8f, test only; off until "prepass mi region e1f on")
        if (g_e1f_want != 1) {
            log::warn("[mi] MainRegionE1Compare=%d is not 1: off", g_e1f_want);
        } else if (!eye2_light::d2l_installed()) {
            log::warn("[mi] MainRegionE1Compare needs [Stereo] PrepassEye2LightDepth=1 installed (D2L, slice 1's depth seed): off");
        } else {
            g_e1f_installed = true;
            g_slice = true;  // (the list hooks' barrier call: the four watched from T to C2)
            if (g_ho_want > 0) {  // ([Stereo] MainRegionHandover, item 8g, test only; off until "prepass mi region e1f handover on")
                if (!g_compile) {
                    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
                    g_compile = dc ? reinterpret_cast<Compile_t>(GetProcAddress(dc, "D3DCompile")) : nullptr;
                }
                if (g_ho_want != 1) log::warn("[mi] MainRegionHandover=%d is not 1: off", g_ho_want);
                else if (!anchors::present(Id::DescAlloc) || !anchors::present(Id::D3dDeviceSingleton))
                    log::warn("[mi] MainRegionHandover needs the anchors DescAlloc and D3dDeviceSingleton (its FSC SRV from the game's allocator): off");
                else if (!g_compile) log::warn("[mi] MainRegionHandover needs d3dcompiler_47.dll's D3DCompile (the merge's shaders): off");
                else {
                    g_ho_installed = true;
                    log::info("[mi] [Stereo] MainRegionHandover on (test only, item 8g): with \"prepass mi region e1f on\" and \"handover on\" pass 2 hands "
                              "S1 over to the game's four at its first e1 draw and drops e1's draws pass 1 twinned; \"handover on|off|nohandover|identity\", "
                              "\"handover strict on|off\", \"handover ref r1|h1\", \"handover trim on|off\"");
                }
            }
            log::info("[mi] [Stereo] MainRegionE1Compare on (test only, item 8f): on joint dup frames with \"prepass mi region e1f on\" slice 1 of S and R "
                      "seeded from D2L, V2 under DLSS (a clear under FXAA), reactivity 0 and FSC's sentinel; R0 <- T at pass 2's e1 start, C <- C2 at its "
                      "end; G0 = R1 vs T, G1 = S1 vs C2; \"prepass mi region e1f on|off|pred live|null\", \"prepass mi control seed|single <vs8>:<ps8>\"");
        }
    }
    log::info("[mi] [Stereo] MainFamily=region on (test only, item 8e): with \"prepass mr compare\" e1's draws are listed from main_region's "
              "classifier on dup frames (a capture armed, 1 in N), twinned through pso::region_twin_of, judged by 8d's verdict; \"prepass mi "
              "region all|solo <vs8>:<ps8>|subset <hex>|swap off|auto|ip on|off|every N|kv|save <dir> [any|fail]\"; pso takes the switch at the device's creation "
              "(until then, and if it falls back to bb, nothing of this runs)");
}
}  // namespace

void install() {
    const int want = config::get_int("Stereo", "MainInstanced", 0);
    if (want != 1 && config::get_int("Stereo", "MainSlice", 0) > 0) log::warn("[mi] MainSlice needs [Stereo] MainInstanced=1: off");
    if (want != 1 && (config::get_int("Stereo", "MainPredication", 0) > 0 || config::get_int("Stereo", "MainUnionCull", 0) > 0))
        log::warn("[mi] MainPredication and MainUnionCull need [Stereo] MainInstanced=1: off");
    if (want <= 0) return;
    if (want != 1) {
        log::warn("[mi] MainInstanced=%d is not 1: off", want);
        return;
    }
    if (config::get_int("Stereo", "PrepassViews", 0) < 2 || !prepass::views_wanted()) {
        log::warn("[mi] MainInstanced needs [Stereo] PrepassViews=2 (eye 2's rows written in pass 1, the twin's): off");
        return;
    }
    if (config::get_string("Stereo", "InstancedShaders", "").empty() && !config::get_bool("Stereo", "InstancedShadersAuto", false)) {
        log::warn("[mi] MainInstanced needs the twins ([Stereo] InstancedShaders or InstancedShadersAuto): off");
        return;
    }
    if (!dual_pass::shadows_wanted()) {
        log::warn("[mi] MainInstanced needs [Stereo] Split_shadows=1 (or Split_shadowunion=1): with each pass's own cascades the twin would "
                  "read eye 1's gCameraMatrix and tree-shadow map (billboard.md 8.3): off");
        return;
    }
    if (anchors::stand_down() || !anchors::exe_matches() || anchors::relocated() || !anchors::present(Id::MainDvlCall)) {
        log::warn("[mi] MainInstanced runs on the analysed RDR.exe build only (MainDvlCall): off");
        return;
    }
    const uintptr_t site = anchors::addr(Id::MainDvlCall);  // a rel32 call of DrawVisList (the return address dual_pass keys on)
    uint8_t op = 0;
    int32_t rel = 0;
    if (!rd(site, &op) || op != 0xe8 || !rd(site + 1, &rel) || site + 5 + static_cast<intptr_t>(rel) != anchors::addr(Id::DrawVisList)) {
        log::warn("[mi] the call at MainDvlCall is not a call of DrawVisList: off");
        return;
    }
    g_ret = site + 5;
    d3d::add_frame_end_listener(&frame_end);
    g_installed = true;
    g_on = true;
    g_noting = true;
    set_armed();
    log::info("[mi] [Stereo] MainInstanced on (test only, compare only): the billboard family's twin drawn in pass 1 into mod-owned arrays, "
              "the game's own pipeline into a reference in each pass; \"prepass mi compare N\" compares eye 2's; %s",
              pso::main_family_on() ? "the family's set on" : "the family's set not read yet (pso reads the switch at the device's creation)");
    // (GOAL-single-pass item 8e, M1) [Stereo] MainFamily=region read here, as pso reads it at the device (which comes after this install)
    g_rg_want = config::get_string("Stereo", "MainFamily", "bb") == "region" && config::get_int("Stereo", "MainRegion", 0) == 1 &&
                config::get_int("Stereo", "MainUnionCull", 0) == 1 && config::get_int("Stereo", "MainPredication", 0) == 1 &&
                config::get_bool("Stereo", "InstancedShadersAuto", false);
    if (!g_rg_want && config::get_string("Stereo", "MainFamily", "bb") == "region")
        log::warn("[mi] [Stereo] MainFamily=region needs [Stereo] MainRegion=1, MainUnionCull=1, MainPredication=1 and InstancedShadersAuto=1: "
                  "the region listing off (pso lists the billboards)");
    g_e1f_want = config::get_int("Stereo", "MainRegionE1Compare", 0);  // (item 8f, test only; installed with the region, install_region)
    g_ho_want = config::get_int("Stereo", "MainRegionHandover", 0);  // (item 8g, test only; installed with the e1f machinery)
    if (g_ho_want > 0 && (!g_rg_want || g_e1f_want != 1)) log::warn("[mi] MainRegionHandover needs [Stereo] MainRegionE1Compare=1 and MainFamily=region: off");
    if (g_e1f_want > 0 && !g_rg_want) log::warn("[mi] MainRegionE1Compare needs [Stereo] MainFamily=region: off");
    install_slice();
    install_pred_ucull();
    install_region();
}

std::string command(const std::vector<std::string>& t) {
    if (!g_installed.load())
        return "mi: not installed (start with [Stereo] MainInstanced=1, PrepassViews=2, the twins and Split_shadows, on the analysed RDR.exe)";
    if (t.size() >= 3) {
        const std::string& w = t[2];
        if (w == "kv") return kv();
        if (w == "on" || w == "off") {
            if (w == "on" && g_stood.load()) return "ERROR stood down (" + std::string(g_why) + "): restart the game";
            g_on = w == "on";
            set_armed();
        } else if (w == "reset") {
            reset();
        } else if (w == "control" && t.size() >= 4 && (t[3] == "seed" || t[3] == "single" || t[3] == "rmdrop" || t[3] == "rmpoison")) {
            // ([Stereo] MainRegionE1Compare, item 8f, test only) the playback-only controls of e1f frames (latched at pass 1's open): seed (S1's
            // stencil from D2L, eye 2's after its lights, for D2) and single (that pair's pass-1 draws by the game's pipeline into S0 only, no twin)
            if (!g_e1f_installed.load()) return "mi control seed|single: not installed (start with [Stereo] MainRegionE1Compare=1; test only)";
            int o = -1;
            if (t[3] == "rmdrop" && t.size() == 5 && t[4] == "all") {
                o = -2;  // (every listed draw while the remap stands)
            } else if (t[3] == "single" || t[3] == "rmdrop") {
                unsigned vs = 0, ps = 0;
                char tail = 0;
                if (t.size() != 5 || t[4].size() != 17 || t[4][8] != ':' || sscanf_s(t[4].c_str(), "%8x:%8x%c", &vs, &ps, &tail, 1) != 2)
                    return "ERROR prepass mi control single|rmdrop <vs 8 hex>:<ps 8 hex>";
                o = main_region::ord_find(vs, ps);
                if (o < 0) return "ERROR mi control " + t[3] + " " + t[4] + ": that pair has no ordinal";
            } else if (t.size() != 4) {
                return "ERROR prepass mi control seed|rmpoison";
            }
            g_control = 0;
            g_e1f_single.store(o, std::memory_order_relaxed);  // (before the kind, released by it: a latch that reads single reads its ordinal)
            g_e1f_ctl.store(t[3] == "seed" ? kCkSeed : t[3] == "single" ? kCkSingle : t[3] == "rmdrop" ? kCkRmDrop : kCkRmPoison, std::memory_order_release);
            log::info("[mi] control %s%s%s (e1f frames, the playback's, from the next pass 1)", t[3].c_str(), o != -1 ? " " : "", o != -1 ? t[4].c_str() : "");
        } else if (w == "control") {  // the positive control: eye 2's rows changed for pass 1's DVL call 1 (the compare must fail)
            if (t.size() < 4 || (t[3] != "off" && t[3] != "nudge" && t[3] != "eye1" && t[3] != "e2img" && t[3] != "pw"))
                return "ERROR prepass mi control off|nudge|eye1|e2img|pw";
            g_control = t[3] == "nudge" ? 1 : t[3] == "eye1" ? 2 : t[3] == "e2img" ? 3 : t[3] == "pw" ? 4 : 0;
            g_e1f_ctl = 0;  // (item 8f: one control at a time)
            log::info("[mi] control %s (eye 2's rows in pass 1's DVL call 1)", t[3].c_str());
        } else if (w == "ppfix") {  // (run 25, on by default: eye 2's gPrevViewProj copied again at pass 1's DVL call 1 when it was a copy)
            if (t.size() < 4 || (t[3] != "on" && t[3] != "off")) return "ERROR prepass mi ppfix on|off";
            g_ppfix = t[3] == "on";
            log::info("[mi] ppfix %s (eye 2's rage_prev_proj rows 4-7 from rows 0-3 at pass 1's DVL call 1, on copied frames)", t[3].c_str());
        } else if (w == "slice") {  // ([Stereo] MainSlice, run 25 item 5) the next pass 1 latches it
            if (!g_sl_installed.load()) return "mi slice: not installed (start with [Stereo] MainSlice=1 and MainInstanced=1)";
            if (t.size() < 4 || (t[3] != "on" && t[3] != "off" && t[3] != "nomerge" && t[3] != "always" && t[3] != "identity"))
                return "ERROR prepass mi slice on|off|nomerge|always|identity";
            if (t[3] != "off" && g_sl_stood.load()) return "ERROR the slice stood down (" + std::string(g_sl_why) + "): restart the game";
            g_sl_mode = t[3] == "on" ? 1 : t[3] == "nomerge" ? 2 : t[3] == "always" ? 3 : t[3] == "identity" ? 4 : 0;
            log::info("[mi] slice %s (from the next pass 1)", t[3].c_str());
        } else if (w == "pred") {  // ([Stereo] MainPredication, item 6d)
            if (!g_pd_installed.load()) return "mi pred: not installed (start with [Stereo] MainPredication=1 and MainInstanced=1)";
            if (t.size() < 4 || (t[3] != "on" && t[3] != "off" && t[3] != "refs")) return "ERROR prepass mi pred on|off|refs";
            if (t[3] != "off" && g_pd_stood.load()) return "ERROR MainPredication stood down (" + std::string(g_pd_why) + "): restart the game";
            g_pd_mode = t[3] == "on" ? 1 : t[3] == "refs" ? 2 : 0;
            log::info("[mi] pred %s", t[3].c_str());
        } else if (w == "p1vp") {  // (run 25 item 6g; test only)
            if (t.size() < 4 || (t[3] != "on" && t[3] != "off")) return "ERROR prepass mi p1vp on|off";
            g_p1vp = t[3] == "on";
            log::info("[mi] p1vp %s (test only: pass 1's DVL call 1 with pass 2's gViewport)", t[3].c_str());
        } else if (w == "refdepth") {  // (run 25, the family compare's diagnostic; test only)
            if (t.size() < 4 || (t[3] != "game" && t[3] != "own")) return "ERROR prepass mi refdepth game|own";
            if (t[3] == "game" && std::strcmp(pso::main_family_key(), "bb") == 0)
                return "ERROR prepass mi refdepth game: not for the billboards (their pipeline writes depth)";
            g_refdepth_game = t[3] == "game";
            log::info("[mi] refdepth %s", t[3].c_str());
        } else if (w == "ucull") {  // ([Stereo] MainUnionCull, item 6e) from the next pass 1
            if (!g_uc_installed.load()) return "mi ucull: not installed (start with [Stereo] MainUnionCull=1 and MainInstanced=1)";
            if (t.size() < 4 || (t[3] != "on" && t[3] != "off")) return "ERROR prepass mi ucull on|off";
            g_uc_on = t[3] == "on";
            log::info("[mi] ucull %s (from the next pass 1)", t[3].c_str());
        } else if (w == "ubox") {  // ([Stereo] MainUnionBox, item 8e) from the next pass 1's swap
            if (!g_ub_installed.load()) return "mi ubox: not installed (start with [Stereo] MainUnionBox=1, MainUnionCull=1 and PrepassViews=2)";
            if (t.size() < 4 || (t[3] != "on" && t[3] != "off")) return "ERROR prepass mi ubox on|off";
            g_ub_on = t[3] == "on";
            log::info("[mi] ubox %s (from the next pass 1's swap)", t[3].c_str());
        } else if (w == "allowtwins") {  // (GOAL-single-pass item 6c, test only, off by default) the next pass 1 latches it
            if (t.size() < 4 || (t[3] != "on" && t[3] != "off")) return "ERROR prepass mi allowtwins on|off";
            pso::set_allow_twins(t[3] == "on");
        } else if (w == "latch") {  // (item 6c) the frame latch's, the lend's and the prevworld gate's counters
            return pso::latch_kv() + " kv_end=1";
        } else if (w == "region") {  // ([Stereo] MainFamily=region, GOAL-single-pass item 8e, test only)
            return region_command(t);
        } else if (w == "compare" && t.size() >= 4 && t[3] == "off") {  // (item 8e's test: an armed capture disarmed; one in flight finishes)
            g_cap_left.store(0);
            int s = kArmed;
            g_cap.compare_exchange_strong(s, kIdle);
        } else if (w == "compare") {
            const int n = t.size() >= 4 ? std::atoi(t[3].c_str()) : 10;
            if (n <= 0 || n > 1000) return "ERROR compare N: 1..1000";
            if (!setup_compare()) return "ERROR the fence, its event or the worker not made";
            const int s = g_cap.load();
            if (s != kIdle && s != kArmed) return "ERROR a capture is in flight (cap=" + std::to_string(s) + ")";
            g_cap_left.store(n);
            g_cap.store(kArmed);
        } else {
            return "ERROR prepass mi [on|off|compare N|compare off|reset|kv|control off|nudge|eye1|ppfix on|off|slice on|off|nomerge|always|identity|allowtwins on|off|latch|"
                   "pred on|off|refs|ucull on|off|region ...]";
        }
    }
    std::string sl;
    if (g_sl_installed.load()) {  // ([Stereo] MainSlice)
        char s[2000];
        std::snprintf(s, sizeof(s),
                      " | slice %s%s: frames %llu, handed over %llu, refused %llu (early %llu, stood %llu, pass 1 not whole %llu, held %llu, SRVs %llu, "
                      "merge %llu, pipeline %llu, query %llu), dropped %llu (last frame %llu, %llu..%llu), extra %llu, short %llu, merges %llu, nomerge %llu, barriers in the run "
                      "%llu, queries in the run %llu, compare frames %llu | G1 compared %llu: exact %llu, pass %llu, marked frames %llu (passing %llu, failing %llu) | last: %s",
                      kSlName[g_sl_mode.load() >= 0 && g_sl_mode.load() <= 4 ? g_sl_mode.load() : 0], g_sl_stood.load() ? " (stood down)" : "", u(g_sl_frames), u(g_sl_handed), u(g_sl_refused),
                      u(g_sl_hr[kHrEarly]), u(g_sl_hr[kHrStood]), u(g_sl_hr[kHrWhole]), u(g_sl_hr[kHrHeld]), u(g_sl_hr[kHrSrv]), u(g_sl_hr[kHrMerge]),
                      u(g_sl_hr[kHrPso]), u(g_sl_hr[kHrQuery]), u(g_sl_dropped), static_cast<unsigned long long>(g_sl_last_dropped.load()),
                      static_cast<unsigned long long>(g_sl_drop_min.load() == ~0ull ? 0 : g_sl_drop_min.load()),
                      static_cast<unsigned long long>(g_sl_drop_max.load()), u(g_sl_extra), u(g_sl_short), u(g_sl_merges), u(g_sl_nomerge), u(g_sl_barrier),
                      u(g_sl_query_in_run), u(g_sl_cmp), u(g_c_sl_frames), u(g_c_g1_exact), u(g_c_sl_pass), u(g_c_sl_marked), u(g_c_pass_frames), u(g_c_fail_frames),
                      g_sl_why[0] ? g_sl_why : "none");
        sl = s;
    }
    if (g_pd_installed.load() || g_uc_installed.load()) {  // (items 6d and 6e)
        char s[600];
        std::snprintf(s, sizeof(s),
                      " | family %s | pred %s%s: held %llu / %llu, nulled %llu, put back %llu, unknown %llu, record moved %llu | ucull %s%s: swaps %llu "
                      "(ends %llu, no union %llu, failed %llu, rewritten %llu), frames judged %llu union / %llu control, pass 2 within pass 1 %llu / "
                      "%llu, misses %llu / %llu%s%s",
                      pso::main_family_key(), g_pd_installed.load() ? kPdName[g_pd_mode.load() >= 0 && g_pd_mode.load() <= 2 ? g_pd_mode.load() : 0] : "not installed",
                      g_pd_stood.load() ? " (stood down)" : "", u(g_pd_held[1]), u(g_pd_held[2]), u(g_pd_nulled), u(g_pd_restored), u(g_pd_unknown),
                      u(g_pd_after_bad), g_uc_installed.load() ? (g_uc_on.load() ? "on" : "off") : "not installed", "", u(g_uc_begins), u(g_uc_ends),
                      u(g_uc_nounion), u(g_uc_failed), u(g_uc_rewritten), u(g_uc_frames[1]), u(g_uc_frames[0]), u(g_uc_subset[1]), u(g_uc_subset[0]),
                      u(g_uc_miss[1]), u(g_uc_miss[0]), g_pd_why[0] ? " | pred stood down: " : "", g_pd_why);
        sl += s;
    }
    char b[3200];
    std::snprintf(b, sizeof(b),
                  "mi %s%s: regions opened pass 1 %llu / pass 2 %llu (marker failed %llu; gate refused pass 1 %llu, pass 2 %llu; rows not ready %llu), "
                  "closed by the Depth Resolve copy %llu, by the fallback %llu | listed draws pass 1 %llu, pass 2 %llu (last region %llu / %llu, first "
                  "at draw %llu / %llu), twin draws %llu, reference draws %llu / %llu, predicated %llu, refused %llu (no twin %llu) | seeds %llu (eye "
                  "2 from the pre-pass array %llu; state followed %llu), arrays made %llu | frames good %llu, excluded %llu (gate %llu, predicated "
                  "%llu, refused %llu, counts %llu, not one run %llu, seed %llu, order %llu, none %llu) | shadow constants across the passes: equal "
                  "%llu, differ %llu, not read %llu | captures: started %llu, done %llu, incomplete %llu, left %d (state %d) | compared %llu: pass %llu, "
                  "eye 2 bit-exact %llu, eye 1 bit-exact %llu, negative control differs %llu, reactivity zero %llu, coverage >= %llu %llu (min %llu, "
                  "max %llu); control (%s) frames %llu, eye 2 differs %llu | last refusal: %s | %s",
                  g_on.load() ? "on" : g_stood.load() ? "stood down" : "off", g_listen.load() ? " (in a region)" : "", u(g_r_opens[1]),
                  u(g_r_opens[2]), u(g_r_open_failed), u(g_r_gate_bad[1]), u(g_r_gate_bad[2]), u(g_r_unready), u(g_p_close_copy),
                  u(g_p_close_marker), u(g_p_listed[1]), u(g_p_listed[2]), static_cast<unsigned long long>(g_last_listed[1].load()),
                  static_cast<unsigned long long>(g_last_listed[2].load()), static_cast<unsigned long long>(g_last_first[1].load()),
                  static_cast<unsigned long long>(g_last_first[2].load()), u(g_p_twin), u(g_p_ref[1]), u(g_p_ref[2]), u(g_p_predicated),
                  u(g_p_refused), u(g_p_no_twin), u(g_p_seeds), u(g_p_seed_e2_array), u(g_p_seed_followed), u(g_p_made), u(g_f_good), u(g_f_bad),
                  u(g_f_x_gate), u(g_f_x_pred), u(g_f_x_refused), u(g_f_x_counts), u(g_f_x_contig), u(g_f_x_seed), u(g_f_x_order), u(g_f_x_none),
                  u(g_dg_equal), u(g_dg_differ), u(g_dg_unread), u(g_cap_started), u(g_cap_done), u(g_cap_incomplete), g_cap_left.load(),
                  g_cap.load(), u(g_c_frames), u(g_c_pass), u(g_c_e2_exact), u(g_c_e1_exact), u(g_c_neg_differ), u(g_c_react_zero),
                  static_cast<unsigned long long>(kCovMin), u(g_c_cov_ok),
                  static_cast<unsigned long long>(g_c_cov_min.load() == ~0ull ? 0 : g_c_cov_min.load()), static_cast<unsigned long long>(g_c_cov_max.load()),
                  g_control.load() == 1 ? "nudge" : g_control.load() == 2 ? "eye1" : g_control.load() == 3 ? "e2img" : g_control.load() == 4 ? "pw" : "off", u(g_c_ctl), u(g_c_ctl_e2_differ),
                  g_why[0] ? g_why : "none", pso::main_status().c_str());
    return b + sl;
}

// ---- (item 6e's draw keys, made general in item 8d: main_instanced.h keys)
namespace keys {
void begin(Tables& t) {
    if (++t.gen == 0) {  // (the generation wrapped: the tables cleared once)
        std::memset(t.tab[0], 0, sizeof(Slot) * t.slots);
        std::memset(t.tab[1], 0, sizeof(Slot) * t.slots);
        t.gen = 1;
    }
}
void make(ID3D12GraphicsCommandList* cl, const void* pso, const d3d::DrawArgs& a, uint64_t* with_vb, uint64_t* without_vb) {
    uint64_t k = mix64(0x6e5eed, reinterpret_cast<uintptr_t>(pso));
    k = mix64(k, (a.indexed ? 1ull : 0ull) | (static_cast<uint64_t>(a.a) << 1));
    k = mix64(k, (static_cast<uint64_t>(a.b) << 32) | a.c);
    k = mix64(k, (static_cast<uint64_t>(static_cast<uint32_t>(a.d)) << 32) | a.e);
    uint64_t loc = 0;
    uint32_t size = 0;
    const bool vb = pso::last_vb(cl, &loc, &size);
    *with_vb = mix64(mix64(k, vb ? loc : 0), vb ? size : 0xffffffffull);
    *without_vb = k;
}
unsigned count(Tables& t, Frame& f, int pass, uint64_t kv, uint64_t k) {
    bool full = false;
    unsigned miss = 0;
    if (pass == 1) {
        bool rep = false;  // (item 8d: this draw's key seen earlier this frame, returned as bit 2 for its pair; f.rep as before)
        key_tab(t, 0, kv, true, &full, &rep);
        if (rep) {
            f.rep = true;
            miss |= 4;
        }
        key_tab(t, 1, k, true, &full);
        ++f.n1;
        f.sum1 += fin64(kv);
    } else {
        if (!key_tab(t, 0, kv, false, &full)) {
            ++f.miss[0];
            miss |= 1;
        }
        if (!key_tab(t, 1, k, false, &full)) {
            ++f.miss[1];
            miss |= 2;
        }
        ++f.n2;
        f.sum2 += fin64(kv);
    }
    if (full) f.over = true;
    return miss;
}
uint64_t digest(uint64_t key) { return fin64(key); }
}  // namespace keys

}  // namespace rdrvr::main_inst
