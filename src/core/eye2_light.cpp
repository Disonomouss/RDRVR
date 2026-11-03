#include "core/eye2_light.h"

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
#include "core/eye2_collector.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/pass2_skip.h"
#include "core/prepass_arrays.h"
#include "core/state.h"

#pragma intrinsic(_ReturnAddress)

namespace rdrvr::prepass {
std::atomic<bool> g_eye2_light_pending_flag{false};
}

namespace rdrvr::eye2_light {
std::atomic<bool> g_lent_flag{false};  // (item 6c, eye2_light.h) SC2L and LB2 lent to pass 1's main walk
namespace {
using anchors::Id;

// FUN_1406e0450(lighting, cull view) (research\run21\p3-light.md 1.1) [V]: its one caller SceneRender (0x1405c3e48, return
// 0x5c3e4d), the cull view SceneRender's [RBP-0x70] (FUN_140191920 then BuildCullView FUN_1408604a0(cv, vp, vp + 0x40))
using Light_t = void (*)(void* lighting, void* cv);
// FUN_140193060(footprint list) [V]: its one caller 0x5cbfd0 (0x1405cc5df, return 0x5cc5e4)
using Foot_t = void (*)(void* list);
using CvCtor_t = void* (*)(void* cv);
using CvBuild_t = void (*)(void* cv, void* vp, void* cam);
using SetCurrent_t = uintptr_t (*)(uintptr_t vp, char push, uintptr_t unused, char keep);
using RsSet_t = void (*)(int id, uint32_t value);
using BindVd_t = void (*)(uintptr_t ctx, uintptr_t decl);
using Scissor_t = void (*)(uintptr_t ctx, const int32_t* rect);  // FUN_140ec7e90(ctx, {l, t, r, b}): ctx +0x148.., then ctx+0x128 vt+0x80
Light_t o_light = nullptr;
Foot_t o_foot = nullptr;
uintptr_t g_ret_l = 0, g_ret_f = 0;

// the cull view: 0x330 B [V p3-light.md 8.1: its vt[0] 0x140191c50 frees 0x330 B; FUN_140191920 writes up to +0x323,
// BuildCullView up to +0x320]; it owns nothing (that vt[0] only resets the vftable), keeps no pointer, and nothing keeps one to it
constexpr size_t kCvBytes = 0x340;
static_assert(kCvBytes >= 0x330, "the cull view is 0x330 bytes");
constexpr int kRsIds = 0x2c;                  // RenderStateSet's ids
constexpr uintptr_t kRvaBindVd = 0xec67a0;    // FUN_140ec67a0 (as eye2_collector: its entry bytes compared at install)
const uint8_t kBindVdEntry[40] = {0x48, 0x85, 0xd2, 0x74, 0x05, 0x8b, 0x42, 0x10, 0xeb, 0x02, 0x33, 0xc0, 0x39, 0x81, 0xa8, 0x01, 0x00, 0x00, 0x74, 0x25,
                                  0x89, 0x81, 0xa8, 0x01, 0x00, 0x00, 0x48, 0x89, 0x91, 0xa0, 0x01, 0x00, 0x00, 0xc6, 0x81, 0x24, 0x01, 0x00, 0x00, 0x01};
// FUN_140ec7e90's code [V]: movups xmm0,[rdx]; movups [rcx+0x148],xmm0; mov rcx,[rcx+0x128]; mov rax,[rcx]; jmp [rax+0x80]
const uint8_t kScissorEntry[27] = {0x0f, 0x10, 0x02, 0x0f, 0x11, 0x81, 0x48, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x89, 0x28,
                                   0x01, 0x00, 0x00, 0x48, 0x8b, 0x01, 0x48, 0xff, 0xa0, 0x80, 0x00, 0x00, 0x00};
// 0x1406df94a: mov rdi,[rip+disp32] = DAT_142ad1d98 in FUN_1406df150 (the volumes' append test): proves LightVolumes
constexpr uintptr_t kRvaVolRef = 0x6df94a;
// the light pools [V FUN_1406df150]: +8 an array of light pointers; DAT_142c8ac98's count at +0x1008, DAT_142c8aca0's at +0x808
constexpr int kPoolA = 0x200, kPoolB = 0x100;
constexpr uintptr_t kRectOff = 0x60, kRectLen = 0x21;  // a light's +0x60..+0x7c (the rect) and +0x80 (a byte), FUN_1406df150
// lighting fields [V]: +0x9c80 lighting on, +0x878 the Light Buffer RT, +0x6c20 the interior location, +0x9c30 the count
constexpr uintptr_t kLOn = 0x9c80, kLRt = 0x878, kLLoc = 0x6c20, kLCount = 0x9c30;

std::atomic<bool> g_installed{false}, g_on{false};
using Count = std::atomic<uint64_t>;
inline void bump(Count& c) { c.fetch_add(1, std::memory_order_relaxed); }
inline unsigned long long u(const Count& c) { return static_cast<unsigned long long>(c.load(std::memory_order_relaxed)); }

// the render thread's
Count g_l_calls, g_l_runs, g_f_calls, g_f_runs, g_no_e2sc, g_no_vp2, g_not_scene, g_not_rt, g_lighting_off, g_no_stream, g_prepass_off,
    g_no_follow, g_not_ready, g_preps, g_fp_unrun, g_fault, g_out_marker_failed, g_p2_calls, g_p2_caps, g_p2_marker_failed, g_p2_other_frame,
    g_states_restored, g_vd_rebinds, g_scissor_restored, g_vol_restored, g_rects_restored, g_ref_restored, g_l_ticks, g_f_ticks, g_counts_differ,
    g_interior, g_no_snap, g_vol_moved, g_clear_owed_skips, g_clear_marker_failed;
// (review S6) (F)'s own gate counts: no_e2sc and not_ready are (L)'s only; every other gate (F) refused, in f_gated
Count g_f_no_e2sc, g_f_not_ready, g_f_gated;
// (review S3) a clear marker owed: a pass 1 whose (L) did not run eye 2's could not append its clear (SC2L and D2 then hold a
// copy no later IN may use): the next pass 1 runs no eye-2 step, it appends the clear first. Set too at "on"
std::atomic<bool> g_clear_owed{false};
uintptr_t g_rs_cache = 0, g_bind_vd = 0, g_vol_global = 0;
uint64_t g_l_last_e2 = 0;          // eye2_collector::runs() at the last call of (L) in either pass: moved = the collector ran since
bool g_f_called = false, g_f_ran = false;  // this pass 1's (F): the game drew footprints / eye 2's ran (reset at each (L))
bool g_l_fp = false;  // (item 7's S2) the last pass 1's (L): the game drew footprints in that pass 1 (eye 2's then ran too, if (L)'s did)
uint64_t g_pass1_seq = 0, g_cap_seq = ~0ull;
int g_last_n2 = -1;  // the render thread: eye 2's light count after its call in this pass 1
double g_qpc_us = 0.0;
// the playback thread's
Count g_inf_ok, g_outf_ok, g_inl_ok, g_outl_ok, g_in2_ok, g_out2_ok, g_from_ok, g_from_refused, g_lb_unknown, g_made, g_make_failed,
    g_ops_overflow, g_ops_other_thread, g_eye1_exposed, g_not_direct, g_in2_refused, g_out2_refused, g_lb_saved_n, g_lb_unsaved, g_lb_put_back,
    g_lb_not_restored, g_clears, g_lb_resolves, g_lb_changes, g_save2_ok, g_save2_failed, g_nosaves, g_lb_touched;
Count g_save2_marker_failed;  // (run 22, the render thread) pass 2's save marker not appended
// (item 7's S2, research\run25\item7.md 2 and 3; [Stereo] Pass2FrontSkip, pass2_skip.cpp; test only) P2IN, the playback thread's:
// done, refused (each one eye 2 exposed: pass2_skip stands down), OUT-L's that set have2, P2INs with a copy left out (the
// control), OUT-Ls that left LB2 stale (the control); the controls themselves (pass2_skip's command sets them)
Count g_p2in_ok, g_p2in_refused, g_have2_n, g_p2in_ctl_n, g_stale_n, g_p2in_marker_failed;
std::atomic<unsigned> g_p2in_nocopy{0};  // p2skip::kNoCopy* bits: the copies P2IN leaves out ("control nocopy lb|sc|d|all")
std::atomic<bool> g_p2in_stale{false};   // ("control stale") OUT-L does not copy the Light Buffer into LB2 (LB2 an older frame's)
std::atomic<bool> g_img_ready{false};  // the images made for the current targets (the render thread gates on it)
std::atomic<bool> g_stood{false};
char g_why[200] = "";

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
bool raw_read(uintptr_t a, void* out, size_t n) {
    if (!a) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
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

void stand_down(const char* why) {
    g_stood = true;
    std::snprintf(g_why, sizeof(g_why), "%s", why);
    if (g_on.exchange(false)) log::error("[e2lb] PrepassEye2Light stood down: %s", why);
}
void exposed(const char* why) {  // eye 2's call is in the stream and eye 1's collector or depth will not be (were not) put back
    bump(g_eye1_exposed);
    stand_down(why);
}
void refuse(Count& c, const char* why) {
    bump(c);
    std::snprintf(g_why, sizeof(g_why), "%s", why);
    log::limited("e2lb.refused", 10, "[e2lb] a step refused: %s", why);
}

// ---- the capture: one in flight (eye2_collector's states)
enum Cap : int { kIdle, kArmed, kRec1, kRec2, kReady, kWait };
std::atomic<int> g_cap{kIdle};
std::atomic<int> g_cap_left{0};
std::atomic<uint64_t> g_cap_since{0}, g_ready_frame{0};
// the captured frame's classes (the render thread writes them while the capture is its own: kRec1 / kRec2)
std::atomic<bool> g_cap_cut{false}, g_cap_fp{false}, g_cap_fp2{false}, g_cap_interior{false};
std::atomic<int> g_cap_n2{-1}, g_cap_n1{-1}, g_cap_np2{-1};
bool g_p2_fp = false;  // the render thread: (F) called in this pass 2 (reset at pass 2's (L))
Count g_cap_started, g_cap_incomplete, g_cap_done;
// kSPrior (review S1, p3-light.md 3.6): the Light Buffer at IN-L (eye 2's prior content, save_lb's), for the compare's classing
// (run 25, item 7's S1, research\run25\item7.md; [Stereo] PrepassEye2LightDepth=1, test only) kSDL1/kSSL1: eye 2's depth after
// its lights (D2L, at OUT-L), kSDL2/kSSL2: pass 2's depth after its own lights (at OUT2), both planes
enum Slot : int { kSIn1, kSD1, kSS1, kSOut1, kSPrior, kSIn2, kSD2, kSS2, kSOut2, kSDL1, kSSL1, kSDL2, kSSL2, kSlots };
constexpr unsigned kDlSlots = (1u << kSDL1) | (1u << kSSL1) | (1u << kSDL2) | (1u << kSSL2);
std::atomic<bool> g_d2l_on{false};   // (install: [Stereo] PrepassEye2LightDepth=1) D2L made and copied at OUT-L, its compare slots taken
std::atomic<bool> g_d2l_ctl{false};  // ("prepass e2lb d2lctl on|off", the compare's control) kSDL1/kSSL1 from D2 (before the lights)
unsigned all_slots() { return ((1u << kSlots) - 1) & ~(g_d2l_on.load(std::memory_order_relaxed) ? 0u : kDlSlots); }
unsigned g_cap_bits = 0;
bool g_cap_bad = false;

// ---- the images (the playback thread)
struct Img {
    ID3D12Resource *game_sc = nullptr, *game_d = nullptr, *game_lb = nullptr;  // the keys (no reference held)
    D3D12_RESOURCE_DESC dsc{}, dd{}, dlb{};
    unsigned planes = 1;
    ID3D12Resource* sc2 = nullptr;      // eye 2's collector: after its directional term (C), then final (F); at rest COPY_DEST
    ID3D12Resource* d2 = nullptr;       // eye 2's depth after its collector step (C), both planes; at rest COPY_DEST
    ID3D12Resource* d2l = nullptr;      // (item 7's S1, PrepassEye2LightDepth) eye 2's depth after its lights (L), both planes; COPY_DEST
    ID3D12Resource* save_sc = nullptr;  // eye 1's collector and depth during (F) and (L); at rest COPY_DEST
    ID3D12Resource* save_d = nullptr;
    ID3D12Resource* lb2 = nullptr;      // eye 2's Light Buffer (L); at rest COPY_DEST
    ID3D12Resource* save_lb = nullptr;  // (review S1) the game's Light Buffer from IN-L to OUT-L (eye 1's prior content); at rest COPY_DEST
    ID3D12Resource* rb = nullptr;       // readback, kSlots footprints
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[kSlots]{};
    UINT rows[kSlots]{};
    UINT64 rowbytes[kSlots]{};
    UINT64 rb_size = 0;
};
Img g_img;
std::atomic<uintptr_t> g_pub_sc2{0}, g_pub_lb2{0};  // (item 6b, kv only) SC2L's and LB2's resources as values (0 none)
// the game's "Light Buffer" (the playback thread): resolved from lighting +0x878 (find_lb), watched, and held with a
// reference of the mod's own (review S2: AddRef when adopted, released through retire() when it changes), so it never dangles
ID3D12Resource* g_lb = nullptr;
struct Retired {
    ID3D12Resource* r;
    uint64_t frame;
};
std::mutex g_retire_mutex;
std::vector<Retired> g_retired;
// (run 22) eye 1's prior content: the game's Light Buffer as pass 2's call left it, copied into save_lb at the save marker
// after that call (nothing writes it again before the next pass 1's call: 0x5c3e48 is 0x6e0450's only caller). IN-L could
// not save it itself: the watch's state is per list and IN-L's list has no transition of the Light Buffer yet (run 22's
// L1/L2: lb_unknown = l_runs, lb_saved 0). Taken at IN-L, dropped at a clear marker and a no-save marker (the render
// thread sends one before the next IN-L when a game call had no marker after it: g_rt_dirty)
bool g_save_ok = false;
const ID3D12Resource* g_save_of = nullptr;  // (the g_lb it was copied from: compared only)
uint64_t g_save_writes = 0;  // (review S1) d3d::watched_writes(g_lb) at the save: moved by IN-L, a write-state transition between
std::atomic<bool> g_rt_dirty{true};
// (review S2) a light call off the render thread (sticky: its work has no order against the stream's markers, so no save is
// taken again in this session; lb_unsaved counts each IN-L then)
std::atomic<bool> g_offthread{false};
// (item 7's S2, item7.md 3; the playback thread) have2: P2IN's gate. Set at an OUT-L that wrote D2L and copied the Light Buffer
// (into LB2, or left LB2 stale under "control stale") after its IN-L found this frame's SC2L and D2 (SC2L then eye 2's final
// collector), not stood down; cleared at P2IN, at a clear marker, at the next IN-L and when the images are made again
bool g_have2 = false;
void retire(ID3D12Resource* r) {
    if (!r) return;
    std::lock_guard lock(g_retire_mutex);
    g_retired.push_back({r, state::presents.load()});
}
void retire_all() {
    retire(g_img.sc2);
    retire(g_img.d2);
    retire(g_img.d2l);
    retire(g_img.save_sc);
    retire(g_img.save_d);
    retire(g_img.lb2);
    retire(g_img.save_lb);
    retire(g_img.rb);
    g_img = Img{};
    g_save_ok = false;
    g_have2 = false;  // (item 7's S2: a remake is another generation)
    g_pub_sc2.store(0, std::memory_order_relaxed);
    g_pub_lb2.store(0, std::memory_order_relaxed);
}
// the playback thread: SC2L and D2 hold this frame's eye 2 (C), set by from_collector at OUT1; cleared at OUT-L (consumed),
// at a clear marker (review S3: a pass 1 whose (L) ran no eye-2 call appends one after eye 1's call, so the next frame's
// IN-F / IN-L never take this frame's copy), at a refused from_collector and when the images are made again
bool g_have = false;
enum Owed : int { kNone, kOwedF, kOwedL };
int g_owed = kNone;   // IN-F / IN-L overwrote the game's collector and depth: its OUT must put them back
bool g_lb_saved = false;  // (review S1) IN-L saved the game's Light Buffer into save_lb: OUT-L puts it back
// (GOAL-single-pass item 6b, PrepassEye2Images' freshness; the playback thread) the last IN-L / OUT-L pair: IN-L clears it, an
// OUT-L that copied the Light Buffer into LB2 sets it (SC2L is then eye 2's final collector: IN-L needs this frame's OUT1 copy,
// and (F)'s OUT-F before it when the game drew footprints), a clear marker clears it. Read by the check step only
bool g_pb_l_ok = false;
// (GOAL-single-pass item 6c) the lend of SC2L and LB2 to pass 1's main walk (eye2_light.h): the images' thread's, except the
// counters. g_lent_sc2 / g_lent_lb2: the resources the lend transitioned (given back as recorded, even if made again since)
ID3D12GraphicsCommandList* g_lent_cl = nullptr;
DWORD g_lent_tid = 0;  // (the review's SF2) the thread that recorded the lend: push() may move g_ops_tid, the lend stays on its own
ID3D12Resource *g_lent_sc2 = nullptr, *g_lent_lb2 = nullptr;
bool g_give_owed = false;
Count g_lends, g_given, g_given_close, g_owed_n, g_given_poll, g_given_list, g_given_step, g_lend_cross, g_lend_cross_step, g_lend_reset, g_lend_refused;
std::atomic<LightsDone_t> g_lights_done{nullptr};
std::atomic<FreshSink_t> g_fresh_sink{nullptr};
Count g_checks, g_checks_fresh, g_checks_now, g_checks_dropped;

// the worker's job
struct Job {
    ID3D12Resource* rb = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[kSlots]{};
    UINT rows[kSlots]{};
    UINT64 rowbytes[kSlots]{};
    UINT64 size = 0;
    bool cut = false, fp_drawn = false, fp2 = false, interior = false;
    bool d2l_ctl = false;  // (item 7's S1) kSDL1/kSSL1 were D2's (the control)
    int n2 = -1, n1 = -1, np2 = -1;
} g_job;
bool g_cap_d2l_ctl = false;  // (the playback thread) this capture's OUT-L took D2 for kSDL1/kSSL1
ID3D12Fence* g_fence = nullptr;
std::atomic<UINT64> g_fence_value{0};
HANDLE g_fence_event = nullptr;
std::mutex g_setup_mutex;

// the compare's results (the worker)
Count g_c_frames, g_c_cut, g_c_fp, g_c_interior, g_c_in_eq, g_c_cut_in_eq, g_c_in_sc_diff, g_c_in_d_diff, g_c_in_s_diff, g_c_d_hi, g_c_out_exact,
    g_c_out_exact_in_eq, g_c_out_diff_in_eq, g_c_map_failed, g_c_timeout, g_c_n_equal, g_c_n_differ, g_c_n_eyes_differ, g_c_fp_in_eq_exact,
    g_c_nofp_in_eq_exact, g_c_prior_only, g_c_prior_only_in_eq, g_c_unexplained_in_eq, g_c_written_zero, g_c_cut_out_exact, g_c_cut_prior_only,
    g_c_dl_eq, g_c_dl_diff, g_c_dl_eq_in_eq, g_c_dl_fp_eq, g_c_dl_fp_diff, g_c_dl_ctl_eq, g_c_dl_ctl_diff, g_d2l_copies;  // (item 7's S1)
std::atomic<uint64_t> g_c_dl_d_max{0}, g_c_dl_s_max{0};
std::atomic<uint64_t> g_c_out_texels_max{0}, g_c_out_worst{0}, g_c_in_sc_texels_max{0}, g_c_in_d_texels_max{0}, g_c_in_s_texels_max{0},
    g_c_prior_texels_max{0}, g_c_cut_out_texels_max{0};
std::atomic<int> g_c_box[4]{{-1}, {-1}, {-1}, {-1}};  // the last differing frame's box (x0, y0, x1, y1)
void store_max(std::atomic<uint64_t>& a, uint64_t v) {
    uint64_t c = a.load(std::memory_order_relaxed);
    while (v > c && !a.compare_exchange_weak(c, v, std::memory_order_relaxed)) {
    }
}

// ---- the op queue (the markers' callbacks, then the next list call's poll)
// (item 6b) kCheck..kCheck + kCheckTags - 1: the check, its tag in the op (kCheck last)
// (item 7's S2) kP2In: P2IN, pass 2's lights' stretch ended ([Stereo] Pass2FrontSkip)
enum Op : uint8_t { kInF, kOutF, kInL, kInLCap, kOutL, kOutLCap, kIn2Cap, kOut2Cap, kPrep, kClear, kSave2, kNoSave, kP2In, kCheck };
uint8_t g_ops[16];
int g_nops = 0;
DWORD g_ops_tid = 0;
void push(Op o) {
    const DWORD tid = GetCurrentThreadId();
    const bool eye1 = o != kIn2Cap && o != kOut2Cap && o != kPrep && o != kClear && o != kSave2 && o != kNoSave && o != kP2In;
    // (item 7's S2) a dropped P2IN: pass 2's front half was skipped and eye 2 keeps pass 1's eye-1 images this frame
    if (o == kP2In && ((g_nops > 0 && tid != g_ops_tid) || g_nops >= static_cast<int>(sizeof(g_ops))))
        p2skip::exposed2("P2IN's marker played on another thread or into a full step queue (dropped)");
    if (g_nops > 0 && tid != g_ops_tid) {
        bump(g_ops_other_thread);
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
    prepass::g_eye2_light_pending_flag.store(true, std::memory_order_release);
}
void cb_inf(void*) { push(kInF); }
void cb_outf(void*) { push(kOutF); }
void cb_inl(void*) { push(kInL); }
void cb_inl_cap(void*) { push(kInLCap); }
void cb_outl(void*) { push(kOutL); }
void cb_outl_cap(void*) { push(kOutLCap); }
void cb_in2_cap(void*) { push(kIn2Cap); }
void cb_out2_cap(void*) { push(kOut2Cap); }
void cb_prep(void*) { push(kPrep); }
void cb_clear(void*) { push(kClear); }
void cb_save2(void*) { push(kSave2); }
void cb_nosave(void*) { push(kNoSave); }
void cb_p2in(void*) { push(kP2In); }

// ---- the targets (prepass_arrays' kinds 0 the depth, 1 the collector; their states from the walk cut's follow)
constexpr int kKDepth = 0, kKCollector = 1;
struct Targets {
    ID3D12Resource *sc = nullptr, *d = nullptr;
    unsigned planes = 1;
    D3D12_RESOURCE_STATES ssc{}, sd[2]{};
};
bool bad_state(D3D12_RESOURCE_STATES s) { return s == D3D12_RESOURCE_STATE_COPY_DEST || s == D3D12_RESOURCE_STATE_COPY_SOURCE; }
// (the review of d6ac220f) cl: the list the targets will be moved on; their followed states must have been set on it (a stale plane
// refused through the callers' own paths, counted per module); null (prep: no barrier) skips that check
const char* gather(Targets* t, ID3D12GraphicsCommandList* cl) {
    if (prepass::active()) return "a pre-pass region open";
    t->sc = prepass::kind_game(kKCollector);
    t->d = prepass::kind_game(kKDepth);
    t->planes = prepass::kind_planes(kKDepth);
    if (!t->sc || !t->d) return "the collector or the depth not known";
    if (t->planes < 1 || t->planes > 2 || prepass::kind_planes(kKCollector) != 1) return "an unexpected plane count";
    if (!prepass::kind_followed(kKCollector, 0, &t->ssc)) return "the collector's state not followed";
    if (cl && !prepass::kind_followed_fresh(kKCollector, 0, cl, &t->ssc)) {
        prepass::follow_stale_note(prepass::kFollowE2lb);
        return "the collector's followed state was set on another list";
    }
    for (unsigned p = 0; p < t->planes; ++p) {
        if (!prepass::kind_followed(kKDepth, p, &t->sd[p])) return "the depth's state not followed";
        if (cl && !prepass::kind_followed_fresh(kKDepth, p, cl, &t->sd[p])) {
            prepass::follow_stale_note(prepass::kFollowE2lb);
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
constexpr UINT kAll = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
constexpr D3D12_RESOURCE_STATES kSrc = D3D12_RESOURCE_STATE_COPY_SOURCE, kDst = D3D12_RESOURCE_STATE_COPY_DEST;
struct Batch {
    D3D12_RESOURCE_BARRIER b[16];
    UINT n = 0;
    void add(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES s, UINT sub = kAll) {
        if (a != s && n < 16) b[n++] = tr(r, a, s, sub);
    }
    void flush(ID3D12GraphicsCommandList* cl) {
        if (n) d3d::raw_barrier(cl, n, b);
        n = 0;
    }
};
// the game's depth, each plane from (to) its own followed state
void depth_to(Batch& b, const Targets& t, D3D12_RESOURCE_STATES s) {
    for (unsigned p = 0; p < t.planes; ++p) b.add(t.d, t.sd[p], s, t.planes > 1 ? p : kAll);
}
void depth_back(Batch& b, const Targets& t, D3D12_RESOURCE_STATES s) {
    for (unsigned p = 0; p < t.planes; ++p) b.add(t.d, s, t.sd[p], t.planes > 1 ? p : kAll);
}
void copy_sub(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, UINT dsub, ID3D12Resource* src, UINT ssub) {
    D3D12_TEXTURE_COPY_LOCATION d{dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    d.SubresourceIndex = dsub;
    D3D12_TEXTURE_COPY_LOCATION s{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    s.SubresourceIndex = ssub;
    d3d::raw_copy_region(cl, &d, &s);
}
void copy_planes(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, ID3D12Resource* src, unsigned planes) {
    for (UINT p = 0; p < planes; ++p) copy_sub(cl, dst, p, src, p);
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

// ---- (GOAL-single-pass item 6c) the lend's barriers (the images' thread): both lent images back to COPY_DEST on the lent list
constexpr D3D12_RESOURCE_STATES kPsr = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
void give_back_now(ID3D12GraphicsCommandList* cl) {
    Batch x;
    x.add(g_lent_sc2, kPsr, kDst);
    x.add(g_lent_lb2, kPsr, kDst);
    x.flush(cl);
    g_lent_cl = nullptr;
    g_lent_tid = 0;
    g_lent_sc2 = g_lent_lb2 = nullptr;
    g_give_owed = false;
    g_lent_flag.store(false, std::memory_order_relaxed);
    bump(g_given);
}
// the lent list, on the lend's thread
bool is_lent_list(const ID3D12GraphicsCommandList* cl) { return cl && cl == g_lent_cl && GetCurrentThreadId() == g_lent_tid; }
// a step that copies into or out of SC2L / LB2 (IN-F/L, OUT-F/L, from_collector; the review's SF1: only those): true when it
// may on cl (not lent, or given back on cl now: the lent list on the lend's thread)
bool step_may_touch(ID3D12GraphicsCommandList* cl) {
    if (!g_lent_flag.load(std::memory_order_relaxed)) return true;
    if (is_lent_list(cl)) {
        bump(g_given_step);
        give_back_now(cl);
        return true;
    }
    bump(g_lend_cross_step);
    return false;
}

bool same_desc(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b) {
    return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format && a.DepthOrArraySize == b.DepthOrArraySize &&
           a.MipLevels == b.MipLevels && a.Flags == b.Flags && a.SampleDesc.Count == b.SampleDesc.Count;
}
// (review S2) the game's Light Buffer, resolved at each call (the playback thread: prep and from_collector, once a frame at
// least) from the lighting's own RT, lighting +0x878, as cut_velocity's find_vel resolves the Velocity RT: the registered
// resource named "Light Buffer" that the RT object points at in its first 0x100 bytes, directly or one pointer deep
// (resource_in_object). The field it was found in is kept: while lighting +0x878 and that field hold the same values the
// resource is the same and the name table is not searched again. No fallback by name alone: the name table keeps the
// names of released resources (a recreated Light Buffer's old pointer stays "Light Buffer"), so a name-only match could be
// a released one. A newly found resource is AddRef'd before it is used (the mod's reference: it cannot be released under the
// mod's copies), the old one's reference released through retire() (after 8 frames), and the barrier watch moved to it
uintptr_t g_lb_rt = 0, g_lb_o1 = 0;
intptr_t g_lb_o2 = -1;
uint64_t g_lb_field = 0;
bool lb_field(uintptr_t rt, uint64_t* v) {
    if (g_lb_o2 < 0) return rd(rt + g_lb_o1, v);
    uintptr_t p = 0;
    return rd(rt + g_lb_o1, &p) && p && rd(p + static_cast<uintptr_t>(g_lb_o2), v);
}
bool add_ref(ID3D12Resource* r) {
    __try {
        r->AddRef();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
const char* find_lb() {
    uintptr_t L = 0, rt = 0;
    if (!rd(anchors::addr(Id::LightingManager), &L) || !L || !rd(L + kLRt, &rt) || !rt) return "the lighting or its Light Buffer RT (lighting +0x878) not set";
    uint64_t v = 0;
    if (g_lb && rt == g_lb_rt && lb_field(rt, &v) && v == g_lb_field) return nullptr;  // (the same RT, the same field value: the same resource)
    bump(g_lb_resolves);
    char where[64] = "";
    ID3D12Resource* r =
        static_cast<ID3D12Resource*>(const_cast<void*>(d3d::resource_in_object(reinterpret_cast<const void*>(rt), 0x100, "Light Buffer", where, sizeof(where))));
    if (!r) return "the Light Buffer not found behind lighting +0x878 (its RT object's first 0x100 bytes, one pointer deep)";
    unsigned long long a = 0, b = 0;
    const int n = sscanf_s(where, "+%llx->+%llx", &a, &b);
    if (n < 1) return "the Light Buffer's field not parsed";
    if (r != g_lb) {
        if (!add_ref(r)) return "the Light Buffer found could not be referenced";
        if (g_lb) {
            d3d::watch_remove(g_lb);
            retire(g_lb);  // (the mod's reference, released after 8 frames)
        }
        g_lb = r;
        d3d::watch_add(g_lb);
        bump(g_lb_changes);
        log::info("[e2lb] the Light Buffer: lighting +0x878 %s (resolution %llu)", where, u(g_lb_resolves));
    }
    g_lb_rt = rt;
    g_lb_o1 = static_cast<uintptr_t>(a);
    g_lb_o2 = n == 2 ? static_cast<intptr_t>(b) : -1;
    if (!lb_field(rt, &g_lb_field)) g_lb_field = 0;  // (then the next call resolves again)
    return nullptr;
}
// the Light Buffer's state from the barrier watch: the last transition the game recorded for it in this list on this thread
bool lb_state(ID3D12GraphicsCommandList* cl, D3D12_RESOURCE_STATES* s) {
    ID3D12GraphicsCommandList* wcl = nullptr;
    uint32_t st = 0;
    if (!g_lb || g_lb != g_img.game_lb || !d3d::watched_state(g_lb, &wcl, &st) || wcl != cl || bad_state(static_cast<D3D12_RESOURCE_STATES>(st)))
        return false;
    *s = static_cast<D3D12_RESOURCE_STATES>(st);
    return true;
}
// the images for these targets (made again, the old ones retired, when the game's change)
const char* ensure(const Targets& t) {
    if (const char* why = find_lb()) return why;
    const D3D12_RESOURCE_DESC dsc = t.sc->GetDesc(), dd = t.d->GetDesc(), dlb = g_lb->GetDesc();
    if (dsc.MipLevels != 1 || dd.MipLevels != 1 || dlb.MipLevels != 1 || dsc.DepthOrArraySize != 1 || dd.DepthOrArraySize != 1 ||
        dlb.DepthOrArraySize != 1)
        return "a target with mips or slices";
    if (g_img.sc2 && g_img.game_sc == t.sc && g_img.game_d == t.d && g_img.game_lb == g_lb && g_img.planes == t.planes && same_desc(g_img.dsc, dsc) &&
        same_desc(g_img.dd, dd) && same_desc(g_img.dlb, dlb))
        return nullptr;
    g_img_ready = false;
    g_have = false;
    if (g_cap.load() != kIdle && g_cap.load() != kArmed) g_cap_bad = true;
    retire_all();
    ID3D12Device* dev = state::device.load();
    if (!dev) return "no device";
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC c = dsc, z = dd, l = dlb;
    c.Alignment = z.Alignment = l.Alignment = 0;
    auto make = [&](const D3D12_RESOURCE_DESC& d, ID3D12Resource** out) {
        return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, kDst, nullptr, IID_PPV_ARGS(out)));
    };
    bool ok = make(c, &g_img.sc2) && make(z, &g_img.d2) && make(c, &g_img.save_sc) && make(z, &g_img.save_d) && make(l, &g_img.lb2) &&
              make(l, &g_img.save_lb) && (!g_d2l_on.load() || make(z, &g_img.d2l));
    UINT64 off = 0;
    for (int s = 0; ok && s < kSlots; ++s) {
        const bool depth = s == kSD1 || s == kSD2 || s == kSS1 || s == kSS2 || s == kSDL1 || s == kSSL1 || s == kSDL2 || s == kSSL2;
        const bool stencil = s == kSS1 || s == kSS2 || s == kSSL1 || s == kSSL2;
        const bool lb = s == kSOut1 || s == kSOut2 || s == kSPrior;
        UINT64 total = 0;
        const bool dl_slot = s == kSDL1 || s == kSSL1 || s == kSDL2 || s == kSSL2;
        if ((stencil && t.planes < 2) || (dl_slot && !g_d2l_on.load())) {  // (no stencil plane, or D2L off: an empty slot)
            g_img.fp[s] = {};
            g_img.rows[s] = 0;
            g_img.rowbytes[s] = 0;
            continue;
        }
        dev->GetCopyableFootprints(depth ? &dd : lb ? &dlb : &dsc, stencil ? 1 : 0, 1, off, &g_img.fp[s], &g_img.rows[s], &g_img.rowbytes[s], &total);
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
        ok = SUCCEEDED(dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &rd, kDst, nullptr, IID_PPV_ARGS(&g_img.rb)));
        g_img.rb_size = off;
    }
    if (!ok) {
        bump(g_make_failed);
        retire_all();
        return "the images or the readback not made";
    }
    g_img.sc2->SetName(L"RDRVR eye 2 Shadow Collector (PrepassEye2Light)");
    g_img.d2->SetName(L"RDRVR eye 2 depth (PrepassEye2Light)");
    if (g_img.d2l) g_img.d2l->SetName(L"RDRVR eye 2 depth after its lights (PrepassEye2LightDepth)");
    g_img.lb2->SetName(L"RDRVR eye 2 Light Buffer (PrepassEye2Light)");
    g_img.save_lb->SetName(L"RDRVR eye 1 Light Buffer save (PrepassEye2Light)");
    g_img.game_sc = t.sc;
    g_img.game_d = t.d;
    g_img.game_lb = g_lb;
    g_img.dsc = dsc;
    g_img.dd = dd;
    g_img.dlb = dlb;
    g_img.planes = t.planes;
    g_img_ready = true;
    g_pub_sc2.store(reinterpret_cast<uintptr_t>(g_img.sc2), std::memory_order_relaxed);
    g_pub_lb2.store(reinterpret_cast<uintptr_t>(g_img.lb2), std::memory_order_relaxed);
    bump(g_made);
    log::info("[e2lb] images made: the collector %llux%u (format %d), the depth %llux%u (format %d, %u planes), the Light Buffer %llux%u (format %d)",
              static_cast<unsigned long long>(dsc.Width), dsc.Height, static_cast<int>(dsc.Format), static_cast<unsigned long long>(dd.Width), dd.Height,
              static_cast<int>(dd.Format), t.planes, static_cast<unsigned long long>(dlb.Width), dlb.Height, static_cast<int>(dlb.Format));
    return nullptr;
}
bool mine(const Targets& t) { return g_img.sc2 && g_img.game_sc == t.sc && g_img.game_d == t.d && g_img.planes == t.planes; }

// ---- the steps (the playback thread, at the list call after their marker)
void prep() {
    Targets t;
    const char* why = gather(&t, nullptr);
    if (!why) why = ensure(t);
    if (why) std::snprintf(g_why, sizeof(g_why), "prep: %s", why);
}
// IN-F / IN-L: eye 1's collector and depth saved (IN-L: the Light Buffer too, into save_lb), eye 2's (SC2L, D2) copied in.
// (review S7) An IN plays back only after eye 2's call has been recorded on the render thread: its draws follow the IN's
// marker in the stream and cannot be taken back, so a refused IN lets them land on eye 1's own collector, depth (and Light
// Buffer): eye 1 exposed, and the module stands down
void in_step(ID3D12GraphicsCommandList* cl, int owed, bool cap) {
    const char* step = owed == kOwedF ? "IN-F" : "IN-L";
    char b[200];
    if (cap) {
        g_cap_bits = 0;
        g_cap_bad = false;
    }
    if (owed == kOwedL) {
        g_lb_saved = false;
        g_pb_l_ok = false;  // (item 6b) set again only by this IN-L's OUT-L
        g_have2 = false;    // (item 7's S2) likewise
    }
    if (g_owed != kNone) {
        g_owed = kNone;
        g_cap_bad = true;
        std::snprintf(b, sizeof(b), "%s found the last OUT not run", step);
        exposed(b);
        return;
    }
    Targets t;
    const char* why = d3d::thread_predicated() ? "the list is predicated" : gather(&t, cl);
    if (!why && !mine(t)) why = "the images are not these targets'";
    if (!why && !g_have) why = "eye 2's collector and depth not kept this frame (the collector step's OUT1)";
    if (why) {
        g_cap_bad = true;
        std::snprintf(b, sizeof(b), "%s refused after eye 2's call: %s", step, why);
        exposed(b);
        return;
    }
    // (review S1, p3-light.md 1.2 and 3.3; run 22) IN-L takes the game's Light Buffer (eye 1's prior content) as pass 2's
    // save marker left it in save_lb; OUT-L puts it back. No save since the last game call: not saved (counted: lb_unsaved),
    // eye 1's call then starts from eye 2's Light Buffer this frame, and a capture is not used
    if (owed == kOwedL) {
        // (review S1) the claim checked: no game transition of the Light Buffer into a write state since the save (lb_touched)
        const bool touched = g_save_ok && d3d::watched_writes(g_lb) != g_save_writes;
        if (touched) bump(g_lb_touched);
        if (g_img.save_lb && g_save_ok && g_save_of == g_lb && !touched && !g_stood.load() &&
            !g_offthread.load(std::memory_order_relaxed)) {  // (review S3: not after a stand-down)
            g_lb_saved = true;
        } else {
            bump(g_lb_unsaved);
            if (cap) g_cap_bad = true;
        }
        g_save_ok = false;  // (eye 1's call writes the Light Buffer after this IN-L's OUT-L)
    }
    Batch x;
    x.add(t.sc, t.ssc, kSrc);
    depth_to(x, t, kSrc);
    x.flush(cl);
    copy_sub(cl, g_img.save_sc, 0, t.sc, 0);
    copy_planes(cl, g_img.save_d, t.d, t.planes);
    if (g_lb_saved) {
        if (cap) {  // (p3-light.md 3.6: eye 2's prior content, for the compare's classing; save_lb holds it, at rest COPY_DEST)
            x.add(g_img.save_lb, kDst, kSrc);
            x.flush(cl);
            copy_rb(cl, kSPrior, g_img.save_lb, 0);
            x.add(g_img.save_lb, kSrc, kDst);
        }
        bump(g_lb_saved_n);
    }
    x.add(t.sc, kSrc, kDst);
    for (unsigned p = 0; p < t.planes; ++p) x.add(t.d, kSrc, kDst, t.planes > 1 ? p : kAll);
    x.add(g_img.sc2, kDst, kSrc);
    x.add(g_img.d2, kDst, kSrc);
    x.flush(cl);
    copy_sub(cl, t.sc, 0, g_img.sc2, 0);
    copy_planes(cl, t.d, g_img.d2, t.planes);
    if (cap) {
        copy_rb(cl, kSIn1, g_img.sc2, 0);
        copy_rb(cl, kSD1, g_img.d2, 0);
        if (t.planes > 1) copy_rb(cl, kSS1, g_img.d2, 1);
        else g_cap_bits |= 1u << kSS1;
    }
    x.add(t.sc, kDst, t.ssc);
    depth_back(x, t, kDst);
    x.add(g_img.sc2, kSrc, kDst);
    x.add(g_img.d2, kSrc, kDst);
    x.flush(cl);
    g_owed = owed;
    bump(owed == kOwedF ? g_inf_ok : g_inl_ok);
}
// OUT-F: the collector (eye 2's final) into SC2L and the depth into D2 (review S5: the footprints' writes kept, if any);
// OUT-L: the Light Buffer into LB2. Then eye 1's collector and depth back (OUT-L: and its Light Buffer, from save_lb).
void out_step(ID3D12GraphicsCommandList* cl, int owed, bool cap) {
    const char* step = owed == kOwedF ? "OUT-F" : "OUT-L";
    char b[200];
    const bool saved = owed == kOwedL && g_lb_saved;
    if (owed == kOwedL) {
        g_lb_saved = false;
        g_have = false;   // (consumed, whatever follows)
        g_have2 = false;  // (item 7's S2: set again below only by this OUT-L)
    }
    if (g_owed != owed) {  // (its IN refused: it stood down already)
        g_cap_bad = true;
        if (g_owed != kNone) {
            g_owed = kNone;
            std::snprintf(b, sizeof(b), "%s after another step's IN", step);
            exposed(b);
        }
        return;
    }
    g_owed = kNone;
    Targets t;
    const char* why = d3d::thread_predicated() ? "the list is predicated" : gather(&t, cl);
    if (!why && (!mine(t) || !g_img.save_sc)) why = "the images changed since the IN";
    if (why) {
        g_cap_bad = true;
        std::snprintf(b, sizeof(b), "%s could not put eye 1's collector and depth back: %s", step, why);
        exposed(b);
        return;
    }
    Batch x;
    bool put = false;  // (OUT-L) the Light Buffer put back from save_lb
    bool lb2_written = false;  // (item 6b) OUT-L copied the Light Buffer into LB2
    bool stale = false, dl_written = false;  // (item 7's S2) LB2 left stale by the control; D2L written
    D3D12_RESOURCE_STATES ls{};
    if (owed == kOwedF) {
        x.add(t.sc, t.ssc, kSrc);
        depth_to(x, t, kSrc);
        x.flush(cl);
        copy_sub(cl, g_img.sc2, 0, t.sc, 0);           // eye 2's final collector
        copy_planes(cl, g_img.d2, t.d, t.planes);      // (review S5) eye 2's depth after its footprints, both planes
        x.add(t.sc, kSrc, kDst);
        for (unsigned p = 0; p < t.planes; ++p) x.add(t.d, kSrc, kDst, t.planes > 1 ? p : kAll);
    } else {
        const bool lb_ok = lb_state(cl, &ls);
        put = lb_ok && saved && g_img.save_lb;
        // (item 7's S1) eye 2's depth after its lights into D2L (both planes); on a capture frame also to readback (the control
        // "d2lctl": D2's instead, eye 2's depth before its lights, which must differ where the lights marked the stencil)
        const bool dl = g_d2l_on.load(std::memory_order_relaxed) && g_img.d2l;
        if (dl) {
            depth_to(x, t, kSrc);
            const bool ctl = cap && g_d2l_ctl.load(std::memory_order_relaxed);
            if (cap) g_cap_d2l_ctl = ctl;
            if (ctl) x.add(g_img.d2, kDst, kSrc);
            x.flush(cl);
            copy_planes(cl, g_img.d2l, t.d, t.planes);
            bump(g_d2l_copies);
            dl_written = true;
            if (cap) {
                ID3D12Resource* src = ctl ? g_img.d2 : t.d;
                copy_rb(cl, kSDL1, src, 0);
                if (t.planes > 1) copy_rb(cl, kSSL1, src, 1);
                else g_cap_bits |= 1u << kSSL1;
            }
            if (ctl) x.add(g_img.d2, kSrc, kDst);
            for (unsigned pl = 0; pl < t.planes; ++pl) x.add(t.d, kSrc, kDst, t.planes > 1 ? pl : kAll);  // (on to eye 1's restore)
        }
        if (lb_ok) {
            x.add(g_lb, ls, kSrc);
            x.flush(cl);
            // (item 7's S2, "prepass p2skip control stale", test only) LB2 left as an earlier frame's: P2IN then copies a stale one
            stale = g_p2in_stale.load(std::memory_order_relaxed);
            if (stale) bump(g_stale_n);
            else copy_sub(cl, g_img.lb2, 0, g_lb, 0);  // eye 2's Light Buffer
            lb2_written = !stale;
            if (cap) copy_rb(cl, kSOut1, g_lb, 0);
            x.add(g_lb, kSrc, put ? kDst : ls);
        } else {
            bump(g_lb_unknown);  // (eye 1's collector and depth are still put back below; this frame's LB2 and Light Buffer are not)
            g_cap_bad = g_cap_bad || cap;
        }
        if (saved && !put) bump(g_lb_not_restored);  // (eye 1's call starts from eye 2's Light Buffer this frame)
        if (put) x.add(g_img.save_lb, kDst, kSrc);
        x.add(t.sc, t.ssc, kDst);
        if (!dl) depth_to(x, t, kDst);
    }
    x.add(g_img.save_sc, kDst, kSrc);
    x.add(g_img.save_d, kDst, kSrc);
    x.flush(cl);
    copy_sub(cl, t.sc, 0, g_img.save_sc, 0);  // eye 1's back
    copy_planes(cl, t.d, g_img.save_d, t.planes);
    if (put) copy_sub(cl, g_lb, 0, g_img.save_lb, 0);  // (review S1) eye 1's Light Buffer back: its call starts from today's
    x.add(t.sc, kDst, t.ssc);
    depth_back(x, t, kDst);
    x.add(g_img.save_sc, kSrc, kDst);
    x.add(g_img.save_d, kSrc, kDst);
    if (put) {
        x.add(g_lb, kDst, ls);
        x.add(g_img.save_lb, kSrc, kDst);
        bump(g_lb_put_back);
    }
    x.flush(cl);
    bump(owed == kOwedF ? g_outf_ok : g_outl_ok);
    if (owed == kOwedL) g_pb_l_ok = lb2_written && !g_stood.load();  // (item 6b) SC2L and LB2 both this pass 1's
    if (owed == kOwedL) {  // (item 7's S2) P2IN's gate: D2L, LB2 (or the stale control's) and SC2L this pass 1's
        g_have2 = dl_written && (lb2_written || stale) && !g_stood.load();
        if (g_have2) bump(g_have2_n);
    }
}
// (item 6b) the check step: after every step before its marker, the last IN-L / OUT-L pair's result to PrepassEye2Images
// tag: the check marker's own (0..kCheckTags-1, carried by its callback and its queued op: the caller's frame slot)
void check_step(int tag) {
    bump(g_checks);
    const bool fresh = g_pb_l_ok && !g_stood.load();
    if (fresh) bump(g_checks_fresh);
    if (const FreshSink_t sink = g_fresh_sink.load(std::memory_order_acquire)) sink(fresh, g_made.load(std::memory_order_relaxed), tag);
}
// its marker's callback (the playback thread): at once when no step is queued (every step before it has run), else queued
// behind them; a check that cannot be queued (another thread's queue, the queue full) reports not fresh, and is not a
// stand-down (it changes nothing of the game's)
template <int Tag>
void cb_check(void*) {
    static_assert(Tag >= 0 && Tag < kCheckTags, "a check tag");
    const DWORD tid = GetCurrentThreadId();
    const bool mine = g_ops_tid == 0 || tid == g_ops_tid;  // (the steps' thread: g_pb_l_ok is its own)
    if (mine && g_nops == 0) {
        bump(g_checks_now);
        check_step(Tag);
        return;
    }
    if (!mine || g_nops >= static_cast<int>(sizeof(g_ops))) {
        bump(g_checks_dropped);
        if (const FreshSink_t sink = g_fresh_sink.load(std::memory_order_acquire)) sink(false, 0, Tag);
        return;
    }
    g_ops[g_nops++] = static_cast<uint8_t>(kCheck + Tag);
    prepass::g_eye2_light_pending_flag.store(true, std::memory_order_release);
}
static_assert(kCheckTags == 4, "kCbCheck and poll's check cases list four tags");  // (the delta review: never a null marker)
void (*const kCbCheck[kCheckTags])(void*) = {&cb_check<0>, &cb_check<1>, &cb_check<2>, &cb_check<3>};
// (run 22) pass 2's save marker: the game's Light Buffer into save_lb from the state the watch saw in this list (pass 2's
// call transitioned it here, as eye 2's does before OUT-L)
void save2_step(ID3D12GraphicsCommandList* cl) {
    g_save_ok = false;
    D3D12_RESOURCE_STATES ls{};
    if (d3d::thread_predicated() || !g_img.save_lb || !lb_state(cl, &ls)) {
        bump(g_save2_failed);
        return;
    }
    Batch x;
    x.add(g_lb, ls, kSrc);
    x.flush(cl);
    copy_sub(cl, g_img.save_lb, 0, g_lb, 0);
    x.add(g_lb, kSrc, ls);
    x.flush(cl);
    g_save_ok = true;
    g_save_of = g_lb;
    g_save_writes = d3d::watched_writes(g_lb);
    bump(g_save2_ok);
}
// (item 7's S2, research\run25\item7.md 2 and 3; [Stereo] Pass2FrontSkip) P2IN: pass 2's front half (the decals, the directional
// term, the footprints and the lights) was recorded with the recorder's writers skipped, so the game's collector, depth and
// Light Buffer hold pass 1's eye-1 results and slice 1's depth. Eye 2's, made in pass 1, copied in: SC2L -> the collector, D2L
// -> the depth (both planes), LB2 -> the Light Buffer. Its marker is recorded right after the lights' stretch ended, before
// pass 2's save marker, so save2_step then takes the copied Light Buffer, as two-pass takes pass 2's own. The game targets from
// their followed states (the collector and each depth plane: op 9's barriers were skipped, so the depth's state is the
// follow's, not two-pass's; item7.md 7, design change 3) and the barrier watch's (the Light Buffer: the lights' 0x25 binds
// still play), each moved to COPY_DEST and back exactly; the images COPY_DEST -> COPY_SOURCE and back (their rest state). A
// refusal (no have2, a predicated list, a state not known, the images not these targets', SC2L or LB2 lent on another list)
// leaves eye 2 with pass 1's eye-1 images for this frame: exposed2, and pass2_skip stands down. Lent on this list: given back
// first (step_may_touch, the lend rules). "control nocopy lb|sc|d|all" leaves those copies out (the positive controls)
void p2in_step(ID3D12GraphicsCommandList* cl) {
    const bool have = g_have2;
    g_have2 = false;  // (consumed, whatever follows)
    Targets t;
    D3D12_RESOURCE_STATES ls{};
    const char* why = !have                      ? "eye 2's images not kept this frame (no OUT-L with D2L, LB2 and SC2L final)"
                      : g_stood.load()           ? "PrepassEye2Light stood down"
                      : d3d::thread_predicated() ? "the list is predicated"
                      : !step_may_touch(cl)      ? "SC2L or LB2 lent to the main walk on another open list or thread"
                                                 : gather(&t, cl);
    if (!why && (!mine(t) || !g_img.d2l || !g_img.lb2 || g_img.game_lb != g_lb)) why = "the images are not these targets'";
    if (!why && !lb_state(cl, &ls)) why = "the Light Buffer's state not seen in this list";
    if (why) {
        char b[200];
        std::snprintf(b, sizeof(b), "P2IN refused: %s", why);
        refuse(g_p2in_refused, b);
        p2skip::exposed2(b);
        return;
    }
    const unsigned no = g_p2in_nocopy.load(std::memory_order_relaxed);
    const bool sc = !(no & p2skip::kNoCopySc), d = !(no & p2skip::kNoCopyD), lb = !(no & p2skip::kNoCopyLb);
    if (no) bump(g_p2in_ctl_n);
    Batch x;
    if (sc) {
        x.add(t.sc, t.ssc, kDst);
        x.add(g_img.sc2, kDst, kSrc);
    }
    if (d) {
        depth_to(x, t, kDst);
        x.add(g_img.d2l, kDst, kSrc);
    }
    if (lb) {
        x.add(g_lb, ls, kDst);
        x.add(g_img.lb2, kDst, kSrc);
    }
    x.flush(cl);
    if (sc) copy_sub(cl, t.sc, 0, g_img.sc2, 0);         // eye 2's final collector
    if (d) copy_planes(cl, t.d, g_img.d2l, t.planes);    // eye 2's depth after its lights (the light stencil marks)
    if (lb) copy_sub(cl, g_lb, 0, g_img.lb2, 0);         // eye 2's Light Buffer
    if (sc) {
        x.add(t.sc, kDst, t.ssc);
        x.add(g_img.sc2, kSrc, kDst);
    }
    if (d) {
        depth_back(x, t, kDst);
        x.add(g_img.d2l, kSrc, kDst);
    }
    if (lb) {
        x.add(g_lb, kDst, ls);
        x.add(g_img.lb2, kSrc, kDst);
    }
    x.flush(cl);
    bump(g_p2in_ok);
}
// pass 2 (a capture): its collector and depth at its call (with_inputs), or its Light Buffer after it, to readback
bool read_pass2(ID3D12GraphicsCommandList* cl, bool with_inputs, Count& refused) {
    if (d3d::thread_predicated()) {
        refuse(refused, "pass 2: the list is predicated");
        return false;
    }
    if (with_inputs) {
        Targets t;
        const char* why = gather(&t, cl);
        if (!why && (!g_img.rb || !mine(t))) why = "pass 2: the images are not this capture's";
        if (why) {
            refuse(refused, why);
            return false;
        }
        Batch x;
        x.add(t.sc, t.ssc, kSrc);
        depth_to(x, t, kSrc);
        x.flush(cl);
        copy_rb(cl, kSIn2, t.sc, 0);
        copy_rb(cl, kSD2, t.d, 0);
        if (t.planes > 1) copy_rb(cl, kSS2, t.d, 1);
        else g_cap_bits |= 1u << kSS2;
        x.add(t.sc, kSrc, t.ssc);
        depth_back(x, t, kSrc);
        x.flush(cl);
        return true;
    }
    D3D12_RESOURCE_STATES ls{};
    if (!g_img.rb || !lb_state(cl, &ls)) {
        bump(g_lb_unknown);
        refuse(refused, "pass 2: the Light Buffer's state not seen in this list");
        return false;
    }
    Targets t;  // (item 7's S1) pass 2's depth after its own lights
    const bool dl = g_d2l_on.load(std::memory_order_relaxed) && g_img.d2l;
    if (dl) {
        const char* why = gather(&t, cl);
        if (!why && !mine(t)) why = "pass 2: the images are not this capture's";
        if (why) {
            refuse(refused, why);
            return false;
        }
    }
    Batch x;
    x.add(g_lb, ls, kSrc);
    if (dl) depth_to(x, t, kSrc);
    x.flush(cl);
    copy_rb(cl, kSOut2, g_lb, 0);
    if (dl) {
        copy_rb(cl, kSDL2, t.d, 0);
        if (t.planes > 1) copy_rb(cl, kSSL2, t.d, 1);
        else g_cap_bits |= 1u << kSSL2;
        depth_back(x, t, kSrc);
    }
    x.add(g_lb, kSrc, ls);
    x.flush(cl);
    return true;
}
void out2_done() {
    if (g_cap_bits == all_slots() && !g_cap_bad && g_img.rb) {
        g_job.rb = g_img.rb;
        std::memcpy(g_job.fp, g_img.fp, sizeof(g_job.fp));
        std::memcpy(g_job.rows, g_img.rows, sizeof(g_job.rows));
        std::memcpy(g_job.rowbytes, g_img.rowbytes, sizeof(g_job.rowbytes));
        g_job.size = g_img.rb_size;
        g_job.cut = g_cap_cut.load();
        g_job.fp_drawn = g_cap_fp.load();
        g_job.fp2 = g_cap_fp2.load();
        g_job.interior = g_cap_interior.load();
        g_job.d2l_ctl = g_cap_d2l_ctl;
        g_job.n2 = g_cap_n2.load();
        g_job.n1 = g_cap_n1.load();
        g_job.np2 = g_cap_np2.load();
        g_ready_frame.store(state::presents.load());
        int s = kRec2;
        if (!g_cap.compare_exchange_strong(s, kReady)) bump(g_cap_incomplete);
        return;
    }
    bump(g_cap_incomplete);
    int s = kRec2;
    g_cap.compare_exchange_strong(s, kArmed);
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
// (review S1, p3-light.md 3.6) the Light Buffer's differences classed against eye 2's prior content (kSPrior, the Light
// Buffer at IN-L = save_lb): a differing texel where LB2 still equals the prior is one eye 2's call did not write (pass 2's
// call starts from eye 1's Light Buffer, eye 2's from last frame's: such texels differ by construction). written: the
// texels eye 2's call changed (0: it wrote nothing, and "prior only" would explain everything: not accepted)
struct LbDiff {
    uint64_t texels = 0, at_prior = 0, written = 0;
    bool bad = false;
};
LbDiff diff_lb(const uint8_t* base, const Job& j) {
    LbDiff d;
    const UINT w = j.fp[kSOut1].Footprint.Width, h = j.rows[kSOut1];
    const UINT64 rb = j.rowbytes[kSOut1];
    const UINT bpt = w ? static_cast<UINT>(rb / w) : 0;
    if (!bpt || j.fp[kSOut2].Footprint.Width != w || j.fp[kSPrior].Footprint.Width != w || j.rows[kSOut2] != h || j.rows[kSPrior] != h) {
        d.bad = true;
        return d;
    }
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* pa = base + j.fp[kSOut1].Offset + static_cast<UINT64>(y) * j.fp[kSOut1].Footprint.RowPitch;
        const uint8_t* pb = base + j.fp[kSOut2].Offset + static_cast<UINT64>(y) * j.fp[kSOut2].Footprint.RowPitch;
        const uint8_t* pp = base + j.fp[kSPrior].Offset + static_cast<UINT64>(y) * j.fp[kSPrior].Footprint.RowPitch;
        if (!std::memcmp(pa, pb, static_cast<size_t>(rb)) && !std::memcmp(pa, pp, static_cast<size_t>(rb))) continue;
        for (UINT x = 0; x < w; ++x) {
            const size_t o = static_cast<size_t>(x) * bpt;
            const bool wrote = std::memcmp(pa + o, pp + o, bpt) != 0;
            if (wrote) ++d.written;
            if (!std::memcmp(pa + o, pb + o, bpt)) continue;
            ++d.texels;
            if (!wrote) ++d.at_prior;
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
    const Diff in_sc = diff(p, j, kSIn1, kSIn2, false), in_d = diff(p, j, kSD1, kSD2, true), in_s = diff(p, j, kSS1, kSS2, false),
               out = diff(p, j, kSOut1, kSOut2, false);
    const LbDiff lbd = diff_lb(p, j);
    const bool dl = g_d2l_on.load(std::memory_order_relaxed);
    const Diff dl_d = dl ? diff(p, j, kSDL1, kSDL2, true) : Diff{}, dl_s = dl ? diff(p, j, kSSL1, kSSL2, false) : Diff{};
    D3D12_RANGE none{0, 0};
    j.rb->Unmap(0, &none);
    bump(g_c_frames);
    if (j.cut) bump(g_c_cut);
    if (j.fp_drawn || j.fp2) bump(g_c_fp);
    if (j.interior) bump(g_c_interior);
    const bool in_eq = in_sc.texels == 0 && in_d.texels == 0 && in_s.texels == 0;
    if (in_eq) bump(g_c_in_eq);
    if (in_eq && j.cut) bump(g_c_cut_in_eq);
    if (in_sc.texels) bump(g_c_in_sc_diff);
    if (in_d.texels) bump(g_c_in_d_diff);
    if (in_s.texels) bump(g_c_in_s_diff);
    if (in_d.hi) bump(g_c_d_hi);
    if (out.texels == 0) bump(g_c_out_exact);
    if (out.texels == 0 && in_eq) {
        bump(g_c_out_exact_in_eq);
        bump(j.fp_drawn || j.fp2 ? g_c_fp_in_eq_exact : g_c_nofp_in_eq_exact);
    }
    if (out.texels != 0 && in_eq) bump(g_c_out_diff_in_eq);
    const bool prior_only = out.texels != 0 && !lbd.bad && lbd.texels == out.texels && lbd.at_prior == lbd.texels && lbd.written > 0;
    if (!lbd.bad && lbd.written == 0) bump(g_c_written_zero);
    if (prior_only) bump(g_c_prior_only);
    if (out.texels != 0 && in_eq) bump(prior_only ? g_c_prior_only_in_eq : g_c_unexplained_in_eq);
    if (j.cut) {  // (review S6: the Light Buffer on cut frames reported whatever the inputs, for indiff=info)
        if (out.texels == 0) bump(g_c_cut_out_exact);
        else if (prior_only) bump(g_c_cut_prior_only);
        store_max(g_c_cut_out_texels_max, out.texels);
    }
    store_max(g_c_prior_texels_max, lbd.bad ? 0 : lbd.at_prior);
    if (dl) {  // (item 7's S1) D2L against pass 2's depth after its lights; the control frames (d2lctl) counted apart
        const bool eq = dl_d.texels == 0 && dl_s.texels == 0;
        if (j.d2l_ctl) bump(eq ? g_c_dl_ctl_eq : g_c_dl_ctl_diff);
        else {
            bump(eq ? g_c_dl_eq : g_c_dl_diff);
            if (eq && in_eq) bump(g_c_dl_eq_in_eq);
            if (j.fp_drawn || j.fp2) bump(eq ? g_c_dl_fp_eq : g_c_dl_fp_diff);
        }
        if (!j.d2l_ctl) {  // (the review: the control's counted apart)
            store_max(g_c_dl_d_max, dl_d.texels);
            store_max(g_c_dl_s_max, dl_s.texels);
        }
    }
    if (j.n2 >= 0 && j.np2 >= 0) bump(j.n2 == j.np2 ? g_c_n_equal : g_c_n_differ);
    if (j.n2 >= 0 && j.n1 >= 0 && j.n2 != j.n1) bump(g_c_n_eyes_differ);
    if (out.texels)
        for (int i = 0; i < 4; ++i) g_c_box[i].store(out.box[i]);
    store_max(g_c_out_texels_max, out.texels);
    store_max(g_c_out_worst, out.worst);
    store_max(g_c_in_sc_texels_max, in_sc.texels);
    store_max(g_c_in_d_texels_max, in_d.texels);
    store_max(g_c_in_s_texels_max, in_s.texels);
    log::limited("e2lb.cmp", 40, "[e2lb] compared (%s%s%s): inputs collector %llu, depth %llu, stencil %llu texels differ; eye 2's Light Buffer %llu "
                 "texels differ (worst %llu, box %d,%d..%d,%d; at eye 2's prior %llu, eye 2 wrote %llu); lights eye 2 %d, eye 1 %d, pass 2 %d",
                 j.cut ? "cut" : "not cut", j.fp_drawn || j.fp2 ? ", footprints" : "", j.interior ? ", interior" : "",
                 static_cast<unsigned long long>(in_sc.texels), static_cast<unsigned long long>(in_d.texels), static_cast<unsigned long long>(in_s.texels),
                 static_cast<unsigned long long>(out.texels), static_cast<unsigned long long>(out.worst), out.box[0], out.box[1], out.box[2], out.box[3],
                 static_cast<unsigned long long>(lbd.at_prior), static_cast<unsigned long long>(lbd.written), j.n2, j.n1, j.np2);
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

// ---- the render thread: what one call leaves, snapshot and put back
struct Snap {
    uint32_t rs[kRsIds];
    bool have_rs;
    uintptr_t ctx, vd;
    bool have_vd;
    int32_t sc[4];
    bool have_sc;
    uint32_t ref;
    bool have_ref;
    uintptr_t vol;
    uint16_t vol_n;
    bool have_vol;
};
// the lights' rects (the render thread only): the pool's array slot, the light it held, its bytes. Put back only where the
// slot still holds that light (a light the game removed meanwhile is not written)
struct Rect {
    uintptr_t slot, light;
    uint8_t b[kRectLen];
};
Rect g_rects[kPoolA + kPoolB];
int g_nrects = 0;
// (review M1) the recorder context as the game reads it (PassBind 0x14015157b) and prepass::thread_stream_ok and rec_state's
// check compare it: TEB +0x58 is the TLS array, its slot 0 (*gs:[0x58]) the block, the block's +0x38 byte its lazy init, the
// block +0x28 the context; and it must be the device's (*(D3dDeviceSingleton) +0x58), the context the markers record into.
// (It was read one level too deep: the block's first qword taken as the block, as eye2_collector's run_eye2 did before its fix.)
// 0: not that context (the run is skipped before anything is recorded: no_snap)
uintptr_t tls_ctx() {
    uintptr_t dev = 0, hctx = 0, block = 0, ctx = 0;
    unsigned char init = 0;
    const uintptr_t tls = static_cast<uintptr_t>(__readgsqword(0x58));
    if (!rd(anchors::addr(Id::D3dDeviceSingleton), &dev) || !dev || !rd(dev + 0x58, &hctx) || !hctx) return 0;
    if (!rd(tls, &block) || !block || !rd(block + 0x38, &init) || !init || !rd(block + 0x28, &ctx) || ctx != hctx) return 0;
    return ctx;
}
// false: a part eye 1's call needs put back cannot be read (the run is skipped, before anything is recorded)
bool snap_take(Snap* s, bool light) {
    std::memset(s, 0, sizeof(*s));
    s->have_rs = raw_read(g_rs_cache, s->rs, sizeof(s->rs));
    s->ctx = tls_ctx();
    s->have_vd = g_bind_vd && s->ctx && rd(s->ctx + 0x1a0, &s->vd);
    if (!s->have_rs || !s->have_vd) return false;
    if (!light) return true;
    s->have_sc = s->ctx && raw_read(s->ctx + 0x148, s->sc, sizeof(s->sc));
    s->have_ref = rd(anchors::addr(Id::LightStencilRef), &s->ref);
    // (the list's pointer, null or not, and its count when there is one: snap_put compares both)
    s->have_vol = rd(g_vol_global, &s->vol) && (!s->vol || rd(s->vol + 8, &s->vol_n));
    if (!s->have_sc || !s->have_ref || !s->have_vol) return false;
    g_nrects = 0;
    const uintptr_t pools[2] = {anchors::addr(Id::LightPoolA), anchors::addr(Id::LightPoolB)};
    const uintptr_t counts[2] = {0x1008, 0x808};
    const int caps[2] = {kPoolA, kPoolB};
    for (int k = 0; k < 2; ++k) {
        uintptr_t pool = 0;
        int n = 0;
        if (!rd(pools[k], &pool) || !pool || !rd(pool + counts[k], &n) || n <= 0) continue;
        if (n > caps[k]) n = caps[k];
        for (int i = 0; i < n && g_nrects < kPoolA + kPoolB; ++i) {
            uintptr_t l = 0;
            const uintptr_t slot = pool + 8 + static_cast<uintptr_t>(i) * 8;
            if (!rd(slot, &l) || !l) continue;
            Rect& r = g_rects[g_nrects];
            if (!raw_read(l + kRectOff, r.b, kRectLen)) continue;
            r.slot = slot;
            r.light = l;
            ++g_nrects;
        }
    }
    return true;
}
bool snap_put(const Snap& s, bool light, uint64_t* restored) {
    bool ok = true;
    if (s.have_rs) {
        const RsSet_t rs_set = reinterpret_cast<RsSet_t>(anchors::addr(Id::RenderStateSet));
        for (int id = 0; id < kRsIds; ++id) {
            uint32_t now = 0;
            if (!rd(g_rs_cache + static_cast<uintptr_t>(id) * 4, &now) || now == s.rs[id]) continue;
            __try {
                rs_set(id, s.rs[id]);
                ++*restored;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
        }
    } else {
        ok = false;  // (the cache not read: eye 1 would start from eye 2's states)
    }
    uintptr_t vd_now = 0;
    if (s.have_vd && rd(s.ctx + 0x1a0, &vd_now) && vd_now != s.vd) {
        __try {
            reinterpret_cast<BindVd_t>(g_bind_vd)(s.ctx, s.vd);
            ++*restored;
            bump(g_vd_rebinds);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    if (!light) return ok;
    int32_t sc_now[4] = {};
    if (s.have_sc && raw_read(s.ctx + 0x148, sc_now, sizeof(sc_now)) && std::memcmp(sc_now, s.sc, sizeof(sc_now)) != 0) {
        __try {
            reinterpret_cast<Scissor_t>(anchors::addr(Id::ScissorSet))(s.ctx, s.sc);
            ++*restored;
            bump(g_scissor_restored);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    uint32_t ref_now = 0;
    if (s.have_ref && rd(anchors::addr(Id::LightStencilRef), &ref_now) && ref_now != s.ref) {
        if (wr(anchors::addr(Id::LightStencilRef), s.ref)) bump(g_ref_restored);
        else ok = false;
    }
    uintptr_t vol_now = 0;
    uint16_t n_now = 0;
    if (!s.have_vol || !rd(g_vol_global, &vol_now) || vol_now != s.vol) {
        // (review S4) the light-volume list replaced (or made) during eye 2's call: eye 2's appends cannot be taken back, eye
        // 1's image would draw them: a fault, the module stands down
        bump(g_vol_moved);
        ok = false;
    } else if (s.vol) {
        if (!rd(s.vol + 8, &n_now)) {
            ok = false;
        } else if (n_now != s.vol_n) {
            if (wr(s.vol + 8, s.vol_n)) bump(g_vol_restored);  // (eye 2's appended volumes dropped: eye 1's appends overwrite them)
            else ok = false;
        }
    }
    uint64_t rects = 0;
    for (int i = 0; i < g_nrects; ++i) {
        uint8_t now[kRectLen];
        uintptr_t still = 0;
        if (!rd(g_rects[i].slot, &still) || still != g_rects[i].light) continue;
        if (!raw_read(g_rects[i].light + kRectOff, now, kRectLen) || !std::memcmp(now, g_rects[i].b, kRectLen)) continue;
        if (raw_write(g_rects[i].light + kRectOff, g_rects[i].b, kRectLen)) ++rects;
        else ok = false;
    }
    if (rects) g_rects_restored.fetch_add(rects, std::memory_order_relaxed);
    return ok;
}
// ([Stereo] PrepassEye2Velocity, eye2_velocity.cpp; test only) called right after eye 2's lights, vp2 still current, before
// SetCurrent(cur) and snap_put (null unless installed)
std::atomic<LightTail_t> g_tail{nullptr};
// eye 2's call between two SetCurrent calls, under SEH (no C++ objects in this frame)
bool call_light(void* lighting, uintptr_t vp2, uintptr_t cur) {
    alignas(16) unsigned char cv2[kCvBytes];
    std::memset(cv2, 0, sizeof(cv2));
    const SetCurrent_t set_current = reinterpret_cast<SetCurrent_t>(anchors::addr(Id::ViewportSetCurrent));
    bool ok = true;
    __try {
        reinterpret_cast<CvCtor_t>(anchors::addr(Id::CullViewCtor))(cv2);
        reinterpret_cast<CvBuild_t>(anchors::addr(Id::BuildCullView))(cv2, reinterpret_cast<void*>(vp2), reinterpret_cast<void*>(vp2 + 0x40));
        set_current(vp2, 1, 0, 0);
        o_light(lighting, cv2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (ok) {  // (PrepassEye2Velocity: eye 2's velocity pass, research\run25\velocity2.md 2.3; it keeps its own SEH)
        const LightTail_t tail = g_tail.load(std::memory_order_relaxed);
        if (tail) tail();
    }
    __try {
        set_current(cur, 1, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}
bool call_foot(void* list, uintptr_t vp2, uintptr_t cur) {
    const SetCurrent_t set_current = reinterpret_cast<SetCurrent_t>(anchors::addr(Id::ViewportSetCurrent));
    bool ok = true;
    __try {
        set_current(vp2, 1, 0, 0);
        o_foot(list);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    __try {
        set_current(cur, 1, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

bool render_thread() {
    DWORD id = 0;
    return rd(anchors::addr(Id::RenderThreadId), &id) && id == GetCurrentThreadId();
}
// the gates both (F) and (L) share in pass 1 (eye2_collector's MUST-1: what predicts the IN's success, before anything).
// foot: (F)'s call: its refusals counted apart (review S6): no collector run and not ready in f_no_e2sc and f_not_ready,
// every other reason in f_gated; (L)'s keep their own counters
bool gates(void** vp2_out, uintptr_t* cur_out, bool foot) {
    auto no = [foot](Count& l) {
        bump(foot ? g_f_gated : l);
        return false;
    };
    if (g_clear_owed.load(std::memory_order_relaxed)) {  // (review S3: the last clear not appended; (L) appends it after eye 1's call)
        bump(g_clear_owed_skips);
        return false;
    }
    if (!render_thread()) return no(g_not_rt);
    if (!eye2_collector::usable() || eye2_collector::runs() == g_l_last_e2) {  // (the collector step did not run in this pass 1)
        bump(foot ? g_f_no_e2sc : g_no_e2sc);
        return false;
    }
    void* vp2 = camera_lever::eye2_viewport();
    if (!vp2) return no(g_no_vp2);
    uintptr_t cur = 0;
    if (!rd(anchors::addr(Id::ViewportCurrent), &cur) || !cur || reinterpret_cast<void*>(cur) != camera_lever::scene_viewport()) return no(g_not_scene);
    if (!prepass::thread_stream_ok()) return no(g_no_stream);
    if (!prepass::cut_tracking()) return no(g_no_follow);
    if (!prepass::on() || !prepass::instanced_on()) return no(g_prepass_off);
    if (!prepass::pass1_walk_ready()) {
        bump(foot ? g_f_not_ready : g_not_ready);
        return false;
    }
    if (!g_img_ready.load()) {
        if (foot) return no(g_preps);  // ((L) appends the prep marker)
        if (prepass::stream_marker(&cb_prep)) bump(g_preps);
        return false;
    }
    *vp2_out = vp2;
    *cur_out = cur;
    return true;
}

// (F) pass 1: eye 2's footprints into its collector
void foot_pass1(void* list) {
    bump(g_f_calls);
    g_f_called = true;
    void* vp2 = nullptr;
    uintptr_t cur = 0;
    if (!gates(&vp2, &cur, true)) return;
    LARGE_INTEGER t0{}, t1{};
    QueryPerformanceCounter(&t0);
    Snap s;
    if (!snap_take(&s, false)) {
        bump(g_no_snap);
        return;
    }
    if (!prepass::stream_marker(&cb_inf)) {
        bump(g_no_stream);
        return;
    }
    bool ok = call_foot(list, reinterpret_cast<uintptr_t>(vp2), cur);
    uint64_t restored = 0;
    ok = snap_put(s, false, &restored) && ok;
    g_states_restored.fetch_add(restored, std::memory_order_relaxed);
    const bool out = prepass::stream_marker(&cb_outf);
    QueryPerformanceCounter(&t1);
    g_f_ticks.fetch_add(static_cast<uint64_t>(t1.QuadPart - t0.QuadPart), std::memory_order_relaxed);
    bump(g_f_runs);
    g_f_ran = true;
    if (!out) {
        bump(g_out_marker_failed);
        exposed("OUT-F's marker not appended after eye 2's footprints");
    }
    if (!ok) {
        bump(g_fault);
        stand_down("a fault in eye 2's footprints (ViewportSetCurrent, FUN_140193060 on vp2 or the states put back)");
    }
}
void hk_foot(void* list) {
    if (!g_on.load(std::memory_order_relaxed) || reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_ret_f) return o_foot(list);
    const int pass = dual_pass::pass();
    if (pass == 1) foot_pass1(list);
    else if (pass == 2) g_p2_fp = true;
    // (item 7's S2, [Stereo] Pass2FrontSkip; one relaxed load when not installed) pass 2's call in a record-skipped stretch
    // when this pass 2 skips (pass2_skip's enter: nothing in pass 1)
    p2skip::around(p2skip::kFoot, [&] { o_foot(list); });
}

// (L) pass 1: eye 2's lights into the Light Buffer, before eye 1's
void light_pass1(void* lighting) {
    bump(g_l_calls);
    ++g_pass1_seq;
    const bool f_called = g_f_called, f_ran = g_f_ran;
    g_f_called = g_f_ran = false;
    g_l_fp = f_called;  // (item 7's S2)
    void* vp2 = nullptr;
    uintptr_t cur = 0;
    const uint64_t e2 = eye2_collector::runs();
    const bool ok_gates = gates(&vp2, &cur, false);
    g_l_last_e2 = e2;
    if (!ok_gates) return;
    uint8_t lon = 0;
    uintptr_t lrt = 0;
    const uintptr_t L = reinterpret_cast<uintptr_t>(lighting);
    if (!rd(L + kLOn, &lon) || !lon || !rd(L + kLRt, &lrt) || !lrt) {
        bump(g_lighting_off);
        return;
    }
    if (f_called && !f_ran) {  // (the game drew footprints and eye 2's did not run: SC2L is not eye 2's final collector)
        bump(g_fp_unrun);
        return;
    }
    LARGE_INTEGER t0{}, t1{};
    QueryPerformanceCounter(&t0);
    Snap sn;
    if (!snap_take(&sn, true)) {
        bump(g_no_snap);
        return;
    }
    int s = kArmed;
    const bool cap = g_cap.compare_exchange_strong(s, kRec1);
    if (cap) {
        g_cap_since.store(state::presents.load());
        g_cap_seq = g_pass1_seq;
        g_cap_fp.store(f_called);
        uintptr_t loc = 0;
        g_cap_interior.store(rd(L + kLLoc, &loc) && loc != 0);
        bump(g_cap_started);
    }
    uintptr_t loc = 0;
    if (rd(L + kLLoc, &loc) && loc) bump(g_interior);
    if (g_rt_dirty.exchange(false, std::memory_order_relaxed)) {  // (run 22) a game call since the last save marker had no marker after it
        if (!prepass::stream_marker(&cb_nosave)) {
            g_rt_dirty.store(true, std::memory_order_relaxed);
            bump(g_no_stream);
            if (cap) g_cap.store(kArmed);
            return;
        }
    }
    if (!prepass::stream_marker(cap ? &cb_inl_cap : &cb_inl)) {
        bump(g_no_stream);
        if (cap) g_cap.store(kArmed);
        return;
    }
    bool ok = call_light(lighting, reinterpret_cast<uintptr_t>(vp2), cur);
    int n2 = -1;
    if (!rd(L + kLCount, &n2)) n2 = -1;
    uint64_t restored = 0;
    ok = snap_put(sn, true, &restored) && ok;
    g_states_restored.fetch_add(restored, std::memory_order_relaxed);
    const bool out = prepass::stream_marker(cap ? &cb_outl_cap : &cb_outl);
    QueryPerformanceCounter(&t1);
    g_l_ticks.fetch_add(static_cast<uint64_t>(t1.QuadPart - t0.QuadPart), std::memory_order_relaxed);
    bump(g_l_runs);
    g_last_n2 = n2;
    if (cap) g_cap_n2.store(n2);
    if (!out) {
        bump(g_out_marker_failed);
        exposed("OUT-L's marker not appended after eye 2's lights");
    }
    if (!ok) {
        bump(g_fault);
        stand_down("a fault in eye 2's lights (the cull view, ViewportSetCurrent, FUN_1406e0450 on vp2, the states put back, or the "
                   "light-volume list replaced during the call)");
    }
}
// (run 22, the render thread) after pass 2's call: the save marker (eye 1's prior content for the next pass 1's IN-L)
void save_mark() {
    if (render_thread() && prepass::stream_marker(&cb_save2)) {
        g_rt_dirty.store(false, std::memory_order_relaxed);
        return;
    }
    bump(g_save2_marker_failed);
    g_rt_dirty.store(true, std::memory_order_relaxed);
}
// (item 7's S2, [Stereo] Pass2FrontSkip; one relaxed load when not installed) pass 2's call of the lights: in a record-skipped
// stretch when pass2_skip decided this pass 2 skips, then P2IN's marker (pass2_skip's leave, before save_mark's: save_lb takes
// the copied Light Buffer); otherwise the game's call as before
void light2(void* lighting, void* cv) {
    p2skip::around(p2skip::kLight, [&] { o_light(lighting, cv); });
}
void hk_light(void* lighting, void* cv) {
    if (!g_on.load(std::memory_order_relaxed) || reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_ret_l) {
        g_rt_dirty.store(true, std::memory_order_relaxed);  // (run 22: a call with no marker after it)
        // (item 7's S2) a pass 2 pass2_skip latched while e2lb was on still gets its P2IN ("prepass e2lb off" in between)
        if (p2skip::installed() && reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_ret_l && dual_pass::pass() == 2) return light2(lighting, cv);
        return o_light(lighting, cv);
    }
    if (!g_offthread.load(std::memory_order_relaxed) && !render_thread()) {  // (review S2)
        g_offthread.store(true, std::memory_order_relaxed);
        log::warn("[e2lb] a light call off the render thread: the Light Buffer is not saved again in this session (lb_unsaved)");
    }
    const int pass = dual_pass::pass();
    const uintptr_t L = reinterpret_cast<uintptr_t>(lighting);
    if (pass == 1) {
        const uint64_t before = u(g_l_runs);
        const int cs = g_cap.load();
        light_pass1(lighting);
        o_light(lighting, cv);
        if (u(g_l_runs) != before) {  // (eye 2's call ran: eye 1's light count against it; the capture's, if this call started one)
            int n1 = -1;
            if (!rd(L + kLCount, &n1)) n1 = -1;
            if (n1 >= 0 && g_last_n2 >= 0 && n1 != g_last_n2) bump(g_counts_differ);
            if (cs == kArmed && g_cap.load() == kRec1) g_cap_n1.store(n1);
        } else if (prepass::stream_marker(&cb_clear)) {  // (review S3) no IN-L in this pass: this frame's SC2L and D2 unusable
            g_clear_owed = false;                         // after it (OUT-L clears them when eye 2's call ran)
        } else {
            bump(g_clear_marker_failed);
            g_clear_owed = true;  // (the next pass 1 runs no eye-2 step and appends it here again)
            g_rt_dirty.store(true, std::memory_order_relaxed);
        }
        // (item 6b, PrepassEye2Images' freshness; null unless it installed it) after eye 1's call, before the MRT bind and the walk
        if (const LightsDone_t done = g_lights_done.load(std::memory_order_acquire)) done(u(g_l_runs) != before && !g_stood.load());
        // (item 7's S2, [Stereo] Pass2FrontSkip) this pass 1's eye-2 front half for the pass 2 that follows: eye 2's lights ran
        // (so e2sc's term ran and eye 2's footprints where the game drew them), D2L made, the game drew footprints
        if (p2skip::installed())
            p2skip::pass1_lights(u(g_l_runs) != before && !g_stood.load(), g_d2l_on.load(std::memory_order_relaxed) && g_img_ready.load(), g_l_fp);
        return;
    }
    if (pass == 2) {
        bump(g_p2_calls);
        // (item 7's S2) every pass-2 call below through light2: the stretch and P2IN when pass2_skip decided this pass 2 skips
        g_l_last_e2 = eye2_collector::runs();  // (a frame whose pass 1 had no (L): the collector's run not taken for the next)
        const bool fp2 = g_p2_fp;
        g_p2_fp = false;
        int s = kRec1;
        if (g_cap.load() == kRec1 && g_cap_seq != g_pass1_seq) {
            if (g_cap.compare_exchange_strong(s, kArmed)) {
                bump(g_p2_other_frame);
                bump(g_cap_incomplete);
            }
            light2(lighting, cv);
            save_mark();
            return;
        }
        s = kRec1;
        if (!render_thread() || !g_cap.compare_exchange_strong(s, kRec2)) {
            light2(lighting, cv);
            save_mark();
            return;
        }
        g_cap_seq = ~0ull;
        bump(g_p2_caps);
        g_cap_cut.store(prepass::walk_was_cut());
        g_cap_fp2.store(fp2);
        const bool m1 = prepass::stream_marker(&cb_in2_cap);
        light2(lighting, cv);
        int np2 = -1;
        if (!rd(L + kLCount, &np2)) np2 = -1;
        g_cap_np2.store(np2);
        const bool m2 = m1 && prepass::stream_marker(&cb_out2_cap);
        if (!m1 || !m2) {
            bump(g_p2_marker_failed);
            s = kRec2;
            g_cap.compare_exchange_strong(s, kArmed);
        }
        save_mark();
        return;
    }
    o_light(lighting, cv);
    g_rt_dirty.store(true, std::memory_order_relaxed);  // (run 22: a call with no marker after it)
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

double us_per(const Count& ticks, const Count& runs) {
    return u(runs) ? static_cast<double>(u(ticks)) * g_qpc_us / static_cast<double>(u(runs)) : 0.0;
}
std::string kv() {
    char b[6000];
    std::snprintf(
        b, sizeof(b),
        "on=%d stood=%d cap=%d left=%d l_calls=%llu l_runs=%llu us_per_run=%.1f f_calls=%llu f_runs=%llu us_per_f=%.1f no_e2sc=%llu no_vp2=%llu "
        "not_scene=%llu not_rt=%llu lighting_off=%llu no_stream=%llu prepass_off=%llu no_follow=%llu not_ready=%llu preps=%llu fp_unrun=%llu no_snap=%llu "
        "fault=%llu out_marker_failed=%llu p2_calls=%llu p2_caps=%llu p2_marker_failed=%llu p2_other_frame=%llu states_restored=%llu "
        "vd_rebinds=%llu scissor_restored=%llu vol_restored=%llu rects_restored=%llu ref_restored=%llu counts_differ=%llu interior_runs=%llu "
        "from_ok=%llu from_refused=%llu inf_ok=%llu outf_ok=%llu inl_ok=%llu outl_ok=%llu in2_ok=%llu in2_refused=%llu out2_ok=%llu "
        "out2_refused=%llu lb_unknown=%llu made=%llu make_failed=%llu ops_overflow=%llu ops_other_thread=%llu eye1_exposed=%llu not_direct=%llu "
        "caps_started=%llu caps_done=%llu caps_incomplete=%llu frames=%llu cut_frames=%llu fp_frames=%llu interior_frames=%llu in_equal=%llu "
        "cut_in_equal=%llu in_sc_diff=%llu in_d_diff=%llu in_s_diff=%llu d_hi_only=%llu out_exact=%llu out_exact_in_equal=%llu "
        "out_diff_in_equal=%llu fp_exact_in_equal=%llu nofp_exact_in_equal=%llu n_equal=%llu n_differ=%llu n_eyes_differ=%llu "
        "out_texels_max=%llu out_worst=%llu in_sc_texels_max=%llu in_d_texels_max=%llu in_s_texels_max=%llu box=%d,%d,%d,%d map_failed=%llu "
        "timeouts=%llu f_no_e2sc=%llu f_not_ready=%llu f_gated=%llu vol_moved=%llu clear_owed_skips=%llu clear_marker_failed=%llu clears=%llu "
        "lb_saved=%llu lb_unsaved=%llu lb_put_back=%llu lb_not_restored=%llu lb_resolves=%llu lb_changes=%llu prior_only=%llu "
        "prior_only_in_equal=%llu unexplained_in_equal=%llu written_zero=%llu cut_out_exact=%llu cut_prior_only=%llu cut_out_texels_max=%llu "
        "prior_texels_max=%llu save2_ok=%llu save2_failed=%llu save2_marker_failed=%llu nosaves=%llu lb_touched=%llu offthread=%d "
        "checks=%llu checks_fresh=%llu checks_now=%llu checks_dropped=%llu sc2l=%llu lb2=%llu d2l=%d d2l_copies=%llu dl_eq=%llu dl_diff=%llu "
        "dl_eq_in_equal=%llu dl_fp_eq=%llu dl_fp_diff=%llu dl_ctl_eq=%llu dl_ctl_diff=%llu dl_d_max=%llu dl_s_max=%llu d2lctl=%d",
        g_on.load() ? 1 : 0, g_stood.load() ? 1 : 0, g_cap.load(), g_cap_left.load(), u(g_l_calls), u(g_l_runs), us_per(g_l_ticks, g_l_runs),
        u(g_f_calls), u(g_f_runs), us_per(g_f_ticks, g_f_runs), u(g_no_e2sc), u(g_no_vp2), u(g_not_scene), u(g_not_rt), u(g_lighting_off),
        u(g_no_stream), u(g_prepass_off), u(g_no_follow), u(g_not_ready), u(g_preps), u(g_fp_unrun), u(g_no_snap), u(g_fault), u(g_out_marker_failed),
        u(g_p2_calls), u(g_p2_caps), u(g_p2_marker_failed), u(g_p2_other_frame), u(g_states_restored), u(g_vd_rebinds), u(g_scissor_restored),
        u(g_vol_restored), u(g_rects_restored), u(g_ref_restored), u(g_counts_differ), u(g_interior), u(g_from_ok), u(g_from_refused),
        u(g_inf_ok), u(g_outf_ok), u(g_inl_ok), u(g_outl_ok), u(g_in2_ok), u(g_in2_refused), u(g_out2_ok), u(g_out2_refused), u(g_lb_unknown),
        u(g_made), u(g_make_failed), u(g_ops_overflow), u(g_ops_other_thread), u(g_eye1_exposed), u(g_not_direct), u(g_cap_started),
        u(g_cap_done), u(g_cap_incomplete), u(g_c_frames), u(g_c_cut), u(g_c_fp), u(g_c_interior), u(g_c_in_eq), u(g_c_cut_in_eq),
        u(g_c_in_sc_diff), u(g_c_in_d_diff), u(g_c_in_s_diff), u(g_c_d_hi), u(g_c_out_exact), u(g_c_out_exact_in_eq), u(g_c_out_diff_in_eq),
        u(g_c_fp_in_eq_exact), u(g_c_nofp_in_eq_exact), u(g_c_n_equal), u(g_c_n_differ), u(g_c_n_eyes_differ),
        static_cast<unsigned long long>(g_c_out_texels_max.load()), static_cast<unsigned long long>(g_c_out_worst.load()),
        static_cast<unsigned long long>(g_c_in_sc_texels_max.load()), static_cast<unsigned long long>(g_c_in_d_texels_max.load()),
        static_cast<unsigned long long>(g_c_in_s_texels_max.load()), g_c_box[0].load(), g_c_box[1].load(), g_c_box[2].load(), g_c_box[3].load(),
        u(g_c_map_failed), u(g_c_timeout), u(g_f_no_e2sc), u(g_f_not_ready), u(g_f_gated), u(g_vol_moved), u(g_clear_owed_skips),
        u(g_clear_marker_failed), u(g_clears), u(g_lb_saved_n), u(g_lb_unsaved), u(g_lb_put_back), u(g_lb_not_restored), u(g_lb_resolves),
        u(g_lb_changes), u(g_c_prior_only), u(g_c_prior_only_in_eq), u(g_c_unexplained_in_eq), u(g_c_written_zero), u(g_c_cut_out_exact),
        u(g_c_cut_prior_only), static_cast<unsigned long long>(g_c_cut_out_texels_max.load()),
        static_cast<unsigned long long>(g_c_prior_texels_max.load()), u(g_save2_ok), u(g_save2_failed), u(g_save2_marker_failed), u(g_nosaves), u(g_lb_touched), g_offthread.load() ? 1 : 0,
        u(g_checks), u(g_checks_fresh), u(g_checks_now), u(g_checks_dropped), static_cast<unsigned long long>(g_pub_sc2.load()),
        static_cast<unsigned long long>(g_pub_lb2.load()), g_d2l_on.load() ? 1 : 0, u(g_d2l_copies), u(g_c_dl_eq), u(g_c_dl_diff), u(g_c_dl_eq_in_eq),
        u(g_c_dl_fp_eq), u(g_c_dl_fp_diff), u(g_c_dl_ctl_eq), u(g_c_dl_ctl_diff), static_cast<unsigned long long>(g_c_dl_d_max.load()),
        static_cast<unsigned long long>(g_c_dl_s_max.load()), g_d2l_ctl.load() ? 1 : 0);
    return b;
}
void reset() {
    for (Count* c : {&g_l_calls, &g_l_runs, &g_f_calls, &g_f_runs, &g_no_e2sc, &g_no_vp2, &g_not_scene, &g_not_rt, &g_lighting_off, &g_no_stream,
                     &g_prepass_off, &g_no_follow, &g_not_ready, &g_preps, &g_fp_unrun, &g_p2_calls, &g_p2_caps, &g_p2_marker_failed,
                     &g_p2_other_frame, &g_states_restored, &g_vd_rebinds, &g_scissor_restored, &g_vol_restored, &g_rects_restored, &g_ref_restored,
                     &g_l_ticks, &g_f_ticks, &g_counts_differ, &g_interior, &g_no_snap, &g_from_ok, &g_from_refused, &g_inf_ok, &g_outf_ok, &g_inl_ok, &g_outl_ok,
                     &g_in2_ok, &g_in2_refused, &g_out2_ok, &g_out2_refused, &g_lb_unknown, &g_cap_started, &g_cap_done, &g_cap_incomplete,
                     &g_c_dl_eq, &g_c_dl_diff, &g_c_dl_eq_in_eq, &g_c_dl_fp_eq, &g_c_dl_fp_diff, &g_c_dl_ctl_eq, &g_c_dl_ctl_diff, &g_d2l_copies,
                     &g_c_dl_d_max, &g_c_dl_s_max,
                     &g_c_frames, &g_c_cut, &g_c_fp, &g_c_interior, &g_c_in_eq, &g_c_cut_in_eq, &g_c_in_sc_diff, &g_c_in_d_diff, &g_c_in_s_diff,
                     &g_c_d_hi, &g_c_out_exact, &g_c_out_exact_in_eq, &g_c_out_diff_in_eq, &g_c_map_failed, &g_c_timeout, &g_c_n_equal,
                     &g_c_n_differ, &g_c_n_eyes_differ, &g_c_fp_in_eq_exact, &g_c_nofp_in_eq_exact, &g_f_no_e2sc, &g_f_not_ready, &g_f_gated,
                     &g_clear_owed_skips, &g_clear_marker_failed, &g_clears, &g_lb_saved_n, &g_lb_unsaved, &g_lb_put_back, &g_lb_not_restored,
                     &g_c_prior_only, &g_c_prior_only_in_eq, &g_c_unexplained_in_eq, &g_c_written_zero, &g_c_cut_out_exact, &g_c_cut_prior_only,
                     &g_save2_ok, &g_save2_failed, &g_save2_marker_failed, &g_nosaves, &g_lb_touched, &g_checks, &g_checks_fresh, &g_checks_now,
                     &g_checks_dropped})
        c->store(0);
    for (auto* a : {&g_c_out_texels_max, &g_c_out_worst, &g_c_in_sc_texels_max, &g_c_in_d_texels_max, &g_c_in_s_texels_max, &g_c_prior_texels_max,
                    &g_c_cut_out_texels_max})
        a->store(0);
    for (auto& x : g_c_box) x.store(-1);
    // (fault, out_marker_failed, make_failed, ops_*, eye1_exposed, not_direct, vol_moved: kept, they mark a stand-down;
    // lb_resolves and lb_changes kept: the Light Buffer's resolutions over the session)
}

}  // namespace

void from_collector(ID3D12GraphicsCommandList* cl, ID3D12Resource* sc, ID3D12Resource* d, unsigned planes, D3D12_RESOURCE_STATES ssc,
                    const D3D12_RESOURCE_STATES* sd) {
    // (review M2) gated on the install and the stand-down, not on the run-time switch g_on: an IN-F / IN-L recorded while on
    // plays back after this frame's OUT1 even when "prepass e2lb off" came in between, and must find this frame's copy
    // (S3's clear marker keeps an older one from being used)
    if (!g_installed.load() || g_stood.load() || !cl || !sc || !d || !sd || planes < 1 || planes > 2) return;
    if (!step_may_touch(cl)) {  // (item 6c) SC2L
        // lent to the main walk on another open list (or thread): not copied into
        g_have = false;
        refuse(g_from_refused, "SC2L lent to the main walk on another open list or thread");
        return;
    }
    Targets t;
    t.sc = sc;
    t.d = d;
    t.planes = planes;
    t.ssc = ssc;
    t.sd[0] = sd[0];
    t.sd[1] = planes > 1 ? sd[1] : sd[0];
    const char* why = d3d::thread_predicated() ? "the list is predicated" : ensure(t);
    if (why) {
        g_have = false;
        refuse(g_from_refused, why);  // (eye 1 untouched: this frame's (F) and (L) then refuse and stand down, if they ran)
        return;
    }
    Batch x;
    x.add(sc, ssc, kSrc);
    depth_to(x, t, kSrc);
    x.flush(cl);
    copy_sub(cl, g_img.sc2, 0, sc, 0);  // eye 2's collector after its directional term
    copy_planes(cl, g_img.d2, d, planes);  // eye 2's depth after its collector step (the interior branch's stencil marks)
    x.add(sc, kSrc, ssc);
    depth_back(x, t, kSrc);
    x.flush(cl);
    g_have = true;
    bump(g_from_ok);
}

void poll(ID3D12GraphicsCommandList* cl) {
    // (item 6c) a give-back owed (pass 1's walk closed by its marker): on the lent list's next call here; on another list it
    // waits for the lent list's Close (list_closing)
    if (g_give_owed && cl && (GetCurrentThreadId() == g_ops_tid || GetCurrentThreadId() == g_lent_tid)) {
        if (!g_lent_flag.load(std::memory_order_relaxed)) {
            g_give_owed = false;
        } else if (is_lent_list(cl) && cl->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            bump(g_given_poll);
            give_back_now(cl);
        }
        if (GetCurrentThreadId() == g_ops_tid && g_nops == 0) {  // (only the give-back was owed; the 6c delta review: the flag
            // cleared only on the ops thread, whose g_nops this is, so a push() made meanwhile is never wiped)
            prepass::g_eye2_light_pending_flag.store(false, std::memory_order_relaxed);
            return;
        }
        if (GetCurrentThreadId() != g_ops_tid) return;
    }
    if (!cl || GetCurrentThreadId() != g_ops_tid || g_nops == 0) return;
    if (cl->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        bump(g_not_direct);
        exposed("a step's list call on a non-DIRECT list");
        return;
    }
    prepass::g_eye2_light_pending_flag.store(false, std::memory_order_relaxed);
    const int n = g_nops;
    g_nops = 0;
    for (int i = 0; i < n; ++i) {
        // (item 6c, the review's SF1) only the steps that copy SC2L or LB2 wait for the lend: given back first on the lent
        // list; on another open list (or thread) such a step is not run (eye 1 exposed: the module stands down). The others
        // (the check, the clear, the prep, pass 2's save and captures) never touch them and run as before
        const uint8_t op = g_ops[i];
        const bool touches = op == kInF || op == kOutF || op == kInL || op == kInLCap || op == kOutL || op == kOutLCap;
        if (touches && !step_may_touch(cl)) {
            g_have = false;
            g_pb_l_ok = false;
            exposed("a step that copies SC2L or LB2 while they are lent to the main walk on another open list (not run)");
            continue;
        }
        switch (op) {
            case kInF: in_step(cl, kOwedF, false); break;
            case kOutF: out_step(cl, kOwedF, false); break;
            case kInL: in_step(cl, kOwedL, false); break;
            case kInLCap: in_step(cl, kOwedL, true); break;
            case kOutL: out_step(cl, kOwedL, false); break;
            case kOutLCap: out_step(cl, kOwedL, true); break;
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
            case kClear:
                g_have = false;
                g_save_ok = false;  // (run 22: eye 1's call wrote the Light Buffer)
                g_pb_l_ok = false;  // (item 6b)
                g_have2 = false;    // (item 7's S2)
                bump(g_clears);
                break;
            case kP2In: p2in_step(cl); break;  // (item 7's S2: it takes the lend rules itself)
            case kCheck:
            case kCheck + 1:
            case kCheck + 2:
            case kCheck + 3: check_step(g_ops[i] - kCheck); break;
            case kSave2: save2_step(cl); break;
            case kNoSave:
                g_save_ok = false;
                bump(g_nosaves);
                break;
        }
    }
}

namespace {
// a call site: a rel32 call of the target (the return address the hook keys on)
bool call_site(Id site, Id target, uintptr_t* ret) {
    const uintptr_t s = anchors::addr(site);
    uint8_t op = 0;
    int32_t rel = 0;
    if (!rd(s, &op) || op != 0xe8 || !rd(s + 1, &rel) || s + 5 + static_cast<intptr_t>(rel) != anchors::addr(target)) return false;
    *ret = s + 5;
    return true;
}
// (the leaf-hook rule) the target is a .pdata function starting there: safetyhook's trampoline then keeps the unwind
bool has_pdata(uintptr_t f) {
    DWORD64 image = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(static_cast<DWORD64>(f), &image, nullptr);
    return fn && image && image + fn->BeginAddress == f;
}
}  // namespace

void install() {
    const int want = config::get_int("Stereo", "PrepassEye2Light", 0);
    if (want <= 0) return;
    if (want != 1) {
        log::warn("[e2lb] PrepassEye2Light=%d is not 1: off", want);
        return;
    }
    if (config::get_int("Stereo", "PrepassEye2Collector", 0) != 1 || !eye2_collector::usable()) {
        log::warn("[e2lb] PrepassEye2Light needs [Stereo] PrepassEye2Collector=1 installed (its OUT1 keeps eye 2's collector and depth): off");
        return;
    }
    if (anchors::stand_down() || !anchors::exe_matches() || anchors::relocated()) {
        log::warn("[e2lb] PrepassEye2Light runs on the analysed RDR.exe build only: off");
        return;
    }
    for (Id id : {Id::LightFrontHalf, Id::LightFrontHalfCall, Id::CullViewCtor, Id::BuildCullView, Id::Footprints, Id::FootprintsCall, Id::LightVolumes,
                  Id::LightPoolA, Id::LightPoolB, Id::ScissorSet, Id::LightStencilRef, Id::LightingManager, Id::D3dDeviceSingleton})
        if (!anchors::present(id)) {
            log::warn("[e2lb] the anchor %s is absent on this build: off", anchors::name(id));
            return;
        }
    if (!call_site(Id::LightFrontHalfCall, Id::LightFrontHalf, &g_ret_l) || !call_site(Id::FootprintsCall, Id::Footprints, &g_ret_f)) {
        log::warn("[e2lb] a call site is not a call of its function (LightFrontHalfCall, FootprintsCall): off");
        return;
    }
    if (!has_pdata(anchors::addr(Id::LightFrontHalf)) || !has_pdata(anchors::addr(Id::Footprints))) {
        log::warn("[e2lb] FUN_1406e0450 or FUN_140193060 has no .pdata entry starting there (a leaf hook would need a stub): off");
        return;
    }
    {  // DAT_142ad1d98 through FUN_1406df150's own reference (0x1406df94a: 48 8b 3d disp32)
        const uintptr_t at = anchors::base() + kRvaVolRef;
        uint8_t op[3] = {};
        int32_t disp = 0;
        if (!raw_read(at, op, 3) || op[0] != 0x48 || op[1] != 0x8b || op[2] != 0x3d || !rd(at + 3, &disp) ||
            at + 7 + static_cast<intptr_t>(disp) != anchors::addr(Id::LightVolumes)) {
            log::warn("[e2lb] FUN_1406df150's reference of the light-volume list is not as analysed: off");
            return;
        }
        g_vol_global = anchors::addr(Id::LightVolumes);
    }
    {
        uint8_t now[sizeof(kScissorEntry)] = {};
        if (!raw_read(anchors::addr(Id::ScissorSet), now, sizeof(now)) || std::memcmp(now, kScissorEntry, sizeof(now)) != 0) {
            log::warn("[e2lb] FUN_140ec7e90's code is not as analysed (the scissor not put back): off");
            return;
        }
    }
    {  // RenderStateSet 0x147110 [V]: the render-state cache's RVA (eye2_collector's check)
        const uintptr_t f = anchors::addr(Id::RenderStateSet);
        uint8_t code[28] = {};
        uint32_t disp = 0;
        int32_t lea = 0;
        const uint8_t lea_op[3] = {0x4c, 0x8d, 0x05}, mov_op[4] = {0x41, 0x89, 0x94, 0x80}, cmp_op[3] = {0x83, 0xf9, 0x2b};
        if (!raw_read(f, code, sizeof(code)) || std::memcmp(code + 7, lea_op, 3) || std::memcmp(code + 14, mov_op, 4) ||
            std::memcmp(code + 22, cmp_op, 3)) {
            log::warn("[e2lb] RenderStateSet's code is not as analysed (the render-state cache not found): off");
            return;
        }
        std::memcpy(&lea, code + 10, 4);
        std::memcpy(&disp, code + 18, 4);
        if (f + 14 + static_cast<intptr_t>(lea) != anchors::base()) {
            log::warn("[e2lb] RenderStateSet's base is not the image's: off");
            return;
        }
        g_rs_cache = anchors::base() + disp;
    }
    {
        uint8_t now[sizeof(kBindVdEntry)] = {};
        const uintptr_t f = anchors::base() + kRvaBindVd;
        if (raw_read(f, now, sizeof(now)) && !std::memcmp(now, kBindVdEntry, sizeof(now))) {
            g_bind_vd = f;
        } else {
            log::warn("[e2lb] FUN_140ec67a0's entry is not as analysed (the recorder's binding not put back): off");
            return;
        }
    }
    LARGE_INTEGER fr{};
    QueryPerformanceFrequency(&fr);
    g_qpc_us = fr.QuadPart ? 1e6 / static_cast<double>(fr.QuadPart) : 0.0;
    if (!hooks::install("RDR footprints into the collector (FUN_140193060, PrepassEye2Light)", reinterpret_cast<void*>(anchors::addr(Id::Footprints)),
                        &hk_foot, &o_foot))
        return;
    if (!hooks::install("RDR lights into the Light Buffer (FUN_1406e0450, PrepassEye2Light)", reinterpret_cast<void*>(anchors::addr(Id::LightFrontHalf)),
                        &hk_light, &o_light)) {
        log::warn("[e2lb] the lights' hook not installed: off (the footprints' hook passes through)");
        return;
    }
    d3d::add_frame_end_listener(&frame_end);
    if (config::get_int("Stereo", "PrepassEye2LightDepth", 0) == 1) {  // (item 7's S1, test only, off) D2L and its compare
        g_d2l_on = true;
        log::info("[e2lb] [Stereo] PrepassEye2LightDepth on (test only): eye 2's depth after its lights kept (D2L) and compared with pass 2's");
    }
    g_installed = true;
    g_on = true;
    log::info("[e2lb] [Stereo] PrepassEye2Light on (test only): eye 2's footprints and lights made in pass 1 on its own cull view and the private "
              "eye-2 viewport, from its collector and depth, into LB2; eye 1's collector and depth put back; \"prepass e2lb compare N\" compares "
              "LB2 with pass 2's Light Buffer");
}

std::string command(const std::vector<std::string>& t) {
    if (!g_installed.load())
        return "e2lb: not installed (start with [Stereo] PrepassEye2Light=1 and PrepassEye2Collector=1 and its switches, on the analysed RDR.exe)";
    if (t.size() >= 3) {
        const std::string& w = t[2];
        if (w == "kv") return kv();
        if (w == "on" || w == "off") {
            if (w == "on" && g_stood.load()) return "ERROR stood down (" + std::string(g_why) + "): restart the game";
            if (w == "on" && !g_on.load()) g_clear_owed = true;  // (review S3: the first pass 1 appends a clear before any eye-2 step)
            g_on = w == "on";
        } else if (w == "d2lctl" && t.size() >= 4 && (t[3] == "on" || t[3] == "off")) {  // (item 7's S1, the D2L compare's control)
            g_d2l_ctl = t[3] == "on";
        } else if (w == "reset") {
            reset();
        } else if (w == "compare") {
            const int n = t.size() >= 4 ? std::atoi(t[3].c_str()) : 10;
            if (n <= 0 || n > 1000) return "ERROR compare N: 1..1000";
            if (!setup_compare()) return "ERROR the fence, its event or the worker not made";
            const int s = g_cap.load();
            if (s != kIdle && s != kArmed) return "ERROR a capture is in flight (cap=" + std::to_string(s) + ")";
            g_cap_left.store(n);
            g_cap.store(kArmed);
        } else {
            return "ERROR prepass e2lb [on|off|compare N|reset|kv]";
        }
    }
    char b[3200];
    std::snprintf(b, sizeof(b),
                  "e2lb %s: pass 1 (L) calls %llu, eye-2 runs %llu (%.1f us each), footprints (F) calls %llu, runs %llu (%.1f us) (no collector "
                  "run %llu, no vp2 %llu, not the scene viewport %llu, not the render thread %llu, lighting off %llu, no stream %llu, not ready %llu, "
                  "preps %llu, footprints not run %llu) | put back: states %llu, scissor %llu, volumes %llu, rects %llu, stencil ref %llu | eye 1 "
                  "exposed %llu | from OUT1 %llu (refused %llu), IN-F %llu, OUT-F %llu, IN-L %llu, OUT-L %llu, LB state unknown %llu, LB saved %llu "
                  "(not %llu), put back %llu (not %llu), clears %llu (owed skips %llu) | captures: "
                  "started %llu, done %llu, incomplete %llu, left %d (state %d) | compared %llu frames (cut %llu, footprints %llu, interior %llu): "
                  "inputs equal %llu (collector %llu, depth %llu, stencil %llu differ), LB2 bit-exact %llu (with equal inputs %llu; differs with "
                  "equal inputs %llu, of which at eye 2's prior only %llu; most texels %llu, worst %llu); cut frames' LB bit-exact %llu, prior only "
                  "%llu; light counts eye 2 = pass 2 %llu, differ %llu; eyes differ %llu | last "
                  "refusal: %s",
                  g_on.load() ? "on" : g_stood.load() ? "stood down" : "off", u(g_l_calls), u(g_l_runs), us_per(g_l_ticks, g_l_runs), u(g_f_calls),
                  u(g_f_runs), us_per(g_f_ticks, g_f_runs), u(g_no_e2sc), u(g_no_vp2), u(g_not_scene), u(g_not_rt), u(g_lighting_off), u(g_no_stream),
                  u(g_not_ready), u(g_preps), u(g_fp_unrun), u(g_states_restored), u(g_scissor_restored), u(g_vol_restored), u(g_rects_restored),
                  u(g_ref_restored), u(g_eye1_exposed), u(g_from_ok), u(g_from_refused), u(g_inf_ok), u(g_outf_ok), u(g_inl_ok), u(g_outl_ok),
                  u(g_lb_unknown), u(g_lb_saved_n), u(g_lb_unsaved), u(g_lb_put_back), u(g_lb_not_restored), u(g_clears), u(g_clear_owed_skips),
                  u(g_cap_started), u(g_cap_done), u(g_cap_incomplete), g_cap_left.load(), g_cap.load(), u(g_c_frames), u(g_c_cut),
                  u(g_c_fp), u(g_c_interior), u(g_c_in_eq), u(g_c_in_sc_diff), u(g_c_in_d_diff), u(g_c_in_s_diff), u(g_c_out_exact),
                  u(g_c_out_exact_in_eq), u(g_c_out_diff_in_eq), u(g_c_prior_only_in_eq), static_cast<unsigned long long>(g_c_out_texels_max.load()),
                  static_cast<unsigned long long>(g_c_out_worst.load()), u(g_c_cut_out_exact), u(g_c_cut_prior_only), u(g_c_n_equal), u(g_c_n_differ), u(g_c_n_eyes_differ),
                  g_why[0] ? g_why : "none");
    return b;
}

}  // namespace rdrvr::eye2_light

namespace rdrvr::eye2_light {
// ([Stereo] PrepassEye2Images, eye2_images.cpp, test only) usable(): installed, on and not stood down (eye2_collector's
// equivalent); lb2_image(): the playback thread, where ensure() makes and retires LB2: LB2 (null when not made) and its
// generation (the makes so far: it moves at each new LB2) and the thread that plays its steps (0 none yet)
bool usable() { return g_installed.load() && g_on.load() && !g_stood.load(); }
void set_light_tail(LightTail_t fn) { g_tail = fn; }
// ([Stereo] Pass2FrontSkip, item 7's S2, eye2_light.h)
bool d2l_installed() { return g_d2l_on.load(); }
bool capturing() {
    const int s = g_cap.load();
    return s == kRec1 || s == kRec2;
}
bool p2in_marker() {
    if (render_thread() && prepass::stream_marker(&cb_p2in)) return true;
    bump(g_p2in_marker_failed);
    return false;
}
void set_p2in_control(unsigned nocopy, bool stale) {
    g_p2in_nocopy.store(nocopy, std::memory_order_relaxed);
    g_p2in_stale.store(stale, std::memory_order_relaxed);
}
void p2in_reset() {  // (p2in_refused, p2in_marker_failed kept: a stand-down's cause)
    for (Count* c : {&g_p2in_ok, &g_have2_n, &g_p2in_ctl_n, &g_stale_n}) c->store(0);
}
std::string p2in_kv() {
    char b[300];
    std::snprintf(b, sizeof(b), "p2in_ok=%llu p2in_refused=%llu have2_set=%llu p2in_ctl=%llu stale_outl=%llu p2in_marker_failed=%llu nocopy=%u stale=%d",
                  u(g_p2in_ok), u(g_p2in_refused), u(g_have2_n), u(g_p2in_ctl_n), u(g_stale_n), u(g_p2in_marker_failed), g_p2in_nocopy.load(),
                  g_p2in_stale.load() ? 1 : 0);
    return b;
}
ID3D12Resource* lb2_image(uint64_t* gen, unsigned long* tid) {
    if (gen) *gen = g_made.load(std::memory_order_relaxed);
    if (tid) *tid = g_ops_tid;
    return g_img.lb2;
}
// (item 6b) SC2L, as lb2_image (made and retired together by ensure(): one generation)
ID3D12Resource* sc2l_image(uint64_t* gen, unsigned long* tid) {
    if (gen) *gen = g_made.load(std::memory_order_relaxed);
    if (tid) *tid = g_ops_tid;
    return g_img.sc2;
}
// (GOAL-single-pass item 8f, test only: main_instanced's MainRegionE1Compare) D2L, as lb2_image; fresh: have2 (an OUT-L of this pass 1
// wrote it), not stood down, and no step queued (every step before the caller's list call has run); game: the depth it was made for.
// (the review's nit) the images' plain state is read only on their own thread: another caller gets tid, null, not fresh and no game
ID3D12Resource* d2l_image(uint64_t* gen, unsigned long* tid, bool* fresh, ID3D12Resource** game) {
    const DWORD ot = g_ops_tid;
    const bool mine = ot != 0 && GetCurrentThreadId() == ot;
    if (gen) *gen = g_made.load(std::memory_order_relaxed);
    if (tid) *tid = ot;
    if (fresh) *fresh = mine && g_have2 && !g_stood.load() && g_nops == 0;
    if (game) *game = mine ? g_img.game_d : nullptr;
    return mine ? g_img.d2l : nullptr;
}
// (item 8f, its "seed" control) D2, eye 2's depth before its lights (at rest COPY_DEST, made with D2L), as d2l_image
ID3D12Resource* d2_image(uint64_t* gen, unsigned long* tid, ID3D12Resource** game) {
    const DWORD ot = g_ops_tid;
    const bool mine = ot != 0 && GetCurrentThreadId() == ot;
    if (gen) *gen = g_made.load(std::memory_order_relaxed);
    if (tid) *tid = ot;
    if (game) *game = mine ? g_img.game_d : nullptr;
    return mine ? g_img.d2 : nullptr;
}
void set_lights_done(LightsDone_t fn) { g_lights_done.store(fn, std::memory_order_release); }
void set_fresh_sink(FreshSink_t fn) { g_fresh_sink.store(fn, std::memory_order_release); }
bool check_marker(int tag) {
    return tag >= 0 && tag < kCheckTags && g_installed.load() && !g_stood.load() && render_thread() && prepass::stream_marker(kCbCheck[tag]);
}

// ---- (GOAL-single-pass item 6c) the lend (eye2_light.h). The images' thread only (g_ops_tid: where ensure() made them and
// their steps play); the states: at rest COPY_DEST (every step leaves them there: ensure() makes them in it, in_step / out_step /
// from_collector transition and put back within one list), lent PIXEL_SHADER_RESOURCE
bool lend(ID3D12GraphicsCommandList* cl) {
    if (!cl || g_ops_tid == 0 || GetCurrentThreadId() != g_ops_tid) {
        bump(g_lend_refused);
        return false;
    }
    if (g_lent_flag.load(std::memory_order_relaxed)) {
        if (is_lent_list(cl)) return true;
        bump(g_lend_cross);  // (another list still open holds the lend: its Close gives them back)
        return false;
    }
    if (!g_installed.load() || g_stood.load() || !g_img_ready.load() || !g_img.sc2 || !g_img.lb2 || g_nops != 0 ||
        cl->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {  // (g_nops: a step still queued would copy them after the lend)
        bump(g_lend_refused);
        return false;
    }
    g_lent_sc2 = g_img.sc2;
    g_lent_lb2 = g_img.lb2;
    Batch x;
    x.add(g_lent_sc2, kDst, kPsr);
    x.add(g_lent_lb2, kDst, kPsr);
    x.flush(cl);
    g_lent_cl = cl;
    g_lent_tid = GetCurrentThreadId();
    g_give_owed = false;
    g_lent_flag.store(true, std::memory_order_release);  // (the 6c delta review: publishes g_lent_cl / g_lent_tid with it)
    bump(g_lends);
    return true;
}
bool lent_on(const ID3D12GraphicsCommandList* cl) {
    return g_lent_flag.load(std::memory_order_acquire) && is_lent_list(cl);
}
bool give_back(ID3D12GraphicsCommandList* cl) {
    if (!g_lent_flag.load(std::memory_order_acquire)) return true;
    if (!is_lent_list(cl)) return false;
    bump(g_given_close);
    give_back_now(cl);
    return true;
}
void owe_give_back() {
    if (!g_lent_flag.load(std::memory_order_acquire) || GetCurrentThreadId() != g_lent_tid) return;
    if (!g_give_owed) bump(g_owed_n);
    g_give_owed = true;
    prepass::g_eye2_light_pending_flag.store(true, std::memory_order_release);  // (poll at the next list call)
}
void list_closing(ID3D12GraphicsCommandList* cl) {
    if (!g_lent_flag.load(std::memory_order_acquire) || !is_lent_list(cl)) return;
    bump(g_given_list);
    give_back_now(cl);
}
void list_reset(ID3D12GraphicsCommandList* cl) {
    if (!g_lent_flag.load(std::memory_order_acquire) || !is_lent_list(cl)) return;
    // (the lent list's Reset with no Close seen by the hook: never expected. Their state is not known, so it is not guessed:
    // the module stands down and the images are retired, never used again; a later make starts at rest)
    bump(g_lend_reset);
    g_lent_cl = nullptr;
    g_lent_tid = 0;
    g_lent_sc2 = g_lent_lb2 = nullptr;
    g_give_owed = false;
    g_lent_flag.store(false, std::memory_order_relaxed);
    g_have = false;
    g_pb_l_ok = false;
    stand_down("SC2L and LB2 lent on a list reset without its Close (their state not known: retired)");
    g_img_ready = false;
    retire_all();
}
std::string lend_kv() {
    char b[400];
    std::snprintf(b, sizeof(b),
                  "lends=%llu given_back=%llu at_close=%llu owed=%llu at_poll=%llu at_list_close=%llu by_step=%llu cross=%llu cross_step=%llu "
                  "reset_dropped=%llu lend_refused=%llu lent=%d",
                  u(g_lends), u(g_given), u(g_given_close), u(g_owed_n), u(g_given_poll), u(g_given_list), u(g_given_step), u(g_lend_cross),
                  u(g_lend_cross_step), u(g_lend_reset), u(g_lend_refused), g_lent_flag.load() ? 1 : 0);
    return b;
}
bool trace_images(ID3D12GraphicsCommandList* cl, int pass, ID3D12Resource* rb, const uint64_t off[2], uint64_t cap, D3D12_SUBRESOURCE_FOOTPRINT fp[2],
                  const D3D12_BOX& box, bool pred_nulled, const char** why) {
    *why = nullptr;
    if (!cl || !rb || (pass != 1 && pass != 2) || cl->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return (*why = "no list, not a direct list, no readback or not a pass", false);
    if (d3d::thread_predicated() && !pred_nulled) return (*why = "the list is predicated (its predicate not nulled here)", false);
    ID3D12Resource* src[2] = {};
    D3D12_RESOURCE_STATES st[2] = {};
    if (pass == 1) {
        if (!lent_on(cl) || !g_lent_sc2 || !g_lent_lb2) return (*why = "pass 1: SC2L and LB2 not lent on this list", false);
        src[0] = g_lent_sc2;
        src[1] = g_lent_lb2;
        st[0] = st[1] = kPsr;
    } else {
        Targets t;
        if (const char* w = gather(&t, cl)) return (*why = w, false);
        if (!mine(t)) return (*why = "pass 2: the collector is not the images' own", false);
        if (!lb_state(cl, &st[1])) return (*why = "pass 2: the Light Buffer's state not seen in this list", false);
        src[0] = t.sc;
        src[1] = g_lb;
        st[0] = t.ssc;  // (prepass's follow is per thread, not per list: read_pass2's known limit, the same here)
        if (st[0] == D3D12_RESOURCE_STATE_COMMON || st[1] == D3D12_RESOURCE_STATE_COMMON)
            return (*why = "pass 2: an image in COMMON (its promoted state not known)", false);
    }
    ID3D12Device* dev = state::device.load();
    if (!dev) return (*why = "no device", false);
    for (int k = 0; k < 2; ++k) {
        D3D12_RESOURCE_DESC d = src[k]->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.MipLevels != 1 || d.DepthOrArraySize != 1 || d.SampleDesc.Count != 1 ||
            (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) || box.right > d.Width ||
            box.bottom > d.Height || box.left >= box.right || box.top >= box.bottom || box.front != 0 || box.back != 1)
            return (*why = "an image's box does not fit", false);
        d.Width = box.right - box.left;  // (the box's own footprint: the format's pitch, as a copy of that size needs)
        d.Height = box.bottom - box.top;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT pf{};
        UINT64 total = 0;
        dev->GetCopyableFootprints(&d, 0, 1, 0, &pf, nullptr, nullptr, &total);
        if (total == ~0ull || total > cap) return (*why = "an image's box larger than the slot", false);
        fp[k] = pf.Footprint;
    }
    Batch x;
    for (int k = 0; k < 2; ++k) x.add(src[k], st[k], kSrc);
    x.flush(cl);
    for (int k = 0; k < 2; ++k) {
        D3D12_TEXTURE_COPY_LOCATION dst{rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        dst.PlacedFootprint.Offset = off[k];
        dst.PlacedFootprint.Footprint = fp[k];
        D3D12_TEXTURE_COPY_LOCATION s{src[k], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        s.SubresourceIndex = 0;
        d3d::raw_copy_region(cl, &dst, &s, &box);
    }
    for (int k = 0; k < 2; ++k) x.add(src[k], kSrc, st[k]);
    x.flush(cl);
    return true;
}
}  // namespace rdrvr::eye2_light
