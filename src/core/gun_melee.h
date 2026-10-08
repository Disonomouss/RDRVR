#pragma once
// Run 7 item 2: the gun-butt melee ([Gestures] GunMelee, off; research\round13\gun-melee.md). A fast swing of the drawn
// gun's strike point (a long gun's butt and barrel, a pistol's frame) is found at the frame end (gestures.cpp); the
// plugin, on its script tick, finds the actor and the bone the strike swept through (RDRVR_NATIVE_GUN_MELEE) and calls
// hit() in that same tick. hit() checks both handles through the pools, gates the victim as the game's melee does,
// builds the DamageInfo the fist melee builds (FUN_140d007f0; the layout in the research's 3.1) and calls the game's
// hit, FUN_140ae1760 (anchor ActorHit), once: the AI's attacker memory, the KO points (or the health), the hit record
// the victim's action tree reacts to. No hook and no write to game memory by the mod: the game's own writes, inside its
// own function, on the game thread. The call has no SEH wrapper (the hit takes a critical section): everything it
// reads unchecked is validated before it.

#include <cstdint>
#include <string>

#include "common/rdrvr_api.h"

namespace rdrvr::gun_melee {

struct Config {
    bool on;       // GunMelee
    float speed;   // GunMeleeSpeed: m/s a hit needs (the strike point's own motion)
    float arm;     // GunMeleeArm: m/s that starts the scans
    float damage;  // GunMeleeDamage: at GunMeleeSpeed (the weapon tunes' MeleeDamage is 10)
    float heavy;   // GunMeleeHeavy: m/s at which the damage is doubled (linear from GunMeleeSpeed)
    bool lethal;   // GunMeleeLethal: 0 knock-out points (flags 2), 1 health (flags 4)
    float force;   // GunMeleeForce: the reaction's push, times the strike's unit direction (DamageInfo +0xa0)
    bool dry;      // GunMeleeDryRun: the hit built and logged, the game's never called
    float stock;   // GunMeleeStockLen: a long gun's butt behind its origin along its back axis (m)
    bool by_peak;  // GunMeleeByPeak (run 8): a hit judged on the swing's peak, the contact at half the speed
};

void init();                      // reads [Gestures] GunMelee*, logs them
Config config();                  // any thread
bool enabled();                   // GunMelee (relaxed): the frame end's one check while off
void set_enabled(bool on, bool save);  // save: the user ini too (the menu); "gunmelee on|off" is the session's
bool dry_run();                   // GunMeleeDryRun
void set_dry(bool on, bool save);      // save: the user ini too (the menu); "gunmelee dry on|off" is the session's
// GunMeleeSpeed (the menu's "Hit speed", 1-6 m/s): the arm speed kept below it, the heavy speed above it
void set_speed(float mps, bool save);
void set_by_peak(bool on);  // GunMeleeByPeak for the session (the test channel's "gunmelee speed <v> peak|contact")
// RdrvrApi::gun_melee_hit (API v7): the plugin's script tick only (refused elsewhere). Returns an RdrvrMeleeHitCode.
int hit(const RdrvrMeleeHit* h);
// aim.cpp's punch start (the game thread, inside the game's call, the player's melee controller M): the game's own
// melee force scale [[M +0x10] +0xd4] read (SEH) and logged, the GunMeleeForce calibration (research 6.3.6). GunMelee only.
void note_punch(uintptr_t m);
// The game's hit zone for a bone name (FUN_140ae4bb0: 0 head, neck, facial; 1 spine03, spine02 and the default; 3
// spine00; 4 the other spine, root, shoulders, clavicles; 5 / 6 arms; 7 / 8 legs and the pelvis, the left ones by a
// final 'l')
int zone_of(const char* bone);
std::string status();             // the hit side: calls, hits made, dry hits, refusals by reason, the force read

}  // namespace rdrvr::gun_melee
