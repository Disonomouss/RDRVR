#pragma once
// The player's body in first person (G-C): the head and hat hidden. The player's skeleton is reached the way the
// GET_OBJECT_NAMED_BONE_POSITION handler (FUN_140230700) reaches it: the object handle (from the gameplay plugin's
// GET_OBJECT_FROM_ACTOR) -> the ObjectsPool slot -> the object -> its virtual +0x100 (1) = the skeleton:
//   skeleton +0x08  skeleton data: +0x00 bone records (0x110 bytes each, +0x00 the name), +0x30 the bone count (u16)
//   skeleton +0x28  the bones' world matrices, 0x40 bytes each (the translation at +0x30)
// The skeleton itself is rewritten by the update thread while the scene renders, so the hide works on the render-side
// copies at the entity draw (DrawVisEntity, 0x640120): the player's grmMatrixSet (record [0x12]; record [0x13] +0x18
// is the skeleton) has its head bones collapsed to the head joint, and one-matrix draws on the head bone (the hat)
// are shrunk to a point. Gameplay never sees it. [Body] HideHead=1: while the camera anchor is active.
// Headset round 5 added, on the same copy: the upper body held upright over the hips (or only its forward/back lean
// removed), the drawn body moved back by [Body] BodyBack, and optionally with the headset's offset.

#include <string>

