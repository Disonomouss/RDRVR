#pragma once
// R4 native TAA per eye (DESIGN 3.6; research\native-taa-study.md; ENGINE-NOTES item 7). The engine's technique 2 keeps
// the TAA bookkeeping (Halton jitter on the projection, NDCToPrevNDC and PrevViewProjection, the velocity pass) and
// ships the resolve shader (rdr2_taa.fxc, technique TAAResolve), but never draws it and makes no history. With
// [Render] ForceAntiAliasing=2 (or "aa taa") the core selects technique 2 and this module completes it:
//  - per scene pass, right after SceneRender's wait for the frame's data (PreRender's TAA update for the tick is done
//    then, and the viewport holds the pass's camera): a projection rebuilt without jitter, then, in a double frame,
//    the pass's TAA snapshot (PostFx +0x370..+0x37f, +0x430..+0x4af) in, the jitter index set to the tick's sample,
//    FUN_1408728f0 on the pass's viewport (one jitter, the pass's own NDCToPrevNDC and PrevViewProjection), snapshot
//    out; in a mono frame the tick's jitter is put back on the rebuilt projection;
//  - in each post run's anti-aliasing slot (FUN_140873e60, before bloom, exposure and tonemap): TAAResolve from
//    FullScreenCopy, the run's previous history, depth and velocity into its other history target, which is copied
//    to a scratch target the chain continues from (the post rain draws onto that, not into the history);
//  - one history pair per pass of a double frame (the first-eye run and RenderFrame's own run; a mono frame continues
//    the second), made once by the engine's render-target factory as FullScreenCopy is;
//  - the scene's rain draw is left to the post (once per eye, after the resolve, as for DLSS), and the post rain's TAA
//    call leaves the pass's TAA state as it found it.

#include <cstddef>
#include <cstdint>

namespace rdrvr::taa {

bool install();
bool active();                    // technique 2 applied and the targets made
void frame_start();               // render thread, before SceneRender: makes the targets once, when technique 2 is on
void after_scene_wait(int pass);  // render thread, SceneRender's frame-data wait: 0 mono, 1, 2
bool skip_scene_rain(uintptr_t ret);  // the rain draw returning to ret is SceneRender's: left to the post
bool set_params(const float v[4]);  // TAAParams: still weight, moving weight, velocity scale, sharpening ("taa params")
void set_shared(bool on);         // positive control: both eyes on one history and one TAA state ("taa shared on")
// A mono pass whose camera the lever changed (the truth references) gets the update a double pass gets, on its own
// snapshot, so it reprojects through the camera it draws with ("taa monopass on|off", default on). Off: the tick's
// jitter only, on PreRender's update for the game's camera (cycles 25-26: such a reference accumulates about a third
// as well at yaw presets other than 0).
void set_mono_pass(bool on);
void status_text(char* out, size_t len);

}  // namespace rdrvr::taa
