#pragma once
// Bring-up render settings forced in memory, never in graphicsOptions.xml (DESIGN §3.6; the game rewrites that file on
// every boot from its options struct 0x1422ca340, which this module never touches):
//   [Render] ForceAntiAliasing = -1 game's own | 0 Off | 1 FXAA   (FXAA/Off have no jitter: frames compare pixel for pixel)
//                                | 2 native TAA | 3 DLSS (the game's, technique 5; boot only; [Render] DlssQuality 0..5, 0 = DLAA)
//   [Debug]  FixedRenderScale  = 0 off | 0.5..1.0                  (the DRS controller's fixed-scale path, for S10)
// The AA/upscaler/DRS setter 0x1405cfad0 (every option setter and ApplyAll call it) gets a copy of its 6-int argument
// {mode, frame gen, DLSS quality, FSR quality, sharpening, DRS} with mode, frame gen and DRS forced; the user's values
// stay in the options and runtime structs. Per frame, PostFx+0x86c (the technique the render thread applies) is kept
// at the forced technique, because frames before ApplyAll default to FSR.

#include <cstddef>

namespace rdrvr::render_settings {

bool install();
bool set_fixed_scale(float scale);  // 0 = off; needs ForceAntiAliasing >= 0
void status_text(char* out, size_t len);

// 0 Off, 1 FXAA, 2 native TAA (technique 2 with the mod's resolve); false unless the core forces the AA mode.
bool set_aa(int mode);
int forced_aa();
// [Render] DlssFullJitter: DLSS's jitter sequence at 8 x (output / render width)^2 samples (the game leaves DLAA's 8)
void set_full_jitter(bool on);
void jitter_text(char* out, size_t len);  // "dlss jitter: ..."
// run 10: [Render] MotionBlur off|game (forced off in memory: PostFx +0x5a4/+0xe1c each frame) and HeatHaze 1|0 (the
// HeatShimmer strength zeroed at the Mirager pass)
bool motion_blur_off();
void set_motion_blur_off(bool off, bool save);
bool heat_haze_on();
void set_heat_haze(bool on, bool save);
void post_text(char* out, size_t len);  // "postfx: ..."
void post_test(const char* what, float v);  // tests: "style" 0..2 (the game's motion blur style set), "hazetest" <strength> (-1 off)
int dlss_quality();  // the DLSS quality index in effect since boot (0..5)  // the AA mode forced since boot or the last set_aa (-1 none, 0..3)

}  // namespace rdrvr::render_settings
