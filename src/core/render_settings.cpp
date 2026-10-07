#include "core/render_settings.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "core/anchors.h"
#include "core/config.h"
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

using AaSet_t = void (*)(int* p);
AaSet_t o_AaSet = nullptr;
std::atomic<int> g_force_aa{-1};
int g_dlss_quality = 0;  // [Render] DlssQuality: the game's DLSS quality index 0..5 (0 DLAA, 1 dynamic, 2 ultra perf ... 5 quality)
std::atomic<float> g_fixed_scale{0.0f};
std::atomic<bool> g_release_scale{false};  // fixed scale switched off: hand DRS back once
std::atomic<int> g_calls{0};
std::atomic<int> g_last_game_mode{-1};

char* pointer_at(anchors::Id id) { return *reinterpret_cast<char* const*>(anchors::addr(id)); }

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
