#include "core/wheel.h"

#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>

#include "common/rdrvr_api.h"
#include "core/anchors.h"
#include "core/api.h"
#include "core/config.h"
#include "core/controllers.h"
#include "core/controls.h"
#include "core/hands.h"
#include "core/log.h"
#include "core/physics.h"
#include "core/pose.h"
#include "core/vr_mode.h"
#include "core/xinput.h"
#include "core/xr.h"

namespace rdrvr::wheel {
namespace {

constexpr double kMinHoldMs = 300.0;   // the game opens the wheel after 0.2 s of LB; a shorter press is its holster tap
constexpr double kStickTailMs = 150.0; // the stick kept after LB is let go (the game equips on the release)
constexpr uint32_t kRefreshMs = 250;   // the override's life, refreshed each poll (a stalled poll lets it go)
std::atomic<bool> g_mode{false};
std::atomic<float> g_dead{0.04f}, g_angle{0.0f};
std::atomic<bool> g_flip_y{false};
std::atomic<int> g_grips{3};  // bit 0 the left controller's grip, bit 1 the right's

std::atomic<bool> g_stick_dpad{true};  // [Holsters] WheelStickDpad: the holding hand's stick as the D-pad (controls.cpp)
int g_ctrl = -1;              // the controller holding the wheel open (-1: closed)
double g_open_ms = 0.0, g_release_ms = 0.0;
bool g_releasing = false;     // the grip let go before the minimum hold: LB held to it, then a cancel
float g_centre[3] = {};       // the hand at the press (LOCAL)
float g_right[3] = {1, 0, 0}, g_up[3] = {0, 1, 0};  // the head's axes at the press (LOCAL)
float g_head_q[4] = {0, 0, 0, 1};                   // the head's turn at the press (the hand quad faces it)
std::atomic<bool> g_at_hand{true};                  // [Holsters] WheelAtHand
float g_rect[4] = {0.33f, 0.04f, 0.71f, 0.74f};     // [Holsters] WheelRect (set at init only)
std::mutex g_quad_mutex;                            // the press's centre and turn: the pad's poll writes, the XR frame reads
float g_stick[2] = {0, 0};    // the stick now (-1..1)
int g_sector = -1;            // 0..7 (45-degree sectors from the right, counter-clockwise), -1 the dead zone
std::atomic<bool> g_open{false};
std::atomic<uint64_t> g_opens{0}, g_commits{0}, g_cancels{0};
std::atomic<int> g_last_sector{-1};
std::atomic<int> g_ctrl_pub{-1}, g_sector_pub{-1};   // the poll's state for the status (another thread)
std::atomic<float> g_stick_pub[2] = {0.0f, 0.0f};
bool g_free_was[2] = {false, false};                // a press opens the wheel, not a grip still held
int g_pend_ctrl = -1, g_pend_weapon = -1;            // a commit's hand, until the weapon in hand changes (or 1.5 s)
double g_pend_until = 0.0;
void publish() {
    g_ctrl_pub.store(g_ctrl, std::memory_order_relaxed);
    g_sector_pub.store(g_sector, std::memory_order_relaxed);
    g_stick_pub[0].store(g_stick[0], std::memory_order_relaxed);
    g_stick_pub[1].store(g_stick[1], std::memory_order_relaxed);
}
std::atomic<float> g_last_off{0.0f};

void rotate(const float q[4], const float v[3], float out[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2.0f * (y * v[2] - z * v[1]), ty = 2.0f * (z * v[0] - x * v[2]), tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

bool hand_pos(int c, float out[3]) {
    const hands::Hand hd = hands::get(c);
    if (!(hd.grip_valid || hd.valid)) return false;
    std::memcpy(out, hd.grip_valid ? hd.grip_pos : hd.pos, 12);
    return true;
}

void begin(int c, double now) {
    float p[3];
    if (!hand_pos(c, p)) return;
    xr::EyeView v[2];
    if (xr::eye_views_peek(v)) {  // a peek: not a scene pass's hand-out
        const float rx[3] = {1, 0, 0}, uy[3] = {0, 1, 0};
        rotate(v[0].orientation, rx, g_right);
        rotate(v[0].orientation, uy, g_up);
        std::lock_guard lock(g_quad_mutex);
        std::memcpy(g_head_q, v[0].orientation, sizeof(g_head_q));
    } else {
        g_right[0] = 1, g_right[1] = g_right[2] = 0;
        g_up[1] = 1, g_up[0] = g_up[2] = 0;
    }
    {
        std::lock_guard lock(g_quad_mutex);
        std::memcpy(g_centre, p, sizeof(p));
    }
    g_ctrl = c;
    g_open_ms = now;
    g_releasing = false;
    g_stick[0] = g_stick[1] = 0.0f;
    g_sector = -1;
    g_open = true;
    g_opens.fetch_add(1, std::memory_order_relaxed);
    controllers::pulse(c, 0.2f, 15);
    log::info("[wheel] the %s grip: the weapon wheel (LB held), the centre at the hand", c ? "right" : "left");
}

void steer() {
    float p[3];
    if (!hand_pos(g_ctrl, p)) return;
    float d[3];
    for (int k = 0; k < 3; ++k) d[k] = p[k] - g_centre[k];
    const float x = d[0] * g_right[0] + d[1] * g_right[1] + d[2] * g_right[2];
    const float y = d[0] * g_up[0] + d[1] * g_up[1] + d[2] * g_up[2];
    const float r = std::sqrt(x * x + y * y);
    g_last_off.store(r, std::memory_order_relaxed);
    if (r < g_dead.load(std::memory_order_relaxed)) {
        g_stick[0] = g_stick[1] = 0.0f;
        g_sector = -1;
        return;
    }
    const float a = std::atan2(y, x) + g_angle.load(std::memory_order_relaxed) * 0.0174532925f;
    g_stick[0] = std::cos(a);
    g_stick[1] = (g_flip_y.load(std::memory_order_relaxed) ? -1.0f : 1.0f) * std::sin(a);
    float deg = std::atan2(g_stick[1], g_stick[0]) * 57.29578f;  // the stick sent (WheelAngle, WheelFlipY): the game's sector
    if (deg < 0) deg += 360.0f;
    const int s = static_cast<int>((deg + 22.5f) / 45.0f) % 8;
    if (s != g_sector) {
        g_sector = s;
        controllers::pulse(g_ctrl, 0.12f, 10);  // a tick at each new sector
    }
}

// ---- [Holsters] WheelPause (2026-10-27, the user: "Weapon wheel pausing game until you pick a weapon";
// research\run27\wheelpause.md, all static). The game's own time-warp controller, the one Dead Eye and the duel use,
// through the game's own script natives, both handlers read (wheelpause.md 2): SET_TIME_WARP(scale, rampIn, duration)
// calls FUN_1402085b0, which saves the base scale only while idle (so a call while active is a renewal: the same base,
// a new duration), and CANCEL_TIME_WARP(0) puts back the base, the timer's max dt and idle at once (a no-op while
// idle). Neither takes a pointer or a handle. Never PAUSE_GAME / UNPAUSE_GAME (the 2026-10-03 crash; crashes upload).
// The scale is never 0: the timer's 1/dt (FUN_14080fb30, no guard) would be +inf (wheelpause.md 3).
constexpr uint32_t kSetTimeWarp = 0xD93E1BCBu;     // SET_TIME_WARP (handler 0x1406583c0)
constexpr uint32_t kCancelTimeWarp = 0xAF50E8A1u;  // CANCEL_TIME_WARP (handler 0x140658400)
constexpr double kLeaseEveryMs = 250.0;  // a renewal each 250 ms of wall time (the pad's poll), while the grip holds
// the lease: the controller counts its duration down by dt0 a game frame (the scaled dt when it started, never
// refreshed), so 1 s is about 1 s of frames: should the renewals stop (a stalled poll, alt-tab), the game ramps back
// by itself (about 0.3 s from 0.02, wheelpause.md 1.2)
constexpr float kLeaseSeconds = 1.0f;
// the ramp-in: the ramp's factor clamp(dt0 x 125, 0, 1) is 1 up to 125 fps, the target at once. It is also our mark:
// FUN_1402085b0 stores it at +0x20 (0x140208625) and nothing else changes it while the mode byte +0x33 stays 0 (the
// frame update FUN_1402089d0 copies +0x40 over it only then; no writer of +0x33 found, the reset zeroes it). Not 100:
// that is the reset's default (+0x20 and +0x40 = 100.0, FUN_140208670), so another warp could carry it too
// (2026-10-27, the review: "ours" by the scale alone took Dead Eye's 0.1 for ours)
constexpr float kRampIn = 125.0f;
// the player info's byte the wheel's OnActivate FUN_1403fac10 sets (0x1403fb97f) and OnDeactivate clears (0x1403fbbce):
// the game's wheel shown. Its open timer FUN_1406c4110 adds the scaled dt, so a warp before it shows would hold the
// opening back by 1/scale: the pause waits for this byte
constexpr uintptr_t kWheelShown = 0x43f;
// [TimeWarpClock]: the target scale (float), the ramp-in rate (float), active (byte)
constexpr uintptr_t kWarpTarget = 0x0c, kWarpRampIn = 0x20, kWarpActive = 0x30;
// the scale's range: never 0 (1/dt), and kept below Dead Eye's 0.1 (playertune InitialSlowMoTimeScale), so a Dead Eye's
// target is never ours (the review, 2026-10-27)
constexpr float kScaleMin = 0.01f, kScaleMax = 0.08f;
std::atomic<bool> g_pause_opt{false};      // [Holsters] WheelPause
std::atomic<float> g_pause_scale{0.02f};   // [Holsters] WheelPauseScale
std::atomic<bool> g_lease_hold{false};     // "wheel pause hold" (test only): no renewals until this pause ends
bool g_paused = false;          // the pad's poll's: a warp of ours asked for and not yet ended
bool g_pause_done = false;      // one pause per open: after an end the same grip's wheel runs on at the game's speed
bool g_pause_mounted = false;   // RDRVR_ACTOR_MOUNTED at the start (a change ends the pause)
bool g_pause_anchor = false;    // pose::anchor_active() at the start (it going off ends the pause: a script's camera)
double g_lease_ms = 0.0;
uint64_t g_set_id = 0;          // the latest SET_TIME_WARP's request (0: none outstanding)
bool g_set_done = false;        // its result seen (it ran)
double g_set_ms = 0.0;          // when it was queued (a tick that pops a request runs it in that tick: well inside 0.5 s)
// the requests whose results are still to be collected (the actions' sounds' pattern: an uncollected result stays in the
// API's map until its clear at 4096); forgotten after 5 s (a result cancel_op dropped never comes)
constexpr int kReqs = 16;
uint64_t g_req[kReqs] = {};
double g_req_ms[kReqs] = {};
std::atomic<float> g_warp_scale{0.0f};  // the scale asked for: the controller's +0x0c is exactly it while the warp is ours
std::atomic<bool> g_paused_pub{false};
std::atomic<uint64_t> g_pauses{0}, g_leases{0}, g_pause_ends{0}, g_set_calls{0}, g_cancel_calls{0};
std::atomic<const char*> g_end_why{"none"};  // a string literal: the last end's reason
bool g_absent_logged = false, g_down_logged = false;

float clamp_scale(float s) { return s != s ? 0.02f : s < kScaleMin ? kScaleMin : s > kScaleMax ? kScaleMax : s; }  // never 0 (NaN: the default)
uint64_t fbits(float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}
// game memory read with SEH as the backstop (read only: the controller and the player info)
bool raw(uintptr_t a, void* out, size_t n) {
    if (!a) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T>
bool rd(uintptr_t a, T* out) {
    return raw(a, out, sizeof(T));
}
bool can_warp() { return !anchors::stand_down() && anchors::present(anchors::Id::TimeWarpClock); }
// the controller now (read only); false when it cannot be read (the anchor absent, the clock not yet made: the SET
// handler dereferences it unchecked, so nothing is asked of it then)
struct Warp {
    bool active = false;
    float target = 0.0f, ramp = 0.0f;
};
bool warp_state(Warp* w) {
    uintptr_t c = 0;
    uint8_t a = 0;
    float t = 0.0f, r = 0.0f;
    if (!can_warp() || !rd(anchors::addr(anchors::Id::TimeWarpClock), &c) || !c || !rd(c + kWarpActive, &a) || !rd(c + kWarpTarget, &t) ||
        !rd(c + kWarpRampIn, &r))
        return false;
    w->active = a != 0;
    w->target = t;
    w->ramp = r;
    return true;
}
// ours: active with our target and our ramp-in (Dead Eye's, a duel's, a kill's slow motion are left to their own code)
bool is_ours(const Warp& w) { return w.active && w.target == g_warp_scale.load(std::memory_order_relaxed) && w.ramp == kRampIn; }
bool warp_is_ours() {
    Warp w;
    return warp_state(&w) && is_ours(w);
}
void track(uint64_t id, double now) {  // a request whose result collect() takes; the oldest forgotten when full
    int k = 0;
    for (int i = 0; i < kReqs; ++i) {
        if (!g_req[i]) {
            k = i;
            break;
        }
        if (g_req_ms[i] < g_req_ms[k]) k = i;
    }
    g_req[k] = id;
    g_req_ms[k] = now;
}
void untrack(uint64_t id) {
    for (int i = 0; i < kReqs; ++i)
        if (g_req[i] == id) g_req[i] = 0;
}
void collect(double now) {  // each poll: the answered requests' results taken (the latest SET's marks it done)
    RdrvrNativeResult r{};
    for (int i = 0; i < kReqs; ++i) {
        if (!g_req[i]) continue;
        if (api::wait_native(g_req[i], &r, 0)) {
            if (g_req[i] == g_set_id) g_set_done = true;
            g_req[i] = 0;
        } else if (now - g_req_ms[i] > 5000.0) {
            g_req[i] = 0;
        }
    }
}
int wheel_shown() {  // 1 shown, 0 not, -1 unreadable
    uintptr_t p = 0;
    uint8_t s = 0;
    if (anchors::stand_down() || !rd(anchors::addr(anchors::Id::PlayerInfo), &p) || !p || !rd(p + kWheelShown, &s)) return -1;
    return s ? 1 : 0;
}
bool prop_held() { return physics::holding(0) || physics::holding(1); }

void warp(float scale, double now) {  // SET_TIME_WARP(scale, our ramp-in: at once, the lease's duration), next script tick
    collect(now);
    if (g_set_id && !g_set_done && api::cancel_op(g_set_id)) untrack(g_set_id);  // superseded: off the queue, never runs
    const uint64_t a[3] = {fbits(scale), fbits(kRampIn), fbits(kLeaseSeconds)};
    g_set_id = api::queue_native(kSetTimeWarp, a, 3, 0, nullptr);
    g_set_done = false;
    g_set_ms = now;
    track(g_set_id, now);
    g_set_calls.fetch_add(1, std::memory_order_relaxed);
}
void end_pause(const char* why) {
    if (!g_paused) return;
    const double now = log::now_ms();
    g_paused = false;
    g_pause_done = true;
    g_lease_hold = false;
    g_paused_pub.store(false, std::memory_order_relaxed);
    // The latest SET may not have run yet: answered, it ran; still queued, it is taken off (it never runs); popped by a
    // tick and not answered, it runs in that tick before a CANCEL queued now (one FIFO), so that CANCEL goes, but only
    // while the controller is idle or ours (else a pick within a tick of the start would leave a 1 s warp behind, the
    // memory read seeing it idle). Only a recent one: an old one unanswered had its result dropped (cancel_op's race)
    collect(now);
    bool pending = false;
    if (g_set_id && !g_set_done) {
        if (api::cancel_op(g_set_id)) untrack(g_set_id);
        else pending = now - g_set_ms < 500.0;
    }
    g_set_id = 0;
    Warp w;
    const bool readable = warp_state(&w);
    const bool ours = readable && is_ours(w);
    const bool cancel = ours || (pending && readable && !w.active);
    if (cancel) {
        const uint64_t a[1] = {0};  // 0: the base back at once (else the controller ramps)
        track(api::queue_native(kCancelTimeWarp, a, 1, 0, nullptr), now);
        g_cancel_calls.fetch_add(1, std::memory_order_relaxed);
    }
    g_pause_ends.fetch_add(1, std::memory_order_relaxed);
    g_end_why.store(why, std::memory_order_relaxed);
    log::info("[wheel] the pause ended: %s (the warp %s)", why,
              ours ? "ours: cancelled at once" : cancel ? "just asked for: cancelled after it" : "over or not ours: left alone");
}
// A script's camera, a cutscene: the anchor's scripted camera made but not current (RDRVR_ACTOR_CAM_AWAY), the
// cutscene state (vr_mode's CUTSCENE_MANAGER_IS_CUTSCENE_PLAYING watch, bit 16) (the review, 2026-10-27)
bool scripted(const RdrvrActorState& st) { return (st.flags & RDRVR_ACTOR_CAM_AWAY) || (vr_mode::state_flags() & 16); }
// The start, while the grip holds the wheel open past the minimum hold: the game's wheel shown, not Dead Eye (its own
// warp) nor any other warp running (Dead Eye's ramp-out, a kill's slow motion: their state is not ours to restore), no
// script camera or cutscene, no prop in a hand (a held prop is moved by wall time: at a dt 1/scale smaller the engine
// reads a velocity 1/scale larger, wheelpause.md 3)
void try_start(double now) {
    if (anchors::stand_down()) {  // not verified yet (the boot), or the mod stood down: not latched, tried again next poll
        if (!g_down_logged) {
            g_down_logged = true;
            log::warn("[wheel] WheelPause: the anchors are not verified (or the mod stood down): no pause meanwhile");
        }
        return;
    }
    if (!anchors::present(anchors::Id::TimeWarpClock)) {  // this build lacks it: for good
        g_pause_done = true;
        if (!g_absent_logged) {
            g_absent_logged = true;
            log::warn("[wheel] WheelPause: the TimeWarpClock anchor is not in this build: the pause is off (the wheel works as before)");
        }
        return;
    }
    if (!api::plugin_attached() || wheel_shown() != 1 || prop_held()) return;
    RdrvrActorState st{};
    if (!api::actor_state(&st) || (st.weapon_flags & RDRVR_WEAPON_DEADEYE) || scripted(st)) return;
    Warp w;
    if (!warp_state(&w) || (w.active && !is_ours(w))) return;  // unreadable, or another warp running
    const float s = g_pause_scale.load(std::memory_order_relaxed);
    g_warp_scale.store(s, std::memory_order_relaxed);
    g_lease_hold = false;
    warp(s, now);
    g_paused = true;
    g_paused_pub.store(true, std::memory_order_relaxed);
    g_lease_ms = now;
    g_pause_mounted = (st.flags & RDRVR_ACTOR_MOUNTED) != 0;
    g_pause_anchor = pose::anchor_active();
    g_pauses.fetch_add(1, std::memory_order_relaxed);
    log::info("[wheel] the pause: the world at %.3fx while the wheel is open (the game's time warp, a %.0f s lease every %.0f ms)", s,
              kLeaseSeconds, kLeaseEveryMs);
}
// The renewal, only while the controller is idle (our lease lapsed, or our first SET not run yet) or ours: another
// warp found running (a Dead Eye, a kill's slow motion took it) ends our pause with no CANCEL (theirs to end), and the
// same opening does not pause again (end_pause latches g_pause_done)
void lease(double now) {
    if (g_lease_hold.load(std::memory_order_relaxed) || now - g_lease_ms < kLeaseEveryMs) return;
    Warp w;
    if (!warp_state(&w)) return end_pause("the controller not readable");
    if (w.active && !is_ours(w)) {
        log::info("[wheel] another warp took over (target %.3f, ramp-in %.1f): ours ends without a cancel", w.target, w.ramp);
        return end_pause("another warp took over");
    }
    warp(g_warp_scale.load(std::memory_order_relaxed), now);
    g_lease_ms = now;
    g_leases.fetch_add(1, std::memory_order_relaxed);
}
// each poll while paused: the ways out the mod does not cause itself (death, a cutscene or a script's camera, a mount,
// a script or the pause menu closing the game's wheel; Dead Eye; a grab begun with the other hand; the option off)
void pause_guard() {
    if (!g_pause_opt.load(std::memory_order_relaxed)) return end_pause("WheelPause off");
    RdrvrActorState st{};
    if (!api::actor_state(&st)) return end_pause("the player actor not valid");
    if (st.weapon_flags & RDRVR_WEAPON_DEADEYE) return end_pause("Dead Eye");
    if (st.flags & RDRVR_ACTOR_CAM_AWAY) return end_pause("a script's camera");
    if (vr_mode::state_flags() & 16) return end_pause("a cutscene");
    if (g_pause_anchor && !pose::anchor_active()) return end_pause("the camera anchor off (a script's camera)");
    if (((st.flags & RDRVR_ACTOR_MOUNTED) != 0) != g_pause_mounted) return end_pause("mounting changed");
    if (prop_held()) return end_pause("a hand holds a prop");
    if (wheel_shown() != 1) return end_pause("the game closed its wheel");
}

}  // namespace

void init() {
    g_stick_dpad = config::get_bool("Holsters", "WheelStickDpad", true);
    const std::string m = config::get_string("Holsters", "Mode", "holsters");
    g_mode = m == "wheel";
    float dz = config::get_float("Holsters", "WheelDeadZone", 0.04f);
    g_dead = !(dz >= 0.01f) ? 0.01f : dz > 0.15f ? 0.15f : dz;
    float an = config::get_float("Holsters", "WheelAngle", 0.0f);
    g_angle = !(an >= -180.0f) ? 0.0f : an > 180.0f ? 180.0f : an;
    g_flip_y = config::get_bool("Holsters", "WheelFlipY", false);
    g_at_hand = config::get_bool("Holsters", "WheelAtHand", true);
    {
        const std::string r = config::get_string("Holsters", "WheelRect", "0.33 0.04 0.71 0.74");
        float a = 0, b = 0, c = 0, d = 0;
        if (sscanf_s(r.c_str(), "%f %f %f %f", &a, &b, &c, &d) == 4 && a >= 0 && b >= 0 && c <= 1 && d <= 1 && c - a > 0.05f && d - b > 0.05f)
            g_rect[0] = a, g_rect[1] = b, g_rect[2] = c, g_rect[3] = d;
    }
    const std::string g = config::get_string("Holsters", "WheelGrips", "both");
    g_grips = g == "left" ? 1 : g == "right" ? 2 : 3;
    log::info("[wheel] the weapon wheel mode %d (Mode=%s): the grips %s, the dead zone %.2f m, the angle %+.0f deg, y %s", g_mode.load() ? 1 : 0,
              m.c_str(), g.c_str(), g_dead.load(), g_angle.load(), g_flip_y.load() ? "flipped" : "as the hand");
    // [Holsters] WheelPause (2026-10-27): whether the anchor is in this build is known only after anchors::verify() (run
    // later): try_start() checks it at each open and logs once
    g_pause_opt = config::get_bool("Holsters", "WheelPause", false);
    g_pause_scale = clamp_scale(config::get_float("Holsters", "WheelPauseScale", 0.02f));
    log::info("[wheel] WheelPause %d: the world at %.3fx while a grip holds the wheel open (the game's time warp, Dead Eye's)",
              g_pause_opt.load() ? 1 : 0, g_pause_scale.load());
}

bool wheel_pause() { return g_pause_opt.load(std::memory_order_relaxed); }
void set_wheel_pause(bool on, bool save) {  // off while paused: the next poll's guard ends the pause (the pad's thread)
    if (g_pause_opt.exchange(on) != on) log::info("[wheel] WheelPause: %s (%s)", on ? "on" : "off", save ? "saved" : "the session");
    if (save) config::set("Holsters", "WheelPause", on ? "1" : "0");
}
float wheel_pause_scale() { return g_pause_scale.load(std::memory_order_relaxed); }
void set_wheel_pause_scale(float scale, bool save) {  // the next pause's (a pause keeps its own: its warp stays ours)
    const float s = clamp_scale(scale);
    if (g_pause_scale.exchange(s) != s) log::info("[wheel] WheelPauseScale: %.3f (%s)", s, save ? "saved" : "the session");
    if (save) {
        char b[16];
        std::snprintf(b, sizeof(b), "%.2f", s);
        config::set("Holsters", "WheelPauseScale", b);
    }
}
// No core shutdown runs today: the exit guard ends the process with TerminateProcess (exit_guard.h) and the warp lives
// only in the game's memory, and the game's reset FUN_1401f8a00 cancels it on any load (wheelpause.md 4). Kept for a
// future core shutdown, on the pad's thread (no script tick runs the cancel after the game's last frame).
void shutdown() { end_pause("shutdown"); }

bool wheel_mode() { return g_mode.load(std::memory_order_relaxed); }
void set_wheel_mode(bool on, bool save) {
    if (g_mode.exchange(on) != on) log::info("[wheel] the mode: %s (%s)", on ? "the weapon wheel" : "the holsters", save ? "saved" : "the session");
    if (save) config::set("Holsters", "Mode", on ? "wheel" : "holsters");
}
bool open() { return g_open.load(std::memory_order_relaxed); }
int holder() { return g_ctrl_pub.load(std::memory_order_relaxed); }
bool stick_dpad() { return g_stick_dpad.load(std::memory_order_relaxed); }
void set_stick_dpad(bool on, bool save) {
    if (g_stick_dpad.exchange(on) != on) log::info("[wheel] the holding hand's stick as the D-pad: %s", on ? "on" : "off");
    if (save) config::set("Holsters", "WheelStickDpad", on ? "1" : "0");
}
bool hand_quad(float pos[3], float orient[4], float rect[4]) {
    if (!g_open.load(std::memory_order_relaxed) || !g_at_hand.load(std::memory_order_relaxed)) return false;
    std::lock_guard lock(g_quad_mutex);
    std::memcpy(pos, g_centre, 12);
    std::memcpy(orient, g_head_q, 16);
    std::memcpy(rect, g_rect, 16);
    return true;
}
void cancel() {
    end_pause("the controllers' pad stopped (the menu, or not in use)");  // (nothing while not paused)
    if (g_ctrl < 0) return;
    xinput::set_override(0, false, 0, 0, 0);
    g_ctrl = -1;
    g_open = false;
    publish();
    g_cancels.fetch_add(1, std::memory_order_relaxed);
    log::info("[wheel] cancelled: the controllers' pad stopped (the menu, or not in use)");
}

void poll(double now, const bool free[2], bool taken[2]) {
    taken[0] = taken[1] = false;
    const bool pressed[2] = {free[0] && !g_free_was[0], free[1] && !g_free_was[1]};
    g_free_was[0] = free[0];
    g_free_was[1] = free[1];
    collect(now);  // [Holsters] WheelPause: the warp natives' results taken
    if (g_pend_ctrl >= 0) {  // a commit: the gun hand moves once the game has changed the weapon (the review's finding)
        RdrvrActorState st{};
        if (api::actor_state(&st) && st.weapon != g_pend_weapon) {
            controls::set_draw_hand(g_pend_ctrl, -1, true);
            g_pend_ctrl = -1;
        } else if (now > g_pend_until) {
            g_pend_ctrl = -1;
        }
    }
    if (!g_mode.load(std::memory_order_relaxed)) {
        end_pause("the mode off (the holsters)");
        if (g_ctrl >= 0) {  // switched off while open: let go at once
            xinput::set_override(0, false, 0, 0, 0);
            g_ctrl = -1;
            g_open = false;
            publish();
        }
        return;
    }
    const int grips = g_grips.load(std::memory_order_relaxed);
    if (g_ctrl < 0 && now >= g_release_ms) {
        for (int c = 0; c < 2; ++c)
            if (pressed[c] && (grips & (1 << c))) {
                begin(c, now);
                g_pause_done = false;  // [Holsters] WheelPause: this open may pause once
                break;
            }
    }
    if (g_ctrl >= 0) {
        taken[g_ctrl] = true;
        if (g_paused) pause_guard();  // the game closed its wheel, the actor, mounting, Dead Eye, a grab, the option off
        const bool held = free[g_ctrl] && !g_releasing;
        if (held) {
            steer();
            xinput::set_override(XINPUT_GAMEPAD_LEFT_SHOULDER, true, g_stick[0], g_stick[1], kRefreshMs);
            // [Holsters] WheelPause: started once the game shows its wheel (its open timer runs on the scaled dt), then
            // renewed as a lease while the grip holds
            if (g_paused) lease(now);
            else if (!g_pause_done && g_pause_opt.load(std::memory_order_relaxed) && now - g_open_ms >= kMinHoldMs) try_start(now);
        } else if (now - g_open_ms < kMinHoldMs) {  // let go too soon: LB held to the minimum, then a cancel (no holster tap)
            g_releasing = true;
            g_sector = -1;
            xinput::set_override(XINPUT_GAMEPAD_LEFT_SHOULDER, true, 0, 0, kRefreshMs);
        } else {
            // LB let go: the stick kept (the game equips the highlight at the release), then let go too
            const bool commit = g_sector >= 0 && !g_releasing;
            xinput::set_override(0, true, commit ? g_stick[0] : 0.0f, commit ? g_stick[1] : 0.0f, static_cast<uint32_t>(kStickTailMs));
            g_last_sector.store(commit ? g_sector : -1, std::memory_order_relaxed);
            if (commit) {
                g_commits.fetch_add(1, std::memory_order_relaxed);
                RdrvrActorState st{};  // the new weapon into the gripping hand, once the game has changed it
                g_pend_ctrl = g_ctrl;
                g_pend_weapon = api::actor_state(&st) ? st.weapon : -2;
                g_pend_until = now + 1500.0;
                log::info("[wheel] let go: the sector %d (%d degrees from the hand's right), the weapon into the %s hand", g_sector, g_sector * 45,
                          g_ctrl ? "right" : "left");
            } else {
                g_cancels.fetch_add(1, std::memory_order_relaxed);
                log::info("[wheel] let go in the dead zone (or too soon): cancelled");
            }
            // [Holsters] WheelPause: LB's release reaches the game at its next pad poll, the cancel at the next script
            // tick: within a frame of each other, so the draw (the game equips at the release) plays at full speed
            end_pause(commit ? "a pick" : "no pick (the dead zone)");
            g_ctrl = -1;
            g_open = false;
            g_release_ms = now + kStickTailMs + 100.0;  // no new wheel until the stick is let go
        }
    }
    if (g_ctrl < 0) end_pause("the wheel closed");  // (a backstop: every close above ends it first)
    publish();
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, w;
    in >> c;
    while (in >> w) {
        if (w == "on" || w == "off") set_wheel_mode(w == "on", false);
        if (w == "reset") g_opens = g_commits = g_cancels = 0, g_pauses = g_leases = g_pause_ends = g_set_calls = g_cancel_calls = 0;
        if (w == "pause") {  // wheel pause on|off | scale <f> | hold: [Holsters] WheelPause / WheelPauseScale for the session
            std::string v;
            if (in >> v) {
                if (v == "on" || v == "off") set_wheel_pause(v == "on", false);
                if (v == "scale") {
                    float f;
                    if (in >> f) set_wheel_pause_scale(f, false);
                }
                if (v == "hold") g_lease_hold = true;  // test only: this pause's renewals stop (the lease's own end)
            }
        }
        if (w == "dpad") {  // wheel dpad on|off: [Holsters] WheelStickDpad for the session
            std::string v;
            if (in >> v) set_stick_dpad(v != "off", false);
        }
        if (w == "angle") {
            float a;
            if (in >> a) g_angle = a;
        }
        if (w == "athand") {
            std::string v;
            in >> v;
            g_at_hand = v != "off";
        }
        if (w == "flipy") {
            std::string v;
            in >> v;
            g_flip_y = v == "on";
        }
    }
    char b[360];
    std::snprintf(b, sizeof(b),
                  "wheel mode %d, open %d (the %s grip), the stick (%.2f %.2f), sector %d, the hand %.3f m from the centre | opens %llu, commits %llu, "
                  "cancels %llu, the last sector %d | dead zone %.2f, angle %+.0f, flip y %d",
                  g_mode.load() ? 1 : 0, g_open.load() ? 1 : 0, g_ctrl_pub.load() == 1 ? "right" : g_ctrl_pub.load() == 0 ? "left" : "no",
                  g_stick_pub[0].load(), g_stick_pub[1].load(), g_sector_pub.load(),
                  g_last_off.load(), static_cast<unsigned long long>(g_opens.load()), static_cast<unsigned long long>(g_commits.load()),
                  static_cast<unsigned long long>(g_cancels.load()), g_last_sector.load(), g_dead.load(), g_angle.load(), g_flip_y.load() ? 1 : 0);
    // [Holsters] WheelPause: the controller and the game's time scale read now (read only; -1: not readable)
    Warp cw;
    float ts = -1.0f;
    const bool wok = warp_state(&cw);
    if (anchors::stand_down() || !rd(anchors::addr(anchors::Id::GlobalTimeScale), &ts)) ts = -1.0f;
    char p[420];
    std::snprintf(p, sizeof(p),
                  " | pause %d (scale %.3f), paused %d, hold %d, warp active %d ours %d target %.3f ramp %.1f, time scale %.3f, shown %d | "
                  "pauses %llu, leases %llu, ends %llu, sets %llu, cancel calls %llu, the last end: %s",
                  g_pause_opt.load() ? 1 : 0, g_pause_scale.load(), g_paused_pub.load() ? 1 : 0, g_lease_hold.load() ? 1 : 0,
                  wok ? (cw.active ? 1 : 0) : -1, wok && is_ours(cw) ? 1 : 0, wok ? cw.target : -1.0f, wok ? cw.ramp : -1.0f, ts, wheel_shown(),
                  static_cast<unsigned long long>(g_pauses.load()),
                  static_cast<unsigned long long>(g_leases.load()), static_cast<unsigned long long>(g_pause_ends.load()),
                  static_cast<unsigned long long>(g_set_calls.load()), static_cast<unsigned long long>(g_cancel_calls.load()), g_end_why.load());
    return std::string(b) + " | stick dpad " + (g_stick_dpad.load() ? "1" : "0") + ", steps " + std::to_string(controls::wheel_steps()) + p;
}

}  // namespace rdrvr::wheel
