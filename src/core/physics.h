#pragma once

#include <string>

// Hands that push loose objects ([Physics] HandCollision, off; run 3 item 6, research\run3\handphysics.md 6.3): an empty
// hand moving through a loose prop (a bottle, a crate, a chair: the game's p_* fragments) pushes it along the hand,
// through the game's own natives in one script tick (the plugin's HAND_PUSH: LOCATE_PHYSINST_OF_PARTIAL_TYPE, then
// SET_PROP_VELOCITY). Never an actor. NPCs react through the game's melee instead ([Gestures] Melee). Grabbing (item 7)
// is designed in the research, not built.
namespace rdrvr::physics {

void init();  // a frame-end listener (after the holsters')

bool enabled();
void set_enabled(bool on);  // also written to the user ini
// [Physics] Grab (run 5 item 5, off): an empty hand gripping at a loose prop holds it (frozen, at the palm), and lets it
// go thrown with the hand's velocity; out of first person or turned off, it is let go unfrozen.
bool grab_enabled();
void set_grab_enabled(bool on);  // also written to the user ini
bool holding(int ctrl);  // that controller's hand holds (or is asking for) a prop
// controls.cpp at a grip's press (any thread): Grab is on and that hand is empty, so the press is held back from the
// game until the grab is known; grab_held(ctrl) then keeps it the mod's, a change of grab_misses(ctrl) (no prop there)
// gives it to the game
bool grip_wanted(int ctrl);
bool grab_held(int ctrl);
uint32_t grab_misses(int ctrl);

std::string command(const std::string& line);  // "physics [on|off] [probe <radius>]": the counters, the last push

}  // namespace rdrvr::physics
