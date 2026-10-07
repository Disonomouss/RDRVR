#pragma once

#include <cstdint>
// The automatic holster (headset round 6b: "guns get put away automatically after a moment if they are not used").
// The player's gun-ready action node holds a transition with no conditions that starts 3 s into the node and goes to
// the holster node. With [Hands] KeepGunDrawn that scheduled transition is cancelled while a gun is drawn, so the gun
// stays out until it is put away by hand (the body holsters) or by the game's own requests.

#include <string>

#include "core/body.h"

namespace rdrvr::holster {

void init();
bool install();                 // after anchors::verify()
bool keeps_drawn();
void set_keeps_drawn(bool on);
// The body holsters ([Holsters] Enabled; zones RightHip, Back, Belt, Knife: <Key>Offset "right up forward" m,
// <Key>Radius m), the menu's Holsters tab.
bool holsters_enabled();
void set_holsters_enabled(bool on);  // also written to the user ini
bool grip_consumed(int h);           // that hand's grip is a holster's (draw, put away, a round, the foregrip): not LB/RB
// The two-handed aim's weight (0..1): the front hand holds the long gun's foregrip ([Reload] TwoHanded). body.cpp turns
// the gun hand so the barrel points at the front hand by this much. Any thread.
float two_hand_blend();
// A grip pressed now would be a holster's: the hand is in a zone, or it is the front hand at a long gun's foregrip
// (as of the last frame end). Any thread.
bool grip_wanted(int h);
// The game's gun is in its aim pose (the gun controller's +0x5d6 bit 0x40, set by the aim-pose op): only then does a
// fire request shoot (FUN_140d169a0 drops it otherwise). As of the last frame end; any thread.
bool aim_pose();
// The gun hand raised: a gun in hand, the barrel within [Hands] RaisedPitchMin..Max degrees of level and the hand
// RaisedReach ahead of the chest, not at a holster nor loading (debounced; the last frame end). Any thread.
bool gun_raised();
// The gun ready to fire now (the gun controller's states and timers FUN_140d16400 checks; the last frame end).
bool fire_ready();
// The player's gun controller's fire clip (G +0x24, read at each frame end): its phase from 0, below 0 when none plays.
float fire_clip_phase();
// [Holsters] ShowGuns (off; run 6 item 6a): each gun zone's gun shown at it as a game prop (a hip gun barrel down, a back
// gun diagonal), hidden while that gun is in a hand; destroyed when off or out of first person.
bool show_guns();
void set_show_guns(bool on, bool save = true);
// [Holsters] ShowBackGuns (on; round 13 item 14): off, ShowGuns shows the hips' guns only (not the back's, the left shoulder's)
bool show_back_guns();
void set_show_back_guns(bool on, bool save = true);
// The gun hand inside a holster zone (the last frame end): no aim stance through a put-away.
bool gun_at_zone();
// The rings ([Holsters] ShowZones, the menu's "Show the holsters"; zone_rings.h draws them): each zone, the foregrip
// while a long gun is in hand (two-handed on), the loading point while a round is held, and each hand's test point,
// in the game's world, with the frame camera the points were placed with. Published at each frame end on the
// presenting thread, before the XR frame end (holster::init registers its listener before xr::init).
enum MarkerKind : uint8_t { kRing = 0, kDot = 1 };
enum MarkerState : uint8_t { kIdle = 0, kHandIn = 1, kHeld = 2, kGunIdle = 3 };
constexpr int kMaxMarkers = 14;
struct Marker {
    float pos[3];
    float radius;
    MarkerKind kind;
    MarkerState state;
    int8_t id;  // a zone, or kZones + 0 the foregrip, + 1 the loading point, + 2 + controller a hand, + 4 the action hint
};
struct Markers {
    float cam[16];
    int n;
    Marker m[kMaxMarkers];
};
// The foregrip ([Reload] ForegripOffset, ForegripRadius, TwoHandedSnap; the menu's Reloading tab): the point is the
// game's own front-hand grip on the drawn long gun (body.cpp learns it from the game's animated left hand) moved by
// the offset (right, up, forward in the gun's axes, metres); the front hand's point within the radius of it engages
// the two-handed hold, and while held John's front hand snaps onto the gun there (the game's hand pose and fingers).
void foregrip(float off[3], float* radius);
void set_foregrip(const float off[3], float radius, bool save);
// [Hands] InteractOffset (round 13): the hands' interaction spot, what grabs at the holsters, the chest, the rings and
// an action's parts: the wrist target moved by right, up, forward (m) in the hand's axes, the left hand's right mirrored
// (0 0 0: the wrist, as before). Any thread.
// Round 13 item 8: each gun's own adjustments (the menu's "Gun in hand" tab): kAdjForegrip, where John's front hand sits
// (ForegripOffset's); kAdjForeRing, where the front hand takes hold and its size (ForegripZoneOffset, -Radius's);
// kAdjLoadRing, where a round goes in and its size (LoadPointOffset, -Radius's). Offsets right, up, forward (m) in the
// gun's axes. gun_adjust: the values in effect for gun w, its own if set (own), else every gun's. Any thread.
enum GunAdj { kAdjForegrip, kAdjForeRing, kAdjLoadRing };
void gun_adjust(int w, int what, float off[3], float* radius, bool* own = nullptr);
void set_gun_adjust(int w, int what, const float off[3], float radius, bool save);  // save: the user ini's [Weapon.<token>]
void clear_gun_adjust(int w, int what);  // back to every gun's (its entries cleared)
int gun_in_hand();                       // the gun the holsters' last update saw in hand (0-20; -1 none)
void interact_offset(float off[3]);
void set_interact_offset(const float off[3], bool save);  // save: also written to the user ini
// John's hand j's interaction spot from the body's points (world)
void grab_point(const body::BodyPoints& bp, int j, float out[3]);  // save: also written to the user ini
// Where a round goes into the gun in hand ([Reload] LoadPointOffset, LoadPointRadius, InsertOnTouch; the Reloading tab):
// 0.10 m out along the barrel from the gun hand's wrist, moved by the offset (right, up, forward in the gun's axes); a
// round held within the radius goes in (let go: within it + 0.04 m); on touch, the drawn round's own position counts.
void load_point(float off[3], float* radius, bool* touch);
// The foregrip ring apart from the grip ([Reload] ForegripZoneOffset, ForegripZoneRadius; run 6): where the front hand
// engages is the grip point moved again (right, up, forward in the gun's axes), within the zone's radius; the grip point
// (ForegripOffset) stays where John's hand sits. ForegripRadius is the zone radius's default when its key is absent.
void foregrip_zone(float off[3], float* radius);
void set_foregrip_zone(const float off[3], float radius, bool save);  // save: also written to the user ini
void set_load_point(const float off[3], float radius, bool touch, bool save);  // save: also written to the user ini
bool foregrip_snap();
void set_foregrip_snap(bool on, bool save);
// The weapon in each holster ([Holsters] WeaponChoice, <Key>Weapon; the menu's Weapons tab): a holster with a chosen
// weapon the player owns draws that one (ACTOR_PUT_WEAPON_IN_HAND takes any owned weapon; the game then keeps it in
// that slot), else its slots in order as before. AnyWeapon lists every owned weapon for every holster.
constexpr int kWeapons = 38;
const char* weapon_label(int w);  // "Cattleman Revolver"; "" out of range
bool weapon_choice();
void set_weapon_choice(bool on);  // also written to the user ini
bool any_weapon();
void set_any_weapon(bool on);
int zone_weapon(int z);           // -1: automatic
void set_zone_weapon(int z, int w, bool save);
bool zone_takes_weapons(int z);   // not the chest
bool zone_fits(int z, int equip_slot);
struct Arsenal {
    bool valid;
    uint64_t owned;
    int8_t equip_slot[kWeapons];
};
bool arsenal(Arsenal* out);       // the posted owned weapons (false before the first read). Any thread.
bool show_zones();
void set_show_zones(bool on);  // also written to the user ini
void set_show_zones_session(bool on);
bool markers(Markers* out);    // false when off, out of first person, or older than 100 ms. Any thread.
// [Holsters] UnarmedAfterHolster: a put-away at a holster also selects unarmed (the fists), as the weapon wheel's
// Unarmed does (SET_PLAYER_MELEE_MODE_SELECTED): LT is then the fist-fight stance, not a draw of the last gun.
bool unarmed_after_holster();
void set_unarmed_after_holster(bool on);  // also written to the user ini
constexpr int kZones = 8;  // RightHip, Back, Belt, Knife, Ammo (the chest: a round to reload, reload.h), LeftHip, LeftShoulder, LowerBack
bool zone_enabled(int z);
void set_zone_enabled(int z, bool on, bool save);  // [Holsters] <Key>Enabled
int zone_count();
const char* zone_label(int i);
void zone(int i, float off[3], float* radius);
void set_zone(int i, const float off[3], float radius, bool save);  // save: also written to the user ini
// "holster [on|off|trace [off]|dump <file>|zones|instant on|off]": the switches, the zones and the counters
std::string command(const std::string& line);

}  // namespace rdrvr::holster
