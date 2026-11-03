#include "core/pass2_skip.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/anchors.h"
#include "core/config.h"
#include "core/dual_pass.h"
#include "core/eye2_collector.h"
#include "core/eye2_light.h"
#include "core/eye2_velocity.h"
#include "core/fcensus.h"
#include "core/gpu_stages.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/main_instanced.h"
#include "core/prepass_arrays.h"
#include "core/rec_state.h"

#pragma intrinsic(_ReturnAddress)

namespace rdrvr::p2skip {
std::atomic<bool> g_installed_flag{false};
namespace {
using anchors::Id;

// DecalMgr vt+0x70 = FUN_1405e0dc0(mgr) (research\run23\decals.md; .pdata 0x5e0dc0-0x5e0e33): its call in 0x5cbfd0 at
// 0x1405cc3dc (return 0x5cc3df) is pass 2's D; e2sc's own eye-2 call (eye2_collector's decal_call, pass 1) returns elsewhere
using Decal_t = void (*)(void* mgr);
Decal_t o_decal = nullptr;
uintptr_t g_ret_d = 0;
// FadeValue's globals (read from its code at install): its on byte (0x22e1758), its value (0x22e1754: the sun occlusion's
// visible-pixel fraction), the low end (0x22e174c) and the range (0x22e1750) it is mapped from
uintptr_t g_fade_on = 0, g_fade_now = 0, g_fade_start = 0, g_fade_len = 0;
constexpr uintptr_t kPassFade = 0x87;  // the pass object's +0x87 (at the cascade's +0x1f8; FUN_1407059e0 draws only when it is set)

std::atomic<bool> g_on{false}, g_stood{false};
char g_why[200] = "";  // (the stand-down's reason: any thread writes, the status reads; test only)
using Count = std::atomic<uint64_t>;
inline void bump(Count& c, uint64_t n = 1) { c.fetch_add(n, std::memory_order_relaxed); }
inline unsigned long long u(const Count& c) { return static_cast<unsigned long long>(c.load(std::memory_order_relaxed)); }
const char* const kCallName[kCalls] = {"dec", "dir", "fp", "light"};

// "control ...": the positive controls (eye2_light applies them at P2IN and OUT-L)
enum Ctl : int { kCtlNone, kCtlLb, kCtlSc, kCtlD, kCtlAll, kCtlStale, kCtls };
const char* const kCtlName[kCtls] = {"none", "nocopy lb", "nocopy sc", "nocopy d", "nocopy all", "stale"};
std::atomic<int> g_ctl{kCtlNone};

// the render thread's: this frame's pass 1 (pass1_lights), and the latch of the pass 2 that follows it
uint64_t g_p1_seq = ~0ull;  // the walk cut's decision count at pass 1's (L): this frame's pass 2 sees it + 1
bool g_p1_ran = false, g_p1_d2l = false, g_p1_fp = false, g_p1_dec = false;
uint64_t g_latch_seq = ~0ull;      // the pass 2 the latch is for (its decision's sequence)
bool g_skip = false, g_owed = false;  // this pass 2 skips; its P2IN not yet appended

// the decisions (one a pass 2), the calls by kind, the stretches, P2IN's marker
Count g_p2_frames, g_cut_frames, g_skip_frames, g_d_off, g_d_uncut, g_d_stretch, g_d_no_p1, g_d_no_e2lb, g_d_no_d2l, g_d_no_dec, g_d_capture,
    g_d_modules, g_d_fade, g_d_fade_unread;
Count g_calls[kCalls], g_stretches[kCalls], g_refused[kCalls], g_recheck[kCalls], g_ended[kCalls], g_end_failed[kCalls], g_writes[kCalls], g_masked[kCalls];
// (run 25, item7.md 10-11; "prepass p2skip calls dec,dir,fp,light|all|none") which calls a skipped pass 2 stretches (the others record as
// the game's; P2IN still copies, its images being what those calls would have made): the bisection the first cycles needed
std::atomic<uint32_t> g_mask{0xfu};
// mismatch_f: pass 2 called the footprints where pass 1's eye-2 run had none; mismatch_f_rev: the reverse (pass 1's eye-2 run
// drew footprints, pass 2 did not call them). (No decals' counter: the decision needs eye 2's decals drawn, and pass 2 calls
// them in every frame it calls the others)
Count g_mismatch_f, g_mismatch_f_rev, g_p2in_marks, g_p2in_mark_failed, g_exposed2, g_unended, g_d_fade_slot;
bool g_p2_fp_called = false;  // (the render thread) the latched pass 2 called the footprints

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
bool raw(uintptr_t a, void* out, size_t n) {
    if (!a) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void stand_down(const char* why) {
    g_stood = true;
    std::snprintf(g_why, sizeof(g_why), "%s", why);
    if (g_on.exchange(false)) log::error("[p2skip] Pass2FrontSkip stood down: %s", why);
}

bool capture_in_flight() { return eye2_collector::capturing() || eye2_light::capturing() || eye2_velocity::capturing(); }

// The fade branch at 0x1405cc4a4 (item7.md 7, design change 2) [V the disassembly 0x1405cc4a4..0x1405cc4fa]: when its gates
// hold and FadeValue() == 0, 0x5cbfd0 calls 0x5c1220 (CPU only) and the cascade object's (FrontPassOwner +8) vt+0x18, the
// thunk 0x1409e79b0 (mov rcx,[rcx+0x1f8]; mov rax,[rcx]; jmp [rax+0x38]) into the pass object at its +0x1f8 (renderer+0x1d0):
// its vt+0x38 = FUN_1407059e0, which draws stencil-only marks into the depth only when that pass's +0x87 != 0. P2IN would erase
// them (D2L starts from slice 1, before they ran), so such a frame is not skipped. Conservative: the gates before the call
// (0x1422e17a8, [0x142ac50d0] +0x1fa4, the renderer's (RSI) +0x1fa5, 0x142ad1c90's +0x2524e/f) are not read, and a FadeValue
// that is NaN counts as 0. (r88 G1: the first version took the cascade object's vt+0x18 for FUN_1407059e0 and read +0x87 on the
// cascade object; every frame was refused as "not the slot".) The thunk's bytes and the pass's vt+0x38 = FrontPassMarks are
// checked at run time: anything else is "can draw" (-1, counted apart).
// 1: it can draw; 0: it cannot; -1: not read, or not the analysed slot (taken as "can")
constexpr uint8_t kCascadeThunk[14] = {0x48, 0x8b, 0x89, 0xf8, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x01, 0x48, 0xff, 0x60, 0x38};
constexpr uintptr_t kCascadePass = 0x1f8, kPassMarksSlot = 0x38;
int fade_can_draw(bool count = true) {  // (count: decide()'s; item 8d's accessor reads it without)
    uintptr_t owner = 0, cascade = 0, cvt = 0, thunk = 0, pass = 0, pvt = 0, slot = 0;
    uint8_t p87 = 0, on = 0, tb[sizeof(kCascadeThunk)] = {};
    float now = 0, start = 0, len = 0;
    if (!rd(anchors::addr(Id::FrontPassOwner), &owner) || !owner || !rd(owner + 8, &cascade) || !cascade) return -1;
    if (!rd(cascade, &cvt) || !cvt || !rd(cvt + 0x18, &thunk) || !thunk || !raw(thunk, tb, sizeof(tb)) || std::memcmp(tb, kCascadeThunk, sizeof(tb)) != 0 ||
        !rd(cascade + kCascadePass, &pass) || !pass || !rd(pass, &pvt) || !pvt || !rd(pvt + kPassMarksSlot, &slot) ||
        slot != anchors::addr(Id::FrontPassMarks)) {
        if (count) bump(g_d_fade_slot);
        return -1;
    }
    if (!rd(pass + kPassFade, &p87)) return -1;
    if (!p87) return 0;  // (FUN_1407059e0 draws nothing)
    if (!rd(g_fade_on, &on)) return -1;
    if (!on) return 0;  // (FadeValue 1: the branch's calls not made)
    if (!rd(g_fade_now, &now) || !rd(g_fade_start, &start) || !rd(g_fade_len, &len)) return -1;
    const float v = (now - start) / len;  // (FadeValue's own expression)
    return v > 0.0f ? 0 : 1;  // (FadeValue clamps to [0, 1]: 0 exactly when v <= 0)
}

// The pass 2 named by seq: skip its front half? (once, at its first wrapped call)
bool decide(bool cut, uint64_t seq) {
    bump(g_p2_frames);
    if (!g_on.load(std::memory_order_relaxed) || g_stood.load(std::memory_order_relaxed)) {
        bump(g_d_off);
        return false;
    }
    if (!cut) {  // (slice 1 is complete only on a cut frame: p3-textures.md 2(b))
        bump(g_d_uncut);
        return false;
    }
    bump(g_cut_frames);
    if (!rec_state::stretch_ready()) {
        bump(g_d_stretch);
        stand_down("rec_state's stretches stood down (a re-emit refused at a stretch's end)");
        return false;
    }
    Count* no = g_p1_seq + 1 != seq ? &g_d_no_p1  // (this frame's pass 1 (L) not seen: e2lb off, or a frame without its call)
                : !g_p1_ran          ? &g_d_no_e2lb
                : !g_p1_d2l          ? &g_d_no_d2l
                : !g_p1_dec          ? &g_d_no_dec
                : capture_in_flight() ? &g_d_capture  // (those frames run as shadow frames: pass 2's own images are read)
                : !eye2_collector::usable() || !eye2_light::usable() ? &g_d_modules
                                                                    : nullptr;
    if (no) {
        bump(*no);
        return false;
    }
    const int fade = fade_can_draw();
    if (fade != 0) {
        bump(fade < 0 ? g_d_fade_unread : g_d_fade);
        return false;
    }
    bump(g_skip_frames);
    return true;
}

void hk_decal(void* mgr) {
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_ret_d) return o_decal(mgr);
    around(kDecals, [&] { o_decal(mgr); });
}

// (the leaf-hook rule) the target is a .pdata function starting there: safetyhook's trampoline then keeps the unwind
bool has_pdata(uintptr_t f) {
    DWORD64 image = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(static_cast<DWORD64>(f), &image, nullptr);
    return fn && image && image + fn->BeginAddress == f;
}
uintptr_t rip_target(uintptr_t at, const uint8_t* code, int disp_off, int end_off) {
    int32_t d = 0;
    std::memcpy(&d, code + disp_off, 4);
    return at + static_cast<uintptr_t>(end_off) + static_cast<intptr_t>(d);
}
// DecalDrawCall [V 0x1405cc3d2]: 48 8b 0d <disp32: DecalMgr> (mov rcx,[DecalMgr]) | 48 8b 01 (mov rax,[rcx]) | ff 50 70
// (call [rax+0x70]): the return address the detour keys on is its end
const char* check_decal_site() {
    const uintptr_t s = anchors::addr(Id::DecalDrawCall);
    uint8_t c[13] = {};
    const uint8_t mov_rcx[3] = {0x48, 0x8b, 0x0d}, mov_rax[3] = {0x48, 0x8b, 0x01}, call70[3] = {0xff, 0x50, 0x70};
    if (!raw(s, c, sizeof(c)) || std::memcmp(c, mov_rcx, 3) || std::memcmp(c + 7, mov_rax, 3) || std::memcmp(c + 10, call70, 3))
        return "the decals' call site is not mov rcx,[rip]; mov rax,[rcx]; call [rax+0x70]";
    if (rip_target(s, c, 3, 7) != anchors::addr(Id::DecalMgr)) return "the decals' call site does not load DecalMgr";
    g_ret_d = s + sizeof(c);
    return nullptr;
}
// FadeBranchCall [V 0x1405cc4df]: e8 <rel32: FadeValue> | 0f 2e c6 (ucomiss xmm0,xmm6) | 7a 14 | 75 12 | 48 8b ce | e8 <rel32:
// 0x5c1220> | 48 8b 4d 08 (mov rcx,[rbp+8]: the cascade object, rbp = [FrontPassOwner] at 0x1405cc15c) | 48 8b 01 | ff 50 18
const char* check_fade_branch() {
    const uintptr_t s = anchors::addr(Id::FadeBranchCall);
    uint8_t c[30] = {};
    const uint8_t cmp[5] = {0x0f, 0x2e, 0xc6, 0x7a, 0x14}, pass[10] = {0x48, 0x8b, 0x4d, 0x08, 0x48, 0x8b, 0x01, 0xff, 0x50, 0x18};
    if (!raw(s, c, sizeof(c)) || c[0] != 0xe8 || std::memcmp(c + 5, cmp, 5) || c[10] != 0x75 || c[15] != 0xe8 || std::memcmp(c + 20, pass, 10))
        return "the fade branch is not FadeValue's call, its compare and the cascade object's vt+0x18";
    if (rip_target(s, c, 1, 5) != anchors::addr(Id::FadeValue)) return "the fade branch does not call FadeValue";
    return nullptr;
}
// FadeValue [V 0x1405bc930, FUN_1405bc930]: 80 3d <disp32: on> 00 (cmp byte [on],0) | 75 09 | f3 0f 10 05 <disp32> (movss xmm0,
// [1.0]) | c3 | f3 0f 10 05 <disp32: the value, the sun occlusion's visible-pixel fraction> | 0f 57 c9 | f3 0f 5c 05 <disp32: the
// low end> (subss) | f3 0f 5e 05 <disp32: the range> (divss)
const char* read_fade() {
    const uintptr_t f = anchors::addr(Id::FadeValue);
    uint8_t c[45] = {};
    const uint8_t ld[4] = {0xf3, 0x0f, 0x10, 0x05}, sub[4] = {0xf3, 0x0f, 0x5c, 0x05}, dv[4] = {0xf3, 0x0f, 0x5e, 0x05};
    if (!raw(f, c, sizeof(c)) || c[0] != 0x80 || c[1] != 0x3d || c[6] != 0 || c[7] != 0x75 || std::memcmp(c + 9, ld, 4) || c[0x11] != 0xc3 ||
        std::memcmp(c + 0x12, ld, 4) || std::memcmp(c + 0x1d, sub, 4) || std::memcmp(c + 0x25, dv, 4))
        return "FadeValue's code is not as analysed";
    g_fade_on = rip_target(f, c, 2, 7);
    g_fade_now = rip_target(f, c, 0x16, 0x1a);
    g_fade_start = rip_target(f, c, 0x21, 0x25);
    g_fade_len = rip_target(f, c, 0x29, 0x2d);
    return nullptr;
}

std::string kv() {
    char b[2400];
    bool cut = false;  // (a part cut by its buffer: kv_truncated=1 before the sentinel, so the test fails instead of reading 0s)
    int n = std::snprintf(b, sizeof(b),
                          "on=%d stood=%d control=%d p2_frames=%llu cut_frames=%llu skip_frames=%llu d_off=%llu d_uncut=%llu d_stretch=%llu "
                          "d_no_p1=%llu d_no_e2lb=%llu d_no_d2l=%llu d_no_dec=%llu d_capture=%llu d_modules=%llu d_fade=%llu d_fade_unread=%llu "
                          "d_fade_slot=%llu mismatch_f=%llu mismatch_f_rev=%llu p2in_marks=%llu p2in_mark_failed=%llu exposed2=%llu unended=%llu",
                          g_on.load() ? 1 : 0, g_stood.load() ? 1 : 0, g_ctl.load(), u(g_p2_frames), u(g_cut_frames), u(g_skip_frames), u(g_d_off),
                          u(g_d_uncut), u(g_d_stretch), u(g_d_no_p1), u(g_d_no_e2lb), u(g_d_no_d2l), u(g_d_no_dec), u(g_d_capture), u(g_d_modules),
                          u(g_d_fade), u(g_d_fade_unread), u(g_d_fade_slot), u(g_mismatch_f), u(g_mismatch_f_rev), u(g_p2in_marks),
                          u(g_p2in_mark_failed), u(g_exposed2), u(g_unended));
    cut = cut || n < 0 || n >= static_cast<int>(sizeof(b));
    std::string out = b;
    for (int c = 0; c < kCalls; ++c) {  // (per call: calls in pass 2, stretches, refused at begin, not begun on the re-check, ended, failed at the end, writes skipped)
        const char* nm = kCallName[c];
        n = std::snprintf(b, sizeof(b), " calls_%s=%llu stretches_%s=%llu refused_%s=%llu recheck_%s=%llu ended_%s=%llu end_failed_%s=%llu writes_%s=%llu masked_%s=%llu",
                          nm, u(g_calls[c]), nm, u(g_stretches[c]), nm, u(g_refused[c]), nm, u(g_recheck[c]), nm, u(g_ended[c]), nm,
                          u(g_end_failed[c]), nm, u(g_writes[c]), nm, u(g_masked[c]));
        cut = cut || n < 0 || n >= static_cast<int>(sizeof(b));
        out += b;
    }
    out += " calls_mask=" + std::to_string(g_mask.load());
    return out + " " + eye2_light::p2in_kv() + " " + rec_state::stretch_kv() + (cut ? " kv_truncated=1" : "") + " kv_end=1";
}
void reset() {
    for (Count* c : {&g_p2_frames, &g_cut_frames, &g_skip_frames, &g_d_off, &g_d_uncut, &g_d_stretch, &g_d_no_p1, &g_d_no_e2lb, &g_d_no_d2l,
                     &g_d_no_dec, &g_d_capture, &g_d_modules, &g_d_fade, &g_d_fade_unread, &g_d_fade_slot, &g_mismatch_f, &g_mismatch_f_rev,
                     &g_p2in_marks})
        c->store(0);
    for (int c = 0; c < kCalls; ++c)
        for (Count* x : {&g_calls[c], &g_stretches[c], &g_refused[c], &g_recheck[c], &g_ended[c], &g_writes[c], &g_masked[c]}) x->store(0);
    eye2_light::p2in_reset();
    rec_state::stretch_reset();
    // (p2in_mark_failed, exposed2, unended, end_failed: kept, they mark a stand-down)
}
}  // namespace

bool enter(Call c) {
    if (dual_pass::pass() != 2) return false;
    bump(g_calls[c]);
    uint64_t seq = 0;
    const bool cut = prepass::walk_cut_decided(&seq);
    if (seq != g_latch_seq) {  // (this pass 2's first wrapped call: the decision, latched for its other calls)
        if (g_owed) {  // (an earlier skipped pass 2 never reached its lights' call: no P2IN was appended for it)
            g_owed = false;
            bump(g_unended);
            exposed2("a skipped pass 2 ended without its lights' call (no P2IN)");
        }
        g_latch_seq = seq;
        g_skip = g_owed = decide(cut, seq);
        g_p2_fp_called = false;
    }
    if (!g_skip) return false;
    // (expected 0: P2IN overwrites pass 2's footprints with eye 2's collector from pass 1, which had none)
    if (c == kFoot) {
        g_p2_fp_called = true;
        if (!g_p1_fp) bump(g_mismatch_f);
    }
    // (re-checked per call: "off", a stand-down or a capture armed since the latch; the call then records as the game's, and
    // P2IN still copies, which the shadow compare showed equal to pass 2's own)
    if (!g_on.load(std::memory_order_relaxed) || g_stood.load(std::memory_order_relaxed) || capture_in_flight() || !rec_state::stretch_ready()) {
        bump(g_recheck[c]);
        return false;
    }
    if (!(g_mask.load(std::memory_order_relaxed) & (1u << c))) {  // (not this call: it records as the game's)
        bump(g_masked[c]);
        return false;
    }
    if (!rec_state::stretch_begin()) {  // (refused at entry, by reason in rec_state: the call records as the game's)
        bump(g_refused[c]);
        return false;
    }
    bump(g_stretches[c]);
    return true;
}

void leave(Call c, bool began) {
    if (began) {
        uint64_t n = 0;
        if (rec_state::stretch_end(&n)) {
            bump(g_ended[c]);
        } else {
            bump(g_end_failed[c]);
            stand_down("a stretch's re-emit refused at its end (rec_state repaired the context; its stretches stood down)");
        }
        bump(g_writes[c], n);
    }
    if (c != kLight || !g_owed || dual_pass::pass() != 2) return;
    // P2IN's marker: after the lights' stretch (its re-emit included), before eye2_light's save marker (save_mark). The latch
    // cleared with it: no later wrapped call of this pass 2 is stretched without a P2IN after it
    g_owed = false;
    g_skip = false;
    if (g_p1_fp && !g_p2_fp_called) bump(g_mismatch_f_rev);  // (expected 0: SC2L has footprints pass 2 did not draw)
    if (eye2_light::p2in_marker()) {
        bump(g_p2in_marks);
    } else {
        bump(g_p2in_mark_failed);
        exposed2("P2IN's marker not appended after pass 2's lights");
    }
}

// ---- (item 8c) the record-skip probe of pass 2's e1: the render thread only (t_probe_began)
std::atomic<int> g_probe_flag{0};
namespace {
thread_local bool t_probe_began = false;
thread_local bool t_prev_e1 = false;  // (the crash review's MF1) the last probed pass 2's DVL call 1 reached e1: recskip begins only then
thread_local uint32_t t_prev_gen = 0;  // (of this probe setting: g_probe_gen, moved by each "probe" command, so a stale t_prev_e1 never counts)
std::atomic<uint32_t> g_probe_gen{1};
Count g_pr_calls{0}, g_pr_stretches{0}, g_pr_refused{0}, g_pr_ended{0}, g_pr_end_failed{0}, g_pr_late{0}, g_pr_writes{0}, g_pr_r0{0},
    g_pr_r0_refused{0}, g_pr_noe1{0}, g_pr_noe1_gate{0}, g_pr_r0_late{0}, g_pr_r0_late_refused{0}, g_pr_main{0}, g_pr_fprobe{0};
thread_local bool t_probe_e1 = false;  // this pass 2's e1 point reached
void probe_end(bool late) {
    t_probe_began = false;
    uint64_t n = 0;
    if (rec_state::stretch_end(&n)) {
        bump(late ? g_pr_late : g_pr_ended);
    } else {
        bump(g_pr_end_failed);
        log::error("[p2skip] the e1 probe's stretch refused at its %s (rec_state repaired the context): the probe and the stretches stand down",
                   late ? "fallback end (DVL call 1's return)" : "end (ForestDraw mode 0)");
        g_probe_flag = 0;
        stand_down("the probe's stretch refused at its end (rec_state repaired the context; its stretches stood down)");
    }
    bump(g_pr_writes, n);
}
std::string probe_kv() {
    char b[700];
    std::snprintf(b, sizeof(b),
                  "probe=%d calls=%llu stretches=%llu refused=%llu ended=%llu end_failed=%llu late=%llu writes=%llu r0=%llu r0_refused=%llu "
                  "r0_late=%llu r0_late_refused=%llu no_e1=%llu noe1_gate=%llu main_armed=%llu fprobe=%llu stood=%d ",
                  g_probe_flag.load(), u(g_pr_calls), u(g_pr_stretches), u(g_pr_refused), u(g_pr_ended), u(g_pr_end_failed), u(g_pr_late),
                  u(g_pr_writes), u(g_pr_r0), u(g_pr_r0_refused), u(g_pr_r0_late), u(g_pr_r0_late_refused), u(g_pr_noe1), u(g_pr_noe1_gate),
                  u(g_pr_main), u(g_pr_fprobe), g_stood.load() ? 1 : 0);
    return std::string(b) + rec_state::stretch_kv() + " kv_end=1";
}
}  // namespace

void probe_open() {
    t_probe_e1 = false;
    if (dual_pass::pass() != 2 || !probe_on()) return;
    bump(g_pr_calls);
    if (g_probe_flag.load(std::memory_order_relaxed) != 2 || g_stood.load(std::memory_order_relaxed) || !rec_state::stretch_ready()) return;
    if (main_inst::armed()) {  // (the crash review's SF2: the re-emit's binds never inside a main region's remaps)
        bump(g_pr_main);
        return;
    }
    if (fcensus::g_probe_flag.load(std::memory_order_relaxed) != 0) {  // (route F's probe on: never both in one frame)
        bump(g_pr_fprobe);
        return;
    }
    if (!t_prev_e1 || t_prev_gen != g_probe_gen.load(std::memory_order_relaxed)) {  // (MF1: the end must be e1's, the checked point;
                                                                                         // DVL call 1's return is never a planned end)
        bump(g_pr_noe1_gate);
        return;
    }
    if (!rec_state::stretch_begin()) {  // (refused at entry, by reason in rec_state: the call records as the game's)
        bump(g_pr_refused);
        return;
    }
    bump(g_pr_stretches);
    t_probe_began = true;
}
void probe_e1() {
    if (dual_pass::pass() != 2 || t_probe_e1) return;
    t_probe_e1 = true;
    if (t_probe_began) {
        probe_end(false);
    } else if (g_probe_flag.load(std::memory_order_relaxed) == 1 && !g_stood.load(std::memory_order_relaxed) && !main_inst::armed()) {
        bump(rec_state::reemit_now() ? g_pr_r0 : g_pr_r0_refused);  // (R0: the re-emit here with nothing skipped)
    }
}
void probe_close() {
    if (dual_pass::pass() != 2) return;
    t_prev_e1 = t_probe_e1;
    t_prev_gen = g_probe_gen.load(std::memory_order_relaxed);
    if (!t_probe_e1) {
        bump(g_pr_noe1);
        // (MF1) R0 at the fallback point too: what a late end would meet
        if (g_probe_flag.load(std::memory_order_relaxed) == 1 && !g_stood.load(std::memory_order_relaxed) && !main_inst::armed())
            bump(rec_state::reemit_now() ? g_pr_r0_late : g_pr_r0_late_refused);
    }
    if (t_probe_began) probe_end(true);
}

void pass1_lights(bool eye2_ran, bool d2l, bool fp) {
    uint64_t seq = 0;
    prepass::walk_cut_decided(&seq);
    g_p1_seq = seq;
    g_p1_ran = eye2_ran;
    g_p1_d2l = d2l;
    g_p1_fp = fp;
    g_p1_dec = eye2_collector::last_run_decals();  // (this pass 1's e2sc run: eye2_ran says it ran)
}

void exposed2(const char* why) {
    bump(g_exposed2);
    stand_down(why);
}

void install() {
    const int want = config::get_int("Stereo", "Pass2FrontSkip", 0);
    if (want <= 0) return;
    if (want != 1) {
        log::warn("[p2skip] Pass2FrontSkip=%d is not 1: off", want);
        return;
    }
    const char* need = nullptr;
    if (config::get_int("Stereo", "PrepassEye2Collector", 0) != 1 || !eye2_collector::usable())
        need = "[Stereo] PrepassEye2Collector=1 installed (eye 2's directional term in pass 1)";
    else if (config::get_int("Stereo", "PrepassEye2Decals", 0) != 1 || !eye2_collector::decals_installed())
        need = "[Stereo] PrepassEye2Decals=1 installed (eye 2's collector decals in pass 1)";
    else if (config::get_int("Stereo", "PrepassEye2Light", 0) != 1 || !eye2_light::usable())
        need = "[Stereo] PrepassEye2Light=1 installed (eye 2's footprints and lights in pass 1: SC2L, LB2)";
    else if (config::get_int("Stereo", "PrepassEye2LightDepth", 0) != 1 || !eye2_light::d2l_installed())
        need = "[Stereo] PrepassEye2LightDepth=1 installed (D2L, eye 2's depth after its lights)";
    else if (!config::get_bool("Stereo", "RecorderReemit", false) || !rec_state::installed())
        need = "[Stereo] RecorderReemit=1 installed (the re-emit after each stretch)";
    else if (!config::get_bool("Stereo", "PrepassWalkCut", false) || !prepass::cut_tracking() || !dual_pass::masks_hooked())
        need = "the walk cut's hooks ([Stereo] PrepassWalkCut=1 installed, [Compat] DrawMaskInit=1)";
    else if (anchors::stand_down() || !anchors::exe_matches() || anchors::relocated())
        need = "the analysed RDR.exe build";
    if (need) {
        log::warn("[p2skip] Pass2FrontSkip needs %s: off", need);
        return;
    }
    for (Id id : {Id::DecalMgr, Id::DecalDraw, Id::DecalDrawCall, Id::FadeValue, Id::FadeBranchCall, Id::FrontPassOwner, Id::FrontPassMarks})
        if (!anchors::present(id)) {
            log::warn("[p2skip] the anchor %s is absent on this build: off", anchors::name(id));
            return;
        }
    const char* why = check_decal_site();
    if (!why) why = check_fade_branch();
    if (!why) why = read_fade();
    if (!why && !has_pdata(anchors::addr(Id::DecalDraw))) why = "FUN_1405e0dc0 has no .pdata entry starting there (a leaf hook would need a stub)";
    if (why) {
        log::warn("[p2skip] %s: off", why);
        return;
    }
    // (the crash review's SF1) the hook first: inert until g_installed_flag (around() calls through), so a failure here leaves
    // the recorder's vtable unpatched and R0's re-emit as it was
    if (!hooks::install("RDR collector decals (FUN_1405e0dc0, Pass2FrontSkip)", reinterpret_cast<void*>(anchors::addr(Id::DecalDraw)), &hk_decal,
                        &o_decal)) {
        log::warn("[p2skip] the decals' hook not installed: off");
        return;
    }
    if (!rec_state::stretch_install()) return;  // (logged; the decals' hook stays inert)
    g_on = true;
    g_installed_flag = true;
    log::info("[p2skip] [Stereo] Pass2FrontSkip on (test only): on cut frames whose pass 1 made eye 2's front half, pass 2's decals, directional "
              "term, footprints and lights recorded with the recorder's writers skipped (then the re-emit), and eye 2's SC2L, D2L and LB2 copied "
              "into the game's collector, depth and Light Buffer after its lights (P2IN)");
}

int fade_branch() {  // (item 8d: once a pass 1, read only under the SEH helpers; no counter moves)
    if (!g_installed_flag.load(std::memory_order_relaxed)) return -2;
    return fade_can_draw(false);
}

std::string command(const std::vector<std::string>& t) {
    if (!installed())
        return "p2skip: not installed (start with [Stereo] Pass2FrontSkip=1, PrepassEye2Collector=1, PrepassEye2Decals=1, PrepassEye2Light=1, "
               "PrepassEye2LightDepth=1, RecorderReemit=1 and the walk cut's switches, on the analysed RDR.exe)";
    if (t.size() >= 3) {
        const std::string& w = t[2];
        if (w == "kv") return kv();
        if (w == "probe") {  // (item 8c, test only; the simulator only: eye 2 is wrong under recskip)
            if (t.size() == 4 && (t[3] == "off" || t[3] == "r0" || t[3] == "recskip")) {
                if (t[3] != "off" && g_stood.load()) return "ERROR stood down (" + std::string(g_why) + "): restart the game";
                if (t[3] != "off" && main_inst::armed()) return "ERROR refused: MainInstanced is armed (the re-emit never inside a main region)";
                if (t[3] != "off" && fcensus::g_probe_flag.load() != 0) return "ERROR refused: route F's probe is on (prepass fcensus probe off first)";
                if (t[3] != "off" && !gpu_stages::walk_ret()) return "ERROR refused: DVL call 1 not found ([Stereo] PrepassGpuStages=1 and MainDvlCall)";
                g_probe_gen.fetch_add(1);  // (the next probed pass 2 only records whether e1 is reached: recskip from the one after)
                g_probe_flag = t[3] == "off" ? 0 : t[3] == "r0" ? 1 : 2;
                log::info("[p2skip] probe %s (test only)", t[3].c_str());
            } else if (t.size() == 4 && t[3] == "reset") {
                for (Count* c : {&g_pr_calls, &g_pr_stretches, &g_pr_refused, &g_pr_ended, &g_pr_late, &g_pr_writes, &g_pr_r0, &g_pr_r0_refused, &g_pr_noe1,
                                 &g_pr_noe1_gate, &g_pr_r0_late, &g_pr_r0_late_refused, &g_pr_main, &g_pr_fprobe})
                    c->store(0);  // (end_failed kept: it marks a stand-down)
                rec_state::stretch_reset();
            } else if (t.size() != 3) {
                return "ERROR prepass p2skip probe [off|r0|recskip|reset]";
            }
            return probe_kv();
        }
        if (w == "on" || w == "off") {
            if (w == "on" && g_stood.load()) return "ERROR stood down (" + std::string(g_why) + "): restart the game";
            g_on = w == "on";  // (from the next pass 2's decision; a latched pass 2 still gets its P2IN)
        } else if (w == "reset") {
            reset();
        } else if (w == "control") {  // (the positive controls, test only: each must make eye 2 differ)
            std::string what;
            for (size_t i = 3; i < t.size(); ++i) what += (i > 3 ? " " : "") + t[i];
            int k = -1;
            for (int i = 0; i < kCtls; ++i)
                if (what == kCtlName[i]) k = i;
            if (k < 0) return "ERROR prepass p2skip control none|nocopy lb|nocopy sc|nocopy d|nocopy all|stale";
            const unsigned nocopy = k == kCtlLb ? kNoCopyLb : k == kCtlSc ? kNoCopySc : k == kCtlD ? kNoCopyD : k == kCtlAll ? kNoCopyLb | kNoCopySc | kNoCopyD : 0u;
            eye2_light::set_p2in_control(nocopy, k == kCtlStale);
            g_ctl = k;
            log::info("[p2skip] control %s", kCtlName[k]);
        } else if (w == "calls" && t.size() >= 4) {  // (the bisection: which calls are stretched)
            uint32_t m = 0;
            if (t[3] == "all") m = 0xfu;
            else if (t[3] != "none") {
                size_t a = 0;
                while (a <= t[3].size()) {
                    const size_t e = t[3].find(',', a) == std::string::npos ? t[3].size() : t[3].find(',', a);
                    const std::string one = t[3].substr(a, e - a);
                    int k = -1;
                    for (int i = 0; i < kCalls; ++i)
                        if (one == kCallName[i]) k = i;
                    if (k < 0) return "ERROR prepass p2skip calls dec,dir,fp,light|all|none";
                    m |= 1u << k;
                    a = e + 1;
                }
            }
            g_mask = m;
            log::info("[p2skip] calls mask %#x", m);
        } else {
            return "ERROR prepass p2skip [on|off|kv|reset|calls dec,dir,fp,light|all|none|control none|nocopy lb|sc|d|all|stale]";
        }
    }
    uint64_t st = 0, ref = 0, re = 0, fail = 0;
    for (int c = 0; c < kCalls; ++c) st += u(g_stretches[c]), ref += u(g_refused[c]), re += u(g_recheck[c]), fail += u(g_end_failed[c]);
    char b[1400];
    std::snprintf(b, sizeof(b),
                  "p2skip %s%s%s: pass 2 frames %llu, cut %llu, skipped %llu (not: off %llu, uncut %llu, no pass-1 lights %llu, eye 2's lights "
                  "not run %llu, no D2L %llu, no eye-2 decals %llu, a capture %llu, modules off %llu, the fade branch %llu (not read %llu)) | "
                  "stretches %llu (dec %llu, dir %llu, fp %llu, light %llu), refused %llu, re-check %llu, failed at the end %llu | P2IN marks %llu "
                  "(failed %llu), eye 2 exposed %llu, unended %llu | mismatches: footprints in pass 2 only %llu, in pass 1 only %llu | last: %s",
                  g_on.load() ? "on" : g_stood.load() ? "stood down" : "off", g_ctl.load() ? ", control " : "", g_ctl.load() ? kCtlName[g_ctl.load()] : "",
                  u(g_p2_frames), u(g_cut_frames), u(g_skip_frames), u(g_d_off), u(g_d_uncut), u(g_d_no_p1), u(g_d_no_e2lb), u(g_d_no_d2l),
                  u(g_d_no_dec), u(g_d_capture), u(g_d_modules), u(g_d_fade), u(g_d_fade_unread), static_cast<unsigned long long>(st),
                  u(g_stretches[kDecals]), u(g_stretches[kDir]), u(g_stretches[kFoot]), u(g_stretches[kLight]), static_cast<unsigned long long>(ref),
                  static_cast<unsigned long long>(re), static_cast<unsigned long long>(fail), u(g_p2in_marks), u(g_p2in_mark_failed), u(g_exposed2),
                  u(g_unended), u(g_mismatch_f), u(g_mismatch_f_rev), g_why[0] ? g_why : "none");
    return b;
}
}  // namespace rdrvr::p2skip
