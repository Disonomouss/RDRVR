#pragma once

#include <cstdint>
#include <string>

// Gestures in first person ([Gestures], the menu's Gestures tab; each off by default, for the headset round to judge):
// thrown weapons leave the hand with its velocity, a fast forward swing punches or stabs (the game's attack on RT),
// the lasso is thrown by a swing toward where the hand points (RT) and pulled by a yank back (the left stick back:
// John steps back on the rope). The hands' velocities come from the controllers in the recentred local frame (their own motion: not
// walking, turning, riding or the head-bone anchor's), taken into the world by the game camera's heading for a throw.
namespace rdrvr::gestures {

void init();  // a frame-end listener
bool throw_by_hand();  // [Gestures] Throw
void set_throw_by_hand(bool on);  // these setters also write the user ini
bool melee_by_swing();  // [Gestures] Melee
void set_melee_by_swing(bool on);
bool lasso_by_hand();  // [Gestures] Lasso
void set_lasso_by_hand(bool on);

float swing_speed();  // [Gestures] SwingSpeed, m/s
float throw_gain();   // [Gestures] ThrowGain
void set_tuning(float swing_speed, float throw_gain, bool save);  // save: also written to the user ini

bool is_thrown(int32_t weapon);  // the thrown weapons by eWeapon (fire bottle, dynamite, knives, tomahawk, ...)

// aim.cpp's projectile launch for the player's thrown weapons: the gun hand's fastest velocity in the last 0.8 s (times
// [Gestures] ThrowGain) and where the hand is now, when it moved fast enough; false: the game's own throw. Any thread.
bool throw_launch(float vel[3], float origin[3]);

std::string command(const std::string& line);  // "gestures [throw|melee|lasso on|off]": the switches, counters, speeds
// Run 7 item 2, [Gestures] GunMelee (gun_melee.h; the swing's detector lives here, beside the punch's): "gunmelee [on|off]
// [dry on|off] [reset] [scan [radius m]]" (the switches for the session; a scan lists the actors near the strike point):
// the settings, the swing's state and counters (armed, requests, scans, actors seen, the iterators made and destroyed,
// hits, dry hits, refusals, the swings suppressed by each window) and the hit side's (gun_melee::status)
std::string gun_melee_command(const std::string& line);

}  // namespace rdrvr::gestures
