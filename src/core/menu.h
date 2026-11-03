#pragma once
// G-A in-headset menu (DESIGN 3.10): Dear ImGui on an OpenXR quad, opened with the menu button (or "menu on|off"),
// driven by a laser from the right hand (the trigger clicks). Tabs General, Comfort, Screen and Debug; every setting
// applies live and is written to the user ini (config::set). Rendered at frame end with the mod's own list into a
// UNORM target, then converted into an sRGB quad image (ImGui's colours are sRGB values). The quad is placed 1.2 m
// ahead of the head when the menu opens and stays world-locked.

#include <cstdint>
#include <string>

#include <openxr/openxr.h>

namespace rdrvr::menu {

void init();
bool visible();
void set_visible(bool on);
// Presenting thread, XR frame end: polls the menu button, draws the menu if it is open, and fills `quad` (true when
// the layer should be submitted). `views` are this frame's located views (the head pose for the placement).
bool frame(const XrView* views, XrSession session, XrSpace space, XrCompositionLayerQuad* quad);
// 2026-10-29 (the user: "Mod menu isn't visible in recordings"; research/run28/desktop_menu.md): the menu reaches the
// headset as its own quad layer, so the desktop window (the game's back buffer, a mirror of one eye) never had it.
// Presenting thread, xr.cpp's frame end, after frame() submitted the quad and after xr::image_written() (the present
// queue then waits for the menu's draw on the image queue): the menu's own image (its UNORM target, the pointer
// included; never the released quad image) drawn over the back buffer about to be presented, on the present queue,
// where the quad sits in the mirrored eye's view: the quad's corners projected through views[eye]'s pose and FOV onto
// the eye image's place in the back buffer (all of it; with content_w x content_h given, that size fitted and centred
// in it: [XR] EyeShape's repaint), perspective-correct, cut at the image's edges as the headset cuts it; not drawn when
// a corner is behind the eye. flat (the cinema: the desktop is the flat game, no eye): centred, 60% of the window's
// height. Nothing the headset sees is touched: the eye images and the cinema's copy are made from other textures or
// earlier in the frame. Returns whether it drew.
bool desktop(const XrView* views, int eye, uint32_t content_w, uint32_t content_h, bool flat);
// The test channel's "menu desktop on|off" (the session only; on by default): the desktop copy drawn or not.
void set_desktop(bool on);
// Test aid: the LOCAL-space point at the centre of the widget labelled `label` in the last drawn frame
// ("menu find <label>" -> "x y z"), so a synthetic hand can be aimed at it.
std::string find(const std::string& label);
// The test channel's "menu scroll <y>": the page scrolled to y (pixels) at its next draw, for a row below its bottom
void scroll(float y);
// The test channel's "menu grab <path.bmp>" (run 7 item 5): the menu's image at its next draw, written as a 24-bit BMP
// (1024x768, over black); "wrote <path> ..." or "ERROR ... (NOT MEASURED)". Waits up to 3 s for a drawn menu frame.
std::string grab(const std::string& path);
void status_text(char* out, size_t len);

}  // namespace rdrvr::menu
