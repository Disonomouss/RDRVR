#pragma once
// R5 UI offscreen layer (DESIGN R5: "UI never in the eye images"; the UI on its own quad). The game draws its UI last,
// onto the post chain's output (Post FXAA Target, or FXAATarget under TAA) with its own cleared depth buffer bound
// (frame graph: [Post FXAA Target | DS] ~150 draws, then the back-buffer gamma blit). With the redirect on, every bind
// of that target WITH a depth view gets the render-target view of a mod-owned RGBA16F target instead, cleared to
// transparent at the frame's first such bind, so the UI lands there alone. Only a CPU descriptor handle changes in the
// game's call; the mod's target stays in RENDER_TARGET except inside the mod's own frame-end list.
// Step 1 (this file): the redirect behind a test switch ("ui redirect on|off", default off) and a capture of the UI
// target ("grabui <path>", RDE1 like the eye grab) to learn how the UI's blending leaves alpha.

#include <d3d12.h>
#include <cstdint>
#include <string>

namespace rdrvr::ui_layer {

void init();  // installs the RTV substitution and a frame-end listener
void set_redirect(bool on);
std::string grab(const std::string& path, unsigned timeout_ms = 3000);
void status_text(char* out, size_t len);
// Step 2. While the redirect is on, each frame's UI is also drawn back onto the monitor's back buffer (blended in
// display space, the game's Gamma applied), so the desktop keeps its HUD. For the XR quad layer (presenting thread,
// xr.cpp's frame end): whether this frame drew any UI, the UI target's size, and the draw into an acquired sRGB quad
// image (linear premultiplied, coverage = sqrt(stored alpha)).
bool has_ui();
void size(uint32_t* w, uint32_t* h);
// The whole UI into `dst`, without the UI pixels in `hole` (x0 y0 x1 y1; the wrist HUD's part) when given.
bool draw_quad(ID3D12Resource* dst, DXGI_FORMAT fmt, const int hole[4] = nullptr);
// The wrist HUD: the UI from pixel (x0, y0) on, the size of `dst`.
bool draw_crop(ID3D12Resource* dst, DXGI_FORMAT fmt, int x0, int y0);

}  // namespace rdrvr::ui_layer
