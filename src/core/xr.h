#pragma once
// OpenXR (DESIGN §3.8). R0: an idle session on the game's own D3D12 device and present queue, running a frame loop
// with no layers on its own thread, so the session reaches FOCUSED and the runtime path is proven end to end.
// R5 step 1 ([XR] Submit=1): one XR frame per game frame, driven from the game's frame end (the presenting thread,
// before its Present): release the eye images and xrEndFrame with a projection layer of the views the frame was
// rendered for, then xrWaitFrame (which paces the game), xrBeginFrame, xrLocateViews for the next frame, and acquire
// its images. The images are filled on the engine's recording thread at the game's binds of the Post FXAA Target (the
// eye grab's sequence): the first pass's eye before its second FXAA bind after a UI bind, the second pass's before the
// UI bind, or one image for both eyes after a single post run (mono). Same-format copies (RGBA16F): colour conversion
// to the runtime's linear space comes later.
// The runtime is chosen per process: RDRVR_xr_runtime.txt next to RDR.exe names a runtime manifest, set as
// XR_RUNTIME_JSON in-process (Steam and the Rockstar launcher do not pass environment variables through).

#include <cstdint>

namespace rdrvr::xr {

struct EyeView {
    float fov[4];          // XrFovf angles in radians: left (<0), right, up, down (<0)
    float orientation[4];  // x, y, z, w in the LOCAL reference space
    float position[3];     // metres in the LOCAL reference space
};

void init();                      // registers a frame listener that starts the session once D3D12 is up
// The latest xrLocateViews result (left, right) for the predicted display time; false until views are valid.
bool eye_views(EyeView out[2], uint64_t* xr_frame = nullptr);
// The wrist HUD shown this frame (looked at, the palm flat; [XR] Hud=wrist). Any thread.
bool wrist_hud_shown();
// The same views without counting them as drawn with (the pose check): for the visibility cover.
bool eye_views_peek(EyeView out[2]);
void rings_status(char* out, size_t len);  // the holster rings' layers and the runtime's layer limit
const char* session_state();      // "none", "idle", "ready", "synchronized", "visible", "focused", "stopping", ...
uint64_t frames();                // xrEndFrame calls
const char* runtime_name();
// R5: frames submitted, copies, misses, ..., then " | cap WxH | ui quad WxH of WxH, wrist WxH of WxH | cinema quad WxH,
// shown WxH (copies n, resampled n) | resized 0|1 (what)": the runtime's largest image, each quad's swapchain against
// its source, and the live-resize guard (frame_resized). 1024 characters hold it.
void submit_status(char* out, size_t len);
bool submitting();  // the frame loop runs on the presenting thread with the eye swapchains made
// R5 late latch (DESIGN 3.8, [XR] LateLatch): render thread, just before a double frame's first eye pass: the views
// are located again for the same predicted display time, and their new orientations (positions kept from the frame's
// start) become the frame's views, both for the eye cameras and for the projection layer.
void late_latch();
// The hands' late latch ([XR] LateLatchHands): render thread, at the scene's start, before the body's IK
// (controllers::relocate for the frame's predicted display time).
void latch_hands();
void set_latch_hands(bool on);  // the session only ("controllers latch on|off")
bool latch_hands_on();
// Recentre (DESIGN 3.7): the cinema screen and the UI quad are placed again from the head at the next frame.
void recentre_layers();
// R6: a measurement window of XR frames, missed display periods and mean frame times ("perf reset", "perf").
void perf_reset();
void perf_status(char* out, size_t len);
// R5 step 3: cinema mode (DESIGN §3.7): the game's finished frame (with its UI) on a 3.2 m screen 3 m ahead of the
// head pose taken when it is switched on, world-locked, in a dark room (no projection layer). The back buffer is copied
// into a quad swapchain of the sRGB variant of its format, so the runtime reads its display-referred bytes as sRGB.
void set_cinema(bool on);
// The live-resize guard: the game made its frame again at another size while the session runs (its Graphics menu's
// resolution: a "Post FXAA Target" or "Main Backbuffer" named at another size, or ResizeBuffers to one;
// the first sign, the targets are named ~6 ms before ResizeBuffers). From then on the eye images stop (no projection
// layer, the bind tap and the target watch removed), the UI redirect and EyeShape are off, and the flat game is shown on
// the cinema screen (resampled to its quad); a restart of the game brings the headset view back (for the menu's note).
// Any thread.
bool frame_resized();
// Test aid ("xr resized"): the guard's stop as at a real resize (the frame's size unchanged), for this start.
void force_frame_resized();
// The HUD's place ([XR] Hud, the menu's XR tab): the floating quad (the default) or the off hand's wrist, shown while
// you look at it (the radar's corner of the UI, [XR] WristHudRect; the prompts stay on the quad). Writes the user ini.
bool hud_on_wrist();
void set_hud_on_wrist(bool on);
void hud_status(char* out, size_t len);  // "hud [wrist|quad]"
// Moves the frame loop to the presenting thread (submission) or back to the idle loop, at the next idle-frame boundary.
void set_submit(bool on);

}  // namespace rdrvr::xr
