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
#include "core/dual.h"
#include "core/gun_melee.h"
#include "core/hands.h"
#include "core/holster.h"
#include "core/log.h"
#include "core/menu.h"
#include "core/offhand.h"
#include "core/pose.h"
#include "core/reload.h"
#include "core/whistle.h"
#include "core/xr.h"

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
    float turn;  // 2026-10-27 ([XR] BodyFollowsTurn): the follow's turn the recentred frame carried (camera_lever::local_rel)
};
// (the review of BodyFollowsTurn) the recentred frame turns with the body's follow (the origin's yaw), so a still hand's
// reading turns about the recentre point: up to 4 m/s of false swing 1.5 m from it. A sample is taken into the newest
// one's frame first (turned by -(the newest's turn - its own) about y, as local_rel would read it now): only the hand's
// own motion is differentiated. Off (no turn ever), the turns are equal and the sample is used as it is
void in_frame(const Sample& s, float turn_now, float out[3]) {
    if (s.turn == turn_now) {
        out[0] = s.p[0], out[1] = s.p[1], out[2] = s.p[2];
        return;
    }
    const float d = std::remainder(turn_now - s.turn, 360.0f) * 0.0174532925f, c = std::cos(d), sn = std::sin(d);
    out[0] = s.p[0] * c - s.p[2] * sn;
    out[1] = s.p[1];
    out[2] = s.p[0] * sn + s.p[2] * c;
}
constexpr int kSamples = 32;
Sample g_hist[2][kSamples] = {};
int g_count[2] = {0, 0}, g_head[2] = {0, 0};
float g_world[2][3] = {}, g_back[2] = {0.0f, 1.0f};
std::mutex g_mutex;  // the history: the frame end writes, the projectile launch (the game thread) reads

std::atomic<uint64_t> g_punches{0}, g_lasso_throws{0}, g_yanks{0}, g_throws{0}, g_punches_riding{0};
// 2026-10-12 (the user: the lasso "hard to aim and trigger the throw, not a sensitivity issue"): [Gestures] LassoThrow
// (0 swing, 1 trigger, 2 both) and LassoAim (0 the lasso hand, 1 the head, 2 the other hand's controller). A swing
// counts when the hand moves toward the aim (within 60 deg), not along the controller's own axis (an overhand or a
// sidearm throw); with the hand's aim it is frozen once the hand starts the swing (over 1 m/s), so the swing's wrist
// cannot turn it. The trigger: its press holds the game's aim (LT) kLassoLeadMs for its lock, then throws (RT); the
// controller's own RT is held back meanwhile (controls.cpp)
std::atomic<int> g_lasso_throw{1}, g_lasso_aim{1};  // the defaults the trigger and the head (the user: "work well")
std::atomic<bool> g_lasso_trig_owned{false};
std::atomic<uint64_t> g_lasso_trigger_throws{0}, g_lasso_freezes{0};
constexpr int kLassoLeadMs = 300;
// 2026-10-30 (item 12; the user: "Holding left trigger locks on to the npc you're looking at and activates the lasso
// spinning animation. and right trigger or a throwing motion throws it."): LassoThrow=lock (3). The game's own LT (the
// layout's other hand's trigger, passed as it is) is its lock and its spin; the lasso's ray along the head while it is
// in hand (whatever LassoAim says), so the lock takes the one you look at. While LT is held, its RT (the lasso hand's
// trigger, held back as for trigger) or a swing toward where you look throws: RT at once when LT has been held
// kLassoLeadMs (the lock settled), else after the rest of that lead (no LT added: the player's own, a part press too,
// stays the game's, so a caught one is not dropped as an injected LT ends). Without LT neither throws (counted). LT let
// go without a throw: nothing sent (the game puts the lasso down). A rope already on someone (the yank) as before.
constexpr int kLassoLock = 3;
std::atomic<uint64_t> g_lock_holds{0}, g_lock_rt_throws{0}, g_lock_swing_throws{0}, g_lock_no_throw{0}, g_lock_swings_unheld{0}, g_lock_rt_unheld{0},
    g_lock_rt_mouth{0}, g_lock_cancels{0};
