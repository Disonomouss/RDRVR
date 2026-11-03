#include "core/camera_lever.h"
#include "core/round_draw.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "core/aim.h"
#include "core/anchors.h"
#include "core/api.h"
#include "core/diag.h"
#include "core/dual_pass.h"
#include "core/frame_probe.h"
#include "core/taa.h"
#include "core/hooks.h"
#include "core/body.h"
#include "core/config.h"
#include "core/log.h"
#include "core/pose.h"
#include "core/ring_probe.h"
#include "core/prepass_arrays.h"
#include "core/scope.h"
#include "core/state.h"
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
std::atomic<AfterPush_t> g_after_push{nullptr};  // [Stereo] PrepassWvpMirror (set_after_push; test only): unset, one relaxed load

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
            if (const AfterPush_t fn = g_after_push.load(std::memory_order_relaxed)) fn(vp, scene_viewport());
            return;
        }
    }
    o_PushGlobals(vp, push_view_inverse);
    if (const AfterPush_t fn = g_after_push.load(std::memory_order_relaxed)) fn(vp, scene_viewport());
} std::atomic<bool> g_double{false}, g_swap{false};
// (r55 item 11, [Scope]) the scope's own change of the eyes' order (single pass, the scope at the right eye, [Scope]
// SinglePassSwap): set by scope::frame_begin at a frame's start, before its passes; the frame's order is g_swap's turned by it
std::atomic<bool> g_scope_swap{false};
bool eff_swap() { return g_swap.load(std::memory_order_relaxed) != g_scope_swap.load(std::memory_order_relaxed); }
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

// [Body] FirstPersonMoments (camera_lever.h): the anchor's camera matrix kept while it is current (the reference, with
// the view's heading then); while a script's camera holds the channel, the frame's camera is the reference turned by
// the heading since (yaw_rows: about the camera's up, level) at the place the plugin put the anchor's camera this tick
std::atomic<bool> g_fpm{false};
std::mutex g_fp_mutex;
alignas(16) float g_fp_ref[16];  // under g_fp_mutex
float g_fp_ref_heading = 0.0f;
bool g_fp_ref_ok = false, g_fp_was = false;
alignas(16) float g_fp_mark[16];  // "cam fp mark": a reference kept for the check ("cam fp check")
float g_fp_mark_heading = 0.0f;
bool g_fp_mark_ok = false;
thread_local alignas(16) float t_fp_cam[16];
std::atomic<uint64_t> g_fp_frames{0}, g_fp_moments{0};
void fp_build(const float* ref, float ref_heading, const RdrvrActorState& st, float heading, float* out) {
    std::memcpy(out, ref, 64);
    yaw_rows(out, heading - ref_heading);
    for (int k = 0; k < 3; ++k) out[12 + k] = st.camera_pos[k];
}
// the frame's camera for the scene and the culling: the game's, or (a first-person moment) the anchor's
const float* fp_cam(const float* cam) {
    if (!cam || !g_fpm.load(std::memory_order_relaxed) || !pose::anchor_active()) return cam;
    RdrvrActorState st{};
    if (!api::actor_state(&st) || !st.camera) return cam;
    const float hd = pose::body_heading_deg();
    std::lock_guard lock(g_fp_mutex);
    if (!(st.flags & RDRVR_ACTOR_CAM_AWAY)) {  // ours current: the reference
        // taken only from the anchor's own camera (at the place the plugin put it, level: a script's camera a frame
        // early is neither) with the heading still (the matrix lags the heading by a tick: after a turn, three frames)
        static float last_hd = 0.0f;
        static int still = 0;
        still = std::fabs(hd - last_hd) < 0.5f ? still + 1 : 0;
        last_hd = hd;
        const float dx = cam[12] - st.camera_pos[0], dy = cam[13] - st.camera_pos[1], dz = cam[14] - st.camera_pos[2];
        if (still >= 3 && dx * dx + dy * dy + dz * dz < 0.3f * 0.3f && cam[5] > 0.95f) {
            std::memcpy(g_fp_ref, cam, 64);
            g_fp_ref_heading = hd;
            g_fp_ref_ok = true;
        }
        if (g_fp_was) log::info("[cam] first-person moment over: the anchor's camera current again");
        g_fp_was = false;
        return cam;
    }
    if (!g_fp_ref_ok) return cam;
    if (!g_fp_was) {
        g_fp_moments.fetch_add(1, std::memory_order_relaxed);
        log::info("[cam] first-person moment: a script's camera holds the channel; the eyes kept at John's head");
    }
    g_fp_was = true;
    fp_build(g_fp_ref, g_fp_ref_heading, st, hd, t_fp_cam);
    g_fp_frames.fetch_add(1, std::memory_order_relaxed);
    return t_fp_cam;
}
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
// [XR] RoomScale (2026-10-27; the user: "Implement roomscale and physical crouch", research/run27/roomscale.md B2): while
// the room-scale walk owns John's walk, every metre his anchored camera moves is taken off the head's offset by moving
// g_p0 (room_shift_origin), so the eye's world place moves only with the head and John walks in under it. The take-off
// is the frame's own camera (room_take_frame, in hk_SceneRender before anything reads g_p0), horizontal only: his whole
// move a game tick as a vector (cycle r163), at most a walk's 2.5 m/s over the tick's time in length: a train or a wagon
// under John, a lasso's drag, a ragdoll or a stagger is not his walk, its excess stays in the view (and pose.cpp's gates
// stand the walk down for them). pose.cpp's own
// shifts (the blocked clamp, the re-anchor) are queued and applied here too, at the frame's start, so both eyes' passes
// see one origin. g_recentre_gen is NOT bumped (the holsters' headset anchor would take its places again): only a
// recentre does that. g_p0_base keeps the recentre's own origin, for local_rel (the hand's own motion, the gestures'
// swings: a shifted origin would read John's walk as the hands' motion). Under g_recentre_mutex
float g_p0_base[3] = {0, 0, 0};
float g_room_shift[3] = {0, 0, 0};  // every shift so far, LOCAL raw metres (never reset: its differences are used)
float g_room_q[2] = {0, 0};         // shifts queued by pose.cpp (game metres along the recentred x and z), applied at the frame's start
// bit 1: the walk owns the take-off; bit 2: a shift queued. 0 (the option off, nothing queued): room_take_frame is one
// relaxed load of this and nothing else
std::atomic<int> g_room_state{0};
std::atomic<bool> g_room_take_on{true};  // "pose room take off" (a test's negative control for the judder): no take-off
// the judder's spread (cycle r159: a 23 mm peak against 50 mm with the take-off off; a rare spike and a steady one told
// apart): every head-still frame's eye step while the walk owns the take-off, its sum, its count over 5 mm, the last 1024
std::mutex g_judder_mutex;
float g_judder_ring[1024];
uint32_t g_judder_n = 0, g_judder_head = 0;  // under g_judder_mutex
double g_judder_sum = 0.0;
uint64_t g_judder_over = 0, g_judder_total = 0;
std::atomic<float> g_room_taken{0.0f}, g_room_judder{0.0f}, g_room_refused{0.0f};  // metres taken off; the eye's largest step in a frame (the head still); not taken
std::atomic<uint64_t> g_room_frames{0}, g_room_rejects{0};
std::atomic<float> g_room_allow_m{0.0f};        // room_allow_step: the raised cap and until when
std::atomic<double> g_room_allow_until{0.0};
constexpr float kRoomWalkMax = 2.5f;  // m/s: the most of John's move taken off a second (a walk; RoomScaleMaxStick is at most 0.60)
// [XR] BodyFollowsTurn (2026-10-27; the user: "Turning physically will turn johns body right?", then "Add it after room
// scale works."): pose.cpp turns the body heading after the head; the view must not turn with it (the view is the
// head). The eye cameras are the frame's camera (its heading) composed with the head relative to g_q0's yaw, so the
// origin's yaw is turned by the same angle, but only by what the frame's camera already shows: the anchored camera takes
// a heading once a script tick and its matrix lags the command by a tick or more (fp_cam's "after a turn, three frames"),
// and an origin turned at once would swing the view by the part still pending, a few degrees, every tick. pose.cpp
// pushes every frame's commanded heading with the follow's total then (body_turn_push); at each presented frame's start
// (room_take_frame) the frame camera's heading is matched against them (newest first, within 2 degrees) and the total
// that command carried is the one the camera shows: the origin turned by its change since the last frame. Bit 4 of
// g_room_state while pose pushes (the option on, or a turn still pending after it went off); off, nothing here runs.
// The ring under g_bt_mutex; g_bt_applied under g_recentre_mutex (a recentre takes a fresh origin: the total it already
// shows stays applied, so only the turns after it move the new one)
struct BtEntry {
    float h, f;   // the heading commanded (degrees, positive left), the follow's total in it (degrees, wrapped)
    bool ours;    // the anchor's own camera on foot (the offset's calibration takes only these)
};
constexpr int kBtRing = 160;  // (about 1.8 s at 90 Hz: a stalled tick's lag well inside)
std::mutex g_bt_mutex;
BtEntry g_bt_ring[kBtRing];
int g_bt_n = 0, g_bt_head = 0;  // under g_bt_mutex
float g_bt_off = 0.0f;          // the frame camera's heading less the commanded, learned standing still (0 expected)
int g_bt_still = 0;
float g_bt_last_hcam = 0.0f;
bool g_bt_off_logged = false;
int g_bt_miss_run = 0;                  // misses in a row (held: the origin not turned, up to kBtMissHold frames)
constexpr int kBtMissHold = 30;
std::atomic<bool> g_bt_ready{false};    // the offset learned once (pose starts no follow before: body_turn_ready)
float g_bt_applied = 0.0f;  // under g_recentre_mutex: the follow's total the origin's yaw has taken
std::atomic<float> g_bt_shown_pub{0.0f}, g_bt_err_pub{0.0f}, g_bt_off_pub{0.0f}, g_bt_view_pub{0.0f}, g_bt_view_step{0.0f};
std::atomic<uint64_t> g_bt_frames{0}, g_bt_misses{0}, g_bt_turned{0}, g_bt_still_frames{0};
float bt_wrap(float d) { return std::remainder(d, 360.0f); }
// [Comfort] WorldScale (2026-10-13, the user: "Did we add a world scale setting at any point?"): every headset-space
// position put into the game (the eyes, so their separation too, the head's and the neck's moves, the hands) divided
// by it: above 1 the world looks bigger (you smaller), below 1 smaller. 1 = true scale. The game's world is unchanged
std::atomic<float> g_world_scale{1.0f};
float ws_inv() {
    const float s = g_world_scale.load(std::memory_order_relaxed);
    return s > 0.01f ? 1.0f / s : 1.0f;
}
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
    // [XR] PhysicalCrouch (2026-10-27, cycle r157): a recentre while John is crouched keeps the origin's height (the turn
    // and the floor place retaken): taken from a crouched head, standing up would lift the view by the crouch's depth
    // (0.68 m). Only while the old origin is in the same LOCAL space (not after the runtime's own recentre moved it)
    // (the review: g_recentred is always false here, cleared by recentre() before this pass: an origin of its own)
    static uint64_t s_lchg = 0;
    static bool s_have = false;
    const uint64_t lchg = xr::local_space_changes();
    if (s_have && lchg == s_lchg && pose::crouch_keeps_recentre_height()) {
        log::info("[cam] recentre while crouched: the origin's height kept (%.3f, the head now %.3f)", g_p0[1], mid[1]);
        mid[1] = g_p0[1];
    }
    s_lchg = lchg;
    s_have = true;
    quat_matrix(v[0].orientation, r);
    float yaw = std::atan2(r[2], r[8]);  // the back axis (column 2) in the horizontal plane
    g_q0[0] = 0, g_q0[1] = std::sin(0.5f * yaw), g_q0[2] = 0, g_q0[3] = std::cos(0.5f * yaw);
    std::memcpy(g_p0, mid, sizeof(mid));
    std::memcpy(g_p0_base, mid, sizeof(mid));  // ([XR] RoomScale: the room's shifts start again from here)
    g_room_q[0] = g_room_q[1] = 0.0f;  // (a shift queued for the old origin is not the new one's)
    g_room_state.fetch_and(~2, std::memory_order_relaxed);
    g_recentred = true;
    g_recentre_gen.fetch_add(1, std::memory_order_relaxed);
    log::info("[cam] recentred: yaw %.2f deg, origin (%.3f %.3f %.3f)", yaw * 57.29578f, mid[0], mid[1], mid[2]);
}

