#pragma once

#include <cstdint>
#include <string>

// Working each gun's action by hand ([Reload] ManualActions, off; run 3 item 8, research\run3\manualactions.md 6):
// revolvers open by a flick of the right stick down and close by a flick of the gun hand (a wrist roll or a sideways
// flick; the Schofield, a top-break, also upward); bolt actions are cycled by the other hand gripping the bolt and
// pulling it back, then forward; lever actions by a down-then-up flick of the gun hand; the pump by the front hand
// sliding the fore-end back, then forward (two-handed). Until the action is worked the trigger's pull is held back
// from the game (RT 0), so the game's own ammo count is never touched. Input level only: no game memory, no natives.
// The bow is not in the game's data (BLOCKED, research 4).
struct RdrvrActorState;
namespace rdrvr::body {
struct BodyPoints;
}
namespace rdrvr::dual {
struct State;
}
namespace rdrvr::actions {

void init();  // a frame-end listener (after the holsters')

bool enabled();
void set_enabled(bool on);  // also written to the user ini

// controls.cpp, each pad poll (the presenting thread): the turn stick after the layout swap and the menu chord, and its
// click (the layout's right stick's). A revolver in hand: a flick down opens it ([Reload] RevolverControl=cock: cocks a
// single action; held down, the click opens it and is consumed, not the game's); the stick in the downward cone is
// consumed (no turn, no camera pitch).
// seated: riding or driving (a revolver drawn into the off controller is then still this stick's: the off stick steers)
void filter_stick(float* rx, float* ry, bool* click = nullptr, bool seated = false);
// [Reload] OpenStickY and OpenFlickMs, as the flicks above use them (controls.cpp's flick up, FlickUpRB)
float open_stick_y();
bool second_open();  // DualWieldOpen: the second gun (or the copy) open now
float open_flick_ms();
// [Hands] DualWieldOpen (2026-10-28, off; round 37 check 4): the second gun (DualWield's revolver of another slot, or
// DualWieldCopy's copy of a revolver), not carried and shut, opened by its own hand's stick held down and clicked, shut by
// a flick of that hand's roll or yaw; Dead Eye shuts it; open, its pull is held. Its state apart from the gun in hand's.
// filter_second_stick: controls.cpp, each pad poll after filter_stick: the layout's left stick (sx, sy) and its click,
// whose controller is `ctrl` (the second gun in the other controller: refused, said once). Held down and clicked: the
// open asked (riding too), the click consumed until let go, the stick zeroed from it until it comes back toward the centre.
// (2026-10-29) While armed: the stick down in the open cone held at the middle for DualWieldOpenWaitMs before its walk back
// passes, and the click held back for DualWieldClickHoldMs before it is the game's (L3, or the brake riding); a quick click
// sends nothing; a click passed is kept down 120 ms at least. A click held back and then the stick down to the hold within
// that wait: the open too. other_click: the other stick's click down (the recentre chord: the click passes at once).
// seated: riding (the stick's walk wait off: the horse's, as before). skip: not filtered this poll (the wheel's D-pad):
// the state reset, nothing held back or to come late. *click may be set (a passed click's least length).
void filter_second_stick(int ctrl, float* sx, float* sy, bool* click, bool other_click, bool seated, bool skip);
// the left click now passing waited DualWieldClickHoldMs (controls.cpp: PhysicalCrouch's chord wait not added on top)
bool second_click_waited();
// a left press held back now (within DualWieldClickHoldMs): controls::left_click_held (PhysicalCrouch's guard) counts it
bool second_press_pending();
bool dual_open();
float dual_open_wait_ms();   // [Hands] DualWieldOpenWaitMs (250; 0 - 600)
float dual_click_hold_ms();  // [Hands] DualWieldClickHoldMs (200; 0 - 600)
void set_dual_open_waits(float walk_ms, float click_ms, bool save);  // save: also written to the user ini (the menu)
void set_dual_open(bool on, bool save);  // save: also written to the user ini (the menu; "actions second on|off" is the session's)
// holster.cpp's gate for the second gun's rounds (DualWieldCrossReload): as before (the copy always; another slot's gun
// by can_load, so a revolver refused with LoadOnlyWhenOpen), with DualWieldOpen a revolver only while it is open
bool can_load_second(const dual::State& ds);  // (2026-10-31) a carried gun (ds.carry): can_load_carried
bool can_load_carried(int weapon);
bool muzzle_loader(int weapon);  // (step 8, R14) the Blunderbuss with BreechParts: loaded at its muzzle, then rammed
void carried_round_in(int weapon);  // (R17) rounds went into the carried gun: its carried state takes them (a breech's round)
// dual.cpp, a fresh pull of the second gun's trigger on controller ctrl: held while it is open (true; a buzz, counted)
bool second_pull_held(int ctrl);
// body.cpp, the second gun's draw (copy false) or the copy's prop's (true): its opening's driver (kDrvOpen) with
// [Reload] OpenCylinder, and the eWeapon whose parts to draw; mask 0 when none. Any thread.
unsigned second_drivers(bool copy, int* weapon, float* d);
// The gun's action is not worked (open, or a shot not cycled): RT goes to the game as 0, and after it a fresh pull.
bool fire_blocked();
// A hand at the bolt (the controller): its grip is the action's, never LB/RB (as a holster's).
bool grip_wanted(int ctrl);
// [Reload] PartHandSnap (on; round 13): while the off hand grips an action's part (a bolt, a breech's trapdoor or spur,
// the semi-auto's bolt handle), its handle in the drawn gun's own frame (m: x right, y up, z back) and the hand's
// weight on it (0-1, eased 0.1 s either way). Any thread.
// (2026-10-28, the user: "Bolt actions don't seem to have a snap on pose for grabbing the bolt") turn_deg: the handle's
// turn about the gun's z as drawn (the bolt's lift times kBoltTurnDeg; 0 for the other parts). bolt: the part is a bolt's
// handle, on which body.cpp holds the hand as it took hold (turned with the handle), its fingers closed on it (the review:
// the other parts as before, the hand at the controller's turn)
float part_snap(float gun_p[3], float* turn_deg = nullptr, bool* bolt = nullptr);
// The same for [Reload] CarriedBolt's bolt gripped by the free gun hand: the handle as drawn (hand_p) and the carried
// gun's axes (gun_R, columns x right, y up, z back) in the carrying hand's target frame (body.cpp's ik target: its
// orientation's columns and its place), the handle's turn, John's carrying hand; the weight (0-1, eased 0.1 s). Any thread.
float carried_part_snap(float hand_p[3] = nullptr, float gun_R[9] = nullptr, float* turn_deg = nullptr, int* carry_j = nullptr);
constexpr float kBoltTurnDeg = 59.7f;  // the bolt handle's turn up (body.cpp's kGunModels rows: the Bolt Action's own clips)
bool part_hand_snap();
void set_part_hand_snap(bool on);
// [Reload] HeldClick (on; round 13 item 13): a pull held back (the gun open or its action not worked) also plays the
// gun's empty click
bool held_click();
void set_held_click(bool on);

// holster.cpp: rounds go into a revolver only while it is open ([Reload] LoadOnlyWhenOpen, with ManualRevolver).
// [Reload] ActionHints: while the action holds the gun in hand's shot, what it wants: 1 a revolver open (rounds go in,
// a flick closes it), 2 the bolt (pos: its zone), 3 the lever (a flick), 4 the pump; 5 (2026-10-31, R11) shut it: a breech
// open with its round, a revolver or break-open open and full, a bolt opened with nothing to cycle; 0 nothing. Any thread.
int hint(float pos[3]);
// [Reload] CarriedBolt and CarriedPump (2026-10-30, round 52: "Glow points disappear when gun is being held by only the
// foregrip"): hint()'s want for the gun carried by its foregrip ([Hands] CarryByForegrip), for the places: 2 its bolt to
// work (pos: the carried bolt's zone, world, as last stepped: only with the gun hand free, as the bolt is worked), 4 its
// pump to work (by the carrying hand's jerk), 0 none. ActionHints as hint(). Any thread.
int carried_hint(float pos[3]);
// [Reload] CarriedBoltTrigger (round 55): the point within the carried bolt's zone (its trigger the bolt's, its grip the
// take-back's there too: holster.cpp's carry_near)
bool carried_bolt_near(const float p[3]);
bool can_load(int weapon);
// [Reload] OpenCylinder (run 5 item 3, off): how far the gun in hand's revolver is drawn open, 0 closed - 1 open (eased
// over a quarter second), and its eWeapon; 0 when off or not a revolver open. Any thread.
float open_amount(int* weapon);
bool open_cylinder();
void set_open_cylinder(bool on);  // also written to the user ini
// [Reload] LeverParts (off): the lever guns worked by hand and drawn so (the Carbine: research\run3\manualactions.md
// 6.9; run 6 item 9c: the Winchester, the Henry, the Evans, the Volcanic). A flick down opens the lever and it stays
// open; a flick up closes it (chambered after a shot). body.cpp draws the lever, the slide and the hammer from these
// and holds the gun still in the hand through John's fire clip.
bool lever_parts();
void set_lever_parts(bool on);  // also written to the user ini
// How the lever is worked: 4 hold (the gun's flicks, down opens and up shuts, while the right stick is held down: the one
// way, 2026-11-02, GOAL-older-items item 3); the tests' only (their ini's [Reload] LeverControl, or "actions levercontrol"):
// 0 flick (the gun down then up), 1 stick (a flick of the right stick down; the Volcanic's VolcanicControl=stick too), 2 lock
// (the gun's flick, the drawn gun's orientation held while the other hand's trigger is held: lever_lock(), body.cpp), 3 front
int lever_control();
// (2026-11-06, the user: "simpler lever action use", the stick's flick and the gun's back) the menu's four: 4 hold, 5 roll
// (the gun hand rolled LeverRollDeg either way opens the lever, rolled back shuts it), 1 stick, 0 flick (the gun flicked
// down then up); also written to the user ini
void set_lever_control(int mode, bool save);
float lever_roll_deg();  // [Reload] LeverRollDeg (8 - 60, 20 by default)
void set_lever_roll_deg(float deg, bool save);
// [Reload] VolcanicControl (2026-10-26): the Volcanic's lever by a flick of the right stick down (as LeverControl=stick)
bool volcanic_stick();
void set_volcanic_stick(bool on);  // also written to the user ini
// [Reload] RevolverControl (2026-10-26): 0 open (a flick of the stick opens a revolver), 1 cock (the single actions, the
// Cattleman, the Schofield and the LeMat, cocked by the flick after each shot; every revolver opened by the stick held
// down and clicked)
int revolver_control();
void set_revolver_control(int mode);  // also written to the user ini
// The single action in hand cocked by hand (RevolverControl=cock), else -1; its drawn hammer (0 down - 1 cocked). body.cpp
// stops its own fire clip (its re-cock) and draws the hammer from it. Any thread.
int hammer_weapon(float* hammer);
// [Reload] ThumbCock (2026-10-28, on; acts with RevolverControl=cock): the stick's cock drawn by John's thumb on the hammer.
// thumb_on_hammer: the single action in hand as hammer_weapon (else -1), its drawn hammer, and the thumb's weight on its
// spur now (0-1: on it about 90 ms after the flick, resting 20 ms, riding the hammer back the next 90 ms, off over 100 ms; 0 with the
// option off or no flick). Any thread.
bool thumb_cock();
void set_thumb_cock(bool on);  // also written to the user ini
int thumb_on_hammer(float* weight, float* hammer);
// [Reload] FanHammer (on; acts with RevolverControl=cock): the other hand's palm swept back across a single action's
// hammer cocks it; with the gun's trigger held, the shot goes at once (fanning)
bool fan_hammer();
void set_fan_hammer(bool on);  // also written to the user ini
// (2026-11-06, another player: "fanning the revolver hit box to be larger/more sensitive") the hammer's zone, [Reload]
// FanZoneRadius (0.02-0.3 m round the hammer), and the sweep's speed, [Reload] FanSpeed (0.2-6 m/s); set_ also writes the
// user ini when save
float fan_zone_radius();
void set_fan_zone_radius(float m, bool save);
float fan_speed();
void set_fan_speed(float mps, bool save);
// [Reload] ShowFanZone (2026-11-06, the user: "an option to see the zone represented"; off by default): the zone drawn as
// a ring while fanning could act (a single action in hand, RevolverControl=cock). fan_zone: its place (world), radius and
// state (0 the hammer down and a sweep would cock it, 1 the hand in it, 2 the hand to leave it first, 3 the hammer
// back); false while it is hidden or not placed within 100 ms. The frame end's thread (as action_hint)
bool show_fan_zone();
void set_show_fan_zone(bool on, bool save);  // save: also written to the user ini
bool fan_zone(float pos[3], float* radius, int* state);
bool lever_lock();                 // the drawn gun's orientation held now (any thread)
// 3 front (run 14): the gun's flicks; held two-handed with the lever waiting, body.cpp carries the gun in the front hand
bool lever_front();                // the lever waits to be worked under LeverControl=front (any thread)
// 4 hold (2026-10-28): the right stick held with the lever to work (or shut under 200 ms ago): body.cpp holds the drawn
// gun (one-handed: the gun hand's controller pose, its orientation and place; two-handed: carried by the front hand)
bool lever_hold_lock();            // (any thread)
// holster.cpp: a round put into the gun in hand by hand (at `at`, world): its own insert sound there, for the guns that
// have one while their switch is on (the Double-action with OpenCylinder).
// n: the rounds put in at once (one sound; a revolver's cylinder turned n chambers)
// second (2026-10-27, [Hands] DualWieldCrossReload): the rounds went into the dual-wielded second gun, not the gun in
// hand: its insert sound only (the cylinder count, the breech's round and the hand-load time are the gun in hand's)
void round_in(int weapon, const float at[3], int n = 1, bool second = false);
// [Reload] PumpParts (off; run 6 item 9e): the Pump-action's fore-end drawn where the front hand has it (back along the
// barrel 1:1 to its rear stop, unlocked by a shot), John's front hand drawn on it; the game's own pump stopped.
bool pump_parts();
void set_pump_parts(bool on);  // also written to the user ini
// The drawn fore-end (0 forward - 1 at the rear stop) of the Pump-action in hand with PumpParts; 0 otherwise. Any thread.
float pump_drawn();
// [Reload] BoltParts (off; run 6 item 9f): the bolt actions' bolts worked by the off hand and drawn so (the handle turned
// up by the hand's rise, the bolt drawn back and forward by its pull, the handle down chambers); rounds in only with
// the bolt back; the game's own bolt work stopped.
bool bolt_parts();
void set_bolt_parts(bool on);  // also written to the user ini
// [Reload] CarriedBolt (2026-10-28, off; research/run28/bolt.md): a bolt gun carried by its foregrip ([Hands]
// CarryByForegrip) has its bolt worked by the free gun hand by the same strokes (with BoltParts: up, back, forward, down;
// else back and forward), at the bolt's place on the carried gun, measured against the carried gun. Its bolt's state goes
// with it: a shot not cycled may be handed over, is kept through the carry, and the take-back finds the bolt as the gun
// hand left it (worked while carried: chambered). A grip nearer the carried gun's grip than its bolt stays the take-back;
// the carried gun never fires.
bool carried_bolt();
void set_carried_bolt(bool on);  // also written to the user ini
// [Reload] CarriedBoltTrigger (2026-10-30, round 55; on): the carried bolt worked with the free gun hand's trigger (its
// grip then always the take-back); carried_trigger_wanted(ctrl): that controller's trigger is the bolt's now (at its zone
// or holding it): the game's RT held back
bool carried_bolt_trigger();
// [Reload] OffHandBoltTrigger (2026-11-01, GOAL-offhand-changes C5; on): the gun in hand's bolt worked with the off hand's
// trigger (its LT held from the game while that hand is at the bolt or holds it), not its grip
bool offhand_bolt_trigger();
// (2026-11-01, GOAL-offhand-changes C3) the second gun's (or the copy's) stick cock: its eWeapon while its thumb is on the
// hammer (by weight, as thumb_on_hammer's), -1 none
int second_thumb_on_hammer(float* weight);
// (2026-11-01, GOAL-offhand-changes C7) PistolMagazines for the second gun: its magazine out (by its stick's flick) and room:
// a magazine for it from the chest; second_mag_insert puts one in (the rounds put in, 0 none)
bool second_mag_wanted(const dual::State& ds);
bool second_mag_pistol(const dual::State& ds);  // a second pistol reloaded by its magazine (no rounds alone, no squeeze)
int second_mag_insert(const dual::State& ds, int32_t actor);  // (-1: not now, the copy's spare settling: hold on)
// [Reload] RamrodAtChest (2026-11-01, GOAL-offhand-changes C15; on): the Blunderbuss's ramrod taken at the chest once its round
// is down the barrel (ramrod_ready: the off hand's press there takes it, take_ramrod), then in and out of the barrel
bool ramrod_ready();
// (2026-11-02, C15h) the controller whose press at the chest takes the ramrod now: the off hand for the Blunderbuss in hand,
// the free gun hand for one carried by the off hand; -1 none (RamrodFrom=gun: never the chest's)
int ramrod_taker();
void take_ramrod(int ctrl);  // (ctrl must be ramrod_taker())
int ramrod_hand();  // the controller holding it, -1 none
bool ramrod_at_chest();
void set_ramrod_at_chest(bool on);  // also written to the user ini
// (2026-11-02, round 67 check 3) [Reload] RamrodOffset: the held ramrod's head moved from the hand's grab point (right, up,
// forward in the hand's target axes; m; 0 -0.04 0: 4 cm lower, the user's "lower in hand")
// (2026-11-02, GOAL-offhand-changes C15d; round 68 check 3: adjustable as the round) hand: the controller holding it (0 left,
// 1 right; RamrodOffsetRight, RamrodTurnRight, RamrodTiltRight: the right's own once set); turn, tilt: the rod's direction
// from where the hand points, about the hand's up, then its right (degrees)
struct RodPlace {
    float offset[3];
    float turn, tilt;
};
RodPlace ramrod_place(int hand);
void set_ramrod_place(int hand, const RodPlace& p, bool save);
// (C15e; round 68 check 3: the ramrod taken from its place on the gun) [Reload] RamrodFrom: 1 the gun (its rod under the
// barrel, the off hand's grip at RamrodGrabOffset from its grab point, the gun's frame: right, up, forward), 0 the chest
bool ramrod_from_gun();
void set_ramrod_from_gun(bool on, bool save);
void ramrod_grab_offset(float o[3]);
void set_ramrod_grab_offset(const float o[3], bool save);
void set_offhand_bolt_trigger(bool on);  // also written to the user ini
void set_carried_bolt_trigger(bool on);  // also written to the user ini
bool carried_trigger_wanted(int ctrl);
// [Reload] CarriedPump (2026-10-29, off): the Pump-action carried by its foregrip ([Hands] CarryByForegrip) is pumped
// by the carrying hand alone: a jerk back along the barrel (CarriedPumpSpeed, 1.5 m/s, or faster) opens it, a jerk
// forward closes it (with PumpParts its fore-end drawn so; the game's pump sounds). Its pump's state goes with it as
// CarriedBolt's bolt: a shot not pumped may be handed over, is kept through the carry, and the take-back finds it as the
// carrying hand left it. Works with a sidearm in the gun hand too. The carried gun never fires.
bool carried_pump();
void set_carried_pump(bool on);  // also written to the user ini
// holster.cpp's hand-over (CarryByForegrip): nothing of the gun in hand's action held back, or (CarriedBolt) only a bolt
// gun's shot not cycled with its bolt shut and not in a hand
bool carry_handover_ok();
// body.cpp, the carried gun's draw: the carried bolt gun's eWeapon and its bolt's drivers (kDrvBoltLift, kDrvBoltSlide)
// with BoltParts, or (CarriedPump) the carried Pump-action's fore-end (kDrvPump) with PumpParts; mask 0 when none. Any thread.
unsigned carry_drivers(int* weapon, float* d);
// [Reload] BreechParts (off; run 6 item 9g): the single-shot rifles worked by hand and drawn so (the Springfield's
// trapdoor flipped up by the off hand, the Rolling Block's block rolled back by its pull, the Buffalo's block by the
// gun's flicks); open, one round in by hand; shut with it, ready; the game's own reload after each shot stopped.
bool breech_parts();
void set_breech_parts(bool on);  // also written to the user ini
// [Reload] SemiAutoParts (off; run 6 item 9h): the Semi-Auto Shotgun's bolt racked by the off hand after loading it
// from empty (drawn back with the hand, it springs forward let go: a shell chambered); its cycling between shots stays.
bool semi_auto_parts();
void set_semi_auto_parts(bool on);  // also written to the user ini
// [Reload] PistolMagazines (2026-10-28, off; research\run28\magazines.md): the Semi-Automatic Pistol and the High Power
// drop their magazine by a flick of the right stick down (a round stays chambered if the gun was not empty; the
// magazine's rounds go back to the spare) and take a new one in the other hand (the chest, or [Reload] RoundSource)
// pushed up into the grip's well: loaded to the gun's size from the spare. The Mauser's bolt is pulled back by its ears
// (held open on an empty magazine, or pulled back with room) and a stripper clip pushed down into its top loads up to
// ten. No single rounds go into these three.
bool pistol_mags();
void set_pistol_mags(bool on);  // also written to the user ini
// [Reload] PistolSlides (2026-10-28, off): the three pistols' slides (the Semi-Auto's toggle, the Mauser's bolt) racked
// by the other hand (gripped, pulled back, let go; the game's sounds on the steps, drawn): a rack chambers a round from
// the magazine and ejects one already chambered (lost); the last shot locks the slide back; with nothing chambered the
// trigger fires nothing; the game's own fire clip stopped (the slide's shot drawn by the mod).
bool pistol_slides();
void set_pistol_slides(bool on);  // also written to the user ini
// holster.cpp: the gun in hand is loaded by magazine (the Mauser by a stripper clip) now: PistolMagazines on with
// ManualActions, one of the three. mag_wanted: it takes one now (its magazine out; the Mauser: no clip seated, room in
// st's count), spare rounds, no Dead Eye nor the game's reload.
bool mag_gun(int weapon);
bool mag_wanted(const RdrvrActorState& st);
// Where the gun in hand's magazine goes in (the well under the grip; the Mauser's clip guide on top), world, and the
// gun's axes (9 floats, columns x right, y up, z back), from the body's points (the drawn gun's frame, else the gun
// hand's target). False when not known.
bool mag_point(const body::BodyPoints& bp, int weapon, float out[3], float axes[9]);
// A magazine (the Mauser: a stripper clip) pushed into the gun in hand at `at` (world, its sound there): the clip
// topped up from the spare (the Mauser: up to the clip's ten, as there is room). False when the gun takes none now.
// rounds: the rounds put in.
bool insert_mag(const RdrvrActorState& st, const float at[3], int* rounds);
// The pistols' drawn parts (body.cpp's kGunModels rows): the Semi-Auto's toggle (its rear link turned about its hinge to
// kToggleDeg; the front link carried by the knee's rise and move back, turned about the knee by its curve), the High
// Power's slide and the Mauser's bolt straight back (the game's fire clips' travels as optimumbox measured them, MIT [I])
constexpr float kToggleDeg = 66.6f;
constexpr float kToggleHinge[3] = {0.0f, 0.06692f, 0.11253f}, kToggleKnee[3] = {0.0f, 0.07101f, 0.08351f};  // the model's bones 1, 2
constexpr float kToggleDy = 0.02417f, kToggleDz = 0.02125f, kToggleFrontDeg = -32.24f;
constexpr float kSlideTravelHP = 0.0328f, kBoltTravelMauser = 0.0205f;
// The gun in hand whose action the player works by hand and whose parts are drawn so (a lever gun with LeverParts, the
// Pump-action with PumpParts, the bolt actions with BoltParts, the single shots with BreechParts, the Semi-Auto Shotgun
// with SemiAutoParts): its
// eWeapon, else -1. body.cpp stops that gun's own fire clip and holds it still through John's, as its model says.
int parts_weapon();
// How long since the last shot seen (the game's count dropped, the manual actions on), ms; huge before any. Any thread.
double since_shot_ms();
// The gun in hand's drawn lever (0 closed - 1 open) and hammer (0 down - 1 cocked), eased; false when off. Any thread.
bool lever_drawn(int* weapon, float* lever, float* hammer);
// Run 6 item 9: the drivers of the gun in hand's drawn parts (body.cpp's kGunModels), set by the player's gestures: the
// opening (0 shut - 1 open), the lever (0 closed - 1 open), the hammer (0 down - 1 cocked), the cylinder's index (the
// rounds put in by hand, eased: each turns it one chamber), the pump (0 forward - 1 back), the bolt's lift and slide
// (0 - 1), a breech (0 shut - 1 open).
// the break actions' barrels opened by hand (round 13: further than the game's own 31 degrees)
constexpr float kBreakOpenDeg = 45.0f;
// (2026-11-01, C15b) kDrvRod: the Blunderbuss's rod in the hand holding it (rod_slide's place; its amount 1 while held)
enum Driver { kDrvOpen, kDrvLever, kDrvHammer, kDrvIndex, kDrvPump, kDrvBoltLift, kDrvBoltSlide, kDrvBreech, kDrvRod, kDrvCount };
// (C15b; round 65 check 15, the user: the ramrod did not appear) the ramrod held ([Reload] RamrodAtChest): its head's move in
// the Blunderbuss's model (m, x right, y up, z back) from where it lies under the barrel to the holding hand; (C15c; round 66
// check 7: it stayed turned one way) Q (3x3 rows, the model's frame) its turn about its head, its body (+z from the head)
// along where that hand points. Any thread.
bool rod_place(float Q[9], float s[3]);
// The gun in hand's eWeapon and the drivers' amounts (d[kDrvCount]); the mask has a bit (1 << Driver) for each driver
// whose switch is on for that gun (its parts are drawn from it). 0: none. Any thread.
unsigned gun_drivers(int* weapon, float* d);
// [Reload] ManualBreak (off): the Double-barrel and the Sawed-off broken open by the stick's flick (their barrels drawn tipped down), shut
// by a flick of the gun upward or by the off hand gripping the open barrels and swinging them up; loaded only open
bool manual_break();
void set_manual_break(bool on);  // also written to the user ini
// The Double-barrel open (holster.cpp: the two-handed hold waits; the off hand is for the barrels). Any thread.
bool break_open();
// [Reload] BarrelHandSnap (run 6 item 7, on): the weight (0-1, eased over 0.1 s) of John's off hand drawn on the
// Double-barrel's open barrels while that hand holds them; 0 when off or not held. Any thread.
float barrel_snap();
bool barrel_hand_snap();
void set_barrel_hand_snap(bool on);  // also written to the user ini

std::string command(const std::string& line);  // "actions [on|off] [open] [reset]": the state, counters, speeds

}  // namespace rdrvr::actions