std::atomic<bool> g_lock_lt{false};    // LT held now (lock mode, the lasso in hand)
std::atomic<double> g_lock_since{0.0};  // when that hold began
// "gestures lassoprobe on" (a test's; reads game memory each frame for 3 s after a mod throw): at the throw the game's
// lock (aim::lasso_probe's lock, soft), then the first target the lasso took in flight
std::atomic<bool> g_lasso_probe{false};
std::atomic<uint32_t> g_probe_lock{0}, g_probe_soft{0}, g_probe_target{0};
std::atomic<int> g_probe_max_state{-1};
double g_probe_until = 0;  // frame end only
bool g_lock_thrown = false;  // (frame end only) a throw made in this hold
double g_lock_rt_from = 0;   // (frame end only) a lock throw's delayed RT starts then (the lead's rest); cancelled if LT goes first
// (2026-11-03, round 73 check 4, the user: the lasso "Inconsistent, sometimes it throws, sometimes it stop before hitting the
// target"; their log: 29 throws, one catch, most sent 300 ms after LT with no lock yet; research/run17/lasso.md 4: the aim
// stance and the lock take 0.5-0.8 s) a lock throw with no lock yet waits for it: RT once the game locks someone (and LT
// has been held kLassoLeadMs), or kLassoLockWaitMs after LT without one (thrown as before); cancelled as the lead's RT is
constexpr int kLassoLockWaitMs = 1000;  // (cycle N28: the lock came 689 ms after LT; one throw waited 800 ms without it)
double g_lock_wait_until = 0;  // (frame end only) a held lock throw's last moment, 0: none held
const char* g_lock_wait_by = "";
std::atomic<uint64_t> g_lock_waits{0}, g_lock_waits_locked{0};
// (the re-review) a lock throw's RT still waiting for its lead: taken back (LT let go, the lasso put away, out of first
// person); the yank disarmed with it, the next press not held off (next_ok, if given). True if it was still pending.
// ("thrown by RT / by a swing" count the accepted presses, a cancelled one too)
bool cancel_lock_throw(double* lasso_out_until, double* next_ok, const char* why) {
    if (g_lock_thrown && g_lock_wait_until > 0) {  // (held for the lock: nothing sent yet)
        g_lock_wait_until = 0;
        g_lock_thrown = false;
        *lasso_out_until = 0;
        if (next_ok) *next_ok = 0;
        g_probe_until = 0;
        g_probe_max_state.store(-1, std::memory_order_relaxed);
        g_lock_cancels.fetch_add(1, std::memory_order_relaxed);
        log::info("[gestures] lasso: %s while the throw waited for the game's lock (nothing thrown)", why);
        return true;
    }
    if (!g_lock_thrown || g_lock_rt_from <= 0) return false;
    g_lock_rt_from = 0;
    if (!controls::cancel_pending_rt()) return false;  // (its RT already went out: thrown)
    g_lock_thrown = false;
    *lasso_out_until = 0;
    if (next_ok) *next_ok = 0;
    g_probe_until = 0;
    g_probe_max_state.store(-1, std::memory_order_relaxed);  // ("at the last throw": a throw that never went out)
    g_lock_cancels.fetch_add(1, std::memory_order_relaxed);
    log::info("[gestures] lasso: %s inside the lead: the throw's RT cancelled (nothing thrown)", why);
    return true;
}
int lasso_aim_mode() { return g_lasso_throw.load(std::memory_order_relaxed) == kLassoLock ? 1 : g_lasso_aim.load(std::memory_order_relaxed); }
float g_lasso_frozen[3] = {0, 0, -1}, g_lasso_off_dir[3] = {0, 0, -1};  // world; under g_mutex
double g_lasso_frozen_until = 0;  // under g_mutex
// (2026-11-01, GOAL-offhand-changes C11; round 64, the user: "Lock on does not seem to be working, aim follows head even if
// I am holding LT") lock mode: once the game holds a lock with LT held, the lasso's ray kept where the head looked then
// (world; under g_mutex), not along the head after: the head's ray moved the game's lock off its target
float g_lock_dir[3] = {0, 0, -1};
bool g_lock_dir_on = false;  // under g_mutex
bool g_lasso_off_ok = false;      // under g_mutex
double g_last_swing_ms = 0;  // the last punch's swing (frame end only): the strike probe's reference
// run 10 item 7 ([Gestures] Guard; the user: "with fists equipped, holding hands in front of face in a guarding pose"):
// both hands in a box in front of the face (the head's frame: GuardBox forward min max, up min max, across), held 150 ms:
// the game's block held (GuardButtons: rb+lt by default, the fighting stance with MELEE_BLOCK, pad RB in FUN_14000c0b0's
// table: research\run10\inputs-greet-block.md), let go 200 ms after the pose is left; fists only, never riding,
// driving or in the menu, and not within 0.5 s of a punch (one injection slot: the punch's own)
std::atomic<bool> g_guard{false}, g_guard_up{false};
std::atomic<bool> g_guard_probe{false};  // "gestures guardprobe on": the block state read each guarded frame (a test's; reads game memory)
std::atomic<int> g_guard_buttons{3};  // bit 0 RB, bit 1 LT
// 2026-10-12 (the user: "raising fists enters melee to guard and scares npcs"; "unarmed doesn't become melee unless
// both triggers are held"): [Gestures] GuardTriggers, the guard's pose only with both controllers' triggers held too
// (the fists' selection and the game's fighting stance with it); 0: the hands in the box alone, as before
std::atomic<bool> g_guard_triggers{true};
std::atomic<bool> g_guard_pose{false};  // the guard's pose this frame (allowed; before its 150 ms are up too)
std::atomic<uint64_t> g_guards{0}, g_guard_frames{0}, g_guard_block_frames{0}, g_guard_fists{0};
float g_gbox[5] = {0.05f, 0.40f, -0.25f, 0.10f, 0.25f};  // forward min, max; up min, max; across (|right|) max; under g_mutex
float g_guard_hand[2][3] = {};  // each hand in the head's frame (right, up, forward) at the last frame; under g_mutex
void qrot(const float q[4], const float v[3], float out[3]) {  // q (x y z w) applied to v
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2.0f * (y * v[2] - z * v[1]), ty = 2.0f * (z * v[0] - x * v[2]), tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}
void guard_frame(double now, const RdrvrActorState& st) {
    static double in_since = -1.0, out_since = -1.0;
    // (round 125) never while a minigame runs or the game holds the player's control (its LT and RB would reach a table game:
    // RB is poker's skip to your turn)
    const bool allowed = g_guard.load(std::memory_order_relaxed) && st.weapon < 0 && !(st.flags & (RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING)) &&
                         !menu::visible() && !controls::game_menu_open() && now - g_last_swing_ms > 500.0 && !(holster::game_gate() & 3);
    static double up_since = 0.0;
    bool pose = false;
    xr::EyeView v[2];
    if (allowed && xr::eye_views_peek(v)) {
        float box[5];
        {
            std::lock_guard lock(g_mutex);
            std::memcpy(box, g_gbox, sizeof(box));
        }
        const float e[3] = {0.5f * (v[0].position[0] + v[1].position[0]), 0.5f * (v[0].position[1] + v[1].position[1]),
                            0.5f * (v[0].position[2] + v[1].position[2])};
        const float qc[4] = {-v[0].orientation[0], -v[0].orientation[1], -v[0].orientation[2], v[0].orientation[3]};  // the head's inverse
        pose = true;
        for (int h = 0; h < 2; ++h) {
            const hands::Hand hd = hands::get(h);
            if (!(hd.grip_valid || hd.valid)) {
                pose = false;
                continue;
            }
            const float* p = hd.grip_valid ? hd.grip_pos : hd.pos;
            const float d[3] = {p[0] - e[0], p[1] - e[1], p[2] - e[2]};
            float l[3];
            qrot(qc, d, l);  // (right, up, back) in the head's frame
            const float rf[3] = {l[0], l[1], -l[2]};
            {
                std::lock_guard lock(g_mutex);
                std::memcpy(g_guard_hand[h], rf, sizeof(rf));
            }
            if (!(rf[2] >= box[0] && rf[2] <= box[1] && rf[1] >= box[2] && rf[1] <= box[3] && std::fabs(rf[0]) <= box[4])) pose = false;
            if (g_guard_triggers.load(std::memory_order_relaxed) && hd.trigger < 0.5f) pose = false;  // GuardTriggers: held too
        }
    }
    g_guard_pose.store(allowed && pose, std::memory_order_relaxed);
    bool up = g_guard_up.load(std::memory_order_relaxed);
    if (!allowed) {
        in_since = out_since = -1.0;
        if (up) log::info("[gestures] guard down (%s)", st.weapon >= 0 ? "a weapon in hand" : "not allowed now");
        up = false;
    } else if (pose) {
        out_since = -1.0;
        if (in_since < 0.0) {
            in_since = now;
            // 2026-10-12 (reg2, cycle H5): with a gun still selected (put away by the game or a script, not at a holster by
            // hand: no fists selected) the guard's LT drew that gun and aimed it. The fists selected as the hands come
            // into the box (SET_PLAYER_MELEE_MODE_SELECTED, as UnarmedAfterHolster; a script tick before the 150 ms the
            // guard waits), at most every 2 s; any draw clears it
            static double sel_ms = -1e9;
            if (now - sel_ms > 2000.0) {
                const uint64_t m[2] = {1, 1};
                api::queue_native(0xAC1285A3, m, 2, 0, nullptr);  // SET_PLAYER_MELEE_MODE_SELECTED(1): the wheel's Unarmed
                sel_ms = now;
                g_guard_fists.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (!up && now - in_since >= 150.0) {
            up = true;
            up_since = now;
            g_guards.fetch_add(1, std::memory_order_relaxed);
            log::info("[gestures] guard up: both fists before the face (the game's block held)");
        }
    } else {
        in_since = -1.0;
        if (out_since < 0.0) out_since = now;
        if (up && now - out_since >= 200.0) {
            up = false;
            log::info("[gestures] guard down");
        }
    }
    g_guard_up.store(up, std::memory_order_relaxed);
    if (up) {
        g_guard_frames.fetch_add(1, std::memory_order_relaxed);
        if (g_guard_probe.load(std::memory_order_relaxed) && aim::player_melee_state() == 2) g_guard_block_frames.fetch_add(1, std::memory_order_relaxed);
        const int b = g_guard_buttons.load(std::memory_order_relaxed);
        // the fighting stance (LT) first, RB after the punch's own lead (RB alone is the game's cover: the review's)
        const bool lead = (b & 2) && now - up_since < static_cast<double>(g_fist_lead_ms.load(std::memory_order_relaxed) + 60);
        controls::inject((b & 1) && !lead ? 0x0200 : 0, 0, 0, 120, (b & 2) ? 255 : 0);  // 0x0200: XINPUT_GAMEPAD_RIGHT_SHOULDER (RB)
    }
}
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
        float w[3], ap[3], bpp[3];
        in_frame(a, last.turn, ap);  // ([XR] BodyFollowsTurn: both in the newest sample's frame)
        in_frame(b, last.turn, bpp);
        for (int k = 0; k < 3; ++k) w[k] = static_cast<float>((ap[k] - bpp[k]) / dt);
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
// velocity; a hit (or a dry one) ends the swing's scans, and the next swing arms once the points slow below 0.8 m/s.
// A hit needs the swing's peak since it armed at GunMeleeSpeed or more and the point moving into the bone at half of it
// (GunMeleeByPeak; run 8 item 1: people slow down into a virtual contact), else the speed at contact.
// Not while the menu is open, without a gun, in Dead Eye or the game's reload, riding or driving, drawing or putting
// away (the gun hand's grip at a holster, the gun at a zone with that hand's grip pressed; 0.7 s after the weapon in
// hand changes, 0.25 s after the holster's grip), working the gun's action (open, a flick wanted, the off hand at a part; 0.4 s after), or in the
// two-handed grip's take (0.3 s). Out of first person (a cutscene, the cinema: no camera anchor) the swing is forgotten.
namespace gm {
constexpr float kRearm = 0.8f;  // m/s: below it the swing is over (run 8: 1.0, above the slow end of a real swing)
constexpr double kDrawMs = 700.0, kHolsterMs = 250.0, kActionMs = 400.0, kTakeMs = 300.0, kReqMs = 500.0;
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
// the armed swing's peak, its fast part and its scans (the "swing over" line); the last suppression line's time
float swing_peak = 0.0f, swing_near = -1.0f, swing_near_speed = 0.0f;  // near: the nearest a scan came to a bone (m; -1 none)
int swing_seen = 0, swing_fit = 0;  // (round 73) the most actors a scan of the swing found within kRadiusCm, and of them to hit
bool over_pending = false;  // a swing over without a hit, its line kept until its last scan has answered (the review's finding)
float over_peak = 0.0f;
int over_part = 0, over_scans = 0, over_family = 0;
int swing_part = 0, swing_scans = 0;
double suppressed_log_ms = -1e12;
uint64_t n_swings_missed = 0;
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
        float w[3], ap[3], bpp[3];
        in_frame(a, last_s.turn, ap);  // ([XR] BodyFollowsTurn: both in the newest sample's frame)
        in_frame(b, last_s.turn, bpp);
        for (int k = 0; k < 3; ++k) w[k] = static_cast<float>((ap[k] - bpp[k]) / dt);
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
        case RDRVR_GUN_MELEE_NONE:
            ++n_none;
            if (swing && r.vec[3] >= 0.0f && (swing_near < 0.0f || r.vec[3] < swing_near)) {
                swing_near = r.vec[3];
                swing_near_speed = r.vec[0];
            }
            if (swing) {
                const int sn = static_cast<int>((r.value >> 48) & 0xff), fn = std::isfinite(r.vec[2]) ? static_cast<int>(r.vec[2]) : 0;
                if (sn > swing_seen) swing_seen = sn;
                if (fn > swing_fit) swing_fit = fn;
            }
            break;
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
        swing_peak = 0.0f;
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
        float rel[3], turn = 0.0f;  // (one origin for both steps: camera_lever::world_to_local_rel)
        if (k >= np || !camera_lever::world_to_local_rel(bp->cam, wp[k], rel, &turn)) {
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
        s.turn = turn;
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
    // ([Holsters] HoldToKeep: the gun hand's grip held as the gun's hold away from the holsters is no holster's grip)
    // (2026-11-05, round 87 check 1: "Works but I had trouble using it a few times": 34 of the user's swings refused as
    // "holster", the gun held through a holster's zone on its way back) with HoldToKeep a zone with the grip held is no
    // put-away (the let-go is): the zone counts only without it
    const bool cons = (holster::grip_consumed(gun_h) && !holster::grip_keeps(gun_h)) ||
                      (!holster::hold_to_keep() && holster::gun_at_zone() && hands::get(gun_h).grip > 0.5f);
    if (holster_was && !cons) holster_ms = now;
    holster_was = cons;
    float part[3];
    const int hint = actions::hint(nullptr);  // 1 open, 3 a flick wanted, 5 shut it (step 7), 0 unknown (ActionHints off) or nothing
    const bool act = (actions::fire_blocked() && (hint == 0 || hint == 1 || hint == 3 || hint == 5)) || actions::grip_wanted(1 - gun_h) ||
                     actions::part_snap(part) > 0.0f || actions::carried_part_snap() > 0.0f || actions::barrel_snap() > 0.0f;
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
    // the swing: armed past GunMeleeArm, over below 0.8 m/s; one hit a swing
    if (armed && speed > swing_peak) {
        swing_peak = speed;
        swing_part = fast;
    }
    if ((measured || np == 0) && speed < kRearm) {
        if (armed && !struck) {  // a swing over without a hit: its line once its last scan has answered
            over_pending = true;
            over_peak = swing_peak;
            over_part = swing_part;
            over_scans = swing_scans;
            over_family = family;
        }
        armed = struck = counted = false;
    }
    if (over_pending && !req) {  // the swing's numbers, so a missed hit says why (a hit by its last scan: no line)
        over_pending = false;
        if (!struck) {
            ++n_swings_missed;
            char nr[160];
            std::snprintf(nr, sizeof(nr), "no one near (actors within %.0f m: %d, to hit %d)", kRadiusCm * 0.01f, swing_seen, swing_fit);
            if (swing_near >= 0.0f) std::snprintf(nr, sizeof(nr), "the nearest bone missed by %.2f m at %.1f m/s", swing_near, swing_near_speed);
            log::info("[gunmelee] swing over: peak %.1f m/s (the %s), %d scan%s, no hit, %s (a hit needs a bone entered %s %.1f m/s)", over_peak,
                      over_part ? "barrel" : over_family == 0 ? "frame" : "butt", over_scans, over_scans == 1 ? "" : "s", nr,
                      cfg.by_peak ? "after a peak of" : "at", cfg.speed);
        }
    }
    if (why >= 0) {
        if (speed > cfg.arm && !counted) {
            counted = true;
            ++n_suppressed[why];
            if (now - suppressed_log_ms > 1000.0) {
                suppressed_log_ms = now;
                log::info("[gunmelee] a swing at %.1f m/s not armed: %s (weapon %d)", speed, kReasonName[why], w);
            }
        }
        armed = have_prev = false;
        return;
    }
    if (!armed && !struck && !counted && speed > cfg.arm) {  // (a swing begun inside a window never arms)
        armed = true;
        have_prev = false;
        swing_peak = speed;
        swing_part = fast;
        swing_scans = 0;
        swing_near = -1.0f;
        swing_seen = swing_fit = 0;
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
    // by the peak: once the swing has reached the hit speed, the contact may come as it slows (half of it, into the bone)
    a.speed = cfg.by_peak && swing_peak >= cfg.speed ? 0.5f * cfg.speed : cfg.speed;
    ++swing_scans;
    uint64_t args[12];
    std::memcpy(args, &a, sizeof(args));
    req = api::queue_op(RDRVR_NATIVE_GUN_MELEE, args, 12, nullptr);
    req_ms = now;
    ++n_requests;
    for (int k = 0; k < 2; ++k) std::memcpy(prev[k], pt[k].world, sizeof(prev[k]));
    have_prev = true;
}
}  // namespace gm

// ---- [Hands] OffHandItems (offhand.h): the off hand's knife (or torch), a stab or a slash. Its two points (the hilt at
// the drawn hand; the knife's tip kBlade, the torch's head kTorchHead, along the controller's pointing) followed in the
// recentred local frame as the gun melee's strike points (gm above: the same history, the same velocity), the same rule
// for a hit (armed past GunMeleeArm, a hit at GunMeleeSpeed, by the swing's peak as GunMeleeByPeak says) and the same scan
// (RDRVR_NATIVE_GUN_MELEE, its weapon the item's: gun_melee::hit takes it for the off hand's own); a slow move never arms.
// GunMelee itself may be off. Not while the menu is open, riding or driving, in Dead Eye. Out of first person, or the item
// put back, it is forgotten.
namespace ohs {
constexpr float kBlade = 0.22f;      // m: the tip from the grip [I] (the game's knife model's blade; not measured)
constexpr float kTorchHead = 0.45f;  // m: the torch's burning head from the grip [I] (not measured)
std::mutex mu;
gm::Point pt[2];
bool armed = false, struck = false, have_prev = false;
int weapon = -1, hand = -1;
float prev[2][3] = {}, peak = 0.0f, speed_now = 0.0f;
uint64_t req = 0;
double req_ms = 0.0;
uint64_t n_armed = 0, n_scans = 0, n_hits = 0, n_dry = 0, n_refused = 0, n_none = 0, n_timeouts = 0, n_slow = 0;
char last[200] = "none";

void reset() {
    armed = struck = have_prev = false;
    peak = 0.0f;
    for (gm::Point& p : pt) p.count = 0, p.ok = false;
}
void frame(double now, const body::BodyPoints* bp, const RdrvrActorState* st) {
    int h = -1, jh = -1, w = -1;
    const bool on = offhand::strike_hand(&h, &jh, &w);
    if (!on && !req && !armed && weapon < 0) return;  // off: this check only
    std::lock_guard lock(mu);
    if (req) {
        RdrvrNativeResult r{};
        if (api::wait_native(req, &r, 0)) {
            req = 0;
            ++n_scans;
            const uint32_t outcome = static_cast<uint32_t>(r.value >> 32) & 0xff, code = static_cast<uint32_t>(r.value >> 40) & 0xff;
            if (outcome == RDRVR_GUN_MELEE_HIT) {
                const bool made = code == RDRVR_MELEE_HIT_OK, dry = code == RDRVR_MELEE_HIT_DRY;
                made ? ++n_hits : dry ? ++n_dry : ++n_refused;
                if (made || dry) {
                    struck = true;
                    if (hand >= 0) controllers::pulse(hand, made ? 0.8f : 0.4f, 30);
                }
                std::snprintf(last, sizeof(last), "%s 0x%x at (%.2f %.2f %.2f), the core's answer %u", made ? "hit" : dry ? "dry hit" : "refused",
                              static_cast<uint32_t>(r.value), r.vec[0], r.vec[1], r.vec[2], code);
                log::info("[offhand] the stab's scan: %s", last);
            } else if (outcome == RDRVR_GUN_MELEE_NONE) {
                ++n_none;
            }
        } else if (now - req_ms > gm::kReqMs) {
            api::cancel_op(req);
            req = 0;
            ++n_timeouts;
        }
    }
    if (!on || !bp || !st || !st->actor || w != weapon || h != hand) {
        if (req && (!on || w != weapon)) {  // (the review) the item put back or another: its scan still queued is dropped
            api::cancel_op(req);
            req = 0;
        }
        reset();
        weapon = on ? w : -1;
        hand = on ? h : -1;
        speed_now = 0.0f;
        if (!on || !bp || !st || !st->actor) return;
    }
    if (jh < 0 || jh > 1 || !bp->hand_ok[jh] || !bp->cam_ok) {
        reset();
        return;
    }
    const float* R = bp->target_rot[jh];
    const float bt[3] = {0.0f, 0.174f, -0.985f};
    float u[3], wp[2][3];
    for (int k = 0; k < 3; ++k) u[k] = R[k * 3] * bt[0] + R[k * 3 + 1] * bt[1] + R[k * 3 + 2] * bt[2];
    for (int k = 0; k < 3; ++k) {
        wp[0][k] = bp->hand[jh][k];
        wp[1][k] = bp->hand[jh][k] + u[k] * (w == 33 ? kTorchHead : kBlade);
    }
    for (int k = 0; k < 2; ++k) {
        gm::Point& p = pt[k];
        float rel[3], turn = 0.0f;
        if (!camera_lever::world_to_local_rel(bp->cam, wp[k], rel, &turn)) {
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
        s.turn = turn;
        p.head = (p.head + 1) % kSamples;
        if (p.count < kSamples) ++p.count;
    }
    float v[2][3] = {}, sp[2] = {}, speed = 0.0f;
    bool measured = false;
    for (int k = 0; k < 2; ++k) {
        if (!pt[k].ok || !gm::point_velocity(pt[k], now, 60.0, v[k], &sp[k])) continue;
        measured = true;
        if (sp[k] > speed) speed = sp[k];
    }
    speed_now = speed;
    const gun_melee::Config cfg = gun_melee::config();
    if (armed && speed > peak) peak = speed;
    if (measured && speed < gm::kRearm) {
        if (armed && !struck) log::info("[offhand] the stab over: peak %.1f m/s, no hit", peak);
        armed = struck = false;
    }
    const bool blocked = menu::visible() || (st->flags & (RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING)) || (st->weapon_flags & RDRVR_WEAPON_DEADEYE);
    if (blocked) {
        armed = have_prev = false;
        return;
    }
    if (!armed && !struck && speed > cfg.arm) {
        armed = true;
        have_prev = false;
        peak = speed;
        ++n_armed;
        log::info("[offhand] the %s armed at %.1f m/s (the %s hand)", w == 33 ? "torch" : "knife", speed, h ? "right" : "left");
    }
    if (!armed) {
        if (measured && speed > 0.3f && speed <= cfg.arm) ++n_slow;  // (a slow move: no scan; the status counts the frames)
        return;
    }
    if (struck || req || anchors::stand_down()) return;
    float bx = 0.0f, bz = 1.0f;
    camera_lever::game_heading(&bx, &bz);
    const float right[3] = {bz, 0.0f, -bx}, back[3] = {bx, 0.0f, bz};
    RdrvrGunMeleeArgs a{};
    for (int k = 0; k < 2; ++k) {
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
    a.flags = static_cast<uint16_t>(RDRVR_GUN_MELEE_POINT2 | RDRVR_GUN_MELEE_NONHUMAN);  // (step 5: zombies too)
    a.radius_cm = gm::kRadiusCm;
    a.speed = cfg.by_peak && peak >= cfg.speed ? 0.5f * cfg.speed : cfg.speed;
    uint64_t args[12];
    std::memcpy(args, &a, sizeof(args));
    req = api::queue_op(RDRVR_NATIVE_GUN_MELEE, args, 12, nullptr);
    req_ms = now;
    for (int k = 0; k < 2; ++k) std::memcpy(prev[k], pt[k].world, sizeof(prev[k]));
    have_prev = true;
}
}  // namespace ohs

// ---- (2026-11-05, round 88: "Hits don't register with a dual wielded copy gun"; "melee for right handed side arm and long
// guns felt good") the second gun in the other hand ([Hands] DualWield, the copy too): its own swing, as gm's pistol (its
// frame at the drawn hand, its barrel 0.15 m ahead; a long gun, the Sawed-off copy: its butt and barrel along the hand's
// ray, as gm's family 2), the same rule for a hit and the same scan. Not while the menu is open, in Dead Eye or the
// game's reload, riding or driving, in the draw's 0.7 s, with its controller's grip at a holster (as gm: HoldToKeep's
// own hold excepted), or while it is open or its rounds are in the other hand (0.4 s after: the review, the flick that
// shuts it). GunMelee on; out of first person, or the second gun put back, it is forgotten.
namespace dgs {
std::mutex mu;
gm::Point pt[2];
bool armed = false, struck = false, counted = false, have_prev = false;
int weapon = -1, ctrl = -1;
float prev[2][3] = {}, peak = 0.0f, speed_now = 0.0f;
uint64_t req = 0;
double req_ms = 0.0, ready_ms = -1e12, action_ms = -1e12;
bool action_was = false;
uint64_t n_armed = 0, n_scans = 0, n_hits = 0, n_dry = 0, n_refused = 0, n_none = 0, n_timeouts = 0, n_held_off = 0;
char last[200] = "none";

void reset() {
    armed = struck = counted = have_prev = false;
    peak = 0.0f;
    for (gm::Point& p : pt) p.count = 0, p.ok = false;
}
void frame(double now, const body::BodyPoints* bp, const RdrvrActorState* st) {
    const dual::State ds = gun_melee::enabled() && bp && st ? dual::state() : dual::State{};
    const bool on = ds.on && ds.ready && !ds.carry && ds.john >= 0 && ds.john <= 1 && ds.ctrl >= 0 && ds.ctrl <= 1 && reload::is_gun(ds.weapon) &&
                    st->actor;
    if (!on && !req && !armed && weapon < 0) return;  // none out: this check only
    std::lock_guard lock(mu);
    if (req) {
        RdrvrNativeResult r{};
        if (api::wait_native(req, &r, 0)) {
            req = 0;
            ++n_scans;
            const uint32_t outcome = static_cast<uint32_t>(r.value >> 32) & 0xff, code = static_cast<uint32_t>(r.value >> 40) & 0xff;
            if (outcome == RDRVR_GUN_MELEE_HIT) {
                const bool made = code == RDRVR_MELEE_HIT_OK, dry = code == RDRVR_MELEE_HIT_DRY;
                made ? ++n_hits : dry ? ++n_dry : ++n_refused;
                if (made || dry) {
                    struck = true;
                    if (ctrl >= 0) controllers::pulse(ctrl, made ? 0.8f : 0.4f, 30);
                }
                std::snprintf(last, sizeof(last), "%s 0x%x at (%.2f %.2f %.2f), the core's answer %u", made ? "hit" : dry ? "dry hit" : "refused",
                              static_cast<uint32_t>(r.value), r.vec[0], r.vec[1], r.vec[2], code);
                log::info("[gunmelee] the second gun's swing: %s", last);
            } else if (outcome == RDRVR_GUN_MELEE_NONE) {
                ++n_none;
            }
        } else if (now - req_ms > gm::kReqMs) {
            api::cancel_op(req);
            req = 0;
            ++n_timeouts;
        }
    }
    if (!on || ds.weapon != weapon || ds.ctrl != ctrl) {
        if (req) {  // (as ohs) put back or another: its scan still queued is dropped
            api::cancel_op(req);
            req = 0;
        }
        reset();
        weapon = on ? ds.weapon : -1;
        ctrl = on ? ds.ctrl : -1;
        ready_ms = now;  // (a draw: its window)
        speed_now = 0.0f;
        if (!on) return;
    }
    const int jh = ds.john;
    if (!bp->hand_ok[jh] || !bp->cam_ok) {
        reset();
        return;
    }
    const float* R = bp->target_rot[jh];
    const float bt[3] = {0.0f, 0.174f, -0.985f};
    float u[3], wp[2][3];
    for (int k = 0; k < 3; ++k) u[k] = R[k * 3] * bt[0] + R[k * 3 + 1] * bt[1] + R[k * 3 + 2] * bt[2];
    const bool long_gun = reload::is_long_gun(weapon);
    const float stock = gun_melee::config().stock;
    for (int k = 0; k < 3; ++k) {
        wp[0][k] = long_gun ? bp->hand[jh][k] - u[k] * (stock - 0.10f) : bp->hand[jh][k];
        wp[1][k] = bp->hand[jh][k] + u[k] * (long_gun ? 0.35f : 0.15f);
    }
    for (int k = 0; k < 2; ++k) {
        gm::Point& p = pt[k];
        float rel[3], turn = 0.0f;
        if (!camera_lever::world_to_local_rel(bp->cam, wp[k], rel, &turn)) {
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
        s.turn = turn;
        p.head = (p.head + 1) % kSamples;
        if (p.count < kSamples) ++p.count;
    }
    float v[2][3] = {}, sp[2] = {}, speed = 0.0f;
    bool measured = false;
    for (int k = 0; k < 2; ++k) {
        if (!pt[k].ok || !gm::point_velocity(pt[k], now, 60.0, v[k], &sp[k])) continue;
        measured = true;
        if (sp[k] > speed) speed = sp[k];
    }
    speed_now = speed;
    const gun_melee::Config cfg = gun_melee::config();
    if (armed && speed > peak) peak = speed;
    if (measured && speed < gm::kRearm) {
        if (armed && !struck) log::info("[gunmelee] the second gun's swing over: peak %.1f m/s, no hit", peak);
        armed = struck = counted = false;
    }
    // (its controller's grip at a holster: drawing or putting it back; HoldToKeep's own hold excepted, as gm)
    const bool held = holster::grip_consumed(ctrl) && !holster::grip_keeps(ctrl);
    const bool act = actions::second_open() || holster::rounds_for_second();
    if (action_was && !act) action_ms = now;
    action_was = act;
    const bool blocked = menu::visible() || (st->flags & (RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING)) ||
                         (st->weapon_flags & (RDRVR_WEAPON_DEADEYE | RDRVR_WEAPON_RELOADING)) || now - ready_ms < gm::kDrawMs || held || act ||
                         now - action_ms < gm::kActionMs;
    if (blocked) {
        if (speed > cfg.arm && !counted) counted = true, ++n_held_off;
        armed = have_prev = false;
        return;
    }
    if (!armed && !struck && !counted && speed > cfg.arm) {
        armed = true;
        have_prev = false;
        peak = speed;
        ++n_armed;
        log::info("[gunmelee] the second gun's swing armed at %.1f m/s (weapon %d, %s, John's %s hand)", speed, weapon, long_gun ? "by the hand's ray" : "a pistol",
                  jh ? "right" : "left");
    }
    if (!armed || struck || req || anchors::stand_down()) return;
    float bx = 0.0f, bz = 1.0f;
    camera_lever::game_heading(&bx, &bz);
    const float right[3] = {bz, 0.0f, -bx}, back[3] = {bx, 0.0f, bz};
    RdrvrGunMeleeArgs a{};
    for (int k = 0; k < 2; ++k) {
        const float* from = have_prev ? prev[k] : pt[k].last_world;
        for (int i = 0; i < 3; ++i) {
            a.seg[k][0][i] = from[i];
            a.seg[k][1][i] = pt[k].world[i];
            a.vel[k][i] = right[i] * v[k][0] + (i == 1 ? v[k][1] : 0.0f) + back[i] * v[k][2];
        }
    }
    a.guard = static_cast<uint64_t>(anchors::addr(anchors::Id::ScriptThreadCurrent));
    a.actor = static_cast<uint32_t>(st->actor);
    a.weapon = weapon;
    a.flags = static_cast<uint16_t>(RDRVR_GUN_MELEE_POINT2);
    a.radius_cm = gm::kRadiusCm;
    a.speed = cfg.by_peak && peak >= cfg.speed ? 0.5f * cfg.speed : cfg.speed;
    uint64_t args[12];
    std::memcpy(args, &a, sizeof(args));
    req = api::queue_op(RDRVR_NATIVE_GUN_MELEE, args, 12, nullptr);
    req_ms = now;
    for (int k = 0; k < 2; ++k) std::memcpy(prev[k], pt[k].world, sizeof(prev[k]));
    have_prev = true;
}
}  // namespace dgs

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
        ohs::frame(now, nullptr, nullptr);  // [Hands] OffHandItems: the knife's stab forgotten
        dgs::frame(now, nullptr, nullptr);  // the second gun's swing forgotten
        // the guard down (its update not run here: third person, a cutscene, a load), so the right trigger's RT is not
        // held back meanwhile (the 0.8.6 review)
        if (g_guard_up.exchange(false, std::memory_order_relaxed)) log::info("[gestures] guard down (out of first person)");
        g_guard_pose.store(false, std::memory_order_relaxed);
        g_lasso_trig_owned.store(false, std::memory_order_relaxed);  // nor the lasso's (the 0.8.7 review: a pause kept it)
        cancel_lock_throw(&lasso_out_until, &next_ok[controls::gun_hand()], "out of first person");  // (item 12) a pending lock throw's RT not sent there
        g_lock_thrown = false;
        g_lock_lt.store(false, std::memory_order_relaxed);  // (item 12) nor a lock's hold (not counted: no frame to judge it)
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
            if (!bp.hand_ok[jh] || !(hd.grip_valid || hd.valid) || !camera_lever::local_rel(lp, s.p, &s.turn)) {
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
    ohs::frame(now, &bp, &st);  // [Hands] OffHandItems: the off hand's knife (none held: one check)
    dgs::frame(now, &bp, &st);  // the second gun's own swing (none out: one check)
    guard_frame(now, st);       // [Gestures] Guard (run 10 item 7)
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
    // the lasso's aim in the local frame (the swing is judged toward it), and the other hand's pointing (world)
    float aim_l[3] = {point[0], point[1], point[2]};
    const bool lasso_in = lasso && st.weapon == kLassoWeapon;
    auto local = [&](const float* w, float* o) {
        o[0] = w[0] * right[0] + w[2] * right[2];
        o[1] = w[1];
        o[2] = w[0] * back[0] + w[2] * back[2];
    };
    {
        // the other controller's own pointing: its aim pose's -z (the local frame: x right, y up, z back), in the world
        // by the camera's rows (cycle LA1: the drawn hand's frame, calibrated for a gun, pointed 130 deg off with none)
        float wo[3] = {0, 0, -1};
        const hands::Hand oh = hands::get(1 - gun_h);
        const bool off_ok = oh.valid;
        if (off_ok) {
            const float f[3] = {0.0f, 0.0f, -1.0f};
            float l[3];
            qrot(oh.rot, f, l);
            for (int k = 0; k < 3; ++k) wo[k] = right[k] * l[0] + (k == 1 ? l[1] : 0.0f) + back[k] * l[2];
        }
        const int am = lasso_aim_mode();  // (item 12: lock mode, the head)
        float hf[3];
        const bool head_ok = am == 1 && camera_lever::head_forward(hf);
        // (C11) the game's lock with LT held (the last frame's hold): the head's ray then kept, until LT is let go (cycle H3:
        // the game drops its lock as the head turns away from it; the ray kept, the throw still goes at it)
        const bool lt_on = am == 1 && lasso_in && g_lock_lt.load(std::memory_order_relaxed);
        const bool locked = lt_on && (g_lock_dir_on || aim::lasso_probe().lock != 0);
        std::lock_guard lock(g_mutex);
        g_lasso_off_ok = off_ok;
        if (off_ok) std::memcpy(g_lasso_off_dir, wo, sizeof(wo));
        if (!locked) g_lock_dir_on = false;
        else if (!g_lock_dir_on && head_ok) {
            std::memcpy(g_lock_dir, hf, sizeof(hf));
            g_lock_dir_on = true;
            log::info("[gestures] lasso: the game's lock held (LT): its ray kept where the head looked (%.2f %.2f %.2f)", hf[0], hf[1], hf[2]);
        }
        if (g_lock_dir_on) local(g_lock_dir, aim_l);
        else if (head_ok) local(hf, aim_l);
        else if (am == 2 && off_ok) local(wo, aim_l);
        else if (am == 0 && now < g_lasso_frozen_until) local(g_lasso_frozen, aim_l);
    }
    static bool trig_down = false, lasso_was = false;
    // (2026-10-31, the off-hand build step 1, R16; round 59, the user: "Holding lasso by either hand should mean that holding
    // left trigger aims and right trigger throws") the trigger throw is the layout's gun-hand controller's (the right; the
    // left mirrored), whichever hand holds the lasso: controls.cpp keeps the triggers unswapped while it is in hand
    const int lasso_rt = controls::layout_gun_hand();
    if (lasso_in && !lasso_was) trig_down = hands::get(lasso_rt).trigger > 0.4f;  // taken with it held: no press (the review)
    lasso_was = lasso_in;
    // (item 12) LassoThrow=lock: the game's LT as the controllers drive it (the hold: on above 0.25, off below 0.12, or no
    // pad for 200 ms: a light press is the game's aim too), only with the lasso in hand; its end without a throw sends nothing
    const bool lock_mode = lasso_in && g_lasso_throw.load(std::memory_order_relaxed) == kLassoLock;
    {
        double lt_ms = 0;
        const float ltv = controls::player_lt(&lt_ms);
        const bool was = g_lock_lt.load(std::memory_order_relaxed);
        const bool held = lock_mode && now - lt_ms < 200.0 && ltv > (was ? 0.12f : 0.25f);
        if (held && !was) {
            g_lock_since.store(now, std::memory_order_relaxed);
            g_lock_thrown = false;
            g_lock_holds.fetch_add(1, std::memory_order_relaxed);
            log::info("[gestures] lasso: LT held (the game's lock and its spin, the lasso aimed by the head)");
        } else if (!held && was) {
            // (the re-review) let go inside the lead: the RT not yet sent is taken back (then counted as let go without a throw)
            cancel_lock_throw(&lasso_out_until, &next_ok[gun_h], lock_mode ? "LT let go" : "the lasso put away or the mode changed");
            if (!g_lock_thrown && lock_mode) {  // (the review) LT let go itself, not the lasso put away or the mode changed
                g_lock_no_throw.fetch_add(1, std::memory_order_relaxed);
                log::info("[gestures] lasso: LT let go after %.0f ms without a throw (nothing thrown)", now - g_lock_since.load(std::memory_order_relaxed));
            }
            g_lock_thrown = false;
        }
        g_lock_lt.store(held, std::memory_order_relaxed);
    }
    // a throw in lock mode (LT held): RT at once once LT has been held the lead, else after the rest of it; the player's
    // own LT (no injected LT: its end would drop a caught one, research/run17/lasso.md 1.2)
    auto lock_throw = [&](const char* by) {
        const double held_ms = now - g_lock_since.load(std::memory_order_relaxed);
        if (g_lock_wait_until > 0) {  // (the review) one throw a hold: a second press while the first waits adds none
            log::info("[gestures] lasso: %s while the throw waits for the game's lock: no second throw", by);
            return;
        }
        if (aim::lasso_probe().lock == 0 && held_ms < kLassoLockWaitMs) {  // no lock yet: held for it (below)
            g_lock_thrown = true;
            g_lock_rt_from = 0;
            g_lock_wait_until = g_lock_since.load(std::memory_order_relaxed) + kLassoLockWaitMs;
            g_lock_wait_by = by;
            g_lock_waits.fetch_add(1, std::memory_order_relaxed);
            log::info("[gestures] lasso thrown by %s in lock mode (LT held %.0f ms): no lock yet, the throw waits for it (up to %.0f ms more)", by, held_ms,
                      g_lock_wait_until - now);
            return;
        }
        const int lead = held_ms >= kLassoLeadMs ? 0 : kLassoLeadMs - static_cast<int>(held_ms);
        controls::inject(0, 255, 0, 150, 0, lead);
        g_lock_thrown = true;
        g_lock_rt_from = lead > 0 ? now + lead : 0;
        log::info("[gestures] lasso thrown by %s in lock mode (LT held %.0f ms; RT after %d ms more)", by, held_ms, lead);
    };
    if (g_lock_wait_until > 0) {  // a held lock throw: RT on the game's lock (LT held the lead), or at its last moment
        const double held_ms = now - g_lock_since.load(std::memory_order_relaxed);
        const bool lk = aim::lasso_probe().lock != 0;
        if ((lk && held_ms >= kLassoLeadMs) || now >= g_lock_wait_until) {
            g_lock_wait_until = 0;
            controls::inject(0, 255, 0, 150, 0, 0);
            if (lasso_out_until > 0) lasso_out_until = now + 15000.0;  // (the review) the yank's 600 ms counted from this RT
            if (lk) g_lock_waits_locked.fetch_add(1, std::memory_order_relaxed);
            log::info("[gestures] lasso: the throw by %s sent %s, LT held %.0f ms", g_lock_wait_by, lk ? "on the game's lock" : "without a lock (none came)", held_ms);
        }
    }
    auto probe_start = [&] {  // "gestures lassoprobe on": the lock at the throw
        if (!g_lasso_probe.load(std::memory_order_relaxed)) return;
        const aim::LassoProbe p = aim::lasso_probe();
        g_probe_lock.store(p.lock, std::memory_order_relaxed);
        g_probe_soft.store(p.soft, std::memory_order_relaxed);
        g_probe_target.store(0, std::memory_order_relaxed);
        g_probe_max_state.store(p.lasso_state, std::memory_order_relaxed);
        g_probe_until = now + 3000.0;
    };
    if (lasso_in) {  // the trigger throw: its press (with hysteresis) holds the aim for the lock, then throws
        const int tm = g_lasso_throw.load(std::memory_order_relaxed);
        const float tv = hands::get(lasso_rt).trigger;
        const bool was = trig_down;
        trig_down = tv > (was ? 0.4f : 0.6f);
        // (item 12) lock mode: RT only while LT is held; without it (or at the mouth / the hat: the whistle's, the hat
        // tip's) nothing, the press counted
        const bool lt_held = g_lock_lt.load(std::memory_order_relaxed), mouth = tm == kLassoLock && whistle::at_mouth(lasso_rt);
        const bool lock_refused = tm == kLassoLock && (!lt_held || mouth);
        if (lock_refused && trig_down && !was) {
            (lt_held ? g_lock_rt_mouth : g_lock_rt_unheld).fetch_add(1, std::memory_order_relaxed);
            log::info("[gestures] lasso: RT in lock mode %s: not thrown", lt_held ? "at the mouth or the hat (the whistle's, the hat tip's)" : "without LT held");
        }
        if ((tm == 1 || tm == 2 || tm == kLassoLock) && !lock_refused && trig_down && !was && now >= next_ok[gun_h]) {
            next_ok[gun_h] = now + 800.0;
            lasso_out_until = now + 15000.0;
            probe_start();
            if (tm == kLassoLock) {
                lock_throw("the trigger");
                g_lock_rt_throws.fetch_add(1, std::memory_order_relaxed);
            } else {
                controls::inject(0, 255, 0, 150, 255, kLassoLeadMs);
                log::info("[gestures] lasso thrown by the trigger (the aim held %d ms first)", kLassoLeadMs);
            }
            g_lasso_trigger_throws.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (g_lasso_probe.load(std::memory_order_relaxed) && now < g_probe_until) {  // (the probe) the first target the thrown lasso took, its furthest state
        const aim::LassoProbe p = aim::lasso_probe();
        if (p.lasso_state > g_probe_max_state.load(std::memory_order_relaxed)) g_probe_max_state.store(p.lasso_state, std::memory_order_relaxed);
        if (p.lasso_state >= 3 && p.target && !g_probe_target.load(std::memory_order_relaxed)) g_probe_target.store(p.target, std::memory_order_relaxed);
    }
    // (2026-11-01, GOAL-offhand-changes C12b; round 66 check 6, the user: the lasso still damages the NPC) a diagnostic, always
    // on: the one on the lasso (its probe's target, state 3 or more) and its health (GET_ACTOR_HEALTH, a native each 250 ms for
    // 8 s from the catch), logged as it changes, with each throw (RT) made meanwhile: at the catch, or by the game's pulls
    {
        static uint32_t hp_target = 0, hp_last_target = 0;
        static double hp_until = 0, hp_next = 0;
        static uint64_t hp_id = 0, hp_throws = 0;
        static float hp_last = -1.0f;
        if (lasso_in) {
            const aim::LassoProbe lp = aim::lasso_probe();
            // (the review) on someone (state 8 or more), once a target (the lasso keeps its last), its last result dropped
            if (lp.ok && lp.lasso_state >= 8 && lp.target && lp.target != hp_target && lp.target != hp_last_target) {
                if (hp_id) api::cancel_op(hp_id);
                hp_id = 0;
                hp_target = hp_last_target = lp.target;
                hp_until = now + 8000.0;
                hp_last = -1.0f;
                hp_throws = g_lasso_throws.load(std::memory_order_relaxed) + g_lasso_trigger_throws.load(std::memory_order_relaxed);
                log::info("[gestures] lasso: on 0x%x (state %d): its health watched 8 s", lp.target, lp.lasso_state);
            }
        }
        if (hp_target && now < hp_until) {
            RdrvrNativeResult r{};
            if (hp_id && api::wait_native(hp_id, &r, 0)) {
                float h = 0.0f;
                const uint32_t bits = static_cast<uint32_t>(r.value);
                std::memcpy(&h, &bits, sizeof(h));
                const uint64_t th = g_lasso_throws.load(std::memory_order_relaxed) + g_lasso_trigger_throws.load(std::memory_order_relaxed);
                if (std::isfinite(h) && std::fabs(h - hp_last) > 0.01f)
                    log::info("[gestures] lasso: 0x%x's health %.1f (was %.1f) %.0f ms from the catch, %llu throws (RT) since", hp_target, h, hp_last,
                              8000.0 - (hp_until - now), static_cast<unsigned long long>(th - hp_throws));
                hp_last = h;
                hp_id = 0;
            }
            if (!hp_id && now >= hp_next) {
                hp_next = now + 250.0;
                const uint64_t a1[1] = {static_cast<uint64_t>(hp_target)};
                hp_id = api::queue_native(0xF246F15D, a1, 1);  // GET_ACTOR_HEALTH
            }
        } else if (hp_target && now >= hp_until) {
            if (hp_id) api::cancel_op(hp_id);
            hp_target = 0;
            hp_id = 0;
        }
    }
    g_lasso_trig_owned.store(lasso_in && g_lasso_throw.load(std::memory_order_relaxed) != 0, std::memory_order_relaxed);
    // [Hands] DualWieldBesideTorch (2026-10-29): the controller holding DualWield's second gun beside the torch (-1: none);
    // its swings are no torch attack (its thrusts aim and fire a gun). Only with the option's torch in hand
    int torch_sec = -1;
    if (dual::beside_torch(st.weapon)) {  // (step 6: the knife too: its stab only by the hand that holds it)
        const dual::State ds = dual::state();
        if (ds.on && !ds.copy && !ds.carry) torch_sec = ds.ctrl;
    }
    for (int h = 0; h < 2; ++h) {
        float v[3], s = 0;
        if (!velocity(h, now, 60.0, v, &s, nullptr)) continue;  // the latest step or two
        g_last_speed[h] = s;
        if (s > g_peak_speed[h]) g_peak_speed[h] = s;
        const bool stance = st.weapon < 0 && controls::fist_stance();  // [Gestures] FistsBothTriggers: both held
        if (!melee && !lasso && !stance) continue;
        const float along = v[0] * point[0] + v[1] * point[1] + v[2] * point[2];
        // a punch or a stab: fists (nothing in hand), the knife or the torch; either hand, past the threshold and
        // forward (pulling the hand back after a punch is not another)
        const float fwd_speed = v[0] * fwd_l[0] + v[2] * fwd_l[2];
        // the fists' swing: with FistsBothTriggers only while both triggers are held (the stance), whatever Melee says;
        // without it as Melee says. The knife and the torch: Melee.
        const bool fists_ok = st.weapon < 0 && (controls::fists_both_triggers() ? stance : melee);
        // (step 5, O4) not the hand holding an off-hand item: its own stab (offhand's) strikes, not the game's attack
        if ((fists_ok || (melee && ((st.weapon == kKnife || st.weapon == kTorch) && h != torch_sec && !offhand::held(h)))) &&
            s > g_swing_speed.load(std::memory_order_relaxed) && !(holster::game_gate() & 3) &&  // (round 125: no attack at a table game)
            fwd_speed > 0.4f * s && now >= next_ok[h] && !whistle::at_mouth(h)) {  // run 8: a hand to the mouth is no punch
            next_ok[h] = now + 400.0;
            // run 10 item 6 (the user: "moving hand rapidly while on horseback draws last held gun"): riding or driving,
            // the game's attack with empty hands draws the last weapon, so no swing sends it there (as the gun melee)
            if (st.flags & (RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING)) {
                g_punches_riding.fetch_add(1, std::memory_order_relaxed);
                static double logged = -1e9;
                if (now - logged > 5000.0) {
                    logged = now;
                    log::info("[gestures] %s hand swing %.1f m/s while riding: no attack (it would draw the last gun)", h ? "right" : "left", s);
                }
                continue;
            }
            if (st.weapon < 0)
                controls::inject(0, 255, 0, 120, 255, g_fist_lead_ms.load(std::memory_order_relaxed));  // the fists: the stance first
            else
                controls::inject(0, 255, 0, 120);
            g_punches.fetch_add(1, std::memory_order_relaxed);
            g_last_swing_ms = now;
            log::info("[gestures] %s hand swing %.1f m/s: the attack (weapon %d)", h ? "right" : "left", s, st.weapon);
        }
        if (lasso && st.weapon == kLassoWeapon && h == gun_h) {
            const int tm = g_lasso_throw.load(std::memory_order_relaxed);
            // the hand's aim frozen as the swing starts (its wrist turns through the swing): for 0.7 s, or through a throw
            if (lasso_aim_mode() == 0 && tm != 1 && s > 1.0f) {
                float w[3], bt[3] = {0.0f, 0.174f, -0.985f};
                aim::barrel_in_target(bt);
                const int gj = bp.gun;
                for (int k = 0; k < 3; ++k) w[k] = bp.target_rot[gj][k * 3] * bt[0] + bp.target_rot[gj][k * 3 + 1] * bt[1] + bp.target_rot[gj][k * 3 + 2] * bt[2];
                std::lock_guard lock(g_mutex);
                if (now >= g_lasso_frozen_until) {
                    std::memcpy(g_lasso_frozen, w, sizeof(w));
                    g_lasso_freezes.fetch_add(1, std::memory_order_relaxed);
                    local(w, aim_l);  // (the swing judged toward the frozen aim from here)
                }
                g_lasso_frozen_until = std::fmax(g_lasso_frozen_until, now + 700.0);
            }
            const float al = std::sqrt(aim_l[0] * aim_l[0] + aim_l[1] * aim_l[1] + aim_l[2] * aim_l[2]);
            const float toward = al > 1e-4f ? (v[0] * aim_l[0] + v[1] * aim_l[1] + v[2] * aim_l[2]) / al : along;
            // thrown by a swing toward the aim (within 60 deg); then a yank back steps John back on the rope for a moment.
            // (item 12) Lock mode: the swing toward where you look, only while LT is held (without it: nothing, counted)
            const bool swing0 = tm != 1 && s > g_swing_speed.load(std::memory_order_relaxed) && toward > 0.5f * s;
            const bool unheld = swing0 && tm == kLassoLock && !g_lock_lt.load(std::memory_order_relaxed);
            static double unheld_ok = 0;
            if (unheld && now >= unheld_ok) {
                unheld_ok = now + 800.0;
                g_lock_swings_unheld.fetch_add(1, std::memory_order_relaxed);
                log::info("[gestures] lasso: a %.1f m/s swing without LT held in lock mode (not thrown)", s);
            }
            if (swing0 && !unheld && now >= next_ok[h]) {
                next_ok[h] = now + 800.0;
                {
                    std::lock_guard lock(g_mutex);
                    if (lasso_aim_mode() == 0) g_lasso_frozen_until = std::fmax(g_lasso_frozen_until, now + 400.0);
                }
                lasso_out_until = now + 15000.0;
                probe_start();
                if (tm == kLassoLock) {
                    lock_throw("a swing");
                    g_lock_swing_throws.fetch_add(1, std::memory_order_relaxed);
                } else {
                    controls::inject(0, 255, 0, 150);
                    log::info("[gestures] lasso thrown by a %.1f m/s swing", s);
                }
                g_lasso_throws.fetch_add(1, std::memory_order_relaxed);
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
    {  // 2026-10-12: [Gestures] LassoThrow, LassoAim
        // the defaults (the user, after their headset round: "head aim and trigger work well"): the trigger, the head
        const std::string t = config::get_string("Gestures", "LassoThrow", "trigger"), a = config::get_string("Gestures", "LassoAim", "head");
        g_lasso_throw = t == "swing" ? 0 : t == "both" ? 2 : t == "lock" ? kLassoLock : 1;  // (item 12: lock)
        g_lasso_aim = a == "hand" ? 0 : a == "offhand" ? 2 : 1;
    }
    g_swing_speed = config::get_float("Gestures", "SwingSpeed", 2.5f);
    g_throw_gain = config::get_float("Gestures", "ThrowGain", 2.0f);
    g_fist_lead_ms = config::get_int("Gestures", "FistLeadMs", 40);
    g_guard = config::get_bool("Gestures", "Guard", true);  // run 10 item 7: on (the user's request)
    {
        g_guard_triggers = config::get_bool("Gestures", "GuardTriggers", true);
        const std::string gb = config::get_string("Gestures", "GuardButtons", "rb+lt");
        g_guard_buttons = gb == "rb" ? 1 : gb == "lt" ? 2 : 3;
        float b[5];
        const std::string s = config::get_string("Gestures", "GuardBox", "0.05 0.40 -0.25 0.10 0.25");
        if (sscanf_s(s.c_str(), "%f %f %f %f %f", &b[0], &b[1], &b[2], &b[3], &b[4]) == 5 && b[0] < b[1] && b[2] < b[3] && b[4] > 0.0f) {
            std::lock_guard lock(g_mutex);
            std::memcpy(g_gbox, b, sizeof(b));
        }
        log::info("[gestures] the guard by both fists before the face %d (the game's %s; the box forward %.2f..%.2f, up %.2f..%.2f, across %.2f m; "
                  "both triggers held too %d)",
                  g_guard.load() ? 1 : 0, gb.c_str(), g_gbox[0], g_gbox[1], g_gbox[2], g_gbox[3], g_gbox[4], g_guard_triggers.load() ? 1 : 0);
    }
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
bool guard() { return g_guard.load(); }
bool guard_up() {  // only with GuardTriggers on (off: RT passed as in 0.8.5, the hands alone)
    return g_guard_triggers.load(std::memory_order_relaxed) && (g_guard_up.load(std::memory_order_relaxed) || g_guard_pose.load(std::memory_order_relaxed));
}
void set_guard(bool on, bool save_it) {
    if (g_guard.exchange(on) != on) log::info("[gestures] the guard by both fists: %s", on ? "on" : "off");
    if (save_it) save("Guard", on);
}
bool lasso_by_hand() { return g_lasso.load(); }
int lasso_throw() { return g_lasso_throw.load(); }
void set_lasso_throw(int m, bool save_it) {
    m = m < 0 || m > kLassoLock ? 0 : m;
    static const char* const kN[4] = {"swing", "trigger", "both", "lock"};
    if (g_lasso_throw.exchange(m) != m) log::info("[gestures] the lasso thrown by: %s", kN[m]);
    if (save_it) config::set("Gestures", "LassoThrow", kN[m]);
}
int lasso_aim() { return g_lasso_aim.load(); }
void set_lasso_aim(int m, bool save_it) {
    m = m < 0 || m > 2 ? 0 : m;
    static const char* const kN[3] = {"hand", "head", "offhand"};
    if (g_lasso_aim.exchange(m) != m) log::info("[gestures] the lasso aimed by: %s", kN[m]);
    if (save_it) config::set("Gestures", "LassoAim", kN[m]);
}
bool lasso_aim_dir(const float* hand_dir, float d[3]) {
    (void)hand_dir;
    const int am = lasso_aim_mode();  // (item 12: lock mode, the head)
    {
        std::lock_guard lock(g_mutex);
        if (am == 1 && g_lock_dir_on) {  // (C11) the ray kept at the lock
            std::memcpy(d, g_lock_dir, 12);
            return true;
        }
    }
    if (am == 1) return camera_lever::head_forward(d);
    std::lock_guard lock(g_mutex);
    if (am == 2) {
        if (!g_lasso_off_ok) return false;
        std::memcpy(d, g_lasso_off_dir, 12);
    } else {
        if (log::now_ms() >= g_lasso_frozen_until) return false;
        std::memcpy(d, g_lasso_frozen, 12);
    }
    const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (n < 1e-4f) return false;
    for (int k = 0; k < 3; ++k) d[k] /= n;
    return true;
}
bool lasso_trigger_owned() { return g_lasso_trig_owned.load(std::memory_order_relaxed); }
bool lasso_lock_held() { return lasso_aim_mode() == 1 && g_lasso_throw.load(std::memory_order_relaxed) == kLassoLock && g_lock_lt.load(std::memory_order_relaxed); }
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

bool hand_throw(int h, double window_ms, float vel[3], float origin[3], float* speed) {
    if (h < 0 || h > 1 || !pose::anchor_active()) return false;
    float v[3], s = 0, b[2] = {0.0f, 1.0f};
    if (!velocity(h, log::now_ms(), window_ms, v, &s, origin, b)) return false;
    const float right[3] = {b[1], 0.0f, -b[0]}, back[3] = {b[0], 0.0f, b[1]};  // as throw_launch: the camera's rows
    for (int k = 0; k < 3; ++k) vel[k] = (right[k] * v[0] + (k == 1 ? v[1] : 0.0f) + back[k] * v[2]) * g_throw_gain.load(std::memory_order_relaxed);
    if (speed) *speed = s;
    return true;
}
bool hand_velocity(int h, double window_ms, float vel[3]) {
    if (h < 0 || h > 1 || !pose::anchor_active()) return false;
    float v[3], s = 0, b[2] = {0.0f, 1.0f};
    if (!velocity(h, log::now_ms(), window_ms, v, &s, nullptr, b)) return false;
    const float right[3] = {b[1], 0.0f, -b[0]}, back[3] = {b[0], 0.0f, b[1]};  // as hand_throw
    for (int k = 0; k < 3; ++k) vel[k] = right[k] * v[0] + (k == 1 ? v[1] : 0.0f) + back[k] * v[2];
    return true;
}

std::string offhand_strike_status() {
    std::lock_guard lock(ohs::mu);
    char b[400];
    std::snprintf(b, sizeof(b), "the stab: weapon %d hand %d, speed %.2f m/s, armed %d (peak %.2f), struck %d | armed %llu, scans %llu, hits %llu, dry %llu, "
                  "refused %llu, none %llu, timeouts %llu, slow frames %llu | last: %s",
                  ohs::weapon, ohs::hand, ohs::speed_now, ohs::armed ? 1 : 0, ohs::peak, ohs::struck ? 1 : 0, static_cast<unsigned long long>(ohs::n_armed),
                  static_cast<unsigned long long>(ohs::n_scans), static_cast<unsigned long long>(ohs::n_hits), static_cast<unsigned long long>(ohs::n_dry),
                  static_cast<unsigned long long>(ohs::n_refused), static_cast<unsigned long long>(ohs::n_none),
                  static_cast<unsigned long long>(ohs::n_timeouts), static_cast<unsigned long long>(ohs::n_slow), ohs::last);
    return b;
}

std::string second_gun_strike_status() {
    std::lock_guard lock(dgs::mu);
    char b[400];
    std::snprintf(b, sizeof(b), "the second gun's swing: weapon %d controller %d, speed %.2f m/s, armed %d (peak %.2f), struck %d | armed %llu, scans %llu, "
                  "hits %llu, dry %llu, refused %llu, none %llu, timeouts %llu, held off %llu | last: %s",
                  dgs::weapon, dgs::ctrl, dgs::speed_now, dgs::armed ? 1 : 0, dgs::peak, dgs::struck ? 1 : 0, static_cast<unsigned long long>(dgs::n_armed),
                  static_cast<unsigned long long>(dgs::n_scans), static_cast<unsigned long long>(dgs::n_hits), static_cast<unsigned long long>(dgs::n_dry),
                  static_cast<unsigned long long>(dgs::n_refused), static_cast<unsigned long long>(dgs::n_none),
                  static_cast<unsigned long long>(dgs::n_timeouts), static_cast<unsigned long long>(dgs::n_held_off), dgs::last);
    return b;
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
        if (w == "lassothrow") set_lasso_throw(v == "trigger" ? 1 : v == "both" ? 2 : v == "lock" ? kLassoLock : 0, false);  // the session only
        if (w == "lassoprobe") g_lasso_probe = v == "on";  // (item 12) a test's: the lasso's state read (game memory)
        if (w == "lassoaim") set_lasso_aim(v == "head" ? 1 : v == "offhand" ? 2 : 0, false);
        if (w == "gripthrow") aim::set_grip_throw(on, false);  // [Gestures] ThrowByGrip (the session only)
        if (w == "peak") g_peak_speed[0] = g_peak_speed[1] = 0;  // "peak reset"
        if (w == "strike") aim::set_melee_strike(static_cast<float>(std::atof(v.c_str())));  // the session only
        if (w == "guard") {  // gestures guard on|off|rb|lt|rb+lt|triggers|notriggers (the session only)
            if (v == "on" || v == "off") set_guard(v == "on", false);
            if (v == "triggers" || v == "notriggers") g_guard_triggers = v == "triggers";
            if (v == "rb") g_guard_buttons = 1;
            if (v == "lt") g_guard_buttons = 2;
            if (v == "rb+lt") g_guard_buttons = 3;
        }
        if (w == "guardprobe") g_guard_probe = v == "on";
        if (w == "fistlead") g_fist_lead_ms = std::atoi(v.c_str());                            // the session only
        if (w == "tip" || w == "tipaxis" || w == "tippivot") aim::tip_command(w, v);
    }
    char b[400];
    std::snprintf(b, sizeof(b),
                  "throw %d melee %d lasso %d | swing %.1f m/s gain %.2f strike %.2f fist lead %d ms | speed L %.2f R %.2f, peak L %.2f R %.2f | "
                  "punches %llu (riding, not sent %llu) "
                  "lasso throws %llu yanks %llu throws %llu, last throw (%.2f %.2f %.2f) %.0f ms after the hand's peak",
                  g_throw.load() ? 1 : 0, g_melee.load() ? 1 : 0, g_lasso.load() ? 1 : 0, g_swing_speed.load(), g_throw_gain.load(),
                  aim::melee_strike(), g_fist_lead_ms.load(), g_last_speed[0],
                  g_last_speed[1], g_peak_speed[0], g_peak_speed[1], static_cast<unsigned long long>(g_punches.load()),
                  static_cast<unsigned long long>(g_punches_riding.load()), static_cast<unsigned long long>(g_lasso_throws.load()), static_cast<unsigned long long>(g_yanks.load()),
                  static_cast<unsigned long long>(g_throws.load()), g_last_throw[0], g_last_throw[1], g_last_throw[2], g_last_throw_age);
    float gh[2][3];
    {
        std::lock_guard lock(g_mutex);
        std::memcpy(gh, g_guard_hand, sizeof(gh));
    }
    char g[400];
    const int gb = g_guard_buttons.load();
    std::snprintf(g, sizeof(g), " | guard %d (%s) up %d, guards %llu (the fists selected %llu), frames %llu, blocking %llu, melee state %d | the hands before the face L (%.2f %.2f %.2f) R (%.2f %.2f %.2f), both triggers needed %d",
                  g_guard.load() ? 1 : 0, gb == 1 ? "rb" : gb == 2 ? "lt" : "rb+lt", g_guard_up.load() ? 1 : 0,
                  static_cast<unsigned long long>(g_guards.load()), static_cast<unsigned long long>(g_guard_fists.load()),
                  static_cast<unsigned long long>(g_guard_frames.load()),
                  static_cast<unsigned long long>(g_guard_block_frames.load()), aim::player_melee_state(), gh[0][0], gh[0][1], gh[0][2], gh[1][0], gh[1][1],
                  gh[1][2], g_guard_triggers.load() ? 1 : 0);
    char l[200];
    {
        static const char* const kT[4] = {"swing", "trigger", "both", "lock"};
        static const char* const kA[3] = {"hand", "head", "offhand"};
        float fz[3];
        double until;
        {
            std::lock_guard lock(g_mutex);
            std::memcpy(fz, g_lasso_frozen, sizeof(fz));
            until = g_lasso_frozen_until;
        }
        std::snprintf(l, sizeof(l), " | lasso throw by %s, aim by %s: trigger throws %llu, aims frozen %llu (frozen now %d at (%.2f %.2f %.2f)), RT owned %d",
                      kT[g_lasso_throw.load()], kA[g_lasso_aim.load()], static_cast<unsigned long long>(g_lasso_trigger_throws.load()),
                      static_cast<unsigned long long>(g_lasso_freezes.load()), log::now_ms() < until ? 1 : 0, fz[0], fz[1], fz[2],
                      g_lasso_trig_owned.load() ? 1 : 0);
    }
    char k[480];  // (item 12) LassoThrow=lock; the probe's reads only when it is on
    {
        const bool held = g_lock_lt.load();
        std::snprintf(k, sizeof(k), " | lasso lock: LT held %d (%.0f ms), holds %llu, thrown by RT %llu, by a swing %llu, LT let go without a throw %llu, "
                      "swings without LT %llu, RT without LT %llu, RT at the mouth or hat %llu, RT cancelled in the lead %llu, waited for the lock %llu (locked %llu)",
                      held ? 1 : 0, held ? log::now_ms() - g_lock_since.load() : 0.0, static_cast<unsigned long long>(g_lock_holds.load()),
                      static_cast<unsigned long long>(g_lock_rt_throws.load()), static_cast<unsigned long long>(g_lock_swing_throws.load()),
                      static_cast<unsigned long long>(g_lock_no_throw.load()), static_cast<unsigned long long>(g_lock_swings_unheld.load()),
                      static_cast<unsigned long long>(g_lock_rt_unheld.load()), static_cast<unsigned long long>(g_lock_rt_mouth.load()),
                      static_cast<unsigned long long>(g_lock_cancels.load()), static_cast<unsigned long long>(g_lock_waits.load()),
                      static_cast<unsigned long long>(g_lock_waits_locked.load()));
    }
    std::string probe;
    if (g_lasso_probe.load()) {
        const aim::LassoProbe p = aim::lasso_probe();
        char q[360];
        std::snprintf(q, sizeof(q), " | lasso probe: ok %d aim state %d user state %d lasso state %d, target 0x%x candidate 0x%x, reticle 0x%x lock 0x%x "
                      "soft 0x%x | at the last throw: lock 0x%x soft 0x%x, its target in flight 0x%x, the lasso's furthest state %d",
                      p.ok ? 1 : 0, p.aim_state, p.user_state, p.lasso_state, p.target, p.candidate, p.reticle, p.lock, p.soft, g_probe_lock.load(),
                      g_probe_soft.load(), g_probe_target.load(), g_probe_max_state.load());
        probe = q;
    }
    return b + std::string(g) + l + k + probe + aim::tip_command("", "");
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
            std::lock_guard lock2(dgs::mu);
            dgs::n_armed = dgs::n_scans = dgs::n_hits = dgs::n_dry = dgs::n_refused = dgs::n_none = dgs::n_timeouts = dgs::n_held_off = 0;
        }
        if (w == "damage") {  // damage <v>: GunMeleeDamage, the session only (the knock-out points a hit takes)
            std::string v;
            if (in >> v) gun_melee::set_damage(static_cast<float>(std::atof(v.c_str())), false);
        }
        if (w == "force") {  // force <push>: GunMeleeForce, the session only (75 = the game's punch)
            std::string v;
            if (in >> v) gun_melee::set_force(static_cast<float>(std::atof(v.c_str())), false);
        }
        if (w == "speed") {  // speed <m/s> [peak|contact]: the hit speed and its rule, the session only (run 8 item 1)
            std::string v, rule;
            if (in >> v) gun_melee::set_speed(static_cast<float>(std::atof(v.c_str())), false);
            if (in >> rule) gun_melee::set_by_peak(rule != "contact");
        }
        if (w == "second") return second_gun_strike_status();  // gunmelee second: the second gun's own swing
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
                      "gunmelee %d dry %d | speed %.1f (%s) arm %.1f damage %.1f heavy %.1f lethal %d force %.2f stock %.2f | now %.2f m/s peak %.2f, armed %d struck %d, "
                      "points %d (family %d, weapon %d), swings over without a hit %llu | armed %llu requests %llu scans %llu actors %llu iterators made %llu destroyed %llu | none %llu hits %llu "
                      "dry %llu refused %llu guard %llu no iterator %llu no core %llu timeouts %llu | suppressed: %s | frame-end cost %.1f us/frame over %llu "
                      "frames | last: %s | %s%s%s",
                      cfg.on ? 1 : 0, cfg.dry ? 1 : 0, cfg.speed, cfg.by_peak ? "at the peak" : "at contact", cfg.arm, cfg.damage, cfg.heavy, cfg.lethal ? 1 : 0, cfg.force, cfg.stock, gm::speed_now,
                      gm::speed_peak, gm::armed ? 1 : 0, gm::struck ? 1 : 0, gm::npts, gm::family, gm::weapon, static_cast<unsigned long long>(gm::n_swings_missed),
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
