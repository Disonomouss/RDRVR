#pragma once
// Settings: RDRVR.ini next to RDR.exe (shipped, every key commented, replaced by each release), then
// %LOCALAPPDATA%\RDRVR\RDRVR.user.ini (only what the menu writes, or RDRVR_Launcher.exe while the game is closed;
// never overwritten by releases or tests).
// A key in the user file wins. Every key read is echoed to the log once with its value and its source,
// so a test can prove a switch reached the code.

#include <string>

namespace rdrvr::config {

void load();    // (re)reads both files; safe to call again (e.g. from the test channel)
// [Stereo] SinglePass at the last load (GOAL-single-pass-default.md item 3): -1 no key (the code's defaults), 0 two-pass
// (the switch), 1 single pass (the stack's keys supplied where unset), 2 asked for but the shader converter is missing,
// 3 asked for but a user key turns it off (single_pass_off_key(): which)
int single_pass_state();
std::string single_pass_off_key();

bool get_bool(const char* section, const char* key, bool def);
int get_int(const char* section, const char* key, int def);
float get_float(const char* section, const char* key, float def);
std::string get_string(const char* section, const char* key, const char* def);
// the shipped RDRVR.ini's value whatever the user file says (2026-10-13: the menu's resets to the defaults)
std::string get_shipped(const char* section, const char* key, const char* def);

const wchar_t* user_ini_path();
// The menu's write: `key` = `value` in the user file (created on first use; only that key's line changes, every
// other line and comment is kept) and in the values read from now on. Logged with the old and new values.
bool set(const char* section, const char* key, const std::string& value);
// Test aid ("cfg userini <path>"): the user file is `path` from now on and both files are read again, so a test of
// the menu never writes the real user file.
void set_user_path(const wchar_t* path);

}  // namespace rdrvr::config
