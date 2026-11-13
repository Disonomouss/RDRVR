#include "core/pose.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "core/aim.h"
#include "core/api.h"
#include "core/camera_lever.h"
#include "core/deadeye.h"
#include "core/prepass_arrays.h"
#include "core/config.h"
#include "core/controls.h"
#include "core/d3d_hooks.h"
#include "core/holster.h"
#include "core/log.h"
#include "core/menu.h"
#include "core/state.h"
#include "core/wheel.h"
#include "core/vr_mode.h"
#include "core/reload.h"
#include "core/xinput.h"
#include "core/xr.h"
#include "core/xr_blit.h"

namespace rdrvr::pose {
namespace {

std::atomic<bool> g_anchor{false}, g_recentre{false};
std::atomic<bool> g_keep_cam{true};  // [Body] KeepAnchorCamera: the anchor's camera kept current (a shop's took it)
// presenting thread
bool g_active = false, g_have_heading = false, g_snapped = false;
std::atomic<bool> g_active_shared{false};
std::atomic<float> g_heading_shared{0.0f};
// headset round 5: the eye height follows the head bone's height over the root (crouching, riding), low-passed so the
// walk bob stays out ([Body] EyeHeightFollow, HeightSmoothing seconds)
bool g_height_follow = true;
float g_height_tau = 0.35f, g_height_s = 0.0f;
bool g_height_s_valid = false;
bool g_no_idles = true;  // [Body] NoIdleAnims
bool g_hide_reticle = true;  // [Hands] HideReticle
std::atomic<bool> g_mod_hides_reticle{false};  // the frame's camera job hides the flat reticle (mod_hides_reticle)
float g_heading = 0, g_height = 1.60f, g_lift = 0.05f, g_forward = 0.05f, g_turn_speed = 90, g_snap_angle = 30;
// (GOAL-doorway-and-poker item 3) [Body] TableGameHeadView, off by default: while a minigame runs, the view at John's head
std::atomic<bool> g_table_head{false};
std::atomic<uint64_t> g_table_head_frames{0};
// (round 130, the user on a turning train: "while sitting John does not turn with the train, so if the train turns, the
// camera ends up away from Johns body"; their log: seated, John's heading went 162 -> 157 -> -173 -> -157 deg with the car
// while the view's stayed at 162.4, and his head, 0.86 m behind his place there, was up to 0.53 m to the side of the line
// the view is put on) seated on a train the view's heading turns with the car (by the course his place travels: below),
// and the seated view has a sideways offset too (the job's `right`). [Debug] TrainTurn
std::atomic<bool> g_train_turn{true};
std::atomic<float> g_train_turned{0.0f};      // degrees the view was turned with the car so far
std::atomic<uint32_t> g_train_turn_refused{0};  // the course's steps not taken (too large for a train)
constexpr float kTrainTurnMax = 3.0f;         // degrees a script tick (the smoothed course's step)
std::atomic<bool> g_table_view_active{false};
// (round 142, the review) John's own heading while the view sits at his head: the body is the game's then (no facing
// lock), so the frames built on the torso's heading (the holsters, the belt's props, the elbows' hints) take his
std::atomic<float> g_seat_heading_shared{0.0f};  // the view at the head or easing back (the body seated while it is: body.cpp)
bool g_table_head_was = false;
int g_up_axis = 1;
int g_orient_mode = 1;  // SET_CAMERA_ORIENTATION: 1 = heading on the vector's y (round 1: 0, the heading on z, rolled the view)
bool g_auto_height = true, g_have_scale = false;
float g_measured_head = 0;
double g_last_ms = 0;
bool g_snap = false;
// [Comfort] ThirdPersonSnap (2026-11-06, GOAL-remaining 6c; another player: "Snap turn ... in third person"; off by
// default): with First person off, in VR, a push of the controllers' right stick past 0.7 turns the view by SnapAngle
// at once: the game camera is driven by the stick toward the step (it eases and coasts: r139, so the stick brakes ahead
// of it), and meanwhile the eye views are turned by what it has still to go (camera_lever's view yaw), so the player
// sees one step. The player's own stick is held off the game until it arrives (1.5 s at most). (Round 93 check 3: the
// view's yaw is taken at the render from that frame's camera and turned about John: camera_lever::set_view_yaw_target)
std::atomic<bool> g_tps_cfg{false};
// (round 98, the user: "Works great, add it to the menu with the ability to change how many degrees the snap turns")
// [Comfort] ThirdPersonSnapAngle: the third person step, apart from the first person SnapAngle (10-90 deg)
std::atomic<float> g_tps_angle{30.0f};
bool g_tps_active = false, g_tps_latched = false, g_tps_owns_view = false;  // (owns: the view's yaw is the snap's to clear)
float g_tps_target = 0.0f, g_tps_prev_h = 0.0f;
double g_tps_start_ms = 0.0, g_tps_prev_ms = 0.0, g_tps_fade_ms = -1.0, g_tps_fade_len = 200.0;
// (round 93 check 3) the game camera's place at the last frame: the hold ends once the camera stands still too (SF1: by a
// wall it swings out 0.9 m as it turns), and the ease out is longer the farther it stopped from the held place
float g_tps_prev_p[2] = {};
constexpr double kTpsTimeoutMs = 2500.0;
// (round 96, the user: "Still pulls back slightly", in the open) the camera taken as stopped only nearly still for three
// frames running (was under 0.3 m/s once: it could still be creeping); and each hand-over logged with how far the camera
// went on in the second after it
int g_tps_still_n = 0;
double g_tps_after_ms = -1.0;
float g_tps_after_p[2] = {}, g_tps_after_h = 0.0f, g_tps_after_dm = 0.0f, g_tps_after_dh = 0.0f, g_tps_after_err = 0.0f, g_tps_after_off = 0.0f;
bool g_tps_after_blink = false;
// (round 95, the user: "Still moves back but much less so. You also can't snap turn again until it has moved back") a
// camera stopped well off the held place (by a wall it cannot swing out: 0.9 m at the spawn building's door) is handed
// over in a blink (the eyes faded to black over kBlinkOutMs, the view put back on the camera there, faded in over
// kBlinkInMs) rather than eased across; a little off, the ease as before. A push while a snap holds or eases chains
// another step from its target at once (the held place's start kept)
constexpr double kBlinkOutMs = 70.0, kBlinkInMs = 90.0;
constexpr float kBlinkM = 0.15f, kBlinkDeg = 1.5f;
double g_tps_blink_ms = -1.0;
float g_tps_blink = 0.0f;  // the snap's own fade now (rs_set_fade keeps the larger of it and the room's)
// (the review) the blink's phases: 0 fading out; 1 black, the view put on the camera, held 2 frames (the view's change
// reaches the render a frame after the fade); 2 fading in from g_tps_blink_in_ms
int g_tps_blink_phase = 0, g_tps_blink_black = 0;
double g_tps_blink_in_ms = 0.0;
float rs_fade_now();  // room scale's own fade (below)
std::atomic<uint64_t> g_tps_snaps{0}, g_tps_done{0}, g_tps_timeouts{0}, g_tps_blinks{0}, g_tps_chained{0};
std::atomic<float> g_tps_last_err{0.0f}, g_tps_last_ms{0.0f};
float tps_wrap(float d) { return std::remainder(d, 360.0f); }
void tps_set_blink(float f) {
    g_tps_blink = f;
    xr_blit::set_fade(std::fmax(f, rs_fade_now()));  // (room scale's own writes keep the larger too: rs_set_fade)
}
void tps_stop_blink() {  // the snap stopped: no blink left behind
    if (g_tps_blink_ms >= 0.0 || g_tps_blink > 0.0f) tps_set_blink(0.0f);
    g_tps_blink_ms = -1.0;
}
// one frame of the snap: pad.rx replaced while it runs, the view's yaw offset set
void tps_frame(xinput::PadState& pad, double now) {
    float bx = 0.0f, bz = 0.0f;
    if (!camera_lever::game_heading(&bx, &bz)) {
        g_tps_active = false;
        if (g_tps_owns_view) camera_lever::set_view_yaw_target(false, 0.0f, 0.0f);
        g_tps_owns_view = false;
        g_tps_fade_ms = -1.0;  // (the review: no ease left to turn the view on again unowned)
        tps_stop_blink();
        return;
    }
    const float h = std::atan2(bx, bz) * 57.2957795f;
    const float sx = pad.rx / 32767.0f;
    if (g_tps_after_ms >= 0.0) {  // (round 96) the second after a hand-over: how far the camera went on
        float ax = 0.0f, az = 0.0f;
        if (camera_lever::game_place(&ax, &az))
            g_tps_after_dm = std::fmax(g_tps_after_dm, std::hypot(ax - g_tps_after_p[0], az - g_tps_after_p[1]));
        g_tps_after_dh = std::fmax(g_tps_after_dh, std::fabs(tps_wrap(h - g_tps_after_h)));
        if (now - g_tps_after_ms >= 1000.0 || g_tps_active) {
            log::info("[pose] third person snap handed over %s: the camera %.2f deg short, %.3f m from the held place; in the %s after, it "
                      "moved on %.3f m and turned %.2f deg",
                      g_tps_after_blink ? "in a blink" : "by the ease", g_tps_after_err, g_tps_after_off, g_tps_active ? "time (a new snap)" : "second",
                      g_tps_after_dm, g_tps_after_dh);
            g_tps_after_ms = -1.0;
        }
    }
    if (g_tps_latched && std::fabs(sx) < 0.3f) g_tps_latched = false;
    if (!g_tps_latched && std::fabs(sx) > 0.7f && g_tps_blink_ms < 0.0) {  // (r139: the stick right lowers the heading)
        // (round 95) chained: a snap holding or easing the view goes on from its target, its held place's start kept
        const bool chain = g_tps_active || (g_tps_fade_ms >= 0.0 && g_tps_owns_view);
        const float step = g_tps_angle.load(std::memory_order_relaxed);
        g_tps_target = tps_wrap((chain ? g_tps_target : h) + (sx > 0.0f ? -step : step));
        g_tps_active = g_tps_latched = true;
        g_tps_start_ms = now;
        if (!chain) {
            g_tps_prev_h = h;
            g_tps_prev_ms = now;
        }
        camera_lever::set_view_yaw_target(true, g_tps_target, 1.0f, !chain);
        g_tps_owns_view = true;
        g_tps_snaps.fetch_add(1, std::memory_order_relaxed);
        if (chain) {
            g_tps_chained.fetch_add(1, std::memory_order_relaxed);
            camera_lever::view_yaw_rereference();  // (the review: the held view's moves measured from the new step)
        }
    }
    if (!g_tps_active) {
        if (std::fabs(sx) > 0.3f) pad.rx = 0;  // (the review: a push held on, or one short of 0.7, never turns it smoothly)
        if (g_tps_blink_ms >= 0.0) {  // (round 95) the hand-over in a blink: black, the view put on the camera, back
            const double t = now - g_tps_blink_ms;
            float f = 1.0f;
            if (g_tps_blink_phase == 0 && t < kBlinkOutMs) {
                f = static_cast<float>(t / kBlinkOutMs);
            } else if (g_tps_blink_phase == 0) {  // full black: the view put back on the camera here
                if (g_tps_owns_view) camera_lever::set_view_yaw_target(false, 0.0f, 0.0f);
                g_tps_owns_view = false;
                g_tps_blink_phase = 1;
                g_tps_blink_black = 0;
            } else if (g_tps_blink_phase == 1) {
                if (++g_tps_blink_black >= 2) {
                    g_tps_blink_phase = 2;
                    g_tps_blink_in_ms = now;
                }
            } else {
                f = 1.0f - static_cast<float>((now - g_tps_blink_in_ms) / kBlinkInMs);
            }
            if (f <= 0.0f) {
                tps_stop_blink();
            } else {
                tps_set_blink(f);
            }
            return;
        }
        if (g_tps_fade_ms >= 0.0) {
            const float f = 1.0f - static_cast<float>((now - g_tps_fade_ms) / g_tps_fade_len);
            if (f > 0.0f) {
                camera_lever::set_view_yaw_target(true, g_tps_target, f);
                g_tps_owns_view = true;  // (the review: its yaw cleared by whatever stops the snap)
            } else {
                camera_lever::set_view_yaw_target(false, 0.0f, 0.0f);
                g_tps_owns_view = false;
                g_tps_fade_ms = -1.0;
            }
        }
        return;
    }
    g_tps_fade_ms = -1.0;  // (a new snap begun during the ease: it takes the view)
    const float err = tps_wrap(g_tps_target - h);
    const double dt = (now - g_tps_prev_ms) * 0.001;
    const float vel = dt > 0.002 ? tps_wrap(h - g_tps_prev_h) / static_cast<float>(dt) : 0.0f;  // degrees a second
    float px = 0.0f, pz = 0.0f, pv = 0.0f;  // the game camera's place and its speed (m/s)
    if (camera_lever::game_place(&px, &pz) && dt > 0.002) pv = std::hypot(px - g_tps_prev_p[0], pz - g_tps_prev_p[1]) / static_cast<float>(dt);
    g_tps_prev_p[0] = px;
    g_tps_prev_p[1] = pz;
    g_tps_prev_h = h;
    g_tps_prev_ms = now;
    // (round 94, the user: "a moment after the camera changes, it moves back slightly in the direction it came from": the
    // camera stopped 3 deg short and the ease turned the view back by them) within 1.3 deg (R98: 1 timed out), nearly still; the stick's least
    // push (0.6) still drives it on while more than that is left
    const bool turned = std::fabs(err) < 1.3f && std::fabs(vel) < 4.0f, timeout = now - g_tps_start_ms > kTpsTimeoutMs;
    g_tps_still_n = turned && pv < 0.08f ? g_tps_still_n + 1 : 0;
    if ((g_tps_still_n >= 3 && now - g_tps_start_ms > 50.0) || timeout) {
        g_tps_still_n = 0;
        (timeout ? g_tps_timeouts : g_tps_done).fetch_add(1, std::memory_order_relaxed);
        g_tps_last_err.store(err, std::memory_order_relaxed);
        g_tps_last_ms.store(static_cast<float>(now - g_tps_start_ms), std::memory_order_relaxed);
        g_tps_active = false;
        float md = 0.0f, mm = 0.0f, me = 0.0f;
        camera_lever::view_yaw_target_moves(&md, &mm, &me);
        g_tps_after_blink = me > kBlinkM || std::fabs(err) > kBlinkDeg;
        if (g_tps_after_blink) {  // (round 95) well off: in a blink
            g_tps_blink_ms = now;
            g_tps_blink_phase = 0;
            g_tps_fade_ms = -1.0;
            g_tps_blinks.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_tps_fade_ms = now;  // (SN1: a little short: the view eased onto the camera over 0.2 s, not popped)
            g_tps_fade_len = 200.0;
        }
        g_tps_after_ms = now;
        g_tps_after_p[0] = px;
        g_tps_after_p[1] = pz;
        g_tps_after_h = h;
        g_tps_after_dm = g_tps_after_dh = 0.0f;
        g_tps_after_err = err;
        g_tps_after_off = me;
        pad.rx = 0;
        return;
    }
    if (turned) {  // turned, the camera still moving: the stick let go, the view held
        pad.rx = 0;
        camera_lever::set_view_yaw_target(true, g_tps_target, 1.0f);
        g_tps_owns_view = true;
        return;
    }
    // the stick from what is left to go, less what the camera will still coast (its speed over ~0.25 s), 15 deg a full push
    const float pred = err - vel * 0.25f;
    float u = -pred / 15.0f;
    u = u < -1.0f ? -1.0f : u > 1.0f ? 1.0f : u;
    // (SN1: under 0.4 of a push the game's camera stops 3-4 deg short) at least 0.6 while still on the way
    if (std::fabs(pred) > 0.8f && std::fabs(u) < 0.6f) u = u < 0.0f || (u == 0.0f && pred > 0.0f) ? -0.6f : 0.6f;
    pad.rx = static_cast<int16_t>(u * 32767.0f);
    // (round 93 check 3) the view held at the target and the camera's end place: from each frame's own camera, at the render
    camera_lever::set_view_yaw_target(true, g_tps_target, 1.0f);
    g_tps_owns_view = true;
}
uint64_t g_turns = 0, g_walk_frames = 0;
float g_last_head_yaw = 0;
// riding ([Horse]): steering by the stick (the view faces the horse, the stick steers it) or by the head (as on foot:
// the stick turned by the head's yaw); the saddle anchor (the camera above the horse's own root, the rider's head
// offset from it averaged over about a second, the height smoothed by the plugin)
std::atomic<bool> g_steer_head{false}, g_saddle{true};
std::atomic<bool> g_saddle_climb{true};  // [Horse] SaddleClimb (run 6 item 8): the plugin's climb feed-forward and floor
// [Horse] StickTurn (run 9 item 4, the user: "Turn view with joystick on horse, currently stuck facing forward"): with
// stick steering the right stick turns the view as on foot (snap or smooth), an offset over the mount's heading that the
// plugin adds to the mount's own (the job's `weapons` bit 3: heading_deg is that offset). Mounting starts it at 0 (the
// view faces the horse), recentre clears it; the left stick still steers the horse relative to itself
std::atomic<bool> g_ride_turn{true};
std::atomic<bool> g_vehicle_heading{true};
// run 10 item 8b ([Body] ArmedStance=stand; the user: "When john draws a gun, he changes his stance, bending his knees and
// lowering his height slightly. Can we prevent this?"): measured (run10_armed), the game's armed stance lowers the head
// bone 6 cm (0.60 -> 0.54 m over the root) and bends the knees 167 -> 153 deg; the eye height followed it. Standing (not
// crouching, not riding or driving) with a gun in hand, the eye height is held at its value from before
std::atomic<bool> g_armed_stand{true};
// 2026-10-12 (round 16 check 57, the user: "After putting a gun away John crouches slightly then slowly returns to normal
// posture. John almost crouching when fists are drawn"): held too in the fists' fighting stance (empty hands, the game's
// LT pressed within the last 1.5 s: the guard, a punch's lead, the player's own LT), and after either ends while the head
// is still below the held height (the game's stance easing back after a put-away), at most 8 s (the simulator, r16_stance:
// a gun put back by hand dips the head 10 cm for 6-7 s; the fists' guard lowers it 32 cm while held)
bool armed_cond(const RdrvrActorState& st, double now) {
    if (!g_armed_stand.load(std::memory_order_relaxed) ||
        (st.flags & (RDRVR_ACTOR_CROUCHING | RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING | RDRVR_ACTOR_IN_COVER)))
        return false;
    // (2026-11-04, GOAL-next-items item 1, the review) the knife's and the torch's guard as the fists' (their LT)
    const bool guard_hand = st.weapon < 0 || st.weapon == 22 || st.weapon == 33;
    return reload::is_gun(st.weapon) || (guard_hand && now - controls::last_lt_ms() < 1500.0);
}
// 2026-11-04 (GOAL-next-items item 1, the user: "He still does this with melee, throwables and the lasso"): held too with
// any other item in hand (the knife, the torch, the lasso, a throwable; not the mounted guns 26-28), standing as above,
// but only for the stance's own drop: a deeper one (a kneel to skin or to hogtie, with the knife or the lasso in hand)
// lets the view down with John, and keeps it down (the held height then follows him) until his head is back within the
// stance's 15 cm of the height held before it (the review: compared with the live held height, the hold caught the view
// again 15 cm over the kneeling head once it had followed that far); then held again and eased back up to that height
// (the item's stance leaves the head 6 cm under it: only a rise to the head would leave the view low). Released at 10 cm,
// above the 15 cm it latched at, and never latched again on its release frame (the review: one line for both, a head
// hovering there pumped the view, and a same-frame latch took the lowered height as its reference)
constexpr float kItemHoldMaxM = 0.15f, kItemBackM = 0.10f;
std::atomic<uint64_t> g_hold_items{0}, g_hold_items_deep{0};  // frames held for an item; frames let down (deeper)
bool g_item_down = false;     // let down by a deeper drop, until back up
float g_item_ref = 0.0f;      // the height held before it
float g_item_restore = 0.0f;  // back up: the height the view is eased to (0: none)
std::atomic<bool> g_item_restoring{false};  // g_item_restore set (the still body eases back to its own height meanwhile)
bool g_hold_by_item = false;  // the last hold an item's (its after-window capped as it was)
bool item_cond(const RdrvrActorState& st) {
    return g_armed_stand.load(std::memory_order_relaxed) && st.weapon >= 0 && !reload::is_gun(st.weapon) &&
           (st.weapon < 26 || st.weapon > 28) &&
           !(st.flags & (RDRVR_ACTOR_CROUCHING | RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING | RDRVR_ACTOR_IN_COVER));
}
std::atomic<bool> g_hold_now{false};  // the frame's verdict (the still body reads it: body.cpp)
std::atomic<uint64_t> g_hold_after{0};  // frames held after the stance ended (the head still low)
double g_hold_release_ms = 0.0;
bool armed_hold(const RdrvrActorState& st, float up, float held, double now) {
    bool h = armed_cond(st, now), by_item = false;
    // (the review) a guard's or a gun's own after-window still running: a deep head is its easing back, not a kneel
    const bool other_after = !g_hold_by_item && g_hold_release_ms > 0.0 && now - g_hold_release_ms < 8000.0;
    if (!h && item_cond(st)) {
        bool released = false;
        if (g_item_down && up >= g_item_ref - kItemBackM) {  // stood up again: held, eased back to the height before
            g_item_down = false;
            g_item_restore = g_item_ref;
            released = true;
        }
        const bool deep = up < held - kItemHoldMaxM && !released;
        if (!g_item_down && deep && !other_after) {
            g_item_down = true;
            g_item_ref = held;
            g_item_restore = 0.0f;
        }
        if (g_item_down) {
            g_hold_release_ms = 0.0;  // (not held after it either: the kneel's way down is John's)
            g_hold_items_deep.fetch_add(1, std::memory_order_relaxed);
        } else if (!deep) {  // (deep within another's after-window: left to that window below)
            h = by_item = true;
            g_hold_items.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (!item_cond(st)) {
        g_item_down = false;
        g_item_restore = 0.0f;
    }
    g_item_restoring.store(g_item_restore > 0.0f, std::memory_order_relaxed);
    if (h) {
        g_hold_release_ms = now;
        g_hold_by_item = by_item;
    } else if (g_hold_release_ms > 0.0 && g_armed_stand.load(std::memory_order_relaxed) &&
               !(st.flags & (RDRVR_ACTOR_CROUCHING | RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING | RDRVR_ACTOR_IN_COVER)) &&
               now - g_hold_release_ms < 8000.0 && up < held - 0.005f && !(g_hold_by_item && up < held - kItemHoldMaxM)) {
        h = true;  // the stance easing back: held until the head is back up
        g_hold_after.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_hold_release_ms = 0.0;
    }
    g_hold_now.store(h, std::memory_order_relaxed);
    return h;
}  // [Horse] VehicleHeading (run 10 item 3): driving, the view's heading the vehicle's
float g_ride_off = 0.0f;
double g_pass_since = -1.0;      // (round 142) a passenger since (ms), -1 not
uint64_t g_pass_turns0 = 0;      // the stick's turns then
std::atomic<float> g_ride_off_shared{0.0f};
bool g_ride_snapped = false;
uint64_t g_ride_turns = 0;
// run 6 item 8, "pose trace on|off|reset|dump <file>": one row per script tick while riding (the presenting thread
// appends; the command thread dumps a CSV): the mount's position and heading, the camera, the rider's head and root
// (the seat), the 1 s averages of the head over the root
struct TraceRow {
    double ms;
    uint64_t tick;
    float mount[3], mh, cam[3], head[3], seat[3], h, f;
};
std::mutex g_trace_mutex;
std::vector<TraceRow> g_trace;  // under g_trace_mutex
std::atomic<bool> g_trace_on{false};
uint64_t g_trace_tick = 0;
std::atomic<float> g_saddle_tau{0.15f};
float g_saddle_h = 0.0f, g_saddle_f = 0.0f;
// (round 135) the saddle camera stands at the rider's head as averaged over the mount's root, plus the view's lift and its
// forward offset along the mount's heading: those two, for the still body's place under it (body.cpp); valid only while
// the frame's job carries the saddle
std::atomic<bool> g_job_saddle{false};
std::atomic<float> g_job_saddle_h{0.0f}, g_job_saddle_f{0.0f};
bool g_saddle_valid = false, g_was_riding = false;
uint64_t g_ride_frames = 0;
// the view's steadiness while riding: the camera's height over the horse's root, its change per script tick (RMS)
double g_jit_sum = 0.0, g_jit_abs = 0.0;  // the camera over the horse; the camera's own height
uint64_t g_jit_n = 0, g_jit_tick = 0;
float g_jit_last = 0.0f, g_jit_last_y = 0.0f;

int16_t stick(float v) {
    v = v < -1 ? -1 : v > 1 ? 1 : v;
    return static_cast<int16_t>(v * 32767.0f);
}

std::atomic<bool> g_mounted_fp{false};             // 2026-10-13 (the Gatling probe): first person kept on a mounted gun
std::atomic<bool> g_on_mounted_gun_shared{false};  // a mounted gun in use now (or fired within 1.5 s)
std::atomic<bool> g_mounted_held_shared{false};    // one in hand now (the actor state's weapon 26-28)

// 2026-10-27 ([XR] PhysicalCrouch; the user: "Implement roomscale and physical crouch", research/run27/roomscale.md
// Part C): the user's own crouch crouches John. The drop is the neck's (camera_lever::neck_local: raw LOCAL metres, the
// world scale left out, the user's real body) below its standing reference, less what a lean explains ([Holsters]
// LeanSteady's model, TorsoLength: a bend or a look at the ground is no crouch). The reference is taken at each recentre
// (not while the game says crouched, nor with the drop already over half the crouch's: the previous one kept; a recentre
// of the runtime's own always takes it, its LOCAL space moved) and by the menu's "Set my standing height" (3 s). H, the
// standing eye height, from STAGE where the runtime has one, else [XR] StandingHeight. Crouched from a drop of
// CrouchShare H, standing again at StandShare H (the hysteresis), each held CrouchHoldMs (a dip or a nod is nothing).
// The game's crouch is a press toggle (@FOOT.CROUCH, L3): pressed only on a change of the physical state the game's
// flag (IS_ACTOR_CROUCHING, the plugin's) disagrees with, a 150 ms pulse, verified within 1 s, retried once, then
// left until the next change. The game's own changes (a sprint, a jump, cover, the player's L3) are adopted, never
// fought. Not riding or driving, in cover, under a script's camera, in a menu or the wheel, nor while the player holds
// the left stick click, nor within 1 s of a recentre. While the physical crouch owns John's crouch the view's height is
// held at its standing value (the game's 0.77 m on top of the user's own drop put the eyes near the ground): the
// user's head 1:1. No new native: the press is the game's pad, the flag the plugin's existing one.
// 2026-10-28 (round 37 check 1, the user: "Lags behind player", with CrouchShare at its least): the hold shrinks with
// the depth, CrouchHoldMs just past the threshold down to CrouchQuickHoldMs at kPcQuickShare past it (a real squat is
// there within a tenth of a second; a nod or a bob never is), CrouchHoldMs 150 by default (was 300: the user's head went
// another 0.06-0.17 m down while it ran), and the press goes out in the frame that decides it (it went out a frame late:
// the controllers' pad is mapped at the frame's start, before pc_frame)
std::atomic<bool> g_pc_on{false};
std::atomic<float> g_pc_crouch{0.30f}, g_pc_stand{0.18f}, g_pc_hold{150.0f}, g_pc_quick{50.0f}, g_pc_height_ini{1.65f};
constexpr float kPcQuickShare = 0.40f;  // the hold at CrouchQuickHoldMs this share of the threshold past it
std::atomic<double> g_pc_cal_at{0.0};  // "Set my standing height": the reference taken at that log::now_ms (0 none)
constexpr double kPcVerifyMs = 1000.0, kPcRecentreMs = 1000.0, kPcAfterMs = 2000.0;
constexpr int kPcPulseMs = 150;
constexpr uint16_t kPadL3 = 0x0040;  // XINPUT_GAMEPAD_LEFT_THUMB
// presenting thread
bool g_pc_ref = false;
float g_pc_ref_pos[3] = {0, 0, 0};  // the neck standing (LOCAL, raw metres)
float g_pc_H = 1.65f;
bool g_pc_H_stage = false;
uint32_t g_pc_gen = 0;
uint64_t g_pc_lchg = 0;
double g_pc_recentre_ms = -1e9;
bool g_pc_running = false;  // on and the anchor active at the last frame
bool g_pc_want = false;     // the physical state (after the hold)
double g_pc_since = -1.0;   // the other state's candidate since
double g_pc_edge_cross = -1.0;  // the last edge's candidate since (the press's log: how long after the threshold it went)
float g_pc_edge_hold = 0.0f;    // the hold that edge took (ms)
bool g_pc_press_frame = false;  // a press made this frame (on_frame_end puts L3 on this frame's pad)
int g_pc_pend = -1;         // a press out: the flag it wants (0 standing, 1 crouched), -1 none
double g_pc_pend_ms = 0.0;
int g_pc_tries = 0;
bool g_pc_reeval = false;   // the physical state changed while a press was out: decided once it resolves
bool g_pc_owned = false, g_pc_after = false;
double g_pc_after_ms = 0.0;
float g_pc_own_h = 0.0f, g_pc_stand_h = 0.0f;  // the view's held height; the last standing one
float g_pc_drop = 0.0f;
// published for the status
std::atomic<float> g_pc_drop_pub{0.0f}, g_pc_lean_pub{0.0f}, g_pc_H_pub{1.65f}, g_pc_ystand_pub{0.0f}, g_pc_job_h_pub{0.0f};
std::atomic<int> g_pc_bits_pub{0};  // 1 want, 2 have, 4 owned, 8 a press out, 16 a reference, 32 H from STAGE, 64 the view held
std::atomic<uint64_t> g_pc_presses{0}, g_pc_retries{0}, g_pc_refusals{0}, g_pc_giveups{0}, g_pc_edges{0}, g_pc_refs{0}, g_pc_kept{0},
    g_pc_recentres{0};
std::atomic<const char*> g_pc_why{"none"};  // the last refusal's gate
// the review (2026-10-27): the reference's place in STAGE when it was taken (carried across the runtime's recentre); a
// recentre made crouched with no valid reference to keep waits for the first standing frame; the physical state set
// again, with no edge, after any change of reference
bool g_pc_ref_stage_ok = false;
float g_pc_ref_shift[3] = {0, 0, 0};  // camera_lever::room_shift_total when the reference was taken ([XR] RoomScale)
float g_pc_ref_stage[3] = {0, 0, 0};
bool g_pc_ref_wait = false;  // a reference wanted, taken once the game's flag says standing
bool g_pc_resync = false;    // the reference changed: g_pc_want set to the current state (pc_frame), no press
std::atomic<double> g_pc_cal_fail_ms{0.0};  // "Set my standing height" not taken: when (the menu's message for 8 s)
std::atomic<int> g_pc_cal_fail_why{0};      // 1 the headset never tracked, 2 John crouched
std::atomic<uint64_t> g_pc_cal_refused{0};
constexpr double kPcCalTimeoutMs = 10000.0, kPcUserL3Ms = 500.0, kPcDeferMs = 3000.0;
// the re-review (2026-10-27): the first standing frame after a recentre made crouched takes the reference only with the
// neck near standing too: within half the crouch's drop of the old reference (in STAGE, or in LOCAL when it has not
// moved), else risen a quarter of H above where it was at the recentre (or the player's calibrate)
float g_pc_wait_base_y = 0.0f;  // the neck's LOCAL y at that recentre
uint64_t g_pc_wait_lchg = 0;    // LOCAL's change count then
bool g_pc_wait_old_stage = false, g_pc_wait_old_local = false;
float g_pc_wait_old_stage_y = 0.0f, g_pc_wait_old_local_y = 0.0f;
uint64_t g_pc_ref_lchg = 0;     // LOCAL's change count when the reference was taken (or carried)
uint64_t g_pc_schg = 0;         // STAGE's change count seen (a change: the reference's STAGE place, and H's floor, stale)
// a change of the physical state refused by a gate (a menu, the wheel, the recentre's second, the head untracked): kept
// for kPcDeferMs, pressed once the gate has been open and the state still differs for CrouchHoldMs, dropped if the game
// comes to match by itself
bool g_pc_defer = false;
double g_pc_defer_ms = 0.0, g_pc_defer_ok_since = -1.0;
std::atomic<uint64_t> g_pc_deferred{0}, g_pc_def_pressed{0}, g_pc_def_dropped{0};
float pc_threshold(bool crouch) { return (crouch ? g_pc_crouch.load(std::memory_order_relaxed) : g_pc_stand.load(std::memory_order_relaxed)) * g_pc_H; }
// (2026-10-28) the hold a change to `crouch` takes at the drop now: CrouchHoldMs at the threshold, eased linearly to
// CrouchQuickHoldMs (never over CrouchHoldMs) at kPcQuickShare of the threshold past it (deeper for a crouch, higher for
// a stand). Judged every frame against the time since the threshold was crossed, so a squat that keeps going is taken
// as soon as it is deep enough, and a dip just past it that comes back up is not. (2026-10-29, the review; the
// coordinator's decision, round 39 asks the user) a deep duck held a tenth of a second is a crouch: at 1.33 times the
// threshold the hold is about 65 ms
float pc_hold_ms(bool crouch) {
    const float full = g_pc_hold.load(std::memory_order_relaxed), quick = std::fmin(full, g_pc_quick.load(std::memory_order_relaxed));
    const float thr = pc_threshold(crouch);
    if (!(thr > 0.01f)) return full;
    float t = (crouch ? g_pc_drop - thr : thr - g_pc_drop) / (kPcQuickShare * thr);
    t = !(t > 0.0f) ? 0.0f : t > 1.0f ? 1.0f : t;
    return full + (quick - full) * t;
}
void quat_rows(const float* q, float* r) {  // 3x3 row-major (camera_lever's quat_matrix)
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    r[0] = 1 - 2 * (y * y + z * z), r[1] = 2 * (x * y - z * w), r[2] = 2 * (x * z + y * w);
    r[3] = 2 * (x * y + z * w), r[4] = 1 - 2 * (x * x + z * z), r[5] = 2 * (y * z - x * w);
    r[6] = 2 * (x * z - y * w), r[7] = 2 * (y * z + x * w), r[8] = 1 - 2 * (x * x + y * y);
}
// H from STAGE: never from a crouched eye (the game's flag crouched, or more than half the crouch's drop below the H it
// replaces when that came from STAGE too); the 1.0-2.3 m range logged when it rejects one
bool pc_stage_height(bool crouched, float* out) {
    float h = 0.0f;
    if (!g_pc_on.load(std::memory_order_relaxed) || !xr::standing_eye_height(&h)) return false;
    if (!(h > 1.0f && h < 2.3f)) {
        log::limited("pose.pc.range", 4, "[pose] physical crouch: the floor gives a standing eye height of %.2f m, outside 1.0-2.3: [XR] StandingHeight %.2f m kept",
                     h, g_pc_height_ini.load());
        return false;
    }
    if (crouched || (g_pc_H_stage && h < g_pc_H - 0.5f * g_pc_crouch.load(std::memory_order_relaxed) * g_pc_H)) {
        log::limited("pose.pc.low", 4, "[pose] physical crouch: the floor's eye height %.2f m not taken (%s): %.2f m kept", h,
                     crouched ? "John is crouched" : "well below the last", g_pc_H);
        return false;
    }
    *out = h;
    return true;
}
void pc_take_reference(const float nl[3], const char* why, bool crouched) {
    std::memcpy(g_pc_ref_pos, nl, sizeof(g_pc_ref_pos));
    g_pc_ref = true;
    g_pc_ref_wait = false;
    g_pc_resync = true;
    g_pc_ref_lchg = xr::local_space_changes();
    camera_lever::room_shift_total(g_pc_ref_shift);
    float sp[3], sq[4], r[9];
    g_pc_ref_stage_ok = xr::local_in_stage(sp, sq);
    if (g_pc_ref_stage_ok) {  // the neck in STAGE: P + R(q) n
        quat_rows(sq, r);
        for (int k = 0; k < 3; ++k) g_pc_ref_stage[k] = sp[k] + r[k * 3] * nl[0] + r[k * 3 + 1] * nl[1] + r[k * 3 + 2] * nl[2];
    }
    float h = 0.0f;
    if (pc_stage_height(crouched, &h)) {
        g_pc_H = h;
        g_pc_H_stage = true;
    } else if (!g_pc_H_stage) {
        g_pc_H = g_pc_height_ini.load(std::memory_order_relaxed);
    }
    g_pc_refs.fetch_add(1, std::memory_order_relaxed);
    if (g_pc_on.load(std::memory_order_relaxed))
        log::info("[pose] physical crouch: the standing reference taken (%s): the neck at %.3f m (LOCAL), the standing eye height %.2f m (%s); "
                  "crouched from a drop of %.2f m, standing below %.2f m", why, nl[1], g_pc_H, g_pc_H_stage ? "the floor, STAGE" : "[XR] StandingHeight",
                  pc_threshold(true), pc_threshold(false));
}
// the runtime moved LOCAL: the old reference carried into the new LOCAL through its place in STAGE (false: not possible)
bool pc_carry_reference() {
    float sp[3], sq[4], r[9];
    if (!g_pc_ref_stage_ok || !xr::local_in_stage(sp, sq)) return false;
    quat_rows(sq, r);
    const float d[3] = {g_pc_ref_stage[0] - sp[0], g_pc_ref_stage[1] - sp[1], g_pc_ref_stage[2] - sp[2]};
    for (int k = 0; k < 3; ++k) g_pc_ref_pos[k] = r[k] * d[0] + r[3 + k] * d[1] + r[6 + k] * d[2];  // R^T (s - P)
    g_pc_resync = true;
    g_pc_ref_lchg = xr::local_space_changes();
    camera_lever::room_shift_total(g_pc_ref_shift);
    return true;
}
// every frame, whatever the anchor: the reference at a recentre (the generation the eyes' origin was taken in) or at the
// calibrate's end, and the drop below it (the headset tracked: a reference is never taken from a lost pose)
void pc_reference(double now) {
    float nl[3];
    const bool neck = camera_lever::neck_local(nl) && xr::head_tracked();
    RdrvrActorState st{};
    const bool crouched = api::actor_state(&st) && (st.flags & RDRVR_ACTOR_CROUCHING);
    const bool on = g_pc_on.load(std::memory_order_relaxed);
    if (const uint64_t sc = xr::stage_space_changes(); sc != g_pc_schg) {  // (the re-review) the floor's space reset
        g_pc_schg = sc;
        if (on && (g_pc_ref_stage_ok || g_pc_H_stage))
            log::info("[pose] physical crouch: the STAGE space changed: the reference's place in it dropped, the standing height taken from the floor again");
        g_pc_ref_stage_ok = false;
        g_pc_wait_old_stage = false;
        g_pc_H_stage = false;  // (H itself kept until the floor gives a new one, standing near the reference)
    }
    const uint32_t gen = camera_lever::recentre_gen();
    if (gen != g_pc_gen && neck) {
        g_pc_gen = gen;
        g_pc_recentre_ms = now;
        g_pc_recentres.fetch_add(1, std::memory_order_relaxed);
        const uint64_t lc = xr::local_space_changes();
        const bool moved = lc != g_pc_lchg;  // the runtime's own recentre: the old reference is in a space that moved
        g_pc_lchg = lc;
        bool valid = g_pc_ref && !moved;  // the previous reference still means something in this LOCAL
        if (g_pc_ref && moved && pc_carry_reference()) {
            valid = true;
            if (on) log::info("[pose] physical crouch: the headset's recentre: the standing reference carried across (the floor's frame), now %.3f m", g_pc_ref_pos[1]);
        }
        const float drop = valid ? g_pc_ref_pos[1] - nl[1] : 0.0f;
        if (valid && (crouched || drop > 0.5f * pc_threshold(true))) {
            g_pc_kept.fetch_add(1, std::memory_order_relaxed);
            if (on)
                log::info("[pose] physical crouch: a recentre %s: the standing reference kept (the neck %.2f m below it)",
                          crouched ? "while John is crouched" : "with the head well below standing", drop);
        } else if (crouched) {  // no reference to keep, John crouched: none taken from a crouched head; the first standing frame's
            g_pc_wait_old_stage = g_pc_ref && g_pc_ref_stage_ok;  // the old one compared in STAGE
            g_pc_wait_old_stage_y = g_pc_ref_stage[1];
            g_pc_wait_old_local = g_pc_ref && g_pc_ref_lchg == lc;  // or in LOCAL, not moved since it was taken
            g_pc_wait_old_local_y = g_pc_ref_pos[1];
            g_pc_wait_base_y = nl[1];
            g_pc_wait_lchg = lc;
            g_pc_ref = valid;
            g_pc_ref_wait = true;
            if (on) log::info("[pose] physical crouch: a recentre while John is crouched, no reference to keep: taken once he stands, the head standing");
        } else {
            pc_take_reference(nl, moved ? "the headset's recentre" : "a recentre", crouched);
        }
    }
    if (g_pc_ref_wait && neck && !crouched) {  // (the re-review) the head standing too, not only John
        bool head_near = false;
        float sp[3], sq[4], r[9];
        const float half = 0.5f * pc_threshold(true);
        if (g_pc_wait_old_stage && xr::local_in_stage(sp, sq)) {
            quat_rows(sq, r);
            const float y = sp[1] + r[3] * nl[0] + r[4] * nl[1] + r[5] * nl[2];  // the neck's height in STAGE
            head_near = g_pc_wait_old_stage_y - y < half;
        } else if (g_pc_wait_old_local && xr::local_space_changes() == g_pc_ref_lchg) {
            head_near = g_pc_wait_old_local_y - nl[1] < half;
        } else if (xr::local_space_changes() == g_pc_wait_lchg) {
            head_near = nl[1] - g_pc_wait_base_y >= 0.25f * g_pc_H;  // no old one to compare: the head risen from where it was
        }
        if (head_near) pc_take_reference(nl, "the first standing frame after a recentre made crouched", crouched);
    }
    {  // "Set my standing height": taken at its time with the headset tracked, or dropped after 10 s (a later request kept)
        double cal = g_pc_cal_at.load(std::memory_order_relaxed);
        if (cal > 0.0 && now >= cal) {
            if (crouched) {
                // (the re-review) John crouched (the player sat, and the physical crouch crouched him): a reference taken now
                // would leave him crouched with nothing pressed. Stand him first (L3), then calibrate, seated if so
                if (g_pc_cal_at.compare_exchange_strong(cal, 0.0)) {
                    g_pc_cal_fail_why = 2;
                    g_pc_cal_fail_ms = now;
                    g_pc_cal_refused.fetch_add(1, std::memory_order_relaxed);
                    log::info("[pose] physical crouch: \"Set my standing height\" refused: John is crouched; stand up (L3) first, then set your standing height");
                }
            } else if (neck) {
                if (g_pc_cal_at.compare_exchange_strong(cal, 0.0)) pc_take_reference(nl, "Set my standing height", crouched);
            } else if (now >= cal + kPcCalTimeoutMs && g_pc_cal_at.compare_exchange_strong(cal, 0.0)) {
                g_pc_cal_fail_why = 1;
                g_pc_cal_fail_ms = now;
                log::info("[pose] physical crouch: \"Set my standing height\" not taken: the headset was not tracked for %.0f s", kPcCalTimeoutMs / 1000.0);
            }
        }
    }
    // the floor known only later (STAGE made when the option came on, located 45 frames on): H from it, standing near the reference
    if (on && g_pc_ref && !g_pc_H_stage && neck && !crouched && std::fabs(g_pc_ref_pos[1] - nl[1]) < 0.5f * pc_threshold(true)) {
        float h = 0.0f;
        if (pc_stage_height(false, &h)) {
            g_pc_H = h;
            g_pc_H_stage = true;
            log::info("[pose] physical crouch: the standing eye height from the floor: %.2f m", h);
        }
    }
    if (!g_pc_ref) g_pc_drop = 0.0f;  // (not tracked: the last drop kept, and pc_frame makes no change of it)
    float lean = g_pc_lean_pub.load(std::memory_order_relaxed);
    if (g_pc_ref && neck) {
        float n[3] = {nl[0] - g_pc_ref_pos[0], nl[1] - g_pc_ref_pos[1], nl[2] - g_pc_ref_pos[2]};
        // ([XR] RoomScale, 2026-10-27) a step the room-scale walk took John under is the body's move, not a lean: the
        // origin's shifts since the reference taken off the horizontal (raw LOCAL metres, as the reference)
        float sh[3];
        camera_lever::room_shift_total(sh);
        n[0] -= sh[0] - g_pc_ref_shift[0];
        n[2] -= sh[2] - g_pc_ref_shift[2];
        lean = camera_lever::lean_split(n, holster::torso_length());  // a bend's drop given back
        g_pc_drop = -n[1];
    }
    g_pc_drop_pub.store(g_pc_drop, std::memory_order_relaxed);
    g_pc_lean_pub.store(lean, std::memory_order_relaxed);
    g_pc_H_pub.store(g_pc_H, std::memory_order_relaxed);
    g_pc_ystand_pub.store(g_pc_ref_pos[1], std::memory_order_relaxed);
}
// the gate a press needs (nullptr: open)
const char* pc_gate(const RdrvrActorState& st, bool have_st, double now) {
    if (!have_st) return "no actor state";
    if (st.mount != 0 || (st.flags & (RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING))) return "riding or driving";
    if (st.flags & RDRVR_ACTOR_IN_COVER) return "in cover";
    if (st.flags & RDRVR_ACTOR_CAM_AWAY) return "a script's camera";
    if (controls::game_menu_open() || menu::visible()) return "a menu";
    if (wheel::holder() >= 0) return "the weapon wheel";
    if (controls::left_click_held()) return "the left stick click held";
    if (controls::click_tap_pending()) return "the left stick click's tap";
    if (now - controls::last_user_l3_ms() < kPcUserL3Ms || xinput::pad_l3_age_ms() < kPcUserL3Ms) return "the player's own L3";
    if (!xr::head_tracked()) return "the headset not tracked";
    if (now - g_pc_recentre_ms < kPcRecentreMs) return "a recentre";
    return nullptr;
}
// the player's own L3 (the controllers' mapping or tap, the real gamepad's) after `t` (log::now_ms)
bool pc_user_l3_since(double t, double now) {
    return controls::last_user_l3_ms() > t || now - xinput::pad_l3_age_ms() > t;
}
void pc_press(bool controllers, double now) {
    // the controllers' pad (pad(): after its mapping) from the next frame on, and this frame's own pad too (on_frame_end,
    // g_pc_press_frame: the pad was mapped before pc_frame, so the pulse alone went out a frame late)
    if (controllers) {
        controls::crouch_pulse(kPcPulseMs);
        g_pc_press_frame = true;
    } else {
        xinput::press(kPadL3, kPcPulseMs);  // a gamepad alone: pad 0's own press (the game's next poll)
    }
    g_pc_pend_ms = now;
}
void pc_own(bool on) {
    if (on && !g_pc_owned) g_pc_own_h = g_pc_stand_h > 0.2f ? g_pc_stand_h : g_height_s;  // the standing view's height
    g_pc_owned = on;
}
void pc_reset() {
    g_pc_defer = false;
    g_pc_pend = -1;
    g_pc_reeval = false;
    g_pc_owned = false;
    g_pc_after = false;
    g_pc_since = -1.0;
}
// the presenting thread, the anchor active: the hysteresis and hold, the edge's press, its check and retry
void pc_frame(const RdrvrActorState& st, bool have_st, bool controllers, double now) {
    g_pc_press_frame = false;
    const bool have = have_st && (st.flags & RDRVR_ACTOR_CROUCHING);
    const bool on = g_pc_on.load(std::memory_order_relaxed);
    if (!on) {
        if (g_pc_running) pc_reset();
        g_pc_running = false;
        return;
    }
    if (!g_pc_running) {  // switched on, or the anchor back: the physical state as it is, no edge (no press)
        pc_reset();
        g_pc_want = g_pc_ref && g_pc_drop >= pc_threshold(true);
        if (g_pc_want && have) pc_own(true);
        g_pc_running = true;
        g_pc_resync = false;
    }
    if (g_pc_resync) {  // (the review) a new or carried reference: the physical state as it reads now, no edge (no press)
        g_pc_resync = false;
        g_pc_since = -1.0;
        g_pc_want = g_pc_ref && g_pc_drop >= pc_threshold(true);
        if (g_pc_want && have) pc_own(true);
        else if (!g_pc_want && have && g_pc_pend < 0) g_pc_owned = false;  // John's crouch the game's now: the view follows him
    }
    const bool tracked = xr::head_tracked();
    bool cand = g_pc_want;
    // (the headset off or its tracking lost: the drop is stale, no change of state read from it)
    if (g_pc_ref && tracked) cand = g_pc_want ? g_pc_drop > pc_threshold(false) : g_pc_drop >= pc_threshold(true);
    bool edge = false, fresh = false;  // (fresh: this frame's own change, not a deferred or re-decided one)
    if (cand != g_pc_want) {
        if (g_pc_since < 0.0) g_pc_since = now;
        const float hold = pc_hold_ms(cand);  // (2026-10-28: shorter the deeper past the threshold)
        if (now - g_pc_since >= hold) {
            g_pc_want = cand;
            g_pc_edge_cross = g_pc_since;
            g_pc_edge_hold = hold;
            g_pc_since = -1.0;
            edge = fresh = true;
            g_pc_edges.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        g_pc_since = -1.0;
    }
    if (g_pc_owned && !have && g_pc_pend != 1) {  // John stood, by any means: the ownership ends (the view eased back)
        g_pc_owned = false;
        g_pc_after = true;
        g_pc_after_ms = now;
    }
    if (g_pc_pend >= 0) {
        if ((have ? 1 : 0) == g_pc_pend) {
            log::info("[pose] physical crouch: John %s (%.0f ms after the press)", have ? "crouched" : "stood", now - g_pc_pend_ms);
            g_pc_pend = -1;
        } else if (pc_user_l3_since(g_pc_pend_ms + 1.0, now)) {
            // (the review) the player's own L3 after the press: theirs from here, no retry (it would undo theirs)
            log::info("[pose] physical crouch: the player's own L3 after the press: no retry, the game's state adopted");
            if (g_pc_pend == 1 && !have) g_pc_owned = false;
            if (g_pc_pend == 0 && have) g_pc_owned = false;
            g_pc_pend = -1;
            g_pc_reeval = false;
        } else if (now - g_pc_pend_ms >= kPcVerifyMs) {
            const char* why = pc_gate(st, have_st, now);
            if (g_pc_tries < 2 && !why) {
                pc_press(controllers, now);
                ++g_pc_tries;
                g_pc_retries.fetch_add(1, std::memory_order_relaxed);
                log::info("[pose] physical crouch: the game did not %s within 1 s: pressed again", g_pc_pend ? "crouch" : "stand");
            } else {
                g_pc_giveups.fetch_add(1, std::memory_order_relaxed);
                log::info("[pose] physical crouch: the game did not %s (%s): left until the next change", g_pc_pend ? "crouch" : "stand",
                          why ? why : "twice");
                if (g_pc_pend == 1 && !have) g_pc_owned = false;  // the crouch never came: nothing held
                if (g_pc_pend == 0 && have) g_pc_owned = false;   // John stays crouched: his own, the view follows him
                g_pc_pend = -1;
            }
        }
        if (g_pc_pend < 0 && g_pc_reeval) {
            g_pc_reeval = false;
            edge = true;
        }
    }
    if (edge) g_pc_defer = false;  // (a new change replaces a deferred one)
    if (!edge && g_pc_defer && g_pc_pend < 0) {  // (the re-review) a refused change kept: pressed once its gate opens
        const char* why = pc_gate(st, have_st, now);
        const bool seated = have_st && (st.mount != 0 || (st.flags & (RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING)));
        if (g_pc_want == have) {
            g_pc_defer = false;  // the game came to match by itself
        } else if (pc_user_l3_since(g_pc_defer_ms, now) || seated) {
            // (the third review) the player's own L3 since: theirs is the last word, never undone; or now riding or
            // driving: a change made on foot is not one for the dismount
            g_pc_defer = false;
            g_pc_def_dropped.fetch_add(1, std::memory_order_relaxed);
            log::info("[pose] physical crouch: the deferred %s dropped (%s)", g_pc_want ? "crouch" : "stand", seated ? "riding or driving" : "the player's own L3");
        } else if (now - g_pc_defer_ms >= kPcDeferMs) {
            g_pc_defer = false;
            g_pc_def_dropped.fetch_add(1, std::memory_order_relaxed);
            log::info("[pose] physical crouch: the deferred %s dropped (the gate stayed shut: %s)", g_pc_want ? "crouch" : "stand", why ? why : "-");
        } else if (why) {
            g_pc_defer_ok_since = -1.0;
        } else {
            if (g_pc_defer_ok_since < 0.0) g_pc_defer_ok_since = now;
            if (now - g_pc_defer_ok_since >= g_pc_hold.load(std::memory_order_relaxed)) {
                g_pc_defer = false;
                g_pc_def_pressed.fetch_add(1, std::memory_order_relaxed);
                edge = true;  // decided below as a change now
                log::info("[pose] physical crouch: the deferred %s, its gate open", g_pc_want ? "crouch" : "stand");
            }
        }
    }
    if (!edge) return;
    if (g_pc_pend >= 0) {
        g_pc_reeval = true;  // a press still out: decided when it resolves
        return;
    }
    if (g_pc_want == have) {  // the game agrees already (crouched by L3, or it stood on its own): nothing pressed
        if (have) pc_own(true);
        return;
    }
    if (const char* why = pc_gate(st, have_st, now)) {
        g_pc_refusals.fetch_add(1, std::memory_order_relaxed);
        g_pc_why.store(why, std::memory_order_relaxed);
        if (!g_pc_want) g_pc_owned = false;  // standing up refused: John's crouch is the game's now
        // (the re-review) kept for its gate, unless riding or driving: a crouch made in the saddle is not one for the dismount;
        // (the third review) nor refused for the player's own stick click or L3: that is them changing John themselves
        const bool user_click = controls::left_click_held() || controls::click_tap_pending() ||
                                now - controls::last_user_l3_ms() < kPcUserL3Ms || xinput::pad_l3_age_ms() < kPcUserL3Ms;
        const bool keep = !(have_st && (st.mount != 0 || (st.flags & (RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING)))) && !user_click;
        if (keep && !g_pc_defer) {
            g_pc_defer = true;
            g_pc_defer_ms = now;
            g_pc_defer_ok_since = -1.0;
            g_pc_deferred.fetch_add(1, std::memory_order_relaxed);
        }
        log::info("[pose] physical crouch: %s not pressed (%s)%s", g_pc_want ? "a crouch" : "standing up", why, keep ? ": deferred" : "");
        return;
    }
    if (g_pc_want) pc_own(true);  // the view held from the press (John's head drops as soon as the game takes it)
    pc_press(controllers, now);
    g_pc_pend = g_pc_want ? 1 : 0;
    g_pc_tries = 1;
    g_pc_presses.fetch_add(1, std::memory_order_relaxed);
    if (fresh)  // (2026-10-28) the time from the threshold's crossing to the press, and the hold that depth took
        log::info("[pose] physical crouch: %s (the drop %.2f m of %.2f; L3 pressed %.0f ms after it crossed %.2f m, the hold %.0f ms)",
                  g_pc_want ? "crouching" : "standing up", g_pc_drop, g_pc_H, now - g_pc_edge_cross, pc_threshold(g_pc_want), g_pc_edge_hold);
    else
        log::info("[pose] physical crouch: %s (the drop %.2f m of %.2f; L3 pressed)", g_pc_want ? "crouching" : "standing up", g_pc_drop, g_pc_H);
}

// 2026-10-27 ([XR] RoomScale; the user: "Implement roomscale and physical crouch", research/run27/roomscale.md Part B):
// the headset is the truth for the view. The neck's horizontal offset since recentre (room_neck_offset: a look around
// moves it little), less what a lean explains (camera_lever::lean_split, TorsoLength: a bend walks no one), beyond
// RoomScaleDeadZone starts a walk: John first turned to face it (SET_ACTOR_HEADING: he does not strafe, and from his
// heading a sideways stick walked a 90-degree arc), then the left stick the mod already sends, pointing along the
// offset in the body's frame (x right, z back), RoomScaleMinStick to RoomScaleMaxStick (0.45: a walk, never a run),
// until he is within half the dead zone (at most 0.10 m) of the head along the walk (or a tick ahead of that); then he is put under it
// (TELEPORT_ACTOR, at most 0.22 m, which also stops his stride: his stop after the stick is 1-5 cm or a whole 0.5 m
// stride, unpredictably, r186-r188).
// While it owns the walk (the command, its slide, and 600 ms after the command at least), camera_lever takes John's
// whole horizontal move each tick
// (at most 2.5 m/s) off the head's offset (room_take_frame), so the view moves only with the head and John walks in under
// it; no new walk until his slide is over. The player's own stick always wins (past XInput's 0.24 dead zone: the offset
// kept, nothing taken off). The eyes fade from geometry over RoomScaleMaxOffset +- 0.10 m, held at its top (black),
// whatever the detector says. John blocked (his own speed along the commanded direction over his last 400 ms under
// 0.15 m/s, the stick commanded 600 ms without a break; nothing else judges it): the stick stopped at once (pressing into
// an NPC provokes him), the eyes faded to black over the 0.05-0.20 m past b0, where he stopped (kept at least 0.20 m
// under RoomScaleMaxOffset, so the fade is black before the clamp), eased in over 0.15 s, and the offset clamped at
// RoomScaleMaxOffset only once it is black (RoomScaleFade; never a jump of the view). Left when the head comes back
// inside the dead zone or turns 45 degrees off (along the wall); retried after 1, 2, 4, then 8 s, b0 kept (the fade
// clears only with the head back within it). Only on foot, first person with the head's position, not in cover, under
// a script's camera, in a menu or the wheel, with the head tracked; not falling (John's height changing over 2.5 m/s:
// stairs and slopes pass), not moved with no stick (a train, a wagon, a lasso's drag: over 0.6 m/s, 1 s) nor faster
// than a walk under the command (a stagger, a ragdoll: over 3 m/s); riding and cutscenes keep today's offset,
// unbounded (the actor state has no flag for death or ragdoll: those read as moved, or as blocked). On resume, John
// walks back under the head up to 1 m; beyond that the offset is set again behind a 0.3 s fade. The walk is the game's
// pad; two natives, both as the test channel's `teleport` calls them, with the player's handle the game returned
// (r186-r193): SET_ACTOR_HEADING at a walk's start, TELEPORT_ACTOR at its end (rs_face, rs_snap); John's place the
// plugin's GET_POSITION every tick.
std::atomic<bool> g_rs_on{false}, g_rs_fade_on{true};
// (cycle r159: the game's walk starts between stick 0.30 and 0.35: 0.30 for 1.5 s moved John 0.0 m, 0.35 2.6 m; so
// MinStick is 0.38, its floor too, and MaxStick 0.40-0.60)
// (cycle r163: MaxOffset 0.65 by default: the fade from geometry starts 0.10 m under it, so a 0.5 m step never fades)
// (2026-10-28, round 37 check 6, the user: "Room scale movement ... lag behind the users movements", with the dead zone
// at its least, 0.10: John moved in 10 cm hops 0.7-2 s apart) the dead zone 0.15 by default (was 0.20), its floor 0.05
// (was 0.10); the walk ends within half the dead zone (at most kRsSnapM: 0.10 m, so the dead zone's floor and the snap
// no longer meet); the lean taken out only from a drop past RoomScaleLeanSlack (below); a head moving steadily away
// starts the walk early (RoomScaleStartLead, below)
std::atomic<float> g_rs_dead{0.15f}, g_rs_stop{0.06f}, g_rs_min{0.38f}, g_rs_max{0.45f}, g_rs_max_off{0.65f};
// (2026-10-29, the review) RoomScaleStopRadius as set (0.02-0.40), kept apart from the one in use (g_rs_stop: at most
// 0.04 under the dead zone, 0.02 at the least): a small dead zone never writes a smaller radius to the ini
std::atomic<float> g_rs_stop_cfg{0.06f};
constexpr float kRsMinStickFloor = 0.38f, kRsMaxStickFloor = 0.40f, kRsDeadFloor = 0.05f;
// (2026-10-28) the lean (camera_lever::lean_split, TorsoLength) explains a horizontal move of sqrt(2 L drop): 1 cm under
// the recentre's neck height (a relaxed stance, a step's bob) hid 0.10 m of a step, 2 cm 0.15 m, a second dead zone on
// top of the first. The walk's lean is taken from the drop past RoomScaleLeanSlack (0.02 m: a 15-degree lean's 1.9 cm
// counts as a step, a 30-degree bend's 7.4 cm still explains 0.24 of its 0.275 m); the holsters and the crouch keep the
// whole drop (their own lean_split)
std::atomic<float> g_rs_lean_slack{0.02f};
// (2026-10-28) the early start: the offset's own velocity (user metres a second, eased over kRsVelTauS) along it over
// kRsStartV with the offset past kRsStartShare of the dead zone (and kRsStartOverSnapM past the snap's distance, so the
// walk does not end as it starts) and the offset a RoomScaleStartLead ahead past the dead zone: the walk starts then
// (0.25 s: a 0.4 m step over half a second starts it about 50 ms sooner; 0 = the dead zone alone, as before)
std::atomic<float> g_rs_start_lead{0.25f};
constexpr float kRsStartV = 0.25f, kRsStartShare = 0.6f, kRsStartOverSnapM = 0.03f, kRsVelTauS = 0.06f, kRsNearM = 0.03f;
// (cycle r166, the review: simplified) blocked: John's own speed along the commanded direction over his last 400 ms
// under 0.15 m/s, once the stick has been commanded for 600 ms without a break; nothing else
constexpr float kRsBlockV = 0.15f;
constexpr double kRsBlockWinMs = 400.0, kRsBlockCmdMs = 600.0;
// (cycles r162-r187) John slides on after the stick goes to 0, and no prediction holds: r186-r187's 90 slides were either
// 0.01-0.05 m (he stops at once) or 0.45-0.6 m over about 0.8 s (one more stride), whatever the walk's length, speed or
// time, so a walk ended short strode past the head and the walk back did the same (r187's ping-pong). r188's probe: an
// in-place TELEPORT_ACTOR at the release stopped him dead 5 of 5 times (plain releases: 4 of 15 strode on, to 0.54 m).
// So the walk ends within kRsSnapM of the head along it (or one lead's time ahead of that, kRsLeadS: his posted place
// lags a tick and the native runs at the next) and John is put under the head (rs_snap): at most kRsSnapMaxM from his
// place, under the take-off's 0.125 m a step, so the move is taken off and the view holds; further, at his own place
// (stopped; what is left is the next walk's). Every slide after it is still measured and logged
// (2026-10-28: within half the dead zone, at most kRsSnapM: rs_snap_m)
constexpr float kRsSnapM = 0.10f, kRsSnapMaxM = 0.22f, kRsSnapNearM = 0.11f, kRsLeadS = 0.05f;
float rs_snap_m(float dead) { return std::fmin(kRsSnapM, 0.5f * dead); }
constexpr double kRsSlideHoldMaxMs = 1500.0;  // no new walk while he slides (his speed under 0.1 m/s ends it), at most this
                                              // (unless the offset is 0.1 m past the dead zone)
constexpr float kRsUserStick = 0.24f;  // XInput's left-stick dead zone (7849 / 32767): a resting or drifting stick is not the player's walk
constexpr float kRsResumeMax = 1.0f, kRsFallSpeed = 2.5f, kRsRamp = 0.20f, kRsMoved = 0.6f, kRsStagger = 3.0f,
                kRsFadeLead = 0.20f;
constexpr double kRsTailMs = 600.0, kRsRetryMs = 1000.0, kRsRetryMaxMs = 8000.0,
                 kRsResetHalfMs = 150.0, kRsFallHoldMs = 500.0, kRsMovedHoldMs = 1000.0, kRsUserTailMs = 800.0, kRsFadeRampMs = 150.0;
// presenting thread
bool g_rs_running = false, g_rs_suspended = false, g_rs_walking = false, g_rs_blocked = false, g_rs_reset_done = false;
double g_rs_walk_ms = 0.0, g_rs_cmd_ms = -1e9, g_rs_block_ms = 0.0, g_rs_reset_ms = -1.0;
double g_rs_fall_until = 0.0, g_rs_moved_until = 0.0, g_rs_stagger_until = 0.0, g_rs_tick_ms = 0.0, g_rs_user_ms = -1e9, g_rs_frame_ms = 0.0;
int g_rs_retry_n = 0;
float g_rs_b0 = 0.0f, g_rs_bdir[2] = {0, 0}, g_rs_tick_pos[3] = {0, 0, 0};
float g_rs_fade_cur = 0.0f, g_rs_ux = 0.0f, g_rs_uz = 0.0f;
uint64_t g_rs_tick = 0;
bool g_rs_tick_ok = false;
double g_rs_end_ms = -1e9;         // the walk's last end (the slide after it)
float g_rs_v_ease = 0.0f;          // his speed always (user metres a second, eased over his ticks): the slide's end
double g_rs_cmd_since = 0.0;       // the stick commanded without a break since then (0: not now)
double g_rs_trace_next = 0.0;      // the next trace line of a walk's first 3 s
double g_rs_hold_log_ms = 0.0;     // the next "held back" log line
int g_rs_chk_n = 0;                // the walk's direction checks still to log (at 0.5 s, 1 s)
float g_rs_chk_w[2] = {0, 0}, g_rs_chk_p0[2] = {0, 0};
// (2026-10-28) the offset's eased velocity (the recentred frame, user metres a second) and its last value; the head last
// within kRsNearM of him (each walk's start and end logged from it: the lag the user feels)
float g_rs_vel[2] = {0, 0}, g_rs_vprev[2] = {0, 0};
bool g_rs_vel_ok = false;
double g_rs_near_ms = 0.0;
// (2026-10-28) [XR] BodyFollowsTurn during a walk: the follow's total the origin showed at the last frame; a walk's
// direction (and a block's) turned by its change, so the walk keeps its line in the world while the body comes round
float g_rs_shown_last = 0.0f;
// (2026-10-29, the review) the follow's total the origin showed at the last recentre: the origin's yaw turns about the
// recentre's centre eye, the offset is the neck's (kRsNeckBackM behind it), so a follow of S since then moves the offset
// by (R(-S) - I) of that lever (2 x 0.08 sin(S / 2): 0.16 m at 180 degrees, a walk past the dead zone from turning in
// place, and 0.08 times the turn's rate in the velocity): taken off in rs_frame
float g_rs_shown_rc = 0.0f;
constexpr float kRsNeckBackM = 0.08f;  // camera_lever's kNeck: the neck 0.08 m behind the centre eye (the head's z)
std::atomic<float> g_rs_vout_pub{0.0f};
std::atomic<uint64_t> g_rs_early{0}, g_rs_turned{0};
// (cycle r186) John faces the walk before the stick goes: his heading set to the walk's (SET_ACTOR_HEADING, as `teleport`
// calls it), the stick held at 0 until his posted heading is within kRsFaceTolDeg of it, kRsFaceMaxMs at most
double g_rs_face_until = 0.0;
float g_rs_face_h = 0.0f;
std::atomic<uint64_t> g_rs_faces{0};
constexpr float kRsFaceTurnDeg = 15.0f, kRsFaceTolDeg = 10.0f;
constexpr double kRsFaceMaxMs = 400.0;

float rs_wrap_deg(float a) {
    a = std::fmod(a + 180.0f, 360.0f);
    return (a < 0.0f ? a + 360.0f : a) - 180.0f;
}

// Turns John to face the world direction (wx, wz) (a heading h faces (-sin h, -cos h)) when he faces over
// kRsFaceTurnDeg away from it. (cycle r186: from his standing heading a right step's stick walked him a 90-degree arc,
// his first 0.43 m at 44 degrees from the stick, half way between his forward and his right; the arc's sideways part
// was left over every walk, 9 walks for a 0.5 m step. Facing first, he walks straight)
void rs_face(const RdrvrActorState& st, bool have_st, float wx, float wz, double now) {
    g_rs_face_until = 0.0;
    if (!have_st || st.actor == 0 || !std::isfinite(st.heading_deg) || !(wx * wx + wz * wz >= 0.25f)) return;  // (NaN refused too)
    const float h = std::atan2(-wx, -wz) * 57.29578f;
    if (!std::isfinite(h)) return;
    const float off = rs_wrap_deg(h - st.heading_deg);
    if (std::fabs(off) <= kRsFaceTurnDeg) return;
    uint32_t bits;
    std::memcpy(&bits, &h, 4);
    const uint64_t args[3] = {static_cast<uint32_t>(st.actor), bits, 0};
    api::queue_native(0xECE8520Bu, args, 3, 0, nullptr);  // SET_ACTOR_HEADING(the player, h, 0)
    g_rs_face_until = now + kRsFaceMaxMs;
    g_rs_face_h = h;
    g_rs_faces.fetch_add(1, std::memory_order_relaxed);
    log::info("[pose] room scale: John faces the walk first: heading %.1f -> %.1f (%.0f degrees)", st.heading_deg, h, off);
}

std::atomic<uint64_t> g_rs_snaps{0}, g_rs_snap_stops{0}, g_rs_snap_skips{0};
double g_rs_snap_ms = -1e9;                   // the last snap's time: the tick gates ignore its jump (kRsSnapGateMs)
float g_rs_last_hs = 0.0f, g_rs_last_vy = 0.0f;  // his last tick's speeds (game metres a second): level and vertical
constexpr double kRsSnapGateMs = 250.0, kRsGateWinMs = 100.0;
constexpr float kRsSnapMaxVy = 0.3f;          // (the review of r188) no snap while he rises or falls: a slope, stairs, a climb
// (cycle r188) the walk's end: John put under the head (TELEPORT_ACTOR(the player, &place, 0, 0, 0), as `teleport` calls
// it), which also stops his stride. (ox, oz): the head's offset from his posted place in the world, game metres, at most
// kRsSnapMaxM (cycle r189: the walks ended a lead ahead leave 0.15-0.19 m, over the take-off's 0.125 m a step, and were
// all refused, 26 of 32: he stood that short). The take-off's cap raised to the jump for the snap's window
// (camera_lever::room_allow_step), so the whole jump is taken off and the view holds. Further: at his own posted place
// (stopped only). Over kRsSnapNearM only while he walks toward it (`toward`: the review of r189, no jump of that length
// onto ground he was not walking onto); `stop` a stop only (a recentre). None unless his
// last tick was a level walk (the review: a slope's or a climb's height would be wrong, a stagger or a ragdoll is not a
// walk). True when sent; *place the place
bool rs_snap(const RdrvrActorState& st, bool have_st, float ox, float oz, bool toward, bool stop, float place[3]) {
    if (!have_st || st.actor == 0 || !std::isfinite(st.pos[0]) || !std::isfinite(st.pos[1]) || !std::isfinite(st.pos[2])) return false;
    if (!(g_rs_last_vy < kRsSnapMaxVy) || !(g_rs_last_hs < kRsStagger)) {
        g_rs_snap_skips.fetch_add(1, std::memory_order_relaxed);
        log::info("[pose] room scale: no snap (his last tick %.2f m/s level, %.2f m/s up or down): the walk just ends", g_rs_last_hs, g_rs_last_vy);
        return false;
    }
    const float om = std::sqrt(ox * ox + oz * oz);
    const bool under = !stop && std::isfinite(om) && om <= (toward ? kRsSnapMaxM : kRsSnapNearM);
    const float p[3] = {st.pos[0] + (under ? ox : 0.0f), st.pos[1], st.pos[2] + (under ? oz : 0.0f)};
    const uint64_t args[5] = {static_cast<uint32_t>(st.actor), 0, 0, 0, 0};
    api::queue_native(0x2D54B916u, args, 5, 2, p);  // TELEPORT_ACTOR(the player, &p, 0, 0, 0): args[1] the place
    (under ? g_rs_snaps : g_rs_snap_stops).fetch_add(1, std::memory_order_relaxed);
    g_rs_snap_ms = log::now_ms();
    camera_lever::room_allow_step((under ? om : 0.0f) + 0.10f, g_rs_snap_ms + kRsSnapGateMs);  // (a tick's walk on top)
    std::memcpy(place, p, sizeof(p));
    log::info("[pose] room scale: John %s (%.3f game m, the move %.3f %.3f)", under ? "put under the head" : stop ? "stopped where he is (a recentre)" : "stopped where he is: the head too far to put him under",
              om, under ? ox : 0.0f, under ? oz : 0.0f);
    return true;
}
std::atomic<uint64_t> g_rs_rewalks{0};  // walks started within 4 s of the last one's end (stopped short: the next one)
// [XR] RoomScaleDirect (on; 2026-11-02, GOAL-older-items item 1; the user: room scale "still too laggy"; their log of
// 10-30: each 5-8 cm step began 80-130 ms after the head left him, turned him 20-160 degrees, walked 70-150 ms and ended
// in the snap: a quarter of a second, John spinning to face each): a step up to kRsDirectM (user metres) put under the
// head at once (the walk end's snap: no turn, no walk) while a world probe (CAMERA_PROBE from his chest along the step,
// kRsDirectPadM past it) is clear, so no wall is stepped through; else the walk as before. The probe each tick while the
// offset is past half the dead zone, its answer used while under kRsDirectProbeMs old and within 20 degrees of the step
// (cycles N22-N25: his teleport moved him but its move was not taken off the head's offset (the room did not own it); with
// that, a 0.10 and a 0.18 m step put under the head 33-35 ms after it left him, no walk, no turn, against the walk's 187-198
// ms and its 90-180 degree turn)
std::atomic<bool> g_rs_direct{true};
// (the review: the probe at 0.6 m over his feet, under rails, tables and low fences, not at his chest)
constexpr float kRsDirectM = 0.20f, kRsDirectPadM = 0.30f, kRsDirectChestM = 0.6f;
constexpr double kRsDirectProbeMs = 150.0;
uint64_t g_rsd_req = 0;
double g_rsd_req_ms = 0.0, g_rsd_ms = -1e12;
float g_rsd_req_dir[2] = {}, g_rsd_dir[2] = {};
bool g_rsd_clear = false;
double g_rsd_hold_until = 0.0;  // (the review) after a direct step, no walk nor step until his teleport has landed
// (cycle N21: put into something the probe missed, the game kept him where he was: the same 0.084 m step every 250 ms)
// a step that did not bring him nearer (the offset after it over 0.7 of the one it took) stops the direct steps until the
// head is back near him; the walk then, with its blocked judgement and the fade
float g_rsd_last_dist = 0.0f;
float g_rsd_pos0[3] = {}, g_rsd_want_m = 0.0f;  // (cycle N24) his place at the step and how far it sent him (game metres)
bool g_rsd_pending_check = false, g_rsd_off = false;
// (the review) the room's ownership of the take-off set a frame before the teleport is sent (camera_lever seeds it on a
// frame already owned: a teleport seen in the seed's frame would never be taken off)
bool g_rsd_armed = false;
double g_rsd_arm_ms = 0.0;
std::atomic<uint64_t> g_rsd_noeffect{0};
std::atomic<uint64_t> g_rsd_steps{0}, g_rsd_blocked{0}, g_rsd_probes{0};
uint64_t f32bits_rs(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}
uint64_t pack2_rs(float a, float b) { return f32bits_rs(a) | (f32bits_rs(b) << 32); }
// his place each tick, the last 64 (the blocked judgement's 400 ms, the stop's 200 ms): log::now_ms and x, z
struct RsTick {
    double ms;
    float x, z, y;
};
RsTick g_rs_ring[64];
int g_rs_ring_n = 0, g_rs_ring_head = 0;
// his speed along `wd` (world, unit) over the last `window_ms` of ticks: the oldest tick at or after now - window_ms
// against the newest; 0 with under two ticks; *dt the span (s), p0 the older place
float rs_ring_speed(const float* wd, double window_ms, double now, double* dt, float* p0);
// (cycle r183) his velocity over the last `window_ms` of ticks as a vector (the world's x and z, game metres a second):
// RDR's John does not strafe, he turns toward the stick and walks an arc, so his motion is judged whole, not along u
void rs_ring_vel(double window_ms, double now, double* dt, float* v) {
    const float ex[3] = {1, 0, 0}, ez[3] = {0, 0, 1};
    float p0[3];
    v[0] = rs_ring_speed(ex, window_ms, now, dt, p0);
    v[1] = rs_ring_speed(ez, window_ms, now, dt, p0);
}
// (cycle r192) his level and vertical speed from the ring's place at least `window_ms` before `now` to `pos` (game metres
// a second), false while the ring holds no tick that old. The tick's own speed is over the render frames that saw two
// ticks, 11-13 ms at the game's frame-rate ticks: a few millimetres of his idle sway read over 0.6 m/s, "moved with no
// stick", and the room suspended while he stood (r192: the fade cleared at 0.73 m past the wall)
bool rs_ring_speed_to(const float* pos, double now, double window_ms, float* hs, float* vy) {
    const RsTick* old = nullptr;
    for (int k = 0; k < g_rs_ring_n; ++k) {  // from the newest back: the first at least window_ms old
        const RsTick& t = g_rs_ring[(g_rs_ring_head + 63 - k) % 64];
        if (t.ms <= now - window_ms) {
            old = &t;
            break;
        }
    }
    if (!old) return false;
    const double dt = (now - old->ms) / 1000.0;
    if (dt <= 0.0 || dt > 0.5) return false;
    const float mx = pos[0] - old->x, mz = pos[2] - old->z;
    *hs = static_cast<float>(std::sqrt(mx * mx + mz * mz) / dt);
    *vy = static_cast<float>(std::fabs(pos[1] - old->y) / dt);
    return true;
}
float rs_ring_speed(const float* wd, double window_ms, double now, double* dt, float* p0) {
    *dt = 0.0;
    if (g_rs_ring_n < 2) return 0.0f;
    const RsTick& nw = g_rs_ring[(g_rs_ring_head + 63) % 64];
    const RsTick* old = nullptr;
    for (int k = g_rs_ring_n - 1; k >= 1; --k) {  // from the oldest kept forward
        const RsTick& t = g_rs_ring[(g_rs_ring_head + 64 - 1 - k) % 64];
        if (t.ms >= now - window_ms) {
            old = &t;
            break;
        }
    }
    if (!old) old = &g_rs_ring[(g_rs_ring_head + 62) % 64];  // (none in the window: the tick before the newest)
    *dt = (nw.ms - old->ms) / 1000.0;
    p0[0] = old->x, p0[1] = 0.0f, p0[2] = old->z;
    if (*dt <= 0.0) return 0.0f;
    return static_cast<float>(((nw.x - old->x) * wd[0] + (nw.z - old->z) * wd[2]) / *dt);
}
bool g_rs_slide_on = false;        // a slide being measured: from the walk's end to his speed under 0.1 m/s
float g_rs_slide_p0[3] = {0, 0, 0}, g_rs_slide_v0 = 0.0f;
double g_rs_slide_ms = 0.0;
std::atomic<float> g_rs_slide_pub{0.0f}, g_rs_slide_v_pub{0.0f}, g_rs_slide_t_pub{0.0f}, g_rs_fade_geo_pub{0.0f};
std::atomic<uint64_t> g_rs_slides{0};
float g_rs_last_m = 0.0f;          // the last commanded stick's length (the trace)
uint32_t g_rs_gen = 0;
int g_rs_reset_black = -1;      // the re-anchor: -1 the fade rising, 0..4 its black frames (the shift at 2), then falling
double g_rs_reset_fall_ms = 0.0;
// published for the status
std::atomic<float> g_rs_d_pub{0.0f}, g_rs_dx_pub{0.0f}, g_rs_dz_pub{0.0f}, g_rs_lx_pub{0.0f}, g_rs_ly_pub{0.0f}, g_rs_fade_pub{0.0f},
    g_rs_speed_pub{0.0f}, g_rs_lean_pub{0.0f}, g_rs_b0_pub{0.0f}, g_rs_hs_pub{0.0f};
std::atomic<int> g_rs_bits_pub{0};  // 1 running, 2 walking, 4 blocked, 8 owns, 16 the player's stick, 32 a reset fade, 64 suspended, 128 judged (600 ms commanded)
std::atomic<const char*> g_rs_why{"none"};  // the gate shut now ("none": open)
std::atomic<uint64_t> g_rs_walks{0}, g_rs_blocks{0}, g_rs_retries{0}, g_rs_resets{0}, g_rs_clamps{0}, g_rs_user_frames{0}, g_rs_cmd_frames{0};
std::atomic<uint64_t> g_rs_moved{0}, g_rs_staggers{0};
struct RsCmd {
    bool on = false;
    float lx = 0.0f, ly = 0.0f;  // -1..1, the body's frame (x right, y forward)
};
void rs_set_fade(float f) { xr_blit::set_fade(std::fmax(f, g_tps_blink)); }  // (round 95: the snap's blink kept)
float rs_fade_now() { return g_rs_fade_pub.load(std::memory_order_relaxed); }  // (cycle r159: written every frame, not on a change only: never a fade left behind)
void rs_clear_fade() {  // the option off, a gate, the anchor gone: exactly 0, every frame, and no easing or start kept
    g_rs_fade_cur = 0.0f;
    g_rs_b0 = 0.0f;
    g_rs_fade_pub.store(0.0f, std::memory_order_relaxed);
    g_rs_b0_pub.store(0.0f, std::memory_order_relaxed);
    rs_set_fade(0.0f);
}
void rs_idle(bool suspend) {  // nothing commanded, nothing taken off (the offset kept as it is)
    g_rsd_pending_check = g_rsd_armed = false;  // (the review: a direct step's check not judged across a pause)
    if (g_rs_running && suspend) g_rs_suspended = true;
    g_rs_running = false;
    g_rs_walking = false;
    g_rs_blocked = false;
    g_rs_b0 = 0.0f;
    g_rs_fade_cur = 0.0f;
    g_rs_retry_n = 0;
    g_rs_cmd_ms = -1e9;
    camera_lever::room_set_owns(false);
}
float bt_pending();  // [XR] BodyFollowsTurn (below): the follow's part the camera does not show yet (degrees)
const char* rs_gate(const RdrvrActorState& st, bool have_st, double now) {
    if (!have_st) return "no actor state";
    if (!camera_lever::eyes_follow_head()) return "the head's position off";
    if (st.mount != 0 || (st.flags & (RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING))) return "riding or driving";
    if (st.flags & RDRVR_ACTOR_IN_COVER) return "in cover";
    if (st.flags & RDRVR_ACTOR_CAM_AWAY) return "a script's camera";
    if (controls::game_menu_open() || menu::visible()) return "a menu";
    if (wheel::holder() >= 0) return "the weapon wheel";
    if (!xr::head_tracked()) return "the headset not tracked";
    if (now < g_rs_fall_until) return "falling";
    if (now < g_rs_moved_until) return "moved with no stick (a vehicle, a drag)";
    if (now < g_rs_stagger_until) return "faster than a walk (a stagger)";
    return nullptr;
}
// the frame's end for the room scale: who owns the walk (the take-off), the re-anchor's fade, the fade, the status
void rs_finish(bool owns, float fade, float dx, float dz, float dist, float ws, float user, double now) {
    camera_lever::room_set_owns(owns);
    if (g_rs_reset_ms >= 0.0) {
        // the re-anchor's fade (the review): up to black over 0.15 s; two frames fully black; the shift queued (applied at
        // the next frame's start); two more black frames; back over 0.15 s. The view never seen to move
        float rf = 1.0f;
        if (g_rs_reset_black < 0) {
            rf = static_cast<float>((now - g_rs_reset_ms) / kRsResetHalfMs);
            if (rf >= 1.0f) {
                rf = 1.0f;
                g_rs_reset_black = 0;
            }
        } else if (g_rs_reset_black < 4) {
            if (++g_rs_reset_black == 2 && !g_rs_reset_done) {
                camera_lever::room_shift_origin(dx / ws, dz / ws);
                g_rs_reset_done = true;
                g_rs_resets.fetch_add(1, std::memory_order_relaxed);
                log::info("[pose] room scale: the offset (%.2f m) set again behind a fade (beyond %.1f m on resume)", dist, kRsResumeMax);
            }
            if (g_rs_reset_black == 4) g_rs_reset_fall_ms = now;
        } else {
            rf = 1.0f - static_cast<float>((now - g_rs_reset_fall_ms) / kRsResetHalfMs);
            if (rf <= 0.0f) {
                rf = 0.0f;
                g_rs_reset_ms = -1.0;
            }
        }
        fade = std::fmax(fade, std::fmin(1.0f, std::fmax(0.0f, rf)));
    }
    rs_set_fade(fade);
    g_rs_fade_pub.store(fade, std::memory_order_relaxed);
    g_rs_b0_pub.store(g_rs_b0, std::memory_order_relaxed);
    const int bits = (g_rs_running ? 1 : 0) | (g_rs_walking ? 2 : 0) | (g_rs_blocked ? 4 : 0) | (owns ? 8 : 0) | (user > kRsUserStick ? 16 : 0) |
                     (g_rs_reset_ms >= 0.0 ? 32 : 0) | (g_rs_suspended ? 64 : 0) |
                     (g_rs_cmd_since > 0.0 && now - g_rs_cmd_since >= kRsBlockCmdMs ? 128 : 0);
    g_rs_bits_pub.store(bits, std::memory_order_relaxed);
}
// the frame's room-scale command (pose's on_frame_end, the anchor active, before the stick's head turn): `user` the
// player's own left stick's length (the controllers' or the gamepad's)
RsCmd rs_frame(const RdrvrActorState& st, bool have_st, float user, double now) {
    RsCmd c;
    g_rs_lx_pub.store(0.0f, std::memory_order_relaxed);  // (the published stick: 0 unless commanded below)
    g_rs_ly_pub.store(0.0f, std::memory_order_relaxed);
    const bool on = g_rs_on.load(std::memory_order_relaxed);
    double fdt = (now - g_rs_frame_ms) / 1000.0;
    g_rs_frame_ms = now;
    fdt = fdt < 0.0 ? 0.0 : fdt > 0.1 ? 0.1 : fdt;
    if (user > kRsUserStick) g_rs_user_ms = now;
    if (have_st && st.tick != g_rs_tick) {  // John per script tick: a fall, a move with no stick, a stagger
        const double dt = (now - g_rs_tick_ms) / 1000.0;
        if (g_rs_tick_ok && dt > 0.005 && dt < 0.5) {
            const float hx = st.pos[0] - g_rs_tick_pos[0], hz = st.pos[2] - g_rs_tick_pos[2];
            const float hs = std::sqrt(hx * hx + hz * hz) / static_cast<float>(dt), vy = std::fabs(st.pos[1] - g_rs_tick_pos[1]) / static_cast<float>(dt);
            g_rs_hs_pub.store(hs, std::memory_order_relaxed);
            // (cycle r192) the gates and the snap judge his speed over 100 ms of ticks (none while the ring is younger)
            float hs_w = 0.0f, vy_w = 0.0f;
            const bool have_w = rs_ring_speed_to(st.pos, now, kRsGateWinMs, &hs_w, &vy_w);
            g_rs_last_hs = have_w ? hs_w : hs;
            g_rs_last_vy = have_w ? vy_w : vy;
            const bool snapped = now - g_rs_snap_ms < kRsSnapGateMs;  // (the review of r188: the snap's own jump is no stagger nor fall)
            // TODO (the review of r188): user metres are game metres times WorldScale (room_shift_origin is given user / ws),
            // so this, v_now_u, v_along and the logged slide divide where they should multiply: right only at WorldScale 1
            // (the default); fix them before RoomScale is tested with another WorldScale or made a default
            const float wsc = camera_lever::world_scale(), hu = hs / (wsc > 0.01f ? wsc : 1.0f);  // (user metres)
            g_rs_v_ease += (hu - g_rs_v_ease) * 0.5f;  // (a tick that happens not to move him is not his stop: r164's 50 ms slides)
            const bool timed_out = now - g_rs_slide_ms > 2000.0;
            if (g_rs_slide_on && (g_rs_v_ease < 0.1f || timed_out)) {  // the slide over: measured
                g_rs_slide_on = false;
                const float sx = st.pos[0] - g_rs_slide_p0[0], sz = st.pos[2] - g_rs_slide_p0[2];
                const float sl = std::sqrt(sx * sx + sz * sz) / (wsc > 0.01f ? wsc : 1.0f);
                const float t = g_rs_slide_v0 > 0.05f ? sl / g_rs_slide_v0 : 0.0f;
                const double over_ms = now - g_rs_slide_ms;
                g_rs_slide_pub.store(sl, std::memory_order_relaxed);
                g_rs_slide_v_pub.store(g_rs_slide_v0, std::memory_order_relaxed);
                g_rs_slide_t_pub.store(t, std::memory_order_relaxed);
                g_rs_slides.fetch_add(1, std::memory_order_relaxed);
                log::info("[pose] room scale: the slide after the stick %.3f m from %.2f m/s (%.3f s of that speed), over in %.0f ms%s; "
                          "the lead %.2f s",
                          sl, g_rs_slide_v0, t, over_ms, timed_out ? " (the 2 s limit)" : "", kRsLeadS);
            }
            // (cycle r184: his own slide after the walk is no move "with no stick": the slide's time counts as commanded)
            const bool commanded = now - g_rs_cmd_ms < kRsTailMs || (g_rs_slide_on && now - g_rs_end_ms < kRsSlideHoldMaxMs),
                       stick = now - g_rs_user_ms < kRsUserTailMs;
            if (!snapped && (have_w ? vy_w : vy) > kRsFallSpeed) g_rs_fall_until = now + kRsFallHoldMs;  // (r192's review: a curb's 3 cm in a tick is no fall)
            if (on && !snapped && have_w && !commanded && !stick && hs_w > kRsMoved) {
                if (now >= g_rs_moved_until) g_rs_moved.fetch_add(1, std::memory_order_relaxed);
                g_rs_moved_until = now + kRsMovedHoldMs;
            }
            if (on && !snapped && have_w && commanded && hs_w > kRsStagger) {
                if (now >= g_rs_stagger_until) g_rs_staggers.fetch_add(1, std::memory_order_relaxed);
                g_rs_stagger_until = now + kRsMovedHoldMs;
            }
        }
        g_rs_ring[g_rs_ring_head] = {now, st.pos[0], st.pos[2], st.pos[1]};  // (his place this tick: the blocked judgement, the stop)
        g_rs_ring_head = (g_rs_ring_head + 1) % 64;
        if (g_rs_ring_n < 64) ++g_rs_ring_n;
        g_rs_tick = st.tick;
        g_rs_tick_ms = now;
        std::memcpy(g_rs_tick_pos, st.pos, sizeof(g_rs_tick_pos));
        g_rs_tick_ok = true;
    }
    if (const uint32_t gen = camera_lever::recentre_gen(); gen != g_rs_gen) {  // a recentre: the offset is 0 again, all anew
        g_rs_gen = gen;
        if (g_rs_walking && !g_rs_blocked) {  // (the review of r189) a walk cut by it: stopped where he is, no stride on
            float sp[3];
            rs_snap(st, have_st, 0.0f, 0.0f, false, true, sp);
        }
        g_rs_suspended = false;
        g_rs_reset_ms = -1.0;
        g_rs_walking = g_rs_blocked = false;
        g_rs_b0 = 0.0f;
        g_rs_fade_cur = 0.0f;
        g_rs_retry_n = 0;
        g_rs_vel_ok = false;  // (2026-10-28: the offset's jump to 0 is no velocity)
        g_rs_shown_rc = camera_lever::body_turn_shown();  // (2026-10-29: the fresh origin's pivot term 0 again)
    }
    if (!on) {
        g_rs_vel_ok = false;
        g_rs_shown_last = camera_lever::body_turn_shown();
        if (g_rs_running || camera_lever::room_owns()) rs_idle(false);
        g_rs_reset_ms = -1.0;
        g_rs_suspended = false;
        rs_clear_fade();  // (cycle r159: 4c read 0.97 after the option went off: now 0, every frame)
        g_rs_bits_pub.store(0, std::memory_order_relaxed);
        return c;
    }
    float no[3];
    const bool have_no = camera_lever::room_neck_offset(no);
    const float ws = camera_lever::world_scale();
    float n[3] = {no[0] * ws, no[1] * ws, no[2] * ws};  // the user's real metres (the torso's model is real)
    {
        // (2026-10-29, the review) [XR] BodyFollowsTurn's pivot: the origin turned by S since the recentre about the centre
        // eye; the neck lever (0, 0, kRsNeckBackM) then reads R_y(-S) lever - lever. Taken off: turning in place is no step
        const float S = std::remainder(camera_lever::body_turn_shown() - g_rs_shown_rc, 360.0f) * 0.0174532925f;
        if (S != 0.0f && std::isfinite(S)) {
            n[0] += kRsNeckBackM * std::sin(S);
            n[2] -= kRsNeckBackM * (std::cos(S) - 1.0f);
        }
    }
    // (2026-10-28) the lean judged on the drop past RoomScaleLeanSlack only: a relaxed stance or a step's bob is no lean
    if (n[1] < 0.0f) n[1] = std::fmin(0.0f, n[1] + g_rs_lean_slack.load(std::memory_order_relaxed));
    const float lean = have_no ? camera_lever::lean_split(n, holster::torso_length()) : 0.0f;
    const float dx = have_no ? n[0] : 0.0f, dz = have_no ? n[2] : 0.0f, dist = std::sqrt(dx * dx + dz * dz);
    {
        // ([XR] BodyFollowsTurn during a walk, 2026-10-28) the origin's yaw turned by what the camera newly shows of a
        // follow: a world direction's coordinates in it turn the other way round the same as the body's frame does
        // (right (cos h, -sin h), back (sin h, cos h) at h + s): the walk's direction (the stick's), a block's and the
        // velocity's last value turned with it, so the walk keeps its line in the world and the turn is no velocity
        const float shown = camera_lever::body_turn_shown();
        const float turn = std::remainder(shown - g_rs_shown_last, 360.0f);
        g_rs_shown_last = shown;
        if (turn != 0.0f && std::isfinite(turn)) {
            const float a = turn * 0.0174532925f, ca = std::cos(a), sa = std::sin(a);
            auto rot = [ca, sa](float* x, float* z) {
                const float x0 = *x, z0 = *z;
                *x = x0 * ca - z0 * sa;
                *z = x0 * sa + z0 * ca;
            };
            rot(&g_rs_vprev[0], &g_rs_vprev[1]);
            rot(&g_rs_vel[0], &g_rs_vel[1]);
            if (g_rs_walking || g_rs_blocked) {
                rot(&g_rs_ux, &g_rs_uz);
                rot(&g_rs_bdir[0], &g_rs_bdir[1]);
                g_rs_turned.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // (2026-10-28) the offset's velocity (the early start) and the head's last time near him (the logs' lag)
        if (have_no && g_rs_vel_ok && fdt > 0.0) {
            const float a = 1.0f - std::exp(-static_cast<float>(fdt) / kRsVelTauS), inv = 1.0f / static_cast<float>(fdt);
            g_rs_vel[0] += ((dx - g_rs_vprev[0]) * inv - g_rs_vel[0]) * a;
            g_rs_vel[1] += ((dz - g_rs_vprev[1]) * inv - g_rs_vel[1]) * a;
        } else if (!g_rs_vel_ok) {
            g_rs_vel[0] = g_rs_vel[1] = 0.0f;
        }
        g_rs_vel_ok = have_no;
        g_rs_vprev[0] = dx, g_rs_vprev[1] = dz;
        if (!have_no || dist <= kRsNearM) g_rs_near_ms = now;
    }
    const float v_out = dist > 1e-3f ? (g_rs_vel[0] * dx + g_rs_vel[1] * dz) / dist : 0.0f;  // (away from him: positive)
    g_rs_vout_pub.store(v_out, std::memory_order_relaxed);
    g_rs_dx_pub.store(dx, std::memory_order_relaxed);
    g_rs_dz_pub.store(dz, std::memory_order_relaxed);
    g_rs_d_pub.store(dist, std::memory_order_relaxed);
    g_rs_lean_pub.store(lean, std::memory_order_relaxed);
    const char* why = have_no ? rs_gate(st, have_st, now) : "no head offset";
    g_rs_why.store(why ? why : "none", std::memory_order_relaxed);
    if (why) {  // riding, cover, a menu...: the offset rendered as it is (today's), taken up again on resume; no fade
        rs_idle(true);
        g_rs_reset_ms = -1.0;  // (a re-anchor not yet done: begun again on resume, if the head is still far)
        rs_finish(false, 0.0f, dx, dz, dist, ws, user, now);
        rs_clear_fade();
        return c;
    }
    if (!g_rs_running) {
        g_rs_running = true;
        if (g_rs_suspended && dist > kRsResumeMax && g_rs_reset_ms < 0.0) {  // walked far about the room meanwhile: no long walk unasked
            g_rs_reset_ms = now;
            g_rs_reset_black = -1;
            g_rs_reset_done = false;
        }
        g_rs_suspended = false;
    }
    if (g_rs_reset_ms >= 0.0) {  // (no walk while the offset is being set again)
        g_rs_walking = false;
        rs_finish(false, 0.0f, dx, dz, dist, ws, user, now);
        return c;
    }
    if (user > kRsUserStick) {  // the player's own stick wins: nothing sent, nothing taken off, the offset kept
        g_rs_user_frames.fetch_add(1, std::memory_order_relaxed);
        g_rs_walking = false;
        g_rs_blocked = false;
        g_rs_b0 = 0.0f;
        g_rs_fade_cur = 0.0f;
        g_rs_cmd_ms = -1e9;
        g_rs_slide_on = false;  // (the review: the player's walk never owned again as a slide, nor learned from)
        rs_finish(false, 0.0f, dx, dz, dist, ws, user, now);
        return c;
    }
    const float dead = g_rs_dead.load(std::memory_order_relaxed), stop = g_rs_stop.load(std::memory_order_relaxed);
    const float mo = g_rs_max_off.load(std::memory_order_relaxed);
    const float b0_cap = std::fmin(mo - 0.05f, std::fmax(0.05f, mo - kRsFadeLead));  // the fade black by the clamp (MaxOffset >= 0.30)
    // (cycle r164) no new walk while he still slides (his eased speed over 0.1 m/s), at most 1.5 s after the last end,
    // unless the offset is 0.1 m past the dead zone: once he stands, the offset decides (stopped short: a new walk)
    const bool sliding = g_rs_slide_on && now - g_rs_end_ms < kRsSlideHoldMaxMs;
    // the walk's direction in the world (the camera's rows a and c, level) and his speed along it now (the stop's
    // prediction: user metres a second over his last 200 ms of ticks)
    // (cycle r182: the walk's direction was built from the scene camera's back row, about 90 degrees off John's own
    // walking in the game's world, so his speed along it read about 0: 16 of 22 blocks with him at 1.3-1.45 m/s, and the
    // stop's speed 0 so it never came early) the direction the stick is sent in: the offset (x right, z back) in the body
    // frame of the anchor's heading, which the game walks John by (the stick goes out after the head-yaw turn, in that
    // frame): right (cos h, -sin h), back (sin h, cos h) in the world's x and z, h the heading (positive turns left)
    // (cycle r185: the sign chain checked against r183-r185's numbers: a right step's stick (+lx) walked him right and
    // forward (an arc: right 0.66, forward 0.74 of his first move at heading -157.5), the take-off took that off d as
    // (-0.35 right, +0.18 back), so d shrank along the stick and grew behind him: every sign is right. What kept d from
    // settling is John: he does not strafe, and re-aimed every frame he orbits a target nearer than his turning circle,
    // the zig-zag x of r185's statuses. So each walk keeps the direction it started with (u, fixed): he turns once and
    // walks straight; what his arc leaves sideways is the next walk's, if it is over the dead zone)
    float wd[3] = {0, 0, 0};
    const bool have_dir = dist > 1e-3f;
    // ([XR] BodyFollowsTurn, the review) the heading the camera shows (the offset is over that origin): less a follow's
    // part still pending (off: none, g_heading itself)
    const float bt_pend = bt_pending();
    const float h_now = (bt_pend != 0.0f ? g_heading - bt_pend : g_heading) * 0.0174532925f;
    const float rgt_w[2] = {std::cos(h_now), -std::sin(h_now)}, bck_w[2] = {std::sin(h_now), std::cos(h_now)};
    if (have_dir) {
        const float ux = g_rs_walking ? g_rs_ux : dx / dist, uz = g_rs_walking ? g_rs_uz : dz / dist;  // (the walk's own, while it runs)
        wd[0] = rgt_w[0] * ux + bck_w[0] * uz;
        wd[2] = rgt_w[1] * ux + bck_w[1] * uz;
    }
    // (cycle r183) his velocity over his last 200 ms of ticks (the world, game metres a second) and where he will stop:
    // his place plus that velocity times the lead (kRsLeadS); the head against that place, in user metres
    double ndt = 0.0;
    float vel[2] = {0, 0};
    if (have_st) rs_ring_vel(200.0, now, &ndt, vel);
    const float v_now_u = std::sqrt(vel[0] * vel[0] + vel[1] * vel[1]) / ws;  // his speed now (user metres a second)
    // (cycles r183-r185) along the walk's own direction: how far the head still is (user metres), his speed along it, and
    // where he will be along it a lead ahead (that speed times kRsLeadS). The predicted stop counts only once
    // he walks along the walk: his velocity within 30 degrees of it and over 0.2 m/s (in his turn it points elsewhere)
    const float along = g_rs_walking ? dx * g_rs_ux + dz * g_rs_uz : dist;
    const float v_along = (vel[0] * wd[0] + vel[1] * wd[2]) / ws;
    const float d_stop = along - std::fmax(0.0f, v_along) * kRsLeadS;
    const float vm = std::sqrt(vel[0] * vel[0] + vel[1] * vel[1]);
    const bool toward = vm / ws > 0.2f && v_along * ws >= 0.866f * vm;
    // (the review of r186) and none while his slide already carries him toward the head to within the dead zone (his
    // predicted stop): a walk ended by that prediction would start again the next frame, the stick pulsed every other
    // frame (r186: "again, 12 ms after the last")
    // (2026-10-28) the early start: the head moving steadily away, past kRsStartShare of the dead zone (and clear of the
    // snap's distance), and a RoomScaleStartLead ahead past the dead zone
    const float snap_m = rs_snap_m(dead), lead_s = g_rs_start_lead.load(std::memory_order_relaxed);
    const bool early = lead_s > 0.0f && v_out > kRsStartV && dist > std::fmax(kRsStartShare * dead, snap_m + kRsStartOverSnapM) &&
                       dist + v_out * lead_s > dead;
    const bool beyond = dist > dead || early;
    if (g_rs_direct.load(std::memory_order_relaxed) && have_st && have_dir && st.actor) {  // [XR] RoomScaleDirect: the probe
        if (g_rsd_req) {
            RdrvrNativeResult r{};
            if (api::wait_native(g_rsd_req, &r, 0)) {
                g_rsd_req = 0;
                g_rsd_clear = (r.value & 0xff) == 0;
                g_rsd_ms = g_rsd_req_ms;
                std::memcpy(g_rsd_dir, g_rsd_req_dir, sizeof(g_rsd_dir));
            } else if (now - g_rsd_req_ms > 500.0) {
                api::cancel_op(g_rsd_req);
                g_rsd_req = 0;
            }
        }
        if (!g_rsd_req && !g_rs_walking && dist > 0.5f * dead && dist <= kRsDirectM) {
            const float wx = (rgt_w[0] * dx + bck_w[0] * dz) / dist, wz = (rgt_w[1] * dx + bck_w[1] * dz) / dist;
            const float len = dist / ws + kRsDirectPadM;
            const float sy = st.pos[1] + kRsDirectChestM;
            const uint64_t args[7] = {0, pack2_rs(st.pos[0], sy), f32bits_rs(st.pos[2]), pack2_rs(st.pos[0] + wx * len, sy), f32bits_rs(st.pos[2] + wz * len),
                                      static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), 0};
            g_rsd_req = api::queue_native(0x720F2CA7u, args, 7, 1);  // CAMERA_PROBE(Result*, Source, Target, Owner, 0), as aim.cpp's
            g_rsd_req_ms = now;
            g_rsd_req_dir[0] = wx, g_rsd_req_dir[1] = wz;
            g_rsd_probes.fetch_add(1, std::memory_order_relaxed);
        }
    }
    bool direct_held = now < g_rsd_hold_until;  // (the review: a walk started in the step's own frame turned him after all)
    if (g_rsd_pending_check && !direct_held) {  // his teleport landed: did it move him? (cycle N24: by his own place, not the
        g_rsd_pending_check = false;              // offset: a head still moving away grows it back)
        const float mx = have_st ? st.pos[0] - g_rsd_pos0[0] : 0.0f, mz = have_st ? st.pos[2] - g_rsd_pos0[2] : 0.0f;
        if (have_st && std::sqrt(mx * mx + mz * mz) < 0.5f * g_rsd_want_m) {
            g_rsd_off = true;
            g_rsd_noeffect.fetch_add(1, std::memory_order_relaxed);
            log::info("[pose] room scale: a direct step did not move him (%.3f of %.3f game m; the offset %.3f m after %.3f): something in the way; walks until "
                      "the head is back", std::sqrt(mx * mx + mz * mz), g_rsd_want_m, dist, g_rsd_last_dist);
        }
    }
    if (g_rsd_off && dist < 0.5f * dead) g_rsd_off = false;
    if (g_rs_direct.load(std::memory_order_relaxed) && !g_rsd_off && !direct_held && !g_rs_walking && beyond && !sliding && have_st && have_dir && dist <= kRsDirectM &&
        dist / ws <= kRsSnapMaxM) {
        const float wx = (rgt_w[0] * dx + bck_w[0] * dz) / dist, wz = (rgt_w[1] * dx + bck_w[1] * dz) / dist;
        const bool fresh = now - g_rsd_ms < kRsDirectProbeMs && wx * g_rsd_dir[0] + wz * g_rsd_dir[1] > 0.94f;  // (20 degrees)
        if (fresh && g_rsd_clear && (!g_rsd_armed || now - g_rsd_arm_ms > 150.0)) {  // first the ownership, the step next frame
            g_rsd_armed = true;
            g_rsd_arm_ms = now;
            g_rs_cmd_ms = now;
            direct_held = true;
        } else if (fresh && g_rsd_clear) {
            g_rsd_armed = false;
            float sp[3] = {0, 0, 0};
            if (rs_snap(st, have_st, (rgt_w[0] * dx + bck_w[0] * dz) / ws, (rgt_w[1] * dx + bck_w[1] * dz) / ws, true, false, sp)) {
                g_rsd_steps.fetch_add(1, std::memory_order_relaxed);
                g_rs_end_ms = now;
                g_rs_slide_on = false;  // (no stride to wait out: put, not walked)
                g_rsd_ms = -1e12;  // (the next step its own probe)
                g_rsd_hold_until = now + kRsSnapGateMs;  // (his teleport lands at the next script tick: the offset reads old till then)
                direct_held = true;
                g_rsd_last_dist = dist;
                std::memcpy(g_rsd_pos0, st.pos, sizeof(g_rsd_pos0));
                g_rsd_want_m = dist / ws;
                g_rsd_pending_check = true;
                g_rs_cmd_ms = now;  // (cycle N23: he was moved, but the take-off of his move runs only while the room owns it: as a
                                    // walk's command does, its 600 ms tail, so the view holds and the offset drops)
                log::info("[pose] room scale: a direct step (%.3f m, %.0f ms after the head left him): put under the head, no walk", dist, now - g_rs_near_ms);
            }
        } else if (fresh && !g_rsd_clear) {
            g_rsd_armed = false;
            g_rsd_blocked.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!g_rs_walking && !direct_held && beyond && (!sliding || (dist > dead + 0.10f && !(toward && d_stop < dead)))) {
        // (cycle r184) a walk after a walk (stopped short, his slide over) starts here at once, counted and logged
        const bool again = now - g_rs_end_ms < 4000.0;
        if (again) g_rs_rewalks.fetch_add(1, std::memory_order_relaxed);
        if (dist <= dead) g_rs_early.fetch_add(1, std::memory_order_relaxed);
        log::info("[pose] room scale: a walk starts, the offset %.3f m%s (moving %.2f m/s away; %.0f ms after the head left him)%s", dist,
                  dist <= dead ? " (early: the head moving away)" : "", v_out, now - g_rs_near_ms,
                  again ? (std::string(" (again, ") + std::to_string(static_cast<int>(now - g_rs_end_ms)) + " ms after the last)").c_str() : "");
        g_rs_walking = true;
        g_rs_walk_ms = now;
        g_rs_cmd_since = 0.0;  // (the review of r186: a gate's early return leaves it set; a fresh walk's 600 ms start here)
        g_rs_slide_on = false;
        g_rs_ux = dx / dist;  // (the walk's direction, kept until it ends)
        g_rs_uz = dz / dist;
        g_rs_walks.fetch_add(1, std::memory_order_relaxed);
        rs_face(st, have_st, rgt_w[0] * g_rs_ux + bck_w[0] * g_rs_uz, rgt_w[1] * g_rs_ux + bck_w[1] * g_rs_uz, now);
        // (cycle r185) the chain's check: where the stick sends him in the world against where he goes, at 0.5 s and 1 s
        g_rs_chk_n = have_st ? 2 : 0;
        g_rs_chk_w[0] = rgt_w[0] * g_rs_ux + bck_w[0] * g_rs_uz;
        g_rs_chk_w[1] = rgt_w[1] * g_rs_ux + bck_w[1] * g_rs_uz;
        if (have_st) g_rs_chk_p0[0] = st.pos[0], g_rs_chk_p0[1] = st.pos[2];
    } else if (!g_rs_walking && beyond && sliding && now >= g_rs_hold_log_ms) {
        g_rs_hold_log_ms = now + 500.0;  // (what holds a walk back: logged twice a second at most)
        log::info("[pose] room scale: a walk held back while he slides (the offset %.3f m, %.0f ms after the last end)", dist, now - g_rs_end_ms);
    } else if (g_rs_walking && !g_rs_blocked && (dist < stop || along < snap_m || (toward && d_stop < snap_m))) {
        log::info("[pose] room scale: the walk ends (%s): the offset %.3f m (%.3f along the walk), %.3f m along it a lead ahead, his speed %.2f "
                  "m/s (%.2f along); %.0f ms after it started, %.0f ms after the head left him",
                  dist < stop ? "at the head" : along < snap_m ? "near the head along the walk" : "near it a lead ahead", dist, along, d_stop, v_now_u,
                  v_along, now - g_rs_walk_ms, now - g_rs_near_ms);
        // (cycle r188) put under the head (or stopped), at once: no stride past it. (The review: d is in user metres, the
        // world in game metres: game = user / WorldScale, as room_shift_origin is given it)
        float snap_p[3] = {0, 0, 0};
        const bool snapped = rs_snap(st, have_st, (rgt_w[0] * dx + bck_w[0] * dz) / ws, (rgt_w[1] * dx + bck_w[1] * dz) / ws, toward, false, snap_p);
        // (cycles r166-r188) the slide after it (none, put under the head) taken off in the tail and measured (logged only)
        g_rs_end_ms = now;
        g_rs_slide_on = have_st;
        g_rs_slide_v0 = v_now_u;
        g_rs_slide_ms = now;
        if (have_st) std::memcpy(g_rs_slide_p0, snapped ? snap_p : st.pos, sizeof(g_rs_slide_p0));  // (the slide from where he was put)
        g_rs_walking = false;
        g_rs_b0 = 0.0f;
        g_rs_retry_n = 0;
    }
    // (cycles r166-r183) blocked from one judgement only: John's own whole speed (st.pos, the plugin's GET_POSITION every
    // tick: his 400 ms displacement's length over its time) under 0.15 m/s, once the stick has been commanded for 600 ms
    // without a break. A wall stops him entirely; an arc toward the stick never does (r183: along the walk read him slow
    // while he arced at 1.2-1.6 m/s, 12 of 15 blocks false). Nothing else sets it
    if (g_rs_chk_n > 0 && have_st && now - g_rs_walk_ms >= (g_rs_chk_n == 2 ? 500.0 : 1000.0)) {
        const float mx = st.pos[0] - g_rs_chk_p0[0], mz = st.pos[2] - g_rs_chk_p0[1], mm = std::sqrt(mx * mx + mz * mz);
        const float cs = mm > 1e-3f ? (mx * g_rs_chk_w[0] + mz * g_rs_chk_w[1]) / mm : 0.0f;
        log::info("[pose] room scale: the stick (%.2f %.2f) sends him (%.2f %.2f) in the world; after %.0f ms he went (%.2f %.2f), %.2f m: "
                  "%.0f degrees off (an arc while he turns; the stick's frame wrong if it stays large)",
                  g_rs_ux, -g_rs_uz, g_rs_chk_w[0], g_rs_chk_w[1], now - g_rs_walk_ms, mm > 1e-3f ? mx / mm : 0.0f, mm > 1e-3f ? mz / mm : 0.0f, mm,
                  std::acos(std::fmax(-1.0f, std::fmin(1.0f, cs))) * 57.29578f);
        --g_rs_chk_n;
    }
    if (g_rs_walking && have_st && have_dir && now - g_rs_walk_ms < 3000.0 && now >= g_rs_trace_next) {
        // the walk's first 3 s, 4 a second (his speed along the walk and in all, his own move, the predicted stop)
        g_rs_trace_next = now + 250.0;
        double tdt = 0.0;
        float tp[3] = {0, 0, 0};
        const float along400 = rs_ring_speed(wd, kRsBlockWinMs, now, &tdt, tp);
        const float mx = st.pos[0] - tp[0], mz = st.pos[2] - tp[2], mm = std::sqrt(mx * mx + mz * mz);
        log::info("[pose] room scale walk +%4.0f ms: stick %.2f, d %.3f m, John along the walk %.2f m/s over 400 ms (now %.2f; all %.2f; his tick %.2f), "
                  "the walk (%.2f %.2f), his own move (%.2f %.2f), the head from where he will stop %.3f m, heading %.1f, commanded %.0f ms, "
                  "blocked %d, at (%.2f %.2f %.2f)",
                  now - g_rs_walk_ms, g_rs_last_m, dist, along400, v_now_u * ws, tdt > 0.0 ? mm / tdt : 0.0,
                  g_rs_hs_pub.load(std::memory_order_relaxed), wd[0], wd[2], mm > 1e-4f ? mx / mm : 0.0f, mm > 1e-4f ? mz / mm : 0.0f, d_stop,
                  g_heading, g_rs_cmd_since > 0.0 ? now - g_rs_cmd_since : 0.0, g_rs_blocked ? 1 : 0, st.pos[0], st.pos[1], st.pos[2]);
    }
    if (g_rs_walking && !g_rs_blocked && have_st && have_dir && g_rs_cmd_since > 0.0 && now - g_rs_cmd_since >= kRsBlockCmdMs) {
        double bdt = 0.0;
        float p0[3] = {0, 0, 0};
        const float along400 = rs_ring_speed(wd, kRsBlockWinMs, now, &bdt, p0);
        const float bmx = st.pos[0] - p0[0], bmz = st.pos[2] - p0[2];
        const float v400 = bdt > 0.0 ? static_cast<float>(std::sqrt(bmx * bmx + bmz * bmz) / bdt) : 0.0f;  // his whole speed
        g_rs_speed_pub.store(v400, std::memory_order_relaxed);
        if (bdt >= 0.3 && v400 < kRsBlockV) {
            const float cand = std::fmin(dist, b0_cap);
            g_rs_b0 = g_rs_b0 > 0.0f ? std::fmin(g_rs_b0, cand) : cand;  // (a re-block keeps the old start)
            g_rs_blocked = true;
            g_rs_block_ms = now;
            g_rs_bdir[0] = dx / dist;
            g_rs_bdir[1] = dz / dist;
            g_rs_blocks.fetch_add(1, std::memory_order_relaxed);
            log::info("[pose] room scale: John blocked: %.3f m/s in all over %.0f ms (along the walk %.3f; from (%.2f %.2f) to (%.2f %.2f), the walk "
                      "(%.2f %.2f)), his tick speed %.2f m/s, commanded %.0f ms (stick %.2f), the offset %.2f m: the stick stopped, the fade from %.2f m",
                      v400, bdt * 1000.0, along400, p0[0], p0[2], st.pos[0], st.pos[2], wd[0], wd[2], g_rs_hs_pub.load(std::memory_order_relaxed),
                      now - g_rs_cmd_since, g_rs_last_m, dist, g_rs_b0);
        }
    }
    if (g_rs_blocked) {
        const float cosang = dist > 1e-3f ? (dx * g_rs_bdir[0] + dz * g_rs_bdir[1]) / dist : 1.0f;
        const double wait = std::fmin(kRsRetryMaxMs, kRsRetryMs * static_cast<double>(1 << (g_rs_retry_n < 4 ? g_rs_retry_n : 3)));
        if (dist < dead) {  // stepped back
            g_rs_blocked = false;
            g_rs_walking = false;
            g_rs_b0 = 0.0f;
            g_rs_retry_n = 0;
        } else if (cosang < 0.7071f) {  // along the wall: try walking again
            g_rs_blocked = false;
            g_rs_walk_ms = now;
            g_rs_ux = dx / dist;  // (aimed again)
            g_rs_uz = dz / dist;
            g_rs_cmd_since = 0.0;
            rs_face(st, have_st, rgt_w[0] * g_rs_ux + bck_w[0] * g_rs_uz, rgt_w[1] * g_rs_ux + bck_w[1] * g_rs_uz, now);
        } else if (now - g_rs_block_ms >= wait) {  // an NPC walked off? tried again after 1, 2, 4, 8 s
            g_rs_blocked = false;
            g_rs_walk_ms = now;
            g_rs_ux = dx / dist;  // (aimed again)
            g_rs_uz = dz / dist;
            g_rs_cmd_since = 0.0;
            rs_face(st, have_st, rgt_w[0] * g_rs_ux + bck_w[0] * g_rs_uz, rgt_w[1] * g_rs_ux + bck_w[1] * g_rs_uz, now);
            ++g_rs_retry_n;
            g_rs_retries.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (g_rs_b0 > 0.0f && !g_rs_blocked && dist <= g_rs_b0) g_rs_b0 = 0.0f;  // the head back within it: the fade's start forgotten
    float target = 0.0f;
    if (g_rs_b0 > 0.0f && g_rs_fade_on.load(std::memory_order_relaxed)) {  // the fade past where he stopped
        float t = (dist - g_rs_b0 - 0.05f) / 0.15f;
        t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
        target = t * t * (3.0f - 2.0f * t);
    }
    const float step = static_cast<float>(fdt * 1000.0 / kRsFadeRampMs);  // (eased: 0.15 s from clear to black)
    g_rs_fade_cur = target > g_rs_fade_cur ? std::fmin(target, g_rs_fade_cur + step) : std::fmax(target, g_rs_fade_cur - step);
    // (cycle r163) the fade from geometry, whatever the detector says: black over MaxOffset +- 0.10 m, so the view is
    // never past MaxOffset unfaded; the frame's fade the larger of it and the blocked fade. A walk in open ground keeps the
    // offset well under it (a 0.5 m step under the default 0.65's 0.55)
    float geo = 0.0f;
    if (g_rs_fade_on.load(std::memory_order_relaxed)) {
        float t = (dist - (mo - 0.10f)) / 0.20f;
        t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
        geo = t * t * (3.0f - 2.0f * t);
    }
    g_rs_fade_geo_pub.store(geo, std::memory_order_relaxed);
    const float fade_now = std::fmax(g_rs_fade_cur, geo);
    // the backstop: the offset clamped at RoomScaleMaxOffset while he is blocked, once the eyes are black (the fade leads:
    // the view is never seen to jump); with the fade off, at once (the view held there)
    // (cycle r161: the clamp fired once and then let d grow to 0.85 m: a retry ended "blocked" while the head pushed on)
    // held every frame while the fade's start is kept (b0: until the head is back within it)
    // (cycle r163) and by geometry: past MaxOffset + 0.10 m, where that fade is black, held there whatever the detector
    // says; with the fade off, held at MaxOffset itself (the backstop)
    float hold = -1.0f;
    if ((g_rs_blocked || g_rs_b0 > 0.0f) && dist > mo && (!g_rs_fade_on.load(std::memory_order_relaxed) || g_rs_fade_cur >= 0.99f)) hold = mo;
    else if (g_rs_fade_on.load(std::memory_order_relaxed) ? dist > mo + 0.10f : dist > mo) hold = g_rs_fade_on.load(std::memory_order_relaxed) ? mo + 0.10f : mo;
    if (hold > 0.0f) {
        const float ex = (dist - hold) / dist;
        camera_lever::room_shift_origin(dx * ex / ws, dz * ex / ws);  // (queued: room_neck_offset reads it from the next frame)
        g_rs_clamps.fetch_add(1, std::memory_order_relaxed);
        g_rs_d_pub.store(hold, std::memory_order_relaxed);  // (the status: the clamped offset, as the next frame has it)
    }
    // (cycle r186) the stick held at 0 while he turns to face the walk (his posted heading not yet within kRsFaceTolDeg)
    if (g_rs_face_until > 0.0 && (!g_rs_walking || g_rs_blocked || now >= g_rs_face_until ||
                                  (have_st && std::fabs(rs_wrap_deg(st.heading_deg - g_rs_face_h)) <= kRsFaceTolDeg))) {
        if (g_rs_walking && !g_rs_blocked)
            log::info("[pose] room scale: John faces the walk (heading %.1f, %.0f ms)%s", have_st ? st.heading_deg : 0.0f,
                      kRsFaceMaxMs - (g_rs_face_until - now), now >= g_rs_face_until ? ": the wait ran out" : "");
        g_rs_face_until = 0.0;
    }
    if (g_rs_walking && !g_rs_blocked && dist > 1e-3f && g_rs_face_until <= 0.0) {
        const float mn = g_rs_min.load(std::memory_order_relaxed), mx = g_rs_max.load(std::memory_order_relaxed);
        float m = mn + (mx - mn) * (along - stop) / kRsRamp;
        m = m < mn ? mn : m > mx ? mx : m;  // (never under MinStick: below the game's walk threshold John would not move)
        g_rs_last_m = m;
        c.on = true;
        if (g_rs_cmd_since <= 0.0) g_rs_cmd_since = now;  // (the stick commanded without a break since then: the block's 600 ms)
        // (the walk's own direction, set at its start, over the origin: the heading shown). (2026-10-29, the review) The
        // game reads the stick against the heading commanded, a follow's pending part further round: the direction in
        // that frame (as rs_frame's turn above, by bt_pend)
        float sx = g_rs_ux, sz = g_rs_uz;
        if (bt_pend != 0.0f && std::isfinite(bt_pend)) {
            const float a = bt_pend * 0.0174532925f, ca = std::cos(a), sa = std::sin(a);
            sx = g_rs_ux * ca - g_rs_uz * sa;
            sz = g_rs_ux * sa + g_rs_uz * ca;
        }
        c.lx = sx * m;
        c.ly = -sz * m;  // (z back: forward is -z)
        g_rs_cmd_ms = now;
        g_rs_cmd_frames.fetch_add(1, std::memory_order_relaxed);
    }
    if (!c.on) g_rs_cmd_since = 0.0;
    g_rs_lx_pub.store(c.lx, std::memory_order_relaxed);
    g_rs_ly_pub.store(c.ly, std::memory_order_relaxed);
    // the take-off owns the command, 600 ms after it at least, and the slide until it is over (at most 1.5 s)
    rs_finish(c.on || now - g_rs_cmd_ms < kRsTailMs || (g_rs_slide_on && now - g_rs_end_ms < kRsSlideHoldMaxMs), fade_now, dx, dz, dist, ws,
              user, now);
    return c;
}

// 2026-10-27 ([XR] BodyFollowsTurn; the user: "Turning physically will turn johns body right?", told it did not (only
// the view turned: his heading moved by the turn stick, a recentre or a mount), "Add it after room scale works."): the
// user's physical turn turns John. The head's yaw over the body (as commanded: the origin's yaw has taken only what the
// camera already shows, camera_lever::body_turn_shown) held beyond BodyTurnDeadZone for BodyTurnHoldMs is a turn, not a
// look aside: the body heading (g_heading) then follows it smoothly (eased in over BodyTurnEaseS, at most BodyTurnSpeed
// deg/s, slowing as it nears: BodyTurnGain a second of what is left) until within 3 degrees. It is the turn stick's own path (the
// anchored camera's heading, so LockFacing's drawn body, the holsters' frame and the room walk's frame all turn with it,
// and the actor's own heading as with the stick: not set, no native); the view does not turn: camera_lever turns the
// recentre origin's yaw by what the frame's camera shows of it (body_turn_push), so the head's yaw over the body drops as
// the body comes round, and the walk stick's head-yaw turn (stick_yaw) with it: no double turn, the stick still walks where
// the head faces. The head's yaw, not the torso's: the neck model (LeanSteady) has the torso's lean, not its yaw; the
// dead zone and the hold tell the look aside. Not riding or driving, in cover, under a script's camera, in a menu or the
// weapon wheel, with a long gun held two-handed (its aim is the hands'), during a room-scale walk with
// BodyTurnDuringWalk=0 (its stick is in the body's frame; on, rs_frame turns the walk with the origin), nor with the
// eyes not from the headset or the headset not tracked. Off by default;
// off, nothing is pushed and nothing turns (today's behaviour)
// 2026-10-28 (round 37 check 7, the user: "turning ... lag behind the users movements": John faced the head 1.3-1.6 s
// after the turn began): the dead zone 35 degrees (was 40), the hold 100 ms (was 250), at most 300 deg/s (was 150),
// eased in over BodyTurnEaseS 0.08 s (was 0.15), the tail BodyTurnGain 12 a second of what is left (was 6): a 90-degree
// turn is within 10 degrees in about 0.45 s. And not held back by a room-scale walk ([XR] BodyTurnDuringWalk, on): the
// walk's direction turns with the origin (rs_frame), so the walk keeps its line in the world while he comes round
std::atomic<bool> g_bt_on{false}, g_bt_walk{true};
std::atomic<float> g_bt_dead{35.0f}, g_bt_hold{100.0f}, g_bt_speed{300.0f}, g_bt_gain{12.0f}, g_bt_ease_s{0.08f};
constexpr float kBtStopDeg = 3.0f;
// presenting thread
float g_bt_total = 0.0f;  // the follow's turn so far (degrees, wrapped; only its changes matter)
// the follow's own steps only (a recentre's correction of g_bt_total left out: the holsters' frame, body_turn_steps_deg)
std::atomic<float> g_bt_steps{0.0f};
double g_bt_beyond_ms = -1.0, g_bt_began_ms = 0.0;  // (the hold's start: the time to face is judged from it)
bool g_bt_following = false, g_bt_pushing = false, g_bt_near = false;
float g_bt_ease = 0.0f;
std::atomic<float> g_bt_rel_pub{0.0f}, g_bt_total_pub{0.0f};
std::atomic<const char*> g_bt_why{"off"};
std::atomic<int> g_bt_state_pub{0};  // 1 following, 2 beyond the dead zone (held), 4 pushing
std::atomic<uint64_t> g_bt_follows{0}, g_bt_frames{0};
float bt_wrap(float d) { return std::remainder(d, 360.0f); }
float bt_pending() { return bt_wrap(g_bt_total - camera_lever::body_turn_shown()); }
const char* bt_gate(const RdrvrActorState& st, bool have_st, bool riding) {
    if (!have_st) return "no actor state";
    if (!camera_lever::xr_double()) return "the eyes not from the headset";
    if (riding || st.mount != 0 || (st.flags & (RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING))) return "riding or driving";
    if (st.flags & RDRVR_ACTOR_IN_COVER) return "in cover";
    if (st.flags & RDRVR_ACTOR_CAM_AWAY) return "a script's camera";
    if (controls::game_menu_open() || menu::visible()) return "a menu";
    if (wheel::holder() >= 0 || wheel::open()) return "the weapon wheel";
    if (holster::two_hand_blend() > 0.5f) return "a long gun held two-handed";
    if (!xr::head_tracked()) return "the headset not tracked";
    if (!g_bt_walk.load(std::memory_order_relaxed) && (g_rs_walking || g_rs_blocked || camera_lever::room_owns())) return "a room-scale walk";
    // (the review) the frame camera's heading matched to the commands first (its offset learned standing still a third
    // of a second): before, a follow's turn could not be told apart from the camera's lag
    if (!camera_lever::body_turn_ready()) return "the camera's heading not yet learned";
    return nullptr;
}
void bt_reset() {
    g_bt_following = false;
    g_bt_beyond_ms = -1.0;
    g_bt_ease = 0.0f;
}
// the frame's follow (after the turn stick, before the heading is wrapped and sent); turns g_heading
void bt_frame(const RdrvrActorState& st, bool have_st, bool riding, float dt, double now) {
    if (!g_bt_on.load(std::memory_order_relaxed)) {
        if (g_bt_following || g_bt_beyond_ms >= 0.0) bt_reset();
        g_bt_why.store("off", std::memory_order_relaxed);
        return;
    }
    float yaw = 0.0f;
    const bool have_yaw = camera_lever::head_yaw_deg(&yaw);
    const float rel = bt_wrap(yaw - bt_wrap(g_bt_total - camera_lever::body_turn_shown()));  // over the body as commanded
    g_bt_rel_pub.store(have_yaw ? rel : 0.0f, std::memory_order_relaxed);
    const char* why = have_yaw ? bt_gate(st, have_st, riding) : "no head pose";
    g_bt_why.store(why ? why : "none", std::memory_order_relaxed);
    g_bt_frames.fetch_add(1, std::memory_order_relaxed);
    if (why) {
        if (g_bt_following) log::info("[pose] body turn: stopped (%s), the head %.1f deg over the body", why, rel);
        bt_reset();
        return;
    }
    const float a = std::fabs(rel);
    if (!g_bt_following) {
        if (a <= g_bt_dead.load(std::memory_order_relaxed)) {
            g_bt_beyond_ms = -1.0;
            return;
        }
        if (g_bt_beyond_ms < 0.0) g_bt_beyond_ms = now;
        if (now - g_bt_beyond_ms < g_bt_hold.load(std::memory_order_relaxed)) return;
        g_bt_following = true;
        g_bt_ease = 0.0f;
        g_bt_near = false;
        g_bt_began_ms = g_bt_beyond_ms;
        g_bt_follows.fetch_add(1, std::memory_order_relaxed);
        log::info("[pose] body turn: the head %.1f deg over the body for %.0f ms: John follows (heading %.1f)", rel, now - g_bt_beyond_ms, g_heading);
    }
    const float ease_s = g_bt_ease_s.load(std::memory_order_relaxed);
    g_bt_ease = ease_s > 0.001f ? std::fmin(1.0f, g_bt_ease + dt / ease_s) : 1.0f;
    const float rate = std::fmin(g_bt_speed.load(std::memory_order_relaxed), a * g_bt_gain.load(std::memory_order_relaxed)) * g_bt_ease;  // degrees a second
    float step = std::fmin(a, rate * dt);
    if (rel < 0.0f) step = -step;
    g_heading += step;  // (positive turns left, as the head's yaw: a recentre adds it the same way)
    g_bt_total = bt_wrap(g_bt_total + step);
    g_bt_steps.store(bt_wrap(g_bt_steps.load(std::memory_order_relaxed) + step), std::memory_order_relaxed);
    if (!g_bt_near && a - std::fabs(step) < 10.0f) {  // (the acceptance's "within about 1 s": from the head's turn past the dead zone)
        g_bt_near = true;
        log::info("[pose] body turn: within 10 deg of the head %.0f ms after it turned past the dead zone", now - g_bt_began_ms);
    }
    if (a - std::fabs(step) < kBtStopDeg) {
        log::info("[pose] body turn: John faces the head (heading %.1f, %.1f deg left, %.0f ms after the head turned)", g_heading, a - std::fabs(step),
                  now - g_bt_began_ms);
        bt_reset();
    }
}

void on_frame_end(uint64_t) {
    // the controllers mapped onto the game's pad (every frame: the menu button and the recentre combo are watched there)
    xinput::PadState pad;
    bool controllers = controls::pad(&pad);
    bool want = g_anchor.load(std::memory_order_relaxed) && xr::submitting() && vr_mode::gameplay_stereo();
    // 2026-10-13 (the user: "a mission in which you control a gatling gun. While holding it johns body and the gun were
    // spinning around with no way to stop"): a mounted gun (the Gatling 26, the Browning 27, the cannon 28: the game's
    // turret, its own Gatling_Camera, aimed by the right stick) is not a gun in John's hands: while one is in use the
    // anchor stands down (the game's camera, body and aim, in stereo, as with First person off), and every hand rule
    // with it (they all wait on anchor_active: the arms, the gun's place in the hand, the shots along its barrel)
    static bool was_mounted = false;
    {
        RdrvrActorState ms{};
        const bool held = api::actor_state(&ms) && ms.weapon >= 26 && ms.weapon <= 28;
        const bool mounted = held || aim::mounted_gun_fired(1500.0);
        g_mounted_held_shared.store(held, std::memory_order_relaxed);
        if (mounted != was_mounted) {
            was_mounted = mounted;
            log::info("[pose] %s", mounted ? "a mounted gun in use (the Gatling, the Browning or the cannon): the game's own camera and aim until it is left"
                                           : "the mounted gun left: first person again");
        }
        g_on_mounted_gun_shared.store(mounted, std::memory_order_relaxed);
        // (the Gatling probe: first person kept only while the turret drive can read the gun; else the game's own camera,
        // so the right stick still turns it: the review of 0.8.10)
        // (round 137, the review) and only while the eyes can be drawn at John's head with the game's camera current (the
        // gun's camera on the channel: weapons bit 16 below); else the hands' ray would be taken from the gun's own camera
        if (mounted && !(g_mounted_fp.load(std::memory_order_relaxed) && held && holster::turret_ready() && camera_lever::first_person_ready()))
            want = false;
    }
    if (want != g_active) {
        g_active = want;
        g_active_shared = want;
        g_have_heading = false;
        g_last_ms = 0;
        log::info("[pose] camera anchor %s", want ? "on: the scripted camera follows the actor" : "off");
        if (!want) {
            xinput::set_source(false, {});
            xinput::set_pad_turn(false, 0);
        }
    }
    pc_reference(log::now_ms());  // [XR] PhysicalCrouch: the standing reference and the drop (2026-10-27)
    if (!want && g_pc_running) {  // the anchor gone (a cutscene, the cinema screen): nothing owned, nothing out
        pc_reset();
        g_pc_running = false;
    }
    if (!want) {  // [XR] RoomScale: suspended (taken up on resume); the eyes' fade exactly 0 every frame
        if (g_rs_running || camera_lever::room_owns()) rs_idle(true);
        g_rs_vel_ok = false;  // (2026-10-28: the offset's velocity begun again on resume)
        g_rs_shown_last = camera_lever::body_turn_shown();
        g_rs_reset_ms = -1.0;
        rs_clear_fade();  // (never a faded cutscene or cinema screen)
        g_rs_lx_pub.store(0.0f, std::memory_order_relaxed);
        g_rs_ly_pub.store(0.0f, std::memory_order_relaxed);
    }
    if (!want) g_pc_bits_pub.store((g_pc_ref ? 16 : 0) | (g_pc_H_stage ? 32 : 0) | (g_pc_ref_wait ? 512 : 0), std::memory_order_relaxed);
    RsCmd rs_cmd;  // [XR] RoomScale's stick this frame
    RdrvrCameraJob job{};
    job.enabled = want ? 1 : 0;
    job.up_axis = static_cast<uint32_t>(g_up_axis);
    job.height = g_height + g_lift;
    job.forward = g_forward;
    job.orient_mode = static_cast<uint32_t>(g_orient_mode);
    job.no_idles = g_no_idles ? 1 : 0;
    // (2026-11-06, GOAL-remaining 6c) bit 2: in third person (First person off, in VR) with the mod's reticle on, the game's
    // flat one hidden too (drawn on the HUD's quad it sits at the quad's depth, not the target's: doubled)
    job.hide_reticle = (g_hide_reticle ? 1u : 0u) |
                       (!want && aim::third_person_reticle_live() && camera_lever::eyes_follow_head() && xr::submitting() && vr_mode::gameplay_stereo() ? 2u : 0u);
    g_mod_hides_reticle.store((job.enabled && (job.hide_reticle & 1u)) || (job.hide_reticle & 2u), std::memory_order_relaxed);  // (the plugin's own test)
    job.weapons = (holster::weapon_choice() ? 1u : 0u) | (g_keep_cam.load(std::memory_order_relaxed) ? 2u : 0u);
    if (aim::gameplay_camera_current()) job.weapons |= 8u;  // no script's camera holds channel 0: the anchor may take it back
    // (round 137) a mounted gun in first person: the game's own camera wanted on the channel (it turns the gun; with the
    // anchor's current the turret does not answer the stick: cycle RU6), the eyes from John's head as in a script's shot
    if (want && g_mounted_held_shared.load(std::memory_order_relaxed) && g_mounted_fp.load(std::memory_order_relaxed)) job.weapons |= 16u;
    if (want) {
        RdrvrActorState st{};
        if (!g_have_scale && g_auto_height && api::actor_state(&st) && st.head_valid) {
            float up = g_up_axis == 1 ? st.head_pos[1] - st.pos[1] : st.head_pos[2] - st.pos[2];
            if (up > 0.5f && up < 2.5f) {  // a standing human's head bone above the root
                g_measured_head = up;
                g_height = up;
                g_have_scale = true;
                log::info("[pose] world scale: the head bone is %.3f m above the actor's root; eye height %.3f m (+ lift %.2f)",
                          up, g_height, g_lift);
            }
        }
        if (!g_have_heading) {
            job.heading_from_actor = 1;
            if (api::actor_state(&st) && st.camera) {
                g_heading = st.heading_deg;
                g_have_heading = true;
                log::info("[pose] body heading from the actor: %.1f deg; actor (%.2f %.2f %.2f), camera (%.2f %.2f %.2f)",
                          g_heading, st.pos[0], st.pos[1], st.pos[2], st.camera_pos[0], st.camera_pos[1], st.camera_pos[2]);
            }
        }
        if (g_recentre.exchange(false)) {
            // [XR] BodyFollowsTurn (the review): the head's yaw is over the origin, which has taken only what the camera
            // shows of a follow; the part still pending is in g_heading already, so it is not added again, and it is
            // dropped from the total (the fresh origin is the head now: nothing of it is to be turned in later), the
            // pushed commands forgotten. Off (nothing pending): the head's yaw, as before
            const float bt_pend = bt_wrap(g_bt_total - camera_lever::body_turn_shown());
            const float add = bt_pend != 0.0f ? g_last_head_yaw - bt_pend : g_last_head_yaw;
            log::info("[pose] recentre: body heading %.1f -> %.1f deg (the head's yaw %.1f; the ride's turn %.1f cleared)", g_heading,
                      g_heading + add, g_last_head_yaw, g_ride_off);
            g_heading += add;
            g_last_head_yaw = 0;
            g_ride_off = 0.0f;
            bt_reset();  // [XR] BodyFollowsTurn: the body faces the head now; a turn judged anew
            if (bt_pend != 0.0f) g_bt_total = bt_wrap(g_bt_total - bt_pend);
            if (g_bt_pushing) camera_lever::body_turn_clear();
        }
        // turning on the right stick (positive heading turns left): the controllers' or the gamepad's, the larger
        double now = log::now_ms();
        float dt = g_last_ms > 0 ? static_cast<float>((now - g_last_ms) / 1000.0) : 0.0f;
        g_last_ms = now;
        if (dt <= 0 || dt > 0.1f) dt = 1.0f / 90.0f;
        // riding: the rider's head over the horse's root, averaged (the gait's bounce out), for the saddle anchor
        // run 10 item 3 ([Horse] VehicleHeading): driving a wagon or a coach rides as a horse does for the view's heading
        // (it follows the vehicle's, StickTurn's offset over it), not for the saddle anchor (the driver's bench is not
        // over the vehicle's middle line: the camera stays over John's own root)
        const bool have_st = api::actor_state(&st);
        const bool vehicle = have_st && st.mount != 0 && (st.flags & RDRVR_ACTOR_VEHICLE);
        const bool passenger = vehicle && (st.flags & RDRVR_ACTOR_PASSENGER);  // (round 141)
        const bool riding = have_st && st.mount != 0 && (!vehicle || g_vehicle_heading.load(std::memory_order_relaxed));
        static int ride_kind_was = 0;
        const double now_pass = log::now_ms();  // (the review: 0 not, 1 riding or driving, 2 a passenger: a change of seat is a change too)
        const int ride_kind = !riding ? 0 : passenger ? 2 : 1;
        if (ride_kind != ride_kind_was) {
            ride_kind_was = ride_kind;
            g_was_riding = riding;
            g_saddle_valid = false;
            g_ride_off = 0.0f;  // [Horse] StickTurn: the view faces the horse at the mount (the dismount keeps the turned heading)
            g_ride_snapped = true;  // a stick still held from a turn on foot: no snap until it comes back
            // (round 141) a passenger (the mission's wagon, a stagecoach): the view keeps the way it faces and turns with
            // the vehicle from there (its heading plus the turn the view had against it as he got in; the right stick
            // turns it further, as riding)
            // (round 142) as his seat faces: John's own heading against the vehicle's (the Fort Mercer wagon's seat faces
            // back, 172 degrees; the view's own heading then was whatever the cutscene left)
            if (riding && passenger) g_ride_off = std::fmod(st.heading_deg - st.mount_heading + 540.0f, 360.0f) - 180.0f;
            g_pass_since = riding && passenger ? now_pass : -1.0;
            g_pass_turns0 = g_ride_turns;
            if (vehicle && passenger)
                log::info("[pose] a passenger (the view turns with the vehicle: its heading %.1f deg, John's %.1f, the view's %.1f before: "
                          "his seat's %.1f from it kept)", st.mount_heading, st.heading_deg, g_heading, g_ride_off);
            else if (vehicle)
                log::info("[pose] driving (the view's heading the vehicle's %.1f deg, steering by the %s)", st.mount_heading,
                          g_steer_head.load() ? "head" : "stick");
            else
                log::info("[pose] %s (steering by the %s, saddle anchor %s)", riding ? "riding" : "on foot", g_steer_head.load() ? "head" : "stick",
                          g_saddle.load() ? "on" : "off");
        }
        if (riding && !vehicle && st.head_valid && g_up_axis == 1) {
            const float mh = st.mount_heading * 0.0174532925f;
            const float up = st.head_pos[1] - st.mount_pos[1];
            const float fw = (st.head_pos[0] - st.mount_pos[0]) * -std::sin(mh) + (st.head_pos[2] - st.mount_pos[2]) * -std::cos(mh);
            if (up > 0.5f && up < 3.5f && std::fabs(fw) < 2.0f) {
                const float a = 1.0f - std::exp(-dt / 1.0f);
                g_saddle_h = g_saddle_valid ? g_saddle_h + (up - g_saddle_h) * a : up;
                g_saddle_f = g_saddle_valid ? g_saddle_f + (fw - g_saddle_f) * a : fw;
                g_saddle_valid = true;
            }
            ++g_ride_frames;
            if (st.tick != g_jit_tick) {
                const float rel = st.camera_pos[1] - st.mount_pos[1];
                if (g_jit_tick && st.tick == g_jit_tick + 1) {
                    g_jit_sum += static_cast<double>(rel - g_jit_last) * (rel - g_jit_last);
                    g_jit_abs += static_cast<double>(st.camera_pos[1] - g_jit_last_y) * (st.camera_pos[1] - g_jit_last_y);
                    ++g_jit_n;
                }
                g_jit_last = rel;
                g_jit_last_y = st.camera_pos[1];
                g_jit_tick = st.tick;
            }
        }
        const bool stick_steer = riding && (passenger || !g_steer_head.load(std::memory_order_relaxed));  // (a passenger steers nothing)
        if (riding && st.tick != g_trace_tick && g_trace_on.load(std::memory_order_relaxed)) {
            g_trace_tick = st.tick;
            TraceRow r{};
            r.ms = now;
            r.tick = st.tick;
            std::memcpy(r.mount, st.mount_pos, sizeof(r.mount));
            r.mh = st.mount_heading;
            std::memcpy(r.cam, st.camera_pos, sizeof(r.cam));
            std::memcpy(r.head, st.head_pos, sizeof(r.head));
            std::memcpy(r.seat, st.pos, sizeof(r.seat));
            r.h = g_saddle_h;
            r.f = g_saddle_f;
            std::lock_guard lock(g_trace_mutex);
            if (g_trace.size() < 20000) g_trace.push_back(r);
        }
        if (riding && !vehicle && g_saddle_valid && g_saddle.load(std::memory_order_relaxed)) {
            job.saddle = g_saddle_climb.load(std::memory_order_relaxed) ? 3u : 1u;
            job.saddle_height = g_saddle_h + g_lift;
            job.saddle_forward = g_saddle_f + g_forward;
            job.saddle_tau = g_saddle_tau.load(std::memory_order_relaxed);
            g_job_saddle_h.store(g_lift, std::memory_order_relaxed);
            g_job_saddle_f.store(g_forward, std::memory_order_relaxed);
            g_job_saddle.store(true, std::memory_order_relaxed);
        } else {
            g_job_saddle.store(false, std::memory_order_relaxed);
        }
        float sx = xinput::pad_right_x();
        if (controllers && std::fabs(pad.rx / 32767.0f) > std::fabs(sx)) sx = pad.rx / 32767.0f;
        // (round 138) [DeadEye] DuelNoTurn: in a duel the stick's sideways push turns nothing (the user flicking it up for
        // an X turned the view). (The review) after it the stick must come back to the middle before it turns again: held
        // sideways as the duel ends, it would snap at once
        {
            static bool duel_hold = false;
            if (deadeye::duel_no_turn() && deadeye::in_duel()) {
                duel_hold = true;
                sx = 0.0f;
            } else if (duel_hold) {
                if (std::fabs(sx) < 0.3f) duel_hold = false;
                else sx = 0.0f;
            }
        }
        if (stick_steer) {
            // the view faces the horse and the left stick steers it; the right stick turns the view by an offset over
            // the mount's heading ([Horse] StickTurn), or not at all
            job.heading_from_mount = 1;
            if (g_ride_turn.load(std::memory_order_relaxed) || passenger) {  // (the review: a passenger whatever StickTurn says)
                if (g_snap) {
                    if (std::fabs(sx) > 0.7f && !g_ride_snapped) {
                        g_ride_off += sx > 0 ? -g_snap_angle : g_snap_angle;
                        g_ride_snapped = true;
                        ++g_ride_turns;
                    } else if (std::fabs(sx) < 0.3f) {
                        g_ride_snapped = false;
                    }
                } else if (std::fabs(sx) > 0.15f) {
                    g_ride_off -= sx * g_turn_speed * dt;
                    ++g_ride_turns;
                }
                g_ride_off = std::fmod(g_ride_off + 540.0f, 360.0f) - 180.0f;
            } else {
                g_ride_off = 0.0f;
            }
            // (round 142, the review) a passenger's seat followed for his first 2.5 s (the flag can rise as he climbs in),
            // until the stick turns the view
            if (passenger && g_pass_since >= 0.0 && now - g_pass_since < 2500.0 && g_ride_turns == g_pass_turns0)
                g_ride_off = std::fmod(st.heading_deg - st.mount_heading + 540.0f, 360.0f) - 180.0f;
            if (g_ride_off != 0.0f) job.weapons |= 4u;  // heading_deg below is the offset over the mount's heading (else absolute, as before)
            g_heading = st.mount_heading + g_ride_off;
            sx = 0.0f;
        }
        // (the review: not for a passenger: his body faces as the view does, as on foot; his seat may face any way)
        g_ride_off_shared.store(stick_steer && !passenger ? g_ride_off : 0.0f, std::memory_order_relaxed);
        if (g_snap) {
            if (std::fabs(sx) > 0.7f && !g_snapped) {
                g_heading += sx > 0 ? -g_snap_angle : g_snap_angle;
                g_snapped = true;
                ++g_turns;
            } else if (std::fabs(sx) < 0.3f) {
                g_snapped = false;
            }
        } else if (std::fabs(sx) > 0.15f) {
            g_heading -= sx * g_turn_speed * dt;
            ++g_turns;
        }
        {  // (round 130) seated on a train: the view's heading turns with the car, by the course his place travels
           // (cycle RE1: by John's own heading it ran away where he is not seated and turns to face the view himself (a
           // forced seat, a gun out: the view drifted 8 degrees in 4 s with no one turning): his heading is the game's to
           // change for other reasons, the way his place moves is the car's alone). The course over the last 0.4-0.8 s of
           // his place at the script ticks, at 2.5 m/s or more, smoothed; its change added to the view's heading (the
           // changes sum to the course's own change: nothing gathers on a straight)
            struct TrainSmp {
                double ms;
                float x, z;
            };
            static TrainSmp ring[64];
            static int rn = 0;
            static bool c_ok = false;
            static float c_s = 0.0f;  // the smoothed course, degrees (a heading: facing (-sin h, -cos h))
            static uint64_t tick_was = 0;
            const bool seat = g_train_turn.load(std::memory_order_relaxed) && (holster::game_gate() & 32) != 0 && have_st && !stick_steer && g_up_axis == 1;
            if (!seat) {
                rn = 0;
                c_ok = false;
            } else if (st.tick != tick_was && (rn == 0 || now - ring[rn - 1].ms >= 12.5)) {  // (the third review: 64 samples reach 0.8 s at any tick rate)
                tick_was = st.tick;
                int old = 0;
                while (old < rn && now - ring[old].ms > 800.0) ++old;
                if (old == 0 && rn == 64) old = 1;
                if (old) {
                    std::memmove(ring, ring + old, static_cast<size_t>(rn - old) * sizeof(TrainSmp));
                    rn -= old;
                }
                const TrainSmp* base = rn > 0 && now - ring[0].ms >= 400.0 ? &ring[0] : nullptr;  // the oldest kept, 0.4-0.8 s back
                ring[rn++] = {now, st.pos[0], st.pos[2]};
                bool moving = false;
                if (base) {
                    const float dx = st.pos[0] - base->x, dz = st.pos[2] - base->z, dist = std::hypot(dx, dz);
                    const float span_s = static_cast<float>(now - base->ms) * 0.001f;
                    if (dist >= 2.5f * span_s && dist < 60.0f * span_s) {  // (2.5 m/s or more; not a jump)
                        moving = true;
                        const float c = std::atan2(-dx, -dz) * 57.2957795f;
                        if (!c_ok) {
                            c_s = c;
                            c_ok = true;
                        } else {
                            const float d = (std::fmod(c - c_s + 540.0f, 360.0f) - 180.0f) * 0.3f;
                            c_s = std::fmod(c_s + d + 540.0f, 360.0f) - 180.0f;
                            if (std::fabs(d) < kTrainTurnMax) {
                                g_heading += d;
                                g_train_turned.store(g_train_turned.load(std::memory_order_relaxed) + std::fabs(d), std::memory_order_relaxed);
                            } else {
                                g_train_turn_refused.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                }
                if (base && !moving) c_ok = false;  // (stopped, or a jump: the course taken afresh after)
            }
        }
        bt_frame(st, have_st, riding, dt, now);  // [XR] BodyFollowsTurn (2026-10-27): off, one relaxed load
        g_heading = std::fmod(g_heading + 540.0f, 360.0f) - 180.0f;
        job.heading_deg = (job.weapons & 4u) ? g_ride_off : g_heading;
        g_heading_shared = g_heading;
        // [XR] BodyFollowsTurn: the heading commanded with the follow's total in it, for camera_lever's match (while on, or
        // a turn still to be shown after it went off); then nothing pushed and the match stopped
        if (g_bt_on.load(std::memory_order_relaxed) || std::fabs(bt_wrap(g_bt_total - camera_lever::body_turn_shown())) > 0.01f) {
            camera_lever::body_turn_push(g_heading, g_bt_total, !riding && !(have_st && (st.flags & RDRVR_ACTOR_CAM_AWAY)));
            g_bt_pushing = true;
        } else if (g_bt_pushing) {
            camera_lever::body_turn_stop();
            g_bt_pushing = false;
        }
        g_bt_total_pub.store(g_bt_total, std::memory_order_relaxed);
        g_bt_state_pub.store((g_bt_following ? 1 : 0) | (g_bt_beyond_ms >= 0.0 ? 2 : 0) | (g_bt_pushing ? 4 : 0), std::memory_order_relaxed);
        pc_frame(st, have_st, controllers, now);  // [XR] PhysicalCrouch: the edge's press, its check (2026-10-27)
        // (2026-10-28) its press on this frame's pad too: controls::pad mapped it at the frame's start, before the press
        // (the pulse's later frames come from pad() itself; the player's own L3 is read there, before any press)
        if (controllers && g_pc_press_frame) pad.buttons |= kPadL3;
        const bool have = g_height_follow && g_have_scale && g_height_s_valid && api::actor_state(&st) && st.head_valid;
        const float up_now = have ? (g_up_axis == 1 ? st.head_pos[1] - st.pos[1] : st.head_pos[2] - st.pos[2]) : 0.0f;
        const bool hold = have ? armed_hold(st, up_now, g_height_s, log::now_ms()) : (g_hold_now.store(false), false);
        // [XR] PhysicalCrouch (2026-10-27, roomscale.md C1): while the physical crouch owns John's crouch (its press
        // crouched him, or the user crouched with him already down), the height held at the standing view's (eased up
        // to it when he was down already), only a rise let through: the user's own drop is the view's, 1:1. After he
        // stands, held while his head bone is still below it (the stand's animation, at most 2 s). A crouch by L3 or the
        // game's, the user standing, follows as before. The still body keeps the game's height (not g_hold_now)
        bool pc_hold = have && g_pc_running && (g_pc_owned || g_pc_pend == 1);
        if (have && !pc_hold && g_pc_after) {
            if (g_pc_running && log::now_ms() - g_pc_after_ms < kPcAfterMs && up_now < g_height_s - 0.005f) pc_hold = true;
            else g_pc_after = false;
        }
        if (pc_hold) {
            const float target = std::fmax(up_now, g_pc_owned || g_pc_pend == 1 ? g_pc_own_h : 0.0f);
            if (target > g_height_s && target < 2.5f) {
                const float a = 1.0f - std::exp(-dt / (g_height_tau > 0.01f ? g_height_tau : 0.01f));
                g_height_s += (target - g_height_s) * a;
            }
            job.height = g_height_s + g_lift;
        } else if (hold) {  // run 10 item 8b: the height held (its last value), only a rise let through (a crouch left with the gun out)
            // (2026-11-04, item 1) back up from a kneel with an item in hand: eased to the height held before it
            const float up = g_item_restore > 0.0f ? std::fmax(up_now, g_item_restore) : up_now;
            if (up > g_height_s && up < 2.5f) {
                const float a = 1.0f - std::exp(-dt / (g_height_tau > 0.01f ? g_height_tau : 0.01f));
                g_height_s += (up - g_height_s) * a;
            }
            if (g_item_restore > 0.0f && g_height_s >= g_item_restore - 0.002f) {
                g_item_restore = 0.0f;
                g_item_restoring.store(false, std::memory_order_relaxed);
            }
            job.height = g_height_s + g_lift;
        } else if (g_height_follow && g_have_scale && api::actor_state(&st) && st.head_valid) {
            float up = g_up_axis == 1 ? st.head_pos[1] - st.pos[1] : st.head_pos[2] - st.pos[2];
            if (up > 0.2f && up < 2.5f) {
                float a = 1.0f - std::exp(-dt / (g_height_tau > 0.01f ? g_height_tau : 0.01f));
                g_height_s = g_height_s_valid ? g_height_s + (up - g_height_s) * a : up;
                g_height_s_valid = true;
                job.height = g_height_s + g_lift;
            }
        }
        if (g_height_s_valid && !pc_hold && !(have_st && (st.flags & RDRVR_ACTOR_CROUCHING))) g_pc_stand_h = g_height_s;  // the standing view's
        {  // (GOAL-doorway-and-poker item 3; [Body] TableGameHeadView, off by default; the user at poker: "Johns body is too
           // far forward. It is clipping into the table") at the table the view stood at the standing eye height over the
           // root, 0.05 m ahead (round 115: 1.69 m, the first-person moment's camera at the anchor's place), while John sat
           // with his head lower and over the table: his body drawn in front of the view, through the table. While a
           // minigame runs, the view at his head bone: its height (and SeatedLift) and its place ahead of the root along
           // the view's heading (and EyeForward); the plugin puts the anchor's camera there. Not to the side: the job has
           // no sideways offset
            // (the review) eased by HeightSmoothing as the height follow is, in and back out: the head's every move at the
            // table (chips, the cards) and the gate's switch never moved the view in one frame
            static float tg_h = 0.0f, tg_f = 0.0f, tg_r = 0.0f;
            static bool tg_on = false;
            const float tg_a = 1.0f - std::exp(-dt / (g_height_tau > 0.01f ? g_height_tau : 0.01f));
            // (round 128) at every table game (only poker raises the minigame's flag), and on a train's seat
            // (round 141) and whatever the option: a passenger in a vehicle (his seat's own place in it), and a mounted
            // gun kept in first person (the user: "John needs to be a little further back, currently the gun clips into
            // camera"; cycle RU8 on the mission's Gatling: his head 0.21 m behind the view put over his root, the gun's
            // rear cut by the near plane)
            const bool gun_view = g_on_mounted_gun_shared.load(std::memory_order_relaxed) && g_mounted_fp.load(std::memory_order_relaxed);
            const bool on = ((g_table_head.load(std::memory_order_relaxed) && holster::seated_view()) || passenger || gun_view) && have_st && st.head_valid;
            bool used = false;
            if (on) {
                const float hr_ = g_heading * 0.0174532925f, fx = -std::sin(hr_), fz = -std::cos(hr_);
                const float up = g_up_axis == 1 ? st.head_pos[1] - st.pos[1] : st.head_pos[2] - st.pos[2];
                const float ahead = (st.head_pos[0] - st.pos[0]) * fx + (st.head_pos[2] - st.pos[2]) * fz;
                // (round 130) and to the side: the heading's right is (-fz, fx) (y up; the plugin's `right`). On the train's
                // seat his head is 0.86-0.95 m from his place (the user's log), 0.32-0.37 m at the tables
                const float right = g_up_axis == 1 ? (st.head_pos[0] - st.pos[0]) * -fz + (st.head_pos[2] - st.pos[2]) * fx : 0.0f;
                if (up > 0.2f && up < 2.5f && std::hypot(ahead, right) < 1.5f) {
                    if (!tg_on) {  // from the view as it was
                        tg_h = job.height;
                        tg_f = job.forward;
                        tg_r = 0.0f;
                        tg_on = true;
                    }
                    tg_h += (up + g_lift - tg_h) * tg_a;
                    tg_f += (ahead + g_forward - tg_f) * tg_a;
                    tg_r += (right - tg_r) * tg_a;
                    job.height = tg_h;
                    job.forward = tg_f;
                    job.right = tg_r;
                    g_table_head_frames.fetch_add(1, std::memory_order_relaxed);
                    used = true;
                }
            }
            if (!used && tg_on) {  // eased back to the anchor's view
                tg_h += (job.height - tg_h) * tg_a;
                tg_f += (job.forward - tg_f) * tg_a;
                tg_r += (0.0f - tg_r) * tg_a;
                if (std::fabs(tg_h - job.height) < 0.005f && std::fabs(tg_f - job.forward) < 0.005f && std::fabs(tg_r) < 0.005f) {
                    tg_on = false;
                } else {
                    job.height = tg_h;
                    job.forward = tg_f;
                    job.right = tg_r;
                }
            }
            if (have_st) g_seat_heading_shared.store(st.heading_deg, std::memory_order_relaxed);
            g_table_view_active.store(used || tg_on, std::memory_order_relaxed);
            if (used != g_table_head_was) {
                g_table_head_was = used;
                log::info("[pose] a table game or a seat (gate %d): the view %s (TableGameHeadView)", holster::game_gate(),
                          used ? "at John's head" : "back at the anchor's eye height");
            }
        }
        {  // (round 117, the user at poker: "Johns body is too far forward. It is clipping into the table") while a minigame
           // runs, every 5 s: John's head against his root (height, ahead along his heading, right) and the view's place
           // (the job's height, its forward), to see where the seated head is against the view (no change: the log only)
            static double mg_log_ms = 0.0;
            {  // (round 125: John left his seat at poker and the game's skip never faded back in) said the moment his head
               // rises from seated (under 1.38 m over his root) to standing (over 1.48 m) while the minigame runs, with the pad's
               // fists line: what the mod sent
                static bool mg_seated = false;
                if (!(holster::game_gate() & 1)) {  // (the review: the minigame's flag only: at horseshoes John stands and bends)
                    mg_seated = false;
                } else if (have_st && st.head_valid) {
                    const float hh = g_up_axis == 1 ? st.head_pos[1] - st.pos[1] : st.head_pos[2] - st.pos[2];
                    if (hh < 1.38f) {
                        mg_seated = true;
                    } else if (mg_seated && hh > 1.48f) {
                        mg_seated = false;
                        log::warn("[pose] minigame: John left his seat (his head %.2f m above his root, his heading %.1f deg, weapon %d) | %s", hh,
                                  st.heading_deg, st.weapon, controls::fists_status().c_str());
                    }
                }
            }
            if (holster::seated_view() && have_st && st.head_valid && now - mg_log_ms >= 5000.0) {
                mg_log_ms = now;
                const float hr_ = g_heading * 0.0174532925f, fx = -std::sin(hr_), fz = -std::cos(hr_);
                const float dx = st.head_pos[0] - st.pos[0], dz = st.head_pos[2] - st.pos[2];
                log::info("[pose] seated (gate %d): John's head %.2f m above his root, %.2f m ahead and %.2f m right of it; the view %.2f m above "
                          "it, %.2f m ahead, %.2f m right (eye height%s%s, lift %.2f) | the view's heading %.1f deg, John's %.1f; turned with a "
                          "train's course %.1f deg so far (steps not taken %u)", holster::game_gate(),
                          g_up_axis == 1 ? st.head_pos[1] - st.pos[1] : st.head_pos[2] - st.pos[2],
                          dx * fx + dz * fz, dx * -fz + dz * fx, job.height, job.forward, job.right, hold ? " held" : "", pc_hold ? " crouch-held" : "",
                          g_lift, g_heading, st.heading_deg, g_train_turned.load(std::memory_order_relaxed),
                          g_train_turn_refused.load(std::memory_order_relaxed));
            }
        }
        int bits = (g_pc_want ? 1 : 0) | ((have_st && (st.flags & RDRVR_ACTOR_CROUCHING)) ? 2 : 0) | (g_pc_owned ? 4 : 0) | (g_pc_pend >= 0 ? 8 : 0) |
                   (g_pc_ref ? 16 : 0) | (g_pc_H_stage ? 32 : 0) | (pc_hold ? 64 : 0) | (g_pc_running ? 128 : 0) | (g_pc_defer ? 256 : 0) |
                   (g_pc_ref_wait ? 512 : 0);
        g_pc_bits_pub.store(bits, std::memory_order_relaxed);
        g_pc_job_h_pub.store(job.height, std::memory_order_relaxed);
        // walking: the left stick turned by the head's yaw relative to the body. Riding: the game steers the horse
        // relative to itself (a stick held left keeps turning it), so steering by the head turns the stick by the
        // angle from the horse's heading to where the head faces, which goes to zero as the horse comes round; with
        // stick steering it is not turned at all
        // [XR] RoomScale (2026-10-27): the player's own stick (the controllers', the gamepad's), then the room's command
        const float user = std::fmax(controllers ? std::sqrt(static_cast<float>(pad.lx) * pad.lx + static_cast<float>(pad.ly) * pad.ly) / 32767.0f : 0.0f,
                                     xinput::pad_left_mag());
        rs_cmd = rs_frame(st, have_st, user, now);
        float yaw = 0;
        if (camera_lever::head_yaw_deg(&yaw)) g_last_head_yaw = yaw;
        float stick_yaw = stick_steer ? 0.0f : g_last_head_yaw;
        if (riding && !stick_steer) stick_yaw = std::fmod(g_heading + g_last_head_yaw - st.mount_heading + 540.0f, 360.0f) - 180.0f;
        // the gamepad's left stick turned by the head's yaw (and its right x kept for turning), and the controllers' too
        xinput::set_pad_turn(true, stick_yaw);
        if (controllers) {
            float a = stick_yaw * 0.0174532925f, lx = pad.lx / 32767.0f, ly = pad.ly / 32767.0f;
            pad.lx = stick(lx * std::cos(a) - ly * std::sin(a));
            pad.ly = stick(lx * std::sin(a) + ly * std::cos(a));
            pad.rx = 0;  // the anchored camera is not the game's to turn
            if (lx != 0 || ly != 0) ++g_walk_frames;
        }
        if (rs_cmd.on && controllers) {  // the room's stick in the body's frame: after the head's turn (the offset is in it already)
            pad.lx = stick(rs_cmd.lx);
            pad.ly = stick(rs_cmd.ly);
        }
    }
    // [Comfort] ThirdPersonSnap: First person off, in VR, the controllers' stick
    if (!want && controllers && g_tps_cfg.load(std::memory_order_relaxed) && xr::submitting() && vr_mode::gameplay_stereo() &&
        !g_mounted_held_shared.load(std::memory_order_relaxed) && !aim::aiming()) {  // (the review: a turret, the aim, Dead Eye)
        tps_frame(pad, log::now_ms());
    } else if (g_tps_active || g_tps_owns_view) {  // (SN1: only the snap's own yaw cleared, never a test's)
        g_tps_active = false;
        camera_lever::set_view_yaw_target(false, 0.0f, 0.0f);
        g_tps_owns_view = false;
        g_tps_fade_ms = -1.0;  // (the review: an ease cut short is not resumed after)
        tps_stop_blink();
    } else if (g_tps_blink_ms >= 0.0) {
        tps_stop_blink();
    }
    static bool rs_drove = false;  // the room drove the gamepad's source last frame (its toggles not logged each walk)
    if (rs_cmd.on && !controllers) {  // a gamepad alone: the room's stick as the mod's source (the merge takes the further stick)
        xinput::PadState rp;
        rp.lx = stick(rs_cmd.lx);
        rp.ly = stick(rs_cmd.ly);
        xinput::set_source(true, rp, true);
        rs_drove = true;
    } else {
        xinput::set_source(controllers, controllers ? pad : xinput::PadState{}, rs_drove && !controllers);
        rs_drove = false;
    }
    api::set_camera_job(job);
}

}  // namespace

void init() {
    g_anchor = config::get_bool("Body", "CameraAnchor", false);
    g_keep_cam = config::get_bool("Body", "KeepAnchorCamera", true);
    g_mounted_fp = config::get_bool("Hands", "MountedGunFirstPerson", false);  // 2026-10-13: experimental, off
    camera_lever::train_cam_status(config::get_bool("Debug", "TrainCamera", true) ? 1 : 0);  // (round 129)
    camera_lever::near_fix_status(config::get_bool("Debug", "NearPlaneFix", true) ? 1 : 0);  // (round 130)
    g_train_turn = config::get_bool("Debug", "TrainTurn", true);                             // (round 130)
    camera_lever::set_first_person_moments(config::get_bool("Body", "FirstPersonMoments", true));  // 2026-10-12: on (the user's)
    g_height = config::get_float("Body", "EyeHeight", 0.0f);  // 0 = measured from the head bone
    g_auto_height = g_height <= 0;
    if (g_auto_height) g_height = 1.60f;  // until measured
    g_lift = config::get_float("Body", "SeatedLift", 0.09f);
    g_table_head = config::get_bool("Body", "TableGameHeadView", false);  // (GOAL-doorway-and-poker item 3: off by default)
    g_no_idles = config::get_bool("Body", "NoIdleAnims", true);
    g_hide_reticle = config::get_bool("Hands", "HideReticle", true);
    g_forward = config::get_float("Body", "EyeForward", 0.05f);
    g_up_axis = config::get_int("Body", "UpAxis", 1);
    g_orient_mode = config::get_int("Body", "OrientMode", 1);
    g_height_follow = config::get_bool("Body", "EyeHeightFollow", true);
    g_height_tau = config::get_float("Body", "HeightSmoothing", 0.35f);
    g_snap = config::get_string("Comfort", "TurnMode", "smooth") == "snap";
    g_tps_cfg = config::get_bool("Comfort", "ThirdPersonSnap", false);
    g_turn_speed = config::get_float("Comfort", "TurnSpeed", 90.0f);
    g_snap_angle = config::get_float("Comfort", "SnapAngle", 30.0f);
    {
        const float ta = config::get_float("Comfort", "ThirdPersonSnapAngle", 30.0f);
        g_tps_angle = std::isfinite(ta) ? std::fmin(90.0f, std::fmax(10.0f, ta)) : 30.0f;
    }
    g_steer_head = config::get_string("Horse", "Steering", "stick") == "head";
    g_saddle = config::get_bool("Horse", "SaddleAnchor", true);
    g_saddle_tau = config::get_float("Horse", "SaddleSmoothing", 0.15f);
    g_saddle_climb = config::get_bool("Horse", "SaddleClimb", true);
    g_ride_turn = config::get_bool("Horse", "StickTurn", true);
    g_armed_stand = config::get_string("Body", "ArmedStance", "stand") != "game";
    g_vehicle_heading = config::get_bool("Horse", "VehicleHeading", true);
    g_pc_on = config::get_bool("XR", "PhysicalCrouch", false);  // 2026-10-27: off until a headset round
    xr::set_stage_wanted(g_pc_on.load());  // (the floor's STAGE space made and located only while it is on)
    set_crouch_tuning(config::get_float("XR", "CrouchShare", 0.30f), config::get_float("XR", "StandShare", 0.18f),
                      config::get_float("XR", "CrouchHoldMs", 150.0f), config::get_float("XR", "StandingHeight", 1.65f), false,
                      config::get_float("XR", "CrouchQuickHoldMs", 50.0f));
    g_pc_H = g_pc_height_ini.load();
    g_rs_on = config::get_bool("XR", "RoomScale", false);  // 2026-10-27: off until a headset round
    g_rs_fade_on = config::get_bool("XR", "RoomScaleFade", true);
    set_room_tuning(config::get_float("XR", "RoomScaleDeadZone", 0.15f), config::get_float("XR", "RoomScaleStopRadius", 0.06f),
                    config::get_float("XR", "RoomScaleMinStick", 0.38f), config::get_float("XR", "RoomScaleMaxStick", 0.45f),
                    config::get_float("XR", "RoomScaleMaxOffset", 0.65f), false);
    set_room_response(config::get_float("XR", "RoomScaleLeanSlack", 0.02f), config::get_float("XR", "RoomScaleStartLead", 0.25f), false);
    g_rs_direct = config::get_bool("XR", "RoomScaleDirect", true);
    log::info("[pose] room scale %d: a walk beyond %.2f m (early with the head moving away, %.2f s ahead), ended within %.2f m, to %.2f m, "
              "the stick %.2f-%.2f, the lean from %.2f m of drop; blocked: the fade %d, the offset held at %.2f m",
              g_rs_on.load() ? 1 : 0, g_rs_dead.load(), g_rs_start_lead.load(), rs_snap_m(g_rs_dead.load()), g_rs_stop.load(), g_rs_min.load(),
              g_rs_max.load(), g_rs_lean_slack.load(), g_rs_fade_on.load() ? 1 : 0, g_rs_max_off.load());
    g_bt_on = config::get_bool("XR", "BodyFollowsTurn", false);  // 2026-10-27: off until a headset round
    set_body_turn_tuning(config::get_float("XR", "BodyTurnDeadZone", 35.0f), config::get_float("XR", "BodyTurnHoldMs", 100.0f),
                         config::get_float("XR", "BodyTurnSpeed", 300.0f), false, config::get_float("XR", "BodyTurnGain", 12.0f),
                         config::get_float("XR", "BodyTurnEaseS", 0.08f));
    g_bt_walk = config::get_bool("XR", "BodyTurnDuringWalk", true);
    log::info("[pose] body follows turn %d: the head %.0f deg over the body for %.0f ms turns John, at most %.0f deg/s (eased in over %.2f s, "
              "%.0f a second of what is left), during a room-scale walk %d", g_bt_on.load() ? 1 : 0, g_bt_dead.load(), g_bt_hold.load(),
              g_bt_speed.load(), g_bt_ease_s.load(), g_bt_gain.load(), g_bt_walk.load() ? 1 : 0);
    log::info("[pose] physical crouch %d: crouched from %.0f%% of the standing height, standing below %.0f%%, each held %.0f ms "
              "(%.0f ms at %.0f%% past it); standing height %.2f m without the headset's floor", g_pc_on.load() ? 1 : 0, g_pc_crouch.load() * 100.0f,
              g_pc_stand.load() * 100.0f, g_pc_hold.load(), std::fmin(g_pc_hold.load(), g_pc_quick.load()), kPcQuickShare * 100.0f,
              g_pc_height_ini.load());
    log::info("[pose] horse: steering by the %s, saddle anchor %d (smoothing %.2f s), the right stick turns the view %d", g_steer_head.load() ? "head" : "stick",
              g_saddle.load() ? 1 : 0, g_saddle_tau.load(), g_ride_turn.load() ? 1 : 0);
    d3d::add_frame_end_listener(on_frame_end);
}

void set_turning(bool snap, float speed_deg_s, float snap_deg) {
    g_snap = snap;
    g_turn_speed = speed_deg_s;
    g_snap_angle = snap_deg;
    log::info("[pose] turning: %s (%.0f deg/s, snap %.0f deg)", snap ? "snap" : "smooth", speed_deg_s, snap_deg);
}
bool snap_turning() { return g_snap; }
bool third_person_snap() { return g_tps_cfg.load(std::memory_order_relaxed); }
void set_third_person_snap(bool on, bool save) {
    if (g_tps_cfg.exchange(on) != on) log::info("[pose] snap turns in third person: %s", on ? "on" : "off");
    if (save) config::set("Comfort", "ThirdPersonSnap", on ? "1" : "0");
}
float third_person_snap_angle() { return g_tps_angle.load(std::memory_order_relaxed); }
void set_third_person_snap_angle(float deg, bool save) {
    deg = std::isfinite(deg) ? std::fmin(90.0f, std::fmax(10.0f, deg)) : 30.0f;
    if (g_tps_angle.exchange(deg) != deg) log::info("[pose] the third person snap: %.0f deg", deg);
    if (save) config::set("Comfort", "ThirdPersonSnapAngle", std::to_string(static_cast<int>(deg)));
}
std::string tps_command(const std::string& v) {  // "pose tpsnap [on|off]": [Comfort] ThirdPersonSnap for the session, its counters
    if (v == "on" || v == "off") g_tps_cfg = v == "on";
    char b[380];
    float md = 0.0f, mm = 0.0f, me = 0.0f;
    camera_lever::view_yaw_target_moves(&md, &mm, &me);
    std::snprintf(b, sizeof(b), "third person snap %d: snaps %llu, arrived %llu, timed out %llu, the last %.1f deg off after %.0f ms, the view's yaw now %.1f, "
                  "the drawn view moved %.2f deg %.3f m while held, the camera %.3f m from it at the end, blinks %llu, chained %llu",
                  g_tps_cfg.load() ? 1 : 0, static_cast<unsigned long long>(g_tps_snaps.load()), static_cast<unsigned long long>(g_tps_done.load()),
                  static_cast<unsigned long long>(g_tps_timeouts.load()), g_tps_last_err.load(), g_tps_last_ms.load(), camera_lever::view_yaw_frame(), md, mm, me, static_cast<unsigned long long>(g_tps_blinks.load()),
                  static_cast<unsigned long long>(g_tps_chained.load()));
    return b;
}
float turn_speed() { return g_turn_speed; }
float snap_angle() { return g_snap_angle; }

void on_recentre() { g_recentre = true; }

void set_orient_mode(int m) {
    g_orient_mode = m;
    log::info("[pose] scripted camera orientation mode %d", m);
}

bool armed_stand() { return g_armed_stand.load(); }
void set_armed_stand(bool on, bool save) {
    if (g_armed_stand.exchange(on) != on) log::info("[pose] a gun drawn: the view's height %s", on ? "held (standing)" : "the game's stance");
    if (save) config::set("Body", "ArmedStance", on ? "stand" : "game");
}
bool holds_armed_height(const RdrvrActorState&) { return g_hold_now.load(std::memory_order_relaxed); }  // the frame's verdict
bool item_restoring() { return g_item_restoring.load(std::memory_order_relaxed); }
uint64_t hold_after_frames() { return g_hold_after.load(std::memory_order_relaxed); }
bool vehicle_heading() { return g_vehicle_heading.load(); }
void set_vehicle_heading(bool on, bool save) {
    if (g_vehicle_heading.exchange(on) != on) log::info("[pose] driving: the view's heading %s", on ? "the vehicle's" : "as on foot");
    if (save) config::set("Horse", "VehicleHeading", on ? "1" : "0");
}
bool stick_turn_riding() { return g_ride_turn.load(); }
void set_stick_turn_riding(bool on, bool save) {
    if (g_ride_turn.exchange(on) != on) log::info("[pose] the right stick turns the view on horseback (StickTurn) %d", on ? 1 : 0);
    if (save) config::set("Horse", "StickTurn", on ? "1" : "0");
}
bool room_scale() { return g_rs_on.load(std::memory_order_relaxed); }
void set_room_scale(bool on, bool save) {
    if (g_rs_on.exchange(on) != on) log::info("[pose] room scale %s", on ? "on: a step in your room walks John under you" : "off");
    if (save) config::set("XR", "RoomScale", on ? "1" : "0");
}
bool room_fade() { return g_rs_fade_on.load(std::memory_order_relaxed); }
void set_room_fade(bool on, bool save) {
    g_rs_fade_on = on;
    if (save) config::set("XR", "RoomScaleFade", on ? "1" : "0");
}
void room_tuning(float* dead, float* stop, float* min_stick, float* max_stick, float* max_offset) {
    *dead = g_rs_dead.load();
    *stop = g_rs_stop_cfg.load();  // (as set: the menu writes it back unchanged)
    *min_stick = g_rs_min.load();
    *max_stick = g_rs_max.load();
    *max_offset = g_rs_max_off.load();
}
void set_room_tuning(float dead, float stop, float min_stick, float max_stick, float max_offset, bool save) {
    dead = !(dead >= kRsDeadFloor) ? kRsDeadFloor : dead > 0.40f ? 0.40f : dead;  // (2026-10-28: the floor 0.05, was 0.10)
    // (2026-10-29, the review) the radius as set, 0.02-0.40, and the one in use: at most 0.04 under the dead zone (the
    // hysteresis), but never under 0.02 (the dead zone's 0.05 floor: then 0.03 under it)
    stop = !(stop >= 0.02f) ? 0.02f : stop > 0.40f ? 0.40f : stop;
    g_rs_stop_cfg = stop;
    const float stop_cap = std::fmax(0.02f, dead - 0.04f);
    const float stop_use = stop > stop_cap ? stop_cap : stop;
    // (cycle r159) MinStick at least 0.38 whatever the pace (the game's walk starts between 0.30 and 0.35), the pace
    // 0.40-0.60, MaxOffset at least 0.30 (the fade black before the clamp)
    max_stick = !(max_stick >= kRsMaxStickFloor) ? kRsMaxStickFloor : max_stick > 0.60f ? 0.60f : max_stick;  // (0.60: still a walk)
    min_stick = !(min_stick >= kRsMinStickFloor) ? kRsMinStickFloor : min_stick > max_stick ? max_stick : min_stick;
    max_offset = !(max_offset >= dead + 0.10f) ? dead + 0.10f : max_offset > 1.0f ? 1.0f : max_offset;
    max_offset = max_offset < 0.30f ? 0.30f : max_offset;
    g_rs_dead = dead;
    g_rs_stop = stop_use;
    g_rs_min = min_stick;
    g_rs_max = max_stick;
    g_rs_max_off = max_offset;
    if (!save) return;
    char b[16];
    const struct {
        const char* key;
        float v;
    } kv[] = {{"RoomScaleDeadZone", dead}, {"RoomScaleStopRadius", stop}, {"RoomScaleMinStick", min_stick}, {"RoomScaleMaxStick", max_stick},
              {"RoomScaleMaxOffset", max_offset}};
    for (const auto& e : kv) {
        std::snprintf(b, sizeof(b), "%.2f", e.v);
        config::set("XR", e.key, b);
    }
}
void room_response(float* lean_slack, float* start_lead) {
    *lean_slack = g_rs_lean_slack.load();
    *start_lead = g_rs_start_lead.load();
}
bool room_direct() { return g_rs_direct.load(std::memory_order_relaxed); }
void set_room_direct(bool on, bool save) {
    if (g_rs_direct.exchange(on) != on) log::info("[pose] room scale: direct steps (RoomScaleDirect) %s", on ? "on" : "off");
    if (save) config::set("XR", "RoomScaleDirect", on ? "1" : "0");
}
void set_room_response(float lean_slack, float start_lead, bool save) {
    lean_slack = !(lean_slack >= 0.0f) ? 0.0f : lean_slack > 0.10f ? 0.10f : lean_slack;
    start_lead = !(start_lead >= 0.0f) ? 0.0f : start_lead > 0.5f ? 0.5f : start_lead;
    g_rs_lean_slack = lean_slack;
    g_rs_start_lead = start_lead;
    if (!save) return;
    char b[16];
    std::snprintf(b, sizeof(b), "%.3f", lean_slack);
    config::set("XR", "RoomScaleLeanSlack", b);
    std::snprintf(b, sizeof(b), "%.2f", start_lead);
    config::set("XR", "RoomScaleStartLead", b);
}
void set_room_take(bool on) {
    if (camera_lever::room_take() != on) log::info("[pose] room scale: the take-off %s (a test)", on ? "on" : "OFF: the view rides with John");
    camera_lever::room_set_take(on);
}
std::string room_status() {
    const int b = g_rs_bits_pub.load(std::memory_order_relaxed);
    float taken = 0.0f, judder = 0.0f, refused = 0.0f, ew[3] = {0, 0, 0}, ho[3] = {0, 0, 0};
    uint64_t frames = 0, rejects = 0;
    camera_lever::room_stats(&taken, &judder, &frames, &rejects, &refused);
    float jmean = 0.0f, jp95 = 0.0f;
    uint64_t jn = 0, jover = 0;
    camera_lever::room_judder_stats(&jmean, &jp95, &jn, &jover);
    const bool eye = camera_lever::eye_world(ew), head = camera_lever::head_offset(ho);
    char s[2000];
    std::snprintf(s, sizeof(s),
                  "room scale %s (running %d, suspended %d; gate: %s): d %.3f m (x %.3f z %.3f, lean %.3f), walking %d, blocked %d, owns %d, "
                  "the player's stick %d, stick (%.2f %.2f), speed %.2f m/s over 400 ms (his tick %.2f), judged %d, b0 %.2f, fade %.2f (geometry %.2f, on %d, reset %d), "
                  "slide %.3f m from %.2f m/s (%.3f s, %llu; the lead %.3f s) | dead %.2f stop %.2f stick %.2f-%.2f "
                  "max offset %.2f | walks %llu (again %llu, faced first %llu, put under %llu, stopped %llu, no snap %llu), blocks %llu, retries %llu, clamps %llu, resets %llu, moved %llu, staggers %llu, command frames %llu, the player's %llu | "
                  "taken %.3f m over %llu frames%s, not taken %.3f m (%llu over a walk), judder %.4f m (mean %.4f, p95 %.4f, over 5 mm %llu of %llu) | eye%s (%.3f %.3f %.3f), head offset (%.3f %.3f %.3f) | "
                  "quick: lean slack %.3f lead %.2f snap %.3f, away %.2f m/s, early %llu, turned %llu | direct %d: steps %llu, blocked %llu, probes %llu, no effect %llu",
                  g_rs_on.load() ? "on" : "off", (b & 1) ? 1 : 0, (b & 64) ? 1 : 0, g_rs_why.load(), g_rs_d_pub.load(), g_rs_dx_pub.load(),
                  g_rs_dz_pub.load(), g_rs_lean_pub.load(), (b & 2) ? 1 : 0, (b & 4) ? 1 : 0, (b & 8) ? 1 : 0, (b & 16) ? 1 : 0, g_rs_lx_pub.load(),
                  g_rs_ly_pub.load(), g_rs_speed_pub.load(), g_rs_hs_pub.load(), (b & 128) ? 1 : 0, g_rs_b0_pub.load(), g_rs_fade_pub.load(), g_rs_fade_geo_pub.load(), g_rs_fade_on.load() ? 1 : 0, (b & 32) ? 1 : 0, g_rs_slide_pub.load(), g_rs_slide_v_pub.load(),
                  g_rs_slide_t_pub.load(), static_cast<unsigned long long>(g_rs_slides.load()), kRsLeadS, g_rs_dead.load(),
                  g_rs_stop.load(), g_rs_min.load(), g_rs_max.load(), g_rs_max_off.load(), static_cast<unsigned long long>(g_rs_walks.load()), static_cast<unsigned long long>(g_rs_rewalks.load()),
                  static_cast<unsigned long long>(g_rs_faces.load()), static_cast<unsigned long long>(g_rs_snaps.load()),
                  static_cast<unsigned long long>(g_rs_snap_stops.load()), static_cast<unsigned long long>(g_rs_snap_skips.load()),
                  static_cast<unsigned long long>(g_rs_blocks.load()), static_cast<unsigned long long>(g_rs_retries.load()),
                  static_cast<unsigned long long>(g_rs_clamps.load()), static_cast<unsigned long long>(g_rs_resets.load()),
                  static_cast<unsigned long long>(g_rs_moved.load()), static_cast<unsigned long long>(g_rs_staggers.load()),
                  static_cast<unsigned long long>(g_rs_cmd_frames.load()), static_cast<unsigned long long>(g_rs_user_frames.load()), taken,
                  static_cast<unsigned long long>(frames), camera_lever::room_take() ? "" : " (the take-off OFF)", refused, static_cast<unsigned long long>(rejects), judder, jmean, jp95,
                  static_cast<unsigned long long>(jover), static_cast<unsigned long long>(jn), eye ? "" : " (none)", ew[0], ew[1], ew[2],
                  head ? ho[0] : 0.0f, head ? ho[1] : 0.0f, head ? ho[2] : 0.0f, g_rs_lean_slack.load(), g_rs_start_lead.load(),
                  rs_snap_m(g_rs_dead.load()), g_rs_vout_pub.load(), static_cast<unsigned long long>(g_rs_early.load()),
                  static_cast<unsigned long long>(g_rs_turned.load()), g_rs_direct.load() ? 1 : 0, static_cast<unsigned long long>(g_rsd_steps.load()),
                  static_cast<unsigned long long>(g_rsd_blocked.load()), static_cast<unsigned long long>(g_rsd_probes.load()),
                  static_cast<unsigned long long>(g_rsd_noeffect.load()));
    return s;
}
bool body_follows_turn() { return g_bt_on.load(std::memory_order_relaxed); }
float body_turn_total_deg() { return g_bt_total_pub.load(std::memory_order_relaxed); }
float body_turn_steps_deg() { return g_bt_steps.load(std::memory_order_relaxed); }
void set_body_follows_turn(bool on, bool save) {
    if (g_bt_on.exchange(on) != on) log::info("[pose] body follows turn %s", on ? "on: your physical turn turns John" : "off");
    if (save) config::set("XR", "BodyFollowsTurn", on ? "1" : "0");
}
bool body_turn_during_walk() { return g_bt_walk.load(std::memory_order_relaxed); }
void set_body_turn_during_walk(bool on, bool save) {
    if (g_bt_walk.exchange(on) != on) log::info("[pose] body follows turn during a room-scale walk %d", on ? 1 : 0);
    if (save) config::set("XR", "BodyTurnDuringWalk", on ? "1" : "0");
}
void body_turn_tuning(float* dead, float* hold_ms, float* speed) {
    *dead = g_bt_dead.load();
    *hold_ms = g_bt_hold.load();
    *speed = g_bt_speed.load();
}
void set_body_turn_tuning(float dead, float hold_ms, float speed, bool save, float gain, float ease_s) {
    dead = !(dead >= 20.0f) ? 20.0f : dead > 90.0f ? 90.0f : dead;
    hold_ms = !(hold_ms >= 0.0f) ? 0.0f : hold_ms > 1500.0f ? 1500.0f : hold_ms;
    speed = !(speed >= 30.0f) ? 30.0f : speed > 360.0f ? 360.0f : speed;
    g_bt_dead = dead;
    g_bt_hold = hold_ms;
    g_bt_speed = speed;
    // (2026-10-28) the tail's gain (2-30 a second) and the ease-in (0-0.5 s); negative: unchanged. Ini only
    if (gain >= 0.0f) g_bt_gain = gain < 2.0f ? 2.0f : gain > 30.0f ? 30.0f : gain;
    if (ease_s >= 0.0f) g_bt_ease_s = ease_s > 0.5f ? 0.5f : ease_s;
    if (!save) return;
    char b[16];
    std::snprintf(b, sizeof(b), "%.0f", dead);
    config::set("XR", "BodyTurnDeadZone", b);
    std::snprintf(b, sizeof(b), "%.0f", hold_ms);
    config::set("XR", "BodyTurnHoldMs", b);
    std::snprintf(b, sizeof(b), "%.0f", speed);
    config::set("XR", "BodyTurnSpeed", b);
}
std::string body_turn_status() {
    const int b = g_bt_state_pub.load(std::memory_order_relaxed);
    char s[1200];
    std::snprintf(s, sizeof(s),
                  "body follows turn %s (gate: %s): the head %.1f deg over the body, following %d, beyond the dead zone %d, pushing %d, "
                  "body heading %.1f deg, the follow's total %.2f deg | dead %.0f hold %.0f speed %.0f gain %.1f ease %.2f walk %d | follows %llu, frames %llu | "
                  "the stick: %s %.0f deg/s, snap %.0f | %s",
                  g_bt_on.load() ? "on" : "off", g_bt_why.load(), g_bt_rel_pub.load(), (b & 1) ? 1 : 0, (b & 2) ? 1 : 0, (b & 4) ? 1 : 0,
                  g_heading_shared.load(), g_bt_total_pub.load(), g_bt_dead.load(), g_bt_hold.load(), g_bt_speed.load(), g_bt_gain.load(),
                  g_bt_ease_s.load(), g_bt_walk.load() ? 1 : 0,
                  static_cast<unsigned long long>(g_bt_follows.load()), static_cast<unsigned long long>(g_bt_frames.load()),
                  g_snap ? "snap" : "smooth", g_turn_speed, g_snap_angle, camera_lever::body_turn_status().c_str());
    return s;
}
bool physical_crouch() { return g_pc_on.load(std::memory_order_relaxed); }
bool crouch_keeps_recentre_height() {
    if (!g_pc_on.load(std::memory_order_relaxed)) return false;
    RdrvrActorState st{};
    return api::actor_state(&st) && (st.flags & RDRVR_ACTOR_CROUCHING);
}
void set_physical_crouch(bool on, bool save) {
    if (g_pc_on.exchange(on) != on) log::info("[pose] physical crouch %s", on ? "on: your own crouch crouches John" : "off");
    xr::set_stage_wanted(on);
    if (save) config::set("XR", "PhysicalCrouch", on ? "1" : "0");
}
float crouch_share() { return g_pc_crouch.load(std::memory_order_relaxed); }
void set_crouch_share(float share, bool save) {
    set_crouch_tuning(share, g_pc_stand.load(), g_pc_hold.load(), g_pc_height_ini.load(), false, -1.0f);
    if (!save) return;
    char b[16];
    std::snprintf(b, sizeof(b), "%.2f", g_pc_crouch.load());
    config::set("XR", "CrouchShare", b);
    std::snprintf(b, sizeof(b), "%.2f", g_pc_stand.load());  // (kept under it)
    config::set("XR", "StandShare", b);
}
float crouch_hold_ms() { return g_pc_hold.load(std::memory_order_relaxed); }
void set_crouch_hold(float hold_ms, bool save) {
    set_crouch_tuning(g_pc_crouch.load(), g_pc_stand.load(), hold_ms, g_pc_height_ini.load(), false, -1.0f);
    if (!save) return;
    char b[16];
    std::snprintf(b, sizeof(b), "%.0f", g_pc_hold.load());
    config::set("XR", "CrouchHoldMs", b);
}
void set_crouch_tuning(float crouch, float stand, float hold_ms, float standing_m, bool save, float quick_ms) {
    crouch = !(crouch >= 0.15f) ? 0.15f : crouch > 0.5f ? 0.5f : crouch;
    stand = !(stand >= 0.05f) ? 0.05f : stand > crouch - 0.04f ? crouch - 0.04f : stand;  // the hysteresis kept
    // (2026-10-29, the review: 0 defeated the quick hold's floor) 30 ms at the least, as the quick hold
    hold_ms = !(hold_ms >= 30.0f) ? 30.0f : hold_ms > 2000.0f ? 2000.0f : hold_ms;
    standing_m = !(standing_m >= 1.0f) ? 1.0f : standing_m > 2.3f ? 2.3f : standing_m;
    g_pc_crouch = crouch;
    g_pc_stand = stand;
    g_pc_hold = hold_ms;
    g_pc_height_ini = standing_m;
    // (2026-10-28) the quick hold (negative: unchanged), at least 30 ms (a frame or two of noise is never a crouch);
    // above CrouchHoldMs it is CrouchHoldMs (pc_hold_ms), so setting it so turns the shrink off
    if (quick_ms >= 0.0f) g_pc_quick = quick_ms < 30.0f ? 30.0f : quick_ms > 2000.0f ? 2000.0f : quick_ms;
    if (!save) return;
    char b[16];
    std::snprintf(b, sizeof(b), "%.2f", crouch);
    config::set("XR", "CrouchShare", b);
    std::snprintf(b, sizeof(b), "%.2f", stand);
    config::set("XR", "StandShare", b);
    std::snprintf(b, sizeof(b), "%.0f", hold_ms);
    config::set("XR", "CrouchHoldMs", b);
    if (quick_ms >= 0.0f) {
        std::snprintf(b, sizeof(b), "%.0f", g_pc_quick.load());
        config::set("XR", "CrouchQuickHoldMs", b);
    }
    std::snprintf(b, sizeof(b), "%.2f", standing_m);
    config::set("XR", "StandingHeight", b);
}
void calibrate_standing(double ms) {
    g_pc_cal_fail_ms = 0.0;
    g_pc_cal_at = std::fmax(1.0, log::now_ms() + (ms > 0.0 ? ms : 0.0));  // (a newer request replaces it; none is lost: pc_reference's exchange)
    log::info("[pose] physical crouch: the standing height taken in %.1f s (stand straight)", ms / 1000.0);
}
const char* standing_calibrate_message() {
    const double f = g_pc_cal_fail_ms.load(std::memory_order_relaxed);
    if (!(f > 0.0 && log::now_ms() - f < 8000.0)) {
        // (the third review) a reference waiting for a standing head (a recentre made crouched): said, and how to end it
        if (g_pc_on.load(std::memory_order_relaxed) && (g_pc_bits_pub.load(std::memory_order_relaxed) & 512))
            return "Waiting for you to stand up straight (or press Set my standing height) before crouching works again.";
        return nullptr;
    }
    return g_pc_cal_fail_why.load(std::memory_order_relaxed) == 2 ? "John is crouched: stand up (L3) first, then set your standing height."
                                                                   : "Your standing height was not set: the headset was not tracked. Try again.";
}
double standing_calibrate_in() {
    const double at = g_pc_cal_at.load(std::memory_order_relaxed);
    return at > 0.0 ? std::fmax(0.0, at - log::now_ms()) : -1.0;
}
float standing_height(bool* from_floor) {
    const int bits = g_pc_bits_pub.load(std::memory_order_relaxed);
    if (from_floor) *from_floor = (bits & 32) != 0;
    return (bits & 16) ? g_pc_H_pub.load(std::memory_order_relaxed) : g_pc_height_ini.load(std::memory_order_relaxed);
}
std::string crouch_status() {
    const int b = g_pc_bits_pub.load(std::memory_order_relaxed);
    const float H = (b & 16) ? g_pc_H_pub.load() : g_pc_height_ini.load();
    float ho[3] = {0, 0, 0};
    const bool head = camera_lever::head_offset(ho);
    const float jh = g_pc_job_h_pub.load();
    const double cal = standing_calibrate_in();
    char neck_s[24] = "none";  // the neck's raw LOCAL height now (the status: is the head seen moving)
    if (float nl[3]; camera_lever::neck_local(nl)) std::snprintf(neck_s, sizeof(neck_s), "%.3f", nl[1]);
    char s[1200];
    std::snprintf(s, sizeof(s),
                  "physical crouch %s (running %d): drop %.3f m (lean %.3f), H %.3f m (%s; without the floor %.2f), y_stand %.3f (reference %d: taken %llu, kept %llu, waiting %d; "
                  "calibrate %s, refused %llu), crouch at %.3f m (%.2f), stand at %.3f m (%.2f), hold %.0f ms (quick %.0f) | want %d have %d owned %d pending %d, "
                  "presses %llu, retries %llu, refusals %llu (the last: %s), deferred %llu (pressed %llu, dropped %llu, waiting %d), gave up %llu, "
                  "edges %llu, recentres %llu, chords kept %llu | "
                  "view held %d, eye %.3f (anchor %.3f + head %.3f%s) | tracked %d (shown %d, position %d), neck y %s",
                  g_pc_on.load() ? "on" : "off", (b & 128) ? 1 : 0, g_pc_drop_pub.load(), g_pc_lean_pub.load(), H, (b & 32) ? "stage" : "ini", g_pc_height_ini.load(),
                  g_pc_ystand_pub.load(), (b & 16) ? 1 : 0, static_cast<unsigned long long>(g_pc_refs.load()),
                  static_cast<unsigned long long>(g_pc_kept.load()), (b & 512) ? 1 : 0,
                  cal >= 0.0 ? (std::to_string(static_cast<int>(cal)) + " ms").c_str() : "none", static_cast<unsigned long long>(g_pc_cal_refused.load()),
                  g_pc_crouch.load() * H, g_pc_crouch.load(), g_pc_stand.load() * H, g_pc_stand.load(), g_pc_hold.load(),
                  std::fmin(g_pc_hold.load(), g_pc_quick.load()), (b & 1) ? 1 : 0,
                  (b & 2) ? 1 : 0, (b & 4) ? 1 : 0, (b & 8) ? 1 : 0, static_cast<unsigned long long>(g_pc_presses.load()),
                  static_cast<unsigned long long>(g_pc_retries.load()), static_cast<unsigned long long>(g_pc_refusals.load()), g_pc_why.load(),
                  static_cast<unsigned long long>(g_pc_deferred.load()), static_cast<unsigned long long>(g_pc_def_pressed.load()),
                  static_cast<unsigned long long>(g_pc_def_dropped.load()), (b & 256) ? 1 : 0, static_cast<unsigned long long>(g_pc_giveups.load()), static_cast<unsigned long long>(g_pc_edges.load()),
                  static_cast<unsigned long long>(g_pc_recentres.load()), static_cast<unsigned long long>(controls::chords_held()),
                  (b & 64) ? 1 : 0, jh + ho[1], jh, ho[1], head ? "" : ", no head offset", xr::head_tracked() ? 1 : 0,
                  xr::session_focused() ? 1 : 0, xr::head_position_valid() ? 1 : 0, neck_s);
    return s;
}
float ride_turn_deg() { return g_ride_off_shared.load(std::memory_order_relaxed); }
float torso_heading_deg() {
    if (g_table_view_active.load(std::memory_order_relaxed)) return g_seat_heading_shared.load(std::memory_order_relaxed);  // (round 142)
    return g_heading_shared.load(std::memory_order_relaxed) - g_ride_off_shared.load(std::memory_order_relaxed);
}
bool steer_by_head() { return g_steer_head.load(); }
void set_steer_by_head(bool on) {
    if (g_steer_head.exchange(on) != on) log::info("[pose] horse steering by the %s", on ? "head" : "stick");
    config::set("Horse", "Steering", on ? "head" : "stick");
}
bool saddle_anchor() { return g_saddle.load(); }
bool saddle_camera(float* lift, float* forward) {
    if (!g_job_saddle.load(std::memory_order_relaxed)) return false;
    *lift = g_job_saddle_h.load(std::memory_order_relaxed);
    *forward = g_job_saddle_f.load(std::memory_order_relaxed);
    return true;
}
bool saddle_climb() { return g_saddle_climb.load(); }
void set_saddle_climb(bool on, bool save) {
    if (g_saddle_climb.exchange(on) != on) log::info("[pose] the saddle anchor follows climbs %d", on ? 1 : 0);
    if (save) config::set("Horse", "SaddleClimb", on ? "1" : "0");
}
std::string trace_command(const std::string& verb, const std::string& arg) {
    if (verb == "on" || verb == "off") {
        g_trace_on = verb == "on";
    } else if (verb == "reset") {
        std::lock_guard lock(g_trace_mutex);
        g_trace.clear();
    } else if (verb == "dump") {
        std::vector<TraceRow> rows;
        {
            std::lock_guard lock(g_trace_mutex);
            rows = g_trace;
        }
        FILE* fp = nullptr;
        if (arg.empty() || fopen_s(&fp, arg.c_str(), "w") != 0 || !fp) return "ERROR cannot write " + arg;
        std::fprintf(fp, "ms,tick,mount_x,mount_y,mount_z,mount_heading,cam_x,cam_y,cam_z,head_x,head_y,head_z,seat_x,seat_y,seat_z,h,f\n");
        for (const TraceRow& r : rows)
            std::fprintf(fp, "%.1f,%llu,%.4f,%.4f,%.4f,%.2f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", r.ms,
                         static_cast<unsigned long long>(r.tick), r.mount[0], r.mount[1], r.mount[2], r.mh, r.cam[0], r.cam[1], r.cam[2], r.head[0],
                         r.head[1], r.head[2], r.seat[0], r.seat[1], r.seat[2], r.h, r.f);
        std::fclose(fp);
        return "trace: " + std::to_string(rows.size()) + " rows written to " + arg;
    }
    std::lock_guard lock(g_trace_mutex);
    return std::string("trace ") + (g_trace_on.load() ? "on" : "off") + ", " + std::to_string(g_trace.size()) + " rows; saddle climb " +
           (g_saddle_climb.load() ? "on" : "off");
}
float saddle_smoothing() { return g_saddle_tau.load(); }
void reset_ride_jitter() {
    g_jit_sum = 0.0;
    g_jit_abs = 0.0;
    g_jit_n = 0;
}
void set_saddle(bool on, float tau, bool save) {
    g_saddle = on;
    g_saddle_tau = tau < 0.0f ? 0.0f : tau > 1.0f ? 1.0f : tau;
    if (!save) return;
    config::set("Horse", "SaddleAnchor", on ? "1" : "0");
    char b[32];
    std::snprintf(b, sizeof(b), "%.2f", g_saddle_tau.load());
    config::set("Horse", "SaddleSmoothing", b);
}

bool anchor_active() { return g_active_shared.load(std::memory_order_relaxed); }
bool mod_hides_reticle() { return g_mod_hides_reticle.load(std::memory_order_relaxed); }
bool on_mounted_gun() { return g_on_mounted_gun_shared.load(std::memory_order_relaxed); }
bool mounted_gun_held() { return g_mounted_held_shared.load(std::memory_order_relaxed); }
bool mounted_first_person() { return g_mounted_fp.load(std::memory_order_relaxed); }
void set_mounted_first_person(bool on, bool save) {
    if (g_mounted_fp.exchange(on) != on) log::info("[pose] a mounted gun: first person %s", on ? "kept (the motion aim)" : "left to the game");
    if (save) config::set("Hands", "MountedGunFirstPerson", on ? "1" : "0");
}
bool anchor_enabled() { return g_anchor.load(std::memory_order_relaxed); }
float body_heading_deg() { return g_heading_shared.load(std::memory_order_relaxed); }
float eye_lift() { return g_lift; }
bool table_game_head_view() { return g_table_head.load(std::memory_order_relaxed); }
void set_table_game_head_view(bool on) {
    if (g_table_head.exchange(on) != on) log::info("[pose] TableGameHeadView %s", on ? "on" : "off");
}
uint64_t table_game_head_frames() { return g_table_head_frames.load(std::memory_order_relaxed); }
bool table_game_view_active() { return g_table_view_active.load(std::memory_order_relaxed); }
void set_eye_lift(float m) {
    g_lift = m < -0.3f ? -0.3f : m > 0.4f ? 0.4f : m;
}

void set_keep_camera(bool on) {
    if (g_keep_cam.exchange(on) != on) log::info("[pose] the anchor's camera kept current (KeepAnchorCamera) %d", on ? 1 : 0);
}
void set_anchor(bool on) {
    g_anchor = on;
    log::info("[pose] camera anchor switch %s", on ? "on" : "off");
}

void status_text(char* out, size_t len) {
    RdrvrActorState st{};
    bool have = api::actor_state(&st);
    std::snprintf(out, len,
                  "anchor %s (%s), heading %.1f deg, head yaw %.1f, eye height %.3f%s, turns %llu, walk frames %llu | actor %s (%.2f %.2f %.2f) "
                  "heading %.1f, camera %d at (%.2f %.2f %.2f), tick %llu | mount %d (%.2f %.2f %.2f) heading %.1f, saddle %s h %.2f f %.2f, "
                  "steer %s, ride turn %d %.1f deg (turns %llu), ride frames %llu, jitter %.4f m/tick (camera height %.4f) over %llu | "
                  "armed hold %d (after the stance %llu, LT %.0f ms ago), the head's height %.4f; an item in hand held %llu, let down %llu",
                  g_anchor.load() ? "on" : "off", g_active ? "active" : "idle", g_heading, g_last_head_yaw, g_height,
                  g_have_scale ? " (head bone)" : g_auto_height ? " (not yet measured)" : " (ini)",
                  static_cast<unsigned long long>(g_turns), static_cast<unsigned long long>(g_walk_frames), have ? "" : "none",
                  st.pos[0], st.pos[1], st.pos[2], st.heading_deg, st.camera, st.camera_pos[0], st.camera_pos[1], st.camera_pos[2],
                  static_cast<unsigned long long>(st.tick), st.mount, st.mount_pos[0], st.mount_pos[1], st.mount_pos[2], st.mount_heading,
                  g_saddle.load() ? "on" : "off", g_saddle_h, g_saddle_f, g_steer_head.load() ? "head" : "stick", g_ride_turn.load() ? 1 : 0,
                  g_ride_off_shared.load(), static_cast<unsigned long long>(g_ride_turns),
                  static_cast<unsigned long long>(g_ride_frames), g_jit_n ? std::sqrt(g_jit_sum / static_cast<double>(g_jit_n)) : 0.0,
                  g_jit_n ? std::sqrt(g_jit_abs / static_cast<double>(g_jit_n)) : 0.0,
                  static_cast<unsigned long long>(g_jit_n), g_hold_now.load() ? 1 : 0,
                  static_cast<unsigned long long>(g_hold_after.load()), log::now_ms() - controls::last_lt_ms(), g_height_s,
                  static_cast<unsigned long long>(g_hold_items.load()), static_cast<unsigned long long>(g_hold_items_deep.load()));
}

}  // namespace rdrvr::pose
