#pragma once
// [Gestures] HorseshoeThrow (round 133, off by default; the user: "Horseshoes is unplayable, there is no way to tell what
// you are supposed to do" ... "Horse shoes having a real throw would be good"): at the game of horseshoes the shoe is
// thrown by your own arm. See horseshoe.cpp.

#include <string>

namespace rdrvr::horseshoe {

void init();     // the settings
void install();  // the game hooks (with the others, after the anchors' check)
// Each frame (holster_frame, after the game's gate and the minigames' scripts are polled).
void frame();
// The option on and horseshoes' script running: John's arms stay on the controllers there ([Body] TableGameArms does not
// apply), and the script's swing and release are answered from the hand.
bool active();
// active(), with a shoe hung on John's right wrist by the script: the prop at his right hand's attachment is the hand's at
// once (body.cpp's hand-held props: no move asked for first, and not laid down while his hands rest).
bool holds();
// "horseshoe [on|off] [gain <x>]": the session's; the counts.
std::string command(const std::string& line);
bool enabled();
void set_enabled(bool on, bool save);

}  // namespace rdrvr::horseshoe
