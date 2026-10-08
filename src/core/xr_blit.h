#pragma once
// R5 colour conversion for the eye images (research\r5-plan-notes.md). The post chain's output (Post FXAA Target, or
// FXAATarget under TAA: RGBA16F) is what the game's last pass, Apply360GammaCorrect (FUN_140869930), turns into the
// back buffer with out = pow(rgb, Gamma), Gamma = PostFx+0x88c. The eye images are copied at the game's binds into two
// staging textures owned by the mod (a plain copy: no descriptor heap is bound on the game's lists); at frame end, on
// the present queue with the mod's own list and heaps, each staging texture is drawn into the runtime's sRGB swapchain
// image as srgb_to_linear(pow(rgb, Gamma)), so the stored sRGB bytes equal the bytes the game would present.

#include <d3d12.h>
#include <cstdint>

namespace rdrvr::xr_blit {

// The swapchain format for the eye images: R8G8B8A8_UNORM_SRGB or B8G8R8A8_UNORM_SRGB, whichever the runtime offers
// (DXGI_FORMAT_UNKNOWN if neither).
DXGI_FORMAT pick_format(const int64_t* formats, uint32_t count);
// Pipeline and staging textures for w x h images of `src_format`, drawing into `dst_format`. Presenting thread.
bool init(ID3D12Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT src_format, DXGI_FORMAT dst_format);
// Staging textures, kept in PIXEL_SHADER_RESOURCE between uses: [0] and [1] the eyes as copied at the UI bind (or both
// at once in a mono frame), [2..4] the post output at a double frame's 2nd..4th bind (round 2: when the screen overlays
// run for the first eye too, its final image is at the bind that starts the second eye's post run, which the frame's
// bind count only tells at the UI bind).
constexpr int kStages = 5;
ID3D12Resource* stage(int i);
void set_source(int eye, int stage_index);  // which staging texture this frame's eye is drawn from
// [XR] EyeShape (eye_shape.h): the eye swapchains' size when it is not the staging textures' (init's w x h).
void set_dst_size(uint32_t w, uint32_t h);
// [XR] EyeShape: this frame's eye images. cw x ch: the content, the top-left of each staging texture (the whole of it
// without the eye shape); iw x ih: the image drawn at the swapchain image's origin (the layer's imageRect). Equal sizes
// are copied texel for texel; else the content is resampled into the image (the game's 16:9 frame in an eye-shaped
// swapchain, while the shape is not applied; or a frame larger than the runtime's largest image, [Render]
// RenderResolution): one bilinear tap per pixel up to 2 content texels a pixel (it reads every texel), more over the
// pixel's footprint above that. dw x dh: the scene depth's content behind the image (the round in hand; 0: the whole
// depth target).
struct Frame {
    uint32_t cw, ch, iw, ih, dw, dh;
};
// Draws both staging textures into dst[0], dst[1] (in RENDER_TARGET, as acquired) and submits on `queue`. A list's
// allocator is reused only after the GPU has passed it (fence); if it has not within 50 ms the frame is skipped.
// frame: nullptr = the whole staging texture into the whole image (the default, without EyeShape).
bool blit(ID3D12CommandQueue* queue, ID3D12Resource* const dst[2], float gamma, const Frame* frame = nullptr);
// [XR] EyeShape: the monitor's back buffer `bb` (in PRESENT) repainted with the left eye's content (the top-left cw x ch
// of its staging texture) scaled to fit and centred (black beside it), in the game's final gamma. The presenting thread,
// at the frame end, before the UI mirror draws over it.
bool repaint(ID3D12CommandQueue* queue, ID3D12Resource* bb, uint32_t cw, uint32_t ch, float gamma);
// The cinema (xr.cpp, R5 step 3), when its quad image is not the back buffer's size and format (a CopyResource needs
// both: a frame larger than [XR] CinemaMaxWidth or the runtime's largest image, or the game's frame resized while it
// runs): the back buffer `bb` (in PRESENT) drawn filtered into the top-left w x h of the quad image `dst` (in
// RENDER_TARGET, as acquired; format `dst_format`). The buffers may take no shader view (the swapchain's usage), so the
// back buffer is first copied into a texture of its own size and format, made again when either changes; read through
// an sRGB view into an sRGB image, so the filter averages light, as the runtime reads the copied bytes. Its own lists
// and fence on `queue`; the presenting thread, at the frame end.
bool cinema_resample(ID3D12Device* dev, ID3D12CommandQueue* queue, ID3D12Resource* bb, ID3D12Resource* dst, DXGI_FORMAT dst_format,
                     uint32_t w, uint32_t h);
// The cinema's resamples drawn, and the last one's size in the quad image (0 x 0: none yet).
void cinema_status(uint64_t* resamples, uint32_t* w, uint32_t* h);
// The Gamma the game's back-buffer blit reads (PostFx+0x88c), or `def` without a PostFx object.
float game_gamma(float def);
// Positive control: a fixed Gamma instead of the game's ("xr gamma <value>"); 0 = the game's again ("xr gamma live").
void set_gamma_override(float g);
void status_text(char* out, size_t len);

}  // namespace rdrvr::xr_blit
