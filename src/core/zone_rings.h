#pragma once

#include <openxr/openxr.h>

#include <cstddef>
#include <string>

#include "core/holster.h"

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
bool reticle_frame(const XrView* views, XrSession session, XrSpace space, XrCompositionLayerQuad* out, int which = 0);  // which 1: the second gun's
void status_text(char* out, size_t len);
// [Holsters] InteractionHighlights (2026-10-29; holster.h): the presenting thread, before the rings: up to `max` quads,
// a soft glow at each of holster::highlights()' places as strong as its strength (eased in over 0.12 s, out over 0.2 s),
// facing the centre eye, from the highlights' own atlas (made the first time one is drawn). With the mode rings: 0,
// nothing made or drawn. Returns how many it filled.
int highlights_frame(const XrView* views, XrSession session, XrSpace space, XrCompositionLayerQuad* out, int max);
void highlights_status(char* out, size_t len);  // the layers, each place's drawn frames, the last frame's (each eye's u v)
// (run 29 item 2, [Holsters] InteractionHighlights=parts) each place as last drawn: its eased strength (target), its last place
// (world) and size, with the camera they are in; false when not drawn within 100 ms (the mode rings, out of first person).
// project_world: where eye 0 or 1 of the last frame sees a world point (the frame camera cam), 0-1 across its view (the tests'
// masks); false behind it or before a frame. Any thread (a mutex)
bool highlights_shown(holster::Highlight out[holster::kHlCount], float cam[16]);
bool project_world(const float cam[16], const float w[3], int eye, float uv[2]);
// Test aid ("rings atlas <path.bmp>"): the atlas's pixels (sRGB, premultiplied) as a 32-bit BMP.
std::string write_atlas(const std::string& path);

}  // namespace rdrvr::zone_rings
