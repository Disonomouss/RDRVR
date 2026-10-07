#pragma once
// Exact-pixel captures for R1/R3's gates ("zero offset is pixel-identical", dense disparity): at the next Present
// the current back buffer is copied to a readback buffer on the present queue and written as a 32-bit BMP. The
// presenting thread waits for that one copy, so a grab costs a frame; it is a test command, never on in play.

#include <string>

namespace rdrvr::frame_grab {

void init();                                       // frame-end listener
// Grabs the next presented frame to `path` (relative paths are in the game folder). Blocks up to timeout_ms;
// returns a one-line result ("wrote <path> 2560x1440 ..." or "ERROR ...").
std::string grab(const std::string& path, unsigned timeout_ms = 3000);
// The scene depth of the next frame (copied from the game's own "Depth Resolve" copy), as "RDZ1" + width, height,
// format + raw 32-bit texels. Absolute path.
std::string grab_depth(const std::string& path, unsigned timeout_ms = 3000);

}  // namespace rdrvr::frame_grab
