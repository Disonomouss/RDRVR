#pragma once
// R3/R4 per-eye captures of the post output: the Post FXAA Target (tonemapped and anti-aliased, before the screen
// effects, the UI and the final gamma blit). The copies are recorded on the engine's recording thread, at the game's own
// binds of that target (d3d::set_bind_tap), from the state its last barrier in that list left it in (never a guessed
// state), into readback buffers; the files are written once the list has been submitted. A test command, never on in
// play.

#include <string>

namespace rdrvr::eye_grab {

void init();  // frame-end listener

// Captures the next full bind sequence of the target. A double frame with the first-eye post run gives
// "<prefix>-L.rde" and "<prefix>-R.rde", a single post run "<prefix>-M.rde". File: uint32 "RDE1", width, height, DXGI
// format, eye (-1 mono, 0 left, 1 right), frame (low 32 bits), bytes per row, 0; then the rows, top first. Absolute
// prefix. Blocks up to timeout_ms; an eye that could not be captured is reported as NOT MEASURED with the reason.
std::string grab(const std::string& prefix, unsigned timeout_ms = 4000);

}  // namespace rdrvr::eye_grab
