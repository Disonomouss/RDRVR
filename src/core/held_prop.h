#pragma once
// Run 5: a game prop made at run time and held in a hand (research\run4\rounds.md: the request, the stream, the loaded
// check, then CREATE_PROP_IN_LAYOUT), for the shotgun shell in the hand ([Reload] RoundInHand = 2) and the other
// revolver of a pair (item 2). The object is frozen, its collisions off, and kept near the drawn hand by
// SET_OBJECT_POSITION each update so the game draws it (its culling); its draw is moved exactly onto the wanted pose
// in body.cpp's draw hook (matched by its drawable, learned from its first draws at the object's position), so it
// follows the drawn hand of the same frame without the script tick's lag. Destroyed when no longer wanted, when first
// person ends or the switch is turned off; one object per slot at most. Natives only with the handles the game
// returned; every result collected.

#include <cstdint>
#include <string>

namespace rdrvr::held_prop {

// 0 the round in hand (the shotgun shell; 2026-10-27: retired for the pool below), 1 the other revolver (run 5 item 2),
// 2-5 the guns at the holsters (run 6 item 6: [Holsters] ShowGuns; the right hip, the back, the left hip, the left
// shoulder), 6-7 the holster models at the right and left hips ([Holsters] ShowModels, 2026-10-09), 8 the props
// census's (run 11 item 1: "props show"), 9 the off hand's item ([Hands] OffHandItems: the knife, a throwable, the torch),
// 10-15 free (run 11's props belt, retired in run 12), 16-29 the shotgun shells' pool (2026-10-27), 30-31 free
constexpr int kSlots = 32;
constexpr int kHolsterSlot = 2;
constexpr int kModelSlot = 6;
constexpr int kCensusSlot = 8;
constexpr int kOffhandSlot = 9;
// 2026-10-27 ([Reload] RoundInHand = 2; the user: "Use the games shotgun shells"; research/run27/rounds.md section 5):
// the game's own shell wherever a shell is drawn, the hand's handful (RoundsPerGrab, up to 8) and the row at the chest
// (up to 6) at once: a pool of up to 14 objects of one model, made while a shotgun is in hand (not per grab), its
// objects interchangeable: pose k of the wanted list is drawn by the k-th live one, the others' draws hidden (their axes
// shrunk, as the belt's twin's), none destroyed when a shell goes in. Each is put at its pose (or, spare, at `base`) a
// few cm off along its own direction, so its draws are told apart by their nearest put (props carry no instance)
constexpr int kPoolFirst = 16;
constexpr int kPoolMax = 14;

// Game thread (the holsters' update), every frame: the natives' state machine (requests, polls, creation, moves,
// destruction) and the results collected.
void frame(uint32_t actor);
// Any thread: what slot `s` should hold: its fragment (a static string, the file name without .wft) and the drawn
// pose (12 floats: the axes X, Y, Z as columns' world vectors, then the position), or fragment nullptr: none. With an
// anchor (the body's pelvis the pose was placed by, world), the pose is drawn moved by its move since (note_root).
void want(int s, const char* fragment, const float* pose, const float* anchor = nullptr);
// Any thread: slot s's object turned by SET_OBJECT_ORIENTATION (Euler degrees, the native's three values) with each
// move; nullptr: not turned (the default; the draw hook moves a learned prop's draws instead)
void want_angles(int s, const float* deg);
// The render thread, once a frame as the body's points are published: the body's anchor point of that frame (world;
// the pelvis as drawn, body::kPelvisPoint).
void note_root(const float* root);
// Slot s's last drawn position (world), when (ms), that position less the body's root its draw used (rel), and how
// many draws of its model were gated out (beyond 1.5 m of its recent puts); false before any draw. Any thread.
bool last_drawn(int s, float pos[3], double* ms, float rel[3] = nullptr, uint64_t* gated = nullptr);
bool last_put(int s, float pos[3]);  // run 11: slot s's last queued position (world); false: none
// The draw hook (render thread): if `drawable` is a held prop's (or, before it is known, the record's position `pos`
// is within 3 cm of where the object was put), its wanted pose (12 floats as above), its slot (if `slot`) and true. Of
// the slots with that drawable the one put nearest `pos`, within 1.5 m of one of its last four puts (two props of one
// model; another's gun of it).
// inst: the draw's instance (its record's owner: run 10 item 2), 0 when unknown
// hide (2026-10-27): set when the draw is a pool object's with no pose to draw (the caller hides it: its axes shrunk);
// the pose is then not filled. nullptr: such draws are not matched.
bool match(uintptr_t drawable, uintptr_t inst, const float* pos, float* pose, int* slot = nullptr, bool* hide = nullptr);
void set_inst_check(bool on);  // "props inst on|off" (on by default)
// 2026-10-12: a holster's shown gun (slots kHolsterSlot..+3, wanted) has this model: the game's own holstered gun of the
// same model on John's belt is hidden then (body.cpp), so one gun shows at the hip. ([Hands] OffHandItems) the off hand's
// item too (kOffhandSlot): the game's own knife at its sheath hidden while our copy is in the hand
bool shown_at_holster(uintptr_t drawable);
void set_twins(bool on);  // "props twins on|off" (the session)
void set_puts(int n);          // "props puts <n>": the recent puts a draw is matched against (8; 1-64; the session)
void set_gate(float m);         // "props gate <m>": a draw matched within that distance of a recent put ([Holsters] PropGate)
// "props diag [on|off|reset]" (a test command): the draws near each placed slot (within 0.5 m of its recent puts) by
// pass, of its own model and of any other (the last other's drawable). The draw hook calls diag_draw only while on.
bool diag_on();
void diag_draw(uintptr_t drawable, const float* pos, uint64_t pass, const float* m = nullptr);  // m: the record's 4x4 (axes rows 0-2)
std::string diag_command(const std::string& arg);
int live_objects();  // made and not yet destroyed
// Any thread: on, no holster slot's object is made or kept and none counts as shown, so the game's own holstered gun
// shows; the wants are kept for when it is off. hogtie.cpp: while someone is on the player's lasso (the game's hogtie
// check sweeps between the two, and these free objects are in its way; after optimumbox's 0.8.11.8). 2026-10-27: the
// shells' pool too (the row stands at the chest, in the sweep's way; round_draw's own shells stand in meanwhile).
void set_clear(bool on);
bool holster_wanted();  // a holster's gun or model (or, 2026-10-27, the shells' pool) is wanted now (hogtie.cpp polls the lasso only then)
// Slot s's prop drawn (a draw of it moved) within the last 100 ms. Any thread.
bool shown(int s);
// The pool (2026-10-27), the game thread (the holsters' update, before frame): `objects` of `fragment` kept (0 or
// nullptr: none, each destroyed; at most kPoolMax), the first n of `poses` (12 floats each, as want's) drawn by them,
// the first n_free as given, the others moved with the body's walk since `anchor` (the row at the chest; nullptr: none
// moved), any object beyond them spare (put near `base`, its draws hidden). hogtie's set_clear takes them away too.
void want_pool(const char* fragment, int objects, const float* base, const float* poses, int n, int n_free, const float* anchor);
// Pose k of the pool's wanted list drawn by a game object (a draw of it moved) within the last 60 ms: round_draw draws
// its own stand-in shell for every pose that is not. Any thread.
bool pool_shown(int k);
std::string status();

}  // namespace rdrvr::held_prop
