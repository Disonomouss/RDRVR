#pragma once
// [Hands] OffHandItems (off; research\run28\offhand_items.md): with a gun in one hand, the other (free) hand's grip at
// the knife's sheath or the satchel draws the knife or a throwable into that hand as the mod's own drawn copy of the
// item's model (held_prop kOffhandSlot: the game's own model, loaded at run time by its fragment name), never the game's
// item in hand: the gun stays the weapon in hand and keeps aiming, shooting and reloading. The game's own knife at the
// sheath is hidden while the copy is out. Let go at the zone it came from, the item is put back. Each item's effect:
// - the knife (eWeapon 22): a fast stab or slash of its blade into a person's bone sphere is the game's hit, made as the
//   gun melee makes its hit (RDRVR_NATIVE_GUN_MELEE's scan on the plugin's tick, gun_melee::hit, FUN_140ae1760 with a
//   DamageInfo the fist melee's layout), lethal (flags 4: health), weapon 22; a slow move makes no hit (GunMeleeArm and
//   GunMeleeSpeed, as the gun melee's swing). Let go elsewhere, it goes back to its sheath;
// - a throwable (23 the fire bottle, 24 dynamite, 25 the throwing knife, 29 the tomahawk, 35-37 the Undead Nightmare
//   ones): let go during a throw (the hand faster than [Gestures] SwingSpeed in the last 200 ms), it leaves the hand
//   with the hand's velocity times [Gestures] ThrowGain: FIRE_PROJECTILE's own core, FUN_1403ba970 (anchor
//   FireProjectileCore), called on the plugin's script tick with the item's own projectile (its weapon info's first
//   fire entry, as the game's own throw takes it), the launch's velocity and place set to the hand's in aim.cpp's launch
//   hook; the inventory's count lowered by one (as reload.cpp's lose_round lowers a count). Let go slowly elsewhere, it
//   goes back to the satchel (no throw);
// - the torch (33, from the knife's zone when it is the melee slot's item or the zone's choice): a fast swing of its head
//   into a person is the game's hit as the knife's (a knock-out, flags 2, weapon 33) and sets them alight
//   (FIRE_CREATE_HANDLE, FIRE_CREATE_ON_ACTOR on the person struck, the handle released 15 s later). Its own flame and
//   light are the held weapon's idle effect, which the game starts only for the item in hand; [Hands] OffHandTorchLit
//   (off; Roo Code's, 2026-10-29: research run28 torch_second_gun.md way (d')) has dual.cpp ask the game for it each
//   weapon tick for the torch's own weapon object (its equip slot's), placed at the drawn torch (torch_pose). Without a
//   torch weapon object (the torch in no equip slot) it stays unlit. Let go anywhere, it goes back.
// The item never leaves the inventory until it is thrown: "back" only ends the drawn copy.

#include <cstdint>
#include <string>

#include "common/rdrvr_api.h"

namespace rdrvr::body {
struct BodyPoints;
}

