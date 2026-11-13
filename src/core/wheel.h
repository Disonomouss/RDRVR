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
// (round 143, the user: "Add option to have holding a trigger while hand is on a holster open the weapon wheel. Then
// whatever weapon you select is put into that holster.") [Holsters] WheelAtHolster, off: with the holsters (not
// Mode=wheel), a trigger pulled with its hand empty at a holster (trig_zone[c], the holsters' own test) opens the same
// wheel, steered by that hand as above; the trigger let go picks (in the dead zone: nothing). The pick's holster is
// handed to the holsters (take_assign) to become its weapon, the weapon put away into it. taken_trig[c]: that
// controller's trigger held back from the game while it holds the wheel.
void poll_triggers(const bool trig[2], const int trig_zone[2], bool taken_trig[2]);
// the last trigger-wheel pick: the holster, the weapon in hand at the pick, the controller; once (false: none)
bool take_assign(int* zone, int* weapon_before, int* ctrl);
// a trigger-wheel cancelled (the weapon in hand then); once. The game may draw the last weapon at the cancel's LB
bool take_cancel(int* weapon_before);
bool open();          // the wheel's button held by the mod now
// The controller whose grip holds the wheel open now (-1: none). controls::pad: that hand's stick is the D-pad while it
// is (2026-10-26, the user: "should be able to use joystick of whichever hands grip opened it to act as dpad. So you can
// select which gun of each category to select"), its own axes held back. [Holsters] WheelStickDpad (on).
int holder();
bool stick_dpad();
void set_stick_dpad(bool on, bool save);
// (round 145, the user: "Weapon wheel should be controlled with joysticks. Left to select gun catagory, right as D pad to
// switch between each gun within the catagory.") [Holsters] WheelSteer=sticks (the default; hand: as before): every
// wheel the mod holds open (a grip's in Mode=wheel, a trigger's at a holster) takes the layout's left stick as the game's
// wheel stick (kept where last pushed past 0.6; never pushed: a cancel) and the layout's right stick's left and right as
// the D-pad (controls::pad); neither stick walks nor turns meanwhile. poll_sticks: the layout's left stick, before poll.
void poll_sticks(const float left[2]);
bool steer_by_sticks();
void set_steer_by_sticks(bool on, bool save);
void cancel();        // the pad's poll stopped (the menu, no controllers): the wheel let go, nothing committed
// The hand-placed wheel ([Holsters] WheelAtHand, on): while open, where its quad goes (LOCAL: the hand at the press, the
// head's turn then) and the UI target's rect it shows (fractions x0 y0 x1 y1, [Holsters] WheelRect). False: no quad.
bool hand_quad(float pos[3], float orient[4], float rect[4]);
// [Holsters] WheelPause (off; 2026-10-27, the user: "Weapon wheel pausing game until you pick a weapon"; research\run27\
// wheelpause.md): while a grip holds the game's wheel open (shown: the player info's +0x43f, after the 300 ms open), the
// world is held near still by the game's own time warp, Dead Eye's (SET_TIME_WARP at WheelPauseScale, 0.02: never 0,
// the engine's 1/dt), renewed as a 1 s lease every 250 ms so it ends by itself should the renewals stop; every way out
// (a pick or none, cancel(), the mode or the option off, the game's wheel closed, the actor gone, mounting changed,
// Dead Eye, a cutscene or a script's camera, a grab, shutdown()) cancels it at once (CANCEL_TIME_WARP 0), and only
// while the warp is still ours (our target and our ramp-in); another warp found at a renewal ends ours uncancelled.
// Not started under Dead Eye, another warp, a cutscene or a script's camera, or while a hand holds a prop (physics: a
// held prop moved by wall time at a tiny dt).
bool wheel_pause();
void set_wheel_pause(bool on, bool save);
float wheel_pause_scale();
void set_wheel_pause_scale(float scale, bool save);  // clamped to 0.01..0.08 (below Dead Eye's 0.1)
void shutdown();      // ends the pause (the warp cancelled if ours); for a core shutdown (none runs today: see wheel.cpp)
// "wheel [on|off] [reset] [pause on|off|scale <f>|hold]": the mode, the pause for the session (hold: test only, the
// renewals stop so the lease's own end is seen), the counts
std::string command(const std::string& line);

}  // namespace rdrvr::wheel
