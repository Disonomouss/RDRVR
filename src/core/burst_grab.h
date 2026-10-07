#pragma once
// R4 flicker measurement: K consecutive frames of each eye's post output, a centre crop of the Post FXAA Target,
// recorded like the eye grab (bind tap on the recording thread, from the state the barrier watch saw in the same list,
// into readback buffers) and written once the last frame's list has been submitted. A test command, never on in play.
//   grabburst <prefix> <frames> [crop_w crop_h]
// writes "<prefix>-L.rdb" and "<prefix>-R.rdb" (double frames; a frame that was not double leaves its slot empty) or
// "<prefix>-M.rdb" (mono). File: uint32 "RDB1", crop width, height, DXGI format, eye (-1 mono, 0 left, 1 right), frames
// recorded, first frame (low 32 bits), bytes per row; then per frame a uint32 valid flag and the rows, top first.

#include <string>

namespace rdrvr::burst_grab {

void init();  // frame-end listener
std::string grab(const std::string& prefix, int frames, int crop_w, int crop_h, unsigned timeout_ms = 10000);

}  // namespace rdrvr::burst_grab