namespace rdrvr::body {

bool install();  // the entity draw hook
bool hide_enabled();         // [Body] HideHead
void set_hide_enabled(bool on);
int stance();                // [Body] Stance: 0 the game's animation, 1 upper body upright, 2 the forward/back lean removed
void set_stance(int s);
float body_back();           // [Body] BodyBack: metres the drawn body sits behind the camera's place
void set_body_back(float m);
bool follows_head();         // [Body] BodyFollowsHead: the drawn body moves with the headset's horizontal offset
void set_follows_head(bool on);
bool locks_facing();         // [Body] LockFacing: the drawn body faces the camera's heading (no shoulder in view when
void set_locks_facing(bool on);  //  walking sideways)
float torso_pitch();         // [Body] TorsoPitch: degrees added to the upright torso (positive leans it forward)
void set_torso_pitch(float deg);
// The arms on the controllers ([Hands]): two-bone IK of each tracked hand's arm to its grip pose, the hand turned to
// the controller's orientation. ArmIK on/off, WristOffset (metres from the grip origin back to the wrist joint along
// the grip's +z), and the hand's calibration turn in the grip frame (degrees; mirrored for the left hand).
struct HandCfg {
    bool ik = true;
    float wrist_offset = 0.065f;
    float yaw = 0, pitch = -68, roll = 0;  // the user's calibration (round 6)
    int show = 0;  // [Body] Show: 0 the whole body, 1 the forearms and hands, 2 the hands only
    bool stretch = true;       // [Hands] ArmStretch: past John's reach the arm lengthens to the controller
    float stretch_max = 1.3f;  // [Hands] ArmStretchMax: the longest arm, times John's (1.0 - 1.6)
};
// The hand's world correction of the latest drawn player set (T(x) = A x + a takes the game's pose of anything on that
// wrist to where it is drawn): false when no player set was corrected in the last 250 ms. Any thread. h: 0 left, 1 right.
bool hand_correction(int h, float A[9], float a[3], bool* ik);
// What the game holds in its hand g (0 left, 1 right: the hand it hangs the item on) as drawn: that wrist's
// correction, or, left-handed with [Hands] GunInGunHand, the transplant onto John's left hand (a rigid move: the gun
// sits on the left controller as it sits on the right one right-handed). The aim, the held prop. Any thread.
bool item_correction(int g, float A[9], float a[3], bool* ik, double* ad = nullptr);  // ad: a in double (the world's coordinates are large)
// [Hands] GunInGunHand (left-handed: each arm on its own controller, the gun in John's left hand), TransplantFingers
// (0 unchanged, 1 the gun hand takes the gripping hand's curl and the other the game's empty hand's, 2 the other open)
bool gun_in_gun_hand();
void set_gun_in_gun_hand(bool on);  // also written to the user ini
// [Hands] FixedGunGrip (off; run 7 item 1c): each long gun held in the drawn hand by its aiming hold in every pose
bool fixed_gun_grip();
void set_fixed_gun_grip(bool on, bool save);  // save: also written to the user ini (the menu; "skel grip on|off" is the session's)
int transplant_fingers();
void set_transplant_fingers(int mode);  // also written to the user ini
// The visibility build (camera_lever.cpp's hook, the update thread, just before the game builds its draw records):
// the item in hand's game matrix is sampled, so the draws of its prop are known by that matrix ([Body] HeldPropFix).
void on_visibility_build();
// The drawn body's points for the holsters (world, from the latest corrected player set): each hand's wrist target
// (where the IK puts the wrist: the controller), the holster bones and the root joint. False when stale (250 ms).
constexpr int kHolsterBones = 6;  // 0 pistol (right hip), 1 rifle (back), 2 thrower, 3 melee (pelvis), 4 the chest (spine03), 5 the lower back (the pelvis bone)
constexpr int kPelvisPoint = 5;   // the pelvis as drawn: the held props' anchor (held_prop::note_root)
struct BodyPoints {
    double ms = 0;
    bool hand_ok[2] = {false, false};
    float hand[2][3] = {};
    bool bone_ok[kHolsterBones] = {};
    float bone[kHolsterBones][3] = {};
    float root[3] = {};
    // the wrists' orientations (world, columns the axes): the IK target's (the controller with the calibration), the
    // game's animated wrist and the drawn one (its correction times the animated)
    float target_rot[2][9] = {};
    float wrist_anim[2][9] = {};
    float wrist_drawn[2][9] = {};
    // the frame's game camera (SceneRender's: rows right, up, back, then the position) these points were placed with,
    // for camera_lever::world_to_local (the holster rings)
    float cam[16] = {};
    bool cam_ok = false;
    // the foregrip (a long gun in hand): where John's front (left) wrist goes on the drawn gun, the game's own grip
    // moved by [Reload] ForegripOffset (world)
    bool fore_ok = false;
    float fore[3] = {};
    int gun = 1;               // John's gun hand (0 left-handed with GunInGunHand)
    // run 5 (RoundInHand): each hand's thumb and index fingertip joints as drawn (thumb_03, finger_13; world)
    bool tips_ok[2] = {false, false};
    float thumb_tip[2][3] = {};
    float index_tip[2][3] = {};
    int ctrl[2] = {0, 1};      // the controller each of John's hands follows
    // run 6 item 9g: the drawn long gun's own frame (world; its axes as columns: x right, y up, z back), for the zones
    // of its parts (actions.cpp: the single-shot breeches)
    bool gun_frame_ok = false;
    float gun_frame_R[9] = {};
    float gun_frame_o[3] = {};
};
bool body_points(BodyPoints* out);
// [Body] HiddenGeometry: 0 draw (the hidden parts drawn collapsed, as before), 1 skip (the geometries with every bone
// hidden not drawn), 2 filter (also the mixed ones drawn without their hidden triangles that span two cuts: the sliver)
int hidden_geometry();
void set_hidden_geometry(int mode);  // also written to the user ini
const char* grip_source_name();  // the foregrip's grip now: "the weapon's", "<gun>'s (borrowed)", "the template", "built-in"
bool auto_shows();           // [Body] AutoShow: forearms and hands while crouching, in cover, riding or driving
void set_auto_shows(bool on);
bool locks_torso();          // [Body] LockTorso: the upper body kept over the hips horizontally (running leans)
void set_locks_torso(bool on);
HandCfg hand_cfg();
void set_hand_cfg(const HandCfg& c);
std::string command(const std::string& line);
// The scene render hook, first thing (after the game's update, before its draw lists): the head hide when on.
void before_scene(const float* cam);  // cam: the frame's game camera matrix (for the controllers' world poses)

}  // namespace rdrvr::body
