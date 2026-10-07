#pragma once
// G-B pose service (DESIGN §5: camera anchor, posture, turning, walking, recentre). In gameplay stereo, with
// [Body] CameraAnchor on, the gameplay plugin keeps a scripted camera at the player actor's root plus the eye height
// (+ the seated lift) and 5 cm forward, level, facing the body frame's heading; the eye cameras are that camera
// composed with the head pose since recentre, so the animated head bone and the walk bob never move the view.
//  - the body heading starts at the actor's heading; the right stick turns it (smooth by default, snap optional);
//  - the left stick is rotated by the head's yaw relative to the body, so walking follows the head;
//  - recentre (camera_lever::recentre) resets the head origin, the cinema screen and the UI quad together.
// Ships off ([Body] CameraAnchor=0) until judged in the headset; "pose anchor on|off" switches it in a test.

#include <cstddef>
#include <string>

namespace rdrvr::pose {

void init();  // frame-end listener
void set_anchor(bool on);    // the switch ([Body] CameraAnchor, the menu, "pose anchor on|off")
bool anchor_active();        // the camera anchor is placing the view this frame (first person)
bool anchor_enabled();
// Riding ([Horse], the menu's Body tab): steering by the head (the stick turned by the head's yaw, as on foot) or by
// the stick (the view faces the horse); the saddle anchor (the camera above the horse's root, not the rider's bouncing
// head) and its height smoothing (seconds). The setters write the user ini (set_saddle when `save`).
bool steer_by_head();
void set_steer_by_head(bool on);
bool saddle_anchor();
// [Horse] SaddleClimb (run 6 item 8, on): with the saddle anchor, the plugin feeds the horse's own vertical speed into
// the height's filter (it fell behind climbs) and keeps the view above the rider's root + 0.5 m
bool saddle_climb();
void set_saddle_climb(bool on, bool save);
// "pose trace on|off|reset|dump <file>": a row per script tick while riding (a test aid), written as CSV on dump
std::string trace_command(const std::string& verb, const std::string& arg);
float saddle_smoothing();
void set_saddle(bool on, float tau, bool save);
void reset_ride_jitter();  // the test channel's "pose jitter reset"
float body_heading_deg();    // the anchored camera's body heading (degrees; positive turns left)
float eye_lift();            // [Body] SeatedLift: metres added to the eye height
void set_eye_lift(float m);
// Turning ([Comfort] TurnMode / TurnSpeed / SnapAngle), applied at once (the menu's Comfort tab, "pose turn ...").
void set_turning(bool snap, float speed_deg_s, float snap_deg);
bool snap_turning();
float turn_speed();
float snap_angle();
// Recentre (DESIGN 3.7): the body turns to where the head looks, so the recentred forward is the new body forward.
void on_recentre();
// Test aid ("pose orient 0|1|2|3"): which SET_CAMERA_ORIENTATION component carries the heading (0 z, 1 y, 2 x, 3 y
// negated). Round 1 saw mode 0 roll the view; [Body] OrientMode.
void set_orient_mode(int m);
void status_text(char* out, size_t len);

}  // namespace rdrvr::pose
