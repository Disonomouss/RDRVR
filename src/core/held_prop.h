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

// 0 the round in hand (the shotgun shell), 1 the other revolver (run 5 item 2), 2-5 the guns at the holsters (run 6
// item 6: [Holsters] ShowGuns; the right hip, the back, the left hip, the left shoulder), 6-7 the holster models at the
// right and left hips ([Holsters] ShowModels, 2026-10-09)
constexpr int kSlots = 8;
constexpr int kHolsterSlot = 2;
constexpr int kModelSlot = 6;

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
// The draw hook (render thread): if `drawable` is a held prop's (or, before it is known, the record's position `pos`
// is within 3 cm of where the object was put), its wanted pose (12 floats as above), its slot (if `slot`) and true. Of
// the slots with that drawable the one put nearest `pos`, within 1.5 m of one of its last four puts (two props of one
// model; another's gun of it).
bool match(uintptr_t drawable, const float* pos, float* pose, int* slot = nullptr);
// "props diag [on|off|reset]" (a test command): the draws near each placed slot (within 0.5 m of its recent puts) by
// pass, of its own model and of any other (the last other's drawable). The draw hook calls diag_draw only while on.
bool diag_on();
void diag_draw(uintptr_t drawable, const float* pos, uint64_t pass, const float* m = nullptr);  // m: the record's 4x4 (axes rows 0-2)
std::string diag_command(const std::string& arg);
int live_objects();  // made and not yet destroyed
// Slot s's prop drawn (a draw of it moved) within the last 100 ms. Any thread.
bool shown(int s);
std::string status();

}  // namespace rdrvr::held_prop
