#pragma once
// The upscaler's alignment, measured (the DLSS judder study, 2026-10-08: in the headset "the world lags, then catches
// up"). K consecutive frames of the second eye of double frames, three resources copied at the same bind (the post
// output's depth-bound bind, after that eye's post run): the post output (the upscaler's result after the tonemap),
// FullScreenCopy (the upscaler's input, render size) and the Velocity RT (its motion vectors), with the jitter that
// eye's DLSS constants carried. tools/dlss_align.py then measures, frame by frame, where the output sits against its
// own input: the jitter apart when aligned, plus a share of the frame's motion when it trails. A test command, never
// on in play.
//   aligngrab <prefix> <frames> [crop_w crop_h]   (the crop in output pixels; the input's crop is the same field)
// writes "<prefix>-post.rdb", "<prefix>-scene.rdb", "<prefix>-vel.rdb" (tools/flicker.py's RDB1 format, eye 1 = the
// second pass's eye) and "<prefix>-jitter.txt" (per frame: index, valid, jitter x y in render pixels).

#include <string>

namespace rdrvr::align_grab {

void init();  // frame-end listener
std::string grab(const std::string& prefix, int frames, int crop_w, int crop_h, unsigned timeout_ms = 10000);

}  // namespace rdrvr::align_grab
