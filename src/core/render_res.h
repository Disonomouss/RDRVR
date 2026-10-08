#pragma once
// [Render] RenderResolution: the game's frame at a size of the player's choosing, apart from the monitor's modes and
// from the window (off by default: the game's own size). Each eye is the game's frame (or, with [XR] EyeShape, the
// eye's shape inside it, at most its height), so a taller frame gives the headset its full density without DSR.
//
// Applied at the start only, through the game's own boot path (research: the render resolution study, 2026-10-08):
//  - the device init FUN_14012e420 reads the sysParams "width"/"height" (their value pointers 0x1422bdc48/0x1422bdc80,
//    read nowhere else) over the xml's size: the mod points them at the chosen size before it runs (RenderThreadId,
//    written at its entry, still 0), so the swapchain and every target are made at that size;
//  - Windowed: the window create FUN_140ec3770 (pre-hook) keeps the player's own window size (0x14225b088/08c), the
//    swapchain is larger and DXGI's STRETCH scales it into the window; the game's mouse is normalised by the client;
//  - the game's Resolution list (user32 EnumDisplaySettingsW through RDR.exe's import slot, its two list calls only)
//    gets the chosen size as one more mode, so the Graphics menu selects it and never snaps to another;
//  - the options save FUN_140158be0 (pre/post) keeps graphicsOptions.xml at the player's own size: without the mod
//    (or with the option off) the game starts as before.
// All or nothing: each piece checked and installed before the sysParams are written, or none is. Refused (and said
// why in the menu) on a low-VRAM card, above a share of the VRAM, after a start at that size that ended within two
// minutes and not by quitting (the boot sentinel), or too late (the game's device already made). A frame larger than
// the runtime's largest image is scaled down for it (xr.cpp).

#include <cstddef>
#include <cstdint>
#include <string>

namespace rdrvr::render_res {

// bootstrap, right after config::load (on the analysed build: before the game's device init, which reads the size)
void early_arm();
// with the RDR.exe hooks, after anchors::verify(): on a relocated build, the same arm if the game has not read the
// size yet
void install();

// The XR session thread, after xrGetSystem: the runtime's recommended eye image (the larger eye) and the largest image
// it takes (min of maxSwapchainImage and the views' maxImageRect), kept per runtime for Automatic and the menu's limits.
void record_runtime(const char* runtime, uint32_t rec_w, uint32_t rec_h, uint32_t cap_w, uint32_t cap_h);

// This start's override, once the game took it (the frame is w x h); false: the game's own size.
bool active(uint32_t* w = nullptr, uint32_t* h = nullptr);
// The process's clean exit (exit_guard): the boot sentinel cleared (as are two minutes at the size).
void on_exit();

// The menu: the choices (index 0 = the game's own, 1 = Automatic, then the fixed sizes).
int choice_count();
const char* choice_label(int i);   // e.g. "2208 tall: Quest 3 (3926 x 2208)"
bool choice_size(int i, uint32_t* w, uint32_t* h);  // the size it gives now (Automatic: from the last session's record)
int choice();                      // the saved choice ([Render] RenderResolution)
void set_choice(int i);            // saved to the user ini; applies from the next start
bool choice_allowed(int i, char* why, size_t why_len);  // false with the reason (VRAM, the runtime's largest image)
void status_text(char* out, size_t len);  // "running 3926 x 2208 (your window 2560 x 1440)" or why not

// "renderres [status]": the state for tests
std::string command(const std::string& line);

}  // namespace rdrvr::render_res
