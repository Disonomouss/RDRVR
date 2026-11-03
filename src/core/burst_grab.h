#pragma once
// R4 flicker measurement: K consecutive frames of each eye's post output, a centre crop of the Post FXAA Target,
// recorded like the eye grab (bind tap on the recording thread, from the state the barrier watch saw in the same list,
// into readback buffers) and written once the last frame's list has been submitted. A test command, never on in play.
//   grabburst <prefix> <frames> [crop_w crop_h] [post|scene|vel] [pereye]
// The source (taken at binds of the post output: the first eye before the 2nd bind, the last at the UI bind): the post
// output (the default), the scene colour (FullScreenCopy, PostFx+0x10: the upscaler's input, render size) or the
// Velocity RT (PostFx+0x3f0, RG16F: the motion vectors). Those two are rewritten by every pass's scene, so without
// "pereye" both files hold the last pass's (the older tests' meaning: r12_armmv, r12_gunmv, r12_bodyghost, dlss_xr_slide).
// "pereye" (scene/vel; run 19, research\run19\velocity.md 1.3): the first eye copied at the 1st bind instead, which must
// fall in the first-eye post run (dual_pass's markers around first_eye_post), the last at the UI bind. A frame whose
// copy could not be made (the resource's last barrier not in the bind's list, the target made again, or bind 1 outside
// the first-eye post run) leaves its slot invalid and is reported "NOT MEASURED" in the result, never a stale copy.
// Writes "<prefix>-L.rdb" and "<prefix>-R.rdb" (double frames; a frame that was not double leaves its slot empty) or
// "<prefix>-M.rdb" (mono). File: uint32 "RDB1", crop width, height, DXGI format, eye (-1 mono, 0 left, 1 right), frames
// recorded, first frame (low 32 bits), bytes per row; then per frame a uint32 valid flag and the rows, top first.

#include <string>

namespace rdrvr::burst_grab {

void init();  // frame-end listener
std::string grab(const std::string& prefix, int frames, int crop_w, int crop_h, unsigned timeout_ms = 10000, int source = 0,
                 bool pereye = false);
// dual_pass, the render thread, around first_eye_post (begin, then end): markers telling the playback thread the
// first-eye post run is playing (only while a per-eye grab waits or records; an end always follows its begin)
void first_post_mark(bool begin);

}  // namespace rdrvr::burst_grab
