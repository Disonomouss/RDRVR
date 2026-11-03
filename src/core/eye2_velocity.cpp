#include "core/eye2_velocity.h"

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
#include "core/config.h"
#include "core/cut_velocity.h"
#include "core/d3d_hooks.h"
#include "core/dual_pass.h"
#include "core/eye2_light.h"
#include "core/log.h"
#include "core/prepass_arrays.h"
#include "core/state.h"
#include "core/taa.h"

namespace rdrvr::prepass {
std::atomic<bool> g_eye2_vel_pending_flag{false};
}

namespace rdrvr::eye2_velocity {
namespace {
using anchors::Id;

// PostFx fields (research\run25\velocity2.md 1.1; cut_velocity.cpp, taa.cpp) [V]: +0x868 the technique applied, +0x5a4 the
// improved motion blur, +0xf8 the depth RT the pass samples, +0x128 the rdr2_taa effect, +0x3c4 its NDCToPrevNDC variable,
// +0x3f0 the Velocity RT, +0x4b0 NDCToPrevNDC (TaaUpdate's)
constexpr uintptr_t kTech = 0x868, kMotionBlur = 0x5a4, kDepthRt = 0xf8, kTaaEffect = 0x128, kVarNdcToPrev = 0x3c4, kVarTexel = 0x3c8, kVelRt = 0x3f0,
                    kNdcToPrev = 0x4b0;
constexpr int kRsIds = 0x2c;  // RenderStateSet's ids (eye2_light's): the inherited render states compared at both calls
// (r54, velocity2.md amendment) the ids the class compares: those 0x873920 does not set itself (it pushes 0x1e, 6, 0xb, 0x14, 0x19,
// 0x1b, 0x17, 0x15, 0x18, 0x23, 0x25, 0x21, 0x1f, 0x22) and whose RenderStateSet case [V 0x140147110] reaches a state the draw
// emits: a raster/depth-stencil/blend global (DAT_142b07xxx, the dirty bytes DAT_1422bdb99 / DAT_1422a463e, the write masks
// DAT_1422bdb9c) or an implemented case of FUN_140149930. The rest only write a mirror nothing reads (ids 3, 4, 0xf, 0x11-0x13,
// 0x1c, 0x23-0x26 call FUN_140149930 cases it does not have; 7 likewise), have no case (9), or write a global read only by
// RenderVisPhase (1): 0, 2, 5, 8, 0xa, 0xc, 0xd, 0xe, 0x10, 0x16, 0x1a, 0x1d, 0x20, 0x27, 0x28, 0x29, 0x2a, 0x2b
constexpr uint64_t kRsFeeds = (1ull << 0x0) | (1ull << 0x2) | (1ull << 0x5) | (1ull << 0x8) | (1ull << 0xa) | (1ull << 0xc) | (1ull << 0xd) |
                              (1ull << 0xe) | (1ull << 0x10) | (1ull << 0x16) | (1ull << 0x1a) | (1ull << 0x1d) | (1ull << 0x20) | (1ull << 0x27) |
                              (1ull << 0x28) | (1ull << 0x29) | (1ull << 0x2a) | (1ull << 0x2b);
inline bool rs_feeds(int id) { return id >= 0 && id < kRsIds && ((kRsFeeds >> id) & 1) != 0; }
// ShadowCollectorOwner (DAT_142ad2150) +0x48: the main depth's RT, as SceneRender stores it into PostFx +0xf8 before the
// velocity pass (0x1405c3e5b..0x1405c3e6d [V, the disassembly])
constexpr uintptr_t kMainDepth = 0x48;
// the effect (FUN_140151020 [V]): +0x00 its values (entry h-1 of 16 bytes, +8 the pointer), +0x10 the owner
constexpr uintptr_t kEffValues = 0x00, kEffOwner = 0x10;
using SetVar_t = void (*)(void* owner, void* eff, int handle, const void* data, int size, int count);
constexpr int kKDepth = 0;  // prepass_arrays' kind 0, the depth

std::atomic<bool> g_installed{false}, g_on{false}, g_stood{false};
char g_why[200] = "";
using Count = std::atomic<uint64_t>;
inline void bump(Count& c) { c.fetch_add(1, std::memory_order_relaxed); }
inline unsigned long long u(const Count& c) { return static_cast<unsigned long long>(c.load(std::memory_order_relaxed)); }

// the render thread's
// (r54) eye 1 exact, readable under d4: at eye 1's call in a pass 1 eye 2's ran, PostFx +0x4b0 still the value saved before eye
// 2's call (kept), and the variable's CB bytes (rdr2_taa's CB storage at the variable's member, what the draw uploads) equal to it
Count g_e1_kept_ok, g_e1_kept_bad, g_e1_cb_ok, g_e1_cb_bad, g_e1_cb_unread, g_e1_ctl_ok, g_e1_ctl_bad;
float g_e1_saved[16];  // the render thread: +0x4b0 before eye 2's call
int g_ran_ctl = 0;     // the render thread: the control eye 2's call of this pass 1 ran with
Count g_calls, g_off_calls, g_runs, g_ctl_runs, g_gate_closed, g_not_dlss, g_no_ndc, g_no_postfx, g_no_stream, g_preps, g_fault, g_out_marker_failed,
    g_p2_caps, g_p2_marker_failed, g_p2_other_frame, g_e1_marks, g_e1_marker_failed, g_e1_local_ok, g_e1_local_bad, g_ticks;
uint64_t g_last_seq = 0;      // the eye-2 update's count used by the last run (each update is used once)
uint64_t g_seq_p1_end = 0;    // the eye-2 update's count at the last pass 1's eye-1 velocity call (a newer one is this frame's)
bool g_ran_pass1 = false;     // eye 2's call ran in this pass 1 (eye 1's call checks its own variable then)
uint64_t g_p1_count = 0;      // pass 1's game velocity calls so far (the capture's frame key)
uint64_t g_cap_p1 = ~0ull;    // the capture's: g_p1_count after its pass 1's eye-1 call
bool g_in2_marked = false;    // pass 2's IN2V marker appended (its OUT2V then)
double g_qpc_us = 0.0;
std::atomic<int> g_control{0};  // 0 none, 1 nudge (eye 2's [3][0] + 1e-3), 2 eye 1's matrix
// the playback thread's
Count g_made, g_make_failed, g_inv_ok, g_inv_unarmed, g_seeded, g_seed_failed, g_outv_ok, g_outv_nodraw, g_sub_nodraw, g_extra_draws, g_subst,
    g_subst_missed, g_eye1_exposed, g_unpaired, g_unclosed, g_ops_overflow, g_ops_other_thread, g_not_direct, g_in2_ok, g_in2_refused, g_out2_ok,
    g_out2_refused, g_e1_ok, g_e1_refused, g_vel_resolves, g_vel_changes, g_cap_started, g_cap_done, g_cap_incomplete;
std::atomic<bool> g_img_ready{false};  // V2 made for the current Velocity RT (the render thread gates on it)
std::atomic<uintptr_t> g_ready_rt{0};    // (review item 3) PostFx +0x3f0, the RT object V2 was made for (the render thread checks it)
Count g_unarmed_exposed;  // (review item 3) a window not armed in which the pass drew (into the game's Velocity RT): counted, stood down
// (GOAL-single-pass item 8f, test only: main_instanced's MainRegionE1Compare reads it through v2_image) V2 holds this pass 1's eye-2
// velocity: set at OUT-V's ok, cleared at IN-V, at a stand-down, when V2 is made again and when main_inst spends it (v2_spent)
std::atomic<bool> g_v2_fresh{false};

void stand_down(const char* why) {
    g_stood = true;
    g_v2_fresh = false;  // (item 8f)
    std::snprintf(g_why, sizeof(g_why), "%s", why);
    if (g_on.exchange(false)) log::error("[e2vel] PrepassEye2Velocity stood down: %s", why);
}
void exposed(const char* why) {  // eye 2's velocity went (may have gone) into the game's Velocity RT: eye 1's DLSS input
    bump(g_eye1_exposed);
    stand_down(why);
}
void refuse(Count& c, const char* why) {
    bump(c);
    std::snprintf(g_why, sizeof(g_why), "%s", why);
    log::limited("e2vel.refused", 10, "[e2vel] a step refused: %s", why);
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
bool raw_read(uintptr_t a, void* out, size_t n) {
    if (!a) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool add_ref(ID3D12Resource* r) {
    __try {
        r->AddRef();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool render_thread() {
    DWORD id = 0;
    return rd(anchors::addr(Id::RenderThreadId), &id) && id == GetCurrentThreadId();
}
// the effect's own value of a variable (what its CB is uploaded from), 16 floats
bool effect_value(uintptr_t eff, int handle, float* out, size_t bytes = 64) {
    uintptr_t values = 0, local = 0;
    return handle > 0 && handle <= 4096 && rd(eff + kEffValues, &values) && values &&
           rd(values + static_cast<uintptr_t>(handle) * 16 - 8, &local) && local && raw_read(local, out, bytes);
}
// (r54) a variable's bytes in the effect's constant buffer storage (cut_velocity's read_sample layout [V FUN_140151020,
// FUN_140ec8bd0]): eff +0x10 the owner (+0x20 its variable table, entries 0x48, +0x20 the member), eff +0x38 the CB object
// (+0x00 storage, +0x08 the layout: +0x00 its member table, entries 0x14, +0 the offset, +8 the size)
bool cb_value(uintptr_t eff, int handle, float* out) {
    uintptr_t owner = 0, vars = 0, cb = 0, storage = 0, layout = 0, members = 0;
    int member = -1, off = -1, size = 0;
    return handle > 0 && handle <= 4096 && rd(eff + 0x10, &owner) && owner && rd(owner + 0x20, &vars) && vars &&
           rd(vars + static_cast<uintptr_t>(handle - 1) * 0x48 + 0x20, &member) && member >= 0 && member < 4096 && rd(eff + 0x38, &cb) && cb &&
           rd(cb, &storage) && storage && rd(cb + 0x08, &layout) && layout && rd(layout, &members) && members &&
           rd(members + static_cast<uintptr_t>(member) * 0x14, &off) && rd(members + static_cast<uintptr_t>(member) * 0x14 + 0x08, &size) && off >= 0 &&
           off < 0x10000 && size >= 64 && raw_read(storage + static_cast<uintptr_t>(off), out, 64);
}
// (review item 4, velocity2.md 2.4) the inherited state at a call's entry: the render-state cache (DAT_142ac0b60, ids
// 0..0x2b, found in RenderStateSet's code at install as eye2_light does) and the recorder's scissor (ctx +0x148..+0x157, the
// context as eye2_light's tls_ctx reads it: TLS slot 0's block +0x28, its +0x38 init byte, and the device's +0x58)
uintptr_t g_rs_cache = 0;
uintptr_t tls_ctx() {
    uintptr_t dev = 0, hctx = 0, block = 0, ctx = 0;
    unsigned char init = 0;
    const uintptr_t tls = static_cast<uintptr_t>(__readgsqword(0x58));
    if (!rd(anchors::addr(Id::D3dDeviceSingleton), &dev) || !dev || !rd(dev + 0x58, &hctx) || !hctx) return 0;
    if (!rd(tls, &block) || !block || !rd(block + 0x38, &init) || !init || !rd(block + 0x28, &ctx) || ctx != hctx) return 0;
    return ctx;
}
// (the delta review's must-fix) the cull mode in force: RenderStateSet's case 0 [V, 0x14014713f..0x140147187] writes the cache
// entry 0, its mirror DAT_1422724a8 = DAT_141c47800[value], the rasterizer's cull DAT_142b073a0 (0, 1 or 2 from the mirror) and,
// when the cull changed, the dirty byte DAT_1422bdb99 = 1. FUN_140149930 and FUN_140a88aa0 also write the cull without the
// cache, so cache[0] alone does not say which cull is in force. The four addresses are read from RenderStateSet's own code at
// install (cull_globals); without them "cull p2" is refused and the class compares no cull
uintptr_t g_cull = 0, g_cull_mirror = 0, g_cull_dirty = 0, g_cull_table = 0;
struct CullState {
    uint32_t c0, mirror, cull;
};
bool cull_read(CullState* c) {
    return g_cull && g_rs_cache && rd(g_rs_cache, &c->c0) && rd(g_cull_mirror, &c->mirror) && rd(g_cull, &c->cull);
}
// RenderStateSet's mapping of a cache value: the mirror table[v] and the cull it gives (0/1 -> 0, 2 -> 1, 3 -> 2, else 0)
bool cull_of(uint32_t v, uint32_t* mirror, uint32_t* cull) {
    if (v > 2 || !rd(g_cull_table + static_cast<uintptr_t>(v) * 4, mirror)) return false;  // (the three values id 0 takes)
    *cull = *mirror == 2 ? 1u : *mirror == 3 ? 2u : 0u;
    return true;
}
struct Inherited {
    uint32_t rs[kRsIds];
    int32_t sc[4];
    uint32_t cull, mirror;  // (the delta review's should-fix) the cull in force, raw
    bool ok, cull_ok;
};
void take_inherited(Inherited* x) {
    std::memset(x, 0, sizeof(*x));
    const uintptr_t ctx = tls_ctx();
    x->ok = g_rs_cache && raw_read(g_rs_cache, x->rs, sizeof(x->rs)) && ctx && raw_read(ctx + 0x148, x->sc, sizeof(x->sc));
    x->cull_ok = g_cull && rd(g_cull, &x->cull) && rd(g_cull_mirror, &x->mirror);
}

// ---- the capture: one in flight (eye2_light's states)
enum Cap : int { kIdle, kArmed, kRec1, kRec2, kReady, kWait };
std::atomic<int> g_cap{kIdle};
std::atomic<int> g_cap_left{0};
std::atomic<uint64_t> g_cap_since{0}, g_ready_frame{0};
// the captured frame's (the render thread writes them while the capture is its own: kRec1 / kRec2, before the markers)
std::atomic<bool> g_cap_cut{false};
std::atomic<int> g_cap_ctl{0};
float g_cap_ndc1[16], g_cap_ndc2[16];  // the variable's value at eye 2's call in pass 1, and at pass 2's call
std::atomic<uintptr_t> g_cap_f8_1{0}, g_cap_f8_2{0};
std::atomic<bool> g_cap_cpu1{false}, g_cap_cpu2{false};  // those read
Inherited g_cap_in1{}, g_cap_in2{};  // (review item 4) the render states and the scissor at each call's entry
// (r53) render state id 0 is the cull mode (RenderStateSet 0x140147110 [V]: id 0's value v indexes DAT_141c47800 = {1, 3, 2}, whose
// 0/1, 2 and 3 give DAT_142b073a0 = 0, 1, 2, the rasterizer's cull mode, dirty flag DAT_1422bdb99; 0x873920 does not set it, it
// inherits it). r53: it differed at every compare between eye 2's call (after eye 2's lights in (L)) and pass 2's (after pass
// 2's lights). 0x6e0450 pushes and pops it (p3-light 1.3), so each call sees the value its pass had before its lights [I].
// "prepass e2vel cull p2|off" (off by default): eye 2's call runs with id 0 set to the value pass 2's call had in the last
// frame (render thread: recorded at every pass 2 entry), put back right after the call (snap_put would also put it back)
std::atomic<int> g_rs0_mode{0};      // 0 as inherited, 1 pass 2's last
std::atomic<bool> g_cap_rs0_set{false};
CullState g_p2_cull{};               // the render thread: the cull in force (raw) and cache[0] at the last pass 2's velocity call
bool g_p2_cull_ok = false;
Count g_rs0_sets, g_rs0_same, g_rs0_none, g_rs0_failed, g_rs0_inconsistent;
float g_cap_tx1[4], g_cap_tx2[4];    // TexelSize as each call left it (set inside it)
std::atomic<bool> g_cap_tx1_ok{false}, g_cap_tx2_ok{false};
enum Slot : int { kVIn1, kVD1, kVS1, kVOut1, kVE1, kVIn2, kVD2, kVS2, kVOut2, kSlots };
constexpr unsigned kAllSlots = (1u << kSlots) - 1;
constexpr unsigned kNeeded = kAllSlots & ~(1u << kVE1);  // (eye 1's own output, the negative control: when taken)
unsigned g_cap_bits = 0;
bool g_cap_bad = false;

// ---- the images (the playback thread)
struct Img {
    ID3D12Resource *game_vel = nullptr, *game_d = nullptr;  // the keys (the Velocity RT's reference is g_vel's)
    D3D12_RESOURCE_DESC dv{}, dd{};
    unsigned planes = 1;
    DXGI_FORMAT view_fmt = DXGI_FORMAT_UNKNOWN;  // the game's views of the Velocity RT (V2's view is made with it)
    ID3D12Resource* v2 = nullptr;  // eye 2's velocity; at rest RENDER_TARGET
    ID3D12Resource* rb = nullptr;  // readback, kSlots footprints
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[kSlots]{};
    UINT rows[kSlots]{};
    UINT64 rowbytes[kSlots]{};
    UINT64 rb_size = 0;
};
Img g_img;
ID3D12DescriptorHeap* g_rtv_heap = nullptr;  // one RTV (V2's), made once
// the game's "Velocity RT" (the playback thread): resolved from PostFx +0x3f0 (eye2_light's find_lb pattern), watched, and
// held with the mod's own reference (released through retire() when it changes)
ID3D12Resource* g_vel = nullptr;
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
void retire_all() {
    retire(g_img.v2);
    retire(g_img.rb);
    g_img = Img{};
    g_v2_fresh = false;  // (item 8f: another V2)
}

// the window from IN-V to OUT-V (the playback thread): the one-shot substitution and the draws' intervals between binds
bool g_window = false, g_armed = false, g_bound_v2 = false, g_win_subst = false, g_win_unarmed = false;
ID3D12GraphicsCommandList* g_win_cl = nullptr;
uint64_t g_mark = 0, g_good = 0, g_bad = 0;  // the thread's draws at the last bind; draws into V2 / into anything else
int g_binds = 0;
bool g_in2_pending = false;  // pass 2: the game Velocity RT and the depth at the velocity pass's own bind (the next bind)
std::atomic<bool> g_sub_flag{false};  // (any bind's first check) the window open or a bind owed
std::atomic<DWORD> g_ops_tid{0};
void update_flag() { g_sub_flag.store(g_window || g_in2_pending, std::memory_order_relaxed); }

// the worker's job
struct Job {
    ID3D12Resource* rb = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[kSlots]{};
    UINT rows[kSlots]{};
    UINT64 rowbytes[kSlots]{};
    UINT64 size = 0;
    unsigned bits = 0;
    bool cut = false, cpu = false, cpu_eq = false, f8_eq = false, tx_eq = false, rs_eq = false, sc_eq = false, states = false;
    int rs_first = -1;
    int ctl = 0;
    uint32_t rs1[kRsIds] = {}, rs2[kRsIds] = {};  // (r53) the render states at both calls' entries, for the log
    bool rs0_set = false;                          // (r53) eye 2's call ran with id 0 set to pass 2's ("prepass e2vel cull p2")
    bool cull_known = false, cull_eq = false;      // the cull in force at both calls (raw)
    bool rs_any = false;                           // (r54) any id differs, the draw's or not (the log, and rs_other)
    uint32_t cull1 = 0, cull2 = 0;
} g_job;
ID3D12Fence* g_fence = nullptr;
std::atomic<UINT64> g_fence_value{0};
HANDLE g_fence_event = nullptr;
std::mutex g_setup_mutex;

// the compare's results (the worker)
Count g_c_frames, g_c_cut, g_c_in_eq, g_c_cpu_eq, g_c_cpu_unread, g_c_in_v_diff, g_c_in_d_diff, g_c_in_s_diff, g_c_d_hi, g_c_out_exact,
    g_c_out_exact_eq, g_c_out_diff_eq, g_c_cut_exact_eq, g_c_cov_zero, g_c_neg_measured, g_c_neg_differ, g_c_ctl, g_c_ctl_differ, g_c_ctl_in_eq,
    g_c_map_failed, g_c_timeout;
Count g_c_states_unread, g_c_ndc_diff, g_c_f8_diff, g_c_tx_diff, g_c_rs_diff, g_c_sc_diff, g_c_p2w_zero, g_c_rs0_set, g_c_cull_diff, g_c_rs_other;
std::atomic<uint64_t> g_c_p2w_min{~0ull}, g_c_p2w_max{0};  // (r53) pass 2's own written texels (its output against its prior)  // (review item 4) the CPU's inputs, per part
std::atomic<int> g_c_rs_first{-1};  // the last differing frame's first differing render-state id
std::atomic<uint64_t> g_c_out_texels_max{0}, g_c_out_worst{0}, g_c_in_v_texels_max{0}, g_c_in_d_texels_max{0}, g_c_in_s_texels_max{0},
    g_c_cov_min{~0ull}, g_c_cov_max{0}, g_c_neg_min{~0ull}, g_c_ctl_min{~0ull};
std::atomic<int> g_c_box[4]{{-1}, {-1}, {-1}, {-1}};
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

// ---- the op queue (the markers' callbacks, then the next list call's poll)
enum Op : uint8_t { kPrep, kInV, kInVCap, kOutV, kOutVCap, kE1Cap, kIn2Cap, kOut2Cap };
uint8_t g_ops[16];
int g_nops = 0;
void push(Op o) {
    const DWORD tid = GetCurrentThreadId();
    const bool window = o == kInV || o == kInVCap || o == kOutV || o == kOutVCap;
    if (g_nops > 0 && tid != g_ops_tid.load(std::memory_order_relaxed)) {
        bump(g_ops_other_thread);
        if (window) exposed("a step's marker played on another thread (dropped)");
        else stand_down("a step's marker played on another thread (dropped)");
        return;
    }
    if (g_nops >= static_cast<int>(sizeof(g_ops))) {
        bump(g_ops_overflow);
        if (window) exposed("the step queue full (a step dropped)");
        else stand_down("the step queue full (a step dropped)");
        return;
    }
    g_ops_tid.store(tid, std::memory_order_relaxed);
    g_ops[g_nops++] = o;
    prepass::g_eye2_vel_pending_flag.store(true, std::memory_order_release);
}
void cb_prep(void*) { push(kPrep); }
void cb_inv(void*) { push(kInV); }
void cb_inv_cap(void*) { push(kInVCap); }
void cb_outv(void*) { push(kOutV); }
void cb_outv_cap(void*) { push(kOutVCap); }
void cb_e1_cap(void*) { push(kE1Cap); }
void cb_in2_cap(void*) { push(kIn2Cap); }
void cb_out2_cap(void*) { push(kOut2Cap); }

// ---- barriers and copies
D3D12_RESOURCE_BARRIER tr(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b, UINT sub) {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    x.Transition.Subresource = sub;
    return x;
}
constexpr UINT kAll = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
constexpr D3D12_RESOURCE_STATES kSrc = D3D12_RESOURCE_STATE_COPY_SOURCE, kDst = D3D12_RESOURCE_STATE_COPY_DEST,
                                kRt = D3D12_RESOURCE_STATE_RENDER_TARGET;
struct Batch {
    D3D12_RESOURCE_BARRIER b[8];
    UINT n = 0;
    void add(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES s, UINT sub = kAll) {
        if (a != s && n < 8) b[n++] = tr(r, a, s, sub);
    }
    void flush(ID3D12GraphicsCommandList* cl) {
        if (n) d3d::raw_barrier(cl, n, b);
        n = 0;
    }
};
bool bad_state(D3D12_RESOURCE_STATES s) { return s == kDst || s == kSrc; }
void copy_sub(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, UINT dsub, ID3D12Resource* src, UINT ssub) {
    D3D12_TEXTURE_COPY_LOCATION d{dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    d.SubresourceIndex = dsub;
    D3D12_TEXTURE_COPY_LOCATION s{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    s.SubresourceIndex = ssub;
    d3d::raw_copy_region(cl, &d, &s);
}
void copy_rb(ID3D12GraphicsCommandList* cl, int slot, ID3D12Resource* src, UINT ssub) {
    if (!g_img.rb) return;
    D3D12_TEXTURE_COPY_LOCATION d{g_img.rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    d.PlacedFootprint = g_img.fp[slot];
    D3D12_TEXTURE_COPY_LOCATION s{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    s.SubresourceIndex = ssub;
    d3d::raw_copy_region(cl, &d, &s);
    g_cap_bits |= 1u << slot;
}
// V2 (at rest RENDER_TARGET) to a readback slot
void v2_to_rb(ID3D12GraphicsCommandList* cl, int slot) {
    Batch x;
    x.add(g_img.v2, kRt, kSrc);
    x.flush(cl);
    copy_rb(cl, slot, g_img.v2, 0);
    x.add(g_img.v2, kSrc, kRt);
    x.flush(cl);
}
// the game's depth, both planes from the walk cut's followed states (eye2_light's gather), to two readback slots
bool depth_to_rb(ID3D12GraphicsCommandList* cl, int sd, int ss) {
    ID3D12Resource* d = prepass::kind_game(kKDepth);
    const unsigned planes = prepass::kind_planes(kKDepth);
    if (prepass::active() || !d || d != g_img.game_d || planes != g_img.planes || planes < 1 || planes > 2) return false;
    D3D12_RESOURCE_STATES st[2]{};
    for (unsigned p = 0; p < planes; ++p)
        if (!prepass::kind_followed(kKDepth, p, &st[p]) || bad_state(st[p])) return false;
    for (unsigned p = 0; p < planes; ++p)  // (the review of d6ac220f: the planes' states set on this list, else refused, counted)
        if (!prepass::kind_followed_fresh(kKDepth, p, cl, &st[p])) {
            prepass::follow_stale_note(prepass::kFollowE2vel);
            return false;
        }
    Batch x;
    for (unsigned p = 0; p < planes; ++p) x.add(d, st[p], kSrc, planes > 1 ? p : kAll);
    x.flush(cl);
    copy_rb(cl, sd, d, 0);
    if (planes > 1) copy_rb(cl, ss, d, 1);
    else g_cap_bits |= 1u << ss;
    for (unsigned p = 0; p < planes; ++p) x.add(d, kSrc, st[p], planes > 1 ? p : kAll);
    x.flush(cl);
    return true;
}

bool same_desc(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b) {
    return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format && a.DepthOrArraySize == b.DepthOrArraySize &&
           a.MipLevels == b.MipLevels && a.Flags == b.Flags && a.SampleDesc.Count == b.SampleDesc.Count;
}
// the game's Velocity RT, resolved from PostFx +0x3f0 (the RT object) as eye2_light's find_lb resolves the Light Buffer: the
// registered resource named "Velocity RT" the object points at in its first 0x100 bytes, directly or one pointer deep; the
// field it was found in kept (while +0x3f0 and that field hold the same values, the same resource). A newly found one is
// AddRef'd, the old one's reference retired, the barrier watch moved to it. No fallback by name alone (a released
// resource keeps its name)
uintptr_t g_vel_rt = 0, g_vel_o1 = 0;
intptr_t g_vel_o2 = -1;
uint64_t g_vel_field = 0;
bool vel_field(uintptr_t rt, uint64_t* v) {
    if (g_vel_o2 < 0) return rd(rt + g_vel_o1, v);
    uintptr_t p = 0;
    return rd(rt + g_vel_o1, &p) && p && rd(p + static_cast<uintptr_t>(g_vel_o2), v);
}
const char* find_vel() {
    uintptr_t p = 0, rt = 0;
    if (!rd(anchors::addr(Id::PostFxSingleton), &p) || !p || !rd(p + kVelRt, &rt) || !rt) return "PostFx or its Velocity RT (PostFx +0x3f0) not set";
    uint64_t v = 0;
    if (g_vel && rt == g_vel_rt && vel_field(rt, &v) && v == g_vel_field) return nullptr;
    bump(g_vel_resolves);
    char where[64] = "";
    ID3D12Resource* r =
        static_cast<ID3D12Resource*>(const_cast<void*>(d3d::resource_in_object(reinterpret_cast<const void*>(rt), 0x100, "Velocity RT", where, sizeof(where))));
    if (!r) return "the Velocity RT not found behind PostFx +0x3f0 (its RT object's first 0x100 bytes, one pointer deep)";
    unsigned long long a = 0, b = 0;
    const int n = sscanf_s(where, "+%llx->+%llx", &a, &b);
    if (n < 1) return "the Velocity RT's field not parsed";
    if (r != g_vel) {
        if (!add_ref(r)) return "the Velocity RT found could not be referenced";
        if (g_vel) {
            d3d::watch_remove(g_vel);
            retire(g_vel);  // (the mod's reference, released after 8 frames)
        }
        g_vel = r;
        d3d::watch_add(g_vel);
        bump(g_vel_changes);
        log::info("[e2vel] the Velocity RT: PostFx +0x3f0 %s (resolution %llu)", where, u(g_vel_resolves));
    }
    g_vel_rt = rt;
    g_vel_o1 = static_cast<uintptr_t>(a);
    g_vel_o2 = n == 2 ? static_cast<intptr_t>(b) : -1;
    if (!vel_field(rt, &g_vel_field)) g_vel_field = 0;  // (then the next call resolves again)
    return nullptr;
}
// the Velocity RT's state from the barrier watch: the last transition the game recorded for it in this list on this thread
bool vel_state(ID3D12GraphicsCommandList* cl, D3D12_RESOURCE_STATES* s) {
    ID3D12GraphicsCommandList* wcl = nullptr;
    uint32_t st = 0;
    if (!g_vel || g_vel != g_img.game_vel || !d3d::watched_state(g_vel, &wcl, &st) || wcl != cl || bad_state(static_cast<D3D12_RESOURCE_STATES>(st)))
        return false;
    *s = static_cast<D3D12_RESOURCE_STATES>(st);
    return true;
}
// V2 for the current Velocity RT and depth (made again, the old one retired, when the game's change)
const char* ensure() {
    if (const char* why = find_vel()) return why;
    ID3D12Resource* d = prepass::kind_game(kKDepth);
    const unsigned planes = prepass::kind_planes(kKDepth);
    ID3D12Resource* arr = prepass::velocity_array();
    if (!d || planes < 1 || planes > 2) return "the depth not known";
    if (!arr) return "the pre-pass velocity array not made yet";
    const D3D12_RESOURCE_DESC dv = g_vel->GetDesc(), dd = d->GetDesc(), da = arr->GetDesc();
    if (dv.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dv.DepthOrArraySize != 1 || dv.MipLevels != 1 || dv.SampleDesc.Count != 1 ||
        !(dv.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) || dd.MipLevels != 1 || dd.DepthOrArraySize != 1)
        return "the Velocity RT is not one 2D render-target subresource (or the depth has mips or slices)";
    if (da.Width != dv.Width || da.Height != dv.Height || da.Format != dv.Format || da.DepthOrArraySize != 2 || da.MipLevels != 1)
        return "the velocity array is not the Velocity RT's (size, format, 2 slices)";
    DXGI_FORMAT vf = DXGI_FORMAT_UNKNOWN;  // (the game's views' format: the PSO's; one only)
    {
        uint64_t h[8] = {};
        const int n = d3d::rtv_handles_of(g_vel, h, 8);
        for (int i = 0; i < n; ++i) {
            int f = 0;
            if (d3d::view_resource(static_cast<uintptr_t>(h[i]), &f) != g_vel) continue;
            const DXGI_FORMAT g = f ? static_cast<DXGI_FORMAT>(f) : dv.Format;
            if (vf == DXGI_FORMAT_UNKNOWN) vf = g;
            else if (vf != g) return "the Velocity RT's views have two formats";
        }
        if (vf == DXGI_FORMAT_UNKNOWN) vf = dv.Format;
    }
    if (g_img.v2 && g_img.game_vel == g_vel && g_img.game_d == d && g_img.planes == planes && g_img.view_fmt == vf && same_desc(g_img.dv, dv) &&
        same_desc(g_img.dd, dd)) {
        // (the delta review) V2 still fits: ready again for the RT object found now (an unarmed IN-V cleared these, and a
        // new RT object over the same resource changed g_vel_rt), else tail would send preps for good
        g_ready_rt.store(g_vel_rt);
        g_img_ready = true;
        return nullptr;
    }
    if (g_window) return "a window is open (IN-V without its OUT-V): not made again now";
    g_img_ready = false;
    if (g_cap.load() != kIdle && g_cap.load() != kArmed) g_cap_bad = true;
    retire_all();
    ID3D12Device* dev = state::device.load();
    if (!dev) return "no device";
    if (!g_rtv_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtv_heap)))) {
            g_rtv_heap = nullptr;
            bump(g_make_failed);
            return "the RTV heap not made";
        }
    }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC c = dv;
    c.Alignment = 0;
    bool ok = SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &c, kRt, nullptr, IID_PPV_ARGS(&g_img.v2))) && g_img.v2;
    UINT64 off = 0;
    for (int s = 0; ok && s < kSlots; ++s) {
        const bool depth = s == kVD1 || s == kVD2 || s == kVS1 || s == kVS2;
        const bool stencil = s == kVS1 || s == kVS2;
        UINT64 total = 0;
        if (stencil && planes < 2) {  // (no stencil plane: an empty slot)
            g_img.fp[s] = {};
            g_img.rows[s] = 0;
            g_img.rowbytes[s] = 0;
            continue;
        }
        dev->GetCopyableFootprints(depth ? &dd : &dv, stencil ? 1 : 0, 1, off, &g_img.fp[s], &g_img.rows[s], &g_img.rowbytes[s], &total);
        off = (g_img.fp[s].Offset + total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~static_cast<UINT64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    }
    if (ok) {
        D3D12_HEAP_PROPERTIES rh{};
        rh.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rdsc{};
        rdsc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rdsc.Width = off;
        rdsc.Height = 1;
        rdsc.DepthOrArraySize = 1;
        rdsc.MipLevels = 1;
        rdsc.SampleDesc.Count = 1;
        rdsc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ok = SUCCEEDED(dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &rdsc, kDst, nullptr, IID_PPV_ARGS(&g_img.rb))) && g_img.rb;
        g_img.rb_size = off;
    }
    if (!ok) {
        bump(g_make_failed);
        retire_all();
        return "V2 or the readback not made";
    }
    D3D12_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = vf;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_img.rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(g_img.v2, &rv, g_img.rtv);
    g_img.v2->SetName(L"RDRVR eye 2 Velocity (PrepassEye2Velocity)");
    g_img.game_vel = g_vel;
    g_img.game_d = d;
    g_img.dv = dv;
    g_img.dd = dd;
    g_img.planes = planes;
    g_img.view_fmt = vf;
    g_ready_rt.store(g_vel_rt);
    g_img_ready = true;
    bump(g_made);
    log::info("[e2vel] V2 made: the Velocity RT %llux%u (format %d, view format %d), the depth %llux%u (format %d, %u planes)",
              static_cast<unsigned long long>(dv.Width), dv.Height, static_cast<int>(dv.Format), static_cast<int>(vf),
              static_cast<unsigned long long>(dd.Width), dd.Height, static_cast<int>(dd.Format), planes);
    return nullptr;
}

// ---- the steps (the playback thread, at the list call after their marker)
void prep() {
    if (const char* why = ensure()) std::snprintf(g_why, sizeof(g_why), "prep: %s", why);
}
// IN-V: V2 seeded from the array's slice 1, a capture's inputs read back, the one-shot substitution armed. Eye 2's call is
// recorded already: V2 is armed whenever it is the current Velocity RT's, whatever else is refused (only the seed and the
// capture then fail); not armed, a draw of the pass lands in the game's Velocity RT and OUT-V stands the module down
void inv_step(ID3D12GraphicsCommandList* cl, bool cap) {
    g_v2_fresh = false;  // (item 8f: set again only by this IN-V's OUT-V)
    if (cap) {
        g_cap_bits = 0;
        g_cap_bad = false;
    }
    if (g_window) {  // (the last OUT-V never ran: its draws are not known)
        bump(g_unclosed);
        g_cap_bad = true;
        exposed("IN-V found the last OUT-V not run");
    }
    const char* why = find_vel();
    if (!why && (!g_img.v2 || g_img.game_vel != g_vel || !same_desc(g_img.dv, g_vel->GetDesc()))) why = "V2 is not the current Velocity RT's";
    if (!why && g_stood.load()) why = "stood down (nothing is armed after a stand-down)";  // (review item 1)
    g_window = true;
    g_armed = false;
    g_bound_v2 = false;
    g_win_subst = false;
    g_win_unarmed = false;
    g_win_cl = cl;
    g_mark = d3d::thread_draws();
    g_good = g_bad = 0;
    g_binds = 0;
    if (why) {
        // (review item 3) eye 2's call is recorded already: if it draws, it draws into the game's Velocity RT (eye 1's DLSS
        // input) until OUT-V stands the module down; counted (unarmed_exposed). The render thread checks the RT object V2 was
        // made for before the call (g_ready_rt); what is left is a new resource behind the same object, or a stand-down
        bump(g_inv_unarmed);
        g_win_unarmed = true;
        g_img_ready = false;  // (the render thread asks for a prep)
        g_ready_rt.store(0);
        g_cap_bad = true;
        std::snprintf(g_why, sizeof(g_why), "IN-V not armed: %s", why);
        update_flag();
        return;
    }
    const bool pred = d3d::thread_predicated();
    ID3D12Resource* arr = prepass::velocity_array();
    bool seeded = false;
    if (!pred && arr && !prepass::active()) {
        const D3D12_RESOURCE_DESC da = arr->GetDesc();
        if (da.Width == g_img.dv.Width && da.Height == g_img.dv.Height && da.Format == g_img.dv.Format && da.DepthOrArraySize == 2 && da.MipLevels == 1) {
            Batch x;
            x.add(arr, kRt, kSrc, 1);  // (slice 1's subresource only: the array at rest RENDER_TARGET)
            x.add(g_img.v2, kRt, kDst);
            x.flush(cl);
            copy_sub(cl, g_img.v2, 0, arr, 1);
            x.add(arr, kSrc, kRt, 1);
            x.add(g_img.v2, kDst, kRt);
            x.flush(cl);
            seeded = true;
        }
    }
    bump(seeded ? g_seeded : g_seed_failed);
    if (!seeded) g_cap_bad = true;
    if (cap && !pred) {
        v2_to_rb(cl, kVIn1);
        if (!depth_to_rb(cl, kVD1, kVS1)) g_cap_bad = true;
    } else if (cap) {
        g_cap_bad = true;
    }
    g_armed = true;
    bump(g_inv_ok);
    update_flag();
}
// a draw interval ends at a bind (or OUT-V): its draws counted into V2 or into something else
void close_interval() {
    const uint64_t now = d3d::thread_draws();
    const uint64_t n = now - g_mark;
    if (g_bound_v2) g_good += n;
    else g_bad += n;
    g_mark = now;
}
void outv_step(ID3D12GraphicsCommandList* cl, bool cap) {
    if (!g_window) {  // (its IN-V did not run)
        bump(g_unpaired);
        g_cap_bad = true;
        update_flag();
        return;
    }
    close_interval();
    g_window = false;
    g_armed = false;
    g_bound_v2 = false;
    update_flag();
    if (g_bad) {
        g_cap_bad = true;
        if (g_win_unarmed) bump(g_unarmed_exposed);
        char b[200];
        std::snprintf(b, sizeof(b), "eye 2's velocity pass drew %llu time(s) not into V2 (binds %d, into V2 %llu): eye 1's Velocity RT written",
                      static_cast<unsigned long long>(g_bad), g_binds, static_cast<unsigned long long>(g_good));
        exposed(b);
        return;
    }
    if (!g_good) {  // (the pass's gate closed in the call: no draw; or a substitution with no draw after it)
        bump(g_win_subst ? g_sub_nodraw : g_outv_nodraw);
        g_cap_bad = true;
        return;
    }
    if (g_good > 1) bump(g_extra_draws);
    bump(g_outv_ok);
    g_v2_fresh = !g_stood.load();  // (item 8f) V2 is this pass 1's eye-2 velocity
    if (cap && !d3d::thread_predicated() && g_img.v2) v2_to_rb(cl, kVOut1);
    else if (cap) g_cap_bad = true;
}
// a game Velocity RT read back at a list call (OUT2V, SE1) from the state the watch saw in this list
bool read_vel(ID3D12GraphicsCommandList* cl, int slot, Count& refused, const char* step) {
    D3D12_RESOURCE_STATES vs{};
    char b[160];
    if (d3d::thread_predicated() || !g_img.rb || !vel_state(cl, &vs)) {
        std::snprintf(b, sizeof(b), "%s: %s", step, d3d::thread_predicated() ? "the list is predicated" : "the Velocity RT's state not seen in this list");
        refuse(refused, b);
        return false;
    }
    Batch x;
    x.add(g_vel, vs, kSrc);
    x.flush(cl);
    copy_rb(cl, slot, g_vel, 0);
    x.add(g_vel, kSrc, vs);
    x.flush(cl);
    return true;
}
// pass 2 (a capture): at the velocity pass's own bind (after the engine's barrier): its prior and the depth
void in2_at_bind(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts) {
    if (g_cap.load() != kRec2) return;
    const void* r = n == 1 && rts ? d3d::view_resource(static_cast<uintptr_t>(rts[0].ptr)) : nullptr;
    if (!r || r != g_vel) {
        refuse(g_in2_refused, "IN2V: the first bind after it is not the velocity pass's (one target, the Velocity RT)");
        g_cap_bad = true;
        return;
    }
    if (!read_vel(cl, kVIn2, g_in2_refused, "IN2V")) {
        g_cap_bad = true;
        return;
    }
    if (!depth_to_rb(cl, kVD2, kVS2)) {
        refuse(g_in2_refused, "IN2V: the depth's state not followed (or not V2's depth)");
        g_cap_bad = true;
        return;
    }
    bump(g_in2_ok);
}
void out2_done() {
    if ((g_cap_bits & kNeeded) == kNeeded && !g_cap_bad && g_img.rb) {
        g_job.rb = g_img.rb;
        std::memcpy(g_job.fp, g_img.fp, sizeof(g_job.fp));
        std::memcpy(g_job.rows, g_img.rows, sizeof(g_job.rows));
        std::memcpy(g_job.rowbytes, g_img.rowbytes, sizeof(g_job.rowbytes));
        g_job.size = g_img.rb_size;
        g_job.bits = g_cap_bits;
        g_job.cut = g_cap_cut.load();
        g_job.ctl = g_cap_ctl.load();
        std::atomic_thread_fence(std::memory_order_acquire);
        g_job.cpu = g_cap_cpu1.load() && g_cap_cpu2.load();
        g_job.cpu_eq = g_job.cpu && !std::memcmp(g_cap_ndc1, g_cap_ndc2, sizeof(g_cap_ndc1));
        g_job.f8_eq = g_cap_f8_1.load() && g_cap_f8_1.load() == g_cap_f8_2.load();
        g_job.states = g_cap_in1.ok && g_cap_in2.ok && g_cap_tx1_ok.load() && g_cap_tx2_ok.load();
        g_job.tx_eq = g_job.states && !std::memcmp(g_cap_tx1, g_cap_tx2, sizeof(g_cap_tx1));
        g_job.sc_eq = g_job.states && !std::memcmp(g_cap_in1.sc, g_cap_in2.sc, sizeof(g_cap_in1.sc));
        g_job.rs_first = -1;
        g_job.rs_any = false;
        for (int id = 0; id < kRsIds; ++id) {
            if (g_cap_in1.rs[id] == g_cap_in2.rs[id]) continue;
            g_job.rs_any = true;  // (logged)
            if (rs_feeds(id) && g_job.rs_first < 0) g_job.rs_first = id;  // (r54: the class: only the ids that reach the draw)
        }
        g_job.rs_eq = g_job.states && g_job.rs_first < 0;
        // (the delta review's should-fix) the cull in force, raw, compared too (cache[0] alone can disagree with it)
        g_job.cull_known = g_cap_in1.cull_ok && g_cap_in2.cull_ok;
        g_job.cull_eq = g_job.cull_known && g_cap_in1.cull == g_cap_in2.cull && g_cap_in1.mirror == g_cap_in2.mirror;
        g_job.cull1 = g_cap_in1.cull;
        g_job.cull2 = g_cap_in2.cull;
        std::memcpy(g_job.rs1, g_cap_in1.rs, sizeof(g_job.rs1));
        std::memcpy(g_job.rs2, g_cap_in2.rs, sizeof(g_job.rs2));
        g_job.rs0_set = g_cap_rs0_set.load();
        g_ready_frame.store(state::presents.load());
        int s = kRec2;
        if (!g_cap.compare_exchange_strong(s, kReady)) bump(g_cap_incomplete);
        return;
    }
    bump(g_cap_incomplete);
    int s = kRec2;
    g_cap.compare_exchange_strong(s, kArmed);
}

// the second substitution slot (d3d::set_rtv_substitute2): every game bind, any thread; the window's thread only
bool subst(ID3D12GraphicsCommandList* cl, unsigned n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, const D3D12_CPU_DESCRIPTOR_HANDLE* ds,
           D3D12_CPU_DESCRIPTOR_HANDLE* out) {
    if (!g_sub_flag.load(std::memory_order_relaxed) || GetCurrentThreadId() != g_ops_tid.load(std::memory_order_relaxed)) return false;
    bool res = false;
    if (g_window) {
        close_interval();
        ++g_binds;
        g_bound_v2 = false;
        if (g_armed) {
            g_armed = false;  // (one-shot: the first bind after IN-V, whichever it is; none after a stand-down: review item 1)
            int f = 0;
            const void* r = n == 1 && rts && ds ? d3d::view_resource(static_cast<uintptr_t>(rts[0].ptr), &f) : nullptr;
            const DXGI_FORMAT vf = f ? static_cast<DXGI_FORMAT>(f) : g_img.dv.Format;
            if (!g_stood.load() && cl == g_win_cl && r && r == g_vel && r == g_img.game_vel && g_img.v2 && vf == g_img.view_fmt && out) {
                *out = g_img.rtv;
                g_bound_v2 = true;
                g_win_subst = true;
                bump(g_subst);
                res = true;
            } else {
                bump(g_subst_missed);
            }
        }
    }
    if (g_in2_pending) {
        g_in2_pending = false;
        in2_at_bind(cl, n, rts);
    }
    update_flag();
    return res;
}

// ---- the compare (the worker)
struct Diff {
    uint64_t texels = 0, worst = 0, hi = 0;
    int box[4] = {-1, -1, -1, -1};
};
Diff diff(const uint8_t* base, const Job& j, int a, int b, bool depth) {
    Diff d;
    const UINT w = j.fp[a].Footprint.Width, h = j.rows[a];
    const UINT64 rb = j.rowbytes[a];
    if (!w && !j.fp[b].Footprint.Width) return d;  // (an empty slot in both: no stencil plane)
    const UINT bpt = w ? static_cast<UINT>(rb / w) : 0;
    if (!bpt || j.fp[a].Footprint.Width != j.fp[b].Footprint.Width || j.rows[a] != j.rows[b] || j.rowbytes[a] != j.rowbytes[b]) {
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
            if (depth && bpt == 4) {  // (plane 0 of D24S8: the depth in the low 24 bits, the high byte undefined)
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
            } else if (bpt == 4) {  // (R16G16_FLOAT: the larger of the two halves' bit distances, as integers)
                uint16_t ha[2], hb[2];
                std::memcpy(ha, ta, 4);
                std::memcpy(hb, tb, 4);
                for (int k = 0; k < 2; ++k) {
                    const uint64_t dd = ha[k] > hb[k] ? ha[k] - hb[k] : hb[k] - ha[k];
                    if (dd > d.worst) d.worst = dd;
                }
            } else {
                for (UINT k = 0; k < bpt; ++k) {
                    const uint64_t dd = ta[k] > tb[k] ? ta[k] - tb[k] : tb[k] - ta[k];
                    if (dd > d.worst) d.worst = dd;
                }
            }
            ++d.texels;
            const int xi = static_cast<int>(x), yi = static_cast<int>(y);
            if (d.box[0] < 0 || xi < d.box[0]) d.box[0] = xi;
            if (d.box[1] < 0 || yi < d.box[1]) d.box[1] = yi;
            if (xi > d.box[2]) d.box[2] = xi;
            if (yi > d.box[3]) d.box[3] = yi;
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
    const Diff in_v = diff(p, j, kVIn1, kVIn2, false), in_d = diff(p, j, kVD1, kVD2, true), in_s = diff(p, j, kVS1, kVS2, false),
               out = diff(p, j, kVOut1, kVOut2, false), cov = diff(p, j, kVOut1, kVIn1, false);
    const bool have_e1 = (j.bits & (1u << kVE1)) != 0;
    const Diff neg = have_e1 ? diff(p, j, kVOut2, kVE1, false) : Diff{};
    const Diff p2w = diff(p, j, kVOut2, kVIn2, false);  // (r53) the texels pass 2's own call wrote: none = nothing to draw
    D3D12_RANGE none{0, 0};
    j.rb->Unmap(0, &none);
    bump(g_c_frames);
    if (j.cut) bump(g_c_cut);
    const bool in_eq = in_v.texels == 0 && in_d.texels == 0 && in_s.texels == 0;
    // (review item 4) the CPU's inputs: NDCToPrevNDC (not for a control frame: changed on purpose), +0xf8, TexelSize, the
    // inherited render states 0..0x2b and the scissor
    const bool states_eq = j.f8_eq && j.tx_eq && j.rs_eq && j.sc_eq && (!g_cull || j.cull_eq);
    if (g_cull && !j.cull_eq) bump(g_c_cull_diff);
    const bool cpu_eq = j.cpu_eq && states_eq;
    if (!j.cpu) bump(g_c_cpu_unread);
    if (!j.states) bump(g_c_states_unread);
    if (j.cpu && !j.cpu_eq && !j.ctl) bump(g_c_ndc_diff);
    if (!j.f8_eq) bump(g_c_f8_diff);
    if (j.states && !j.tx_eq) bump(g_c_tx_diff);
    if (j.states && !j.sc_eq) bump(g_c_sc_diff);
    if (p2w.texels == 0 || p2w.texels == ~0ull) bump(g_c_p2w_zero);
    if (p2w.texels != ~0ull) {
        store_min(g_c_p2w_min, p2w.texels);
        store_max(g_c_p2w_max, p2w.texels);
    }
    if (j.rs0_set) bump(g_c_rs0_set);
    if (j.states && j.rs_any && j.rs_eq) bump(g_c_rs_other);  // (r54: only ids that do not reach the draw differ)
    if (j.states && j.rs_any) {  // (r53) every differing id, p1 / p2 (id 0: the cull mode; * one the draw reads)
        char line[600];
        int n = std::snprintf(line, sizeof(line), "[e2vel] render states differ at the calls' entries (eye 2's in pass 1 / pass 2's%s):",
                              j.rs0_set ? "; id 0 set to pass 2's last" : "");
        if (j.cull_known)
            n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " the cull in force %u / %u;", j.cull1, j.cull2);
        for (int id = 0; id < kRsIds && n > 0 && n < static_cast<int>(sizeof(line)) - 24; ++id)
            if (j.rs1[id] != j.rs2[id])
                n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " id %#x%s%s %u / %u;", id, rs_feeds(id) ? "*" : "", id == 0 ? " (cull)" : "",
                                   j.rs1[id], j.rs2[id]);
        log::limited("e2vel.rs", 40, "%s", line);
    }
    if (j.states && !j.rs_eq) {
        bump(g_c_rs_diff);
        g_c_rs_first.store(j.rs_first);
    }
    if (in_v.texels) bump(g_c_in_v_diff);
    if (in_d.texels) bump(g_c_in_d_diff);
    if (in_s.texels) bump(g_c_in_s_diff);
    if (in_d.hi) bump(g_c_d_hi);
    if (cov.texels == 0 || cov.texels == ~0ull) bump(g_c_cov_zero);
    if (cov.texels != ~0ull) {
        store_min(g_c_cov_min, cov.texels);
        store_max(g_c_cov_max, cov.texels);
    }
    if (j.ctl) {  // (a positive control frame: eye 2's matrix changed on purpose; must differ)
        bump(g_c_ctl);
        if (in_eq && states_eq) bump(g_c_ctl_in_eq);  // (review item 5: every input but the matrix equal)
        if (out.texels) bump(g_c_ctl_differ);
        store_min(g_c_ctl_min, out.texels);
    } else {
        if (in_eq) bump(g_c_in_eq);
        if (cpu_eq) bump(g_c_cpu_eq);
        if (out.texels == 0) bump(g_c_out_exact);
        if (in_eq && cpu_eq) {
            bump(out.texels == 0 ? g_c_out_exact_eq : g_c_out_diff_eq);
            if (j.cut && out.texels == 0) bump(g_c_cut_exact_eq);
        }
        if (have_e1) {
            bump(g_c_neg_measured);
            if (neg.texels) bump(g_c_neg_differ);
            store_min(g_c_neg_min, neg.texels);
        }
        if (out.texels)
            for (int i = 0; i < 4; ++i) g_c_box[i].store(out.box[i]);
        store_max(g_c_out_texels_max, out.texels);
        store_max(g_c_out_worst, out.worst);
    }
    store_max(g_c_in_v_texels_max, in_v.texels);
    store_max(g_c_in_d_texels_max, in_d.texels);
    store_max(g_c_in_s_texels_max, in_s.texels);
    log::limited("e2vel.cmp", 40, "[e2vel] compared (%s%s): inputs prior %llu, depth %llu, stencil %llu texels differ, CPU %s; eye 2's velocity %llu "
                 "texels differ (worst %llu, box %d,%d..%d,%d); written %llu; against eye 1's %s",
                 j.cut ? "cut" : "not cut", j.ctl == 1 ? ", control nudge" : j.ctl == 2 ? ", control eye1" : "",
                 static_cast<unsigned long long>(in_v.texels), static_cast<unsigned long long>(in_d.texels), static_cast<unsigned long long>(in_s.texels),
                 !j.cpu ? "unread" : cpu_eq ? "equal" : "differs", static_cast<unsigned long long>(out.texels), static_cast<unsigned long long>(out.worst),
                 out.box[0], out.box[1], out.box[2], out.box[3], static_cast<unsigned long long>(cov.texels),
                 have_e1 ? (neg.texels ? "differs" : "EQUAL") : "not taken");
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

// ---- the frame end (the presenting thread)
void frame_end(uint64_t f) {
    {
        std::lock_guard lock(g_retire_mutex);
        const int cs = g_cap.load();
        if (cs == kIdle || cs == kArmed)
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
        if (g_cap.compare_exchange_strong(s, kArmed)) bump(g_cap_incomplete);
        return;
    }
    if (s == kReady && f >= g_ready_frame.load() + 2) {
        ID3D12CommandQueue* q = state::present_queue.load();
        if (!q || !g_fence || !g_fence_event) return;
        const UINT64 v = g_fence_value.load() + 1;
        g_fence_value.store(v);
        g_cap.store(kWait);
        q->Signal(g_fence, v);
        g_fence->SetEventOnCompletion(v, g_fence_event);
    }
}

// ---- the render thread
// eye 2's call under SEH (no C++ objects in this frame): its NDCToPrevNDC into +0x4b0 and the variable, +0xf8 the main
// depth, the pass through the trampoline; then eye 1's NDCToPrevNDC back (also after a fault)
bool call_pass(uintptr_t p, uintptr_t eff, uintptr_t owner, int handle, int htex, const float* e2, const float* e1, uintptr_t depth, float* used,
               bool* have_used, float* tx, bool* have_tx) {
    const SetVar_t set = reinterpret_cast<SetVar_t>(anchors::addr(Id::EffectSetVar));
    bool ok = true, have_f8 = false;
    uintptr_t f8_saved = 0;
    __try {
        std::memcpy(reinterpret_cast<void*>(p + kNdcToPrev), e2, 64);
        set(reinterpret_cast<void*>(owner), reinterpret_cast<void*>(eff), handle, e2, 0x40, 1);
        *have_used = effect_value(eff, handle, used);
        f8_saved = *reinterpret_cast<const uintptr_t*>(p + kDepthRt);  // (review item 2: put back after the call, whatever it held)
        have_f8 = true;
        *reinterpret_cast<uintptr_t*>(p + kDepthRt) = depth;  // (SceneRender's own store before its call, the same value)
        cut_velocity::call_velocity(reinterpret_cast<void*>(p));
        *have_tx = effect_value(eff, htex, tx, 16);  // (TexelSize: set inside the call)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    __try {
        std::memcpy(reinterpret_cast<void*>(p + kNdcToPrev), e1, 64);
        set(reinterpret_cast<void*>(owner), reinterpret_cast<void*>(eff), handle, e1, 0x40, 1);  // (re-armed: BeginPass unmapped the CB)
        if (have_f8) *reinterpret_cast<uintptr_t*>(p + kDepthRt) = f8_saved;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}
bool raw_write(uintptr_t a, const void* in, size_t n) {
    if (!a) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(a), in, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// the cull state written raw (cache[0], the mirror, the cull), the dirty byte set when the cull in force changes, as
// RenderStateSet does
bool cull_write(const CullState& c) {
    uint32_t now = 0;
    const bool changed = !rd(g_cull, &now) || now != c.cull;
    const uint8_t one = 1;
    bool ok = raw_write(g_rs_cache, &c.c0, 4) && raw_write(g_cull_mirror, &c.mirror, 4) && raw_write(g_cull, &c.cull, 4);
    if (changed) ok = raw_write(g_cull_dirty, &one, 1) && ok;
    return ok;
}
// "cull p2": the cull state as pass 2's call had it, set raw; refused (counted) unless the state in force now is the one
// RenderStateSet would make of cache[0] (only then is what is put back after the call the state that was there)
bool rs0_to(CullState* was) {
    if (!g_p2_cull_ok || !g_cull) {
        bump(g_rs0_none);
        return false;
    }
    uint32_t m = 0, c = 0;
    if (!cull_read(was)) {
        bump(g_rs0_failed);
        return false;
    }
    if (!cull_of(was->c0, &m, &c) || m != was->mirror || c != was->cull) {
        bump(g_rs0_inconsistent);
        return false;
    }
    if (was->c0 == g_p2_cull.c0 && was->mirror == g_p2_cull.mirror && was->cull == g_p2_cull.cull) {
        bump(g_rs0_same);
        return false;
    }
    if (!cull_write(g_p2_cull)) {
        cull_write(*was);
        bump(g_rs0_failed);
        return false;
    }
    bump(g_rs0_sets);
    return true;
}
void rs0_back(const CullState& was) {  // (every path after a set: the three put back raw)
    if (!cull_write(was)) bump(g_rs0_failed);
}
// eye2_light's (L), pass 1, right after eye 2's lights (eye2_light::set_light_tail)
void tail() {
    bump(g_calls);
    if (!g_on.load(std::memory_order_relaxed) || g_stood.load(std::memory_order_relaxed)) {
        bump(g_off_calls);
        return;
    }
    uintptr_t p = 0;
    int tech = 0;
    uint8_t mb = 0;
    if (!rd(anchors::addr(Id::PostFxSingleton), &p) || !p || !rd(p + kTech, &tech) || !rd(p + kMotionBlur, &mb)) {
        bump(g_no_postfx);
        return;
    }
    if (!(static_cast<unsigned>(tech - 2) <= 3u || mb)) {  // (0x873955..0x873970 [V]; the upscaler-only case left to OUT-V's count)
        bump(g_gate_closed);  // (FXAA, motion blur off: the game's pass does not run either)
        return;
    }
    if (tech != 5) {  // (DLSS only: TAA, FSR or the motion blur under FXAA are not this step's)
        bump(g_not_dlss);
        return;
    }
    float ndc2[16], ndc1[16];
    uint64_t nf = 0, nseq = 0;
    // (this pass 1's: made after the last pass 1's velocity call, at this pass's scene wait; not used before)
    if (!taa::eye2_ndc(ndc2, &nf, &nseq) || nseq == g_seq_p1_end || nseq == g_last_seq) {
        bump(g_no_ndc);
        return;
    }
    if (!prepass::thread_stream_ok()) {
        bump(g_no_stream);
        return;
    }
    uintptr_t vel_rt = 0;  // (review item 3) the Velocity RT object V2 was made for, checked here before eye 2's call is recorded
    if (!g_img_ready.load() || !rd(p + kVelRt, &vel_rt) || !vel_rt || vel_rt != g_ready_rt.load()) {
        if (prepass::stream_marker(&cb_prep)) bump(g_preps);
        return;
    }
    uintptr_t eff = 0, owner = 0, own = 0, depth = 0;
    int handle = 0, htex = 0;
    if (!rd(p + kTaaEffect, &eff) || !eff || !rd(eff + kEffOwner, &owner) || !owner || !rd(p + kVarNdcToPrev, &handle) || handle <= 0 || handle > 4096 ||
        !rd(p + kVarTexel, &htex) ||
        !raw_read(p + kNdcToPrev, ndc1, sizeof(ndc1)) || !rd(anchors::addr(Id::ShadowCollectorOwner), &own) || !own || !rd(own + kMainDepth, &depth) ||
        !depth) {
        bump(g_no_postfx);
        return;
    }
    const int ctl = g_control.load(std::memory_order_relaxed);
    float use[16];
    std::memcpy(use, ctl == 2 ? ndc1 : ndc2, sizeof(use));
    if (ctl == 1) use[12] += 1e-3f;  // ([3][0], row-major: about 0.5 w 1e-3 render pixels, velocity2.md 4)
    LARGE_INTEGER t0{}, t1{};
    QueryPerformanceCounter(&t0);
    // (r53) "cull p2": id 0 (the cull mode) as pass 2's call had it, before the capture's snapshot and the call; put back after
    CullState rs0_was{};
    const bool rs0_set = g_rs0_mode.load(std::memory_order_relaxed) == 1 && rs0_to(&rs0_was);
    int s = kArmed;
    const bool cap = g_cap.compare_exchange_strong(s, kRec1);
    if (cap) {
        g_cap_since.store(state::presents.load());
        g_cap_p1 = g_p1_count + 1;  // (eye 1's call of this pass 1 counts it)
        g_cap_ctl.store(ctl);
        g_cap_cpu1.store(false);
        g_cap_cpu2.store(false);
        g_cap_f8_1.store(depth);
        g_cap_f8_2.store(0);
        g_cap_tx1_ok.store(false);
        g_cap_tx2_ok.store(false);
        take_inherited(&g_cap_in1);  // (review item 4: eye 2's lights' leftovers, at the call's entry)
        g_cap_rs0_set.store(rs0_set);
        g_cap_in2 = Inherited{};
        bump(g_cap_started);
    }
    if (!prepass::stream_marker(cap ? &cb_inv_cap : &cb_inv)) {
        bump(g_no_stream);
        if (cap) g_cap.store(kArmed);
        if (rs0_set) rs0_back(rs0_was);
        return;
    }
    g_last_seq = nseq;
    float used[16] = {}, tx[4] = {};
    bool have_used = false, have_tx = false;
    const bool ok = call_pass(p, eff, owner, handle, htex, use, ndc1, depth, used, &have_used, tx, &have_tx);
    if (rs0_set) rs0_back(rs0_was);
    if (cap) {
        std::memcpy(g_cap_ndc1, used, sizeof(used));
        std::memcpy(g_cap_tx1, tx, sizeof(tx));
        g_cap_tx1_ok.store(have_tx);
        std::atomic_thread_fence(std::memory_order_release);
        g_cap_cpu1.store(have_used);
    }
    const bool out = prepass::stream_marker(cap ? &cb_outv_cap : &cb_outv);
    QueryPerformanceCounter(&t1);
    g_ticks.fetch_add(static_cast<uint64_t>(t1.QuadPart - t0.QuadPart), std::memory_order_relaxed);
    bump(g_runs);
    if (ctl) bump(g_ctl_runs);
    g_ran_pass1 = true;
    std::memcpy(g_e1_saved, ndc1, sizeof(g_e1_saved));
    g_ran_ctl = ctl;
    if (!out) {
        bump(g_out_marker_failed);
        exposed("OUT-V's marker not appended after eye 2's velocity pass");
    }
    if (!ok) {
        bump(g_fault);
        // (the delta review, accepted for a test switch) OUT-V is already in the stream, so this frame's IN-V plays after
        // g_stood and refuses: the faulting call's draws, if any, land in the game's Velocity RT for that one frame
        stand_down("a fault in eye 2's velocity pass (EffectSetVar, FUN_140873920, or eye 1's NDCToPrevNDC put back)");
    }
}
// the shared hook on 0x873920 (cut_velocity::set_e2v), around the game's own call
void on_velocity(void* postfx, bool after) {
    const int pass = dual_pass::pass();
    const uintptr_t p = reinterpret_cast<uintptr_t>(postfx);
    if (pass == 1) {
        if (!after) {
            if (!g_ran_pass1 || !p) return;
            // eye 1's variable as eye 2's call left it: eye 1's own (put back), before eye 1's call uploads it
            uintptr_t eff = 0;
            int handle = 0;
            float v[16], e1[16];
            const bool read = rd(p + kTaaEffect, &eff) && eff && rd(p + kVarNdcToPrev, &handle) && effect_value(eff, handle, v) &&
                              raw_read(p + kNdcToPrev, e1, sizeof(e1));
            const bool local = read && std::memcmp(v, e1, sizeof(v)) == 0;
            bump(local ? g_e1_local_ok : g_e1_local_bad);
            const bool kept = read && std::memcmp(e1, g_e1_saved, sizeof(e1)) == 0;  // (r54)
            bump(kept ? g_e1_kept_ok : g_e1_kept_bad);
            float cbv[16];
            const bool cb_read = read && cb_value(eff, handle, cbv);
            const bool cb = cb_read && std::memcmp(cbv, e1, sizeof(cbv)) == 0;
            bump(!cb_read ? g_e1_cb_unread : cb ? g_e1_cb_ok : g_e1_cb_bad);
            if (g_ran_ctl) bump(local && kept && cb ? g_e1_ctl_ok : g_e1_ctl_bad);
            return;
        }
        const bool ran = g_ran_pass1;
        g_ran_pass1 = false;
        ++g_p1_count;
        {
            float n16[16];
            uint64_t nf = 0, nseq = 0;
            if (taa::eye2_ndc(n16, &nf, &nseq)) g_seq_p1_end = nseq;
        }
        if (ran && g_cap.load() == kRec1 && g_cap_p1 == g_p1_count) {  // (the negative control: eye 1's own output)
            if (render_thread() && prepass::stream_marker(&cb_e1_cap)) bump(g_e1_marks);
            else bump(g_e1_marker_failed);
        }
        return;
    }
    if (pass != 2) return;
    if (!after) {
        g_in2_marked = false;
        g_p2_cull_ok = cull_read(&g_p2_cull);  // (r53: the cull in force and cache[0] at pass 2's own call, for "cull p2")
        if (g_cap.load() != kRec1) return;
        int s = kRec1;
        if (g_cap_p1 != g_p1_count) {  // (not the pass 1 the capture started in)
            if (g_cap.compare_exchange_strong(s, kArmed)) {
                bump(g_p2_other_frame);
                bump(g_cap_incomplete);
            }
            return;
        }
        if (!render_thread() || !g_cap.compare_exchange_strong(s, kRec2)) return;
        g_cap_p1 = ~0ull;
        bump(g_p2_caps);
        g_cap_cut.store(prepass::walk_was_cut());
        uintptr_t eff = 0, f8 = 0;
        int handle = 0;
        float v[16] = {};
        const bool have = p && rd(p + kTaaEffect, &eff) && eff && rd(p + kVarNdcToPrev, &handle) && effect_value(eff, handle, v);
        std::memcpy(g_cap_ndc2, v, sizeof(v));
        if (p) rd(p + kDepthRt, &f8);
        g_cap_f8_2.store(f8);
        take_inherited(&g_cap_in2);  // (review item 4: pass 2's lights' leftovers, at the call's entry)
        std::atomic_thread_fence(std::memory_order_release);
        g_cap_cpu2.store(have);
        g_in2_marked = prepass::stream_marker(&cb_in2_cap);
        if (!g_in2_marked) {
            bump(g_p2_marker_failed);
            s = kRec2;
            g_cap.compare_exchange_strong(s, kArmed);
        }
        return;
    }
    if (!g_in2_marked) return;
    g_in2_marked = false;
    if (g_cap.load() == kRec2) {  // (review item 4) TexelSize as pass 2's call left it
        uintptr_t eff = 0;
        int htex = 0;
        float tx[4] = {};
        const bool have = p && rd(p + kTaaEffect, &eff) && eff && rd(p + kVarTexel, &htex) && effect_value(eff, htex, tx, 16);
        std::memcpy(g_cap_tx2, tx, sizeof(tx));
        std::atomic_thread_fence(std::memory_order_release);
        g_cap_tx2_ok.store(have);
    }
    if (g_cap.load() == kRec2 && !prepass::stream_marker(&cb_out2_cap)) {
        bump(g_p2_marker_failed);
        int s = kRec2;
        g_cap.compare_exchange_strong(s, kArmed);
    }
}

bool setup_compare() {
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

unsigned long long mn(const std::atomic<uint64_t>& a) {
    const uint64_t v = a.load(std::memory_order_relaxed);
    return v == ~0ull ? 0ull : static_cast<unsigned long long>(v);
}
double us_per_run() {
    return u(g_runs) ? static_cast<double>(u(g_ticks)) * g_qpc_us / static_cast<double>(u(g_runs)) : 0.0;
}
std::string kv() {
    char b[4000];
    std::snprintf(
        b, sizeof(b),
        "on=%d stood=%d cap=%d left=%d control=%d calls=%llu off_calls=%llu runs=%llu ctl_runs=%llu us_per_run=%.1f gate_closed=%llu not_dlss=%llu "
        "no_ndc=%llu no_postfx=%llu no_stream=%llu preps=%llu fault=%llu out_marker_failed=%llu p2_caps=%llu p2_marker_failed=%llu "
        "p2_other_frame=%llu e1_marks=%llu e1_marker_failed=%llu e1_local_ok=%llu e1_local_bad=%llu made=%llu make_failed=%llu inv_ok=%llu "
        "inv_unarmed=%llu seeded=%llu seed_failed=%llu outv_ok=%llu outv_nodraw=%llu sub_nodraw=%llu extra_draws=%llu subst=%llu "
        "subst_missed=%llu eye1_exposed=%llu unpaired=%llu unclosed=%llu ops_overflow=%llu ops_other_thread=%llu not_direct=%llu in2_ok=%llu "
        "in2_refused=%llu out2_ok=%llu out2_refused=%llu e1_ok=%llu e1_refused=%llu vel_resolves=%llu vel_changes=%llu caps_started=%llu "
        "caps_done=%llu caps_incomplete=%llu frames=%llu cut_frames=%llu in_equal=%llu cpu_equal=%llu cpu_unread=%llu in_v_diff=%llu "
        "in_d_diff=%llu in_s_diff=%llu d_hi_only=%llu out_exact=%llu out_exact_equal=%llu out_diff_equal=%llu cut_exact_equal=%llu "
        "cov_zero=%llu cov_min=%llu cov_max=%llu neg_measured=%llu neg_differ=%llu neg_min=%llu ctl_frames=%llu ctl_differ=%llu "
        "ctl_in_equal=%llu ctl_min=%llu out_texels_max=%llu out_worst=%llu in_v_texels_max=%llu in_d_texels_max=%llu in_s_texels_max=%llu "
        "box=%d,%d,%d,%d map_failed=%llu timeouts=%llu unarmed_exposed=%llu states_unread=%llu ndc_diff=%llu f8_diff=%llu tx_diff=%llu "
        "rs_diff=%llu rs_first=%d sc_diff=%llu rs_cache=%d p2w_zero=%llu p2w_min=%llu p2w_max=%llu cull_mode=%d rs0_sets=%llu rs0_same=%llu "
        "rs0_none=%llu rs0_failed=%llu rs0_set_frames=%llu rs0_inconsistent=%llu cull_diff=%llu cull_globals=%d rs_other_only=%llu "
        "e1_kept_ok=%llu e1_kept_bad=%llu e1_cb_ok=%llu e1_cb_bad=%llu e1_cb_unread=%llu e1_ctl_ok=%llu e1_ctl_bad=%llu",
        g_on.load() ? 1 : 0, g_stood.load() ? 1 : 0, g_cap.load(), g_cap_left.load(), g_control.load(), u(g_calls), u(g_off_calls), u(g_runs),
        u(g_ctl_runs), us_per_run(), u(g_gate_closed), u(g_not_dlss), u(g_no_ndc), u(g_no_postfx), u(g_no_stream), u(g_preps), u(g_fault),
        u(g_out_marker_failed), u(g_p2_caps), u(g_p2_marker_failed), u(g_p2_other_frame), u(g_e1_marks), u(g_e1_marker_failed), u(g_e1_local_ok),
        u(g_e1_local_bad), u(g_made), u(g_make_failed), u(g_inv_ok), u(g_inv_unarmed), u(g_seeded), u(g_seed_failed), u(g_outv_ok), u(g_outv_nodraw),
        u(g_sub_nodraw), u(g_extra_draws), u(g_subst), u(g_subst_missed), u(g_eye1_exposed), u(g_unpaired), u(g_unclosed), u(g_ops_overflow),
        u(g_ops_other_thread), u(g_not_direct), u(g_in2_ok), u(g_in2_refused), u(g_out2_ok), u(g_out2_refused), u(g_e1_ok), u(g_e1_refused),
        u(g_vel_resolves), u(g_vel_changes), u(g_cap_started), u(g_cap_done), u(g_cap_incomplete), u(g_c_frames), u(g_c_cut), u(g_c_in_eq),
        u(g_c_cpu_eq), u(g_c_cpu_unread), u(g_c_in_v_diff), u(g_c_in_d_diff), u(g_c_in_s_diff), u(g_c_d_hi), u(g_c_out_exact), u(g_c_out_exact_eq),
        u(g_c_out_diff_eq), u(g_c_cut_exact_eq), u(g_c_cov_zero), mn(g_c_cov_min), static_cast<unsigned long long>(g_c_cov_max.load()),
        u(g_c_neg_measured), u(g_c_neg_differ), mn(g_c_neg_min), u(g_c_ctl), u(g_c_ctl_differ), u(g_c_ctl_in_eq), mn(g_c_ctl_min),
        static_cast<unsigned long long>(g_c_out_texels_max.load()), static_cast<unsigned long long>(g_c_out_worst.load()),
        static_cast<unsigned long long>(g_c_in_v_texels_max.load()), static_cast<unsigned long long>(g_c_in_d_texels_max.load()),
        static_cast<unsigned long long>(g_c_in_s_texels_max.load()), g_c_box[0].load(), g_c_box[1].load(), g_c_box[2].load(), g_c_box[3].load(),
        u(g_c_map_failed), u(g_c_timeout), u(g_unarmed_exposed), u(g_c_states_unread), u(g_c_ndc_diff), u(g_c_f8_diff), u(g_c_tx_diff),
        u(g_c_rs_diff), g_c_rs_first.load(), u(g_c_sc_diff), g_rs_cache ? 1 : 0, u(g_c_p2w_zero), mn(g_c_p2w_min),
        static_cast<unsigned long long>(g_c_p2w_max.load()), g_rs0_mode.load(), u(g_rs0_sets), u(g_rs0_same), u(g_rs0_none), u(g_rs0_failed),
        u(g_c_rs0_set), u(g_rs0_inconsistent), u(g_c_cull_diff), g_cull ? 1 : 0, u(g_c_rs_other), u(g_e1_kept_ok), u(g_e1_kept_bad), u(g_e1_cb_ok),
        u(g_e1_cb_bad), u(g_e1_cb_unread), u(g_e1_ctl_ok), u(g_e1_ctl_bad));
    return b;
}
void reset() {
    for (Count* c : {&g_calls, &g_off_calls, &g_runs, &g_ctl_runs, &g_gate_closed, &g_not_dlss, &g_no_ndc, &g_no_postfx, &g_no_stream, &g_preps,
                     &g_p2_caps, &g_p2_marker_failed, &g_p2_other_frame, &g_e1_marks, &g_e1_marker_failed, &g_e1_local_ok, &g_e1_local_bad, &g_ticks,
                     &g_inv_ok, &g_inv_unarmed, &g_seeded, &g_seed_failed, &g_outv_ok, &g_outv_nodraw, &g_sub_nodraw, &g_extra_draws, &g_subst,
                     &g_subst_missed, &g_unpaired, &g_in2_ok, &g_in2_refused, &g_out2_ok, &g_out2_refused, &g_e1_ok, &g_e1_refused, &g_cap_started,
                     &g_cap_done, &g_cap_incomplete, &g_c_frames, &g_c_cut, &g_c_in_eq, &g_c_cpu_eq, &g_c_cpu_unread, &g_c_in_v_diff, &g_c_in_d_diff,
                     &g_c_in_s_diff, &g_c_d_hi, &g_c_out_exact, &g_c_out_exact_eq, &g_c_out_diff_eq, &g_c_cut_exact_eq, &g_c_cov_zero,
                     &g_c_neg_measured, &g_c_neg_differ, &g_c_ctl, &g_c_ctl_differ, &g_c_ctl_in_eq, &g_c_map_failed, &g_c_timeout,
                     &g_c_states_unread, &g_c_ndc_diff, &g_c_f8_diff, &g_c_tx_diff, &g_c_rs_diff, &g_c_sc_diff, &g_c_p2w_zero, &g_c_rs0_set, &g_rs0_sets,
                     &g_rs0_same, &g_rs0_none, &g_rs0_failed, &g_rs0_inconsistent, &g_c_cull_diff, &g_c_rs_other,
                     &g_e1_kept_ok, &g_e1_kept_bad, &g_e1_cb_ok, &g_e1_cb_bad, &g_e1_cb_unread, &g_e1_ctl_ok, &g_e1_ctl_bad})
        c->store(0);
    g_c_rs_first.store(-1);
    g_c_p2w_min.store(~0ull);
    g_c_p2w_max.store(0);
    for (auto* a : {&g_c_out_texels_max, &g_c_out_worst, &g_c_in_v_texels_max, &g_c_in_d_texels_max, &g_c_in_s_texels_max, &g_c_cov_max})
        a->store(0);
    for (auto* a : {&g_c_cov_min, &g_c_neg_min, &g_c_ctl_min}) a->store(~0ull);
    for (auto& x : g_c_box) x.store(-1);
    // (fault, out_marker_failed, make_failed, made, ops_*, not_direct, eye1_exposed, unclosed: kept, they mark a stand-down;
    // vel_resolves and vel_changes kept: the Velocity RT's resolutions over the session)
}

}  // namespace

void poll(ID3D12GraphicsCommandList* cl) {
    if (!cl || GetCurrentThreadId() != g_ops_tid.load(std::memory_order_relaxed) || g_nops == 0) return;
    if (cl->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        // (review item 1) the queue dropped, the window closed and nothing armed: a lone IN-V must not arm on a later list
        bump(g_not_direct);
        g_nops = 0;
        prepass::g_eye2_vel_pending_flag.store(false, std::memory_order_relaxed);
        g_window = g_armed = g_bound_v2 = false;
        g_in2_pending = false;
        g_cap_bad = true;
        update_flag();
        exposed("a step's list call on a non-DIRECT list");
        return;
    }
    prepass::g_eye2_vel_pending_flag.store(false, std::memory_order_relaxed);
    const int n = g_nops;
    g_nops = 0;
    for (int i = 0; i < n; ++i) {
        switch (g_ops[i]) {
            case kPrep: prep(); break;
            case kInV: inv_step(cl, false); break;
            case kInVCap: inv_step(cl, true); break;
            case kOutV: outv_step(cl, false); break;
            case kOutVCap: outv_step(cl, true); break;
            case kE1Cap: {  // (pass 2's render thread may have taken the capture on to kRec2 already)
                const int cs = g_cap.load();
                if ((cs == kRec1 || cs == kRec2) && read_vel(cl, kVE1, g_e1_refused, "SE1")) bump(g_e1_ok);
                break;
            }
            case kIn2Cap:
                g_in2_pending = true;  // (at the velocity pass's own bind: subst)
                update_flag();
                break;
            case kOut2Cap:
                if (g_in2_pending) {  // (no bind between: the pass did not bind)
                    g_in2_pending = false;
                    refuse(g_in2_refused, "IN2V: no bind before OUT2V");
                    g_cap_bad = true;
                    update_flag();
                }
                if (read_vel(cl, kVOut2, g_out2_refused, "OUT2V")) bump(g_out2_ok);
                else g_cap_bad = true;
                out2_done();
                break;
        }
    }
}

namespace {
// (the leaf-hook rule's check, eye2_light's) the target is a .pdata function starting there
bool has_pdata(uintptr_t f) {
    DWORD64 image = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(static_cast<DWORD64>(f), &image, nullptr);
    return fn && image && image + fn->BeginAddress == f;
}
}  // namespace

void install() {
    const int want = config::get_int("Stereo", "PrepassEye2Velocity", 0);
    if (want <= 0) return;
    if (want != 1) {
        log::warn("[e2vel] PrepassEye2Velocity=%d is not 1: off", want);
        return;
    }
    if (config::get_int("Stereo", "PrepassEye2Light", 0) != 1 || !eye2_light::usable()) {
        log::warn("[e2vel] PrepassEye2Velocity needs [Stereo] PrepassEye2Light=1 installed (its (L) step runs eye 2's velocity pass): off");
        return;
    }
    if (anchors::stand_down() || !anchors::exe_matches() || anchors::relocated()) {
        log::warn("[e2vel] PrepassEye2Velocity runs on the analysed RDR.exe build only: off");
        return;
    }
    if (!anchors::present(Id::VelocityPass) || !cut_velocity::velocity_hooked()) {
        log::warn("[e2vel] the velocity pass's hook (VelocityPass, cut_velocity's shared detour) is not installed: off");
        return;
    }
    if (!has_pdata(anchors::addr(Id::VelocityPass))) {
        log::warn("[e2vel] FUN_140873920 has no .pdata entry starting there: off");
        return;
    }
    for (Id id : {Id::PostFxSingleton, Id::EffectSetVar, Id::ShadowCollectorOwner, Id::RenderThreadId, Id::D3dDeviceSingleton})
        if (!anchors::present(id) || !anchors::addr(id)) {
            log::warn("[e2vel] the anchor %s is absent on this build: off", anchors::name(id));
            return;
        }
    {  // RenderStateSet 0x147110 [V]: the render-state cache's RVA (eye2_light's check; review item 4)
        const uintptr_t f = anchors::addr(Id::RenderStateSet);
        uint8_t code[28] = {};
        uint32_t disp = 0;
        int32_t lea = 0;
        const uint8_t lea_op[3] = {0x4c, 0x8d, 0x05}, mov_op[4] = {0x41, 0x89, 0x94, 0x80}, cmp_op[3] = {0x83, 0xf9, 0x2b};
        if (!raw_read(f, code, sizeof(code)) || std::memcmp(code + 7, lea_op, 3) || std::memcmp(code + 14, mov_op, 4) || std::memcmp(code + 22, cmp_op, 3)) {
            log::warn("[e2vel] RenderStateSet's code is not as analysed (the render-state cache not found): off");
            return;
        }
        std::memcpy(&lea, code + 10, 4);
        std::memcpy(&disp, code + 18, 4);
        if (f + 14 + static_cast<intptr_t>(lea) != anchors::base()) {
            log::warn("[e2vel] RenderStateSet's base is not the image's: off");
            return;
        }
        g_rs_cache = anchors::base() + disp;
        // (the delta review's must-fix) case 0's globals from the same code [V the disassembly]: the jump table's entry 0 is
        // +0x2f; +0x31 mov ecx,[r8+rax*4+disp32] (the mirror table, image-relative), +0x39 mov [rip+d],ecx (the mirror),
        // +0x70 mov [rip+d],r9d (the cull), +0x77 mov byte [rip+d],1 (the dirty byte). Not as analysed: "cull p2" refused
        uint8_t c0[0x7e] = {};
        int32_t jt = 0, e0 = 0, tab = 0, dm = 0, dc = 0, dd = 0;
        const uint8_t jt_op[4] = {0x41, 0x8b, 0x8c, 0x80}, mirror_op[2] = {0x89, 0x0d}, cull_op[3] = {0x44, 0x89, 0x0d}, dirty_op[2] = {0xc6, 0x05};
        if (raw_read(f, c0, sizeof(c0)) && !std::memcmp(c0 + 0x22, jt_op, 4) && !std::memcmp(c0 + 0x31, jt_op, 4) && !std::memcmp(c0 + 0x39, mirror_op, 2) &&
            !std::memcmp(c0 + 0x70, cull_op, 3) && !std::memcmp(c0 + 0x77, dirty_op, 2) && c0[0x7d] == 1) {
            std::memcpy(&jt, c0 + 0x26, 4);
            std::memcpy(&tab, c0 + 0x35, 4);
            std::memcpy(&dm, c0 + 0x3b, 4);
            std::memcpy(&dc, c0 + 0x73, 4);
            std::memcpy(&dd, c0 + 0x79, 4);
            if (rd(anchors::base() + static_cast<uint32_t>(jt), &e0) && anchors::base() + static_cast<uint32_t>(e0) == f + 0x2f) {
                g_cull_table = anchors::base() + static_cast<uint32_t>(tab);
                g_cull_mirror = f + 0x3f + static_cast<intptr_t>(dm);
                g_cull_dirty = f + 0x7e + static_cast<intptr_t>(dd);
                g_cull = f + 0x77 + static_cast<intptr_t>(dc);
            }
        }
        if (!g_cull) log::warn("[e2vel] RenderStateSet's case 0 is not as analysed: \"cull p2\" refused, the class compares no cull");
    }
    if (!prepass::views_wanted()) {
        log::warn("[e2vel] PrepassEye2Velocity needs [Stereo] PrepassViews (the TAA update's eye-2 NDCToPrevNDC in pass 1): off");
        return;
    }
    LARGE_INTEGER fr{};
    QueryPerformanceFrequency(&fr);
    g_qpc_us = fr.QuadPart ? 1e6 / static_cast<double>(fr.QuadPart) : 0.0;
    d3d::add_frame_end_listener(&frame_end);
    d3d::set_rtv_substitute2(&subst);
    cut_velocity::set_e2v(&on_velocity);
    eye2_light::set_light_tail(&tail);
    g_installed = true;
    g_on = true;
    log::info("[e2vel] [Stereo] PrepassEye2Velocity on (test only, DLSS): eye 2's velocity pass made in pass 1 in PrepassEye2Light's (L) step, "
              "into V2 (seeded from the pre-pass velocity array's slice 1) through a one-shot RTV substitution; \"prepass e2vel compare N\" "
              "compares V2 with pass 2's Velocity RT");
}

bool capturing() {  // ([Stereo] Pass2FrontSkip, item 7's S2) a compare's frame in flight (its pass 2 read at the velocity pass)
    const int s = g_cap.load();
    return s == kRec1 || s == kRec2;
}
// (GOAL-single-pass item 8f, test only) V2 on its steps' thread (the playback thread where ensure() makes it); fresh: this pass 1's OUT-V
// drew it and nothing spent it since, not stood down, no step queued; game: the Velocity RT it was made for
// (the review's nit) the plain state is read only on the steps' own thread: another caller gets tid, null, not fresh and no game
ID3D12Resource* v2_image(uint64_t* gen, unsigned long* tid, bool* fresh, ID3D12Resource** game) {
    const DWORD ot = g_ops_tid.load(std::memory_order_relaxed);
    const bool mine = ot != 0 && GetCurrentThreadId() == ot;
    if (gen) *gen = u(g_made);
    if (tid) *tid = ot;
    if (fresh) *fresh = mine && g_v2_fresh.load() && !g_stood.load() && g_nops == 0;
    if (game) *game = mine ? g_img.game_vel : nullptr;
    return mine ? g_img.v2 : nullptr;
}
void v2_spent() { g_v2_fresh = false; }

std::string command(const std::vector<std::string>& t) {
    if (!g_installed.load())
        return "e2vel: not installed (start with [Stereo] PrepassEye2Velocity=1 and PrepassEye2Light=1 and its switches, on the analysed RDR.exe)";
    if (t.size() >= 3) {
        const std::string& w = t[2];
        if (w == "kv") return kv();
        if (w == "on" || w == "off") {
            if (w == "on" && g_stood.load()) return "ERROR stood down (" + std::string(g_why) + "): restart the game";
            g_on = w == "on";
        } else if (w == "reset") {
            reset();
        } else if (w == "cull") {  // (r53, test only) eye 2's call with id 0 (the cull mode) as pass 2's last, or as inherited
            const std::string m = t.size() >= 4 ? t[3] : "";
            if (m == "p2" && !g_cull) return "ERROR cull p2: RenderStateSet's case 0 not as analysed (the cull's globals not found)";
            if (m == "p2") g_rs0_mode = 1;
            else if (m == "off") g_rs0_mode = 0;
            else return "ERROR prepass e2vel cull p2|off";
            log::info("[e2vel] cull %s", m.c_str());
        } else if (w == "control") {
            const std::string m = t.size() >= 4 ? t[3] : "";
            if (m == "off") g_control = 0;
            else if (m == "nudge") g_control = 1;
            else if (m == "eye1") g_control = 2;
            else return "ERROR prepass e2vel control off|nudge|eye1";
            log::info("[e2vel] control %s", m.c_str());
        } else if (w == "compare") {
            const int n = t.size() >= 4 ? std::atoi(t[3].c_str()) : 10;
            if (n <= 0 || n > 1000) return "ERROR compare N: 1..1000";
            if (!setup_compare()) return "ERROR the fence, its event or the worker not made";
            const int s = g_cap.load();
            if (s != kIdle && s != kArmed) return "ERROR a capture is in flight (cap=" + std::to_string(s) + ")";
            g_cap_left.store(n);
            g_cap.store(kArmed);
        } else {
            return "ERROR prepass e2vel [on|off|compare N|control off|nudge|eye1|cull p2|off|reset|kv]";
        }
    }
    char b[2400];
    std::snprintf(b, sizeof(b),
                  "e2vel %s%s: (L) calls %llu, eye-2 velocity runs %llu (%.1f us each; control %llu) (gate closed %llu, not DLSS %llu, no eye-2 "
                  "NDCToPrevNDC %llu, PostFx not read %llu, no stream %llu, preps %llu) | IN-V %llu (not armed %llu; seeded %llu, not %llu), "
                  "substituted %llu (first bind not the pass's %llu), OUT-V ok %llu (no draw %llu, substituted with no draw %llu, more draws %llu) | "
                  "eye 1 exposed %llu, eye 1's variable put back %llu (not %llu) | captures: started %llu, done %llu, incomplete %llu, left %d "
                  "(state %d) | compared %llu frames (cut %llu): inputs equal %llu (prior %llu, depth %llu, stencil %llu differ), CPU equal %llu; "
                  "V2 bit-exact %llu (with equal inputs %llu; differs with equal inputs %llu, most texels %llu, worst %llu); cut frames bit-exact "
                  "with equal inputs %llu; written %llu..%llu (none %llu); negative control differs %llu of %llu; control frames %llu, eye 2 "
                  "differs %llu | last refusal: %s",
                  g_on.load() ? "on" : g_stood.load() ? "stood down" : "off", g_control.load() == 1 ? " (control nudge)" : g_control.load() == 2 ? " (control eye1)" : "",
                  u(g_calls), u(g_runs), us_per_run(), u(g_ctl_runs), u(g_gate_closed), u(g_not_dlss), u(g_no_ndc), u(g_no_postfx), u(g_no_stream),
                  u(g_preps), u(g_inv_ok), u(g_inv_unarmed), u(g_seeded), u(g_seed_failed), u(g_subst), u(g_subst_missed), u(g_outv_ok),
                  u(g_outv_nodraw), u(g_sub_nodraw), u(g_extra_draws), u(g_eye1_exposed), u(g_e1_local_ok), u(g_e1_local_bad), u(g_cap_started),
                  u(g_cap_done), u(g_cap_incomplete), g_cap_left.load(), g_cap.load(), u(g_c_frames), u(g_c_cut), u(g_c_in_eq), u(g_c_in_v_diff),
                  u(g_c_in_d_diff), u(g_c_in_s_diff), u(g_c_cpu_eq), u(g_c_out_exact), u(g_c_out_exact_eq), u(g_c_out_diff_eq),
                  static_cast<unsigned long long>(g_c_out_texels_max.load()), static_cast<unsigned long long>(g_c_out_worst.load()), u(g_c_cut_exact_eq),
                  mn(g_c_cov_min), static_cast<unsigned long long>(g_c_cov_max.load()), u(g_c_cov_zero), u(g_c_neg_differ), u(g_c_neg_measured),
                  u(g_c_ctl), u(g_c_ctl_differ), g_why[0] ? g_why : "none");
    return b;
}

}  // namespace rdrvr::eye2_velocity
