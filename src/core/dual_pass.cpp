#include "core/dual_pass.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/anchors.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/d3d_hooks.h"
#include "core/eye_shape.h"
#include "core/frame_probe.h"
#include "core/ring_probe.h"
#include "core/hooks.h"
#include "core/taa.h"
#include "core/log.h"

// draw_masks.asm
extern "C" void rdrvr_draw_masks_stub();
extern "C" void* rdrvr_draw_masks_original = nullptr;
extern "C" volatile unsigned char rdrvr_draw_masks_on = 1;

namespace rdrvr::dual_pass {
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

thread_local int t_pass = 0;               // 1 = first pass of a double frame, 2 = second
thread_local bool t_ui_wait_taken = false;  // RenderFrame's UI wait was taken between the passes this frame
thread_local bool t_double = false;         // this RenderFrame is a double frame (until its own post run ends)
thread_local bool t_exposure_kept = false;  // this frame's first-eye run adapted the exposure and kept it
thread_local bool t_skip_adapt = false;     // inside the second eye's run: its adaptation pass is skipped
thread_local void* t_first_lum = nullptr;   // PostFx+0x80 after the first-eye run: the adapted value both eyes read
thread_local int t_adapt_draws = 0;         // adaptation passes that ran in this double frame
thread_local int t_rain_draws = 0;          // rain draws so far in this double frame

enum Split {
    kGrass, kGust, kLights, kForest, kPost, kMasks, kExposure, kRain, kPfxMap, kGodRays, kDamage, kClouds, kSunVis, kHold,
    kShadows, kShadowUnion, kSplitCount
};
const char* const kSplitName[kSplitCount] = {"grass",   "gust",   "lights", "forest", "post",   "masks",  "exposure",
                                             "rain",    "pfxmap", "godrays", "damage", "clouds", "sunvis", "hold",
                                             "shadows", "shadowunion"};
// the splits off unless the ini turns them on: a measurement aid and the shared shadows' test control (the shared shadows
// themselves on since the headset session of 2026-10-10)
bool split_default(int i) { return i != kHold && i != kShadowUnion; }
std::atomic<bool> g_on[kSplitCount];
std::atomic<uint64_t> g_applied[kSplitCount];
bool g_masks_hooked = false;

// ---- SceneRender's semaphores. The helpers (0x140ebd8c0 wait, 0x140ebd830 release) take the semaphore object; its
// handle is at +0. SceneRender waits on renderer+0x88 (frame data ready, site 0x5c389d) and releases renderer+0x68
// (scene done, site 0x5c4416). In a double frame the first pass must not release (the update thread would start
// recycling the visibility lists the second pass still draws) and the second must not wait again (nobody releases
// twice: it deadlocked on 2026-10-03, cycle 8). Both skip only when called from those two sites in the matching pass.
using SemWait_t = bool (*)(void* sem, int timeout);
using SemRelease_t = bool (*)(void* sem);
SemWait_t o_SemWait = nullptr;
SemRelease_t o_SemRelease = nullptr;
uintptr_t g_wait_ret = 0, g_release_ret = 0, g_ui_wait_ret = 0;
// The single-pass probe's other wait sites (frame_probe.h): RenderFrame's +0x40 (two) and +0x80, PreRender's +0x18 on
// the main thread, the playback loop's idle wait
uintptr_t g_lock_ret_a = 0, g_lock_ret_b = 0, g_frame_wait_ret = 0, g_main_wait_ret = 0, g_play_wait_ret = 0;

// A timed wait for the probe; the main thread's frame mark after its wait for the render thread
bool timed_wait(void* sem, int timeout, frame_probe::Slot s) {
    const double t0 = log::now_ms();
    const bool got = o_SemWait(sem, timeout);
    frame_probe::add(s, log::now_ms() - t0);
    if (s == frame_probe::kMainWait) frame_probe::thread_frame(frame_probe::kMain);
    return got;
}
std::atomic<uint64_t> g_skipped_waits{0}, g_skipped_releases{0}, g_ui_taken{0}, g_ui_skipped{0}, g_ui_timeouts{0},
    g_post_skipped{0};
std::atomic<bool> g_dlss_first_eye{true};  // [Render] DlssFirstEye: the first-eye post run under DLSS (technique 5)

bool hk_SemWait(void* sem, int timeout) {
    uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (ret == g_wait_ret) {
        bool got = true;
        if (t_pass == 2)
            g_skipped_waits.fetch_add(1, std::memory_order_relaxed);  // the first pass already waited for this frame's data
        else if (frame_probe::on())
            got = timed_wait(sem, timeout, frame_probe::kDataWait);
        else
            got = o_SemWait(sem, timeout);
        // The frame's data is in (PreRender's TAA update for the tick is done) and the viewport holds the pass's camera.
        taa::after_scene_wait(t_pass);
        return got;
    }
    if (t_ui_wait_taken && ret == g_ui_wait_ret) {
        t_ui_wait_taken = false;
        g_ui_skipped.fetch_add(1, std::memory_order_relaxed);
        return true;  // taken between the passes, for the first eye's post run
    }
    if (frame_probe::on()) {
        if (ret == g_ui_wait_ret) return timed_wait(sem, timeout, frame_probe::kUiWait);
        if (ret == g_play_wait_ret) return timed_wait(sem, timeout, frame_probe::kPlayIdle);
        if (ret == g_lock_ret_a || ret == g_lock_ret_b) return timed_wait(sem, timeout, frame_probe::kLockWait);
        if (ret == g_frame_wait_ret) return timed_wait(sem, timeout, frame_probe::kFrameWait);
        if (ret == g_main_wait_ret) return timed_wait(sem, timeout, frame_probe::kMainWait);
    }
    return o_SemWait(sem, timeout);
}

bool hk_SemRelease(void* sem) {
    if (t_pass == 1 && reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_release_ret) {
        g_skipped_releases.fetch_add(1, std::memory_order_relaxed);
        return true;  // the second pass releases, once
    }
    return o_SemRelease(sem);
}

// ---- the shared shadow passes (dual_pass.h shadows_wanted; research\sps\measure.md, step 2). Each renderer is keyed
// on its one call site in SceneRender's tree; any other caller (another shadow object's virtual render) runs as is.
// All three are draw paths that refill nothing (read in Ghidra: no pool, queue or ring is returned to there), so
// running them once a frame is what a mono frame does.
using CascadeRender_t = void (*)(void* cascades, void* a2, void* light_dir, void* a4, uint64_t a5);
// every register argument forwarded (the spot renderer's prologue keeps r9b: a fourth one)
using FacesRender_t = void (*)(void* faces, void* a2, void* a3, void* a4);
using SpotRender_t = void (*)(void* spot, void* a2, void* params, void* a4);
using SetCurrent_t = void* (*)(void* vp, char push, void* p3, char p4);
CascadeRender_t o_CascadeRender = nullptr;
FacesRender_t o_FacesRender = nullptr;
SpotRender_t o_SpotRender = nullptr;
enum ShadowKind { kCascades, kFaces, kSpot, kShadowKinds };
const char* const kShadowName[kShadowKinds] = {"cascades", "faces", "spot"};
uintptr_t g_shadow_ret[kShadowKinds] = {};
thread_local bool t_shadow_done[kShadowKinds] = {};  // this double frame's first pass ran it with the union viewport
std::atomic<uint64_t> g_shadow_union[kShadowKinds], g_shadow_skipped[kShadowKinds], g_shadow_no_union[kShadowKinds];

// The pass's handling of shadow renderer k; true: the caller runs the original with the eye's viewport (as vanilla)
enum class ShadowAct { Original, Skip, Union };
ShadowAct shadow_act(int k, uintptr_t ret) {
    if (ret != g_shadow_ret[k] || t_pass == 0) return ShadowAct::Original;
    const bool share = g_on[kShadows].load(std::memory_order_relaxed);
    if (!share && !g_on[kShadowUnion].load(std::memory_order_relaxed)) return ShadowAct::Original;
    if (t_pass == 2 && share && t_shadow_done[k]) return ShadowAct::Skip;
    if (!camera_lever::shadow_union_vp()) {
        g_shadow_no_union[k].fetch_add(1, std::memory_order_relaxed);
        return ShadowAct::Original;  // no union this frame (no XR pose): both passes as vanilla
    }
    return ShadowAct::Union;
}
template <class Run>
void shadow_run(int k, ShadowAct act, Run run) {
    if (act == ShadowAct::Skip) {
        g_shadow_skipped[k].fetch_add(1, std::memory_order_relaxed);
        g_applied[kShadows].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (act == ShadowAct::Original) {
        run();
        return;
    }
    auto set_current = reinterpret_cast<SetCurrent_t>(anchors::addr(Id::ViewportSetCurrent));
    void* eye = set_current(camera_lever::shadow_union_vp(), 1, nullptr, 0);  // the union's globals pushed
    run();
    set_current(eye, 1, nullptr, 0);  // the eye's viewport and globals again, as after any shared pass
    if (t_pass == 1) t_shadow_done[k] = true;
    g_shadow_union[k].fetch_add(1, std::memory_order_relaxed);
    g_applied[kShadowUnion].fetch_add(1, std::memory_order_relaxed);
}
void hk_CascadeRender(void* cascades, void* a2, void* light_dir, void* a4, uint64_t a5) {
    const ShadowAct act = shadow_act(kCascades, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    shadow_run(kCascades, act, [&] { o_CascadeRender(cascades, a2, light_dir, a4, a5); });
}
void hk_FacesRender(void* faces, void* a2, void* a3, void* a4) {
    const ShadowAct act = shadow_act(kFaces, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    shadow_run(kFaces, act, [&] { o_FacesRender(faces, a2, a3, a4); });
}
void hk_SpotRender(void* spot, void* a2, void* params, void* a4) {
    const ShadowAct act = shadow_act(kSpot, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    shadow_run(kSpot, act, [&] { o_SpotRender(spot, a2, params, a4); });
}

// ---- split state (the grass agent's study, ENGINE-NOTES item 6)
using ForestDraw_t = void (*)(void* forest, char mode);
using LightsUpdate_t = void (*)(void* lights);
using ForestFrame_t = void (*)(void* forest, void* renderer);
ForestDraw_t o_ForestDraw = nullptr;
LightsUpdate_t o_LightsUpdate = nullptr;
ForestFrame_t o_ForestFrame = nullptr;

constexpr size_t kGrassRuns = 0xb0c0, kGrassInstances = 0xa0b8;  // grass manager: run and instance counts
constexpr size_t kForestFactor = 0xf2218;                         // forest: adaptive factor (published to a global)
constexpr size_t kGustBytes = 0x30;                              // 0x1423dc750..77f: gust uniforms, prev, cur
alignas(16) char g_gust[kGustBytes];

// FUN_1405a3020(forest, mode): mode 1 draws the grass instances (and shifts the gust), mode 0 composites them and then
// zeroes the frame's counts (0x1405a3398/0x1405a33a2), so a second pass would draw no grass.
// Round 3: the forest manager's billboards (rage_grass VS_Grass/High/Low, embedded in RDR.exe at 0x23dff59..) expand
// each quad by gViewInverse rows 0 and 1, the eye's right and up: in the headset they tilt with the head. For the draw
// the current viewport's camera copy (+0x140, ViewInverse's source) gets those rows levelled and is pushed; the
// per-draw world pushes (flag 0) keep it; afterwards the rows are put back and pushed again.
using PushGlobals_t = void (*)(void* vp, char push_view_inverse);
std::atomic<uint64_t> g_grass_level_draws{0};

void forest_draw(void* forest, char mode);

void hk_ForestDraw(void* forest, char mode) {
    char* vp = global<char*>(Id::ViewportCurrent);
    if (mode != 1 || !vp || !camera_lever::level_grass_active()) {
        forest_draw(forest, mode);
        return;
    }
    float* vi = reinterpret_cast<float*>(vp + 0x140);
    float saved[8];
    std::memcpy(saved, vi, sizeof(saved));
    float bx = vi[8], bz = vi[10];
    camera_lever::game_heading(&bx, &bz);  // the game camera's heading, not the head's (round 3 correction)
    float n = std::sqrt(bx * bx + bz * bz);
    if (n < 1e-4f) {
        forest_draw(forest, mode);
        return;
    }
    bx /= n;
    bz /= n;
    const float level[8] = {bz, 0, -bx, saved[3], 0, 1, 0, saved[7]};
    auto push = reinterpret_cast<PushGlobals_t>(anchors::addr(Id::ViewportPushGlobals));
    std::memcpy(vi, level, sizeof(level));
    push(vp, 1);
    forest_draw(forest, mode);
    std::memcpy(vi, saved, sizeof(saved));
    push(vp, 1);
    if (g_grass_level_draws.fetch_add(1, std::memory_order_relaxed) == 0)
        log::info("[dual] forest/grass billboards drawn upright (level camera rows)");
}

void forest_draw(void* forest, char mode) {
    char* g = global<char*>(Id::GrassManager);
    if (t_pass == 1 && mode == 0 && g && g_on[kGrass].load(std::memory_order_relaxed)) {
        int runs = at<int>(g, kGrassRuns), instances = at<int>(g, kGrassInstances);
        o_ForestDraw(forest, mode);
        at<int>(g, kGrassRuns) = runs;
        at<int>(g, kGrassInstances) = instances;
        g_applied[kGrass].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    o_ForestDraw(forest, mode);
}

// FUN_14062b480, first thing in SceneRender: advances light animations and flicker phases by the frame's dt.
void hk_LightsUpdate(void* lights) {
    if (t_pass == 2 && g_on[kLights].load(std::memory_order_relaxed)) {
        g_applied[kLights].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    o_LightsUpdate(lights);
}

// FUN_1405a5c30: time-of-day factors, queued imposter work, and the adaptive forest factor (+0xf2218, published to
// 0x14227ca18) from the GPU timers of the frame slot it drains. It also returns those timers to the pool every tree draw
// pops one from (FUN_14059f880, unchecked), so it must run in every pass: skipping it in pass 2 emptied the pool and
// crashed on the first double frame (2026-10-04, cycle 12). In pass 2 the factor it computes from the first pass's
// still-pending timers is put back.
void hk_ForestFrame(void* forest, void* renderer) {
    if (t_pass == 2 && forest && g_on[kForest].load(std::memory_order_relaxed)) {
        float factor = at<float>(forest, kForestFactor);
        o_ForestFrame(forest, renderer);
        at<float>(forest, kForestFactor) = factor;
        global<float>(Id::ForestFactorGlobal) = factor;
        g_applied[kForest].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    o_ForestFrame(forest, renderer);
}


// ---- exposure. A post run adapts the exposure in three steps (R4 study, ENGINE-NOTES item 7): it writes this run's
// average luminance into the next texel of a 16-texel ring (cursor 0x142c8acbc/c0, count 0x14241c678), swaps the
// adapted luminance pair PostFx+0x80/+0x88, and draws CalculateAdaptedLuminance into the new +0x80 from the previous
// value (+0x88) and the ring (FUN_14086a4f0, returning to 0x14067a5e6). The bright pass and the tonemap then read +0x80.
//  - exposure (split, on by default): one writer. The first eye's run adapts and keeps its result. The second eye's
//    run is put back on the same pair (swapped back first, so that its own swap lands on the first run's target), its
//    adaptation pass is skipped, and its ring step is undone afterwards (the texel it wrote is the one the next frame's
//    first run overwrites before any adaptation reads it). Both eyes tonemap with one value, and the exposure
//    advances once per frame, from the first eye's view.
//  - hold (a measurement aid, off by default): every post run's exposure step is undone afterwards (the adapted
//    luminance A/B swap and the luminance ring cursor), so the exposure stays where it was and a truth capture's mono
//    references and the double pair share one exposure state (with one exposure for both eyes, an eye otherwise
//    differs from a single-view render at its pose by that view's own adaptation: cycles 15-16). Hold wins.
using PostRun_t = void (*)(void* postfx, int arg);
using PostDrawPass_t = void (*)(void* postfx, void* dst, void* src, uintptr_t technique, uintptr_t var, uintptr_t a6,
                                uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11, uintptr_t a12,
                                uintptr_t a13, uintptr_t a14);
PostRun_t o_PostRun = nullptr;
PostDrawPass_t o_PostDrawPass = nullptr;
uintptr_t g_adapt_ret = 0;
std::atomic<uint64_t> g_adapt_hist[4], g_adapt_skipped{0}, g_lum_same{0}, g_lum_differ{0};

struct Ring {
    int count, x, y;
};
Ring ring_now() { return {global<int>(Id::LumRingCount), global<int>(Id::LumRingX), global<int>(Id::LumRingY)}; }
void ring_set(const Ring& r) {
    global<int>(Id::LumRingCount) = r.count;
    global<int>(Id::LumRingX) = r.x;
    global<int>(Id::LumRingY) = r.y;
}

void post_run(void* postfx, int arg);

// [XR] EyeShape (eye_shape.h): the post output's logical size is the eye's for this run only (both eyes' runs come
// through here: first_eye_post calls the vtable's +0x30, RenderFrame its own), so the UI pass after it sees W x H.
void hk_PostRun(void* postfx, int arg) {
    eye_shape::RunPoke shape;
    eye_shape::begin_run(postfx, &shape);
    post_run(postfx, arg);
    eye_shape::end_run(postfx, shape);
}

void post_run(void* postfx, int arg) {
    bool second = t_pass == 0 && t_exposure_kept;  // RenderFrame's own run after a first-eye run that kept its exposure
    bool frame_end = t_pass == 0 && t_double;       // the double frame's last post run
    if (t_pass == 0) t_exposure_kept = false;
    char* p = static_cast<char*>(postfx);
    if (!postfx) {
        o_PostRun(postfx, arg);
    } else if (g_on[kHold].load(std::memory_order_relaxed)) {
        char ab[16];
        std::memcpy(ab, p + 0x80, 16);
        Ring r = ring_now();
        o_PostRun(postfx, arg);
        std::memcpy(p + 0x80, ab, 16);
        ring_set(r);
        g_applied[kHold].fetch_add(1, std::memory_order_relaxed);
    } else if (second) {
        std::swap(at<void*>(p, 0x80), at<void*>(p, 0x88));
        Ring r = ring_now();
        t_skip_adapt = true;
        o_PostRun(postfx, arg);
        t_skip_adapt = false;
        ring_set(r);
        (at<void*>(p, 0x80) == t_first_lum ? g_lum_same : g_lum_differ).fetch_add(1, std::memory_order_relaxed);
        g_applied[kExposure].fetch_add(1, std::memory_order_relaxed);
    } else {
        o_PostRun(postfx, arg);
    }
    if (frame_end) {
        t_double = false;
        g_adapt_hist[t_adapt_draws < 3 ? t_adapt_draws : 3].fetch_add(1, std::memory_order_relaxed);
    }
}

// FUN_14086a4f0, every full-screen pass of the chain; only the adaptation pass (by its return address) is looked at.
void hk_PostDrawPass(void* postfx, void* dst, void* src, uintptr_t technique, uintptr_t var, uintptr_t a6, uintptr_t a7,
                     uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11, uintptr_t a12, uintptr_t a13, uintptr_t a14) {
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_adapt_ret) {
        if (t_skip_adapt) {
            g_adapt_skipped.fetch_add(1, std::memory_order_relaxed);
            return;  // the second eye reads the first eye's adapted value
        }
        if (t_double) ++t_adapt_draws;
    }
    o_PostDrawPass(postfx, dst, src, technique, var, a6, a7, a8, a9, a10, a11, a12, a13, a14);
}

// ---- particles (R4b, R4 study). The CPU particles (rmptfx) are stepped once per tick on the update thread and only
// drawn per pass. Three GPU-side pieces run per SceneRender call:
//  - the GPU rain step FUN_140720960 (when it rains; FUN_140a456b0 from the master camera, swapping its position
//    targets): stepped twice, each eye would draw a different particle state. Switch rain: pass 1 only.
//  - the rain draw FUN_140720dd0 first smooths the master camera's velocity (0x142c444b0..cf, reset flag 0x142ad1f74),
//    which stretches the drops: a second call would see no motion. Switch rain: put back before every later draw of
//    the frame (the draw runs in SceneRender for FXAA, in the post chain for techniques 4 and up). The draw itself
//    takes the current (eye) viewport.
//  - the particle collision map FUN_140648c40 renders a top-down depth view and shifts its prev <- cur transforms on
//    every call. Switch pfxmap: pass 1 only (pass 2's particles collide with the same map).
using RainUpdate_t = void (*)();
using RainDraw_t = void (*)(void* rain);
using PfxMap_t = void (*)(void* map);
RainUpdate_t o_RainUpdate = nullptr;
RainDraw_t o_RainDraw = nullptr;
PfxMap_t o_PfxMap = nullptr;
constexpr size_t kRainSmootherBytes = 0x20;
char g_rain_smoother[kRainSmootherBytes];  // render thread only
char g_rain_reset = 0;
std::atomic<uint64_t> g_rain_steps{0}, g_rain_draws{0}, g_rain_restored{0}, g_pfxmap_calls{0};

void hk_RainUpdate() {
    if (t_pass == 2 && g_on[kRain].load(std::memory_order_relaxed)) {
        g_applied[kRain].fetch_add(1, std::memory_order_relaxed);
        return;  // stepped in pass 1; pass 2 draws that state
    }
    g_rain_steps.fetch_add(1, std::memory_order_relaxed);
    o_RainUpdate();
}

void hk_RainDraw(void* rain) {
    if (taa::skip_scene_rain(reinterpret_cast<uintptr_t>(_ReturnAddress()))) return;  // native TAA: drawn in the post
    g_rain_draws.fetch_add(1, std::memory_order_relaxed);
    if (t_double && g_on[kRain].load(std::memory_order_relaxed)) {
        char* sm = reinterpret_cast<char*>(anchors::addr(Id::RainSmoother));
        char& reset = global<char>(Id::RainResetFlag);
        if (t_rain_draws++ == 0) {
            std::memcpy(g_rain_smoother, sm, kRainSmootherBytes);
            g_rain_reset = reset;
        } else {
            std::memcpy(sm, g_rain_smoother, kRainSmootherBytes);
            reset = g_rain_reset;
            g_rain_restored.fetch_add(1, std::memory_order_relaxed);
        }
    }
    o_RainDraw(rain);
}

void hk_PfxMap(void* map) {
    if (t_pass == 2 && g_on[kPfxMap].load(std::memory_order_relaxed)) {
        g_applied[kPfxMap].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_pfxmap_calls.fetch_add(1, std::memory_order_relaxed);
    o_PfxMap(map);
}

// ---- ped damage (cycles 21/22). FUN_1405e1700 (*0x142ac50f8), first in SceneRender's frame work (0x1405c3853), loops
// the ped damage slots and integrates each by dt (FUN_14070c420, returning early while paused): the wet line +0x420
// and the wetness +0x414 move, blood soaks in (+0x3e8, steps drawn into the slot's damage texture), splats age; the
// draw (FUN_14070ce50 from DrawVisEntity) builds each ped's BloodData from those fields. Run twice, the second run's
// render-target work left the scene seen through window glass different between the passes (cycle 21: 2,000-7,700
// texels at yaws 180-270 in the running game; cycle 22 with the skip: exact). It is not the cause of the hair specks
// (cycle 22 A/B/A). It takes its own lock and restores the viewport and render states; skipping it in pass 2 only
// delays a slot release or a queued splat to the next frame, as vanilla does for anything queued during the scene
// (call-then-restore would draw the soak steps twice). Integrated once per frame, peds also dry at vanilla speed.
using PedDamage_t = void (*)(void* mgr);
PedDamage_t o_PedDamage = nullptr;

void hk_PedDamage(void* mgr) {
    if (t_pass == 2 && g_on[kDamage].load(std::memory_order_relaxed)) {
        g_applied[kDamage].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    o_PedDamage(mgr);
}

// ---- cloud shadows (cycle 22's residue, agent study). FUN_140886a30 advances the cloud-shadow scroll (a float2 at
// *(*0x142ad35f0+0x18)+0xf0) by the blended cloud velocity times dt unless its third argument is set, then pushes
// ScrollXZ, the pattern scale, the strength and the cloud textures to the light pre-pass effect; the shadow
// collector draws the directional shadow with it into the screen-space collector that terrain, grass, foliage, props
// and hair read. Its callers in a pass: the collector (0x1405cc493, advancing unless paused), the particle map
// (0x1405cbdd3, pass 1 only already) and the trees (0x1405a6c87, never advancing). Pass 2 now calls it with the flag
// set: it pushes pass 1's final scroll, and the scroll moves at vanilla speed. Visible only while clouds are on (the
// minute after rain in cycle 22: 18,969 texels on grass, dirt, planks and leaves).
using CloudScroll_t = uintptr_t (*)(float* scroll, void* params, uintptr_t no_advance, void* textures);
CloudScroll_t o_CloudScroll = nullptr;

uintptr_t hk_CloudScroll(float* scroll, void* params, uintptr_t no_advance, void* textures) {
    if (t_pass == 2 && (no_advance & 0xff) == 0 && g_on[kClouds].load(std::memory_order_relaxed)) {
        g_applied[kClouds].fetch_add(1, std::memory_order_relaxed);
        no_advance = (no_advance & ~static_cast<uintptr_t>(0xff)) | 1;
    }
    return o_CloudScroll(scroll, params, no_advance, textures);
}

// The sun-visibility averages (0x1422e175c, 0x1422e1760): a bucket callback (FUN_1405c1970) moves them toward a flag
// VisibilityBuild sets once per frame, 0.3 and 0.05 of the way per call; they feed SunVisibility (foliage, the water's
// sun specular). Two calls a frame would move them twice; they differ between the passes for about a second after the
// flag flips. Put back before pass 2.
constexpr size_t kSunVisBytes = 8;
char g_sunvis[kSunVisBytes];

// ---- drawlog: for one double frame, each DrawVisList call (return address, first mask word) with the draws and
// queries it recorded, per pass, to name the calls whose output differs between the passes. A test command.
using DrawVisList_t = void (*)(void* a, void* b, uint32_t* mask, int d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h,
                               uintptr_t i, uintptr_t j);
DrawVisList_t o_DrawVisList = nullptr;
std::atomic<int> g_drawlog{0};  // 0 off, 1 armed (next double frame), 2 logging this frame

void hk_DrawVisList(void* a, void* b, uint32_t* mask, int d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h, uintptr_t i,
                    uintptr_t j) {
    if (g_drawlog.load(std::memory_order_relaxed) != 2 || t_pass == 0) {
        o_DrawVisList(a, b, mask, d, e, f, g, h, i, j);
        return;
    }
    uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - anchors::base();
    uint32_t m = mask ? *mask : 0;
    uint64_t c0 = ring_probe::thread_cbvs();
    o_DrawVisList(a, b, mask, d, e, f, g, h, i, j);
    log::info("[drawlog] pass %d ret %#llx mask %#x arg4 %#x cbvs %llu", t_pass, static_cast<unsigned long long>(ret), m,
              static_cast<unsigned>(d), static_cast<unsigned long long>(ring_probe::thread_cbvs() - c0));
}

// ---- the first eye's post run: RenderFrame 0x1405c8ec9..0x1405c905c, minus what must run once per frame (the mutex
// release, the CPU cull view and visibility work, the dead low-res pass) and with its UI wait taken here.
using Void_t = void (*)();
using Ptr_t = void (*)(void*);
using StateSet_t = int (*)(int state, int value);
using SunRays_t = void (*)(void* sun, void* params, int flags);
using PostTint_t = void (*)(void* postfx, float* rgba);

struct PostState {  // what one run of the chain advances (post-chain study)
    char lum_ab[16];          // PostFx+0x80/+0x88: adapted luminance A/B, swapped per run
    char reset_3b2;           // +0x3b2 history reset flag
    char reset_558[4];        // +0x558 history reset flag
    char blur_fe0[4];         // +0xfe0 recovering blur, integrated with dt
    char flash_fec[4];        // +0xfec flash accumulator, decays per run
    char pause_ab0[4];        // +0xab0 pause-menu flag
    int ring_count, ring_x, ring_y;  // luminance ring cursor
    char cut_latch, change_latch, sun_flag;
    char prev_view[0x40];                // function-static previous view (FUN_14086bbc0)
    char sun_toggle[4];                  // sun+0x2f0: sun-ray history pair toggle
    char sun_prev_vp[0x40];              // sun+0x160..+0x19f: previous view-projection
};

void take(PostState& s, char* postfx, char* sun) {
    std::memcpy(s.lum_ab, postfx + 0x80, 16);
    s.reset_3b2 = postfx[0x3b2];
    std::memcpy(s.reset_558, postfx + 0x558, 4);
    std::memcpy(s.blur_fe0, postfx + 0xfe0, 4);
    std::memcpy(s.flash_fec, postfx + 0xfec, 4);
    std::memcpy(s.pause_ab0, postfx + 0xab0, 4);
    s.ring_count = global<int>(Id::LumRingCount);
    s.ring_x = global<int>(Id::LumRingX);
    s.ring_y = global<int>(Id::LumRingY);
    s.cut_latch = global<char>(Id::CameraCutLatch);
    s.change_latch = global<char>(Id::PostChangeLatch);
    s.sun_flag = global<char>(Id::SunFlag);
    std::memcpy(s.prev_view, reinterpret_cast<void*>(anchors::addr(Id::PostPrevViewStatic)), 0x40);
    if (sun) {
        std::memcpy(s.sun_toggle, sun + 0x2f0, 4);
        std::memcpy(s.sun_prev_vp, sun + 0x160, 0x40);
    }
}

// keep_exposure: the run's exposure step stays (the adapted pair's swap and the ring cursor; split exposure).
void restore(const PostState& s, char* postfx, char* sun, bool keep_exposure) {
    if (!keep_exposure) std::memcpy(postfx + 0x80, s.lum_ab, 16);
    postfx[0x3b2] = s.reset_3b2;
    std::memcpy(postfx + 0x558, s.reset_558, 4);
    std::memcpy(postfx + 0xfe0, s.blur_fe0, 4);
    std::memcpy(postfx + 0xfec, s.flash_fec, 4);
    std::memcpy(postfx + 0xab0, s.pause_ab0, 4);
    if (!keep_exposure) ring_set({s.ring_count, s.ring_x, s.ring_y});
    // The latch (last frame's camera-cut flag, read twice and written once by the chain) is put back either way, so
    // the second eye's run takes the same cut decisions as the first.
    global<char>(Id::CameraCutLatch) = s.cut_latch;
    global<char>(Id::PostChangeLatch) = s.change_latch;
    global<char>(Id::SunFlag) = s.sun_flag;
    std::memcpy(reinterpret_cast<void*>(anchors::addr(Id::PostPrevViewStatic)), s.prev_view, 0x40);
    if (sun) {
        std::memcpy(sun + 0x2f0, s.sun_toggle, 4);
        std::memcpy(sun + 0x160, s.sun_prev_vp, 0x40);
    }
}

// ---- god rays (cycle 20). The sun's ray parameters (sun+0x2b0) hold the accumulation pair "God ray accumulation 0/1"
// (+0x30/+0x38, 256x256 R8) and its toggle (+0x40); the sun keeps the previous view-projection at +0x160..+0x19f.
// FUN_1406cf170 reads acc[1-t], writes acc[t], flips t and stores this call's view-projection. With one history, each
// eye blended the rays the other eye wrote a frame earlier (cycle 20, yaw 216 at noon: the first eye 97.7% against a
// mono render at its pose; the failure follows the eye order). The first eye gets its own pair from the engine's
// creator FUN_1406ca910 on a private parameter block (the engine makes such sets at runtime for other god-ray lights),
// made once at the start of the first double frame, and its own toggle and previous view.
constexpr size_t kSunAccA = 0x2e0, kSunAccB = 0x2e8, kSunAccToggle = 0x2f0, kSunPrevVp = 0x160;
alignas(16) char g_rays_block[0x100];  // the first eye's parameter block (only the pair is used)
struct EyeRays {
    void* acc[2] = {};
    uint32_t toggle = 0;
    char prev_vp[0x40] = {};
};
EyeRays g_eye1_rays;
int g_rays_state = 0;  // 0 not made, 1 made, -1 creation failed (the split then does nothing)
std::atomic<uint64_t> g_rays_swaps{0};

void make_eye1_rays(char* sun) {
    std::memset(g_rays_block, 0, sizeof(g_rays_block));
    reinterpret_cast<Ptr_t>(anchors::addr(Id::SunRayParamsInit))(g_rays_block);
    reinterpret_cast<void (*)(void*, char)>(anchors::addr(Id::SunRayTargets))(g_rays_block, 1);
    void* a = at<void*>(g_rays_block, 0x30);
    void* b = at<void*>(g_rays_block, 0x38);
    if (!a || !b) {
        g_rays_state = -1;
        log::error("[dual] the first eye's god-ray pair was not created: split godrays inactive");
        return;
    }
    g_eye1_rays.acc[0] = a;
    g_eye1_rays.acc[1] = b;
    g_eye1_rays.toggle = 0;
    std::memcpy(g_eye1_rays.prev_vp, sun + kSunPrevVp, 0x40);
    g_rays_state = 1;
    log::info("[dual] the first eye's god-ray pair: %p %p (sun pair %p %p)", a, b, at<void*>(sun, kSunAccA),
              at<void*>(sun, kSunAccB));
}

// Exchanges the sun's god-ray history (pair, toggle, previous view) with the first eye's.
void swap_rays(char* sun) {
    std::swap(at<void*>(sun, kSunAccA), g_eye1_rays.acc[0]);
    std::swap(at<void*>(sun, kSunAccB), g_eye1_rays.acc[1]);
    std::swap(at<uint32_t>(sun, kSunAccToggle), g_eye1_rays.toggle);
    alignas(16) char tmp[0x40];
    std::memcpy(tmp, sun + kSunPrevVp, 0x40);
    std::memcpy(sun + kSunPrevVp, g_eye1_rays.prev_vp, 0x40);
    std::memcpy(g_eye1_rays.prev_vp, tmp, 0x40);
}

// RenderFrame's inline pause-menu block (0x1405c8f7a..0x1405c9034): the menu's tint on the post technique.
void pause_block(char* postfx) {
    if (global<uint64_t>(Id::PauseGate) != 0) return;
    char* menu = global<char*>(Id::PauseMenuState);
    if (!menu || !menu[0x34]) return;
    char* tech = at<char*>(postfx, 0x6c0);
    if (!tech) return;
    at<uint32_t>(tech, 0x200) = 0;
    at<uint64_t>(tech, 0x260) = 0;
    alignas(16) float rgba[4] = {1.0f, 1.0f, 1.0f, global<float>(Id::PauseTint)};
    void** vt = *reinterpret_cast<void***>(postfx);
    reinterpret_cast<PostTint_t>(vt[0x18 / 8])(postfx, rgba);
    std::memcpy(tech + 0x30, rgba, sizeof(rgba));
    at<uint16_t>(tech, 0x1fc) = 1;
    at<float>(tech, 0x1f8) = 1.0f;
    at<uint32_t>(postfx, 0xab0) = 1;
}

// ---- lens drops (headset round 2, check 6: the rain drops on the lens were in the right eye only). RenderFrame draws
// the screen overlays after the post chain (0x1405c907f..0x1405c91ad): PostTail builds the bright-pass textures the
// drops refract from the eye's image and binds the output, then, with render states 0xb, 8, 7 and 2 set, LensDrops
// (FUN_1405ed4d0) spawns new drops from the shared RNG, fades the old ones and draws them. Only the second eye's post
// run got them. The first eye's run now makes the same call after its chain: that call is the frame's drop step.
// The game's own call (the second eye) then only draws the drops that step left. Re-running the step from the same
// state cannot match: other threads draw from the shared RNG during the call (cycles 42-44: a third of the calls).
// The game's call runs with its spawn factors at 0 and its fade at 1. Those are ppp +0x2274, +0x2254 and +0x2278 for
// spawning, and +0x2250 = 0 with +0x2268 huge for the rain-drop fade. The splash drops' fixed 0.95 fade is
// pre-divided out of their alpha. Afterwards the factors and the first eye's drops are put back.
// [Screen] LensDrops=0 leaves them out of both eyes.
using LensDrops_t = void (*)(void* ppp, uintptr_t from_texture);
LensDrops_t o_LensDrops = nullptr;
std::atomic<bool> g_lens_drops{true};
std::atomic<uint64_t> g_drops_first{0}, g_drops_skipped{0}, g_drops_same{0}, g_drops_differ{0};
constexpr size_t kDropEntries = 0xa3c, kDropCount = 0x223c, kMaxDrops = 0x300;
uint64_t g_drops_after[kMaxDrops];  // the first eye's step: the drops (4 halves: x, y, alpha, intensity | type << 8)
int g_drops_after_count = 0;
bool g_drops_pending = false;       // render thread

// IEEE half <-> float for the drop alphas (0..1: normal or zero; no infinities or NaNs to handle)
float half_to_float(uint16_t h) {
    uint32_t e = (h >> 10) & 0x1f, m = h & 0x3ff, f;
    if (e == 0) {
        float v = std::ldexp(static_cast<float>(m), -24);
        return h & 0x8000 ? -v : v;
    }
    f = (static_cast<uint32_t>(h & 0x8000) << 16) | ((e + 112) << 23) | (m << 13);
    float v;
    std::memcpy(&v, &f, 4);
    return v;
}
uint16_t float_to_half(float v) {
    if (!(v > 6.1035156e-05f)) return 0;  // the drop alphas: zero or a normal half
    uint32_t f;
    std::memcpy(&f, &v, 4);
    uint32_t e = ((f >> 23) & 0xff) - 112, m = f & 0x7fffff;
    uint32_t h = (e << 10) | (m >> 13);
    uint32_t rest = m & 0x1fff;
    if (rest > 0x1000 || (rest == 0x1000 && (h & 1))) ++h;  // round to nearest even
    return static_cast<uint16_t>(h > 0x7bff ? 0x7bff : h);
}

void hk_LensDrops(void* ppp_v, uintptr_t from_texture) {
    if (!g_lens_drops.load(std::memory_order_relaxed)) {
        g_drops_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    char* ppp = static_cast<char*>(ppp_v);
    {
        // Round 3 aid: RenderFrame's other second-eye-only overlays are not replayed for the first eye yet; the gate of
        // the full-screen one (FUN_1405ec2b0, ppp+0xa20) is logged when it switches, to name an effect seen in one eye.
        static char last_a20 = 0;
        char a20 = at<char>(ppp, 0xa20);
        if (a20 != last_a20) {
            last_a20 = a20;
            log::info("[dual] screen overlay FUN_1405ec2b0 %s (second eye only)", a20 ? "on" : "off");
        }
    }
    bool pending = g_drops_pending && ppp == global<char*>(Id::PostParamsObject);
    g_drops_pending = false;
    if (!pending) {
        o_LensDrops(ppp, from_texture);
        return;
    }
    // draw only: no spawning, no fading
    constexpr size_t kFields[5] = {0x2250, 0x2254, 0x2268, 0x2274, 0x2278};
    float saved[5];
    for (int i = 0; i < 5; ++i) saved[i] = at<float>(ppp, kFields[i]);
    at<float>(ppp, 0x2250) = 0.0f;
    at<float>(ppp, 0x2254) = 0.0f;
    at<float>(ppp, 0x2268) = 1e30f;
    at<float>(ppp, 0x2274) = 0.0f;
    at<float>(ppp, 0x2278) = 0.0f;
    int n = g_drops_after_count;
    at<int>(ppp, kDropCount) = n;
    uint64_t* d = reinterpret_cast<uint64_t*>(ppp + kDropEntries);
    for (int i = 0; i < n; ++i) {
        d[i] = g_drops_after[i];
        if (g_drops_after[i] >> 56) {  // a splash drop: its draw fades it by 0.95
            uint16_t* h = reinterpret_cast<uint16_t*>(&d[i]);
            float a = half_to_float(h[2]) / 0.95f;
            h[2] = float_to_half(a > 1.0f ? 1.0f : a);
        }
    }
    o_LensDrops(ppp, from_texture);
    for (int i = 0; i < 5; ++i) at<float>(ppp, kFields[i]) = saved[i];
    bool same = at<int>(ppp, kDropCount) == n;
    for (int i = 0; same && i < n; ++i) same = (d[i] >> 56) != 0 || d[i] == g_drops_after[i];
    (same ? g_drops_same : g_drops_differ).fetch_add(1, std::memory_order_relaxed);
    at<int>(ppp, kDropCount) = n;
    std::memcpy(d, g_drops_after, static_cast<size_t>(n) * 8);
}

// ---- god rays behind the eye (headset rounds 1-2, check 5/7: a glow in the sky in one eye, both or neither, "the sun
// was rising behind me"). SunRays (FUN_1406cf170, once per eye with that eye's viewport current) puts the sun's quad at
// the camera + direction (params +0x00) x distance (+0x14), projects its corners with the current view-projection and
// gives up only on a few NDC bounds of two corners and a near depth; the CPU's fade (+0x18 x +0x5c) follows the game
// camera. With the game camera facing the sun and the head turned away, the sun is behind the eye: its corners divide
// by a negative w, land mirrored inside the screen and pass, so the rays glow ahead, in whichever eye's asymmetric
// frustum lets the mirrored quad through. Each eye now takes the game's own no-rays path (SunFlag 0, nothing drawn) when
// the sun lies behind its view plane ([Stereo] SunBehindCheck).
using SunRaysRet_t = uint64_t (*)(void* sun, void* params, char flags);
SunRaysRet_t o_SunRays = nullptr;
std::atomic<bool> g_sun_behind_check{true};
std::atomic<uint64_t> g_sun_behind_skips{0}, g_sun_calls{0};

uint64_t hk_SunRays(void* sun, void* params, char flags) {
    g_sun_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_sun_behind_check.load(std::memory_order_relaxed) && params) {
        const char* vp = global<char*>(Id::ViewportCurrent);
        if (vp) {
            const float* d = static_cast<const float*>(params);
            const float* back = reinterpret_cast<const float*>(vp + 0x60);  // the camera's back row
            float facing = -(d[0] * back[0] + d[1] * back[1] + d[2] * back[2]);
            if (facing <= 0.05f) {
                global<char>(Id::SunFlag) = 0;
                g_sun_behind_skips.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
        }
    }
    return o_SunRays(sun, params, flags);
}

void first_eye_overlays(char* postfx) {
    char* ppp = global<char*>(Id::PostParamsObject);
    if (!ppp || !g_lens_drops.load(std::memory_order_relaxed) || !at<char>(ppp, 0x2272) ||
        global<uint64_t>(Id::ScreenOverlayGate) != 0)
        return;
    reinterpret_cast<Ptr_t>(anchors::addr(Id::PostTail))(postfx);
    auto set = reinterpret_cast<StateSet_t>(anchors::addr(Id::RenderStateSet));
    int old11 = global<int>(Id::RsElevenValue), old8 = global<int>(Id::RsEightValue), old7 = global<int>(Id::RsSevenValue),
        old2 = global<int>(Id::RsTwoValue);
    set(0xb, 0);
    set(8, 1);
    set(7, 0);
    set(2, 0);
    o_LensDrops(ppp, 0);
    set(2, old2);
    set(7, old7);
    set(8, old8);
    set(0xb, old11);
    int n = at<int>(ppp, kDropCount);
    if (n < 0 || n > static_cast<int>(kMaxDrops)) return;
    g_drops_after_count = n;
    std::memcpy(g_drops_after, ppp + kDropEntries, static_cast<size_t>(n) * 8);
    g_drops_pending = true;
    g_drops_first.fetch_add(1, std::memory_order_relaxed);
}

void first_eye_post(void* renderer) {
    char* postfx = global<char*>(Id::PostFxSingleton);
    // The post-chain study covered the FXAA path (applied technique +0x868 == 1, ENGINE-NOTES 3.x) and the native TAA
    // path (2: the same chain with the mod's resolve in the AA slot and no FXAA pass; ENGINE-NOTES item 7). DLSS (5,
    // [Render] DlssFirstEye, on): the game's own chain with its DLSS in the AA slot (research\run6\dlss.md G1; one
    // DLSS viewport for both eyes until the per-eye viewport is in)
    int tech = postfx ? at<int>(postfx, 0x868) : -1;
    const bool dlss_ok = tech == 5 && g_dlss_first_eye.load(std::memory_order_relaxed);
    if (!postfx || (tech != 1 && tech != 2 && !dlss_ok) || !at<void*>(postfx, 0x6c0)) {
        g_post_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // RenderFrame waits on renderer+0x38 after SceneRender for the main thread's UI and post data. The main thread
    // releases it before it waits for the scene (renderer+0x68, released by pass 2), so taking it here cannot close a
    // cycle; the timeout keeps a wrong assumption from hanging the game.
    if (global<uint64_t>(Id::UiWaitGate) == 0) {
        if (void* sem = at<void*>(renderer, 0x38)) {
            if (!(frame_probe::on() ? timed_wait(sem, 500, frame_probe::kUiWaitMid) : o_SemWait(sem, 500))) {
                g_ui_timeouts.fetch_add(1, std::memory_order_relaxed);
                log::limited("dual.uiwait", 8, "[dual] UI data not ready within 500 ms: no first-eye post run this frame");
                return;
            }
            t_ui_wait_taken = true;
            g_ui_taken.fetch_add(1, std::memory_order_relaxed);
        }
    }
    char* sun = global<char*>(Id::SunObject);
    PostState s;
    take(s, postfx, sun);
    bool rays = sun && g_rays_state == 1 && g_on[kGodRays].load(std::memory_order_relaxed);
    if (rays) swap_rays(sun);  // the first eye's history in; take() saw the second eye's
    reinterpret_cast<Void_t>(anchors::addr(Id::DeferredFlush))();
    reinterpret_cast<Ptr_t>(anchors::addr(Id::UnbindTargets))(global<void*>(Id::GrcContext));
    reinterpret_cast<Void_t>(anchors::addr(Id::RenderStateReset))();
    if (sun) {
        reinterpret_cast<StateSet_t>(anchors::addr(Id::RenderStateSet))(0xb, 0);
        reinterpret_cast<SunRays_t>(anchors::addr(Id::SunRays))(sun, sun + 0x2b0, 0);
    }
    if (void* params = global<void*>(Id::PostParamsObject))
        reinterpret_cast<Ptr_t>(anchors::addr(Id::PostParamsBuild))(params);
    pause_block(postfx);
    void** vt = *reinterpret_cast<void***>(postfx);
    reinterpret_cast<Ptr_t>(vt[0x28 / 8])(postfx);
    reinterpret_cast<PostRun_t>(vt[0x30 / 8])(postfx, 0);
    reinterpret_cast<Ptr_t>(vt[0x38 / 8])(postfx);
    first_eye_overlays(postfx);
    if (rays) {
        swap_rays(sun);  // the first eye's updated history out, the second eye's back
        g_rays_swaps.fetch_add(1, std::memory_order_relaxed);
    }
    bool keep = g_on[kExposure].load(std::memory_order_relaxed) && !g_on[kHold].load(std::memory_order_relaxed);
    restore(s, postfx, sun, keep);
    t_exposure_kept = keep;
    t_first_lum = at<void*>(postfx, 0x80);
    g_applied[kPost].fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

bool install() {
    g_dlss_first_eye = config::get_bool("Render", "DlssFirstEye", true);
    if (anchors::stand_down()) return false;
    g_wait_ret = anchors::addr(Id::SceneWaitReturn);
    g_release_ret = anchors::addr(Id::SceneReleaseReturn);
    g_ui_wait_ret = anchors::addr(Id::FrameUiWaitReturn);
    g_adapt_ret = anchors::addr(Id::AdaptDrawReturn);
    g_lock_ret_a = anchors::addr(Id::FrameLockWaitReturnA);
    g_lock_ret_b = anchors::addr(Id::FrameLockWaitReturnB);
    g_frame_wait_ret = anchors::addr(Id::FrameDataWaitReturn);
    g_main_wait_ret = anchors::addr(Id::MainFrameWaitReturn);
    g_play_wait_ret = anchors::addr(Id::PlaybackWaitReturn);
    for (int i = 0; i < kSplitCount; ++i) {
        std::string key = std::string("Split_") + kSplitName[i];
        g_on[i] = config::get_bool("Stereo", key.c_str(), split_default(i));
    }
    bool ok = hooks::install("RDR semaphore wait", reinterpret_cast<void*>(anchors::addr(Id::SemWait)), hk_SemWait,
                             &o_SemWait);
    ok = hooks::install("RDR semaphore release", reinterpret_cast<void*>(anchors::addr(Id::SemRelease)), hk_SemRelease,
                        &o_SemRelease) &&
         ok;
    ok = hooks::install("RDR forest/grass draw", reinterpret_cast<void*>(anchors::addr(Id::ForestDraw)), hk_ForestDraw,
                        &o_ForestDraw) &&
         ok;
    ok = hooks::install("RDR lights update", reinterpret_cast<void*>(anchors::addr(Id::LightsUpdate)), hk_LightsUpdate,
                        &o_LightsUpdate) &&
         ok;
    ok = hooks::install("RDR forest frame", reinterpret_cast<void*>(anchors::addr(Id::ForestFrame)), hk_ForestFrame,
                        &o_ForestFrame) &&
         ok;
    rdrvr_draw_masks_on = g_on[kMasks].load() ? 1 : 0;
    g_shadow_ret[kCascades] = anchors::addr(Id::CascadeRenderReturn);
    g_shadow_ret[kFaces] = anchors::addr(Id::FacesRenderReturn);
    g_shadow_ret[kSpot] = anchors::addr(Id::SpotRenderReturn);
    ok = hooks::install("RDR sun cascades render (shared shadows)", reinterpret_cast<void*>(anchors::addr(Id::CascadeRender)),
                        hk_CascadeRender, &o_CascadeRender) &&
         ok;
    ok = hooks::install("RDR light faces render (shared shadows)", reinterpret_cast<void*>(anchors::addr(Id::FacesRender)),
                        hk_FacesRender, &o_FacesRender) &&
         ok;
    ok = hooks::install("RDR spot shadows render (shared shadows)", reinterpret_cast<void*>(anchors::addr(Id::SpotRender)),
                        hk_SpotRender, &o_SpotRender) &&
         ok;
    ok = hooks::install("RDR PostFx run (exposure)", reinterpret_cast<void*>(anchors::addr(Id::PostRun)), hk_PostRun,
                        &o_PostRun) &&
         ok;
    ok = hooks::install("RDR post full-screen pass (adaptation)", reinterpret_cast<void*>(anchors::addr(Id::PostDrawPass)),
                        hk_PostDrawPass, &o_PostDrawPass) &&
         ok;
    g_lens_drops = config::get_bool("Screen", "LensDrops", true);
    g_sun_behind_check = config::get_bool("Stereo", "SunBehindCheck", true);
    ok = hooks::install("RDR SunRays", reinterpret_cast<void*>(anchors::addr(Id::SunRays)), hk_SunRays, &o_SunRays) && ok;
    ok = hooks::install("RDR lens drops", reinterpret_cast<void*>(anchors::addr(Id::LensDrops)), hk_LensDrops, &o_LensDrops) && ok;
    ok = hooks::install("RDR cloud-shadow scroll", reinterpret_cast<void*>(anchors::addr(Id::CloudScroll)), hk_CloudScroll,
                        &o_CloudScroll) &&
         ok;
    ok = hooks::install("RDR ped damage update", reinterpret_cast<void*>(anchors::addr(Id::PedDamageUpdate)),
                        hk_PedDamage, &o_PedDamage) &&
         ok;
    ok = hooks::install("RDR GPU rain step", reinterpret_cast<void*>(anchors::addr(Id::RainUpdate)), hk_RainUpdate,
                        &o_RainUpdate) &&
         ok;
    ok = hooks::install("RDR rain draw", reinterpret_cast<void*>(anchors::addr(Id::RainDraw)), hk_RainDraw, &o_RainDraw) &&
         ok;
    ok = hooks::install("RDR particle collision map", reinterpret_cast<void*>(anchors::addr(Id::PfxCollisionMap)),
                        hk_PfxMap, &o_PfxMap) &&
         ok;
    ok = hooks::install("RDR DrawVisList (drawlog)", reinterpret_cast<void*>(anchors::addr(Id::DrawVisList)), hk_DrawVisList,
                        &o_DrawVisList) &&
         ok;
    if (config::get_bool("Compat", "DrawMaskInit", true)) {
        g_masks_hooked = hooks::install("RDR draw masks (FUN_140706050 entry stub)",
                                        reinterpret_cast<void*>(anchors::addr(Id::DrawMasksFn)),
                                        reinterpret_cast<void*>(&rdrvr_draw_masks_stub), &rdrvr_draw_masks_original);
        ok = g_masks_hooked && ok;
    }
    return ok;
}

int pass() { return t_pass; }

void shadow_status(char* out, size_t len) {
    size_t n = static_cast<size_t>(std::snprintf(out, len, "shadows %s, shadowunion %s, union viewport %s |",
                                                 g_on[kShadows].load() ? "on" : "off", g_on[kShadowUnion].load() ? "on" : "off",
                                                 camera_lever::shadow_union_vp() ? "built" : "none"));
    for (int k = 0; k < kShadowKinds && n < len; ++k)
        n += static_cast<size_t>(std::snprintf(out + n, len - n, " %s union %llu skipped %llu no union %llu;", kShadowName[k],
                                               static_cast<unsigned long long>(g_shadow_union[k].load()),
                                               static_cast<unsigned long long>(g_shadow_skipped[k].load()),
                                               static_cast<unsigned long long>(g_shadow_no_union[k].load())));
}

bool shadows_wanted() { return g_on[kShadows].load(std::memory_order_relaxed) || g_on[kShadowUnion].load(std::memory_order_relaxed); }

int post_slot() { return t_pass == 1 ? 0 : t_double ? 1 : 2; }

void begin_frame(void*) {
    if (g_rays_state == 0 && g_on[kGodRays].load(std::memory_order_relaxed))
        if (char* sun = global<char*>(Id::SunObject)) make_eye1_rays(sun);  // render thread, before pass 1
    t_ui_wait_taken = false;
    t_exposure_kept = false;
    t_double = true;
    for (bool& d : t_shadow_done) d = false;
    t_adapt_draws = 0;
    t_rain_draws = 0;
    int armed = 1;
    if (g_drawlog.compare_exchange_strong(armed, 2)) log::info("[drawlog] double frame begins");
    std::memcpy(g_gust, reinterpret_cast<void*>(anchors::addr(Id::GustWind)), kGustBytes);
    std::memcpy(g_sunvis, reinterpret_cast<void*>(anchors::addr(Id::SunVisEma)), kSunVisBytes);
    t_pass = 1;
}

void between_passes(void* renderer, int) {
    if (g_on[kPost].load(std::memory_order_relaxed)) first_eye_post(renderer);
    if (g_on[kGust].load(std::memory_order_relaxed)) {
        std::memcpy(reinterpret_cast<void*>(anchors::addr(Id::GustWind)), g_gust, kGustBytes);
        g_applied[kGust].fetch_add(1, std::memory_order_relaxed);
    }
    if (g_on[kSunVis].load(std::memory_order_relaxed)) {
        std::memcpy(reinterpret_cast<void*>(anchors::addr(Id::SunVisEma)), g_sunvis, kSunVisBytes);
        g_applied[kSunVis].fetch_add(1, std::memory_order_relaxed);
    }
    t_pass = 2;
}

void end_frame(int) {
    t_pass = 0;
    int logging = 2;
    if (g_drawlog.compare_exchange_strong(logging, 0)) log::info("[drawlog] double frame ends");
}

void mono_frame() {
    t_exposure_kept = false;
    t_double = false;
}

void arm_drawlog() { g_drawlog = 1; }

void adapt_histogram(uint64_t out[4]) {
    for (int i = 0; i < 4; ++i) out[i] = g_adapt_hist[i].load();
}

void set_sun_behind_check(bool on) {
    g_sun_behind_check = on;
    log::info("[dual] god rays behind the eye: %s", on ? "skipped" : "the game's own test only");
}

void sun_status(char* out, size_t len) {
    std::snprintf(out, len, "sun behind check %s, calls %llu, skipped behind the eye %llu", g_sun_behind_check.load() ? "on" : "off",
                  static_cast<unsigned long long>(g_sun_calls.load()), static_cast<unsigned long long>(g_sun_behind_skips.load()));
}

void set_lens_drops(bool on) {
    g_lens_drops = on;
    log::info("[dual] lens drops %s", on ? "on (both eyes)" : "off");
}

void lens_drops_status(char* out, size_t len) {
    char* ppp = global<char*>(Id::PostParamsObject);
    std::snprintf(out, len, "lens drops %s, first-eye steps %llu, calls skipped %llu, second eye drew the same %llu, differ %llu | drops now %d, flags %d %d, splash %.2f %.2f %.2f",
                  g_lens_drops.load() ? "on" : "off", static_cast<unsigned long long>(g_drops_first.load()),
                  static_cast<unsigned long long>(g_drops_skipped.load()), static_cast<unsigned long long>(g_drops_same.load()),
                  static_cast<unsigned long long>(g_drops_differ.load()), ppp ? at<int>(ppp, 0x223c) : -1,
                  ppp ? at<char>(ppp, 0x2270) : -1, ppp ? at<char>(ppp, 0x2271) : -1, ppp ? at<float>(ppp, 0x227c) : 0.0f,
                  ppp ? at<float>(ppp, 0x2280) : 0.0f, ppp ? at<float>(ppp, 0x2284) : 0.0f);
}

bool set_split(const char* name, bool on) {
    for (int i = 0; i < kSplitCount; ++i) {
        if (std::strcmp(name, kSplitName[i]) == 0) {
            g_on[i] = on;
            if (i == kMasks) rdrvr_draw_masks_on = on ? 1 : 0;
            log::info("[dual] split %s %s", name, on ? "on" : "off");
            return true;
        }
    }
    return false;
}

bool split_on(const char* name) {
    for (int i = 0; i < kSplitCount; ++i)
        if (std::strcmp(name, kSplitName[i]) == 0) return g_on[i].load();
    return false;
}

void status_text(char* out, size_t len) {
    size_t n = static_cast<size_t>(std::snprintf(
        out, len, "skipped waits %llu releases %llu | ui wait taken %llu skipped %llu timeouts %llu | post not run %llu |"
                  " mask stub %s |",
        static_cast<unsigned long long>(g_skipped_waits.load()), static_cast<unsigned long long>(g_skipped_releases.load()),
        static_cast<unsigned long long>(g_ui_taken.load()), static_cast<unsigned long long>(g_ui_skipped.load()),
        static_cast<unsigned long long>(g_ui_timeouts.load()), static_cast<unsigned long long>(g_post_skipped.load()),
        g_masks_hooked ? "hooked" : "absent"));
    if (n < len)
        n += static_cast<size_t>(std::snprintf(
            out + n, len - n, " adapt/double frame 0:%llu 1:%llu 2:%llu 3+:%llu skipped %llu, lum same %llu differ %llu |",
            static_cast<unsigned long long>(g_adapt_hist[0].load()), static_cast<unsigned long long>(g_adapt_hist[1].load()),
            static_cast<unsigned long long>(g_adapt_hist[2].load()), static_cast<unsigned long long>(g_adapt_hist[3].load()),
            static_cast<unsigned long long>(g_adapt_skipped.load()), static_cast<unsigned long long>(g_lum_same.load()),
            static_cast<unsigned long long>(g_lum_differ.load())));
    if (n < len)
        n += static_cast<size_t>(std::snprintf(
            out + n, len - n, " rain steps %llu draws %llu restored %llu, pfxmap %llu | godray pair %s, swaps %llu |",
            static_cast<unsigned long long>(g_rain_steps.load()), static_cast<unsigned long long>(g_rain_draws.load()),
            static_cast<unsigned long long>(g_rain_restored.load()), static_cast<unsigned long long>(g_pfxmap_calls.load()),
            g_rays_state == 1 ? "made" : g_rays_state < 0 ? "FAILED" : "not made",
            static_cast<unsigned long long>(g_rays_swaps.load())));
    for (int i = 0; i < kSplitCount && n < len; ++i)
        n += static_cast<size_t>(std::snprintf(out + n, len - n, " %s %s %llu", kSplitName[i], g_on[i].load() ? "on" : "off",
                                               static_cast<unsigned long long>(g_applied[i].load())));
    for (int k = 0; k < kShadowKinds && n < len; ++k)
        n += static_cast<size_t>(std::snprintf(out + n, len - n, "%s %s union %llu skipped %llu no union %llu", k ? "," : " |",
                                               kShadowName[k], static_cast<unsigned long long>(g_shadow_union[k].load()),
                                               static_cast<unsigned long long>(g_shadow_skipped[k].load()),
                                               static_cast<unsigned long long>(g_shadow_no_union[k].load())));
}

}  // namespace rdrvr::dual_pass
