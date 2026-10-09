#include "core/taa.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/anchors.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/dual_pass.h"
#include "core/eye_shape.h"
#include "core/hooks.h"
#include "core/log.h"

namespace rdrvr::taa {
namespace {

using anchors::Id;

template <typename T>
T& at(void* base, size_t off) {
    return *reinterpret_cast<T*>(static_cast<char*>(base) + off);
}
template <typename T>
T& global(Id id) {
    return *reinterpret_cast<T*>(anchors::addr(id));
}

// PostFx fields (research\native-taa-study.md).
constexpr size_t kTechApplied = 0x868, kFullScreenCopy = 0x10, kDepth = 0xf8, kVelocity = 0x3f0, kTaaEffect = 0x128;
constexpr size_t kTechResolve = 0x3b8, kVarParams = 0x3c0, kVarNdcToPrev = 0x3c4, kVarTexel = 0x3c8, kVarDepth = 0x3cc,
                 kVarCurrent = 0x3d4, kVarPrevious = 0x3d8, kVarVelocity = 0x3dc;
constexpr size_t kTaaParams = 0x390, kReset = 0x3b2, kJitter = 0x370, kJitterDelta = 0x378, kJitterIndex = 0x388,
                 kJitterWrap = 0x38c, kPrevBlock = 0x430, kNdcToPrev = 0x4b0, kScale = 0x3a0;
constexpr size_t kTechCopyRt = 0x7a0, kVarToneMap = 0x5e4;  // rdr2_postfx CopyRT samples ToneMap

using AaSlot_t = void* (*)(void* postfx, void* a2, void* a3, uintptr_t latch);
using TaaUpdate_t = void (*)(void* postfx, void* vp, uintptr_t flag);
using Jitter_t = void (*)(void* vp, const float* jitter);
using Persp_t = void (*)(void* vp);
using SetVar_t = void (*)(void* owner, void* eff, int handle, const void* data, int size, int count);
using VarDirty_t = void (*)(void* eff, int index);
using Quad_t = void (*)(void* eff, float x0, float y0, float x1, float y1, float z, float u0, float v0, float u1, float v1,
                        uint32_t color, int technique);
using Bind_t = void (*)(void* ctx, int slot, void* rt, void* depth, int a5, bool a6, int a7, int a8);
using Unbind_t = void (*)(void* ctx, int slot, void* a3, int a4);
using Create_t = void* (*)(void* factory, const char* name, int type, int w, int h, int bpp, const void* desc);
using RtInt_t = int (*)(void* rt);
using PostPass_t = void (*)(void* postfx, void* dst, void* src, uintptr_t technique, uintptr_t var, uintptr_t a6,
                            uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11, uintptr_t a12,
                            uintptr_t a13, uintptr_t a14);

AaSlot_t o_AaSlot = nullptr;
TaaUpdate_t o_TaaUpdate = nullptr;
uintptr_t g_rain_taa_ret = 0, g_scene_rain_ret = 0;

// One history pair per post run of a double frame: [0] the first-eye run, [1] RenderFrame's own run (and mono).
struct History {
    void* rt[2] = {};
    int t = 0;          // rt[t] holds the last resolve
    bool valid = false;
};
History g_hist[2];
void* g_scratch = nullptr;
int g_w = 0, g_h = 0;
int g_state = 0;  // 0 not made, 1 made, -1 refused (the reason is logged once)

// The TAA snapshot each pass of a double frame keeps between frames (its previous camera and projection).
struct Snap {
    char jitter[0x10];
    char prev[0x80];
    bool valid = false;
};
Snap g_snap[3];  // pass 1, pass 2, a mono pass with a changed camera
thread_local int t_index_base = 0;  // the jitter index PreRender advanced to this tick (read at the first pass's wait)

std::atomic<bool> g_shared{false};  // positive control: one history and one TAA state for both eyes
std::atomic<bool> g_mono_pass{true};  // a mono pass with a lever-changed camera gets its own update
// [Render] DlssPassJitter (on): under DLSS (technique 5) the per-pass jitter and TAA state as for technique 2 (G2)
std::atomic<bool> g_dlss_jitter{true};
// [Render] DlssJitterUnits (on): the per-pass update run with the viewport's screen size (vp+0x330/+0x334, which the
// jitter's NDC shift divides by) set to DLSS's render size (DLSS+0xec/+0xf0): the image then moves by the jitterOffset
// in render pixels, as DLSS is told, instead of by it in output pixels (the game's own: 0.667 of a render pixel at
// Quality, 0.333 at Ultra Performance, so the samples never cover the pixel). At DLAA the two sizes agree.
std::atomic<bool> g_jitter_units{true};
std::atomic<uint64_t> g_units_applied{0};
constexpr size_t kDlssObject = 0x530, kDlssRenderW = 0xec, kDlssRenderH = 0xf0, kVpScreenW = 0x330, kVpScreenH = 0x334;
std::atomic<uint64_t> g_resolves[3], g_resets{0}, g_pass_updates{0}, g_mono_jitters{0}, g_mono_updates{0}, g_rain_restores{0},
    g_rain_skips{0}, g_size_mismatch{0};

char* postfx() { return global<char*>(Id::PostFxSingleton); }

// The per-pass jitter and TAA state: native TAA with its targets made, or DLSS (technique 5: its own resolve, the
// game's jitter sequence; research\run6\dlss.md G2)
bool jitter_active() {
    char* p = postfx();
    if (!p) return false;
    const int t = at<int>(p, kTechApplied);
    return (t == 2 && g_state == 1) || (t == 5 && g_dlss_jitter.load(std::memory_order_relaxed));
}

uintptr_t fbits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

int rt_width(void* rt) { return reinterpret_cast<RtInt_t>((*reinterpret_cast<void***>(rt))[0x38 / 8])(rt); }
int rt_height(void* rt) { return reinterpret_cast<RtInt_t>((*reinterpret_cast<void***>(rt))[0x40 / 8])(rt); }

void set_vec(void* eff, int handle, const void* data, int size) {
    if (!handle) return;
    reinterpret_cast<SetVar_t>(anchors::addr(Id::EffectSetVar))(at<void*>(eff, 0x10), eff, handle, data, size, 1);
}

// The engine's texture-variable store (as in FUN_14086a4f0 and the velocity pass): the effect's variable table, then
// the dirty mark, then the render thread's bound-state cache.
void set_tex(void* eff, int handle, void* tex) {
    if (!handle) return;
    char* table = *reinterpret_cast<char**>(eff);
    void*& slot = *reinterpret_cast<void**>(table - 8 + static_cast<ptrdiff_t>(handle) * 0x10);
    bool dirty = tex && *(static_cast<char*>(tex) + 0x2c) != 0;
    if (slot != tex || dirty) {
        slot = tex;
        reinterpret_cast<VarDirty_t>(anchors::addr(Id::EffectVarDirty))(eff, handle - 1);
    }
    if (global<DWORD>(Id::RenderThreadId) == GetCurrentThreadId()) global<uint64_t>(Id::BindCache) = 0;
}

// A full-resolution RGBA16F render target made the way the engine makes FullScreenCopy (0x140868d03..0x140868dc3).
void* make_target(const char* name, int w, int h) {
    alignas(16) unsigned char d[0x68] = {};
    d[0x00] = 1;
    d[0x08] = 1;
    *reinterpret_cast<uint32_t*>(d + 0x0c) = 1;
    *reinterpret_cast<uint16_t*>(d + 0x10) = 0x0101;
    d[0x12] = 1;
    *reinterpret_cast<uint64_t*>(d + 0x24) = 1;
    *reinterpret_cast<uint16_t*>(d + 0x34) = 1;
    *reinterpret_cast<uint32_t*>(d + 0x38) = global<uint64_t>(Id::TargetFormatFlag) ? 6u : 7u;
    *reinterpret_cast<uint32_t*>(d + 0x3c) = 0x10101;
    d[0x40] = 1;
    *reinterpret_cast<float*>(d + 0x5c) = 1.0f;
    *reinterpret_cast<float*>(d + 0x60) = 1.0f;
    *reinterpret_cast<uint16_t*>(d + 0x64) = 0x0100;
    void* factory = global<void*>(Id::GrcContext);
    auto create = reinterpret_cast<Create_t>((*reinterpret_cast<void***>(factory))[0x88 / 8]);
    return create(factory, name, 3, w, h, 0x20, d);
}

void refuse(const char* why) {
    g_state = -1;
    log::error("[taa] native TAA refused: %s", why);
}

// ---- the anti-aliasing slot: TAAResolve into the run's other history target, copied to the scratch target.
// What makes the game discard the upscaler's history (every technique, every post run): the slot clears the Velocity
// RT when its latch (FUN_1405ed420: RendererSingleton+0x1fa4, the pair *(0x142ad1c90)+0x2524e/+0x2524f, or the pause
// menu / scope toggling), +0x3b2 or +0x558 is set, and sets +0x3b2 for the next run's DLSS reset.
struct Cuts {
    std::atomic<uint64_t> runs{0}, latch{0}, r3b2{0}, r558{0}, clears{0}, src_1fa4{0}, src_2524{0}, src_toggle{0};
};
Cuts g_cuts[3];  // first-eye run, RenderFrame's run of a double frame, mono
std::atomic<uint64_t> g_cut_runs{0};
constexpr uintptr_t kCutFlagsObject = 0x2ad1c90;  // RDR.exe+: the pointer to the object holding +0x2524e/+0x2524f

void count_cut(const char* p, uintptr_t latch) {
    const int slot = dual_pass::post_slot();
    Cuts& c = g_cuts[slot < 0 || slot > 2 ? 2 : slot];
    c.runs.fetch_add(1, std::memory_order_relaxed);
    const bool l = (latch & 0xff) != 0, a = p[kReset] != 0, b = p[0x558] != 0;
    if (l) {
        c.latch.fetch_add(1, std::memory_order_relaxed);
        const char* r = global<char*>(Id::RendererSingleton);
        const char* o = *reinterpret_cast<char* const*>(anchors::base() + kCutFlagsObject);
        if (r && r[0x1fa4]) c.src_1fa4.fetch_add(1, std::memory_order_relaxed);
        else if (o && o[0x2524e] && o[0x2524f]) c.src_2524.fetch_add(1, std::memory_order_relaxed);
        else c.src_toggle.fetch_add(1, std::memory_order_relaxed);
    }
    if (a) c.r3b2.fetch_add(1, std::memory_order_relaxed);
    if (b) c.r558.fetch_add(1, std::memory_order_relaxed);
    if (l || a || b) c.clears.fetch_add(1, std::memory_order_relaxed);
    if (g_cut_runs.fetch_add(1, std::memory_order_relaxed) % 1440 == 1439) {
        char t[600];
        cut_text(t, sizeof(t));
        log::info("[taa] %s", t);
    }
}

void* hk_AaSlot(void* pfx, void* a2, void* a3, uintptr_t latch) {
    if (pfx) count_cut(static_cast<const char*>(pfx), latch);
    void* out = o_AaSlot(pfx, a2, a3, latch);
    char* p = static_cast<char*>(pfx);
    if (g_state != 1 || !p || at<int>(p, kTechApplied) != 2 || !out) return out;
    void* cur = at<void*>(p, kFullScreenCopy);
    if (out != cur) return out;  // not the slot's technique-2 path
    if (rt_width(cur) != g_w || rt_height(cur) != g_h) {
        if (g_size_mismatch.fetch_add(1) == 0)
            log::error("[taa] FullScreenCopy is %dx%d, the history %dx%d: no resolve (resize not handled)", rt_width(cur),
                       rt_height(cur), g_w, g_h);
        return out;
    }
    int slot = dual_pass::post_slot();  // 0 first-eye run, 1 RenderFrame's run in a double frame, 2 mono
    History& h = g_hist[slot == 0 && !g_shared.load(std::memory_order_relaxed) ? 0 : 1];
    void* prev = h.rt[h.t];
    void* next = h.rt[1 - h.t];
    void* eff = at<void*>(p, kTaaEffect);
    bool reset = at<char>(p, kReset) != 0 || !h.valid;
    float params[4];
    std::memcpy(params, p + kTaaParams, sizeof(params));
    if (reset) params[0] = params[1] = 0.0f;  // the current frame only
    const float texel[4] = {1.0f / g_w, 1.0f / g_h, static_cast<float>(g_w), static_cast<float>(g_h)};
    set_vec(eff, at<int>(p, kVarTexel), texel, sizeof(texel));
    set_vec(eff, at<int>(p, kVarParams), params, sizeof(params));
    set_tex(eff, at<int>(p, kVarDepth), at<void*>(p, kDepth));
    set_tex(eff, at<int>(p, kVarCurrent), cur);
    set_tex(eff, at<int>(p, kVarPrevious), prev);
    set_tex(eff, at<int>(p, kVarVelocity), at<void*>(p, kVelocity));
    void* ctx = global<void*>(Id::GrcContext);
    void** vt = *reinterpret_cast<void***>(ctx);
    reinterpret_cast<Bind_t>(vt[0x90 / 8])(ctx, 0, next, nullptr, 0, true, -1, 0);
    reinterpret_cast<Quad_t>(anchors::addr(Id::DrawQuad))(eff, -1.0f, 1.0f, 1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f,
                                                         global<uint32_t>(Id::QuadColor), at<int>(p, kTechResolve));
    reinterpret_cast<Unbind_t>(vt[0x98 / 8])(ctx, 0, nullptr, -1);
    void* none = global<void*>(Id::DefaultTexture);
    set_tex(eff, at<int>(p, kVarCurrent), none);
    set_tex(eff, at<int>(p, kVarPrevious), none);
    set_tex(eff, at<int>(p, kVarVelocity), none);
    set_tex(eff, at<int>(p, kVarDepth), none);
    // The chain continues from the scratch copy: the post rain draws onto it, never into the history.
    reinterpret_cast<PostPass_t>(anchors::addr(Id::PostDrawPass))(
        p, g_scratch, next, static_cast<uint32_t>(at<int>(p, kTechCopyRt)), static_cast<uint32_t>(at<int>(p, kVarToneMap)),
        0, fbits(-1.0f), fbits(1.0f), fbits(1.0f), fbits(-1.0f), fbits(0.0f), fbits(0.0f), fbits(1.0f), fbits(1.0f));
    h.t = 1 - h.t;
    h.valid = true;
    if (reset) g_resets.fetch_add(1, std::memory_order_relaxed);
    g_resolves[slot < 0 || slot > 2 ? 2 : slot].fetch_add(1, std::memory_order_relaxed);
    return g_scratch;
}

// ---- the rain draw's TAA call (FUN_1408728f0 with flag 1 on the rain's viewport copy): it shifts the previous camera,
// advances the jitter index and writes the negated jitter; the pass's TAA state is put back after it.
void hk_TaaUpdate(void* pfx, void* vp, uintptr_t flag) {
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_rain_taa_ret || !pfx || !jitter_active()) {
        o_TaaUpdate(pfx, vp, flag);
        return;
    }
    char* p = static_cast<char*>(pfx);
    char a[0x20], b[0x100];
    std::memcpy(a, p + kJitter, sizeof(a));      // +0x370..+0x38f: jitter, delta, index, wrap
    std::memcpy(b, p + kPrevBlock, sizeof(b));   // +0x430..+0x52f: previous camera/projection, NDCToPrevNDC, PrevVP
    o_TaaUpdate(pfx, vp, flag);
    std::memcpy(p + kJitter, a, sizeof(a));
    std::memcpy(p + kPrevBlock, b, sizeof(b));
    if (void* eff = at<void*>(p, kTaaEffect)) set_vec(eff, at<int>(p, kVarNdcToPrev), p + kNdcToPrev, 0x40);
    g_rain_restores.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

bool install() {
    if (anchors::stand_down()) return false;
    g_dlss_jitter = config::get_bool("Render", "DlssPassJitter", true);
    g_jitter_units = config::get_bool("Render", "DlssJitterUnits", true);
    g_rain_taa_ret = anchors::addr(Id::RainTaaReturn);
    g_scene_rain_ret = anchors::addr(Id::SceneRainReturn);
    bool ok = hooks::install("RDR anti-aliasing slot (TAA resolve)", reinterpret_cast<void*>(anchors::addr(Id::AaSlot)),
                             hk_AaSlot, &o_AaSlot);
    ok = hooks::install("RDR TAA update (rain call)", reinterpret_cast<void*>(anchors::addr(Id::TaaUpdate)), hk_TaaUpdate,
                        &o_TaaUpdate) &&
         ok;
    return ok;
}

bool active() {
    char* p = g_state == 1 ? postfx() : nullptr;
    return p && at<int>(p, kTechApplied) == 2;
}

void frame_start() {
    if (g_state != 0) return;
    char* p = postfx();
    if (!p || at<int>(p, kTechApplied) != 2) return;
    float scale = at<float>(p, kScale);
    int wrap = at<int>(p, kJitterWrap);
    if (scale != 1.0f) return refuse("PostFx+0x3a0 is not 1.0 (an upscaler ratio): technique 2 would render below native");
    if (wrap < 1 || wrap > 72) {
        log::warn("[taa] jitter wrap +0x38c = %d, set to 8", wrap);
        at<int>(p, kJitterWrap) = 8;
    }
    void* fsc = at<void*>(p, kFullScreenCopy);
    if (!fsc) return refuse("no FullScreenCopy");
    g_w = rt_width(fsc);
    g_h = rt_height(fsc);
    if (g_w <= 0 || g_h <= 0) return refuse("FullScreenCopy has no size");
    static const char* const kNames[5] = {"RDRVR TAA history 1a", "RDRVR TAA history 1b", "RDRVR TAA history 2a",
                                          "RDRVR TAA history 2b", "RDRVR TAA output"};
    void* rts[5];
    for (int i = 0; i < 5; ++i) {
        rts[i] = make_target(kNames[i], g_w, g_h);
        if (!rts[i]) return refuse("the render-target factory returned null");
    }
    g_hist[0].rt[0] = rts[0];
    g_hist[0].rt[1] = rts[1];
    g_hist[1].rt[0] = rts[2];
    g_hist[1].rt[1] = rts[3];
    g_scratch = rts[4];
    g_state = 1;
    log::info("[taa] native TAA on: 5 targets %dx%d (%p %p %p %p, output %p), jitter wrap %d, params %.3f %.3f %.3f %.3f",
              g_w, g_h, rts[0], rts[1], rts[2], rts[3], rts[4], at<int>(p, kJitterWrap), at<float>(p, kTaaParams),
              at<float>(p, kTaaParams + 4), at<float>(p, kTaaParams + 8), at<float>(p, kTaaParams + 12));
}

void after_scene_wait(int pass) {
    if (!jitter_active()) return;
    char* p = postfx();
    void* vp = camera_lever::scene_viewport();
    if (!vp) return;
    if (pass <= 1) t_index_base = at<int>(p, kJitterIndex);  // PreRender's sample for this tick
    reinterpret_cast<Persp_t>(anchors::addr(Id::ViewportPerspective))(vp);  // the projection without any jitter
    bool mono_update = pass == 0 && g_mono_pass.load(std::memory_order_relaxed) && camera_lever::mono_camera_changed();
    if (pass == 0 && !mono_update) {
        g_snap[2].valid = false;  // the next changed-camera mono pass starts from PreRender's state
        reinterpret_cast<Jitter_t>(anchors::addr(Id::TaaJitterApply))(vp, reinterpret_cast<const float*>(p + kJitter));
        g_mono_jitters.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Snap& s = g_snap[mono_update ? 2 : g_shared.load(std::memory_order_relaxed) ? 1 : pass - 1];
    if (!s.valid) {  // first double frame: both passes start from PreRender's state
        std::memcpy(s.jitter, p + kJitter, sizeof(s.jitter));
        std::memcpy(s.prev, p + kPrevBlock, sizeof(s.prev));
        s.valid = true;
    }
    char delta[8];
    std::memcpy(delta, p + kJitterDelta, sizeof(delta));
    std::memcpy(p + kJitter, s.jitter, sizeof(s.jitter));
    std::memcpy(p + kPrevBlock, s.prev, sizeof(s.prev));
    at<int>(p, kJitterIndex) = t_index_base - 1;  // the function advances it first: the tick's sample again
    int screen[2] = {at<int>(vp, kVpScreenW), at<int>(vp, kVpScreenH)};
    bool units = false;
    if (g_jitter_units.load(std::memory_order_relaxed) && at<int>(p, kTechApplied) == 5) {
        if (char* d = at<char*>(p, kDlssObject)) {
            int rw = at<int>(d, kDlssRenderW), rh = at<int>(d, kDlssRenderH);
            uint32_t srw = 0, srh = 0, sew = 0, seh = 0;
            if (eye_shape::dlss_shape(&srw, &srh, &sew, &seh)) {  // [XR] EyeShapeDlss: the shaped render size (per axis)
                rw = static_cast<int>(srw);
                rh = static_cast<int>(srh);
            }
            if (rw > 0 && rh > 0 && rw <= screen[0] && rh <= screen[1] && (rw != screen[0] || rh != screen[1])) {
                at<int>(vp, kVpScreenW) = rw;
                at<int>(vp, kVpScreenH) = rh;
                units = true;
            }
        }
    }
    reinterpret_cast<TaaUpdate_t>(anchors::addr(Id::TaaUpdate))(p, vp, 0);
    if (units) {
        at<int>(vp, kVpScreenW) = screen[0];
        at<int>(vp, kVpScreenH) = screen[1];
        g_units_applied.fetch_add(1, std::memory_order_relaxed);
    }
    std::memcpy(s.jitter, p + kJitter, sizeof(s.jitter));
    std::memcpy(s.prev, p + kPrevBlock, sizeof(s.prev));
    std::memcpy(p + kJitterDelta, delta, sizeof(delta));
    (mono_update ? g_mono_updates : g_pass_updates).fetch_add(1, std::memory_order_relaxed);
}

bool skip_scene_rain(uintptr_t ret) {
    if (ret != g_scene_rain_ret || !active()) return false;
    g_rain_skips.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool set_params(const float v[4]) {
    char* p = postfx();
    if (!p) return false;
    std::memcpy(p + kTaaParams, v, 16);  // the dormant block only this resolve reads (still, moving, velocity, sharpen)
    log::info("[taa] params %.3f %.3f %.3f %.3f", v[0], v[1], v[2], v[3]);
    return true;
}

void set_shared(bool on) {
    g_shared = on;
    log::info("[taa] %s", on ? "one history for both eyes (control)" : "a history per eye");
}

void set_jitter_units(bool on) {
    g_jitter_units = on;
    log::info("[taa] DLSS jitter units: %s", on ? "render pixels (the update run at the render size)" : "the game's own (output pixels)");
}

void set_mono_pass(bool on) {
    g_mono_pass = on;
    log::info("[taa] mono pass with a changed camera: %s", on ? "own update" : "jitter only (PreRender's update)");
}

void cut_text(char* out, size_t len) {
    static const char* const kSlot[3] = {"first eye", "second eye", "mono"};
    size_t n = static_cast<size_t>(std::snprintf(out, len, "cuts (the history discarded) since start, per post run:"));
    for (int i = 0; i < 3 && n < len; ++i) {
        const Cuts& c = g_cuts[i];
        n += static_cast<size_t>(std::snprintf(out + n, len - n,
                                               " %s %llu runs: velocity cleared %llu (latch %llu: renderer+0x1fa4 %llu, +0x2524e/f %llu, "
                                               "toggle %llu; +0x3b2 %llu, +0x558 %llu)%s",
                                               kSlot[i], static_cast<unsigned long long>(c.runs.load()),
                                               static_cast<unsigned long long>(c.clears.load()), static_cast<unsigned long long>(c.latch.load()),
                                               static_cast<unsigned long long>(c.src_1fa4.load()), static_cast<unsigned long long>(c.src_2524.load()),
                                               static_cast<unsigned long long>(c.src_toggle.load()), static_cast<unsigned long long>(c.r3b2.load()),
                                               static_cast<unsigned long long>(c.r558.load()), i < 2 ? " |" : ""));
    }
}

void status_text(char* out, size_t len) {
    char* p = anchors::stand_down() ? nullptr : postfx();
    std::snprintf(out, len,
                  "taa %s (technique %d) %dx%d%s | resolves first %llu second %llu mono %llu, resets %llu | pass updates %llu, "
                  "mono jitters %llu, mono updates %llu (monopass %s) | rain: scene skips %llu, TAA restores %llu | size mismatches %llu"
                  " | dlss pass jitter %d, jitter units %s (%llu updates)",
                  g_state == 1 ? "made" : g_state < 0 ? "REFUSED" : "not made", p ? at<int>(p, kTechApplied) : -1, g_w, g_h,
                  g_shared.load() ? " SHARED (control)" : "",
                  static_cast<unsigned long long>(g_resolves[0].load()), static_cast<unsigned long long>(g_resolves[1].load()),
                  static_cast<unsigned long long>(g_resolves[2].load()), static_cast<unsigned long long>(g_resets.load()),
                  static_cast<unsigned long long>(g_pass_updates.load()), static_cast<unsigned long long>(g_mono_jitters.load()),
                  static_cast<unsigned long long>(g_mono_updates.load()), g_mono_pass.load() ? "on" : "off",
                  static_cast<unsigned long long>(g_rain_skips.load()), static_cast<unsigned long long>(g_rain_restores.load()),
                  static_cast<unsigned long long>(g_size_mismatch.load()), g_dlss_jitter.load() ? 1 : 0,
                  g_jitter_units.load() ? "render" : "output", static_cast<unsigned long long>(g_units_applied.load()));
}

}  // namespace rdrvr::taa
