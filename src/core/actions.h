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
namespace rdrvr::actions {

void init();  // a frame-end listener (after the holsters')

bool enabled();
void set_enabled(bool on);  // also written to the user ini

// controls.cpp, each pad poll (the presenting thread): the turn stick after the layout swap and the menu chord. A
// revolver in hand: a flick down opens it; the stick in the downward cone is consumed (no turn, no camera pitch).
void filter_stick(float* rx, float* ry);
// The gun's action is not worked (open, or a shot not cycled): RT goes to the game as 0, and after it a fresh pull.
bool fire_blocked();
// A hand at the bolt (the controller): its grip is the action's, never LB/RB (as a holster's).
bool grip_wanted(int ctrl);
// [Reload] PartHandSnap (on; round 13): while the off hand grips an action's part (a bolt, a breech's trapdoor or spur,
// the semi-auto's bolt handle), its handle in the drawn gun's own frame (m: x right, y up, z back) and the hand's
// weight on it (0-1, eased 0.1 s either way). Any thread.
float part_snap(float gun_p[3]);
bool part_hand_snap();
void set_part_hand_snap(bool on);
// [Reload] HeldClick (on; round 13 item 13): a pull held back (the gun open or its action not worked) also plays the
// gun's empty click
bool held_click();
void set_held_click(bool on);

// holster.cpp: rounds go into a revolver only while it is open ([Reload] LoadOnlyWhenOpen, with ManualRevolver).
// [Reload] ActionHints: while the action holds the gun in hand's shot, what it wants: 1 a revolver open (rounds go in,
// a flick closes it), 2 the bolt (pos: its zone), 3 the lever (a flick), 4 the pump; 0 nothing. Any thread.
int hint(float pos[3]);
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
// holster.cpp: a round put into the gun in hand by hand (at `at`, world): its own insert sound there, for the guns that
// have one while their switch is on (the Double-action with OpenCylinder).
void round_in(int weapon, const float at[3]);
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
// [Reload] BreechParts (off; run 6 item 9g): the single-shot rifles worked by hand and drawn so (the Springfield's
// trapdoor flipped up by the off hand, the Rolling Block's block rolled back by its pull, the Buffalo's block by the
// gun's flicks); open, one round in by hand; shut with it, ready; the game's own reload after each shot stopped.
bool breech_parts();
void set_breech_parts(bool on);  // also written to the user ini
// [Reload] SemiAutoParts (off; run 6 item 9h): the Semi-Auto Shotgun's bolt racked by the off hand after loading it
// from empty (drawn back with the hand, it springs forward let go: a shell chambered); its cycling between shots stays.
bool semi_auto_parts();
void set_semi_auto_parts(bool on);  // also written to the user ini
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
enum Driver { kDrvOpen, kDrvLever, kDrvHammer, kDrvIndex, kDrvPump, kDrvBoltLift, kDrvBoltSlide, kDrvBreech, kDrvCount };
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
