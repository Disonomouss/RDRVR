#pragma once
// The controllers mapped onto the game's pad (ROADMAP step 3, before the G-C gestures take buttons over). The default
// is the game's own scheme with the buttons that share a name: right A / B -> A / B, left X / Y -> X / Y, triggers ->
// LT / RT (analog), grips -> LB / RB, sticks -> sticks, stick clicks -> L3 / R3. Each VR button can be remapped to
// any pad button or trigger ([Controls] RightA=A ..., the menu's Controls tab), and [Controls] LeftHanded=1 swaps the
// two controllers' roles (the left trigger fires, the right stick moves). The menu button (left on Touch) is also a
// modifier, because Touch has no D-pad, Back or second menu button:
//   tap                -> Start (pause)
//   hold MenuHoldMs    -> the RDRVR menu (open / close)
//   held + right stick -> the D-pad (the stick's dominant direction; the stick itself is not passed on)
//   held + A           -> Back (the map)
// Controllers without a menu button (Index) use the chord: both B buttons together act as the menu button
// ([Controls] MenuChord=1). Both stick clicks held for a second recentre, as on the gamepad. While the RDRVR menu is
// open nothing goes to the game (the right trigger clicks in the menu). A real gamepad keeps working alongside.

#include <cstddef>

#include "core/xinput.h"

namespace rdrvr::controls {

// A press made by the mod (gestures.cpp: a punch, the lasso's throw and yank) for `ms`: buttons ORed in, RT at
// least `rt`, the left stick's y set to `ly` when nonzero; LT at least `lt` from now, RT only from `rt_after_ms` on (a
// punch with the fists needs the game's fighting stance, LT, before RT), the whole press lasting rt_after_ms + ms.
// Any thread.
void inject(uint16_t buttons, uint8_t rt, int16_t ly, int ms, uint8_t lt = 0, int rt_after_ms = 0);

void init();
// Presenting thread, once per frame end: true and the pad when a controller is in use and the RDRVR menu is closed.
bool pad(xinput::PadState* out);
bool take_menu_toggle();  // the menu button was held MenuHoldMs (once per hold)
bool take_recentre();     // both stick clicks were held a second (once per hold)
void status_text(char* out, size_t len);

// The remap (menu Controls tab). Sources and targets by index; names are the ini values.
constexpr int kSources = 10;
constexpr int kTargets = 17;
const char* source_label(int s);  // "Right A", ...
const char* source_key(int s);    // the [Controls] key: "RightA", ...
const char* target_name(int t);   // "none", "A", "B", ... "DpadRight"
int mapping(int s);
void set_mapping(int s, int t);   // also written to the user ini
void reset_mapping();             // the defaults, written to the user ini
bool left_handed();
void set_left_handed(bool on);    // also written to the user ini
int gun_hand();                   // the controller holding the gun: the one that drew it ([Hands] DrawToGrabbingHand), else the layout's
int layout_gun_hand();            // 1 right (default), 0 left when left-handed
// [Hands] DrawToGrabbingHand: a holster draw puts the gun in the hand that grabbed it (the trigger roles follow: that
// controller's trigger fires). Active with each arm on its own controller (right-handed, or left-handed with
// GunInGunHand). ctrl: the controller; -1 back to the layout's hand.
void set_draw_hand(int ctrl, int weapon = -1, bool now = true);  // not now: committed once `weapon` is in hand (commit_draw_hand)
void commit_draw_hand(int weapon_in_hand, bool in_hand);  // the holsters' frame: a pending hand, once its weapon is in hand
bool draw_any_active();
bool draw_to_grabbing_hand();
void set_draw_to_grabbing_hand(bool on, bool save = true);
// [Horse] StickClickBrake: on a horse or a wagon the left stick click is the game's brake (its RB)
bool click_brake();
void set_click_brake(bool on);    // also written to the user ini
// [Hands] TriggerAims: with a gun in hand in first person the right trigger alone fires (LT is held for it)
bool trigger_aims();
void set_trigger_aims(bool on);   // also written to the user ini
void set_trigger_aims_session(bool on);  // the test channel: not saved
// [Hands] AimWhenRaised: LT held while the gun hand is raised (the aim stance up, every pull fires at once)
bool aim_when_raised();
void set_aim_when_raised(bool on, bool save = true);
bool sprint_drops_aim();  // [Hands] SprintDropsAim
void set_sprint_drops_aim(bool on, bool save = true);
void set_click_brake_session(bool on);

}  // namespace rdrvr::controls
