#pragma once
// The real controllers through OpenXR actions (ROADMAP step 3). One action set "gameplay" with, per hand
// (/user/hand/left, /user/hand/right): the aim and grip poses, the trigger and grip values, the thumbstick and its
// click, the lower face button (A / X), the upper one (B / Y), the menu button and a haptic output. Bindings are
// suggested for Touch, Index, Vive, Windows Mixed Reality and the Khronos simple controller, and (their extensions
// enabled where the runtime has them) Pico 4 / Neo 3, HP Reverb G2, Vive Cosmos, Vive Focus 3, Touch Pro, Touch Plus
// and Samsung Odyssey; the runtime picks one. WMR and the Vive wands have no A/B: the trackpad's click gives them
// ([Controls] TrackpadButtons; its upper half B / Y, the lower A / X; the Vive's middle stays the stick click).
// Each XR frame the actions are synced and the hands located in the session's LOCAL space for the frame's predicted
// display time: they become hands::get()'s real source (the synthetic test source, when on, still wins). The game's
// rumble (XInputSetState on pad 0) drives the controllers' haptics: the left motor the left hand, the right the right.
// [Controls] Controllers=0 leaves the actions out.

#include <cstddef>
#include <cstdint>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <windows.h>
#include <d3d12.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace rdrvr::controllers {

// The interaction profiles' extensions xr.cpp enables when the runtime offers them (before xrCreateInstance).
inline constexpr const char* kProfileExtensions[] = {
    "XR_BD_controller_interaction",       "XR_EXT_hp_mixed_reality_controller", "XR_HTC_vive_cosmos_controller_interaction",
    "XR_HTC_vive_focus3_controller_interaction", "XR_FB_touch_controller_pro", "XR_META_touch_controller_plus",
    "XR_EXT_samsung_odyssey_controller"};
void note_extension(const char* name);  // xr.cpp: one it enabled
void note_api(XrVersion v);             // xr.cpp: the instance's API version

// The fit per controller type ([Controls] GripFit<Type> = "x y z pitch yaw roll": metres right / up / forward and
// degrees, in the controller's axes, the right hand's; the left hand's mirrored): how the controller sits in the hand.
// Both poses (aim and grip) of a hand move by it. h: 0 left, 1 right. fit_name: that hand's controller type (nullptr:
// none seen yet). Any thread.
const char* fit_name(int h);
bool fit(int h, float off[3], float ang[3]);
void set_fit(int h, const float off[3], const float ang[3], bool save);  // for that hand's type; save: the user ini

// Session thread, right after xrCreateSession (action sets attach once, before the session begins).
bool create(XrInstance inst, XrSession session);
// Once per XR frame, after xrBeginFrame (either frame loop): sync, locate the hands in `base` at `time`, haptics.
void sync(XrSpace base, XrTime time);
// XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: log the profile the runtime now uses.
void on_profile_changed();
// The late latch for the hands ([XR] LateLatchHands): render thread, at the scene's start, before the body's IK: the
// aim and grip poses located again for the same predicted display time (the buttons and values stay the sync's), so
// the hands, like the head's late latch, use the runtime's latest prediction rather than the one from the frame's start.
void relocate(XrSpace base, XrTime time);
// "controllers": the profile, both hands' state, syncs and haptic pulses.
void status_text(char* out, size_t len);
bool active();  // created, synced, and at least one hand tracked in the last sync
// A short vibration on one hand (0 left, 1 right), sent with the next sync unless the game's rumble is on. Any thread.
void pulse(int h, float amplitude, int ms);

}  // namespace rdrvr::controllers
