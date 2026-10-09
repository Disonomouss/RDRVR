#include "core/dlss.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

#include "core/anchors.h"
#include "core/config.h"
#include "core/dual_pass.h"
#include "core/eye_shape.h"
#include "core/hooks.h"
#include "core/log.h"

namespace rdrvr::dlss {
namespace {

using anchors::Id;

// research\run6\dlss.md 2.3-2.6 (verified again in Ghidra, 2026-10-07)
constexpr size_t kDlssObject = 0x530;      // PostFx +0x530: the DLSS object
constexpr size_t kViewport = 0x10;         // DLSS +0x10..+0x37: its sl::ViewportHandle (value at +0x20 of the handle)
constexpr size_t kViewportSize = 0x28;
constexpr size_t kViewportValue = 0x20;
constexpr size_t kOptionsDirty = 0xe8;     // DLSS +0xe8: options to be sent (FUN_140fd0dc0 sends them, then clears it)
constexpr size_t kConstsSize = 0x1c8;      // the sl::Constants static 0x142aac640..0x142aac807
constexpr size_t kConstsReset = 0x1bf;     // its reset (sl::Boolean, a byte)
constexpr size_t kConstsJitter = 0x160;    // its jitterOffset (float2, render pixels; = the static 0x142aac7a0)
constexpr size_t kConstsMvecScale = 0x168;  // its mvecScale (float2: 1/render size, the motion vectors in render pixels)

using Append_t = void (*)(void* recorder, void* fn, const void* data, uint32_t size);
using Callback_t = void (*)(void* data);
using SetTag_t = int (*)(const void* vp, const void* tags, uint32_t n, void* cmd);
using Evaluate_t = int (*)(uint32_t feature, const void* token, const void** inputs, uint32_t n, void* cmd);
using SetConsts_t = int (*)(const void* consts, const void* token, const void* vp);
using Free_t = int (*)(uint32_t feature, const void* vp);
using SetOptions_t = int (*)(const void* vp, const void* options);

Append_t o_append = nullptr;
bool t_shape_recorded = false;  // the render thread: the last run's marker was a shaped one (an off marker follows once)
std::atomic<bool> g_cfg{false};      // [Render] DlssPerEye
std::atomic<bool> g_ready{false};    // the slots swapped and viewport 1 made
std::atomic<bool> g_killed{false};
std::atomic<bool> g_need_reset{true};  // viewport 1's history is stale (first use, or mono frames since its last run)
std::atomic<bool> g_vp1_used{false};
alignas(16) uint8_t g_vp1[kViewportSize];  // the game's handle with value 1
uintptr_t g_cb_consts = 0, g_cb_eval = 0, g_game_vp_static = 0;
SetTag_t o_set_tag = nullptr;
Evaluate_t o_evaluate = nullptr;
SetConsts_t o_set_consts = nullptr;
Free_t o_free = nullptr;
SetOptions_t o_set_options = nullptr;
thread_local bool t_vp1 = false;  // the playback thread is inside the mod's evaluate
std::atomic<uint64_t> g_consts_swaps{0}, g_eval_swaps{0}, g_consts_ok{0}, g_eval_ok{0}, g_tags_ok{0}, g_options_vp1{0},
    g_resets{0}, g_frees{0}, g_mono_runs{0}, g_dup_consts{0}, g_soft[3]{}, g_soft_run[4]{};
std::atomic<int> g_last_error{0};
char g_why[160] = "";

// The constants check (the playback thread, in its order: consts(1), eval(1), consts(0), eval(0) each frame)
struct Check {
    float j1[2] = {0, 0};          // the first eye's jitter (the mod's copy, record time)
    const void* t1 = nullptr;      // its token
    bool have1 = false;            // a first-eye constants since the last second-eye one
    const void* tc[2] = {};        // the token each viewport's constants carried (0, 1)
};
Check g_chk;
std::atomic<uint64_t> g_pairs{0}, g_jit_eq{0}, g_jit_ne{0}, g_tok_eq{0}, g_tok_ne{0}, g_vp0_alone{0}, g_eval_tok_ne[2];
std::atomic<float> g_jit_maxd{0};
std::atomic<uint64_t> g_jit_changed{0}, g_jit_seen{0};  // the first eye's jitter against the previous frame's (it must vary)
float g_jit_prev[2] = {0, 0};
std::atomic<uint64_t> g_reset_seen[2];
float g_last_j0[2] = {0, 0};  // the game's latest constants' jitter (the playback thread)
// Test aid ("dlss mvscale <k>", 1 = off): both eyes' mvecScale times k, the upscaler told the motion is k times what it
// is: the positive control of tools/dlss_align.py (k < 1 must make the output trail its input)
std::atomic<float> g_mv_scale{1.0f};
std::atomic<bool> g_have_j0{false};  // DLSS constants with reset set: [0] the game's (second eye, mono), [1] the first eye's
float g_jit_last_d[2] = {0, 0};  // the last mismatch's (second - first), for the log

struct ConstsPayload {
    alignas(16) uint8_t consts[kConstsSize];
    const void* token;
};

// [XR] EyeShapeDlss (run 9 item 5): each run's shape as recorded (the render thread), set on the playback thread by a
// marker callback appended before that run's constants: the constants' mvecScale and the tags' output extent follow it
struct ShapePayload {
    uint32_t on, rw, rh, ew, eh;
};
ShapePayload t_shape{};  // the playback thread's current run (only the playback thread reads and writes it)
constexpr size_t kOptsOutW = 0x24, kOptsOutH = 0x28;  // sl::DLSSOptions outputWidth/Height (research\run9\eyeshape-dlss.md)
std::atomic<bool> g_opt_shaped{false};                 // the options last sent carried the eye's output size
std::atomic<uint32_t> g_opt_w{0}, g_opt_h{0};
std::atomic<uint64_t> g_shape_marks{0}, g_shape_consts{0}, g_shape_tags{0}, g_shape_opts{0}, g_shape_refused{0}, g_shape_size_skips{0};
std::atomic<int> g_shape_mv{1};  // the test aid "dlss shapemv game|eye": 1 = 1/(rw, rh) (the shaped render size), 0 = the game's
void mod_shape_mark(void* data) {
    if (const auto* p = static_cast<const ShapePayload*>(data)) t_shape = *p;
    if (g_killed.load(std::memory_order_relaxed)) t_shape.on = 0;  // per-eye DLSS off: the game's own constants and tags
    g_shape_marks.fetch_add(1, std::memory_order_relaxed);
}
// the constants' mvecScale for the shaped run: 1 / the shaped render size
void shape_consts(uint8_t* c) {
    if (!t_shape.on || g_killed.load(std::memory_order_relaxed) || !g_shape_mv.load(std::memory_order_relaxed) || !t_shape.rw || !t_shape.rh) return;
    const float m[2] = {1.0f / static_cast<float>(t_shape.rw), 1.0f / static_cast<float>(t_shape.rh)};
    std::memcpy(c + kConstsMvecScale, m, sizeof(m));
    g_shape_consts.fetch_add(1, std::memory_order_relaxed);
}

std::atomic<bool> g_opt_pending{false};  // [XR] EyeShapeDlss: the options to be sent again (set by any thread, acted on by the render thread)
void kill(const char* what, int r) {
    if (g_killed.exchange(true)) return;
    g_opt_pending = true;  // the options sent again at the next run (the shape's output size, then the game's W x H)
    g_last_error = r;
    std::snprintf(g_why, sizeof(g_why), "%s returned %d", what, r);
    log::error("[dlss] per-eye DLSS off: %s (back to one viewport for both eyes)", g_why);
}

// A viewport-1 call's result (run 8 item 6). Fatal: every result but these three, e.g. 18 missing resource state, 19
// invalid integration, 20 missing input, 21/23 not initialised, 22 compute failed, 25 invalid parameter, 28 invalid
// API, 31-37 the feature missing or broken, 38 invalid state. Counted (one frame's, the next frame's call stands on its
// own): 26 missing constants (a hitch's frame), 27 duplicated constants (Streamline keeps the newer set), 39 the
// out-of-VRAM warning; 60 in a row still fall back (a fault that stays). Returns true when the call went through.
// call: 0 the constants, 1 the tags, 2 the evaluate, 3 the options (each its own run of soft results)
bool vp1_result(const char* what, int r, int call) {
    if (r == 0) {
        g_soft_run[call].store(0, std::memory_order_relaxed);
        return true;
    }
    const int k = r == 26 ? 0 : r == 27 ? 1 : r == 39 ? 2 : -1;
    if (k < 0) {
        kill(what, r);
        return false;
    }
    g_soft[k].fetch_add(1, std::memory_order_relaxed);
    if (g_soft_run[call].fetch_add(1, std::memory_order_relaxed) + 1 >= 60) kill(what, r);
    return false;
}

// ---- the mod's callbacks (the playback thread, in the order the first-eye run recorded them)
void mod_consts1(void* data) {
    auto* p = static_cast<ConstsPayload*>(data);
    if (!p || !o_set_consts || g_killed.load(std::memory_order_relaxed)) return;
    // the frame token as the game's own callback reads it: at playback, after this frame's BeginFrame set it (the
    // record-time copy was the frame before's when a hitch let the render thread run ahead: duplicated constants)
    if (const void* live = *reinterpret_cast<const void* const*>(anchors::addr(Id::DlssTokenStatic))) p->token = live;
    std::memcpy(g_chk.j1, p->consts + kConstsJitter, sizeof(g_chk.j1));
    if (g_jit_seen.fetch_add(1, std::memory_order_relaxed) && (g_chk.j1[0] != g_jit_prev[0] || g_chk.j1[1] != g_jit_prev[1]))
        g_jit_changed.fetch_add(1, std::memory_order_relaxed);
    g_jit_prev[0] = g_chk.j1[0];
    g_jit_prev[1] = g_chk.j1[1];
    g_chk.t1 = p->token;
    g_chk.tc[1] = p->token;
    if (p->consts[kConstsReset]) g_reset_seen[1].fetch_add(1, std::memory_order_relaxed);
    g_chk.have1 = true;
    shape_consts(p->consts);  // [XR] EyeShapeDlss
    if (const float k = g_mv_scale.load(std::memory_order_relaxed); k != 1.0f) {
        float m[2];
        std::memcpy(m, p->consts + kConstsMvecScale, sizeof(m));
        m[0] *= k;
        m[1] *= k;
        std::memcpy(p->consts + kConstsMvecScale, m, sizeof(m));
    }
    const int r = o_set_consts(p->consts, p->token, g_vp1);
    if (r == 27) g_dup_consts.fetch_add(1, std::memory_order_relaxed);  // duplicated: Streamline keeps the newer set
    if (vp1_result("slSetConstants(viewport 1)", r, 0)) g_consts_ok.fetch_add(1, std::memory_order_relaxed);
}

void mod_eval1(void* data) {
    if (g_killed.load(std::memory_order_relaxed)) {
        reinterpret_cast<Callback_t>(g_cb_eval)(data);  // the game's evaluate on viewport 0, as without per-eye
        return;
    }
    t_vp1 = true;
    reinterpret_cast<Callback_t>(g_cb_eval)(data);  // FUN_140fcf6b0: slSetTag and slEvaluateFeature, taken below
    t_vp1 = false;
    g_vp1_used = true;
}

// ---- slSetConstants (the game's own calls: viewport 0, the second eye's run of a double frame and mono)
int hk_set_consts(const void* consts, const void* token, const void* vp) {
    if (consts && reinterpret_cast<uintptr_t>(vp) != reinterpret_cast<uintptr_t>(g_vp1)) {
        g_chk.tc[0] = token;
        std::memcpy(g_last_j0, static_cast<const char*>(consts) + kConstsJitter, sizeof(g_last_j0));
        g_have_j0.store(true, std::memory_order_release);
        if (static_cast<const char*>(consts)[kConstsReset]) g_reset_seen[0].fetch_add(1, std::memory_order_relaxed);
        if (g_chk.have1) {  // this frame's second eye: its jitter and token against the first eye's
            g_chk.have1 = false;
            float j0[2];
            std::memcpy(j0, static_cast<const char*>(consts) + kConstsJitter, sizeof(j0));
            g_pairs.fetch_add(1, std::memory_order_relaxed);
            const float dx = j0[0] - g_chk.j1[0], dy = j0[1] - g_chk.j1[1];
            if (dx == 0.0f && dy == 0.0f) {
                g_jit_eq.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_jit_ne.fetch_add(1, std::memory_order_relaxed);
                const float d = std::sqrt(dx * dx + dy * dy);
                if (d > g_jit_maxd.load(std::memory_order_relaxed)) g_jit_maxd.store(d, std::memory_order_relaxed);
                g_jit_last_d[0] = dx;
                g_jit_last_d[1] = dy;
            }
            (token == g_chk.t1 ? g_tok_eq : g_tok_ne).fetch_add(1, std::memory_order_relaxed);
            const uint64_t n = g_pairs.load(std::memory_order_relaxed);
            if (n % 600 == 0)
                log::info("[dlss] constants check, %llu frames: the second eye's jitter = the first's in %llu, differs in %llu (max %.3f px, "
                          "last %+.3f %+.3f), tokens equal %llu differ %llu | evaluate tokens off their constants': first %llu second %llu | the "
                          "first eye's jitter changed from the frame before in %llu of %llu (now %+.3f %+.3f) | resets in the constants: first eye %llu, "
                          "second eye and mono %llu",
                          static_cast<unsigned long long>(n), static_cast<unsigned long long>(g_jit_eq.load()),
                          static_cast<unsigned long long>(g_jit_ne.load()), g_jit_maxd.load(), g_jit_last_d[0], g_jit_last_d[1],
                          static_cast<unsigned long long>(g_tok_eq.load()), static_cast<unsigned long long>(g_tok_ne.load()),
                          static_cast<unsigned long long>(g_eval_tok_ne[1].load()), static_cast<unsigned long long>(g_eval_tok_ne[0].load()),
                          static_cast<unsigned long long>(g_jit_changed.load()), static_cast<unsigned long long>(g_jit_seen.load()),
                          g_chk.j1[0], g_chk.j1[1], static_cast<unsigned long long>(g_reset_seen[1].load()),
                          static_cast<unsigned long long>(g_reset_seen[0].load()));
        } else {
            g_vp0_alone.fetch_add(1, std::memory_order_relaxed);  // mono, or a second eye with no first-eye constants
        }
        if (t_shape.on && !g_killed.load(std::memory_order_relaxed) && g_shape_mv.load(std::memory_order_relaxed)) {  // [XR] EyeShapeDlss: the shaped run's mvecScale (a copy)
            alignas(16) uint8_t c[kConstsSize];
            std::memcpy(c, consts, kConstsSize);
            shape_consts(c);
            if (const float k = g_mv_scale.load(std::memory_order_relaxed); k != 1.0f) {
                float m[2];
                std::memcpy(m, c + kConstsMvecScale, sizeof(m));
                m[0] *= k;
                m[1] *= k;
                std::memcpy(c + kConstsMvecScale, m, sizeof(m));
            }
            return o_set_consts(c, token, vp);
        }
        if (const float k = g_mv_scale.load(std::memory_order_relaxed); k != 1.0f) {  // Streamline copies the constants
            alignas(16) uint8_t c[kConstsSize];
            std::memcpy(c, consts, kConstsSize);
            float m[2];
            std::memcpy(m, c + kConstsMvecScale, sizeof(m));
            m[0] *= k;
            m[1] *= k;
            std::memcpy(c + kConstsMvecScale, m, sizeof(m));
            return o_set_consts(c, token, vp);
        }
    }
    return o_set_consts(consts, token, vp);
}

// run 9 item 5 (step 0, read-only): the tags as the game passes them, once per viewport: each 0x40-byte sl::ResourceTag
// (the type +0x28, the lifecycle +0x2c, the extent {top, left, width, height} +0x30) and its sl::Resource (+0x20: the
// type +0x20, the native pointer +0x28, the width/height +0x44/+0x48, the format +0x4c), every read guarded
bool raw_copy(const void* p, void* out, size_t n) {
    __try {
        std::memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
std::atomic<bool> g_tags_logged[2][2] = {};  // [viewport][the eye shape's run: the tags as the shape passes them]
void log_tags(int v, const void* tags, uint32_t n, int shaped) {
    if (!tags || n == 0 || n > 8 || g_tags_logged[v][shaped].exchange(true)) return;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t t[0x40];
        if (!raw_copy(static_cast<const uint8_t*>(tags) + i * 0x40, t, sizeof(t))) {
            log::info("[dlss] tags (viewport %d%s) %u: unreadable", v, shaped ? ", the eye shape" : "", i);
            return;
        }
        const void* res = *reinterpret_cast<const void* const*>(t + 0x20);
        uint32_t type = 0, life = 0, ext[4] = {};
        std::memcpy(&type, t + 0x28, 4);
        std::memcpy(&life, t + 0x2c, 4);
        std::memcpy(ext, t + 0x30, sizeof(ext));
        uint8_t r[0x50] = {};
        const bool rok = res && raw_copy(res, r, sizeof(r));
        uint32_t rtype = 0, rw = 0, rh = 0, rfmt = 0;
        const void* native = nullptr;
        if (rok) {
            std::memcpy(&rtype, r + 0x20, 4);
            std::memcpy(&native, r + 0x28, 8);
            std::memcpy(&rw, r + 0x44, 4);
            std::memcpy(&rh, r + 0x48, 4);
            std::memcpy(&rfmt, r + 0x4c, 4);
        }
        log::info("[dlss] tags (viewport %d%s) %u of %u: type %u lifecycle %u extent {top %u left %u width %u height %u} | resource %p%s: type %u "
                  "native %p %ux%u format %u",
                  v, shaped ? ", the eye shape" : "", i, n, type, life, ext[0], ext[1], ext[2], ext[3], res, rok ? "" : " (unreadable)", rtype, native, rw, rh, rfmt);
    }
}

// ---- the Streamline slots (H3, H5)
int hk_set_tag(const void* vp, const void* tags, uint32_t n, void* cmd) {
    // [XR] EyeShapeDlss: the shaped run's tags, a copy: the output's (type 4) extent at the eye, checked first (the
    // layout as step 0 read it: type +0x28, lifecycle +0x2c, extent +0x30; the input extents follow their textures)
    alignas(16) uint8_t shaped[5 * 0x40];
    if (t_shape.on && !g_killed.load(std::memory_order_relaxed) && tags && n == 5 && raw_copy(tags, shaped, sizeof(shaped))) {
        bool ok = true, sizes = true;
        int out = -1;
        for (uint32_t i = 0; i < 5 && ok; ++i) {
            uint8_t* t = shaped + i * 0x40;
            uint32_t type = 0, life = 0, ext[4] = {};
            std::memcpy(&type, t + 0x28, 4);
            std::memcpy(&life, t + 0x2c, 4);
            std::memcpy(ext, t + 0x30, sizeof(ext));
            ok = (type == 0 || type == 1 || type == 3 || type == 4 || type == 29) && life == 1 && ext[0] == 0 && ext[1] == 0;
            if (type == 4) {
                ok = ok && out < 0;
                sizes = sizes && t_shape.ew <= ext[2] && t_shape.eh <= ext[3];
                out = static_cast<int>(i);
            } else {
                sizes = sizes && ext[2] <= t_shape.rw + 1 && ext[3] <= t_shape.rh + 1;  // the inputs at the shaped render size
            }
        }
        if (ok && out >= 0 && !sizes) {  // the eye size changed between the record and this playback: the game's tags, this once
            g_shape_size_skips.fetch_add(1, std::memory_order_relaxed);
        } else if (ok && out >= 0) {
            uint32_t e[4] = {0, 0, t_shape.ew, t_shape.eh};
            std::memcpy(shaped + out * 0x40 + 0x30, e, sizeof(e));
            tags = shaped;
            g_shape_tags.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_shape_refused.fetch_add(1, std::memory_order_relaxed);
            log::limited("dlss.shape", 4, "[dlss] the eye shape's tags refused (a field not as expected): the game's tags passed");
            eye_shape::dlss_stop("the DLSS tags were not as expected for the eye shape");
        }
    }
    log_tags(t_vp1 ? 1 : 0, tags, n, tags == shaped ? 1 : 0);  // run 9 item 5: as passed (the shape's copy when shaped)
    if (t_vp1 && reinterpret_cast<uintptr_t>(vp) == g_game_vp_static) {
        const int r = o_set_tag(g_vp1, tags, n, cmd);
        if (vp1_result("slSetTag(viewport 1)", r, 1)) g_tags_ok.fetch_add(1, std::memory_order_relaxed);
        return r;
    }
    return o_set_tag(vp, tags, n, cmd);
}

int hk_evaluate(uint32_t feature, const void* token, const void** inputs, uint32_t n, void* cmd) {
    if (feature == 0) {
        const int v = t_vp1 ? 1 : 0;  // this evaluate's token against its viewport's constants
        if (g_chk.tc[v] && token != g_chk.tc[v]) g_eval_tok_ne[v].fetch_add(1, std::memory_order_relaxed);
    }
    if (t_vp1 && feature == 0 && inputs && n >= 1 && n <= 8) {
        const void* in[8];
        for (uint32_t i = 0; i < n; ++i) in[i] = reinterpret_cast<uintptr_t>(inputs[i]) == g_game_vp_static ? g_vp1 : inputs[i];
        const int r = o_evaluate(feature, token, in, n, cmd);
        if (vp1_result("slEvaluateFeature(viewport 1)", r, 2)) g_eval_ok.fetch_add(1, std::memory_order_relaxed);
        return r;
    }
    return o_evaluate(feature, token, inputs, n, cmd);
}

int hk_free(uint32_t feature, const void* vp) {
    if (feature == 0 && vp != g_vp1 && g_vp1_used.exchange(false)) {
        const int r = o_free(0, g_vp1);
        g_frees.fetch_add(1, std::memory_order_relaxed);
        log::info("[dlss] viewport 1 freed before the game's (%d)", r);
    }
    return o_free(feature, vp);
}

// ---- slDLSSSetOptions (H4: the render thread, synchronous)
int hk_set_options(const void* vp, const void* options) {
    // [XR] EyeShapeDlss: with the shape applied, the output size is the eye's for both viewports (the game's options
    // object written for the calls and put back: the render thread, synchronous)
    uint32_t rw = 0, rh = 0, ew = 0, eh = 0, ow = 0, oh = 0;
    char* o = static_cast<char*>(const_cast<void*>(options));
    const bool want = o && eye_shape::dlss_shape(&rw, &rh, &ew, &eh);
    const bool shape = want && raw_copy(o + kOptsOutW, &ow, 4) && raw_copy(o + kOptsOutH, &oh, 4) && ew <= ow && eh <= oh;
    if (want && !shape) eye_shape::dlss_stop("the DLSS options' output size could not take the eye's");
    if (shape) {
        std::memcpy(o + kOptsOutW, &ew, 4);
        std::memcpy(o + kOptsOutH, &eh, 4);
        g_shape_opts.fetch_add(1, std::memory_order_relaxed);
    }
    const int r = o_set_options(vp, options);
    if (!g_killed.load(std::memory_order_relaxed) && reinterpret_cast<uintptr_t>(vp) != reinterpret_cast<uintptr_t>(g_vp1)) {
        const int r1 = o_set_options(g_vp1, options);
        if (vp1_result("slDLSSSetOptions(viewport 1)", r1, 3)) g_options_vp1.fetch_add(1, std::memory_order_relaxed);
    }
    uint32_t sw = ow, sh = oh;
    if (shape) {
        std::memcpy(o + kOptsOutW, &ow, 4);
        std::memcpy(o + kOptsOutH, &oh, 4);
        sw = ew;
        sh = eh;
    } else if (o) {
        raw_copy(o + kOptsOutW, &sw, 4);
        raw_copy(o + kOptsOutH, &sh, 4);
    }
    g_opt_shaped = shape;
    g_opt_w = sw;
    g_opt_h = sh;
    log::limited("dlss.opts", 200, "[dlss] options sent (viewport %s): the output %ux%u%s, result %d",
                 reinterpret_cast<uintptr_t>(vp) == reinterpret_cast<uintptr_t>(g_vp1) ? "1" : "0", sw, sh, shape ? " (the eye shape)" : "", r);
    return r;
}

// a slot's value points into this module's image
// DLSS +0xe8: the options sent again at the next post run's AA slot (before its evaluate)
void request_options() {
    char* pfx = *reinterpret_cast<char**>(anchors::addr(Id::PostFxSingleton));
    if (char* d = pfx ? *reinterpret_cast<char**>(pfx + kDlssObject) : nullptr) d[kOptionsDirty] = 1;
}

bool in_module(const void* p, HMODULE m) {
    MEMORY_BASIC_INFORMATION mbi{};
    return p && m && VirtualQuery(p, &mbi, sizeof(mbi)) && mbi.AllocationBase == m;
}

template <typename Fn>
bool swap_slot(uintptr_t slot, Fn mine, Fn* orig) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(slot), 8, PAGE_READWRITE, &old)) return false;
    // the original first: the playback thread may call through the slot the moment it changes
    *orig = *reinterpret_cast<Fn*>(slot);
    MemoryBarrier();
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot), reinterpret_cast<void*>(mine));
    DWORD tmp = 0;
    VirtualProtect(reinterpret_cast<void*>(slot), 8, old, &tmp);
    return true;
}

