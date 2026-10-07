#pragma once
// Shooting in first person (G-C: shots from the barrel).
// - The fire trigger (FUN_1403480a0) only fires while the camera on channel 0 is the gameplay camera; the camera
//   anchor's scripted camera is there in first person, so RT did nothing. [Hands] FireInFirstPerson: while the
//   anchor is active, channel 0 points at the gameplay camera for the length of that call.
// - The player's shots go along the reticle ray, built by FUN_140396b50 from the gameplay camera (the target point,
//   the actor under the reticle, the forced hit, the bullet start and direction, the arms' aim all follow it).
//   [Hands] BarrelAim: in first person with the hand on the controller, the ray is the gun as drawn: the game's gun
//   matrix (the weapon object's +0x80, barrel along -row c) taken to the IK hand by the wrist's correction, from the
//   muzzle (the weapon tune's MuzzleOffset).

#include <string>

namespace rdrvr::aim {

void init();
bool install();  // after anchors::verify()
bool barrel_aim();
// [Hands] Reticle (off by default): where the shot lands, the game's own reticle target point (T +0x3810, on the barrel's line at
// the probe's hit) while aiming with a gun in hand; on_actor: an actor under it. False when none (lowered, no gun, a
// stale or off-line point). size_deg: [Hands] ReticleSize. Any thread (SEH-guarded reads).
bool reticle_target(float pos[3], bool* on_actor, float* size_deg);
bool reticle_on();
void set_reticle_on(bool on);  // also written to the user ini
void set_barrel_aim(bool on);
// [Hands] BlockExecutions (on; round 13): the fire trigger close to an NPC fires, never John's third-person execution,
// pistol whip or butt strike (the shot request's close-target scan skipped for the player; NPCs keep theirs)
bool block_executions();
void set_block_executions(bool on);  // also written to the user ini
// The barrel of the gun in hand in its IK target's axes (the controller with the hand calibration; it is rigid there):
// from the latest aim ray, false when none in the last 0.5 s. Any thread.
bool barrel_in_target(float b[3]);
// The player's item in hand as the game places it: W +0x80, its world matrix (rows a b c, then the origin; 4 floats
// each), from which the game also places the gun's drawn prop (FUN_1402fe6d0 -> FUN_1402191c0), and whether the game
// hangs it on the left hand (item +0x98). False with nothing in hand. Any thread: SEH-guarded reads and the item's own
// getter, as the aim ray calls it. wmgr: the player's weapon manager (actor +0x70), also with nothing in hand.
bool held_item_matrix(float m[16], bool* left, uintptr_t* W = nullptr, uintptr_t* wmgr = nullptr);
// The weapon's support-hand offsets (its .weap IKOffset, def +0x2f0, and IKOffsetHold, +0x300; zero for one-handed
// weapons) and whether the player aims (the Dead Eye state's +0x0c bit 3). Any thread, SEH-guarded reads.
bool weapon_ik_offsets(uintptr_t W, float ik[3], float ik_hold[3]);
// The weapon's MuzzleOffset (its tune, weapon info +0x310) in the gun's axes, when sane (under 1 m). Any thread.
bool weapon_muzzle_offset(uintptr_t W, float mo[3]);
bool aiming();
// [Aim] PerfectAccuracy (run 3 item 3, research\run3\accuracy.md; off by default): the player's shots leave exactly
// along the barrel ray: the game's random bloom skipped, the shot direction aligned to the game's own shoot-from row,
// the muzzle-blocked flip to the animated barrel undone, and the shooter's velocity left out of the bullet. Game thread,
// in the spawn's own call (FUN_140302a50). [Aim] ShotgunPattern (on): the pellets keep the game's cone; off, every
// pellet leaves along the barrel (the count unchanged).
bool perfect_accuracy();
void set_perfect_accuracy(bool on, bool save = true);
bool shotgun_pattern();
void set_shotgun_pattern(bool on, bool save = true);
bool spawn_hooked();  // the spawn hook is installed (else both switches are unavailable)
// The punch's timing probe (run 3 item 9): the presenting thread, once a frame (gestures::frame). After a player punch's
// start, the melee controller's strike flag (M +0x554 bit 0x04, cleared at the start, set once the clip reaches the strike
// phase, with or without a target) is polled; its first frame logs "[gestures] strike ... ms after the punch's start, ...
// after the swing". swing_ms: the last swing's log::now_ms (0 if none).
void melee_probe(double swing_ms);
void set_melee_strike(float k);  // [Gestures] MeleeStrike for the session ("gestures strike <k>")
float melee_strike();
std::string command(const std::string& line);  // "aim [barrel on|off] [fire on|off]": the switches, counters, last ray

}  // namespace rdrvr::aim
