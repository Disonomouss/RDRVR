#include "core/actions.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>

#include "core/aim.h"
#include "core/api.h"
#include "core/audio.h"
#include "core/body.h"
#include "core/gestures.h"
#include "core/config.h"
#include "core/controllers.h"
#include "core/controls.h"
#include "core/d3d_hooks.h"
#include "core/dual.h"
#include "core/hands.h"
#include "core/holster.h"
#include "core/log.h"
#include "core/menu.h"
#include "core/pose.h"
#include "core/reload.h"
#include "core/round_draw.h"
#include "core/whistle.h"

namespace rdrvr::actions {
namespace {

// the switches ([Reload], research\run3\manualactions.md 6.2)
std::atomic<bool> g_on{false}, g_revolver{true}, g_bolt{true}, g_lever{true}, g_pump{true}, g_load_open{true}, g_chamber_load{true};
std::atomic<bool> g_hints{true};  // [Reload] ActionHints: a ring where the action wants the hand while it holds the shot
float g_flick_ms = 300.0f;        // [Reload] OpenFlickMs: the stick's flick down and back within this (0: any push)
double g_flick_t0 = 0.0;          // when the push passed OpenStickY (filter_stick's thread)
float g_open_y = 0.85f, g_close_speed = 6.0f, g_close_min_ms = 300.0f, g_bolt_r = 0.07f, g_bolt_travel = 0.06f;
float g_lever_speed = 5.0f, g_lever_window_ms = 400.0f, g_pump_travel = 0.06f;
float g_bolt_off[3] = {0.035f, 0.030f, 0.090f};  // right, up, forward from the gun hand's wrist target, along the barrel

// [Reload] ManualBreak (off): the Double-barrel (and run 6 item 9d: the Sawed-off) broken open by the stick's flick, the barrels drawn tipped down (31
// degrees, the game's own reload measured: body.cpp open_gun), shut by a flick of the gun upward or by the off hand
// gripping the open barrels and swinging them up (they follow the hand)
std::atomic<bool> g_break{false};
// [Reload] BreakOpenClick (2026-10-31, the off-hand build step 1, R7; the user: "All break open guns should be opened with
// stick flicked down then clicked", and 2026-10-31's answer 7: "Hold down and click"): with ManualBreak, the stick held
// down and clicked opens the Double-barrel and the Sawed-off, as the revolvers (RevolverControl=cock); a plain flick no
// longer opens them (off: the flick, as before)
std::atomic<bool> g_break_click{true};
// [Horse] LeftFlickCocks (2026-10-31, the off-hand build step 1, R6; the user: "On horseback, no way to cock hammer, turning
// horse around with stick flicked down is not needed, use that input to cock hammer"): riding, a revolver in the layout's
// off controller (the second gun, the copy, or the gun in hand drawn by that hand) is worked by that controller's stick
// as on foot: its quick flick down cocks, its pull back held DualWieldOpenWaitMs before the horse gets it (off: riding,
// that stick is the horse's alone and the gun in hand drawn by it is the right stick's, as before)
std::atomic<bool> g_ride_cock{true};
// [Reload] BreechParts (off; run 6 item 9g): the single-shot rifles (the Springfield, the Rolling Block, the Buffalo)
std::atomic<bool> g_breech_parts{false};
// [Reload] SemiAutoParts (off; run 6 item 9h): the Semi-Auto Shotgun's bolt racked by the off hand after a load from empty
std::atomic<bool> g_semi_parts{false};
float g_break_deg = kBreakOpenDeg;  // the barrels' opening (the game's reload: 31 held, 33.5 at the swing's end; round 13: 45)
float g_barrel_r = 0.15f;          // [Reload] BarrelGripRadius: the off hand within this of the open barrels takes them
const float kHingeAlong = 0.10f;   // the hinge from the gun hand's wrist: along the barrel, up (m; the loading point's)
const float kHingeUp = 0.03f;

// [Reload] PistolMagazines and PistolSlides (2026-10-28, off; GOAL-weapons-handling.md item 3, research\run28\magazines.md):
// the magazine pistols (the Semi-Automatic Pistol, a toggle lock; the High Power, a slide) and the Mauser (a bolt, its
// fixed magazine loaded from the top by a stripper clip)
std::atomic<bool> g_mags{false}, g_slides{false};
enum Kind { kNone, kRevolver, kTopBreak, kBolt, kLever, kPump, kBreak, kBreech, kSemi, kPistol, kMauser, kMuzzle };
Kind kind_of(int w) {
    switch (w) {
        case 4: case 6: case 7: return kRevolver;  // the Cattleman, the Double-Action, the LeMat (a loading gate)
        case 5: return kTopBreak;                  // the Schofield
        // (2026-11-01, the off-hand build step 8, R14) the Undead Nightmare's Explosive Rifle: the Bolt Action's bolt (its
        // "sliding" bone 2 at (0 0.07632 0.00651), a turn and a slide: fragments.rpf rifle_explosive01x.wft)
        case 13: case 20: case 31: return kBolt;   // the Bolt Action, the Carcano, the Explosive Rifle
        case 0: case 8: case 9: case 10: case 11: return kLever;  // the Volcanic, the Carbine, Winchester, Henry, Evans
        case 17: return kPump;
        case 15: case 16: return kBreak;           // the Sawed-off, the Double-barrel
        case 12: case 14: case 19: return kBreech;  // the Springfield, the Buffalo, the Rolling Block (single shots)
        case 18: return kSemi;                     // the Semi-Auto Shotgun
        case 1: case 2: return kPistol;            // the Semi-Automatic Pistol, the High Power (a magazine in the grip)
        case 3: return kMauser;                    // the Mauser (a stripper clip from the top)
        case 34: return kMuzzle;                   // (step 8, R14) the Blunderbuss: a muzzle-loader (BreechParts' family)
        default: return kNone;
    }
}
const char* kind_name(Kind k) {
    static const char* const n[] = {"none", "revolver", "top-break", "bolt", "lever", "pump", "break", "breech", "semi-auto", "pistol", "Mauser", "muzzle-loader"};
    return n[k];
}
bool kind_on(Kind k) {
    return (k == kRevolver || k == kTopBreak) ? g_revolver.load() : k == kBolt ? g_bolt.load() : k == kLever ? g_lever.load() : k == kPump ? g_pump.load()
         : k == kBreak ? g_break.load() : (k == kBreech || k == kMuzzle) ? g_breech_parts.load() : k == kSemi ? g_semi_parts.load()
         : (k == kPistol || k == kMauser) ? g_mags.load() || g_slides.load() : false;
}
bool opens(Kind k) { return k == kRevolver || k == kTopBreak || k == kBreak; }  // opened by the stick, loaded only open
// (2026-11-01, the off-hand build step 4, S4) a second gun its own stick opens ([Hands] DualWieldOpen): the revolvers, the
// top-break and now the break-opens (the Sawed-off, which pairs as a sidearm: P1)
bool sec_opens(Kind k) { return k == kRevolver || k == kTopBreak || k == kBreak; }

// the state: the frame end writes (one thread: the pad poll and the frame end both run there); the test channel reads
std::mutex g_mutex;
int g_weapon = -1;
Kind g_kind = kNone;
bool g_open = false, g_needs = false;  // a revolver open; a shot not cycled
double g_open_ms = 0;
float g_last_clip = -1.0f;
float g_last_clip_max = -1.0f;  // (2026-10-31, step 7, R11) the gun in hand's clip size, the frame's (hint's "full")
bool g_was_empty = false;
std::atomic<bool> g_blocked{false}, g_rt_rearm{false};
std::atomic<bool> g_open_req{false};  // filter_stick's flick, consumed by the frame end
std::atomic<bool> g_open_cyl{false};  // [Reload] OpenCylinder (run 5 item 3)
std::atomic<float> g_open_amt{0.0f};
std::atomic<int> g_open_weapon{-1};
// [Reload] OpenCylinder (run 6 item 9b): each round put into a revolver by hand turns its cylinder one chamber (the
// count per eWeapon, never wrapped; drawn eased, a chamber in 0.15 s)
std::atomic<int> g_cyl_rounds[40] = {};
std::atomic<float> g_cyl_drawn{0.0f};
std::atomic<int> g_cyl_weapon{-1};
// the break action's barrels in the off hand: taken where the hand met them (its angle about the hinge and the drawn
// opening then), followed while the grip is held, shut at the top
bool g_barrel_hold = false, g_barrel_grip_was = false, g_barrel_take = false;  // take: gripped through the opening
float g_barrel_phi0 = 0.0f, g_barrel_a0 = 0.0f, g_barrel_d = 1e9f, g_barrel_at = 0.0f;
std::atomic<float> g_barrel_amt{1.0f};
std::atomic<bool> g_barrel_want[2] = {false, false};
std::atomic<bool> g_break_open{false};  // the Double-barrel open (holster.cpp: no two-handed hold meanwhile)
// [Reload] BarrelHandSnap (run 6 item 7, on; acts only with ManualBreak): while the off hand holds the open barrels,
// John's hand is drawn on them (body.cpp: the foregrip's grip point turned with the barrels); the weight eased in over
// 0.1 s at the take, out over 0.1 s at the shut or the let-go (the two-handed hold takes over at a shut still gripped)
std::atomic<bool> g_barrel_snap_cfg{true};
std::atomic<float> g_barrel_snap{0.0f};
std::atomic<bool> g_part_snap_cfg{true};      // [Reload] PartHandSnap
std::atomic<bool> g_held_click{true};         // [Reload] HeldClick (round 13 item 13): a held-back pull clicks
std::atomic<float> g_part_snap{0.0f};         // the off hand's weight on the gripped part
std::atomic<float> g_part_p[3] = {};          // that part's handle in the drawn gun's frame
std::atomic<float> g_part_turn{0.0f};         // (2026-10-28) that handle's turn about the gun's z (deg: the bolt's lift)
std::atomic<bool> g_part_bolt{false};         // (the review) the part is a bolt's handle: the hand's hold kept on it (the others: none)
// (2026-10-28, the user: "Bolt actions don't seem to have a snap on pose for grabbing the bolt") [Reload] CarriedBolt's
// bolt in the free gun hand: the handle as drawn and the carried gun's axes in the carrying hand's target frame (body.cpp
// takes them through that hand's target of the frame it draws), the handle's turn, the hand's weight on it (eased 0.1 s)
std::atomic<float> g_cpart_snap{0.0f}, g_cpart_turn{0.0f};
std::atomic<float> g_cpart_p[3] = {}, g_cpart_R[9] = {};
std::atomic<int> g_cpart_j{-1};  // John's carrying hand
// (the review) all of them written together (the frame end, one thread) under a sequence count, odd while writing: a
// reader sees one frame's weight with that frame's frame and handle, never the last carry's
std::atomic<uint32_t> g_cpart_seq{0};
void cpart_begin() {
    g_cpart_seq.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
}
void cpart_end() { g_cpart_seq.fetch_add(1, std::memory_order_release); }
float g_breech_hz[3] = {}, g_semi_hz[3] = {};  // the breech's and the semi-auto's handles as drawn (gun frame)
std::atomic<uint64_t> g_hand_closes{0};
// [Reload] LeverParts (off): a lever gun's lever drawn as the player works it (the Carbine, then run 6 item 9c: the
// Winchester, the Henry, the Evans, the Volcanic). A flick down opens it (and stays open), a flick up closes it; the
// hammer is cocked by the opening and falls with the shot. The drawn amounts, eased
std::atomic<bool> g_lever_parts{false};
// [Reload] LeverControl (2026-10-15, the user: "Need more options for lever action control"): how the lever is worked.
// 0 flick: the gun flicked down, then up (as before). 1 stick: a flick of the right stick down works it (with LeverParts:
// the lever drawn open, then shut kStickCloseMs later); the gun's own flicks do nothing. 2 lock: the gun's flicks, and
// while the other hand's trigger is held with the lever to work, the drawn gun keeps its orientation (body.cpp), so the
// wrist works the lever without swinging the gun down.
std::atomic<int> g_lever_mode{0};
std::atomic<bool> g_lever_req{false};   // filter_stick's flick for the lever (mode 1), consumed by the frame end
std::atomic<bool> g_lever_lock{false};  // mode 2: the drawn gun's orientation held now (body.cpp)
// 3 front (run 14, after optimumbox's LeverSteady, 0.8.10.4): the gun's flicks, and held two-handed with the lever to
// work, the gun carried by the front hand (body.cpp: lever_front), eased back onto the gun hand once worked
std::atomic<bool> g_lever_front{false};
// 4 hold (2026-10-26, the user: "holding right stick flicked down and moving controller down and up operates the
// lever. Moving it up and down should remain two separate movements"): with the right stick held down, a flick of the
// gun down opens the lever and it stays open; a flick up, the stick still held, shuts it. The gun's flicks without the
// stick held do nothing (the stick's cone is consumed, as for the revolvers)
std::atomic<bool> g_lever_hold{false};  // the right stick held down now (filter_stick)
// (2026-10-28, round 35, the user: "The flick movement should not influence the position of the rest of the gun") while
// the stick is held with the lever to work (a shot not cycled, the lever open, or shut under kHoldReleaseMs ago), the
// drawn gun is held (body.cpp: one-handed, the gun hand's controller pose, its orientation and its place, as the lock's;
// held two-handed, carried by the front hand, as front's), so only the lever moves with the gun's flicks. The gesture
// is still read from the controller's own turn
std::atomic<bool> g_lever_hold_lock{false};
double g_lever_shut_ms = -1e12;
constexpr double kHoldReleaseMs = 200.0;
// [Reload] VolcanicControl (2026-10-26): "lever" (as LeverControl) or "stick": the Volcanic's lever worked by a flick of
// the right stick down after each shot, whatever LeverControl says (LeverControl=stick's working, for that pistol)
std::atomic<bool> g_volc_stick{false};
int lever_mode_of(int w) { return w == 0 && g_volc_stick.load(std::memory_order_relaxed) ? 1 : g_lever_mode.load(std::memory_order_relaxed); }
// [Reload] RevolverControl (2026-10-26, the user: "flick right stick down after every shot to cock the hammer. Hold it
// flicked down and click stick in to open for reload"): "open" (a flick of the stick down opens a revolver, as before) or
// "cock". The single actions (the Cattleman, the Schofield, the LeMat; "Double action revolver does not require
// cocking") fire with the hammer back: after each shot it is down and the pull held back until a flick of the stick down
// cocks it (the cylinder turned a chamber, the game's REVOLVER_COCK_FIRE at the hand). As the Carbine's design: the
// game's own re-cock in its fire clip stopped (body.cpp: hammer_weapon), the hammer drawn from the flick. Every revolver
// then opens for loading by the stick held down and clicked, and shuts by the gun's flick as before.
std::atomic<int> g_rev_mode{0};
std::atomic<bool> g_cock_req{false};  // filter_stick's flick (cock), consumed by the frame end
std::atomic<bool> g_rev_hold{false};  // the stick held down now (cock): a click opens the revolver
bool g_click_was = false;             // filter_stick's thread
std::atomic<float> g_rev_hammer{1.0f};  // the single action's drawn hammer, 0 down - 1 cocked (eased)
std::atomic<int> g_rev_weapon{-1};      // that revolver in hand with RevolverControl=cock, else -1
std::atomic<uint64_t> g_cocked_n{0};
bool single_action(int w) { return w == 4 || w == 5 || w == 7; }
// (2026-11-01, GOAL-offhand-fixes F3; round 63 check 3: "the Volcanic does not cock") the Volcanic as the second gun with
// VolcanicControl=stick: its lever worked by its stick's flick after each shot, as a single action's cock (its hammer
// drawn down until then, the pull held); it never opens
bool sec_volcanic(int w) { return w == 0 && kind_on(kLever) && lever_mode_of(0) == 1; }
// [Reload] ThumbCock (2026-10-28, on; acts with RevolverControl=cock; round 35, the user: "the thumb cocking animation
// should play when I activate the flick"): the stick's cock drawn by John's thumb (body.cpp: the gun hand's thumb turned
// onto the hammer's spur). It reaches the spur over kThumbReachMs, rests on it kThumbSettleMs (the hammer drawn down until
// then: r232, with none the hammer's first step came in the very frame the thumb was full on it, 0.16 of its travel at
// the frame's 12 ms), rides the hammer back over kThumbRideMs and goes back over kThumbBackMs. The shot is ready at the flick as before: a pull before the
// hammer is drawn back still fires, and the thumb then leaves at once. The fan's cock (the other hand's) has none.
std::atomic<bool> g_thumb_cfg{true};
std::atomic<double> g_thumb_t0{-1e12}, g_thumb_ret{-1e12};  // the stick's cock; the thumb's return from the spur (log::now_ms)
std::atomic<int> g_thumb_weapon{-1};
constexpr double kThumbReachMs = 90.0, kThumbSettleMs = 20.0, kThumbRideMs = 90.0, kThumbBackMs = 100.0;
// (2026-11-01, GOAL-offhand-changes C3; the user: the second gun's hammer cocked by the thumb as the gun in hand's) the second
// gun's (or the copy's) stick cock: its thumb's times, as the gun in hand's (body.cpp draws its hand's thumb from the pose the
// gun in hand's thumb had on its spur)
std::atomic<double> g_sthumb_t0{-1e12}, g_sthumb_ret{-1e12};
std::atomic<int> g_sthumb_weapon{-1};
float smooth01(double x) {
    const float t = x <= 0.0 ? 0.0f : x >= 1.0 ? 1.0f : static_cast<float>(x);
    return t * t * (3.0f - 2.0f * t);
}
// [Reload] FanHammer (2026-10-26, the user: "The ability to fan the revolver hammer with off hand when manual cocking is
// enabled"; on, with RevolverControl=cock): the other hand's palm swept back across the hammer cocks it, as the stick's
// flick does; with the gun's trigger held, the shot goes at once (fanning: the trigger kept pulled, each sweep a shot).
// The zone: the hammer's bind origin on the drawn gun (the drawn gun's frame, else the gun hand's: the model's origin
// taken 8 cm ahead of the wrist), moved by [Reload] FanZoneOffset (right, up, forward), [Reload] FanZoneRadius around
// it; a sweep: the other hand's interaction spot inside it at [Reload] FanSpeed (m/s) or more, mostly back along the
// gun (its velocity's back part at least half its speed); again only after the hand has left the zone.
std::atomic<bool> g_fan{true};
float g_fan_off[3] = {0.0f, 0.0f, 0.0f}, g_fan_r = 0.08f, g_fan_speed = 1.0f;
float g_fan_zone[3] = {}, g_fan_d = 1e9f, g_fan_v = 0.0f, g_fan_back = 0.0f;
float g_fan_prev[3] = {};  // the hand against the zone at the last body points (gun-relative: walking, riding, turning cancel)
double g_fan_prev_ms = 0;  // those points' time (bp.ms)
double g_fan_last_ms = 0;  // the last sweep (a 200 ms pause before the next arms: the drawn hand settling back crossed it twice)
bool g_fan_armed = true, g_fan_frame = false;  // (the zone from the drawn gun's frame)
std::atomic<uint64_t> g_fans{0}, g_fan_shots{0};
// (2026-10-26, the user: fanning as real fanning, each sweep a shot) a fanned shot the game did not take (its own fire
// interval not over) waits: the held pull let go for a frame and pulled again every kFanRetryMs until the game fires
// (its count drops), the trigger is let go, or kFanWaitMs pass. Never with the gun empty.
bool g_fan_pending = false;
double g_fan_pend_ms = 0, g_fan_retry_ms = 0;
uint64_t g_fan_pend_shots = 0;  // aim's count of the player's shots at the sweep: one more, the shot was taken (the review:
                                // not the clip's drop alone, which an endless-ammo game never shows)
int g_fan_low = 0;  // frames the pull is still let go (two: the game's own read of the pad must see it; then its fresh press)
constexpr double kFanRetryMs = 80.0, kFanWaitMs = 1500.0;  // (the first retry 100 ms after the sweep: a shot the game took is seen first)
std::atomic<uint64_t> g_fan_retries{0}, g_fan_late{0};
// [Hands] GunCollision (2026-10-29, off; GOAL-weapons-handling item 4: "including the ability to cock hammer with another
// gun"): the other gun's body swept back across the single action's hammer cocks it, as the stick's flick does (the same
// gate: RevolverControl=cock, the hammer down after a shot, shut, not in Dead Eye; the cock sound at the hammer, its shot at
// once; John's thumb not drawn: the gun did it). The other gun (the second gun, the copy or the carried gun) as the
// collisions took it (body::collision_guns: as held, never the collision's move: the drawn gun rides over the hammer, the
// hand's sweep goes through it): its body's capsule along the barrel within kGunCockTouch of the hammer's spur (the hammer
// down, the gun's own frame: walking, riding and turning cancel), at [Reload] FanSpeed or more, mostly back along the gun
// (its back part at least half its speed); again only after it has left (twice the reach) and 200 ms
constexpr float kGunCockTouch = 0.015f;
float g_gc_prev[2][3] = {};  // the other gun's body's ends in the gun's frame at the last guns
double g_gc_prev_ms = 0.0, g_gc_last_ms = 0.0;
int g_gc_prev_kind = -1;
bool g_gc_armed = true;
float g_gc_d = 1e9f, g_gc_v = 0.0f, g_gc_back = 0.0f;
std::atomic<uint64_t> g_gc_sweeps{0}, g_gc_cocks{0}, g_gc_notback{0}, g_gc_already{0};
// (the review) a step of the other gun over kGunCockJump in a frame, or faster than kGunCockMaxV, is a jump of the guns'
// frames, not a hand: no sweep on it. g_gc_in: the other gun in the zone now (its visit's fastest, g_gc_vis_*); g_gc_nb_said:
// that visit's "not back" counted. A sweep counts (g_gc_sweeps) only when it passes: fast enough and back
constexpr float kGunCockJump = 0.25f, kGunCockMaxV = 8.0f;
bool g_gc_in = false, g_gc_nb_said = false;
float g_gc_vis_v = 0.0f, g_gc_vis_back = 0.0f;
std::atomic<uint64_t> g_gc_jumps{0};
// the distance of point p from segment a b, and the segment's parameter at the closest point
float point_seg(const float* p, const float* a, const float* b, float* t_out) {
    float ab[3], ap[3];
    for (int i = 0; i < 3; ++i) ab[i] = b[i] - a[i], ap[i] = p[i] - a[i];
    const float l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float t = l2 > 1e-10f ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / l2 : 0.0f;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    float d2 = 0.0f;
    for (int i = 0; i < 3; ++i) d2 += (ap[i] - ab[i] * t) * (ap[i] - ab[i] * t);
    if (t_out) *t_out = t;
    return std::sqrt(d2);
}
const float* hammer_origin(int w) {  // the single actions' hammer bind origins (research run 27, the model's frame: x right, y up, z back)
    static const float kCat[3] = {0.0f, 0.02744f, 0.02093f}, kSho[3] = {0.0f, 0.04084f, 0.03053f}, kLem[3] = {0.0f, 0.02664f, 0.01354f};
    return w == 4 ? kCat : w == 5 ? kSho : w == 7 ? kLem : nullptr;
}
double g_lever_stick_close_ms = 0;      // mode 1 with LeverParts: when the stick's opened lever shuts
constexpr double kStickCloseMs = 180.0;
bool g_lever_open = false, g_hammer_down = false;
std::atomic<float> g_lever_amt{0.0f}, g_hammer_amt{1.0f};
std::atomic<int> g_lever_weapon{-1};
// [Reload] PumpParts (off; run 6 item 9e): the Pump-action's fore-end drawn where the front hand has it (its travel back
// along the barrel, 1:1, to the rear stop: the game's own fire clip, 82.3 mm), John's front hand drawn on it (body.cpp);
// unlocked only by a shot (locked while chambered, as the 1897's action). The game's own pump after each shot is stopped
// and the gun held still through John's fire clip (body.cpp, its model's flags); the pump's sounds on the strokes
std::atomic<bool> g_pump_parts{false};
std::atomic<float> g_pump_amt{0.0f};
std::atomic<int> g_pump_weapon{-1};
constexpr float kPumpStroke = 0.0823f;  // the fore-end's travel (m): body.cpp's row
// [Reload] BoltParts (off; run 6 item 9f): the bolt actions' bolts (the Bolt Action, the Carcano) worked by the off hand
// and drawn so. Gripped at the bolt, the hand's rise turns the handle up (59.7 degrees, the Bolt Action's own clips),
// then its pull draws the bolt back (1:1 to its stop, 82.5 mm), forward again, and the handle turned down: chambered.
// Rounds go in only with the bolt back, and the shot waits for the handle down. The game's own bolt work after each shot
// is stopped and the gun held still through John's fire clip (body.cpp, its model's flags); the four strokes' sounds
std::atomic<bool> g_bolt_parts{false};
std::atomic<float> g_bolt_lift{0.0f}, g_bolt_slide{0.0f};  // the drawn handle (0 down - 1 up) and bolt (0 shut - 1 back)
std::atomic<int> g_bolt_weapon{-1};
constexpr float kBoltRise = 0.03f, kBoltStroke = 0.0825f;  // the hand's rise for the full turn; the bolt's travel (m)
bool g_bolt_up = false, g_bolt_cycled = false;  // the handle up; back and forward since the shot (the handle down chambers)
float g_bolt_h0 = 0.0f;                         // the hand's height over the gun hand at the grip, less the drawn turn's
// [Reload] BreechParts (run 6 item 9g): the single-shot rifles worked by hand and drawn so. The Springfield's trapdoor
// flipped up by the off hand at it (its trapdoor and the release on it, 90 degrees forward about its hinge: the game's
// own fire clip, cycle J), the Rolling Block's breech block rolled back by the off hand's pull at its spur (75 degrees),
// the Buffalo's falling block dropped by a flick of the gun down and raised by a flick up (37 degrees: its prop clip's;
// the game never draws it so). Open, the spent case is out and one round goes in by hand; shut with it, the rifle is
// ready, its hammer cocked (by the shut; the Rolling Block's by the opening). Their own reload after each shot (the fire
// clips) is stopped and the rifle held still through John's fire clip (body.cpp); their sounds on the steps
struct Breech {
    int weapon;
    int gesture;             // 0 the gun's flicks (down opens, up shuts), 1 the off hand's pull back, 2 its rise
    float travel;            // the hand's travel for the full opening (m)
    float shut[3], open[3];  // the part's handle in the drawn gun's own frame, shut and open (m; x right, y up, z back)
    bool cock_on_open;       // the hammer cocked by the opening (else by the shut)
};
const Breech kBreeches[] = {
    // the Springfield: the trapdoor's thumb piece behind its hinge (y 66.7, z -117.8 mm), up and over it open
    {12, 2, 0.05f, {0.0f, 0.070f, -0.060f}, {0.0f, 0.1245f, -0.1211f}, false},
    // the Rolling Block: the breech block's spur above its pivot (y 52.7, z 24.5 mm), back and down as it rolls
    {19, 1, 0.025f, {0.0f, 0.075f, 0.020f}, {0.0f, 0.0627f, 0.0450f}, true},
    // the Buffalo: the falling block by the gun's flicks (the lever under its grip)
    {14, 0, 0.0f, {}, {}, false},
};
const Breech* breech_of(int w) {
    for (const Breech& b : kBreeches)
        if (b.weapon == w) return &b;
    return nullptr;
}
std::atomic<float> g_breech_amt{0.0f}, g_breech_hammer{1.0f};  // the drawn opening (0 shut - 1 open), the hammer (1 cocked)
// (2026-11-01, the off-hand build step 8, R14; research/run33/offhand_plan.md step 8) the Blunderbuss (34), a muzzle-loader
// with BreechParts: after a shot a round goes in at the loading point, then the off hand rams it: gripped at the muzzle,
// pushed back along the barrel 6 cm and drawn forward again, twice (each stroke the game's own plunger sounds, from its
// fire clip's: research/run6/clip-sounds.md rifle_bld); then ready. Its fire clip's own reload (the 4 s ramrod) stopped
// (body.cpp's row, as the breeches'). [I: the muzzle's place on the model, 0.55 m ahead of the gun's origin and 45 mm up,
// from its rod bone's bind (0 0.024 -0.460)]
bool g_muz_round = true;  // a round down the barrel since the shot
int g_muz_strokes = 0;
bool g_muz_grip = false, g_muz_grip_was = false, g_muz_down = false;
float g_muz_zone[3] = {}, g_muz_d = 1e9f, g_muz_s0 = 0.0f;  // (the stroke's start: the off hand from the gun hand, along the barrel)
constexpr float kMuzzleAt[3] = {0.0f, 0.045f, -0.55f};  // the gun's frame (x right, y up, z back)
constexpr float kRamStroke = 0.06f;
std::atomic<float> g_muz_rod{0.0f}, g_smz_rod{0.0f};  // (the rod drawn: the in-hand and the second gun's ram, 0 home - 1 the stroke)
// (2026-11-01, GOAL-offhand-changes C15; round 64 check 5, the user: "Could the ramrod appear in chest holster after the bullet
// is loaded and need to be slid into and out of the barrel to finish loading the gun?") [Reload] RamrodAtChest (on): the
// Blunderbuss in hand with its round down the barrel: the off hand's press at the chest takes the ramrod (holster.cpp:
// take_ramrod), held by its grip; brought to the muzzle it is drawn out of the barrel at its full stroke; pushed in and
// pulled out again: rammed home (one stroke in place of the bare hand's two). Its grip let go before that: back at the chest
std::atomic<bool> g_rod_chest{true};
std::atomic<int> g_rod_hand{-1};            // the controller holding the ramrod (-1: at the chest)
std::atomic<bool> g_rod_ready{false};       // the frame end's: the ramrod to take now (the muzzle-loader's round in, not rammed)
// (C15b) the ramrod drawn in the hand holding it: the rod's head (bone 4's bind, under the muzzle) moved to that hand's spot,
// in the gun's model (the drawn gun's frame); body.cpp turns it half round about its head (out of the muzzle, forward)
constexpr float kRodHead[3] = {0.0f, 0.024f, -0.460f};
std::atomic<bool> g_rod_shown{false};
std::atomic<float> g_rod_s[3] = {};
float g_rod_q[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // (C15c) under g_rod_mx
std::mutex g_rod_mx;
RodPlace g_rod_place[2] = {{{0.0f, -0.04f, 0.0f}, 0.0f, 0.0f}, {{0.0f, -0.04f, 0.0f}, 0.0f, 0.0f}};  // [Reload] RamrodOffset/Turn/Tilt (C15d), under g_rod_mx
bool g_rod_place_r = false;  // the right hand's own set (else the left's)
// (C15e) [Reload] RamrodFrom (1 the gun), RamrodGrabOffset; the grab point on the gun: the rod's middle under the barrel,
// 0.15 m back from its head (the gun's frame, bind (0 0.024 -0.46): x right, y up, z back) [I: the rod's length unmeasured]
std::atomic<bool> g_rod_from_gun{true};
float g_rod_grab_off[3] = {0.0f, 0.0f, 0.0f};  // under g_rod_mx
constexpr float kRodGrab[3] = {0.0f, 0.024f, -0.31f};
float g_rod_zone[3] = {};      // (the frame end's, under g_mutex for hint) the grab point now (world)
bool g_rod_zone_on = false;    // the ramrod to take there now
float g_rod_zone_d = 1e9f;     // the off hand's grab spot from it
constexpr int kRamStrokes = 2;
std::atomic<int> g_breech_weapon{-1};
std::atomic<bool> g_breech_round{true};  // a live round chambered (holster.cpp's round in sets it)
std::atomic<bool> g_breech_want[2] = {false, false};
bool g_breech_open = false, g_breech_grip = false, g_breech_grip_was = false;
double g_breech_ms = 0, g_breech_shut_ms = 0;
// the lever to work now (the frame end's thread): a lever gun's shot not cycled or its lever open (release: or shut under
// kHoldReleaseMs ago, LeverControl=hold's); (C4b) the Buffalo's (BreechParts' gesture 0) the same
bool lever_to_work(double now, bool release) {
    const Kind k = kind_of(g_weapon);
    if (k == kLever) return g_needs || g_lever_open || (release && now - g_lever_shut_ms < kHoldReleaseMs);
    if (k == kBreech && g_weapon == 14) return g_needs || g_breech_open || (release && now - g_breech_shut_ms < kHoldReleaseMs);
    return false;
}
int g_breech_flicks = 0;  // the Buffalo's down flick: consecutive two-frame steps past the speed
float g_breech_s0 = 0.0f, g_breech_d = 1e9f, g_breech_zone[3] = {};
// [Reload] SemiAutoParts (run 6 item 9h): the Semi-Auto Shotgun's bolt racked by the off hand after loading it from
// empty: gripped at its handle (right of the receiver) and pulled back (drawn 1:1 to its stop), then let go (it springs
// forward) or pushed forward: a shell chambered. Between shots its cycling stays the game's (it is semi-automatic). Its
// prop's bolt never moves in any of the game's clips, so its travel is the mod's: 65 mm, the Auto-5's it copies
constexpr float kSemiTravel = 0.065f;
const float kSemiHandle[3] = {0.030f, 0.074f, -0.035f};  // the handle in the gun's frame: right of the bolt's bind origin
std::atomic<float> g_semi_amt{0.0f};
std::atomic<int> g_semi_weapon{-1};
std::atomic<bool> g_semi_want[2] = {false, false};
bool g_semi_grip = false, g_semi_grip_was = false, g_semi_back = false;
float g_semi_s0 = 0.0f, g_semi_d = 1e9f, g_semi_zone[3] = {};
// [Reload] PistolMagazines (2026-10-28, off; the user: "Magazines for pistols (clip for mauser)", the release by "a flick
// of the right stick down"): the Semi-Automatic Pistol and the High Power drop their magazine by a flick of the right
// stick down (a round stays in the chamber if the gun was not empty; the magazine's rounds go back to the spare: none
// lost), and take a new one in the other hand (at the chest, or by [Reload] RoundSource) pushed up into the grip's well:
// loaded to the gun's size from the spare (reload::fill_clip). No single rounds go in. The Mauser's bolt is pulled back
// by its ears with the other hand and held open (on an empty magazine, or pulled back with room: optimumbox's rule,
// MIT, v0.8.11.10); a stripper clip of ten pushed down into the top loads what there is room for; the bolt let forward
// (pulled and let go, or the clip pulled out by the flick) chambers a round and pushes the clip out.
// [Reload] PistolSlides (2026-10-28, off; the amendment, the user: "manually operating the pistol slides"): the slide
// (the Semi-Auto's toggle, the Mauser's bolt) gripped by the other hand, pulled back and let go, each step its game sound
// and drawn; racking chambers a round from the magazine, racking with one chambered ejects it (lost: the user's choice);
// the last shot locks it back; a new magazine then needs a rack; with nothing chambered the trigger fires nothing. The
// game's own fire clip (its slide's cycle and its sound) is stopped, the shot's slide drawn by the mod.
struct PistolDef {
    int weapon;
    float handle[3];    // where the other hand grips: the slide's rear, the toggle's knee, the bolt's ears (the gun's frame, m)
    float travel;       // the slide's (bolt's, the knee's) travel back (m)
    float well[3];      // the magazine's well under the grip; the Mauser's clip guide over its magazine (the gun's frame)
    const char *back, *fwd, *shot;  // the game's sounds: the slide back, forward, the shot's (the stopped fire clip's)
};
// the frames from the models' own bones and meshes (research\run28\magazines.md: pistol_highpower01x, pistol_semiauto01x,
// pistol_mauser01x; the gun's frame x right, y up, z back). The travels are the game's fire clips' as optimumbox measured
// them (MIT, v0.8.11.10) [I]: the High Power's slide 32.8 mm, the Mauser's bolt 20.5 mm, the Semi-Auto's knee 32.3 mm
// along its chord
// (kToggleDeg, kToggleHinge, kToggleKnee, kToggleDy, kToggleDz, kToggleFrontDeg, kSlideTravelHP, kBoltTravelMauser:
// actions.h, body.cpp's kGunModels rows share them)
const float kTogglePull[3] = {0.0f, 0.7510f, 0.6603f};  // the knee's pull: its chord, up and back
const PistolDef kPistols[] = {
    {1, {0.0f, 0.0740f, 0.0835f}, 0.0323f, {0.0f, -0.049f, 0.056f}, "PISTOL_ACTION1_HIPWR_MASTER", "PISTOL_ACTION2_HIPWR_MASTER", "PISTOL_RELEASE_SHORT_MASTER"},
    {2, {0.0f, 0.0720f, 0.0500f}, kSlideTravelHP, {0.0f, -0.049f, 0.046f}, "PISTOL_ACTION1_HIPWR_MASTER", "PISTOL_ACTION2_HIPWR_MASTER", "PISTOL_RELEASE_MASTER"},
    {3, {-0.0016f, 0.0740f, 0.0720f}, kBoltTravelMauser, {-0.0016f, 0.076f, -0.024f}, "PISTOL_ACTION1_HIPWR_MASTER", "PISTOL_MAUSER_PST_MASTER", "PISTOL_RELEASE_SHORT_MASTER"},
};
const PistolDef* pistol_of(int w) {
    for (const PistolDef& p : kPistols)
        if (p.weapon == w) return &p;
    return nullptr;
}
// (2026-11-01, GOAL-offhand-changes C38; the user: each pistol's magazine set separately, and for each hand) the well in the
// gun's frame (x right, y up, z back), moved by the gun's own [Weapon.<gun>] MagazineWellOffset (right, up, forward) for John's
// hand holding it (the left's ...Left, else the right's): o, holster::gun_adjust's kAdjMag (read outside g_mutex: holster's
// zone lock is taken before this one)
void well_at(const PistolDef* pd, const float o[3], float out[3]) {
    out[0] = pd->well[0] + o[0], out[1] = pd->well[1] + o[1], out[2] = pd->well[2] - o[2];
}
float g_mag_off[2][3] = {};  // (the frame end's) the gun in hand's MagazineOffset for John's left [0] and right [1], read before g_mutex
// (2026-11-01, GOAL-offhand-changes C36; the user: the rings of the bolt actions' bolts and the guns' other interaction spots
// adjustable) the gun in hand's [Weapon.<gun>] PartOffset (right, up, forward) for John's left [0] and right [1], read as
// g_mag_off: the action's zone moved by it (the bolt's from the wrist in the hand's axes; the others in the gun's frame,
// x right, y up, z back: part_hz); the drawn parts are not moved
float g_part_off[2][3] = {};
void part_hz(const float* hz, int gj, float* out) {
    const float* o = g_part_off[gj < 0 || gj > 1 ? 1 : gj];
    out[0] = hz[0] + o[0], out[1] = hz[1] + o[1], out[2] = hz[2] - o[2];
}
constexpr float kPistolGripR = 0.07f;  // the slide's zone (m)
constexpr int kClipRounds = 10;        // the C96's stripper clip
// the Semi-Auto's toggle at a (0 shut - 1 at the fire clip's top): the knee (the gun's frame) and the front link's turn
// (its curve against the rear link's, optimumbox's measure of the game's fire clip, MIT [I])
void toggle_pose(float a, float knee[3], float* front_deg) {
    const float th = a * kToggleDeg * 0.0174532925f, c = std::cos(th), s = std::sin(th);
    const float ry = kToggleKnee[1] - kToggleHinge[1], rz = kToggleKnee[2] - kToggleHinge[2];
    knee[0] = 0.0f;
    knee[1] = kToggleHinge[1] + c * ry - s * rz;
    knee[2] = kToggleHinge[2] + s * ry + c * rz;
    static const float kAt[4][2] = {{0.0f, 0.0f}, {24.6f, -15.36f}, {59.3f, -32.24f}, {66.6f, -30.47f}};
    const float d = a * kToggleDeg;
    *front_deg = kAt[3][1];
    for (int i = 0; i < 3; ++i)
        if (d <= kAt[i + 1][0]) {
            const float t = (d - kAt[i][0]) / (kAt[i + 1][0] - kAt[i][0]);
            *front_deg = kAt[i][1] + t * (kAt[i + 1][1] - kAt[i][1]);
            break;
        }
}
std::atomic<bool> g_mag_req{false};  // filter_stick's flick down with a magazine pistol in hand, consumed by the frame end
bool g_mag_out[4] = {};              // each pistol's magazine out (kept over a put-away, for the session)
std::atomic<bool> g_mag_in{true};    // the gun in hand's magazine in it
bool g_chambered = true;             // (PistolSlides) a round in the chamber
bool g_locked = false;               // the slide (the Mauser's bolt) held back by its stop: empty, or the Mauser open for a clip
bool g_pgrip = false, g_pgrip_was = false, g_pback = false, g_pgrip_locked = false;
float g_ps0 = 0.0f, g_pd = 1e9f, g_pdrawn = 0.0f, g_pzone[3] = {}, g_phz[3] = {}, g_pwell[3] = {};
bool g_pwell_ok = false;
int g_pmag_at_back = 0;              // the magazine's rounds as the slide reached the back (after an eject)
bool g_clip_seated = false;          // the Mauser's stripper clip in its guide
int g_clip_left = 0;                 // its rounds that did not go in
double g_pblow_ms = -1e12;           // the last shot: the slide's own blowback drawn after it (80 ms)
// the game's count as the mod's own natives will leave it (the release's ACTOR_SET_WEAPON_AMMO, an ejected round, a
// magazine's or a clip's ACTOR_ADD_WEAPON_AMMO), until the actor state shows it: that drop is not a shot, and the
// magazine's rounds are counted from it meanwhile
int g_expect_clip = 0;
double g_expect_until = 0.0;
int g_lose_pending = 0;              // an ejected round to take from the count (once the spare natives have run)
double g_lose_ms = 0.0;
std::atomic<float> g_pslide{0.0f};   // the drawn slide (0 shut - 1 back), for body.cpp
std::atomic<int> g_pslide_weapon{-1};
std::atomic<bool> g_pslide_want[2] = {false, false};
std::atomic<uint64_t> g_mag_drops{0}, g_mag_inserts{0}, g_clip_inserts{0}, g_racks{0}, g_ejects{0}, g_lost{0}, g_chambers{0}, g_locks{0},
    g_clip_outs{0};
// The game's own action sounds, played at the player's own steps (the gun's clips that play them in the game do not
// run: the hand reload holds the game's reloads back, and the Carbine's fire clip is stopped, body.cpp):
// PLAY_SOUND_FROM_POSITION(name, xy, z) at the hand. The names are the gun's prop clip set's (animationres.rpf
// <set>.was, research\run3\manualactions.md 1): by gun, each behind the switch that makes that gun's action the
// player's (the design for every gun: the parts drawn from the gesture, the game's own cycling gone, its sounds on
// the player's steps).
constexpr uint32_t kSoundAt = 0x05BC72D7;
enum SoundGate { kGateLeverParts, kGateOpenCylinder, kGateManualBreak, kGatePumpParts, kGateBoltParts, kGateBreechParts, kGateSemiParts };
struct GunSounds {
    int weapon;
    SoundGate gate;
    const char* open;    // the action opened (the lever down, the cylinder out, the pump back)
    const char* close;   // closed (the lever up, the cylinder back in, the pump forward)
    const char* insert;  // a round put in by hand
    const char* close2;  // with the closing (the Double-barrel's hammers cocked; a bolt's handle turned down)
    const char* open2;   // the opening's second stroke (a bolt drawn back after its handle's turn up)
};
const GunSounds kGunSounds[] = {
    // the Carbine (Rifle_lvr): the lever's strokes, a round into its side gate (run 6: its rld_mid's RIFLE_INSERT_BULLET);
    // its BULLET_SHELLS_RIFLE is not a sound event (another class: the case thrown, research\run6\clip-sounds.md)
    {8, kGateLeverParts, "REPEATER_LEVER_WIN_OUT_MASTER", "REPEATER_LEVER_WIN_IN_MASTER", "RIFLE_INSERT_BULLET_MASTER", nullptr},
    // run 6 item 9c: the Winchester, the Henry and the Evans share the Carbine's clip set (the same four names); the
    // Volcanic (pistol_vol): its lever down and up, a round into its tube (research\run6\clip-sounds.md 3)
    {9, kGateLeverParts, "REPEATER_LEVER_WIN_OUT_MASTER", "REPEATER_LEVER_WIN_IN_MASTER", "RIFLE_INSERT_BULLET_MASTER", nullptr},
    {10, kGateLeverParts, "REPEATER_LEVER_WIN_OUT_MASTER", "REPEATER_LEVER_WIN_IN_MASTER", "RIFLE_INSERT_BULLET_MASTER", nullptr},
    {11, kGateLeverParts, "REPEATER_LEVER_WIN_OUT_MASTER", "REPEATER_LEVER_WIN_IN_MASTER", "RIFLE_INSERT_BULLET_MASTER", nullptr},
    {0, kGateLeverParts, "PISTOL_ACTION1_HIPWR_MASTER", "PISTOL_ACTION2_HIPWR_MASTER", "REVOLVER_INSERT_BULLET_MASTER", nullptr},
    // the Double-action (pistol_dbl): the cylinder out, back in, a round into it
    {6, kGateOpenCylinder, "REVOLVER_CHAMBER_OUT_MASTER", "REVOLVER_CHAMBER_IN_MASTER", "REVOLVER_INSERT_BULLET_MASTER"},
    // the Cattleman (pistol_cat): the same three, for its loading gate
    {4, kGateOpenCylinder, "REVOLVER_CHAMBER_OUT_MASTER", "REVOLVER_CHAMBER_IN_MASTER", "REVOLVER_INSERT_BULLET_MASTER"},
    // run 6 item 9b: the Schofield (pistol_sho): REVOLVER_TOPBREAK as it breaks open and as it shuts (both its reload's
    // rld_pre and rld_pst), a round; the LeMat (pistol_lem): its chamber out and in, a round (its prop set is gate-style)
    {5, kGateOpenCylinder, "REVOLVER_TOPBREAK_MASTER", "REVOLVER_TOPBREAK_MASTER", "REVOLVER_INSERT_BULLET_MASTER"},
    {7, kGateOpenCylinder, "REVOLVER_CHAMBER_OUT_MASTER", "REVOLVER_CHAMBER_IN_MASTER", "REVOLVER_INSERT_BULLET_MASTER"},
    // the Double-barrel (rifle_brk): the break open and shut (its reload's first and last), a shell in, the hammers cocked
    {16, kGateManualBreak, "SHOTGUN_BREAK_MASTER", "SHOTGUN_BREAK_MASTER", "SHOTGUN_SHELL1_INSERT_MASTER", "SHOTGUN_COCK_DBLE_MASTER"},
    // run 6 item 9d: the Sawed-off (its own prop set has the same four)
    {15, kGateManualBreak, "SHOTGUN_BREAK_MASTER", "SHOTGUN_BREAK_MASTER", "SHOTGUN_SHELL1_INSERT_MASTER", "SHOTGUN_COCK_DBLE_MASTER"},
    // run 6 item 9e: the Pump-action (rifle_pmp): the pump back and forward (on its fire clip's strokes), a shell in
    {17, kGatePumpParts, "SHOTGUN_PUMP1_MOSS_MASTER", "SHOTGUN_PUMP2_MOSS_MASTER", "SHOTGUN_SHELL1_INSERT_MASTER", nullptr},
    // run 6 item 9f: the bolt actions (rifle_blt, rifle_crc): the handle up, the bolt back (open2), forward (close), the
    // handle down (close2); a round each (their sets have only the stripper clip's RIFLE_STRIP_INSERT: one round at a
    // time is the single shots' RIFLE_INSERT_BULLET_LOUD, which played in all of cycle M1's trials, where
    // RIFLE_INSERT_BULLET played in one of three)
    {13, kGateBoltParts, "RIFLE_BOLT_OPEN1_MASTER", "RIFLE_BOLT_CLOSE1_MASTER", "RIFLE_INSERT_BULLET_LOUD_MASTER", "RIFLE_BOLT_CLOSE2_MASTER", "RIFLE_BOLT_OPEN2_MASTER"},
    {20, kGateBoltParts, "RIFLE_BOLT_OPEN1_MASTER", "RIFLE_BOLT_CLOSE1_MASTER", "RIFLE_INSERT_BULLET_LOUD_MASTER", "RIFLE_BOLT_CLOSE2_MASTER", "RIFLE_BOLT_OPEN2_MASTER"},
    // (step 8, R14) the Blunderbuss: the ram's push (open), its draw back (close), the round, the last stroke's (close2)
    {34, kGateBreechParts, "RELOAD_RIFLE_BLUNDERBUSS_PLUNGER_DOWN", "RELOAD_RIFLE_BLUNDERBUSS_PLUNGER_UP", "RIFLE_INSERT_BULLET_LOUD", "RIFLE_BLUNDERBUSS_PLUNGER",
     nullptr},
    // (2026-11-01, step 8, R14; the review: its fire clip stopped, its bolt otherwise silent) the Explosive Rifle: the Bolt
    // Action's [I: research/run33/verified/game.md names its own ANTITANK_BOLT_OPEN1/2, not yet played by this table]
    {31, kGateBoltParts, "RIFLE_BOLT_OPEN1_MASTER", "RIFLE_BOLT_CLOSE1_MASTER", "RIFLE_INSERT_BULLET_LOUD_MASTER", "RIFLE_BOLT_CLOSE2_MASTER", "RIFLE_BOLT_OPEN2_MASTER"},
    // run 6 item 9g: the Springfield (rifle_spr): the trapdoor up and down (its fire clip's TRAP_OPEN1 and 2), a round,
    // the hammer cocked; the Rolling Block (rifle_rol) and the Buffalo (rifle_buf): the breech open, shut with the
    // hammer's click (their fire clips' RIFLE_COCK_FIRE comes 0.17 s after the shut), a round
    {12, kGateBreechParts, "RIFLE_TRAP_OPEN1_MASTER", "RIFLE_TRAP_OPEN2_MASTER", "RIFLE_INSERT_BULLET_LOUD_MASTER", "RIFLE_COCK_MASTER"},
    {19, kGateBreechParts, "ARMOR_HORSE_HVY_MASTER", "RIFLE_COCK_FIRE_MASTER", "RIFLE_INSERT_BULLET_LOUD_MASTER", nullptr},
    {14, kGateBreechParts, "ARMOR_HORSE_HVY_MASTER", "RIFLE_COCK_FIRE_MASTER", "RIFLE_INSERT_BULLET_LOUD_MASTER", nullptr},
    // run 6 item 9h: the Semi-Auto Shotgun (rifle_sem): racked back, forward (with the bolt's close), a shell into its tube
    {18, kGateSemiParts, "SHOTGUN_PUMP1_REM_MASTER", "SHOTGUN_PUMP2_REM_MASTER", "SHOTGUN_SHELL_DBLE_INSERT_MASTER", "RIFLE_BOLT_CLOSE2_MASTER"},
};
std::mutex g_sound_mutex;       // the frame end's actions and the holsters' loads (one thread today; cheap)
uint64_t g_sound_req[16] = {};  // their results, collected (the result map clears everyone's at 4096)
int g_nsound = 0;
std::atomic<uint64_t> g_sounds{0};
void play_at(const char* name, const float* at) {
    if (!name || !at) return;
    uint32_t x, y, z;
    std::memcpy(&x, &at[0], 4);
    std::memcpy(&y, &at[1], 4);
    std::memcpy(&z, &at[2], 4);
    const uint64_t args[3] = {reinterpret_cast<uint64_t>(name), x | (static_cast<uint64_t>(y) << 32), z};
    std::lock_guard lock(g_sound_mutex);
    RdrvrNativeResult r;
    int k = 0;
    for (int i = 0; i < g_nsound; ++i)
        if (!api::wait_native(g_sound_req[i], &r, 0)) g_sound_req[k++] = g_sound_req[i];
    g_nsound = k;
    if (g_nsound >= 16) return;  // no room to collect its result (an uncollected one stays in the map): skipped
    const uint64_t id = api::queue_native(kSoundAt, args, 3, 0, nullptr);
    if (id) g_sound_req[g_nsound++] = id;
    g_sounds.fetch_add(1, std::memory_order_relaxed);
}
void collect_sounds() {
    std::lock_guard lock(g_sound_mutex);
    RdrvrNativeResult r;
    int k = 0;
    for (int i = 0; i < g_nsound; ++i)
        if (!api::wait_native(g_sound_req[i], &r, 0)) g_sound_req[k++] = g_sound_req[i];
    g_nsound = k;
}
const GunSounds* gun_sounds(int weapon) {  // the gun's sounds while its switch is on, else none
    for (const GunSounds& g : kGunSounds) {
        if (g.weapon != weapon) continue;
        const bool on = g.gate == kGateLeverParts ? g_lever_parts.load(std::memory_order_relaxed)
                        : g.gate == kGateManualBreak  ? g_break.load(std::memory_order_relaxed)
                        : g.gate == kGatePumpParts    ? g_pump_parts.load(std::memory_order_relaxed)
                        : g.gate == kGateBoltParts    ? g_bolt_parts.load(std::memory_order_relaxed)
                        : g.gate == kGateBreechParts  ? g_breech_parts.load(std::memory_order_relaxed)
                        : g.gate == kGateSemiParts    ? g_semi_parts.load(std::memory_order_relaxed)
                                                      : g_open_cyl.load(std::memory_order_relaxed);
        return on ? &g : nullptr;
    }
    return nullptr;
}
std::atomic<double> g_hand_round_ms{-1e12};  // the last round put in by hand (round_in)
bool reloading_by_hand_ms(double now) { return now - g_hand_round_ms.load(std::memory_order_relaxed) < 1500.0; }
bool g_stick_armed = true;
// the bolt: gripped in its zone, travel along the gun's back axis from the grip point
std::atomic<bool> g_bolt_want[2] = {false, false};
bool g_bolt_grip = false, g_bolt_back = false, g_grip_was = false, g_ooo = false;  // g_ooo: this stroke's out-of-order counted
int g_bolt_ctrl = -1;
float g_bolt_s0 = 0.0f, g_bolt_s = 0.0f, g_bolt_zone[3] = {}, g_bolt_d = 1e9f;
// (2026-11-01, step 7, R9) each bolt gun's knob in its own frame (bind space, m): its bolt's outermost vertex along +x
// ("holster glow" far: r288, r88 K1) in by the knob's radius (6 mm along x)
constexpr float kKnob13a[3] = {0.022f, 0.047f, 0.002f};   // the Bolt Action's (far (0.0283 0.0469 0.0024))
constexpr float kKnob20[3] = {0.0345f, 0.1006f, -0.0249f};  // the Carcano's (far (0.0405 0.1006 -0.0249))
float g_bolt_z0_gun[3] = {};  // (r88 K2) the in-hand zone shut, in the drawn gun's frame (the status), under g_mutex
bool g_bolt_z0_gun_ok = false;
// [Reload] CarriedBolt (2026-10-28, off; GOAL-weapons-handling.md item 1, the user: "When holding bolt action with
// foregrip only, should be able to work bolt with gun hand"): a bolt gun carried by its foregrip ([Hands]
// CarryByForegrip) has its bolt worked by the free gun hand by the same strokes as the other hand's in hand (BoltParts:
// up, back, forward, down), at the bolt's place on the carried gun (learned in the drawn gun's own frame while in hand),
// the strokes measured against the carried gun (its motion with the front hand does not count). The bolt's state goes
// with the gun: a shot not cycled may be handed over (refused before, and still with the option off), is kept through
// the carry, and the take-back finds the bolt as the gun hand left it. A grip nearer the carried gun's grip (the
// take-back's point, holster.cpp) than its bolt stays the take-back. The carried gun never fires (dual.cpp: a carry is
// never fired). research\run28\bolt.md
struct BoltState {
    bool needs = false, grip = false, back = false, up = false, cycled = false, ooo = false, grip_was = false;
    int ctrl = -1;
    float s0 = 0.0f, s = 0.0f, h0 = 0.0f, lift = 0.0f, slide = 0.0f;
};
std::atomic<bool> g_carried_bolt{false};
// (2026-10-30, round 55, the user: "Bolt needs to be workable with right trigger instead of grip, currently too easy to grab
// handle instead") [Reload] CarriedBoltTrigger (on): the carried bolt worked with the free gun hand's trigger, its grip then
// always the take-back (never the bolt's); while that hand is at the bolt or holds it, the game gets no RT from it (a
// trigger pulled elsewhere and brought to the bolt held: the game's no more until let go, nor the bolt's: pull it again)
std::atomic<bool> g_cb_trigger{true};
// (2026-11-01, GOAL-offhand-changes C5; the user: a bolt worked by the off hand takes the grip, not the trigger) [Reload]
// OffHandBoltTrigger (on): the gun in hand's bolt worked by the off hand's trigger, as the carried bolt's; while that hand is
// at the bolt or holds it, the game gets no LT from that trigger (g_cb_trig_latch, as the carried bolt's RT)
std::atomic<bool> g_ob_trigger{true};
// (the review) the trigger the carried bolt took: latched from its press there until that trigger is let go below 0.15,
// whatever comes between (a take-back with the trigger still pulled: the gun in hand would fire at once), so the game never
// sees that press; set too while the hand is at the bolt. Each controller's (controls: RT held back)
std::atomic<bool> g_cb_trig_latch[2] = {false, false};
BoltState g_bsave;  // the bolt gun's state as it left the hand (reset_gun), its eWeapon and when
int g_bsave_weapon = -1;
double g_bsave_ms = -1e12;
BoltState g_cbs;  // the carried bolt's (the frame end's, under g_mutex)
bool g_cb_active = false;
int g_cb_weapon = -1;
double g_cb_ret_ms = -1e12;  // the carry's take-back last seen under way
float g_cb_zone[3] = {}, g_cb_d = 1e9f, g_cb_take = 1e9f, g_cb_fwd[3] = {}, g_cb_up[3] = {};
bool g_cb_ran = false;  // the strokes measured this frame (the carried gun's frame and the gun hand known)
// (cycle r198: the handle's down stroke read 0.75 cm short, lift 0.25: the game update's placement of the carried gun,
// carry_frame, bobbed about 1 cm against the carrying hand between the strokes, and the strokes were measured from it)
// The carried gun's frame in the carrying hand's target frame (its controller with the calibration, to which John's
// carrying hand and the gun on it are drawn rigidly), averaged over its first kCbRelN placements once its T is made, then
// kept for the carry: the strokes measured from the carrying hand's target, as the in-hand bolt's from the gun hand's
constexpr int kCbRelN = 6;  // (2026-10-31, the off-hand build step 3, R10: 20 before; made after the carry's ease, not T's settling)
bool g_cb_rel_ok = false;
int g_cb_rel_n = 0;
float g_cb_relR_sum[9] = {}, g_cb_relo_sum[3] = {}, g_cb_relR[9] = {}, g_cb_relo[3] = {};
uint64_t g_cb_rel_serial = ~0ull;  // the carry it was made for (dual::carry_serial)
std::atomic<int> g_cb_pub{-1};  // the carried bolt gun whose bolt is drawn so (BoltParts; body.cpp's carried draw), else -1
std::atomic<float> g_cb_lift{0.0f}, g_cb_slide{0.0f};
std::atomic<uint64_t> g_cb_handovers{0}, g_cb_restores{0};
// the bolt's shut zone in the drawn gun's own frame (x right, y up, z back), learned in hand: [0] the Bolt Action, [1] the Carcano
float g_bolt_zone_gun[3][3] = {};
bool g_bolt_zone_gun_ok[3] = {false, false, false};
int bolt_slot(int w) { return w == 13 ? 0 : w == 20 ? 1 : w == 31 ? 2 : -1; }
BoltState bolt_get() {
    BoltState b;
    b.needs = g_needs;
    b.grip = g_bolt_grip;
    b.back = g_bolt_back;
    b.up = g_bolt_up;
    b.cycled = g_bolt_cycled;
    b.ooo = g_ooo;
    b.grip_was = g_grip_was;
    b.ctrl = g_bolt_ctrl;
    b.s0 = g_bolt_s0;
    b.s = g_bolt_s;
    b.h0 = g_bolt_h0;
    b.lift = g_bolt_lift.load(std::memory_order_relaxed);
    b.slide = g_bolt_slide.load(std::memory_order_relaxed);
    return b;
}
void bolt_put(const BoltState& b) {
    g_needs = b.needs;
    g_bolt_grip = b.grip;
    g_bolt_back = b.back;
    g_bolt_up = b.up;
    g_bolt_cycled = b.cycled;
    g_ooo = b.ooo;
    g_grip_was = b.grip_was;
    g_bolt_ctrl = b.ctrl;
    g_bolt_s0 = b.s0;
    g_bolt_s = b.s;
    g_bolt_h0 = b.h0;
    g_bolt_lift.store(b.lift, std::memory_order_relaxed);
    g_bolt_slide.store(b.slide, std::memory_order_relaxed);
}
// the lever: a pitch flick, down then up
double g_lever_down_ms = 0;
// the pump: the front hand's travel along the barrel while two-handed
bool g_pump_have = false, g_pump_back = false;
float g_pump_s0 = 0.0f, g_pump_s = 0.0f;  // the hold; the travel back from it (the state line)
// [Reload] CarriedPump (2026-10-29, off; the user: "Pump action shotgun should be able to be pumped while only held by
// off hand"): the Pump-action carried by its foregrip ([Hands] CarryByForegrip) is pumped by the carrying hand alone,
// by a jerk: the hand snapped back along the barrel (CarriedPumpSpeed or faster) opens it, snapped forward closes it,
// as a one-handed pump uses the gun's own weight. Its state goes with it as the carried bolt's: a shot not pumped may
// be handed over and is kept through the carry; the take-back finds it as the carrying hand left it (pumped: chambered;
// left back: back). Pumped chambered: nothing (counted). The carried gun never fires.
std::atomic<bool> g_carried_pump{false};
std::atomic<float> g_cp_speed{1.5f};  // [Reload] CarriedPumpSpeed, m/s along the barrel
constexpr double kCarriedPumpWindowMs = 60.0;  // the hand's velocity: its fastest two-frame step this recent (gestures.cpp)
bool g_psave_needs = false, g_psave_back = false;  // the Pump-action's state as it left the hand (reset_gun), its eWeapon and when
int g_psave_weapon = -1;
double g_psave_ms = -1e12;
bool g_cp_active = false, g_cp_needs = false, g_cp_back = false, g_cp_ooo = false;  // the carried pump's (under g_mutex)
int g_cp_weapon = -1;
double g_cp_ret_ms = -1e12;  // its take-back last seen under way
float g_cp_v = 0.0f;         // the carrying hand's last velocity along the barrel (m/s, + forward; the state line)
float g_cp_fwd[3] = {};      // the carried gun's forward (world) as last measured (the state line)
std::atomic<int> g_cp_pub{-1};  // the carried Pump-action whose fore-end is drawn so (PumpParts; body.cpp's carried draw), else -1
std::atomic<float> g_cp_amt{0.0f};
std::atomic<uint64_t> g_cp_handovers{0}, g_cp_restores{0}, g_cp_pumps{0};
// [Reload] CarriedAnyState (2026-10-31, the off-hand build step 3, R4; the user: "Can't let go of non bolt action
// rifles"): a lever, breech, break-open or semi-auto gun may be handed over to the foregrip with its action as it is (a
// shot not worked, a breech open with or without its round, the lever down); its state goes with it as the carried bolt's
// and pump's do, untouched while carried (those actions need the gun hand: the carried lever waits for the take-back),
// and the take-back finds it as it was left
std::atomic<bool> g_carried_any{true};
struct GunSave {
    int weapon = -1;
    double ms = -1e12;
    bool needs = false, open = false, breech_open = false, breech_round = false, lever_open = false, hammer_down = false, semi_back = false;
    float breech_amt = 0.0f, breech_hammer = 1.0f, semi_amt = 0.0f;
    bool muz_round = false;  // (C16c) the muzzle-loader's round down the barrel; its ram strokes made
    int muz_strokes = 0;
};
GunSave g_gsave, g_gc;  // the last such gun's state as it left the hand (reset_gun); the carried one's (under g_mutex)
bool g_gc_active = false;
double g_gc_ret_ms = -1e12;
std::atomic<uint64_t> g_gc_handovers{0}, g_gc_restores{0};
// (2026-11-02, GOAL-offhand-changes C16c; round 69, the user: the Blunderbuss cannot be carried after firing) the
// muzzle-loader too: carried unrammed, rammed once taken back
bool any_kind(int k) { return k == kLever || k == kBreech || k == kBreak || k == kSemi || k == kMuzzle; }
// (2026-11-01, GOAL-offhand-fixes F6; the user: "the Springfield cannot be worked while carried by the off hand; its
// trapdoor, if up, goes down when carried and up again when the gun hand takes it back") a carried single-shot breech
// (BreechParts, a hand-worked one: the Springfield's trapdoor, the Rolling Block's block) drawn as left and worked by the
// free gun hand at its handle as the off hand works it in hand: gripped there, drawn by the hand's travel along the
// carried gun (up: the trapdoor; back along the barrel: the Rolling Block's spur). Under g_mutex
bool g_gcb_grip = false, g_gcb_grip_was = false;
float g_gcb_s0 = 0.0f, g_gcb_d = 1e9f, g_gcb_zone[3] = {};
uint64_t g_gcb_serial = ~0ull;
float g_gcb_relR[9] = {}, g_gcb_relo[3] = {};  // (the review) the carried gun's frame in the carrying hand's target, taken at the grip
std::atomic<int> g_gcb_pub{-1};                       // the carried breech's weapon while drawn (carry_drivers), -1 none
std::atomic<float> g_gcb_amt{0.0f}, g_gcb_ham{1.0f};  // its drawn opening (0 shut - 1 open), its hammer (1 cocked)
std::atomic<uint64_t> g_gcb_opens{0}, g_gcb_shuts{0};
// the gun hand's orientations (the controller's own frame), for its angular velocity over two-frame steps
struct OriSample {
    float q[4];
    double ms;
};
OriSample g_ori[3];
int g_ori_n = 0;
float g_w[3] = {}, g_w_peak[3] = {};  // the last angular velocity (rad/s, controller axes x right, y up, z back), its peak
int g_close_hits = 0;
bool g_trig_was = false;
std::atomic<uint64_t> g_opens{0}, g_closes{0}, g_cycles{0}, g_held{0}, g_out_of_order{0}, g_shots_seen{0};
std::atomic<double> g_last_shot_ms{-1e12};  // the last shot seen (the game's count dropped): body.cpp's steadying
// [Hands] DualWieldOpen (2026-10-28, off; round 37 check 4, the user: "works but we need an input to open second gun"):
// with a second gun out (DualWield's of another slot, or DualWieldCopy's copy) that is a revolver (its kind's switch on,
// ManualRevolver), not carried, and shut, its own hand's stick held down (filter_stick's cone, past OpenStickY) and
// clicked opens it for loading (DualWieldCrossReload's rounds go in only then with LoadOnlyWhenOpen: can_load_second);
// a flick of that hand's roll or yaw (CloseFlickSpeed, after CloseFlickMinOpenMs; the top-break also upward) shuts it, as
// the gun in hand's. The layout's left stick only: a second gun in the gun stick's controller (a gun drawn by the other
// hand) is refused, said once. The click is the gun's (no L3, no crouch) and the stick's walk back is held at zero after
// it until the stick comes back; a plain click stays the game's; seated with [Horse] StickClickBrake the click is the
// brake. Open, its pull is held (a buzz, as HeldClick's: no click, it is not empty). Dead Eye shuts it. Its own state:
// the gun in hand's (g_open, g_open_amt, the cylinder's) untouched. With OpenCylinder it is drawn open (second_drivers).
std::atomic<bool> g_dual_open{false};
std::atomic<bool> g_sec_open_req{false};  // filter_second_stick's click, consumed by the frame end (the pad poll's thread)
std::atomic<bool> g_sec_open{false};      // the second gun (or the copy) open
std::atomic<int> g_sec_open_weapon{-1}, g_sec_open_ctrl{-1};  // the gun it is for (kept while its drawn opening eases shut)
std::atomic<bool> g_sec_open_copy{false};
std::atomic<float> g_sec_open_amt{0.0f};  // the drawn opening, eased (a quarter second either way)
double g_sec_open_ms = 0;
int g_sec_close_hits = 0;
OriSample g_sori[3];  // that hand's controller's orientations, for its angular velocity (as the gun hand's)
int g_sori_n = 0, g_sori_ctrl = -1;
float g_sw[3] = {};
std::atomic<uint64_t> g_sec_opens{0}, g_sec_closes{0}, g_sec_held{0}, g_sec_clicks{0}, g_sec_refused{0}, g_sec_zeroed{0}, g_sec_deadeye{0};
std::atomic<bool> g_sec_refuse_said{false};
// (2026-10-29, round 40: "Cocking the left hand gun does not seem to work") RevolverControl=cock with DualWieldOpen: the
// second gun's (the copy's) single action needs cocking after each of its shots, as the gun in hand's: its pull held until
// a quick flick down of its own stick (into the open cone past OpenStickY and back to the middle within the walk's wait,
// no click) cocks it, with the game's cock sound and a buzz; its hammer drawn down meanwhile (80 ms either way)
std::atomic<bool> g_sec_cock_req{false};  // filter_second_stick's flick, consumed by the frame end
std::atomic<bool> g_sec_needs{false};     // the second gun's hammer down: its pull held until cocked
std::atomic<float> g_sec_hammer{1.0f};    // its drawn hammer, 0 down - 1 cocked (eased)
std::atomic<int> g_sec_hammer_weapon{-1};  // the single action it is drawn on (-1: none)
uint64_t g_sec_shots_seen = 0;
std::atomic<uint64_t> g_sec_cocks{0}, g_sec_cock_held{0}, g_sec_flicks{0};
// (2026-10-29, the user: the walk back and the crouch or the brake not on a quick flick or click) while the gesture is armed
// (filter_second_stick): [Hands] DualWieldOpenWaitMs, the stick down in the open cone held at the middle that long before
// its walk back; DualWieldClickHoldMs, its click held back that long before it is the game's (L3, the brake)
std::atomic<float> g_open_wait_ms{250.0f}, g_click_hold_ms{200.0f};
std::atomic<bool> g_sec_armed{false};    // the gesture armed at the last poll
std::atomic<bool> g_click_waited{false};  // the click now passing waited its hold (controls.cpp: no chord wait on top)
std::atomic<bool> g_press_pending{false};  // a press held back now, within its wait (controls.cpp's left_click_held: PhysicalCrouch)
std::atomic<uint64_t> g_sec_walk_waits{0}, g_sec_walk_passed{0}, g_sec_click_passed{0}, g_sec_click_dropped{0};
float clamp_wait(float ms, float lo, float hi, float def) { return std::isfinite(ms) ? (ms < lo ? lo : ms > hi ? hi : ms) : def; }

void qmul(const float* a, const float* b, float* o) {  // o = a b (x y z w)
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
// the angular velocity in the controller's own frame from q0 to q1 over dt: dq = conj(q0) q1, w = axis * angle / dt
bool omega(const float* q0, const float* q1, double dt_ms, float* w) {
    if (dt_ms < 5.0) return false;
    const float c0[4] = {-q0[0], -q0[1], -q0[2], q0[3]};
    float d[4];
    qmul(c0, q1, d);
    if (d[3] < 0) for (float& x : d) x = -x;
    const float s = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const float ang = 2.0f * std::atan2(s, d[3]);
    const float k = s > 1e-6f ? ang / s / static_cast<float>(dt_ms * 0.001) : 0.0f;
    for (int i = 0; i < 3; ++i) w[i] = d[i] * k;
    return std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) < 40.0f;  // above: a tracking jump
}

// the drawn hand of controller c (world), from the body's points of the frame
bool body_hand(int c, float* out) {
    body::BodyPoints bp;
    if (!body::body_points(&bp)) return false;
    for (int j = 0; j < 2; ++j)
        if (bp.ctrl[j] == c && bp.hand_ok[j]) {
            std::memcpy(out, bp.hand[j], sizeof(bp.hand[j]));
            return true;
        }
    return false;
}

void reset_gun(int w, float clip) {
    if (kind_of(g_weapon) == kBolt) {  // [Reload] CarriedBolt: the bolt as it left the hand (a carry takes it on)
        g_bsave = bolt_get();
        g_bsave_weapon = g_weapon;
        g_bsave_ms = log::now_ms();
    }
    if (kind_of(g_weapon) == kPump) {  // [Reload] CarriedPump: the pump as it left the hand (a carry takes it on)
        g_psave_needs = g_needs;
        g_psave_back = g_pump_back;
        g_psave_weapon = g_weapon;
        g_psave_ms = log::now_ms();
    }
    if (any_kind(kind_of(g_weapon))) {  // [Reload] CarriedAnyState: the action as it left the hand (a carry takes it on)
        GunSave& g = g_gsave;
        g.weapon = g_weapon;
        g.ms = log::now_ms();
        g.needs = g_needs;
        g.open = g_open;
        g.breech_open = g_breech_open;
        g.breech_round = g_breech_round.load(std::memory_order_relaxed);
        g.breech_amt = g_breech_amt.load(std::memory_order_relaxed);
        g.breech_hammer = g_breech_hammer.load(std::memory_order_relaxed);
        g.lever_open = g_lever_open;
        g.hammer_down = g_hammer_down;
        g.semi_back = g_semi_back;
        g.semi_amt = g_semi_amt.load(std::memory_order_relaxed);
        g.muz_round = g_muz_round;
        g.muz_strokes = g_muz_strokes;
    }
    g_weapon = w;
    g_kind = kind_of(w);
    g_open = g_needs = false;
    g_last_clip = clip;
    g_was_empty = clip >= 0.0f && clip < 0.5f;
    g_bolt_grip = g_bolt_back = false;
    g_bolt_up = g_bolt_cycled = false;
    g_bolt_lift.store(0.0f, std::memory_order_relaxed);
    g_bolt_slide.store(0.0f, std::memory_order_relaxed);
    g_breech_open = g_breech_grip = false;
    g_breech_round.store(clip >= 0.5f, std::memory_order_relaxed);  // drawn loaded: chambered
    g_muz_round = clip >= 0.5f;  // (step 8) the muzzle-loader drawn loaded: ready (empty: a round wanted, below)
    g_muz_rod.store(0.0f, std::memory_order_relaxed);
    g_muz_strokes = 0;
    g_muz_grip = g_muz_down = false;
    g_breech_amt.store(0.0f, std::memory_order_relaxed);
    g_breech_hammer.store(1.0f, std::memory_order_relaxed);
    g_semi_grip = g_semi_back = false;
    g_semi_amt.store(0.0f, std::memory_order_relaxed);
    // PistolMagazines / PistolSlides: drawn loaded, chambered; drawn empty, the slide (bolt) back on its stop; the
    // magazine as it was left this session
    if (w >= 1 && w <= 3 && clip >= 1.5f) g_mag_out[w] = false;  // (the review: more than the chamber's round: a magazine is in)
    g_mag_in.store(!(w >= 1 && w <= 3 && g_mag_out[w]), std::memory_order_relaxed);
    g_chambered = clip >= 0.5f;
    g_locked = clip >= 0.0f && clip < 0.5f && (g_mag_in.load(std::memory_order_relaxed) || kind_of(w) == kMauser);
    g_pgrip = g_pback = g_pgrip_locked = false;
    g_pdrawn = g_locked ? 1.0f : 0.0f;
    g_clip_seated = false;
    g_clip_left = 0;
    g_expect_until = 0.0;
    g_lose_pending = 0;
    g_pblow_ms = -1e12;
    g_pump_have = g_pump_back = false;
    g_lever_down_ms = 0;
    g_lever_open = g_hammer_down = false;
    g_close_hits = 0;
    g_barrel_hold = g_barrel_take = false;
    g_rev_hammer.store(1.0f, std::memory_order_relaxed);  // drawn cocked (not eased from the last gun's hammer)
    g_thumb_t0.store(-1e12, std::memory_order_relaxed);  // ThumbCock: no thumb on the new gun's hammer
    g_thumb_weapon.store(-1, std::memory_order_relaxed);
    g_lever_shut_ms = -1e12;
    g_fan_pending = false;  // (the review: a redraw within the wait must not fire the new gun)
    g_fan_low = 0;
    g_gc_prev_ms = 0.0;  // [Hands] GunCollision: the other gun's sweep begun again on the new gun
    g_gc_armed = true;
    g_gc_in = g_gc_nb_said = false;
    // [Reload] CarriedBolt: the carried bolt gun taken back by the gun hand (its draw under way, or done within 2 s):
    // its bolt as the gun hand left it (a shot not cycled still holds the pull; worked while carried, chambered)
    if (g_cb_active && w >= 0 && w == g_cb_weapon && (dual::carry_returning() || log::now_ms() - g_cb_ret_ms < 2000.0)) {
        BoltState b = g_cbs;
        b.grip = false;
        b.ctrl = -1;
        bolt_put(b);
        g_cb_active = false;
        g_cb_restores.fetch_add(1, std::memory_order_relaxed);
        log::info("[actions] bolt (%d): taken back from the carry, its bolt as the gun hand left it (needs %d, up %d, back %d, lift %.2f, slide %.2f)", w,
                  b.needs ? 1 : 0, b.up ? 1 : 0, b.back ? 1 : 0, b.lift, b.slide);
    }
    // [Reload] CarriedPump: the carried Pump-action taken back: its pump as the carrying hand left it (a shot not pumped
    // still holds the pull; pumped while carried, chambered; left back, back: the front hand's next hold finds it there)
    // [Reload] CarriedAnyState: the carried lever, breech, break-open or semi-auto taken back: its action as it was left
    if (g_gc_active && w >= 0 && w == g_gc.weapon && (dual::carry_returning() || log::now_ms() - g_gc_ret_ms < 2000.0)) {
        const GunSave& g = g_gc;
        g_needs = g.needs;
        g_open = g.open;
        g_breech_open = g.breech_open;
        g_breech_round.store(g.breech_round, std::memory_order_relaxed);
        g_breech_amt.store(g.breech_amt, std::memory_order_relaxed);
        g_breech_hammer.store(g.breech_hammer, std::memory_order_relaxed);
        g_lever_open = g.lever_open;
        g_hammer_down = g.hammer_down;
        g_semi_back = g.semi_back;
        g_semi_amt.store(g.semi_amt, std::memory_order_relaxed);
        if (kind_of(w) == kMuzzle) {  // (C16c) its round and its ram as left (the review: a round the clip shows is down the barrel)
            g_muz_round = g.muz_round || clip >= 0.5f;
            g_muz_strokes = g.muz_round ? g.muz_strokes : 0;
        }
        g_gc_active = false;
        g_gc_restores.fetch_add(1, std::memory_order_relaxed);
        log::info("[actions] %s (%d): taken back from the carry, its action as it was left (needs %d, open %d, breech open %d round %d, lever down %d, semi back %d)",
                  kind_name(kind_of(w)), w, g.needs ? 1 : 0, g.open ? 1 : 0, g.breech_open ? 1 : 0, g.breech_round ? 1 : 0, g.lever_open ? 1 : 0,
                  g.semi_back ? 1 : 0);
    }
    if (g_cp_active && w >= 0 && w == g_cp_weapon && (dual::carry_returning() || log::now_ms() - g_cp_ret_ms < 2000.0)) {
        g_needs = g_cp_needs || g_cp_back;
        g_pump_back = g_cp_back;
        g_pump_amt.store(g_cp_back ? 1.0f : 0.0f, std::memory_order_relaxed);
        g_cp_active = false;
        g_cp_pub.store(-1, std::memory_order_relaxed);
        g_cp_restores.fetch_add(1, std::memory_order_relaxed);
        log::info("[actions] pump (%d): taken back from the carry, its pump as the carrying hand left it (needs %d, back %d)", w, g_needs ? 1 : 0,
                  g_cp_back ? 1 : 0);
    }
}
// the gun shut (a flick or the off hand): its sounds at the gun hand
void shut_sounds(int w, int gun_h) {
    if (const GunSounds* gs = gun_sounds(w)) {
        float at[3];
        if (body_hand(gun_h, at)) {
            play_at(gs->close, at);
            play_at(gs->close2, at);
        }
    }
}

// (cycle r199) "actions trace": each frame's bolt strokes while gripped (in hand and carried): the hand's height over
// the grip's along the gun's up from the stroke's reference (the handle's lift = it / kBoltRise, from position: never
// summed from per-frame steps), its travel back, the drawn lift and slide, the reference and the hand (world). Under g_mutex
struct BoltTrace {
    double ms;
    bool carried, up, back;
    float h, s, lift, slide, ref[3], oh[3];
};
constexpr int kTraceN = 1024;
BoltTrace g_trace[kTraceN];
uint64_t g_trace_n = 0;
// The bolt's strokes, one frame (the in-hand bolt's globals: g_bolt_*, g_needs): the hand `bh` (its interaction spot oh,
// its grip gripv) at the shut handle's zone zone0 (BoltParts: moved as drawn, up with its turn, back with the bolt),
// the strokes measured along the gun's forward u and up from ref, a point that moves with the gun; `holder` holds the
// gun (its pulses). take_d: [Reload] CarriedBolt, the hand's distance to the carried gun's take-back point (the zone
// takes the hand only nearer the bolt; 1e9 in hand). blift, bslide: the drawn handle and bolt, as left, updated.
void bolt_step(int w, const char* tag, bool bparts, int bh, int holder, const float* oh, float gripv, const float* ref, const float* u,
               const float* up, const float* zone0, float take_d, float& blift, float& bslide) {
    // the zone: the bolt's handle, with BoltParts where it is drawn (up with its turn, back with the bolt)
    for (int i = 0; i < 3; ++i)
        g_bolt_zone[i] = zone0[i] + (bparts ? up[i] * blift * kBoltRise - u[i] * bslide * kBoltStroke : 0.0f);
    const float dx = oh[0] - g_bolt_zone[0], dy = oh[1] - g_bolt_zone[1], dz = oh[2] - g_bolt_zone[2];
    g_bolt_d = std::sqrt(dx * dx + dy * dy + dz * dz);
    const bool in = g_bolt_d < g_bolt_r && g_bolt_d < take_d;  // (CarriedBolt: nearer the bolt than the take-back)
    const bool grip = gripv > 0.6f;
    // the travel along the gun's back axis (-u), from ref (in hand the gun hand): the gun's own motion does not count
    const float s = -((oh[0] - ref[0]) * u[0] + (oh[1] - ref[1]) * u[1] + (oh[2] - ref[2]) * u[2]);
    const float hgt = (oh[0] - ref[0]) * up[0] + (oh[1] - ref[1]) * up[1] + (oh[2] - ref[2]) * up[2];
    g_bolt_want[bh] = in || g_bolt_grip;
    if (grip && !g_grip_was && in) {
        g_bolt_grip = true;
        g_bolt_ctrl = bh;
        // a bolt already back stays back (BoltParts: where it is drawn, its handle as turned)
        g_bolt_s0 = bparts ? s - bslide * kBoltStroke : g_bolt_back ? s - g_bolt_travel : s;
        g_bolt_h0 = hgt - blift * kBoltRise;
        g_ooo = false;
        controllers::pulse(bh, 0.35f, 30);
        log::info("[actions] bolt (%d%s): gripped (%.3f m from its zone)%s", w, tag, g_bolt_d, g_needs ? "" : ", the action already worked");
    }
    if (bparts && g_needs && !g_bolt_back && bslide >= 0.9f) g_bolt_back = true;  // loaded from empty with it back
    if (g_bolt_grip && grip && bparts) {
        g_bolt_s = s - g_bolt_s0;
        const GunSounds* gs = gun_sounds(w);
        if (bslide <= 0.0f) {  // the bolt forward: the handle turns with the hand's rise
            const float l = (hgt - g_bolt_h0) / kBoltRise;
            blift = l < 0.0f ? 0.0f : l > 1.0f ? 1.0f : l;
            if (!g_bolt_up && blift >= 0.95f) {
                g_bolt_up = true;
                g_bolt_s0 = s;  // the pull counted from here
                g_bolt_s = 0.0f;
                if (gs) play_at(gs->open, oh);
                controllers::pulse(bh, 0.3f, 10);
                log::info("[actions] bolt (%d%s): the handle up", w, tag);
            } else if (g_bolt_up && blift <= 0.05f) {
                g_bolt_up = false;
                if (gs) play_at(gs->close2, oh);
                controllers::pulse(bh, 0.5f, 15);
                if (g_bolt_cycled) {
                    g_bolt_cycled = false;
                    g_needs = false;
                    g_cycles.fetch_add(1, std::memory_order_relaxed);
                    controllers::pulse(holder, 0.6f, 20);
                    log::info("[actions] bolt (%d%s): the handle down, chambered", w, tag);
                } else {
                    log::info("[actions] bolt (%d%s): the handle down", w, tag);
                }
            }
        }
        if (g_bolt_up) {  // the handle up: the bolt drawn back by the pull, 1:1 to its stop
            const float b = g_bolt_s / kBoltStroke;
            bslide = g_bolt_s <= 0.015f ? 0.0f : b > 1.0f ? 1.0f : b;  // within 1.5 cm of shut: shut
            if (bslide > 0.0f) blift = 1.0f;  // out of its lock the handle is all the way up
            if (!g_bolt_back && g_bolt_s >= g_bolt_travel) {
                if (g_needs) {
                    g_bolt_back = true;
                    if (gs) play_at(gs->open2, oh);
                    controllers::pulse(bh, 0.5f, 15);
                    controllers::pulse(holder, 0.3f, 10);
                    log::info("[actions] bolt (%d%s): back %.3f m", w, tag, g_bolt_s);
                } else if (!g_ooo) {  // drawn back while chambered: nothing (counted once a stroke)
                    g_ooo = true;
                    g_out_of_order.fetch_add(1, std::memory_order_relaxed);
                    log::info("[actions] bolt (%d%s): worked while chambered: nothing", w, tag);
                }
            } else if (g_bolt_back && g_bolt_s <= 0.015f) {
                g_bolt_back = false;
                g_bolt_cycled = true;
                if (gs) play_at(gs->close, oh);
                controllers::pulse(bh, 0.5f, 15);
                log::info("[actions] bolt (%d%s): forward (the handle down chambers it)", w, tag);
            } else if (g_bolt_s <= 0.015f) {
                g_ooo = false;
            }
        }
    } else if (g_bolt_grip && grip) {
        g_bolt_s = s - g_bolt_s0;
        if (!g_bolt_back && g_bolt_s >= g_bolt_travel) {
            if (g_needs) {
                g_bolt_back = true;
                controllers::pulse(bh, 0.5f, 15);
                controllers::pulse(holder, 0.3f, 10);
                log::info("[actions] bolt (%d%s): back %.3f m", w, tag, g_bolt_s);
            } else if (!g_ooo) {  // pulled back while chambered: nothing (counted once a stroke)
                g_ooo = true;
                g_out_of_order.fetch_add(1, std::memory_order_relaxed);
                log::info("[actions] bolt (%d%s): worked while chambered: nothing", w, tag);
            }
        } else if (g_bolt_back && g_bolt_s <= 0.015f) {
            g_bolt_back = false;
            g_needs = false;
            g_cycles.fetch_add(1, std::memory_order_relaxed);
            controllers::pulse(bh, 0.6f, 20);
            controllers::pulse(holder, 0.6f, 20);
            log::info("[actions] bolt (%d%s): forward, chambered", w, tag);
        }
    }
    if (g_bolt_grip && grip) {  // ("actions trace")
        BoltTrace& t = g_trace[g_trace_n++ % kTraceN];
        t.ms = log::now_ms();
        t.carried = tag && tag[0];
        t.up = g_bolt_up;
        t.back = g_bolt_back;
        t.h = hgt - g_bolt_h0;
        t.s = g_bolt_s;
        t.lift = blift;
        t.slide = bslide;
        std::memcpy(t.ref, ref, sizeof(t.ref));
        std::memcpy(t.oh, oh, sizeof(t.oh));
    }
    if (!grip) g_bolt_grip = false;
    g_grip_was = grip;
}

// (2026-11-01, the off-hand build step 4; the user's answer 2, "Pistol") a long bolt second gun (DualWield's, not the copy
// nor a carry) beside the gun in hand: its own bolt (g_sbs), needing working after a shot with rounds left, worked by the
// gun hand's trigger at its knob on the second gun as drawn (body's drawn_second; the knobs K1 measured), the gun in hand's
// fire held while that hand is there (CarriedBoltTrigger's latch), the second gun's pull held until worked
// (second_pull_held). The in-hand bolt state swapped out around its step and put back. Under g_mutex.
BoltState g_sbs;
bool g_sb_active = false;
int g_sb_weapon = -1;
uint64_t g_sb_shots = 0;
double g_sb_shot_ms = -1.0;  // (the review) a shot seen, its "rounds left" judged 150 ms on (dual's clip is set before the shot)
float g_sb_zone[3] = {}, g_sb_d = 1e9f, g_sb_u[3] = {}, g_sb_up[3] = {};  // (the status: the second gun's barrel and up)
std::atomic<bool> g_sb_hold{false};  // the second gun's pull held (its bolt not worked or open): second_pull_held
std::atomic<int> g_sb_pub{-1};  // (as CarriedBolt's g_cb_pub: the render thread reads these, never g_mutex) its weapon, -1 none
std::atomic<float> g_sb_lift{0.0f}, g_sb_slide{0.0f};
// (the review) published none: frame()'s early branch (no gun in hand) and a frame not wanting it; the state kept for
// the same second gun (a flicker of ready, BoltParts toggled), dropped for another
void second_bolt_idle() {
    g_sb_hold.store(false, std::memory_order_relaxed);
    g_sb_pub.store(-1, std::memory_order_relaxed);
    g_sb_active = false;
}
void second_bolt_frame(bool on, int in_hand) {
    const dual::State ds = dual::state();
    // (the review: not with a bolt gun in hand, whose drawn bolt and loading gate read the swapped values)
    const bool want = on && g_bolt.load(std::memory_order_relaxed) && g_bolt_parts.load(std::memory_order_relaxed) && ds.on && ds.ready && !ds.copy &&
                      !ds.carry && ds.weapon >= 0 && kind_of(ds.weapon) == kBolt && ds.ctrl >= 0 && ds.ctrl <= 1 && kind_of(in_hand) != kBolt;
    if (!want) {
        second_bolt_idle();
        return;
    }
    int sw = -1;
    const uint64_t n = dual::shots_fired(&sw);
    const double now = log::now_ms();
    if (g_sb_weapon != ds.weapon) {
        g_sbs = BoltState{};
        g_sb_shot_ms = -1.0;
        log::info("[actions] bolt (%d, the second gun): its own bolt, worked by the gun hand's trigger at it (clip %.0f)", ds.weapon, ds.clip);
    }
    if (!g_sb_active || g_sb_weapon != ds.weapon) g_sb_shots = n;  // (the copy's or the absent time's shots not counted)
    g_sb_active = true;
    g_sb_weapon = ds.weapon;
    if (n > g_sb_shots) {
        g_sb_shots = n;
        g_sb_shot_ms = now;
    }
    if (g_sb_shot_ms > 0.0 && now - g_sb_shot_ms >= 150.0) {  // (the review: dual's clip read after the shot by then)
        g_sb_shot_ms = -1.0;
        if (ds.clip >= 0.5f) {
            g_sbs.needs = true;
            log::info("[actions] bolt (%d, the second gun): shot, the bolt needs working (clip %.0f)", ds.weapon, ds.clip);
        }
    }
    g_sb_hold.store(g_sbs.needs || g_sb_shot_ms > 0.0 || g_sbs.lift > 0.05f || g_sbs.slide > 0.0f, std::memory_order_relaxed);
    const int gun_h = controls::gun_hand();
    float R[9], o[3];
    body::BodyPoints bp;
    if (gun_h == ds.ctrl || !body::drawn_second(R, o, nullptr, nullptr) || !body::body_points(&bp)) return;
    const int jh = bp.ctrl[0] == gun_h ? 0 : 1;
    if (!bp.hand_ok[jh]) return;
    float oh[3], u[3], upv[3], z0[3];
    holster::grab_point(bp, jh, oh);
    const float* zg = ds.weapon == 20 ? kKnob20 : kKnob13a;  // (the Explosive Rifle's the Bolt Action's)
    for (int i = 0; i < 3; ++i) {  // drawn_second's axes as its columns: x right, y up, z back
        u[i] = -R[i * 3 + 2];
        upv[i] = R[i * 3 + 1];
        z0[i] = o[i] + R[i * 3] * zg[0] + R[i * 3 + 1] * zg[1] + R[i * 3 + 2] * zg[2];
    }
    const hands::Hand hh = hands::get(gun_h);
    const float pv = menu::visible() ? 0.0f : hh.trigger > (g_sbs.grip ? 0.4f : 0.6f) ? 1.0f : 0.0f;
    const BoltState keep = bolt_get();
    float kz[3];
    std::memcpy(kz, g_bolt_zone, sizeof(kz));
    const float kd = g_bolt_d;
    const bool kwant = g_bolt_want[gun_h].load(std::memory_order_relaxed);
    bolt_put(g_sbs);
    float lift = g_sbs.lift, slide = g_sbs.slide;
    bolt_step(ds.weapon, ", the second gun", true, gun_h, ds.ctrl, oh, pv, o, u, upv, z0, 1e9f, lift, slide);
    g_sbs = bolt_get();
    g_sbs.lift = lift;
    g_sbs.slide = slide;
    std::memcpy(g_sb_zone, g_bolt_zone, sizeof(g_sb_zone));
    g_sb_d = g_bolt_d;
    std::memcpy(g_sb_u, u, sizeof(g_sb_u));
    std::memcpy(g_sb_up, upv, sizeof(g_sb_up));
    const bool at = g_sbs.grip || g_bolt_d < g_bolt_r;
    bolt_put(keep);
    std::memcpy(g_bolt_zone, kz, sizeof(kz));
    g_bolt_d = kd;
    g_bolt_want[gun_h].store(kwant, std::memory_order_relaxed);
    if (at && gun_h >= 0 && gun_h < 2) g_cb_trig_latch[gun_h].store(true, std::memory_order_relaxed);  // the gun in hand's fire held there
    g_sb_hold.store(g_sbs.needs || g_sb_shot_ms > 0.0 || g_sbs.lift > 0.05f || g_sbs.slide > 0.0f, std::memory_order_relaxed);
    g_sb_lift.store(g_sbs.lift, std::memory_order_relaxed);
    g_sb_slide.store(g_sbs.slide, std::memory_order_relaxed);
    g_sb_pub.store(g_sb_weapon, std::memory_order_relaxed);
}

// (2026-11-01, the off-hand build step 8) the Blunderbuss as the second gun (BreechParts' muzzle-loader): after its shot a
// round across at its muzzle (DualWieldCrossReload, can_load_second), then rammed by the gun hand's trigger at its muzzle
// (as answer 2's bolt: two strokes back along its barrel and forward), its pulls held until then. Under g_mutex
struct MuzState {
    bool needs = false, round = true, down = false, grip = false, grip_was = false;
    int strokes = 0;
    float s0 = 0.0f;
};
MuzState g_smz;
bool g_smz_active = false;
uint64_t g_smz_shots = 0;
double g_smz_shot_ms = -1.0;  // (the review) its last shot's time: a clip still loaded 300 ms on (infinite ammo) is loaded
float g_smz_last_clip = -1.0f, g_smz_zone[3] = {}, g_smz_d = 1e9f, g_smz_u[3] = {}, g_smz_up[3] = {};
std::atomic<bool> g_smz_hold{false};
void second_muzzle_idle() {
    g_smz_hold.store(false, std::memory_order_relaxed);
    g_smz_rod.store(0.0f, std::memory_order_relaxed);
    g_smz_active = false;
}
void second_muzzle_frame(bool on) {
    const dual::State ds = dual::state();
    const bool want = on && g_breech_parts.load(std::memory_order_relaxed) && ds.on && ds.ready && !ds.copy && !ds.carry && ds.weapon >= 0 &&
                      kind_of(ds.weapon) == kMuzzle && ds.ctrl >= 0 && ds.ctrl <= 1;
    if (!want) {
        second_muzzle_idle();
        return;
    }
    int sw = -1;
    const uint64_t n = dual::shots_fired(&sw);
    if (!g_smz_active) {
        g_smz = MuzState{};  // (the review: as drawn: loaded is ready, empty wants a round (below), never a state from before)
        g_smz.round = true;
        g_smz_shot_ms = -1.0;
        g_smz_shots = n;
        g_smz_last_clip = ds.clip;
        log::info("[actions] muzzle-loader (%d, the second gun): rammed by the gun hand's trigger at its muzzle (clip %.0f)", ds.weapon, ds.clip);
    }
    g_smz_active = true;
    if (n > g_smz_shots) {
        g_smz_shots = n;
        g_smz.needs = true;
        g_smz.round = false;
        g_smz.strokes = 0;
        g_smz.down = false;
        g_smz_shot_ms = log::now_ms();
        log::info("[actions] muzzle-loader (%d, the second gun): shot, a round in at its muzzle and rammed", ds.weapon);
    }
    if (g_smz.needs && !g_smz.round && g_smz_shot_ms > 0.0 && log::now_ms() - g_smz_shot_ms > 300.0) {
        g_smz_shot_ms = -1.0;
        if (ds.clip >= 0.5f) {  // (the review) the shot left it loaded (infinite ammo): nothing to put in, ready
            g_smz.needs = false;
            g_smz.round = true;
        }
    }
    if (!g_smz.needs && ds.clip >= 0.0f && ds.clip < 0.5f && g_smz_last_clip >= 0.0f && g_smz_last_clip < 0.5f) {
        g_smz.needs = true;  // (drawn empty)
        g_smz.round = false;
        g_smz.strokes = 0;
    }
    if (g_smz.needs && !g_smz.round && g_smz_last_clip >= 0.0f && ds.clip > g_smz_last_clip + 0.5f) {
        g_smz.round = true;
        g_smz.strokes = 0;
        g_smz.down = false;
        log::info("[actions] muzzle-loader (%d, the second gun): a round down its barrel: ram it (%d strokes)", ds.weapon, kRamStrokes);
    }
    g_smz_last_clip = ds.clip;
    g_smz_hold.store(g_smz.needs, std::memory_order_relaxed);
    const int gun_h = controls::gun_hand();
    const hands::Hand hh = hands::get(gun_h);
    const bool pv = !menu::visible() && hh.trigger > (g_smz.grip ? 0.4f : 0.6f);
    float R[9], o[3];
    body::BodyPoints bp;
    if (!(g_smz.needs && g_smz.round) || gun_h == ds.ctrl || !body::drawn_second(R, o, nullptr, nullptr) || !body::body_points(&bp)) {
        g_smz.grip = false;
        g_smz.grip_was = pv;  // (the review: a trigger held as the round went in is no fresh press)
        g_smz_rod.store(0.0f, std::memory_order_relaxed);  // (the review: the rod home)
        return;
    }
    const int jh = bp.ctrl[0] == gun_h ? 0 : 1;
    if (!bp.hand_ok[jh]) {
        g_smz_rod.store(0.0f, std::memory_order_relaxed);
        return;
    }
    float oh[3], u[3];
    holster::grab_point(bp, jh, oh);
    for (int i = 0; i < 3; ++i) {  // drawn_second's axes as its columns: x right, y up, z back
        u[i] = -R[i * 3 + 2];
        g_smz_u[i] = u[i];
        g_smz_up[i] = R[i * 3 + 1];
        g_smz_zone[i] = o[i] + R[i * 3] * kMuzzleAt[0] + R[i * 3 + 1] * kMuzzleAt[1] + R[i * 3 + 2] * kMuzzleAt[2];
    }
    const float dx = oh[0] - g_smz_zone[0], dy = oh[1] - g_smz_zone[1], dz = oh[2] - g_smz_zone[2];
    g_smz_d = std::sqrt(dx * dx + dy * dy + dz * dz);
    const bool in = g_smz_d < g_bolt_r;
    const float s = -((oh[0] - o[0]) * u[0] + (oh[1] - o[1]) * u[1] + (oh[2] - o[2]) * u[2]);  // back along its barrel, from the gun
    if (pv && !g_smz.grip_was && in) {
        g_smz.grip = true;
        g_smz.down = false;
        g_smz.s0 = s;
        controllers::pulse(gun_h, 0.35f, 25);
        log::info("[actions] muzzle-loader (%d, the second gun): the ram gripped at its muzzle (%.3f m), stroke %d of %d", ds.weapon, g_smz_d,
                  g_smz.strokes + 1, kRamStrokes);
    }
    if (g_smz.grip && pv) {
        const GunSounds* gs = gun_sounds(ds.weapon);
        float at[3];
        const bool hand = body_hand(ds.ctrl, at);
        const float st = s - g_smz.s0;
        g_smz_rod.store(std::fmax(0.0f, std::fmin(1.0f, st / kRamStroke)), std::memory_order_relaxed);  // (the rod with the hand)
        if (!g_smz.down && st >= kRamStroke) {
            g_smz.down = true;
            if (gs && hand) play_at(gs->open, at);
            controllers::pulse(gun_h, 0.4f, 15);
        } else if (g_smz.down && st <= kRamStroke * 0.3f) {
            g_smz.down = false;
            ++g_smz.strokes;
            if (gs && hand) play_at(gs->close, at);
            controllers::pulse(gun_h, 0.4f, 15);
            log::info("[actions] muzzle-loader (%d, the second gun): rammed, stroke %d of %d", ds.weapon, g_smz.strokes, kRamStrokes);
            if (g_smz.strokes >= kRamStrokes) {
                g_smz.needs = false;
                if (gs && hand) play_at(gs->close2, at);
                controllers::pulse(ds.ctrl, 0.6f, 20);
                log::info("[actions] muzzle-loader (%d, the second gun): rammed home: ready", ds.weapon);
            }
        }
    }
    if (!pv) {
        g_smz.grip = false;
        g_smz_rod.store(0.0f, std::memory_order_relaxed);
    }
    g_smz.grip_was = pv;
    if ((in || g_smz.grip) && gun_h >= 0 && gun_h < 2) g_cb_trig_latch[gun_h].store(true, std::memory_order_relaxed);  // the gun in hand's fire held
    g_smz_hold.store(g_smz.needs, std::memory_order_relaxed);
}

// (2026-11-01, the off-hand build step 4, S4) a pistol as the second gun (DualWield's, not the copy) with PistolSlides: its
// slide held back on its last round (its pulls held), released forward by the gun hand's trigger at its slide's handle on
// the second gun as drawn once it has rounds again (cross reload), the game's slide sound. Its magazine: not this.
bool g_spl_active = false, g_spl_locked = false, g_spl_grip_was = false;
int g_spl_weapon = -1;
uint64_t g_spl_shots = 0;
double g_spl_shot_ms = -1.0;
float g_spl_zone[3] = {}, g_spl_d = 1e9f;
std::atomic<bool> g_spl_hold{false};
std::atomic<int> g_spl_pub{-1};
std::atomic<float> g_spl_amt{0.0f};
void second_pistol_idle() {
    g_spl_grip_was = hands::get(controls::gun_hand()).trigger > 0.6f;  // (the review: a press held into its start is no fresh one)
    g_spl_hold.store(false, std::memory_order_relaxed);
    g_spl_pub.store(-1, std::memory_order_relaxed);
    g_spl_active = false;
}
// (2026-11-01, GOAL-offhand-changes C7; round 63 check 18, the user: a pistol as the second gun reloads by rounds, not a
// magazine) [Reload] PistolMagazines for the second gun (DualWield's, not the copy, not a carry): its own stick's flick drops
// its magazine (its count to the chambered round, the rest back to the spare: set_clip's), the chest then gives a magazine
// for it (holster.cpp), brought to its grip it goes in (load_second's, as many as it holds); rounds alone it takes none
std::atomic<bool> g_smag_req{false};  // filter_second_stick's flick for the second pistol's magazine
bool g_smag_out[4] = {};              // each pistol's magazine out as the second gun (the session's)
bool g_smag_low[4] = {};              // and its count seen at the chambered round since (the drop queued: a tick late)
// (the review: with DualWieldCrossReload, which offers the chest's magazine to the other hand; without it, rounds as before)
// (cycle H7: a pistol beside a revolver is the same slot's, never a second gun: the copy, two of one pistol, too)
bool second_mag_gun(const dual::State& ds) {
    return g_on.load(std::memory_order_relaxed) && g_mags.load(std::memory_order_relaxed) && dual::cross_reload() && ds.on && ds.ready &&
           !ds.carry && kind_of(ds.weapon) == kPistol && ds.weapon >= 1 && ds.weapon <= 3;
}
void second_mag_frame(bool on, const RdrvrActorState& st) {
    const bool req = g_smag_req.exchange(false);
    const dual::State ds = dual::state();
    // (the review) a magazine put in by any way (its count past the chambered round): in
    // (the re-review: only once the drop's own lower count has been seen: its native is queued, the old count reads a tick on)
    if (second_mag_gun(ds) && g_smag_out[ds.weapon]) {
        if (ds.clip < 1.5f) g_smag_low[ds.weapon] = true;
        else if (g_smag_low[ds.weapon]) g_smag_out[ds.weapon] = false;
    }
    if (!on || !req) return;
    if (!second_mag_gun(ds) || (st.weapon_flags & RDRVR_WEAPON_DEADEYE) || menu::visible()) return;
    const int w = ds.weapon;
    if (g_smag_out[w]) {
        log::info("[actions] pistol (%d, the second gun): its stick's flick: no magazine in it", w);
        return;
    }
    const int n = static_cast<int>(std::floor(ds.clip + 0.01f));
    const int keep = n >= 1 ? 1 : 0;  // (the chambered round stays)
    if (ds.copy) {  // (the copy: its own count, dual's)
        const int r = n > keep ? dual::drop_copy_mag(st.actor) : 1;
        if (r < 0) g_smag_req = true;  // (the review: its spare settling: the flick kept for the next frame)
        if (r <= 0) return;
    } else {
        RdrvrActorState sx = st;
        sx.weapon = w;
        sx.clip = static_cast<float>(n);
        if (n > keep && !reload::set_clip(sx, keep)) return;
    }
    g_smag_out[w] = true;
    g_smag_low[w] = n <= keep;  // (already at the chamber: seen low)
    float at[3];
    if (body_hand(ds.ctrl, at)) play_at("ARMOR_METAL_SHORT_MASTER", at);
    controllers::pulse(ds.ctrl, 0.5f, 15);
    log::info("[actions] pistol (%d, the second gun): the magazine dropped by its stick (clip %d -> %d)", w, n, keep);
}
void second_pistol_frame(bool on) {
    const dual::State ds = dual::state();
    const PistolDef* pd = ds.weapon >= 0 ? pistol_of(ds.weapon) : nullptr;
    const bool want = on && g_slides.load(std::memory_order_relaxed) && ds.on && ds.ready && !ds.copy && !ds.carry && pd && ds.ctrl >= 0 &&
                      ds.ctrl <= 1;
    if (!want) {
        second_pistol_idle();
        return;
    }
    int sw = -1;
    const uint64_t n = dual::shots_fired(&sw);
    const double now = log::now_ms();
    if (!g_spl_active || g_spl_weapon != ds.weapon) {
        if (g_spl_weapon != ds.weapon || ds.clip >= 0.5f) g_spl_locked = false;  // (the review: loaded since, not held back)
        g_spl_shots = n;
        g_spl_shot_ms = -1.0;
        log::info("[actions] %s (%d, the second gun): its slide held back on its last round, released by the gun hand's trigger at it",
                  kind_name(kind_of(ds.weapon)), ds.weapon);
    }
    g_spl_active = true;
    g_spl_weapon = ds.weapon;
    if (n > g_spl_shots) {
        g_spl_shots = n;
        g_spl_shot_ms = now;
    }
    if (g_spl_shot_ms > 0.0 && now - g_spl_shot_ms >= 150.0) {  // (as the bolt's: dual's clip read after the shot by then)
        g_spl_shot_ms = -1.0;
        if (ds.clip >= 0.0f && ds.clip < 0.5f && !g_spl_locked) {
            g_spl_locked = true;
            log::info("[actions] %s (%d, the second gun): the last round fired, the slide held back", kind_name(kind_of(ds.weapon)), ds.weapon);
        }
    }
    const int gun_h = controls::gun_hand();
    const hands::Hand hh = hands::get(gun_h);
    const bool pv = !menu::visible() && hh.trigger > 0.6f;
    float R[9], o[3];
    body::BodyPoints bp;
    if (g_spl_locked && gun_h != ds.ctrl && body::drawn_second(R, o, nullptr, nullptr) && body::body_points(&bp)) {
        const int jh = bp.ctrl[0] == gun_h ? 0 : 1;
        if (bp.hand_ok[jh]) {
            float oh[3];
            holster::grab_point(bp, jh, oh);
            for (int i = 0; i < 3; ++i)  // drawn_second's axes as its columns; the handle back by the slide's travel
                // (the review: the Semi-Auto's toggle is not drawn back here: its knobs where they are)
                g_spl_zone[i] = o[i] + R[i * 3] * pd->handle[0] + R[i * 3 + 1] * pd->handle[1] +
                                R[i * 3 + 2] * (pd->handle[2] + (ds.weapon == 1 ? 0.0f : pd->travel));
            const float dx = oh[0] - g_spl_zone[0], dy = oh[1] - g_spl_zone[1], dz = oh[2] - g_spl_zone[2];
            g_spl_d = std::sqrt(dx * dx + dy * dy + dz * dz);
            const bool in = g_spl_d < g_bolt_r;
            if (in && gun_h >= 0 && gun_h < 2) g_cb_trig_latch[gun_h].store(true, std::memory_order_relaxed);  // the gun in hand's fire held
            if (in && pv && !g_spl_grip_was && ds.clip >= 0.5f) {
                g_spl_locked = false;
                float at[3];
                if (body_hand(ds.ctrl, at)) play_at(pd->fwd, at);
                controllers::pulse(gun_h, 0.4f, 15);
                controllers::pulse(ds.ctrl, 0.5f, 15);
                log::info("[actions] %s (%d, the second gun): the slide released forward by the gun hand (%.3f m from it), clip %.0f",
                          kind_name(kind_of(ds.weapon)), ds.weapon, g_spl_d, ds.clip);
            }
        }
    }
    g_spl_grip_was = pv;
    g_spl_hold.store(g_spl_locked, std::memory_order_relaxed);
    g_spl_amt.store(g_spl_locked ? 1.0f : 0.0f, std::memory_order_relaxed);
    g_spl_pub.store(ds.weapon, std::memory_order_relaxed);
}

// [Reload] CarriedBolt: no gun in hand (frame()'s early branch, after its resets): the bolt of the bolt gun carried by
// its foregrip, worked by the free gun hand at the carried gun (body::carry_frame: where the game update placed it at
// the front hand), its zone learned in hand. on: ManualActions on, first person, the player known. Under g_mutex.
void carried_frame(double now, bool on) {
    const dual::State ds = dual::state();
    const bool carried = ds.carry && kind_of(ds.carry_weapon) == kBolt;
    // the switches (kept through a frame without the player's state); the strokes only with it
    const bool cfg = g_on.load(std::memory_order_relaxed) && g_carried_bolt.load(std::memory_order_relaxed) && g_bolt.load(std::memory_order_relaxed);
    const bool opt = on && cfg;
    const int gun_h = controls::gun_hand();
    // [Reload] CarriedBoltTrigger: the free gun hand's press on the bolt, its trigger (else its grip, as before)
    const bool by_trig = g_cb_trigger.load(std::memory_order_relaxed);
    // (the review: the trigger's grip at 0.6, its let-go at 0.4, as dual.cpp's pull; nothing while the menu is open)
    auto press = [&]() {
        const hands::Hand hh = hands::get(gun_h);
        if (!by_trig) return hh.grip;
        if (menu::visible()) return 0.0f;
        return hh.trigger > (g_cbs.grip ? 0.4f : 0.6f) ? 1.0f : 0.0f;
    };
    if (carried && dual::carry_returning()) g_cb_ret_ms = now;
    if (!g_cb_active && opt && carried && ds.carry_weapon == g_bsave_weapon && now - g_bsave_ms < 3000.0 && g_bsave_ms >= ds.carry_ms - 1.0) {
        // the hand-over: the bolt as it left the hand goes with the gun (a shot not cycled stays so)
        g_cbs = g_bsave;
        g_cbs.grip = false;
        g_cbs.ctrl = -1;
        g_cbs.grip_was = press() > 0.6f;
        g_cb_active = true;
        g_cb_weapon = ds.carry_weapon;
        g_cb_ret_ms = -1e12;
        g_bsave_weapon = -1;  // (taken once)
        g_cb_handovers.fetch_add(1, std::memory_order_relaxed);
        log::info("[actions] bolt (%d, carried): carried by the foregrip, its bolt worked by the gun hand (needs %d, up %d, back %d, zone %s)", g_cb_weapon,
                  g_cbs.needs ? 1 : 0, g_cbs.up ? 1 : 0, g_cbs.back ? 1 : 0,
                  bolt_slot(g_cb_weapon) >= 0 && g_bolt_zone_gun_ok[bolt_slot(g_cb_weapon)] ? "learned" : "not learned: none");
    }
    if (g_cb_active) {  // kept while carried, and 2 s after a take-back under way (the draw lands: reset_gun restores it)
        const bool keep = cfg && (ds.carry ? ds.carry_weapon == g_cb_weapon : now - g_cb_ret_ms < 2000.0);
        if (!keep) {
            g_cb_active = false;
            log::info("[actions] bolt (%d, carried): %s: its bolt's state dropped", g_cb_weapon,
                      !cfg ? "the option or the manual actions off" : ds.carry ? "another gun carried" : "the carry ended (not taken back)");
        }
    }
    const bool bparts = g_bolt_parts.load(std::memory_order_relaxed);
    float lift = g_cbs.lift, slide = g_cbs.slide;  // as left
    g_cb_ran = false;
    bool cheld = false;  // [Reload] PartHandSnap: the free gun hand on the carried bolt this frame (its handle, turn, carrying hand)
    float cp[3] = {}, cturn = 0.0f;
    int cj = -1;
    g_cb_take = 1e9f;
    if (const uint64_t ser = dual::carry_serial(); ser != g_cb_rel_serial || !g_cb_active) {  // a new carry: its frame made again
        g_cb_rel_serial = ser;
        g_cb_rel_ok = false;
        g_cb_rel_n = 0;
        std::memset(g_cb_relR_sum, 0, sizeof(g_cb_relR_sum));
        std::memset(g_cb_relo_sum, 0, sizeof(g_cb_relo_sum));
    }
    if (opt && g_cb_active && ds.carry && !dual::carry_returning() && gun_h != ds.ctrl) {
        const int zs = bolt_slot(g_cb_weapon);
        const int jc = ds.john;  // John's carrying hand
        body::BodyPoints bp;
        float R[9], o[3], Rg[9], og[3];
        const bool pts_ok = body::body_points(&bp) && jc >= 0 && jc <= 1 && bp.hand_ok[jc];
        // (2026-10-31, R10; 54.1 / 55.6 "takes a moment before bolt is interactable", r77 h5: 1.3 s still, over 18.5 s moving,
        // waiting for T to settle, which moving it never does) once the carry's ease is over: the gun is placed rigidly at
        // the front target from then, settled or not
        const bool cb_frame_ready = ds.carry_T_ok || now > body::carry_ease_end() + 20.0;
        if (pts_ok && !g_cb_rel_ok && cb_frame_ready && body::carry_frame(Rg, og, false)) {  // the frame on the carrying hand, averaged (the placement: C37b's review)
            const float* Rt = bp.target_rot[jc];  // columns: the target's axes
            const float* ht = bp.hand[jc];
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j)
                    g_cb_relR_sum[i * 3 + j] += Rt[0 * 3 + i] * Rg[0 * 3 + j] + Rt[1 * 3 + i] * Rg[1 * 3 + j] + Rt[2 * 3 + i] * Rg[2 * 3 + j];
                g_cb_relo_sum[i] += Rt[0 * 3 + i] * (og[0] - ht[0]) + Rt[1 * 3 + i] * (og[1] - ht[1]) + Rt[2 * 3 + i] * (og[2] - ht[2]);
            }
            if (++g_cb_rel_n >= kCbRelN) {  // the mean, its axes made square again (x, then y, z = x cross y)
                float c[3][3];
                for (int j = 0; j < 3; ++j)
                    for (int i = 0; i < 3; ++i) c[j][i] = g_cb_relR_sum[i * 3 + j];
                auto unit = [](float* v) {
                    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                    if (l > 1e-6f)
                        for (int k = 0; k < 3; ++k) v[k] /= l;
                    return l > 1e-6f;
                };
                bool ok = unit(c[0]);
                const float d = c[1][0] * c[0][0] + c[1][1] * c[0][1] + c[1][2] * c[0][2];
                for (int k = 0; k < 3; ++k) c[1][k] -= d * c[0][k];
                ok = unit(c[1]) && ok;
                c[2][0] = c[0][1] * c[1][2] - c[0][2] * c[1][1];
                c[2][1] = c[0][2] * c[1][0] - c[0][0] * c[1][2];
                c[2][2] = c[0][0] * c[1][1] - c[0][1] * c[1][0];
                for (int j = 0; j < 3; ++j)
                    for (int i = 0; i < 3; ++i) g_cb_relR[i * 3 + j] = c[j][i];
                for (int i = 0; i < 3; ++i) g_cb_relo[i] = g_cb_relo_sum[i] / static_cast<float>(g_cb_rel_n);
                g_cb_rel_ok = ok;
                if (!ok) g_cb_rel_n = 0;
                log::info("[actions] bolt (%d, carried): the gun's frame on the carrying hand %s (%d placements): at (%.3f %.3f %.3f) in its target", g_cb_weapon,
                          ok ? "kept" : "not made", kCbRelN, g_cb_relo[0], g_cb_relo[1], g_cb_relo[2]);
            }
        }
        if (pts_ok && g_cb_rel_ok) {  // the carried gun's frame now: the carrying hand's target times it
            const float* Rt = bp.target_rot[jc];
            const float* ht = bp.hand[jc];
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) R[i * 3 + j] = Rt[i * 3 + 0] * g_cb_relR[0 * 3 + j] + Rt[i * 3 + 1] * g_cb_relR[1 * 3 + j] + Rt[i * 3 + 2] * g_cb_relR[2 * 3 + j];
                o[i] = ht[i] + Rt[i * 3 + 0] * g_cb_relo[0] + Rt[i * 3 + 1] * g_cb_relo[1] + Rt[i * 3 + 2] * g_cb_relo[2];
            }
        }
        if (zs >= 0 && (g_bolt_zone_gun_ok[zs] || g_cb_weapon == 13 || g_cb_weapon == 20 || g_cb_weapon == 31) && pts_ok && g_cb_rel_ok) {  // (the review: the knob needs no learning)
            const int jh = bp.ctrl[0] == gun_h ? 0 : 1;  // John's hand on the gun hand's controller
            if (bp.hand_ok[jh]) {
                float oh[3], u[3], upv[3], z0[3], tp[3];
                holster::grab_point(bp, jh, oh);
                // (2026-10-30, round 52: "Right hands bolt action pose is too far to the right. Just holds air while working the
                // bolt") the carried bolt gripped at its knob. The learned zone is the off hand's reach over the top
                // (BoltZoneOffset from the gun hand's wrist), about 5 cm above the knob and 4 cm right of it (r287): the free gun
                // hand, drawn with that point between its thumb and index (body.cpp's snap), closed on air beside the gun. The
                // knob in the gun's own frame (bind space, m): the Bolt Action's bolt's outermost vertex along +x ("holster glow"
                // far (0.0283 0.0469 0.0024), r288) moved in by about the knob's radius (6 mm along x, the vertex on its
                // outer side); a gun without one keeps the learned zone ([Reload] BoltZoneOffset then moves it)
                static constexpr float kKnob13[3] = {0.022f, 0.047f, 0.002f};
                // (the review) the Explosive Rifle's bolt the Bolt Action's (the same bone and axis) [I: its knob unmeasured]
                const float* zg = g_cb_weapon == 13 || g_cb_weapon == 31 ? kKnob13 : g_cb_weapon == 20 ? kKnob20 : g_bolt_zone_gun[zs];
                // (2026-11-01, GOAL-offhand-changes C37b; round 66 check 12: the rings kept their places while the carried gun
                // moved off them) the zone on the gun as drawn (body::carry_frame); the strokes' axes and reference still the
                // frame held on the carrying hand (its bob against the hand no stroke)
                float Rd[9], od[3];
                const bool drawn_f = body::carry_frame(Rd, od);  // (the review: gripped too: no jump at the grip)
                const float* Rz = drawn_f ? Rd : R;
                const float* oz = drawn_f ? od : o;
                for (int i = 0; i < 3; ++i) {  // the carried gun's axes (columns: x right, y up, z back)
                    u[i] = -R[i * 3 + 2];
                    upv[i] = R[i * 3 + 1];
                    z0[i] = oz[i] + Rz[i * 3] * zg[0] + Rz[i * 3 + 1] * zg[1] + Rz[i * 3 + 2] * zg[2];
                }
                if (holster::carry_take_point(tp))
                    g_cb_take = std::sqrt((oh[0] - tp[0]) * (oh[0] - tp[0]) + (oh[1] - tp[1]) * (oh[1] - tp[1]) + (oh[2] - tp[2]) * (oh[2] - tp[2]));
                bolt_put(g_cbs);  // the carried bolt into the working state, its strokes, and out again
                // (CarriedBoltTrigger: the trigger works it wherever the take-back's point is, and the grip is never the bolt's)
                bolt_step(g_cb_weapon, ", carried", bparts, gun_h, ds.ctrl, oh, press(), o, u, upv, z0, by_trig ? 1e9f : g_cb_take, lift, slide);
                if (by_trig) g_bolt_want[gun_h] = false;
                g_cbs = bolt_get();
                // (the review) and while the hand is at the bolt: the pull's own ramp to the grip never the game's either
                if (by_trig && (g_cbs.grip || g_bolt_d < g_bolt_r) && gun_h >= 0 && gun_h < 2) g_cb_trig_latch[gun_h].store(true, std::memory_order_relaxed);
                std::memcpy(g_cb_zone, g_bolt_zone, sizeof(g_cb_zone));
                g_cb_d = g_bolt_d;
                std::memcpy(g_cb_fwd, u, sizeof(g_cb_fwd));
                std::memcpy(g_cb_up, upv, sizeof(g_cb_up));
                bolt_put(BoltState{});  // no gun in hand: the in-hand bolt none
                g_bolt_zone[0] = g_bolt_zone[1] = g_bolt_zone[2] = 0.0f;
                g_bolt_d = 1e9f;
                g_cb_ran = true;
                // (2026-10-28) [Reload] PartHandSnap: the gripped carried bolt for body.cpp, the handle as drawn and the gun's
                // axes in the carrying hand's target frame (its frame as body.cpp draws it, not these points' frame)
                if (g_cbs.grip && g_part_snap_cfg.load(std::memory_order_relaxed)) {
                    const float* Rt = bp.target_rot[jc];
                    const float* ht = bp.hand[jc];
                    for (int i = 0; i < 3; ++i)
                        cp[i] = Rt[0 * 3 + i] * (g_cb_zone[0] - ht[0]) + Rt[1 * 3 + i] * (g_cb_zone[1] - ht[1]) + Rt[2 * 3 + i] * (g_cb_zone[2] - ht[2]);
                    cturn = bparts ? lift * kBoltTurnDeg : 0.0f;
                    cj = jc;
                    cheld = true;
                }
            }
        }
    }
    {  // the free gun hand's weight on the carried bolt, 0.1 s either way (as the in-hand part's)
        static double last = 0;  // (not run while a gun is in hand: a step after that gap is one frame's)
        const float step = last > 0 && now - last < 50.0 ? static_cast<float>((now - last) / 100.0) : 0.1f;
        last = now;
        const float target = cheld ? 1.0f : 0.0f;
        float b = g_cpart_snap.load(std::memory_order_relaxed);
        b = b < target ? (b + step > target ? target : b + step) : (b - step < target ? target : b - step);
        cpart_begin();
        if (cheld) {
            for (int i = 0; i < 3; ++i) g_cpart_p[i].store(cp[i], std::memory_order_relaxed);
            for (int i = 0; i < 9; ++i) g_cpart_R[i].store(g_cb_relR[i], std::memory_order_relaxed);
            g_cpart_turn.store(cturn, std::memory_order_relaxed);
            g_cpart_j.store(cj, std::memory_order_relaxed);
        }
        g_cpart_snap.store(b, std::memory_order_relaxed);
        cpart_end();
    }
    if (!g_cb_ran) {  // a hand away, not tracked, or no carry: as left (as the in-hand bolt's)
        g_cbs.grip = false;
        g_cbs.grip_was = press() > 0.6f;
        g_cb_d = 1e9f;
    }
    if (!bparts) {
        lift = slide = 0.0f;
        g_cbs.up = g_cbs.cycled = false;
    }
    g_cbs.lift = lift;
    g_cbs.slide = slide;
    // the drawn bolt of the carried gun (BoltParts): body.cpp's carried draw (carry_drivers)
    const bool pub = g_cb_active && bparts && ds.carry && ds.carry_weapon == g_cb_weapon;
    g_cb_lift.store(pub ? lift : 0.0f, std::memory_order_relaxed);
    g_cb_slide.store(pub ? slide : 0.0f, std::memory_order_relaxed);
    g_cb_pub.store(pub ? g_cb_weapon : -1, std::memory_order_relaxed);
}

// (C15b-d) the ramrod drawn in hand rh (controller; John's hand jr): its head at that hand's spot moved by RamrodOffset, its
// body along where the hand points turned by RamrodTurn and RamrodTilt, in the gun's model (GR, GO: the drawn gun's frame,
// its columns x right, y up, z back): g_rod_s, g_rod_q; head_w its head (world). (C15h: the carried gun's too.)
bool rod_in_hand(const body::BodyPoints& bp, int rh, int jr, const float* spot, const float* GR, const float* GO, float head_w[3]) {
    float ro[3];
    float rturn = 0.0f, rtilt = 0.0f;
    {
        std::lock_guard lock(g_rod_mx);
        const RodPlace& rp = g_rod_place[rh == 1 && g_rod_place_r ? 1 : 0];
        std::memcpy(ro, rp.offset, sizeof(ro));
        rturn = rp.turn, rtilt = rp.tilt;
    }
    const float* Th = bp.target_rot[jr];  // (round 67: RamrodOffset in the hand's axes: x right, y up, -z forward)
    float hp[3];
    for (int i = 0; i < 3; ++i) hp[i] = spot[i] + Th[i * 3] * ro[0] + Th[i * 3 + 1] * ro[1] - Th[i * 3 + 2] * ro[2];
    std::memcpy(head_w, hp, sizeof(hp));  // (the review: the muzzle's zone and the strokes by the rod's head)
    const float d[3] = {hp[0] - GO[0], hp[1] - GO[1], hp[2] - GO[2]};
    for (int j = 0; j < 3; ++j) {
        const float p = GR[j] * d[0] + GR[3 + j] * d[1] + GR[6 + j] * d[2];
        g_rod_s[j].store(p - kRodHead[j], std::memory_order_relaxed);
    }
    // (C15c) the rod along where the holding hand points (its target's barrel axis, as a gun held in it would point),
    // in the model: the turn taking the rod's own +z there (about the axis z x v)
    float bt0[3] = {0.0f, 0.174f, -0.985f}, bt[3], vw[3], v[3];
    aim::barrel_in_target(bt0);
    {  // (C15d) turned about the hand's up (y) by RamrodTurn, then its right (x) by RamrodTilt
        const float a = rturn * 0.0174532925f, c = rtilt * 0.0174532925f;
        const float t1[3] = {bt0[0] * std::cos(a) + bt0[2] * std::sin(a), bt0[1], -bt0[0] * std::sin(a) + bt0[2] * std::cos(a)};
        bt[0] = t1[0], bt[1] = t1[1] * std::cos(c) - t1[2] * std::sin(c), bt[2] = t1[1] * std::sin(c) + t1[2] * std::cos(c);
    }
    const float* To = bp.target_rot[jr];
    for (int i = 0; i < 3; ++i) vw[i] = To[i * 3] * bt[0] + To[i * 3 + 1] * bt[1] + To[i * 3 + 2] * bt[2];
    for (int j = 0; j < 3; ++j) v[j] = GR[j] * vw[0] + GR[3 + j] * vw[1] + GR[6 + j] * vw[2];
    const float vl = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    float Q[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (vl > 1e-4f && std::isfinite(vl)) {
        for (float& x : v) x /= vl;
        const float c = v[2];                              // cos: z . v
        const float ax[3] = {-v[1], v[0], 0.0f};           // z x v
        const float sn = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1]);
        if (sn > 1e-5f) {
            const float kx = ax[0] / sn, ky = ax[1] / sn, t = 1.0f - c;  // Rodrigues about (kx, ky, 0)
            Q[0] = c + kx * kx * t, Q[1] = kx * ky * t, Q[2] = ky * sn;
            Q[3] = kx * ky * t, Q[4] = c + ky * ky * t, Q[5] = -kx * sn;
            Q[6] = -ky * sn, Q[7] = kx * sn, Q[8] = c;
        } else if (c < 0.0f) {  // straight back: half round about y
            Q[0] = -1.0f, Q[8] = -1.0f;
        }
    }
    {
        std::lock_guard lock(g_rod_mx);
        std::memcpy(g_rod_q, Q, sizeof(Q));
    }
    return std::isfinite(g_rod_s[0].load()) && std::isfinite(g_rod_s[1].load()) && std::isfinite(g_rod_s[2].load());
}
// [Reload] CarriedPump (above): the carried Pump-action's frame, with or without a gun in the gun hand (the carrying hand
// works it). Under g_mutex.
// [Reload] CarriedAnyState: the hand-over takes the saved action on; kept while carried and 2 s after a take-back under
// way (reset_gun restores it), as carried_pump_frame's
void carried_any_frame(double now, bool on) {
    const dual::State ds = dual::state();
    const bool carried = ds.carry && any_kind(kind_of(ds.carry_weapon));
    const bool cfg = g_on.load(std::memory_order_relaxed) && g_carried_any.load(std::memory_order_relaxed);
    if (carried && dual::carry_returning()) g_gc_ret_ms = now;
    // (the review) a save made after this carry began: the put-away comes after begin_carry, a redraw's older save is not it
    if (!g_gc_active && on && cfg && carried && ds.carry_weapon == g_gsave.weapon && now - g_gsave.ms < 3000.0 && g_gsave.ms >= ds.carry_ms - 1.0) {
        g_gc = g_gsave;
        g_gc_active = true;
        g_gc_ret_ms = -1e12;
        g_gsave.weapon = -1;  // (taken once)
        g_gc_handovers.fetch_add(1, std::memory_order_relaxed);
        log::info("[actions] %s (%d, carried): carried by the foregrip, its action kept as left (needs %d, open %d, breech open %d, lever down %d)",
                  kind_name(kind_of(g_gc.weapon)), g_gc.weapon, g_gc.needs ? 1 : 0, g_gc.open ? 1 : 0, g_gc.breech_open ? 1 : 0, g_gc.lever_open ? 1 : 0);
    }
    if (g_gc_active) {
        const bool keep = cfg && (ds.carry ? ds.carry_weapon == g_gc.weapon : now - g_gc_ret_ms < 2000.0);
        if (!keep) {
            g_gc_active = false;
            log::info("[actions] %s (%d, carried): %s: its action's state dropped", kind_name(kind_of(g_gc.weapon)), g_gc.weapon,
                      !cfg ? "the option or the manual actions off" : ds.carry ? "another gun carried" : "the carry ended (not taken back)");
        }
    }
    // (F6) the carried breech: drawn as left, worked by the free gun hand (g_weapon < 0: the gun hand holds no gun)
    // (the review: drawn through the take-back too, until reset_gun restores it in hand; worked only while carried)
    const Breech* gbz = g_gc_active && ds.carry && ds.carry_weapon == g_gc.weapon && kind_of(g_gc.weapon) == kBreech &&
                                g_breech_parts.load(std::memory_order_relaxed)
                            ? breech_of(g_gc.weapon)
                            : nullptr;
    if (const uint64_t ser = dual::carry_serial(); ser != g_gcb_serial) {
        g_gcb_serial = ser;
        g_gcb_grip = false;
    }
    if (!gbz) {
        g_gcb_grip = false;
        g_gcb_d = 1e9f;
        g_gcb_pub.store(-1, std::memory_order_relaxed);
        return;
    }
    const int gun_h = controls::gun_hand();
    const bool free_hand = g_weapon < 0 && gun_h != ds.ctrl && on && gbz->gesture != 0 && !dual::carry_returning();
    if (free_hand) g_breech_want[0] = g_breech_want[1] = false;  // (the no-gun frame: the in-hand section's reset does not run)
    body::BodyPoints bp;
    float R[9], o[3];
    if (free_hand && body::body_points(&bp) && body::carry_frame(R, o)) {
        const int jh = bp.ctrl[0] == gun_h ? 0 : 1;  // John's hand on the gun hand's controller
        const int jc = ds.john;                     // John's carrying hand
        // (the review: carry_frame bobs about 1 cm against the carrying hand, r198, 40% of the Rolling Block's stroke) while
        // gripped the stroke is measured in the gun's frame held fixed on the carrying hand's target as it was at the grip
        auto fixed_frame = [&](float* Rf, float* of) {
            const float* Rt = bp.target_rot[jc];  // columns: the target's axes
            const float* ht = bp.hand[jc];
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) Rf[i * 3 + j] = Rt[i * 3] * g_gcb_relR[j] + Rt[i * 3 + 1] * g_gcb_relR[3 + j] + Rt[i * 3 + 2] * g_gcb_relR[6 + j];
                of[i] = ht[i] + Rt[i * 3] * g_gcb_relo[0] + Rt[i * 3 + 1] * g_gcb_relo[1] + Rt[i * 3 + 2] * g_gcb_relo[2];
            }
        };
        if (bp.hand_ok[jh] && jc >= 0 && jc <= 1 && bp.hand_ok[jc]) {
            if (g_gcb_grip) fixed_frame(R, o);
            float oh[3], hz[3];
            holster::grab_point(bp, jh, oh);
            const float amt0 = g_gc.breech_amt;
            for (int i = 0; i < 3; ++i) hz[i] = gbz->shut[i] + (gbz->open[i] - gbz->shut[i]) * amt0;
            for (int i = 0; i < 3; ++i) g_gcb_zone[i] = o[i] + R[i * 3] * hz[0] + R[i * 3 + 1] * hz[1] + R[i * 3 + 2] * hz[2];
            const float dx = oh[0] - g_gcb_zone[0], dy = oh[1] - g_gcb_zone[1], dz = oh[2] - g_gcb_zone[2];
            g_gcb_d = std::sqrt(dx * dx + dy * dy + dz * dz);
            const bool in = g_gcb_d < g_bolt_r;
            const bool grip = hands::get(gun_h).grip > 0.6f;
            // the hand's travel along the carried gun: up (its y: the trapdoor), or back along the barrel (its z: the spur)
            const int ax = gbz->gesture == 2 ? 1 : 2;
            const float s = (oh[0] - o[0]) * R[ax] + (oh[1] - o[1]) * R[3 + ax] + (oh[2] - o[2]) * R[6 + ax];
            g_breech_want[gun_h] = in || g_gcb_grip;  // (holster: no take-back while this hand is at the breech)
            if (grip && !g_gcb_grip_was && in) {
                g_gcb_grip = true;
                {  // the gun's frame in the carrying hand's target now (Rt^T R, Rt^T (o - ht))
                    const float* Rt = bp.target_rot[jc];
                    const float* ht = bp.hand[jc];
                    float Rp[9], op[3];  // (C37b, the review: the placement, paired with this frame's points; not the drawn)
                    if (body::carry_frame(Rp, op, false)) {
                        std::memcpy(R, Rp, sizeof(Rp));
                        std::memcpy(o, op, sizeof(op));
                    }
                    for (int i = 0; i < 3; ++i) {
                        for (int j = 0; j < 3; ++j) g_gcb_relR[i * 3 + j] = Rt[i] * R[j] + Rt[3 + i] * R[3 + j] + Rt[6 + i] * R[6 + j];
                        g_gcb_relo[i] = Rt[i] * (o[0] - ht[0]) + Rt[3 + i] * (o[1] - ht[1]) + Rt[6 + i] * (o[2] - ht[2]);
                    }
                }
                g_gcb_s0 = s - amt0 * gbz->travel;  // taken where it is drawn
                controllers::pulse(gun_h, 0.35f, 25);
                log::info("[actions] breech (%d, carried): gripped by the gun hand (%.3f m from its zone, drawn %.2f)", g_gc.weapon, g_gcb_d, amt0);
            }
            if (g_gcb_grip && grip) {
                const float b = (s - g_gcb_s0) / gbz->travel;
                g_gc.breech_amt = b < 0.0f ? 0.0f : b > 1.0f ? 1.0f : b;
                const GunSounds* gs = gun_sounds(g_gc.weapon);
                float at[3];
                const bool hand = body_hand(gun_h, at);
                if (!g_gc.breech_open && g_gc.breech_amt >= 0.95f) {
                    g_gc.breech_open = true;
                    if (gbz->cock_on_open) {
                        g_gc.hammer_down = false;
                        g_gc.breech_hammer = 1.0f;  // (the review: in hand as left)
                    }
                    if (gs && hand) play_at(gs->open, at);
                    controllers::pulse(ds.ctrl, 0.4f, 15);
                    g_gcb_opens.fetch_add(1, std::memory_order_relaxed);
                    log::info("[actions] breech (%d, carried): open%s, clip %.0f/%.0f", g_gc.weapon, g_gc.breech_round ? " (a live round in it)" : ", the case out",
                              ds.clip, ds.clip_max);
                } else if (g_gc.breech_open && g_gc.breech_amt <= 0.05f) {
                    g_gc.breech_open = false;
                    g_gc.hammer_down = false;
                    g_gc.breech_hammer = 1.0f;
                    if (gs && hand) {
                        play_at(gs->close, at);
                        play_at(gs->close2, at);
                    }
                    controllers::pulse(ds.ctrl, 0.6f, 20);
                    g_gcb_shuts.fetch_add(1, std::memory_order_relaxed);
                    // (the review: as in hand) no spare rounds but the gun's own count has some: from those
                    if (!g_gc.breech_round && ds.spare >= 0.0f && ds.spare < 1.0f && ds.clip >= 0.5f) {
                        g_gc.breech_round = true;
                        log::info("[actions] breech (%d, carried): no spare rounds: chambered from the gun's own count (clip %.0f)", g_gc.weapon, ds.clip);
                    }
                    if (g_gc.breech_round) {
                        if (g_gc.needs) g_cycles.fetch_add(1, std::memory_order_relaxed);
                        g_gc.needs = false;
                    }
                    log::info("[actions] breech (%d, carried): shut%s", g_gc.weapon, g_gc.breech_round ? ", ready" : " with no round in: not ready");
                }
            }
            if (!grip) g_gcb_grip = false;
            g_gcb_grip_was = grip;
        }
    } else {
        g_gcb_grip = false;
        g_gcb_grip_was = hands::get(gun_h).grip > 0.6f;
    }
    // the drawn block and hammer (the hammer down after the shot until the breech is shut, or opened where that cocks it)
    g_gcb_amt.store(g_gc.breech_amt, std::memory_order_relaxed);
    g_gcb_ham.store(g_gc.hammer_down ? 0.0f : 1.0f, std::memory_order_relaxed);
    g_gcb_pub.store(g_gc.weapon, std::memory_order_relaxed);
}
// (2026-11-02, GOAL-offhand-changes C15h; round 70 check 2, the user: "ramrod cannot be grabbed by gun hand when it is carried
// by off hand") the carried Blunderbuss rammed by the free gun hand: its round down the barrel (carried_round_in), the ramrod
// taken from its place on the carried gun (RamrodFrom=gun) or at the chest (holster's press, ramrod_taker), drawn in that
// hand, in at the carried muzzle and out: rammed home as carried (g_gc.needs off: the take-back finds it ready). RamrodAtChest
// off: the bare hand's strokes there. The strokes measured in the gun's frame held on the carrying hand from the muzzle's
// grip (carry_frame bobs about 1 cm, the stroke is 6). Under g_mutex: the no-gun frame, after carried_any_frame.
std::atomic<int> g_gcr_hand{-1};       // the controller holding the carried gun's ramrod (-1 none)
std::atomic<bool> g_gcr_ready{false};  // the frame end's: the carried gun's ramrod to take at the chest now
std::atomic<int> g_gcr_pub{-1};        // the carried gun's rod drawn in the hand: its eWeapon (carry_drivers), -1 none
std::atomic<float> g_gcr_rod{0.0f};    // (RamrodAtChest off) the carried gun's rod with the bare hand's ram (kDrvPump)
bool g_gcr_on = false;                 // the frame end's: a carried ram under way (carried_hint)
bool g_gcr_grip = false, g_gcr_grip_was = false, g_gcr_take_was = false, g_gcr_down = false;
float g_gcr_relR[9] = {}, g_gcr_relo[3] = {}, g_gcr_s0 = 0.0f;
float g_gcr_back[3] = {};                 // (the status) the carried gun's back along its barrel (world)
float g_gcr_zone[3] = {}, g_gcr_d = 1e9f;  // (carried_hint) the rod's place to take, else the muzzle (world); the hand (its rod's head) from it
void carried_muzzle_frame(bool on) {
    g_rod_shown.store(false, std::memory_order_relaxed);  // (no gun in hand: the in-hand rod not drawn)
    const dual::State ds = dual::state();
    const int gun_h = controls::gun_hand();
    const bool rod_mode = g_rod_chest.load(std::memory_order_relaxed);
    const bool act = on && g_breech_parts.load(std::memory_order_relaxed) && kind_on(kMuzzle) && g_gc_active && ds.carry && !dual::carry_returning() &&
                     ds.carry_weapon == g_gc.weapon && kind_of(g_gc.weapon) == kMuzzle && g_gc.needs && g_gc.muz_round && gun_h >= 0 && gun_h <= 1 &&
                     gun_h != ds.ctrl && g_weapon < 0;
    const bool grip = gun_h >= 0 && gun_h <= 1 && hands::get(gun_h).grip > 0.6f;
    g_gcr_on = false;
    g_gcr_d = 1e9f;
    if (!act) {
        if (g_gcr_hand.load(std::memory_order_relaxed) >= 0) log::info("[actions] muzzle-loader (carried): the ramrod put back (the ram over)");
        g_gcr_hand.store(-1, std::memory_order_relaxed);
        g_gcr_ready.store(false, std::memory_order_relaxed);
        g_gcr_pub.store(-1, std::memory_order_relaxed);
        g_gcr_rod.store(0.0f, std::memory_order_relaxed);
        g_gcr_grip = false;
        g_gcr_grip_was = g_gcr_take_was = grip;
        return;
    }
    int rh = g_gcr_hand.load(std::memory_order_relaxed);
    if (rh >= 0 && (rh != gun_h || !rod_mode || hands::get(rh).grip < 0.4f)) {
        log::info("[actions] muzzle-loader (%d, carried): the ramrod let go: back in its place", g_gc.weapon);
        g_gcr_hand.store(-1, std::memory_order_relaxed);
        rh = -1;
        g_gcr_grip = false;
    }
    g_gcr_ready.store(rod_mode && rh < 0, std::memory_order_relaxed);
    body::BodyPoints bp;
    float R[9], o[3];
    const bool fr = body::body_points(&bp) && body::carry_frame(R, o);
    const int jh = fr && bp.ctrl[0] == gun_h ? 0 : 1, jc = ds.john;
    if (!fr || !bp.hand_ok[jh] || jc < 0 || jc > 1 || !bp.hand_ok[jc]) {
        g_gcr_pub.store(-1, std::memory_order_relaxed);
        g_gcr_rod.store(0.0f, std::memory_order_relaxed);  // (the review: the bare hand's rod not left drawn)
        g_gcr_grip_was = g_gcr_take_was = grip;
        return;
    }
    g_gcr_on = true;
    g_gcr_back[0] = R[2], g_gcr_back[1] = R[5], g_gcr_back[2] = R[8];
    float spot[3];
    holster::grab_point(bp, jh, spot);
    bool want = false;  // (holster: no take-back while the gun hand works here)
    if (rod_mode && rh < 0 && g_rod_from_gun.load(std::memory_order_relaxed)) {  // the rod's place under the carried gun's barrel
        float go[3];
        {
            std::lock_guard lock(g_rod_mx);
            std::memcpy(go, g_rod_grab_off, sizeof(go));
        }
        const float gp[3] = {kRodGrab[0] + go[0], kRodGrab[1] + go[1], kRodGrab[2] - go[2]};
        for (int i = 0; i < 3; ++i) g_gcr_zone[i] = o[i] + R[i * 3] * gp[0] + R[i * 3 + 1] * gp[1] + R[i * 3 + 2] * gp[2];
        const float dx = spot[0] - g_gcr_zone[0], dy = spot[1] - g_gcr_zone[1], dz = spot[2] - g_gcr_zone[2];
        g_gcr_d = std::sqrt(dx * dx + dy * dy + dz * dz);
        want = g_gcr_d < std::fmax(g_bolt_r, 0.09f);
        if (grip && !g_gcr_take_was && want) {
            rh = gun_h;
            g_gcr_hand.store(rh, std::memory_order_relaxed);
            g_gcr_ready.store(false, std::memory_order_relaxed);
            controllers::pulse(gun_h, 0.35f, 25);
            log::info("[actions] muzzle-loader (%d, carried): the ramrod taken from the gun by the %s hand (%.3f m from its place)", g_gc.weapon,
                      gun_h ? "right" : "left", g_gcr_d);
        }
    }
    g_gcr_take_was = grip;
    float head[3] = {spot[0], spot[1], spot[2]};
    const bool shown = rod_mode && rh >= 0 && rod_in_hand(bp, rh, jh, spot, R, o, head);
    g_gcr_pub.store(shown ? g_gc.weapon : -1, std::memory_order_relaxed);
    {  // the muzzle: the frame as it was at its grip while gripped
        float Rm[9], om[3];
        if (g_gcr_grip) {
            const float* Rt = bp.target_rot[jc];
            const float* ht = bp.hand[jc];
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) Rm[i * 3 + j] = Rt[i * 3] * g_gcr_relR[j] + Rt[i * 3 + 1] * g_gcr_relR[3 + j] + Rt[i * 3 + 2] * g_gcr_relR[6 + j];
                om[i] = ht[i] + Rt[i * 3] * g_gcr_relo[0] + Rt[i * 3 + 1] * g_gcr_relo[1] + Rt[i * 3 + 2] * g_gcr_relo[2];
            }
        } else {
            std::memcpy(Rm, R, sizeof(Rm));
            std::memcpy(om, o, sizeof(om));
        }
        float mz[3];
        for (int i = 0; i < 3; ++i) mz[i] = om[i] + Rm[i * 3] * kMuzzleAt[0] + Rm[i * 3 + 1] * kMuzzleAt[1] + Rm[i * 3 + 2] * kMuzzleAt[2];
        const bool held = rod_mode && rh >= 0;
        const float* hp = held ? head : spot;  // (the rod's head, where it is drawn)
        const float dx = hp[0] - mz[0], dy = hp[1] - mz[1], dz = hp[2] - mz[2];
        const float md = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!rod_mode || rh >= 0) {
            std::memcpy(g_gcr_zone, mz, sizeof(mz));
            g_gcr_d = md;
        }
        const bool in = md < (held ? std::fmax(g_bolt_r, 0.12f) : g_bolt_r);
        const bool mg = grip && (!rod_mode || held);
        const float s = dx * Rm[2] + dy * Rm[5] + dz * Rm[8];  // back along the barrel from the muzzle (z back)
        if (mg && !g_gcr_grip && in && (held || !g_gcr_grip_was)) {
            g_gcr_grip = true;
            g_gcr_down = false;
            g_gcr_s0 = s;
            const float* Rt = bp.target_rot[jc];  // the gun's frame in the carrying hand's target now (Rt^T R, Rt^T (o - ht))
            const float* ht = bp.hand[jc];
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) g_gcr_relR[i * 3 + j] = Rt[i] * R[j] + Rt[3 + i] * R[3 + j] + Rt[6 + i] * R[6 + j];
                g_gcr_relo[i] = Rt[i] * (o[0] - ht[0]) + Rt[3 + i] * (o[1] - ht[1]) + Rt[6 + i] * (o[2] - ht[2]);
            }
            controllers::pulse(gun_h, 0.35f, 25);
            log::info("[actions] muzzle-loader (%d, carried): %s at the muzzle (%.3f m from it)", g_gc.weapon, held ? "the ramrod" : "the ram gripped", md);
        }
        if (g_gcr_grip && mg) {
            const GunSounds* gs = gun_sounds(g_gc.weapon);
            float at[3];
            const bool hand = body_hand(ds.ctrl, at);
            if (held) {  // in (past 0.7 of the stroke), then out (under 0.05): rammed home
                if (!g_gcr_down && s >= kRamStroke * 0.7f) {
                    g_gcr_down = true;
                    if (gs && hand) play_at(gs->open, at);
                    controllers::pulse(gun_h, 0.4f, 15);
                    log::info("[actions] muzzle-loader (%d, carried): the ramrod in", g_gc.weapon);
                } else if (g_gcr_down && s <= kRamStroke * 0.05f) {
                    g_gc.needs = false;
                    g_gc.hammer_down = false;
                    g_cycles.fetch_add(1, std::memory_order_relaxed);
                    g_gcr_hand.store(-1, std::memory_order_relaxed);
                    g_gcr_pub.store(-1, std::memory_order_relaxed);
                    g_gcr_grip = false;
                    if (gs && hand) {
                        play_at(gs->close, at);
                        play_at(gs->close2, at);
                    }
                    controllers::pulse(ds.ctrl, 0.6f, 20);
                    log::info("[actions] muzzle-loader (%d, carried): the ramrod in and out: rammed home: ready", g_gc.weapon);
                }
            } else {  // the bare hand's strokes
                const float st = s - g_gcr_s0;
                g_gcr_rod.store(std::fmax(0.0f, std::fmin(1.0f, st / kRamStroke)), std::memory_order_relaxed);
                if (!g_gcr_down && st >= kRamStroke) {
                    g_gcr_down = true;
                    if (gs && hand) play_at(gs->open, at);
                    controllers::pulse(gun_h, 0.4f, 15);
                } else if (g_gcr_down && st <= kRamStroke * 0.3f) {
                    g_gcr_down = false;
                    ++g_gc.muz_strokes;
                    if (gs && hand) play_at(gs->close, at);
                    controllers::pulse(gun_h, 0.4f, 15);
                    log::info("[actions] muzzle-loader (%d, carried): rammed, stroke %d of %d", g_gc.weapon, g_gc.muz_strokes, kRamStrokes);
                    if (g_gc.muz_strokes >= kRamStrokes) {
                        g_gc.needs = false;
                        g_gc.hammer_down = false;
                        g_cycles.fetch_add(1, std::memory_order_relaxed);
                        g_gcr_grip = false;
                        g_gcr_rod.store(0.0f, std::memory_order_relaxed);
                        if (gs && hand) play_at(gs->close2, at);
                        controllers::pulse(ds.ctrl, 0.6f, 20);
                        log::info("[actions] muzzle-loader (%d, carried): rammed home: ready", g_gc.weapon);
                    }
                }
            }
        }
        if (!mg) {
            g_gcr_grip = false;
            g_gcr_rod.store(0.0f, std::memory_order_relaxed);
        }
        want = want || in || g_gcr_grip;
    }
    g_gcr_grip_was = grip;
    g_breech_want[gun_h] = want;
}
void carried_pump_frame(double now, bool on) {
    const dual::State ds = dual::state();
    const bool carried = ds.carry && kind_of(ds.carry_weapon) == kPump;
    const bool cfg = g_on.load(std::memory_order_relaxed) && g_carried_pump.load(std::memory_order_relaxed) && g_pump.load(std::memory_order_relaxed);
    if (carried && dual::carry_returning()) g_cp_ret_ms = now;
    if (!g_cp_active && on && cfg && carried && ds.carry_weapon == g_psave_weapon && now - g_psave_ms < 3000.0 && g_psave_ms >= ds.carry_ms - 1.0) {
        // the hand-over: the pump as it left the hand goes with the gun (a shot not pumped stays so)
        g_cp_active = true;
        g_cp_weapon = ds.carry_weapon;
        g_cp_needs = g_psave_needs;
        g_cp_back = g_psave_back;
        g_cp_ooo = false;
        g_cp_ret_ms = -1e12;
        g_psave_weapon = -1;  // (taken once)
        g_cp_handovers.fetch_add(1, std::memory_order_relaxed);
        log::info("[actions] pump (%d, carried): carried by the foregrip, pumped by a jerk of the carrying hand (needs %d, back %d)", g_cp_weapon,
                  g_cp_needs ? 1 : 0, g_cp_back ? 1 : 0);
    }
    if (g_cp_active) {  // kept while carried, and 2 s after a take-back under way (the draw lands: reset_gun restores it)
        const bool keep = cfg && (ds.carry ? ds.carry_weapon == g_cp_weapon : now - g_cp_ret_ms < 2000.0);
        if (!keep) {
            g_cp_active = false;
            log::info("[actions] pump (%d, carried): %s: its pump's state dropped", g_cp_weapon,
                      !cfg ? "the option or the manual actions off" : ds.carry ? "another gun carried" : "the carry ended (not taken back)");
        }
    }
    g_cp_v = 0.0f;
    float R[9], o[3], v[3];
    if (on && g_cp_active && ds.carry && !dual::carry_returning() && body::carry_frame(R, o) &&
        gestures::hand_velocity(ds.ctrl, kCarriedPumpWindowMs, v)) {
        // the hand's velocity along the carried gun's forward (its z is back)
        g_cp_v = -(v[0] * R[2] + v[1] * R[5] + v[2] * R[8]);
        for (int i = 0; i < 3; ++i) g_cp_fwd[i] = -R[i * 3 + 2];
        const float need = g_cp_speed.load(std::memory_order_relaxed);
        float at[3];
        const bool at_ok = body_hand(ds.ctrl, at);
        if (!g_cp_back && g_cp_v <= -need) {
            if (g_cp_needs) {
                g_cp_back = true;
                controllers::pulse(ds.ctrl, 0.5f, 15);
                if (const GunSounds* gs = gun_sounds(g_cp_weapon); gs && at_ok) play_at(gs->open, at);
                log::info("[actions] pump (%d, carried): back by a jerk (%.2f m/s)", g_cp_weapon, -g_cp_v);
            } else if (!g_cp_ooo) {
                g_cp_ooo = true;  // jerked while chambered: nothing (counted once a stroke)
                g_out_of_order.fetch_add(1, std::memory_order_relaxed);
                log::info("[actions] pump (%d, carried): jerked while chambered: nothing", g_cp_weapon);
            }
        } else if (g_cp_back && g_cp_v >= need) {
            g_cp_back = g_cp_needs = false;
            g_cycles.fetch_add(1, std::memory_order_relaxed);
            g_cp_pumps.fetch_add(1, std::memory_order_relaxed);
            controllers::pulse(ds.ctrl, 0.5f, 15);
            if (const GunSounds* gs = gun_sounds(g_cp_weapon); gs && at_ok) play_at(gs->close, at);
            log::info("[actions] pump (%d, carried): forward by a jerk (%.2f m/s), chambered", g_cp_weapon, g_cp_v);
        } else if (g_cp_ooo && g_cp_v >= need) {
            g_cp_ooo = false;
        }
    }
    {  // the drawn fore-end (PumpParts): eased back or forward over 60 ms, as fast as the jerk
        static double last = 0;
        const float step = last > 0 ? static_cast<float>((now - last) / 60.0) : 1.0f;
        last = now;
        const bool pub = g_cp_active && ds.carry && ds.carry_weapon == g_cp_weapon && g_pump_parts.load(std::memory_order_relaxed);
        float a = g_cp_amt.load(std::memory_order_relaxed);
        const float target = pub && g_cp_back ? 1.0f : 0.0f;
        a = a < target ? std::fmin(target, a + step) : std::fmax(target, a - step);
        g_cp_amt.store(pub ? a : 0.0f, std::memory_order_relaxed);
        g_cp_pub.store(pub ? g_cp_weapon : -1, std::memory_order_relaxed);
    }
}

// [Hands] DualWieldOpen: the second gun shut and forgotten (no gun in hand, the manual actions off), its drawn opening at once
void second_reset(const char* why) {
    if (g_sec_open.exchange(false)) log::info("[actions] the second gun (%d): shut (%s)", g_sec_open_weapon.load(), why);
    g_sec_open_req = false;
    g_sec_open_amt.store(0.0f, std::memory_order_relaxed);
    g_sec_open_weapon.store(-1, std::memory_order_relaxed);
    g_sori_n = 0;
    g_sec_needs = false;  // (round 40) another gun, or none: no hammer to cock
    g_sec_cock_req = false;
    g_sec_hammer.store(1.0f, std::memory_order_relaxed);
    g_sec_hammer_weapon.store(-1, std::memory_order_relaxed);
}
// The frame end (under g_mutex): the second gun's opening by its stick's click (filter_second_stick), its shut by a flick
// of its own hand, Dead Eye's shut, its drawn opening eased
void second_frame(double now, bool deadeye) {
    const dual::State ds = dual::state();
    const Kind k = kind_of(ds.weapon);
    const bool rev = g_dual_open.load(std::memory_order_relaxed) && ds.on && !ds.carry && ds.ready && ds.ctrl >= 0 && ds.ctrl < 2 &&
                     sec_opens(k) && kind_on(k);
    const bool vlev = g_dual_open.load(std::memory_order_relaxed) && ds.on && !ds.carry && ds.ready && ds.ctrl >= 0 && ds.ctrl < 2 &&
                      sec_volcanic(ds.weapon);  // (F3: worked by its flick, never opened)
    const bool worked = rev || vlev;
    if (!ds.on) g_sec_refuse_said = false;
    if (!worked || ds.weapon != g_sec_open_weapon.load(std::memory_order_relaxed) || ds.ctrl != g_sec_open_ctrl.load(std::memory_order_relaxed) ||
        ds.copy != g_sec_open_copy.load(std::memory_order_relaxed)) {
        if (g_sec_open.load(std::memory_order_relaxed) || g_sec_open_weapon.load(std::memory_order_relaxed) >= 0)
            second_reset(!ds.on ? "put away" : !worked ? "not a revolver to open, carried, or the option off" : "another second gun");
        if (!worked) {
            g_sec_open_req = false;
            g_sec_shots_seen = dual::shots_fired(nullptr);  // (round 40) no shot before it is a revolver here needs a cock
            return;
        }
        g_sec_open_weapon.store(ds.weapon, std::memory_order_relaxed);
        g_sec_open_ctrl.store(ds.ctrl, std::memory_order_relaxed);
        g_sec_open_copy.store(ds.copy, std::memory_order_relaxed);
    }
    // that hand's angular velocity over a two-frame step (its controller's own frame: x right, y up, z back)
    if (g_sori_ctrl != ds.ctrl) {
        g_sori_ctrl = ds.ctrl;
        g_sori_n = 0;
    }
    const hands::Hand sh = hands::get(ds.ctrl);
    if (sh.valid) {
        g_sori[g_sori_n % 3] = {{sh.rot[0], sh.rot[1], sh.rot[2], sh.rot[3]}, now};
        ++g_sori_n;
        if (g_sori_n >= 3) {
            const OriSample& a = g_sori[(g_sori_n - 3) % 3];
            const OriSample& b = g_sori[(g_sori_n - 1) % 3];
            float wv[3];
            if (omega(a.q, b.q, b.ms - a.ms, wv)) std::memcpy(g_sw, wv, sizeof(g_sw));
        }
    }
    const char* kn = ds.copy ? "the copy" : "the second gun";
    if (deadeye) {  // Dead Eye fires the gun in hand: the second gun shut, a click meanwhile dropped
        g_sec_open_req = false;
        if (g_sec_open.exchange(false)) {
            g_sec_deadeye.fetch_add(1, std::memory_order_relaxed);
            log::info("[actions] %s (%d): shut by Dead Eye", kn, ds.weapon);
        }
    } else if (g_sec_open_req.exchange(false) && rev && !g_sec_open.load(std::memory_order_relaxed) && !menu::visible()) {
        g_sec_open = true;
        g_sec_open_ms = now;
        g_sec_close_hits = 0;
        g_sec_opens.fetch_add(1, std::memory_order_relaxed);
        controllers::pulse(ds.ctrl, 0.6f, 15);
        if (const GunSounds* gs = gun_sounds(ds.weapon)) {
            float at[3];
            if (body_hand(ds.ctrl, at)) play_at(gs->open, at);
        }
        log::info("[actions] %s (%s %d): opened by its stick, clip %.0f/%.0f (DualWieldOpen)", kn, kind_name(k), ds.weapon, ds.clip, ds.clip_max);
    }
    // (2026-10-31, S3) not while a hand holds rounds for it (r79 s2: the roll of holding it out to load shut it)
    if (g_sec_open.load(std::memory_order_relaxed) && now - g_sec_open_ms >= g_close_min_ms && !holster::rounds_for_second()) {
        // as the gun in hand's: roll about the barrel (z), yaw (y); the top-break also upward (pitch, x > 0)
        const float roll = std::fabs(g_sw[2]), yaw = std::fabs(g_sw[1]), up = g_sw[0];
        // (S4) a break-open shuts by its flick up alone (as the gun in hand's: hold down and click opens it, a flick up shuts it)
        const bool flick = k == kBreak ? up >= g_close_speed : roll >= g_close_speed || yaw >= g_close_speed || (k == kTopBreak && up >= g_close_speed);
        g_sec_close_hits = flick ? g_sec_close_hits + 1 : 0;
        if (g_sec_close_hits >= 2) {  // two consecutive two-frame steps
            g_sec_open = false;
            g_sec_closes.fetch_add(1, std::memory_order_relaxed);
            controllers::pulse(ds.ctrl, 0.7f, 20);
            shut_sounds(ds.weapon, ds.ctrl);
            log::info("[actions] %s (%d): closed by a flick of its hand (roll %.1f, yaw %.1f, pitch %.1f rad/s, open %.1f s), clip %.0f/%.0f", kn, ds.weapon,
                      g_sw[2], g_sw[1], g_sw[0], (now - g_sec_open_ms) * 0.001, ds.clip, ds.clip_max);
        }
    }
    {  // (round 40) RevolverControl=cock: the second gun's single action cocked by its stick's flick after each shot
        // (2026-11-03, the user: "Double action revolver when held as the second gun required cocking. It should just fire
        // repeatedly like it does as the main gun"; their log: the copy of the Cattleman in hand drawn as the Double-action)
        // the copy judged by the gun it shows (dual::copy_model), not by the gun in hand whose weapon object it fires
        const int cm = ds.copy ? dual::copy_model() : -1;
        const int cw = cm >= 0 ? cm : ds.weapon;
        const bool cocks = (single_action(cw) && g_rev_mode.load(std::memory_order_relaxed) == 1) || vlev;
        int sw = -1;
        const uint64_t fired = dual::shots_fired(&sw);
        if (fired != g_sec_shots_seen) {
            if (cocks && sw == ds.weapon && fired > g_sec_shots_seen && !deadeye) g_sec_needs = true;
            g_sec_shots_seen = fired;
        }
        if (!cocks) g_sec_needs = false;
        if (g_sec_cock_req.exchange(false)) {
            g_sec_flicks.fetch_add(1, std::memory_order_relaxed);
            if (cocks && g_sec_needs.load(std::memory_order_relaxed) && !g_sec_open.load(std::memory_order_relaxed) && !menu::visible()) {
                g_sec_needs = false;
                g_sec_cocks.fetch_add(1, std::memory_order_relaxed);
                if (!vlev && g_thumb_cfg.load(std::memory_order_relaxed)) {  // (C3) its thumb on the hammer, as ThumbCock's
                    g_sthumb_t0.store(now, std::memory_order_relaxed);
                    g_sthumb_ret.store(now + kThumbReachMs + kThumbSettleMs + kThumbRideMs, std::memory_order_relaxed);
                    g_sthumb_weapon.store(ds.weapon, std::memory_order_relaxed);
                }
                controllers::pulse(ds.ctrl, 0.5f, 15);
                float at[3];
                const GunSounds* vs = vlev ? gun_sounds(ds.weapon) : nullptr;  // (F3: the Volcanic's lever down and up)
                if (body_hand(ds.ctrl, at)) {
                    if (vs) {
                        play_at(vs->open, at);
                        play_at(vs->close, at);
                    } else if (!vlev) {  // (the review: the Volcanic without LeverParts silent, as in hand)
                        play_at("REVOLVER_COCK_FIRE_MASTER", at);
                    }
                }
                log::info("[actions] %s (%d): %s by its stick's flick, clip %.0f/%.0f", kn, ds.weapon, vlev ? "its lever worked" : "cocked", ds.clip, ds.clip_max);
            }
        }
        static double last = 0;
        const float step = last > 0 ? static_cast<float>((now - last) / 80.0) : 1.0f;
        last = now;
        const float target = cocks && g_sec_needs.load(std::memory_order_relaxed) ? 0.0f : 1.0f;
        float h = g_sec_hammer.load(std::memory_order_relaxed);
        h = h < target ? std::fmin(target, h + step) : std::fmax(target, h - step);
        g_sec_hammer.store(h, std::memory_order_relaxed);
        g_sec_hammer_weapon.store(cocks ? ds.weapon : -1, std::memory_order_relaxed);
    }
    {  // the drawn opening toward the state (a quarter second either way)
        static double last = 0;
        const float step = last > 0 ? static_cast<float>((now - last) * 0.004) : 1.0f;
        last = now;
        const float target = g_sec_open.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        float a = g_sec_open_amt.load(std::memory_order_relaxed);
        a = a < target ? (a + step > target ? target : a + step) : (a - step < target ? target : a - step);
        g_sec_open_amt.store(a, std::memory_order_relaxed);
    }
}

void frame() {
    const double now = log::now_ms();
    {  // [Hands] GunCollision (2026-10-29): two guns met at speed: the game's handling foley (its reloads' metal knock) at the contact
        float at[3];
        if (body::gun_collision() && body::collision_sound(at)) play_at("ARMOR_METAL_SHORT_MASTER", at);
    }
    RdrvrActorState st{};
    const bool on = g_on.load(std::memory_order_relaxed) && pose::anchor_active() && api::actor_state(&st) && st.actor;
    // (C7, the review: before the early return: beside the torch or the knife too; before g_mutex: the copy's drop takes
    // dual's lock, which never waits on this one)
    second_mag_frame(on, st);
    if (on && pistol_of(st.weapon))  // (C38: holster's lock before g_mutex)
        for (int h = 0; h < 2; ++h) holster::gun_adjust(st.weapon, holster::kAdjMag, g_mag_off[h], nullptr, nullptr, h);
    if (on && reload::is_gun(st.weapon))  // (C36, as above)
        for (int h = 0; h < 2; ++h) holster::gun_adjust(st.weapon, holster::kAdjPart, g_part_off[h], nullptr, nullptr, h);
    std::lock_guard lock(g_mutex);
    const int w = on && reload::is_gun(st.weapon) ? st.weapon : -1;
    if (!on || w < 0) {
        g_rod_ready.store(false, std::memory_order_relaxed);  // (C15, the review: no ramrod at the chest without the gun)
        g_rod_hand.store(-1, std::memory_order_relaxed);
        if (g_weapon != -1) reset_gun(-1, -1.0f);
        g_blocked = false;
        g_rt_rearm = false;
        g_bolt_want[0] = g_bolt_want[1] = false;
        g_open_req = false;
        g_lever_req = false;
        g_lever_lock.store(false, std::memory_order_relaxed);
        g_lever_front.store(false, std::memory_order_relaxed);
        g_lever_hold_lock.store(false, std::memory_order_relaxed);
        g_lever_weapon.store(-1, std::memory_order_relaxed);
        g_lever_amt.store(0.0f, std::memory_order_relaxed);
        g_pump_weapon.store(-1, std::memory_order_relaxed);
        g_pump_amt.store(0.0f, std::memory_order_relaxed);
        g_bolt_weapon.store(-1, std::memory_order_relaxed);
        g_bolt_lift.store(0.0f, std::memory_order_relaxed);
        g_bolt_slide.store(0.0f, std::memory_order_relaxed);
        g_breech_weapon.store(-1, std::memory_order_relaxed);
        g_breech_want[0] = g_breech_want[1] = false;
        g_semi_weapon.store(-1, std::memory_order_relaxed);
        g_semi_want[0] = g_semi_want[1] = false;
        g_pslide_weapon.store(-1, std::memory_order_relaxed);  // PistolSlides
        g_pslide.store(0.0f, std::memory_order_relaxed);
        g_pslide_want[0] = g_pslide_want[1] = false;
        g_mag_req = false;
        g_pwell_ok = false;
        g_barrel_want[0] = g_barrel_want[1] = false;
        g_break_open = false;
        g_open_amt.store(0.0f, std::memory_order_relaxed);
        g_open_weapon.store(-1, std::memory_order_relaxed);
        g_barrel_snap.store(0.0f, std::memory_order_relaxed);
        g_cyl_weapon.store(-1, std::memory_order_relaxed);
        g_cyl_drawn.store(0.0f, std::memory_order_relaxed);
        g_cock_req = false;
        g_rev_weapon.store(-1, std::memory_order_relaxed);
        g_rev_hammer.store(1.0f, std::memory_order_relaxed);
        g_thumb_weapon.store(-1, std::memory_order_relaxed);
        g_part_snap.store(0.0f, std::memory_order_relaxed);  // (no gun in hand: no part of it in the off hand)
        g_fan_pending = false;
        g_fan_low = 0;
        // [Hands] DualWieldBesideTorch (2026-10-29): the torch in hand (exactly it, the option on): the second gun's own
        // opening runs as beside a gun (its state apart from the gun in hand's, which there is none of); else, as before
        if (on && dual::beside_torch(st.weapon))
            second_frame(now, (st.weapon_flags & RDRVR_WEAPON_DEADEYE) != 0);
        else if (g_sec_open.load(std::memory_order_relaxed) || g_sec_open_weapon.load(std::memory_order_relaxed) >= 0)
            second_reset(!on ? "the manual actions off or not in first person" : "no gun in hand");  // [Hands] DualWieldOpen
        second_bolt_idle();  // (step 4, answer 2; the review) no gun in hand: the second gun's bolt not held nor drawn
        second_muzzle_idle();
        second_pistol_idle();
        body::set_drawn_second_bolt(false);
        carried_frame(now, on);  // [Reload] CarriedBolt: a bolt gun carried by the foregrip, its bolt by the gun hand
        // (the review) its take-back under way with the bolt not worked (or open): the pull held from the draw's first
        // frame, before this branch's next frame sees the gun in hand and reset_gun puts the bolt's state back
        if (g_cb_active && (g_cbs.needs || g_cbs.lift > 0.05f || g_cbs.slide > 0.0f) && (dual::carry_returning() || now - g_cb_ret_ms < 2000.0))
            g_blocked = true;
        carried_pump_frame(now, on);  // [Reload] CarriedPump: a Pump-action carried by the foregrip, pumped by that hand
        if (g_cp_active && (g_cp_needs || g_cp_back) && (dual::carry_returning() || now - g_cp_ret_ms < 2000.0)) g_blocked = true;
        carried_any_frame(now, on);  // [Reload] CarriedAnyState: the other actions carried as left
        carried_muzzle_frame(on);    // (C15h) the carried Blunderbuss rammed by the free gun hand
        if (g_gc_active && (g_gc.needs || g_gc.open || g_gc.breech_open || g_gc.lever_open || g_gc.semi_back) &&
            (dual::carry_returning() || now - g_gc_ret_ms < 2000.0))
            g_blocked = true;  // (as the bolt's: the take-back's pull held from the draw's first frame)
        return;
    }
    if (w != g_weapon) reset_gun(w, st.clip);
    carried_pump_frame(now, on);  // (a sidearm in the gun hand: the carrying hand still pumps the carried gun)
    carried_any_frame(now, on);
    g_gcr_hand.store(-1, std::memory_order_relaxed);  // (C15h: a gun in the gun hand: no carried ram; the review: the bare hand's too)
    g_gcr_pub.store(-1, std::memory_order_relaxed);
    g_gcr_ready.store(false, std::memory_order_relaxed);
    g_gcr_rod.store(0.0f, std::memory_order_relaxed);
    g_gcr_grip = g_gcr_down = false;
    g_gcr_on = false;
    second_bolt_frame(on, w);  // (step 4, answer 2) a long bolt second gun's own bolt, by the gun hand's trigger
    second_muzzle_frame(on);   // (step 8) the Blunderbuss as the second gun
    second_pistol_frame(on);   // (step 4, S4) a pistol as the second gun: its slide
    body::set_drawn_second_bolt(g_sb_active || g_smz_active || g_spl_active);  // (its frame for any: one call)
    cpart_begin();  // (a gun in hand: no carried bolt in the gun hand)
    g_cpart_snap.store(0.0f, std::memory_order_relaxed);
    cpart_end();
    g_cb_ran = false;  // (the review: carried_hint's zone only as stepped this frame; with a sidearm in hand none is)
    // [Reload] CarriedBolt: a gun in the gun hand (a sidearm drawn while carrying): the carried bolt kept as left; dropped
    // once the carry is over (not taken back)
    if (g_cb_active && !dual::state().carry && now - g_cb_ret_ms >= 2000.0) {
        g_cb_active = false;
        g_cb_pub.store(-1, std::memory_order_relaxed);
        log::info("[actions] bolt (%d, carried): the carry ended (not taken back): its bolt's state dropped", g_cb_weapon);
    }
    const Kind k = kind_on(g_kind) ? g_kind : kNone;
    if (k == kNone && (g_needs || g_open)) {  // 1: its kind's switch turned off with a shot pending or the gun open
        g_needs = g_open = false;
        g_breech_open = g_breech_grip = false;
        g_semi_back = g_semi_grip = false;
        log::info("[actions] weapon %d: its manual action off: nothing held", w);
    }
    const bool deadeye = (st.weapon_flags & RDRVR_WEAPON_DEADEYE) != 0;
    const int gun_h = controls::gun_hand(), off_h = 1 - gun_h;
    const hands::Hand gh = hands::get(gun_h);
    // the gun hand's angular velocity over a two-frame step
    if (gh.valid) {
        g_ori[g_ori_n % 3] = {{gh.rot[0], gh.rot[1], gh.rot[2], gh.rot[3]}, now};
        ++g_ori_n;
        if (g_ori_n >= 3) {
            const OriSample& a = g_ori[(g_ori_n - 3) % 3];
            const OriSample& b = g_ori[(g_ori_n - 1) % 3];
            float wv[3];
            if (omega(a.q, b.q, b.ms - a.ms, wv)) {
                std::memcpy(g_w, wv, sizeof(g_w));
                for (int i = 0; i < 3; ++i)
                    if (std::fabs(wv[i]) > std::fabs(g_w_peak[i])) g_w_peak[i] = wv[i];
            }
        }
    }
    // a shot (the game's own count dropped, not reloading): a bolt, lever or pump gun needs cycling while rounds are left
    const bool reloading = (st.weapon_flags & RDRVR_WEAPON_RELOADING) != 0;
    // PistolMagazines / PistolSlides: the count the mod's own natives leave (a magazine dropped, a round ejected, a
    // magazine or a clip in) seen: a drop to it is no shot
    const bool pk = k == kPistol || k == kMauser;
    bool ours_drop = false;
    if (g_expect_until > 0.0) {
        if (std::fabs(st.clip - static_cast<float>(g_expect_clip)) < 0.5f) {
            ours_drop = g_last_clip >= 0.0f && st.clip < g_last_clip - 0.5f;
            g_expect_until = 0.0;
        } else if (now > g_expect_until) {
            log::info("[actions] weapon %d: the count %d the mod's natives were to leave not seen in time (clip %.0f)", w, g_expect_clip, st.clip);
            g_expect_until = 0.0;
        }
    }
    if (g_last_clip >= 0.0f && st.clip < g_last_clip - 0.5f && !reloading && !ours_drop) {
        g_shots_seen.fetch_add(1, std::memory_order_relaxed);
        g_fan_pending = false;  // the fanned shot taken (aim's count says so first, as a rule)
        g_last_shot_ms.store(now, std::memory_order_relaxed);
        g_hammer_down = true;
        if ((k == kBolt || k == kLever || k == kPump) && st.clip >= 0.5f && !deadeye) {
            g_needs = true;
            log::info("[actions] %s (%d): shot, the %s needs working (clip %.0f/%.0f)", kind_name(k), w,
                      k == kBolt ? "bolt" : k == kLever ? "lever" : "pump", st.clip, st.clip_max);
        }
        if (now < g_thumb_ret.load(std::memory_order_relaxed)) g_thumb_ret.store(now, std::memory_order_relaxed);  // ThumbCock: off the hammer
        if (opens(k) && k != kBreak && g_rev_mode.load(std::memory_order_relaxed) == 1 && single_action(w) && !deadeye) {
            g_needs = true;  // RevolverControl=cock: the hammer down, even with the cylinder empty
            log::info("[actions] %s (%d): shot, the hammer down: a flick of the stick cocks it (clip %.0f/%.0f)", kind_name(k), w, st.clip, st.clip_max);
        }
        if (k == kMuzzle && !deadeye) {  // (step 8, R14) the Blunderbuss: empty; a round at the muzzle, then rammed
            g_needs = true;
            g_muz_round = false;
            g_muz_strokes = 0;
            g_muz_down = false;
            log::info("[actions] muzzle-loader (%d): shot, a round in at the loading point and rammed (clip %.0f/%.0f, spare %.0f)", w, st.clip, st.clip_max, st.spare);
        }
        if (k == kBreech && !deadeye) {  // a single shot: the spent case in the chamber, whatever the game's count
            g_needs = true;
            g_breech_round.store(false, std::memory_order_relaxed);
            log::info("[actions] breech (%d): shot, open it and put a round in (clip %.0f/%.0f, spare %.0f)", w, st.clip, st.clip_max, st.spare);
        }
        if (pk && !deadeye) {  // PistolSlides: the slide's own cycle drawn, the next round chambered; the last locks it back
            const bool sl = g_slides.load(std::memory_order_relaxed);
            const bool fed = g_mag_in.load(std::memory_order_relaxed) || k == kMauser;  // (no magazine: nothing holds it back)
            if (sl || (k == kMauser && g_mags.load(std::memory_order_relaxed))) {
                g_pblow_ms = now;
                if (st.clip < 0.5f && fed && !g_locked) {
                    g_locked = true;
                    g_locks.fetch_add(1, std::memory_order_relaxed);
                    log::info("[actions] %s (%d): the last round fired, the %s held back on its stop (spare %.0f)", kind_name(k), w,
                              k == kMauser ? "bolt" : "slide", st.spare);
                }
            }
            if (sl) g_chambered = st.clip >= 0.5f && fed;
            if (sl || (k == kMauser && g_mags.load(std::memory_order_relaxed))) {  // (its fire clip stopped: parts_weapon)
                float at[3];
                if (const PistolDef* pd = pistol_of(w); pd && body_hand(gun_h, at)) play_at(pd->shot, at);  // (the stopped fire clip's)
            }
        }
    }
    if (pk && g_last_clip >= 0.0f && st.clip > g_last_clip + 0.5f && !reloading_by_hand_ms(now)) {
        // the count rose by none of the player's (the game's own reload, Dead Eye's refill): loaded and chambered, the
        // game's way. (The review) also while one of the mod's counts is awaited (a drop's): the game's load supersedes it
        if (g_expect_until > 0.0 || g_lose_pending > 0)
            log::info("[actions] %s (%d): the game loaded it while the count %d was awaited: the game's count kept", kind_name(k), w, g_expect_clip);
        g_expect_until = 0.0;
        g_lose_pending = 0;
        g_chambered = true;
        g_locked = false;
        g_pdrawn = 0.0f;
        g_mag_in.store(true, std::memory_order_relaxed);
        if (w >= 1 && w <= 3) g_mag_out[w] = false;
        g_clip_seated = false;
        log::info("[actions] %s (%d): loaded by the game (clip %.0f -> %.0f): chambered", kind_name(k), w, g_last_clip, st.clip);
    }
    if (k == kMuzzle && g_needs && !g_muz_round && g_last_clip >= 0.0f && st.clip > g_last_clip + 0.5f) {
        // (step 8) a round down the barrel (by hand, or the game's): rammed next
        g_muz_round = true;
        g_muz_strokes = 0;
        g_muz_down = false;
        log::info("[actions] muzzle-loader (%d): a round down the barrel (clip %.0f -> %.0f): ram it (%d strokes)", w, g_last_clip, st.clip, kRamStrokes);
    }
    if (k == kBreech && g_last_clip >= 0.0f && st.clip > g_last_clip + 0.5f && !reloading_by_hand_ms(now)) {
        // 2: the clip rose without a round by hand: the game's own reload. A round in; with the breech shut, ready
        g_breech_round.store(true, std::memory_order_relaxed);
        if (!g_breech_open && g_needs) {
            g_needs = false;
            log::info("[actions] breech (%d): reloaded by the game (clip %.0f -> %.0f): ready", w, g_last_clip, st.clip);
        }
    }
    if (st.clip < 0.5f) g_was_empty = true;
    if (g_was_empty && st.clip >= 0.5f) {  // loaded from empty: a bolt, lever or pump gun (or the semi-auto) chambers first
        g_was_empty = false;
        if ((k == kBolt || k == kLever || k == kPump || k == kSemi) && g_chamber_load.load(std::memory_order_relaxed)) {
            g_needs = true;
            log::info("[actions] %s (%d): loaded from empty, the action needs working", kind_name(k), w);
        }
    }
    g_last_clip = st.clip;
    g_last_clip_max = st.clip_max;
    if (deadeye) {  // Dead Eye fires its marks: nothing held, everything ready after it
        g_needs = false;
        g_open = false;
        g_lever_open = g_hammer_down = false;
        if (pk) {  // (a pistol: chambered on whatever it holds, the slide forward; the Mauser's clip gone with its bolt shut)
            g_chambered = st.clip >= 0.5f;
            g_locked = false;
            g_clip_seated = false;
            g_clip_left = 0;
        }
    }
    // (step 8, the review) the muzzle-loader empty however it got so (drawn empty, a Dead Eye shot, a load cut short): a
    // round wanted
    if (k == kMuzzle && !deadeye && !g_needs && st.clip >= 0.0f && st.clip < 0.5f) {
        g_needs = true;
        g_muz_round = false;
        g_muz_strokes = 0;
        g_muz_down = false;
    }
    // revolvers (and the break action): open by the stick flick (filter_stick), close by a flick of the gun hand
    if (opens(k) && !deadeye) {
        // run 7 item 5: a flick while the menu is open is the menu's (its stick scrolls the page): taken, nothing opened
        if (g_open_req.exchange(false) && !g_open && !menu::visible()) {
            g_open = true;
            g_open_ms = now;
            g_close_hits = 0;
            // the off hand on the foregrip through the opening: it may take the barrels without a fresh grip
            g_barrel_take = k == kBreak && hands::get(off_h).grip > 0.6f;
            g_opens.fetch_add(1, std::memory_order_relaxed);
            controllers::pulse(gun_h, 0.6f, 15);
            if (const GunSounds* gs = gun_sounds(w); gs && gh.valid) {
                float at[3];
                if (body_hand(gun_h, at)) play_at(gs->open, at);
            }
            log::info("[actions] %s (%d): opened by the stick, clip %.0f/%.0f, spare %.0f", kind_name(k), w, st.clip, st.clip_max, st.spare);
        }
        // [Reload] RevolverControl=cock: the stick's flick cocks a single action's hammer after its shot: the cylinder
        // turned a chamber (drawn), the game's own cocking sound at the hand. Shut only (open, the flick does nothing)
        if (g_cock_req.exchange(false) && k != kBreak && g_rev_mode.load(std::memory_order_relaxed) == 1 && single_action(w) && g_needs && !g_open &&
            !menu::visible()) {
            g_needs = false;
            if (w >= 0 && w < 40) g_cyl_rounds[w].fetch_add(1, std::memory_order_relaxed);
            g_cocked_n.fetch_add(1, std::memory_order_relaxed);
            dual::quick_shot(w);  // cocked by hand: its shot at once, not at the game's rate
            controllers::pulse(gun_h, 0.5f, 15);
            if (gh.valid) {
                float at[3];
                if (body_hand(gun_h, at)) play_at("REVOLVER_COCK_FIRE_MASTER", at);
            }
            if (g_thumb_cfg.load(std::memory_order_relaxed)) {  // [Reload] ThumbCock: John's thumb draws the hammer back
                g_thumb_t0.store(now, std::memory_order_relaxed);
                g_thumb_ret.store(now + kThumbReachMs + kThumbSettleMs + kThumbRideMs, std::memory_order_relaxed);
                g_thumb_weapon.store(w, std::memory_order_relaxed);
            }
            log::info("[actions] %s (%d): cocked by the stick's flick, clip %.0f/%.0f%s", kind_name(k), w, st.clip, st.clip_max,
                      g_thumb_cfg.load(std::memory_order_relaxed) ? " (the thumb on the hammer)" : "");
        }
        if (g_open && now - g_open_ms >= g_close_min_ms) {
            // roll about the barrel (the controller's z), yaw (y); the top-break also closes upward (pitch, x > 0);
            // the break action only upward, and not while the off hand has its barrels
            const float roll = std::fabs(g_w[2]), yaw = std::fabs(g_w[1]), up = g_w[0];
            const bool flick = k == kBreak ? up >= g_close_speed && !g_barrel_hold
                                           : roll >= g_close_speed || yaw >= g_close_speed || (k == kTopBreak && up >= g_close_speed);
            g_close_hits = flick ? g_close_hits + 1 : 0;
            if (g_close_hits >= 2) {  // two consecutive two-frame steps (20 ms or more)
                g_open = false;
                g_closes.fetch_add(1, std::memory_order_relaxed);
                controllers::pulse(gun_h, 0.7f, 20);
                shut_sounds(w, gun_h);
                log::info("[actions] %s (%d): closed by a flick (roll %.1f, yaw %.1f, pitch %.1f rad/s, open %.1f s), clip %.0f/%.0f", kind_name(k), w,
                          g_w[2], g_w[1], g_w[0], (now - g_open_ms) * 0.001, st.clip, st.clip_max);
            }
        }
    } else {
        g_open_req = false;
        g_open = false;
    }
    if (!opens(k) || k == kBreak || deadeye) g_cock_req = false;
    second_frame(now, deadeye);  // [Hands] DualWieldOpen: the second gun's own opening (its state apart from the gun in hand's)
    {  // RevolverControl=cock: the single action's hammer as drawn, down after the shot until cocked, 80 ms either way
       // (ThumbCock: cocked by the stick, held down until the thumb is on its spur, then back with the thumb)
        static double last = 0;
        const bool cocks = (k == kRevolver || k == kTopBreak) && g_rev_mode.load(std::memory_order_relaxed) == 1 && single_action(w) && !deadeye;
        const double tt = now - g_thumb_t0.load(std::memory_order_relaxed);
        const bool thumb = cocks && g_thumb_weapon.load(std::memory_order_relaxed) == w && tt >= 0.0 && tt < kThumbReachMs + kThumbSettleMs + kThumbRideMs &&
                           now < g_thumb_ret.load(std::memory_order_relaxed);
        // (with the thumb: back 15 ms before the ride ends, so the thumb holds it cocked a moment)
        const float step = last > 0 ? static_cast<float>((now - last) / (thumb ? kThumbRideMs - 15.0 : 80.0)) : 1.0f;
        last = now;
        const float target = cocks && (g_needs || (thumb && tt < kThumbReachMs + kThumbSettleMs)) ? 0.0f : 1.0f;
        float a = g_rev_hammer.load(std::memory_order_relaxed);
        a = a < target ? (a + step > target ? target : a + step) : (a - step < target ? target : a - step);
        g_rev_hammer.store(a, std::memory_order_relaxed);
        g_rev_weapon.store(cocks ? w : -1, std::memory_order_relaxed);
    }
    {  // [Reload] OpenCylinder: the drawn opening eased toward the state (a quarter second either way)
        static double last = 0;
        const double t = log::now_ms();
        const float step = last > 0 ? static_cast<float>((t - last) * 0.004) : 1.0f;
        last = t;
        const float target = g_open && opens(k) ? 1.0f : 0.0f;
        float a = g_open_amt.load(std::memory_order_relaxed);
        a = a < target ? (a + step > target ? target : a + step) : (a - step < target ? target : a - step);
        if (g_barrel_hold && g_open) a = g_barrel_amt.load(std::memory_order_relaxed);  // the barrels where the hand has them
        g_open_amt.store(a, std::memory_order_relaxed);
        g_open_weapon.store(w, std::memory_order_relaxed);
        // the cylinder's index toward the rounds put in (a chamber in 0.15 s; another gun: at once)
        const float want = w >= 0 && w < 40 ? static_cast<float>(g_cyl_rounds[w].load(std::memory_order_relaxed)) : 0.0f;
        float ci = g_cyl_drawn.load(std::memory_order_relaxed);
        if (g_cyl_weapon.exchange(w, std::memory_order_relaxed) != w) ci = want;
        const float cs = step * (0.25f / 0.15f);  // step is a quarter second's fraction: a chamber in 0.15 s
        ci = ci < want ? (ci + cs > want ? want : ci + cs) : (ci - cs < want ? want : ci - cs);
        g_cyl_drawn.store(ci, std::memory_order_relaxed);
    }
    // the body's points: the gun hand's wrist target and its axes, the barrel's direction, the off hand
    body::BodyPoints bp;
    const bool pts = body::body_points(&bp);
    float u[3] = {0, 0, -1}, rgt[3] = {1, 0, 0}, up[3] = {0, 1, 0};
    int gj = 1, oj = 0;
    float ogb[3] = {};  // [Hands] InteractOffset: the off hand's interaction spot (what grips the parts)
    if (pts) {
        gj = bp.gun;
        oj = bp.ctrl[0] == off_h ? 0 : 1;
        holster::grab_point(bp, oj, ogb);
        float bt[3] = {0.0f, 0.174f, -0.985f};
        aim::barrel_in_target(bt);
        const float* R = bp.target_rot[gj];  // columns: the target's axes
        for (int i = 0; i < 3; ++i) {
            u[i] = R[i * 3] * bt[0] + R[i * 3 + 1] * bt[1] + R[i * 3 + 2] * bt[2];
            rgt[i] = R[i * 3];
        }
        // up = right x forward (the target frame's, square to the barrel)
        up[0] = rgt[1] * u[2] - rgt[2] * u[1];
        up[1] = rgt[2] * u[0] - rgt[0] * u[2];
        up[2] = rgt[0] * u[1] - rgt[1] * u[0];
        const float ul = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
        if (ul > 1e-4f)
            for (float& x : up) x /= ul;
    }
    // [Reload] FanHammer: the other hand's sweep across a single action's hammer, with RevolverControl=cock
    {
        // (the review) an open, tracked hand only: not gripping (a round, a holster, a part), not the round's trigger
        const hands::Hand oh_now = hands::get(off_h);
        const bool fan_on = g_fan.load(std::memory_order_relaxed) && (k == kRevolver || k == kTopBreak) && single_action(w) &&
                            g_rev_mode.load(std::memory_order_relaxed) == 1 && pts && bp.hand_ok[gj] && bp.hand_ok[oj] && oh_now.valid &&
                            oh_now.grip < 0.5f && !holster::grip_consumed(off_h) && !holster::trigger_for_round(off_h) && !deadeye && !g_open &&
                            !(dual::state().on && dual::state().ctrl == off_h);
        if (fan_on) {
            const float* ho = hammer_origin(w);
            // (2026-10-27, [Hands] GunKick; the user: "Look into the recoil optimumbox added") the kick is put on the drawn gun
            // only (body.cpp's kick_draw): the gun frame published here is the gun as held, so the zone does not ride the kick
            // and the sweep's speed is measured against the gun in the hand, never the bucking one; nor does the aim move
            float hz0[3] = {ho[0] + g_fan_off[0], ho[1] + g_fan_off[1], ho[2] - g_fan_off[2]}, hz[3];  // (forward is -z in the model)
            part_hz(hz0, gj, hz);  // (C36)
            for (int i = 0; i < 3; ++i)
                g_fan_zone[i] = bp.gun_frame_ok ? bp.gun_frame_o[i] + bp.gun_frame_R[i * 3] * hz[0] + bp.gun_frame_R[i * 3 + 1] * hz[1] +
                                                      bp.gun_frame_R[i * 3 + 2] * hz[2]
                                                : bp.hand[gj][i] + u[i] * (0.08f - hz[2]) + rgt[i] * hz[0] + up[i] * hz[1];
            const float rel[3] = {ogb[0] - g_fan_zone[0], ogb[1] - g_fan_zone[1], ogb[2] - g_fan_zone[2]};  // the hand against the hammer
            g_fan_d = std::sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
            g_fan_frame = bp.gun_frame_ok;
            if (bp.ms == g_fan_prev_ms) goto fan_done;  // the same body points as last frame: nothing new
            {
                float v[3] = {0, 0, 0};
                const double dt = (bp.ms - g_fan_prev_ms) * 0.001;
                const bool have = g_fan_prev_ms > 0 && dt > 0.002 && dt < 0.1;
                if (have)
                    for (int i = 0; i < 3; ++i) v[i] = static_cast<float>((rel[i] - g_fan_prev[i]) / dt);  // relative to the gun
                g_fan_v = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                g_fan_back = -(v[0] * u[0] + v[1] * u[1] + v[2] * u[2]);  // back along the gun (toward the shooter)
                // the path since the last points against the zone (a fast sweep crosses it between two frames)
                float dmin = g_fan_d;
                if (have) {
                    float s[3], ss = 0.0f, t = 0.0f;
                    for (int i = 0; i < 3; ++i) {
                        s[i] = rel[i] - g_fan_prev[i];
                        ss += s[i] * s[i];
                    }
                    if (ss > 1e-8f) t = -(g_fan_prev[0] * s[0] + g_fan_prev[1] * s[1] + g_fan_prev[2] * s[2]) / ss;
                    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
                    const float c[3] = {g_fan_prev[0] + s[0] * t, g_fan_prev[1] + s[1] * t, g_fan_prev[2] + s[2] * t};
                    dmin = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
                }
                if (g_fan_d > g_fan_r * 1.3f && dmin > g_fan_r && now - g_fan_last_ms > 200.0) g_fan_armed = true;
                if (have && g_fan_armed && dmin < g_fan_r && g_fan_v >= g_fan_speed && g_fan_back >= 0.5f * g_fan_v && !menu::visible()) {
                g_fan_armed = false;
                g_fan_last_ms = now;
                g_fans.fetch_add(1, std::memory_order_relaxed);
                controllers::pulse(off_h, 0.5f, 15);
                if (!g_needs)
                    log::info("[actions] %s (%d): a sweep with the hammer back already (%.2f m/s, %.2f back; %.3f m): nothing", kind_name(k), w, g_fan_v,
                              g_fan_back, dmin);
                if (g_needs) {  // the hammer down: cocked by the sweep (the cylinder a chamber, the cock sound at the hammer)
                    g_needs = false;
                    if (w >= 0 && w < 40) g_cyl_rounds[w].fetch_add(1, std::memory_order_relaxed);
                    g_cocked_n.fetch_add(1, std::memory_order_relaxed);
                    dual::quick_shot(w);  // fanned: its shot at once, not at the game's rate
                    play_at("REVOLVER_COCK_FIRE_MASTER", g_fan_zone);
                    // fanning: the gun's trigger held, its pull reaches the game now as a fresh one (the shot at once)
                    const bool held = gh.trigger > 0.6f && !whistle::suppressed(gun_h);
                    if (held) {
                        g_rt_rearm = false;
                        g_fan_shots.fetch_add(1, std::memory_order_relaxed);
                        g_fan_pending = st.clip >= 0.5f;  // (an empty gun: no shot to wait for)
                        g_fan_pend_shots = aim::player_shots();
                        g_fan_pend_ms = now;
                        g_fan_retry_ms = now + 20.0;
                    }
                    log::info("[actions] %s (%d): cocked by the other hand's sweep (%.2f m/s, %.2f back; %.3f m from the hammer)%s", kind_name(k), w, g_fan_v,
                              g_fan_back, dmin, held ? ": the trigger held, its pull passed on" : "");
                }
                }
            }
            std::memcpy(g_fan_prev, rel, sizeof(g_fan_prev));
            g_fan_prev_ms = bp.ms;
        fan_done:;
        } else {
            g_fan_prev_ms = 0;
            g_fan_armed = true;
            g_fan_d = 1e9f;
        }
    }
    // [Hands] GunCollision: the other gun's body swept back across the single action's hammer
    {
        body::CollGuns cg;
        const bool gc_on = body::gun_collision() && (k == kRevolver || k == kTopBreak) && single_action(w) && g_rev_mode.load(std::memory_order_relaxed) == 1 &&
                           !deadeye && !g_open && body::collision_guns(&cg) && cg.gun[0].ok && cg.gun[0].spur_ok && cg.gun[0].weapon == w;
        const int bk = gc_on ? (cg.gun[1].ok ? 1 : cg.gun[2].ok ? 2 : -1) : -1;
        if (bk > 0 && cg.gun[bk].r > 0.0f) {
            const body::CollGun& A = cg.gun[0];
            const body::CollGun& G = cg.gun[bk];
            if (cg.ms != g_gc_prev_ms) {
                float e[2][3];  // its body's ends in A's frame (A.R's columns its axes: local = R^T (p - o))
                for (int j = 0; j < 2; ++j) {
                    const float* l = j ? G.b : G.a;
                    float p[3];
                    for (int i = 0; i < 3; ++i) p[i] = G.o[i] + G.R[i * 3] * l[0] + G.R[i * 3 + 1] * l[1] + G.R[i * 3 + 2] * l[2] - A.o[i];
                    for (int i = 0; i < 3; ++i) e[j][i] = A.R[i] * p[0] + A.R[3 + i] * p[1] + A.R[6 + i] * p[2];
                }
                float tb = 0.0f;
                g_gc_d = point_seg(A.spur, e[0], e[1], &tb) - G.r;
                const double dt = (cg.ms - g_gc_prev_ms) * 0.001;
                bool have = g_gc_prev_ms > 0 && g_gc_prev_kind == bk && dt > 0.002 && dt < 0.1;
                float dmin = g_gc_d, v[3] = {0.0f, 0.0f, 0.0f};
                if (have) {  // the path since the last guns (a fast sweep crosses the spur between two): the body lerped, 8 steps
                    for (int s = 0; s < 8; ++s) {
                        const float su = s / 8.0f;
                        float a[3], b[3], t = 0.0f;
                        for (int i = 0; i < 3; ++i) {
                            a[i] = g_gc_prev[0][i] + (e[0][i] - g_gc_prev[0][i]) * su;
                            b[i] = g_gc_prev[1][i] + (e[1][i] - g_gc_prev[1][i]) * su;
                        }
                        const float d = point_seg(A.spur, a, b, &t) - G.r;
                        if (d < dmin) dmin = d, tb = t;
                    }
                    float step2 = 0.0f;
                    for (int i = 0; i < 3; ++i) {  // its point nearest the spur, relative to the gun
                        const float pn = e[0][i] + (e[1][i] - e[0][i]) * tb, pp = g_gc_prev[0][i] + (g_gc_prev[1][i] - g_gc_prev[0][i]) * tb;
                        v[i] = static_cast<float>((pn - pp) / dt);
                        step2 += (pn - pp) * (pn - pp);
                    }
                    // (the review) a jump of the guns' frames (a build skipped, a teleport, a re-anchor), not a hand's sweep: the step
                    // taken as no path (nothing cocks on it)
                    if (step2 > kGunCockJump * kGunCockJump || v[0] * v[0] + v[1] * v[1] + v[2] * v[2] > kGunCockMaxV * kGunCockMaxV) {
                        have = false;
                        dmin = g_gc_d;
                        v[0] = v[1] = v[2] = 0.0f;
                        g_gc_jumps.fetch_add(1, std::memory_order_relaxed);
                        log::limited("actions.gc.jump", 10, "[actions] %s (%d): the other gun jumped %.3f m in a frame: not a sweep", kind_name(k), w,
                                     std::sqrt(step2));
                    }
                }
                g_gc_v = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                g_gc_back = v[2];  // (+z: back along the gun, toward the shooter)
                if (g_gc_d > 2.0f * kGunCockTouch && dmin > 2.0f * kGunCockTouch) {
                    g_gc_in = g_gc_nb_said = false;  // (out of the zone: a later visit counts its own "not back")
                    if (now - g_gc_last_ms > 200.0) g_gc_armed = true;
                }
                if (have && dmin < kGunCockTouch) {  // in the zone this frame: its visit's fastest
                    if (!g_gc_in) g_gc_vis_v = g_gc_vis_back = 0.0f;
                    g_gc_in = true;
                    g_gc_vis_v = std::fmax(g_gc_vis_v, g_gc_v);
                    g_gc_vis_back = std::fmax(g_gc_vis_back, g_gc_back);
                }
                // (the review) disarmed only by a sweep that passes, as the fan: one resting on the hammer, or still speeding up as it
                // comes in, cocks once it is fast and back enough; a wrong one counted once a visit, nothing more
                const bool fast_back = g_gc_v >= g_fan_speed && g_gc_back >= 0.5f * g_gc_v;
                if (have && g_gc_armed && dmin < kGunCockTouch && !menu::visible() && !fast_back && !g_gc_nb_said) {
                    g_gc_nb_said = true;
                    g_gc_notback.fetch_add(1, std::memory_order_relaxed);
                    log::info("[actions] %s (%d): the other gun across the hammer, not a sweep back (%.2f m/s, %.2f back; %.3f m): nothing", kind_name(k), w,
                              g_gc_v, g_gc_back, dmin);
                }
                if (have && g_gc_armed && dmin < kGunCockTouch && !menu::visible() && fast_back) {
                    g_gc_armed = false;
                    g_gc_last_ms = now;
                    g_gc_sweeps.fetch_add(1, std::memory_order_relaxed);
                    float at[3];
                    for (int i = 0; i < 3; ++i) at[i] = A.o[i] + A.R[i * 3] * A.spur[0] + A.R[i * 3 + 1] * A.spur[1] + A.R[i * 3 + 2] * A.spur[2];
                    if (!g_needs) {
                        g_gc_already.fetch_add(1, std::memory_order_relaxed);
                        log::info("[actions] %s (%d): the other gun's sweep with the hammer back already (%.2f m/s, %.2f back; %.3f m): nothing", kind_name(k), w,
                                  g_gc_v, g_gc_back, dmin);
                    } else {  // the hammer down: cocked, as the stick's flick (the cylinder a chamber, the cock sound at the hammer)
                        g_needs = false;
                        if (w >= 0 && w < 40) g_cyl_rounds[w].fetch_add(1, std::memory_order_relaxed);
                        g_cocked_n.fetch_add(1, std::memory_order_relaxed);
                        g_gc_cocks.fetch_add(1, std::memory_order_relaxed);
                        dual::quick_shot(w);  // cocked by hand: its shot at once, not at the game's rate
                        controllers::pulse(gun_h, 0.5f, 15);
                        const int oc = pts && G.john >= 0 && G.john <= 1 ? bp.ctrl[G.john] : -1;  // (that gun's controller)
                        if (oc >= 0 && oc <= 1 && oc != gun_h) controllers::pulse(oc, 0.5f, 15);
                        play_at("REVOLVER_COCK_FIRE_MASTER", at);
                        log::info("[actions] %s (%d): cocked by the other gun's sweep (%.2f m/s, %.2f back; %.3f m from the spur), clip %.0f/%.0f", kind_name(k), w,
                                  g_gc_v, g_gc_back, dmin, st.clip, st.clip_max);
                    }
                }
                std::memcpy(g_gc_prev, e, sizeof(g_gc_prev));
                g_gc_prev_ms = cg.ms;
                g_gc_prev_kind = bk;
            }
        } else {
            g_gc_prev_ms = 0.0;
            g_gc_armed = true;
            g_gc_d = 1e9f;
            g_gc_in = g_gc_nb_said = false;
        }
    }
    // bolt actions: the other hand grips at the bolt, pulls it back, then forward ([Reload] BoltParts: the handle turned
    // up by the hand's rise first and down again after; the bolt drawn where the hand has it)
    g_bolt_want[0] = g_bolt_want[1] = false;
    const bool bparts = k == kBolt && g_bolt_parts.load(std::memory_order_relaxed);
    float blift = g_bolt_lift.load(std::memory_order_relaxed), bslide = g_bolt_slide.load(std::memory_order_relaxed);  // as left
    if (k == kBolt && pts && bp.hand_ok[gj] && bp.hand_ok[oj] && !deadeye) {
        float z0[3];  // the zone: the bolt's handle shut, from the gun hand's wrist target ([Reload] BoltZoneOffset)
        const float* po = g_part_off[gj];  // (C36) the gun's own spot moved too
        for (int i = 0; i < 3; ++i)
            z0[i] = bp.hand[gj][i] + rgt[i] * (g_bolt_off[0] + po[0]) + up[i] * (g_bolt_off[1] + po[1]) + u[i] * (g_bolt_off[2] + po[2]);
        // (2026-11-01, step 7, R9; 51.3, the user: "other bolt guns need adjusting") BoltZoneOffset is the Bolt Action's: the
        // Carcano's knob is elsewhere on its gun (r88 K1: its bolt's tip (0.0405 0.1006 -0.0249) against the Bolt Action's
        // (0.0283 0.0469 0.0024), the gun's frame), and the gun hand holds it otherwise (the game's grip: r313 B21 K2, the
        // zone less the knob 3.1 cm from the Bolt Action's with the knobs' difference alone, (-0.0014 0.0176 -0.0253)): its
        // zone moved by the knobs' difference less that (its learned carried zone too)
        static constexpr float kZoneShift20[3] = {0.0139f, 0.0360f, -0.0020f};
        if (w == 20 && bp.gun_frame_ok)
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) z0[i] += bp.gun_frame_R[i * 3 + j] * kZoneShift20[j];
        g_bolt_z0_gun_ok = bp.gun_frame_ok;
        for (int j = 0; j < 3 && bp.gun_frame_ok; ++j) {
            g_bolt_z0_gun[j] = 0.0f;
            for (int i = 0; i < 3; ++i) g_bolt_z0_gun[j] += bp.gun_frame_R[i * 3 + j] * (z0[i] - bp.gun_frame_o[i]);
        }
        // [Reload] CarriedBolt: that zone in the drawn gun's own frame, learned for the carry: held in the gun hand alone (the
        // two-handed hold turns the drawn gun off the gun hand's target), not through John's fire clip (the drawn gun is
        // steadied then), nor while the bolt is gripped
        const int zs = bolt_slot(w);
        if (zs >= 0 && bp.gun_frame_ok && !g_bolt_grip && since_shot_ms() > 2000.0 && holster::two_hand_blend() < 0.05f) {
            for (int j = 0; j < 3; ++j) {
                float v = 0.0f;
                for (int i = 0; i < 3; ++i) v += bp.gun_frame_R[i * 3 + j] * (z0[i] - bp.gun_frame_o[i]);
                g_bolt_zone_gun[zs][j] = v;
            }
            g_bolt_zone_gun_ok[zs] = true;
        }
        // (2026-11-01, GOAL-offhand-changes C37; the user: the interaction spots move when the gun is held by the off hand) two-
        // handed, the zone from its place in the drawn gun's frame (learned above, held in the gun hand alone), not the gun hand's
        // target the two-handed hold turns the gun off; eased in by the hold's weight
        if (const float tb = holster::two_hand_blend(); tb > 0.05f && zs >= 0 && g_bolt_zone_gun_ok[zs] && bp.gun_frame_ok && !g_bolt_grip) {
            for (int i = 0; i < 3; ++i) {
                const float m = bp.gun_frame_o[i] + bp.gun_frame_R[i * 3] * g_bolt_zone_gun[zs][0] + bp.gun_frame_R[i * 3 + 1] * g_bolt_zone_gun[zs][1] +
                                bp.gun_frame_R[i * 3 + 2] * g_bolt_zone_gun[zs][2];
                z0[i] += (m - z0[i]) * std::fmin(1.0f, tb);
            }
        }
        // ([Reload] OffHandBoltTrigger: the off hand's trigger, its grip at 0.6, its let-go at 0.4, as the carried bolt's)
        const bool ob_trig = g_ob_trigger.load(std::memory_order_relaxed);
        const hands::Hand ohh = hands::get(off_h);
        const float opress = !ob_trig ? ohh.grip : menu::visible() ? 0.0f : ohh.trigger > (g_bolt_grip ? 0.4f : 0.6f) ? 1.0f : 0.0f;
        bolt_step(w, "", bparts, off_h, gun_h, ogb, opress, bp.hand[gj], u, up, z0, 1e9f, blift, bslide);
        if (ob_trig && (g_bolt_grip || g_bolt_d < g_bolt_r) && off_h >= 0 && off_h < 2)
            g_cb_trig_latch[off_h].store(true, std::memory_order_relaxed);  // (its LT held from the game there)
        if (ob_trig) g_bolt_want[off_h] = false;  // (the review: the grip is never the bolt's then: the foregrip's, as before)
    } else {
        g_bolt_grip = false;
        g_grip_was = hands::get(off_h).grip > 0.6f;
        g_bolt_z0_gun_ok = false;  // (the review: the status never a stale zone)
    }
    // [Reload] BoltParts: the drawn bolt (a hand away or not tracked: as it was left)
    g_bolt_lift.store(bparts ? blift : 0.0f, std::memory_order_relaxed);
    g_bolt_slide.store(bparts ? bslide : 0.0f, std::memory_order_relaxed);
    g_bolt_weapon.store(bparts ? w : -1, std::memory_order_relaxed);
    if (!bparts) g_bolt_up = g_bolt_cycled = false;
    // [Reload] BreechParts: the single-shot breeches
    const Breech* brz = k == kBreech ? breech_of(w) : nullptr;
    float bdrawn = brz ? g_breech_amt.load(std::memory_order_relaxed) : 0.0f;  // as left
    g_breech_want[0] = g_breech_want[1] = false;
    if (brz && deadeye) g_lever_req = false;  // (C4, the review: a stick flick in Dead Eye not kept for after)
    if (brz && !deadeye) {
        const GunSounds* gs = gun_sounds(w);
        bool want_open = g_breech_open, moved = false;
        if (brz->gesture == 0) {  // the gun's flicks: down opens, up shuts (the lever under the grip)
            // (2026-11-01, GOAL-offhand-changes C4; the user: the Buffalo's lever worked as the lever actions') [Reload]
            // LeverControl as the lever guns': flick (the gun's flicks, as before), stick (the stick's flick down opens it,
            // another shuts it), hold (the gun's flicks with the stick held down)
            const int bmode = lever_mode_of(w);
            const bool bheld = bmode != 4 || g_lever_hold.load(std::memory_order_relaxed);
            const bool bstick = bmode == 1 && g_lever_req.exchange(false);
            const bool can_open = !g_breech_open && (g_needs || !g_breech_round.load(std::memory_order_relaxed)) && now - g_breech_shut_ms >= 200.0;
            const bool down = can_open && bmode != 1 && bheld && g_w[0] <= -g_lever_speed;
            g_breech_flicks = down ? g_breech_flicks + 1 : 0;
            if (g_breech_flicks >= 2 || (can_open && bstick)) want_open = true;  // two two-frame steps: not the shut's own back swing
            else if (g_breech_open && now - g_breech_ms >= 150.0 && (bmode == 1 ? bstick : bheld && g_w[0] >= g_lever_speed)) want_open = false;
        } else if (pts && bp.hand_ok[gj] && bp.hand_ok[oj]) {
            // the zone: the part's handle as drawn, in the drawn gun's own frame (else about the gun hand, the model's
            // origin taken 8 cm ahead of the wrist along the barrel)
            float hz[3];
            for (int i = 0; i < 3; ++i) hz[i] = brz->shut[i] + (brz->open[i] - brz->shut[i]) * bdrawn;
            std::memcpy(g_breech_hz, hz, sizeof(g_breech_hz));
            float zh[3];
            part_hz(hz, gj, zh);  // (C36: the zone, not the drawn part)
            for (int i = 0; i < 3; ++i)
                g_breech_zone[i] = bp.gun_frame_ok ? bp.gun_frame_o[i] + bp.gun_frame_R[i * 3] * zh[0] + bp.gun_frame_R[i * 3 + 1] * zh[1] + bp.gun_frame_R[i * 3 + 2] * zh[2]
                                                   : bp.hand[gj][i] + u[i] * (0.08f - zh[2]) + rgt[i] * zh[0] + up[i] * zh[1];
            const float* oh = ogb;
            const float dx = oh[0] - g_breech_zone[0], dy = oh[1] - g_breech_zone[1], dz = oh[2] - g_breech_zone[2];
            g_breech_d = std::sqrt(dx * dx + dy * dy + dz * dz);
            const bool in = g_breech_d < g_bolt_r;
            const bool grip = hands::get(off_h).grip > 0.6f;
            // the hand's travel from the gun hand: back along the barrel (the Rolling Block's spur), or up (the trapdoor)
            float rel[3] = {oh[0] - bp.hand[gj][0], oh[1] - bp.hand[gj][1], oh[2] - bp.hand[gj][2]};
            const float s = brz->gesture == 1 ? -(rel[0] * u[0] + rel[1] * u[1] + rel[2] * u[2]) : rel[0] * up[0] + rel[1] * up[1] + rel[2] * up[2];
            g_breech_want[off_h] = in || g_breech_grip;
            if (grip && !g_breech_grip_was && in) {
                g_breech_grip = true;
                g_breech_s0 = s - bdrawn * brz->travel;  // taken where it is drawn
                controllers::pulse(off_h, 0.35f, 25);
                log::info("[actions] breech (%d): gripped (%.3f m from its zone, drawn %.2f)", w, g_breech_d, bdrawn);
            }
            if (g_breech_grip && grip) {
                const float b = (s - g_breech_s0) / brz->travel;
                bdrawn = b < 0.0f ? 0.0f : b > 1.0f ? 1.0f : b;
                moved = true;
                if (!g_breech_open && bdrawn >= 0.95f) want_open = true;
                else if (g_breech_open && bdrawn <= 0.05f) want_open = false;
            }
            if (!grip) g_breech_grip = false;
            g_breech_grip_was = grip;
        } else {  // the hands not tracked: the part let go where it is
            g_breech_grip = false;
            g_breech_grip_was = hands::get(off_h).grip > 0.6f;
        }
        if (want_open != g_breech_open) {
            g_breech_open = want_open;
            float at[3];
            const bool hand = body_hand(gun_h, at);
            if (g_breech_open) {
                g_breech_ms = now;
                g_opens.fetch_add(1, std::memory_order_relaxed);
                if (gs && hand) play_at(gs->open, at);
                if (brz->cock_on_open) g_hammer_down = false;
                controllers::pulse(gun_h, 0.4f, 15);
                log::info("[actions] breech (%d): open%s, clip %.0f/%.0f, spare %.0f", w,
                          g_breech_round.load(std::memory_order_relaxed) ? " (a live round in it)" : ", the case out", st.clip, st.clip_max, st.spare);
            } else {
                g_breech_shut_ms = now;
                g_closes.fetch_add(1, std::memory_order_relaxed);
                if (gs && hand) {
                    play_at(gs->close, at);
                    play_at(gs->close2, at);
                }
                g_hammer_down = false;
                // no spare rounds left but the game's count has some (the Springfield's clip holds four): from those
                if (!g_breech_round.load(std::memory_order_relaxed) && st.spare < 1.0f && st.clip >= 0.5f) {
                    g_breech_round.store(true, std::memory_order_relaxed);
                    log::info("[actions] breech (%d): no spare rounds: chambered from the gun's own count (clip %.0f)", w, st.clip);
                }
                controllers::pulse(gun_h, 0.6f, 20);
                if (g_breech_round.load(std::memory_order_relaxed)) {
                    if (g_needs) g_cycles.fetch_add(1, std::memory_order_relaxed);
                    g_needs = false;
                    log::info("[actions] breech (%d): shut, ready", w);
                } else {
                    log::info("[actions] breech (%d): shut with no round in: not ready", w);
                }
            }
        }
        if (!moved && brz->gesture == 0) {  // the flicked block drawn toward its state, 80 ms either way
            static double last = 0;
            const float step = last > 0 ? static_cast<float>((now - last) / 80.0) : 1.0f;
            last = now;
            const float target = g_breech_open ? 1.0f : 0.0f;
            bdrawn = bdrawn < target ? (bdrawn + step > target ? target : bdrawn + step) : (bdrawn - step < target ? target : bdrawn - step);
        }
    } else if (!brz) {
        g_breech_open = g_breech_grip = false;
    } else {  // Dead Eye: the part let go
        g_breech_grip = false;
    }
    {  // the drawn hammer: down after the shot until the breech is worked, 80 ms either way
        static double last = 0;
        const float step = last > 0 ? static_cast<float>((now - last) / 80.0) : 1.0f;
        last = now;
        const float target = brz && g_hammer_down ? 0.0f : 1.0f;
        float hm = g_breech_hammer.load(std::memory_order_relaxed);
        hm = hm < target ? (hm + step > target ? target : hm + step) : (hm - step < target ? target : hm - step);
        g_breech_hammer.store(hm, std::memory_order_relaxed);
    }
    g_breech_amt.store(brz ? bdrawn : 0.0f, std::memory_order_relaxed);
    // (step 8, R14) the muzzle-loader's ram: the off hand at the muzzle, its strokes back along the barrel and forward
    // (C15, RamrodAtChest: the ramrod taken at the chest first; the stroke in, then out)
    const bool rod_mode = g_rod_chest.load(std::memory_order_relaxed);
    g_rod_ready.store(rod_mode && k == kMuzzle && !deadeye && g_needs && g_muz_round && g_rod_hand.load(std::memory_order_relaxed) < 0,
                      std::memory_order_relaxed);
    if (rod_mode && (k != kMuzzle || !g_needs || !g_muz_round || deadeye)) g_rod_hand.store(-1, std::memory_order_relaxed);
    if (rod_mode && g_rod_hand.load(std::memory_order_relaxed) >= 0 && hands::get(g_rod_hand.load(std::memory_order_relaxed)).grip < 0.4f) {
        log::info("[actions] muzzle-loader (%d): the ramrod let go: back at the chest", w);
        g_rod_hand.store(-1, std::memory_order_relaxed);
    }
    float rod_head_w[3] = {ogb[0], ogb[1], ogb[2]};  // the held ramrod's head (world), the hand's spot without one
    {  // (C15b) the ramrod in the hand holding it: its head at that hand's spot, in the gun's model
        const int rh = g_rod_hand.load(std::memory_order_relaxed);
        bool shown = false;
        // (2026-11-02, GOAL-offhand-changes C15f; round 69, the user: no ramrod in the right hand with the Blunderbuss in the left)
        // the left-held gun's mirrored frame (a rotation: the model's points as they are) when the gun hand's own is not drawn
        const bool rmf = !bp.gun_frame_ok && bp.gun_mframe_ok;
        const float* GR = rmf ? bp.gun_mframe_R : bp.gun_frame_R;
        const float* GO = rmf ? bp.gun_mframe_o : bp.gun_frame_o;
        if (rod_mode && k == kMuzzle && !deadeye && rh >= 0 && rh == off_h && pts && (bp.gun_frame_ok || rmf) && bp.hand_ok[oj]) {
            shown = rod_in_hand(bp, rh, oj, ogb, GR, GO, rod_head_w);
        }
        g_rod_shown.store(shown, std::memory_order_relaxed);
    }
    {  // (C15e) [Reload] RamrodFrom=gun: the off hand's grip at the rod's grab point under the barrel takes the ramrod
        static bool grip_was = false;
        // (the review: a left-held gun has only the mirrored frame drawn: that one then)
        const bool mfr = !bp.gun_frame_ok && bp.gun_mframe_ok;
        const float* FR = mfr ? bp.gun_mframe_R : bp.gun_frame_R;
        const float* FO = mfr ? bp.gun_mframe_o : bp.gun_frame_o;
        const bool ready = rod_mode && g_rod_from_gun.load(std::memory_order_relaxed) && k == kMuzzle && !deadeye && g_needs && g_muz_round &&
                           g_rod_hand.load(std::memory_order_relaxed) < 0 && pts && (bp.gun_frame_ok || mfr) && bp.hand_ok[oj];
        const bool grip = hands::get(off_h).grip > 0.6f;
        g_rod_zone_on = ready;
        g_rod_zone_d = 1e9f;
        if (ready) {
            float go[3];
            {
                std::lock_guard lock(g_rod_mx);
                std::memcpy(go, g_rod_grab_off, sizeof(go));
            }
            const float gp[3] = {kRodGrab[0] + go[0], kRodGrab[1] + go[1], kRodGrab[2] - go[2]};
            for (int i = 0; i < 3; ++i) g_rod_zone[i] = FO[i] + FR[i * 3] * gp[0] + FR[i * 3 + 1] * gp[1] + FR[i * 3 + 2] * gp[2];
            const float dx = ogb[0] - g_rod_zone[0], dy = ogb[1] - g_rod_zone[1], dz = ogb[2] - g_rod_zone[2];
            g_rod_zone_d = std::sqrt(dx * dx + dy * dy + dz * dz);
            g_breech_want[off_h] = g_breech_want[off_h] || g_rod_zone_d < std::fmax(g_bolt_r, 0.09f);  // (its grip not the foregrip's)
            if (grip && !grip_was && g_rod_zone_d < std::fmax(g_bolt_r, 0.09f)) {
                g_rod_hand.store(off_h, std::memory_order_relaxed);
                g_rod_ready.store(false, std::memory_order_relaxed);
                controllers::pulse(off_h, 0.35f, 25);
                log::info("[actions] muzzle-loader: the ramrod taken from the gun by the %s hand (%.3f m from its place)", off_h ? "right" : "left", g_rod_zone_d);
            }
        }
        grip_was = grip;
    }
    if (k == kMuzzle && !deadeye && g_needs && g_muz_round && pts && bp.hand_ok[gj] && bp.hand_ok[oj]) {
        float ma[3];
        part_hz(kMuzzleAt, gj, ma);  // (C36)
        const bool mz_mf = !bp.gun_frame_ok && bp.gun_mframe_ok;  // (C15f: a left-held gun's mirrored frame)
        const float* MR = mz_mf ? bp.gun_mframe_R : bp.gun_frame_R;
        const float* MO = mz_mf ? bp.gun_mframe_o : bp.gun_frame_o;
        for (int i = 0; i < 3; ++i)
            g_muz_zone[i] = bp.gun_frame_ok || mz_mf ? MO[i] + MR[i * 3] * ma[0] + MR[i * 3 + 1] * ma[1] + MR[i * 3 + 2] * ma[2]
                                                     : bp.hand[gj][i] + u[i] * -ma[2] + up[i] * ma[1] + rgt[i] * ma[0];
        const bool rod_held = g_rod_hand.load(std::memory_order_relaxed) == off_h;
        const float* oh = rod_mode && rod_held ? rod_head_w : ogb;  // (the review: the ramrod's head, where it is drawn)
        const float dx = oh[0] - g_muz_zone[0], dy = oh[1] - g_muz_zone[1], dz = oh[2] - g_muz_zone[2];
        g_muz_d = std::sqrt(dx * dx + dy * dy + dz * dz);
        // (C15) the ramrod held: the muzzle's zone 12 cm (the bare hand's 7: the user found the strokes hard to hit)
        const bool in = g_muz_d < (rod_mode && rod_held ? std::fmax(g_bolt_r, 0.12f) : g_bolt_r);
        const bool grip = hands::get(off_h).grip > 0.6f && (!rod_mode || rod_held);
        g_breech_want[off_h] = g_breech_want[off_h] || in || g_muz_grip;  // (the foregrip's snap not taken there)
        if (rod_mode && rod_held && grip && !g_muz_grip && in) {  // (C15) the ramrod brought to the muzzle: its tip at the muzzle
            g_muz_grip = true;
            g_muz_down = false;
            g_muz_s0 = -((oh[0] - bp.hand[gj][0]) * u[0] + (oh[1] - bp.hand[gj][1]) * u[1] + (oh[2] - bp.hand[gj][2]) * u[2]);
            controllers::pulse(off_h, 0.35f, 25);
            log::info("[actions] muzzle-loader (%d): the ramrod at the muzzle (%.3f m from it): in, then out", w, g_muz_d);
        } else if (!rod_mode && grip && !g_muz_grip_was && in) {
            g_muz_grip = true;
            g_muz_down = false;
            // (the review) the stroke measured against the gun (the gun hand), not the world: a walk or a turn is no stroke
            g_muz_s0 = -((oh[0] - bp.hand[gj][0]) * u[0] + (oh[1] - bp.hand[gj][1]) * u[1] + (oh[2] - bp.hand[gj][2]) * u[2]);
            controllers::pulse(off_h, 0.35f, 25);
            log::info("[actions] muzzle-loader (%d): the ram gripped at the muzzle (%.3f m from it), stroke %d of %d", w, g_muz_d, g_muz_strokes + 1, kRamStrokes);
        }
        if (g_muz_grip && grip && rod_mode) {  // (C15) the ramrod: in (back along the barrel past 0.7 of the stroke), then out (under 0.05)
            // (cycle H7u: from the grab point the hand's approach counted: one taken in front of the muzzle never got past 0)
            // measured from the muzzle itself: 0 the hand at it, the stroke the hand that far back along the barrel
            const float s = -((oh[0] - g_muz_zone[0]) * u[0] + (oh[1] - g_muz_zone[1]) * u[1] + (oh[2] - g_muz_zone[2]) * u[2]);
            g_muz_rod.store(std::fmax(0.0f, std::fmin(1.0f, s / kRamStroke)), std::memory_order_relaxed);
            const GunSounds* gs = gun_sounds(w);
            float at[3];
            const bool hand = body_hand(gun_h, at);
            if (!g_muz_down && s >= kRamStroke * 0.7f) {
                g_muz_down = true;
                if (gs && hand) play_at(gs->open, at);
                controllers::pulse(off_h, 0.4f, 15);
                log::info("[actions] muzzle-loader (%d): the ramrod in", w);
            } else if (g_muz_down && s <= kRamStroke * 0.05f) {
                g_needs = false;
                g_hammer_down = false;
                g_cycles.fetch_add(1, std::memory_order_relaxed);
                g_rod_hand.store(-1, std::memory_order_relaxed);
                g_muz_grip = false;
                g_muz_rod.store(0.0f, std::memory_order_relaxed);
                if (gs && hand) {
                    play_at(gs->close, at);
                    play_at(gs->close2, at);
                }
                controllers::pulse(gun_h, 0.6f, 20);
                log::info("[actions] muzzle-loader (%d): the ramrod in and out: rammed home: ready (the ramrod back at the chest)", w);
            }
        } else if (g_muz_grip && grip) {
            const float s = -((oh[0] - bp.hand[gj][0]) * u[0] + (oh[1] - bp.hand[gj][1]) * u[1] + (oh[2] - bp.hand[gj][2]) * u[2]) - g_muz_s0;  // back along the barrel
            g_muz_rod.store(std::fmax(0.0f, std::fmin(1.0f, s / kRamStroke)), std::memory_order_relaxed);  // (the rod with the hand)
            const GunSounds* gs = gun_sounds(w);
            float at[3];
            const bool hand = body_hand(gun_h, at);
            if (!g_muz_down && s >= kRamStroke) {
                g_muz_down = true;
                if (gs && hand) play_at(gs->open, at);
                controllers::pulse(off_h, 0.4f, 15);
            } else if (g_muz_down && s <= kRamStroke * 0.3f) {
                g_muz_down = false;
                ++g_muz_strokes;
                if (gs && hand) play_at(gs->close, at);
                controllers::pulse(off_h, 0.4f, 15);
                log::info("[actions] muzzle-loader (%d): rammed, stroke %d of %d", w, g_muz_strokes, kRamStrokes);
                if (g_muz_strokes >= kRamStrokes) {
                    g_needs = false;
                    g_hammer_down = false;
                    g_cycles.fetch_add(1, std::memory_order_relaxed);
                    if (gs && hand) play_at(gs->close2, at);
                    controllers::pulse(gun_h, 0.6f, 20);
                    log::info("[actions] muzzle-loader (%d): rammed home: ready", w);
                }
            }
        }
        if (!grip) {
            g_muz_grip = false;
            g_muz_rod.store(0.0f, std::memory_order_relaxed);
        }
        g_muz_grip_was = grip;
    } else {
        g_muz_grip = false;
        g_muz_grip_was = hands::get(off_h).grip > 0.6f;
        g_muz_rod.store(0.0f, std::memory_order_relaxed);
    }
    // (the muzzle-loader's fire clip stopped as the breeches': parts_weapon reads this)
    g_breech_weapon.store(brz || k == kMuzzle ? w : -1, std::memory_order_relaxed);
    // [Reload] SemiAutoParts: the bolt racked by the off hand
    const bool sparts = k == kSemi;
    float sdrawn = sparts ? g_semi_amt.load(std::memory_order_relaxed) : 0.0f;
    g_semi_want[0] = g_semi_want[1] = false;
    bool chamber = false;
    if (sparts && !deadeye && pts && bp.hand_ok[gj] && bp.hand_ok[oj]) {
        const float hz[3] = {kSemiHandle[0], kSemiHandle[1], kSemiHandle[2] + sdrawn * kSemiTravel};  // the handle as drawn
        std::memcpy(g_semi_hz, hz, sizeof(g_semi_hz));
        float zh[3];
        part_hz(hz, gj, zh);  // (C36: the zone, not the drawn part)
        for (int i = 0; i < 3; ++i)
            g_semi_zone[i] = bp.gun_frame_ok ? bp.gun_frame_o[i] + bp.gun_frame_R[i * 3] * zh[0] + bp.gun_frame_R[i * 3 + 1] * zh[1] + bp.gun_frame_R[i * 3 + 2] * zh[2]
                                             : bp.hand[gj][i] + u[i] * (0.08f - zh[2]) + rgt[i] * zh[0] + up[i] * zh[1];
        const float* oh = ogb;
        const float dx = oh[0] - g_semi_zone[0], dy = oh[1] - g_semi_zone[1], dz = oh[2] - g_semi_zone[2];
        g_semi_d = std::sqrt(dx * dx + dy * dy + dz * dz);
        const bool in = g_semi_d < g_bolt_r;
        const bool grip = hands::get(off_h).grip > 0.6f;
        const float s = -((oh[0] - bp.hand[gj][0]) * u[0] + (oh[1] - bp.hand[gj][1]) * u[1] + (oh[2] - bp.hand[gj][2]) * u[2]);  // back
        g_semi_want[off_h] = in || g_semi_grip;
        if (grip && !g_semi_grip_was && in) {
            g_semi_grip = true;
            g_semi_s0 = s - sdrawn * kSemiTravel;
            controllers::pulse(off_h, 0.35f, 25);
            log::info("[actions] semi-auto (%d): the bolt gripped (%.3f m from its handle)%s", w, g_semi_d, g_needs ? "" : ", a shell already in");
        }
        if (g_semi_grip && grip) {
            const float b = (s - g_semi_s0) / kSemiTravel;
            sdrawn = b < 0.0f ? 0.0f : b > 1.0f ? 1.0f : b;
            if (!g_semi_back && sdrawn >= 0.9f) {
                g_semi_back = true;
                if (const GunSounds* gs = gun_sounds(w)) play_at(gs->open, oh);
                controllers::pulse(off_h, 0.5f, 15);
                log::info("[actions] semi-auto (%d): the bolt back", w);
            } else if (g_semi_back && sdrawn <= 0.2f) {
                chamber = true;  // pushed forward
            }
        }
        if (!grip && g_semi_grip) {
            g_semi_grip = false;
            if (g_semi_back) chamber = true;  // let go back: it springs forward
        }
        g_semi_grip_was = grip;
    } else {
        g_semi_grip = false;
        g_semi_grip_was = hands::get(off_h).grip > 0.6f;
        if (sparts && g_semi_back) chamber = true;
    }
    if (chamber) {
        g_semi_back = false;
        float at[3];
        if (const GunSounds* gs = gun_sounds(w); gs && body_hand(gun_h, at)) {
            play_at(gs->close, at);
            play_at(gs->close2, at);
        }
        controllers::pulse(gun_h, 0.6f, 20);
        if (g_needs) g_cycles.fetch_add(1, std::memory_order_relaxed);
        g_needs = false;
        log::info("[actions] semi-auto (%d): the bolt forward, a shell chambered", w);
    }
    if (sparts && !g_semi_grip) {  // not held: it springs shut (40 ms)
        static double last = 0;
        const float step = last > 0 ? static_cast<float>((now - last) / 40.0) : 1.0f;
        last = now;
        sdrawn = sdrawn - step < 0.0f ? 0.0f : sdrawn - step;
    }
    g_semi_amt.store(sparts ? sdrawn : 0.0f, std::memory_order_relaxed);
    g_semi_weapon.store(sparts ? w : -1, std::memory_order_relaxed);
    // [Reload] PistolMagazines / PistolSlides: the three pistols
    const PistolDef* pdef = pk ? pistol_of(w) : nullptr;
    const bool pmags = pdef && g_mags.load(std::memory_order_relaxed);
    const bool pslides = pdef && g_slides.load(std::memory_order_relaxed);
    const bool mauser = k == kMauser;
    const bool pparts_on = pdef && (pslides || (mauser && pmags));  // the slide (the bolt) worked by hand and drawn
    const bool toggle = w == 1;
    // the count as the mod's natives will leave it (their wait), else the game's
    const int count_now = g_expect_until > 0.0 ? g_expect_clip : static_cast<int>(std::floor(st.clip + 0.01f));
    // a point in the gun's frame (x right, y up, z back) in the world: the drawn gun's frame, else the gun hand's target
    // (the model's origin taken 8 cm ahead of the wrist, as the semi-auto's)
    auto gun_pt = [&](const float* g, float* o) {
        for (int i = 0; i < 3; ++i)
            o[i] = bp.gun_frame_ok ? bp.gun_frame_o[i] + bp.gun_frame_R[i * 3] * g[0] + bp.gun_frame_R[i * 3 + 1] * g[1] + bp.gun_frame_R[i * 3 + 2] * g[2]
                                   : bp.hand[gj][i] + u[i] * (0.08f - g[2]) + rgt[i] * g[0] + up[i] * g[1];
    };
    g_pwell_ok = pdef && pts && bp.hand_ok[gj];
    if (g_pwell_ok) {
        float wl[3];
        well_at(pdef, g_mag_off[gj], wl);
        gun_pt(wl, g_pwell);
    }
    // an ejected round waits to be taken from the count (its natives once the spare is settled; 2 s at most)
    if (g_lose_pending > 0) {
        RdrvrActorState sx = st;
        // (the review) only with no other count of the mod's awaited: one expectation at a time
        if (g_expect_until <= 0.0 && count_now >= 1 && reload::lose_round(sx, 6)) {  // (the pistols' ammo, AMMO_TYPE_PISTOL 6: Enums.h, dual.cpp)
            --g_lose_pending;
            g_lost.fetch_add(1, std::memory_order_relaxed);
            g_expect_clip = static_cast<int>(std::floor(st.clip + 0.01f)) - 1;  // (lose_round's own count)
            g_expect_until = now + 1500.0;
            g_lose_ms = now;
            log::info("[actions] %s (%d): the ejected round taken from the count (clip %d -> %d, spare %.0f kept)", kind_name(k), w, count_now, g_expect_clip,
                      st.spare);
        } else if (now - g_lose_ms > 2000.0 || count_now < 1) {
            g_lose_pending = 0;  // (any still waiting dropped: logged)
            log::info("[actions] %s (%d): the ejected round not taken from the count (the spare not settled in 2 s, or the gun empty)", kind_name(k), w);
        }
    }
    // the magazine's release (the stick's flick down, filter_stick): the gun's count lowered to the round in the chamber
    // (one if the gun was not empty: with PistolSlides the chamber's own state), the magazine's rounds back to the spare
    bool mag_dropped = false;
    int dropped_rounds = 0;
    const bool req = g_mag_req.exchange(false);
    if (pmags && k == kPistol && req && !deadeye && !menu::visible()) {
        if (g_expect_until > 0.0 || g_lose_pending > 0) {  // (the review: one count of the mod's awaited at a time)
            log::info("[actions] pistol (%d): the stick's flick: the gun's count not settled yet, nothing dropped (flick again)", w);
        } else if (!g_mag_in.load(std::memory_order_relaxed)) {
            log::info("[actions] pistol (%d): the stick's flick: no magazine in it", w);
        } else {
            const int keep = (pslides ? g_chambered : count_now >= 1) && count_now >= 1 ? 1 : 0;
            RdrvrActorState sx = st;
            sx.clip = static_cast<float>(count_now);
            // (the review) the magazine's rounds go back to the spare only as far as its cap (the game's give-back is
            // clamped there [I]): a drop whose rounds would not all fit, or with the inventory not read, is refused
            float inv_n = 0.0f, inv_cap = 0.0f;
            const bool inv_ok = count_now <= keep || dual::inv_entry(6, &inv_n, &inv_cap);
            const bool fits = count_now <= keep || (inv_ok && (inv_n >= 9999.0f || inv_n + static_cast<float>(count_now - keep) <= inv_cap + 0.01f));
            if (!fits) {
                controllers::pulse(gun_h, 0.15f, 10);
                log::info("[actions] pistol (%d): the magazine not dropped: its %d rounds would not fit back in the spare (%s %.0f of %.0f)", w,
                          count_now - keep, inv_ok ? "the spare" : "the inventory not read:", inv_n, inv_cap);
            }
            const bool lower = !fits ? false : count_now > keep ? reload::set_clip(sx, keep) : true;  // (already at it: nothing to queue)
            if (lower && !reloading) {
                g_mag_in.store(false, std::memory_order_relaxed);
                g_mag_out[w] = true;
                dropped_rounds = count_now - keep;
                if (count_now > keep) {
                    g_expect_clip = keep;
                    g_expect_until = now + 1500.0;
                }
                if (pslides) g_chambered = keep == 1;
                g_mag_drops.fetch_add(1, std::memory_order_relaxed);
                mag_dropped = true;
                controllers::pulse(gun_h, 0.5f, 15);
                float at[3];
                if (body_hand(gun_h, at)) play_at("ARMOR_METAL_SHORT_MASTER", at);  // (the reload's first sound: the magazine out)
                log::info("[actions] pistol (%d): the magazine dropped by the stick (clip %d -> %d, %d rounds back to the spare %.0f)%s", w, count_now, keep,
                          dropped_rounds, st.spare, keep ? ", a round left in the chamber" : "");
            }
        }
    }
    // the Mauser's clip pulled out by the flick: the bolt goes forward, a round chambered
    bool clip_out = false;
    int clip_out_left = 0;
    bool bolt_fwd = false;  // the slide (bolt) came forward this frame
    bool pushed = false;    // ... pushed forward by the hand (not let go)
    if (pmags && mauser && req && !deadeye && !menu::visible() && g_clip_seated) {
        clip_out = true;
        clip_out_left = g_clip_left;
        g_clip_seated = false;
        g_clip_left = 0;
        g_clip_outs.fetch_add(1, std::memory_order_relaxed);
        if (g_locked || g_pback) {
            g_locked = false;
            g_pback = true;
            bolt_fwd = true;  // (chambered below; its sound and its count there)
            if (g_pgrip) {  // (the review) a hand on the bolt lets go of it: no second stroke from where it holds
                g_pgrip = false;
                g_pgrip_was = true;
            }
        }
        log::info("[actions] Mauser (%d): the clip pulled out by the stick (%d rounds left on it): the bolt forward", w, clip_out_left);
    }
    // the rack: the other hand grips the slide (the toggle's knee, the bolt's ears), pulls it back (drawn 1:1 to its stop)
    // and lets go (it springs forward) or pushes it forward. Back: a chambered round ejected (lost); forward: the next
    // chambered from the magazine. A slide held back on its stop is taken where it is: let go, it goes forward (the
    // slingshot: the slide release)
    float pdr = pparts_on ? g_pdrawn : 0.0f;
    g_pslide_want[0] = g_pslide_want[1] = false;
    auto slide_back = [&](const char* how) {
        g_pback = true;
        g_racks.fetch_add(1, std::memory_order_relaxed);
        const bool was = pslides && g_chambered;
        if (was) {  // the chambered round thrown out: lost (the user's choice)
            g_chambered = false;
            g_ejects.fetch_add(1, std::memory_order_relaxed);
            ++g_lose_pending;  // (the review: two racks before the first is taken, both lost)
            g_lose_ms = now;
        }
        g_pmag_at_back = count_now - (was ? 1 : 0);
        log::info("[actions] %s (%d): the %s back%s (%s; the magazine's rounds %d)", kind_name(k), w, mauser ? "bolt" : toggle ? "toggle" : "slide",
                  was ? ", the chambered round ejected (lost)" : "", how, g_pmag_at_back);
    };
    if (pparts_on && !deadeye && pts && bp.hand_ok[gj] && bp.hand_ok[oj]) {
        float hz[3];
        if (toggle) {
            float fd;
            toggle_pose(pdr, hz, &fd);
            hz[1] += pdef->handle[1] - kToggleKnee[1];  // (the knobs just above the knee)
            hz[2] += pdef->handle[2] - kToggleKnee[2];
        } else {
            for (int i = 0; i < 3; ++i) hz[i] = pdef->handle[i];
            hz[2] += pdr * pdef->travel;
        }
        std::memcpy(g_phz, hz, sizeof(g_phz));
        {
            float zh[3];
            part_hz(hz, gj, zh);  // (C36: the zone, not the drawn slide)
            gun_pt(zh, g_pzone);
        }
        const float* oh = ogb;
        const float dx = oh[0] - g_pzone[0], dy = oh[1] - g_pzone[1], dz = oh[2] - g_pzone[2];
        g_pd = std::sqrt(dx * dx + dy * dy + dz * dz);
        const bool in = g_pd < kPistolGripR;
        const bool grip = hands::get(off_h).grip > 0.6f && !(dual::state().on && dual::state().ctrl == off_h);
        float pw[3];  // the pull: back along the gun; the toggle's knee up and back along its chord
        for (int i = 0; i < 3; ++i)
            pw[i] = !toggle ? (bp.gun_frame_ok ? bp.gun_frame_R[i * 3 + 2] : -u[i])
                  : bp.gun_frame_ok ? bp.gun_frame_R[i * 3 + 1] * kTogglePull[1] + bp.gun_frame_R[i * 3 + 2] * kTogglePull[2]
                                    : up[i] * kTogglePull[1] - u[i] * kTogglePull[2];
        const float s = (oh[0] - bp.hand[gj][0]) * pw[0] + (oh[1] - bp.hand[gj][1]) * pw[1] + (oh[2] - bp.hand[gj][2]) * pw[2];
        g_pslide_want[off_h] = in || g_pgrip;
        if (grip && !g_pgrip_was && in) {
            g_pgrip = true;
            g_pgrip_locked = g_locked;
            g_ps0 = s - pdr * pdef->travel;
            controllers::pulse(off_h, 0.35f, 25);
            log::info("[actions] %s (%d): the %s gripped (%.3f m from it)%s", kind_name(k), w, mauser ? "bolt" : toggle ? "toggle" : "slide", g_pd,
                      g_locked ? ", held back: let go to send it forward" : "");
            if (g_locked) {
                g_locked = false;  // (the hand holds it now)
                if (!g_pback) {
                    g_pback = true;
                    g_pmag_at_back = count_now - (pslides && g_chambered ? 1 : 0);
                }
            }
        }
        if (g_pgrip && grip) {
            const float b = (s - g_ps0) / pdef->travel;
            pdr = !(b >= 0.0f) ? 0.0f : b > 1.0f ? 1.0f : b;  // (a NaN: shut)
            if (!g_pback && pdr >= 0.9f) {
                slide_back("pulled");
                play_at(pdef->back, oh);
                controllers::pulse(off_h, 0.5f, 15);
            } else if (g_pback && pdr <= 0.2f) {
                bolt_fwd = true;  // pushed forward
                pushed = true;
            }
        }
        if (!grip && g_pgrip) {
            g_pgrip = false;
            if (g_pback) bolt_fwd = true;  // let go back: it springs forward
        }
        g_pgrip_was = grip;
    } else {
        g_pgrip = false;
        g_pgrip_was = hands::get(off_h).grip > 0.6f;
        g_pd = 1e9f;
        if (pparts_on && g_pback && !g_locked) bolt_fwd = true;
    }
    if (bolt_fwd && pparts_on) {
        g_pback = false;
        // the Mauser's bolt stays back (held open) on an empty magazine, or when pulled back from shut with room for rounds
        // and spare ones and no clip in (optimumbox's rule, MIT: the clip goes in while it is open); taken while held
        // open, or with the clip pulled out, it goes forward
        // the magazine's rounds now: the count less the chambered one and an ejected one not yet taken from it
        const int m_fwd = count_now - (pslides && g_chambered ? 1 : 0) - g_lose_pending;
        const bool room = count_now + 1 <= static_cast<int>(std::floor(st.clip_max + 0.01f)) && st.spare >= 1.0f;
        if (mauser && !clip_out && !pushed && (m_fwd < 1 || (pmags && room && !g_pgrip_locked && !g_clip_seated))) {
            g_locked = true;
            pdr = 1.0f;
            log::info("[actions] Mauser (%d): the bolt held open (the magazine's rounds %d, room %d): a clip goes in from the top", w, m_fwd,
                      room ? 1 : 0);
        } else {
            g_locked = false;
            if (g_clip_seated) {  // the bolt going forward strips the clip out of its guide
                clip_out = true;
                clip_out_left = g_clip_left;
                g_clip_seated = false;
                g_clip_left = 0;
                g_clip_outs.fetch_add(1, std::memory_order_relaxed);
            }
            const bool pch = m_fwd >= 1 && (g_mag_in.load(std::memory_order_relaxed) || mauser);
            if (pslides) {
                g_chambered = pch;
                if (pch) g_chambers.fetch_add(1, std::memory_order_relaxed);
            }
            float at[3];
            if (body_hand(gun_h, at)) play_at(pdef->fwd, at);
            controllers::pulse(gun_h, 0.6f, 20);
            log::info("[actions] %s (%d): the %s forward: %s", kind_name(k), w, mauser ? "bolt" : toggle ? "toggle" : "slide",
                      !pslides ? "shut" : pch ? "a round chambered" : "nothing to chamber");
        }
    }
    g_pgrip_locked = g_pgrip && g_pgrip_locked;
    if (pparts_on && !g_pgrip) {  // not held: it springs shut (40 ms), or stays on its stop
        static double last = 0;
        const float step = last > 0 ? static_cast<float>((now - last) / 40.0) : 1.0f;
        last = now;
        pdr = g_locked ? 1.0f : pdr - step < 0.0f ? 0.0f : pdr - step;
    }
    g_pdrawn = pparts_on ? pdr : 0.0f;
    {  // drawn: the hand's (or the stop's), and the shot's own blowback (back and forward in 80 ms)
        const double tb = now - g_pblow_ms;
        const float blow = tb >= 0.0 && tb < 80.0 ? static_cast<float>(1.0 - std::fabs(tb - 40.0) / 40.0) : 0.0f;
        g_pslide.store(pparts_on ? std::fmax(g_pdrawn, blow) : 0.0f, std::memory_order_relaxed);
        g_pslide_weapon.store(pparts_on ? w : -1, std::memory_order_relaxed);
    }
    // the magazine (the clip) seen falling, and the Mauser's clip in its guide (round_draw), upright in the gun
    if ((mag_dropped || clip_out || (mauser && g_clip_seated)) && pts && bp.hand_ok[gj] && g_pwell_ok) {
        float pz[12];
        for (int i = 0; i < 3; ++i) {  // the gun's axes (columns: x right, y up, z back)
            pz[i] = bp.gun_frame_ok ? bp.gun_frame_R[i * 3] : rgt[i];
            pz[3 + i] = bp.gun_frame_ok ? bp.gun_frame_R[i * 3 + 1] : up[i];
            pz[6 + i] = bp.gun_frame_ok ? bp.gun_frame_R[i * 3 + 2] : -u[i];
        }
        if (mag_dropped) {  // its top at the well: the magazine below it
            const float len = round_draw::mag_length(w);
            for (int i = 0; i < 3; ++i) pz[9 + i] = g_pwell[i] - pz[3 + i] * len;
            round_draw::drop_mag(pz, w, dropped_rounds);
        } else {
            std::memcpy(pz + 9, g_pwell, sizeof(g_pwell));  // the clip's foot in the guide
            if (clip_out) round_draw::drop_mag(pz, w, clip_out_left);
            else round_draw::seat_clip(pz, g_clip_left);
        }
    }
    // the break action open: the off hand grips the barrels (near them, tipped down at the hinge as drawn) and swings
    // them up; their opening follows the hand's angle about the hinge in the gun's frame (the gun's own motion does not
    // count) from where it took them, and at the top they shut. Let go before that: they fall open again
    g_barrel_want[0] = g_barrel_want[1] = false;
    {
        const bool grip = hands::get(off_h).grip > 0.6f;
        if (!grip) g_barrel_take = false;
        if (k == kBreak && g_open && pts && bp.hand_ok[gj] && bp.hand_ok[oj] && !deadeye) {
            const float a = g_open_amt.load(std::memory_order_relaxed);
            const float th = g_break_deg * a * 0.0174532925f, max_rad = g_break_deg * 0.0174532925f;
            float v[3], ub[3];
            for (int i = 0; i < 3; ++i) {
                v[i] = ogb[i] - (bp.hand[gj][i] + u[i] * kHingeAlong + up[i] * kHingeUp);
                ub[i] = u[i] * std::cos(th) - up[i] * std::sin(th);  // the barrels as drawn
            }
            const float along = v[0] * ub[0] + v[1] * ub[1] + v[2] * ub[2];
            float perp2 = 0.0f;
            for (int i = 0; i < 3; ++i) perp2 += (v[i] - along * ub[i]) * (v[i] - along * ub[i]);
            g_barrel_d = std::sqrt(perp2);
            const float phi = std::atan2(-(v[0] * up[0] + v[1] * up[1] + v[2] * up[2]), v[0] * u[0] + v[1] * u[1] + v[2] * u[2]);  // below the line
            // the hand's place in the barrels' swing (0 shut, 1 open): its angle about the hinge against the foregrip's
            // when shut (the game's grip point; the fore-end is just ahead of the hinge, so the distance alone would
            // take a hand still at the shut foregrip)
            float phi_c = 0.17f;
            if (bp.fore_ok) {
                float f[3];
                for (int i = 0; i < 3; ++i) f[i] = bp.fore[i] - (bp.hand[gj][i] + u[i] * kHingeAlong + up[i] * kHingeUp);
                phi_c = std::atan2(-(f[0] * up[0] + f[1] * up[1] + f[2] * up[2]), f[0] * u[0] + f[1] * u[1] + f[2] * u[2]);
            }
            g_barrel_at = (phi - phi_c) / max_rad;
            const bool in = along > 0.08f && along < 0.70f && g_barrel_d < g_barrel_r && std::fabs(g_barrel_at - a) < 0.4f;
            g_barrel_want[off_h] = in || g_barrel_hold;
            if (!g_barrel_hold && grip && in && (!g_barrel_grip_was || (g_barrel_take && a >= 0.95f))) {
                g_barrel_hold = true;
                g_barrel_take = false;
                g_barrel_phi0 = phi;
                g_barrel_a0 = a;
                g_barrel_amt.store(a, std::memory_order_relaxed);
                controllers::pulse(off_h, 0.35f, 25);
                log::info("[actions] break (%d): the barrels taken by the off hand (%.3f m off them, %.2f m along, the hand at %.2f, open %.2f)", w, g_barrel_d,
                          along, g_barrel_at, a);
            }
            if (g_barrel_hold && grip) {
                float b = g_barrel_a0 + (phi - g_barrel_phi0) / max_rad;
                b = b < 0.0f ? 0.0f : b > 1.0f ? 1.0f : b;
                g_barrel_amt.store(b, std::memory_order_relaxed);
                if (b <= 0.06f) {  // swung shut
                    g_barrel_hold = false;
                    g_open = false;
                    g_closes.fetch_add(1, std::memory_order_relaxed);
                    g_hand_closes.fetch_add(1, std::memory_order_relaxed);
                    controllers::pulse(off_h, 0.7f, 20);
                    controllers::pulse(gun_h, 0.7f, 20);
                    shut_sounds(w, gun_h);
                    log::info("[actions] break (%d): shut by the off hand (open %.1f s), clip %.0f/%.0f", w, (now - g_open_ms) * 0.001, st.clip, st.clip_max);
                }
            }
            if (!grip && g_barrel_hold) {
                g_barrel_hold = false;
                log::info("[actions] break (%d): the barrels let go (at %.2f): they fall open", w, g_barrel_amt.load());
            }
        } else {
            g_barrel_hold = false;
        }
        g_barrel_grip_was = grip;
    }
    {  // [Reload] PartHandSnap (round 13): the gripped part's handle in the drawn gun's frame, the hand's weight on it
        float pg[3] = {}, turn = 0.0f;
        bool held = false, bolt = false;
        // (2026-11-02, the review of GOAL-older-items item 20: a pistol publishes no drawn long gun's frame; its slide's
        // handle is in its own gun's frame, body.cpp draws the hand onto it)
        if (g_part_snap_cfg.load(std::memory_order_relaxed) && pts && (bp.gun_frame_ok || (pparts_on && g_pgrip)) && !deadeye) {
            if (bp.gun_frame_ok && k == kBolt && bparts && g_bolt_grip && g_bolt_ctrl == off_h) {  // the zone is world: into the gun's frame
                // (C36, the review) less the gun's PartOffset (the zone's, in the hand's axes): the hand on the drawn handle, as
                // the other parts' snaps
                const float* po = g_part_off[gj];
                for (int j = 0; j < 3; ++j)
                    for (int i = 0; i < 3; ++i)
                        pg[j] += bp.gun_frame_R[i * 3 + j] * (g_bolt_zone[i] - rgt[i] * po[0] - up[i] * po[1] - u[i] * po[2] - bp.gun_frame_o[i]);
                // (2026-11-03, round 74, the user: "Carcano rifle bolt hand pose is too far from the bolt"; cycle N36: the handle
                // 6.2 cm from the knob on both bolt guns, the zone being where the hand takes it, not the knob) the knob in the
                // closed hand: the handle moved by the knob's place less the shut zone's (both the gun's frame), its stroke kept
                if (g_bolt_z0_gun_ok && (w == 13 || w == 20 || w == 31)) {
                    const float* kn = w == 20 ? kKnob20 : kKnob13a;  // (the Explosive Rifle's the Bolt Action's)
                    for (int j = 0; j < 3; ++j) {
                        float pw = 0.0f;  // the gun's PartOffset (the zone's, in the hand's axes) in the gun's frame
                        for (int i = 0; i < 3; ++i) pw += bp.gun_frame_R[i * 3 + j] * (rgt[i] * po[0] + up[i] * po[1] + u[i] * po[2]);
                        const float c = kn[j] - (g_bolt_z0_gun[j] - pw);
                        if (std::isfinite(c)) pg[j] += c;
                    }
                }
                turn = g_bolt_lift.load(std::memory_order_relaxed) * kBoltTurnDeg;  // (2026-10-28) the hand turns up with the handle
                held = bolt = true;
            } else if (k == kBreech && g_breech_grip) {
                std::memcpy(pg, g_breech_hz, sizeof(pg));
                held = true;
            } else if (k == kSemi && g_semi_grip) {
                std::memcpy(pg, g_semi_hz, sizeof(pg));
                held = true;
            } else if (pparts_on && g_pgrip) {  // PistolSlides: the slide's rear, the toggle's knobs, the bolt's ears
                // (2026-11-02, GOAL-older-items item 20; the user: "All of these guns need snap on poses for their slide
                // interactions") held as a bolt's handle: the hand kept at its turn as it took hold, its fingers closed on it
                std::memcpy(pg, g_phz, sizeof(pg));
                held = bolt = true;
            }
        }
        if (held) {
            for (int i = 0; i < 3; ++i) g_part_p[i].store(pg[i], std::memory_order_relaxed);
            g_part_turn.store(turn, std::memory_order_relaxed);
            g_part_bolt.store(bolt, std::memory_order_relaxed);
        }
        static double last = 0;
        const float step = last > 0 ? static_cast<float>((now - last) / 100.0) : 1.0f;
        last = now;
        const float target = held ? 1.0f : 0.0f;
        float b = g_part_snap.load(std::memory_order_relaxed);
        b = b < target ? (b + step > target ? target : b + step) : (b - step < target ? target : b - step);
        g_part_snap.store(b, std::memory_order_relaxed);
    }
    g_break_open = k == kBreak && g_open;
    {  // [Reload] BarrelHandSnap: the hand's weight on the barrels, 0.1 s either way
        static double last = 0;
        const float step = last > 0 ? static_cast<float>((now - last) / 100.0) : 1.0f;
        last = now;
        const float target = g_barrel_snap_cfg.load(std::memory_order_relaxed) && g_barrel_hold && g_open && k == kBreak ? 1.0f : 0.0f;
        float b = g_barrel_snap.load(std::memory_order_relaxed);
        b = b < target ? (b + step > target ? target : b + step) : (b - step < target ? target : b - step);
        g_barrel_snap.store(b, std::memory_order_relaxed);
    }
    // lever actions drawn (LeverParts): a flick down opens the lever (it stays open: the slide back, the hammer cocked),
    // a flick up closes it; closed after a shot or a load from empty, the round is chambered (every lever gun: body.cpp
    // draws each one's parts; with LeverParts off they keep the down-then-up flick)
    const bool parts = k == kLever && g_lever_parts.load(std::memory_order_relaxed);
    collect_sounds();
    const int lmode = lever_mode_of(w);  // [Reload] VolcanicControl=stick: the Volcanic by the stick
    // [Reload] LeverControl: the gesture's two strokes this frame (the stick's flick: both, the close a moment later;
    // hold: the gun's flicks with the stick held down)
    const bool stick_flick = k == kLever && g_lever_req.exchange(false);
    const bool held = lmode == 4 && g_lever_hold.load(std::memory_order_relaxed);
    const bool lv_down = lmode == 1 ? stick_flick : lmode == 4 ? held && g_w[0] <= -g_lever_speed : g_w[0] <= -g_lever_speed;
    if (lmode == 1 && stick_flick && g_needs && parts && !g_lever_open) g_lever_stick_close_ms = now + kStickCloseMs;
    const bool lv_up = lmode == 1   ? (g_lever_open && g_lever_stick_close_ms > 0 && now >= g_lever_stick_close_ms)
                       : lmode == 4 ? held && g_w[0] >= g_lever_speed
                                    : g_w[0] >= g_lever_speed;
    if (lmode == 1 && k == kLever && !parts && stick_flick && g_needs && !deadeye) {  // undrawn: the stick's flick works it at once
        g_needs = false;
        g_cycles.fetch_add(1, std::memory_order_relaxed);
        controllers::pulse(gun_h, 0.6f, 20);
        log::info("[actions] lever (%d): worked by the stick's flick", w);
    } else if (k == kLever && (parts || lmode == 4) && !deadeye) {  // hold, drawn or not: two strokes, the lever open between them
        if (!g_lever_open && g_needs && lv_down) {
            g_lever_open = true;
            g_hammer_down = false;
            g_lever_down_ms = now;
            if (pts && bp.hand_ok[gj]) play_at(gun_sounds(w) ? gun_sounds(w)->open : nullptr, bp.hand[gj]);
            controllers::pulse(gun_h, 0.35f, 15);
            log::info("[actions] lever (%d): opened by %s (%.1f rad/s)", w, lmode == 1 ? "the stick's flick" : lmode == 4 ? "a flick down, the stick held" : "a flick down", g_w[0]);
        } else if (g_lever_open && lv_up) {
            g_lever_open = false;
            g_lever_stick_close_ms = 0;
            g_needs = false;
            if (pts && bp.hand_ok[gj]) play_at(gun_sounds(w) ? gun_sounds(w)->close : nullptr, bp.hand[gj]);
            g_cycles.fetch_add(1, std::memory_order_relaxed);
            g_lever_shut_ms = now;
            controllers::pulse(gun_h, 0.6f, 20);
            log::info("[actions] lever (%d): closed %s (%.0f ms open), chambered", w, lmode == 1 ? "after the stick's flick" : lmode == 4 ? "by a flick up, the stick held" : "by a flick up",
                      now - g_lever_down_ms);
        }
    } else if (k == kLever && g_needs && !deadeye && lmode != 1) {  // lever actions: a flick of the gun hand, muzzle down then up, within the window
        if (g_w[0] <= -g_lever_speed) g_lever_down_ms = now;
        if (g_lever_down_ms > 0 && g_w[0] >= g_lever_speed) {
            if (now - g_lever_down_ms <= g_lever_window_ms) {
                g_needs = false;
                g_cycles.fetch_add(1, std::memory_order_relaxed);
                controllers::pulse(gun_h, 0.6f, 20);
                log::info("[actions] lever (%d): worked by a flick (%.0f ms down to up)", w, now - g_lever_down_ms);
            }
            g_lever_down_ms = 0;
        }
    } else if (k == kLever && !g_needs && g_w[0] >= g_lever_speed && g_lever_down_ms > 0) {
        g_lever_down_ms = 0;
    }
    {  // [Reload] LeverControl=lock: the drawn gun's orientation held while the other hand's trigger is held and the lever
       // waits for working (opened and not yet shut, or a shot not cycled); let go, or worked: the gun follows the hand again
        // (with [Reload] PhysicalStock the other trigger holds the stock's foregrip: no lock, the stock is the steady)
        // (nor while that hand's trigger is the round's: TriggerTakesRound, at the ammo zone or carrying the round it took)
        // (2026-11-01, GOAL-offhand-changes C4b; round 65 check 8, the user: the Buffalo should stay still while its lever is
        // worked, as the other lever actions) the Buffalo's lever (C4) held as the lever guns' in each mode: lever_to_work
        const bool lgun = k == kLever || (k == kBreech && w == 14 && kind_on(k));
        const bool lw = lgun && lever_to_work(now, false);
        const bool lk = lmode == 2 && lw && !deadeye && hands::get(off_h).trigger > 0.5f && !holster::physical_stock() &&
                        !holster::trigger_for_round(off_h);
        if (lk != g_lever_lock.load(std::memory_order_relaxed)) log::info("[actions] lever (%d): the gun's orientation %s", w, lk ? "held" : "let go");
        g_lever_lock.store(lk, std::memory_order_relaxed);
        // LeverControl=front: the lever waiting (body.cpp carries the gun in the front hand while held two-handed)
        // (not with [Reload] PhysicalStock: the stock is the steady, and its calibration samples the gun hand's own target)
        g_lever_front.store(lmode == 3 && lw && !deadeye && !holster::physical_stock(), std::memory_order_relaxed);
        // LeverControl=hold: the drawn gun held while the stick is held and the lever is worked (not with the stock: as front)
        const bool hk = lmode == 4 && lgun && lever_to_work(now, true) && !deadeye && held && !holster::physical_stock();
        if (hk != g_lever_hold_lock.load(std::memory_order_relaxed))
            log::info("[actions] lever (%d): the drawn gun %s (LeverControl=hold)", w, hk ? "held while the stick is held" : "let go");
        g_lever_hold_lock.store(hk, std::memory_order_relaxed);
    }
    {  // the lever and hammer as drawn: the lever over 80 ms either way, the hammer with it
        static double last = 0;
        const float step = last > 0 ? static_cast<float>((now - last) / 80.0) : 1.0f;
        last = now;
        auto ease = [step](std::atomic<float>& v, float target) {
            float a = v.load(std::memory_order_relaxed);
            a = a < target ? (a + step > target ? target : a + step) : (a - step < target ? target : a - step);
            v.store(a, std::memory_order_relaxed);
        };
        ease(g_lever_amt, parts && g_lever_open ? 1.0f : 0.0f);
        ease(g_hammer_amt, parts && g_hammer_down && !g_lever_open ? 0.0f : 1.0f);
        g_lever_weapon.store(parts ? w : -1, std::memory_order_relaxed);
    }
    // the pump: two-handed, the front hand back along the barrel, then forward
    const bool pparts = k == kPump && g_pump_parts.load(std::memory_order_relaxed);
    float pd = g_pump_back ? g_pump_amt.load(std::memory_order_relaxed) : 0.0f;  // let go while back: it stays back
    if (k == kPump && pts && holster::two_hand_blend() > 0.5f && bp.hand_ok[gj] && bp.hand_ok[1 - gj] && !deadeye) {
        const float* fh = bp.hand[1 - gj];
        const float s = (fh[0] - bp.hand[gj][0]) * u[0] + (fh[1] - bp.hand[gj][1]) * u[1] + (fh[2] - bp.hand[gj][2]) * u[2];
        g_pump_s = g_pump_s0 - s;
        if (!g_pump_have) {
            g_pump_have = true;
            // [Reload] PumpParts: held again while the fore-end is back, it is where it was left (not shut by the grip)
            g_pump_s0 = pparts && g_pump_back ? s + g_pump_amt.load(std::memory_order_relaxed) * kPumpStroke : s;
        }
        if (pparts && (g_needs || g_pump_back)) {  // unlocked by the shot: the fore-end where the hand has it
            const float b = (g_pump_s0 - s) / kPumpStroke;
            pd = b < 0.0f ? 0.0f : b > 1.0f ? 1.0f : b;
        }
        if (g_needs && !g_pump_back && s <= g_pump_s0 - g_pump_travel) {
            g_pump_back = true;
            controllers::pulse(1 - gun_h, 0.5f, 15);
            if (const GunSounds* gs = gun_sounds(w)) play_at(gs->open, fh);
            log::info("[actions] pump (%d): back %.3f m", w, g_pump_s0 - s);
        } else if (g_pump_back && s >= g_pump_s0 - 0.015f) {
            g_pump_back = false;
            g_needs = false;
            pd = 0.0f;
            g_cycles.fetch_add(1, std::memory_order_relaxed);
            controllers::pulse(1 - gun_h, 0.5f, 15);
            if (const GunSounds* gs = gun_sounds(w)) play_at(gs->close, fh);
            log::info("[actions] pump (%d): forward, chambered", w);
        } else if (!g_needs && s <= g_pump_s0 - g_pump_travel && !g_ooo) {
            g_ooo = true;  // pumped while chambered: nothing (counted once a stroke)
            g_out_of_order.fetch_add(1, std::memory_order_relaxed);
            log::info("[actions] pump (%d): worked while chambered: nothing", w);
        } else if (s >= g_pump_s0 - 0.015f) {
            g_ooo = false;
        }
        if (!g_needs && !g_pump_back) g_pump_s0 = s > g_pump_s0 ? s : g_pump_s0 * 0.98f + s * 0.02f;  // follows the hold
    } else {
        g_pump_have = false;
    }
    // a second gun out (dual wield) leaves no hand for the pump: it is not held then (review 2)
    // (r265: not a carry, whose hand-over has the gun still in hand a moment: CarriedPump keeps its shot not pumped)
    if (k == kPump && g_needs && dual::state().on && !dual::state().carry) g_needs = false;
    if (!g_needs && !g_pump_back) pd = 0.0f;
    g_pump_amt.store(pparts ? pd : 0.0f, std::memory_order_relaxed);
    g_pump_weapon.store(pparts ? w : -1, std::memory_order_relaxed);
    // the hold: open or not cycled; a pull while held is counted (a faint buzz), and after it a fresh pull is needed
    // (PistolSlides: nothing chambered, or the slide back; the Mauser's bolt open with PistolMagazines too)
    const bool block = !deadeye && (g_open || g_needs || (bparts && (blift > 0.05f || bslide > 0.0f)) || (brz && (g_breech_open || bdrawn > 0.05f)) ||
                                    (sparts && sdrawn > 0.05f) || (pparts_on && (g_locked || g_pdrawn > 0.05f || (pslides && !g_chambered))));
    // the menu's clicks are not pulls (round 10: three refusals counted while the menu was open)
    const bool trig = gh.trigger > 0.6f && !menu::visible() && !whistle::suppressed(gun_h);  // run 8: not the whistle's
    if (block && trig && !g_trig_was) {
        g_held.fetch_add(1, std::memory_order_relaxed);
        controllers::pulse(gun_h, 0.45f, 35);  // felt, and unlike the empty click (round 10: an open full revolver felt dead)
        // round 13 item 13: "guns should click when attempting to fire without having performed the necessary reload
        // actions yet": the gun's family's click too ([Reload] EmptyClick's mode, volume and sounds). Not while it is
        // open (round 10: "an open revolver sounded empty": a buzz only; run 4's check, item8_actions, run 7's G2)
        if (g_held_click.load(std::memory_order_relaxed) && !g_open) audio::empty_click(gun_h, w);
        log::info("[actions] fire held back: the %s %s (pulls %llu)", kind_name(k),
                  g_open || (bparts && g_bolt_up) || (pparts_on && (g_locked || g_pdrawn > 0.05f)) ? "is open"
                  : pparts_on && pslides && !g_chambered                                         ? "has nothing chambered"
                                                                                                  : "is not worked",
                  static_cast<unsigned long long>(g_held.load()));
    }
    if (block && trig) g_rt_rearm = true;
    if (g_rt_rearm && gh.trigger < 0.15f) g_rt_rearm = false;
    g_trig_was = trig;
    // the fanned shot waiting: the held pull let go one frame and pulled again until the game fires it
    if (g_fan_low > 0) --g_fan_low;
    if (g_fan_pending && aim::player_shots() != g_fan_pend_shots) {  // the game made the shot (its count may not show it yet)
        if (now - g_fan_pend_ms > 30.0) g_fan_late.fetch_add(1, std::memory_order_relaxed);
        g_fan_pending = false;
        g_fan_low = 0;
    }
    if (g_fan_pending) {
        if (!trig || block || deadeye || now - g_fan_pend_ms > kFanWaitMs) {
            g_fan_pending = false;
        } else if (now - g_fan_retry_ms >= kFanRetryMs) {
            g_fan_retry_ms = now;
            g_fan_low = 2;
            g_fan_retries.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_blocked = block || g_rt_rearm.load() || g_fan_low > 0;
}

}  // namespace

void init() {
    g_on = config::get_bool("Reload", "ManualActions", false);
    g_revolver = config::get_bool("Reload", "ManualRevolver", true);
    g_bolt = config::get_bool("Reload", "ManualBolt", true);
    g_lever = config::get_bool("Reload", "ManualLever", true);
    g_pump = config::get_bool("Reload", "ManualPump", true);
    g_load_open = config::get_bool("Reload", "LoadOnlyWhenOpen", true);
    g_chamber_load = config::get_bool("Reload", "ChamberAfterLoad", true);
    g_open_y = config::get_float("Reload", "OpenStickY", 0.85f);
    g_flick_ms = config::get_float("Reload", "OpenFlickMs", 300.0f);
    g_hints = config::get_bool("Reload", "ActionHints", true);
    g_fan = config::get_bool("Reload", "FanHammer", true);
    g_thumb_cfg = config::get_bool("Reload", "ThumbCock", true);
    {
        const std::string fz = config::get_string("Reload", "FanZoneOffset", "0 0 0");
        sscanf_s(fz.c_str(), "%f %f %f", &g_fan_off[0], &g_fan_off[1], &g_fan_off[2]);
        for (float& x : g_fan_off)
            if (!std::isfinite(x) || std::fabs(x) > 0.3f) x = 0.0f;
        const float r = config::get_float("Reload", "FanZoneRadius", 0.08f), s = config::get_float("Reload", "FanSpeed", 1.0f);
        g_fan_r = r >= 0.02f && r <= 0.3f ? r : 0.08f;
        g_fan_speed = s >= 0.2f && s <= 6.0f ? s : 1.0f;
    }
    g_open_cyl = config::get_bool("Reload", "OpenCylinder", false);
    g_lever_parts = config::get_bool("Reload", "LeverParts", false);
    {
        // (2026-11-02, GOAL-older-items item 3; the user: the hold "is the best one. Remove the other options") the lever by
        // the right stick held, the gun down then up (hold), the only way now: no menu row nor ini entry; the key is read
        // only for the tests' inis (their scripted flicks: flick, stick, lock, front), with [Debug] LeverModes=1 (the review:
        // a LeverControl a player once saved from the old menu would win, with no row left to change it)
        const std::string lc = config::get_bool("Debug", "LeverModes", false) ? config::get_string("Reload", "LeverControl", "hold") : std::string("hold");
        g_lever_mode = lc == "stick" ? 1 : lc == "lock" ? 2 : lc == "front" ? 3 : lc == "hold" ? 4 : 0;
    }
    g_volc_stick = config::get_string("Reload", "VolcanicControl", "lever") == "stick";
    g_rev_mode = config::get_string("Reload", "RevolverControl", "open") == "cock" ? 1 : 0;
    g_pump_parts = config::get_bool("Reload", "PumpParts", false);
    g_bolt_parts = config::get_bool("Reload", "BoltParts", false);
    g_carried_bolt = config::get_bool("Reload", "CarriedBolt", false);
    g_cb_trigger = config::get_bool("Reload", "CarriedBoltTrigger", true);
    g_ob_trigger = config::get_bool("Reload", "OffHandBoltTrigger", true);
    g_rod_chest = config::get_bool("Reload", "RamrodAtChest", true);
    for (int hs = 0; hs < 2; ++hs) {  // (C15d) RamrodOffset/Turn/Tilt (the left's, and every hand's), ...Right (the right's own once set)
        const char* sfx = hs == 1 ? "Right" : "";
        const std::string os = config::get_string("Reload", (std::string("RamrodOffset") + sfx).c_str(), hs ? "" : "0 -0.04 0");
        // (the review: the right's own when any of its keys is there)
        if (hs == 1 && os.empty() && config::get_string("Reload", "RamrodTurnRight", "").empty() && config::get_string("Reload", "RamrodTiltRight", "").empty())
            continue;
        RodPlace p{{0.0f, -0.04f, 0.0f}, 0.0f, 0.0f};
        sscanf_s(os.c_str(), "%f %f %f", &p.offset[0], &p.offset[1], &p.offset[2]);
        p.turn = config::get_float("Reload", (std::string("RamrodTurn") + sfx).c_str(), 0.0f);
        p.tilt = config::get_float("Reload", (std::string("RamrodTilt") + sfx).c_str(), 0.0f);
        set_ramrod_place(hs, p, false);
    }
    g_rod_from_gun = config::get_string("Reload", "RamrodFrom", "gun") != "chest";
    {
        float go[3] = {0.0f, 0.0f, 0.0f};
        sscanf_s(config::get_string("Reload", "RamrodGrabOffset", "0 0 0").c_str(), "%f %f %f", &go[0], &go[1], &go[2]);
        set_ramrod_grab_offset(go, false);
    }
    g_carried_pump = config::get_bool("Reload", "CarriedPump", false);
    g_carried_any = config::get_bool("Reload", "CarriedAnyState", true);
    g_cp_speed = std::fmin(5.0f, std::fmax(0.5f, config::get_float("Reload", "CarriedPumpSpeed", 1.5f)));
    g_breech_parts = config::get_bool("Reload", "BreechParts", false);
    g_semi_parts = config::get_bool("Reload", "SemiAutoParts", false);
    g_mags = config::get_bool("Reload", "PistolMagazines", false);
    g_slides = config::get_bool("Reload", "PistolSlides", false);
    g_break = config::get_bool("Reload", "ManualBreak", false);
    g_break_click = config::get_bool("Reload", "BreakOpenClick", true);
    g_ride_cock = config::get_bool("Horse", "LeftFlickCocks", true);
    g_barrel_r = config::get_float("Reload", "BarrelGripRadius", 0.15f);
    g_barrel_snap_cfg = config::get_bool("Reload", "BarrelHandSnap", true);
    g_part_snap_cfg = config::get_bool("Reload", "PartHandSnap", true);
    g_held_click = config::get_bool("Reload", "HeldClick", true);
    g_dual_open = config::get_bool("Hands", "DualWieldOpen", false);
    g_open_wait_ms = clamp_wait(config::get_float("Hands", "DualWieldOpenWaitMs", 250.0f), 0.0f, 600.0f, 250.0f);
    g_click_hold_ms = clamp_wait(config::get_float("Hands", "DualWieldClickHoldMs", 200.0f), 0.0f, 600.0f, 200.0f);
    g_close_speed = config::get_float("Reload", "CloseFlickSpeed", 6.0f);
    g_close_min_ms = config::get_float("Reload", "CloseFlickMinOpenMs", 300.0f);
    g_bolt_r = config::get_float("Reload", "BoltZoneRadius", 0.07f);
    g_bolt_travel = config::get_float("Reload", "BoltTravel", 0.06f);
    g_lever_speed = config::get_float("Reload", "LeverFlickSpeed", 5.0f);
    g_lever_window_ms = config::get_float("Reload", "LeverFlickWindowMs", 400.0f);
    g_pump_travel = config::get_float("Reload", "PumpTravel", 0.06f);
    {
        const std::string bz = config::get_string("Reload", "BoltZoneOffset", "0.035 0.030 0.090");
        sscanf_s(bz.c_str(), "%f %f %f", &g_bolt_off[0], &g_bolt_off[1], &g_bolt_off[2]);
    }
    log::info("[actions] manual actions %d (revolver %d, bolt %d, lever %d by flick, pump %d; the bow: not in the game data)", g_on.load() ? 1 : 0,
              g_revolver.load() ? 1 : 0, g_bolt.load() ? 1 : 0, g_lever.load() ? 1 : 0, g_pump.load() ? 1 : 0);
    d3d::add_frame_end_listener([](uint64_t) { frame(); });
}

bool enabled() { return g_on.load(); }
void set_enabled(bool on) {
    if (g_on.exchange(on) != on) log::info("[actions] manual actions %d", on ? 1 : 0);
    config::set("Reload", "ManualActions", on ? "1" : "0");
}

float open_stick_y() { return g_open_y; }
bool second_open() { return g_sec_open.load(std::memory_order_relaxed); }
float open_flick_ms() { return g_flick_ms; }

// (2026-10-31, the off-hand build step 4, S2; 56.4, the user: "If gun is in off hand it does not cock"; the user's answer 1:
// each gun worked by the stick of the hand holding it) the second gun (or the copy) in the layout's gun-hand controller,
// the gun in hand drawn by the other (whose stick, filter_second_stick's main_here, works it): this stick works the second
// gun as its own stick would (r79 s1: it locked after one shot, this stick working nothing): held down and clicked opens
// it, a quick flick down cocks it. True when it did (filter_stick then leaves the stick alone otherwise)
bool second_on_gun_stick(float* rx, float* ry, bool* click, bool seated) {
    static bool s_held = false, s_click_ours = false, s_click_was = false, s_armed = true;
    static double s_t0 = 0.0;
    const dual::State ds = dual::state();
    const Kind k = kind_of(ds.weapon);
    const bool sopen = sec_opens(k) && kind_on(k);
    const bool here = g_dual_open.load(std::memory_order_relaxed) && ds.on && !ds.carry && ds.ready && (sopen || sec_volcanic(ds.weapon)) &&
                      ds.ctrl == controls::layout_gun_hand() && controls::gun_hand() != controls::layout_gun_hand() && !menu::visible() &&
                      (!seated || g_ride_cock.load(std::memory_order_relaxed));  // (the review: riding without LeftFlickCocks this stick works the gun in hand)
    const bool clicked = click && *click;
    if (!here) {
        s_held = s_click_ours = false;
        s_t0 = 0.0;
        s_armed = true;
        s_click_was = clicked;
        return false;
    }
    const float x = *rx, y = *ry;
    const bool cone = y < 0.0f && std::fabs(x) <= 0.6f * std::fabs(y);
    s_held = cone && y <= -g_open_y ? true : std::fabs(y) < 0.3f ? false : s_held;
    if (!clicked) s_click_ours = false;
    if (sopen && s_held && clicked && !s_click_was && !s_click_ours) {  // (F3: the Volcanic's click the game's)
        if (!g_sec_open.load(std::memory_order_relaxed)) g_sec_open_req = true;
        g_sec_clicks.fetch_add(1, std::memory_order_relaxed);
        s_click_ours = true;
        s_t0 = 0.0;  // (no flick follows a click)
        s_armed = false;
    }
    s_click_was = clicked;
    if (s_click_ours && click) *click = false;  // the gun's until let go: never the game's R3 (Dead Eye)
    const double now = log::now_ms();
    const float fms = g_flick_ms > 0.0f ? g_flick_ms : 300.0f;
    if (cone && y <= -g_open_y && s_armed && !clicked) {
        s_armed = false;
        s_t0 = now;
    }
    if (s_t0 > 0.0 && now - s_t0 > fms) s_t0 = 0.0;
    if (std::sqrt(x * x + y * y) < 0.3f) {
        if (s_t0 > 0.0 && !s_click_ours) g_sec_cock_req = true;
        s_t0 = 0.0;
        s_armed = true;
    }
    if (cone && std::fabs(y) > 0.5f) *rx = *ry = 0.0f;  // a mostly-down push neither turns nor pitches the camera
    return true;
}

void filter_stick(float* rx, float* ry, bool* click, bool seated) {
    if (!g_on.load(std::memory_order_relaxed) || !rx || !ry) return;
    if (second_on_gun_stick(rx, ry, click, seated)) return;  // (S2) the second gun's, in this controller
    const Kind k = kind_of(g_weapon);  // the frame end's weapon (the same thread)
    const int lmode = lever_mode_of(g_weapon);
    // (C4) the Buffalo's lever (BreechParts' gesture 0) as the lever guns': by the stick, or the stick held
    const bool lev = (k == kLever && kind_on(k)) || (g_weapon == 14 && k == kBreech && kind_on(k));
    const bool lever = lev && lmode == 1;  // [Reload] LeverControl=stick (VolcanicControl=stick)
    const bool hold = lev && lmode == 4;   // LeverControl=hold
    // held down: past OpenStickY in the cone, until it comes back toward the centre (|y| < 0.3) (below; cleared while the
    // off controller's stick has the gun: the review, not stale when it comes back)
    static bool s_held = false;
    // (2026-10-30, round 55) a revolver drawn into the layout's off controller (DrawToGrabbingHand, the gun hand empty) is
    // worked by that controller's stick (filter_second_stick), not this one; riding, that stick steers the horse, so this one
    // still works it (the review), unless [Horse] LeftFlickCocks gives it that stick riding too
    const bool rev_off = (!seated || g_ride_cock.load(std::memory_order_relaxed)) && (k == kRevolver || k == kTopBreak) && controls::gun_hand() != controls::layout_gun_hand();
    // (2026-10-31, the off-hand build step 4, A3) a lever (LeverControl=stick), a magazine pistol or a break-open drawn into the
    // off controller: that controller's stick works it (filter_second_stick's main_here), not this one
    // (2026-11-01, GOAL-offhand-changes C20b; round 65 check 4, the user: the left-hand lever readied by the left stick) and
    // LeverControl=hold's: that stick held down (filter_second_stick's m_hold)
    if ((!seated || g_ride_cock.load(std::memory_order_relaxed)) && controls::gun_hand() != controls::layout_gun_hand() &&
        ((lev && (lmode == 1 || lmode == 4)) || ((k == kPistol || k == kMauser) && g_mags.load(std::memory_order_relaxed)) ||
         (k == kBreak && kind_on(k)))) {
        s_held = false;
        g_stick_armed = true;
        g_click_was = click && *click;
        return;
    }
    // [Reload] RevolverControl=cock: the flick cocks (the single actions); held down, a click opens (every revolver)
    const bool cock = !rev_off && (k == kRevolver || k == kTopBreak) && kind_on(k) && g_rev_mode.load(std::memory_order_relaxed) == 1;
    // [Reload] PistolMagazines: the flick drops a pistol's magazine (the Mauser's: pulls its clip out)
    const bool mag = (k == kPistol || k == kMauser) && g_mags.load(std::memory_order_relaxed);
    // [Reload] BreakOpenClick: a break-open as the revolvers' cock mode (held down + click opens; no flick)
    const bool brk = k == kBreak && kind_on(k) && g_break_click.load(std::memory_order_relaxed);
    const float x = *rx, y = *ry;
    const bool cone = y < 0.0f && std::fabs(x) <= 0.6f * std::fabs(y);
    s_held = (hold || cock || brk) && (cone && y <= -g_open_y ? true : std::fabs(y) < 0.3f ? false : s_held);
    g_lever_hold.store(hold && s_held, std::memory_order_relaxed);
    g_rev_hold.store(cock && s_held, std::memory_order_relaxed);
    const bool clicked = click && *click;
    static bool s_click_ours = false;  // a click taken by the revolver stays its own until let go (the review: the stick
                                       // back at the centre first would hand the game a fresh R3, its Dead Eye)
    if (!clicked) s_click_ours = false;
    if ((cock || brk) && s_held && click) {
        if (clicked && !g_click_was) {
            g_open_req = true;  // held down and clicked: opened for loading (no flick follows it)
            g_flick_t0 = 0.0;
            g_stick_armed = false;
            s_click_ours = true;
        }
        *click = false;  // the click while held is the revolver's (not the game's Dead Eye)
    }
    if (s_click_ours && click) *click = false;
    g_click_was = clicked;
    if (hold || brk) {  // the stick's push is the lever's hold (or the break-open's, its click opening) only: it neither turns nor pitches the camera
        g_stick_armed = true;
        if (cone && std::fabs(y) > 0.5f) *rx = *ry = 0.0f;
        return;
    }
    if (!(opens(k) && kind_on(k) && !rev_off) && !lever && !mag) {
        g_stick_armed = true;
        return;
    }
    std::atomic<bool>& req = lever ? g_lever_req : cock ? g_cock_req : mag ? g_mag_req : g_open_req;
    // a flick: past OpenStickY and back to the centre within OpenFlickMs; a held push opens nothing (round 10: a full
    // revolver opened by a push down, probably not meant). OpenFlickMs 0: the push itself opens, as in run 3.
    const double now = log::now_ms();
    // RevolverControl=cock: the cock is always a flick (with OpenFlickMs 0 the push of a hold-and-click would cock first)
    const float flick_ms = cock && g_flick_ms <= 0.0f ? 300.0f : g_flick_ms;
    if (cone && y <= -g_open_y && g_stick_armed) {
        g_stick_armed = false;
        if (flick_ms <= 0.0f)
            req = true;
        else
            g_flick_t0 = now;
    }
    if (g_flick_t0 > 0.0 && now - g_flick_t0 > flick_ms) g_flick_t0 = 0.0;  // held too long: not a flick
    if (std::fabs(y) < 0.3f) {
        if (g_flick_t0 > 0.0) req = true;
        g_flick_t0 = 0.0;
        g_stick_armed = true;
    }
    if (cone && std::fabs(y) > 0.5f) *rx = *ry = 0.0f;  // a mostly-down push neither turns nor pitches the camera
}

void filter_second_stick(int ctrl, float* sx, float* sy, bool* click, bool other_click, bool seated, bool skip) {
    static bool s_held = false, s_click_ours = false, s_click_was = false, s_zero = false;
    static bool s_hold_ours = false, s_lheld = false;  // (C20b) g_lever_hold written by this stick (put back to false once not); its hold
    static double s_down_ms = -1.0;     // when the stick went down into the cone (armed, on foot), -1: not held there
    static bool s_down_pass = false;    // its walk passed: held down past the wait, or walking already (until it centres)
    static double s_out_ms = -1.0;      // when the stick (off the centre, not passed) went outside the cone, -1: not there
    static double s_press_ms = -1.0;    // an armed press of the click held back since, -1: none
    static bool s_press_pass = false;   // held in past the wait (or a chord's): passed on
    static double s_pass_until = 0.0;   // a passed press kept down at least until then (kMinPassMs)
    static bool s_flick = false;        // (round 40) down past OpenStickY in the cone, not passed, no click: a cock if it centres now
    constexpr double kMinPassMs = 120.0;  // (as the chord's tap: a click just past the wait reaches the game whole)
    const double now = log::now_ms();
    if (skip) {  // (the review) not filtered now (the wheel's D-pad): nothing held back, nothing to come late
        if (s_hold_ours) g_lever_hold.store(false, std::memory_order_relaxed);  // (C20b, the review: not left held)
        s_hold_ours = s_lheld = false;
        s_held = s_click_ours = s_zero = s_down_pass = s_press_pass = s_flick = false;
        s_down_ms = s_press_ms = s_out_ms = -1.0;
        s_pass_until = 0.0;
        s_click_was = click && *click;
        g_press_pending.store(false, std::memory_order_relaxed);
        g_click_waited.store(false, std::memory_order_relaxed);
        return;
    }
    const bool clicked = click && *click;
    bool armed = false, sec_lev = false, sec_mag = false;  // (F3: armed for the Volcanic's flick; its click the game's, never an open)
    if (g_on.load(std::memory_order_relaxed) && g_dual_open.load(std::memory_order_relaxed) && sx && sy) {
        const dual::State ds = dual::state();
        const Kind k = kind_of(ds.weapon);
        const bool rev = ds.on && !ds.carry && ds.ready && sec_opens(k) && kind_on(k);
        if (rev && ds.ctrl != ctrl && !g_sec_refuse_said.exchange(true)) {
            g_sec_refused.fetch_add(1, std::memory_order_relaxed);
            log::info("[actions] DualWieldOpen: %s is in the %s controller: worked by that controller's stick %s",
                      ds.copy ? "the copy" : "the second gun", ds.ctrl ? "right" : "left",
                      controls::gun_hand() != controls::layout_gun_hand() ? "(the gun in hand drawn by this one: this stick works it)"
                                                                          : "only with the gun in hand drawn by this one (S2)");
        }
        // (open: the gesture taken all the same, a no-op; seated too: the brake waits as the crouch does)
        sec_lev = ds.on && !ds.carry && ds.ready && sec_volcanic(ds.weapon) && ds.ctrl == ctrl;
        sec_mag = second_mag_gun(ds) && ds.ctrl == ctrl;  // (C7) a magazine pistol's flick drops its magazine
        armed = (rev || sec_lev || sec_mag) && ds.ctrl == ctrl && !menu::visible();
    }
    // (2026-10-30, round 55, the user: "If off hand grabs a revolver with main hand empty. Off hand is unable to cock hammer
    // or open revolver") the gun in hand, a revolver, drawn into this stick's controller (DrawToGrabbingHand, the layout's
    // gun hand empty): its flick (RevolverControl=cock: the cock; open: the opening) and its hold-and-click open are this
    // stick's, filter_stick's requests, with the second gun's walk wait; not riding (the stick the horse's, as the second's)
    bool main_here = false;
    // ([Horse] LeftFlickCocks: riding too; off, riding, filter_stick's as the review had it)
    const bool ride_ok = !seated || g_ride_cock.load(std::memory_order_relaxed);
    // (2026-10-31, the off-hand build step 4, A3) and a lever (LeverControl=stick), a magazine pistol (PistolMagazines) or a
    // break-open (ManualBreak; BreakOpenClick: its click, not a flick) drawn into this controller: as the revolver
    const Kind mk = kind_of(g_weapon);  // (the frame end's weapon: the same thread, as filter_stick's)
    // (C4, the review: the Buffalo's lever too, drawn by the off controller)
    const bool m_lev = (mk == kLever && kind_on(mk)) || (mk == kBreech && g_weapon == 14 && kind_on(mk));
    const bool m_lever = m_lev && lever_mode_of(g_weapon) == 1;
    // (C20b) LeverControl=hold: this stick held down is the hold (g_lever_hold), the gun's flicks work the lever; held while the
    // lever is to be worked, John does not walk back (else the stick walks as it does)
    const bool m_hold = m_lev && lever_mode_of(g_weapon) == 4;
    const bool m_mag = (mk == kPistol || mk == kMauser) && g_mags.load(std::memory_order_relaxed);
    const bool m_brk = mk == kBreak && kind_on(mk);
    if (g_on.load(std::memory_order_relaxed) && sx && sy && !armed && ride_ok) {
        main_here = (((mk == kRevolver || mk == kTopBreak) && kind_on(mk)) || m_lever || m_mag || m_brk || m_hold) && controls::gun_hand() == ctrl &&
                    ctrl != controls::layout_gun_hand() && !menu::visible();
        armed = main_here && !m_hold;  // (the hold: no flick, click or walk wait of its own)
    }
    const bool hold_here = main_here && m_hold;
    if (hold_here || s_hold_ours) {
        const float hx = sx ? *sx : 0.0f, hy = sy ? *sy : 0.0f;
        const bool hcone = hy < 0.0f && std::fabs(hx) <= 0.6f * std::fabs(hy);
        s_lheld = hold_here && (hcone && hy <= -g_open_y ? true : std::fabs(hy) < 0.3f ? false : s_lheld);
        g_lever_hold.store(s_lheld, std::memory_order_relaxed);
        s_hold_ours = hold_here;
        if (hold_here && hcone && std::sqrt(hx * hx + hy * hy) >= 0.3f && lever_to_work(now, true) && sx && sy) *sx = *sy = 0.0f;  // (no walk back)
    }
    const bool main_cock = main_here && (mk == kRevolver || mk == kTopBreak) && g_rev_mode.load(std::memory_order_relaxed) == 1;
    // the main gun's flick here: the lever, the magazine, the revolver's cock or open; a click-opened break-open none
    auto main_flick = [&]() {
        if (m_lever) g_lever_req = true;
        else if (m_mag) g_mag_req = true;
        else if (m_brk) { if (!g_break_click.load(std::memory_order_relaxed)) g_open_req = true; }
        else (main_cock ? g_cock_req : g_open_req) = true;
    };
    // (the review: with DualWieldOpenWaitMs 0 the walk passes in the flick's own poll and no flick were left) the gun in
    // hand's flick has 150 ms at least
    const float open_wait = main_here ? std::fmax(150.0f, g_open_wait_ms.load(std::memory_order_relaxed)) : g_open_wait_ms.load(std::memory_order_relaxed);
    g_sec_armed.store(armed, std::memory_order_relaxed);
    const float x = sx ? *sx : 0.0f, y = sy ? *sy : 0.0f;
    const bool cone = y < 0.0f && std::fabs(x) <= 0.6f * std::fabs(y);
    // held down: past OpenStickY in the cone, until it comes back toward the centre (|y| < 0.3), as filter_stick's
    s_held = cone && y <= -g_open_y ? true : std::fabs(y) < 0.3f ? false : s_held;
    if (!clicked) s_click_ours = false;
    // held down and clicked: opened (the press's, not a click held before); or (the review) a click held back (pending, within
    // its wait) and then the stick down to the hold: the same open
    const bool pending = s_press_ms >= 0.0 && !s_press_pass;
    // (the review: a lever's or a magazine pistol's held click is the game's, never the second gun's open)
    if (armed && s_held && clicked && (!s_click_was || pending) && !s_click_ours && !(main_here && (m_lever || m_mag || m_hold)) && !sec_lev) {
        if (main_here) g_open_req = true;  // (the frame end: open already, nothing)
        else if (!g_sec_open.load(std::memory_order_relaxed)) g_sec_open_req = true;  // (open already: nothing, but no L3 or walk back)
        if (!main_here) g_sec_clicks.fetch_add(1, std::memory_order_relaxed);
        s_click_ours = true;
        s_zero = true;
    }
    // (2026-10-29, the user: "Make it so you don't start moving backwards until stick is held flicked down for longer than a
    // quick flick takes. Same with holding it in to crouch and brake on horse") armed only: a click reaches the game (L3:
    // the crouch on foot, the brake riding with StickClickBrake) once held in past DualWieldClickHoldMs, then as held (and
    // kMinPassMs at least); a quick click let go before sends nothing. A press with the other stick's click already down (the
    // recentre chord): at once, as before. A click begun unarmed is the game's as before
    if (!clicked && pending) g_sec_click_dropped.fetch_add(1, std::memory_order_relaxed);  // a quick one: nothing sent
    const bool click_armed = armed && !sec_lev;  // (F3: the Volcanic opens by no click: its click the game's at once)
    if (!clicked || !click_armed || s_click_ours) {
        if (!(s_press_pass && now < s_pass_until && !s_click_ours)) {  // (a passed press kept down its least, let go or not)
            s_press_ms = -1.0;
            s_press_pass = false;
        }
    }
    if (!clicked && !(s_press_pass && now < s_pass_until)) g_click_waited.store(false, std::memory_order_relaxed);
    if (click_armed && clicked && !s_click_was && !s_click_ours) {
        s_press_ms = now;
        s_press_pass = other_click;
        s_pass_until = other_click ? now + kMinPassMs : 0.0;
    }
    if (s_press_ms >= 0.0 && !s_press_pass) {
        if (now - s_press_ms >= g_click_hold_ms.load(std::memory_order_relaxed)) {
            s_press_pass = true;
            s_pass_until = now + kMinPassMs;
            g_click_waited.store(true, std::memory_order_relaxed);  // (controls.cpp: this press waited already: no chord wait on top)
            g_sec_click_passed.fetch_add(1, std::memory_order_relaxed);
        } else if (click) {
            *click = false;
        }
    }
    if (click && s_press_ms >= 0.0 && s_press_pass && !clicked && now < s_pass_until) *click = true;  // (its least length)
    if (s_click_ours && click) *click = false;  // the gun's until let go: never the game's L3
    g_press_pending.store(s_press_ms >= 0.0 && !s_press_pass, std::memory_order_relaxed);  // (controls.cpp: left_click_held)
    s_click_was = clicked;
    if (s_zero) {  // the hold's walk back stopped from the click until the stick comes back
        if (std::fabs(y) < 0.3f) {
            s_zero = false;
        } else if (sx && sy) {
            *sx = *sy = 0.0f;
            g_sec_zeroed.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // armed, on foot: the stick in the open cone (off the centre: its length 0.3 or more) held at the middle for
    // DualWieldOpenWaitMs (a quick flick's time) before its walk back passes; back to the middle sooner, nothing went out.
    // Outside the cone it is never held, and once it walks there (kOutMs) or the wait has passed it keeps
    // passing until it comes back near the centre (the review: steering back-left past the cone's edge and back is not held
    // again; the re-check: a flick's first poll a little sideways, (0.25, -0.2), is not a walk, so the flick is still held).
    // Riding the same with [Horse] LeftFlickCocks; without it the stick is the horse's as before (no wait)
    constexpr double kOutMs = 100.0;
    const float len = std::sqrt(x * x + y * y);
    // (round 40) the second gun's cock: a quick flick down, back to the middle before its walk passed, no click meanwhile
    if (armed && ride_ok && cone && y <= -g_open_y && !s_down_pass && !s_click_ours && !clicked) s_flick = true;
    if (s_flick && (s_down_pass || clicked || s_click_ours || !armed || !ride_ok)) s_flick = false;
    if (s_flick && len < 0.3f) {
        s_flick = false;
        if (main_here) main_flick();
        else if (sec_mag) g_smag_req = true;
        else g_sec_cock_req = true;
    }
    if (!armed || !ride_ok || len < 0.3f) {  // (LeftFlickCocks: riding, the horse's pull back waits as the walk does)
        s_down_ms = -1.0;
        s_down_pass = false;
        s_out_ms = -1.0;
    } else if (!s_down_pass && !s_zero && sx && sy) {
        if (!cone) {  // outside the cone: passed as it is; a walk there (a while: a fast flick's first poll far out and
            // sideways is not one, the re-check) passes until it centres
            if (s_out_ms < 0.0) s_out_ms = now;
            if (now - s_out_ms >= kOutMs) {
                s_down_pass = true;
                s_down_ms = -1.0;
            }
        } else {
            s_out_ms = -1.0;
            if (s_down_ms < 0.0) {
                s_down_ms = now;
                g_sec_walk_waits.fetch_add(1, std::memory_order_relaxed);
            }
            if (now - s_down_ms >= open_wait) {
                s_down_pass = true;
                g_sec_walk_passed.fetch_add(1, std::memory_order_relaxed);
            } else {
                *sx = *sy = 0.0f;
            }
        }
    }
}
bool second_click_waited() { return g_click_waited.load(std::memory_order_relaxed); }
bool second_press_pending() { return g_press_pending.load(std::memory_order_relaxed); }
// (2026-10-31, the off-hand build step 3, R17) the carried gun takes rounds now, by its kind as carried: a lever, pump or
// semi-auto's tube always; a bolt gun with its bolt back (CarriedBolt's state); a breech open and empty (CarriedAnyState's);
// a break-open never (it is never carried open)
bool muzzle_loader(int weapon) { return g_on.load(std::memory_order_relaxed) && kind_of(weapon) == kMuzzle && kind_on(kMuzzle); }
bool can_load_carried(int weapon) {
    if (!g_on.load(std::memory_order_relaxed) || !g_load_open.load(std::memory_order_relaxed)) return true;
    const Kind k = kind_of(weapon);
    std::lock_guard lock(g_mutex);
    if (k == kBolt && kind_on(k) && g_bolt_parts.load(std::memory_order_relaxed))
        return g_cb_active && g_cb_weapon == weapon && g_cbs.slide >= 0.9f;
    if (k == kBreech && kind_on(k)) return g_gc_active && g_gc.weapon == weapon && g_gc.breech_open && !g_gc.breech_round;
    if (k == kBreak && kind_on(k)) return false;
    // (C16c, the review) the carried muzzle-loader: one round, while it wants one
    if (k == kMuzzle && kind_on(k)) return g_gc_active && g_gc.weapon == weapon && g_gc.needs && !g_gc.muz_round;
    return true;
}
void carried_round_in(int weapon) {
    std::lock_guard lock(g_mutex);
    if (g_gc_active && g_gc.weapon == weapon && kind_of(weapon) == kBreech) g_gc.breech_round = true;  // (the review: shut, it is ready)
    if (g_gc_active && g_gc.weapon == weapon && kind_of(weapon) == kMuzzle) g_gc.muz_round = true, g_gc.muz_strokes = 0;  // (C16c: now to ram)
}
bool second_mag_pistol(const dual::State& ds) { return second_mag_gun(ds); }
bool second_mag_wanted(const dual::State& ds) {
    return second_mag_gun(ds) && g_smag_out[ds.weapon] && ds.clip + 0.5f <= ds.clip_max;
}
int second_mag_insert(const dual::State& ds, int32_t actor) {
    if (!second_mag_wanted(ds)) return 0;
    const int in = ds.copy ? dual::load_copy_rounds(static_cast<int>(ds.clip_max + 0.01f), actor)
                           : dual::load_second(static_cast<int>(ds.clip_max + 0.01f), actor);
    if (in <= 0) return in;  // (-1: the copy's spare settling: kept, tried again; 0: none)
    g_smag_out[ds.weapon] = false;
    float at[3];
    if (body_hand(ds.ctrl, at)) play_at("ARMOR_METAL_SHORT_MASTER", at);
    log::info("[actions] pistol (%d, the second gun): a magazine in (%d rounds)", ds.weapon, in);
    return in;
}
bool can_load_second(const dual::State& ds) {
    if (ds.carry) return can_load_carried(ds.weapon);  // (R17: the carried gun)
    if (second_mag_gun(ds)) return false;  // (C7) a magazine pistol takes no rounds alone: its magazine (second_mag_insert)
    if (!ds.copy && kind_of(ds.weapon) == kBolt && g_on.load(std::memory_order_relaxed) && g_load_open.load(std::memory_order_relaxed)) {
        std::lock_guard lock(g_mutex);  // (answer 2) a long bolt second gun: rounds only with its bolt back
        if (g_sb_active && g_sb_weapon == ds.weapon) return g_sbs.slide >= 0.9f;
    }
    if (!ds.copy && kind_of(ds.weapon) == kMuzzle && g_on.load(std::memory_order_relaxed) && g_breech_parts.load(std::memory_order_relaxed)) {
        std::lock_guard lock(g_mutex);  // (step 8) the Blunderbuss second gun: one round, after its shot, before the ram
        if (g_smz_active) return g_smz.needs && !g_smz.round;  // (the review: else as before, beside the torch say)
    }
    const bool before = ds.copy || can_load(ds.weapon);  // (as before: another slot's revolver refused, the copy ungated)
    if (!g_on.load(std::memory_order_relaxed) || !g_dual_open.load(std::memory_order_relaxed) || !g_load_open.load(std::memory_order_relaxed)) return before;
    const Kind k = kind_of(ds.weapon);
    if (!sec_opens(k) || !kind_on(k)) return before;
    // (the review) a second gun on the gun stick's controller can never be opened by its stick: the old gate for it
    // (S2, the S4 review) unless the gun in hand is drawn into the other controller (second_on_gun_stick's: this stick works it)
    if (ds.ctrl == controls::layout_gun_hand() && controls::gun_hand() == controls::layout_gun_hand()) return before;
    return g_sec_open.load(std::memory_order_relaxed) && g_sec_open_weapon.load(std::memory_order_relaxed) == ds.weapon &&
           g_sec_open_ctrl.load(std::memory_order_relaxed) == ds.ctrl && g_sec_open_copy.load(std::memory_order_relaxed) == ds.copy;
}
bool second_pull_held(int ctrl) {
    if (g_on.load(std::memory_order_relaxed) && g_spl_hold.load(std::memory_order_relaxed)) {  // (S4) a second pistol's slide held back
        const dual::State d = dual::state();
        if (d.ctrl == ctrl && !d.copy && d.weapon == g_spl_pub.load(std::memory_order_relaxed)) {
            g_sec_held.fetch_add(1, std::memory_order_relaxed);
            controllers::pulse(ctrl, 0.45f, 35);
            return true;
        }
    }
    if (g_on.load(std::memory_order_relaxed) && g_smz_hold.load(std::memory_order_relaxed)) {  // (step 8) the Blunderbuss second gun not rammed
        const dual::State d = dual::state();
        if (d.ctrl == ctrl && !d.copy && kind_of(d.weapon) == kMuzzle) {
            g_sec_held.fetch_add(1, std::memory_order_relaxed);
            controllers::pulse(ctrl, 0.45f, 35);
            return true;
        }
    }
    if (g_on.load(std::memory_order_relaxed) && g_sb_hold.load(std::memory_order_relaxed)) {  // (answer 2; the review: the same second gun)
        const dual::State d = dual::state();
        if (d.ctrl == ctrl && !d.copy && d.weapon == g_sb_pub.load(std::memory_order_relaxed)) {
            g_sec_held.fetch_add(1, std::memory_order_relaxed);
            controllers::pulse(ctrl, 0.45f, 35);
            return true;
        }
    }
    if (!g_on.load(std::memory_order_relaxed) || !g_dual_open.load(std::memory_order_relaxed) || g_sec_open_ctrl.load(std::memory_order_relaxed) != ctrl)
        return false;
    if (!g_sec_open.load(std::memory_order_relaxed)) {
        if (!g_sec_needs.load(std::memory_order_relaxed)) return false;
        g_sec_cock_held.fetch_add(1, std::memory_order_relaxed);  // (round 40) the hammer down: the pull does nothing until cocked
        controllers::pulse(ctrl, 0.3f, 20);
        return true;
    }
    g_sec_held.fetch_add(1, std::memory_order_relaxed);
    controllers::pulse(ctrl, 0.45f, 35);  // felt, as the gun in hand's held pull (open: no empty click)
    return true;
}
unsigned second_drivers(bool copy, int* weapon, float* d) {
    for (int i = 0; i < kDrvCount; ++i) d[i] = 0.0f;
    *weapon = -1;
    if (!copy && g_on.load(std::memory_order_relaxed)) {  // (the Blunderbuss second gun's rod with its ram)
        const float rod = g_smz_rod.load(std::memory_order_relaxed);
        if (rod > 0.0f && dual::state().weapon == 34) {
            *weapon = 34;
            d[kDrvPump] = rod;
            return 1u << kDrvPump;
        }
    }
    if (!copy && g_on.load(std::memory_order_relaxed)) {  // (S4) a second pistol's slide held back (the High Power's, the Mauser's)
        const int pw = g_spl_pub.load(std::memory_order_relaxed);
        if ((pw == 2 || pw == 3) && g_spl_amt.load(std::memory_order_relaxed) > 0.0f && dual::state().weapon == pw) {
            *weapon = pw;
            d[kDrvBoltSlide] = g_spl_amt.load(std::memory_order_relaxed);
            return 1u << kDrvBoltSlide;
        }
    }
    if (!copy && g_on.load(std::memory_order_relaxed)) {  // (step 4, answer 2) a long bolt second gun's bolt as worked (lock-free)
        const int sw = g_sb_pub.load(std::memory_order_relaxed);
        const float sl = g_sb_lift.load(std::memory_order_relaxed), ss = g_sb_slide.load(std::memory_order_relaxed);
        if (sw >= 0 && (sl > 0.0f || ss > 0.0f) && dual::state().weapon == sw) {  // (the review: that second gun still out)
            *weapon = sw;
            d[kDrvBoltLift] = sl;
            d[kDrvBoltSlide] = ss;
            return 1u << kDrvBoltLift | 1u << kDrvBoltSlide;
        }
    }
    if (!g_on.load(std::memory_order_relaxed) || !g_dual_open.load(std::memory_order_relaxed)) return 0;
    const int w = g_sec_open_weapon.load(std::memory_order_relaxed);
    // (S4) a break-open's barrels drawn open always (as the gun in hand's with ManualBreak); a revolver's with OpenCylinder
    const float a = g_open_cyl.load(std::memory_order_relaxed) || (w >= 0 && kind_of(w) == kBreak) ? g_sec_open_amt.load(std::memory_order_relaxed) : 0.0f;
    const bool ham = w >= 0 && g_sec_hammer_weapon.load(std::memory_order_relaxed) == w;  // (round 40) its single action's hammer
    if (w < 0 || !(a > 0.0f || ham) || g_sec_open_copy.load(std::memory_order_relaxed) != copy) return 0;
    // (the copy as your other sidearm's model, DualWieldOwnModel: that model's parts)
    *weapon = copy && dual::copy_model() >= 0 ? dual::copy_model() : w;
    unsigned mask = 0;
    if (a > 0.0f) {
        d[kDrvOpen] = a;
        mask |= 1u << kDrvOpen;
    }
    if (ham) {
        d[kDrvHammer] = g_sec_hammer.load(std::memory_order_relaxed);
        mask |= 1u << kDrvHammer;
    }
    return mask;
}
bool dual_open() { return g_dual_open.load(std::memory_order_relaxed); }
float dual_open_wait_ms() { return g_open_wait_ms.load(std::memory_order_relaxed); }
float dual_click_hold_ms() { return g_click_hold_ms.load(std::memory_order_relaxed); }
void set_dual_open_waits(float walk_ms, float click_ms, bool save) {
    g_open_wait_ms = clamp_wait(walk_ms, 0.0f, 600.0f, 250.0f);
    g_click_hold_ms = clamp_wait(click_ms, 0.0f, 600.0f, 200.0f);
    if (!save) return;
    char b[16];
    std::snprintf(b, sizeof(b), "%.0f", g_open_wait_ms.load());
    config::set("Hands", "DualWieldOpenWaitMs", b);
    std::snprintf(b, sizeof(b), "%.0f", g_click_hold_ms.load());
    config::set("Hands", "DualWieldClickHoldMs", b);
}
void set_dual_open(bool on, bool save) {
    if (g_dual_open.exchange(on) != on)
        log::info("[actions] the second gun opened by its stick held down and clicked (DualWieldOpen): %d%s", on ? 1 : 0, save ? "" : " (the session)");
    if (save) config::set("Hands", "DualWieldOpen", on ? "1" : "0");
}
bool fire_blocked() { return g_on.load(std::memory_order_relaxed) && g_blocked.load(std::memory_order_relaxed); }
bool grip_wanted(int ctrl) {
    return ctrl >= 0 && ctrl < 2 && g_on.load(std::memory_order_relaxed) &&
           (g_bolt_want[ctrl].load(std::memory_order_relaxed) || g_barrel_want[ctrl].load(std::memory_order_relaxed) ||
            g_breech_want[ctrl].load(std::memory_order_relaxed) || g_semi_want[ctrl].load(std::memory_order_relaxed) ||
            g_pslide_want[ctrl].load(std::memory_order_relaxed));
}
bool break_open() { return g_on.load(std::memory_order_relaxed) && g_break_open.load(std::memory_order_relaxed); }
float barrel_snap() { return g_on.load(std::memory_order_relaxed) ? g_barrel_snap.load(std::memory_order_relaxed) : 0.0f; }
bool barrel_hand_snap() { return g_barrel_snap_cfg.load(std::memory_order_relaxed); }
float part_snap(float gun_p[3], float* turn_deg, bool* bolt) {
    for (int i = 0; i < 3; ++i) gun_p[i] = g_part_p[i].load(std::memory_order_relaxed);
    if (turn_deg) *turn_deg = g_part_turn.load(std::memory_order_relaxed);
    if (bolt) *bolt = g_part_bolt.load(std::memory_order_relaxed);
    return g_on.load(std::memory_order_relaxed) ? g_part_snap.load(std::memory_order_relaxed) : 0.0f;
}
float carried_part_snap(float hand_p[3], float gun_R[9], float* turn_deg, int* carry_j) {
    // one frame's set (the sequence even and unchanged across the reads), else none: the weight 0
    float p[3] = {}, R[9] = {}, t = 0.0f, w = 0.0f;
    int j = -1;
    bool ok = false;
    for (int tries = 0; tries < 8 && !ok; ++tries) {
        const uint32_t s0 = g_cpart_seq.load(std::memory_order_acquire);
        if (s0 & 1) continue;
        w = g_cpart_snap.load(std::memory_order_relaxed);
        for (int i = 0; i < 3; ++i) p[i] = g_cpart_p[i].load(std::memory_order_relaxed);
        for (int i = 0; i < 9; ++i) R[i] = g_cpart_R[i].load(std::memory_order_relaxed);
        t = g_cpart_turn.load(std::memory_order_relaxed);
        j = g_cpart_j.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        ok = g_cpart_seq.load(std::memory_order_relaxed) == s0;
    }
    // (no clean read: the last clean one, not a release that drops the hold for a frame: the review)
    thread_local float last_p[3] = {}, last_R[9] = {}, last_t = 0.0f, last_w = 0.0f;
    thread_local int last_j = -1;
    if (ok) {
        std::memcpy(last_p, p, sizeof(p));
        std::memcpy(last_R, R, sizeof(R));
        last_t = t, last_w = w, last_j = j;
    } else {
        std::memcpy(p, last_p, sizeof(p));
        std::memcpy(R, last_R, sizeof(R));
        t = last_t, w = last_w, j = last_j;
    }
    if (hand_p) std::memcpy(hand_p, p, sizeof(p));
    if (gun_R) std::memcpy(gun_R, R, sizeof(R));
    if (turn_deg) *turn_deg = t;
    if (carry_j) *carry_j = j;
    return g_on.load(std::memory_order_relaxed) ? w : 0.0f;
}
bool held_click() { return g_held_click.load(std::memory_order_relaxed); }
void set_held_click(bool on) {
    if (g_held_click.exchange(on) != on) log::info("[actions] a held-back pull clicks: %s", on ? "on" : "off");
    config::set("Reload", "HeldClick", on ? "1" : "0");
}
bool part_hand_snap() { return g_part_snap_cfg.load(std::memory_order_relaxed); }
void set_part_hand_snap(bool on) {
    if (g_part_snap_cfg.exchange(on) != on) log::info("[actions] the off hand drawn on the part it grips: %s", on ? "on" : "off");
    config::set("Reload", "PartHandSnap", on ? "1" : "0");
}
void set_barrel_hand_snap(bool on) {
    if (g_barrel_snap_cfg.exchange(on) != on) log::info("[actions] the off hand drawn on the Double-barrel's barrels: %s", on ? "on" : "off");
    config::set("Reload", "BarrelHandSnap", on ? "1" : "0");
}

bool carried_bolt_near(const float p[3]) {
    if (!p || !g_on.load(std::memory_order_relaxed) || !g_cb_trigger.load(std::memory_order_relaxed)) return false;
    std::lock_guard lock(g_mutex);
    // (the review: not while the trigger holds the bolt mid-stroke)
    if (!g_cb_active || !g_cb_ran || g_cbs.grip || !std::isfinite(g_cb_zone[0] + g_cb_zone[1] + g_cb_zone[2])) return false;
    const float dx = p[0] - g_cb_zone[0], dy = p[1] - g_cb_zone[1], dz = p[2] - g_cb_zone[2];
    return dx * dx + dy * dy + dz * dz < g_bolt_r * g_bolt_r;
}
int carried_hint(float pos[3]) {
    if (!g_on.load(std::memory_order_relaxed) || !g_hints.load(std::memory_order_relaxed)) return 0;
    const dual::State ds = dual::state();
    if (!ds.carry || dual::carry_returning()) return 0;
    std::lock_guard lock(g_mutex);
    if (g_cb_active && g_cb_ran && g_cbs.needs && ds.carry_weapon == g_cb_weapon && std::isfinite(g_cb_zone[0] + g_cb_zone[1] + g_cb_zone[2])) {
        if (pos) std::memcpy(pos, g_cb_zone, sizeof(g_cb_zone));
        return 2;
    }
    if (g_cp_active && g_cp_needs && ds.carry_weapon == g_cp_weapon) return 4;
    if (g_gcr_on && g_gc_active && ds.carry_weapon == g_gc.weapon && std::isfinite(g_gcr_zone[0] + g_gcr_zone[1] + g_gcr_zone[2]) &&
        (g_gcr_hand.load(std::memory_order_relaxed) >= 0 || !g_rod_chest.load(std::memory_order_relaxed) || g_rod_from_gun.load(std::memory_order_relaxed))) {
        if (pos) std::memcpy(pos, g_gcr_zone, sizeof(g_gcr_zone));  // (C15h) the carried Blunderbuss: its rod to take, then its muzzle
        return 2;
    }
    return 0;
}

int hint(float pos[3]) {
    if (!g_on.load(std::memory_order_relaxed) || !g_hints.load(std::memory_order_relaxed)) return 0;
    std::lock_guard lock(g_mutex);
    if (kind_of(g_weapon) == kMuzzle && g_breech_parts.load(std::memory_order_relaxed)) {  // (step 8) a round in, then the ram
        if (!g_needs) return 0;
        if (!g_muz_round) return 1;
        if (pos) std::memcpy(pos, g_rod_zone_on ? g_rod_zone : g_muz_zone, sizeof(g_muz_zone));  // (C15e: the rod to take first)
        return 2;
    }
    if (kind_of(g_weapon) == kBreech && g_breech_parts.load(std::memory_order_relaxed)) {  // BreechParts
        if (g_breech_open && !g_breech_round.load(std::memory_order_relaxed)) return 1;  // open: a round goes in
        // (2026-10-31, the off-hand build step 7, R11; the user: "Buffalo and _____ rifles loading point stops glowing after
        // loading a round even though you need to close it"; r80 G3: hint 0 then) open with its round: shut it
        if (g_breech_open) return 5;
        if (!g_needs || g_breech_open) return 0;
        const Breech* b = breech_of(g_weapon);
        if (b && b->gesture) {
            if (pos) std::memcpy(pos, g_breech_zone, sizeof(g_breech_zone));
            return 2;  // the part's zone
        }
        return 3;  // a flick
    }
    if ((kind_of(g_weapon) == kPistol || kind_of(g_weapon) == kMauser) && kind_on(kind_of(g_weapon))) {
        // PistolMagazines: the well with the magazine out (the Mauser's top while it is open for a clip); PistolSlides:
        // the slide to rack with a round to chamber
        const bool mg = g_mags.load(std::memory_order_relaxed);
        const bool open_top = kind_of(g_weapon) == kMauser && mg && g_locked && !g_clip_seated;
        if (g_pwell_ok && mg && ((kind_of(g_weapon) == kPistol && !g_mag_in.load(std::memory_order_relaxed)) || open_top)) {
            if (pos) std::memcpy(pos, g_pwell, sizeof(g_pwell));
            return 2;
        }
        if (g_slides.load(std::memory_order_relaxed) && !g_chambered && g_last_clip >= 0.5f && g_pd < 1e8f &&
            (g_mag_in.load(std::memory_order_relaxed) || kind_of(g_weapon) == kMauser)) {
            if (pos) std::memcpy(pos, g_pzone, sizeof(g_pzone));
            return 2;
        }
        return 0;
    }
    if (kind_of(g_weapon) == kSemi && g_semi_parts.load(std::memory_order_relaxed)) {  // SemiAutoParts: its handle to rack
        if (!g_needs) return 0;
        if (pos) std::memcpy(pos, g_semi_zone, sizeof(g_semi_zone));
        return 2;
    }
    // (R11) an open revolver or break-open that is full: shut it (r80 G3: the Schofield open and full gave hint 1, the
    // loading point lit for a round that can't go in)
    if (g_open) return g_last_clip_max >= 1.0f && g_last_clip + 0.5f >= g_last_clip_max ? 5 : 1;
    // (R11) a bolt opened with nothing to cycle (r80 G3: hint 0): push it home
    // (the review) only when full: an empty or part-loaded rifle's bolt opened is open to load it
    if (kind_of(g_weapon) == kBolt && kind_on(kBolt) && !g_needs && (g_bolt_back || g_bolt_up) && g_last_clip_max >= 1.0f &&
        g_last_clip + 0.5f >= g_last_clip_max)
        return 5;
    if (!g_needs) return 0;
    switch (kind_of(g_weapon)) {
    case kBolt:
        if (pos) std::memcpy(pos, g_bolt_zone, sizeof(g_bolt_zone));
        return 2;
    case kLever: return 3;
    case kPump: return 4;
    case kRevolver:
    case kTopBreak:  // RevolverControl=cock: FanHammer's zone (else the stick's flick)
        if (g_fan.load(std::memory_order_relaxed) && g_fan_d < 1e8f) {
            if (pos) std::memcpy(pos, g_fan_zone, sizeof(g_fan_zone));
            return 2;
        }
        return 3;
    default: return 0;
    }
}

float open_amount(int* weapon) {
    if (!g_on.load(std::memory_order_relaxed)) return 0.0f;
    const int w = g_open_weapon.load(std::memory_order_relaxed);
    // the revolvers drawn open with OpenCylinder; the break action with its own switch (kind_on: it opens only then)
    if (!g_open_cyl.load(std::memory_order_relaxed) && kind_of(w) != kBreak) return 0.0f;
    if (weapon) *weapon = w;
    return g_open_amt.load(std::memory_order_relaxed);
}
bool open_cylinder() { return g_open_cyl.load(std::memory_order_relaxed); }
bool lever_parts() { return g_lever_parts.load(std::memory_order_relaxed); }
bool manual_break() { return g_break.load(std::memory_order_relaxed); }
void set_manual_break(bool on) {
    if (g_break.exchange(on) != on) log::info("[actions] the Double-barrel and the Sawed-off broken open by hand: %s", on ? "on" : "off");
    config::set("Reload", "ManualBreak", on ? "1" : "0");
}
void round_in(int weapon, const float at[3], int n, bool second) {
    if (!g_on.load(std::memory_order_relaxed) || n < 1) return;
    if (second) {  // (2026-10-27, DualWieldCrossReload) the second gun's: its sound only
        if (const GunSounds* gs = gun_sounds(weapon)) play_at(gs->insert, at);
        return;
    }
    g_hand_round_ms.store(log::now_ms(), std::memory_order_relaxed);
    if (const GunSounds* gs = gun_sounds(weapon)) play_at(gs->insert, at);
    const Kind k = kind_of(weapon);
    if ((k == kRevolver || k == kTopBreak) && weapon >= 0 && weapon < 40 && g_open_cyl.load(std::memory_order_relaxed))
        g_cyl_rounds[weapon].fetch_add(n, std::memory_order_relaxed);
    if (k == kBreech && g_breech_parts.load(std::memory_order_relaxed)) g_breech_round.store(true, std::memory_order_relaxed);
}
unsigned gun_drivers(int* weapon, float* d) {
    for (int i = 0; i < kDrvCount; ++i) d[i] = 0.0f;
    *weapon = -1;
    if (!g_on.load(std::memory_order_relaxed)) return 0;
    unsigned mask = 0;
    int ow = -1;
    const float oa = open_amount(&ow);  // OpenCylinder (the revolvers) or ManualBreak (the break actions); 0 when off
    if (oa > 0.0f) {
        d[kDrvOpen] = oa;
        mask |= 1u << kDrvOpen;
        *weapon = ow;
    }
    const int cw = g_open_weapon.load(std::memory_order_relaxed);
    const Kind ck = kind_of(cw);
    // the cylinder turned a chamber a round put in (OpenCylinder) and a cock (RevolverControl=cock)
    const int hw = g_rev_weapon.load(std::memory_order_relaxed);
    if ((g_open_cyl.load(std::memory_order_relaxed) || (hw >= 0 && hw == cw)) && (ck == kRevolver || ck == kTopBreak) && g_revolver.load(std::memory_order_relaxed)) {
        d[kDrvIndex] = g_cyl_drawn.load(std::memory_order_relaxed);
        if (d[kDrvIndex] != 0.0f) mask |= 1u << kDrvIndex;
        *weapon = cw;
    }
    if (hw >= 0) {  // RevolverControl=cock: the single action's hammer
        d[kDrvHammer] = g_rev_hammer.load(std::memory_order_relaxed);
        mask |= 1u << kDrvHammer;
        *weapon = hw;
    }
    int lw = -1;
    float lv = 0.0f, hm = 0.0f;
    if (lever_drawn(&lw, &lv, &hm)) {
        d[kDrvLever] = lv;
        d[kDrvHammer] = hm;
        mask |= 1u << kDrvLever | 1u << kDrvHammer;
        *weapon = lw;
    }
    const int pw = g_pump_weapon.load(std::memory_order_relaxed);
    if (g_pump_parts.load(std::memory_order_relaxed) && pw >= 0) {
        d[kDrvPump] = g_pump_amt.load(std::memory_order_relaxed);
        mask |= 1u << kDrvPump;
        *weapon = pw;
    }
    if (const float rod = g_muz_rod.load(std::memory_order_relaxed);
        rod > 0.0f && g_breech_parts.load(std::memory_order_relaxed) && g_breech_weapon.load(std::memory_order_relaxed) == 34) {  // (the review)
        d[kDrvPump] = rod;  // (the Blunderbuss's rod with the ram)
        mask |= 1u << kDrvPump;
        *weapon = 34;
    }
    if (g_rod_shown.load(std::memory_order_relaxed)) {  // (C15b) the ramrod held (only ever the Blunderbuss: the muzzle-loader)
        d[kDrvRod] = 1.0f;
        mask = (mask & ~(1u << kDrvPump)) | 1u << kDrvRod;
        *weapon = 34;
    }
    const int bw = g_bolt_weapon.load(std::memory_order_relaxed);
    if (g_bolt_parts.load(std::memory_order_relaxed) && bw >= 0) {
        d[kDrvBoltLift] = g_bolt_lift.load(std::memory_order_relaxed);
        d[kDrvBoltSlide] = g_bolt_slide.load(std::memory_order_relaxed);
        mask |= 1u << kDrvBoltLift | 1u << kDrvBoltSlide;
        *weapon = bw;
    }
    const int rw = g_breech_weapon.load(std::memory_order_relaxed);
    if (g_breech_parts.load(std::memory_order_relaxed) && rw >= 0) {
        d[kDrvBreech] = g_breech_amt.load(std::memory_order_relaxed);
        d[kDrvHammer] = g_breech_hammer.load(std::memory_order_relaxed);
        mask |= 1u << kDrvBreech | 1u << kDrvHammer;
        *weapon = rw;
    }
    const int sw = g_semi_weapon.load(std::memory_order_relaxed);
    if (g_semi_parts.load(std::memory_order_relaxed) && sw >= 0) {  // its bolt: the bolt slide's driver
        d[kDrvBoltSlide] = g_semi_amt.load(std::memory_order_relaxed);
        mask |= 1u << kDrvBoltSlide;
        *weapon = sw;
    }
    const int plw = g_pslide_weapon.load(std::memory_order_relaxed);
    if (plw >= 1 && plw <= 3) {  // PistolSlides (the Mauser's bolt with PistolMagazines too): the slide, the bolt, the toggle
        const float a = g_pslide.load(std::memory_order_relaxed);
        if (plw == 1) {  // the toggle: the rear link's turn; the front link by the knee's rise and move back, its own turn
            float knee[3], fd;
            toggle_pose(a, knee, &fd);
            d[kDrvBoltLift] = a;
            d[kDrvPump] = (knee[1] - kToggleKnee[1]) / kToggleDy;
            d[kDrvBoltSlide] = (knee[2] - kToggleKnee[2]) / kToggleDz;
            d[kDrvBreech] = fd / kToggleFrontDeg;
            mask |= 1u << kDrvBoltLift | 1u << kDrvPump | 1u << kDrvBoltSlide | 1u << kDrvBreech;
        } else {
            d[kDrvBoltSlide] = a;
            mask |= 1u << kDrvBoltSlide;
        }
        *weapon = plw;
    }
    return mask;
}
bool semi_auto_parts() { return g_semi_parts.load(std::memory_order_relaxed); }
void set_semi_auto_parts(bool on) {
    if (g_semi_parts.exchange(on) != on) log::info("[actions] the semi-auto shotgun racked by hand, drawn: %s", on ? "on" : "off");
    config::set("Reload", "SemiAutoParts", on ? "1" : "0");
}
bool pistol_mags() { return g_mags.load(std::memory_order_relaxed); }
void set_pistol_mags(bool on) {
    if (g_mags.exchange(on) != on) log::info("[actions] the pistols loaded by magazine (the Mauser by a stripper clip): %s", on ? "on" : "off");
    config::set("Reload", "PistolMagazines", on ? "1" : "0");
}
bool pistol_slides() { return g_slides.load(std::memory_order_relaxed); }
void set_pistol_slides(bool on) {
    if (g_slides.exchange(on) != on) log::info("[actions] the pistols' slides racked by hand: %s", on ? "on" : "off");
    config::set("Reload", "PistolSlides", on ? "1" : "0");
}
bool mag_gun(int weapon) {
    const Kind k = kind_of(weapon);
    return g_on.load(std::memory_order_relaxed) && g_mags.load(std::memory_order_relaxed) && (k == kPistol || k == kMauser);
}
bool mag_wanted(const RdrvrActorState& st) {
    if (!mag_gun(st.weapon) || !st.valid || st.spare < 1.0f || (st.weapon_flags & (RDRVR_WEAPON_DEADEYE | RDRVR_WEAPON_RELOADING))) return false;
    std::lock_guard lock(g_mutex);
    if (st.weapon != g_weapon) return false;
    if (kind_of(st.weapon) == kPistol) return !g_mag_in.load(std::memory_order_relaxed);
    const int count = g_expect_until > 0.0 ? g_expect_clip : static_cast<int>(std::floor(st.clip + 0.01f));
    return !g_clip_seated && count + 1 <= static_cast<int>(std::floor(st.clip_max + 0.01f));
}
bool mag_point(const body::BodyPoints& bp, int weapon, float out[3], float axes[9]) {
    const PistolDef* pd = pistol_of(weapon);
    const int gj = bp.gun;
    if (!pd || gj < 0 || gj > 1 || !bp.hand_ok[gj]) return false;
    float wl[3], mo[3] = {};
    holster::gun_adjust(weapon, holster::kAdjMag, mo, nullptr, nullptr, gj);  // (the caller holds neither lock: holster's frame)
    well_at(pd, mo, wl);
    float R[9];
    if (bp.gun_frame_ok) {
        std::memcpy(R, bp.gun_frame_R, sizeof(R));
        for (int i = 0; i < 3; ++i) out[i] = bp.gun_frame_o[i] + R[i * 3] * wl[0] + R[i * 3 + 1] * wl[1] + R[i * 3 + 2] * wl[2];
    } else {  // the gun hand's target: the barrel's direction in it, its right, up square to both (as frame's)
        float bt[3] = {0.0f, 0.174f, -0.985f}, u[3], rgt[3], up[3];
        aim::barrel_in_target(bt);
        const float* T = bp.target_rot[gj];
        for (int i = 0; i < 3; ++i) {
            u[i] = T[i * 3] * bt[0] + T[i * 3 + 1] * bt[1] + T[i * 3 + 2] * bt[2];
            rgt[i] = T[i * 3];
        }
        up[0] = rgt[1] * u[2] - rgt[2] * u[1];
        up[1] = rgt[2] * u[0] - rgt[0] * u[2];
        up[2] = rgt[0] * u[1] - rgt[1] * u[0];
        const float ul = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
        if (ul < 1e-4f) return false;
        for (float& x : up) x /= ul;
        for (int i = 0; i < 3; ++i) {
            R[i * 3] = rgt[i], R[i * 3 + 1] = up[i], R[i * 3 + 2] = -u[i];
            out[i] = bp.hand[gj][i] + u[i] * (0.08f - wl[2]) + rgt[i] * wl[0] + up[i] * wl[1];
        }
    }
    if (axes) std::memcpy(axes, R, sizeof(R));
    return true;
}
bool insert_mag(const RdrvrActorState& st, const float at[3], int* rounds) {
    if (rounds) *rounds = 0;
    if (!mag_gun(st.weapon) || !reload::hand_reload() || !st.valid || (st.weapon_flags & (RDRVR_WEAPON_DEADEYE | RDRVR_WEAPON_RELOADING))) return false;
    const double now = log::now_ms();
    std::lock_guard lock(g_mutex);
    // (the review) one count of the mod's awaited at a time: a drop or an ejected round not yet seen, the magazine waits
    // (held, it goes in a frame or two later)
    if (st.weapon != g_weapon || g_expect_until > 0.0 || g_lose_pending > 0) return false;
    const int count = static_cast<int>(std::floor(st.clip + 0.01f));
    const int cap = static_cast<int>(std::floor(st.clip_max + 0.01f)), spare = static_cast<int>(std::floor(st.spare + 0.01f));
    RdrvrActorState sx = st;
    sx.clip = static_cast<float>(count);
    int n = 0;
    const bool mauser = kind_of(st.weapon) == kMauser;
    if (mauser) {  // a stripper clip into the open top: as many of its ten as there is room for; the rest stay on it
        if (g_clip_seated || !(g_locked || (g_pgrip && g_pdrawn >= 0.9f))) return false;
        const int on_clip = spare < kClipRounds ? spare : kClipRounds;
        n = cap - count < on_clip ? cap - count : on_clip;
        if (n < 1 || (n = reload::insert_rounds(sx, n)) < 1) return false;
        g_clip_seated = true;
        g_clip_left = on_clip - n;
        g_clip_inserts.fetch_add(1, std::memory_order_relaxed);
    } else {  // a magazine up into the grip: the gun loaded to its size from the spare (reload::fill_clip's count)
        if (g_mag_in.load(std::memory_order_relaxed)) return false;
        const int room = cap - count < spare ? cap - count : spare;
        n = room >= 1 ? reload::insert_rounds(sx, room) : 0;
        g_mag_in.store(true, std::memory_order_relaxed);
        if (st.weapon >= 1 && st.weapon <= 3) g_mag_out[st.weapon] = false;
        g_mag_inserts.fetch_add(1, std::memory_order_relaxed);
    }
    if (n > 0) {
        g_expect_clip = count + n;
        g_expect_until = now + 1500.0;
    }
    g_hand_round_ms.store(now, std::memory_order_relaxed);  // (the count's rise: not the game's own reload)
    play_at("PISTOL_MAG_INSERT_MASTER", at);
    log::info("[actions] %s (%d): %s in (clip %d -> %d of %d, spare %d -> %d)%s", kind_name(kind_of(st.weapon)), st.weapon,
              mauser ? "a stripper clip" : "a magazine", count, count + n, cap, spare, spare - n,
              mauser ? (g_clip_left ? ", rounds left on the clip" : "") : g_slides.load() && !g_chambered ? ": rack the slide to chamber a round" : "");
    if (rounds) *rounds = n;
    return true;
}
bool breech_parts() { return g_breech_parts.load(std::memory_order_relaxed); }
void set_breech_parts(bool on) {
    if (g_breech_parts.exchange(on) != on) log::info("[actions] the single-shot breeches worked by hand, drawn: %s", on ? "on" : "off");
    config::set("Reload", "BreechParts", on ? "1" : "0");
}
bool bolt_parts() { return g_bolt_parts.load(std::memory_order_relaxed); }
void set_bolt_parts(bool on) {
    if (g_bolt_parts.exchange(on) != on) log::info("[actions] the bolt worked by hand, drawn: %s", on ? "on" : "off");
    config::set("Reload", "BoltParts", on ? "1" : "0");
}
bool carried_bolt() { return g_carried_bolt.load(std::memory_order_relaxed); }
void set_carried_bolt(bool on) {
    if (g_carried_bolt.exchange(on) != on) log::info("[actions] a carried bolt gun's bolt by the gun hand: %s", on ? "on" : "off");
    config::set("Reload", "CarriedBolt", on ? "1" : "0");
}
bool offhand_bolt_trigger() { return g_ob_trigger.load(std::memory_order_relaxed); }
void set_offhand_bolt_trigger(bool on) {
    if (g_ob_trigger.exchange(on) != on) log::info("[actions] the gun in hand's bolt worked with the off hand's %s", on ? "trigger" : "grip");
    config::set("Reload", "OffHandBoltTrigger", on ? "1" : "0");
}
// (C15e) the chest's take: not while the ramrod is taken from the gun
bool ramrod_ready() {
    return g_on.load(std::memory_order_relaxed) && g_rod_ready.load(std::memory_order_relaxed) && !g_rod_from_gun.load(std::memory_order_relaxed);
}
bool rod_place(float Q[9], float s[3]) {
    {
        std::lock_guard lock(g_rod_mx);
        std::memcpy(Q, g_rod_q, sizeof(g_rod_q));
    }
    for (int j = 0; j < 3; ++j) s[j] = g_rod_s[j].load(std::memory_order_relaxed);
    return g_on.load(std::memory_order_relaxed) && (g_rod_shown.load(std::memory_order_relaxed) || g_gcr_pub.load(std::memory_order_relaxed) >= 0);
}
int ramrod_taker() {
    if (!g_on.load(std::memory_order_relaxed) || g_rod_from_gun.load(std::memory_order_relaxed)) return -1;
    if (g_rod_ready.load(std::memory_order_relaxed)) return 1 - controls::gun_hand();  // the gun in hand's: the off hand
    if (g_gcr_ready.load(std::memory_order_relaxed)) return controls::gun_hand();     // (C15h) the carried gun's: the free gun hand
    return -1;
}
void take_ramrod(int ctrl) {
    if (ctrl < 0 || ctrl > 1 || ctrl != ramrod_taker()) return;
    if (g_rod_ready.load(std::memory_order_relaxed)) {
        g_rod_hand.store(ctrl, std::memory_order_relaxed);
        g_rod_ready.store(false, std::memory_order_relaxed);
    } else {
        g_gcr_hand.store(ctrl, std::memory_order_relaxed);
        g_gcr_ready.store(false, std::memory_order_relaxed);
    }
    log::info("[actions] muzzle-loader: the ramrod taken at the chest by the %s hand", ctrl ? "right" : "left");
}
int ramrod_hand() {
    const int h = g_rod_hand.load(std::memory_order_relaxed);
    return h >= 0 ? h : g_gcr_hand.load(std::memory_order_relaxed);
}
bool ramrod_at_chest() { return g_rod_chest.load(std::memory_order_relaxed); }
RodPlace ramrod_place(int hand) {
    std::lock_guard lock(g_rod_mx);
    return g_rod_place[hand == 1 && g_rod_place_r ? 1 : 0];
}
void set_ramrod_place(int hand, const RodPlace& p, bool save) {
    RodPlace c;
    for (int k = 0; k < 3; ++k) c.offset[k] = std::isfinite(p.offset[k]) ? std::fmin(0.2f, std::fmax(-0.2f, p.offset[k])) : 0.0f;
    c.turn = std::isfinite(p.turn) ? std::fmin(180.0f, std::fmax(-180.0f, p.turn)) : 0.0f;
    c.tilt = std::isfinite(p.tilt) ? std::fmin(180.0f, std::fmax(-180.0f, p.tilt)) : 0.0f;
    const int hs = hand == 1 ? 1 : 0;
    {
        std::lock_guard lock(g_rod_mx);
        g_rod_place[hs] = c;
        if (hs == 1) g_rod_place_r = true;
    }
    if (!save) return;
    const char* sfx = hs == 1 ? "Right" : "";
    char b[64];
    std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", c.offset[0], c.offset[1], c.offset[2]);
    config::set("Reload", (std::string("RamrodOffset") + sfx).c_str(), b);
    std::snprintf(b, sizeof(b), "%.0f", c.turn);
    config::set("Reload", (std::string("RamrodTurn") + sfx).c_str(), b);
    std::snprintf(b, sizeof(b), "%.0f", c.tilt);
    config::set("Reload", (std::string("RamrodTilt") + sfx).c_str(), b);
}
bool ramrod_from_gun() { return g_rod_from_gun.load(std::memory_order_relaxed); }
void set_ramrod_from_gun(bool on, bool save) {
    if (g_rod_from_gun.exchange(on) != on) log::info("[actions] the Blunderbuss's ramrod taken from: %s", on ? "the gun" : "the chest");
    if (save) config::set("Reload", "RamrodFrom", on ? "gun" : "chest");
}
void ramrod_grab_offset(float o[3]) {
    std::lock_guard lock(g_rod_mx);
    std::memcpy(o, g_rod_grab_off, sizeof(g_rod_grab_off));
}
void set_ramrod_grab_offset(const float o[3], bool save) {
    float c[3];
    for (int k = 0; k < 3; ++k) c[k] = std::isfinite(o[k]) ? std::fmin(0.3f, std::fmax(-0.3f, o[k])) : 0.0f;
    {
        std::lock_guard lock(g_rod_mx);
        std::memcpy(g_rod_grab_off, c, sizeof(c));
    }
    if (save) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", c[0], c[1], c[2]);
        config::set("Reload", "RamrodGrabOffset", b);
    }
}
void set_ramrod_at_chest(bool on) {
    if (g_rod_chest.exchange(on) != on) log::info("[actions] the Blunderbuss's ramrod at the chest: %s", on ? "on" : "off");
    config::set("Reload", "RamrodAtChest", on ? "1" : "0");
}
bool carried_bolt_trigger() { return g_cb_trigger.load(std::memory_order_relaxed); }
void set_carried_bolt_trigger(bool on) {
    if (g_cb_trigger.exchange(on) != on) log::info("[actions] a carried bolt gun's bolt worked with the gun hand's %s", on ? "trigger" : "grip");
    config::set("Reload", "CarriedBoltTrigger", on ? "1" : "0");
}
bool carried_trigger_wanted(int ctrl) {
    for (int c = 0; c < 2; ++c)  // let go: the latch ends (the pad's poll, as the triggers are read for RT; both: the gun hand may change)
        if (g_cb_trig_latch[c].load(std::memory_order_relaxed) && hands::get(c).trigger < 0.15f) g_cb_trig_latch[c].store(false, std::memory_order_relaxed);
    return ctrl >= 0 && ctrl < 2 && g_cb_trig_latch[ctrl].load(std::memory_order_relaxed);
}
bool carried_pump() { return g_carried_pump.load(std::memory_order_relaxed); }
void set_carried_pump(bool on) {
    if (g_carried_pump.exchange(on) != on) log::info("[actions] a carried Pump-action pumped by a jerk of the carrying hand: %s", on ? "on" : "off");
    config::set("Reload", "CarriedPump", on ? "1" : "0");
}
bool carry_handover_ok() {
    if (!fire_blocked()) return true;  // (as before: nothing held back)
    {  // [Reload] CarriedAnyState: a lever, breech, break-open or semi-auto in any state but a part held mid-stroke
        std::lock_guard lock(g_mutex);
        if (g_carried_any.load(std::memory_order_relaxed) && any_kind(kind_of(g_weapon)) && !g_breech_grip && !g_semi_grip && !g_barrel_hold &&
            !g_muz_grip && g_rod_hand.load(std::memory_order_relaxed) < 0)  // (C16c: not mid-ram, nor the ramrod in a hand)
            return true;
    }
    const bool cbolt = g_carried_bolt.load(std::memory_order_relaxed) && g_bolt.load(std::memory_order_relaxed);
    const bool cpump = g_carried_pump.load(std::memory_order_relaxed) && g_pump.load(std::memory_order_relaxed);
    if (!cbolt && !cpump) return false;
    std::lock_guard lock(g_mutex);
    // [Reload] CarriedBolt: a bolt gun's shot not cycled goes with the gun, the bolt not in a hand. (2026-11-03, round 75, the
    // user: carried after a shot, its bolt opened and a round put in by the gun hand, the bolt left open, the grip taken
    // again: "you can not let go"; their log: the let-go waited, "an action not worked") open too: the hand-over takes the
    // bolt's whole state (up, back, its lift and slide: g_bsave), the carried bolt works from there, the take-back gives it back
    if (cbolt && kind_of(g_weapon) == kBolt && !g_open && !g_bolt_grip) return true;
    // [Reload] CarriedPump: a Pump-action's shot not pumped goes with the gun, its pump forward (not mid-stroke)
    return cpump && kind_of(g_weapon) == kPump && g_needs && !g_open && !g_pump_back && g_pump_amt.load(std::memory_order_relaxed) <= 0.05f;
}
unsigned carry_drivers(int* weapon, float* d) {
    for (int i = 0; i < kDrvCount; ++i) d[i] = 0.0f;
    *weapon = -1;
    if (!g_on.load(std::memory_order_relaxed)) return 0;
    if (const int bw = g_gcb_pub.load(std::memory_order_relaxed); bw >= 0 && g_breech_parts.load(std::memory_order_relaxed)) {
        *weapon = bw;  // (F6) the carried breech's block and hammer
        d[kDrvBreech] = g_gcb_amt.load(std::memory_order_relaxed);
        d[kDrvHammer] = g_gcb_ham.load(std::memory_order_relaxed);
        return 1u << kDrvBreech | 1u << kDrvHammer;
    }
    if (const int rw = g_gcr_pub.load(std::memory_order_relaxed); rw >= 0) {
        *weapon = rw;  // (C15h) the carried Blunderbuss's ramrod in the free gun hand
        d[kDrvRod] = 1.0f;
        return 1u << kDrvRod;
    }
    if (const float rod = g_gcr_rod.load(std::memory_order_relaxed); rod > 0.0f && g_breech_parts.load(std::memory_order_relaxed)) {
        *weapon = 34;  // (C15h, RamrodAtChest off) its rod with the bare hand's ram
        d[kDrvPump] = rod;
        return 1u << kDrvPump;
    }
    if (const int pw = g_cp_pub.load(std::memory_order_relaxed); pw >= 0 && g_pump_parts.load(std::memory_order_relaxed)) {
        *weapon = pw;  // [Reload] CarriedPump: the carried Pump-action's fore-end (kDrvPump)
        d[kDrvPump] = g_cp_amt.load(std::memory_order_relaxed);
        return 1u << kDrvPump;
    }
    if (!g_bolt_parts.load(std::memory_order_relaxed)) return 0;
    const int w = g_cb_pub.load(std::memory_order_relaxed);
    if (w < 0) return 0;
    *weapon = w;
    d[kDrvBoltLift] = g_cb_lift.load(std::memory_order_relaxed);
    d[kDrvBoltSlide] = g_cb_slide.load(std::memory_order_relaxed);
    return 1u << kDrvBoltLift | 1u << kDrvBoltSlide;
}
bool pump_parts() { return g_pump_parts.load(std::memory_order_relaxed); }
void set_pump_parts(bool on) {
    if (g_pump_parts.exchange(on) != on) log::info("[actions] the pump worked by hand, drawn: %s", on ? "on" : "off");
    config::set("Reload", "PumpParts", on ? "1" : "0");
}
float pump_drawn() {
    if (!g_on.load(std::memory_order_relaxed) || !g_pump_parts.load(std::memory_order_relaxed) || g_pump_weapon.load(std::memory_order_relaxed) < 0)
        return 0.0f;
    return g_pump_amt.load(std::memory_order_relaxed);
}
void set_lever_parts(bool on) {
    if (g_lever_parts.exchange(on) != on) log::info("[actions] the lever worked by hand, drawn: %s", on ? "on" : "off");
    config::set("Reload", "LeverParts", on ? "1" : "0");
}
int lever_control() { return g_lever_mode.load(std::memory_order_relaxed); }
bool volcanic_stick() { return g_volc_stick.load(std::memory_order_relaxed); }
void set_volcanic_stick(bool on) {
    if (g_volc_stick.exchange(on) != on) log::info("[actions] the Volcanic's lever by: %s", on ? "the stick's flick" : "LeverControl");
    config::set("Reload", "VolcanicControl", on ? "stick" : "lever");
}
int revolver_control() { return g_rev_mode.load(std::memory_order_relaxed); }
void set_revolver_control(int mode) {
    if (mode < 0 || mode > 1) return;
    if (g_rev_mode.exchange(mode) != mode) {
        log::info("[actions] the revolvers: %s", mode ? "cocked by the stick's flick, opened by the stick held down and clicked" : "opened by the stick's flick");
        std::lock_guard lock(g_mutex);
        if (!mode && (g_kind == kRevolver || g_kind == kTopBreak)) g_needs = false;  // a hammer left down: nothing to cock now
    }
    config::set("Reload", "RevolverControl", mode ? "cock" : "open");
}
bool fan_hammer() { return g_fan.load(std::memory_order_relaxed); }
void set_fan_hammer(bool on) {
    if (g_fan.exchange(on) != on) log::info("[actions] the hammer fanned by the other hand: %s", on ? "on" : "off");
    config::set("Reload", "FanHammer", on ? "1" : "0");
}
int hammer_weapon(float* hammer) {
    const int w = g_on.load(std::memory_order_relaxed) ? g_rev_weapon.load(std::memory_order_relaxed) : -1;
    if (hammer) *hammer = w >= 0 ? g_rev_hammer.load(std::memory_order_relaxed) : 1.0f;
    return w;
}
bool thumb_cock() { return g_thumb_cfg.load(std::memory_order_relaxed); }
void set_thumb_cock(bool on) {
    if (g_thumb_cfg.exchange(on) != on) log::info("[actions] the stick's cock drawn by John's thumb: %s", on ? "on" : "off");
    config::set("Reload", "ThumbCock", on ? "1" : "0");
}
int thumb_on_hammer(float* weight, float* hammer) {
    const int w = hammer_weapon(hammer);
    *weight = 0.0f;
    if (w < 0 || !g_thumb_cfg.load(std::memory_order_relaxed) || g_thumb_weapon.load(std::memory_order_relaxed) != w) return w;
    // from the times (not the frame end's last value): the draws' own frame. On it by kThumbReachMs, until its return, off
    // over kThumbBackMs from the weight it had then
    const double now = log::now_ms(), t0 = g_thumb_t0.load(std::memory_order_relaxed), ret = g_thumb_ret.load(std::memory_order_relaxed);
    if (now < t0) return w;
    *weight = now < ret ? smooth01((now - t0) / kThumbReachMs) : smooth01((ret - t0) / kThumbReachMs) * (1.0f - smooth01((now - ret) / kThumbBackMs));
    return w;
}
int second_thumb_on_hammer(float* weight) {
    *weight = 0.0f;
    const int w = g_sthumb_weapon.load(std::memory_order_relaxed);
    if (w < 0 || !g_on.load(std::memory_order_relaxed) || !g_thumb_cfg.load(std::memory_order_relaxed)) return -1;
    const double now = log::now_ms(), t0 = g_sthumb_t0.load(std::memory_order_relaxed), ret = g_sthumb_ret.load(std::memory_order_relaxed);
    if (now < t0 || now > ret + kThumbBackMs) return -1;
    *weight = now < ret ? smooth01((now - t0) / kThumbReachMs) : smooth01((ret - t0) / kThumbReachMs) * (1.0f - smooth01((now - ret) / kThumbBackMs));
    return w;
}
bool lever_lock() { return g_on.load(std::memory_order_relaxed) && g_lever_lock.load(std::memory_order_relaxed); }
bool lever_front() { return g_on.load(std::memory_order_relaxed) && g_lever_front.load(std::memory_order_relaxed); }
bool lever_hold_lock() { return g_on.load(std::memory_order_relaxed) && g_lever_hold_lock.load(std::memory_order_relaxed); }
double since_shot_ms() { return log::now_ms() - g_last_shot_ms.load(std::memory_order_relaxed); }
int parts_weapon() {
    if (!g_on.load(std::memory_order_relaxed)) return -1;
    if (g_lever_parts.load(std::memory_order_relaxed)) {
        const int w = g_lever_weapon.load(std::memory_order_relaxed);
        if (w >= 0) return w;
    }
    if (g_pump_parts.load(std::memory_order_relaxed)) {
        const int w = g_pump_weapon.load(std::memory_order_relaxed);
        if (w >= 0) return w;
    }
    if (g_bolt_parts.load(std::memory_order_relaxed)) {
        const int w = g_bolt_weapon.load(std::memory_order_relaxed);
        if (w >= 0) return w;
    }
    if (g_breech_parts.load(std::memory_order_relaxed)) {
        const int w = g_breech_weapon.load(std::memory_order_relaxed);
        if (w >= 0) return w;
    }
    if (g_semi_parts.load(std::memory_order_relaxed)) {
        const int w = g_semi_weapon.load(std::memory_order_relaxed);
        if (w >= 0) return w;
    }
    // PistolSlides (and the Mauser with PistolMagazines, its bolt worked by hand for the clip): the pistol's own fire clip
    // stopped, its slide (bolt) drawn by the mod alone, the shot's blowback and sound the mod's (g_pslide_weapon is set
    // only while the slide is the mod's)
    if (g_slides.load(std::memory_order_relaxed) || g_mags.load(std::memory_order_relaxed)) {
        const int w = g_pslide_weapon.load(std::memory_order_relaxed);
        if (w >= 0) return w;
    }
    return -1;
}
bool lever_drawn(int* weapon, float* lever, float* hammer) {
    if (!g_on.load(std::memory_order_relaxed) || !g_lever_parts.load(std::memory_order_relaxed)) return false;
    const int w = g_lever_weapon.load(std::memory_order_relaxed);
    if (w < 0) return false;
    *weapon = w;
    *lever = g_lever_amt.load(std::memory_order_relaxed);
    *hammer = g_hammer_amt.load(std::memory_order_relaxed);
    return true;
}
void set_open_cylinder(bool on) {
    if (g_open_cyl.exchange(on) != on) log::info("[actions] the revolver drawn open: %s", on ? "on" : "off");
    config::set("Reload", "OpenCylinder", on ? "1" : "0");
}

bool can_load(int weapon) {
    if (!g_on.load(std::memory_order_relaxed)) return true;
    const Kind k = kind_of(weapon);
    if ((k == kPistol || k == kMauser) && g_mags.load(std::memory_order_relaxed)) {  // PistolMagazines: a magazine (a clip) only
        std::lock_guard lock(g_mutex);  // (the review: the gun in hand's; a dual-wielded second pistol takes rounds as before)
        if (weapon == g_weapon) return false;
    }
    if (!g_load_open.load(std::memory_order_relaxed)) return true;
    if (k == kMuzzle && kind_on(k)) {  // (step 8) the muzzle-loader: one round, after a shot, before the ram
        std::lock_guard lock(g_mutex);
        return weapon == g_weapon && g_needs && !g_muz_round;
    }
    if (k == kBreech && kind_on(k)) {  // BreechParts: open, and one round at most
        std::lock_guard lock(g_mutex);
        return weapon == g_weapon && g_breech_open && !g_breech_round.load(std::memory_order_relaxed);
    }
    if (k == kBolt && kind_on(k) && g_bolt_parts.load(std::memory_order_relaxed))  // BoltParts: only with the bolt back
        return g_bolt_weapon.load(std::memory_order_relaxed) == weapon && g_bolt_slide.load(std::memory_order_relaxed) >= 0.9f;
    if (!opens(k) || !kind_on(k)) return true;
    std::lock_guard lock(g_mutex);
    return weapon == g_weapon && g_open;
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, w;
    in >> c;
    if (line.find(" second") != std::string::npos) {  // actions second [on|off]: [Hands] DualWieldOpen for the session, its state
        if (line.find(" second on") != std::string::npos) set_dual_open(true, false);
        if (line.find(" second off") != std::string::npos) set_dual_open(false, false);
        {  // actions second waits <walk ms> <click ms>: DualWieldOpenWaitMs and DualWieldClickHoldMs for the session
            const size_t at = line.find(" waits ");
            float wm = 0.0f, cm = 0.0f;
            if (at != std::string::npos && std::sscanf(line.c_str() + at + 7, "%f %f", &wm, &cm) == 2) set_dual_open_waits(wm, cm, false);
        }
        const dual::State ds = dual::state();
        char e[900];
        std::snprintf(e, sizeof(e),
                      "second: dualopen %d (load only when open %d, open cylinder %d, manual revolver %d), open %d (weapon %d, controller %d, copy %d, "
                      "drawn %.2f), req %d | opens %llu, closes %llu, held pulls "
                      "%llu, clicks taken %llu, polls zeroed %llu, Dead Eye shuts %llu, refused (the gun stick's controller) %llu | its hand's angular "
                      "velocity (%.2f %.2f %.2f) rad/s | dual on %d copy %d carry %d ready %d weapon %d controller %d | can load %d | the player's "
                      "L3 %.0f ms ago | armed %d, waits walk %.0f ms click %.0f ms: walk waits %llu (passed %llu), clicks passed after the hold %llu, "
                      "quick clicks dropped %llu | cock: needs %d hammer %.2f (weapon %d) flicks %llu cocks %llu pulls held %llu",
                      g_dual_open.load() ? 1 : 0, g_load_open.load() ? 1 : 0, g_open_cyl.load() ? 1 : 0, g_revolver.load() ? 1 : 0, g_sec_open.load() ? 1 : 0,
                      g_sec_open_weapon.load(), g_sec_open_ctrl.load(), g_sec_open_copy.load() ? 1 : 0,
                      g_sec_open_amt.load(), g_sec_open_req.load() ? 1 : 0, static_cast<unsigned long long>(g_sec_opens.load()),
                      static_cast<unsigned long long>(g_sec_closes.load()), static_cast<unsigned long long>(g_sec_held.load()),
                      static_cast<unsigned long long>(g_sec_clicks.load()), static_cast<unsigned long long>(g_sec_zeroed.load()),
                      static_cast<unsigned long long>(g_sec_deadeye.load()), static_cast<unsigned long long>(g_sec_refused.load()), g_sw[0], g_sw[1],
                      g_sw[2], ds.on ? 1 : 0, ds.copy ? 1 : 0, ds.carry ? 1 : 0, ds.ready ? 1 : 0, ds.weapon, ds.ctrl, ds.on ? (can_load_second(ds) ? 1 : 0) : -1,
                      log::now_ms() - controls::last_user_l3_ms(), g_sec_armed.load() ? 1 : 0, g_open_wait_ms.load(), g_click_hold_ms.load(),
                      static_cast<unsigned long long>(g_sec_walk_waits.load()), static_cast<unsigned long long>(g_sec_walk_passed.load()),
                      static_cast<unsigned long long>(g_sec_click_passed.load()), static_cast<unsigned long long>(g_sec_click_dropped.load()),
                      g_sec_needs.load() ? 1 : 0, g_sec_hammer.load(), g_sec_hammer_weapon.load(), static_cast<unsigned long long>(g_sec_flicks.load()),
                      static_cast<unsigned long long>(g_sec_cocks.load()), static_cast<unsigned long long>(g_sec_cock_held.load()));
        return e;
    }
    bool all = false;  // "actions all": every gun kind's section
    while (in >> w) {
        if (w == "all") all = true;
        if (w == "on" || w == "off") g_on = w == "on";  // the session only
        if (w == "open") g_open_req = true;
        if (w == "opencyl") {  // opencyl on|off: [Reload] OpenCylinder for the session
            std::string v;
            in >> v;
            g_open_cyl = v != "off";
            continue;
        }
        if (w == "leverparts") {  // leverparts on|off: [Reload] LeverParts for the session
            std::string v;
            in >> v;
            g_lever_parts = v != "off";
            continue;
        }
        if (w == "levercontrol") {  // levercontrol flick|stick|lock|front: [Reload] LeverControl for the session
            std::string v;
            in >> v;
            g_lever_mode = v == "stick" ? 1 : v == "lock" ? 2 : v == "front" ? 3 : v == "hold" ? 4 : 0;
            {  // as set_lever_control
                std::lock_guard lock(g_mutex);
                g_lever_open = false;
                g_lever_stick_close_ms = 0;
            }
            if (g_lever_mode.load() != 2) g_lever_lock = false;
            if (g_lever_mode.load() != 3) g_lever_front = false;
            if (g_lever_mode.load() != 4) g_lever_hold_lock = false;
            continue;
        }
        if (w == "revolvercontrol") {  // revolvercontrol open|cock: [Reload] RevolverControl for the session
            std::string v;
            in >> v;
            g_rev_mode = v == "cock" ? 1 : 0;
            if (!g_rev_mode.load()) {  // as set_revolver_control
                std::lock_guard lock(g_mutex);
                if (g_kind == kRevolver || g_kind == kTopBreak) g_needs = false;
            }
            continue;
        }
        if (w == "fan") {  // fan on|off: [Reload] FanHammer for the session
            std::string v;
            in >> v;
            g_fan = v != "off";
            continue;
        }
        if (w == "thumb") {  // thumb on|off: [Reload] ThumbCock for the session
            std::string v;
            in >> v;
            g_thumb_cfg = v != "off";
            continue;
        }
        if (w == "hammerdown") {  // hammerdown (a test aid, r234): a cocked single action's hammer down as after a shot, no shot
            std::lock_guard lock(g_mutex);  // fired (RevolverControl=cock; its flick cocks it again)
            if ((g_kind == kRevolver || g_kind == kTopBreak) && g_rev_mode.load() == 1 && single_action(g_weapon) && !g_open) {
                g_needs = true;
                log::info("[actions] %s (%d): the hammer let down (the test's, no shot)", kind_name(g_kind), g_weapon);
            }
            continue;
        }
        if (w == "volcanic") {  // volcanic lever|stick: [Reload] VolcanicControl for the session
            std::string v;
            in >> v;
            g_volc_stick = v == "stick";
            continue;
        }
        if (w == "sound") {  // sound <NAME>: a game sound played at the gun hand (choosing a gun's sounds); the name kept
            std::string v;    // in a ring until long after its native has run
            in >> v;
            static char names[8][64];
            static int next = 0;
            float at[3];
            if (!v.empty() && v.size() < 64 && body_hand(controls::gun_hand(), at)) {
                char* nm = names[next++ % 8];
                std::memcpy(nm, v.c_str(), v.size() + 1);
                play_at(nm, at);
            }
            continue;
        }
        if (w == "semiparts") {  // semiparts on|off: [Reload] SemiAutoParts for the session
            std::string v;
            in >> v;
            g_semi_parts = v != "off";
            continue;
        }
        if (w == "mags") {  // mags on|off: [Reload] PistolMagazines for the session
            std::string v;
            in >> v;
            g_mags = v != "off";
            continue;
        }
        if (w == "slides") {  // slides on|off: [Reload] PistolSlides for the session
            std::string v;
            in >> v;
            g_slides = v != "off";
            continue;
        }
        if (w == "magreset") {  // magreset: every pistol's magazine back in (a test's setup; the session)
            std::lock_guard lock(g_mutex);
            for (bool& x : g_mag_out) x = false;
            g_mag_in.store(true, std::memory_order_relaxed);
            g_clip_seated = false;
            g_clip_left = 0;
            continue;
        }
        if (w == "breechparts") {  // breechparts on|off: [Reload] BreechParts for the session
            std::string v;
            in >> v;
            g_breech_parts = v != "off";
            continue;
        }
        if (w == "boltparts") {  // boltparts on|off: [Reload] BoltParts for the session
            std::string v;
            in >> v;
            g_bolt_parts = v != "off";
            continue;
        }
        if (w == "carriedpump") {  // carriedpump on|off: [Reload] CarriedPump for the session
            std::string v;
            in >> v;
            g_carried_pump = v != "off";
            continue;
        }
        if (w == "carriedpumpspeed") {  // carriedpumpspeed <m/s>: [Reload] CarriedPumpSpeed for the session
            float v = 0.0f;
            if (in >> v) g_cp_speed = std::fmin(5.0f, std::fmax(0.5f, v));
            continue;
        }
        if (w == "carriedbolt") {  // carriedbolt on|off: [Reload] CarriedBolt for the session
            std::string v;
            in >> v;
            g_carried_bolt = v != "off";
            continue;
        }
        if (w == "ramrod") {  // ramrod on|off: [Reload] RamrodAtChest for the session
            std::string v;
            in >> v;
            g_rod_chest = v != "off";
            continue;
        }
        if (w == "rodplace") {  // rodplace x y z: [Reload] RamrodOffset for both hands for the session (no turn), the tests'
            RodPlace p{{0.0f, 0.0f, 0.0f}, 0.0f, 0.0f};
            in >> p.offset[0] >> p.offset[1] >> p.offset[2];
            set_ramrod_place(0, p, false);
            set_ramrod_place(1, p, false);
            continue;
        }
        if (w == "ramrodfrom") {  // ramrodfrom gun|chest: [Reload] RamrodFrom for the session (C15e)
            std::string v;
            in >> v;
            set_ramrod_from_gun(v != "chest", false);
            continue;
        }
        if (w == "offtrigger") {  // offtrigger on|off: [Reload] OffHandBoltTrigger for the session
            std::string v;
            in >> v;
            g_ob_trigger = v != "off";
            continue;
        }
        if (w == "carriedtrigger") {  // carriedtrigger on|off: [Reload] CarriedBoltTrigger for the session
            std::string v;
            in >> v;
            g_cb_trigger = v != "off";
            continue;
        }
        if (w == "pumpparts") {  // pumpparts on|off: [Reload] PumpParts for the session
            std::string v;
            in >> v;
            g_pump_parts = v != "off";
            continue;
        }
        if (w == "heldclick") {  // heldclick on|off: [Reload] HeldClick for the session
            std::string v;
            in >> v;
            g_held_click = v != "off";
            continue;
        }
        if (w == "partsnap") {  // partsnap on|off: [Reload] PartHandSnap for the session
            std::string v;
            in >> v;
            g_part_snap_cfg = v != "off";
            continue;
        }
        if (w == "barrelsnap") {  // barrelsnap on|off: [Reload] BarrelHandSnap for the session
            std::string v;
            in >> v;
            g_barrel_snap_cfg = v != "off";
            continue;
        }
        if (w == "break") {  // break on|off: [Reload] ManualBreak for the session
            std::string v;
            in >> v;
            g_break = v != "off";
            continue;
        }
        if (w == "trace") {  // trace [n|clear]: the last n (300) frames' bolt strokes written to the log, one line each
            std::string v;
            in >> v;
            std::lock_guard lock(g_mutex);
            if (v == "clear") {
                g_trace_n = 0;
                continue;
            }
            int n = v.empty() ? 300 : std::atoi(v.c_str());
            const uint64_t have = g_trace_n < kTraceN ? g_trace_n : kTraceN;
            if (n < 1 || static_cast<uint64_t>(n) > have) n = static_cast<int>(have);
            log::info("[actions] trace: %d frames (of %llu); the handle down takes h <= %.4f m (lift 0.05 x %.3f), up h >= %.4f", n,
                      static_cast<unsigned long long>(g_trace_n), 0.05f * kBoltRise, kBoltRise, 0.95f * kBoltRise);
            for (uint64_t i = g_trace_n - static_cast<uint64_t>(n); i < g_trace_n; ++i) {
                const BoltTrace& t = g_trace[i % kTraceN];
                log::info("[actions] trace %llu: %.1f ms %s h %.4f s %.4f lift %.3f slide %.3f up %d back %d ref (%.4f %.4f %.4f) hand (%.4f %.4f %.4f)",
                          static_cast<unsigned long long>(i), t.ms, t.carried ? "carried" : "in hand", t.h, t.s, t.lift, t.slide, t.up ? 1 : 0, t.back ? 1 : 0,
                          t.ref[0], t.ref[1], t.ref[2], t.oh[0], t.oh[1], t.oh[2]);
            }
            continue;
        }
        if (w == "reset") {
            std::lock_guard lock(g_mutex);
            g_needs = g_open = false;
            g_lever_open = g_hammer_down = false;
            g_bolt_back = g_bolt_up = g_bolt_cycled = false;
            g_pump_back = g_pump_have = false;  // the pump's stroke too (a stroke left back held the next phase's)
            g_breech_open = g_breech_grip = false;
            g_breech_round.store(true, std::memory_order_relaxed);
            g_semi_back = g_semi_grip = false;
            g_locked = g_pback = g_pgrip = false;  // (PistolSlides: the slide forward, a round chambered if there is one)
            g_chambered = g_last_clip >= 0.5f;
            g_pdrawn = 0.0f;
            g_bolt_lift.store(0.0f, std::memory_order_relaxed);
            g_bolt_slide.store(0.0f, std::memory_order_relaxed);
            g_cbs.needs = g_cbs.back = g_cbs.up = g_cbs.cycled = false;  // ([Reload] CarriedBolt: the carried bolt too)
            g_cbs.lift = g_cbs.slide = 0.0f;
            g_cp_needs = g_cp_back = g_cp_ooo = false;  // ([Reload] CarriedPump: the carried pump too)
            for (float& x : g_w_peak) x = 0.0f;
        }
    }
    char b[400];
    const int hn = hint(nullptr);  // before the lock (hint takes it)
    std::lock_guard lock(g_mutex);
    std::snprintf(b, sizeof(b),
                  "actions %d | weapon %d (%s) open %d needs %d blocked %d hint %d | opens %llu closes %llu cycles %llu held %llu out-of-order %llu shots "
                  "%llu sounds %llu | w (%.1f %.1f %.1f) peak (%.1f %.1f %.1f) rad/s",
                  g_on.load() ? 1 : 0, g_weapon, kind_name(g_kind), g_open ? 1 : 0, g_needs ? 1 : 0, g_blocked.load() ? 1 : 0, hn,
                  static_cast<unsigned long long>(g_opens.load()), static_cast<unsigned long long>(g_closes.load()),
                  static_cast<unsigned long long>(g_cycles.load()), static_cast<unsigned long long>(g_held.load()),
                  static_cast<unsigned long long>(g_out_of_order.load()), static_cast<unsigned long long>(g_shots_seen.load()),
                  static_cast<unsigned long long>(g_sounds.load()), g_w[0], g_w[1], g_w[2], g_w_peak[0], g_w_peak[1], g_w_peak[2]);
    std::string o = b;
    std::snprintf(b, sizeof(b), " | opencyl %d", g_open_cyl.load() ? 1 : 0);  // (r47: the session's switch put back as found)
    o += b;
    // the gun kind in hand's section (the test channel's line is cut at about 1000 characters)
    const Kind gk = g_kind;
    if (all || gk == kBolt) {
        std::snprintf(b, sizeof(b), " | bolt zone (%.3f %.3f %.3f) hand %.3f m, gripped %d back %d s %.3f parts %d up %d lift %.2f slide %.2f", g_bolt_zone[0],
                      g_bolt_zone[1], g_bolt_zone[2], g_bolt_d, g_bolt_grip ? 1 : 0, g_bolt_back ? 1 : 0, g_bolt_s, g_bolt_parts.load() ? 1 : 0, g_bolt_up ? 1 : 0,
                      g_bolt_lift.load(), g_bolt_slide.load());
        o += b;
        std::snprintf(b, sizeof(b), " | bolt zone in the gun %d (%.4f %.4f %.4f)", g_bolt_z0_gun_ok ? 1 : 0, g_bolt_z0_gun[0], g_bolt_z0_gun[1], g_bolt_z0_gun[2]);
        o += b;
    }
    if (all || g_spl_active) {  // (S4) a pistol as the second gun
        std::snprintf(b, sizeof(b), " | second slide %d weapon %d locked %d second slide zone (%.3f %.3f %.3f) %.3f m", g_spl_active ? 1 : 0, g_spl_weapon,
                      g_spl_locked ? 1 : 0, g_spl_zone[0], g_spl_zone[1], g_spl_zone[2], g_spl_d);
        o += b;
    }
    if (all || g_smz_active) {  // (step 8) the Blunderbuss as the second gun
        std::snprintf(b, sizeof(b), " | second muzzle %d needs %d round %d strokes %d down %d gripped %d hold %d second muzzle zone (%.3f %.3f %.3f) %.3f m fwd (%.3f %.3f %.3f) up (%.3f %.3f %.3f)",
                      g_smz_active ? 1 : 0, g_smz.needs ? 1 : 0, g_smz.round ? 1 : 0, g_smz.strokes, g_smz.down ? 1 : 0, g_smz.grip ? 1 : 0,
                      g_smz_hold.load() ? 1 : 0, g_smz_zone[0], g_smz_zone[1], g_smz_zone[2], g_smz_d, g_smz_u[0], g_smz_u[1], g_smz_u[2], g_smz_up[0], g_smz_up[1],
                      g_smz_up[2]);
        o += b;
    }
    if (all || g_sb_active) {  // (step 4, answer 2) a long bolt second gun's own bolt
        std::snprintf(b, sizeof(b), " | second bolt %d weapon %d needs %d up %d back %d lift %.2f slide %.2f gripped %d hold %d second bolt zone (%.3f %.3f %.3f) %.3f m fwd (%.3f %.3f %.3f) up (%.3f %.3f %.3f)",
                      g_sb_active ? 1 : 0, g_sb_weapon, g_sbs.needs ? 1 : 0, g_sbs.up ? 1 : 0, g_sbs.back ? 1 : 0, g_sbs.lift, g_sbs.slide, g_sbs.grip ? 1 : 0,
                      g_sb_hold.load() ? 1 : 0, g_sb_zone[0], g_sb_zone[1], g_sb_zone[2], g_sb_d, g_sb_u[0], g_sb_u[1], g_sb_u[2], g_sb_up[0], g_sb_up[1], g_sb_up[2]);
        o += b;
    }
    if (all || g_gcb_pub.load() >= 0) {  // (F6) the carried breech
        std::snprintf(b, sizeof(b),
                      " | carried breech weapon %d zone (%.3f %.3f %.3f) hand %.3f m gripped %d open %d round %d needs %d drawn %.2f hammer %.2f | opens %llu "
                      "shuts %llu",
                      g_gcb_pub.load(), g_gcb_zone[0], g_gcb_zone[1], g_gcb_zone[2], g_gcb_d, g_gcb_grip ? 1 : 0, g_gc.breech_open ? 1 : 0,
                      g_gc.breech_round ? 1 : 0, g_gc.needs ? 1 : 0, g_gcb_amt.load(), g_gcb_ham.load(), static_cast<unsigned long long>(g_gcb_opens.load()),
                      static_cast<unsigned long long>(g_gcb_shuts.load()));
        o += b;
    }
    if (all || g_cb_active || g_carried_bolt.load()) {  // [Reload] CarriedBolt
        // (cycle r197: "learned" was the carried gun's alone, -1 before a carry: 0 though the gun in hand had learned it)
        const int zs = bolt_slot(g_cb_active ? g_cb_weapon : g_weapon);
        std::snprintf(b, sizeof(b),
                      " | carried bolt %d weapon %d (on %d) zone (%.3f %.3f %.3f) learned %d hand %.3f m take-back %.3f m ran %d, gripped %d back %d up %d "
                      "needs %d lift %.2f slide %.2f drawn %d | handovers %llu restores %llu | fwd (%.3f %.3f %.3f) up (%.3f %.3f %.3f)",
                      g_cb_active ? 1 : 0, g_cb_weapon, g_carried_bolt.load() ? 1 : 0, g_cb_zone[0], g_cb_zone[1], g_cb_zone[2],
                      zs >= 0 && g_bolt_zone_gun_ok[zs] ? 1 : 0, g_cb_d, g_cb_take, g_cb_ran ? 1 : 0, g_cbs.grip ? 1 : 0, g_cbs.back ? 1 : 0,
                      g_cbs.up ? 1 : 0, g_cbs.needs ? 1 : 0, g_cbs.lift, g_cbs.slide, g_cb_pub.load(), static_cast<unsigned long long>(g_cb_handovers.load()),
                      static_cast<unsigned long long>(g_cb_restores.load()), g_cb_fwd[0], g_cb_fwd[1], g_cb_fwd[2], g_cb_up[0], g_cb_up[1], g_cb_up[2]);
        o += b;
        std::snprintf(b, sizeof(b), " | on the carrying hand %d (%d placements) at (%.3f %.3f %.3f) | by trigger %d (wants %d)", g_cb_rel_ok ? 1 : 0, g_cb_rel_n,
                      g_cb_relo[0], g_cb_relo[1], g_cb_relo[2], g_cb_trigger.load() ? 1 : 0,
                      g_cb_trig_latch[0].load() ? 0 : g_cb_trig_latch[1].load() ? 1 : -1);
        o += b;
    }
    if (all || g_cp_active || g_carried_pump.load()) {  // [Reload] CarriedPump
        std::snprintf(b, sizeof(b),
                      " | carried pump %d weapon %d (on %d) needs %d back %d v %.2f speed %.2f drawn %d amt %.2f | handovers %llu restores %llu pumps %llu"
                      " | pump fwd (%.3f %.3f %.3f)",
                      g_cp_active ? 1 : 0, g_cp_weapon, g_carried_pump.load() ? 1 : 0, g_cp_needs ? 1 : 0, g_cp_back ? 1 : 0, g_cp_v, g_cp_speed.load(),
                      g_cp_pub.load(), g_cp_amt.load(), static_cast<unsigned long long>(g_cp_handovers.load()),
                      static_cast<unsigned long long>(g_cp_restores.load()), static_cast<unsigned long long>(g_cp_pumps.load()), g_cp_fwd[0], g_cp_fwd[1],
                      g_cp_fwd[2]);
        o += b;
    }
    if (all || gk == kPump) {
        std::snprintf(b, sizeof(b), " | pump %d parts %d drawn %.2f travel %.3f", g_pump_back ? 1 : 0, g_pump_parts.load() ? 1 : 0, g_pump_amt.load(), g_pump_s);
        o += b;
    }
    if (all || gk == kLever) {
        std::snprintf(b, sizeof(b), " | lever parts %d open %d drawn %.2f hammer %.2f mode %d (volcanic stick %d) held %d gun held %d", g_lever_parts.load() ? 1 : 0,
                      g_lever_open ? 1 : 0, g_lever_amt.load(), g_hammer_amt.load(), lever_mode_of(g_weapon), g_volc_stick.load() ? 1 : 0,
                      g_lever_hold.load() ? 1 : 0, g_lever_hold_lock.load() ? 1 : 0);
        o += b;
    }
    if (all || gk == kRevolver || gk == kTopBreak) {
        std::snprintf(b, sizeof(b), " | revolver control %s hammer weapon %d drawn %.2f cocks %llu stick held %d", g_rev_mode.load() ? "cock" : "open",
                      g_rev_weapon.load(), g_rev_hammer.load(), static_cast<unsigned long long>(g_cocked_n.load()), g_rev_hold.load() ? 1 : 0);
        o += b;
        {  // ThumbCock (thumb_on_hammer takes no lock)
            float tw = 0.0f, th = 1.0f;
            thumb_on_hammer(&tw, &th);
            std::snprintf(b, sizeof(b), " | thumb %d weight %.2f", g_thumb_cfg.load() ? 1 : 0, tw);
            o += b;
        }
        std::snprintf(b, sizeof(b), " | fan %d zone (%.3f %.3f %.3f) r %.3f d %.3f v %.2f back %.2f fans %llu shots %llu frame %d", g_fan.load() ? 1 : 0,
                      g_fan_zone[0], g_fan_zone[1], g_fan_zone[2], g_fan_r, g_fan_d, g_fan_v, g_fan_back, static_cast<unsigned long long>(g_fans.load()),
                      static_cast<unsigned long long>(g_fan_shots.load()), g_fan_frame ? 1 : 0);
        o += b;
        std::snprintf(b, sizeof(b), " | fan wait %d retries %llu late %llu", g_fan_pending ? 1 : 0, static_cast<unsigned long long>(g_fan_retries.load()),
                      static_cast<unsigned long long>(g_fan_late.load()));
        o += b;
        std::snprintf(b, sizeof(b), " | gun cock %d sweeps %llu cocks %llu not back %llu already %llu d %.3f v %.2f back %.2f armed %d visit v %.2f back %.2f jumps %llu fan speed %.2f",
                      body::gun_collision() ? 1 : 0,
                      static_cast<unsigned long long>(g_gc_sweeps.load()), static_cast<unsigned long long>(g_gc_cocks.load()),
                      static_cast<unsigned long long>(g_gc_notback.load()), static_cast<unsigned long long>(g_gc_already.load()), g_gc_d < 1e8f ? g_gc_d : -1.0f,
                      g_gc_v, g_gc_back, g_gc_armed ? 1 : 0, g_gc_vis_v, g_gc_vis_back, static_cast<unsigned long long>(g_gc_jumps.load()),
                      g_fan_speed);
        o += b;
    }
    if (all || gk == kBreak || gk == kRevolver || gk == kTopBreak) {
        std::snprintf(b, sizeof(b), " | break %d barrels held %d at %.2f (off them %.3f m, the hand at %.2f) drawn %.2f, hand closes %llu", g_break.load() ? 1 : 0,
                      g_barrel_hold ? 1 : 0, g_barrel_amt.load(), g_barrel_d, g_barrel_at, g_open_amt.load(), static_cast<unsigned long long>(g_hand_closes.load()));
        o += b;
    }
    if (all || gk == kBreech) {
        std::snprintf(b, sizeof(b), " | breech %d open %d round %d drawn %.2f hammer %.2f gripped %d breech zone (%.3f %.3f %.3f) %.3f m", g_breech_parts.load() ? 1 : 0,
                      g_breech_open ? 1 : 0, g_breech_round.load() ? 1 : 0, g_breech_amt.load(), g_breech_hammer.load(), g_breech_grip ? 1 : 0, g_breech_zone[0],
                      g_breech_zone[1], g_breech_zone[2], g_breech_d);
        o += b;
    }
    if (all || gk == kMuzzle) {  // (step 8, R14)
        std::snprintf(b, sizeof(b), " | muzzle round %d strokes %d of %d down %d gripped %d muzzle zone (%.3f %.3f %.3f) %.3f m rod %.2f", g_muz_round ? 1 : 0,
                      g_muz_strokes, kRamStrokes, g_muz_down ? 1 : 0, g_muz_grip ? 1 : 0, g_muz_zone[0], g_muz_zone[1], g_muz_zone[2], g_muz_d,
                      g_muz_rod.load());
        o += b;
        std::snprintf(b, sizeof(b), " | ramrod drawn %d (%.3f %.3f %.3f) from the gun %d, its place (%.3f %.3f %.3f) on %d %.3f m", g_rod_shown.load() ? 1 : 0,
                      g_rod_s[0].load(), g_rod_s[1].load(), g_rod_s[2].load(), g_rod_from_gun.load() ? 1 : 0, g_rod_zone[0], g_rod_zone[1], g_rod_zone[2],
                      g_rod_zone_on ? 1 : 0, g_rod_zone_d < 1e8f ? g_rod_zone_d : -1.0f);
        o += b;
        // (C15h) the carried Blunderbuss's ram by the free gun hand
        std::snprintf(b, sizeof(b), " | carried ram %d needs %d round %d hand %d ready %d drawn %d zone (%.3f %.3f %.3f) %.3f m back (%.3f %.3f %.3f) gripped %d down %d strokes %d",
                      g_gcr_on ? 1 : 0, g_gc_active && g_gc.needs ? 1 : 0, g_gc_active && g_gc.muz_round ? 1 : 0, g_gcr_hand.load(), g_gcr_ready.load() ? 1 : 0,
                      g_gcr_pub.load() >= 0 ? 1 : 0, g_gcr_zone[0], g_gcr_zone[1], g_gcr_zone[2], g_gcr_d < 1e8f ? g_gcr_d : -1.0f, g_gcr_back[0], g_gcr_back[1],
                      g_gcr_back[2], g_gcr_grip ? 1 : 0, g_gcr_down ? 1 : 0, g_gc.muz_strokes);
        o += b;
    }
    if (all || gk == kSemi) {
        std::snprintf(b, sizeof(b), " | semi %d back %d drawn %.2f gripped %d semi zone (%.3f %.3f %.3f) %.3f m", g_semi_parts.load() ? 1 : 0, g_semi_back ? 1 : 0,
                      g_semi_amt.load(), g_semi_grip ? 1 : 0, g_semi_zone[0], g_semi_zone[1], g_semi_zone[2], g_semi_d);
        o += b;
    }
    if (all || gk == kPistol || gk == kMauser) {  // PistolMagazines / PistolSlides
        std::snprintf(b, sizeof(b),
                      " | pistol mags %d slides %d mag in %d chambered %d locked %d gripped %d back %d drawn %.2f slide %.2f (weapon %d) clip seated %d left %d"
                      " expect %d",
                      g_mags.load() ? 1 : 0, g_slides.load() ? 1 : 0, g_mag_in.load() ? 1 : 0, g_chambered ? 1 : 0, g_locked ? 1 : 0, g_pgrip ? 1 : 0,
                      g_pback ? 1 : 0, g_pdrawn, g_pslide.load(), g_pslide_weapon.load(), g_clip_seated ? 1 : 0, g_clip_left,
                      g_expect_until > 0.0 ? g_expect_clip : -1);
        o += b;
        std::snprintf(b, sizeof(b),
                      " | slide zone (%.3f %.3f %.3f) %.3f m well (%.3f %.3f %.3f) ok %d | drops %llu inserts %llu clips %llu clip outs %llu racks %llu ejects %llu "
                      "lost %llu chambers %llu locks %llu",
                      g_pzone[0], g_pzone[1], g_pzone[2], g_pd < 1e8f ? g_pd : -1.0f, g_pwell[0], g_pwell[1], g_pwell[2], g_pwell_ok ? 1 : 0,
                      static_cast<unsigned long long>(g_mag_drops.load()), static_cast<unsigned long long>(g_mag_inserts.load()),
                      static_cast<unsigned long long>(g_clip_inserts.load()), static_cast<unsigned long long>(g_clip_outs.load()),
                      static_cast<unsigned long long>(g_racks.load()), static_cast<unsigned long long>(g_ejects.load()),
                      static_cast<unsigned long long>(g_lost.load()), static_cast<unsigned long long>(g_chambers.load()),
                      static_cast<unsigned long long>(g_locks.load()));
        o += b;
    }
    return o;
}

}  // namespace rdrvr::actions