// The render thread, at the first-eye run's first DLSS callback: the slots swapped and viewport 1 made, once each is
// ready (sl.interposer loaded and its functions resolved into the game's slots, the options function fetched)
bool ensure() {
    if (g_ready.load(std::memory_order_relaxed)) return true;
    static bool given_up = false;
    if (given_up || g_killed.load(std::memory_order_relaxed)) return false;
    HMODULE sl = GetModuleHandleW(L"sl.interposer.dll");
    char* pfx = *reinterpret_cast<char**>(anchors::addr(Id::PostFxSingleton));
    char* d = pfx ? *reinterpret_cast<char**>(pfx + kDlssObject) : nullptr;
    void* opts = *reinterpret_cast<void**>(anchors::addr(Id::DlssSetOptionsPtr));
    const uintptr_t s_tag = anchors::addr(Id::SlSetTagSlot), s_eval = anchors::addr(Id::SlEvaluateSlot),
                    s_consts = anchors::addr(Id::SlSetConstantsSlot), s_free = anchors::addr(Id::SlFreeResourcesSlot);
    if (!sl || !d || !opts) return false;  // not yet: the next frame tries again
    for (uintptr_t s : {s_tag, s_eval, s_consts, s_free}) {
        if (!in_module(*reinterpret_cast<void**>(s), sl)) {
            given_up = true;
            log::error("[dlss] per-eye DLSS refused: the import slot %p does not point into sl.interposer.dll (%p)", reinterpret_cast<void*>(s),
                       *reinterpret_cast<void**>(s));
            return false;
        }
    }
    std::memcpy(g_vp1, d + kViewport, kViewportSize);
    const uint32_t v0 = *reinterpret_cast<uint32_t*>(g_vp1 + kViewportValue);
    if (v0 != 0) {
        given_up = true;
        log::error("[dlss] per-eye DLSS refused: the game's viewport is %u, not 0", v0);
        return false;
    }
    *reinterpret_cast<uint32_t*>(g_vp1 + kViewportValue) = 1;
    // the constants slot too (the check of the game's own calls); the mod's constants call the original
    bool ok = swap_slot(s_consts, &hk_set_consts, &o_set_consts) && swap_slot(s_tag, &hk_set_tag, &o_set_tag) &&
              swap_slot(s_eval, &hk_evaluate, &o_evaluate) && swap_slot(s_free, &hk_free, &o_free);
    if (!ok) {
        given_up = true;
        log::error("[dlss] per-eye DLSS refused: an import slot could not be written");
        return false;
    }
    o_set_options = reinterpret_cast<SetOptions_t>(opts);
    *reinterpret_cast<void**>(anchors::addr(Id::DlssSetOptionsPtr)) = reinterpret_cast<void*>(&hk_set_options);
    d[kOptionsDirty] = 1;  // the options sent again this run: viewport 1 gets them before its first evaluate
    g_ready = true;
    log::info("[dlss] per-eye DLSS on: viewport 1 for the first eye (the game's DLSS object %p; slots swapped)", d);
    return true;
}

