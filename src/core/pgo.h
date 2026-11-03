#pragma once
// Ped Gunfight Overhaul (2026-10-29; research\pgo\pgo.md): a third-party RedHook plugin (PedGunfightOverhaul.red and
// PedGunfightOverhaul.ini next to RDR.exe; Nexus RedHook-578, v1.1.5). The menu's Gunfights page, shown only while its
// .red is installed, makes every adjustment its own keyboard menu makes: the five [GENERAL] settings, its Save
// (Numpad 5: the values into its ini) and its Reload (Numpad 0: the ini's values), and also [KEYBINDS] InGameControls.
// Its settings are plain globals of its module, which its script re-reads at every pass (the accuracy and damage
// scales each second, the headshot and cripple rules each 250 ms) and its own keys change in place: with its module
// loaded and exactly v1.1.5 (its TimeDateStamp, SizeOfImage and the six instructions that read them), a change here is
// written there, as its keys do, and applies within a second ("live"). Another build, or the .red not loaded: the
// page edits only the values its ini gets on Save (applied at its next start or its own Reload).

#include <string>

namespace rdrvr::pgo {

struct Values {
    float ped_accuracy = 100.0f;    // [GENERAL] PedAccuracy, 0..200: the NPCs' aim (%, the factions' accuracy scale / 100)
    int ped_health = 25;            // PedHealth, 0..1000000: the damage NPCs take is 25 / this (0: none, invincible)
    int player_health = 100;        // PlayerHealth, 0..1000000: the damage John takes is 100 / this (0: none)
    int cripple_at = 100;           // EnableCrippleAtHealth, 0..100: NPCs may limp, kneel or crawl only at or below this % of health
    bool headshot_kills = false;    // HeadshotAlwaysKills
    bool in_game_controls = true;   // [KEYBINDS] InGameControls: its own keyboard menu
};

enum class Link { kNotLoaded, kOtherBuild, kLive };

bool installed();  // PedGunfightOverhaul.red next to RDR.exe (looked up at most every 2 s)
Link link();       // its module now
Values get();      // live: its memory now; else the values its ini gets on Save
void set(const Values& v);        // clamped to its own ranges; live: written into its memory
bool save(std::string* err);      // its Numpad 5: the values into PedGunfightOverhaul.ini (its comments and other keys kept)
bool reload(std::string* err);    // its Numpad 0: the ini's values (live: into its memory)
bool unsaved();                   // the values differ from its ini's as last read or saved

// "pgo": installed, the link, the values, unsaved; "pgo set <key> <value>" (a test's: as the page's arrows),
// "pgo save", "pgo reload"
std::string command(const std::string& line);

}  // namespace rdrvr::pgo
