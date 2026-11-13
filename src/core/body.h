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

#include <cstdint>
#include <string>

namespace rdrvr::body {

bool install();  // the entity draw hook
bool hide_enabled();         // [Body] HideHead
void set_hide_enabled(bool on);
// (round 145) poker's sit-down: the game's character clipping kept on until the hand begins (body.cpp); each frame
void table_clip_frame();
int stance();                // [Body] Stance: 0 the game's animation, 1 upper body upright, 2 the forward/back lean removed, 3 still
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
// correction, or, left-handed, the transplant onto John's left hand (a rigid move: the gun
// sits on the left controller as it sits on the right one right-handed). The aim, the held prop. Any thread.
bool item_correction(int g, float A[9], float a[3], bool* ik, double* ad = nullptr);  // ad: a in double (the world's coordinates are large)
// The same with the item's game matrix (W +0x80) of the build it was drawn with, and its weapon object W, from one
// copy: A x + a on `gun` is the item as drawn (a live read of W +0x80 may be a build newer: up to 19 degrees off in an
// animation's clip). False without that hand's sample. The game's matrix: no GunKick in it. (GOAL-weapons-handling item
// 1b, after optimumbox's fork, v0.8.11.10, MIT.) Any thread. frame: the body frame it was published in. False also for
// an item hung on the game's other hand than g (the sample's side): the aim's g = 1, so an item on the game's left hand
// keeps the old live read. Left-handed the game still hangs the gun on its right hand (the
// transplant is g = 1's correction), so the fix applies there too [I: static reading, research/run28/aimfix.md].
bool item_gun(int g, float A[9], float a[3], bool* ik, float gun[16], uintptr_t* W, uint64_t* frame = nullptr);
// Item 1b's check (read only): the gun in hand's root (bone 0, or its rigid record) as last drawn in an eye's pass (not
// the shadow pre-pass), [0] after the hand's correction and before the steadying, [1] after it (as seen), both before
// GunKick: axes as columns (R[s][k * 3 + j]: axis j's k), the origin (world); the body frame; whether that draw matched
// an older build's sample than the newest. False with none in 250 ms. Any thread.
bool drawn_gun(float R[2][9], float o[2][3], uint64_t* frame, bool* older, double* age_ms);
void set_drawn_gun_capture(bool on);  // drawn_gun's capture on (the session: "aim barrelstep on"); off, the draws are untouched
void held_match_counts(uint64_t* newest, uint64_t* older);  // the held draws matched to the newest build's sample, to an older
// (2026-10-28, round 38 check 2: "works for the main gun, second guns reticle still all over the place") The second gun
// (DualWield's, another slot) or the copy (DualWieldCopy) as drawn in the latest published body frame: its newest placed
// sample with its hand's motion since that placement (as its draws take it), carried or not (dual::state says). m: rows,
// the axes (x right, y up, z back) then the position (world); W its weapon object (the copy: the gun in hand's); john
// John's hand; shift the copy's shot's move by the two models' muzzles (DualWieldOwnModel; its axes, m; else 0). The copy
// with tw_ok: Tw, the gun in hand as the aim takes it (item_gun's correction on that build's game matrix) onto the copy
// as drawn, rows (p' = p Tw). R, t: that hand's IK target in the same frame (R: columns its axes; world). False with none
// published within 250 ms. Any thread.
struct SecondDrawn {
    float m[16] = {};
    uintptr_t W = 0;
    int john = -1;
    bool copy = false;
    float shift[3] = {};
    bool tw_ok = false;
    float Tw[16] = {};
    float R[9] = {}, t[3] = {};
    uint64_t frame = 0, build = 0;
    double ms = 0.0;
};
bool second_drawn(SecondDrawn* out, double* age_ms = nullptr);
void second_drawn_counts(uint64_t* pubs, uint64_t* tw);  // the frames published with one, with the copy's Tw
// The game thread (aim.cpp's shot): the second gun W as this update placed it (W +0x80 then: rows), within 100 ms and
// not a carry; false otherwise
bool secondary_placed(uintptr_t W, float m[16]);
// Its check (read only; "aim barrelstep2"): the second gun's or the copy's prop's root as last drawn in an eye's pass,
// after its hand's correction, before GunKick: axes as columns (R[k * 3 + j]: axis j's k), the origin (world), the body
// frame, whether it was the copy's. False with none in 250 ms; captured only while set_drawn_second_capture is on.
bool drawn_second(float R[9], float o[3], uint64_t* frame, bool* copy);
void set_drawn_second_capture(bool on);
void set_drawn_second_scope(bool on);
void set_drawn_second_bolt(bool on);  // (step 4, answer 2) actions' capture of it (a long bolt second gun's knob)  // (step 4, S5) the scope's own capture of the second gun's root (apart from the test's)
// [Hands] GunInGunHand (left-handed: each arm on its own controller, the gun in John's left hand; always on since
// 2026-11-05, its ini key not read: the test channel's "skel gun gunhand" only), TransplantFingers
// (0 unchanged, 1 the gun hand takes the gripping hand's curl and the other the game's empty hand's, 2 the other open)
bool gun_in_gun_hand();
void set_gun_in_gun_hand(bool on);  // also written to the user ini
// [Hands] FixedGunGrip (off; run 7 item 1c): each long gun held in the drawn hand by its aiming hold in every pose
bool two_handed_steer();  // [Reload] TwoHandedSteer: the front hand on the foregrip turns the gun (0: drawn on it only)
void set_two_handed_steer(bool on);  // also written to the user ini
bool fixed_gun_grip();
void set_fixed_gun_grip(bool on, bool save);
// [Hands] GunKick (2026-10-27, off; the user: "Look into the recoil optimumbox added"; after optimumbox's recoil ([Hands]
// Recoil), 0.8.11.10): each shot kicks the drawn gun muzzle up and back in John's grip (a damped spring per hand: the
// gun in hand's shots, the second gun's or the copy's own), its strength 0 - 2 (0 off). The drawn gun only: the aim, the
// laser, the reticle, held objects and the shots stay on the gun as held. "skel recoil <k>" is the session's
float gun_kick();
void set_gun_kick(float k, bool save);  // save: also written to the user ini (the menu)
// [Hands] GunCollision (2026-10-29, off; GOAL-weapons-handling item 4 and its amendment, research/run28/collision.md): the
// guns in John's hands, his hands and forearms as capsules; a gun that touches another gun or John's other hand or arm
// (or its own forearm past the grip) is drawn held apart at the contact, a pulse on the controllers. The drawn guns only,
// moved without a turn (as GunKick): the aim, the laser, the reticle and the shots stay on the guns as held. A gun held
// by a snap (the foregrip, a part in the off hand, the thumb on the hammer, the carry) is held still; a hand loading or
// gripping is never pushed. "skel collide [on|off]" is the session's
bool gun_collision();
void set_gun_collision(bool on, bool save);  // save: also written to the user ini (the menu)
// The guns as the collisions took them in the last body frame (within 250 ms; only while GunCollision is on), for
// actions.cpp's hammer cocked by the other gun: gun[0] the gun in hand, [1] the second gun or the copy, [2] the carried gun.
// Each as held (no collision move, no kick): its axes as columns (x right, y up, z back), its origin (world), its body's
// capsule along the barrel in its own axes (a, b, r), John's hand holding it, its eWeapon; the gun in hand's hammer spur
// (a single action's, the hammer down; its own axes) if it has one. Any thread.
struct CollGun {
    bool ok = false;
    int weapon = -1, john = -1;
    float R[9] = {}, o[3] = {};
    float a[3] = {}, b[3] = {}, r = 0.0f;
    bool spur_ok = false;
    float spur[3] = {};
};
struct CollGuns {
    double ms = 0.0;
    uint64_t frame = 0;
    CollGun gun[3];
};
bool collision_guns(CollGuns* out);
// The frame end (actions.cpp): a sound for two guns met at speed (the game's ARMOR_METAL_SHORT at `at`, world), once each;
// false when none waits
bool collision_sound(float at[3]);
bool fixed_sidearm_grip();  // [Hands] FixedSidearmGrip: the sidearms held by their aiming hold too
void set_fixed_sidearm_grip(bool on, bool save);  // save: also written to the user ini (the menu; "skel grip on|off" is the session's)
int transplant_fingers();
void set_transplant_fingers(int mode);  // also written to the user ini
// The visibility build (camera_lever.cpp's hook, the update thread, just before the game builds its draw records):
// the item in hand's game matrix is sampled, so the draws of its prop are known by that matrix ([Body] HeldPropFix).
void on_visibility_build();
// (round 136) how far (m) the visibility build's camera is drawn back along the view, 0 not: while props are drawn in the
// drawn hands away from the game's (a table game, a held horseshoe), so that the frame's list has what is by the eyes
float vis_reach();
// The drawn body's points for the holsters (world, from the latest corrected player set): each hand's wrist target
// (where the IK puts the wrist: the controller), the holster bones and the root joint. False when stale (250 ms).
// 6 the lasso (its rope bone: the Lasso holster, 2026-10-13)
constexpr int kHolsterBones = 7;  // 0 pistol (right hip), 1 rifle (back), 2 thrower, 3 melee (pelvis), 4 the chest (spine03), 5 the lower back (the pelvis bone)
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
    int gun = 1;               // John's gun hand (0 left-handed)
    uint64_t frame = 0;        // the body frame these were published in (2026-10-29: aim.cpp's barrelstep2 pairs by it)
    uint64_t build = 0;        // (round 114) the visibility build that frame drew (its newest held sample's; 0 unknown)
    // run 5 (RoundInHand): each hand's thumb and index fingertip joints as drawn (thumb_03, finger_13; world)
    bool tips_ok[2] = {false, false};
    float thumb_tip[2][3] = {};
    float index_tip[2][3] = {};
    // 2026-10-27 (cycle r171): the hand holding a round with a long gun in the other: its fingers given the pinch learned
    // with a sidearm (round_pinch), or, none learned yet, left flat as the game draws them (round_flat: round_draw's
    // fallback along the index finger)
    bool round_pinch[2] = {false, false};
    bool round_flat[2] = {false, false};
    int ctrl[2] = {0, 1};      // the controller each of John's hands follows
    // run 6 item 9g: the drawn long gun's own frame (world; its axes as columns: x right, y up, z back), for the zones
    // of its parts (actions.cpp: the single-shot breeches)
    bool gun_frame_ok = false;
    float gun_frame_R[9] = {};
    float gun_frame_o[3] = {};
    // (2026-11-01, the off-hand build step 7; r80 G4) a long gun drawn mirrored in John's left hand (a transplant): its
    // frame as gun_frame's (a rotation: the model's points as they are). Only the highlights' part points read it (the
    // zones above keep gun_frame's: unchanged for the left-drawn guns)
    bool gun_mframe_ok = false;
    float gun_mframe_R[9] = {};
    float gun_mframe_o[3] = {};
    // (2026-10-29, [Hands] GunCollision) each arm as drawn: its elbow, wrist and the knuckles' middle (world), and the
    // collision's move of that hand as drawn then (world; 0: none): the arm as held is the drawn less it
    bool limb_ok[2] = {false, false};
    float elbow_d[2][3] = {}, wrist_d[2][3] = {}, knuckle_d[2][3] = {};
    float hand_push[2][3] = {};
};
bool body_points(BodyPoints* out);
// [Holsters] InteractionHighlights (2026-10-29): the gun's moving part worked by the driver (actions::Driver) as the
// drawn parts' table has it: its hinge's point in the gun's own frame (m: x right, y up, z back; body_points'
// gun_frame), the first hinged part of that driver; false when the gun has none. A constant table: any thread.
bool gun_part_point(int weapon, int driver, float out[3]);
// [Holsters] InteractionHighlights=parts (run 29 item 2, research\run29\glow.md): the gun's places glowing on its own
// surface (each lit place's triangles drawn again after the game's draw of the gun in hand, the second gun or the copy;
// pso.h glow plays them). part_glow_live: a glow draw recorded for place id (holster::HighlightId) within 100 ms (zone_rings
// leaves its sprite out). Test forms (the session; "holster glow"): glow_force_off (no glow recorded: the tests' mask frame),
// glow_passes (the DrawVisEntity passes recorded in, a bit each; 0xfe by default: all but the shadows'), glow_reset (the
// counters), glow_status (the recording's counters and, per place, its triangles and where each eye sees them). Any thread.
bool part_glow_live(int id);
void glow_force_off(bool off);
void glow_passes(uint32_t mask);
void glow_reset();
std::string glow_status();
// The game thread: the player's item in hand W as the mod placed it at the drawn hand in this game frame's update (its
// prop's world matrix, 4x4 rows), false for another W or none placed within 100 ms (dual.cpp: its idle effect follows it)
bool placed_item(uintptr_t W, float m[16]);
// [Hands] CarryByForegrip: the long gun in hand W held two-handed, as drawn in its last update (within 100 ms): P (4x4
// rows, corrected space). False for another W, none that recent, not fully two-handed, or the transplant. Any thread.
// hand0, rot0: John's front (left) hand's target at that update (body_points: its place, its axes as columns); false
// without it
bool two_hand_P(uintptr_t W, float P[16], float hand0[3], float rot0[9], int fj = -1);
// [Reload] CarriedBolt (actions.cpp): the carried gun's own frame (world; its axes as columns: x right, y up, z back), as
// the game update last placed it at the front hand, within 150 ms; false without a carry placed. Any thread.
// [Hands] CarryEaseMs: the carried gun eased from its last two-handed place over this (ms)
float carry_ease_ms();
double carry_ease_end();  // the current carry's ease ends then (log::now_ms)
// (C37b) drawn: the carried gun as drawn while it is (else, and drawn false, where the update placed it: a frame paired
// with this frame's body points, the hand-fixed frames learned from it)
bool carry_frame(float R[9], float o[3], bool drawn = true);
// (2026-11-02, GOAL-offhand-changes C15g) the gun in hand steadied through John's fire clip now (its model's kSteady, the clip
// playing): the aim's barrel-in-target is kept as before the shot meanwhile (the clip swings the game's gun). Any thread.
bool steadied_fire_clip();
// [Hands] CarryByForegrip's test aid ("skel carry old on|off", the session, off): the carry as before 2026-10-28 (its
// put-away drawn on the gun hand, the gun riding the front hand's move only until T, the take-back zone on the front
// controller), for tests/sim/r32_carry_move.py's control ("skel carry olddelta on|off": only the put-away's draws moved
// by the wrist's correction, as c4279ad). Any thread.
bool carry_old_test();
// (2026-11-03, round 77) a mod put-away of the item in hand begins (holster.cpp, before its ACTOR_PUT_ITEM_AWAY): its draws
// on the way to the holster hidden (the game's clip not seen; John not moved, as the instant holster did). Any thread
void put_away_leaving(int weapon);
// (round 78) W is the leaving item's weapon object (its put-away under way): its held effect (a flame) not shown. Any thread
bool leaving_W(uintptr_t W);
std::string put_away_leaving_status();  // ("holster leaving") its counts
// ("holster hold subst on|off", a test's) a leaving item drawn at its holstered place (as rounds 79-81) instead of hidden
void set_leave_subst(bool on);
// [Body] HiddenGeometry: 0 draw (the hidden parts drawn collapsed, as before), 1 skip (the geometries with every bone
// hidden not drawn), 2 filter (also the mixed ones drawn without their hidden triangles that span two cuts: the sliver)
int hidden_geometry();
void set_hidden_geometry(int mode);  // also written to the user ini
const char* grip_source_name();  // the foregrip's grip now: "the weapon's", "<gun>'s (borrowed)", "the template", "built-in"
bool auto_shows();           // [Body] AutoShow: forearms and hands while crouching, in cover, riding or driving
// Run 12 item 2, [Holsters] OwnBelt: John's own belt's pieces, each on its own bone (the outfit's geometry weighted to
// it: research/run12/ownbelt/pieces.md), moved and turned in the drawn pelvis's frame about the bone's own joint. The
// bone and every bone under it take the move in both halves of the player's set, so the piece's vertices, any prop
// hung on it (the holstered gun, the knife) and its holster point go with it. Off by default.
constexpr int kPieces = 5;  // 0 the holster (pistol), 1 the knife's sheath (melee), 2 the satchel (thrower), 3 the lasso (rope), 4 the rifle's sling (rifle)
struct Piece {
    float d[3] = {};     // m: right, up, forward (the pelvis's frame)
    float turn[3] = {};  // deg: tilt (about right), turn (about up), lean (about forward)
};
const char* piece_key(int i);  // the ini's stem: Holster, Sheath, Satchel, Lasso, Sling
Piece piece(int i);
void set_piece(int i, const Piece& p, bool save);  // save: the user ini, only the changed keys
bool own_pieces();
// [Holsters] OwnBeltRigid (2026-10-13): a moved piece drawn whole where it was put (its model drawn twice); off by default
// since the user's body turned translucent with it on
bool rigid_pieces();
void set_rigid_pieces(bool on, bool save);
void set_own_pieces(bool on, bool save);
// The piece's joint as drawn (world) in the last frames; false when not drawn moved (OwnBelt off) or stale
bool piece_where(int i, float joint[3]);
// (GOAL-cattleman-belts item 2) whether this outfit has piece i (its geometry found; the lasso: its bone in the rig)
bool piece_present(int i);
// (round 103) piece i's bone as drawn (world, the eyes' latest player set: its axes as columns, then its joint), and the
// pelvis joint that same set drew (pelvis: 3 floats, optional: a prop's anchor of the same frame); false: none drawn lately
// (an outfit without the bone: the pelvis's matrix with the default rig's rest joint)
bool piece_bone_drawn(int i, float W[12], float* pelvis = nullptr);
// (round 103, the user: "Get the exact default satchel") [Holsters] OwnBeltProps: the default outfit's own model made as a
// prop (held_prop::kBeltModelSlot) draws only these of its pieces (bits 1 the knife's sheath, 2 the satchel), each on this
// outfit's bone as drawn; 0: none
void set_belt_model_pieces(uint32_t mask);
// (round 121) the outfit's own satchel hidden (its piece's bones collapsed in John's sets) while the belt's throwable is
// shown at its place
void set_hide_satchel(bool on);
uint64_t satchel_hidden_draws();
bool piece_delta(int i, float d[3]);  // its joint's move this frame (world: where the move put it less where it was), 0 unmoved
// its move as a rigid one (world): its joint before (piv), after (want), its turn Rw (a point p on it at want + Rw (p - piv))
bool piece_rigid(int i, float piv[3], float want[3], float Rw[9]);
void save_piece(int i);  // its OwnBelt<Key>Offset and Turn written to the user ini as they are now
// Run 12 item 5, [Holsters] OwnBeltLeftHolster: John's own holster (the outfit's geometry weighted to the pistol bone)
// drawn a second time at the left hip for the second sidearm: reflected across its own thin plane and across the
// body's middle (two reflections: a rotation, so its triangles keep their winding), the rest of that geometry (the
// belt's ring) collapsed to the holster's top; its own place and turn (the Piece's) about its joint. Off by default.
bool left_holster();
bool gun_steady();  // [Hands] GunSteady (2026-11-02): the gun in hand held still against the world floats' steps
void set_gun_steady(bool on, bool save);
bool no_flutter();  // [Body] NoFlutter (2026-11-02): no cloth flutter on any player draw
void set_no_flutter(bool on, bool save);
// [Body] ClothPins (2026-11-06, off by default; round 122's menu row): the duster's coat drawn on the drawn body (each part by
// the body's change at its height, hips to chest), not the game's walk and idle; ClothFollows stands down while it is on
bool cloth_pins();
void set_cloth_pins(bool on, bool save);
// [Body] ClothStill (round 124, seconds, 0 off): with ClothPins, the coat's points eased in the drawn hips' frame, so the game's
// walk and idle sway are smoothed out
float cloth_still();
void set_cloth_still(float seconds, bool save);
bool arm_motion_vectors();  // [Body] ArmMotionVectors
void set_arm_motion_vectors(bool on, bool save);
void set_left_holster(bool on, bool save);
Piece left_holster_piece();
void set_left_holster_piece(const Piece& p, bool save);
// 2026-10-13: the copy taken by hand: its joint as drawn (world), and its move from the grip's (as piece_grab)
bool left_holster_where(float j[3]);
void save_left_holster_piece();  // both its keys written (Offset, Turn)
bool left_holster_grab(const float Rd[9], const float h0[3], const float h1[3], const Piece& p0, Piece* out);
bool left_holster_delta(float d[3]);  // its joint's move from the plain mirror of the right one (world), for the LeftHip zone
bool left_holster_rigid(float piv[3], float want[3], float Rw[9]);  // the copy's own move, as piece_rigid
bool left_holster_move(float tw[3]);  // the copy's move (world, from the pelvis's frame), drawn or not
// a piece's (or the left holster's) offset change as a world move (to less from), and a piece shifted by a world move
bool piece_shift_world(const Piece& from, const Piece& to, float dw[3]);
bool piece_shifted(const Piece& from, const float dw[3], Piece* to);
// A point at the right holster (world: the RightHip zone) mirrored onto the copy as the copy is made (its mirror plane,
// then the copy's own move): the LeftHip zone on it. False when the copy is off or not drawn lately
bool left_holster_place(const float right[3], float out[3]);
void left_holster_refresh();
// [Holsters] OwnBeltRigid (2026-10-13): each moved piece drawn rigid (its geometries found again when the drawables change)
void pieces_geom_refresh();
bool pieces_geom_scan();          // the holster geometry found again when the player's drawables change (rd: not the draw thread)
std::string left_holster_status();  // "skel lefthol"
// Item 3's grab: the piece's move after a hand's move since its grip (the hand turned by Rd about it, from h0 to h1; world)
// applied to the piece as it was at the grip (p0): the new offset and turn in the pelvis's frame (each offset within 0.6 m)
bool piece_grab(int i, const float Rd[9], const float h0[3], const float h1[3], const Piece& p0, Piece* out);
void set_auto_shows(bool on);
// (round 117) [Body] HiddenBodyShadow: the hidden body's shadow in a show mode (on: John's whole shadow; off, the default:
// none of him in the light's views)
bool hidden_body_shadow();
void set_hidden_body_shadow(bool on);
// (round 128) [Body] TableGameArms (off): at the table games without the minigame's flag, John's arms as the game animates them
bool table_game_arms();
void set_table_game_arms(bool on);
// (round 129) John's root (bone 0, world) as the newest visibility build read it; false: not read
bool build_root(float out[3], uint64_t* build = nullptr);
// [Horse] BodyWithCamera (run 12 item 8): riding with Stance=still, the body kept at its standing offset from the camera
// (only a jump over 1.5 m re-learns it, not 0.3 m; the animation pinned while within 1.5 m, not 0.5 m). On by default;
// "pose bodycam on|off" for the session
void set_body_with_camera(bool on, bool save = false);
bool body_with_camera();
// [Weapon.<Gun>] FrontHandPose: a long gun's front hand: 0 automatic (the game's state), 1 the lowered hold, 2 the
// aiming hold (each with its own fingers); front_pose_seen: bit 0/1 the lowered/aiming hold learned this session, bit
// 2/3 its fingers
int front_pose(int w);
int front_pose_seen(int w);
void set_front_pose(int w, int pose, bool save = true);
bool locks_torso();          // [Body] LockTorso: the upper body kept over the hips horizontally (running leans)
void set_locks_torso(bool on);
HandCfg hand_cfg();
void set_hand_cfg(const HandCfg& c);
std::string command(const std::string& line);
// The scene render hook, first thing (after the game's update, before its draw lists): the head hide when on.
void before_scene(const float* cam);  // cam: the frame's game camera matrix (for the controllers' world poses)

}  // namespace rdrvr::body