// ---- H2: the deferred append (the render thread; every deferred callback passes: two compares, then the original)
void hk_append(void* recorder, void* fn, const void* data, uint32_t size) {
    const uintptr_t f = reinterpret_cast<uintptr_t>(fn);
    if (f == g_cb_consts && !data && !size && g_ready.load(std::memory_order_relaxed) && g_opt_pending.exchange(false)) request_options();
    if ((f != g_cb_consts && f != g_cb_eval) || data || size || g_killed.load(std::memory_order_relaxed))
        return o_append(recorder, fn, data, size);
    const int slot = dual_pass::post_slot();  // 0 the first-eye run, 1 RenderFrame's run of a double frame, 2 mono
    if (slot == 2 && f == g_cb_consts) {
        g_need_reset = true;  // a mono frame: viewport 1 sees a gap
        g_mono_runs.fetch_add(1, std::memory_order_relaxed);
    }
    if (f == g_cb_consts && g_ready.load(std::memory_order_relaxed)) {
        // [XR] EyeShapeDlss: this run's shape for the playback thread (every run: the shape's off runs clear it). When the
        // shape is applied but the options last sent were not the eye's (a toggle), they are sent again first (the AA
        // slot sends them before this run's evaluate when DLSS +0xe8 is set)
        ShapePayload sp{};
        uint32_t rw = 0, rh = 0, ew = 0, eh = 0;
        const bool shaped = eye_shape::dlss_shape(&rw, &rh, &ew, &eh);
        if (shaped) sp = {1, rw, rh, ew, eh};
        const bool want_opts = shaped != g_opt_shaped.load(std::memory_order_relaxed) || (shaped && (g_opt_w.load() != ew || g_opt_h.load() != eh));
        if (want_opts) {
            char* pfx = *reinterpret_cast<char**>(anchors::addr(Id::PostFxSingleton));
            if (char* d = pfx ? *reinterpret_cast<char**>(pfx + kDlssObject) : nullptr) d[kOptionsDirty] = 1;
        }
        if (shaped || t_shape_recorded) o_append(recorder, reinterpret_cast<void*>(&mod_shape_mark), &sp, sizeof(sp));
        t_shape_recorded = shaped;
    }
    if (slot != 0 || !ensure()) return o_append(recorder, fn, data, size);
    if (f == g_cb_consts) {
        ConstsPayload p;
        std::memcpy(p.consts, reinterpret_cast<const void*>(anchors::addr(Id::DlssConstantsStatic)), kConstsSize);
        p.token = *reinterpret_cast<const void* const*>(anchors::addr(Id::DlssTokenStatic));
        if (g_need_reset.exchange(false)) {
            p.consts[kConstsReset] = 1;
            g_resets.fetch_add(1, std::memory_order_relaxed);
        }
        g_consts_swaps.fetch_add(1, std::memory_order_relaxed);
        return o_append(recorder, reinterpret_cast<void*>(&mod_consts1), &p, sizeof(p));  // the append copies the payload
    }
    g_eval_swaps.fetch_add(1, std::memory_order_relaxed);
    o_append(recorder, reinterpret_cast<void*>(&mod_eval1), nullptr, 0);
}

}  // namespace

