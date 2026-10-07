#include "core/eye_shape.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>

#include "core/anchors.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/d3d_hooks.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/render_settings.h"
#include "core/state.h"
#include "core/xr.h"
#include "core/xr_blit.h"

namespace rdrvr::eye_shape {
namespace {

using anchors::Id;

// ---- the settings and this session's size
std::atomic<bool> g_on{false};      // [XR] EyeShape now ("eyeshape on|off")
std::atomic<bool> g_ini_on{false};  // as read at boot
std::atomic<float> g_scale{1.0f};   // [XR] EyeScale
std::atomic<bool> g_hooked{false};  // both hooks installed
std::atomic<uint32_t> g_rec_w{0}, g_rec_h{0}, g_max_w{0}, g_max_h{0};
std::atomic<uint32_t> g_W{0}, g_H{0};    // the post output's allocation this session (0: no session yet)
std::atomic<uint32_t> g_sw{0}, g_sh{0};  // the eye swapchains
std::atomic<float> g_aspect{0.0f};       // the FOV's tan width / tan height (the wider eye), or the recommended rect's
std::atomic<int> g_aspect_from{0};       // 1 the located FOV, 2 the recommended rect
std::atomic<uint32_t> g_ew{0}, g_eh{0};  // the eye size wanted (0: none)

// ---- the state (the render thread writes it)
std::atomic<bool> g_applied{false};                    // the render class is in the eye shape
std::atomic<uint32_t> g_aw{0}, g_ah{0}, g_arw{0}, g_arh{0};  // as applied: the eye size and the render size
std::atomic<bool> g_run_shaped{false};                 // the last post run was in the eye shape
std::atomic<uint32_t> g_run_w{0}, g_run_h{0};          // its eye size
std::atomic<uint32_t> g_in_w{0}, g_in_h{0}, g_out_w{0}, g_out_h{0};  // the post output read back inside the run and after it
std::atomic<bool> g_killed{false};
std::mutex g_kill_mutex;
char g_kill_why[192] = "";
enum Why { kWanted, kOff, kKilled, kNoSize, kNoXr, kNotStereo, kTechnique, kNoGame, kWhyCount };
const char* const kWhyName[kWhyCount] = {"wanted",      "off",    "killed",        "no eye size this session",
                                         "no XR submission", "not the XR stereo view", "not FXAA", "no renderer"};
std::atomic<int> g_why{kOff};
thread_local int t_want = -1;  // inside the DRS controller hook: 1 the eye shape, 0 uniform; -1 elsewhere (the game's own calls)

std::atomic<uint64_t> g_ctl_calls{0}, g_applies{0}, g_uniforms{0}, g_game_resizes{0}, g_runs_shaped{0}, g_runs_plain{0},
    g_ui_mismatch{0}, g_repaints{0}, g_repaint_fails{0};

// ---- the game's functions
using DrsSub_t = void (*)(void* renderer, int w, int h);
using DrsCtl_t = void (*)(void* renderer);
using PostFxSizes_t = void (*)(void* postfx, int rw, int rh, int ow, int oh);
using GrassSize_t = void (*)(void* grass, int rw, int rh, int w, int h);
using RtSetSize_t = void (*)(void* rt, int uw, int uh, int aw, int ah, int fmt);
DrsSub_t o_DrsSub = nullptr;
DrsCtl_t o_DrsCtl = nullptr;

// ---- guarded memory access (no C++ objects in these: SEH)
bool raw(uintptr_t a, void* out, size_t n) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T>
bool rd(uintptr_t a, T* out) {
    return a && raw(a, out, sizeof(T));
}
template <class T>
bool rdp(const void* base, size_t off, T* out) {
    return base && raw(reinterpret_cast<uintptr_t>(base) + off, out, sizeof(T));
}
bool write_size(void* rt, uint16_t w, uint16_t h) {
    __try {
        volatile uint16_t* s = reinterpret_cast<volatile uint16_t*>(static_cast<char*>(rt) + 0xb0);
        s[0] = w;
        s[1] = h;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool copy_name(const char* s, char* out, size_t n) {
    __try {
        size_t i = 0;
        for (; i + 1 < n && s[i]; ++i) out[i] = (s[i] >= 0x20 && s[i] < 0x7f) ? s[i] : '?';
        out[i] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
}

// An object of the RT class (its vtable, or one whose SetSize is the RT class's: the same layout behind +0xb0/+0xb2).
bool rt_ok(const void* rt) {
    uintptr_t vt = 0, set = 0;
    if (!rt || !rdp(rt, 0, &vt) || !vt) return false;
    if (vt == anchors::addr(Id::RtVtbl)) return true;
    return rd(vt + 0xe0, &set) && set == anchors::addr(Id::RtSetSize);
}
bool rt_size(const void* rt, uint32_t* w, uint32_t* h) {
    uint16_t s[2];
    if (!rt_ok(rt) || !rdp(rt, 0xb0, &s)) return false;
    *w = s[0];
    *h = s[1];
    return true;
}
void rt_call(void* rt, int uw, int uh, int aw, int ah) {
    void** vt = *static_cast<void***>(rt);
    reinterpret_cast<RtSetSize_t>(vt[0xe0 / 8])(rt, uw, uh, aw, ah, 0);
}
template <class T>
T global(Id id) {
    T v{};
    rd(anchors::addr(id), &v);
    return v;
}

void kill(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    {
        std::lock_guard lock(g_kill_mutex);
        if (g_killed.load()) return;
        std::snprintf(g_kill_why, sizeof(g_kill_why), "%s", buf);
        g_killed.store(true);
    }
    log::error("[eye] KILL SWITCH: %s; the game's uniform render size again at the next DRS controller call (\"eyeshape on\" re-arms)", buf);
}

// ---- the eye size
bool compute(uint32_t W, uint32_t H, uint32_t cap_w, uint32_t cap_h, bool log_it, uint32_t* ew, uint32_t* eh) {
    const uint32_t rec_h = g_rec_h.load();
    const float a = g_aspect.load(), s = g_scale.load();
    if (!rec_h || !W || !H || !(a > 0.2f && a < 5.0f)) {
        if (log_it) log::error("[eye] no eye size: recommended height %u, game %ux%u, FOV aspect %.3f", rec_h, W, H, a);
        return false;
    }
    const double want_h = static_cast<double>(rec_h) * s;
    uint32_t h = static_cast<uint32_t>(std::lround(want_h));
    if (h > H) h = H;
    if (h > cap_h) h = cap_h;
    h &= ~1u;
    uint32_t w = static_cast<uint32_t>(std::lround(static_cast<double>(h) * a));
    if (w > W) w = W;
    if (w > cap_w) w = cap_w;
    w &= ~1u;
    if (w < 64 || h < 64) {
        if (log_it) log::error("[eye] no eye size: %ux%u is too small", w, h);
        return false;
    }
    if (log_it) {
        log::info("[eye] recommended %ux%u (max %ux%u), scale %.2f, FOV aspect %.3f (%s), game %ux%u -> eye %ux%u%s", g_rec_w.load(),
                  rec_h, g_max_w.load(), g_max_h.load(), s, a, g_aspect_from.load() == 1 ? "the located FOV" : "the recommended rect", W, H,
                  w, h, want_h > H + 0.5 ? " (the game's height caps it: raise the game's resolution for full density)" : "");
    }
    *ew = w;
    *eh = h;
    return true;
}

void set_aspect_from(const float (*tan)[4]) {
    float a = 0;
    if (tan) {
        for (int e = 0; e < 2; ++e) {
            const float tw = tan[e][1] - tan[e][0], th = tan[e][2] - tan[e][3];
            if (tw > 0 && th > 0 && tw / th > a) a = tw / th;
        }
    }
    if (a > 0.2f && a < 5.0f) {
        g_aspect = a;
        g_aspect_from = 1;
    } else if (g_rec_w.load() && g_rec_h.load()) {
        g_aspect = static_cast<float>(g_rec_w.load()) / static_cast<float>(g_rec_h.load());
        g_aspect_from = 2;
    }
}

// From the latest located views, when the session's planning had none (a runtime "eyeshape on").
void aspect_from_views() {
    xr::EyeView v[2];
    if (!xr::eye_views_peek(v)) {
        set_aspect_from(nullptr);
        return;
    }
    float tan[2][4];
    for (int e = 0; e < 2; ++e)
        for (int k = 0; k < 4; ++k) tan[e][k] = std::tan(v[e].fov[k]);
    set_aspect_from(tan);
}

// The eye size again (the scale changed, or none this session yet): within the session's swapchains.
void resize_eye(bool log_it) {
    const uint32_t W = g_W.load(), H = g_H.load();
    if (!W) return;
    if (g_aspect_from.load() != 1) aspect_from_views();  // the located FOV when the session's planning had none
    uint32_t ew = 0, eh = 0;
    if (compute(W, H, g_sw.load() ? g_sw.load() : W, g_sh.load() ? g_sh.load() : H, log_it, &ew, &eh)) {
        g_ew = ew;
        g_eh = eh;
    }
}

// ---- the render class in the eye shape: after the original's uniform calls, the render-class calls again with the
// eye size (FUN_1405c26a0's own list and order, the out class left as the original set it). Every pointer is read and
// checked before the first call; the calls themselves are not wrapped (they re-create views, which may lock).
bool shape(char* r, int W, int H) {
    const uint32_t ew = g_ew.load(), eh = g_eh.load();
    if (W <= 0 || H <= 0 || static_cast<uint32_t>(W) != g_W.load() || static_cast<uint32_t>(H) != g_H.load()) {
        kill("the game's size %dx%d is not this session's %ux%u", W, H, g_W.load(), g_H.load());
        return false;
    }
    if (!ew || !eh || ew > static_cast<uint32_t>(W) || eh > static_cast<uint32_t>(H)) {
        kill("the eye size %ux%u does not fit %dx%d", ew, eh, W, H);
        return false;
    }
    float s = 0;
    if (!rdp(r, 0x550, &s) || !(s >= 0.2f && s <= 1.0f)) {
        kill("the render scale (renderer +0x550) %.3f is out of range", s);
        return false;
    }
    int rw = static_cast<int>(std::lround(s * static_cast<float>(ew))), rh = static_cast<int>(std::lround(s * static_cast<float>(eh)));
    if (rw < 16) rw = 16;
    if (rh < 16) rh = 16;
    char* postfx = global<char*>(Id::PostFxSingleton);
    int tech = -1;
    if (!postfx || !rdp(postfx, 0x868, &tech)) {
        kill("no PostFx object");
        return false;
    }
    if (tech != 1) return false;  // not FXAA this frame: the uniform state stays (no kill: it comes back with FXAA)
    void* pf_rt[4] = {};
    const size_t pf_off[4] = {0x10, 0x880, 0x3f0, 0x3f8};
    for (int i = 0; i < 4; ++i)
        if (!rdp(postfx, pf_off[i], &pf_rt[i]) || !rt_ok(pf_rt[i])) {
            kill("PostFx +%#zx (%p) is not an RT", pf_off[i], pf_rt[i]);
            return false;
        }
    void* depth_resolve = nullptr;
    void* r548 = nullptr;
    if (!rdp(r, 0x540, &depth_resolve) || !rt_ok(depth_resolve) || !rdp(r, 0x548, &r548) || !rt_ok(r548)) {
        kill("renderer +0x540 (%p) or +0x548 (%p) is not an RT", depth_resolve, r548);
        return false;
    }
    char* lighting = global<char*>(Id::LightingManager);
    char light_on = 0;
    void* light_rt = nullptr;
    if (!lighting || !rdp(lighting, 0x9c80, &light_on)) {
        kill("no lighting manager");
        return false;
    }
    if (light_on && (!rdp(lighting, 0x878, &light_rt) || !rt_ok(light_rt))) {
        kill("the lighting manager's +0x878 (%p) is not an RT", light_rt);
        return false;
    }
    char* shadow = global<char*>(Id::ShadowCollectorOwner);
    void* sc[2] = {};
    if (!shadow || !rdp(shadow, 0x40, &sc[0]) || !rt_ok(sc[0]) || !rdp(shadow, 0x48, &sc[1]) || !rt_ok(sc[1])) {
        kill("the shadow collector (%p: %p %p) is not two RTs", static_cast<void*>(shadow), sc[0], sc[1]);
        return false;
    }
    char* grass = nullptr;
    if (global<uint64_t>(Id::GrassEnabled) != 0) {
        grass = global<char*>(Id::GrassManager);
        void* g1 = nullptr;
        void* g2 = nullptr;
        if (!grass || !rdp(grass, 0xb130, &g1) || !rt_ok(g1) || !rdp(grass, 0xb128, &g2) || !rt_ok(g2)) {
            kill("the grass manager's targets (%p: %p %p) are not RTs", static_cast<void*>(grass), g1, g2);
            return false;
        }
    }
    void* extra = global<void*>(Id::ExtraSceneTarget);
    if (extra && !rt_ok(extra)) {
        kill("ExtraSceneTarget %p is not an RT", extra);
        return false;
    }
    // the calls, in FUN_1405c26a0's order (its render size first)
    *reinterpret_cast<int*>(r + 0x554) = rw;
    *reinterpret_cast<int*>(r + 0x558) = rh;
    if (light_on) rt_call(light_rt, rw, rh, W, H);
    rt_call(depth_resolve, rw, rh, W, H);
    rt_call(r548, rw, rh, W, H);
    reinterpret_cast<PostFxSizes_t>(anchors::addr(Id::PostFxSetSizes))(postfx, rw, rh, W, H);
    rt_call(sc[0], rw, rh, W, H);
    rt_call(sc[1], rw, rh, W, H);
    if (grass) reinterpret_cast<GrassSize_t>(anchors::addr(Id::GrassSetSize))(grass, rw, rh, W, H);
    if (extra) rt_call(extra, rw, rh, W, H);
    g_aw = ew;
    g_ah = eh;
    g_arw = static_cast<uint32_t>(rw);
    g_arh = static_cast<uint32_t>(rh);
    return true;
}

void hk_DrsSub(void* renderer, int w, int h) {
    o_DrsSub(renderer, w, h);  // the game's uniform state at (s W, s H)
    const int want = t_want;
    if (want != 1) {
        if (g_applied.exchange(false)) {
            g_uniforms.fetch_add(1, std::memory_order_relaxed);
            if (want < 0) g_game_resizes.fetch_add(1, std::memory_order_relaxed);
            log::info("[eye] the game's uniform render size again (%dx%d%s)", w, h, want < 0 ? ": the game's own resize" : "");
        }
        return;
    }
    const uint32_t aw = g_aw.load(), ah = g_ah.load(), arw = g_arw.load(), arh = g_arh.load();
    if (!renderer || !shape(static_cast<char*>(renderer), w, h)) {
        if (g_applied.exchange(false)) g_uniforms.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_applies.fetch_add(1, std::memory_order_relaxed);
    const bool was = g_applied.exchange(true);
    if (!was || aw != g_aw.load() || ah != g_ah.load() || arw != g_arw.load() || arh != g_arh.load())
        log::limited("eye.applied", 64, "[eye] applied: the eyes %ux%u, rendered at %ux%u, in the game's %dx%d", g_aw.load(), g_ah.load(),
                     g_arw.load(), g_arh.load(), w, h);
}

int decide(char* r) {
    if (!g_on.load(std::memory_order_relaxed)) return kOff;
    if (g_killed.load(std::memory_order_relaxed)) return kKilled;
    const uint32_t W = g_W.load(std::memory_order_relaxed), H = g_H.load(std::memory_order_relaxed);
    const uint32_t ew = g_ew.load(std::memory_order_relaxed), eh = g_eh.load(std::memory_order_relaxed);
    if (!W || !ew || !eh) return kNoSize;
    if (!xr::submitting()) return kNoXr;
    if (!camera_lever::xr_double()) return kNotStereo;
    char* postfx = global<char*>(Id::PostFxSingleton);
    int tech = -1;
    if (!r || !postfx || !rdp(postfx, 0x868, &tech)) return kNoGame;
    if (tech != 1) return kTechnique;
    const int sw = global<int>(Id::ScreenWidth), sh = global<int>(Id::ScreenHeight);
    if (sw != static_cast<int>(W) || sh != static_cast<int>(H)) {
        kill("the game's size changed: %dx%d, this session's %ux%u", sw, sh, W, H);
        return kKilled;
    }
    return kWanted;
}

void hk_DrsCtl(void* renderer) {
    if (!g_on.load(std::memory_order_relaxed) && !g_applied.load(std::memory_order_relaxed)) {  // off: one flag check
        o_DrsCtl(renderer);
        return;
    }
    g_ctl_calls.fetch_add(1, std::memory_order_relaxed);
    const int why = decide(static_cast<char*>(renderer));
    if (g_why.exchange(why) != why) log::info("[eye] %s", why == kWanted ? "wanted: the eyes in the headset's shape" : kWhyName[why]);
    const int want = why == kWanted ? 1 : 0;
    t_want = want;
    o_DrsCtl(renderer);  // the dirty bytes of the last change cleared first, as the game's own DRS change does
    const bool applied = g_applied.load();
    const bool change = want ? (!applied || g_aw.load() != g_ew.load() || g_ah.load() != g_eh.load()) : applied;
    if (change && renderer) hk_DrsSub(renderer, global<int>(Id::ScreenWidth), global<int>(Id::ScreenHeight));
    t_want = -1;
}

// ---- the monitor: the left eye pillarboxed onto the back buffer (the gamma blit drew the eye rect in its top-left
// and stale pixels beside it), before the UI mirror draws the UI over it
void on_frame_end(uint64_t) {
    if (!g_run_shaped.load(std::memory_order_relaxed) || !xr::submitting()) return;
    IDXGISwapChain* sc = state::swapchain.load();
    ID3D12CommandQueue* q = state::present_queue.load();
    if (!sc || !q) return;
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) return;
    ID3D12Resource* bb = nullptr;
    HRESULT hr = sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
    sc3->Release();
    if (FAILED(hr) || !bb) return;
    const bool ok = xr_blit::repaint(q, bb, g_run_w.load(), g_run_h.load(), xr_blit::game_gamma(1.0f));
    (ok ? g_repaints : g_repaint_fails).fetch_add(1, std::memory_order_relaxed);
    if (!ok) log::limited("eye.repaint", 4, "[eye] the monitor's repaint was not drawn (the eye blit not ready, or its list busy)");
    bb->Release();
}

// The render-class RTs and the post output, by name (RT +0x60) and logical size: the status's list.
std::string rt_list(bool long_names) {
    struct Slot {
        const char* what;
        void* rt;
    } slots[20];
    int n = 0;
    auto add = [&](const char* what, void* rt) {
        if (n < 20) slots[n++] = {what, rt};
    };
    char* r = global<char*>(Id::RendererSingleton);
    char* postfx = global<char*>(Id::PostFxSingleton);
    char* lighting = global<char*>(Id::LightingManager);
    char* shadow = global<char*>(Id::ShadowCollectorOwner);
    char* grass = global<char*>(Id::GrassManager);
    void* p = nullptr;
    if (r && rdp(r, 0x540, &p)) add("r540", p);
    if (r && rdp(r, 0x548, &p)) add("r548", p);
    if (lighting && rdp(lighting, 0x878, &p)) add("light878", p);
    if (shadow && rdp(shadow, 0x40, &p)) add("sc40", p);
    if (shadow && rdp(shadow, 0x48, &p)) add("sc48", p);
    if (global<uint64_t>(Id::GrassEnabled) && grass && rdp(grass, 0xb128, &p)) add("grass128", p);
    if (global<uint64_t>(Id::GrassEnabled) && grass && rdp(grass, 0xb130, &p)) add("grass130", p);
    if (void* x = global<void*>(Id::ExtraSceneTarget)) add("extra", x);
    const size_t pf[] = {0x10, 0x880, 0x3f0, 0x3f8, 0x898, 0x890, 0x400, 0x408, 0x540, 0x28};
    const char* pfn[] = {"pf10", "pf880", "pf3f0", "pf3f8", "pf898", "pf890", "pf400", "pf408", "pf540", "pf28"};
    for (int i = 0; i < 10; ++i)
        if (postfx && rdp(postfx, pf[i], &p)) add(pfn[i], p);
    std::string out;
    for (int i = 0; i < n; ++i) {
        char name[48] = "?";
        const char* np = nullptr;
        uint32_t w = 0, h = 0;
        const bool ok = rt_size(slots[i].rt, &w, &h);
        if (ok && rdp(slots[i].rt, 0x60, &np) && np) copy_name(np, name, long_names ? sizeof(name) : 20);
        char b[96];
        if (ok)
            std::snprintf(b, sizeof(b), "%s%s %s %ux%u", i ? ", " : "", slots[i].what, name, w, h);
        else
            std::snprintf(b, sizeof(b), "%s%s %s", i ? ", " : "", slots[i].what, slots[i].rt ? "NOT AN RT" : "null");
        out += b;
    }
    return out;
}

}  // namespace

void init() {
    g_on = g_ini_on = config::get_bool("XR", "EyeShape", false);
    float s = config::get_float("XR", "EyeScale", 1.0f);
    if (!(s >= 0.25f && s <= 2.0f)) s = 1.0f;
    g_scale = s;
    log::info("[eye] [XR] EyeShape %s, EyeScale %.2f", g_on.load() ? "on" : "off", s);
    d3d::add_frame_end_listener(on_frame_end);
}

bool install() {
    if (anchors::stand_down()) return false;
    bool ok = hooks::install("RDR DRS sub-region (eye shape)", reinterpret_cast<void*>(anchors::addr(Id::DrsSubregion)), hk_DrsSub, &o_DrsSub);
    ok = hooks::install("RDR DRS controller (eye shape)", reinterpret_cast<void*>(anchors::addr(Id::DrsController)), hk_DrsCtl, &o_DrsCtl) && ok;
    g_hooked = ok;
    if (!ok) {
        g_on = false;
        log::error("[eye] the DRS hooks are not installed: EyeShape stays off");
    }
    return ok;
}

void set_recommended(uint32_t rec_w, uint32_t rec_h, uint32_t max_w, uint32_t max_h) {
    g_rec_w = rec_w;
    g_rec_h = rec_h;
    g_max_w = max_w;
    g_max_h = max_h;
}

bool configured() { return g_on.load() && g_hooked.load() && render_settings::forced_aa() == 1; }

bool enabled() { return g_on.load(); }

bool set_enabled(bool on, bool save) {
    if (on && !g_hooked.load()) return false;
    if (on) {
        {
            std::lock_guard lock(g_kill_mutex);
            if (g_killed.exchange(false)) log::info("[eye] re-armed (the kill switch was: %s)", g_kill_why);
            g_kill_why[0] = 0;
        }
        if (g_W.load() && !g_ew.load()) resize_eye(true);  // the session began with it off: an eye size inside its W x H swapchains
    }
    g_on = on;
    if (save) config::set("XR", "EyeShape", on ? "1" : "0");
    log::info("[eye] EyeShape %s (%s)", on ? "on" : "off", save ? "saved to the user ini" : "the session; the ini is not written");
    return true;
}

void plan_session(uint32_t w, uint32_t h, const float (*tan)[4], uint32_t* sw, uint32_t* sh) {
    g_W = w;
    g_H = h;
    g_sw = w;
    g_sh = h;
    g_ew = 0;
    g_eh = 0;
    *sw = w;
    *sh = h;
    set_aspect_from(tan);
    if (!g_on.load()) {
        log::info("[eye] EyeShape off: the eye swapchains are the game's %ux%u frame", w, h);
        return;
    }
    if (!g_hooked.load()) {
        log::error("[eye] EyeShape refused: the DRS hooks are not installed");
        return;
    }
    if (render_settings::forced_aa() != 1) {
        log::error("[eye] EyeShape refused: [Render] ForceAntiAliasing is %d, not 1 (FXAA): the eye swapchains are the game's %ux%u frame",
                   render_settings::forced_aa(), w, h);
        return;
    }
    uint32_t ew = 0, eh = 0;
    if (!compute(w, h, w, h, true, &ew, &eh)) return;
    g_ew = ew;
    g_eh = eh;
    g_sw = ew;
    g_sh = eh;
    *sw = ew;
    *sh = eh;
}

bool frame_rect(uint32_t* cw, uint32_t* ch, uint32_t* rw, uint32_t* rh) {
    if (!g_run_shaped.load(std::memory_order_acquire)) return false;
    *cw = g_run_w.load(std::memory_order_relaxed);
    *ch = g_run_h.load(std::memory_order_relaxed);
    if (rw) *rw = g_arw.load(std::memory_order_relaxed);
    if (rh) *rh = g_arh.load(std::memory_order_relaxed);
    return true;
}

void begin_run(void* postfx, RunPoke* p) {
    p->rt = nullptr;
    if (!g_applied.load(std::memory_order_relaxed)) {
        if (g_run_shaped.load(std::memory_order_relaxed)) g_run_shaped.store(false, std::memory_order_release);
        return;
    }
    const uint32_t W = g_W.load(), H = g_H.load(), ew = g_aw.load(), eh = g_ah.load();
    int tech = -1;
    void* rt = nullptr;
    uint16_t wh[2] = {};
    bool ok = postfx && rdp(postfx, 0x868, &tech) && tech == 1;  // not FXAA: this run is the game's (no kill)
    if (ok && (!rdp(postfx, 0x898, &rt) || !rt_ok(rt))) {
        kill("the post output (PostFx +0x898, %p) is not an RT", rt);
        ok = false;
    }
    if (ok && (!rdp(rt, 0xb0, &wh) || wh[0] != W || wh[1] != H)) {
        kill("the post output's logical size is %ux%u, not %ux%u", wh[0], wh[1], W, H);
        ok = false;
    }
    if (ok && (!ew || !eh || ew > W || eh > H || !write_size(rt, static_cast<uint16_t>(ew), static_cast<uint16_t>(eh)))) {
        kill("the post output's eye size %ux%u was not written", ew, eh);
        ok = false;
    }
    if (!ok) {
        g_run_shaped.store(false, std::memory_order_release);
        g_runs_plain.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    p->rt = rt;
    p->w = wh[0];
    p->h = wh[1];
    uint16_t in[2] = {};
    if (rdp(rt, 0xb0, &in)) {
        g_in_w.store(in[0], std::memory_order_relaxed);
        g_in_h.store(in[1], std::memory_order_relaxed);
    }
    g_run_w.store(ew, std::memory_order_relaxed);
    g_run_h.store(eh, std::memory_order_relaxed);
    g_run_shaped.store(true, std::memory_order_release);
    g_runs_shaped.fetch_add(1, std::memory_order_relaxed);
}

void end_run(void* postfx, const RunPoke& p) {
    if (!p.rt) return;
    uint16_t out[2] = {};
    if (!write_size(p.rt, p.w, p.h) || !rdp(p.rt, 0xb0, &out) || out[0] != p.w || out[1] != p.h) {
        g_ui_mismatch.fetch_add(1, std::memory_order_relaxed);
        kill("the post output's size was not put back to %ux%u (reads %ux%u)", p.w, p.h, out[0], out[1]);
        return;
    }
    g_out_w.store(out[0], std::memory_order_relaxed);
    g_out_h.store(out[1], std::memory_order_relaxed);
    // the UI pass binds PostFx +0x890 (the chain's output pointer: the Post FXAA Target under FXAA): W x H after the run
    void* ui = nullptr;
    uint32_t uw = 0, uh = 0;
    if (rdp(postfx, 0x890, &ui) && ui && ui != p.rt && rt_size(ui, &uw, &uh) && (uw != p.w || uh != p.h)) {
        g_ui_mismatch.fetch_add(1, std::memory_order_relaxed);
        log::limited("eye.ui", 4, "[eye] the UI pass's target %p is %ux%u after the run, not %ux%u (the game's: not changed by the mod)", ui, uw, uh,
                     p.w, p.h);
    }
}

void status_text(char* out, size_t len) {
    char* r = global<char*>(Id::RendererSingleton);
    char* postfx = global<char*>(Id::PostFxSingleton);
    int rw = 0, rh = 0, tech = -1;
    float s = 0;
    if (r) {
        rdp(r, 0x554, &rw);
        rdp(r, 0x558, &rh);
        rdp(r, 0x550, &s);
    }
    if (postfx) rdp(postfx, 0x868, &tech);
    uint32_t ow = 0, oh = 0;
    void* po = nullptr;
    if (postfx && rdp(postfx, 0x898, &po)) rt_size(po, &ow, &oh);
    char killed[200] = "0";
    if (g_killed.load()) {
        std::lock_guard lock(g_kill_mutex);
        std::snprintf(killed, sizeof(killed), "1 (%.100s)", g_kill_why);
    }
    const int why = g_why.load();
    std::snprintf(out, len,
                  "eyeshape %s, scale %.2f, configured %d, hooks %d | game %ux%u, rec %ux%u, aspect %.3f, eye %ux%u, swapchains %ux%u | "
                  "wanted %d (%s), applied %d, eyes %ux%u render %ux%u, last run %d | killed %s | renderer %dx%d s %.3f tech %d | "
                  "screen %dx%d rt %dx%d | post output now %ux%u, in run %ux%u, after %ux%u | runs %llu/%llu, applies %llu, uniform %llu, "
                  "resizes %llu, ui mismatch %llu, repaints %llu/%llu",
                  g_on.load() ? "on" : "off", g_scale.load(), configured() ? 1 : 0, g_hooked.load() ? 1 : 0, g_W.load(), g_H.load(), g_rec_w.load(),
                  g_rec_h.load(), g_aspect.load(), g_ew.load(), g_eh.load(), g_sw.load(), g_sh.load(), why == kWanted ? 1 : 0, kWhyName[why],
                  g_applied.load() ? 1 : 0, g_aw.load(), g_ah.load(), g_arw.load(), g_arh.load(), g_run_shaped.load() ? 1 : 0, killed, rw, rh, s, tech,
                  global<int>(Id::ScreenWidth), global<int>(Id::ScreenHeight), global<int>(Id::ScreenWidthRT), global<int>(Id::ScreenHeightRT), ow,
                  oh, g_in_w.load(), g_in_h.load(), g_out_w.load(), g_out_h.load(), static_cast<unsigned long long>(g_runs_shaped.load()),
                  static_cast<unsigned long long>(g_runs_plain.load()), static_cast<unsigned long long>(g_applies.load()),
                  static_cast<unsigned long long>(g_uniforms.load()), static_cast<unsigned long long>(g_game_resizes.load()),
                  static_cast<unsigned long long>(g_ui_mismatch.load()), static_cast<unsigned long long>(g_repaints.load()),
                  static_cast<unsigned long long>(g_repaint_fails.load()));
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, v;
    in >> c;  // "eyeshape"
    in >> v;
    if (v == "on" || v == "off") {
        if (!set_enabled(v == "on", false)) return "ERROR the DRS hooks are not installed (the anchors stood down or a hook failed)";
    } else if (v == "scale") {
        std::string x;
        in >> x;
        const float s = static_cast<float>(std::atof(x.c_str()));
        if (!(s >= 0.25f && s <= 2.0f)) return "ERROR usage: eyeshape scale <0.25..2>";
        g_scale = s;
        resize_eye(true);
        log::info("[eye] EyeScale %.2f (the session): the eyes %ux%u from the next frame (within the swapchains %ux%u)", s, g_ew.load(), g_eh.load(),
                  g_sw.load(), g_sh.load());
    } else if (v == "rts") {  // every render-class RT and the post output by name and logical size (also logged in full)
        log::info("[eye] rts: %s", rt_list(true).c_str());
        return rt_list(false);
    } else if (!v.empty() && v != "status") {
        return "ERROR usage: eyeshape [on|off|scale <x>|status|rts]";
    }
    char st[1024];
    status_text(st, sizeof(st));
    log::info("[eye] status: %s | rts: %s", st, rt_list(true).c_str());
    return st;
}

}  // namespace rdrvr::eye_shape
