#pragma once
// G-A in-headset menu (DESIGN 3.10): Dear ImGui on an OpenXR quad, opened with the menu button (or "menu on|off"),
// driven by a laser from the right hand (the trigger clicks). Tabs General, Comfort, Screen and Debug; every setting
// applies live and is written to the user ini (config::set). Rendered at frame end with the mod's own list into a
// UNORM target, then converted into an sRGB quad image (ImGui's colours are sRGB values). The quad is placed 1.2 m
// ahead of the head when the menu opens and stays world-locked.

#include <string>

#include <openxr/openxr.h>

namespace rdrvr::menu {

void init();
bool visible();
void set_visible(bool on);
// Presenting thread, XR frame end: polls the menu button, draws the menu if it is open, and fills `quad` (true when
// the layer should be submitted). `views` are this frame's located views (the head pose for the placement).
bool frame(const XrView* views, XrSession session, XrSpace space, XrCompositionLayerQuad* quad);
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
