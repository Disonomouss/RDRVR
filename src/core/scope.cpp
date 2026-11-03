#include "core/scope.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "core/aim.h"
#include "core/api.h"
#include "core/body.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/dlss.h"
#include "core/dual.h"
#include "core/log.h"
#include "core/taa.h"
#include "core/xr_blit.h"

namespace rdrvr::scope {
namespace {

constexpr float kDeg = 0.0174532925f;

// Each scoped gun's eyepiece and objective in the gun's own frame (the model's: x right, y up, z back; metres), measured
// from the models (fragments.rpf's rifle_*01x.wft, extracted and read in the scratchpad only, r55scope/axis.py; nothing of
// the game's kept): the scope tube's rings, their centres on one line parallel to the barrel. The Rolling Block's tube
// runs from its objective bell (r 11 mm) at z -0.710 to its eyepiece ring (r 10.5 mm) at z +0.048, its axis 91.6 mm up;
// the Carcano's short tube from z -0.250 to +0.019, 127.7 mm up. The Buffalo has no tube (a tang peep sight at z +0.03 to
// +0.057, a globe front sight r 4.6 mm at y 80.8 mm, z -0.697): its line of sight at the globe's height, parallel to the
// barrel [I: the peep's aperture not modelled]. The Explosive Rifle (Undead Nightmare, open sights only) has none: the
// user's round 60, "Explosive rifle should not have a scope". "scope place" (the session) tunes them.
struct Place {
    int weapon;
    const char* name;
    float eye[3], obj[3];
    const char* how;
};
Place g_places[] = {
    {19, "Rolling Block", {0.0f, 0.0916f, 0.0483f}, {0.0f, 0.0916f, -0.7104f}, "tube"},
    {20, "Carcano", {0.0f, 0.1277f, 0.0190f}, {0.0f, 0.1277f, -0.2500f}, "tube"},
    {14, "Buffalo", {0.0f, 0.0808f, 0.0571f}, {0.0f, 0.0808f, -0.6968f}, "sights"},
};
std::mutex g_place_mx;  // g_places (the session's "scope place")

bool place_of(int weapon, Place* out) {
    std::lock_guard lock(g_place_mx);
    for (const Place& p : g_places)
        if (p.weapon == weapon) {
            *out = p;
            return true;
        }
    return false;
}

struct Cfg {
    float power = 4.0f;        // [Scope] Power: realistic magnification
    float game_power = 7.75f;  // [Scope] GamePower: the game's sniper zoom (fovspline.cmt ARC_Sniper_Zoom's 7.75 [I: read as its zoom])
    float apparent = 45.0f;    // [Scope] ApparentField: the circle's size at the eye (degrees; 45 since round 60: "needs to be bigger")
    // (GOAL-scope-entry.md item 2) the scope eye: the eye behind the eyepiece along the scope's axis within EntryDepth
    // (metres), within EntryRadius across it, looking along it within EntryAngle (degrees), for EntryHoldMs
    float depth = 0.15f;       // [Scope] EntryDepth
    float radius = 0.04f;      // [Scope] EntryRadius
    float angle = 15.0f;       // [Scope] EntryAngle
    float hold_ms = 120.0f;    // [Scope] EntryHoldMs: in the zone this long before it enters (a swing past the face: none)
    float max_speed = 0.5f;    // [Scope] EntryMaxSpeed: the eyepiece faster than this against the eye (m/s) restarts the wait
    float hyst = 0.02f;        // [Scope] Hysteresis: the scope eye stays until this much farther out (depth, radius; the angle 5 deg)
    float fade = 0.1f;         // [Scope] Fade: each half of the change (seconds)
    float relief = 0.05f;      // [Scope] EyeRelief: the picture sized for an eye this far behind the eyepiece
    float image_dist = 0.0f;   // [Scope] ImageDistance: the picture this far ahead of the eyepiece along the axis (0: at it)
    float dark = 1.0f;         // [Scope] OtherEyeDark: how dark the other eye goes with OtherEye=dark (1 black)
    uint32_t image = 1024;     // [Scope] ImageSize: the picture's quad image (square)
    bool reticle = true;       // [Scope] Reticle
};
std::mutex g_cfg_mx;
Cfg g_cfg;
std::atomic<bool> g_on{false}, g_game{false}, g_dark{false}, g_sp_swap{false}, g_bino{false};
std::atomic<int> g_force{-2};  // "scope force": -2 auto (the eyepiece), -1 never, 0 / 1 that eye
// (the review) the picture cannot be made (its swapchain or its pipeline failed: their one attempt, latched): the scope
// refused rather than a blind eye, until it is turned on again or "scope reset" (a transient miss, a skipped blit or a
// wait timed out, never counts); any frame published (frame_layer's early out while nothing ever was)
std::atomic<bool> g_pic_broken{false}, g_any_pub{false};
// (the re-review) soft misses: a wanted picture not drawn because its image could not be acquired or waited (not a
// timeout), 45 frames in a row: the scope out, kept out until g_soft_until (2 s later); a drawn picture resets the count
std::atomic<int> g_soft_miss{0};
std::atomic<double> g_soft_until{0.0};
std::atomic<uint64_t> g_soft_outs{0};

Cfg cfg() {
    std::lock_guard lock(g_cfg_mx);
    return g_cfg;
}

// the render thread's state (frame_begin, pass_camera)
struct St {
    int eye = -1;          // the eye the scope is at: 0, 1, 2 both (binoculars); -1 none
    float p = 0.0f;        // its projection toward black (0 the normal view, 1 black)
    float q = 0.0f;        // the picture's opacity
    bool scoped = false;   // its camera is the scope's
    bool need_swap = false, swapped = false, armed = false;
    bool full_noted = false, leave_noted = false;  // (the change's log lines, once each)
    uint64_t last_xr = 0;  // the XR frame of the last call (a jump: frames without a stereo pass)
    int gun_miss = 0;      // frames in a row without the gun's sample while the scope is in or changing
    int last_want = -1;    // the previous frame's choice (held through a short loss of the gun's sample)
    int blink = 0;         // frames both eyes black (the eyes' order changed: single pass, SinglePassSwap)
    int zone_eye = -1;     // the eye in the entry zone while its EntryHoldMs runs (-1 none)
    int zone_miss = 0;     // frames without the gun's sample during that wait
    double zone_since = 0.0;
    float zone_rel[3] = {};  // the waiting eye's place against the eyepiece last frame (world; its speed)
    bool zone_rel_ok = false;
    double zone_rel_ms = 0.0;
    uint64_t shots = ~0ull;  // aim::player_shots at the last frame (a shot's measure line; ~0 none yet)
    double near_ms = 0.0;    // the last "near" measure line
    double last_ms = 0.0;
    bool cam_ok[2] = {false, false};
    float cam[2][16] = {};
    float tan[2][4] = {};
    float qpos[2][3] = {}, qrot[2][4] = {}, qsize[2] = {0.0f, 0.0f};
    bool quad_ok[2] = {false, false};
};
St g_st;

std::atomic<uint64_t> g_frames{0}, g_entries{0}, g_exits{0}, g_resets{0}, g_refused_sp{0}, g_refused_bino{0}, g_gap_resets{0},
    g_l_exact{0}, g_l_other{0}, g_l_none{0}, g_drawn{0}, g_submitted{0}, g_swaps{0};

struct Rec {
    uint64_t frame = 0;
    bool valid = false;
    bool steady = false;  // scoped, the picture fully in, no change under way (its images the scope's whatever the frame)
    Layer L;
};
std::mutex g_ring_mx;
Rec g_ring[8];

// the status's numbers (written by frame_begin)
struct Pub {
    int weapon = -1, want = -1, eye = -1, first = 0;
    bool gun = false, single = false, scoped = false, swapped = false;
    float p = 0, q = 0, field = 0;
    float dist[2] = {-1.0f, -1.0f};
    // (item 1) each eye against the eyepiece (metres, the real world's): behind it along the scope's back axis, across that
    // axis, the angle between the eye's view and the scope's; in the entry zone (before the hold); the hold's wait
    float along[2] = {0, 0}, across[2] = {0, 0}, angle[2] = {0, 0};
    bool zone[2] = {false, false}, ahead[2] = {false, false};
    int pending = -1;
    float speed = -1.0f;  // the waiting eye's speed against the eyepiece (m/s; -1 none)
    float eye_m[3] = {}, obj_m[3] = {}, mo[3] = {};
    bool mo_ok = false;
    float uv[2][2][2] = {};  // [eye][0 eyepiece, 1 objective][u v]
    bool uv_ok[2][2] = {};
    float eyep_w[3] = {}, obj_w[3] = {};
    float qpos[2][3] = {}, qsize[2] = {};
    float eyes[2][3] = {}, fwd[2][3] = {}, back[3] = {}, tan[2][4] = {};
    int blink = 0;
    uint64_t xr_frame = 0;
};
std::mutex g_pub_mx;
Pub g_pub;

float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void cross3(const float* a, const float* b, float* o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
bool norm3(float* v) {
    const float l = std::sqrt(dot3(v, v));
    if (!(l > 1e-6f)) return false;
    for (int k = 0; k < 3; ++k) v[k] /= l;
    return true;
}

// a point in an eye's picture (its camera rows and tangents): u right, v down, 0-1 over the view
bool project(const float* cam, const float* tan, const float* p, float uv[2]) {
    const float v[3] = {p[0] - cam[12], p[1] - cam[13], p[2] - cam[14]};
    const float x = dot3(v, cam), y = dot3(v, cam + 4), z = -dot3(v, cam + 8);
    if (!(z > 1e-4f) || !(tan[1] > tan[0]) || !(tan[2] > tan[3])) return false;
    uv[0] = (x / z - tan[0]) / (tan[1] - tan[0]);
    uv[1] = (tan[2] - y / z) / (tan[2] - tan[3]);
    return true;
}

// a rotation (columns x, y, z: orthonormal) as a quaternion (x y z w)
void mat_quat(const float* X, const float* Y, const float* Z, float q[4]) {
    const float m00 = X[0], m11 = Y[1], m22 = Z[2], tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (Y[2] - Z[1]) / s;
        q[1] = (Z[0] - X[2]) / s;
        q[2] = (X[1] - Y[0]) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q[3] = (Y[2] - Z[1]) / s;
        q[0] = 0.25f * s;
        q[1] = (Y[0] + X[1]) / s;
        q[2] = (Z[0] + X[2]) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q[3] = (Z[0] - X[2]) / s;
        q[0] = (Y[0] + X[1]) / s;
        q[1] = 0.25f * s;
        q[2] = (Z[1] + Y[2]) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q[3] = (X[1] - Y[0]) / s;
        q[0] = (Z[0] + X[2]) / s;
        q[1] = (Z[1] + Y[2]) / s;
        q[2] = 0.25f * s;
    }
    const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l > 1e-6f)
        for (int k = 0; k < 4; ++k) q[k] /= l;
}

// A frame (rows right, up, back at `at`; world) as a LOCAL pose: the place and its orientation through world_to_local
// (points 10 cm along each row: the world scale and the recentre turn in)
bool to_local(const float* cam, const float* at, const float* R, const float* U, const float* B, float pos[3], float q[4]) {
    float l0[3], lx[3], ly[3], lz[3];
    float px[3], py[3], pz[3];
    for (int k = 0; k < 3; ++k) {
        px[k] = at[k] + 0.1f * R[k];
        py[k] = at[k] + 0.1f * U[k];
        pz[k] = at[k] + 0.1f * B[k];
    }
    if (!camera_lever::world_to_local(cam, at, l0) || !camera_lever::world_to_local(cam, px, lx) || !camera_lever::world_to_local(cam, py, ly) ||
        !camera_lever::world_to_local(cam, pz, lz))
        return false;
    float X[3], Y[3], Z[3];
    for (int k = 0; k < 3; ++k) {
        X[k] = lx[k] - l0[k];
        Y[k] = ly[k] - l0[k];
        Z[k] = lz[k] - l0[k];
    }
    if (!norm3(Z)) return false;
    const float yz = dot3(Y, Z);
    for (int k = 0; k < 3; ++k) Y[k] -= yz * Z[k];
    if (!norm3(Y)) return false;
    cross3(Y, Z, X);  // (x = y cross z)
    std::memcpy(pos, l0, sizeof(l0));
    mat_quat(X, Y, Z, q);
    return true;
}

// The gun in John's gun hand as drawn (body::item_gun: the correction on its build's game matrix): its rows (unit) and
// origin, its weapon, its scope's places, and its tune's MuzzleOffset
struct Gun {
    int weapon = -1;
    float R[3][3] = {}, o[3] = {};
    Place place{};
    float mo[3] = {};
    bool mo_ok = false;
};
// (2026-11-01, the off-hand build step 4, S5) no scoped gun in hand: a scoped second gun's (DualWield's, not the copy nor
// a carry), from its drawn root (body's drawn_second: captured only while one is out); its own muzzle offset
bool second_frame(Gun* g) {
    const dual::State ds = dual::state();
    const bool want = ds.on && ds.ready && !ds.copy && !ds.carry && place_of(ds.weapon, &g->place);
    body::set_drawn_second_scope(want);  // (the review: its own bit, set each frame; never the test's "aim barrelstep2")
    float R[9], o[3];
    uint64_t fr = 0;
    bool copy = false;
    if (!want || !body::drawn_second(R, o, &fr, &copy) || copy) return false;
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) g->R[i][k] = R[k * 3 + i];  // (drawn_second's axes are its columns)
        if (!norm3(g->R[i])) return false;
    }
    std::memcpy(g->o, o, sizeof(g->o));
    g->weapon = ds.weapon;
    int john = -1;
    const uintptr_t W = dual::secondary_W(&john);
    g->mo_ok = W && aim::weapon_muzzle_offset(W, g->mo);
    return true;
}
bool gun_frame(Gun* g) {
    RdrvrActorState st{};
    if (!api::actor_state(&st)) {
        body::set_drawn_second_scope(false);
        return false;
    }
    if (!place_of(st.weapon, &g->place)) return second_frame(g);
    body::set_drawn_second_scope(false);  // (the review: a scoped gun in hand: no capture left on)
    float A[9], a[3], gm[16];
    bool ik = false;
    uintptr_t W = 0;
    if (!body::item_gun(1, A, a, &ik, gm, &W) || !ik || !W) return false;
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) g->R[i][k] = A[k * 3] * gm[i * 4] + A[k * 3 + 1] * gm[i * 4 + 1] + A[k * 3 + 2] * gm[i * 4 + 2];
        if (!norm3(g->R[i])) return false;
    }
    for (int k = 0; k < 3; ++k) g->o[k] = A[k * 3] * gm[12] + A[k * 3 + 1] * gm[13] + A[k * 3 + 2] * gm[14] + a[k];
    g->weapon = st.weapon;
    g->mo_ok = aim::weapon_muzzle_offset(W, g->mo);
    return true;
}
void gun_point(const Gun& g, const float* m, float* w) {
    for (int k = 0; k < 3; ++k) w[k] = g.o[k] + m[0] * g.R[0][k] + m[1] * g.R[1][k] + m[2] * g.R[2][k];
}
void gun_dir(const Gun& g, const float* m, float* w) {
    for (int k = 0; k < 3; ++k) w[k] = m[0] * g.R[0][k] + m[1] * g.R[1][k] + m[2] * g.R[2][k];
}

