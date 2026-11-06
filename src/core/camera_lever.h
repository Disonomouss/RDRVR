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
#include <vector>

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
// [Body] FirstPersonMoments (2026-10-12, the user: the "mini cutscenes in which John is seen in third person": a
// mission's short shots, looting and skinning, doors, chests and beds): in gameplay stereo with the camera anchor on,
// while a script's camera holds the channel (the anchor's left current for the script), the eyes, the culling and the
// shadows are drawn from the anchor's camera at John's head (its last matrix turned by the view's heading since, at the
// place the plugin put it this tick) instead of the script's. The script keeps its camera. On by default (the user's
// choice after their headset round: skinning stayed first person)
void set_first_person_moments(bool on);
bool first_person_moments();
std::string fp_status();  // "cam fp": on, frames drawn so, the reference's check against the real camera
std::string fp_check(bool mark);  // "cam fp mark|check" (a test)
// [XR] AudioFollowsHead: the game's audio listeners (panning and distance) from the centre eye's camera (the head) rather
// than the game camera (the body's heading in first person); only with the eyes from the XR views. On by default.
void set_audio_follows_head(bool on);
bool audio_follows_head();
// [XR] AudioRearFold: a mono voice's rear, centre and side gains folded into left and right on a stereo device (the
// game's own fold keeps the rear and the sides at 0.3, -10.5 dB). On by default.
void set_audio_rear_fold(bool on);
bool audio_rear_fold();
// [Comfort] WorldScale: the headset's positions (the eyes, the head, the hands) divided by it (0.5-2; 1 true scale)
float world_scale();
void set_world_scale(float s, bool save);
std::string audio_status();  // "cam audio [on|off] | cam audio fold on|off": both switches and their counters
// (run 29 item 3, test only) "scope game": the scope spike's reads of the game's own zoom, its scope overlay and the
// streamer's sniper byte (the scope itself: scope.h, r55 item 11)
std::string scope_command(const std::vector<std::string>& t);
// "cam audio log on|off|mark <n>|dump <file>" (run 12 item 10, a test's): each listener update, the game camera's and the
// listener's headings and positions, and why the head's was not used
std::string audio_log_command(const std::string& v, const std::string& arg);
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
bool game_place(float* x, float* z);
// (round 94, the user: "No reticle visible in third person") that camera whole (the eye cameras' base in third person,
// where the drawn body's points are not published); false before the first frame. Any thread
bool game_camera(float out[16]);  // (round 93) that camera's place across (x, z); false before the first frame
// (2026-11-06, GOAL-remaining 6c) a yaw (degrees) the eye cameras take over the game camera's, for the third person snap;
// the centre eye's heading as drawn (game_heading's convention, degrees)
void set_view_yaw_offset(float deg);
float view_yaw_offset();
// (round 93) the snap's view held at `target_deg` (game_heading's convention) and at the camera's place once turned (John's
// place plus its start offset turned by the step), times `weight` (its ease out), from each frame's own camera at the
// render; off: the offset above again
// (round 95) fresh: a new snap (its start taken from this frame's camera); else a chained one keeps the start
void set_view_yaw_target(bool on, float target_deg, float weight, bool fresh = false);
// the last snap's drawn centre eye while held: its heading's and its place's largest moves from its first frame; and
// the game camera's distance from the held place at the last held frame (what the ease takes out)
void view_yaw_target_moves(float* deg, float* m, float* end_m);
void view_yaw_rereference();  // (round 95) a chained step: those moves measured from it
float view_yaw_frame();  // the yaw the eye cameras took this frame (degrees)
bool view_heading(float* deg);
void set_vi_level(bool on);
// R5 cutscenes in 3D (DESIGN 3.7): the XR eye cameras keep the head's rotation and each eye's offset from the head's
// centre (times `separation`), without the head's translation, so a cutscene shot is looked around in, not walked in.
void set_xr_head_position(bool on, float separation);
// G-B: the head's yaw relative to the recentred forward (degrees, positive to the left), from the latest XR views;
// false before any view or recentre.
bool head_yaw_deg(float* out);
// Where the head looks (world, unit): the centre eye's forward from the frame's camera and the head pose; false before
// the first frame or without the head pose ([Gestures] LassoAim=head)
bool head_forward(float d[3]);
// The centre eye's position since recentre, in the recentred frame (metres: x right, y up, z back); false before.
bool head_offset(float out[3]);
// The neck's offset since recentre, in the recentred frame (metres: x right, y up, z back): a pivot 0.10 m below and
// 0.08 m behind the centre eye in the head's own axes, less where it is at the recentre (looking level, ahead), so
// turning or nodding the head leaves it nearly still while stepping and crouching move it. False before recentre.
bool neck_offset(float out[3]);
uint32_t recentre_gen();  // counts the recentres (the holsters' headset anchor takes its places again after one)
// [XR] RoomScale (2026-10-27, research/run27/roomscale.md B2): while `owns` is set (pose.cpp: the room-scale walk's
// command, and 600 ms after it), once a presented frame the anchored camera's horizontal move is taken off the head's
// offset by moving the recentre origin (the frame's own camera, in SceneRender), so the view moves only with the head
// while John walks in under it. (Cycle r163) John's whole horizontal move each tick, as a vector, at most a walk's speed a tick in length
// (2.5 m/s): the view is the head's, his slide included; a faster move's excess (a train, a drag) stays in the view.
// recentre_gen() is not bumped by it. Off and nothing queued, the frame's take-off is one relaxed load.
void room_set_owns(bool on);
bool room_owns();
void room_set_take(bool on);  // "pose room take on|off": the take-off itself off (a test's negative control for the judder)
bool room_take();
// (cycle r189) the take-off's cap (2.5 m/s a step, 0.125 m at least) raised to `m` game metres for the steps until
// log::now_ms() reaches `until_ms`: the room walk's end puts John under the head in one jump (pose.cpp rs_snap), taken
// off whole so the view holds. Any thread
void room_allow_step(float m, double until_ms);
// [XR] BodyFollowsTurn (2026-10-27; the user: "Turning physically will turn johns body right?"): pose.cpp turns the body
// heading after the head and pushes each frame's commanded heading with the follow's total in it (degrees, positive
// left; `ours`: the anchor's own camera on foot). At each presented frame's start the frame camera's heading is matched
// against them and the recentre origin's yaw turned by the total it shows, so the view (the camera and the head over the
// origin) never turns with the body, whatever the camera's tick lag. Pushed only while the option is on (or a turn is
// pending); body_turn_stop ends it (nothing then runs: room_take_frame's one load). Any thread
void body_turn_push(float heading_deg, float total_deg, bool ours);
void body_turn_stop();
float body_turn_shown();  // the follow's total the origin's yaw has taken (degrees)
// the camera's heading offset learned (30 still frames): before it pose starts no follow (a miss turns nothing)
bool body_turn_ready();
void body_turn_clear();  // a recentre: the pushed commands forgotten (the fresh origin matched only against new ones)
// the match (error, misses, the camera's offset) and the view's world yaw, with its largest frame step the head still
std::string body_turn_status();
void body_turn_reset_view();
// The origin moved by (dr, 0, db) game metres along the recentred frame's x (right) and z (back): the head's, neck's
// and hands' offsets all less that (the hard clamp, the re-anchor). Queued: applied at the next frame's start, before
// both eyes' passes. Any thread; nothing before the first recentre.
void room_shift_origin(float dr, float db);
// neck_offset as the next frame will have it (the queued shifts in): the room-scale walk's own reading
bool room_neck_offset(float out[3]);
// Every room shift so far added up, LOCAL raw metres (never reset; the physical crouch takes its differences).
void room_shift_total(float out[3]);
// metres taken off, the eye's largest frame step (the head still), frames, frames faster than a walk, metres not taken
void room_stats(float* taken, float* judder, uint64_t* frames, uint64_t* rejects, float* refused);
void room_reset_judder();
// the judder over the head-still frames the walk owned: mean, 95th percentile (the last 1024), frames, frames over 5 mm
void room_judder_stats(float* mean, float* p95, uint64_t* frames, uint64_t* over);
// The centre eye's world place as the last frame drew it (the frame's camera and the views now). False before.
bool eye_world(float out[3]);
// 2026-10-27 ([XR] PhysicalCrouch): the same neck point in the LOCAL space itself (raw metres: not since recentre, not
// divided by the world scale: the user's real body). False before the first views. Any thread.
bool neck_local(float out[3]);
// [Holsters] LeanSteady's model (2026-10-27: shared by the holsters and the physical crouch): a neck offset `n` (x right,
// y up, z back) read as a torso of length L pivoting at the hips: the horizontal move it explains taken off, and the
// drop that lean explains added back to y (a bend is not a crouch). Returns the lean (metres).
float lean_split(float n[3], float L);
// A LOCAL pose (the controllers') in the game's world, as the eye cameras are placed: cam (the frame's game camera
// matrix: rows right, up, back, position) + its rows times the position since recentre (unturned by the recentre
// yaw). rot (x y z w) -> wrot, a 3x3 row-major matrix rotating column vectors into the world. False before recentre.
// The scene viewport's near and far planes as of the last scene render (metres; diagnostics).
void scene_clip(float* near_m, float* far_m);
// The single-pass study step 2 (dual_pass's splits "shadows" and "shadowunion"): a viewport covering both eyes, for
// the shared shadow passes' fitting and culling: the centre eye's camera (the eyes' midpoint, the head's orientation)
// with both eyes' tangents widened by a few degrees. Built on the render thread before pass 1 of a double frame with the
// XR pose; null otherwise (and in a mono frame).
void* shadow_union_vp();
bool local_to_world(const float* cam, const float* pos, const float* rot, float* wpos, float* wrot);
// (2026-10-27: from the recentre's own origin, not the room-scale walk's shifted one)
// A LOCAL position (a controller's) since recentre, in the recentred frame (x right, y up, z back): the hand's own
// motion, without the game camera's (walking, turning, the head-bone anchor). False before recentre. Any thread.
// (2026-10-27, [XR] BodyFollowsTurn) `turn_deg`: the follow's total the origin's yaw carried for this reading (read
// under the same lock): a later reading is turned by its change (prel turned by -change about y), so a history of them
// differentiates to the hand's own motion only (gestures.cpp)
bool local_rel(const float* pos, float* prel, float* turn_deg = nullptr);
// world_to_local then local_rel under one lock (one origin for both), with that reading's turn. Any thread
bool world_to_local_rel(const float* cam, const float* wpos, float* prel, float* turn_deg);
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
// Test aid ("cam yawramp <deg>"): the yaw lever advanced by `deg` every scene frame, before its passes (0 stops it,
// the yaw kept); a deterministic camera turn for measuring the eyes' motion, frame by frame.
void set_yaw_ramp(float deg_per_frame);
// Test aid ("cam moveramp dx dy dz"): the lever's world offset (metres along the camera's rows, as "cam world")
// advanced by (dx, dy, dz) every scene frame; all zero stops it and takes the offset off.
void set_move_ramp(float dx, float dy, float dz);

// The viewport SceneRender is drawing on this thread, while it runs (render thread), or nullptr.
void* scene_viewport();
// [Stereo] PrepassViews: the private eye-2 viewport built in this frame's pass 1, or nullptr. Never current, except under
// [Stereo] PrepassEye2Collector (test only), which makes it current around its one FUN_1406e58a0 call in pass 1 and then
// puts the scene viewport back (eye2_collector.cpp run_eye2)
void* eye2_viewport();
// [Stereo] PrepassWvpMirror (run 17, research\run17\sps.md (A); test only): called by the existing PushGlobals hook
// (0x130e60) after the game's push, on the pushing thread, with the pushed viewport and that thread's scene viewport
// (scene_viewport(), nullptr outside SceneRender). nullptr unsets it; unset costs the hook one relaxed load.
using AfterPush_t = void (*)(void* vp, void* scene_vp);
void set_after_push(AfterPush_t fn);
bool push_hook_installed();  // the PushGlobals hook is in (set_after_push's function can be called)

}  // namespace rdrvr::camera_lever
