#pragma once
// Run 7 item 4: each eye rendered in the headset's own shape ([XR] EyeShape, off; [XR] EyeScale, 1.0;
// research\run7\eye-shape.md 7, option b). The game's frame is 16:9 on a nearly square eye FOV; with this on, each eye
// is rendered at eh = min(H, recommended height x EyeScale) by ew = min(W, eh x the FOV's tan width / tan height),
// square pixels, inside the game's own W x H allocation (nothing is re-allocated):
//  - the render class (the targets DRS renders in a sub-region of: the scene depth, the shadow collector, the lighting,
//    the grass, the depth resolve, the HDR scene, FXAATarget, velocity and reactivity) gets the per-axis logical size
//    (round(s ew), round(s eh)) with the allocation (W, H), through the game's own size calls, issued at DRS's own point
//    (the DRS controller, once a frame on the render thread): the DrsSubregion hook calls the original with (W, H), then
//    the render-class calls again with the eye size (PostFxSetSizes, each RT's SetSize, the grass's), and writes the
//    renderer's render size (+0x554/+0x558);
//  - the post output's (Post FXAA Target) logical size is the eye size only inside each eye's post run (dual_pass's
//    PostRun hook: begin_run / end_run), so the UI pass after it still sees W x H: the UI quad and the wrist HUD are as
//    before;
//  - the XR side submits the top-left ew x eh of each eye image (the eye swapchains at that size when the mode is
//    configured at the session's start, the layer's imageRect, the round in hand's viewport), and the monitor shows the
//    left eye pillarboxed (repainted before the UI mirror).
// Wanted only with EyeShape on, the XR submission running, the double pass with the XR eyes (stereo or 3D cutscenes)
// and FXAA (technique 1; [Render] ForceAntiAliasing=1 at the session's start). Any failed check (an RT's vtable, a null
// owner, the game's size changed, a size out of range) is the kill switch: logged once, the game's uniform state back at
// the next DRS controller call, until "eyeshape on". With EyeShape off the hooks pass through after one flag check.

#include <cstddef>
#include <cstdint>
#include <string>

namespace rdrvr::eye_shape {

void init();     // bootstrap, before ui_layer::init: the settings, and the monitor repaint's frame end (before the UI mirror's)
bool install();  // with the RDR.exe hooks: DrsSubregion, DrsController

// The XR session thread, after xrGetSystem: the runtime's recommended eye image and the largest image it takes (the
// larger of the two eyes; min of maxSwapchainImage and the views' maxImageRect). The eye size never exceeds the latter.
void set_recommended(uint32_t rec_w, uint32_t rec_h, uint32_t max_w, uint32_t max_h);
// Is the mode configured for the session about to start ([XR] EyeShape on now, FXAA forced)? The presenting thread asks
// before make_swapchains, to locate the eyes' FOV first.
bool configured();
// The presenting thread, at make_swapchains: this session's eye size from the post output's W x H and the eyes' FOV
// tangents (tan[eye] = l r u d, l < 0 < r, d < 0 < u; nullptr: none located, the recommended rect's aspect is used).
// fw x fh: the frame's swapchain size (W x H, or W x H fitted into the runtime's largest image: xr's sizing). Returns
// the eye swapchains' size: the eye size when configured(), else fw x fh; a later "eyeshape on" sizes the eye in them.
void plan_session(uint32_t w, uint32_t h, uint32_t fw, uint32_t fh, const float (*tan)[4], uint32_t* sw, uint32_t* sh);
// xr's live-resize guard (the game's frame made again at another size): the kill switch, with the reason.
void stop(const char* why);
// The last post run's content: true when it was rendered in the eye shape, with the eye image's size (cw x ch: the
// top-left of the post output) and the scene's render size behind it (rw x rh). Any thread.
bool frame_rect(uint32_t* cw, uint32_t* ch, uint32_t* rw = nullptr, uint32_t* rh = nullptr);

// dual_pass's PostRun hook (the render thread), around the original: the post output's logical size is the eye size for
// this run, put back right after it.
struct RunPoke {
    void* rt = nullptr;
    uint16_t w = 0, h = 0;  // its logical size before the run
};
void begin_run(void* postfx, RunPoke* p);
void end_run(void* postfx, const RunPoke& p);

// [XR] EyeShape now; set: the menu (saved to the user ini) or the command (the session only). On re-arms the kill
// switch; turned on in a session begun without it, the eye rect is rendered into the W x H swapchains (the eye-shaped
// swapchains need it on at the session's start). Refused (false) when the DRS hooks are not installed.
bool enabled();
bool set_enabled(bool on, bool save);

// eyeshape [on|off|scale <x>|status]: the session only (the ini is not written)
std::string command(const std::string& line);
void status_text(char* out, size_t len);

}  // namespace rdrvr::eye_shape
