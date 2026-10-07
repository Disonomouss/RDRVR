#pragma once
// Virtual pad (DESIGN §5.1): inline hooks on xinput1_4's XInputGetState, XInputSetState and XInputGetCapabilities
// (the game imports ordinals 2, 3 and 4 through its hidden import table). With [Input] VirtualPad=1, pad 0 always
// reports a connected gamepad, because the game polls only the pads XInputGetCapabilities reported. R0 uses it for
// Spike S11: does injected pad state drive the menus? Later the gameplay layer synthesises the whole pad from
// OpenXR actions here, and the rumble the game sends drives controller haptics.

#include <cstdint>

namespace rdrvr::xinput {

bool install();
// Holds `buttons` (XINPUT_GAMEPAD_* bits) on pad 0 for `ms` milliseconds.
void press(uint16_t buttons, uint32_t ms);
// Test aid ("stick lx ly rx ry ms"): pad 0's sticks held at these (-1..1) for `ms`, no buttons.
void press_sticks(float lx, float ly, float rx, float ry, uint32_t ms);
// The gameplay layer's pad (controls.cpp: the controllers mapped onto the game's pad; pose.cpp turns the left stick
// for head-relative walking): while `on`, merged into pad 0 with a real pad's state: buttons or'd, each stick the one
// pushed further, each trigger the larger. Presenting thread.
struct PadState {
    uint16_t buttons = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    uint8_t lt = 0, rt = 0;
};
void set_source(bool on, const PadState& state);
// Round 1 (headset, check 16): with no VR controllers the gamepad stays the input. While `on`, pad 0's left stick is
// turned by `deg` (counter-clockwise seen from above: the head's yaw, for head-relative walking) and the right stick's
// x is kept for the pose service's turning (-1..1) and given to the game as 0 (the anchored camera is not the game's
// to turn).
void set_pad_turn(bool on, float deg);
float pad_right_x();
// Headset round 2: true once after both stick clicks (L3 + R3) were held on the real pad for a second.
bool take_recentre_combo();
uint64_t polls();           // XInputGetState calls seen
uint64_t injected();        // calls that returned injected state
uint64_t caps_queries();    // XInputGetCapabilities calls seen
uint32_t rumble();          // last motor speeds sent to pad 0 (left << 16 | right)
uint64_t rumble_changes();  // times those speeds changed

}  // namespace rdrvr::xinput