// the history of the post run that draws `eye` (slot 0 the first view's), TAA's and DLSS's
void reset_eye(int eye, int first) {
    for (int e = 0; e < 2; ++e) {
        if (eye != 2 && eye != e) continue;
        const int slot = e == first ? 0 : 1;
        taa::request_reset(slot);
        dlss::request_reset(slot);
        g_resets.fetch_add(1, std::memory_order_relaxed);
    }
}

void publish(uint64_t xr_frame, const Layer& L, bool steady) {
    std::lock_guard lock(g_ring_mx);
    Rec& r = g_ring[xr_frame % 8];
    r.frame = xr_frame;
    r.valid = true;
    r.steady = steady;
    r.L = L;
    g_any_pub.store(true, std::memory_order_relaxed);
}

}  // namespace

void init() {
    Cfg c;
    c.power = std::clamp(config::get_float("Scope", "Power", 4.0f), 1.0f, 20.0f);
    c.game_power = std::clamp(config::get_float("Scope", "GamePower", 7.75f), 1.0f, 30.0f);
    c.apparent = std::clamp(config::get_float("Scope", "ApparentField", 45.0f), 5.0f, 80.0f);
    c.depth = std::clamp(config::get_float("Scope", "EntryDepth", 0.15f), 0.02f, 0.40f);
    c.radius = std::clamp(config::get_float("Scope", "EntryRadius", 0.04f), 0.005f, 0.15f);
    c.angle = std::clamp(config::get_float("Scope", "EntryAngle", 15.0f), 1.0f, 60.0f);
    c.hold_ms = std::clamp(config::get_float("Scope", "EntryHoldMs", 120.0f), 0.0f, 5000.0f);
    c.max_speed = std::clamp(config::get_float("Scope", "EntryMaxSpeed", 0.5f), 0.05f, 20.0f);
    c.hyst = std::clamp(config::get_float("Scope", "Hysteresis", 0.02f), 0.0f, 0.20f);
    c.fade = std::clamp(config::get_float("Scope", "Fade", 0.1f), 0.0f, 1.0f);
    c.relief = std::clamp(config::get_float("Scope", "EyeRelief", 0.05f), 0.01f, 0.30f);
    c.image_dist = std::clamp(config::get_float("Scope", "ImageDistance", 0.0f), 0.0f, 50.0f);
    c.dark = std::clamp(config::get_float("Scope", "OtherEyeDark", 1.0f), 0.0f, 1.0f);
    c.image = static_cast<uint32_t>(std::clamp(config::get_int("Scope", "ImageSize", 1024), 128, 4096));
    c.reticle = config::get_bool("Scope", "Reticle", true);
    {
        std::lock_guard lock(g_cfg_mx);
        g_cfg = c;
    }
    g_on = config::get_bool("Scope", "Enabled", false);
    g_game = config::get_string("Scope", "Magnification", "game") == "game";
    g_dark = config::get_string("Scope", "OtherEye", "open") == "dark";
    g_sp_swap = config::get_bool("Scope", "SinglePassSwap", true);  // (GOAL-single-pass-default.md item 4: on, single pass the default)
}

