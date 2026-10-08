#include "core/camera_lever.h"
#include "core/round_draw.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "core/anchors.h"
#include "core/diag.h"
#include "core/dual_pass.h"
#include "core/taa.h"
#include "core/hooks.h"
#include "core/body.h"
#include "core/config.h"
#include "core/log.h"
#include "core/pose.h"
#include "core/ring_probe.h"
#include "core/xr.h"

namespace rdrvr::camera_lever {
namespace {

using SceneRender_t = void (*)(void* renderer, void* vp, const float* cam, void* a4);
using SetCamera_t = void (*)(void* vp, const float* cam);
using Regenerate_t = void (*)(void* vp, char push);
using Perspective_t = void (*)(void* vp);
using VisibilityBuild_t = void (*)(void* renderer, void* vp, void* cam, void* a4);
SceneRender_t o_SceneRender = nullptr;
SetCamera_t o_SetCamera = nullptr;
VisibilityBuild_t o_VisibilityBuild = nullptr;
Regenerate_t g_regenerate = nullptr;
Perspective_t g_perspective = nullptr;

// grcViewport fields (ENGINE-NOTES 1.4): matrix +0x1c0 (P00 +0x1c0, P11 +0x1d4, P20 +0x1e0, P21 +0x1e4), fov degrees
// +0x338, aspect +0x33c (0 = from the window), near +0x340, far +0x344, zoom +0x350/+0x354, off-centre +0x358/+0x35c.
constexpr size_t kViewportSize = 0x470;  // renderer+0x870 .. renderer+0xce0 (the camera matrix follows)
std::atomic<int> g_eye{-1};  // -1 off, 0 left, 1 right, 2 explicit tangents, 3 each pass its own eye (XR views)
std::mutex g_tan_mutex;
float g_tan[4] = {0, 0, 0, 0};
std::atomic<float> g_proj_err{-1.0f};    // max relative error of the read-back tangents, last frame
std::atomic<float> g_cover_vfov{0.0f}, g_cover_aspect{0.0f};
std::atomic<uint64_t> g_cover_builds{0}, g_head_cover_builds{0};
// Round 1 (headset, check 12): with the eyes from the XR views, cull for the head, not the game camera: the visibility
// build gets the centre eye's camera and a symmetric frustum over both eyes' FOVs plus a margin ([XR] HeadCover).
std::atomic<bool> g_head_cover{true};
constexpr float kCoverMarginDeg = 4.0f;
// Round 3 A/B: with the head cover off one nearer tree stopped tilting with the head. VisibilityBuild publishes the
// viewport it gets as worldToScreen (0x1405c2a86), which the forest update reads for tree LOD and facing; with the head
// cover that is the centre eye with the head's roll and pitch. After the cover's build, worldToScreen points at a copy
// with a level camera (head yaw only); culling keeps the full cover. [XR] LevelForestView=1; "billboards w2s level|cover".
std::atomic<bool> g_level_w2s{true};
alignas(16) char g_w2s_vp[0x470 + 0x10];
alignas(16) float g_w2s_cam[16];
// Experiment ("billboards vi level|off", off by default): every ViewInverse push for the eye's scene viewport gets level
// right/up rows, to see whether the distant trees take their tilt from the eye camera's rows 0 and 1.
std::atomic<bool> g_vi_level{false};
std::atomic<void*> g_scene_vp{nullptr};
std::atomic<uint64_t> g_vi_level_pushes{0};
using PushGlobals_t = void (*)(void* vp, char push_view_inverse);
PushGlobals_t o_PushGlobals = nullptr;

// Round 3 correction: the trees turn as the head turns (yaw and pitch, not roll). Every billboard path faced a view
// that follows the head: they must not follow the head's rotation at all. Their facing now comes from the game camera
// (which turns only with the stick or the body), level; each keeps its own position.
alignas(16) float g_game_cam[16];
std::atomic<bool> g_game_cam_valid{false};

// A level camera: the heading of `orient`'s back row, up = world y, at `pos`'s position (rows a, b, c, d).
bool level_from(const float* orient, const float* pos, float* out) {
    float bx = orient[8], bz = orient[10], n = std::sqrt(bx * bx + bz * bz);
    if (n < 1e-4f) return false;
    bx /= n;
    bz /= n;
    const float m[16] = {bz, 0, -bx, 0, 0, 1, 0, 0, bx, 0, bz, 0, pos[12], pos[13], pos[14], 1};
    std::memcpy(out, m, sizeof(m));
    return true;
}
// The same with the game camera's heading when there is one.
bool level_camera(const float* c, float* out) {
    return level_from(g_game_cam_valid.load(std::memory_order_relaxed) ? g_game_cam : c, c, out);
}

void hk_PushGlobals(void* vp, char push_view_inverse) {
    if (push_view_inverse && vp && g_vi_level.load(std::memory_order_relaxed) && vp == g_scene_vp.load(std::memory_order_relaxed)) {
        float* vi = reinterpret_cast<float*>(static_cast<char*>(vp) + 0x140);
        float saved[8], lv[16];
        std::memcpy(saved, vi, sizeof(saved));
        if (level_camera(vi, lv)) {
            std::memcpy(vi, lv, 8 * sizeof(float));
            o_PushGlobals(vp, push_view_inverse);
            std::memcpy(vi, saved, sizeof(saved));
            if (g_vi_level_pushes.fetch_add(1, std::memory_order_relaxed) == 0)
                log::info("[cam] experiment: ViewInverse right/up levelled for the eye passes");
            return;
        }
    }
    o_PushGlobals(vp, push_view_inverse);
} std::atomic<bool> g_double{false}, g_swap{false};
std::atomic<float> g_ipd{0.0f};
std::atomic<float> g_yaw{0.0f};
std::atomic<float> g_yaw_step{0.0f};  // "cam yawramp": added to g_yaw at each scene frame's start
float g_move_step[3] = {0, 0, 0};     // "cam moveramp": added to the world offset at each scene frame's start (g_mutex)
std::atomic<bool> g_move_ramp{false};
std::atomic<bool> g_xr_head_position{true};
std::atomic<float> g_xr_separation{1.0f};

// Rotates the camera matrix's a (right) and c (back) rows about b (up) by `deg`; the position is unchanged.
void yaw_rows(float* m, float deg) {
    float r = deg / 57.29577951f, cs = std::cos(r), sn = std::sin(r);
    for (int k = 0; k < 3; ++k) {
        float a = m[k], c = m[8 + k];
        m[k] = a * cs - c * sn;
        m[8 + k] = a * sn + c * cs;
    }
}
std::atomic<uint64_t> g_double_frames{0};
std::atomic<uint64_t> g_scene_us{0}, g_scene_timed{0};  // the time in SceneRender (the perf status)
std::atomic<bool> g_findcam{false};

// ---- R5 step 2 ("pose xr"): each pass's camera from its eye's located view. OpenXR's LOCAL axes (+x right, +y up,
// +z back) are read as the game camera's rows a, b, c, so an eye pose relative to the recentre pose (yaw only, at the
// mid-point of the eyes) rotates the camera's basis and moves it along that basis. The projection layer then carries
// exactly the poses rendered.
std::atomic<bool> g_xr_pose{false};
std::mutex g_recentre_mutex;
bool g_recentred = false;
std::atomic<uint32_t> g_recentre_gen{0};
float g_q0[4] = {0, 0, 0, 1}, g_p0[3] = {0, 0, 0};  // recentre: yaw-only orientation and position (LOCAL)
std::atomic<uint64_t> g_xr_pose_passes{0};

void quat_mul(const float* a, const float* b, float* o) {  // o = a * b, (x, y, z, w)
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x, o[1] = y, o[2] = z, o[3] = w;
}

void quat_matrix(const float* q, float* r) {  // 3x3, row-major, rotating column vectors
    float x = q[0], y = q[1], z = q[2], w = q[3];
    r[0] = 1 - 2 * (y * y + z * z), r[1] = 2 * (x * y - z * w), r[2] = 2 * (x * z + y * w);
    r[3] = 2 * (x * y + z * w), r[4] = 1 - 2 * (x * x + z * z), r[5] = 2 * (y * z - x * w);
    r[6] = 2 * (x * z - y * w), r[7] = 2 * (y * z + x * w), r[8] = 1 - 2 * (x * x + y * y);
}

void recentre_from(const xr::EyeView* v) {
    float mid[3], r[9];
    for (int k = 0; k < 3; ++k) mid[k] = 0.5f * (v[0].position[k] + v[1].position[k]);
    quat_matrix(v[0].orientation, r);
    float yaw = std::atan2(r[2], r[8]);  // the back axis (column 2) in the horizontal plane
    g_q0[0] = 0, g_q0[1] = std::sin(0.5f * yaw), g_q0[2] = 0, g_q0[3] = std::cos(0.5f * yaw);
    std::memcpy(g_p0, mid, sizeof(mid));
    g_recentred = true;
    g_recentre_gen.fetch_add(1, std::memory_order_relaxed);
    log::info("[cam] recentred: yaw %.2f deg, origin (%.3f %.3f %.3f)", yaw * 57.29578f, mid[0], mid[1], mid[2]);
}

// The camera matrix of eye `eye` (0 left, 1 right, 2 the centre between them): the game camera `cam` composed with
// that eye's located pose. `peek`: the views are read without counting as drawn with (the visibility cover).
bool xr_eye_camera(const float* cam, int eye, float* out, bool peek = false) {
    xr::EyeView v[2];
    if (!(peek ? xr::eye_views_peek(v) : xr::eye_views(v))) return false;
    if (eye == 2) {  // the centre: the eyes' midpoint, the left eye's orientation
        for (int k = 0; k < 3; ++k) v[0].position[k] = 0.5f * (v[0].position[k] + v[1].position[k]);
        eye = 0;
    }
    float q0[4], p0[3];
    {
        std::lock_guard lock(g_recentre_mutex);
        if (!g_recentred) recentre_from(v);
        std::memcpy(q0, g_q0, sizeof(q0));
        std::memcpy(p0, g_p0, sizeof(p0));
    }
    float q0c[4] = {-q0[0], -q0[1], -q0[2], q0[3]}, qrel[4], r0[9], m[9], d[3];
    quat_mul(q0c, v[eye].orientation, qrel);
    quat_matrix(q0c, r0);
    if (g_xr_head_position.load(std::memory_order_relaxed)) {
        for (int k = 0; k < 3; ++k) d[k] = v[eye].position[k] - p0[k];
    } else {  // the eye's offset from the head's centre only
        float s = g_xr_separation.load(std::memory_order_relaxed);
        for (int k = 0; k < 3; ++k) d[k] = s * (v[eye].position[k] - 0.5f * (v[0].position[k] + v[1].position[k]));
        if (peek) d[0] = d[1] = d[2] = 0;  // the cover: the centre
    }
    float prel[3] = {r0[0] * d[0] + r0[1] * d[1] + r0[2] * d[2], r0[3] * d[0] + r0[4] * d[1] + r0[5] * d[2],
                     r0[6] * d[0] + r0[7] * d[1] + r0[8] * d[2]};
    quat_matrix(qrel, m);
    const float* a = cam;
    const float* b = cam + 4;
    const float* c = cam + 8;
    std::memcpy(out, cam, 16 * sizeof(float));
    for (int k = 0; k < 3; ++k) {
        out[k] = m[0] * a[k] + m[3] * b[k] + m[6] * c[k];      // new right = column 0 in the camera's basis
        out[4 + k] = m[1] * a[k] + m[4] * b[k] + m[7] * c[k];  // new up = column 1
        out[8 + k] = m[2] * a[k] + m[5] * b[k] + m[8] * c[k];  // new back = column 2
        out[12 + k] = cam[12 + k] + prel[0] * a[k] + prel[1] * b[k] + prel[2] * c[k];
    }
    return true;
}
alignas(16) char g_saved_vp[0x470 + 0x10];  // render thread only
alignas(16) char g_cover_vp[kViewportSize + 0x10];

float& f32(void* vp, size_t off) { return *reinterpret_cast<float*>(static_cast<char*>(vp) + off); }

// Writes the projection fields for tangents (l < 0 < r, d < 0 < u) and rebuilds the matrix with the engine's own
// Perspective (which also rebuilds the cull planes and regenerates).
void apply_tangents(void* vp, float l, float r, float u, float d) {
    f32(vp, 0x338) = 2.0f * std::atan((u - d) * 0.5f) * 57.29577951f;
    f32(vp, 0x33c) = (r - l) / (u - d);
    f32(vp, 0x350) = 1.0f;
    f32(vp, 0x354) = 1.0f;
    f32(vp, 0x358) = (r + l) / (r - l);
    f32(vp, 0x35c) = (u + d) / (u - d);
    g_perspective(vp);
}

// Reads the tangents back from the matrix: tanR = (1+P20)/P00, tanL = (P20-1)/P00, tanU = (1+P21)/P11,
// tanD = (P21-1)/P11.
float tangent_error(void* vp, float l, float r, float u, float d) {
    float p00 = f32(vp, 0x1c0), p11 = f32(vp, 0x1d4), p20 = f32(vp, 0x1e0), p21 = f32(vp, 0x1e4);
    if (p00 == 0 || p11 == 0) return 1.0f;
    float got[4] = {(p20 - 1) / p00, (1 + p20) / p00, (1 + p21) / p11, (p21 - 1) / p11};
    float want[4] = {l, r, u, d};
    float err = 0;
    for (int k = 0; k < 4; ++k) err = std::fmax(err, std::fabs(got[k] - want[k]) / std::fmax(std::fabs(want[k]), 1e-3f));
    return err;
}

struct Offset {
    Mode mode = Mode::Off;
    float d[3] = {0, 0, 0};
};
std::mutex g_mutex;
Offset g_offset;
std::atomic<bool> g_active{false}, g_log{false};
std::atomic<uint64_t> g_scenes{0}, g_applied{0};
thread_local void* t_scene_vp = nullptr;  // the viewport SceneRender is drawing, while it runs

Offset current() {
    std::lock_guard lock(g_mutex);
    return g_offset;
}

const char* mode_name(Mode m) {
    switch (m) {
        case Mode::Camera: return "camera";
        case Mode::View: return "view";
        case Mode::World: return "world";
        default: return "off";
    }
}

// Camera matrix: four 16-byte rows a, b, c, d (d = position, floats 12..14), as SetCamera reads it.
void move_along_rows(float* m, const float* d) {
    for (int k = 0; k < 3; ++k) m[12 + k] += m[k] * d[0] + m[4 + k] * d[1] + m[8 + k] * d[2];
}

void log_rows(const char* what, const float* m) {
    log::info("[cam] %-10s a(% .4f % .4f % .4f % .4f) b(% .4f % .4f % .4f % .4f) c(% .4f % .4f % .4f % .4f)"
              " d(% .3f % .3f % .3f % .3f)",
              what, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
}

void log_state(void* vp, const float* cam) {
    const char* v = static_cast<const char*>(vp);
    log::info("[cam] SceneRender viewport %p (current viewport before the call %p)", vp,
              *reinterpret_cast<void* const*>(anchors::addr(anchors::Id::ViewportCurrent)));
    if (cam) log_rows("camMtx", cam);
    log_rows("vp+0x40", reinterpret_cast<const float*>(v + 0x40));
    log_rows("vp+0x140", reinterpret_cast<const float*>(v + 0x140));
    log_rows("vp+0x180", reinterpret_cast<const float*>(v + 0x180));
    log_rows("vp+0x1c0", reinterpret_cast<const float*>(v + 0x1c0));
    const float* f = reinterpret_cast<const float*>(v + 0x330);
    const int* n = reinterpret_cast<const int*>(v + 0x330);
    log::info("[cam] vp+0x330.. int %d %d | float fov %.4f aspect %.4f %.4f %.4f | tan %.5f %.5f zoom %.4f %.4f"
              " | offset %.5f %.5f",
              n[0], n[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]);
}

void scene_once(void* renderer, void* vp, const float* cam, void* a4, int pass_eye);

// A double frame (R3, D13): the original runs twice on the same viewport, its 0x470 bytes saved before the first pass
// and restored before the second, so every pointer the frame holds stays vanilla. dual_pass keeps the per-frame state
// the second pass would see consumed, and runs the post chain for the first eye between the passes (with the first
// pass's viewport still current).
std::atomic<float> g_scene_near{0}, g_scene_far{0};

void hk_SceneRender(void* renderer, void* vp, const float* cam, void* a4) {
    if (const float st = g_yaw_step.load(std::memory_order_relaxed); st != 0.0f) {  // the test's ramp, once a frame
        float y = g_yaw.load(std::memory_order_relaxed) + st;
        if (y > 180.0f) y -= 360.0f;
        if (y < -180.0f) y += 360.0f;
        g_yaw.store(y, std::memory_order_relaxed);
    }
    if (g_move_ramp.load(std::memory_order_relaxed)) {  // the test's slide, once a frame
        std::lock_guard lock(g_mutex);
        g_offset.mode = Mode::World;
        for (int k = 0; k < 3; ++k) g_offset.d[k] += g_move_step[k];
        g_active.store(true, std::memory_order_relaxed);
    }
    struct Timed {  // once a frame (no clock reads in the draws under it)
        double t0 = log::now_ms();
        ~Timed() {
            g_scene_us.fetch_add(static_cast<uint64_t>((log::now_ms() - t0) * 1000.0), std::memory_order_relaxed);
            g_scene_timed.fetch_add(1, std::memory_order_relaxed);
        }
    } timed;
    if (vp) {
        g_scene_near = f32(vp, 0x340);
        g_scene_far = f32(vp, 0x344);
    }
    if (g_xr_pose.load(std::memory_order_relaxed)) xr::latch_hands();  // the hands' poses for the IK, as late as the head's
    body::before_scene(cam);
    taa::frame_start();
    g_scene_vp = g_double.load(std::memory_order_relaxed) && g_xr_pose.load(std::memory_order_relaxed) ? vp : nullptr;
    if (cam) {
        std::memcpy(g_game_cam, cam, sizeof(g_game_cam));
        g_game_cam_valid = true;
    }
    if (g_double.load(std::memory_order_relaxed) && vp) {
        if (g_xr_pose.load(std::memory_order_relaxed)) xr::late_latch();
        int first = g_swap.load(std::memory_order_relaxed) ? 1 : 0;
        std::memcpy(g_saved_vp, vp, kViewportSize);
        dual_pass::begin_frame(renderer);
        scene_once(renderer, vp, cam, a4, first);
        dual_pass::between_passes(renderer, first);
        std::memcpy(vp, g_saved_vp, kViewportSize);
        scene_once(renderer, vp, cam, a4, 1 - first);
        dual_pass::end_frame(1 - first);
        g_double_frames.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    dual_pass::mono_frame();
    scene_once(renderer, vp, cam, a4, -1);
}

// One scene pass. pass_eye: -1 mono, 0 left, 1 right (double mode).
void scene_once(void* renderer, void* vp, const float* cam_in, void* a4, int pass_eye) {
    g_scenes.fetch_add(1, std::memory_order_relaxed);
    alignas(16) float yawed[16];
    const float* cam = cam_in;
    float yaw = g_yaw.load(std::memory_order_relaxed);
    if (yaw != 0 && cam_in) {
        std::memcpy(yawed, cam_in, sizeof(yawed));
        yaw_rows(yawed, yaw);
        cam = yawed;
    }
    int eye = g_eye.load(std::memory_order_relaxed);
    xr::EyeView views[2];
    float l = 0, r = 0, u = 0, d = 0;
    bool have = false;
    if (eye == 2) {
        std::lock_guard lock(g_tan_mutex);
        l = g_tan[0], r = g_tan[1], u = g_tan[2], d = g_tan[3];
        have = r > l && u > d;
    } else if (eye >= 0 && xr::eye_views(views)) {
        const float* a = views[eye == 3 ? (pass_eye == 1 ? 1 : 0) : eye].fov;
        l = std::tan(a[0]), r = std::tan(a[1]), u = std::tan(a[2]), d = std::tan(a[3]);
        have = true;
    }
    if (have) {
        apply_tangents(vp, l, r, u, d);  // vp is the render thread's per-frame copy of the master viewport
        g_proj_err = tangent_error(vp, l, r, u, d);
    }
    if (g_log.exchange(false)) log_state(vp, cam);
    void* outer = t_scene_vp;
    t_scene_vp = vp;
    float ipd = g_ipd.load(std::memory_order_relaxed);
    // This pass's camera and the other eye's (for the rage_matrices checker's expectation).
    alignas(16) float copy[16], other[16];
    bool xr_pose = pass_eye >= 0 && cam && g_xr_pose.load(std::memory_order_relaxed) &&
                   xr_eye_camera(cam, pass_eye, copy) && xr_eye_camera(cam, 1 - pass_eye, other);
    if (xr_pose) {
        g_xr_pose_passes.fetch_add(1, std::memory_order_relaxed);
    } else if (cam) {
        std::memcpy(copy, cam, sizeof(copy));
        std::memcpy(other, cam, sizeof(other));
        if (pass_eye >= 0 && ipd != 0) {
            float eo[3] = {pass_eye == 0 ? -0.5f * ipd : 0.5f * ipd, 0, 0}, oo[3] = {-eo[0], 0, 0};
            move_along_rows(copy, eo);
            move_along_rows(other, oo);
        }
    }
    if (cam) ring_probe::matrix_expect(pass_eye, copy + 12, other + 12, cam + 12);
    if (pass_eye >= 0 && (ipd != 0 || xr_pose) && cam) {
        if (have) round_draw::note_pass(pass_eye, copy, l, r, u, d);  // [Reload] RoundInHand: this eye's camera
        o_SceneRender(renderer, vp, copy, a4);
        ring_probe::matrix_expect(-2, nullptr, nullptr, nullptr);
        if (pass_eye == (g_swap.load() ? 1 : 0) && g_findcam.exchange(false)) {
            log::info("[findcam] after the first pass: centre camera (%.3f %.3f %.3f), this eye (%.3f %.3f %.3f)", cam[12],
                      cam[13], cam[14], copy[12], copy[13], copy[14]);
            diag::find_pattern(cam + 12, 12, "centre camera position", 48);
            diag::find_pattern(copy + 12, 12, "this eye's camera position", 16);
        }
        t_scene_vp = outer;
        return;
    }
    if (g_active.load(std::memory_order_relaxed)) {
        Offset o = current();
        if (o.mode == Mode::Camera && cam) {
            alignas(16) float moved[16];
            std::memcpy(moved, cam, sizeof(moved));
            move_along_rows(moved, o.d);
            g_applied.fetch_add(1, std::memory_order_relaxed);
            o_SceneRender(renderer, vp, moved, a4);
            t_scene_vp = outer;
            return;
        }
    }
    o_SceneRender(renderer, vp, cam, a4);
    ring_probe::matrix_expect(-2, nullptr, nullptr, nullptr);
    t_scene_vp = outer;
}

void hk_SetCamera(void* vp, const float* cam) {
    o_SetCamera(vp, cam);
    if (vp != t_scene_vp || !g_active.load(std::memory_order_relaxed)) return;
    Offset o = current();
    char* v = static_cast<char*>(vp);
    if (o.mode == Mode::View) {
        float* view = reinterpret_cast<float*>(v + 0x180);
        for (int k = 0; k < 3; ++k) view[12 + k] -= o.d[k];
    } else if (o.mode == Mode::World) {
        move_along_rows(reinterpret_cast<float*>(v + 0x40), o.d);
    } else {
        return;
    }
    g_applied.fetch_add(1, std::memory_order_relaxed);
    g_regenerate(vp, 1);
}

alignas(16) float g_cover_cam[16];  // the yawed camera handed to the visibility build (it may keep the pointer)

void hk_VisibilityBuild(void* renderer, void* vp, void* cam, void* a4) {
    body::on_visibility_build();  // the item in hand's game matrix, for its draws (body.cpp)
    if (vp && cam && g_head_cover.load(std::memory_order_relaxed) && g_xr_pose.load(std::memory_order_relaxed) &&
        g_double.load(std::memory_order_relaxed)) {
        xr::EyeView v[2];
        if (xr::eye_views_peek(v) && xr_eye_camera(static_cast<const float*>(cam), 2, g_cover_cam, true)) {
            std::memcpy(g_cover_vp, vp, kViewportSize);
            o_SetCamera(g_cover_vp, g_cover_cam);
            const float m = kCoverMarginDeg / 57.29577951f;
            float hw = 0, hh = 0;
            for (int e = 0; e < 2; ++e) {
                hw = std::fmax(hw, std::fmax(std::tan(-v[e].fov[0] + m), std::tan(v[e].fov[1] + m)));
                hh = std::fmax(hh, std::fmax(std::tan(v[e].fov[2] + m), std::tan(-v[e].fov[3] + m)));
            }
            apply_tangents(g_cover_vp, -hw, hw, hh, -hh);
            if (g_head_cover_builds.fetch_add(1, std::memory_order_relaxed) == 0)
                log::info("[cam] head cover: culling for the head pose, %.1f x %.1f deg (both eyes + %.0f deg)",
                          2 * std::atan(hw) * 57.29578f, 2 * std::atan(hh) * 57.29578f, kCoverMarginDeg);
            o_VisibilityBuild(renderer, g_cover_vp, g_cover_cam, a4);
            if (g_level_w2s.load(std::memory_order_relaxed) && level_from(static_cast<const float*>(cam), g_cover_cam, g_w2s_cam)) {
                std::memcpy(g_w2s_vp, g_cover_vp, kViewportSize);
                o_SetCamera(g_w2s_vp, g_w2s_cam);
                *reinterpret_cast<void**>(anchors::addr(anchors::Id::WorldToScreenMatrix)) = g_w2s_vp;
            }
            return;
        }
    }
    float vfov = g_cover_vfov.load(std::memory_order_relaxed);
    float yaw = g_yaw.load(std::memory_order_relaxed);
    if ((vfov <= 0 && yaw == 0) || !vp) {
        o_VisibilityBuild(renderer, vp, cam, a4);
        return;
    }
    std::memcpy(g_cover_vp, vp, kViewportSize);
    void* use_cam = cam;
    if (yaw != 0 && cam) {
        // Cull for the view the scene pass will draw (the truth capture's yaw presets).
        std::memcpy(g_cover_cam, cam, sizeof(g_cover_cam));
        yaw_rows(g_cover_cam, yaw);
        o_SetCamera(g_cover_vp, g_cover_cam);
        use_cam = g_cover_cam;
    }
    if (vfov > 0) {
        float t = std::tan(vfov * 0.5f / 57.29577951f), asp = g_cover_aspect.load();
        apply_tangents(g_cover_vp, -t * asp, t * asp, t, -t);
    }
    g_cover_builds.fetch_add(1, std::memory_order_relaxed);
    o_VisibilityBuild(renderer, g_cover_vp, use_cam, a4);
}

}  // namespace

// ---- tree billboards (headset round 2: "distant trees rotate with the headset"). VisibilityCaller (0x5c7c68) hands
// BillboardDispatch a copy of the master viewport, the game camera; the generator tasks build every distant tree's
// quad facing that camera (cycle 48: the copy at 0x142c0cf60 holds the game camera with or without the head cover).
// In the headset the eyes look elsewhere, so the quads are seen at an angle and appear to turn with the head. With the
// eyes from the XR views the dispatch gets the centre eye's position with the head's yaw only, level (world up): the
// quads face the head and stay upright ([XR] LevelBillboards).
using BillboardDispatch_t = void (*)(void* a1, void* vp);
BillboardDispatch_t o_BillboardDispatch = nullptr;
std::atomic<bool> g_level_billboards{true};
std::atomic<bool> g_level_grass{true};
 // round 3: the forest manager's view-plane billboards (dual_pass)
std::atomic<uint64_t> g_level_billboard_frames{0};
alignas(16) char g_bb_vp[kViewportSize + 0x10];
alignas(16) float g_bb_cam[16];
// Round 3b (the pipeline census): the turning distant trees are the generator's billboards (rdr2_billboard
// VSBillboard_NoCloudShadows). Their corners come from FUN_14087cc00 -> FUN_14087c910, which, given a direction (the
// camera manager's **(0x142ad35f0+0x28)+0x30), builds each quad looking along that one direction: the live view
// direction, so in the headset every tree spins with the head's yaw and leans with its pitch. In VR each quad now gets
// its own direction, from the centre eye to the tree's box centre, flattened: it faces you, stays upright, and does
// not turn when the head does. The eye position is taken on the render thread at the dispatch (the generator runs on
// worker threads). [XR] LevelBillboards; "billboards facing toward|away" flips the sign (test aid).
float g_bb_eye[3] = {0, 0, 0};
std::atomic<bool> g_bb_eye_valid{false};
std::atomic<float> g_bb_sign{1.0f};
std::atomic<uint64_t> g_bb_corner_calls{0};
using BillboardCorners_t = void (*)(void* tree, void* vp, const float* dir, const float* mn, const float* mx, void* c0, float* c1,
                                    float* c2, float* c3);
BillboardCorners_t o_BillboardCorners = nullptr;

void hk_BillboardCorners(void* tree, void* vp, const float* dir, const float* mn, const float* mx, void* c0, float* c1, float* c2,
                         float* c3) {
    if (dir && mn && mx && g_bb_eye_valid.load(std::memory_order_relaxed) && g_level_billboards.load(std::memory_order_relaxed)) {
        float s = g_bb_sign.load(std::memory_order_relaxed);
        float dx = 0.5f * (mn[0] + mx[0]) - g_bb_eye[0], dz = 0.5f * (mn[2] + mx[2]) - g_bb_eye[2];
        float n = std::sqrt(dx * dx + dz * dz);
        if (n > 1e-3f) {
            alignas(16) float d[4] = {s * dx / n, 0.0f, s * dz / n, 0.0f};
            g_bb_corner_calls.fetch_add(1, std::memory_order_relaxed);
            o_BillboardCorners(tree, vp, d, mn, mx, c0, c1, c2, c3);
            return;
        }
    }
    o_BillboardCorners(tree, vp, dir, mn, mx, c0, c1, c2, c3);
}

// FUN_14087ad90 (mode, box min, box max, out direction, out centre, out extent): a diffuse tree imposter's capture
// direction, the current viewport camera's back row with its up component dead-zoned (0.2, rescaled by 0.8) and
// normalised; FUN_14087afc0 / FUN_14087b570 then render the tree type's colour and depth images orthographically from
// 5000 m out along it. The diffuse refresh (FUN_1405a4b90, a few imposters per scene pass) runs inside SceneRender, so
// in stereo the current camera is an eye's and the imposters, one per tree type and shared by every tree, are
// re-captured along the head: the distant trees turned with it in yaw and pitch, never roll (headset rounds 2-3c; the
// run 2 item 11 review). The shadow imposters take the sun's direction and do not come here.
// [XR] LevelImposters: in a stereo frame, along the game camera's back row instead (its heading, level).
using ImposterDir_t = void (*)(char mode, const float* mn, const float* mx, float* dir, float* centre, float* extent);
ImposterDir_t o_ImposterDir = nullptr;
std::atomic<bool> g_level_imposters{false};
std::atomic<uint64_t> g_imposter_calls{0}, g_imposter_levelled{0};

void hk_ImposterDir(char mode, const float* mn, const float* mx, float* dir, float* centre, float* extent) {
    o_ImposterDir(mode, mn, mx, dir, centre, extent);
    g_imposter_calls.fetch_add(1, std::memory_order_relaxed);
    if (!dir || !g_level_imposters.load(std::memory_order_relaxed) || !g_double.load(std::memory_order_relaxed) ||
        !g_xr_pose.load(std::memory_order_relaxed) || !g_game_cam_valid.load(std::memory_order_relaxed))
        return;
    float d[3] = {g_game_cam[8], g_game_cam[9], g_game_cam[10]};
    float& u = mode ? d[2] : d[1];  // the game's own dead zone on the up component
    u = u >= 0.0f ? (u - 0.2f > 0.0f ? u - 0.2f : 0.0f) : (u + 0.2f < 0.0f ? u + 0.2f : 0.0f);
    u /= 0.8f;
    const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (n < 1e-4f) return;
    for (int k = 0; k < 3; ++k) dir[k] = d[k] / n;
    if (g_imposter_levelled.fetch_add(1, std::memory_order_relaxed) == 0)
        log::info("[cam] tree imposters captured along the game camera's heading (%.3f %.3f %.3f)", dir[0], dir[1], dir[2]);
}

void hk_BillboardDispatch(void* a1, void* vp) {
    if (vp && g_level_billboards.load(std::memory_order_relaxed) && g_xr_pose.load(std::memory_order_relaxed) &&
        g_double.load(std::memory_order_relaxed)) {
        const float* cam = reinterpret_cast<const float*>(static_cast<char*>(vp) + 0x40);
        float c[16];
        if (xr_eye_camera(cam, 2, c, true)) {
            // level: the game camera's heading (not the head's), up = world y, at the centre eye
            float bx = cam[8], bz = cam[10], n = std::sqrt(bx * bx + bz * bz);
            if (n > 1e-4f) {
                bx /= n;
                bz /= n;
                float m[16] = {bz, 0, -bx, 0, 0, 1, 0, 0, bx, 0, bz, 0, c[12], c[13], c[14], 1};
                std::memcpy(g_bb_cam, m, sizeof(m));
                g_bb_eye[0] = c[12];
                g_bb_eye[1] = c[13];
                g_bb_eye[2] = c[14];
                g_bb_eye_valid = true;
                std::memcpy(g_bb_vp, vp, kViewportSize);
                o_SetCamera(g_bb_vp, g_bb_cam);
                if (g_level_billboard_frames.fetch_add(1, std::memory_order_relaxed) == 0)
                    log::info("[cam] tree billboards at the centre eye, level, with the game camera's heading (%.3f %.3f)", bx, bz);
                o_BillboardDispatch(a1, g_bb_vp);
                return;
            }
        }
    }
    g_bb_eye_valid = false;
    o_BillboardDispatch(a1, vp);
}

bool install() {
    if (anchors::stand_down()) return false;
    g_regenerate = reinterpret_cast<Regenerate_t>(anchors::addr(anchors::Id::ViewportRegenerate));
    g_perspective = reinterpret_cast<Perspective_t>(anchors::addr(anchors::Id::ViewportPerspective));
    g_head_cover = config::get_bool("XR", "HeadCover", true);
    bool ok = hooks::install("RDR SceneRender", reinterpret_cast<void*>(anchors::addr(anchors::Id::SceneRender)),
                             hk_SceneRender, &o_SceneRender);
    ok = hooks::install("RDR grcViewport::SetCamera", reinterpret_cast<void*>(anchors::addr(anchors::Id::ViewportSetCamera)),
                        hk_SetCamera, &o_SetCamera) &&
         ok;
    ok = hooks::install("RDR VisibilityBuild", reinterpret_cast<void*>(anchors::addr(anchors::Id::VisibilityBuild)),
                        hk_VisibilityBuild, &o_VisibilityBuild) &&
         ok;
    g_level_billboards = config::get_bool("XR", "LevelBillboards", true);
    g_level_grass = config::get_bool("XR", "LevelGrass", true);
    g_level_w2s = config::get_bool("XR", "LevelForestView", true);
    ok = hooks::install("RDR viewport push globals", reinterpret_cast<void*>(anchors::addr(anchors::Id::ViewportPushGlobals)),
                        hk_PushGlobals, &o_PushGlobals) &&
         ok;
    ok = hooks::install("RDR BillboardCorners", reinterpret_cast<void*>(anchors::addr(anchors::Id::BillboardCorners)),
                        hk_BillboardCorners, &o_BillboardCorners) &&
         ok;
    ok = hooks::install("RDR BillboardDispatch", reinterpret_cast<void*>(anchors::addr(anchors::Id::BillboardDispatch)),
                        hk_BillboardDispatch, &o_BillboardDispatch) &&
         ok;
    g_level_imposters = config::get_bool("XR", "LevelImposters", true);
    ok = hooks::install("RDR tree imposter direction", reinterpret_cast<void*>(anchors::addr(anchors::Id::ImposterDir)), hk_ImposterDir,
                        &o_ImposterDir) &&
         ok;
    log::info("[cam] tree imposters captured along %s", g_level_imposters.load() ? "the game camera's heading ([XR] LevelImposters)" : "the eye's view (the game's)");
    return ok;
}

void set(Mode mode, float dx, float dy, float dz) {
    {
        std::lock_guard lock(g_mutex);
        g_offset.mode = mode;
        g_offset.d[0] = dx;
        g_offset.d[1] = dy;
        g_offset.d[2] = dz;
    }
    g_active = mode != Mode::Off;
    log::info("[cam] lever %s (%.4f, %.4f, %.4f) m", mode_name(mode), dx, dy, dz);
}

void request_log() { g_log = true; }

void set_eye_projection(int eye) {
    g_eye = (eye == 0 || eye == 1 || eye == 3) ? eye : -1;
    log::info("[cam] eye projection %s", eye == 0 ? "left" : eye == 1 ? "right" : eye == 3 ? "per pass (XR views)" : "off");
}

void set_tangents(float l, float r, float u, float d) {
    {
        std::lock_guard lock(g_tan_mutex);
        g_tan[0] = l, g_tan[1] = r, g_tan[2] = u, g_tan[3] = d;
    }
    g_eye = 2;
    log::info("[cam] explicit projection tangents L %.4f R %.4f U %.4f D %.4f", l, r, u, d);
}

void set_yaw(float deg) {
    g_yaw = deg;
    log::info("[cam] yaw %.2f deg", deg);
}

void set_move_ramp(float dx, float dy, float dz) {
    const bool on = dx != 0.0f || dy != 0.0f || dz != 0.0f;
    {
        std::lock_guard lock(g_mutex);
        g_move_step[0] = dx;
        g_move_step[1] = dy;
        g_move_step[2] = dz;
        if (!on) {
            g_offset.mode = Mode::Off;
            g_offset.d[0] = g_offset.d[1] = g_offset.d[2] = 0;
        }
    }
    g_move_ramp = on;
    if (!on) g_active = false;
    log::info("[cam] move ramp (%.4f, %.4f, %.4f) m a frame%s", dx, dy, dz, on ? "" : " (off, the offset taken off)");
}

void set_yaw_ramp(float deg_per_frame) {
    g_yaw_step = deg_per_frame;
    log::info("[cam] yaw ramp %.3f deg a frame (yaw now %.2f)", deg_per_frame, g_yaw.load());
}

void set_double(bool on, float ipd, bool swap) {
    g_ipd = ipd;
    g_swap = swap;
    g_double = on;
    log::info("[cam] double scene pass %s (ipd %.4f m, %s first)", on ? "on" : "off", ipd, swap ? "right" : "left");
}

void set_cover(float vfov_deg, float aspect) {
    if (vfov_deg <= 0 || vfov_deg >= 170 || aspect <= 0) vfov_deg = 0;
    g_cover_aspect = aspect;
    g_cover_vfov = vfov_deg;
    log::info("[cam] cover viewport %s (vfov %.2f deg, aspect %.3f)", vfov_deg > 0 ? "on" : "off", vfov_deg, aspect);
}

bool swap_order() { return g_double.load() && g_swap.load(); }

void set_xr_pose(bool on) {
    g_xr_pose = on;
    log::info("[cam] eye cameras from the XR views %s", on ? "on" : "off");
}

bool level_grass_active() {
    return g_level_grass.load(std::memory_order_relaxed) && g_xr_pose.load(std::memory_order_relaxed) &&
           g_double.load(std::memory_order_relaxed);
}

bool game_heading(float* bx, float* bz) {
    if (!g_game_cam_valid.load(std::memory_order_relaxed)) return false;
    *bx = g_game_cam[8];
    *bz = g_game_cam[10];
    return true;
}

void set_billboard_sign(bool toward) {
    g_bb_sign = toward ? 1.0f : -1.0f;
    log::info("[cam] billboard direction: eye -> tree x %.0f (corner calls so far %llu)", g_bb_sign.load(),
              static_cast<unsigned long long>(g_bb_corner_calls.load()));
}

void set_level_w2s(bool on) {
    g_level_w2s = on;
    log::info("[cam] forest view (worldToScreen) %s", on ? "level (head yaw only)" : "the head cover's");
}

void set_vi_level(bool on) {
    g_vi_level = on;
    log::info("[cam] experiment: ViewInverse levelling %s", on ? "on" : "off");
}

void set_level_grass(bool on) {
    g_level_grass = on;
    log::info("[cam] grass-system billboards %s", on ? "upright (level camera rows)" : "against the eye's view plane");
}

void set_level_imposters(bool on) {
    g_level_imposters = on;
    log::info("[cam] tree imposters captured along %s (directions %llu, levelled %llu)", on ? "the game camera's heading" : "the eye's view",
              static_cast<unsigned long long>(g_imposter_calls.load()), static_cast<unsigned long long>(g_imposter_levelled.load()));
}
bool level_imposters() { return g_level_imposters.load(); }
std::string imposter_status() {
    char b[160];
    std::snprintf(b, sizeof(b), "imposters %s, directions %llu, levelled %llu", g_level_imposters.load() ? "game" : "eye",
                  static_cast<unsigned long long>(g_imposter_calls.load()), static_cast<unsigned long long>(g_imposter_levelled.load()));
    return b;
}

void set_level_billboards(bool on) {
    g_level_billboards = on;
    log::info("[cam] tree billboards %s", on ? "face the centre eye, level" : "face the game camera");
}

void set_head_cover(bool on) {
    g_head_cover = on;
    log::info("[cam] head cover %s", on ? "on" : "off (cull for the game camera)");
}

void set_xr_head_position(bool on, float separation) {
    bool was = g_xr_head_position.exchange(on);
    g_xr_separation = separation;
    if (was != on) log::info("[cam] XR head position %s (separation x%.2f)", on ? "tracked" : "ignored, rotation only", separation);
}

uint64_t scene_calls() { return g_scenes.load(std::memory_order_relaxed); }
void scene_time(uint64_t* us, uint64_t* frames) {
    *frames = g_scene_timed.load(std::memory_order_relaxed);
    *us = g_scene_us.load(std::memory_order_relaxed);
}

bool head_yaw_deg(float* out) {
    xr::EyeView v[2];
    // a peek: read by frame-end listeners (pose, gestures), not a scene pass; counted as drawn with, it made the pose
    // check a copy of the submitted views (run 8 item 6b)
    if (!xr::eye_views_peek(v)) return false;
    float q0[4];
    {
        std::lock_guard lock(g_recentre_mutex);
        if (!g_recentred) return false;
        std::memcpy(q0, g_q0, sizeof(q0));
    }
    float q0c[4] = {-q0[0], -q0[1], -q0[2], q0[3]}, q[4];
    quat_mul(q0c, v[0].orientation, q);
    // the head's forward (-z) in the recentred frame, then its angle in the horizontal plane
    float fx = -2 * (q[0] * q[2] + q[3] * q[1]), fz = -(1 - 2 * (q[0] * q[0] + q[1] * q[1]));
    *out = std::atan2(-fx, -fz) * 57.29578f;
    return true;
}

bool head_offset(float out[3]) {
    xr::EyeView v[2];
    if (!xr::eye_views_peek(v)) return false;
    float q0[4], p0[3];
    {
        std::lock_guard lock(g_recentre_mutex);
        if (!g_recentred) return false;
        std::memcpy(q0, g_q0, sizeof(q0));
        std::memcpy(p0, g_p0, sizeof(p0));
    }
    float q0c[4] = {-q0[0], -q0[1], -q0[2], q0[3]}, r0[9], d[3];
    quat_matrix(q0c, r0);
    for (int k = 0; k < 3; ++k) d[k] = 0.5f * (v[0].position[k] + v[1].position[k]) - p0[k];
    for (int r = 0; r < 3; ++r) out[r] = r0[r * 3 + 0] * d[0] + r0[r * 3 + 1] * d[1] + r0[r * 3 + 2] * d[2];
    return true;
}

uint32_t recentre_gen() { return g_recentre_gen.load(std::memory_order_relaxed); }

bool neck_offset(float out[3]) {
    xr::EyeView v[2];
    if (!xr::eye_views_peek(v)) return false;
    float q0[4], p0[3];
    {
        std::lock_guard lock(g_recentre_mutex);
        if (!g_recentred) return false;
        std::memcpy(q0, g_q0, sizeof(q0));
        std::memcpy(p0, g_p0, sizeof(p0));
    }
    constexpr float kNeck[3] = {0.0f, -0.10f, 0.08f};  // the neck from the centre eye, in the head's axes (y up, z back)
    float rh[9], q0c[4] = {-q0[0], -q0[1], -q0[2], q0[3]}, r0[9], d[3];
    quat_matrix(v[0].orientation, rh);
    quat_matrix(q0c, r0);
    for (int k = 0; k < 3; ++k)
        d[k] = 0.5f * (v[0].position[k] + v[1].position[k]) + rh[k * 3 + 0] * kNeck[0] + rh[k * 3 + 1] * kNeck[1] + rh[k * 3 + 2] * kNeck[2] - p0[k];
    for (int r = 0; r < 3; ++r) out[r] = r0[r * 3 + 0] * d[0] + r0[r * 3 + 1] * d[1] + r0[r * 3 + 2] * d[2] - kNeck[r];
    return true;
}

bool local_to_world(const float* cam, const float* pos, const float* rot, float* wpos, float* wrot) {
    if (!cam) return false;
    float q0[4], p0[3];
    {
        std::lock_guard lock(g_recentre_mutex);
        if (!g_recentred) return false;
        std::memcpy(q0, g_q0, sizeof(q0));
        std::memcpy(p0, g_p0, sizeof(p0));
    }
    float q0c[4] = {-q0[0], -q0[1], -q0[2], q0[3]}, r0[9], d[3], prel[3];
    quat_matrix(q0c, r0);
    for (int k = 0; k < 3; ++k) d[k] = pos[k] - p0[k];
    for (int r = 0; r < 3; ++r) prel[r] = r0[r * 3 + 0] * d[0] + r0[r * 3 + 1] * d[1] + r0[r * 3 + 2] * d[2];
    const float *a = cam, *b = cam + 4, *c = cam + 8;
    for (int k = 0; k < 3; ++k) wpos[k] = cam[12 + k] + prel[0] * a[k] + prel[1] * b[k] + prel[2] * c[k];
    if (rot && wrot) {
        float qrel[4], m[9];
        quat_mul(q0c, rot, qrel);
        quat_matrix(qrel, m);
        for (int r = 0; r < 3; ++r)
            for (int col = 0; col < 3; ++col) wrot[r * 3 + col] = a[r] * m[0 * 3 + col] + b[r] * m[1 * 3 + col] + c[r] * m[2 * 3 + col];
    }
    return true;
}

bool world_to_local(const float* cam, const float* wpos, float* lpos) {
    if (!cam || !wpos || !lpos) return false;
    float q0[4], p0[3];
    {
        std::lock_guard lock(g_recentre_mutex);
        if (!g_recentred) return false;
        std::memcpy(q0, g_q0, sizeof(q0));
        std::memcpy(p0, g_p0, sizeof(p0));
    }
    // local_to_world: w = d + N prel (N's columns the rows a, b, c), prel = R0^T (local - p0); so prel = N^-1 (w - d)
    // (N^-1's rows b x c, c x a, a x b over a . (b x c)) and local = p0 + R0 prel
    const float *a = cam, *b = cam + 4, *c = cam + 8;
    const float bc[3] = {b[1] * c[2] - b[2] * c[1], b[2] * c[0] - b[0] * c[2], b[0] * c[1] - b[1] * c[0]};
    const float ca[3] = {c[1] * a[2] - c[2] * a[1], c[2] * a[0] - c[0] * a[2], c[0] * a[1] - c[1] * a[0]};
    const float ab[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    const float det = a[0] * bc[0] + a[1] * bc[1] + a[2] * bc[2];
    if (std::fabs(det) < 1e-6f) return false;
    const float v[3] = {wpos[0] - cam[12], wpos[1] - cam[13], wpos[2] - cam[14]};
    const float prel[3] = {(bc[0] * v[0] + bc[1] * v[1] + bc[2] * v[2]) / det, (ca[0] * v[0] + ca[1] * v[1] + ca[2] * v[2]) / det,
                           (ab[0] * v[0] + ab[1] * v[1] + ab[2] * v[2]) / det};
    float r0[9];
    quat_matrix(q0, r0);
    for (int r = 0; r < 3; ++r) lpos[r] = p0[r] + r0[r * 3] * prel[0] + r0[r * 3 + 1] * prel[1] + r0[r * 3 + 2] * prel[2];
    return true;
}

bool eyes_follow_head() {
    return g_xr_pose.load(std::memory_order_relaxed) && g_double.load(std::memory_order_relaxed) &&
           g_xr_head_position.load(std::memory_order_relaxed);
}

bool xr_double() { return g_xr_pose.load(std::memory_order_relaxed) && g_double.load(std::memory_order_relaxed); }

bool local_rel(const float* pos, float* prel) {
    float q0[4], p0[3];
    {
        std::lock_guard lock(g_recentre_mutex);
        if (!g_recentred) return false;
        std::memcpy(q0, g_q0, sizeof(q0));
        std::memcpy(p0, g_p0, sizeof(p0));
    }
    float q0c[4] = {-q0[0], -q0[1], -q0[2], q0[3]}, r0[9], d[3];
    quat_matrix(q0c, r0);
    for (int k = 0; k < 3; ++k) d[k] = pos[k] - p0[k];
    for (int r = 0; r < 3; ++r) prel[r] = r0[r * 3 + 0] * d[0] + r0[r * 3 + 1] * d[1] + r0[r * 3 + 2] * d[2];
    return true;
}

void recentre() {
    xr::recentre_layers();
    pose::on_recentre();
    std::lock_guard lock(g_recentre_mutex);
    g_recentred = false;  // the next pass recentres on the current views
}

void find_centre_camera() {
    g_findcam = true;
    log::info("[findcam] armed for the next double frame");
}

void describe_current_viewport(char* out, size_t len) {
    auto* cur = *reinterpret_cast<char**>(anchors::addr(anchors::Id::ViewportCurrent));
    auto* renderer = *reinterpret_cast<char**>(anchors::addr(anchors::Id::RendererSingleton));
    const char* which = !cur ? "none"
                        : cur == t_scene_vp ? "scene"
                        : (renderer && cur == renderer + 0x870) ? "master"
                        : cur == g_cover_vp ? "cover"
                                            : "other";
    if (!cur) {
        std::snprintf(out, len, "current viewport none");
        return;
    }
    const float* d = reinterpret_cast<const float*>(cur + 0x70);
    std::snprintf(out, len, "current viewport %s %p (scene %p) camera (%.3f %.3f %.3f)", which, static_cast<void*>(cur),
                  t_scene_vp, d[0], d[1], d[2]);
}

void status_text(char* out, size_t len) {
    Offset o = current();
    std::snprintf(out, len, "%s (%.4f %.4f %.4f) scenes %llu applied %llu | eye proj %d err %.6f | cover %.2f deg x %.3f (%llu builds)"
                  " | double %d ipd %.4f swap %d (%llu frames) | xr pose %d (%llu passes)",
                  mode_name(o.mode), o.d[0], o.d[1], o.d[2], static_cast<unsigned long long>(g_scenes.load()),
                  static_cast<unsigned long long>(g_applied.load()), g_eye.load(), g_proj_err.load(), g_cover_vfov.load(),
                  g_cover_aspect.load(), static_cast<unsigned long long>(g_cover_builds.load()), g_double.load() ? 1 : 0,
                  g_ipd.load(), g_swap.load() ? 1 : 0, static_cast<unsigned long long>(g_double_frames.load()),
                  g_xr_pose.load() ? 1 : 0, static_cast<unsigned long long>(g_xr_pose_passes.load()));
    xr::EyeView v[2];
    if (xr::eye_views_peek(v)) {  // the eyes' FOV now (tangents left right up down), what the projection follows
        const size_t n = std::strlen(out);
        std::snprintf(out + n, n < len ? len - n : 0, " | eye tan L (%.3f %.3f %.3f %.3f) R (%.3f %.3f %.3f %.3f)", std::tan(v[0].fov[0]),
                      std::tan(v[0].fov[1]), std::tan(v[0].fov[2]), std::tan(v[0].fov[3]), std::tan(v[1].fov[0]), std::tan(v[1].fov[1]),
                      std::tan(v[1].fov[2]), std::tan(v[1].fov[3]));
    }
}

void* scene_viewport() { return t_scene_vp; }
void scene_clip(float* near_m, float* far_m) {
    *near_m = g_scene_near.load();
    *far_m = g_scene_far.load();
}

bool mono_camera_changed() {
    if (g_yaw.load(std::memory_order_relaxed) != 0) return true;
    return g_active.load(std::memory_order_relaxed) && current().mode == Mode::Camera;
}

}  // namespace rdrvr::camera_lever
