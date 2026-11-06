#pragma once
// The cutscene log (2026-11-05, the user: "cutscenes have been low fps"). The headset logs of October showed 3D cutscenes
// at 45-63 Hz against 90 in play, with two signatures: on the other PC, Present itself blocking 9.4 ms a frame (60.1 Hz
// exactly); on the user's, the render thread waiting 6-10 ms a frame on the game's main thread, which was busy only
// about 1.5 ms. What holds them is not known; this logs, as "[cutlog]" lines:
//  - a cutscene starting and ending (CUTSCENE_MANAGER_IS_CUTSCENE_PLAYING, vr_mode's watch) with the view mode;
//  - each change of what the game asks of Present (its sync interval and flags) and of its own frame limit;
//  - in each of a session's first six cutscenes, 3 s in, the main thread's stacks ("stacks main", about 1 s), then the
//    playback thread's; and once in play (60 s of the stereo view) the main thread's, to compare.
// [Debug] CutsceneLog (on). One thread of its own, 4 Hz; the samples suspend the sampled thread only to copy its stack.

namespace rdrvr::cut_log {

void start();  // after frame_probe::install (the main thread's id)

}  // namespace rdrvr::cut_log
