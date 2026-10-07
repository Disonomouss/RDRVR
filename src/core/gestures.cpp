#include "core/gestures.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>

#include "core/actions.h"
#include "core/aim.h"
#include "core/anchors.h"
#include "core/api.h"
#include "core/body.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/controllers.h"
#include "core/controls.h"
#include "core/d3d_hooks.h"
#include "core/gun_melee.h"
#include "core/hands.h"
#include "core/holster.h"
#include "core/log.h"
#include "core/menu.h"
#include "core/pose.h"
#include "core/reload.h"

namespace rdrvr::gestures {
namespace {

std::atomic<bool> g_throw{false}, g_melee{false}, g_lasso{false};
std::atomic<float> g_swing_speed{2.5f};  // [Gestures] SwingSpeed: m/s for a punch, a stab or the lasso's throw
std::atomic<float> g_throw_gain{2.0f};   // [Gestures] ThrowGain: the thrown weapon's speed over the hand's
// [Gestures] FistLeadMs (run 3: with bare fists RT alone does not punch; the game wants its fighting stance, LT, first):
// a fist's swing holds LT from the swing and presses RT this many ms later (the knife and the torch strike on RT alone)
std::atomic<int> g_fist_lead_ms{40};
constexpr float kYankSpeed = 1.8f;  // m/s back against where the hand points: the lasso's yank
constexpr float kMaxSpeed = 15.0f;  // m/s: faster steps are jumps, not hand motion
constexpr int kLassoWeapon = 21, kKnife = 22, kTorch = 33;

// each controller over the last frames in the recentred local frame (x right, y up, z back: the hand's own motion,
// so walking, turning, riding and the head-bone anchor are not hand motion); its latest drawn hand (world) and the game
// camera's back row (x, z), which takes a local velocity into the world (the anchor camera is level)
struct Sample {
    double ms;
    float p[3];
};
constexpr int kSamples = 32;
Sample g_hist[2][kSamples] = {};
int g_count[2] = {0, 0}, g_head[2] = {0, 0};
float g_world[2][3] = {}, g_back[2] = {0.0f, 1.0f};
std::mutex g_mutex;  // the history: the frame end writes, the projectile launch (the game thread) reads

std::atomic<uint64_t> g_punches{0}, g_lasso_throws{0}, g_yanks{0}, g_throws{0};
double g_last_swing_ms = 0;  // the last punch's swing (frame end only): the strike probe's reference
float g_last_speed[2] = {0, 0}, g_peak_speed[2] = {0, 0};  // frame end only (status)
float g_last_throw[3] = {0, 0, 0};
double g_last_throw_age = 0;  // ms between the hand's fastest moment and the release

// The fastest velocity of a hand (local frame) within the last `window_ms` (over two-frame steps), where its drawn
// hand is now (world) and the camera's back row.
bool velocity(int h, double now, double window_ms, float v[3], float* speed, float pos[3], float* back = nullptr,
              double* age_ms = nullptr) {
    std::lock_guard lock(g_mutex);
    const int n = g_count[h];
    if (n < 3) return false;
    *speed = 0;
    v[0] = v[1] = v[2] = 0;
    const Sample& last = g_hist[h][(g_head[h] + kSamples - 1) % kSamples];
    if (now - last.ms > 100.0) return false;  // no hand lately
    if (pos) std::memcpy(pos, g_world[h], sizeof(g_world[h]));
    if (back) std::memcpy(back, g_back, sizeof(g_back));
    for (int i = 0; i + 2 < n; ++i) {
        const Sample& a = g_hist[h][(g_head[h] + kSamples - 1 - i) % kSamples];
        const Sample& b = g_hist[h][(g_head[h] + kSamples - 3 - i) % kSamples];
        if (now - a.ms > window_ms) break;
        const double dt = (a.ms - b.ms) * 0.001;
        if (dt < 1e-3) continue;
        float w[3];
        for (int k = 0; k < 3; ++k) w[k] = static_cast<float>((a.p[k] - b.p[k]) / dt);
        const float s = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        if (s > kMaxSpeed) continue;  // a jump (tracking lost and found, a teleport, the recentre), not a swing
        if (s > *speed) {
            *speed = s;
            std::memcpy(v, w, sizeof(w));
            if (age_ms) *age_ms = now - a.ms;
        }
    }
    return true;
}

// ---- run 7 item 2: the gun-butt melee ([Gestures] GunMelee; research\round13\gun-melee.md 5.2, 5.5). The drawn gun's
// strike points (a long gun's butt and its barrel's middle, from the drawn gun's own frame; a pistol's frame at the
// hand and its barrel) are followed in the recentred local frame, as the hands are above (walking, turning and the
// head-bone anchor are not swing). A point faster than GunMeleeArm arms the swing; while armed, one scan at a time
// goes to the plugin's tick (RDRVR_NATIVE_GUN_MELEE) with each point's segment since the last scan (world) and its
// velocity; a hit (or a dry one) ends the swing's scans, and the next swing arms once the points slow below 1 m/s.
// Not while the menu is open, without a gun, in Dead Eye or the game's reload, riding or driving, drawing or putting
// away (the gun hand's grip at a holster, the gun at a zone; 0.7 s after the weapon in hand changes, 0.5 s after the
// holster's grip), working the gun's action (open, a flick wanted, the off hand at a part; 0.4 s after), or in the
// two-handed grip's take (0.3 s). Out of first person (a cutscene, the cinema: no camera anchor) the swing is forgotten.
namespace gm {
constexpr float kRearm = 1.0f;  // m/s: below it the swing is over
constexpr double kDrawMs = 700.0, kHolsterMs = 500.0, kActionMs = 400.0, kTakeMs = 300.0, kReqMs = 500.0;
// the iterator's sphere (cm) about the first strike point: the actors' positions are their roots, at the feet [I], so it
// reaches them from a strike at head height
constexpr uint16_t kRadiusCm = 200;
enum Reason { kMenu, kNoGun, kDeadEye, kReload, kRiding, kHolster, kAction, kTake, kReasons };
const char* const kReasonName[kReasons] = {"menu", "no gun", "Dead Eye", "reloading", "riding", "holster", "action", "two-hand take"};
struct Point {
    Sample hist[kSamples];
    int count = 0, head = 0;
    float world[3] = {};       // now
    float last_world[3] = {};  // the frame before
    bool ok = false;
};
std::mutex mu;  // the frame end writes; the test channel reads (and adds its scans)
Point pt[2];
int npts = 0, family = -1, weapon = -1;  // family: 0 a pistol, 1 a long gun by its frame, 2 a long gun by the hand's ray
bool armed = false, struck = false, counted = false, have_prev = false;
float prev[2][3] = {};  // where each point was at the last scan: the next segment's start
uint64_t req = 0;
double req_ms = 0.0;
double weapon_ms = -1e12, holster_ms = -1e12, action_ms = -1e12, take_ms = -1e12;
bool holster_was = false, action_was = false;
float blend_was = 0.0f, speed_now = 0.0f, speed_peak = 0.0f;
uint64_t n_armed = 0, n_requests = 0, n_scans = 0, n_actors = 0, n_made = 0, n_destroyed = 0, n_none = 0, n_hits = 0, n_dry = 0, n_refused = 0,
         n_guard = 0, n_no_iter = 0, n_no_core = 0, n_timeouts = 0, n_suppressed[kReasons] = {};
char last[220] = "none";
uint64_t cost_ns = 0, cost_frames = 0;  // the frame end's work while on (the perf check: "frame-end cost")

// a point's fastest velocity (local frame) over two-frame steps within `window_ms`, as velocity() above
bool point_velocity(const Point& p, double now, double window_ms, float v[3], float* speed) {
    const int n = p.count;
    *speed = 0.0f;
    v[0] = v[1] = v[2] = 0.0f;
    if (n < 3) return false;
    const Sample& last_s = p.hist[(p.head + kSamples - 1) % kSamples];
    if (now - last_s.ms > 100.0) return false;
    for (int i = 0; i + 2 < n; ++i) {
        const Sample& a = p.hist[(p.head + kSamples - 1 - i) % kSamples];
        const Sample& b = p.hist[(p.head + kSamples - 3 - i) % kSamples];
        if (now - a.ms > window_ms) break;
        const double dt = (a.ms - b.ms) * 0.001;
        if (dt < 1e-3) continue;
        float w[3];
        for (int k = 0; k < 3; ++k) w[k] = static_cast<float>((a.p[k] - b.p[k]) / dt);
        const float s = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        if (s > kMaxSpeed) continue;  // a jump, not a swing
        if (s > *speed) {
            *speed = s;
            std::memcpy(v, w, sizeof(w));
        }
    }
    return true;
}

// a scan's result (the frame end's swing, or the test channel's scan), under mu
void note(const RdrvrNativeResult& r, bool swing, int gun_h) {
    const uint32_t victim = static_cast<uint32_t>(r.value), outcome = static_cast<uint32_t>(r.value >> 32) & 0xff,
                   code = static_cast<uint32_t>(r.value >> 40) & 0xff;
    ++n_scans;
    n_actors += (r.value >> 48) & 0xff;
    n_made += (r.value >> 56) & 0xf;
    n_destroyed += (r.value >> 60) & 0xf;
    switch (outcome) {
        case RDRVR_GUN_MELEE_NONE: ++n_none; break;
        case RDRVR_GUN_MELEE_GUARD: ++n_guard; break;
        case RDRVR_GUN_MELEE_NO_ITERATOR: ++n_no_iter; break;
        case RDRVR_GUN_MELEE_NO_CORE: ++n_no_core; break;
        case RDRVR_GUN_MELEE_HIT: {
            const bool made = code == RDRVR_MELEE_HIT_OK, dry = code == RDRVR_MELEE_HIT_DRY;
            if (made) ++n_hits;
            else if (dry) ++n_dry;
            else ++n_refused;
            if (swing && (made || dry)) {
                struck = true;
                if (gun_h >= 0) controllers::pulse(gun_h, made ? 0.8f : 0.4f, 30);
            }
            std::snprintf(last, sizeof(last), "%s 0x%x at (%.2f %.2f %.2f), the core's answer %u", made ? "hit" : dry ? "dry hit" : "refused", victim, r.vec[0],
                          r.vec[1], r.vec[2], code);
            log::info("[gunmelee] the swing's scan: %s", last);
            break;
        }
        default: break;
    }
}

// each frame end (under no lock): bp and st null out of first person
void frame(double now, const body::BodyPoints* bp, const RdrvrActorState* st) {
    const bool on = gun_melee::enabled();
    if (!on && !req && !armed && !npts) return;  // off: this check only
    std::lock_guard lock(mu);
    struct Cost {  // this call's time, under the lock
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Cost() {
            cost_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
            ++cost_frames;
        }
    } cost;
    const int gun_h = controls::gun_hand();
    if (req) {
        RdrvrNativeResult r{};
        if (api::wait_native(req, &r, 0)) {
            req = 0;
            note(r, true, gun_h);
        } else if (now - req_ms > kReqMs) {
            api::cancel_op(req);  // no script tick answered (paused, loading): dropped, so it does not run late with old segments
            req = 0;
            ++n_timeouts;
        }
    }
    if (!on || !bp || !st || !st->actor) {
        armed = struck = counted = have_prev = false;
        npts = 0;
        for (Point& p : pt) p.count = 0, p.ok = false;
        speed_now = 0.0f;
        return;
    }
    // the strike points (world)
    const gun_melee::Config cfg = gun_melee::config();
    const int w = st->weapon, gj = bp->gun;
    const bool gun = reload::is_gun(w);
    float wp[2][3] = {};
    int np = 0, fam = -1;
    if (gun && bp->hand_ok[gj] && bp->cam_ok) {
        float bt[3] = {0.0f, 0.174f, -0.985f}, u[3];
        aim::barrel_in_target(bt);  // the barrel in the IK target's axes (aim.cpp's ray), else the default
        const float* R = bp->target_rot[gj];
        for (int k = 0; k < 3; ++k) u[k] = R[k * 3] * bt[0] + R[k * 3 + 1] * bt[1] + R[k * 3 + 2] * bt[2];
        const float* hd = bp->hand[gj];
        if (reload::is_long_gun(w) && bp->gun_frame_ok) {  // the butt behind the drawn gun's origin, the barrel's middle ahead
            fam = 1;
            const float* G = bp->gun_frame_R;  // columns: x right, y up, z back
            const float butt[3] = {0.0f, -0.02f, cfg.stock}, mid[3] = {0.0f, 0.0f, -0.35f};
            for (int i = 0; i < 3; ++i) {
                wp[0][i] = bp->gun_frame_o[i] + G[i * 3] * butt[0] + G[i * 3 + 1] * butt[1] + G[i * 3 + 2] * butt[2];
                wp[1][i] = bp->gun_frame_o[i] + G[i * 3] * mid[0] + G[i * 3 + 1] * mid[1] + G[i * 3 + 2] * mid[2];
            }
        } else if (reload::is_long_gun(w)) {  // no drawn frame published: along the hand's barrel ray
            fam = 2;
            for (int i = 0; i < 3; ++i) {
                wp[0][i] = hd[i] - u[i] * (cfg.stock - 0.10f);
                wp[1][i] = hd[i] + u[i] * 0.35f;
            }
        } else {  // a pistol: its frame at the hand and its barrel
            fam = 0;
            for (int i = 0; i < 3; ++i) {
                wp[0][i] = hd[i];
                wp[1][i] = hd[i] + u[i] * 0.15f;
            }
        }
        np = 2;
    }
    if (w != weapon) {  // a draw or a put-away: the window starts, the swing is another
        weapon_ms = now;
        armed = struck = counted = false;
    }
    if (w != weapon || fam != family) {  // the points are others (a long gun's drawn frame published or not): new histories
        for (Point& p : pt) p.count = 0, p.ok = false;
        have_prev = false;
        weapon = w;
        family = fam;
    }
    for (int k = 0; k < 2; ++k) {  // each point in the recentred local frame (the hand's own motion)
        Point& p = pt[k];
        float lp[3], rel[3];
        if (k >= np || !camera_lever::world_to_local(bp->cam, wp[k], lp) || !camera_lever::local_rel(lp, rel)) {
            p.count = 0;
            p.ok = false;
            continue;
        }
        std::memcpy(p.last_world, p.ok ? p.world : wp[k], sizeof(p.last_world));
        std::memcpy(p.world, wp[k], sizeof(p.world));
        p.ok = true;
        Sample& s = p.hist[p.head];
        s.ms = now;
        std::memcpy(s.p, rel, sizeof(rel));
        p.head = (p.head + 1) % kSamples;
        if (p.count < kSamples) ++p.count;
    }
    npts = np;
    float v[2][3] = {}, s[2] = {}, speed = 0.0f;
    int fast = 0;
    bool measured = false;  // a speed read this frame (a new history needs three samples first)
    for (int k = 0; k < np; ++k) {
        if (!pt[k].ok || !point_velocity(pt[k], now, 60.0, v[k], &s[k])) continue;
        measured = true;
        if (s[k] > speed) {
            speed = s[k];
            fast = k;
        }
    }
    speed_now = speed;
    if (speed > speed_peak) speed_peak = speed;
    // the exclusion windows (their edges each frame, so the windows run whatever the speed)
    const bool cons = holster::grip_consumed(gun_h) || holster::gun_at_zone();
    if (holster_was && !cons) holster_ms = now;
    holster_was = cons;
    float part[3];
    const int hint = actions::hint(nullptr);  // 1 open, 3 a flick wanted, 0 unknown (ActionHints off) or nothing
    const bool act = (actions::fire_blocked() && (hint == 0 || hint == 1 || hint == 3)) || actions::grip_wanted(1 - gun_h) ||
                     actions::part_snap(part) > 0.0f || actions::barrel_snap() > 0.0f;
    if (action_was && !act) action_ms = now;
    action_was = act;
    const float blend = holster::two_hand_blend();
    if (blend > 0.0f && blend_was <= 0.0f) take_ms = now;
    blend_was = blend;
    int why = -1;
    if (menu::visible()) why = kMenu;
    else if (!gun || np == 0) why = kNoGun;
    else if (st->weapon_flags & RDRVR_WEAPON_DEADEYE) why = kDeadEye;
    else if (st->weapon_flags & RDRVR_WEAPON_RELOADING) why = kReload;
    else if (st->flags & (RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING)) why = kRiding;
    else if (cons || now - weapon_ms < kDrawMs || now - holster_ms < kHolsterMs) why = kHolster;
    else if (act || now - action_ms < kActionMs) why = kAction;
    else if ((blend > 0.0f && blend < 1.0f) || now - take_ms < kTakeMs) why = kTake;
    // the swing: armed past GunMeleeArm, over below 1 m/s; one hit a swing
    if ((measured || np == 0) && speed < kRearm) armed = struck = counted = false;
    if (why >= 0) {
        if (speed > cfg.arm && !counted) {
            counted = true;
            ++n_suppressed[why];
        }
        armed = have_prev = false;
        return;
    }
    if (!armed && !struck && !counted && speed > cfg.arm) {  // (a swing begun inside a window never arms)
        armed = true;
        have_prev = false;
        ++n_armed;
        log::info("[gunmelee] armed: the %s at %.1f m/s (weapon %d, %s)", fast ? "barrel" : fam == 0 ? "frame" : "butt", speed, w,
                  fam == 0 ? "a pistol" : fam == 1 ? "a long gun by its frame" : "a long gun by the hand's ray");
    }
    if (!armed || struck || req || anchors::stand_down()) return;  // (stood down: the guard's address is not this build's)
    // a scan: each point's segment since the last scan (the first: from the frame before), its velocity in the world
    // (the local one through the game camera's level rows, as throw_launch takes it)
    float bx = 0.0f, bz = 1.0f;
    camera_lever::game_heading(&bx, &bz);
    const float right[3] = {bz, 0.0f, -bx}, back[3] = {bx, 0.0f, bz};
    RdrvrGunMeleeArgs a{};
    for (int k = 0; k < np && k < 2; ++k) {
        const float* from = have_prev ? prev[k] : pt[k].last_world;
        for (int i = 0; i < 3; ++i) {
            a.seg[k][0][i] = from[i];
            a.seg[k][1][i] = pt[k].world[i];
            a.vel[k][i] = right[i] * v[k][0] + (i == 1 ? v[k][1] : 0.0f) + back[i] * v[k][2];
        }
    }
    a.guard = static_cast<uint64_t>(anchors::addr(anchors::Id::ScriptThreadCurrent));
    a.actor = static_cast<uint32_t>(st->actor);
    a.weapon = w;
    a.flags = static_cast<uint16_t>(np > 1 ? RDRVR_GUN_MELEE_POINT2 : 0u);
    a.radius_cm = kRadiusCm;
    a.speed = cfg.speed;
    uint64_t args[12];
    std::memcpy(args, &a, sizeof(args));
    req = api::queue_op(RDRVR_NATIVE_GUN_MELEE, args, 12, nullptr);
    req_ms = now;
    ++n_requests;
    for (int k = 0; k < 2; ++k) std::memcpy(prev[k], pt[k].world, sizeof(prev[k]));
    have_prev = true;
}
}  // namespace gm

void frame() {
    static double next_ok[2] = {0, 0}, lasso_out_until = 0, yank_ok = 0;
    const double now = log::now_ms();
    aim::melee_probe(g_last_swing_ms);  // the punch's strike, timed (log only)
    body::BodyPoints bp;
    RdrvrActorState st{};
    if (!pose::anchor_active() || !body::body_points(&bp) || !api::actor_state(&st)) {
        {
            std::lock_guard lock(g_mutex);
            g_count[0] = g_count[1] = 0;
        }
        gm::frame(now, nullptr, nullptr);  // [Gestures] GunMelee: the swing forgotten
        return;
    }
    float bx = 0.0f, bz = 1.0f;
    camera_lever::game_heading(&bx, &bz);
    const float right[3] = {bz, 0.0f, -bx}, back[3] = {bx, 0.0f, bz};  // the camera's rows a and c (world)
    {
        std::lock_guard lock(g_mutex);
        g_back[0] = bx;
        g_back[1] = bz;
        for (int h = 0; h < 2; ++h) {
            const int jh = bp.ctrl[0] == h ? 0 : 1;  // John's hand on the controller (body.cpp's mapping)
            const hands::Hand hd = hands::get(h);
            const float* lp = hd.grip_valid ? hd.grip_pos : hd.pos;
            Sample& s = g_hist[h][g_head[h]];
            if (!bp.hand_ok[jh] || !(hd.grip_valid || hd.valid) || !camera_lever::local_rel(lp, s.p)) {
                g_count[h] = 0;
                continue;
            }
            s.ms = now;
            std::memcpy(g_world[h], bp.hand[jh], sizeof(g_world[h]));
            g_head[h] = (g_head[h] + 1) % kSamples;
            if (g_count[h] < kSamples) ++g_count[h];
        }
    }
    gm::frame(now, &bp, &st);  // [Gestures] GunMelee (off: one flag check)
    // forward: where the head faces, in the local frame (positive yaw turns left)
    float hy = 0.0f;
    camera_lever::head_yaw_deg(&hy);
    const float fwd_l[3] = {-std::sin(hy * 0.0174532925f), 0.0f, -std::cos(hy * 0.0174532925f)};
    const bool melee = g_melee.load(std::memory_order_relaxed), lasso = g_lasso.load(std::memory_order_relaxed);
    // where the gun hand points (aim.cpp's hand ray: the IK target with the gun's barrel axis): the lasso is swung
    // that way and yanked back against it
    float point[3];  // in the local frame
    {
        float bt[3] = {0.0f, 0.174f, -0.985f}, w[3];
        aim::barrel_in_target(bt);
        const int gj = bp.gun;
        for (int k = 0; k < 3; ++k) w[k] = bp.target_rot[gj][k * 3] * bt[0] + bp.target_rot[gj][k * 3 + 1] * bt[1] + bp.target_rot[gj][k * 3 + 2] * bt[2];
        point[0] = w[0] * right[0] + w[2] * right[2];
        point[1] = w[1];
        point[2] = w[0] * back[0] + w[2] * back[2];
    }
    const int gun_h = controls::gun_hand();
    for (int h = 0; h < 2; ++h) {
        float v[3], s = 0;
        if (!velocity(h, now, 60.0, v, &s, nullptr)) continue;  // the latest step or two
        g_last_speed[h] = s;
        if (s > g_peak_speed[h]) g_peak_speed[h] = s;
        if (!melee && !lasso) continue;
        const float along = v[0] * point[0] + v[1] * point[1] + v[2] * point[2];
        // a punch or a stab: fists (nothing in hand), the knife or the torch; either hand, past the threshold and
        // forward (pulling the hand back after a punch is not another)
        const float fwd_speed = v[0] * fwd_l[0] + v[2] * fwd_l[2];
        if (melee && (st.weapon < 0 || st.weapon == kKnife || st.weapon == kTorch) && s > g_swing_speed.load(std::memory_order_relaxed) &&
            fwd_speed > 0.4f * s && now >= next_ok[h]) {
            next_ok[h] = now + 400.0;
            if (st.weapon < 0)
                controls::inject(0, 255, 0, 120, 255, g_fist_lead_ms.load(std::memory_order_relaxed));  // the fists: the stance first
            else
                controls::inject(0, 255, 0, 120);
            g_punches.fetch_add(1, std::memory_order_relaxed);
            g_last_swing_ms = now;
            log::info("[gestures] %s hand swing %.1f m/s: the attack (weapon %d)", h ? "right" : "left", s, st.weapon);
        }
        if (lasso && st.weapon == kLassoWeapon && h == gun_h) {
            // thrown by a forward swing; then a yank back steps John back on the rope for a moment
            if (s > g_swing_speed.load(std::memory_order_relaxed) && along > 0.5f * s && now >= next_ok[h]) {
                next_ok[h] = now + 800.0;
                lasso_out_until = now + 15000.0;
                controls::inject(0, 255, 0, 150);
                g_lasso_throws.fetch_add(1, std::memory_order_relaxed);
                log::info("[gestures] lasso thrown by a %.1f m/s swing", s);
            } else if (now < lasso_out_until && now > lasso_out_until - 15000.0 + 600.0 && along < -kYankSpeed && now >= yank_ok) {
                yank_ok = now + 600.0;
                controls::inject(0, 0, -32767, 350);
                g_yanks.fetch_add(1, std::memory_order_relaxed);
                log::info("[gestures] lasso yanked back (%.1f m/s)", -along);
            }
        }
    }
}

void save(const char* key, bool on) { config::set("Gestures", key, on ? "1" : "0"); }

}  // namespace

void init() {
    g_throw = config::get_bool("Gestures", "Throw", false);
    g_melee = config::get_bool("Gestures", "Melee", false);
    g_lasso = config::get_bool("Gestures", "Lasso", false);
    g_swing_speed = config::get_float("Gestures", "SwingSpeed", 2.5f);
    g_throw_gain = config::get_float("Gestures", "ThrowGain", 2.0f);
    g_fist_lead_ms = config::get_int("Gestures", "FistLeadMs", 40);
    d3d::add_frame_end_listener([](uint64_t) { frame(); });
    log::info("[gestures] throw by hand %d, melee by swing %d, lasso by hand %d (swing %.1f m/s, throw gain %.2f)", g_throw.load() ? 1 : 0,
              g_melee.load() ? 1 : 0, g_lasso.load() ? 1 : 0, g_swing_speed.load(), g_throw_gain.load());
}

bool throw_by_hand() { return g_throw.load(); }
void set_throw_by_hand(bool on) {
    if (g_throw.exchange(on) != on) log::info("[gestures] throw by hand: %s", on ? "on" : "off");
    save("Throw", on);
}
bool melee_by_swing() { return g_melee.load(); }
void set_melee_by_swing(bool on) {
    if (g_melee.exchange(on) != on) log::info("[gestures] melee by swing: %s", on ? "on" : "off");
    save("Melee", on);
}
bool lasso_by_hand() { return g_lasso.load(); }
void set_lasso_by_hand(bool on) {
    if (g_lasso.exchange(on) != on) log::info("[gestures] lasso by hand: %s", on ? "on" : "off");
    save("Lasso", on);
}

float swing_speed() { return g_swing_speed.load(); }
float throw_gain() { return g_throw_gain.load(); }
void set_tuning(float swing_speed, float throw_gain, bool save) {
    g_swing_speed = swing_speed < 1.0f ? 1.0f : swing_speed > 8.0f ? 8.0f : swing_speed;
    g_throw_gain = throw_gain < 0.5f ? 0.5f : throw_gain > 4.0f ? 4.0f : throw_gain;
    if (!save) return;
    char b[32];
    std::snprintf(b, sizeof(b), "%.2f", g_swing_speed.load());
    config::set("Gestures", "SwingSpeed", b);
    std::snprintf(b, sizeof(b), "%.2f", g_throw_gain.load());
    config::set("Gestures", "ThrowGain", b);
}

bool is_thrown(int32_t w) { return w == 23 || w == 24 || w == 25 || w == 29 || w == 32 || w == 35 || w == 36 || w == 37; }

bool throw_launch(float vel[3], float origin[3]) {
    if (!g_throw.load(std::memory_order_relaxed) || !pose::anchor_active()) return false;
    float v[3], s = 0, b[2] = {0.0f, 1.0f};
    double age = 0;
    // the game lets go some way into its throw clip after the trigger: the fastest moment of the last 0.8 s
    if (!velocity(controls::gun_hand(), log::now_ms(), 800.0, v, &s, origin, b, &age) || s < 1.5f) return false;
    g_last_throw_age = age;
    // the local velocity in the world through the camera's rows (the player's own walking is not added)
    const float right[3] = {b[1], 0.0f, -b[0]}, back[3] = {b[0], 0.0f, b[1]};
    for (int k = 0; k < 3; ++k) {
        vel[k] = (right[k] * v[0] + (k == 1 ? v[1] : 0.0f) + back[k] * v[2]) * g_throw_gain.load(std::memory_order_relaxed);
        g_last_throw[k] = vel[k];
    }
    g_throws.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, w, v;
    in >> c;
    while (in >> w >> v) {
        const bool on = v == "on";
        if (w == "throw") set_throw_by_hand(on);
        if (w == "melee") set_melee_by_swing(on);
        if (w == "lasso") set_lasso_by_hand(on);
        if (w == "peak") g_peak_speed[0] = g_peak_speed[1] = 0;  // "peak reset"
        if (w == "strike") aim::set_melee_strike(static_cast<float>(std::atof(v.c_str())));  // the session only
        if (w == "fistlead") g_fist_lead_ms = std::atoi(v.c_str());                            // the session only
    }
    char b[400];
    std::snprintf(b, sizeof(b),
                  "throw %d melee %d lasso %d | swing %.1f m/s gain %.2f strike %.2f fist lead %d ms | speed L %.2f R %.2f, peak L %.2f R %.2f | "
                  "punches %llu "
                  "lasso throws %llu yanks %llu throws %llu, last throw (%.2f %.2f %.2f) %.0f ms after the hand's peak",
                  g_throw.load() ? 1 : 0, g_melee.load() ? 1 : 0, g_lasso.load() ? 1 : 0, g_swing_speed.load(), g_throw_gain.load(),
                  aim::melee_strike(), g_fist_lead_ms.load(), g_last_speed[0],
                  g_last_speed[1], g_peak_speed[0], g_peak_speed[1], static_cast<unsigned long long>(g_punches.load()),
                  static_cast<unsigned long long>(g_lasso_throws.load()), static_cast<unsigned long long>(g_yanks.load()),
                  static_cast<unsigned long long>(g_throws.load()), g_last_throw[0], g_last_throw[1], g_last_throw[2], g_last_throw_age);
    return b;
}

namespace {
// "gunmelee scan [radius m]" (the test channel's thread): one scan-only op about the first strike point (else 1 m above
// the player's root), waited for here; the plugin logs each actor in the sphere ("[red] gunmelee scan: ...")
std::string gun_melee_scan(float radius) {
    RdrvrActorState st{};
    if (!api::actor_state(&st) || !st.actor) return "scan: no player actor";
    if (!api::plugin_attached()) return "scan: the gameplay plugin is not attached";
    if (anchors::stand_down()) return "scan: the anchors stood down (no ScriptThreadCurrent for the guard)";
    float c[3] = {st.pos[0], st.pos[1] + 1.0f, st.pos[2]};  // y is up
    const char* at = "the player";
    {
        std::lock_guard lock(gm::mu);
        if (gm::npts > 0 && gm::pt[0].ok) {
            std::memcpy(c, gm::pt[0].world, sizeof(c));
            at = "the strike point";
        }
    }
    radius = !(radius >= 0.1f) ? 0.1f : radius > 100.0f ? 100.0f : radius;
    RdrvrGunMeleeArgs a{};
    for (int i = 0; i < 3; ++i) a.seg[0][0][i] = a.seg[0][1][i] = c[i];
    a.guard = static_cast<uint64_t>(anchors::addr(anchors::Id::ScriptThreadCurrent));
    a.actor = static_cast<uint32_t>(st.actor);
    a.weapon = st.weapon;
    a.flags = RDRVR_GUN_MELEE_SCAN;
    a.radius_cm = static_cast<uint16_t>(radius * 100.0f + 0.5f);
    uint64_t args[12];
    std::memcpy(args, &a, sizeof(args));
    const uint64_t id = api::queue_op(RDRVR_NATIVE_GUN_MELEE, args, 12, nullptr);
    RdrvrNativeResult r{};
    if (!api::wait_native(id, &r, 3000)) return "scan: no script tick answered in 3 s";
    {
        std::lock_guard lock(gm::mu);
        gm::note(r, false, -1);
    }
    const uint32_t outcome = static_cast<uint32_t>(r.value >> 32) & 0xff;
    char b[300];
    std::snprintf(b, sizeof(b), "scan: outcome %u, %llu actor(s) within %.1f m of %s (%.2f %.2f %.2f), nearest 0x%x at %.2f m (%.2f %.2f %.2f); iterator made %llu destroyed %llu",
                  outcome, static_cast<unsigned long long>((r.value >> 48) & 0xff), radius, at, c[0], c[1], c[2], static_cast<uint32_t>(r.value), r.vec[3],
                  r.vec[0], r.vec[1], r.vec[2], static_cast<unsigned long long>((r.value >> 56) & 0xf), static_cast<unsigned long long>((r.value >> 60) & 0xf));
    return b;
}
}  // namespace

std::string gun_melee_command(const std::string& line) {
    std::istringstream in(line);
    std::string c, w;
    in >> c;
    std::string scan;
    while (in >> w) {
        if (w == "on" || w == "off") gun_melee::set_enabled(w == "on", false);  // the session only
        if (w == "dry") {                                                        // dry on|off: the session only
            std::string v;
            in >> v;
            gun_melee::set_dry(v != "off", false);
        }
        if (w == "reset") {
            std::lock_guard lock(gm::mu);
            gm::n_armed = gm::n_requests = gm::n_scans = gm::n_actors = gm::n_made = gm::n_destroyed = gm::n_none = gm::n_hits = gm::n_dry = 0;
            gm::n_refused = gm::n_guard = gm::n_no_iter = gm::n_no_core = gm::n_timeouts = 0;
            for (uint64_t& n : gm::n_suppressed) n = 0;
            gm::speed_peak = 0.0f;
            gm::cost_ns = gm::cost_frames = 0;
        }
        if (w == "scan") {
            float r = 5.0f;
            std::string v;
            if (in >> v) r = static_cast<float>(std::atof(v.c_str()));
            scan = gun_melee_scan(r);
        }
    }
    const gun_melee::Config cfg = gun_melee::config();
    char b[2048];
    {
        std::lock_guard lock(gm::mu);
        char sup[320] = "";
        int o = 0;
        for (int i = 0; i < gm::kReasons && o >= 0 && o < static_cast<int>(sizeof(sup)); ++i)
            o += std::snprintf(sup + o, sizeof(sup) - static_cast<size_t>(o), "%s%s %llu", i ? ", " : "", gm::kReasonName[i],
                               static_cast<unsigned long long>(gm::n_suppressed[i]));
        std::snprintf(b, sizeof(b),
                      "gunmelee %d dry %d | speed %.1f arm %.1f damage %.1f heavy %.1f lethal %d force %.2f stock %.2f | now %.2f m/s peak %.2f, armed %d struck %d, "
                      "points %d (family %d, weapon %d) | armed %llu requests %llu scans %llu actors %llu iterators made %llu destroyed %llu | none %llu hits %llu "
                      "dry %llu refused %llu guard %llu no iterator %llu no core %llu timeouts %llu | suppressed: %s | frame-end cost %.1f us/frame over %llu "
                      "frames | last: %s | %s%s%s",
                      cfg.on ? 1 : 0, cfg.dry ? 1 : 0, cfg.speed, cfg.arm, cfg.damage, cfg.heavy, cfg.lethal ? 1 : 0, cfg.force, cfg.stock, gm::speed_now,
                      gm::speed_peak, gm::armed ? 1 : 0, gm::struck ? 1 : 0, gm::npts, gm::family, gm::weapon,
                      static_cast<unsigned long long>(gm::n_armed), static_cast<unsigned long long>(gm::n_requests), static_cast<unsigned long long>(gm::n_scans),
                      static_cast<unsigned long long>(gm::n_actors), static_cast<unsigned long long>(gm::n_made), static_cast<unsigned long long>(gm::n_destroyed),
                      static_cast<unsigned long long>(gm::n_none), static_cast<unsigned long long>(gm::n_hits), static_cast<unsigned long long>(gm::n_dry),
                      static_cast<unsigned long long>(gm::n_refused), static_cast<unsigned long long>(gm::n_guard), static_cast<unsigned long long>(gm::n_no_iter),
                      static_cast<unsigned long long>(gm::n_no_core), static_cast<unsigned long long>(gm::n_timeouts), sup,
                      gm::cost_frames ? static_cast<double>(gm::cost_ns) / 1000.0 / static_cast<double>(gm::cost_frames) : 0.0,
                      static_cast<unsigned long long>(gm::cost_frames), gm::last,
                      gun_melee::status().c_str(), scan.empty() ? "" : " | ", scan.c_str());
    }
    return b;
}

}  // namespace rdrvr::gestures
