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
// As many rounds as the gun takes (up to the spare ones), at once: the chest squeeze's full reload.
bool fill_clip(const RdrvrActorState& st);
// [Reload] ChestSqueeze: the gun hand gripped at the chest (Ammo) holster reloads: 0 off, 1 one round, 2 a full clip
int chest_squeeze();
void set_chest_squeeze(int mode, bool save = true);

std::string command(const std::string& line);  // "ammo [hand|auto|button|two on|off]": the switches, counters, state

}  // namespace rdrvr::reload