namespace rdrvr::offhand {

enum Kind { kNone = 0, kKnife = 1, kThrown = 2, kTorch = 3 };
// the item's kind by eWeapon: 22 the knife; 23, 24, 25, 29, 35, 36, 37 the throwables (not the zombies' spit, 32); 33
// the torch
int kind_of(int weapon);

void init();     // reads [Hands] OffHandItems, logs it; the throw's tick hook (api slot 1)
bool enabled();  // relaxed
void set_enabled(bool on, bool save);  // save: the user ini too (the menu); "offhand on|off" is the session's

// holster.cpp, the frame end: controller h (John's hand jh) pressed its grip at zone `zone` (its key for the log) with a
// gun in the other hand; `weapon` is the zone's item (an off-hand kind, owned). The item is drawn in that hand; true:
// taken (the grip is the item's until it is let go).
bool take(int h, int jh, int weapon, int zone, const char* zone_key, const RdrvrActorState& st);
bool held(int h);        // controller h holds an off-hand item
int held_weapon(int h);  // its eWeapon, -1 none
int held_zone(int h);    // the zone it was taken at, -1 none
// The let-go (holster.cpp, the frame end): at_zone, back at the zone it came from; elsewhere a throwable is thrown when
// the hand moved fast enough, anything else goes back.
void release(int h, bool at_zone);
// (2026-10-31, the off-hand build step 2, R1) the game's throwable in hand let go by the grip (holster.cpp's HoldItems):
// thrown from controller h's hand, unlit, by the same launch as a copy's, when the hand moved fast enough just before
// (true: the launch queued; *speed: the hand's, -1 unknown). prime_type: its ammo type asked ahead (the count needs it)
// window_ms: the hand's motion looked at (from the let-go's start, which the hold's waits may have delayed)
bool throw_in_hand(int h, int w, float clip, uint32_t actor, float* speed, double window_ms);
void prime_type(int w);
// Each frame end (holster.cpp): the drawn item's pose at its hand (grab: each John hand's grab point, world); bp null:
// out of first person (every item put back at once). gun_h: the gun hand (a controller), gun: a gun is the item in hand
// now; an item whose other hand no longer holds a gun is put back.
void frame(const body::BodyPoints* bp, const RdrvrActorState* st, const float (*grab)[3], int gun_h, bool gun);

// gestures.cpp's strike detector: the controller, John's hand and the eWeapon of the held knife or torch; false: none
bool strike_hand(int* h, int* jh, int* weapon);
// gun_melee::hit: the weapon is an off-hand item held now (its hit is the off hand's even with [Gestures] GunMelee off),
// and that hit's damage, flags and dry run
bool strike_weapon(int weapon);
void hit_params(int weapon, float speed, float* damage, uint32_t* flags, bool* dry);
// gun_melee::hit, the plugin's tick, after the game's hit was made (code RDRVR_MELEE_HIT_OK, or a dry one): the torch's
// fire on the person struck (a made hit only)
void on_hit(uint32_t victim, int weapon, bool made);

// aim.cpp's projectile launch (the game thread): inside an off-hand throw's FUN_1403ba970, the velocity (world m/s) and
// the place it leaves from; true once per throw (that launch is the throw's). Any other launch: false.
bool launch_override(float vel[3], float org[3], int* wtype = nullptr, bool* thrown = nullptr);  // (F4's test: -1, false the core's)

// [Hands] OffHandTorchLit (off): the off-hand torch's flame and light (dual.cpp's weapon tick asks the game's idle effect
// for the torch's weapon object, placed at the drawn torch)
bool torch_lit_enabled();
void set_torch_lit(bool on, bool save);  // save: the user ini too (the menu); "offhand lit on|off" is the session's
// the drawn off-hand torch's pose as a row matrix (rows: its axes X, Y, Z, then its position, world; the game's W +0x80
// form), true while a hand holds the torch with its pose and model known, OffHandItems and OffHandTorchLit on; else
// false
bool torch_pose(float m[16]);

// a weapon's info in the game's weapon-info tree (the game's static data: any thread, every read guarded), 0 none
// (dual.cpp: the copy's shown gun's tuning)
uintptr_t info_of(int weapon);

// "offhand [on|off] [dry on|off] [lit on|off] [reset]": the session's switches (lit: OffHandTorchLit); the state of each hand, the counters; "offhand stab":
// the knife's and the torch's strike detector; "offhand throw": the throws; "offhand fire": the torch's fires;
// "offhand inject <weapon> <x> <y> <z> [speed]" (a test's): one strike scan (RDRVR_NATIVE_GUN_MELEE) along world x through
// the point at `speed` (4 m/s) with that weapon, waited for: its outcome and the core's answer (gun_melee::hit's gate)
std::string command(const std::string& line);

}  // namespace rdrvr::offhand
