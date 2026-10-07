#pragma once
// lumcheck, R4a's direct invariant test (test command "lumcheck [frames]"). For a run of frames, at each of the
// game's FXAA binds of the Post FXAA Target (one per post run: one per eye in a double frame, after that run's
// tonemap), the mod copies each adapted luminance target (Adapted Lum A/B, 1x1 R32F) whose last transition the barrier
// watch saw in the same list, and reads the copies back. With one exposure writer (split exposure) both eyes' tonemaps
// read one value, so the two snapshots of a double frame are bit-identical; with a writer per eye they differ
// whenever the eyes' views differ (the positive control).

#include <string>

namespace rdrvr::lum_check {

void init();
std::string run(int frames, unsigned timeout_ms = 8000);

}  // namespace rdrvr::lum_check
