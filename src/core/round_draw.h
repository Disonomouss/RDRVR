#pragma once
// Run 5 item 1b: a round seen in the hand that holds it ([Reload] RoundInHand). The game has no live revolver or rifle
// round (research\run4\rounds.md), so the mod draws its own: a lathe mesh made in code (a rim, the brass case, the lead
// bullet; a shotgun shell's brass head and red hull), drawn into each eye image in the eye blit's own command list
// (xr_blit), at the drawn hand of the frame, with the frame's eye cameras and projections (camera_lever's scene pass).
// It is hidden where the game's scene is nearer: each eye's scene depth is copied inside the game's own CopyResource
// into "Depth Resolve" (the frame grabber's tap) into a buffer the mod owns, and the pixel shader compares the game's
// distance with the round's. No game resource changes state; nothing is written to the game.

#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace rdrvr::body {
struct BodyPoints;
}

namespace rdrvr::round_draw {

void init();  // bootstrap: the settings and the depth tap
bool enabled();
void set_enabled(bool on);  // also written to the user ini
int mode();             // [Reload] RoundInHand: 0 off, 1 the mod's round, 2 also the game's own shell for shotguns
void set_mode(int m);   // also written to the user ini
// Where the round sits in the hand (run 6 item 5; [Reload] RoundOffset, RoundAxis, RoundBrightness; the Reloading tab):
// moved from the pinch by `offset` (m, the hand's wrist axes), pointing along the fingers (RoundAxis 1 0 0), out of the
// hand (auto: from the wrist through the pinch) or along a custom axis (the wrist's axes), drawn `bright` times as bright.
struct Place {
    float offset[3];
    int axis_mode;  // 0 along the fingers, 1 out of the hand, 2 custom
    float axis[3];
    float bright;
};
Place place();
void set_place(const Place& p, bool save);  // save: also written to the user ini

// The render thread, at each eye's scene pass (camera_lever::scene_once): that pass's eye camera (rows right, up,
// back, position) and its tangents (l < 0 < r, d < 0 < u).
void note_pass(int eye, const float* cam, float l, float r, float u, float d);
// The holsters (each update): John's hand holding a round taken at the chest (0 left, 1 right; -1 none) and the gun in
// hand's family (audio::click_family: 1 revolvers and pistols, 2 rifles, 3 shotguns).
void set_held(int john_hand, int family);

// The presenting thread, in xr_blit::blit's list after both eye images are drawn (the list's allocator slot `slot`,
// whose previous use the GPU has passed): the round into each eye's swapchain image dst[e] (RENDER_TARGET, its RTV
// rtv[e]). Changes the list's state (root signature, heaps, pipeline, targets): nothing may be drawn after it without
// setting its own. [XR] EyeShape: vw x vh, the eye image at the swapchain image's origin (0: all of w x h), and dw x dh,
// the scene depth's content behind it (0: the whole depth target).
void record(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12Resource* const dst[2], const D3D12_CPU_DESCRIPTOR_HANDLE rtv[2],
            uint32_t w, uint32_t h, DXGI_FORMAT fmt, int slot, uint32_t vw = 0, uint32_t vh = 0, uint32_t dw = 0, uint32_t dh = 0);

// [Reload] ShowAmmo (off; run 6 item 6b): while a round could go in now (the holsters say), a row of `n` rounds of the
// gun's family (as set_held's) standing at the chest's ammo zone `at`, side by side along the body's right (world:
// the zone, the body's right, up and forward), drawn and hidden by the scene as the round in hand. family 0: none.
bool show_ammo();
void set_show_ammo(bool on, bool save = true);
void set_row(int family, int n, const float at[3], const float rgt[3], const float up[3], const float fwd[3]);
// The round's drawn pose for John's `hand` from the body's points: 12 floats, the axes X, Y (base to tip), Z, then the
// position (world). False when that hand's points are not valid.
bool pose(const body::BodyPoints& bp, int hand, float* out);

std::string command(const std::string& line);  // round [on|off] [offset x y z] [axis x y z] [grab <path.bmp>]
void status_text(char* out, size_t len);

}  // namespace rdrvr::round_draw
