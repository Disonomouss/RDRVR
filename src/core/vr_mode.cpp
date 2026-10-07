#include "core/vr_mode.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "core/api.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/d3d_hooks.h"
#include "core/log.h"
#include "core/xr.h"

namespace rdrvr::vr_mode {
namespace {

enum class View { None, Stereo, Cinema, Cutscene3D };
const char* view_name(View v) {
    switch (v) {
        case View::Stereo: return "stereo";
        case View::Cinema: return "cinema";
        case View::Cutscene3D: return "cutscene 3D";
        default: return "none";
    }
}

std::atomic<bool> g_auto{true}, g_cut3d{false};
std::atomic<int> g_force_cutscene{-1};  // test override of the cutscene state: -1 the native's, 0 off, 1 on
std::atomic<float> g_separation{1.0f};
int g_w_cutscene = -1, g_w_paused = -1, g_w_faded = -1;
// presenting thread
View g_view = View::None;
uint64_t g_last_scenes = 0, g_last_ticks = 0;
int g_no_scene = 0, g_tick_stall = 0, g_clear = 0;
char g_reason[64] = "";
std::atomic<uint64_t> g_switches{0};
std::atomic<int> g_flags{0};  // bits: 1 no scene, 2 paused, 4 stalled, 8 faded, 16 cutscene (for the status)

bool watch_true(int w) {
    uint64_t v = 0, t = 0;
    if (!api::watched(w, &v, &t)) return false;
    if (api::script_ticks() > t + 30) return false;  // stale (no tick for half a second): the stall check covers it
    return (v & 0xff) != 0;
}

void apply(View v) {
    switch (v) {
        case View::Cinema:
            camera_lever::set_double(false, 0.064f, false);
            camera_lever::set_eye_projection(-1);
            camera_lever::set_xr_pose(false);
            xr::set_cinema(true);
            break;
        case View::Stereo:
        case View::Cutscene3D:
            xr::set_cinema(false);
            camera_lever::set_xr_head_position(v == View::Stereo, v == View::Stereo ? 1.0f : g_separation.load());
            camera_lever::set_eye_projection(3);
            camera_lever::set_xr_pose(true);
            camera_lever::set_double(true, 0.064f, false);
            break;
        default: break;
    }
}

void on_frame_end(uint64_t) {
    if (!g_auto.load(std::memory_order_relaxed) || !xr::submitting()) {
        if (g_view != View::None) {
            g_view = View::None;
            log::info("[mode] automatic view modes off");
        }
        return;
    }
    uint64_t scenes = camera_lever::scene_calls(), ticks = api::script_ticks();
    bool scene = scenes != g_last_scenes;
    g_last_scenes = scenes;
    g_no_scene = scene ? 0 : g_no_scene + 1;
    g_tick_stall = ticks != g_last_ticks ? 0 : g_tick_stall + 1;
    g_last_ticks = ticks;
    bool paused = watch_true(g_w_paused), faded = watch_true(g_w_faded), cutscene = watch_true(g_w_cutscene);
    int force = g_force_cutscene.load(std::memory_order_relaxed);
    if (force >= 0) cutscene = force == 1;
    bool stalled = g_tick_stall > 20;
    bool flat = g_no_scene >= 2 || paused || stalled || faded;
    g_flags = (g_no_scene >= 2 ? 1 : 0) | (paused ? 2 : 0) | (stalled ? 4 : 0) | (faded ? 8 : 0) | (cutscene ? 16 : 0);
    bool cut3d = g_cut3d.load(std::memory_order_relaxed);
    View want = flat ? View::Cinema : cutscene ? (cut3d ? View::Cutscene3D : View::Cinema) : View::Stereo;
    if (want == g_view) {
        g_clear = 0;
        return;
    }
    // into the cinema at once; out of it (or between the stereo views) after ten frames of the new state
    if (want != View::Cinema && g_view == View::Cinema && ++g_clear < 10) return;
    g_clear = 0;
    std::snprintf(g_reason, sizeof(g_reason), "%s%s%s%s%s", g_no_scene >= 2 ? "no scene " : "", paused ? "paused " : "",
                  stalled ? "script stalled " : "", faded ? "faded " : "", cutscene ? "cutscene" : "");
    log::info("[mode] %s -> %s (%s)", view_name(g_view), view_name(want), g_reason[0] ? g_reason : "gameplay");
    g_view = want;
    g_switches.fetch_add(1, std::memory_order_relaxed);
    apply(want);
}

}  // namespace

void init() {
    g_auto = config::get_bool("XR", "AutoMode", true);
    g_cut3d = config::get_string("Screen", "CutsceneMode", "Screen") == "3D";
    g_separation = config::get_float("Screen", "Cutscene3DSeparation", 1.0f);
    g_w_cutscene = api::watch_native(0xA61FA36Bu);  // CUTSCENE_MANAGER_IS_CUTSCENE_PLAYING
    g_w_paused = api::watch_native(0x57246C02u);    // IS_GAME_PAUSED
    g_w_faded = api::watch_native(0x4EFFFC06u);     // HUD_IS_FADED
    d3d::add_frame_end_listener(on_frame_end);
}

void set_auto(bool on) {
    g_auto = on;
    log::info("[mode] automatic view modes %s", on ? "on" : "off");
}

void set_cutscene_3d(bool on) {
    g_cut3d = on;
    log::info("[mode] cutscenes: %s", on ? "3D" : "screen");
}

void set_separation(float s) {
    g_separation = s < 0 ? 0 : s > 1 ? 1 : s;
    if (g_view == View::Cutscene3D) camera_lever::set_xr_head_position(false, g_separation.load());
    log::info("[mode] 3D cutscene separation x%.2f", g_separation.load());
}

void force_cutscene(int state) {
    g_force_cutscene = state;
    log::info("[mode] cutscene state %s", state < 0 ? "from the native" : state ? "forced on (test)" : "forced off (test)");
}

bool gameplay_stereo() { return g_auto.load() && g_view == View::Stereo; }

void status_text(char* out, size_t len) {
    int f = g_flags.load();
    std::snprintf(out, len, "auto %s, view %s, cutscenes %s%s, switches %llu | now: %s%s%s%s%s", g_auto.load() ? "on" : "off",
                  view_name(g_view), g_cut3d.load() ? "3D" : "screen", g_force_cutscene.load() >= 0 ? " (FORCED)" : "",
                  static_cast<unsigned long long>(g_switches.load()),
                  f & 1 ? "no-scene " : "", f & 2 ? "paused " : "", f & 4 ? "stalled " : "", f & 8 ? "faded " : "",
                  f & 16 ? "cutscene" : "");
}

}  // namespace rdrvr::vr_mode
