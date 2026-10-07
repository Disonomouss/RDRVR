#pragma once

#include <cstdint>
#include <string>

// Dual wielding ([Hands] DualWield, off; run 3 item 5, research\run3\dualwield.md 6.4, "Tier A"): a second gun in the
// free hand, the gun of another equip slot than the one in hand (a long gun with a sidearm in hand, or the hip sidearm
// with a long gun in hand). The game still holds one item in its hand. The second stays holstered for the game, but
// its weapon object W is placed in the free hand (body.cpp, at the prop placement) and fired through the gun item's
// own trigger (vtable slot 6) at the weapon tick: its own projectiles, ammo, muzzle flash and gunshot.
namespace rdrvr::dual {

void init();     // [Hands] DualWield; a frame-end listener (the second gun's trigger, its end)
bool install();  // the weapon tick hook (startup)

bool enabled();
void set_enabled(bool on);  // also written to the user ini
void set_enabled_session(bool on);

// The frame end (holster.cpp): the free controller `ctrl`, John's hand `john` on it, takes the gun of equip slot `slot`
// (gripped at the zone `zone`); end() puts it back (the game's next update hangs it on its holster again).
void begin(int ctrl, int john, int slot, const char* zone);
void end(const char* why);

// [Hands] DualWieldCopy (run 4 item 3, off): the same sidearm in the free hand. Both sidearms are equip slot 1, which
// holds one item, so the second has no weapon object of its own: the copy is the gun in hand's W drawn a second time at
// the free hand (body.cpp), fired through the gun in hand's own trigger with its shot and muzzle flash moved to the
// copy's muzzle (aim.cpp's launch, the MuzzleFxMatrix hook), and its own clip count, loaded from the spare rounds.
// begin(ctrl, john, kCopySlot, zone) takes it.
constexpr int kCopySlot = 99;
bool copy_enabled();
void set_copy_enabled(bool on);  // also written to the user ini
// The game thread: the gun in hand's W while a copy is out (else 0); body.cpp notes each update's copy placement with
// T, the world map from the gun in hand onto the copy (row vectors, p' = p T: P^-1 C)
uintptr_t copy_W();
// John's hand begin() gave the second gun or the copy (0 left, 1 right; -1 none yet). Any thread, no lock.
int copy_john();
void note_copy_placed(uintptr_t W, const float T[16]);
// The game thread inside the weapon tick that fires the copy: true for its W's shot (the launch and the flash move)
bool copy_armed(uintptr_t W);
bool copy_T(float T[16]);  // the last placement's T
// [Hands] DualWieldOwnModel (run 5 item 2, off): the copy shows the other sidearm's own model when it differs (a prop made
// at the copy's place, held_prop slot 1; the copy's second draw skipped while it is drawn), its shot from that model's
// muzzle. The holsters choose the model when the copy is taken (the hip's weapon, else the first owned revolver other
// than the gun in hand); -1: the same model.
bool own_model();
void set_own_model(bool on);  // also written to the user ini
void set_copy_model(int weapon);
int copy_model();  // the eWeapon shown for the copy as a held prop (held_prop slot 1), or -1: the gun in hand's draw made twice
// [Hands] CopyAsProp (on; run 7 item 1): the copy of the same model as a held prop of that model too (the gun in hand's
// skinned draw made a second time with its bones moved was never seen at the free hand)
bool copy_as_prop();
void set_copy_as_prop(bool on, bool save);  // save: also written to the user ini (the menu; "dual asprop" is the session's)
// [Hands] DualWieldSameAtItsHolster (on; round 13 item 10): with DualWieldOwnModel, the free hand at the gun in hand's own
// holster (the one it was drawn from) takes a second of that gun, its own model (another hip: the other sidearm's)
bool same_at_its_holster();
void set_same_at_its_holster(bool on);  // also written to the user ini
const char* sidearm_fragment(int weapon);  // a sidearm's model (its fragment's name), or nullptr
// A sidearm model's muzzle locator in its own axes (m): its offset from the root bone, read from the fragments (run 5)
bool sidearm_muzzle(int weapon, float out[3]);
// The frame end (holster.cpp): the copy's hand pressed to the chest takes a round from the spare rounds
bool load_copy_round(int weapon, float spare, int32_t actor);

struct State {
    bool on = false;      // a second gun is wanted (begun, not ended)
    bool ready = false;   // the game thread has found it (its W) this generation
    int ctrl = -1;        // the controller holding it
    int john = -1;        // John's hand holding it (0 left, 1 right)
    int slot = -1;        // its equip slot
    int weapon = -1;      // its eWeapon (once ready)
    float clip = -1.0f;   // its clip (once ready)
    bool copy = false;    // a copy of the gun in hand ([Hands] DualWieldCopy): clip is the copy's own count
    float copy_max = 0.0f;
};
State state();

// The game thread (body.cpp's prop placement, aim.cpp's shots): the second gun's weapon object while it is out and
// valid (else 0), and John's hand holding it.
uintptr_t secondary_W(int* john, uintptr_t* item = nullptr);
bool is_secondary_W(uintptr_t W);
// body.cpp's placement hook, when it put the second gun in the hand this update (the game thread): the weapon tick fires
// it only then, never from its holster (review 2)
void note_placed(uintptr_t W);

std::string command(const std::string& line);  // "dual [on|off] [begin <slot> [left|right]] [fire] [end]": the state, counters

}  // namespace rdrvr::dual
