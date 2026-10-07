#include "core/actions.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>

#include "core/aim.h"
#include "core/api.h"
#include "core/audio.h"
#include "core/body.h"
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
// [Reload] BreechParts (off; run 6 item 9g): the single-shot rifles (the Springfield, the Rolling Block, the Buffalo)
std::atomic<bool> g_breech_parts{false};
// [Reload] SemiAutoParts (off; run 6 item 9h): the Semi-Auto Shotgun's bolt racked by the off hand after a load from empty
std::atomic<bool> g_semi_parts{false};
float g_break_deg = kBreakOpenDeg;  // the barrels' opening (the game's reload: 31 held, 33.5 at the swing's end; round 13: 45)
float g_barrel_r = 0.15f;          // [Reload] BarrelGripRadius: the off hand within this of the open barrels takes them
const float kHingeAlong = 0.10f;   // the hinge from the gun hand's wrist: along the barrel, up (m; the loading point's)
const float kHingeUp = 0.03f;

enum Kind { kNone, kRevolver, kTopBreak, kBolt, kLever, kPump, kBreak, kBreech, kSemi };
Kind kind_of(int w) {
    switch (w) {
        case 4: case 6: case 7: return kRevolver;  // the Cattleman, the Double-Action, the LeMat (a loading gate)
        case 5: return kTopBreak;                  // the Schofield
        case 13: case 20: return kBolt;            // the Bolt Action, the Carcano
        case 0: case 8: case 9: case 10: case 11: return kLever;  // the Volcanic, the Carbine, Winchester, Henry, Evans
        case 17: return kPump;
        case 15: case 16: return kBreak;           // the Sawed-off, the Double-barrel
        case 12: case 14: case 19: return kBreech;  // the Springfield, the Buffalo, the Rolling Block (single shots)
        case 18: return kSemi;                     // the Semi-Auto Shotgun
        default: return kNone;
    }
}
const char* kind_name(Kind k) {
    static const char* const n[] = {"none", "revolver", "top-break", "bolt", "lever", "pump", "break", "breech", "semi-auto"};
    return n[k];
}
bool kind_on(Kind k) {
    return (k == kRevolver || k == kTopBreak) ? g_revolver.load() : k == kBolt ? g_bolt.load() : k == kLever ? g_lever.load() : k == kPump ? g_pump.load()
         : k == kBreak ? g_break.load() : k == kBreech ? g_breech_parts.load() : k == kSemi ? g_semi_parts.load() : false;
}
bool opens(Kind k) { return k == kRevolver || k == kTopBreak || k == kBreak; }  // opened by the stick, loaded only open

// the state: the frame end writes (one thread: the pad poll and the frame end both run there); the test channel reads
std::mutex g_mutex;
int g_weapon = -1;
Kind g_kind = kNone;
bool g_open = false, g_needs = false;  // a revolver open; a shot not cycled
double g_open_ms = 0;
float g_last_clip = -1.0f;
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
float g_breech_hz[3] = {}, g_semi_hz[3] = {};  // the breech's and the semi-auto's handles as drawn (gun frame)
std::atomic<uint64_t> g_hand_closes{0};
// [Reload] LeverParts (off): a lever gun's lever drawn as the player works it (the Carbine, then run 6 item 9c: the
// Winchester, the Henry, the Evans, the Volcanic). A flick down opens it (and stays open), a flick up closes it; the
// hammer is cocked by the opening and falls with the shot. The drawn amounts, eased
std::atomic<bool> g_lever_parts{false};
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
std::atomic<int> g_breech_weapon{-1};
std::atomic<bool> g_breech_round{true};  // a live round chambered (holster.cpp's round in sets it)
std::atomic<bool> g_breech_want[2] = {false, false};
bool g_breech_open = false, g_breech_grip = false, g_breech_grip_was = false;
double g_breech_ms = 0, g_breech_shut_ms = 0;
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
// the lever: a pitch flick, down then up
double g_lever_down_ms = 0;
// the pump: the front hand's travel along the barrel while two-handed
bool g_pump_have = false, g_pump_back = false;
float g_pump_s0 = 0.0f, g_pump_s = 0.0f;  // the hold; the travel back from it (the state line)
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
    g_breech_amt.store(0.0f, std::memory_order_relaxed);
    g_breech_hammer.store(1.0f, std::memory_order_relaxed);
    g_semi_grip = g_semi_back = false;
    g_semi_amt.store(0.0f, std::memory_order_relaxed);
    g_pump_have = g_pump_back = false;
    g_lever_down_ms = 0;
    g_lever_open = g_hammer_down = false;
    g_close_hits = 0;
    g_barrel_hold = g_barrel_take = false;
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

