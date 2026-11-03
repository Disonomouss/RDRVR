#pragma once
// [Debug] BackgroundFocus (2026-10-21, run 16, test only: the bench's no-focus simulator cycles; research/run16/focus.md):
// the game reads the pad only while its window is active (WM_ACTIVATEAPP sets the window's +0xad, read through the
// window vtable's IsActive, slot +0xe8), and pauses when it is not (the bFocusPaused option). On, that one vtable slot
// answers "active" to the pad update and the pause check alone (by their return addresses); every other caller (the
// mouse, which zeroes its deltas) gets the real state, so nothing about the cursor or the user's mouse changes. No game
// code is patched and no user setting written. Off by default; never in a player's ini.
#include <string>

namespace rdrvr::bgfocus {
void install();                                // after anchors::verify(): the slot swapped when the ini switch is on
std::string command(const std::string& line);  // "bgfocus [on|off]": the session's switch (installed only), and its counts
}  // namespace rdrvr::bgfocus
