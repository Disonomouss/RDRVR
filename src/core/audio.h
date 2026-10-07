#pragma once

#include <string>

// The empty gun's click ([Reload] EmptyClick, the menu's Reloading tab; round 7: a sound instead of the buzz). The
// game has no dry-fire sound for the player (ENGINE-NOTES, round 7 follow-ups), so the mod plays its own: a hammer
// click synthesised once (or [Reload] EmptyClickWav), on the Windows default audio device through XAudio2 2.9, the
// game's own DLL, device and category, so whatever carries the game's sound to the headset carries the click. Its own
// "RDRVR audio" thread opens the engine and plays; the caller only signals it. Round 9: a click per weapon family, from
// the user's own sounds ([Reload] EmptyClickRevolver, EmptyClickRifle, EmptyClickShotgun: WAVs next to RDR.exe, the
// deploy's RDRVR_click_*.wav), resampled once to the voices' 48 kHz mono.
namespace rdrvr::audio {

enum class ClickMode : int { Sound = 0, Buzz = 1, Both = 2, Off = 3 };

void init();  // reads [Reload] EmptyClick*, starts the audio thread
// The gun hand's trigger on an empty gun (holster.cpp's frame end, presenting thread): the click from that hand's side
// and/or the buzz, per the mode. Atomics and one SetEvent; never blocks. h: the controller, 0 left, 1 right; weapon: the
// eWeapon in hand, which picks the family's click (-1: the default click).
void empty_click(int h, int weapon = -1);
// The family of an eWeapon for its click: 1 revolvers and pistols, 2 rifles and repeaters, 3 shotguns, 0 none.
int click_family(int weapon);
// The menu's Click volume preview: the click an empty `weapon` plays (its family's; the revolvers' when no gun is in
// hand), from the right hand's side.
void preview(int weapon);
ClickMode empty_click_mode();
void set_empty_click_mode(ClickMode m);  // also written to the user ini
float click_volume();
void set_click_volume(float v, bool save);
// "sound": the state; "sound click [left|right] [revolver|rifle|shotgun]", "sound mode sound|buzz|both|off", "sound volume <0..1>",
// "sound pan <0..1>", "sound dump <file.wav>", "sound reset" (the device-lost path)
std::string command(const std::string& line);

}  // namespace rdrvr::audio
