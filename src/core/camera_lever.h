#pragma once
// R1's camera lever (DESIGN R1, DECISIONS D4): offsets the main scene's camera to prove disparity reaches the GPU.
//   camera  the positive control: SceneRender 0x5c36e0 draws with a copy of its camera matrix moved by (dx, dy, dz)
//           along the camera's own rows a, b, c. SetCamera then keeps +0x40, +0x140, View +0x180, the cull planes and
//           the pushed shader globals consistent (ENGINE-NOTES 1.3).
//   view    the negative control: after SetCamera, View (+0x180) alone gets the view-space translation (-dx, -dy,
//           -dz) and the viewport is regenerated. S2 predicts zero disparity in the 151 camera-relative shaders.
//   world   the world-space control: the camera position in +0x40 alone (not +0x140), then regenerate.
// Only the viewport SceneRender draws is touched. (SceneRender calls SetCamera on it twice: before and after making
// it current, the second time through sm_Current 0x2ac0a10.) Off by default; driven by the test channel ("cam",
// "camlog").

#include <cstddef>
#include <cstdint>
#include <string>

namespace rdrvr::camera_lever {

enum class Mode { Off, Camera, View, World };

bool install();
void set(Mode mode, float dx, float dy, float dz);  // metres, along the camera matrix rows a, b, c
void request_log();                                  // log the next scene camera and viewport state
void status_text(char* out, size_t len);
// For the matrices checker, on the render thread: which viewport is current (the scene pass's own, the renderer's
// master, or another) and where its camera is.
void describe_current_viewport(char* out, size_t len);
bool swap_order();  // a double frame renders the right eye first
// A mono scene pass draws with a camera other than the game's: a yaw preset or the camera-mode offset (the truth
// capture's references). PreRender's TAA update saw the game's camera, not this one.
bool mono_camera_changed();
// R5 step 2: in a double frame, each pass's camera is the game camera composed with its eye's located XR view, relative
// to a recentre pose (yaw only, taken from the first views after recentre()).
void set_xr_pose(bool on);
// Round 1 check 12: with the XR pose on in a double frame, the visibility build culls for the head (the centre eye,
// both eyes' FOV + 4 degrees) instead of the game camera. [XR] HeadCover=1; "cover head on|off".
void set_head_cover(bool on);
// Round 2: with the XR pose on in a double frame, the distant tree billboards face the centre eye, level (head yaw
// only) instead of the game camera. [XR] LevelBillboards=1; "billboards level|game".
void set_level_billboards(bool on);
// The run 2 item 11 review: the tree imposters (one image per tree type) are re-captured along the current camera's
// view, an eye's in stereo, so the distant trees turn with the head. [XR] LevelImposters (off until the headset A/B):
// along the game camera's heading instead. "imposters game|eye".
void set_level_imposters(bool on);
bool level_imposters();
std::string imposter_status();
// Headset round 3 ("distant trees tilt with my head"): the forest manager's rage_grass shaders build each quad from
// gViewInverse rows 0 and 1 (the eye's right and up), so they roll and pitch with the head. While true, the forest/grass
// draw gets those two rows levelled (right horizontal, up = world y). [XR] LevelGrass=1; "billboards grass level|view".
bool level_grass_active();
void set_level_grass(bool on);
// Round 3 A/B: worldToScreen (the forest's LOD and facing view) as a level copy of the head cover ([XR] LevelForestView);
// and an experiment levelling every ViewInverse push of the eye passes (off by default).
void set_level_w2s(bool on);
// Round 3b test aid: the per-tree billboard direction eye -> tree (toward) or tree -> eye (away).
void set_billboard_sign(bool toward);
// The game camera's back row x and z (its heading) from this frame's scene camera; false before the first frame.
bool game_heading(float* bx, float* bz);
void set_vi_level(bool on);
// R5 cutscenes in 3D (DESIGN 3.7): the XR eye cameras keep the head's rotation and each eye's offset from the head's
// centre (times `separation`), without the head's translation, so a cutscene shot is looked around in, not walked in.
void set_xr_head_position(bool on, float separation);
// G-B: the head's yaw relative to the recentred forward (degrees, positive to the left), from the latest XR views;
// false before any view or recentre.
bool head_yaw_deg(float* out);
// The centre eye's position since recentre, in the recentred frame (metres: x right, y up, z back); false before.
bool head_offset(float out[3]);
// The neck's offset since recentre, in the recentred frame (metres: x right, y up, z back): a pivot 0.10 m below and
// 0.08 m behind the centre eye in the head's own axes, less where it is at the recentre (looking level, ahead), so
// turning or nodding the head leaves it nearly still while stepping and crouching move it. False before recentre.
bool neck_offset(float out[3]);
uint32_t recentre_gen();  // counts the recentres (the holsters' headset anchor takes its places again after one)
// A LOCAL pose (the controllers') in the game's world, as the eye cameras are placed: cam (the frame's game camera
// matrix: rows right, up, back, position) + its rows times the position since recentre (unturned by the recentre
// yaw). rot (x y z w) -> wrot, a 3x3 row-major matrix rotating column vectors into the world. False before recentre.
// The scene viewport's near and far planes as of the last scene render (metres; diagnostics).
void scene_clip(float* near_m, float* far_m);
bool local_to_world(const float* cam, const float* pos, const float* rot, float* wpos, float* wrot);
// A LOCAL position (a controller's) since recentre, in the recentred frame (x right, y up, z back): the hand's own
// motion, without the game camera's (walking, turning, the head-bone anchor). False before recentre. Any thread.
bool local_rel(const float* pos, float* prel);
// The inverse of local_to_world's position: a point in the game's world as a LOCAL position (the space of the views,
// the controllers and the quad layers), for the same frame camera `cam`. False before recentre. Any thread.
bool world_to_local(const float* cam, const float* wpos, float* lpos);
// The eye cameras follow the located views with the head's position (the XR pose, the double pass and the head
// position on): LOCAL and the world are then one rigid frame through world_to_local.
bool eyes_follow_head();
// The double pass with the eye cameras from the located XR views (the stereo view or 3D cutscenes): the frame's two
// images are the headset's eyes ([XR] EyeShape renders them in the eyes' own shape only then). Any thread.
bool xr_double();
uint64_t scene_calls();  // SceneRender calls so far (a frame without one shows a flat source: loading, bink, front end)
// The render thread's time in SceneRender so far (microseconds, both passes of a stereo frame) and the frames timed:
// the perf status's mean over its window.
void scene_time(uint64_t* us, uint64_t* frames);
void recentre();
// R3 diagnostic: during the next double frame, right after pass 1, search memory for copies of the centre camera's
// position and log where they are (module, stack, or a heap object by its vtable).
void find_centre_camera();

// R2. Eye projection: the scene viewport gets the chosen eye's XrFovf (fov 2*atan((tanU-tanD)/2), aspect, off-centre
// (tanR+tanL)/(tanR-tanL), (tanU+tanD)/(tanU-tanD), zoom 1) through Perspective 0x131ca0; the resulting matrix is read
// back and compared with the requested tangents. -1 = off, 0 = left, 1 = right, 3 = each pass its own eye (R5).
void set_eye_projection(int eye);
// R2. Explicit tangents (l < 0 < r, d < 0 < u) instead of an eye's XrFovf, for the off-centre sign test.
void set_tangents(float l, float r, float u, float d);
// R2. Cover viewport: the visibility build 0x5c2970 gets a copy of its viewport with a symmetric vertical FOV `vfov_deg`
// and `aspect` (0 = off). The copy is static: the build keeps the pointer (worldToScreen 0x2ac5088).
void set_cover(float vfov_deg, float aspect);

// R3, first step. Double scene pass: SceneRender runs twice per frame on the same viewport (its bytes saved before the
// first pass and restored before the second, so every pointer the frame holds stays the vanilla one). Post, UI and
// present then run once, on the second pass's output. With `ipd` > 0 the first pass is the left eye (camera moved
// -ipd/2 along its right axis) and the second the right eye (+ipd/2); `swap` renders right first, so the left eye
// reaches the screen. Zero ipd is R3's identity gate: the double render must equal the single one.
void set_double(bool on, float ipd, bool swap);
// R3 truth capture: a yaw (degrees) applied to the scene camera about its own up axis, before any eye offset, in every
// mode (mono and double). 0 = off.
void set_yaw(float deg);

// The viewport SceneRender is drawing on this thread, while it runs (render thread), or nullptr.
void* scene_viewport();

}  // namespace rdrvr::camera_lever
