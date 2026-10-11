#include "core/render_settings.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "core/anchors.h"
#include "core/config.h"
#include "core/controls.h"
#include "core/d3d_hooks.h"
#include "core/hooks.h"
#include "core/log.h"

namespace rdrvr::render_settings {
namespace {

// Menu AA value -> renderer technique (map 0x142b66a70): 0 Off -> 0, 1 FXAA -> 1, 2 FSR3 -> 4, 3 DLSS -> 5.
constexpr int kTechniqueOf[] = {0, 1, 4, 5};
constexpr uintptr_t kPostFxTechnique = 0x86c;  // PostFx: technique requested (the render thread copies it to +0x868)
constexpr uintptr_t kPostFxApplied = 0x868;    // PostFx: technique the last frame used
constexpr uintptr_t kRendererDrs = 0x570;      // Renderer: DRS on
constexpr uintptr_t kRendererScale = 0x550;    // Renderer: render scale used this frame
constexpr uintptr_t kRendererFixed = 0x5b8;    // Renderer: fixed-scale mode (DRS controller 0x1405c7d80)
constexpr uintptr_t kRendererFixedScale = 0x5bc;
constexpr uintptr_t kRendererScaleStep = 0x5ac;  // the fixed path rounds to multiples of this; 0 would make the scale NaN
constexpr uintptr_t kPostFxDlss = 0x530;         // PostFx: the DLSS object
constexpr uintptr_t kPostFxJitterWrap = 0x38c;   // PostFx: the jitter sequence's length (TaaUpdate wraps the index there)
constexpr uintptr_t kDlssOutputW = 0xac, kDlssRenderW = 0xec;  // DLSS: the output width, the render width (optimal settings)

using AaSet_t = void (*)(int* p);
AaSet_t o_AaSet = nullptr;
std::atomic<int> g_force_aa{-1};
int g_dlss_quality = 0;  // [Render] DlssQuality: the game's DLSS quality index 0..5 (0 DLAA, 1 dynamic, 2 ultra perf ... 5 quality)
std::atomic<float> g_fixed_scale{0.0f};
std::atomic<bool> g_release_scale{false};  // fixed scale switched off: hand DRS back once
std::atomic<int> g_calls{0};
std::atomic<int> g_last_game_mode{-1};

char* pointer_at(anchors::Id id) { return *reinterpret_cast<char* const*>(anchors::addr(id)); }

// DLSS's jitter sequence at its full length ([Render] DlssFullJitter). The game sets the length when the quality is
// applied (FUN_140874400: DLSS vt+0x80 = 8 x (output width / render width)^2), from the render width of the last
// optimal-settings query, which for a new quality has not run yet: at boot it is DLAA's 8 for every scale (Quality needs
// 18, Ultra Performance 72, the Halton table's whole length). Too few sample positions for the upscale: detail pulses
// with the 8-frame cycle. Set again once the render size is known, every frame it differs (a write of one int; the
// render thread wraps the index against it).
std::atomic<bool> g_full_jitter{true};
std::atomic<int> g_phases_game{0}, g_phases_set{0}, g_phases_rw{0}, g_phases_ow{0};
std::atomic<uint64_t> g_phases_fixes{0};

void fix_jitter_phases(char* postfx) {
    if (*reinterpret_cast<int*>(postfx + kPostFxApplied) != 5) return;
    char* dlss = *reinterpret_cast<char**>(postfx + kPostFxDlss);
    if (!dlss) return;
    int& wrap = *reinterpret_cast<int*>(postfx + kPostFxJitterWrap);
    if (!g_full_jitter.load(std::memory_order_relaxed)) {
        const int game = g_phases_game.load(std::memory_order_relaxed);
        if (game > 0 && wrap == g_phases_set.load(std::memory_order_relaxed)) wrap = game;  // "dlss phases off": the game's
        return;
    }
    const int ow = *reinterpret_cast<int*>(dlss + kDlssOutputW), rw = *reinterpret_cast<int*>(dlss + kDlssRenderW);
    if (rw <= 0 || ow <= 0 || rw > ow || ow > 16384) return;
    const float k = static_cast<float>(ow) / static_cast<float>(rw);
    // the game's own formula (vt+0x80) with the render width now known, rounded: the render width is rounded up, so
    // Ultra Performance's 3 comes out 2.999 (71.96 samples)
    int want = static_cast<int>(8.0f * k * k + 0.5f);
    if (want < 8) want = 8;
    if (want > 72) want = 72;  // the Halton table (PostFx+0x130..+0x36f)
    if (wrap == want) return;
    if (wrap != g_phases_set.load(std::memory_order_relaxed)) g_phases_game = wrap;  // the game's own value (it may set it again)
    log::info("[render] DLSS jitter sequence: %d samples (the game's %d), render width %d of %d (8 x %.3f^2)", want, wrap, rw, ow, k);
    wrap = want;
    g_phases_set = want;
    g_phases_rw = rw;
    g_phases_ow = ow;
    g_phases_fixes.fetch_add(1, std::memory_order_relaxed);
}

void on_jitter_frame_end(uint64_t) {
    if (anchors::stand_down()) return;
    if (char* postfx = pointer_at(anchors::Id::PostFxSingleton)) fix_jitter_phases(postfx);
}

void hk_AaSet(int* p) {
    int copy[6];
    std::memcpy(copy, p, sizeof(copy));
    g_last_game_mode = p[0];
    if (g_force_aa >= 0) {
        int force = g_force_aa.load();
        copy[0] = force == 2 ? 1 : force;  // native TAA keeps the menu at FXAA: no DLSS/FSR setup, scale 1.0
        copy[1] = 0;                           // no frame generation
        copy[5] = g_fixed_scale.load() > 0 ? 1 : 0;   // DRS only for the fixed-scale test
        if (force == 3) {
            copy[2] = g_dlss_quality;  // within 0..5 (the setter's map throws past it)
            copy[5] = 0;               // no DRS with DLSS
        }
    }
    int n = g_calls.fetch_add(1) + 1;
    if (n <= 8)
        log::info("[render] AA setter: game {mode %d, framegen %d, dlss %d, fsr %d, sharpen %d, drs %d} -> mode %d drs %d",
                  p[0], p[1], p[2], p[3], p[4], p[5], copy[0], copy[5]);
    o_AaSet(copy);
}

// ---- run 10 items 8c and 8d (research\run10\motionblur-heathaze.md)
// [Render] MotionBlur=off (the user: "can the mod edit the games setting on install to turn off motion blur?"): the
// option's setter 0x1405d2ac0 sets PostFx+0x5a4 (style 2, the improved motion blur: the post chain skips its pass and the
// velocity pass when 0) and +0xe1c (style 1, the speed blur); both kept 0 each frame, the options struct and the xml never
// written. (The game's menu getter reads these flags: while a game menu is open the game's own style is put back in
// them, so its menu shows and saves the player's setting.)
constexpr uintptr_t kPostFxImprovedBlur = 0x5a4, kPostFxSpeedBlur = 0xe1c;
std::atomic<bool> g_blur_off{true};
std::atomic<uint64_t> g_blur_cleared{0};
// [Render] HeatHaze=0 (the user: some users find the heat haze uncomfortable in VR): the time of day's HeatShimmer
// strength (PostFx+0xe48) zeroed at the post chain's first reader, the Mirager downsample (PostFx vt+0x98), so it and the
// later readers (the shimmer pass, HeatShimmerParams) see 0; the time of day writes it again before the next frame
constexpr uintptr_t kPostFxShimmer = 0xe48;
std::atomic<bool> g_haze_on{true};
std::atomic<uint64_t> g_haze_calls{0}, g_haze_cleared{0};
std::atomic<float> g_haze_last{0.0f};  // the strength the game had set at the last call
std::atomic<float> g_haze_test{-1.0f};
std::atomic<int> g_style_req{-1};  // "postfx style <n>": the game's motion blur style set at the next frame end (a test's)  // "postfx hazetest <v>": the strength forced before the switch's check (a test's desert; -1 none)
using Mirager_t = uint64_t (*)(void*, uint64_t, uint64_t, uint64_t);
Mirager_t o_Mirager = nullptr;
uint64_t hk_Mirager(void* postfx, uint64_t a2, uint64_t a3, uint64_t a4) {
    if (postfx) {
        float* s = reinterpret_cast<float*>(static_cast<char*>(postfx) + kPostFxShimmer);
        if (const float t = g_haze_test.load(std::memory_order_relaxed); t >= 0.0f) *s = t;
        g_haze_last.store(*s, std::memory_order_relaxed);
        g_haze_calls.fetch_add(1, std::memory_order_relaxed);
        if (!g_haze_on.load(std::memory_order_relaxed) && *s != 0.0f) {
            *s = 0.0f;
            g_haze_cleared.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return o_Mirager(postfx, a2, a3, a4);
}
void on_post_effects(uint64_t) {
    if (anchors::stand_down() || !anchors::present(anchors::Id::MotionBlurSetter)) return;
    char* postfx = pointer_at(anchors::Id::PostFxSingleton);
    if (!postfx) return;
    if (const int st = g_style_req.exchange(-1); st >= 0) {  // a test's: set as the game's setter does (here, not on the test's thread)
        *reinterpret_cast<uint8_t*>(postfx + kPostFxImprovedBlur) = st == 2 ? 1 : 0;
        *reinterpret_cast<uint8_t*>(postfx + kPostFxSpeedBlur) = st == 1 ? 1 : 0;
        log::info("[render] test: the motion blur style %d set (as the game's setter does)", st);
    }
    if (!g_blur_off.load(std::memory_order_relaxed)) return;
    uint8_t* a = reinterpret_cast<uint8_t*>(postfx + kPostFxImprovedBlur);
    if (controls::game_menu_open() && anchors::present(anchors::Id::GraphicsOptions)) {
        // a game menu open (the graphics page reads these flags): the game's own style back, so an Apply there saves the
        // user's setting, never the mod's Off (the review's); GraphicsOptions +0x21 MotionBlurStyle (0 off, 1, 2)
        const uint8_t style = *reinterpret_cast<const uint8_t*>(anchors::addr(anchors::Id::GraphicsOptions) + 0x21);
        *a = style == 2 ? 1 : 0;
        *reinterpret_cast<uint8_t*>(postfx + kPostFxSpeedBlur) = style == 1 ? 1 : 0;
        return;
    }
    uint8_t* b = reinterpret_cast<uint8_t*>(postfx + kPostFxSpeedBlur);
    if (*a || *b) {
        *a = 0;
        *b = 0;
        g_blur_cleared.fetch_add(1, std::memory_order_relaxed);
    }
}

void on_frame_end(uint64_t) {
    if (anchors::stand_down()) return;
    char* postfx = pointer_at(anchors::Id::PostFxSingleton);
    int force = g_force_aa;
    if (postfx && force >= 0) {
        int want = force == 2 ? 2 : kTechniqueOf[force];  // 2 = technique 2, the engine's TAA (the mod resolves it)
        int* tech = reinterpret_cast<int*>(postfx + kPostFxTechnique);
        if (*tech != want) *tech = want;
    }
    char* renderer = pointer_at(anchors::Id::RendererSingleton);
    float scale = g_fixed_scale.load();
    if (renderer && scale > 0) {
        *reinterpret_cast<bool*>(renderer + kRendererDrs) = true;
        *reinterpret_cast<bool*>(renderer + kRendererFixed) = true;
        *reinterpret_cast<float*>(renderer + kRendererFixedScale) = scale;
    } else if (renderer && g_release_scale.exchange(false)) {
        *reinterpret_cast<bool*>(renderer + kRendererFixed) = false;
        *reinterpret_cast<bool*>(renderer + kRendererDrs) = false;  // the user's DRS is off (forced AA path)
    }
}

}  // namespace

bool install() {
    {  // run 10 items 8c and 8d: before the AA's early return (independent of it)
        g_blur_off = config::get_string("Render", "MotionBlur", "off") != "game";
        g_haze_on = config::get_bool("Render", "HeatHaze", true);
        if (!anchors::stand_down()) {
            if (anchors::present(anchors::Id::MotionBlurSetter)) d3d::add_frame_end_listener(on_post_effects);
            if (anchors::present(anchors::Id::MiragerDownsample))
                hooks::install("RDR Mirager downsample (the heat haze)", reinterpret_cast<void*>(anchors::addr(anchors::Id::MiragerDownsample)),
                               hk_Mirager, &o_Mirager);
        }
        log::info("[render] motion blur %s; the heat haze %s", g_blur_off.load() ? "forced off (in memory)" : "the game's setting",
                  g_haze_on.load() ? "the game's" : "off");
    }
    g_full_jitter = config::get_bool("Render", "DlssFullJitter", true);
    if (!anchors::stand_down()) d3d::add_frame_end_listener(on_jitter_frame_end);  // DLSS from the game's menu too
    g_force_aa = config::get_int("Render", "ForceAntiAliasing", -1);
    if (g_force_aa > 3) g_force_aa = 1;  // Off, FXAA, native TAA and DLSS are forced; other values are not valid targets here
    {
        const int q = config::get_int("Render", "DlssQuality", 0);
        g_dlss_quality = q < 0 ? 0 : q > 5 ? 5 : q;
    }
    float scale = config::get_float("Debug", "FixedRenderScale", 0.0f);
    g_fixed_scale = (scale >= 0.25f && scale <= 1.0f) ? scale : 0.0f;
    if (g_force_aa < 0) return true;  // nothing to force: no hook (the fixed scale needs the forced FXAA/Off path)
    if (anchors::stand_down()) return false;
    bool ok = hooks::install("RDR AA/upscaler setter", reinterpret_cast<void*>(anchors::addr(anchors::Id::AaSetter)), hk_AaSet,
                             &o_AaSet);
    d3d::add_frame_end_listener(on_frame_end);
    log::info("[render] forcing AA %s (DLSS quality index %d), fixed render scale %s",
              g_force_aa < 0 ? "no" : g_force_aa == 0 ? "Off" : g_force_aa == 1 ? "FXAA" : g_force_aa == 2 ? "native TAA (technique 2)"
                                                                                                           : "DLSS (technique 5)",
              g_dlss_quality,
              g_fixed_scale.load() > 0 ? "on" : "off");
    return ok;
}

bool set_aa(int mode) {
    // only while the core forces the AA mode; DLSS (3) is set at boot only: switching to or from it at run time re-creates
    // the game's swapchain (FUN_140fd06d0), which the XR session holds
    if (g_force_aa < 0 || g_force_aa == 3 || mode < 0 || mode > 2) return false;
    g_force_aa = mode;
    log::info("[render] AA now %s", mode == 0 ? "Off" : mode == 1 ? "FXAA" : "native TAA (technique 2)");
    return true;
}

int forced_aa() { return g_force_aa.load(); }

bool motion_blur_off() { return g_blur_off.load(); }
void set_motion_blur_off(bool off, bool save) {
    if (g_blur_off.exchange(off) != off) log::info("[render] motion blur %s", off ? "forced off" : "the game's setting (from its next Apply or start)");
    if (save) config::set("Render", "MotionBlur", off ? "off" : "game");
}
void post_test(const char* what, float v) {  // the test channel's: "style" (the game's motion blur style 0..2 set as its setter does), "hazetest"
    char* postfx = anchors::stand_down() ? nullptr : pointer_at(anchors::Id::PostFxSingleton);
    if (!std::strcmp(what, "hazetest")) {
        g_haze_test = v;
        return;
    }
    if (!std::strcmp(what, "style") && postfx && anchors::present(anchors::Id::MotionBlurSetter)) g_style_req = static_cast<int>(v);
}
bool heat_haze_on() { return g_haze_on.load(); }
void set_heat_haze(bool on, bool save) {
    if (g_haze_on.exchange(on) != on) log::info("[render] the heat haze %s", on ? "the game's" : "off");
    if (save) config::set("Render", "HeatHaze", on ? "1" : "0");
}
void post_text(char* out, size_t len) {
    char* postfx = anchors::stand_down() ? nullptr : pointer_at(anchors::Id::PostFxSingleton);
    std::snprintf(out, len,
                  "postfx: motion blur %s (anchor %d), the flags +0x5a4 %d +0xe1c %d, cleared %llu | heat haze %s (anchor %d), the strength at the pass %.3f, "
                  "now %.3f, calls %llu, cleared %llu",
                  g_blur_off.load() ? "off" : "game", anchors::present(anchors::Id::MotionBlurSetter) ? 1 : 0,
                  postfx ? *reinterpret_cast<uint8_t*>(postfx + kPostFxImprovedBlur) : -1, postfx ? *reinterpret_cast<uint8_t*>(postfx + kPostFxSpeedBlur) : -1,
                  static_cast<unsigned long long>(g_blur_cleared.load()), g_haze_on.load() ? "on" : "off",
                  anchors::present(anchors::Id::MiragerDownsample) ? 1 : 0, g_haze_last.load(),
                  postfx ? *reinterpret_cast<float*>(postfx + kPostFxShimmer) : -1.0f, static_cast<unsigned long long>(g_haze_calls.load()),
                  static_cast<unsigned long long>(g_haze_cleared.load()));
}

void set_full_jitter(bool on) {
    g_full_jitter = on;
    log::info("[render] DLSS jitter sequence: %s", on ? "the full length for the scale" : "the game's own length");
}

void jitter_text(char* out, size_t len) {
    char* postfx = anchors::stand_down() ? nullptr : pointer_at(anchors::Id::PostFxSingleton);
    std::snprintf(out, len, "dlss jitter: full length %s, now %d samples (the game's %d; set %d for render width %d of %d, %llu times)",
                  g_full_jitter.load() ? "on" : "off", postfx ? *reinterpret_cast<int*>(postfx + kPostFxJitterWrap) : -1,
                  g_phases_game.load(), g_phases_set.load(), g_phases_rw.load(), g_phases_ow.load(),
                  static_cast<unsigned long long>(g_phases_fixes.load()));
}
int dlss_quality() { return g_dlss_quality; }

bool set_fixed_scale(float scale) {
    if (g_force_aa < 0) return false;
    if (scale != 0 && (scale < 0.25f || scale > 1.0f)) return false;
    if (scale != 0) {
        char* renderer = anchors::stand_down() ? nullptr : pointer_at(anchors::Id::RendererSingleton);
        float step = renderer ? *reinterpret_cast<float*>(renderer + kRendererScaleStep) : 0.0f;
        if (!(step > 0.001f && step <= 0.5f)) {
            log::warn("[render] fixed render scale refused: DRS step %f is not usable", step);
            return false;
        }
    }
    if (scale == 0 && g_fixed_scale.load() > 0) g_release_scale = true;
    g_fixed_scale = scale;
    log::info("[render] fixed render scale %.3f", scale);
    return true;
}

void status_text(char* out, size_t len) {
    if (anchors::stand_down()) {
        std::snprintf(out, len, "stand down");
        return;
    }
    char* postfx = pointer_at(anchors::Id::PostFxSingleton);
    char* renderer = pointer_at(anchors::Id::RendererSingleton);
    std::snprintf(out, len, "technique req %d applied %d, render scale %.3f (step %.3f), drs %d, setter calls %d (game mode %d), forced aa %d",
                  postfx ? *reinterpret_cast<int*>(postfx + kPostFxTechnique) : -1,
                  postfx ? *reinterpret_cast<int*>(postfx + kPostFxApplied) : -1,
                  renderer ? *reinterpret_cast<float*>(renderer + kRendererScale) : 0.0f,
                  renderer ? *reinterpret_cast<float*>(renderer + kRendererScaleStep) : 0.0f,
                  renderer ? *reinterpret_cast<bool*>(renderer + kRendererDrs) : 0, g_calls.load(), g_last_game_mode.load(),
                  g_force_aa.load());
}

}  // namespace rdrvr::render_settings