bool enabled() { return g_on.load(std::memory_order_relaxed); }
bool picture_broken() { return g_pic_broken.load(std::memory_order_relaxed); }
void set_enabled(bool on, bool save) {
    if (on) g_pic_broken.store(false, std::memory_order_relaxed);  // (another try: a latched failure refuses it again at once)
    if (g_on.exchange(on) != on) log::info("[scope] %s", on ? "on" : "off");
    if (!on) body::set_drawn_second_scope(false);  // (the review: no capture left on with the scope off)
    if (save) config::set("Scope", "Enabled", on ? "1" : "0");
}
bool game_zoom() { return g_game.load(std::memory_order_relaxed); }
void set_game_zoom(bool on, bool save) {
    if (g_game.exchange(on) != on) log::info("[scope] magnification: %s", on ? "the game's zoom" : "realistic");
    if (save) config::set("Scope", "Magnification", on ? "game" : "realistic");
}
bool other_dark() { return g_dark.load(std::memory_order_relaxed); }
void set_other_dark(bool on, bool save) {
    if (g_dark.exchange(on) != on) log::info("[scope] the other eye: %s", on ? "dark" : "open");
    if (save) config::set("Scope", "OtherEye", on ? "dark" : "open");
}

bool frame_begin(const FrameIn& in) {
    const Cfg c = cfg();
    St& s = g_st;
    g_frames.fetch_add(1, std::memory_order_relaxed);
    const double now = log::now_ms();
    const double gap = s.last_ms > 0.0 ? now - s.last_ms : 0.0;
    s.last_ms = now;
    const int base_first = in.first;  // the frame's first eye without the scope's swap (FrameIn.first: g_swap's alone)
    // (the review) frames without a stereo pass (a loading screen, mono: XR frames ended without this call) while in: out at
    // once, its history discarded; a long frame alone (a hitch) is not a reason
    const bool jumped = s.last_xr && in.xr_frame > s.last_xr + 10;
    s.last_xr = in.xr_frame;
    if (jumped && (s.eye >= 0 || s.swapped)) {
        const bool was_swapped = s.swapped, was_scoped = s.scoped;
        const int was_eye = s.eye;
        s = St{};
        s.last_ms = now;
        s.last_xr = in.xr_frame;
        s.blink = was_swapped ? 3 : 0;
        if (was_scoped) reset_eye(was_swapped ? 2 : was_eye, base_first);
        g_gap_resets.fetch_add(1, std::memory_order_relaxed);
        log::limited("scope.change", 4000, "[scope] out at once: %llu XR frames without a stereo pass", static_cast<unsigned long long>(in.xr_frame));
    }
    // (the review) idle a while (nothing asked for the scope): its first frame no step at all
    const float dt = gap > 100.0 ? 0.0f : static_cast<float>(std::max(gap, 0.0)) * 0.001f;
    const float step = c.fade > 0.005f ? dt / c.fade : 1.0f;
    const float mag = g_game.load(std::memory_order_relaxed) ? c.game_power : c.power;
    const float t = std::tan(0.5f * c.apparent * kDeg) / mag;  // the true field's half tangent
    const float ws = camera_lever::world_scale();

    Pub pub;
    pub.single = in.single_pass;
    pub.field = 2.0f * std::atan(t) / kDeg;
    pub.xr_frame = in.xr_frame;
    for (int e = 0; e < 2; ++e)
        for (int k = 0; k < 4; ++k) {
            if (k < 3) pub.eyes[e][k] = in.eye[e][12 + k], pub.fwd[e][k] = -in.eye[e][8 + k];
            pub.tan[e][k] = in.tan[e][k];
        }
    Gun g;
    const bool gun = gun_frame(&g);
    pub.gun = gun;
    pub.weapon = gun ? g.weapon : -1;
    float E[3] = {}, O[3] = {}, C[3] = {}, Rw[3] = {}, Uw[3] = {}, Bw[3] = {};
    bool axis_ok = false;
    if (gun) {
        gun_point(g, g.place.eye, E);
        gun_point(g, g.place.obj, O);
        // the axis: the eyepiece to the objective (the barrel's line in each model), the view from the objective, or from
        // past the muzzle when the barrel reaches beyond it (the Carcano's short tube): the gun's own mesh never in it
        float dm[3] = {g.place.obj[0] - g.place.eye[0], g.place.obj[1] - g.place.eye[1], g.place.obj[2] - g.place.eye[2]};
        if (norm3(dm) && dm[2] < -0.5f) {
            float ahead = 0.01f;
            if (g.mo_ok && g.mo[2] < g.place.obj[2]) ahead += (g.place.obj[2] - g.mo[2]) / -dm[2];
            const float cm[3] = {g.place.obj[0] + ahead * dm[0], g.place.obj[1] + ahead * dm[1], g.place.obj[2] + ahead * dm[2]};
            gun_point(g, cm, C);
            const float back_m[3] = {-dm[0], -dm[1], -dm[2]}, up_m[3] = {0.0f, 1.0f, 0.0f};
            gun_dir(g, back_m, Bw);
            gun_dir(g, up_m, Uw);
            const float ub = dot3(Uw, Bw);
            for (int k = 0; k < 3; ++k) Uw[k] -= ub * Bw[k];
            axis_ok = norm3(Bw) && norm3(Uw);
            if (axis_ok) cross3(Uw, Bw, Rw);
        }
        std::memcpy(pub.eye_m, g.place.eye, sizeof(pub.eye_m));
        std::memcpy(pub.obj_m, g.place.obj, sizeof(pub.obj_m));
        std::memcpy(pub.mo, g.mo, sizeof(pub.mo));
        pub.mo_ok = g.mo_ok;
        std::memcpy(pub.eyep_w, E, sizeof(E));
        std::memcpy(pub.back, Bw, sizeof(Bw));
        std::memcpy(pub.obj_w, O, sizeof(O));
        for (int e = 0; e < 2; ++e) {
            pub.uv_ok[e][0] = project(in.eye[e], in.tan[e], E, pub.uv[e][0]);
            pub.uv_ok[e][1] = project(in.eye[e], in.tan[e], O, pub.uv[e][1]);
        }
    }

    // (GOAL-scope-entry.md item 2) the eye behind the eyepiece: along the scope's back axis no more than 1 cm ahead of the
    // eyepiece and within EntryDepth behind it, within EntryRadius across that axis, looking along the scope within
    // EntryAngle; the scope's own eye stays within each plus Hysteresis (the angle 5 deg more). Of two eyes in it, the
    // nearer the axis. (Round 58: the old 6 cm sphere at the eyepiece was out of reach in the headset.) Distances in the
    // real world's metres (the world scale's).
    int want = -1;
    float near_m = 1e9f;
    if (gun && axis_ok) {
        float best = 1e9f;
        const float us = ws > 0.0f ? ws : 1.0f;
        for (int e = 0; e < 2; ++e) {
            const float* ep = in.eye[e] + 12;
            const float v[3] = {ep[0] - E[0], ep[1] - E[1], ep[2] - E[2]};  // the eyepiece to the eye
            const float along = dot3(v, Bw);
            const float x[3] = {v[0] - along * Bw[0], v[1] - along * Bw[1], v[2] - along * Bw[2]};
            const float fwd[3] = {-in.eye[e][8], -in.eye[e][9], -in.eye[e][10]};
            const float ca = std::clamp(-dot3(fwd, Bw), -1.0f, 1.0f);  // the eye's view against the scope's (forward = -back)
            const float d = std::sqrt(dot3(v, v)) * us, a_m = along * us, x_m = std::sqrt(dot3(x, x)) * us, deg = std::acos(ca) / kDeg;
            pub.dist[e] = d;
            pub.along[e] = a_m;
            pub.across[e] = x_m;
            pub.angle[e] = deg;
            pub.ahead[e] = -dot3(v, fwd) > 0.0f;  // the eyepiece ahead of the eye along its view
            near_m = std::min(near_m, d);
            const bool own = s.eye == e && (s.p > 0.0f || s.scoped);
            const float h = own ? c.hyst : 0.0f;
            const bool zone = a_m > -0.01f && a_m < c.depth + h && x_m < c.radius + h && deg < c.angle + (own ? 5.0f : 0.0f);
            pub.zone[e] = zone;
            if (zone && x_m < best) {
                best = x_m;
                want = e;
            }
        }
        // (the review) the scope eye kept while it is in its own zone: two eyes near the axis (the eyepiece at the nose's
        // bridge) must not trade it frame by frame
        if (s.eye >= 0 && s.eye < 2 && pub.zone[s.eye]) want = s.eye;
        if (!g_on.load(std::memory_order_relaxed)) want = -1;
    }
    // the hold: the eyepiece in the zone (either eye: a change of eye inside it does not restart it) EntryHoldMs before an
    // eye enters; the scope's own eye, in or changing, does not wait. (The review) a few frames without the gun's sample (up
    // to 10) keep the wait as it is.
    if (!(gun && axis_ok)) {
        if (s.zone_eye >= 0 && ++s.zone_miss > 10) s.zone_eye = -1;  // (the speed's last sample kept: it divides by the real time)
    } else if (want >= 0 && want != s.eye) {
        s.zone_miss = 0;
        if (s.zone_eye < 0) s.zone_since = now;
        if (s.zone_eye != want) s.zone_rel_ok = false;  // (another eye: its speed from its own next sample, not the IPD's jump)
        s.zone_eye = want;
        // r307 E1: a sweep across the eye at 1 m/s stayed 120 ms in the zone (the eye began to black). The wait restarts
        // while the eyepiece moves faster than EntryMaxSpeed against the eye: aiming holds it still
        const float* ep = in.eye[want] + 12;
        const float rel[3] = {ep[0] - E[0], ep[1] - E[1], ep[2] - E[2]};
        if (s.zone_rel_ok && now > s.zone_rel_ms) {
            const float dv[3] = {rel[0] - s.zone_rel[0], rel[1] - s.zone_rel[1], rel[2] - s.zone_rel[2]};
            const float sp = std::sqrt(dot3(dv, dv)) * (ws > 0.0f ? ws : 1.0f) / static_cast<float>((now - s.zone_rel_ms) * 0.001);
            pub.speed = sp;
            if (sp > c.max_speed) s.zone_since = now;
        }
        std::memcpy(s.zone_rel, rel, sizeof(rel));
        s.zone_rel_ok = true;
        s.zone_rel_ms = now;
        if (now - s.zone_since < c.hold_ms) {
            pub.pending = want;
            want = -1;
        }
    } else {
        s.zone_miss = 0;
        s.zone_eye = -1;
        s.zone_rel_ok = false;
    }
    // (the review) the gun's sample missing a few frames (an animation's frame, a late publish) while in or changing: the
    // previous frame's choice held (an exit under way goes on; an entry too)
    if (s.eye >= 0 && s.eye < 2 && !(gun && axis_ok)) {
        if (++s.gun_miss <= 10) want = s.last_want;
    } else {
        s.gun_miss = 0;
    }
    if (g_pic_broken.load(std::memory_order_relaxed) || now < g_soft_until.load(std::memory_order_relaxed)) want = -1;
    const int force = g_force.load(std::memory_order_relaxed);
    if (force == -1) want = -1;
    else if ((force == 0 || force == 1) && gun && axis_ok) want = force;
    if (g_bino.load(std::memory_order_relaxed)) {
        if (in.single_pass) {
            want = -1;
            g_refused_bino.fetch_add(1, std::memory_order_relaxed);
        } else {
            want = 2;
        }
    }
    bool need_swap = false;
    if (in.single_pass && (want == 0 || want == 1) && want != base_first) {
        if (g_sp_swap.load(std::memory_order_relaxed)) {
            need_swap = true;
        } else {
            want = -1;  // single pass: the first view only (research/run29/scope.md 4.3)
            g_refused_sp.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // (item 1) the measure: while the eyepiece is within 30 cm of an eye, twice a second, and at each of the player's shots
    // (want: this frame's final choice; "ahead": the eyepiece ahead of that eye along its view)
    {
        const uint64_t shots = aim::player_shots();
        const bool shot = s.shots != ~0ull && shots != s.shots;
        s.shots = shots;
        if (gun && axis_ok && (shot || (near_m < 0.30f && now - s.near_ms >= 500.0))) {
            if (!shot) s.near_ms = now;
            log::limited(shot ? "scope.shot" : "scope.near", shot ? 2000 : 20000,
                         "[scope] %s weapon %d | L dist %.3f along %.3f across %.3f angle %.1f%s%s | R dist %.3f along %.3f across %.3f angle %.1f%s%s | "
                         "want %d eye %d pending %d",
                         shot ? "at a shot:" : "near:", g.weapon, pub.dist[0], pub.along[0], pub.across[0], pub.angle[0], pub.ahead[0] ? " ahead" : "",
                         pub.zone[0] ? " in" : "", pub.dist[1], pub.along[1], pub.across[1], pub.angle[1], pub.ahead[1] ? " ahead" : "",
                         pub.zone[1] ? " in" : "", want, s.eye, pub.pending);
        }
    }
    pub.want = want;
    s.last_want = want;

    // the change: in, the eye to black (its own view), then its camera the scope's (the history discarded) and the picture
    // in; out the other way round. An eye's change is a leave first.
    if (s.eye < 0 && want >= 0) {
        s.eye = want;
        s.need_swap = need_swap;
        s.armed = false;
        s.full_noted = s.leave_noted = false;
        log::limited("scope.change", 4000, "[scope] eye %d: in, to black%s", s.eye, need_swap ? " (single pass: the eyes' order to be swapped)" : "");
    }
    if (s.eye >= 0) {
        const bool stay = want == s.eye;
        if (!stay && !s.leave_noted) {
            s.leave_noted = true;
            log::limited("scope.change", 4000, "[scope] eye %d: out, %s", s.eye, s.scoped ? "the picture fading" : "back from black");
        }
        if (stay) s.leave_noted = false;
        if (stay && !s.scoped) {
            s.p = std::min(1.0f, s.p + step);
            if (s.p >= 1.0f) {
                if (s.need_swap && !s.armed) {  // the eyes' order changes next frame: both eyes black from now
                    s.armed = true;
                    s.blink = 4;
                } else {
                    s.armed = false;
                    s.scoped = true;
                    if (s.need_swap) {
                        s.swapped = true;
                        g_swaps.fetch_add(1, std::memory_order_relaxed);
                    }
                    reset_eye(s.need_swap ? 2 : s.eye, s.swapped ? 1 - base_first : base_first);
                    g_entries.fetch_add(1, std::memory_order_relaxed);
                    log::limited("scope.change", 4000, "[scope] eye %d: the scope's view (its history discarded%s), field %.2f deg", s.eye,
                                 s.swapped ? "; the eyes' order swapped" : "", 2.0f * std::atan(t) / kDeg);
                }
            }
        } else if (stay) {
            s.armed = false;  // (the review: an exit's arm undone by coming back)
            s.q = std::min(1.0f, s.q + step);
            if (s.q >= 1.0f && !s.full_noted) {
                s.full_noted = true;
                log::limited("scope.change", 4000, "[scope] eye %d: the picture in", s.eye);
            }
        } else if (s.scoped) {
            s.q = std::max(0.0f, s.q - step);
            if (s.q <= 0.0f) {
                if (s.swapped && !s.armed) {
                    s.armed = true;
                    s.blink = 4;
                } else {
                    s.armed = false;
                    s.scoped = false;
                    const bool was = s.swapped;
                    s.swapped = false;
                    if (was) g_swaps.fetch_add(1, std::memory_order_relaxed);
                    reset_eye(was ? 2 : s.eye, base_first);
                    g_exits.fetch_add(1, std::memory_order_relaxed);
                    s.full_noted = false;
                    log::limited("scope.change", 4000, "[scope] eye %d: its own view again (its history discarded%s)", s.eye,
                                 was ? "; the eyes' order back" : "");
                }
            }
        } else {
            s.armed = false;  // (the review: an entry's arm undone by going back)
            s.p = std::max(0.0f, s.p - step);
            if (s.p <= 0.0f) {
                log::limited("scope.change", 4000, "[scope] eye %d: out", s.eye);
                s.eye = -1;
                s.need_swap = false;
            }
        }
    }
    const int first_now = s.swapped ? 1 - base_first : base_first;

    // this frame's scope cameras and pictures
    const float tan_sq[4] = {-t, t, t, -t};
    for (int e = 0; e < 2; ++e) {
        const bool mine = s.eye == 2 || s.eye == e;
        if (!mine || (in.single_pass && e != first_now)) {
            s.cam_ok[e] = false;
            s.quad_ok[e] = false;
            continue;
        }
        float cam[16];
        std::memcpy(cam, in.eye[e], sizeof(cam));
        float at[3], R[3], U[3], B[3];
        bool ok = false;
        if (s.eye == 2) {  // binoculars: each eye's own view, narrowed; the picture ahead of that eye
            for (int k = 0; k < 3; ++k) {
                R[k] = in.eye[e][k];
                U[k] = in.eye[e][4 + k];
                B[k] = in.eye[e][8 + k];
                at[k] = in.eye[e][12 + k] - (c.relief / (ws > 0.0f ? ws : 1.0f)) * B[k];
            }
            ok = true;
        } else if (gun && axis_ok) {
            for (int k = 0; k < 3; ++k) {
                cam[k] = Rw[k];
                cam[4 + k] = Uw[k];
                cam[8 + k] = Bw[k];
                cam[12 + k] = C[k];
                R[k] = Rw[k];
                U[k] = Uw[k];
                B[k] = Bw[k];
                at[k] = E[k];
            }
            ok = true;
        }
        if (ok) {
            std::memcpy(s.cam[e], cam, sizeof(cam));
            std::memcpy(s.tan[e], tan_sq, sizeof(tan_sq));
            s.cam_ok[e] = true;
            // the picture: at the eyepiece (or ImageDistance ahead of it along the axis), sized so that it spans
            // ApparentField from an eye EyeRelief behind the eyepiece
            float at2[3];
            const float ahead = c.image_dist / (ws > 0.0f ? ws : 1.0f);
            for (int k = 0; k < 3; ++k) at2[k] = at[k] - ahead * B[k];
            s.quad_ok[e] = to_local(in.cam, at2, R, U, B, s.qpos[e], s.qrot[e]);
            s.qsize[e] = 2.0f * (c.relief + c.image_dist) * std::tan(0.5f * c.apparent * kDeg);
        }
        // (no gun this frame: the last camera kept while it leaves)
    }

    Layer L;
    L.reticle = c.reticle && s.eye != 2;
    L.image = c.image;
    const float dark = g_dark.load(std::memory_order_relaxed) ? c.dark : 0.0f;
    for (int e = 0; e < 2; ++e) {
        const bool mine = s.eye == 2 || s.eye == e;
        if (mine) {
            L.eye_fade[e] = s.p;
            L.alpha[e] = s.scoped && s.cam_ok[e] && s.quad_ok[e] ? s.q : 0.0f;
            L.hide[e] = s.p > 0.0f;
        } else if (s.eye >= 0) {
            L.eye_fade[e] = dark * (s.scoped ? s.q : 0.0f);
        }
        std::memcpy(L.pos[e], s.qpos[e], sizeof(L.pos[e]));
        std::memcpy(L.rot[e], s.qrot[e], sizeof(L.rot[e]));
        L.size[e] = s.qsize[e];
    }
    if (s.blink > 0) {
        --s.blink;
        L.eye_fade[0] = L.eye_fade[1] = 1.0f;
        L.alpha[0] = L.alpha[1] = 0.0f;
        L.hide[0] = L.hide[1] = true;
    }
    publish(in.xr_frame, L, s.scoped && s.q >= 1.0f && s.p >= 1.0f && s.blink == 0 && want == s.eye);

    pub.eye = s.eye;
    pub.p = s.p;
    pub.q = s.q;
    pub.scoped = s.scoped;
    pub.swapped = s.swapped;
    pub.first = first_now;
    pub.blink = s.blink;
    for (int e = 0; e < 2; ++e) {
        std::memcpy(pub.qpos[e], s.qpos[e], sizeof(pub.qpos[e]));
        pub.qsize[e] = s.quad_ok[e] ? s.qsize[e] : 0.0f;
    }
    {
        std::lock_guard lock(g_pub_mx);
        g_pub = pub;
    }
    return s.swapped;
}

bool wanted() {
    const St& s = g_st;
    return g_on.load(std::memory_order_relaxed) || g_force.load(std::memory_order_relaxed) >= 0 || g_bino.load(std::memory_order_relaxed) ||
           s.eye >= 0 || s.swapped || s.blink > 0;
}

bool pass_camera(int eye, float cam[16], float tan[4]) {
    const St& s = g_st;
    if (eye < 0 || eye > 1 || !s.scoped || !s.cam_ok[eye] || !(s.eye == 2 || s.eye == eye)) return false;
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 3; ++k) cam[r * 4 + k] = s.cam[eye][r * 4 + k];
    std::memcpy(tan, s.tan[eye], 4 * sizeof(float));
    return true;
}

bool frame_layer(uint64_t xr_frame, Layer* out) {
    if (!g_any_pub.load(std::memory_order_relaxed)) return false;
    std::lock_guard lock(g_ring_mx);
    const Rec& r = g_ring[xr_frame % 8];
    if (r.valid && r.frame == xr_frame) {
        *out = r.L;
        g_l_exact.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    // not this frame's (a frame without a stereo pass, or the render thread a frame off): the newest one older than it, its
    // pictures left out (their images are not this frame's) and its fades kept
    const Rec* best = nullptr;
    for (const Rec& x : g_ring)
        if (x.valid && x.frame < xr_frame && xr_frame - x.frame <= 3 && (!best || x.frame > best->frame)) best = &x;
    if (!best) {
        g_l_none.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    *out = best->L;
    if (!best->steady) out->alpha[0] = out->alpha[1] = 0.0f;  // (the review: steady, the images are the scope's: kept)
    g_l_other.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void note_drawn(const bool drawn[2], const float alpha[2], bool layers, bool hard, bool soft) {
    if (drawn[0] || drawn[1]) g_drawn.fetch_add(1, std::memory_order_relaxed);
    const bool wanted_pic = alpha[0] > 0.0f || alpha[1] > 0.0f;
    if (drawn[0] || drawn[1]) {
        g_soft_miss.store(0, std::memory_order_relaxed);
    } else if (wanted_pic && soft && g_soft_miss.fetch_add(1, std::memory_order_relaxed) + 1 >= 45) {
        g_soft_miss.store(0, std::memory_order_relaxed);
        g_soft_until.store(log::now_ms() + 2000.0, std::memory_order_relaxed);
        g_soft_outs.fetch_add(1, std::memory_order_relaxed);
        log::limited("scope.soft", 50, "[scope] the picture's image not acquired or waited for 45 frames: the scope out for 2 s");
    }
    if (hard && wanted_pic && !g_pic_broken.exchange(true))
        log::error("[scope] the scope's picture cannot be made (its swapchain or its pipeline failed): the scope refused until turned on again");
    if (layers) g_submitted.fetch_add(1, std::memory_order_relaxed);
}

std::string command(const std::vector<std::string>& t) {
    auto num = [&](size_t i, float lo, float hi, float* v) {
        if (t.size() <= i) return false;
        char* end = nullptr;
        const float x = std::strtof(t[i].c_str(), &end);
        if (end == t[i].c_str() || !(x >= lo && x <= hi)) return false;
        *v = x;
        return true;
    };
    const std::string w = t.size() >= 2 ? t[1] : "status";
    if (w == "game") return camera_lever::scope_command(t);  // (the run 29 spike's reads of the game's own zoom)
    if (w == "on" || w == "off") {
        set_enabled(w == "on", false);
    } else if (w == "mag" && t.size() >= 3 && (t[2] == "realistic" || t[2] == "game")) {
        set_game_zoom(t[2] == "game", false);
    } else if (w == "other" && t.size() >= 3 && (t[2] == "open" || t[2] == "dark")) {
        set_other_dark(t[2] == "dark", false);
    } else if (w == "force" && t.size() >= 3) {
        const int f = t[2] == "auto" ? -2 : t[2] == "off" ? -1 : t[2] == "0" ? 0 : t[2] == "1" ? 1 : -9;
        if (f == -9) return "ERROR scope force <0|1|auto|off>";
        g_force = f;
    } else if (w == "binoculars" && t.size() >= 3 && (t[2] == "on" || t[2] == "off")) {
        g_bino = t[2] == "on";
    } else if (w == "spswap" && t.size() >= 3 && (t[2] == "on" || t[2] == "off")) {
        g_sp_swap = t[2] == "on";
    } else if (w == "reticle" && t.size() >= 3 && (t[2] == "on" || t[2] == "off")) {
        std::lock_guard lock(g_cfg_mx);
        g_cfg.reticle = t[2] == "on";
    } else if (w == "set" && t.size() >= 4) {  // "scope set <fade|field|power|gamepower|depth|radius|angle|hold|speed|hyst|relief|imagedist|dark> <v>"
        float v = 0.0f;
        std::lock_guard lock(g_cfg_mx);
        const std::string& k = t[2];
        if (k == "fade" && num(3, 0.0f, 1.0f, &v)) g_cfg.fade = v;
        else if (k == "field" && num(3, 5.0f, 80.0f, &v)) g_cfg.apparent = v;
        else if (k == "power" && num(3, 1.0f, 20.0f, &v)) g_cfg.power = v;
        else if (k == "gamepower" && num(3, 1.0f, 30.0f, &v)) g_cfg.game_power = v;
        else if (k == "depth" && num(3, 0.02f, 0.40f, &v)) g_cfg.depth = v;
        else if (k == "radius" && num(3, 0.005f, 0.15f, &v)) g_cfg.radius = v;
        else if (k == "angle" && num(3, 1.0f, 60.0f, &v)) g_cfg.angle = v;
        else if (k == "hold" && num(3, 0.0f, 5000.0f, &v)) g_cfg.hold_ms = v;
        else if (k == "speed" && num(3, 0.05f, 20.0f, &v)) g_cfg.max_speed = v;
        else if (k == "hyst" && num(3, 0.0f, 0.20f, &v)) g_cfg.hyst = v;
        else if (k == "relief" && num(3, 0.01f, 0.30f, &v)) g_cfg.relief = v;
        else if (k == "imagedist" && num(3, 0.0f, 50.0f, &v)) g_cfg.image_dist = v;
        else if (k == "dark" && num(3, 0.0f, 1.0f, &v)) g_cfg.dark = v;
        else return "ERROR scope set <fade|field|power|gamepower|depth|radius|angle|hold|speed|hyst|relief|imagedist|dark> <value in range>";
    } else if (w == "place" && t.size() >= 9) {  // "scope place <weapon> ex ey ez ox oy oz" (the gun's frame, metres)
        const int wp = std::atoi(t[2].c_str());
        float v[6];
        for (int i = 0; i < 6; ++i)
            if (!num(3 + i, -1.5f, 1.5f, &v[i])) return "ERROR scope place <weapon> ex ey ez ox oy oz (metres, the gun's frame)";
        std::lock_guard lock(g_place_mx);
        bool found = false;
        for (Place& p : g_places)
            if (p.weapon == wp) {
                for (int k = 0; k < 3; ++k) p.eye[k] = v[k], p.obj[k] = v[3 + k];
                found = true;
            }
        if (!found) return "ERROR scope place: not a scoped weapon (19, 20, 14)";
        log::info("[scope] place of weapon %d for the session: eyepiece (%.4f %.4f %.4f) objective (%.4f %.4f %.4f)", wp, v[0], v[1], v[2], v[3], v[4], v[5]);
    } else if (w == "reset") {
        g_pic_broken = false;
        g_soft_until = 0.0;
        g_soft_miss = 0;
        g_soft_outs = 0;
        for (auto* a : {&g_entries, &g_exits, &g_resets, &g_refused_sp, &g_refused_bino, &g_l_exact, &g_l_other, &g_l_none, &g_drawn, &g_submitted,
                        &g_swaps, &g_gap_resets})
            a->store(0);
    } else if (w != "status") {
        return "ERROR scope [status|on|off|mag realistic|game|other open|dark|force 0|1|auto|off|binoculars on|off|spswap on|off|"
               "reticle on|off|set <key> <v>|place <weapon> ex ey ez ox oy oz|reset]";
    }
    Pub p;
    {
        std::lock_guard lock(g_pub_mx);
        p = g_pub;
    }
    const Cfg c = cfg();
    uint64_t sd = 0, sf = 0, dv1 = 0, dv0 = 0;
    xr_blit::scope_counts(&sd, &sf);
    dlss::reset_counts(&dv1, &dv0);
    float mz[3] = {}, f[3] = {};
    const bool ray = aim::last_ray(mz, f);
    auto uv = [&](int e, int k) {
        static thread_local char b[2][2][40];
        if (!p.uv_ok[e][k]) std::snprintf(b[e][k], sizeof(b[e][k]), "none");
        else std::snprintf(b[e][k], sizeof(b[e][k]), "(%.4f %.4f)", p.uv[e][k][0], p.uv[e][k][1]);
        return b[e][k];
    };
    char o[2800];
    std::snprintf(o, sizeof(o),
                  "scope: on %d mag %s power %.2f field %.2f deg (apparent %.1f) other %s dark %.2f fade %.3f depth %.3f radius %.3f angle %.1f "
                  "hold %.0f speed %.2f hyst %.3f relief %.3f "
                  "imagedist %.2f image %u reticle %d spswap %d force %d binoculars %d | weapon %d gun %d eyepiece (%.4f %.4f %.4f) objective "
                  "(%.4f %.4f %.4f) muzzle %d (%.4f %.4f %.4f) | dist L %.4f R %.4f | along L %.4f R %.4f across L %.4f R %.4f angle L %.2f "
                  "R %.2f zone L %d R %d pending %d speed %.2f ahead L %d R %d | want %d eye %d p %.3f q %.3f scoped %d swapped %d blink %d "
                  "single %d first %d | entries %llu exits %llu resets %llu (taa %llu, dlss vp1 %llu vp0 %llu) refused sp %llu bino %llu gaps %llu "
                  "swaps %llu | layers exact %llu other %llu none %llu drawn %llu submitted %llu pictures %llu failed %llu broken %d soft outs %llu | L eyepiece uv %s "
                  "objective uv %s | R eyepiece uv %s objective uv %s | quad L (%.4f %.4f %.4f) size %.4f R (%.4f %.4f %.4f) size %.4f | "
                  "eyepiece world (%.4f %.4f %.4f) back (%.4f %.4f %.4f) | eye L (%.4f %.4f %.4f) fwd (%.4f %.4f %.4f) R (%.4f %.4f %.4f) fwd "
                  "(%.4f %.4f %.4f) | tan L (%.4f %.4f %.4f %.4f) R (%.4f %.4f %.4f %.4f) | ray %d dir (%.3f %.3f %.3f) muzzle (%.3f %.3f %.3f) | frames %llu xr %llu",
                  g_on.load() ? 1 : 0, g_game.load() ? "game" : "realistic", g_game.load() ? c.game_power : c.power, p.field, c.apparent,
                  g_dark.load() ? "dark" : "open", c.dark, c.fade, c.depth, c.radius, c.angle, c.hold_ms, c.max_speed, c.hyst, c.relief, c.image_dist, c.image,
                  c.reticle ? 1 : 0, g_sp_swap.load() ? 1 : 0, g_force.load(), g_bino.load() ? 1 : 0, p.weapon, p.gun ? 1 : 0, p.eye_m[0], p.eye_m[1],
                  p.eye_m[2], p.obj_m[0], p.obj_m[1], p.obj_m[2], p.mo_ok ? 1 : 0, p.mo[0], p.mo[1], p.mo[2], p.dist[0], p.dist[1], p.along[0],
                  p.along[1], p.across[0], p.across[1], p.angle[0], p.angle[1], p.zone[0] ? 1 : 0, p.zone[1] ? 1 : 0, p.pending, p.speed, p.ahead[0] ? 1 : 0, p.ahead[1] ? 1 : 0, p.want, p.eye, p.p, p.q,
                  p.scoped ? 1 : 0, p.swapped ? 1 : 0, p.blink, p.single ? 1 : 0, p.first, static_cast<unsigned long long>(g_entries.load()),
                  static_cast<unsigned long long>(g_exits.load()), static_cast<unsigned long long>(g_resets.load()),
                  static_cast<unsigned long long>(taa::requested_resets()), static_cast<unsigned long long>(dv1), static_cast<unsigned long long>(dv0),
                  static_cast<unsigned long long>(g_refused_sp.load()), static_cast<unsigned long long>(g_refused_bino.load()),
                  static_cast<unsigned long long>(g_gap_resets.load()), static_cast<unsigned long long>(g_swaps.load()),
                  static_cast<unsigned long long>(g_l_exact.load()), static_cast<unsigned long long>(g_l_other.load()),
                  static_cast<unsigned long long>(g_l_none.load()), static_cast<unsigned long long>(g_drawn.load()),
                  static_cast<unsigned long long>(g_submitted.load()), static_cast<unsigned long long>(sd), static_cast<unsigned long long>(sf),
                  g_pic_broken.load() ? 1 : 0, static_cast<unsigned long long>(g_soft_outs.load()),
                  uv(0, 0), uv(0, 1), uv(1, 0), uv(1, 1), p.qpos[0][0], p.qpos[0][1], p.qpos[0][2], p.qsize[0], p.qpos[1][0], p.qpos[1][1],
                  p.qpos[1][2], p.qsize[1], p.eyep_w[0], p.eyep_w[1], p.eyep_w[2], p.back[0], p.back[1], p.back[2], p.eyes[0][0], p.eyes[0][1], p.eyes[0][2],
                  p.fwd[0][0], p.fwd[0][1], p.fwd[0][2], p.eyes[1][0], p.eyes[1][1], p.eyes[1][2], p.fwd[1][0], p.fwd[1][1], p.fwd[1][2], p.tan[0][0], p.tan[0][1], p.tan[0][2], p.tan[0][3], p.tan[1][0],
                  p.tan[1][1], p.tan[1][2], p.tan[1][3], ray ? 1 : 0, f[0], f[1], f[2], mz[0], mz[1], mz[2],
                  static_cast<unsigned long long>(g_frames.load()), static_cast<unsigned long long>(p.xr_frame));
    return o;
}

}  // namespace rdrvr::scope