bool install() {
    // on with DLSS by default (probe P2: both eyes within mono DLSS's flicker; the shared viewport fails); the hook is
    // installed only when the game's DLSS is forced at boot, so FXAA and TAA keep the render thread's path untouched
    g_cfg = config::get_int("Render", "ForceAntiAliasing", -1) == 3 && config::get_bool("Render", "DlssPerEye", true);
    if (!g_cfg.load() || anchors::stand_down()) return true;  // off: no hook
    g_cb_consts = anchors::addr(Id::DlssConstsCallback);
    g_cb_eval = anchors::addr(Id::DlssEvalCallback);
    g_game_vp_static = anchors::addr(Id::DlssEvalViewportStatic);
    const bool ok = hooks::install("RDR deferred append (DLSS per eye)", reinterpret_cast<void*>(anchors::addr(Id::DeferredAppend)), hk_append,
                                   &o_append);
    log::info("[dlss] per-eye DLSS configured (the first-eye run's viewport made at its first DLSS frame): hook %s", ok ? "in" : "FAILED");
    return ok;
}

bool on() { return g_cfg.load() && g_ready.load() && !g_killed.load(); }

void shape_changed() {
    if (g_ready.load()) request_options();  // killed too: viewport 0 must get the game's output size back
}

void set_mv_scale(float k) {
    g_mv_scale = k;
    log::info("[dlss] motion vector scale x %.3f (test)", k);
}

