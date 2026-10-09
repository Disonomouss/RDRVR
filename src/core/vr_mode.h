#pragma once
// R5 view modes (DESIGN 3.7). While the OpenXR submission runs and [XR] AutoMode is on, each frame end picks one:
//  - stereo: both eye passes from the located XR views (the double pass, per-eye projections and cameras);
//  - cinema: the game's own single view, frame and UI, on the 3.2 m screen. For flat sources: a frame without a
//    SceneRender call (loading screens, bink, the front end), the game paused (IS_GAME_PAUSED, or script ticks stalled
//    while frames go on: a pause from focus loss), HUD_IS_FADED; and cutscenes when [Screen] CutsceneMode = Screen;
//  - cutscene 3D: stereo from the cutscene camera, the head's rotation only (no translation), the eye separation scaled
//    by [Screen] Cutscene3DSeparation; when CutsceneMode = 3D.
// The cutscene state is CUTSCENE_MANAGER_IS_CUTSCENE_PLAYING, polled every script tick. Entering the cinema takes one
// frame of a trigger (two without a scene), leaving it ten clear frames, so a single odd frame does not flip the view.
// "mode cutscene screen|3d" switches live, mid-cutscene included; "mode auto off" leaves the lever to the test commands.

#include <cstddef>

namespace rdrvr::vr_mode {

void init();  // frame-end listener (before the XR frame end) and the watched natives
void set_auto(bool on);
void set_cutscene_3d(bool on);
void force_cutscene(int state);
void set_separation(float s);  // 3D cutscenes' eye separation (fraction of the IPD), applied at once  // test: -1 the native decides, 0 no cutscene, 1 a cutscene ("mode force cutscene ...")
bool gameplay_stereo();
int state_flags();  // the last frame's state bits: 1 no scene, 2 paused, 4 script stalled, 8 faded, 16 cutscene, 32 resized  // the automatic modes run and the view is gameplay stereo (not cinema, not a 3D cutscene)
void status_text(char* out, size_t len);

}  // namespace rdrvr::vr_mode
