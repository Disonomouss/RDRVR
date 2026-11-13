#pragma once

#include <cstdint>
#include <string>

// Dead Eye options (2026-10-26, the user: "Dead eye options needed. Choice between the unlocked dead eye levels and a
// toggle for the sepia filter"; research run 28):
// - [DeadEye] Level: 0 the game's (as unlocked), 1 slow motion only, 2 the marks painted as you sweep, 3 the marks
//   painted by the button. Never above the level the story has unlocked: the game keeps that in the player info
//   (+0x3bd multilock on: level 2 or more; +0x3be bit 1 the automatic paint: level 2, clear: level 3), written by the
//   scripts' SET_DEADEYE_MULTILOCK_ENABLE / SET_DEADEYE_TARGETPAINT_ENABLE. A wrapper on the Dead Eye state machine
//   (FUN_140348980, each frame on the game thread) puts the chosen level there for its call and the game's back after
//   it, so the scripts always read their own (the unlocked level is never lost). The level chosen is taken while Dead
//   Eye is off (a change applies at the next Dead Eye).
// - [DeadEye] Sepia (on): off, Dead Eye's post-process preset (FX_Deadeye.ppp) loses its colour: its colour correction
//   and saturation set to the default preset's (0.5 0.5 0.5 1, 1.0), the slow motion, the sounds and the rest kept.
//   The originals are kept and put back when it is turned on again; a reload of the presets from disk is noticed and
//   the colour taken out again.
namespace rdrvr::deadeye {

void init();     // the settings, the frame-end listener (the level latched while Dead Eye is off)
bool install();  // the wrapper on the Dead Eye state machine; the X's and the barrel paint's hooks when on at the start

int level();  // [DeadEye] Level, 0-3
void set_level(int l);  // also written to the user ini
int unlocked();  // the story's level as last read (0: not read yet)
bool sepia();
// (GOAL-after-0.8.13.2 item 6) [DeadEye] DuelTriggerFires: in a duel the right trigger fires the marks placed (the marks
// from the right stick flicked up, FlickUpRB); off by default
bool duel_trigger_fires();
void set_duel_trigger_fires(bool on, bool save = true);
// (round 138) [DeadEye] DuelNoTurn (off): while a duel runs the right stick does not turn the view; in_duel(): the game's
// duel mode on now (player info +0x3be bit 2: from the duel's Dead Eye's start to its end; read once a frame, pose's)
bool duel_no_turn();
void set_duel_no_turn(bool on, bool save = true);
bool in_duel();
void set_sepia(bool on);  // also written to the user ini

// Run 28 item 1c (after optimumbox's fork, v0.8.11.9, commit ac942427, MIT; research\run28\deadeye.md):
// - [Hands] DeadEyeMarks (off): the game draws each mark's X as an element of its "DeadeyeLocks" UI movie, placed where
//   the flat gameplay camera sees the mark, and runs that UI only while channel 0 holds the gameplay camera (in first
//   person it holds the anchor's), so no X ever showed. On: for the reticle UI's call the gameplay camera is on channel
//   0 (put back after it, if still ours); inside the marks' UI each X is shown and placed at a slot of its own in a grid
//   across the UI target's middle (the game still draws and animates it: light, then darker once locked), each mark's
//   world point kept; the presenting thread draws each slot into both eyes where its mark is (xr.cpp), and the floating
//   HUD and the monitor leave out the slots in use. DeadEyeMarkDegrees: an X's size in the eyes.
// - [Hands] DeadEyeBarrelPaint (on): a paint by the button (level 3) marks the actor on the barrel's ray, not the
//   game's own assisted pick from the flat camera's view.
// Each feature's hooks are installed only when it is on at the start ("off: nothing"); turned on later, it applies at
// the next start. Installed, a later off is each hook's cheap off path (the menu and the test channel).
constexpr int kMax = 27;  // the DeadeyeLocks movie's elements (0x1b, DeadeyeMarksUi's loop)
struct Mark {
    float world[3];  // the mark's world point
    float slot[2];   // its X's centre in the UI target (pixels)
};
bool marks_on();
bool marks_available();  // its hooks installed (on at the start, every anchor present)
bool marks_supported();  // every anchor it needs present on this build
void set_marks_on(bool on, bool save = true);  // save: also written to the user ini (the menu; the test channel's is the session's)
float mark_degrees();
void set_mark_degrees(float deg, bool save = true);
bool barrel_paint();
bool barrel_paint_available();  // its hook installed (on at the start)
bool barrel_paint_supported();
void set_barrel_paint(bool on, bool save = true);
// Presenting thread: the marks shown now (updated within 200 ms), the crop about each slot (UI pixels, square) and
// the X's size in the eyes (degrees across the crop). showing(): marks() would give some (the X's layer is wanted)
int marks(Mark* out, int max, float* crop_px, float* deg);
bool showing();
// The UI rect (x0 y0 x1 y1, UI pixels) of the slots the last redirected update placed X in, with its grid and shift:
// kept, however old, until the game places or hides those elements itself, so a stale X never shows on the HUD or the
// monitor; false when none
bool band(int out[4]);
// xr.cpp: where mark k's X was drawn in an eye (px py: pixels of that eye's half of the layer, -1 not drawn; u v: the
// same as fractions of the eye's view), for the status
void note_drawn(int k, int eye, float px, float py, float u, float v, uint32_t layer_w, uint32_t layer_h);
void note_layer();  // xr.cpp: the X's layer submitted this frame

// deadeye [level 0-3] [sepia on|off] [marks on|off] [barrel on|off] [deg <d>] [pitch <px>] [shift <x> <y>]: the
// session's settings, the state
std::string command(const std::string& line);

}  // namespace rdrvr::deadeye
