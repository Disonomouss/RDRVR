#pragma once

#include <openxr/openxr.h>

#include <cstddef>
#include <string>

// The holsters (and the foregrip, the loading point and each hand's test point) drawn as rings in the headset
// ([Holsters] ShowZones, the menu's Holsters tab), to place and size them by eye. Each marker is one OpenXR quad layer
// in the views' space, at the marker's world point carried back through the frame's camera
// (camera_lever::world_to_local), facing the eyes, as wide as its radius. The styles are cells of one small atlas,
// made on the CPU at boot and copied into a swapchain image once; the runtime keeps composing the image last released.
// No game memory, no natives, no hooks.
namespace rdrvr::zone_rings {

void init();  // the bootstrap thread: the atlas pixels
// Presenting thread, xr.cpp's frame end in a stereo frame with the projection layer: up to `max` quads for
// holster::markers(), in `space`, facing the centre eye of `views`. Returns how many it filled.
int frame(const XrView* views, XrSession session, XrSpace space, XrCompositionLayerQuad* out, int max);
// The reticle ([Hands] Reticle, aim::reticle_target): one quad at the shot's landing point, facing the centre eye, a
// constant angular size ([Hands] ReticleStyle: a ring and a centre dot, or a dot only); white, red over an actor.
// Returns whether it filled `out`.
bool reticle_frame(const XrView* views, XrSession session, XrSpace space, XrCompositionLayerQuad* out);
void status_text(char* out, size_t len);
// Test aid ("rings atlas <path.bmp>"): the atlas's pixels (sRGB, premultiplied) as a 32-bit BMP.
std::string write_atlas(const std::string& path);

}  // namespace rdrvr::zone_rings
