#pragma once
// Run 8 item 5: the game's weapon wheel instead of the holsters ([Holsters] Mode = holsters | wheel; holsters by
// default). With the wheel, the holster zones draw and put away nothing (their rings hidden; the chest's rounds stay),
// and a grip held (a free one: not the foregrip, a grab or a gun's part) holds the game's wheel button (LB: the wheel
// opens after 0.2 s). Where the hand was at the press is the wheel's centre; moving the hand away from it, in the plane
// facing the head, is the right stick's direction (full past [Holsters] WheelDeadZone, 4 cm). The grip let go: LB let
// go with the stick still held (the game equips the highlighted weapon), the stick let go 150 ms after; inside the dead
// zone the wheel cancels. LB is held at least 300 ms (a shorter press is the game's holster tap). The weapon goes to
// the gripping hand (controls::set_draw_hand).

#include <cstdint>
#include <string>

namespace rdrvr::wheel {

void init();          // reads [Holsters] Mode and Wheel*, logs them
bool wheel_mode();    // [Holsters] Mode = wheel (relaxed)
void set_wheel_mode(bool on, bool save);
// controls::pad, each poll: free[c] is controller c's grip down and free (not the holsters', a grab's, a part's).
// Returns true for the controllers whose grip the wheel takes this poll (taken[c]); drives the pad's override itself.
void poll(double now_ms, const bool free[2], bool taken[2]);
bool open();          // the wheel's button held by the mod now
void cancel();        // the pad's poll stopped (the menu, no controllers): the wheel let go, nothing committed
// The hand-placed wheel ([Holsters] WheelAtHand, on): while open, where its quad goes (LOCAL: the hand at the press, the
// head's turn then) and the UI target's rect it shows (fractions x0 y0 x1 y1, [Holsters] WheelRect). False: no quad.
bool hand_quad(float pos[3], float orient[4], float rect[4]);
std::string command(const std::string& line);  // "wheel [on|off] [reset]": the mode for the session, the counts

}  // namespace rdrvr::wheel
