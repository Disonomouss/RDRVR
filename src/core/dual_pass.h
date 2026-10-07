#pragma once
// R3 dual eye pass (ENGINE-NOTES item 6): what a second SceneRender call in the same frame needs so that both passes
// see the frame the way a single pass does, and the first pass's own post-processing run. The camera lever's
// SceneRender hook drives it (begin_frame, pass 1, between_passes, pass 2, end_frame).
//  - SceneRender's two semaphores: pass 1 waits but does not release, pass 2 releases but does not wait (D13).
//  - Split state, runtime switches (`split <name> on|off`, all on by default):
//      grass  the grass composite zeroes the frame's batch counts after pass 1; they are kept for pass 2
//      gust   the grass wind's prev<-cur shift would run twice; restored before pass 2
//      lights animated-light phases and fades advance per call; updated once (skipped in pass 2)
//      forest the forest frame update feeds drained GPU timers into an adaptive factor; it runs in both passes (it
//             also refills the tree draws' timer pool) and pass 2's factor is put back
//      post   the post chain also runs for pass 1's eye, between the passes, with RenderFrame's sequence and a
//             snapshot/restore of the state a run advances; RenderFrame's wait for the main thread's UI data
//             (renderer+0x38, released before the main thread waits for the scene) is taken there instead
//  - exposure (switch "exposure", R4a): one exposure writer. The first eye's post run adapts the exposure and keeps
//    it; the second eye's run tonemaps with that same adapted value (its adaptation pass skipped, its luminance ring
//    step undone), so the exposure advances once per frame and both eyes share it.
//  - rain (switch "rain", R4b): GPU rain is stepped in pass 1 only, and the rain draw's camera-velocity smoother is
//    put back before every later rain draw of the frame, so both eyes draw one particle state with one streak.
//  - pfxmap (switch "pfxmap", R4b): the particle collision map (a top-down depth view) is rendered in pass 1 only.
//  - damage (switch "damage", R4): the ped damage manager integrates wetness, blood soak and splats by dt and draws
//    into its damage targets in every SceneRender call; it runs in pass 1 only (once per frame, as in vanilla).
//  - clouds (switch "clouds", R4): the cloud-shadow scroll (ScrollXZ) steps in every SceneRender call (the shadow
//    collector, the particle map); pass 2 calls it with its no-advance flag, so both eyes get one cloud pattern.
//  - sunvis (switch "sunvis", R4): two sun-visibility averages a bucket callback smooths per pass are put back
//    before pass 2.
//  - godrays (switch "godrays", R4): the sun's god rays blend with a history (an accumulation pair, a toggle and the
//    previous view-projection); the first eye gets a pair of its own, made once by the engine's creator, so neither eye
//    blends the other's rays.
//  - hold (switch "hold", off by default; a measurement aid): every post run's exposure step is undone, so a truth
//    capture's references and the double pair share one exposure state. Hold wins over exposure.
//  - masks (switch "masks"; [Compat] DrawMaskInit=0 leaves the hook out): FUN_140706050 builds its bucket masks from
//    uninitialised stack (zeroed only when a TLS slot is null, which it is not on the render thread), so the set of
//    buckets drawn depends on stack history: a vanilla bug that makes two passes differ. An entry stub
//    (draw_masks.asm) zeroes the four slots first, as a fresh stack would; the game's code is not patched.

#include <cstddef>
#include <cstdint>

namespace rdrvr::dual_pass {

bool install();
int pass();                                          // this thread's pass of a double frame: 0 none, 1, 2
int post_slot();  // the post run on this thread: 0 the first-eye run, 1 RenderFrame's run of a double frame, 2 mono
void begin_frame(void* renderer);                    // before pass 1
void between_passes(void* renderer, int first_eye);  // after pass 1 (its viewport still current), before pass 2
void end_frame(int second_eye);                      // after pass 2
void mono_frame();                                   // instead of the three above, in a single-pass frame
bool set_split(const char* name, bool on);
// Headset round 2: the rain and blood drops on the lens in both eyes ([Screen] LensDrops; off = in neither).
void set_lens_drops(bool on);
// Headset rounds 1-2: the god rays are skipped for an eye the sun is behind ([Stereo] SunBehindCheck).
void set_sun_behind_check(bool on);
void sun_status(char* out, size_t len);
void lens_drops_status(char* out, size_t len);           // false if the name is unknown
void arm_drawlog();                                  // log each DrawVisList call of the next double frame
void status_text(char* out, size_t len);
// Double frames by the number of CalculateAdaptedLuminance passes that ran in them (0, 1, 2, 3 or more): the CPU side
// of R4a's invariant (one writer: all in [1]).
void adapt_histogram(uint64_t out[4]);

}  // namespace rdrvr::dual_pass
