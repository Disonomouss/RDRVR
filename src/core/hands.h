#pragma once
// The controllers as the mod's features read them: a pose in the XR LOCAL space (the space the eye views are located
// in), the trigger and grip values, the face buttons, the menu button and the thumbstick.
// Sources: the OpenXR actions (the real controllers; controllers.cpp), or, with [Debug] SyntheticHands=1, a test-only synthetic
// source scripted through the command channel ("hand l|r <x y z> <qx qy qz qw> <trigger> <grip> <buttons> <sx sy>",
// buttons a bit mask: 1 A/X, 2 B/Y, 4 menu, 8 stick click), because the simulator maps only mouse and keyboard to
// controllers. The synthetic source is never on in a release.

#include <cstdint>
#include <string>

namespace rdrvr::hands {

enum Button : uint32_t { kA = 1, kB = 2, kMenu = 4, kStick = 8 };

struct Hand {
    bool valid = false;           // the aim pose is tracked
    bool connected = false;       // the controller reports input (it may be untracked)
    float pos[3] = {};
    float rot[4] = {0, 0, 0, 1};  // x y z w; the hand points along its -z (the aim pose)
    bool grip_valid = false;      // the grip pose (the palm; for the hand IK later)
    float grip_pos[3] = {};
    float grip_rot[4] = {0, 0, 0, 1};
    float trigger = 0, grip = 0;
    uint32_t buttons = 0;
    float stick[2] = {};
    uint64_t updates = 0;
};

void init();
bool synthetic();
Hand get(int hand);       // 0 left, 1 right: the synthetic source when on, else the controllers
Hand get_real(int hand);  // the controllers only
void set_real(int hand, const Hand& h);  // controllers.cpp, each XR frame
// The synthetic source's command; returns the result line.
std::string command(const std::string& line);

}  // namespace rdrvr::hands
