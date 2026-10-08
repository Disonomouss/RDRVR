#pragma once
// Run 8 item 4: the horse whistle by gesture ([Gestures] Whistle, off). A hand at the mouth (a head-local point,
// [Gestures] WhistleOffset right/up/forward of the eyes' midpoint, within WhistleRadius) and its trigger pressed there:
// the game's whistle (GENERIC.WHISTLE: the pad's D-pad up, FUN_14000c0b0's action table) sent once, a haptic tick, a
// log line. While the hand is at the mouth, and until its trigger is let go after a whistle, that controller's trigger
// reaches nothing (no shot, no aim, no throw, no punch). The gun hand whistles only with no gun in it ([Gestures]
// WhistleGunHand, off: the gun hand at the cheek is a sight picture, not a whistle).

#include <cstdint>
#include <string>

namespace rdrvr::whistle {

void init();  // reads [Gestures] Whistle*, logs them
// controls::pad, each poll, before the triggers are mapped: the gesture judged; true when the whistle's button
// (D-pad up) is to be held in this poll. trig[2] are the controllers' triggers (zeroed where suppressed).
bool poll(double now_ms, float trig[2]);
bool suppressed(int ctrl);  // that controller's trigger is the whistle's (any thread)
bool at_mouth(int ctrl);    // that hand is at the mouth (the punch is not judged for it)
bool enabled();
void set_enabled(bool on, bool save);
std::string command(const std::string& line);  // "whistle [on|off] [offset <r> <u> <f>] [radius <m>] [reset]": the session only

}  // namespace rdrvr::whistle
