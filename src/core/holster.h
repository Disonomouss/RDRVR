#pragma once

#include <cstdint>
// The automatic holster (headset round 6b: "guns get put away automatically after a moment if they are not used").
// The player's gun-ready action node holds a transition with no conditions that starts 3 s into the node and goes to
// the holster node. With [Hands] KeepGunDrawn that scheduled transition is cancelled while a gun is drawn, so the gun
// stays out until it is put away by hand (the body holsters) or by the game's own requests.

#include <string>

#include "core/body.h"

namespace rdrvr::holster {
// [Hands] KnifeSwap (2026-11-01, GOAL-offhand-changes C2; on): the off hand's knife with a gun in hand is the game's weapon,
// the gun the second gun (as TorchSwap's torch)
bool knife_swap();
void set_knife_swap(bool on);  // also written to the user ini

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
// [Hands] CarryByForegrip, [Reload] CarriedBolt: the carried gun's take-back point (world: where the gun hand let go of
// its grip, on the carried gun as last placed, else on the carrying hand's target), as of the last frame end (within 100
// ms); false without a carry, or while taken back. ctl: the same point in the carrying controller's frame (the
// fallback's), by_gun: out is on the placed gun (a test's print)
bool carry_take_point(float out[3], float ctl[3] = nullptr, bool* by_gun = nullptr);
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
// [Holsters] ShowModels: a holster model (the game's prop [Holsters] Model) drawn at each enabled hip holster
bool show_models();
// [Holsters] OwnBeltProps (GOAL-cattleman-belts item 2; off by default): on an outfit without its own knife sheath,
// satchel or lasso, the game's props (the bowie knife, the satchel, the lasso's coil) drawn where the custom belt has them
bool belt_props();
void set_belt_props(bool on, bool save);  // save: also written to the user ini
// [Holsters] OwnBeltPropsDefault (GOAL-after-0.8.13.2 item 7; off by default): with OwnBeltProps, the satchel and the sheath
// from the default outfit's own model (player_default as a prop, only those pieces of it drawn)
bool belt_props_default();
void set_belt_props_default(bool on, bool save);  // save: also written to the user ini
// [Holsters] OwnBeltThrowable (round 117, the user's idea; off by default): with OwnBeltProps, at the satchel's place the
// equipped throwable's own model (the thrown slot's item) instead of the game's other satchel; none equipped, none shown
bool belt_throwable();
// (round 117) the table games' gate, as polled (holster_frame): bit 1 a minigame runs (poker), bit 2 the game holds the
// player's control
int game_gate();
// (round 128) by game_gate(): a table game (the minigame's flag: poker; or the game holding the player's control with John in a
// gringo for 1.5 s: liar's dice, horseshoes, blackjack, five finger fillet, arm wrestling), and the view at John's seated head
// (a table game, or John in a gringo on a train for a second: its seat)
bool table_game();
bool seated_view();
// (round 130) which minigame's script runs, by the game's own list of running scripts (walked on a script tick four times
// a second): bit 1 blackjack, 2 liar's dice, 4 five finger fillet, 8 horseshoes, 16 arm wrestling, 32 poker; 0 none, or the
// list not found on this build (minigame_known() false)
int minigame();
bool minigame_known();
bool minigame_fresh();  // the list walked within 2 s (script ticks running)
void set_belt_throwable(bool on, bool save);  // save: also written to the user ini
void set_show_models(bool on, bool save = true);
bool show_guns();
void set_show_guns(bool on, bool save = true);
// [Holsters] ShowBackGuns (on; round 13 item 14): off, ShowGuns shows the hips' guns only (not the back's, the left shoulder's)
// [Holsters] Anchor (run 7 item 1d): 0 body (the drawn body's holster bones, as before), 1 headset (the zones moved
// with the neck's offset since recentre: they stay with the player when the drawn body does not). Riding, driving
// and cover keep the body's anchor.
int anchor();
void set_anchor(int a, bool save = true);
// [Holsters] TurnWithHead (the headset anchor only): the holsters turned with the headset's yaw since recentre, once it is
// more than TurnDeadZone degrees (0-90) from where they point; off: they face the body's heading
// [Holsters] LeanSteady (the headset anchor): the holsters stay when you lean (the neck's move read as a torso pivoting
// at the hips, TorsoLength); TurnByHands (with TurnWithHead): both hands ahead pull the turn halfway to their direction
bool lean_steady();
void set_lean_steady(bool on, bool save = true);
float torso_length();  // [Holsters] TorsoLength (0.3-0.9 m): the lean model's (the physical crouch reads it too, 2026-10-27)
bool turn_by_hands();
void set_turn_by_hands(bool on, bool save = true);
bool turn_with_head();
void set_turn_with_head(bool on, bool save = true);
float turn_dead_zone();
void set_turn_dead_zone(float deg, bool save = true);
bool show_back_guns();
void set_show_back_guns(bool on, bool save = true);
// The gun hand inside a holster zone (the last frame end): no aim stance through a put-away.
bool gun_at_zone();
// The rings ([Holsters] ShowZones, the menu's "Show the holsters"; zone_rings.h draws them): each zone, the foregrip
// while a long gun is in hand (two-handed on), the loading point while a round is held, and each hand's test point,
// in the game's world, with the frame camera the points were placed with. Published at each frame end on the
// presenting thread, before the XR frame end (holster::init registers its listener before xr::init).
enum MarkerKind : uint8_t { kRing = 0, kDot = 1 };
enum MarkerState : uint8_t { kIdle = 0, kHandIn = 1, kHeld = 2, kGunIdle = 3, kNear = 4 };  // kNear: a hand near (ZonesNear)
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
// (2026-11-01, GOAL-offhand-changes C35, C38) kAdjMag: a magazine pistol's well (MagazineWellOffset's; none of every gun's: 0 0 0),
// and every weapon (the Blunderbuss, the Explosive Rifle) its own, each hand's. (C36) kAdjPart: the action's spot (the bolt's,
// the breech's, the slide's, the fanned hammer's, the muzzle's ram), PartOffset's; none of every gun's (0 0 0)
enum GunAdj { kAdjForegrip, kAdjForeRing, kAdjLoadRing, kAdjMag, kAdjPart, kAdjCount };
// (C8) hand: John's hand holding the gun (0 left: its ...Left set, else the right's; 1 or -1 the right's); exact: the
// left's alone (the menu's: none of its own reads as every gun's)
void gun_adjust(int w, int what, float off[3], float* radius, bool* own = nullptr, int hand = 1, bool exact = false);
// save: the user ini's [Weapon.<token>]; keep_other (C8b, a person's change: the menu's, the tests'): the right hand's set while
// the left has none of its own, the left keeps where it was as its own (never on the ini's load)
void set_gun_adjust(int w, int what, const float off[3], float radius, bool save, int hand = 1, bool keep_other = false);
// back to every gun's (the left's: to the right's) (its entries cleared); keep_other: as set_gun_adjust's (the right's clear)
void clear_gun_adjust(int w, int what, int hand = 1, bool keep_other = false);
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
// [Reload] PhysicalStock (run 14; off): the front hand's trigger also holds the foregrip, the front hand does not turn the
// gun, and every long gun is turned in the gun hand by [Reload] StockTurn (degrees, a rotation vector), learned by
// stock_calibrate: in delay_ms, with a long gun held on both controllers, its barrel set along the line from the gun hand's
// controller to the front hand's, its top up (body.cpp takes the sample: stock_calibrate_due, once).
bool physical_stock();
void set_physical_stock(bool on);  // also written to the user ini
void stock_calibrate(double delay_ms);
double stock_calibrate_in();  // ms until the sample (-1: none asked)
bool stock_calibrate_due();     // once, at its time (a request a second past it is dropped)
void stock_calibrate_expire();  // a request past its time and a second dropped (called each body frame)
void stock_turn(float deg[3]);
// [Reload] TriggerTakesRound (run 14; off): the off hand's trigger pressed at the chest's ammo zone takes a round as the grip
// does, held while the trigger is (the trigger still aims)
bool trigger_takes_round();
void set_trigger_takes_round(bool on);  // also written to the user ini
// [Reload] RoundsPerGrab (2026-10-26; 1): the rounds a hand takes at the chest at once (up to the gun's room and the
// spare ammo, 1-8). [Reload] RoundsGoIn: "one" (one at a time, a round each RoundInsertMs while they are held at the
// gun's loading ring) or "all" (all of them as they reach it). Let go at the gun, the rest go in at once
int rounds_per_grab();
void set_rounds_per_grab(int n);  // also written to the user ini
bool rounds_all_at_once();
void set_rounds_all_at_once(bool on);  // also written to the user ini
// [Reload] RoundSource (2026-10-28; chest): 0 chest, the rounds taken at the chest's ammo zone only; also, by the hand
// that may take them (the free hand; with DualWieldCrossReload each dual-wielding hand, for the other gun), its grip
// pressed out of the holsters' zones where no part of the gun wants it (the action's parts, the foregrip, the carried
// gun's grip): 1 gun, within 15 cm of the target gun's loading ring (inside it too: they go straight in); 2 palm, turned
// palm up, 5 cm or more outside that ring. RoundsPerGrab of them appear in it, loaded at the loading point as the
// chest's are. The test channel's "holster source chest|gun|palm" is the session's (save false)
int round_source();
void set_round_source(int src, bool save = true);
bool trigger_for_round(int h);          // TriggerTakesRound on and controller h's trigger is the round's (at the ammo zone, or
                                        // holding the round it took): not the lever lock's
// [Hands] DualWieldCrossReload (2026-10-27): controller h holds a gun and rounds it took for the gun in the other hand (or
// its trigger, held then, is not yet let go): its own gun does not fire (controls.cpp holds RT back for the gun hand,
// dual.cpp the second gun's pull), so no shot goes off mid-reload. Any thread.
bool cross_holds_fire(int h);
void set_stock_turn(const float deg[3]);  // also written to the user ini
void set_foregrip_snap(bool on, bool save);
// The weapon in each holster ([Holsters] WeaponChoice, <Key>Weapon; the menu's Weapons tab): a holster with a chosen
// weapon the player owns draws that one (ACTOR_PUT_WEAPON_IN_HAND takes any owned weapon; the game then keeps it in
// that slot), else its slots in order as before. AnyWeapon lists every owned weapon for every holster.
constexpr int kWeapons = 38;
const char* weapon_label(int w);  // "Cattleman Revolver"; "" out of range
const char* weapon_token(int w);  // "Cattleman" (the ini's token); "" out of range
// (round 141) on a mounted gun driven by the hands: this controller's grip is open (its arm follows the controller;
// held, the arm is the game's, on the gun's handle or its crank). h: the controller (0 left, 1 right)
bool turret_hand_free(int h);
bool turret_ready();  // [Hands] MountedGunFirstPerson: the turret drive read the mounted gun within 0.5 s (pose keeps first person only then)
bool weapon_choice();
void set_weapon_choice(bool on);  // also written to the user ini
bool any_weapon();
void set_any_weapon(bool on);
// run 10 item 10 ([Holsters] AnyWeapon: "anything in any holster"): a zone's choice may also be the rounds (the reload's:
// the free hand takes a round there, the gun hand squeezed there reloads), kRounds; the chest's automatic is the rounds
constexpr int kRounds = 38;  // kWeapons (an eWeapon past the last)
int zone_weapon(int z);           // -1: automatic
void set_zone_weapon(int z, int w, bool save);
// (round 143) [Holsters] WheelAtHolster (off): a trigger held at a holster with that hand empty opens the weapon wheel;
// the pick becomes that holster's weapon and is put away there (wheel.cpp)
bool wheel_at_holster();
void set_wheel_at_holster(bool on, bool save);
int wheel_zone(int c);  // the holster controller c's empty hand is at while the option is on (-1: none; any thread)
bool zone_takes_weapons(int z);   // not the chest (its rounds)
bool zone_any(int z);             // a gun holster (the hips, the backs): AnyWeapon's (2026-10-13)
int zone_piece(int z);            // its belt piece (body::Piece index; body::kPieces the mirrored left holster), -1 none
bool zone_fits(int z, int equip_slot);
bool zone_fits_weapon(int z, int weapon, int equip_slot);  // zone_fits, and the Sawed-off at the sidearm holsters
struct Arsenal {
    bool valid;
    uint64_t owned;
    int8_t equip_slot[kWeapons];
};
bool arsenal(Arsenal* out);       // the posted owned weapons (false before the first read). Any thread.
bool show_zones();
void set_show_zones(bool on);  // also written to the user ini
// [Holsters] ZonesNear (with ShowZones): a holster's ring only while a hand is within ZonesNearDistance (m) of its edge,
// faint until the hand is in it
// Placing the holsters by hand (the menu; the session only): every ring shown, no draws or put-aways; a grip in a ring
// moves that holster with the hand, letting go saves its offset
bool placing();
void set_placing(bool on);
// [Holsters] RingsFollowHolsters (2026-10-13): a moved belt piece's ring at the same spot on it (gripping the ring moves
// the piece), or each moved on its own
bool rings_follow();
// a piece (body::kPieces the mirrored left holster) or a ring set as the menu's arrows do: with RingsFollowHolsters the
// other moved the same way
void set_piece_linked(int pc, const body::Piece& p, bool save);
void set_zone_linked(int z, const float off[3], float radius, bool save);
void reset_rings();  // every ring's place and size back to the shipped defaults (saved)
// "Customize John's belt" turned on or off: with RingsFollowHolsters the linked rings moved with the pieces
void set_own_pieces_linked(bool on, bool save);
int link_piece(int z);  // the piece a ring is linked to (-1 none, or RingsFollowHolsters off)
void link_piece_moved(int pc, const float dw[3], int except);  // its rings shifted by a world move (but `except`)
void link_zone_moved(int z, const float dw[3]);  // a ring moved: its piece and the piece's other rings the same way
void link_save(int pc, int except);  // the piece and its rings saved
void set_rings_follow(bool on, bool save);
// Run 12 item 3: placing John's belt's pieces by hand (the session; turns [Holsters] OwnBelt on): a grip within 10 cm of
// a piece takes it, it follows the hand's move and turn, letting go saves it. "holster pieces [place on|off]"
bool placing_pieces();
void set_placing_pieces(bool on);
std::string pieces_status();
bool zones_near();
void set_zones_near(bool on, bool save = true);
// [Holsters] SteadyZones: a hand stays in its holster until 3 cm past its radius; another takes it when 3 cm nearer
bool steady_zones();
void set_steady_zones(bool on, bool save = true);
void set_show_zones_session(bool on);
// run 8 item 5b: the hand dots and the weapon points (the foregrip ring, the load point, the action hints), each its
// own switch ([Holsters] ShowHandDots, ShowWeaponPoints; absent: ShowZones' value). save: the user ini too
bool show_hand_dots();
bool show_weapon_points();
void set_show_hand_dots(bool on, bool save);
void set_show_weapon_points(bool on, bool save);
bool markers(Markers* out);    // false when off, out of first person, or older than 100 ms. Any thread.
// [Holsters] InteractionHighlights (2026-10-29, GOAL-weapons-handling.md item 5, research\run28\highlights.md): 0 rings
// (today's: the rings by their own switches, no highlight), 1 highlights (a soft glow over each place a ring marks,
// instead of its ring: on the gun's part, an area glow at a holster; only while using it would do something now and a
// hand is near, brighter at reach; never over the sights), 2 both (the rings as their switches say, and the highlights).
// The hand dots (ShowHandDots) and placing the holsters by hand keep their rings in every mode. save: the user ini too.
// (run 29 item 2, research\run29\glow.md; the user, 2026-10-29: "I want the weapon parts to look like they have a glowing
// texture on them") 3 parts: as highlights, but each place on a gun (the foregrip, the pump, the loading points, the magazine
// well, the action's part, the lever) glows on the gun's own surface (the part's own triangles drawn again with a glow:
// body.cpp, pso.h glow), in the scene, hidden where the hand covers it; the holster zones and the chest keep their glow. A
// gun place keeps the sprite only while no glow draw is played (no glow twin on this PC, no triangles found for it).
int interaction_highlights();
// (2026-10-31, the off-hand build step 4) a hand holds rounds for the second gun or the copy (DualWieldCrossReload), as of
// the last frame end: actions.cpp's wrist-flick shut of that gun waits
bool rounds_for_second();
void set_interaction_highlights(int mode, bool save = true);
// [Holsters] PartGlowColor (sRGB 0-1: "r g b", round 45's warm gold 1.0 0.84 0.52 by default) and PartGlowStrength (0-8,
// 1 by default: the glow's brightness against the part's own diffuse): the parts mode's glow. Any thread. save: the user ini
void part_glow_color(float rgb[3]);
void set_part_glow_color(const float rgb[3], bool save = true);
float part_glow_strength();
void set_part_glow_strength(float s, bool save = true);
// The highlights' places (the update's ids): a holster zone (kHlZone + its index), the foregrip, the gun in hand's
// loading point, its magazine well (PistolMagazines), the second gun's loading point (DualWieldCrossReload), the
// action's part to work (ActionHints 2: a bolt, a breech's part, a slide, the semi-auto's handle, FanHammer's hammer), the
// part a flick works (ActionHints 3: a lever, a hammer cocked by the stick, a breech's block) and the pump's fore-end
// (ActionHints 4)
// (2026-10-31, step 7, R11: kHlShut, the part that closes: actions' hint 5)
enum HighlightId : int8_t { kHlZone = 0, kHlFore = 8 /* kZones, below */, kHlLoad, kHlMag, kHlLoadSec, kHlPart, kHlLever, kHlPump, kHlShut, kHlCount };
enum HighlightShape : uint8_t { kHlShapePart = 0, kHlShapeArea = 1 };
struct Highlight {
    float pos[3];
    float radius;  // the part's (or the area's) size, m: the glow is drawn about twice as wide, fading out
    float target;  // the strength wanted now (0 off, up to 0.65 near, 1 at reach); the drawing eases toward it (below 0: off at once)
    uint8_t shape;
    int8_t id;
    // (2026-11-03, round 74: the Henry's own ring off its surface, its loading place unlit) the loading place's point with every
    // gun's ring, for the glow when the gun's own ring's point is on none of the gun (kHlLoad only; else alt_ok false)
    float alt[3];
    bool alt_ok;
};
struct Highlights {
    float cam[16];
    int n;
    Highlight h[kHlCount];  // every place that exists this frame (target 0 where it is not usable, or over the sights)
};
// The last frame end's (the presenting thread publishes before the XR frame end, as markers()). False with the mode
// rings, out of first person, or older than 100 ms. Any thread.
bool highlights(Highlights* out);
const char* highlight_name(int id);  // "RightHip" ... "Foregrip", "LoadPoint", "MagWell", "LoadPoint2", "Part", "Lever", "Pump"
// (run 29 item 2) the gun a place is on: 0 the gun in hand (the foregrip, the pump, its loading point, its magazine well,
// the action's part, the lever), 1 the other gun (the second gun's or the copy's loading point; round 52: the carried
// gun's part or pump this update, [Hands] CarryByForegrip's gun drawn as the second), -1 none (a holster zone). Any thread.
int highlight_gun(int id);
// [Holsters] UnarmedAfterHolster: a put-away at a holster also selects unarmed (the fists), as the weapon wheel's
// Unarmed does (SET_PLAYER_MELEE_MODE_SELECTED): LT is then the fist-fight stance, not a draw of the last gun.
bool unarmed_after_holster();
void set_unarmed_after_holster(bool on);  // also written to the user ini
// [Holsters] HoldToKeep (2026-10-30, off; holster.cpp's g_hold has the rules): a gun drawn at a holster stays in the hand
// only while the grip that drew it stays held; let go, it goes back to its holster, or with the other hand on a long
// gun's foregrip it stays carried by that hand ([Hands] CarryByForegrip); DualWield's second gun and the copy too
bool hold_to_keep();
bool hold_items_on();  // [Holsters] HoldItems: the knife, the torch, the lasso and the throwables held by the grip too
// (round 122) controller h's grip holds weapon w in hand by HoldToKeep (the holsters' mode, not the wheel's)
bool grip_holds(int h, int w);
void set_hold_to_keep(bool on, bool save);  // save: also written to the user ini (the menu; "holster hold on|off" is the session's)
// that controller's grip is HoldToKeep's hold on its gun, holding no rounds (in a holster's zone too: 2026-11-05) (gestures.cpp: the
// gun's swing is not held off as a holster's grip)
bool grip_keeps(int h);
constexpr int kZones = 8;  // RightHip, Back, Belt, Knife, Ammo (the chest: a round to reload, reload.h), LeftHip, LeftShoulder, LowerBack
bool zone_enabled(int z);
void set_zone_enabled(int z, bool on, bool save);  // [Holsters] <Key>Enabled
int zone_count();
const char* zone_label(int i);
void zone(int i, float off[3], float* radius);
void set_zone(int i, const float off[3], float radius, bool save);  // save: also written to the user ini
// [Holsters] <Key>GunTurn (deg: tilt, turn, lean) and <Key>GunOffset (m: right, up, forward): the gun shown at a holster
// (ShowGuns) turned about its grip and moved (2026-10-12); only the hips' and the backs' zones show a gun
bool zone_shows_gun(int i);
void zone_gun(int i, float turn[3], float off[3]);
void set_zone_gun(int i, const float turn[3], const float off[3], bool save);
// "holster [on|off|trace [off]|dump <file>|zones|instant on|off]": the switches, the zones and the counters
std::string command(const std::string& line);

}  // namespace rdrvr::holster