bool last_jitter(float out[2]) {
    if (!g_have_j0.load(std::memory_order_acquire)) return false;
    out[0] = g_last_j0[0];
    out[1] = g_last_j0[1];
    return true;
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, w;
    in >> c >> w;
    if (w == "off") kill("the test channel", -1);
    if (w == "shapemv") {  // dlss shapemv game|eye: the shaped run's mvecScale (the test aid)
        std::string v;
        in >> v;
        g_shape_mv = v == "game" ? 0 : 1;
    }
    char b[1000];
    std::snprintf(b, sizeof(b),
                  "dlss per eye: configured %d, ready %d, killed %d%s%s | counted, not fatal: missing constants %llu, duplicated %llu, "
                  "out of VRAM %llu | constants swaps %llu ok %llu (duplicates %llu), evaluate swaps %llu ok %llu, tags ok %llu, "
                  "options to viewport 1 %llu, resets %llu, mono runs %llu, frees %llu | check: frames %llu, jitter equal %llu differ %llu "
                  "(max %.3f px), tokens equal %llu differ %llu, second eye alone %llu, evaluate tokens off first %llu second %llu",
                  g_cfg.load() ? 1 : 0, g_ready.load() ? 1 : 0, g_killed.load() ? 1 : 0, g_killed.load() ? " (" : "", g_killed.load() ? g_why : "",
                  static_cast<unsigned long long>(g_soft[0].load()), static_cast<unsigned long long>(g_soft[1].load()),
                  static_cast<unsigned long long>(g_soft[2].load()), static_cast<unsigned long long>(g_consts_swaps.load()), static_cast<unsigned long long>(g_consts_ok.load()),
                  static_cast<unsigned long long>(g_dup_consts.load()), static_cast<unsigned long long>(g_eval_swaps.load()), static_cast<unsigned long long>(g_eval_ok.load()),
                  static_cast<unsigned long long>(g_tags_ok.load()), static_cast<unsigned long long>(g_options_vp1.load()),
                  static_cast<unsigned long long>(g_resets.load()), static_cast<unsigned long long>(g_mono_runs.load()),
                  static_cast<unsigned long long>(g_frees.load()), static_cast<unsigned long long>(g_pairs.load()),
                  static_cast<unsigned long long>(g_jit_eq.load()), static_cast<unsigned long long>(g_jit_ne.load()), g_jit_maxd.load(),
                  static_cast<unsigned long long>(g_tok_eq.load()), static_cast<unsigned long long>(g_tok_ne.load()),
                  static_cast<unsigned long long>(g_vp0_alone.load()), static_cast<unsigned long long>(g_eval_tok_ne[1].load()),
                  static_cast<unsigned long long>(g_eval_tok_ne[0].load()));
    std::string s = b;
    if (g_killed.load()) s += ")";
    char e[260];
    std::snprintf(e, sizeof(e),
                  " | the eye shape (EyeShapeDlss): marks %llu, constants %llu (mvecScale %s), tags %llu, refused %llu, size skips %llu, options shaped %llu (the last %ux%u%s)",
                  static_cast<unsigned long long>(g_shape_marks.load()), static_cast<unsigned long long>(g_shape_consts.load()),
                  g_shape_mv.load() ? "the shaped size" : "the game's", static_cast<unsigned long long>(g_shape_tags.load()),
                  static_cast<unsigned long long>(g_shape_refused.load()), static_cast<unsigned long long>(g_shape_size_skips.load()), static_cast<unsigned long long>(g_shape_opts.load()), g_opt_w.load(),
                  g_opt_h.load(), g_opt_shaped.load() ? ", the eye's" : "");
    return s + e;
}

}  // namespace rdrvr::dlss
