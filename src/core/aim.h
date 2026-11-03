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

#include <cstdint>
#include <string>

namespace rdrvr::aim {
int player_melee_state();  // the player's melee controller's state (1 a punch, 2 blocking; -1 none or unread)
// 2026-10-30 (item 12, [Gestures] LassoThrow=lock; a test's probe, "gestures lassoprobe on"): the lasso's state, read only
// (SEH-guarded, fixed offsets; -1 or 0 unread). aim_state: the player info's aim-trigger state (+0x244: 1 pressed, 2
// held, 3 easing off; research/run17/lasso.md 1.2); user_state: the player's lasso component L ([[ped +0xaa8] +0x40]
// +0x18; 5 holding someone); lasso_state: the lasso's own (L +0x30 -> +0x3d0, FUN_14035d900: 2 coiled, 3 in flight, 8-13
// on someone); target: the lasso's +0xf0 (copied from the candidate +0xf8 for the player when +0xf0 is not live; a
// previous throw's live value is kept: research/run30/lasso_lock.md 3), candidate (+0xf8, from the item's +0x210); from
// the targeting T (actor +0xb8, FUN_14039bec0): reticle (+0x3900, GET_ACTOR_UNDER_RETICLE's 0),
// lock (+0x3914, its 1: the reticle ray's hit, also beyond the reticle's range, or the magnet's), soft (+0x3920, the soft
// lock's pick, an actor-pool handle: never equal to a script handle). The others as the script's (GET_LASSO_TARGET's).
// Any thread.
struct LassoProbe {
    bool ok = false;
    int32_t aim_state = -1, user_state = -1, lasso_state = -1;
    uint32_t target = 0, candidate = 0, reticle = 0, lock = 0, soft = 0;
};
LassoProbe lasso_probe();

void init();
bool install();  // after anchors::verify()
// (2026-10-29, [Hands] OffHandItems) the projectile launch hook is in place: an off-hand throw is made only then (its
// velocity and place are set there; without it the core's 650 m/s would leave the hand)
bool launch_hooked();
bool barrel_aim();
// [Hands] Reticle (off by default): where the shot lands, the game's own reticle target point (T +0x3810, on the barrel's line at
// the probe's hit) while aiming with a gun in hand; on_actor: an actor under it. False when none (lowered, no gun, a
// stale or off-line point). size_deg: [Hands] ReticleSize. Any thread (SEH-guarded reads).
// which: 0 the gun in hand's, 1 the second gun's (dual wield; never red); advance: the frame's call (the XR thread), which
// moves the smoothing on (any other caller reads the last place)
bool reticle_target(float pos[3], bool* on_actor, float* size_deg, int which = 0, bool advance = false);
bool reticle_on();
void set_reticle_on(bool on);  // also written to the user ini
// [Hands] ReticleStyle: ring (a ring and a centre dot, the default) or dot (a dot only)
bool reticle_dot();
void set_reticle_dot(bool dot);  // also written to the user ini
void set_barrel_aim(bool on);
// [Hands] BlockExecutions (on; round 13): the fire trigger close to an NPC fires, never John's third-person execution,
// pistol whip or butt strike (the shot request's close-target scan skipped for the player; NPCs keep theirs)
bool block_executions();
void set_block_executions(bool on);  // also written to the user ini
// [Hands] ShootPastArmBlock (off; run 7 item 1e): the game's arm block (its shoulder-line sweep) lifted for the player
// while the gun in hand is aimed along its barrel; the game's other reasons kept
bool shoot_past_arm_block();
void set_shoot_past_arm_block(bool on, bool save);  // save: also written to the user ini (the menu; "aim armblock" is the session's)
// The barrel of the gun in hand in its IK target's axes (the controller with the hand calibration; it is rigid there):
// from the latest aim ray, false when none in the last 0.5 s. Any thread.
bool barrel_in_target(float b[3]);
// (run 29 item 3, the scope spike; test only) the latest aim ray in the world: the muzzle and the barrel's direction,
// false when none in the last 0.5 s. Any thread (torn reads possible: a test's pictures only).
bool last_ray(float muzzle[3], float dir[3]);
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
// Channel 0's camera is the game's own gameplay camera ([[CamManager +8] +0x18] +0x10 == [CamManager +0x28]): no
// scripted camera (the anchor's, a shop's, a mission's) is current. False when not read.
bool gameplay_camera_current();
// While another hook puts the gameplay camera on channel 0 for one game call (deadeye.cpp's reticle UI update; after
// optimumbox's fork, v0.8.11.9, commit ac942427, MIT): gameplay_camera_current() stays false meanwhile, so the plugin's
// camera job does not take the swap for the game's own. Paired: true before the call, false after.
void camera_swap_flag(bool on);
// [Gestures] ThrowByGrip: hold the grip to hold a throwable, the trigger to start it (the light, the knife by its tip),
// the grip let go to throw it with the swing. grip_throw_wanted(ctrl): that controller's grip is the throwable's (the
// gun hand with a throwable in it), not the game's button
bool grip_throw_wanted(int ctrl);
bool grip_throw();
// (2026-10-31, the off-hand build step 2) the game's own throw under way: armed by the trigger (ThrowByGrip), held, its
// LT tail, or a player throw clip started within the last 1.5 s (any options). holster.cpp's let-go of a held throwable
// leaves it to the game then
bool grip_throw_busy();
// The player's own shots (the gun in hand's, one a shot however many pellets), counted in the spawn hook as the game
// makes them (actions.cpp: a fanned shot taken). Any thread.
uint64_t player_shots();
// The player's gun controller G ([[actor +0x38 (ped)] +0xaa8] +8; +0x24 John's fire clip phase), 0 when none. Game thread.
uintptr_t player_gun_ctl();
bool mounted_gun_fired(double ms);  // the player shot a mounted gun (the Gatling, the Browning, the cannon) within ms
bool mounted_shot(float d[3], double* ms);  // a mounted gun's last round: its direction (world) and when
void on_exit();  // the process exit: others' shots since the last log line
// the pad's triggers (the pad's thread): the trigger with the grip holding a throwable arms it (the game's LT, then its
// RT after a lead), LT kept through the held throw and its tail
void grip_throw_input(float* lt, float* rt);
std::string throw_flight();
double game_throw_launch_ms();  // (GOAL-offhand-changes C1) the player's last thrown item launched by the game's own throw (log::now_ms)  // (GOAL-offhand-changes C13) the last thrown projectile's flight ("offhand": its line)
void throw_hold_tick();  // the game thread, once a frame (the visibility build): the held throw pinned, or let go
// the throwing knife's readied throw held by its tip: its local turn axis (0-2) and pivot (the blade's middle)
bool knife_tip(int* axis, float pivot[3]);
std::string tip_command(const std::string& w, const std::string& v);  // gestures tip/tipaxis/tippivot; the readback
void set_grip_throw(bool on, bool save = true);
// [Aim] PerfectAccuracy (run 3 item 3, research\run3\accuracy.md; on by default since run 8): the player's shots leave exactly
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