void frame() {
    const double now = log::now_ms();
    RdrvrActorState st{};
    const bool on = g_on.load(std::memory_order_relaxed) && pose::anchor_active() && api::actor_state(&st) && st.actor;
    std::lock_guard lock(g_mutex);
    const int w = on && reload::is_gun(st.weapon) ? st.weapon : -1;
    if (!on || w < 0) {
        if (g_weapon != -1) reset_gun(-1, -1.0f);
        g_blocked = false;
        g_rt_rearm = false;
        g_bolt_want[0] = g_bolt_want[1] = false;
        g_open_req = false;
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
        g_barrel_want[0] = g_barrel_want[1] = false;
        g_break_open = false;
        g_open_amt.store(0.0f, std::memory_order_relaxed);
        g_open_weapon.store(-1, std::memory_order_relaxed);
        g_barrel_snap.store(0.0f, std::memory_order_relaxed);
        g_cyl_weapon.store(-1, std::memory_order_relaxed);
        g_cyl_drawn.store(0.0f, std::memory_order_relaxed);
        return;
    }
    if (w != g_weapon) reset_gun(w, st.clip);
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
    if (g_last_clip >= 0.0f && st.clip < g_last_clip - 0.5f && !reloading) {
        g_shots_seen.fetch_add(1, std::memory_order_relaxed);
        g_last_shot_ms.store(now, std::memory_order_relaxed);
        g_hammer_down = true;
        if ((k == kBolt || k == kLever || k == kPump) && st.clip >= 0.5f && !deadeye) {
            g_needs = true;
            log::info("[actions] %s (%d): shot, the %s needs working (clip %.0f/%.0f)", kind_name(k), w,
                      k == kBolt ? "bolt" : k == kLever ? "lever" : "pump", st.clip, st.clip_max);
        }
        if (k == kBreech && !deadeye) {  // a single shot: the spent case in the chamber, whatever the game's count
            g_needs = true;
            g_breech_round.store(false, std::memory_order_relaxed);
            log::info("[actions] breech (%d): shot, open it and put a round in (clip %.0f/%.0f, spare %.0f)", w, st.clip, st.clip_max, st.spare);
        }
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
    if (deadeye) {  // Dead Eye fires its marks: nothing held, everything ready after it
        g_needs = false;
        g_open = false;
        g_lever_open = g_hammer_down = false;
    }
    // revolvers (and the break action): open by the stick flick (filter_stick), close by a flick of the gun hand
    if (opens(k) && !deadeye) {
        if (g_open_req.exchange(false) && !g_open) {
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
    // bolt actions: the other hand grips at the bolt, pulls it back, then forward ([Reload] BoltParts: the handle turned
    // up by the hand's rise first and down again after; the bolt drawn where the hand has it)
    g_bolt_want[0] = g_bolt_want[1] = false;
    const bool bparts = k == kBolt && g_bolt_parts.load(std::memory_order_relaxed);
    float blift = g_bolt_lift.load(std::memory_order_relaxed), bslide = g_bolt_slide.load(std::memory_order_relaxed);  // as left
    if (k == kBolt && pts && bp.hand_ok[gj] && bp.hand_ok[oj] && !deadeye) {
        // the zone: the bolt's handle, with BoltParts where it is drawn (up with its turn, back with the bolt)
        for (int i = 0; i < 3; ++i)
            g_bolt_zone[i] = bp.hand[gj][i] + rgt[i] * g_bolt_off[0] + up[i] * g_bolt_off[1] + u[i] * g_bolt_off[2] +
                             (bparts ? up[i] * blift * kBoltRise - u[i] * bslide * kBoltStroke : 0.0f);
        const float* oh = ogb;
        const float dx = oh[0] - g_bolt_zone[0], dy = oh[1] - g_bolt_zone[1], dz = oh[2] - g_bolt_zone[2];
        g_bolt_d = std::sqrt(dx * dx + dy * dy + dz * dz);
        const bool in = g_bolt_d < g_bolt_r;
        const bool grip = hands::get(off_h).grip > 0.6f;
        // the travel along the gun's back axis (-u), from the gun hand: the gun's own motion does not count
        const float s = -((oh[0] - bp.hand[gj][0]) * u[0] + (oh[1] - bp.hand[gj][1]) * u[1] + (oh[2] - bp.hand[gj][2]) * u[2]);
        const float hgt = (oh[0] - bp.hand[gj][0]) * up[0] + (oh[1] - bp.hand[gj][1]) * up[1] + (oh[2] - bp.hand[gj][2]) * up[2];
        g_bolt_want[off_h] = in || g_bolt_grip;
        if (grip && !g_grip_was && in) {
            g_bolt_grip = true;
            g_bolt_ctrl = off_h;
            // a bolt already back stays back (BoltParts: where it is drawn, its handle as turned)
            g_bolt_s0 = bparts ? s - bslide * kBoltStroke : g_bolt_back ? s - g_bolt_travel : s;
            g_bolt_h0 = hgt - blift * kBoltRise;
            g_ooo = false;
            controllers::pulse(off_h, 0.35f, 30);
            log::info("[actions] bolt (%d): gripped (%.3f m from its zone)%s", w, g_bolt_d, g_needs ? "" : ", the action already worked");
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
                    controllers::pulse(off_h, 0.3f, 10);
                    log::info("[actions] bolt (%d): the handle up", w);
                } else if (g_bolt_up && blift <= 0.05f) {
                    g_bolt_up = false;
                    if (gs) play_at(gs->close2, oh);
                    controllers::pulse(off_h, 0.5f, 15);
                    if (g_bolt_cycled) {
                        g_bolt_cycled = false;
                        g_needs = false;
                        g_cycles.fetch_add(1, std::memory_order_relaxed);
                        controllers::pulse(gun_h, 0.6f, 20);
                        log::info("[actions] bolt (%d): the handle down, chambered", w);
                    } else {
                        log::info("[actions] bolt (%d): the handle down", w);
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
                        controllers::pulse(off_h, 0.5f, 15);
                        controllers::pulse(gun_h, 0.3f, 10);
                        log::info("[actions] bolt (%d): back %.3f m", w, g_bolt_s);
                    } else if (!g_ooo) {  // drawn back while chambered: nothing (counted once a stroke)
                        g_ooo = true;
                        g_out_of_order.fetch_add(1, std::memory_order_relaxed);
                        log::info("[actions] bolt (%d): worked while chambered: nothing", w);
                    }
                } else if (g_bolt_back && g_bolt_s <= 0.015f) {
                    g_bolt_back = false;
                    g_bolt_cycled = true;
                    if (gs) play_at(gs->close, oh);
                    controllers::pulse(off_h, 0.5f, 15);
                    log::info("[actions] bolt (%d): forward (the handle down chambers it)", w);
                } else if (g_bolt_s <= 0.015f) {
                    g_ooo = false;
                }
            }
        } else if (g_bolt_grip && grip) {
            g_bolt_s = s - g_bolt_s0;
            if (!g_bolt_back && g_bolt_s >= g_bolt_travel) {
                if (g_needs) {
                    g_bolt_back = true;
                    controllers::pulse(off_h, 0.5f, 15);
                    controllers::pulse(gun_h, 0.3f, 10);
                    log::info("[actions] bolt (%d): back %.3f m", w, g_bolt_s);
                } else if (!g_ooo) {  // pulled back while chambered: nothing (counted once a stroke)
                    g_ooo = true;
                    g_out_of_order.fetch_add(1, std::memory_order_relaxed);
                    log::info("[actions] bolt (%d): worked while chambered: nothing", w);
                }
            } else if (g_bolt_back && g_bolt_s <= 0.015f) {
                g_bolt_back = false;
                g_needs = false;
                g_cycles.fetch_add(1, std::memory_order_relaxed);
                controllers::pulse(off_h, 0.6f, 20);
                controllers::pulse(gun_h, 0.6f, 20);
                log::info("[actions] bolt (%d): forward, chambered", w);
            }
        }
        if (!grip) g_bolt_grip = false;
        g_grip_was = grip;
    } else {
        g_bolt_grip = false;
        g_grip_was = hands::get(off_h).grip > 0.6f;
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
    if (brz && !deadeye) {
        const GunSounds* gs = gun_sounds(w);
        bool want_open = g_breech_open, moved = false;
        if (brz->gesture == 0) {  // the gun's flicks: down opens, up shuts (the lever under the grip)
            const bool down = !g_breech_open && (g_needs || !g_breech_round.load(std::memory_order_relaxed)) && now - g_breech_shut_ms >= 200.0 &&
                              g_w[0] <= -g_lever_speed;
            g_breech_flicks = down ? g_breech_flicks + 1 : 0;
            if (g_breech_flicks >= 2) want_open = true;  // two two-frame steps: not the shut's own back swing
            else if (g_breech_open && now - g_breech_ms >= 150.0 && g_w[0] >= g_lever_speed) want_open = false;
        } else if (pts && bp.hand_ok[gj] && bp.hand_ok[oj]) {
            // the zone: the part's handle as drawn, in the drawn gun's own frame (else about the gun hand, the model's
            // origin taken 8 cm ahead of the wrist along the barrel)
            float hz[3];
            for (int i = 0; i < 3; ++i) hz[i] = brz->shut[i] + (brz->open[i] - brz->shut[i]) * bdrawn;
            std::memcpy(g_breech_hz, hz, sizeof(g_breech_hz));
            for (int i = 0; i < 3; ++i)
                g_breech_zone[i] = bp.gun_frame_ok ? bp.gun_frame_o[i] + bp.gun_frame_R[i * 3] * hz[0] + bp.gun_frame_R[i * 3 + 1] * hz[1] + bp.gun_frame_R[i * 3 + 2] * hz[2]
                                                   : bp.hand[gj][i] + u[i] * (0.08f - hz[2]) + rgt[i] * hz[0] + up[i] * hz[1];
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
    g_breech_weapon.store(brz ? w : -1, std::memory_order_relaxed);
    // [Reload] SemiAutoParts: the bolt racked by the off hand
    const bool sparts = k == kSemi;
    float sdrawn = sparts ? g_semi_amt.load(std::memory_order_relaxed) : 0.0f;
    g_semi_want[0] = g_semi_want[1] = false;
    bool chamber = false;
    if (sparts && !deadeye && pts && bp.hand_ok[gj] && bp.hand_ok[oj]) {
        const float hz[3] = {kSemiHandle[0], kSemiHandle[1], kSemiHandle[2] + sdrawn * kSemiTravel};  // the handle as drawn
        std::memcpy(g_semi_hz, hz, sizeof(g_semi_hz));
        for (int i = 0; i < 3; ++i)
            g_semi_zone[i] = bp.gun_frame_ok ? bp.gun_frame_o[i] + bp.gun_frame_R[i * 3] * hz[0] + bp.gun_frame_R[i * 3 + 1] * hz[1] + bp.gun_frame_R[i * 3 + 2] * hz[2]
                                             : bp.hand[gj][i] + u[i] * (0.08f - hz[2]) + rgt[i] * hz[0] + up[i] * hz[1];
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
        float pg[3] = {};
        bool held = false;
        if (g_part_snap_cfg.load(std::memory_order_relaxed) && pts && bp.gun_frame_ok && !deadeye) {
            if (k == kBolt && bparts && g_bolt_grip && g_bolt_ctrl == off_h) {  // the zone is world: into the gun's frame
                for (int j = 0; j < 3; ++j)
                    for (int i = 0; i < 3; ++i) pg[j] += bp.gun_frame_R[i * 3 + j] * (g_bolt_zone[i] - bp.gun_frame_o[i]);
                held = true;
            } else if (k == kBreech && g_breech_grip) {
                std::memcpy(pg, g_breech_hz, sizeof(pg));
                held = true;
            } else if (k == kSemi && g_semi_grip) {
                std::memcpy(pg, g_semi_hz, sizeof(pg));
                held = true;
            }
        }
        if (held)
            for (int i = 0; i < 3; ++i) g_part_p[i].store(pg[i], std::memory_order_relaxed);
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
    if (parts && !deadeye) {
        if (!g_lever_open && g_needs && g_w[0] <= -g_lever_speed) {
            g_lever_open = true;
            g_hammer_down = false;
            g_lever_down_ms = now;
            if (pts && bp.hand_ok[gj]) play_at(gun_sounds(w) ? gun_sounds(w)->open : nullptr, bp.hand[gj]);
            controllers::pulse(gun_h, 0.35f, 15);
            log::info("[actions] lever (%d): opened by a flick down (%.1f rad/s)", w, g_w[0]);
        } else if (g_lever_open && g_w[0] >= g_lever_speed) {
            g_lever_open = false;
            g_needs = false;
            if (pts && bp.hand_ok[gj]) play_at(gun_sounds(w) ? gun_sounds(w)->close : nullptr, bp.hand[gj]);
            g_cycles.fetch_add(1, std::memory_order_relaxed);
            controllers::pulse(gun_h, 0.6f, 20);
            log::info("[actions] lever (%d): closed by a flick up (%.0f ms open), chambered", w, now - g_lever_down_ms);
        }
    } else if (k == kLever && g_needs && !deadeye) {  // lever actions: a flick of the gun hand, muzzle down then up, within the window
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
    if (k == kPump && g_needs && dual::state().on) g_needs = false;
    if (!g_needs && !g_pump_back) pd = 0.0f;
    g_pump_amt.store(pparts ? pd : 0.0f, std::memory_order_relaxed);
    g_pump_weapon.store(pparts ? w : -1, std::memory_order_relaxed);
    // the hold: open or not cycled; a pull while held is counted (a faint buzz), and after it a fresh pull is needed
    const bool block = !deadeye && (g_open || g_needs || (bparts && (blift > 0.05f || bslide > 0.0f)) || (brz && (g_breech_open || bdrawn > 0.05f)) ||
                                    (sparts && sdrawn > 0.05f));
    // the menu's clicks are not pulls (round 10: three refusals counted while the menu was open)
    const bool trig = gh.trigger > 0.6f && !menu::visible();
    if (block && trig && !g_trig_was) {
        g_held.fetch_add(1, std::memory_order_relaxed);
        controllers::pulse(gun_h, 0.45f, 35);  // felt, and unlike the empty click (round 10: an open full revolver felt dead)
        // round 13 item 13: "guns should click when attempting to fire without having performed the necessary reload
        // actions yet": the gun's family's click too ([Reload] EmptyClick's mode, volume and sounds)
        if (g_held_click.load(std::memory_order_relaxed)) audio::empty_click(gun_h, w);
        log::info("[actions] fire held back: the %s %s (pulls %llu)", kind_name(k), g_open || (bparts && g_bolt_up) ? "is open" : "is not worked",
                  static_cast<unsigned long long>(g_held.load()));
    }
    if (block && trig) g_rt_rearm = true;
    if (g_rt_rearm && gh.trigger < 0.15f) g_rt_rearm = false;
    g_trig_was = trig;
    g_blocked = block || g_rt_rearm.load();
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
    g_open_cyl = config::get_bool("Reload", "OpenCylinder", false);
    g_lever_parts = config::get_bool("Reload", "LeverParts", false);
    g_pump_parts = config::get_bool("Reload", "PumpParts", false);
    g_bolt_parts = config::get_bool("Reload", "BoltParts", false);
    g_breech_parts = config::get_bool("Reload", "BreechParts", false);
    g_semi_parts = config::get_bool("Reload", "SemiAutoParts", false);
    g_break = config::get_bool("Reload", "ManualBreak", false);
    g_barrel_r = config::get_float("Reload", "BarrelGripRadius", 0.15f);
    g_barrel_snap_cfg = config::get_bool("Reload", "BarrelHandSnap", true);
    g_part_snap_cfg = config::get_bool("Reload", "PartHandSnap", true);
    g_held_click = config::get_bool("Reload", "HeldClick", true);
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

void filter_stick(float* rx, float* ry) {
    if (!g_on.load(std::memory_order_relaxed) || !rx || !ry) return;
    const Kind k = kind_of(g_weapon);  // the frame end's weapon (the same thread)
    if (!(opens(k) && kind_on(k))) {
        g_stick_armed = true;
        return;
    }
    const float x = *rx, y = *ry;
    const bool cone = y < 0.0f && std::fabs(x) <= 0.6f * std::fabs(y);
    // a flick: past OpenStickY and back to the centre within OpenFlickMs; a held push opens nothing (round 10: a full
    // revolver opened by a push down, probably not meant). OpenFlickMs 0: the push itself opens, as in run 3.
    const double now = log::now_ms();
    if (cone && y <= -g_open_y && g_stick_armed) {
        g_stick_armed = false;
        if (g_flick_ms <= 0.0f)
            g_open_req = true;
        else
            g_flick_t0 = now;
    }
    if (g_flick_t0 > 0.0 && now - g_flick_t0 > g_flick_ms) g_flick_t0 = 0.0;  // held too long: not a flick
    if (std::fabs(y) < 0.3f) {
        if (g_flick_t0 > 0.0) g_open_req = true;
        g_flick_t0 = 0.0;
        g_stick_armed = true;
    }
    if (cone && std::fabs(y) > 0.5f) *rx = *ry = 0.0f;  // a mostly-down push neither turns nor pitches the camera
}

bool fire_blocked() { return g_on.load(std::memory_order_relaxed) && g_blocked.load(std::memory_order_relaxed); }
bool grip_wanted(int ctrl) {
    return ctrl >= 0 && ctrl < 2 && g_on.load(std::memory_order_relaxed) &&
           (g_bolt_want[ctrl].load(std::memory_order_relaxed) || g_barrel_want[ctrl].load(std::memory_order_relaxed) ||
            g_breech_want[ctrl].load(std::memory_order_relaxed) || g_semi_want[ctrl].load(std::memory_order_relaxed));
}
bool break_open() { return g_on.load(std::memory_order_relaxed) && g_break_open.load(std::memory_order_relaxed); }
float barrel_snap() { return g_on.load(std::memory_order_relaxed) ? g_barrel_snap.load(std::memory_order_relaxed) : 0.0f; }
bool barrel_hand_snap() { return g_barrel_snap_cfg.load(std::memory_order_relaxed); }
float part_snap(float gun_p[3]) {
    for (int i = 0; i < 3; ++i) gun_p[i] = g_part_p[i].load(std::memory_order_relaxed);
    return g_on.load(std::memory_order_relaxed) ? g_part_snap.load(std::memory_order_relaxed) : 0.0f;
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

int hint(float pos[3]) {
    if (!g_on.load(std::memory_order_relaxed) || !g_hints.load(std::memory_order_relaxed)) return 0;
    std::lock_guard lock(g_mutex);
    if (kind_of(g_weapon) == kBreech && g_breech_parts.load(std::memory_order_relaxed)) {  // BreechParts
        if (g_breech_open && !g_breech_round.load(std::memory_order_relaxed)) return 1;  // open: a round goes in
        if (!g_needs || g_breech_open) return 0;
        const Breech* b = breech_of(g_weapon);
        if (b && b->gesture) {
            if (pos) std::memcpy(pos, g_breech_zone, sizeof(g_breech_zone));
            return 2;  // the part's zone
        }
        return 3;  // a flick
    }
    if (kind_of(g_weapon) == kSemi && g_semi_parts.load(std::memory_order_relaxed)) {  // SemiAutoParts: its handle to rack
        if (!g_needs) return 0;
        if (pos) std::memcpy(pos, g_semi_zone, sizeof(g_semi_zone));
        return 2;
    }
    if (g_open) return 1;
    if (!g_needs) return 0;
    switch (kind_of(g_weapon)) {
    case kBolt:
        if (pos) std::memcpy(pos, g_bolt_zone, sizeof(g_bolt_zone));
        return 2;
    case kLever: return 3;
    case kPump: return 4;
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
void round_in(int weapon, const float at[3]) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    g_hand_round_ms.store(log::now_ms(), std::memory_order_relaxed);
    if (const GunSounds* gs = gun_sounds(weapon)) play_at(gs->insert, at);
    const Kind k = kind_of(weapon);
    if ((k == kRevolver || k == kTopBreak) && weapon >= 0 && weapon < 40) g_cyl_rounds[weapon].fetch_add(1, std::memory_order_relaxed);
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
    if (g_open_cyl.load(std::memory_order_relaxed) && (ck == kRevolver || ck == kTopBreak) && g_revolver.load(std::memory_order_relaxed)) {
        d[kDrvIndex] = g_cyl_drawn.load(std::memory_order_relaxed);
        if (d[kDrvIndex] != 0.0f) mask |= 1u << kDrvIndex;
        *weapon = cw;
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
    return mask;
}
bool semi_auto_parts() { return g_semi_parts.load(std::memory_order_relaxed); }
void set_semi_auto_parts(bool on) {
    if (g_semi_parts.exchange(on) != on) log::info("[actions] the semi-auto shotgun racked by hand, drawn: %s", on ? "on" : "off");
    config::set("Reload", "SemiAutoParts", on ? "1" : "0");
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
    if (!g_on.load(std::memory_order_relaxed) || !g_load_open.load(std::memory_order_relaxed)) return true;
    const Kind k = kind_of(weapon);
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
        if (w == "reset") {
            std::lock_guard lock(g_mutex);
            g_needs = g_open = false;
            g_lever_open = g_hammer_down = false;
            g_bolt_back = g_bolt_up = g_bolt_cycled = false;
            g_pump_back = g_pump_have = false;  // the pump's stroke too (a stroke left back held the next phase's)
            g_breech_open = g_breech_grip = false;
            g_breech_round.store(true, std::memory_order_relaxed);
            g_semi_back = g_semi_grip = false;
            g_bolt_lift.store(0.0f, std::memory_order_relaxed);
            g_bolt_slide.store(0.0f, std::memory_order_relaxed);
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
    // the gun kind in hand's section (the test channel's line is cut at about 1000 characters)
    const Kind gk = g_kind;
    if (all || gk == kBolt) {
        std::snprintf(b, sizeof(b), " | bolt zone (%.3f %.3f %.3f) hand %.3f m, gripped %d back %d s %.3f parts %d up %d lift %.2f slide %.2f", g_bolt_zone[0],
                      g_bolt_zone[1], g_bolt_zone[2], g_bolt_d, g_bolt_grip ? 1 : 0, g_bolt_back ? 1 : 0, g_bolt_s, g_bolt_parts.load() ? 1 : 0, g_bolt_up ? 1 : 0,
                      g_bolt_lift.load(), g_bolt_slide.load());
        o += b;
    }
    if (all || gk == kPump) {
        std::snprintf(b, sizeof(b), " | pump %d parts %d drawn %.2f travel %.3f", g_pump_back ? 1 : 0, g_pump_parts.load() ? 1 : 0, g_pump_amt.load(), g_pump_s);
        o += b;
    }
    if (all || gk == kLever) {
        std::snprintf(b, sizeof(b), " | lever parts %d open %d drawn %.2f hammer %.2f", g_lever_parts.load() ? 1 : 0, g_lever_open ? 1 : 0, g_lever_amt.load(),
                      g_hammer_amt.load());
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
    if (all || gk == kSemi) {
        std::snprintf(b, sizeof(b), " | semi %d back %d drawn %.2f gripped %d semi zone (%.3f %.3f %.3f) %.3f m", g_semi_parts.load() ? 1 : 0, g_semi_back ? 1 : 0,
                      g_semi_amt.load(), g_semi_grip ? 1 : 0, g_semi_zone[0], g_semi_zone[1], g_semi_zone[2], g_semi_d);
        o += b;
    }
    return o;
}

}  // namespace rdrvr::actions
