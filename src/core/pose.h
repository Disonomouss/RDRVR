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

#include "common/rdrvr_api.h"

namespace rdrvr::pose {

void init();  // frame-end listener
void set_keep_camera(bool on);  // [Body] KeepAnchorCamera for the session ("pose keepcam on|off")
void set_anchor(bool on);    // the switch ([Body] CameraAnchor, the menu, "pose anchor on|off")
bool anchor_active();   // the camera anchor is placing the view this frame (first person)
bool on_mounted_gun();  // a mounted gun (the Gatling, the Browning, the cannon) in use now, or fired within 1.5 s
bool mounted_gun_held();  // one in hand now
bool mounted_first_person();
void set_mounted_first_person(bool on, bool save = false);
bool anchor_enabled();
// Riding ([Horse], the menu's Body tab): steering by the head (the stick turned by the head's yaw, as on foot) or by
// the stick (the view faces the horse); the saddle anchor (the camera above the horse's root, not the rider's bouncing
// head) and its height smoothing (seconds). The setters write the user ini (set_saddle when `save`).
bool steer_by_head();
void set_steer_by_head(bool on);
// [Horse] StickTurn (run 9 item 4, on): with stick steering the right stick turns the view on horseback (TurnMode's
// snap or smooth), an offset over the horse's heading; mounting and recentre set it to 0 ("pose rideturn on|off")
bool stick_turn_riding();
bool vehicle_heading();
// [Body] ArmedStance=stand (run 10 item 8b): a gun drawn standing, the view's (and the still body's) height held
bool armed_stand();
void set_armed_stand(bool on, bool save);
bool holds_armed_height(const RdrvrActorState& st);
// (2026-11-04, GOAL-next-items item 1) the view eased back up after a kneel with an item in hand: the still body then
// eases back to the height it held before the kneel too
bool item_restoring();  // [Horse] VehicleHeading (run 10 item 3): driving a wagon, the view's heading the vehicle's
void set_vehicle_heading(bool on, bool save);
void set_stick_turn_riding(bool on, bool save = true);
float ride_turn_deg();  // the offset now (0 on foot or steering by the head)
// [XR] PhysicalCrouch (2026-10-27, research/run27/roomscale.md Part C; off by default): the user's own crouch crouches
// John (the neck's drop below its standing reference, the lean taken out; the game's L3 pressed on the change, checked
// on IS_ACTOR_CROUCHING, retried once, never against the game's own changes); while it owns John's crouch the view's
// height is held at the standing one. CrouchShare / StandShare of the standing eye height (STAGE's, else
// StandingHeight), each held CrouchHoldMs. The setters write the user ini when `save` ("pose crouch ..." does not).
// [XR] RoomScale (2026-10-27, research/run27/roomscale.md Part B; off by default): a step in the room walks John under
// the head (the left stick, at most RoomScaleMaxStick: a walk), the view moving only with the head (camera_lever's
// take-off); the player's stick wins; a blocked John stops the stick and fades the eyes (RoomScaleFade) with the offset
// clamped at RoomScaleMaxOffset. On foot only. The setters write the user ini when `save` ("pose room ..." does not).
bool room_scale();
void set_room_scale(bool on, bool save = true);
bool room_fade();
void set_room_fade(bool on, bool save = true);
void room_tuning(float* dead, float* stop, float* min_stick, float* max_stick, float* max_offset);
void set_room_tuning(float dead, float stop, float min_stick, float max_stick, float max_offset, bool save);
std::string room_status();
// (2026-10-28) [XR] RoomScaleLeanSlack (m: the drop a lean is judged from, 0-0.10) and RoomScaleStartLead (s: the early
// start's look ahead, 0-0.5; 0 = the dead zone alone). Ini only ("pose room quick <slack> <lead>" for the session)
void room_response(float* lean_slack, float* start_lead);
void set_room_response(float lean_slack, float start_lead, bool save);
// [XR] RoomScaleDirect (2026-11-02): a small step put under the head at once, no walk, where a probe finds no wall
bool room_direct();
void set_room_direct(bool on, bool save);
// "pose room": the offset, the walk, blocked, the fade, the slide, the metres taken off, the eye's world place
void set_room_take(bool on);  // "pose room take on|off": the take-off itself (a test's negative control; the session only)
// [XR] BodyFollowsTurn (2026-10-27; the user: "Turning physically will turn johns body right?"; off by default): the
// head's yaw over the body held beyond BodyTurnDeadZone degrees for BodyTurnHoldMs turns the body heading after it
// (smooth, at most BodyTurnSpeed deg/s), the view kept still (camera_lever turns the recentre origin's yaw by what the
// camera shows). On foot only: not riding, in cover, a script's camera, a menu, the wheel, a long gun two-handed, a
// room-scale walk. The setters write the user ini when `save` ("pose bodyturn ..." does not).
bool body_follows_turn();
void set_body_follows_turn(bool on, bool save = true);
void body_turn_tuning(float* dead, float* hold_ms, float* speed);
// gain, ease_s (2026-10-28): [XR] BodyTurnGain (a second, the tail) and BodyTurnEaseS (the ease-in); negative: unchanged
void set_body_turn_tuning(float dead, float hold_ms, float speed, bool save, float gain = -1.0f, float ease_s = -1.0f);
// [XR] BodyTurnDuringWalk (2026-10-28, on): the follow also during a room-scale walk (its direction turned with it)
bool body_turn_during_walk();
void set_body_turn_during_walk(bool on, bool save = true);
std::string body_turn_status();  // "pose bodyturn": the gate, the head over the body, following, the origin's match, the view
// the follow's turn so far (degrees, wrapped; 0 while the option was never on): its changes are the body's turns by it
// (holster.cpp's TurnWithHead keeps its frame where it was in the world). Any thread
float body_turn_total_deg();
// the follow's own steps only, summed (degrees, wrapped): a recentre's correction of the total never in it (the holsters'
// TurnWithHead turns its frame back by its changes alone: a recentre resets that frame itself). Any thread
float body_turn_steps_deg();
bool physical_crouch();
void set_physical_crouch(bool on, bool save = true);
// A recentre now keeps its origin's height (camera_lever::recentre_from): PhysicalCrouch on and John crouched
bool crouch_keeps_recentre_height();
float crouch_share();
void set_crouch_share(float share, bool save = true);  // (StandShare kept 0.04 under it)
// quick_ms: [XR] CrouchQuickHoldMs, the hold at 40 % past the threshold (2026-10-28; negative: unchanged)
void set_crouch_tuning(float crouch_share, float stand_share, float hold_ms, float standing_m, bool save, float quick_ms = -1.0f);
float crouch_hold_ms();                              // [XR] CrouchHoldMs (the menu's "Hold before he crouches")
void set_crouch_hold(float hold_ms, bool save = true);  // (the other crouch keys kept)
void calibrate_standing(double ms);  // "Set my standing height": the reference taken `ms` from now
double standing_calibrate_in();      // ms left of that countdown (0: due, waiting for the headset's tracking), -1 none
// the last one not taken, for 8 s (nullptr otherwise): John crouched (refused: stand him first), or the headset never tracked (10 s)
const char* standing_calibrate_message();
float standing_height(bool* from_floor);  // H now (from_floor: the headset's STAGE space gave it)
std::string crouch_status();         // "pose crouch": drop, H, y_stand, want/have/owned, presses, retries, refusals
// John's body's heading: the body heading less the ride's turn (the horse's while riding with the view turned): the
// holsters' frame, the facing lock, the elbow hints
float torso_heading_deg();
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
// "pose tpsnap [on|off]": [Comfort] ThirdPersonSnap for the session (2026-11-06, GOAL-remaining 6c), its counters
std::string tps_command(const std::string& v);
// [Comfort] ThirdPersonSnap and ThirdPersonSnapAngle (round 98: the menu's; 10-90 deg); save: also written to the user ini
bool third_person_snap();
void set_third_person_snap(bool on, bool save);
float third_person_snap_angle();
void set_third_person_snap_angle(float deg, bool save);
float turn_speed();
float snap_angle();
// Recentre (DESIGN 3.7): the body turns to where the head looks, so the recentred forward is the new body forward.
void on_recentre();
// Test aid ("pose orient 0|1|2|3"): which SET_CAMERA_ORIENTATION component carries the heading (0 z, 1 y, 2 x, 3 y
// negated). Round 1 saw mode 0 roll the view; [Body] OrientMode.
void set_orient_mode(int m);
void status_text(char* out, size_t len);

}  // namespace rdrvr::pose
