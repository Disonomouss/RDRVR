#pragma once

#include <cstdint>
#include <string>

// Dual wielding ([Hands] DualWield, off; run 3 item 5, research\run3\dualwield.md 6.4, "Tier A"): a second gun in the
// free hand, the gun of another equip slot than the one in hand (a long gun with a sidearm in hand, or the hip sidearm
// with a long gun in hand). The game still holds one item in its hand. The second stays holstered for the game, but
// its weapon object W is placed in the free hand (body.cpp, at the prop placement) and fired through the gun item's
// own trigger (vtable slot 6) at the weapon tick: its own projectiles, ammo, muzzle flash and gunshot.
namespace rdrvr::dual {

void init();     // [Hands] DualWield; a frame-end listener (the second gun's trigger, its end)
bool install();  // the weapon tick hook (startup)

bool enabled();
void set_enabled(bool on);  // also written to the user ini
void set_enabled_session(bool on);

// The frame end (holster.cpp): the free controller `ctrl`, John's hand `john` on it, takes the gun of equip slot `slot`
// (gripped at the zone `zone`); end() puts it back (the game's next update hangs it on its holster again).
void begin(int ctrl, int john, int slot, const char* zone);
void end(const char* why);

// [Hands] DualWieldCopy (run 4 item 3, off): the same sidearm in the free hand. Both sidearms are equip slot 1, which
// holds one item, so the second has no weapon object of its own: the copy is the gun in hand's W drawn a second time at
// the free hand (body.cpp), fired through the gun in hand's own trigger with its shot and muzzle flash moved to the
// copy's muzzle (aim.cpp's launch, the MuzzleFxMatrix hook), and its own clip count, loaded from the spare rounds.
// begin(ctrl, john, kCopySlot, zone) takes it.
constexpr int kCopySlot = 99;
bool copy_enabled();
// 2026-10-26 (the user: "Should be able to fire as fast as you can fan and fire as fast as you can cock the hammer"): a
// single action cocked by hand (actions.cpp: RevolverControl=cock, the stick's flick or the fan) has its next shot let
// through at once. The game holds a revolver's shot until its ChamberTime (W +0x138, else its tuning's +0xd4) has passed
// since its last (W +0x130, game time) and John's fire clip has ended (the gun controller's G +0x24 < 0, or G +0x5d8 bit 4,
// the game's own early-fire path for fast guns; research run 29). At the next weapon tick (the game thread) the last
// shot's time is put back by ChamberTime and bit 4 set; the game clears it at the next clip. The player's gun in hand only
// (weapon must still be in hand), not in Dead Eye, not empty. The shot after it is the game's rate again unless cocked.
void quick_shot(int weapon);
void set_copy_enabled(bool on);  // also written to the user ini
// The game thread: the gun in hand's W while a copy is out (else 0); body.cpp notes each update's copy placement with
// T, the world map from the gun in hand onto the copy (row vectors, p' = p T: P^-1 C)
uintptr_t copy_W();
// John's hand begin() gave the second gun or the copy (0 left, 1 right; -1 none yet). Any thread, no lock.
int copy_john();
void note_copy_placed(uintptr_t W, const float T[16]);
// The game thread inside the weapon tick that fires the copy: true for its W's shot (the launch and the flash move)
bool copy_armed(uintptr_t W);
bool copy_T(float T[16]);  // the last placement's T
// [Hands] DualWieldOwnModel (run 5 item 2, off): the copy shows the other sidearm's own model when it differs (a prop made
// at the copy's place, held_prop slot 1; the copy's second draw skipped while it is drawn), its shot from that model's
// muzzle. The holsters choose the model when the copy is taken (the hip's weapon, else the first owned revolver other
// than the gun in hand); -1: the same model.
bool own_model();
void set_own_model(bool on);  // also written to the user ini
void set_copy_model(int weapon);
int copy_model();  // the eWeapon shown for the copy as a held prop (held_prop slot 1), or -1: the gun in hand's draw made twice
// aim.cpp's launch of the copy's shot: the weapon type its projectile is launched with ("dual copywt on", research for
// GOAL-older-items item 28: the shown gun's, else the game's `wtype`)
int copy_shot_weapon(int wtype);
// [Hands] CopyAsProp (on; run 7 item 1): the copy of the same model as a held prop of that model too (the gun in hand's
// skinned draw made a second time with its bones moved was never seen at the free hand)
bool copy_as_prop();
void set_copy_as_prop(bool on, bool save);  // save: also written to the user ini (the menu; "dual asprop" is the session's)
// [Hands] DualWieldSameAtItsHolster (on; round 13 item 10): with DualWieldOwnModel, the free hand at the gun in hand's own
// holster (the one it was drawn from) takes a second of that gun, its own model (another hip: the other sidearm's)
bool same_at_its_holster();
void set_same_at_its_holster(bool on);  // also written to the user ini
const char* sidearm_fragment(int weapon);  // a sidearm's model (its fragment's name), or nullptr
// A sidearm model's muzzle locator in its own axes (m): its offset from the root bone, read from the fragments (run 5)
bool sidearm_muzzle(int weapon, float out[3]);
// The frame end (holster.cpp): the copy's hand pressed to the chest takes a round from the spare rounds
bool load_copy_round(int weapon, float spare, int32_t actor);

// [Hands] DualWieldCrossReload (2026-10-27, off; the user: "When dual weilding, add option for both hands to be able to
// grab the other guns bullets and reload"): with a second gun out (DualWield, or the copy of DualWieldCopy; not a carry),
// each hand takes rounds at the chest for the gun in the OTHER hand and loads them at that gun's loading ring
// (holster.cpp): the gun hand for the second gun or the copy, that hand for the gun in hand. The gun in hand loads as a
// free hand's rounds do (reload::insert_rounds); the second gun through load_second, the copy through load_copy_rounds.
bool cross_reload();
void set_cross_reload(bool on, bool save);  // save: also written to the user ini ("dual cross on|off" is the session's)

// [Hands] DualWieldBesideTorch (2026-10-29, off; the user: the torch lit in one hand, a working gun in the other;
// research\run28\torch_second_gun.md 4): with DualWield on, the torch (eWeapon 33, an Undead Nightmare weapon) as the
// game's weapon in hand stands in for the gun in hand for DualWield's second gun only: its draw at a gun holster, its own
// reticle, its trigger, its open (DualWieldOpen) and its chest squeeze. The torch stays the game's weapon in hand, so the
// game keeps its flame and light lit (the idle effect of the item in hand). Exactly the torch: the knife, the throwables,
// the lasso and the fists never. Not the copy (DualWieldCopy fires the gun in hand's own weapon object), and no cross
// reload (the torch hand has no ammo of its own; the second gun loads by its own chest squeeze, as beside a gun with
// DualWieldCrossReload off).
// beside_torch(w): w (the eWeapon of the item in hand) is the torch, and DualWield and this option are on.
// second_beside(w): DualWield's second gun may be beside w: a gun (reload::is_gun, as before) or beside_torch(w).
constexpr int kTorchWeapon = 33;
bool beside_torch_enabled();
void set_beside_torch(bool on, bool save);  // save: also written to the user ini ("dual torch on|off" is the session's)
bool beside_torch(int weapon);
bool second_beside(int weapon);
// The frame end (holster.cpp): up to n rounds into the second gun from its own ammo type's spare rounds,
// ACTOR_ADD_WEAPON_AMMO(actor, its eWeapon, m) as the chest squeeze's (the game's FUN_140300f40 clamps to its capacity and
// the spare it takes them from). m = n limited by its room and its spare as last read (State's clip_max, spare), less
// the last load's rounds until the game thread has read the gun after the script tick that loaded them (the native's
// result's tick). Returns m (0: none queued: not ready, full, no spare).
int load_second(int n, int32_t actor);
// The frame end (holster.cpp), DualWieldCrossReload with the copy out: up to n rounds into the copy's own count from the
// gun in hand's ammo type's spare, read from the game's inventory now, ACTOR_SET_INV_AMMO(spare - m). The write is
// absolute, so it is made only when the mod's queued spare changes have run and the game's own reload is quiet
// (reload::spare_natives_done, reload_quiet): else -1, try again (a frame or two). Returns m (0: none: full, no spare).
// The copy's chest squeeze (load_copy_round), its fill at begin() and its give-back at end() follow the same rule
// (the fill and the give-back wait in the frame end, made after 3 s unsettled).
int load_copy_rounds(int n, int32_t actor);
int drop_copy_mag(int32_t actor);  // (C7) the copy's magazine out: its rounds but the chambered one back to the spare (-1: not now)

// [Hands] CarryByForegrip (on; 2026-10-26, the user: "When holding long guns, I'd like to be able to let go of the guns grip
// and hold it with just the foregrip. Freeing up the main hand to use another weapon"): a long gun held two-handed, the
// gun hand's grip pressed and let go away from the holsters, hands it to the front hand. The game's item is put away
// (holstered in its slot, 4-6), and its W is placed at the front hand as a second gun is (body.cpp), at T, the gun's
// placement in the front hand's corrected attachment bone's frame at the hand-over (row vectors: the gun = T x the bone),
// so it keeps its pose on that hand. Carried only: never fired. Works with DualWield off. The gun hand gripping near the
// gun's grip takes it back (carry_return: an instant draw; the carry is placed until the game has it in hand); the front
// hand let go ends it (the gun is already holstered for the game).
bool carry_enabled();
void set_carry_enabled(bool on, bool save);  // save: also written to the user ini (the menu; "dual carry on|off" is the session's)
// The frame end (holster.cpp): the controller `ctrl`, John's hand `john` on it, carries slot `slot`'s item `item` (its
// W), drawn at P (the gun's place as drawn at the hand-over, body::two_hand_P). Sets the game thread's view itself, so
// the gun's first holstered update makes its place on the hand (carry_T_set) and is placed there.
// hand0, rot0: John's front hand's target as it was when P was (body_points: its place, its axes as columns): until T is
// made the gun moves rigidly with that target (its move and its turn since), so it keeps its place on the hand
void begin_carry(int ctrl, int john, int slot, uintptr_t item, uintptr_t W, const float P[16], const float hand0[3], const float rot0[9]);
bool carry_start(float P[16], float hand0[3], float rot0[9]);  // the game thread, while T is to make (place_mode 4): begin_carry's
uint64_t carry_serial();                        // the carries begun (a new carry: a new number)
bool carry_T(float T[16]);  // the game thread: the carry's T once made (false: none, or not yet made)
// The game thread, the carried gun's first holstered update: its T (the gun in the hand's corrected bone's frame, as that
// update reads the bone: the review, 2026-10-26: the frame end may read it mid-update, before the game's arm IK)
void carry_T_set(const float T[16]);
// The game thread's placement of secondary_W (body.cpp): 0 none (ended: at its holster), 1 the dual-wielded second gun,
// 2 carried at T, 3 carried but T not read this time (not placed), 4 carried, its T to make (carry_start)
int place_mode(float T[16]);
void carry_return();        // the gun hand took it back (its draw queued): placed at the carry until the game has it in hand
bool carry_returning();

struct State {
    bool on = false;      // a second gun is wanted (begun, not ended)
    bool carry = false;   // [Hands] CarryByForegrip: a long gun carried by the front hand (never fired)
    int carry_weapon = -1;  // its eWeapon
    bool carry_T_ok = false;  // its T is made (its first holstered update)
    double carry_ms = 0.0;    // begin_carry's time (log::now_ms)
    bool ready = false;   // the game thread has found it (its W) this generation
    int ctrl = -1;        // the controller holding it
    int john = -1;        // John's hand holding it (0 left, 1 right)
    int slot = -1;        // its equip slot
    int weapon = -1;      // its eWeapon (once ready)
    float clip = -1.0f;   // its clip (once ready)
    // (2026-10-27, DualWieldCrossReload) once ready, not the copy: its clip's size (its tuning's AmmoPerClip, TD +0xc4,
    // the single player's, as FUN_140300f40 reads it) and its ammo type's spare rounds (the inventory entry the game takes
    // them from, wmgr +0x418 -> + type * 12; 9999 when that type is infinite); -1 not read. The copy: clip_max its
    // copy_max, spare -1 (its spare is the gun in hand's)
    float clip_max = -1.0f;
    float spare = -1.0f;
    bool copy = false;    // a copy of the gun in hand ([Hands] DualWieldCopy): clip is the copy's own count
    float copy_max = 0.0f;
};
State state();

// The game thread (body.cpp's prop placement, aim.cpp's shots): the second gun's weapon object while it is out and
// valid (else 0), and John's hand holding it.
uintptr_t secondary_W(int* john, uintptr_t* item = nullptr);
bool is_secondary_W(uintptr_t W);
// body.cpp's placement hook, when it put the second gun in the hand this update (the game thread): the weapon tick fires
// it only then, never from its holster (review 2)
void note_placed(uintptr_t W);

// [Hands] GunKick (2026-10-27): the second gun's and the copy's shots fired (each pull the game's trigger took: fire(),
// fire_copy()), and the eWeapon of the last (if `weapon`). Any thread.
uint64_t shots_fired(int* weapon);

// (2026-10-28, [Reload] PistolMagazines) The player's spare rounds of an ammo type now and the entry's cap, from the
// game's own inventory (wmgr +0x418 -> + type * 12: {float count, float max, byte infinite}; SEH-guarded reads only): 9999
// both for an endless type. False when not read (or the count over the cap). Any thread.
bool inv_entry(int type, float* count, float* cap);
std::string command(const std::string& line);  // "dual [on|off] [carry on|off] [begin <slot> [left|right]] [fire] [end]": the state, counters

}  // namespace rdrvr::dual
