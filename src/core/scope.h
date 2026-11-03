#pragma once
// [Scope] (r55 item 11, GOAL-round52-fixes-and-scopes.md; the design and the user's answers: research/run29/scope.md 6-8):
// a scoped rifle brought to an eye makes that eye the scope eye (GOAL-scope-entry.md, 2026-10-31: the eye behind the
// eyepiece along the scope's axis within [Scope] EntryDepth, EntryRadius across it, looking along it within EntryAngle,
// for EntryHoldMs; Hysteresis to stay; the "[scope] near:" and "at a shot:" log lines measure it): its own scene pass is drawn from the scope's objective along the scope's axis (the gun's own frame, so
// a canted rifle cants the view) with the scope's true field (the apparent field over the magnification: realistic,
// [Scope] Power, or the game's sniper zoom, [Scope] GamePower), its projection layer fades to black, and its image is
// shown round on a quad layer at the eyepiece that only that eye sees (XrCompositionLayerQuad eyeVisibility); the other
// eye open or darkened ([Scope] OtherEye). Entering: the eye fades to black over [Scope] Fade (its normal view), then
// its camera turns to the scope's (the eye's TAA or DLSS history discarded) and the picture fades in; leaving the other
// way round. One hand is enough. Single pass draws the second eye from the first's work, so only the first view can be
// the scope's: the right eye there with [Scope] SinglePassSwap (on since GOAL-single-pass-default.md item 4: the eyes'
// order swapped while scoped, both eyes black 4 frames at each change; off, the right eye is refused in single pass).
// Binoculars ("scope binoculars on", test only): both eyes, two-pass only. Off by default.
//
// Threads: frame_begin and pass_camera on the render thread (SceneRender, a stereo frame with the eyes from the XR
// views); frame_layer on the presenting thread (the XR frame's end); command on the test channel's.

#include <cstdint>
#include <string>
#include <vector>

namespace rdrvr::scope {

void init();  // [Scope]'s keys (the start)
bool enabled();
void set_enabled(bool on, bool save);  // save: also written to the user ini (the menu)
bool game_zoom();                      // [Scope] Magnification: false realistic (Power), true the game's (GamePower)
void set_game_zoom(bool on, bool save);
bool other_dark();                     // [Scope] OtherEye: false open, true dark while scoped
void set_other_dark(bool on, bool save);

struct FrameIn {
    float cam[16];       // the frame's game camera as the passes draw it (rows right, up, back, position): world_to_local's
    float eye[2][16];    // each eye's camera this frame (the same rows; world)
    float tan[2][4];     // each eye's FOV tangents l r u d
    bool single_pass;    // only the first view may be the scope's ([Stereo] PrepassViews: the second eye from the first's work)
    int first;           // the first view's eye without the scope's own swap (1: the eyes' order swapped by the test's g_swap)
    uint64_t xr_frame;   // the XR frame the views are for (the layer's)
};
// The render thread, before a stereo frame's passes: the scope's state for this frame. Returns whether the scope wants
// the eyes' order swapped against the frame's own (single pass, the scope at the right eye, SinglePassSwap): the caller
// applies it to this frame's passes.
bool frame_begin(const FrameIn& in);
// The render thread: frame_begin is wanted this frame ([Scope] on, a test's force or binoculars, or the scope still in or
// changing): off and idle, nothing runs (a few loads)
bool wanted();
// The render thread, an eye's pass of that frame: true with the scope's camera for that eye (rows, place; the 4th
// column of each row taken from `cam` as given) and its tangents (l r u d: square, the scope's true field)
bool pass_camera(int eye, float cam[16], float tan[4]);

// The presenting thread, at XR frame xr_frame's end: what the scope adds. False: nothing (no fade, no picture).
struct Layer {
    float alpha[2] = {0.0f, 0.0f};     // each eye's picture's opacity (0: no quad)
    float eye_fade[2] = {0.0f, 0.0f};  // each eye's projection toward black
    float pos[2][3] = {};              // each picture's quad: its centre (LOCAL), orientation (x y z w), size (metres)
    float rot[2][4] = {};
    float size[2] = {0.0f, 0.0f};
    bool reticle = true;
    bool hide[2] = {false, false};     // the eye's other quads (the rings, the highlights, the reticles) kept out of it
    uint32_t image = 1024;             // the quads' image size ([Scope] ImageSize)
};
bool frame_layer(uint64_t xr_frame, Layer* out);
// the presenting thread, after the blit: the pictures drawn (alpha: the ones wanted), submitted; hard: a wanted picture's
// swapchain or pipeline cannot be made, which refuses the scope (picture_broken) until it is turned on again or "scope
// reset" (a blind eye otherwise; transient misses never count)
// soft: a wanted picture's image not acquired or its wait failed (not a timeout): 45 such frames in a row with none drawn
// leave the scope and keep it out for 2 s (then it may come in again); a drawn picture resets the count
void note_drawn(const bool drawn[2], const float alpha[2], bool layers, bool hard, bool soft);
bool picture_broken();

std::string command(const std::vector<std::string>& t);  // "scope ..." (the test channel; "scope" alone: the status)

}  // namespace rdrvr::scope