// The camera matrix of eye `eye` (0 left, 1 right, 2 the centre between them): the game camera `cam` composed with
// that eye's located pose. `peek`: the views are read without counting as drawn with (the visibility cover).
bool xr_eye_camera(const float* cam, int eye, float* out, bool peek = false, bool may_recentre = true) {
    xr::EyeView v[2];
    if (!(peek ? xr::eye_views_peek(v) : xr::eye_views(v))) return false;
    if (eye == 2) {  // the centre: the eyes' midpoint, the left eye's orientation
        for (int k = 0; k < 3; ++k) v[0].position[k] = 0.5f * (v[0].position[k] + v[1].position[k]);
        eye = 0;
    }
    float q0[4], p0[3];
    {
        std::lock_guard lock(g_recentre_mutex);
        if (!g_recentred) {
            if (!may_recentre) return false;  // (2026-10-27: off the render pass, no origin taken from peeked views)
            recentre_from(v);
        }
        std::memcpy(q0, g_q0, sizeof(q0));
        std::memcpy(p0, g_p0, sizeof(p0));
    }
    float q0c[4] = {-q0[0], -q0[1], -q0[2], q0[3]}, qrel[4], r0[9], m[9], d[3];
    quat_mul(q0c, v[eye].orientation, qrel);
    quat_matrix(q0c, r0);
    if (g_xr_head_position.load(std::memory_order_relaxed)) {
        const float wi = ws_inv();
        for (int k = 0; k < 3; ++k) d[k] = (v[eye].position[k] - p0[k]) * wi;
    } else {  // the eye's offset from the head's centre only
        float s = g_xr_separation.load(std::memory_order_relaxed) * ws_inv();  // (the world scale: the separation too)
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
// [XR] RoomScale's take-off (see g_p0_base): once a presented frame, while the room-scale walk owns the walk
void room_shift_locked(float dr, float db) {  // under g_recentre_mutex: the origin moved by (dr, 0, db) game metres in the recentred frame
    float r[9];
    quat_matrix(g_q0, r);
    const float s = 1.0f / ws_inv();  // (the world scale: d = (eye - p0) / scale)
    for (int k = 0; k < 3; ++k) {
        const float v = (r[k * 3 + 0] * dr + r[k * 3 + 2] * db) * s;
        g_p0[k] += v;
        g_room_shift[k] += v;
    }
}
// the centre eye's world place for the frame's camera `cam` and an origin already copied (q0, p0): no recentre taken
void centre_eye_world(const float* cam, const xr::EyeView* v, const float* q0, const float* p0, float* out) {
    float q0c[4] = {-q0[0], -q0[1], -q0[2], q0[3]}, r0[9], d[3];
    quat_matrix(q0c, r0);
    for (int k = 0; k < 3; ++k) d[k] = (0.5f * (v[0].position[k] + v[1].position[k]) - p0[k]) * ws_inv();
    const float prel[3] = {r0[0] * d[0] + r0[1] * d[1] + r0[2] * d[2], r0[3] * d[0] + r0[4] * d[1] + r0[5] * d[2], r0[6] * d[0] + r0[7] * d[1] + r0[8] * d[2]};
    for (int k = 0; k < 3; ++k) out[k] = cam[12 + k] + prel[0] * cam[k] + prel[1] * cam[4 + k] + prel[2] * cam[8 + k];
}
// [XR] BodyFollowsTurn: the follow's total the frame camera `cam` shows (BtEntry): its heading (the back row (sin h,
// cos h): h = atan2(bx, bz), as gestures.cpp reads it) matched against the commands pushed, newest first (a near tie the
// newer's), within 2 degrees; none that near (a script's camera, the ride's): the newest total, as if caught up (counted)
float bt_shown(const float* cam) {
    const float hcam = std::atan2(cam[8], cam[10]) * 57.29578f;
    std::lock_guard lock(g_bt_mutex);
    if (g_bt_n == 0) return g_bt_shown_pub.load(std::memory_order_relaxed);
    const BtEntry& nw = g_bt_ring[(g_bt_head + kBtRing - 1) % kBtRing];
    // the offset (expected 0): learned with the command and the camera both still for a third of a second, the anchor's own
    bool still = nw.ours && std::fabs(bt_wrap(hcam - g_bt_last_hcam)) < 0.05f && g_bt_n >= 30;
    for (int i = 1; i < 30 && still; ++i) {
        const BtEntry& e = g_bt_ring[(g_bt_head + kBtRing - 1 - i) % kBtRing];
        still = e.ours && std::fabs(bt_wrap(e.h - nw.h)) < 0.05f;
    }
    g_bt_still = still ? g_bt_still + 1 : 0;
    g_bt_last_hcam = hcam;
    if (g_bt_still >= 30) {
        const float off = bt_wrap(hcam - nw.h);
        if (!g_bt_off_logged || std::fabs(bt_wrap(off - g_bt_off)) > 1.0f) {
            log::info("[cam] body turn: the frame camera's heading %.2f deg, the commanded %.2f: read %.2f deg off it", hcam, nw.h, off);
            g_bt_off_logged = true;
        }
        g_bt_off = off;
        g_bt_off_pub.store(off, std::memory_order_relaxed);
        g_bt_ready.store(true, std::memory_order_relaxed);
    }
    float best = 1e9f, f = nw.f;
    for (int i = 0; i < g_bt_n; ++i) {
        const BtEntry& e = g_bt_ring[(g_bt_head + kBtRing - 1 - i) % kBtRing];
        const float err = std::fabs(bt_wrap(e.h + g_bt_off - hcam));
        if (err < best - 1e-3f) {
            best = err;
            f = e.f;
        }
    }
    g_bt_err_pub.store(best, std::memory_order_relaxed);
    g_bt_frames.fetch_add(1, std::memory_order_relaxed);
    // (the review) a miss turns nothing for up to kBtMissHold frames in a row (a wrong offset, a script's camera: the
    // view never swung by a guess); only a run longer than that takes the newest total (the anchor gone mid-turn: a
    // pending part left for ever otherwise), at once, counted
    if (best > 2.0f) {
        g_bt_misses.fetch_add(1, std::memory_order_relaxed);
        if (++g_bt_miss_run <= kBtMissHold) return g_bt_shown_pub.load(std::memory_order_relaxed);
        f = nw.f;
    } else {
        g_bt_miss_run = 0;
    }
    return f;
}
// the view's world yaw this frame (the frame camera's heading plus the head's yaw over the origin's, as the eyes are
// drawn) and its largest step between frames the head itself held its yaw (under 0.05 degrees): the test's measure that
// the body's turn never shows in the view. Under g_recentre_mutex (the origin just turned)
void bt_view_locked(const float* cam, const xr::EyeView* v, bool reset) {
    static float s_view = 0.0f, s_head = 0.0f;
    static bool s_ok = false;
    if (reset) {
        s_ok = false;
        return;
    }
    auto yaw_of = [](const float* q) {  // the forward (-z) of q in the horizontal plane (degrees, positive left)
        const float fx = -2 * (q[0] * q[2] + q[3] * q[1]), fz = -(1 - 2 * (q[0] * q[0] + q[1] * q[1]));
        return std::atan2(-fx, -fz) * 57.29578f;
    };
    float q0c[4] = {-g_q0[0], -g_q0[1], -g_q0[2], g_q0[3]}, q[4];
    quat_mul(q0c, v[0].orientation, q);
    const float view = bt_wrap(std::atan2(cam[8], cam[10]) * 57.29578f + yaw_of(q)), head = yaw_of(v[0].orientation);
    if (s_ok && std::fabs(bt_wrap(head - s_head)) < 0.05f) {
        const float step = std::fabs(bt_wrap(view - s_view));
        if (step > g_bt_view_step.load(std::memory_order_relaxed)) g_bt_view_step.store(step, std::memory_order_relaxed);
        g_bt_still_frames.fetch_add(1, std::memory_order_relaxed);
    }
    s_view = view;
    s_head = head;
    s_ok = true;
    g_bt_view_pub.store(view, std::memory_order_relaxed);
}
void room_take_frame(const float* cam) {
    static uint64_t s_frame = ~0ull;
    static float s_last[3], s_eye[3], s_head[3];
    static bool s_last_ok = false, s_eye_ok = false;
    static double s_ms = 0.0;
    static double s_move_ms = 0.0;  // the frame the anchored camera last moved (it moves once a script tick)
    static float s_dbg[5] = {};     // this frame's take-off, for the log of a frame over 5 mm
    const int bits = g_room_state.load(std::memory_order_relaxed);
    if (!bits || !cam) {  // the option off and nothing queued: this load only
        s_last_ok = s_eye_ok = false;
        s_move_ms = 0.0;  // (the next walk's first step capped over a tick from its own start, not from an old walk)
        return;
    }
    const uint64_t f = state::presents.load();
    if (f == s_frame) return;  // (once a presented frame: both passes after it, one origin)
    s_frame = f;
    const bool owns = (bits & 1) != 0;
    // [XR] BodyFollowsTurn (bit 4): the follow's total this frame's camera shows, and the views (both before the mutex)
    const bool bt = (bits & 4) != 0;
    const float bt_f = bt ? bt_shown(cam) : 0.0f;
    xr::EyeView btv[2];
    const bool bt_v = bt && xr::eye_views_peek(btv);
    float q0[4], p0[3];
    {
        std::lock_guard lock(g_recentre_mutex);
        if (!g_recentred) {
            s_last_ok = s_eye_ok = false;
            g_room_q[0] = g_room_q[1] = 0.0f;
            g_room_state.fetch_and(~2, std::memory_order_relaxed);
            if (bt) {  // (the next pass takes a fresh origin: what this camera shows is in it)
                g_bt_applied = bt_f;
                g_bt_shown_pub.store(bt_f, std::memory_order_relaxed);
            }
            return;
        }
        if (g_room_q[0] != 0.0f || g_room_q[1] != 0.0f) {  // pose's queued shifts (the clamp, the re-anchor): before the passes
            room_shift_locked(g_room_q[0], g_room_q[1]);
            g_room_q[0] = g_room_q[1] = 0.0f;
        }
        g_room_state.fetch_and(~2, std::memory_order_relaxed);  // (the queue applied, under the mutex the queue is filled under)
        if (bt) {  // the origin's yaw turned by what the camera newly shows of the body's turn (after the queue: it was the old frame's)
            const float d = bt_wrap(bt_f - g_bt_applied);
            if (d != 0.0f) {
                const float yaw = 2.0f * std::atan2(g_q0[1], g_q0[3]) + d * 0.0174532925f;  // (g_q0 is yaw only: recentre_from)
                g_q0[0] = g_q0[2] = 0.0f;
                g_q0[1] = std::sin(0.5f * yaw);
                g_q0[3] = std::cos(0.5f * yaw);
                g_bt_turned.fetch_add(1, std::memory_order_relaxed);
            }
            g_bt_applied = bt_f;
            g_bt_shown_pub.store(bt_f, std::memory_order_relaxed);
            if (bt_v) bt_view_locked(cam, btv, false);
        } else {
            bt_view_locked(nullptr, nullptr, true);
        }
        if (!owns) {
            s_last_ok = s_eye_ok = false;  // seeded again when it next owns the walk
            s_move_ms = 0.0;
            return;
        }
        const double now = log::now_ms();
        if (s_last_ok) {
            const float dx = cam[12] - s_last[0], dz = cam[14] - s_last[2];
            // (cycle r159) the cap over the time since the camera last moved: it moves once a game tick, a step of a tick's
            // walk in one render frame, which a cap over the render frame clipped (the clipped part left in the view: the
            // 23 mm judder)
            const bool stepped = dx * dx + dz * dz > 1e-10f;
            double dt = (now - (s_move_ms > 0.0 ? s_move_ms : s_ms)) / 1000.0;
            // (cycle r161: the first step after seeding was capped by one render frame, about 27 mm, and a step coming a
            // frame early by less than a tick) a game tick's time at least (50 ms: 20 Hz and up), at most a quarter second
            dt = dt < 0.05 ? 0.05 : dt > 0.25 ? 0.25 : dt;
            const double fdt = (now - s_ms) / 1000.0;
            if (stepped) s_move_ms = now;
            // the move along the camera's own rows a (right) and c (back): the head's offset is in those
            const float dr = dx * cam[0] + dz * cam[2], db = dx * cam[8] + dz * cam[10];
            // (cycle r163: five rounds of a part along the walk's direction and its limits never converged) John's whole
            // horizontal move, as a vector, at most a walk's speed a tick in length: the view is the head by construction,
            // his slide included, in any direction; the part over the cap (a train, a drag) stays in the view
            const float all = std::sqrt(dr * dr + db * db);
            const float cap0 = kRoomWalkMax * static_cast<float>(dt);
            float cap = cap0;
            // (the review of r189) room_allow_step's raise is one jump's: spent on the first step over the usual cap
            const bool allow = now < g_room_allow_until.load(std::memory_order_acquire);
            if (allow) cap = std::fmax(cap0, g_room_allow_m.load(std::memory_order_relaxed));
            if (allow && all > cap0) g_room_allow_until.store(0.0, std::memory_order_relaxed);
            float k = all > cap ? cap / all : 1.0f;
            if (!g_room_take_on.load(std::memory_order_relaxed)) k = 0.0f;  // (the test's negative control)
            if (k > 0.0f && all > 0.0f) room_shift_locked(dr * k, db * k);
            const float take = all * k;
            s_dbg[0] = static_cast<float>(fdt * 1000.0), s_dbg[1] = static_cast<float>(dt * 1000.0), s_dbg[2] = all;
            s_dbg[3] = take, s_dbg[4] = cap;
            g_room_taken.store(g_room_taken.load(std::memory_order_relaxed) + take, std::memory_order_relaxed);
            if (all - take > 1e-4f) g_room_refused.store(g_room_refused.load(std::memory_order_relaxed) + (all - take), std::memory_order_relaxed);
            if (all > cap + 0.01f) g_room_rejects.fetch_add(1, std::memory_order_relaxed);  // faster than a walk: not all taken
        }
        std::memcpy(s_last, cam + 12, sizeof(s_last));
        s_ms = now;
        s_last_ok = true;
        std::memcpy(q0, g_q0, sizeof(q0));
        std::memcpy(p0, g_p0, sizeof(p0));
    }
    g_room_frames.fetch_add(1, std::memory_order_relaxed);
    // the measure: the centre eye's world place frame to frame, over frames the head itself held still (under 2 mm)
    xr::EyeView v[2];
    if (xr::eye_views_peek(v)) {
        float h[3], e[3];
        for (int k = 0; k < 3; ++k) h[k] = 0.5f * (v[0].position[k] + v[1].position[k]);
        centre_eye_world(cam, v, q0, p0, e);
        if (s_eye_ok) {
            const float hm = std::sqrt((h[0] - s_head[0]) * (h[0] - s_head[0]) + (h[1] - s_head[1]) * (h[1] - s_head[1]) + (h[2] - s_head[2]) * (h[2] - s_head[2]));
            const float em = std::sqrt((e[0] - s_eye[0]) * (e[0] - s_eye[0]) + (e[2] - s_eye[2]) * (e[2] - s_eye[2]));  // (horizontal)
            if (hm < 0.002f) {
                if (em > g_room_judder.load(std::memory_order_relaxed)) g_room_judder.store(em, std::memory_order_relaxed);
                if (em > 0.005f)  // (cycle r161: which frames step: a short frame, a cap, the limit)
                    log::limited("cam.room.judder", 60,
                                 "[cam] room take-off: the eye stepped %.4f m (head still): frame %.1f ms, since the camera moved %.1f ms, "
                                 "the camera %.4f m, taken %.4f, cap %.4f",
                                 em, s_dbg[0], s_dbg[1], s_dbg[2], s_dbg[3], s_dbg[4]);
                std::lock_guard jl(g_judder_mutex);
                g_judder_ring[g_judder_head] = em;
                g_judder_head = (g_judder_head + 1) % 1024;
                if (g_judder_n < 1024) ++g_judder_n;
                g_judder_sum += em;
                ++g_judder_total;
                if (em > 0.005f) ++g_judder_over;
            }
        }
        std::memcpy(s_eye, e, sizeof(s_eye));
        std::memcpy(s_head, h, sizeof(s_head));
        s_eye_ok = true;
    }
}

alignas(16) char g_saved_vp[0x470 + 0x10];  // render thread only
alignas(16) char g_cover_vp[kViewportSize + 0x10];
alignas(16) char g_union_vp[kViewportSize + 0x10];  // render thread only: shadow_union_vp
alignas(16) char g_eye2_vp[kViewportSize + 0x10];   // render thread only: [Stereo] PrepassViews' private eye-2 viewport (never current but
                                                     // under PrepassEye2Collector: around its one call in pass 1)
uint64_t g_eye2_frame = ~0ull;                       // the frame it was built in
bool g_union_ok = false;
// The union's margin: each eye sits 3 cm or so off the centre, so its frustum edge leaves the centre's by up to
// atan(0.032/z) at depth z, against tangents near 1.4 at the edge: 3 degrees covers it from about 0.25 m on.
constexpr float kUnionMarginDeg = 3.0f;

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
bool build_union_vp(void* vp, const float* cam_in);

// A double frame (R3, D13): the original runs twice on the same viewport, its 0x470 bytes saved before the first pass
// and restored before the second, so every pointer the frame holds stays vanilla. dual_pass keeps the per-frame state
// the second pass would see consumed, and runs the post chain for the first eye between the passes (with the first
// pass's viewport still current).
std::atomic<float> g_scene_near{0}, g_scene_far{0};
void scope_frame(const float* cam_in);  // (r55 item 11; below)
thread_local float t_frame_yaw = 0.0f;  // the test yaw latched once a frame: both passes and the union see the same

void room_take_frame(const float* cam);
void hk_SceneRender(void* renderer, void* vp, const float* cam_game, void* a4) {
    const float* cam = fp_cam(cam_game);  // [Body] FirstPersonMoments: the anchor's camera while a script's holds the channel
    room_take_frame(cam);  // [XR] RoomScale (2026-10-27): off and nothing queued, one relaxed load (else g_recentre_mutex once a frame)
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
    t_frame_yaw = g_yaw.load(std::memory_order_relaxed);
    struct Timed {  // once a frame (no clock reads in the draws under it)
        double t0 = log::now_ms();
        ~Timed() {
            g_scene_us.fetch_add(static_cast<uint64_t>((log::now_ms() - t0) * 1000.0), std::memory_order_relaxed);
            g_scene_timed.fetch_add(1, std::memory_order_relaxed);
        }
    } timed;
    const bool probe = frame_probe::on();  // the single-pass probe: once a pass (frame_probe.h)
    if (probe) {
        frame_probe::render_frame_start(timed.t0);
        frame_probe::set_phase(frame_probe::kBefore);
    }
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
        scope_frame(cam);  // (r55 item 11) the scope's state for this frame: its eye, and the eyes' order it wants
        int first = eff_swap() ? 1 : 0;
        std::memcpy(g_saved_vp, vp, kViewportSize);
        g_union_ok = (dual_pass::shadows_wanted() || prepass::union_cull_wanted()) && g_xr_pose.load(std::memory_order_relaxed) &&
                     build_union_vp(vp, cam);
        dual_pass::begin_frame(renderer);
        const double t1 = probe ? log::now_ms() : 0;
        if (probe) frame_probe::set_phase(frame_probe::kPhasePass1);
        scene_once(renderer, vp, cam, a4, first);
        const double t2 = probe ? log::now_ms() : 0;
        if (probe) frame_probe::set_phase(frame_probe::kBetween);
        dual_pass::between_passes(renderer, first);
        const double t3 = probe ? log::now_ms() : 0;
        if (probe) frame_probe::set_phase(frame_probe::kPhasePass2);
        std::memcpy(vp, g_saved_vp, kViewportSize);
        scene_once(renderer, vp, cam, a4, 1 - first);
        dual_pass::end_frame(1 - first);
        g_double_frames.fetch_add(1, std::memory_order_relaxed);
        if (probe) {
            frame_probe::set_phase(frame_probe::kOutside);
            frame_probe::add(frame_probe::kPre, t1 - timed.t0);
            frame_probe::add(frame_probe::kPass1, t2 - t1);
            frame_probe::add(frame_probe::kMid, t3 - t2);
            frame_probe::add(frame_probe::kPass2, log::now_ms() - t3);
        }
        return;
    }
    dual_pass::mono_frame();
    g_union_ok = false;
    const double t1 = probe ? log::now_ms() : 0;
    if (probe) frame_probe::set_phase(frame_probe::kPhaseMono);
    scene_once(renderer, vp, cam, a4, -1);
    if (probe) {
        frame_probe::set_phase(frame_probe::kOutside);
        frame_probe::add(frame_probe::kPre, t1 - timed.t0);
        frame_probe::add(frame_probe::kMono, log::now_ms() - t1);
    }
}

// The shadow passes' union viewport (shadow_union_vp): the frame's viewport with the centre eye's camera and the union
// of both eyes' tangents plus the margin, through the engine's SetCamera and Perspective (the head cover's way).
bool build_union_vp(void* vp, const float* cam_in) {
    xr::EyeView v[2];
    if (!vp || !cam_in || !xr::eye_views_peek(v)) return false;
    alignas(16) float c[16], centre[16];
    std::memcpy(c, cam_in, sizeof(c));
    const float yaw = t_frame_yaw;
    if (yaw != 0) yaw_rows(c, yaw);  // the passes draw the yawed camera (the truth capture's presets)
    if (!xr_eye_camera(c, 2, centre, true)) return false;
    const float m = kUnionMarginDeg / 57.29577951f;
    const float l = std::tan(std::fmin(v[0].fov[0], v[1].fov[0]) - m), r = std::tan(std::fmax(v[0].fov[1], v[1].fov[1]) + m);
    const float u = std::tan(std::fmax(v[0].fov[2], v[1].fov[2]) + m), d = std::tan(std::fmin(v[0].fov[3], v[1].fov[3]) - m);
    if (!(r > l && u > d)) return false;
    std::memcpy(g_union_vp, vp, kViewportSize);
    o_SetCamera(g_union_vp, centre);
    apply_tangents(g_union_vp, l, r, u, d);
    return true;
}

// (run 29 item 3, the scope spike's reads, kept for "scope game") the game's own vertical FOV at each eye pass's start
// (vp +0x338, before the eye's tangents replace it)
std::atomic<float> g_game_vfov[2] = {};
std::atomic<uint64_t> g_scope_passes{0};
// (r55 item 11) the scope's state for this frame (scope.h): the eyes' cameras and tangents as the passes will draw them,
// once a stereo frame with the eyes from the XR views; nothing while [Scope] is off and nothing is forced (one load each)
void scope_frame(const float* cam_in) {
    if (!g_xr_pose.load(std::memory_order_relaxed)) {  // (the review: no eyes from the views, no scope's order kept)
        g_scope_swap.store(false, std::memory_order_relaxed);
        return;
    }
    if (!cam_in || !scope::wanted()) return;
    xr::EyeView v[2];
    uint64_t xf = 0;
    if (!xr::eye_views(v, &xf)) return;
    scope::FrameIn in{};
    std::memcpy(in.cam, cam_in, sizeof(in.cam));
    if (t_frame_yaw != 0) yaw_rows(in.cam, t_frame_yaw);  // (the passes draw the yawed camera)
    for (int e = 0; e < 2; ++e) {
        if (!xr_eye_camera(in.cam, e, in.eye[e])) return;
        for (int k = 0; k < 4; ++k) in.tan[e][k] = std::tan(v[e].fov[k]);
    }
    in.single_pass = prepass::views_wanted();
    in.first = g_swap.load(std::memory_order_relaxed) ? 1 : 0;  // (the base order: the scope's own swap is its state's)
    in.xr_frame = xf;
    g_scope_swap.store(scope::frame_begin(in), std::memory_order_relaxed);
}

// One scene pass. pass_eye: -1 mono, 0 left, 1 right (double mode).
void scene_once(void* renderer, void* vp, const float* cam_in, void* a4, int pass_eye) {
    g_scenes.fetch_add(1, std::memory_order_relaxed);
    alignas(16) float yawed[16];
    const float* cam = cam_in;
    float yaw = t_frame_yaw;
    if (yaw != 0 && cam_in) {
        std::memcpy(yawed, cam_in, sizeof(yawed));
        yaw_rows(yawed, yaw);
        cam = yawed;
    }
    if (pass_eye == 0 || pass_eye == 1) g_game_vfov[pass_eye].store(*reinterpret_cast<const float*>(static_cast<const char*>(vp) + 0x338), std::memory_order_relaxed);
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
    if (xr_pose && have && pass_eye >= 0) {  // (r55 item 11, [Scope]) this eye's pass from the scope's objective, its true field
        float st[4];
        if (scope::pass_camera(pass_eye, copy, st)) {
            l = st[0], r = st[1], u = st[2], d = st[3];
            apply_tangents(vp, l, r, u, d);
            g_scope_passes.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (cam) ring_probe::matrix_expect(pass_eye, copy + 12, other + 12, cam + 12);
    if (xr_pose && have && eye == 3 && pass_eye == (eff_swap() ? 1 : 0) && prepass::views_wanted() &&
        xr::eye_views(views)) {
        // [Stereo] PrepassViews: the other eye's viewport now, as pass 2 will make it (its saved copy, that eye's
        // tangents, its camera); never current, so the engine pushes and records nothing for it ([Stereo]
        // PrepassEye2Collector, test only, makes it current around one call in pass 1: FUN_140130f00 then sets its
        // +0x330/+0x334 to the locked target's size)
        const float* a2 = views[pass_eye == 1 ? 0 : 1].fov;
        std::memcpy(g_eye2_vp, g_saved_vp, kViewportSize);
        float l2 = std::tan(a2[0]), r2 = std::tan(a2[1]), u2 = std::tan(a2[2]), d2 = std::tan(a2[3]);
        apply_tangents(g_eye2_vp, l2, r2, u2, d2);
        o_SetCamera(g_eye2_vp, other);
        prepass::set_eye2(g_eye2_vp, nullptr);  // (without jitter; the TAA update's jittered rows replace these)
        g_eye2_frame = state::presents.load();
    }
    if (pass_eye >= 0 && (ipd != 0 || xr_pose) && cam) {
        if (have) round_draw::note_pass(pass_eye, copy, l, r, u, d);  // [Reload] RoundInHand: this eye's camera
        o_SceneRender(renderer, vp, copy, a4);
        ring_probe::matrix_expect(-2, nullptr, nullptr, nullptr);
        if (pass_eye == (eff_swap() ? 1 : 0) && g_findcam.exchange(false)) {
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

void hk_VisibilityBuild(void* renderer, void* vp, void* cam_game, void* a4) {
    // [Body] FirstPersonMoments: culled for the anchor's camera too (its copy: the build may keep the pointer)
    void* cam = cam_game;
    if (const float* fc = fp_cam(static_cast<const float*>(cam_game)); fc != cam_game) {
        static alignas(16) float vis_cam[16];
        std::memcpy(vis_cam, fc, sizeof(vis_cam));
        cam = vis_cam;
    }
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

// [XR] AudioFollowsHead (the user, 2026-10-12: "npcs behind you sound far away ... can't tell where things are coming
// from"): the game's audio listeners, the panning one (0x142c34bf0, a copy) and the volume's (0x142c34bb0, its
// distance), are taken each frame from the update side's camera matrix (AudioUpdateListeners, the main thread, rows
// right, up, back, position; research/run12/audio.md). In first person that camera faces the body's heading, so every
// sound was placed for the body and not the head, and the rear's loss (3 dB from 45 to 135 deg off its forward) fell on
// what the face looked at. With the eyes from the XR views the listeners get the centre eye's camera of that camera
// (its orientation, and its position when the head's position is tracked), as the eyes are drawn.
using AudioListeners_t = void (*)(const float* cam);
AudioListeners_t o_AudioListeners = nullptr;
std::atomic<bool> g_audio_head{true};
std::atomic<uint64_t> g_audio_calls{0}, g_audio_from_head{0};
std::atomic<float> g_audio_turn{0.0f};  // the last listener's yaw from the camera's (deg, positive: turned left)
// run 12 item 10, "cam audio log on|off|mark <n>|dump <file>" (a test's): each listener update in a ring, the game
// camera's and the listener's headings and positions and why the head's was not used (cycle AU7's sound all in the left
// ear with the turn read right). The main thread writes, the command thread dumps (under g_alog_mutex).
struct AudioLogRow {
    double ms;
    float game_h, out_h, game_p[3], out_p[3];
    int why;  // 0 the head's, 1 the switch off, 2 no XR pose, 3 not a double frame, 4 no eye camera, 5 not finite, 9 a mark
};
std::mutex g_alog_mutex;
AudioLogRow g_alog[8192];  // under g_alog_mutex
size_t g_alog_n = 0;
std::atomic<bool> g_alog_on{false};
void audio_log(const float* cam, const float* out, int why) {
    if (!g_alog_on.load(std::memory_order_relaxed)) return;
    AudioLogRow r{};
    r.ms = log::now_ms();
    r.why = why;
    if (cam) {
        r.game_h = std::atan2(cam[8], cam[10]) * 57.29578f;
        for (int k = 0; k < 3; ++k) r.game_p[k] = cam[12 + k];
    }
    if (out) {
        r.out_h = std::atan2(out[8], out[10]) * 57.29578f;
        for (int k = 0; k < 3; ++k) r.out_p[k] = out[12 + k];
    }
    std::lock_guard lock(g_alog_mutex);
    g_alog[g_alog_n % 8192] = r;
    ++g_alog_n;
}
void hk_AudioListeners(const float* cam) {
    g_audio_calls.fetch_add(1, std::memory_order_relaxed);
    int why = !cam ? 5 : !g_audio_head.load(std::memory_order_relaxed) ? 1 : !g_xr_pose.load(std::memory_order_relaxed) ? 2
                                                                         : !g_double.load(std::memory_order_relaxed) ? 3 : 0;
    if (why == 0) {  // (the review: as the eyes, from the XR views in a double frame)
        alignas(16) float c[16];
        why = 4;
        if (xr_eye_camera(cam, 2, c, true)) {
            bool ok = true;
            for (float v : c) ok = ok && std::isfinite(v);
            why = 5;
            if (ok) {
                // the turn about the world's up: the back rows' angle in the horizontal plane
                const float a = std::atan2(cam[8], cam[10]), b = std::atan2(c[8], c[10]);
                g_audio_turn.store(std::remainder(b - a, 6.2831853f) * 57.29578f, std::memory_order_relaxed);
                g_audio_from_head.fetch_add(1, std::memory_order_relaxed);
                audio_log(cam, c, 0);
                o_AudioListeners(c);
                return;
            }
        }
    }
    audio_log(cam, cam, why);
    o_AudioListeners(cam);
}

// [XR] AudioRearFold (the same report: "npcs behind you sound far away"): the engine pans every positioned sound over
// eight speakers (FL FR C LFE BL BR SL SR; the four corners at +-45 deg front and rear). Each mono voice goes to an
// 8-channel submix and that to the device (AudioOutputMatrix, 0x140f6baa0, builds both matrices; cycle AP1: 1 -> 8 and
// 8 -> 2); on a stereo device the submix's fold is the game's d11 table (0x1410921c0: L = FL + 0.68 C + 0.3 LFE
// + 0.3 BL + 0.3 SL), the rear at 0.3 (-10.5 dB), with the rear rule's 3 dB: measured 14-20 dB under the same sound in
// front. With the device stereo (the 8-channel voice that feeds it built to 2), a mono voice's matrix gets each side's
// power in its front pair, L = sqrt(FL^2 + BL^2 + SL^2 + C^2/2) and R likewise, the rest 0 (LFE kept): a sound in front
// as before, one behind as loud as in front but for the game's own 3 dB rear rule (cycle AU6: -8.6 -> -3.1 dB), one to
// the side 3 dB up (both its speakers now). A 5.1 or 7.1 device keeps the game's.
// Any audio thread: atomics only.
using AudioMatrix_t = void (*)(char* voice, int send, float* out);
AudioMatrix_t o_AudioMatrix = nullptr;
std::atomic<bool> g_audio_fold{true};
std::atomic<uint64_t> g_fold_calls{0}, g_fold_done{0};
std::atomic<bool> g_master_stereo{false};  // an 8-channel voice seen built to 2 channels (the device stereo)
std::atomic<uint64_t> g_fold_pairs[10][10] = {};  // the matrices built, by the voice's +0x2c and its destination's +0x2c
bool audio_rd(uintptr_t a, void* o, size_t n) {  // a guarded read (the voice's destination's sends)
    __try {
        std::memcpy(o, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void hk_AudioMatrix(char* voice, int send, float* out) {
    o_AudioMatrix(voice, send, out);
    g_fold_calls.fetch_add(1, std::memory_order_relaxed);
    if (!voice || !out || send < 0 || !g_audio_fold.load(std::memory_order_relaxed)) return;
    const uint32_t src = *reinterpret_cast<const uint32_t*>(voice + 0x2c);
    char* sends = *reinterpret_cast<char**>(voice + 0x68);
    if (!sends) return;
    char* s = sends + static_cast<ptrdiff_t>(send) * 0x28;
    const char* dst = *reinterpret_cast<char**>(s);
    const uint32_t dch = dst ? *reinterpret_cast<const uint32_t*>(dst + 0x2c) : 0;
    g_fold_pairs[src < 9 ? src : 9][dch < 9 ? dch : 9].fetch_add(1, std::memory_order_relaxed);  // "cam audio": the pairs seen
    // an 8-channel voice (the submix) to a 2-channel one (the device): the game's own fold (its d11 table, the rear and
    // the sides at 0.3) follows, so the output is stereo; to more channels it is not
    if (src == 8) {  // (the review: only the voice that feeds the device, a destination with no sends of its own, decides)
        uintptr_t b = 0, e = 1;
        if (audio_rd(reinterpret_cast<uintptr_t>(dst) + 0x68, &b, sizeof(b)) && audio_rd(reinterpret_cast<uintptr_t>(dst) + 0x70, &e, sizeof(e)) && b == e)
            g_master_stereo.store(dch == 2, std::memory_order_relaxed);
        return;
    }
    // a mono voice (cycle AP1: every positioned sound) to the submix (8) or to the device (2), with the device stereo:
    // each side's power from all its speakers into the front pair (FL FR, which the table passes at 1)
    if (src != 1 || !(dch == 2 || (dch == 8 && g_master_stereo.load(std::memory_order_relaxed)))) return;
    const float* g = reinterpret_cast<const float*>(s + 8);  // FL FR C LFE BL BR SL SR
    const float l = g[0] * g[0] + g[4] * g[4] + g[6] * g[6] + 0.5f * g[2] * g[2];
    const float r = g[1] * g[1] + g[5] * g[5] + g[7] * g[7] + 0.5f * g[2] * g[2];
    if (!(l >= 0.0f && r >= 0.0f && l < 64.0f && r < 64.0f)) return;  // (finite and sane)
    out[0] = std::sqrt(l);
    out[1] = std::sqrt(r);
    if (dch == 8) out[2] = out[4] = out[5] = out[6] = out[7] = 0.0f;  // (LFE, out[3], kept)
    g_fold_done.fetch_add(1, std::memory_order_relaxed);
}

bool install() {
    if (anchors::stand_down()) return false;
    g_audio_head = config::get_bool("XR", "AudioFollowsHead", true);
    {  // [Comfort] WorldScale (2026-10-13)
        const float ws = config::get_float("Comfort", "WorldScale", 1.0f);
        g_world_scale = std::fmin(2.0f, std::fmax(0.5f, ws));
    }
    g_audio_fold = config::get_bool("XR", "AudioRearFold", true);
    // (the review: each hook installed only with its switch on in the ini, so 0 is a real off; "cam audio" then changes
    // nothing; both anchors optional: a build without them keeps the game's sound)
    if (g_audio_fold.load() && anchors::present(anchors::Id::AudioOutputMatrix) &&
        !hooks::install("RDR audio output matrix", reinterpret_cast<void*>(anchors::addr(anchors::Id::AudioOutputMatrix)), hk_AudioMatrix,
                        &o_AudioMatrix))
        log::warn("[cam] the audio output matrix's hook not installed: the sounds behind you as the game mixes them");
    if (g_audio_head.load() && anchors::present(anchors::Id::AudioUpdateListeners) &&
        !hooks::install("RDR audio listeners", reinterpret_cast<void*>(anchors::addr(anchors::Id::AudioUpdateListeners)), hk_AudioListeners,
                        &o_AudioListeners))
        log::warn("[cam] the audio listeners' hook not installed: the game's own (the body's heading)");
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

bool swap_order() { return g_double.load() && eff_swap(); }

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

void set_audio_follows_head(bool on) {
    if (g_audio_head.exchange(on) != on) log::info("[cam] the audio listeners %s", on ? "at the head (the centre eye)" : "at the game camera (the body's heading)");
}
bool audio_follows_head() { return g_audio_head.load(); }
bool audio_rear_fold() { return g_audio_fold.load(); }
void set_audio_rear_fold(bool on) {
    if (g_audio_fold.exchange(on) != on) log::info("[cam] the audio's rear on stereo %s", on ? "folded into left and right" : "dropped (the game's)");
}
std::string audio_log_command(const std::string& v, const std::string& arg) {
    if (v == "on" || v == "off") {
        if (v == "on") {
            std::lock_guard lock(g_alog_mutex);
            g_alog_n = 0;
        }
        g_alog_on = v == "on";
        return "audio log " + v;
    }
    if (v == "mark") {  // a row marking a test's block (its number in the heading's place)
        std::lock_guard lock(g_alog_mutex);
        AudioLogRow r{};
        r.ms = log::now_ms();
        r.why = 9;
        r.game_h = static_cast<float>(std::atof(arg.c_str()));
        g_alog[g_alog_n % 8192] = r;
        ++g_alog_n;
        return "audio log mark " + arg;
    }
    if (v == "dump" && !arg.empty()) {
        FILE* f = std::fopen(arg.c_str(), "w");
        if (!f) return "ERROR cannot write " + arg;
        std::lock_guard lock(g_alog_mutex);
        std::fprintf(f, "ms,why,game_h,out_h,game_x,game_y,game_z,out_x,out_y,out_z\n");
        const size_t n = g_alog_n < 8192 ? g_alog_n : 8192, first = g_alog_n - n;
        for (size_t i = 0; i < n; ++i) {
            const AudioLogRow& r = g_alog[(first + i) % 8192];
            std::fprintf(f, "%.1f,%d,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", r.ms, r.why, r.game_h, r.out_h, r.game_p[0], r.game_p[1], r.game_p[2],
                         r.out_p[0], r.out_p[1], r.out_p[2]);
        }
        std::fclose(f);
        return "audio log dumped " + std::to_string(n) + " rows (of " + std::to_string(g_alog_n) + "), now " + std::to_string(log::now_ms());
    }
    return "audio log " + std::string(g_alog_on.load() ? "on" : "off") + ", " + std::to_string(g_alog_n) + " rows";
}
std::string audio_status() {
    char b[320];
    std::snprintf(b, sizeof(b),
                  "audio follows head %d: listener updates %llu, from the head %llu, the last turned %+.1f deg from the game camera's | rear "
                  "fold %d (stereo out %d): output matrices %llu, folded (mono to stereo) %llu",
                  g_audio_head.load() ? 1 : 0, static_cast<unsigned long long>(g_audio_calls.load()),
                  static_cast<unsigned long long>(g_audio_from_head.load()), g_audio_turn.load(), g_audio_fold.load() ? 1 : 0, g_master_stereo.load() ? 1 : 0,
                  static_cast<unsigned long long>(g_fold_calls.load()), static_cast<unsigned long long>(g_fold_done.load()));
    std::string o = b;
    o += " | pairs (voice+0x2c, dst+0x2c):";
    for (int x = 0; x < 10; ++x)
        for (int y = 0; y < 10; ++y)
            if (const uint64_t n = g_fold_pairs[x][y].load(std::memory_order_relaxed)) o += " " + std::to_string(x) + "/" + std::to_string(y) + "=" + std::to_string(n);
    return o;
}
void set_first_person_moments(bool on) {
    if (g_fpm.exchange(on) != on) log::info("[cam] first-person moments (a script's camera: the eyes kept at John's head) %s", on ? "on" : "off");
}
bool first_person_moments() { return g_fpm.load(); }
std::string fp_status() {
    std::lock_guard lock(g_fp_mutex);
    char b[200];
    std::snprintf(b, sizeof(b), "first-person moments %d: moments %llu, frames drawn %llu, now %d, reference %d (heading %.1f)", g_fpm.load() ? 1 : 0,
                  static_cast<unsigned long long>(g_fp_moments.load()), static_cast<unsigned long long>(g_fp_frames.load()), g_fp_was ? 1 : 0,
                  g_fp_ref_ok ? 1 : 0, g_fp_ref_heading);
    return b;
}
// "cam fp mark" / "cam fp check": the reference kept, then (the view turned meanwhile, our camera current) the camera
// built from it against the game's real one: the rows' and the place's largest differences (the yaw's sign, the layout)
std::string fp_check(bool mark) {
    RdrvrActorState st{};
    api::actor_state(&st);
    const float hd = pose::body_heading_deg();
    if (!g_game_cam_valid) return "fp: no game camera yet";
    std::lock_guard lock(g_fp_mutex);
    if (mark) {
        std::memcpy(g_fp_mark, g_game_cam, 64);
        g_fp_mark_heading = hd;
        g_fp_mark_ok = true;
        char b[160];
        std::snprintf(b, sizeof(b), "fp marked: heading %.1f, camera at (%.3f %.3f %.3f), the anchor's put at (%.3f %.3f %.3f)", hd, g_game_cam[12],
                      g_game_cam[13], g_game_cam[14], st.camera_pos[0], st.camera_pos[1], st.camera_pos[2]);
        return b;
    }
    if (!g_fp_mark_ok) return "fp: no mark";
    float built[16];
    fp_build(g_fp_mark, g_fp_mark_heading, st, hd, built);
    float rows = 0, place = 0;
    for (int k = 0; k < 12; ++k) rows = std::fmax(rows, std::fabs(built[k] - g_game_cam[k]));
    for (int k = 12; k < 15; ++k) place = std::fmax(place, std::fabs(built[k] - g_game_cam[k]));
    char b[200];
    std::snprintf(b, sizeof(b), "fp check: heading %.1f -> %.1f, rows off by %.4f, place off by %.4f m%s", g_fp_mark_heading, hd, rows, place,
                  (st.flags & RDRVR_ACTOR_CAM_AWAY) ? " (our camera not current: no check)" : "");
    return b;
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

bool head_forward(float d[3]) {
    if (!g_game_cam_valid.load(std::memory_order_relaxed) || !g_xr_pose.load(std::memory_order_relaxed)) return false;
    float cam[16], c[16];
    std::memcpy(cam, g_game_cam, sizeof(cam));
    if (!xr_eye_camera(cam, 2, c, true, false)) return false;  // (2026-10-27: read off the render pass: never takes the recentre)
    const float n = std::sqrt(c[8] * c[8] + c[9] * c[9] + c[10] * c[10]);
    if (n < 1e-4f) return false;
    for (int k = 0; k < 3; ++k) d[k] = -c[8 + k] / n;  // the camera looks along -c
    return true;
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
    for (int k = 0; k < 3; ++k) d[k] = (0.5f * (v[0].position[k] + v[1].position[k]) - p0[k]) * ws_inv();
    for (int r = 0; r < 3; ++r) out[r] = r0[r * 3 + 0] * d[0] + r0[r * 3 + 1] * d[1] + r0[r * 3 + 2] * d[2];
    return true;
}

uint32_t recentre_gen() { return g_recentre_gen.load(std::memory_order_relaxed); }
void room_set_owns(bool on) {
    if (on) g_room_state.fetch_or(1, std::memory_order_relaxed);
    else if (g_room_state.load(std::memory_order_relaxed) & 1) g_room_state.fetch_and(~1, std::memory_order_relaxed);
}
bool room_owns() { return (g_room_state.load(std::memory_order_relaxed) & 1) != 0; }
void room_set_take(bool on) { g_room_take_on.store(on, std::memory_order_relaxed); }
bool room_take() { return g_room_take_on.load(std::memory_order_relaxed); }
void room_allow_step(float m, double until_ms) {
    g_room_allow_m.store(m, std::memory_order_relaxed);
    g_room_allow_until.store(until_ms, std::memory_order_release);  // (after m: a reader that sees the window sees its m)
}
void body_turn_push(float heading_deg, float total_deg, bool ours) {
    {
        std::lock_guard lock(g_bt_mutex);
        g_bt_ring[g_bt_head] = BtEntry{heading_deg, total_deg, ours};
        g_bt_head = (g_bt_head + 1) % kBtRing;
        if (g_bt_n < kBtRing) ++g_bt_n;
    }
    if (!(g_room_state.load(std::memory_order_relaxed) & 4)) g_room_state.fetch_or(4, std::memory_order_relaxed);
}
void body_turn_stop() {
    if (!(g_room_state.load(std::memory_order_relaxed) & 4)) return;
    g_room_state.fetch_and(~4, std::memory_order_relaxed);
    std::lock_guard lock(g_bt_mutex);
    g_bt_n = 0;  // (pushed again from scratch: an old command never matched)
    g_bt_still = 0;
}
float body_turn_shown() { return g_bt_shown_pub.load(std::memory_order_relaxed); }
bool body_turn_ready() { return g_bt_ready.load(std::memory_order_relaxed); }
void body_turn_clear() {
    std::lock_guard lock(g_bt_mutex);
    g_bt_n = 0;  // (a recentre: the commands before it are not matched against the fresh origin)
    g_bt_still = 0;
    g_bt_miss_run = 0;
}
void body_turn_reset_view() { g_bt_view_step.store(0.0f, std::memory_order_relaxed); }
std::string body_turn_status() {
    char b[400];
    std::snprintf(b, sizeof(b),
                  "the origin's yaw: shown %.2f deg (frames %llu, turned %llu, matched within %.3f deg, misses %llu, the camera's offset %.2f, learned %d) | "
                  "the view's yaw %.2f deg, its largest step with the head still %.3f deg over %llu frames",
                  g_bt_shown_pub.load(), static_cast<unsigned long long>(g_bt_frames.load()), static_cast<unsigned long long>(g_bt_turned.load()),
                  g_bt_err_pub.load(), static_cast<unsigned long long>(g_bt_misses.load()), g_bt_off_pub.load(), g_bt_ready.load() ? 1 : 0, g_bt_view_pub.load(),
                  g_bt_view_step.load(), static_cast<unsigned long long>(g_bt_still_frames.load()));
    return b;
}
void room_shift_origin(float dr, float db) {  // queued: room_take_frame applies it before the next frame's passes
    std::lock_guard lock(g_recentre_mutex);
    if (!g_recentred) return;
    g_room_q[0] += dr;
    g_room_q[1] += db;
    g_room_state.fetch_or(2, std::memory_order_relaxed);
}
void room_shift_total(float out[3]) {
    std::lock_guard lock(g_recentre_mutex);
    std::memcpy(out, g_room_shift, sizeof(g_room_shift));
}
void room_stats(float* taken, float* judder, uint64_t* frames, uint64_t* rejects, float* refused) {
    *refused = g_room_refused.load(std::memory_order_relaxed);
    *taken = g_room_taken.load(std::memory_order_relaxed);
    *judder = g_room_judder.load(std::memory_order_relaxed);
    *frames = g_room_frames.load(std::memory_order_relaxed);
    *rejects = g_room_rejects.load(std::memory_order_relaxed);
}
void room_reset_judder() {
    g_room_judder.store(0.0f, std::memory_order_relaxed);
    std::lock_guard jl(g_judder_mutex);
    g_judder_n = g_judder_head = 0;
    g_judder_sum = 0.0;
    g_judder_over = 0;
    g_judder_total = 0;
}
void room_judder_stats(float* mean, float* p95, uint64_t* frames, uint64_t* over) {
    float v[1024];
    uint32_t nn = 0;
    {
        std::lock_guard jl(g_judder_mutex);
        nn = g_judder_n;
        std::memcpy(v, g_judder_ring, nn * sizeof(float));
        *frames = g_judder_total;
        *over = g_judder_over;
        *mean = g_judder_total ? static_cast<float>(g_judder_sum / static_cast<double>(g_judder_total)) : 0.0f;
    }
    std::sort(v, v + nn);
    *p95 = nn ? v[std::min<uint32_t>(nn - 1, static_cast<uint32_t>(0.95 * nn))] : 0.0f;  // (over the last 1024 such frames)
}
bool eye_world(float out[3]) {  // (a status: the frame camera's copy unsynchronised, a torn read only a wrong number; never a recentre)
    xr::EyeView v[2];
    if (!g_game_cam_valid.load(std::memory_order_relaxed) || !g_xr_pose.load(std::memory_order_relaxed) || !xr::eye_views_peek(v)) return false;
    float cam[16], q0[4], p0[3];
    std::memcpy(cam, g_game_cam, sizeof(cam));
    {
        std::lock_guard lock(g_recentre_mutex);
        if (!g_recentred) return false;
        std::memcpy(q0, g_q0, sizeof(q0));
        std::memcpy(p0, g_p0, sizeof(p0));
    }
    centre_eye_world(cam, v, q0, p0, out);
    return true;
}

bool neck_offset_impl(float out[3], bool queued) {
    xr::EyeView v[2];
    if (!xr::eye_views_peek(v)) return false;
    float q0[4], p0[3];
    {
        std::lock_guard lock(g_recentre_mutex);
        if (!g_recentred) return false;
        std::memcpy(q0, g_q0, sizeof(q0));
        std::memcpy(p0, g_p0, sizeof(p0));
        if (queued && (g_room_q[0] != 0.0f || g_room_q[1] != 0.0f)) {  // ([XR] RoomScale) as the next frame will have it
            float r[9];
            quat_matrix(g_q0, r);
            const float sc = 1.0f / ws_inv();
            for (int k = 0; k < 3; ++k) p0[k] += (r[k * 3 + 0] * g_room_q[0] + r[k * 3 + 2] * g_room_q[1]) * sc;
        }
    }
    constexpr float kNeck[3] = {0.0f, -0.10f, 0.08f};  // the neck from the centre eye, in the head's axes (y up, z back)
    float rh[9], q0c[4] = {-q0[0], -q0[1], -q0[2], q0[3]}, r0[9], d[3];
    quat_matrix(v[0].orientation, rh);
    quat_matrix(q0c, r0);
    for (int k = 0; k < 3; ++k)
        d[k] = (0.5f * (v[0].position[k] + v[1].position[k]) + rh[k * 3 + 0] * kNeck[0] + rh[k * 3 + 1] * kNeck[1] + rh[k * 3 + 2] * kNeck[2] - p0[k]) *
               ws_inv();
    for (int r = 0; r < 3; ++r) out[r] = r0[r * 3 + 0] * d[0] + r0[r * 3 + 1] * d[1] + r0[r * 3 + 2] * d[2] - kNeck[r];
    return true;
}

bool neck_offset(float out[3]) { return neck_offset_impl(out, false); }
bool room_neck_offset(float out[3]) { return neck_offset_impl(out, true); }

// 2026-10-27 ([XR] PhysicalCrouch; the user: "Implement roomscale and physical crouch", research/run27/roomscale.md
// C1): the same neck point in the LOCAL space itself, raw metres: not since the recentre, not divided by the world scale
// (the user's real body: a crouch is judged on it, whatever the view's scale)
bool neck_local(float out[3]) {
    xr::EyeView v[2];
    if (!xr::eye_views_peek(v)) return false;
    constexpr float kNeck[3] = {0.0f, -0.10f, 0.08f};  // (neck_offset's)
    float rh[9];
    quat_matrix(v[0].orientation, rh);
    for (int k = 0; k < 3; ++k)
        out[k] = 0.5f * (v[0].position[k] + v[1].position[k]) + rh[k * 3 + 0] * kNeck[0] + rh[k * 3 + 1] * kNeck[1] + rh[k * 3 + 2] * kNeck[2];
    return true;
}

// [Holsters] LeanSteady's model, shared (2026-10-27, roomscale.md B2/C1: the holsters and the physical crouch agree): the
// torso pivoting at the hips: its drop gives the lean's most forward reach (L sin a with L (1 - cos a) = the drop), the
// forward move up to that is the lean, the rest a step; the drop the lean explains is not a crouch
float lean_split(float n[3], float L) {
    if (!(L > 0.05f)) return 0.0f;
    const float drop = std::fmax(0.0f, -n[1]), dh = std::sqrt(n[0] * n[0] + n[2] * n[2]);
    const float c = 1.0f - std::fmin(drop, L) / L;
    const float lean = std::fmin(dh, L * std::sqrt(std::fmax(0.0f, 1.0f - c * c)));
    if (dh > 1e-4f) {
        n[0] *= (dh - lean) / dh;
        n[2] *= (dh - lean) / dh;
    }
    const float sa = std::fmin(1.0f, lean / L);
    n[1] += L * (1.0f - std::sqrt(1.0f - sa * sa));
    return lean;
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
    for (int k = 0; k < 3; ++k) d[k] = (pos[k] - p0[k]) * ws_inv();
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
    // (N^-1's rows b x c, c x a, a x b over a . (b x c)) and local = p0 + R0 prel (times the world scale: local_to_world's
    // inverse, so the rings, the reticle and the gestures' points stay where the scaled view draws them)
    const float *a = cam, *b = cam + 4, *c = cam + 8;
    const float bc[3] = {b[1] * c[2] - b[2] * c[1], b[2] * c[0] - b[0] * c[2], b[0] * c[1] - b[1] * c[0]};
    const float ca[3] = {c[1] * a[2] - c[2] * a[1], c[2] * a[0] - c[0] * a[2], c[0] * a[1] - c[1] * a[0]};
    const float ab[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    const float det = a[0] * bc[0] + a[1] * bc[1] + a[2] * bc[2];
    if (std::fabs(det) < 1e-6f) return false;
    const float v[3] = {wpos[0] - cam[12], wpos[1] - cam[13], wpos[2] - cam[14]};
    const float sc = det * ws_inv();  // (prel = N^-1 (w - d), then times the scale)
    const float prel[3] = {(bc[0] * v[0] + bc[1] * v[1] + bc[2] * v[2]) / sc, (ca[0] * v[0] + ca[1] * v[1] + ca[2] * v[2]) / sc,
                           (ab[0] * v[0] + ab[1] * v[1] + ab[2] * v[2]) / sc};
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

float world_scale() { return g_world_scale.load(std::memory_order_relaxed); }
void set_world_scale(float s, bool save) {
    s = std::fmin(2.0f, std::fmax(0.5f, s));
    if (std::fabs(g_world_scale.exchange(s) - s) > 1e-4f) log::info("[cam] world scale %.2f (the headset's positions divided by it)", s);
    if (save) {
        char b[16];
        std::snprintf(b, sizeof(b), "%.2f", s);
        config::set("Comfort", "WorldScale", b);
    }
}
bool local_rel(const float* pos, float* prel, float* turn_deg) {
    float q0[4], p0[3];
    {
        std::lock_guard lock(g_recentre_mutex);
        if (!g_recentred) return false;
        if (turn_deg) *turn_deg = g_bt_applied;  // ([XR] BodyFollowsTurn: the turn this q0 carries, read with it)
        std::memcpy(q0, g_q0, sizeof(q0));
        std::memcpy(p0, g_p0_base, sizeof(p0));  // ([XR] RoomScale: the recentre's origin, not the room-shifted one: the hand's own motion)
    }
    float q0c[4] = {-q0[0], -q0[1], -q0[2], q0[3]}, r0[9], d[3];
    quat_matrix(q0c, r0);
    for (int k = 0; k < 3; ++k) d[k] = (pos[k] - p0[k]) * ws_inv();
    for (int r = 0; r < 3; ++r) prel[r] = r0[r * 3 + 0] * d[0] + r0[r * 3 + 1] * d[1] + r0[r * 3 + 2] * d[2];
    return true;
}

// world_to_local then local_rel with one origin (the review of BodyFollowsTurn: two locks could see two yaws, a turn
// between them a frame's step of false motion in a GunMelee point's history), and the follow's turn it carries
bool world_to_local_rel(const float* cam, const float* wpos, float* prel, float* turn_deg) {
    if (!cam || !wpos || !prel) return false;
    float q0[4], p0[3], pb[3], turn;
    {
        std::lock_guard lock(g_recentre_mutex);
        if (!g_recentred) return false;
        std::memcpy(q0, g_q0, sizeof(q0));
        std::memcpy(p0, g_p0, sizeof(p0));
        std::memcpy(pb, g_p0_base, sizeof(pb));
        turn = g_bt_applied;
    }
    const float *a = cam, *b = cam + 4, *c = cam + 8;  // (world_to_local's, then local_rel's, as they compute them)
    const float bc[3] = {b[1] * c[2] - b[2] * c[1], b[2] * c[0] - b[0] * c[2], b[0] * c[1] - b[1] * c[0]};
    const float ca[3] = {c[1] * a[2] - c[2] * a[1], c[2] * a[0] - c[0] * a[2], c[0] * a[1] - c[1] * a[0]};
    const float ab[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    const float det = a[0] * bc[0] + a[1] * bc[1] + a[2] * bc[2];
    if (std::fabs(det) < 1e-6f) return false;
    const float v[3] = {wpos[0] - cam[12], wpos[1] - cam[13], wpos[2] - cam[14]};
    const float sc = det * ws_inv();
    const float pr[3] = {(bc[0] * v[0] + bc[1] * v[1] + bc[2] * v[2]) / sc, (ca[0] * v[0] + ca[1] * v[1] + ca[2] * v[2]) / sc,
                         (ab[0] * v[0] + ab[1] * v[1] + ab[2] * v[2]) / sc};
    float r0[9], lpos[3];
    quat_matrix(q0, r0);
    for (int r = 0; r < 3; ++r) lpos[r] = p0[r] + r0[r * 3] * pr[0] + r0[r * 3 + 1] * pr[1] + r0[r * 3 + 2] * pr[2];
    float q0c[4] = {-q0[0], -q0[1], -q0[2], q0[3]}, r1[9], d[3];
    quat_matrix(q0c, r1);
    for (int k = 0; k < 3; ++k) d[k] = (lpos[k] - pb[k]) * ws_inv();
    for (int r = 0; r < 3; ++r) prel[r] = r1[r * 3 + 0] * d[0] + r1[r * 3 + 1] * d[1] + r1[r * 3 + 2] * d[2];
    if (turn_deg) *turn_deg = turn;
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
void set_after_push(AfterPush_t fn) { g_after_push.store(fn, std::memory_order_relaxed); }
bool push_hook_installed() { return o_PushGlobals != nullptr; }  // (hooks::install leaves it null when the hook failed)
void* eye2_viewport() { return g_eye2_frame == state::presents.load() ? static_cast<void*>(g_eye2_vp) : nullptr; }
void* shadow_union_vp() { return g_union_ok ? g_union_vp : nullptr; }
void scene_clip(float* near_m, float* far_m) {
    *near_m = g_scene_near.load();
    *far_m = g_scene_far.load();
}

bool mono_camera_changed() {
    if (g_yaw.load(std::memory_order_relaxed) != 0) return true;
    return g_active.load(std::memory_order_relaxed) && current().mode == Mode::Camera;
}

// (the scope spike) its reads, SEH only (no objects with destructors in this frame). The overlay's and the streamer's
// globals are RDR.exe+0x2c45770 and +0x2ac5140 of the build the research read (no anchors: a test's numbers only, tagged
// so in the status)
static void scope_reads(int* zoom, int* aimbit, int* ov_on, int* ov_drawn, int* ov_2c, int* ov_model, int* sniper) {
    __try {
        const uintptr_t pia = anchors::addr(anchors::Id::PlayerInfo);
        const uintptr_t pi = pia ? *reinterpret_cast<const uintptr_t*>(pia) : 0;
        if (pi) {
            const uintptr_t R = *reinterpret_cast<const uintptr_t*>(pi + 0x410);
            if (R) *zoom = *reinterpret_cast<const int*>(R + 0x14), *aimbit = (*reinterpret_cast<const uint32_t*>(R + 0xc) >> 3) & 1;
        }
        const uintptr_t ov = *reinterpret_cast<const uintptr_t*>(anchors::base() + 0x2c45770);
        if (ov) {
            *ov_on = *reinterpret_cast<const uint8_t*>(ov + 0x28), *ov_drawn = *reinterpret_cast<const uint8_t*>(ov + 0x29);
            *ov_2c = *reinterpret_cast<const uint8_t*>(ov + 0x2c), *ov_model = *reinterpret_cast<const int*>(ov + 0x34);
        }
        const uintptr_t st = *reinterpret_cast<const uintptr_t*>(anchors::base() + 0x2ac5140);
        if (st) *sniper = *reinterpret_cast<const uint8_t*>(st + 0x126);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *zoom = -8;
    }
}

// (run 29 item 3) the scope spike's reads, test only ("scope game"; scope.cpp's command hands it over): the game's zoom
// (PlayerInfo +0x410 -> +0x14), the aim bit, the scope overlay (*(0x142c45770): +0x28 on, +0x29 drawn, +0x2c, +0x34 the
// model), the streamer's sniper byte (*(0x142ac5140) +0x126), the game's own vertical FOV at each eye pass's start
std::string scope_command(const std::vector<std::string>&) {
    char o[400];
    int zoom = -9, aimbit = -1, ov_on = -1, ov_drawn = -1, ov_2c = -1, ov_model = -9, sniper = -1;
    scope_reads(&zoom, &aimbit, &ov_on, &ov_drawn, &ov_2c, &ov_model, &sniper);
    std::snprintf(o, sizeof(o), "scope game: zoom state %d aim %d | overlay on %d drawn %d 2c %d model %d | streamer sniper byte %d (both at the "
                  "research build's rvas) | game vfov %.3f %.3f | scoped passes %llu",
                  zoom, aimbit, ov_on, ov_drawn, ov_2c, ov_model, sniper, g_game_vfov[0].load(), g_game_vfov[1].load(),
                  static_cast<unsigned long long>(g_scope_passes.load()));
    return o;
}

}  // namespace rdrvr::camera_lever
