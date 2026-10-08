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
    g_resets{0}, g_frees{0}, g_mono_runs{0}, g_dup_consts{0};
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

void kill(const char* what, int r) {
    if (g_killed.exchange(true)) return;
    g_last_error = r;
    std::snprintf(g_why, sizeof(g_why), "%s returned %d", what, r);
    log::error("[dlss] per-eye DLSS off: %s (back to one viewport for both eyes)", g_why);
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
    if (const float k = g_mv_scale.load(std::memory_order_relaxed); k != 1.0f) {
        float m[2];
        std::memcpy(m, p->consts + kConstsMvecScale, sizeof(m));
        m[0] *= k;
        m[1] *= k;
        std::memcpy(p->consts + kConstsMvecScale, m, sizeof(m));
    }
    const int r = o_set_consts(p->consts, p->token, g_vp1);
    if (r == 27) {  // sl::Result::eErrorDuplicatedConstants: Streamline keeps the newer set; the evaluate has its constants
        g_dup_consts.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (r != 0) return kill("slSetConstants(viewport 1)", r);
    g_consts_ok.fetch_add(1, std::memory_order_relaxed);
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

// ---- the Streamline slots (H3, H5)
int hk_set_tag(const void* vp, const void* tags, uint32_t n, void* cmd) {
    if (t_vp1 && reinterpret_cast<uintptr_t>(vp) == g_game_vp_static) {
        const int r = o_set_tag(g_vp1, tags, n, cmd);
        if (r != 0) kill("slSetTag(viewport 1)", r);
        else g_tags_ok.fetch_add(1, std::memory_order_relaxed);
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
        if (r != 0) kill("slEvaluateFeature(viewport 1)", r);
        else g_eval_ok.fetch_add(1, std::memory_order_relaxed);
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
    const int r = o_set_options(vp, options);
    if (!g_killed.load(std::memory_order_relaxed) && reinterpret_cast<uintptr_t>(vp) != reinterpret_cast<uintptr_t>(g_vp1)) {
        const int r1 = o_set_options(g_vp1, options);
        if (r1 != 0) kill("slDLSSSetOptions(viewport 1)", r1);
        else g_options_vp1.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

// a slot's value points into this module's image
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
    if ((f != g_cb_consts && f != g_cb_eval) || data || size || g_killed.load(std::memory_order_relaxed))
        return o_append(recorder, fn, data, size);
    const int slot = dual_pass::post_slot();  // 0 the first-eye run, 1 RenderFrame's run of a double frame, 2 mono
    if (slot == 2 && f == g_cb_consts) {
        g_need_reset = true;  // a mono frame: viewport 1 sees a gap
        g_mono_runs.fetch_add(1, std::memory_order_relaxed);
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
    char b[900];
    std::snprintf(b, sizeof(b),
                  "dlss per eye: configured %d, ready %d, killed %d%s%s | constants swaps %llu ok %llu (duplicates %llu), evaluate swaps %llu ok %llu, tags ok %llu, "
                  "options to viewport 1 %llu, resets %llu, mono runs %llu, frees %llu | check: frames %llu, jitter equal %llu differ %llu "
                  "(max %.3f px), tokens equal %llu differ %llu, second eye alone %llu, evaluate tokens off first %llu second %llu",
                  g_cfg.load() ? 1 : 0, g_ready.load() ? 1 : 0, g_killed.load() ? 1 : 0, g_killed.load() ? " (" : "", g_killed.load() ? g_why : "",
                  static_cast<unsigned long long>(g_consts_swaps.load()), static_cast<unsigned long long>(g_consts_ok.load()),
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
    return s;
}

}  // namespace rdrvr::dlss
