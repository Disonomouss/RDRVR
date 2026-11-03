#pragma once

#include <cstdint>
#include <string>

#include "common/rdrvr_api.h"

// The hand reload ([Reload], the menu's Reloading tab). In first person the game's own reloads (automatic after the
// last round, on drawing an empty gun and on the empty trigger; the reload button) are held back unless their switch
// is on, and rounds go in one at a time by hand: holster.cpp takes a round from the chest zone to the gun. The game's
// switch to another weapon when one runs dry is held back too (the empty gun stays in the hand and clicks).
namespace rdrvr::reload {

void init();
bool install();  // after anchors::verify()

bool hand_reload();  // [Reload] Hand
void set_hand_reload(bool on);  // these setters also write the user ini
bool automatic();    // [Reload] Automatic: the game's automatic reloads (after the last round, on the draw, empty trigger)
void set_automatic(bool on);
bool button();       // [Reload] Button: the game's reload button
void set_button(bool on);
bool two_handed();   // [Reload] TwoHanded: the front hand on a long gun aims it with the gun hand
void set_two_handed(bool on);

// The guns (pistols, revolvers, repeaters, rifles, shotguns, snipers): by eWeapon. Long guns are held with two hands.
bool is_gun(int32_t weapon);
bool is_long_gun(int32_t weapon);

// aim.cpp's fire-trigger hook, after the game's fire trigger ran: `before` is the player info's reload request byte
// (+0x248) from before the call. An empty trigger that asked for a reload is held back unless automatic reloads are
// on. Game thread.
void after_fire_trigger(uintptr_t info, uint8_t before);

// One round from the spare ammo into the gun in hand (ACTOR_ADD_WEAPON_AMMO with 1.0, the game's own count); false
// when the gun is full, has no spare rounds or Dead Eye is on. Any thread (a queued native).
bool insert_round(const RdrvrActorState& st);
// Up to n rounds at once (as many as the gun has room for and the spare ammo gives): the number put in, 0 as
// insert_round's false. [Reload] RoundsGoIn=all: a handful taken at the chest
int insert_rounds(const RdrvrActorState& st, int n);
// As many rounds as the gun takes (up to the spare ones), at once: the chest squeeze's full reload.
bool fill_clip(const RdrvrActorState& st);
// (2026-10-28, [Reload] PistolMagazines) The gun in hand's clip lowered to n, the rest given back to the spare
// (ACTOR_SET_WEAPON_AMMO, FUN_140300db0: research\gc-weapons.md 3.1 [V]): a magazine dropped. False when n is not lower,
// Dead Eye, the game's reload. Any thread (a queued native).
bool set_clip(const RdrvrActorState& st, int n);
// ([Reload] PistolSlides) One round taken from the gun in hand's clip and lost (an ejected round): the clip one lower,
// then the spare of ammo_type set back to st.spare (ACTOR_SET_INV_AMMO, as dual.cpp's). Only with the spare settled
// (the last spare native run and seen, the game's reload quiet): false otherwise, the caller tries again.
bool lose_round(const RdrvrActorState& st, int ammo_type);
// [Reload] ChestSqueeze: the gun hand gripped at the chest (Ammo) holster reloads: 0 off, 1 one round, 2 a full clip
int chest_squeeze();
void set_chest_squeeze(int mode, bool save = true);

// (2026-10-27, [Hands] DualWieldCrossReload's review: an absolute ACTOR_SET_INV_AMMO from a stale spare lost rounds) The
// mod's spare-changing natives (this file's ACTOR_ADD_WEAPON_AMMO, dual.cpp's ACTOR_SET_INV_AMMO) noted by their request
// id as they are queued (they run in the order queued: the last one run, all have).
void note_spare_native(uint64_t id);
// The last noted native has run (its result seen, else api::result_posted: a later result posted; else 60 ticks after
// the queue): the game's inventory itself is then as the mod left it. Any thread.
bool spare_natives_done();
// ... and the actor state st was read after it (st.tick past the tick it ran on: the plugin posts the state at the start
// of a tick, before the tick's requests): st.spare and st.clip show it. Any thread.
bool spare_seen(const RdrvrActorState& st);
// The game's own reload is not under way nor just was (its flag within 3 ticks of st): it takes its rounds from the
// inventory itself, which an absolute spare set from an older reading would undo. Any thread.
bool reload_quiet(const RdrvrActorState& st);
void watch_reload_flag(const RdrvrActorState& st);  // the holsters, each update: the flag seen, for reload_quiet

std::string command(const std::string& line);  // "ammo [hand|auto|button|two on|off]": the switches, counters, state

}  // namespace rdrvr::reload
