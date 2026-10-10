#include "core/holster.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>

#include "core/actions.h"
#include "core/aim.h"
#include "core/anchors.h"
#include "core/api.h"
#include "core/audio.h"
#include "core/body.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/controllers.h"
#include "core/controls.h"
#include "core/d3d_hooks.h"
#include "core/dual.h"
#include "core/hands.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/menu.h"
#include "core/pose.h"
#include "core/reload.h"
#include "core/round_draw.h"
#include "core/held_prop.h"
#include "core/wheel.h"
#include "core/whistle.h"

namespace rdrvr::holster {
namespace {

std::atomic<bool> g_keep{true};          // [Hands] KeepGunDrawn (on with the body holsters)
std::atomic<uintptr_t> g_player_ped{0};  // the player's ped (actor +0x38), refreshed every frame
std::atomic<bool> g_aim_pose{false};      // the gun controller's +0x5d6 bit 0x40, at the last frame end
std::atomic<bool> g_fire_ready{false};
std::atomic<float> g_fire_phase{-1.0f};  // the gun controller's fire clip (G +0x24): its phase, below 0 when none plays    // FUN_140d16400's fire test, at the last frame end
std::atomic<bool> g_raised{false}, g_gun_at_zone{false};
// run 8 item 2: the foregrip ring in the drawn gun's frame (R^T (ring - o)), each holster frame with a long gun
struct ForeTrace {
    int weapon = -1;
    uint64_t frames = 0, jumps = 0;  // jumps: over 5 mm from the frame before
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f}, last[3] = {}, max_step = 0.0f;
    bool have = false;
};
std::mutex g_fore_mutex;
ForeTrace g_fore_trace;
float g_log_ref[3] = {};
int g_log_weapon = -1;
double g_log_ms = -1e12;
void fore_trace(int weapon, const float* R, const float* o, const float* ring, double now) {
    float d[3], g[3];
    for (int k = 0; k < 3; ++k) d[k] = ring[k] - o[k];
    for (int i = 0; i < 3; ++i) g[i] = R[0 * 3 + i] * d[0] + R[1 * 3 + i] * d[1] + R[2 * 3 + i] * d[2];  // the columns: the gun's axes
    {
        std::lock_guard lock(g_fore_mutex);
        ForeTrace& t = g_fore_trace;
        if (weapon != t.weapon) t = ForeTrace{}, t.weapon = weapon;
        if (t.have) {
            const float s = std::sqrt((g[0] - t.last[0]) * (g[0] - t.last[0]) + (g[1] - t.last[1]) * (g[1] - t.last[1]) + (g[2] - t.last[2]) * (g[2] - t.last[2]));
            if (s > 0.005f) ++t.jumps;
            if (s > t.max_step) t.max_step = s;
        }
        for (int k = 0; k < 3; ++k) {
            t.lo[k] = std::fmin(t.lo[k], g[k]);
            t.hi[k] = std::fmax(t.hi[k], g[k]);
            t.last[k] = g[k];
        }
        t.have = true;
        ++t.frames;
    }
    // the log: a move over 1 cm in the gun's frame since the last line (once a second at most), the weapon kept
    if (weapon != g_log_weapon) {
        g_log_weapon = weapon;
        std::memcpy(g_log_ref, g, sizeof(g));
        return;
    }
    const float m = std::sqrt((g[0] - g_log_ref[0]) * (g[0] - g_log_ref[0]) + (g[1] - g_log_ref[1]) * (g[1] - g_log_ref[1]) +
                              (g[2] - g_log_ref[2]) * (g[2] - g_log_ref[2]));
    if (m > 0.01f && now - g_log_ms > 1000.0) {
        g_log_ms = now;
        log::info("[holster] the foregrip ring moved %.0f mm in the gun's frame (weapon %d): now (%.3f %.3f %.3f), the grip from %s", m * 1000.0f,
                  weapon, g[0], g[1], g[2], body::grip_source_name());
        std::memcpy(g_log_ref, g, sizeof(g));
    }
}
std::atomic<uint64_t> g_raises{0};
float g_raise_pmin = -35.0f, g_raise_pmax = 60.0f, g_raise_reach = 0.25f;  // [Hands] RaisedPitchMin/Max, RaisedReach
std::atomic<float> g_raise_dbg[4] = {};  // the last frame's pitch, reach, ahead, and the conditions (bits) for "holster"
std::atomic<bool> g_unarmed{true};        // [Holsters] UnarmedAfterHolster
std::atomic<bool> g_show_zones{false};    // [Holsters] ShowZones: the holster rings (run 8 item 5b: those only)
std::atomic<bool> g_zones_near{false};    // [Holsters] ZonesNear (2026-10-09): a ring only with a hand near it
std::atomic<float> g_near_dist{0.25f};    // [Holsters] ZonesNearDistance: how near (m, past the ring's radius)
std::atomic<bool> g_steady_zones{false};  // [Holsters] SteadyZones (2026-10-09): the zones' edges with hysteresis
constexpr float kZoneExit = 0.03f, kZoneSticky = 0.03f;  // SteadyZones: past the radius to leave; nearer to change
std::atomic<bool> g_place{false};  // placing the holsters by hand (the menu; the session only)
std::atomic<uint64_t> g_placed{0};  // holsters moved by hand and saved
void save_zone(int i);              // (below) a zone's offset and radius into the user ini
std::atomic<bool> g_ring_gun_axes{true};
std::atomic<bool> g_steady_ring{true};  // [Reload] SteadyRing (run 8 item 2): the ring's place on the gun locked after the draw
std::atomic<uint64_t> g_ring_locks{0};  // [Reload] RingInGunAxes (run 8 item 2): the ring's offset along the drawn gun's axes
std::atomic<bool> g_show_dots{false};     // [Holsters] ShowHandDots (run 8 item 5b; absent: ShowZones')
std::atomic<bool> g_show_points{false};   // [Holsters] ShowWeaponPoints: the foregrip ring, the load point, the action hints
// [Holsters] ReleaseMargin: a grip pressed in a zone counts as let go there within this times its radius (run 4: with a
// gun in hand the drawn wrist sits near the hip zone's edge, and a release drifted just outside it)
std::atomic<float> g_release_margin{1.3f};
float g_fore_off[3] = {0, 0, 0};          // [Reload] ForegripOffset (right, up, forward), under g_zone_mutex
std::atomic<float> g_grab_off[3] = {};     // [Hands] InteractOffset (right, up, forward)
struct GunAdjSet {  // round 13 item 8: one gun's own adjustments (kAdjForegrip, kAdjForeRing, kAdjLoadRing), under g_zone_mutex
    bool own[3] = {};
    float off[3][3] = {};
    float radius[3] = {};
};
constexpr int kAdjGuns = 21;  // the guns (0-20)
GunAdjSet g_gun_adj[kAdjGuns];
std::atomic<int> g_gun_in_hand{-1};
const char* const kAdjKey[3][2] = {{"ForegripOffset", nullptr}, {"ForegripRingOffset", "ForegripRingRadius"}, {"LoadPointOffset", "LoadPointRadius"}};
float g_fore_radius = 0.20f;              // [Reload] ForegripRadius (since run 6 only the zone radius's default)
// run 6 item 4: the foregrip ring apart from the grip: where the front hand engages is the grip point (the game's grip
// moved by ForegripOffset, where John's hand sits) moved again by [Reload] ForegripZoneOffset (right, up, forward in the
// gun's axes), within [Reload] ForegripZoneRadius (ForegripRadius when the key is absent). Under g_zone_mutex.
float g_fzone_off[3] = {0, 0, 0};
float g_fzone_radius = 0.20f;
// run 6 item 3: where a round goes into the gun in hand: the loading point (0.10 m out along the barrel from the gun
// hand's wrist) moved by [Reload] LoadPointOffset (right, up, forward in the gun's axes, m), the ring's radius
// [Reload] LoadPointRadius (a round held within it goes in; let go within it + 0.04 m), and [Reload] InsertOnTouch: the
// drawn round itself (its pose, not the wrist) within the radius puts it in, the grip still held (on by default, round
// 13 item 11). Under g_zone_mutex.
float g_load_off[3] = {0, 0, 0};
float g_load_radius = 0.12f;
bool g_insert_touch = true;
struct LoadDiag {  // "holster load": the last frame's point and the held round's distances (-1: none)
    float pt[3] = {}, round[3] = {}, axis[3] = {};  // the round's position and its axis (base to tip)
    float wrist_d = -1.0f, round_d = -1.0f;
    bool round_ok = false;
    uint64_t inserts_touch = 0, inserts_wrist = 0, inserts_let_go = 0;
} g_load_diag;
std::atomic<bool> g_fore_snap{true};      // [Reload] TwoHandedSnap
Markers g_markers{};                      // under g_hdiag_mutex
bool g_markers_valid = false;
double g_markers_ms = 0;
std::atomic<uint64_t> g_unarmed_sets{0};
std::atomic<uint64_t> g_player_triggers{0};

// Game memory the action trees themselves work from, read and written with SEH as the backstop (no system calls:
// every actor's transition ops run through the hooked update every frame).
bool raw(uintptr_t a, void* out, size_t n) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
bool rd(uintptr_t a, T* out) {
    return a && raw(a, out, sizeof(T));
}

template <class T>
bool wr(uintptr_t a, T v) {
    __try {
        *reinterpret_cast<T*>(a) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The player's actor (the object's +0xb0 handle in the actor pool), as body.cpp finds it.
uintptr_t player_actor();
uintptr_t holster_player_actor() { return player_actor(); }
uintptr_t player_actor() {
    RdrvrActorState st{};
    if (!api::actor_state(&st) || !st.object) return 0;
    uint32_t h = static_cast<uint32_t>(st.object);
    uintptr_t pool = 0, obj = 0, apool = 0, actor = 0;
    uint16_t gen = 0, idx = 0, agen = 0, sgen = 0;
    if (!rd(anchors::addr(anchors::Id::ObjectsPool), &pool) || !pool) return 0;
    uintptr_t slot = pool + static_cast<uintptr_t>(h & 0xffff) * 0x10;
    if (!rd(slot + 8, &gen) || gen != static_cast<uint16_t>(h >> 16) || !rd(slot, &obj) || !obj) return 0;
    if (!rd(obj + 0xb0, &idx) || !rd(obj + 0xb2, &agen) || !rd(anchors::addr(anchors::Id::ActorPool), &apool) || !apool) return 0;
    if (!rd(apool + idx * 0x10u + 8, &sgen) || sgen != agen || !rd(apool + idx * 0x10u, &actor)) return 0;
    return actor;
}

// An op belongs to the player when its context (op +0x28) has the player's ped at +0x30; the context's +0x10 is the
// running node instance, whose +0x18 is the time in the node and +0xc8 the list of running ops {vtable, next, ...}.
bool player_op(uintptr_t op, uintptr_t* node, float* t) {
    uintptr_t ctx = 0, ped = 0;
    const uintptr_t pp = g_player_ped.load(std::memory_order_relaxed);
    if (!pp || !rd(op + 0x28, &ctx) || !rd(ctx + 0x30, &ped) || ped != pp) return false;
    *node = 0;
    *t = -1;
    if (rd(ctx + 0x10, node) && *node) rd(*node + 0x18, t);
    return true;
}

// The gun-ready node: the running node holds the aim-pose op (vtable 0x141d4c1b8; it sets gun +0x5d6 bit 0x40 on
// start and clears it on exit, and the gun state machine, FUN_140d198f0 state 8, holsters once that bit is clear).
bool has_aim_op(uintptr_t node) {
    const uintptr_t aim_vtbl = anchors::addr(anchors::Id::AimPoseOpVtbl);
    uintptr_t o = 0;
    if (!rd(node + 0xc8, &o)) return false;
    for (int g = 0; o && g < 24; ++g) {
        uintptr_t vt = 0, next = 0;
        if (!rd(o, &vt) || !rd(o + 8, &next)) return false;
        if (vt == aim_vtbl) return true;
        o = next;
    }
    return false;
}

int32_t gun_state(uintptr_t ped) {
    uintptr_t anim = 0, gun = 0;
    int32_t state = -1;
    if (ped && rd(ped + 0xaa8, &anim) && rd(anim + 8, &gun)) rd(gun + 0x3bc, &state);
    return state;
}

// ---- the block. The player's weapon controller (FUN_14034cae0, called each frame with the player info): in its
// state 1 (+0x504, a gun in hand) it counts +0x3e0 down by the frame time; aiming, firing and the game's "in combat"
// flags set it back to the draw time (DAT_142b24a58, set from the tuning at load); when it reaches 0 the gun is put
// away (FUN_140340080, the weapon manager's +0x80 hand slot cleared; ConditionWeaponDrawn then turns false and the
// action trees play the holster) and the state goes to 2. The game's own holster inputs go through FUN_14034ca10
// (state 4) and are untouched. With KeepGunDrawn the countdown is topped up before each update while a gun is drawn.
using PlayerWeapon_t = void (*)(uintptr_t info, int mode);
PlayerWeapon_t o_player_weapon = nullptr;
std::atomic<uint64_t> g_topped{0};
std::atomic<float> g_reset_seen{0.0f};

void hk_player_weapon(uintptr_t info, int mode) {
    if (g_keep.load(std::memory_order_relaxed) && mode != 6) {
        int32_t st = 0;
        float t = 0, reset = 0;
        if (rd(info + 0x504, &st) && st == 1 && rd(info + 0x3e0, &t) && rd(anchors::addr(anchors::Id::DrawTimeReset), &reset)) {
            g_reset_seen.store(reset, std::memory_order_relaxed);
            // only while it counts down (> 0): it is 0 while the aim is held, and Dead Eye starts only at 0
            const float want = reset > 0.5f ? reset : 3.0f;
            if (t > 0.0f && t < want && wr<float>(info + 0x3e0, want)) g_topped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    o_player_weapon(info, mode);
}


// ---- the body holsters ([Holsters], the menu's Holsters tab): draw a weapon by gripping at its holster on the drawn
// body, put the gun in hand away by gripping and letting go at its holster. A zone sits at one of the drawn body's
// holster bones (body::BodyPoints: pistol on the right hip, rifle on the back, thrower and melee on the pelvis) plus an
// offset in the body frame (right, up, forward, on the body heading), with a radius; the hand is the IK's wrist
// target (the controller). A grip used at a holster is the holster's: controls does not pass it on as LB/RB until it
// is let go. The draw is the game's own: GET_WEAPON_EQUIPPED names the slot's weapon, then ACTOR_PUT_WEAPON_IN_HAND
// (its equip task puts a different gun in hand away first; [Holsters] InstantDraw: at once, else with the draw clip).
// The holster too (ACTOR_PUT_ITEM_AWAY). The slot selection natives do not draw on the same tick (the current slot,
// wmgr +0x448, follows a tick later), so the grip at a holster drew nothing the first time.
struct Zone {
    const char* key;    // the ini prefix
    const char* label;  // the menu's
    int bone;           // body::BodyPoints::bone index
    int slots[3];       // the equip slots it draws, in order (-1 none)
    float off[3];       // right, up, forward (m)
    float radius;
    int weapon = -1;    // [Holsters] <Key>Weapon: the eWeapon it draws, -1 automatic (its slots in order)
    bool mirror = false;   // its bone mirrored across the body's middle (the left twins of the right hip and the back)
    int partner = -1;      // automatic: another owned weapon than this zone's (the twin's), when there is one
    bool enabled = true;   // [Holsters] <Key>Enabled
};
Zone g_zones[kZones] = {
    {"RightHip", "Right hip (sidearm)", 0, {1, -1, -1}, {0.0f, 0.0f, 0.0f}, 0.15f, -1},
    {"Back", "Back (long gun)", 1, {5, 6, 4}, {0.0f, 0.05f, 0.0f}, 0.25f, -1},
    {"Belt", "Belt (thrown)", 2, {2, -1, -1}, {0.0f, 0.0f, 0.0f}, 0.12f, -1},
    {"Knife", "Left hip (knife and lasso; off: moved to the lower back)", 3, {0, 3, -1}, {0.0f, 0.0f, 0.0f}, 0.12f, -1, false, -1, false},
    {"Ammo", "Chest (a round to reload; the gun hand squeezed here reloads)", 4, {-1, -1, -1}, {0.0f, 0.0f, 0.15f}, 0.15f, -1},
    // round 8: "another long holster behind the left shoulder, another pistol holster on the left hip, move the melee
    // holster to the lower back". The twins mirror the right hip's and the back's bone and offset (unless set).
    {"LeftHip", "Left hip (a second sidearm)", 0, {1, -1, -1}, {0.0f, 0.0f, 0.0f}, 0.15f, -1, true, 0, true},
    {"LeftShoulder", "Back, left shoulder (a second long gun)", 1, {5, 6, 4}, {0.0f, 0.05f, 0.0f}, 0.25f, -1, true, 1, true},
    {"LowerBack", "Lower back (knife and lasso)", 5, {0, 3, -1}, {0.0f, 0.05f, -0.12f}, 0.16f, -1},
};
// eWeapon order (WeaponModel 0..37): the ini's tokens and the menu's labels
const char* const kWeaponToken[kWeapons] = {"Volcanic", "SemiAutoPistol", "HighPower", "Mauser", "Cattleman", "Schofield", "DoubleAction", "LeMat", "Carbine", "Winchester", "Henry", "Evans", "Springfield", "BoltAction", "Buffalo", "SawedOff", "DoubleBarrel", "PumpAction", "SemiAutoShotgun", "RollingBlock", "Carcano", "Lasso", "Knife", "FireBottle", "Dynamite", "ThrowingKnife", "Gatling", "Browning", "Cannon", "Tomahawk", "Bow", "ExplosiveRifle", "ZombieSpit", "Torch", "Blunderbuss", "HolyWater", "ZombieBait", "BoomBait"};
const char* const kWeaponLabel[kWeapons] = {"Volcanic Pistol", "Semi-Automatic Pistol", "High Power Pistol", "Mauser Pistol", "Cattleman Revolver", "Schofield Revolver", "Double-Action Revolver", "LeMat Revolver", "Carbine Repeater", "Winchester Repeater", "Henry Repeater", "Evans Repeater", "Springfield Rifle", "Bolt Action Rifle", "Buffalo Rifle", "Sawed-off Shotgun", "Double-barreled Shotgun", "Pump-action Shotgun", "Semi-Auto Shotgun", "Rolling Block Rifle", "Carcano Rifle", "Lasso", "Knife", "Fire Bottle", "Dynamite", "Throwing Knife", "Gatling Gun", "Browning Gun", "Cannon", "Tomahawk", "Bow", "Explosive Rifle", "Zombie Spit", "Torch", "Blunderbuss", "Holy Water", "Zombie Bait", "Boom Bait"};
std::atomic<bool> g_weapon_choice{true}, g_any_weapon{false};
// [Holsters] ShowGuns (off; run 6 item 6a): each gun zone's gun shown at it (held_prop slots 2-5), hidden while in a hand
std::atomic<bool> g_show_guns{false};
// [Holsters] ShowBackGuns (on; round 13 item 14): off, ShowGuns leaves out the back and the left shoulder's long guns
std::atomic<bool> g_show_back_guns{true};
std::atomic<int> g_anchor{1};  // [Holsters] Anchor: 0 body, 1 headset (run 7 item 1d; the default)
std::atomic<bool> g_anchor_used{false};  // the last update moved the zones with the neck
// [Holsters] TurnWithHead / TurnDeadZone (2026-10-09, the user's request): with the headset anchor, the holsters turned
// with the headset's yaw since recentre (about the neutral head), once it is more than the dead zone from where they point
std::atomic<bool> g_turn_head{false};
std::atomic<float> g_turn_dead{0.0f}, g_zone_yaw_pub{0.0f};
// [Holsters] LeanSteady, TorsoLength, TurnByHands (2026-10-09, the holsters' polish)
std::atomic<bool> g_lean_steady{false}, g_turn_hands{false};
std::atomic<float> g_torso_len{0.55f}, g_lean_pub{0.0f}, g_hands_yaw_pub{-999.0f};
float g_anchor_shift[3] = {0, 0, 0};    // that move (world, m); under g_zone_mutex
float g_anchor_cam[3] = {0, 0, 0};      // the frame camera's position (the neutral head) then; under g_zone_mutex
const int kShowZones[4] = {0, 1, 5, 6};  // the right hip, the back, the left hip, the left shoulder
// [Holsters] ShowModels (2026-10-09, the user's request): a holster model at the hips (held_prop kModelSlot + i), the
// game's own prop by its fragment name ([Holsters] Model), turned by ModelTurn (degrees about the holster's up, right,
// forward) and moved by ModelOffset (m: right, up, forward; the left hip's right mirrored, its turn about up reversed)
const int kModelZones[2] = {0, 5};  // the right hip, the left hip
std::atomic<bool> g_show_models{false};
char g_model_name[64] = "p_gen_gunbelt01x";
float g_model_off[3] = {0, 0, 0}, g_model_rot[3] = {90, 90, 0};  // under g_zone_mutex
float g_model_raw[3] = {0, 0, 0};  // "holster model raw a b c": the native's angles as given (the axes' test)
bool g_model_raw_on = false;       // under g_zone_mutex
// The model's pose at a hip (12 floats: X, Y, Z columns, then the position): the holster's frame (X right, Y forward,
// Z up) turned by the model's turn, at the zone moved by its offset
void model_pose(int i, const float* at, const float* rgt, const float* up, const float* fwd, const float* off, const float* rot, float* pz) {
    const float side = i == 0 ? 1.0f : -1.0f;
    const float ya = side * rot[0] * 0.0174532925f, pa = rot[1] * 0.0174532925f, ra = side * rot[2] * 0.0174532925f;
    // R = Rz(yaw) * Rx(pitch) * Ry(roll) in the holster's (right, forward, up) basis: its columns, then to the world
    const float cy = std::cos(ya), sy = std::sin(ya), cp = std::cos(pa), sp = std::sin(pa), cr = std::cos(ra), sr = std::sin(ra);
    const float Rz[9] = {cy, -sy, 0, sy, cy, 0, 0, 0, 1}, Rx[9] = {1, 0, 0, 0, cp, -sp, 0, sp, cp}, Ry[9] = {cr, 0, sr, 0, 1, 0, -sr, 0, cr};
    float T[9], M[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) T[r * 3 + c] = Rx[r * 3 + 0] * Ry[0 * 3 + c] + Rx[r * 3 + 1] * Ry[1 * 3 + c] + Rx[r * 3 + 2] * Ry[2 * 3 + c];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) M[r * 3 + c] = Rz[r * 3 + 0] * T[0 * 3 + c] + Rz[r * 3 + 1] * T[1 * 3 + c] + Rz[r * 3 + 2] * T[2 * 3 + c];
    const float* B[3] = {rgt, fwd, up};  // the holster's basis vectors (world)
    for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 3; ++k) pz[c * 3 + k] = B[0][k] * M[0 * 3 + c] + B[1][k] * M[1 * 3 + c] + B[2][k] * M[2 * 3 + c];
    for (int k = 0; k < 3; ++k) pz[9 + k] = at[k] + rgt[k] * off[0] * side + up[k] * off[1] + fwd[k] * off[2];
}
// the shown guns' error: the drawn position against the wanted one of the frame after (the walk's lag), the worst (m)
struct ShowDiag {
    int weapon[4] = {-1, -1, -1, -1};
    float err[4] = {};    // the last frame's: the drawn place on the body against the wanted one (m)
    float worst[4] = {};
    uint64_t stale[4] = {};  // frames shown with no draw of it in the last 100 ms (its draws not moved)
    uint64_t gated[4] = {};  // its model's draws gated out (held_prop)
    uint64_t over[4] = {};   // frames whose error passed 2 cm
    uint64_t frames = 0;
};
ShowDiag g_show_diag;  // under g_zone_mutex
// the gun models (fragments.rpf root/fragments, the 01x variants: the ones the clip and skeleton research read)
const char* gun_fragment(int w) {
    static const char* const kLong[13] = {"repeater_carbine01x", "repeater_winchester01x", "repeater_henry01x", "repeater_evans01x",
                                          "rifle_springfield01x", "rifle_boltaction01x", "rifle_buffalo01x", "shotgun_sawed01x",
                                          "shotgun_doublebarrel01x", "shotgun_pumpaction01x", "shotgun_semiauto01x", "rifle_rollingblock01x",
                                          "rifle_carcano01x"};
    if (w >= 0 && w < 8) return dual::sidearm_fragment(w);
    return w >= 8 && w <= 20 ? kLong[w - 8] : nullptr;
}
// A shown gun's pose at its zone (12 floats: the axes X, Y, Z, then the position; world): at a hip barrel down, its top
// forward (its grip at the zone); on the back diagonal, the barrel up and out over its shoulder, its right side facing
// back, its grip 30 cm down the barrel's line from the zone
void show_pose(int zi, const float* at, const float* rgt, const float* up, const float* fwd, const float* root, float* pz) {
    float X[3], Y[3], Z[3], P[3];
    if (zi == 0 || zi == 5) {
        for (int k = 0; k < 3; ++k) X[k] = rgt[k], Y[k] = fwd[k], Z[k] = up[k], P[k] = at[k];
    } else {
        const float side = (at[0] - root[0]) * rgt[0] + (at[2] - root[2]) * rgt[2] >= 0.0f ? 1.0f : -1.0f;
        float d[3];
        for (int k = 0; k < 3; ++k) d[k] = up[k] * 0.8f + rgt[k] * 0.5f * side;
        const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int k = 0; k < 3; ++k) {
            d[k] /= n;
            Z[k] = -d[k];
            X[k] = -fwd[k];
            P[k] = at[k] - d[k] * 0.30f;
        }
        Y[0] = Z[1] * X[2] - Z[2] * X[1];  // Y = Z x X: a right-handed frame
        Y[1] = Z[2] * X[0] - Z[0] * X[2];
        Y[2] = Z[0] * X[1] - Z[1] * X[0];
    }
    std::memcpy(pz, X, sizeof(X));
    std::memcpy(pz + 3, Y, sizeof(Y));
    std::memcpy(pz + 6, Z, sizeof(Z));
    std::memcpy(pz + 9, P, sizeof(P));
}
std::atomic<uint64_t> g_choice_draws{0};
int weapon_from_token(const std::string& t) {
    if (t.empty() || _stricmp(t.c_str(), "auto") == 0) return -1;
    for (int w = 0; w < kWeapons; ++w)
        if (_stricmp(t.c_str(), kWeaponToken[w]) == 0) return w;
    char* end = nullptr;
    const long v = std::strtol(t.c_str(), &end, 10);
    if (end && !*end && v >= 0 && v < kWeapons) return static_cast<int>(v);
    log::warn("[holster] unknown weapon \"%s\" (automatic)", t.c_str());
    return -1;
}
constexpr int kAmmoZone = 4;
std::atomic<float> g_two_blend{0.0f};  // the two-handed aim's weight (body.cpp), 0..1 over a tenth of a second
std::atomic<uint64_t> g_rounds_taken{0}, g_rounds_in{0}, g_clicks{0}, g_two_grips{0}, g_squeezes{0};
std::mutex g_zone_mutex;  // the zone settings: the menu writes, the frame end reads
std::atomic<bool> g_holsters{true};
std::atomic<bool> g_instant{true};  // [Holsters] InstantDraw
std::atomic<bool> g_consumed[2] = {false, false};
std::atomic<bool> g_wanted[2] = {false, false};  // grip_wanted: as of the last frame end
float g_grip_on = 0.6f;
std::atomic<uint64_t> g_draws{0}, g_puts{0}, g_entries{0};
std::atomic<uint64_t> g_twin_hidden{0};  // run 7 item 1b: frames a twin hip hid the gun its partner shows
// the frame end's last view, for the status line
struct HolsterDiag {
    bool valid = false;
    float zone[kZones][3] = {};
    bool zone_ok[kZones] = {};
    float hand[2][3] = {};
    bool hand_ok[2] = {};
    int in_zone[2] = {-1, -1};
    int cur_slot = -1;
    bool in_hand = false;
};
HolsterDiag g_hdiag;
std::mutex g_hdiag_mutex;

const uint32_t kWeaponEquipped = 0x42C0FAAA;   // GET_WEAPON_EQUIPPED(actor, slot): the slot's eWeapon, -1 none
const uint32_t kPutWeaponInHand = 0x8F4B473D;  // ACTOR_PUT_WEAPON_IN_HAND(actor, eWeapon, animated)
const uint32_t kSetNextWeapon = 0xBFD6D55F;     // ACTOR_SET_NEXT_WEAPON(actor, eWeapon): its slot's object becomes it (not drawn)
std::atomic<uint64_t> g_sec_switches{0}, g_sec_refused{0};
const uint32_t kPutItemAway = 0x13A63AA7;      // ACTOR_PUT_ITEM_AWAY(actor)
// SET_PLAYER_MELEE_MODE_SELECTED: the handler (0x140ad7470) takes only its first argument, the flag (the natives DB's
// "player, mode" order is not the handler's): P +0x3bc bit 5, the weapon wheel's Unarmed (FUN_1403f92e0). Any later
// draw into the hand clears it (FUN_14033d6d0, FUN_140341430).
const uint32_t kMeleeModeSelected = 0xAC1285A3;

void holster_frame() {
    static bool grip_down[2] = {false, false}, pending[2] = {false, false};
    static int zone_in[2] = {-1, -1}, press_zone[2] = {-1, -1};
    static uint64_t draw_req[2] = {0, 0};  // a queued GET_WEAPON_EQUIPPED, answered on a script tick
    static double draw_ms[2] = {0, 0};
    static int draw_slot[2] = {-1, -1};
    static bool round_held[2] = {false, false}, two_hold[2] = {false, false}, trig_down = false;
    static bool two_paused[2] = {false, false};  // [Reload] ManualBreak: the two-handed hold waiting while the gun is open
    static bool sec_pending[2] = {false, false};  // [Hands] DualWield: the second gun's hand let go here puts it back
    // the put-back arms once the hand has left the zone it took the second gun at (round 10: a grip again at the same
    // holster a second after the take put it straight back)
    static bool sec_armed[2] = {false, false};
    static int sec_zone[2] = {-2, -2};  // the zone at the first frame the second gun is out (-1: none); -2: no second gun
    static double last_ms = 0;
    static double flip_since = 0;  // AimWhenRaised's debounce
    const double now_ms = log::now_ms();
    const float dt = last_ms > 0 ? static_cast<float>((now_ms - last_ms) * 0.001) : 0.0f;
    last_ms = now_ms;
    for (int h = 0; h < 2; ++h) {
        const float g = hands::get(h).grip;
        const bool was = grip_down[h];
        grip_down[h] = g > g_grip_on ? true : g < g_grip_on - 0.2f ? false : was;
    }
    body::BodyPoints bp;
    RdrvrActorState st{};
    const uintptr_t actor = holster_player_actor();
    static double points_ms = 0;  // the last frame the body's points were read
    const bool on = g_holsters.load(std::memory_order_relaxed) && pose::anchor_active();
    const bool pts = on && body::body_points(&bp);
    if (pts) points_ms = now_ms;
    if (on && !pts && now_ms - points_ms < 100.0) return;  // a missed read: everything kept as it was for this frame
    if (!pts || !api::actor_state(&st) || !st.actor || !actor) {
        for (int h = 0; h < 2; ++h) {
            g_consumed[h] = false;
            pending[h] = false;
            zone_in[h] = -1;
            draw_req[h] = 0;
            round_held[h] = false;
            two_hold[h] = false;
            g_wanted[h] = false;
        }
        g_raised.store(false, std::memory_order_relaxed);  // no aim stance held for a gun the holsters no longer see
        g_gun_at_zone.store(false, std::memory_order_relaxed);
        flip_since = 0;
        g_two_blend = 0.0f;
        round_draw::set_held(-1, 0);
        held_prop::want(0, nullptr, nullptr);  // out of first person: the shell goes
        held_prop::want(1, nullptr, nullptr);  // and the copy's own model
        for (int i = 0; i < 4; ++i) held_prop::want(held_prop::kHolsterSlot + i, nullptr, nullptr);  // and the holsters' guns
        for (int i = 0; i < 2; ++i) held_prop::want(held_prop::kModelSlot + i, nullptr, nullptr);    // and their models
        held_prop::frame(0);
        std::lock_guard lock(g_hdiag_mutex);
        g_hdiag.valid = false;
        g_markers_valid = false;
        return;
    }
    Zone zones[kZones];
    {
        std::lock_guard lock(g_zone_mutex);
        std::memcpy(zones, g_zones, sizeof(zones));
    }
    const bool lefty = controls::left_handed();
    const float hd = pose::torso_heading_deg() * 0.0174532925f;  // run 9: the horse's, not the ride's turned view
    const float fwd[3] = {-std::sin(hd), 0.0f, -std::cos(hd)}, right[3] = {std::cos(hd), 0.0f, -std::sin(hd)}, up[3] = {0, 1, 0};
    // the holsters' own frame: the body's, or turned with the headset ([Holsters] TurnWithHead, below)
    float hr[3] = {right[0], right[1], right[2]}, hf[3] = {fwd[0], fwd[1], fwd[2]};
    float zp[kZones][3], bases[kZones][3];
    bool zok[kZones];
    for (int z = 0; z < kZones; ++z) {
        // the wheel mode (run 8 item 5): no holster draws or put-aways, the chest's rounds kept
        zok[z] = zones[z].enabled && bp.bone_ok[zones[z].bone] && (z == kAmmoZone || !wheel::wheel_mode());
        float base[3] = {bp.bone[zones[z].bone][0], bp.bone[zones[z].bone][1], bp.bone[zones[z].bone][2]};
        if (zones[z].mirror) {  // the twin's bone: mirrored across the body's middle
            const float d = (base[0] - bp.root[0]) * right[0] + (base[2] - bp.root[2]) * right[2];
            for (int k = 0; k < 3; ++k) base[k] -= 2.0f * d * right[k];
        }
        std::memcpy(bases[z], base, sizeof(base));
        for (int k = 0; k < 3; ++k)
            zp[z][k] = base[k] + right[k] * zones[z].off[0] + up[k] * zones[z].off[1] + fwd[k] * zones[z].off[2];
        if (lefty) {  // the whole layout mirrored across the body's middle: the sidearm on the left hip
            const float d = (zp[z][0] - bp.root[0]) * right[0] + (zp[z][2] - bp.root[2]) * right[2];
            for (int k = 0; k < 3; ++k) zp[z][k] -= 2.0f * d * right[k];
        }
    }
    {  // [Holsters] Anchor=headset: every zone moved with the neck's offset since recentre (in the game camera's rows:
       // the camera is the neutral head, level, facing the body), so the holsters stay with the player when the drawn
       // body does not; with BodyFollowsHead the body already moves sideways with the head: only the height then
        float no[3], sh[3] = {0, 0, 0};
        const bool seated = (st.flags & (RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_DRIVING | RDRVR_ACTOR_IN_COVER)) != 0;
        const bool use = g_anchor.load(std::memory_order_relaxed) == 1 && !seated && bp.cam_ok && camera_lever::neck_offset(no);
        static int was_anchor = 0;  // the last update used the headset anchor (a new choice of it takes the places again)
        if (use) {
            // the zones' places relative to the neutral head (the camera), in the body's yaw frame: taken over 30 frames
            // standing, after the anchor is chosen and after each recentre; then the zones rigid to the neck
            static float acc[kZones][3], rel[kZones][3];
            static int rel_n = -1;  // frames averaged; -1 none yet
            static bool rel_ok = false;
            static uint32_t rel_gen = ~0u;
            const float* cp = bp.cam + 12;
            const uint32_t gen = camera_lever::recentre_gen();
            const bool standing = !(st.flags & RDRVR_ACTOR_CROUCHING);
            if (gen != rel_gen || was_anchor != 1) {
                rel_gen = gen;
                rel_n = 0;
                rel_ok = false;  // the body's places until the new ones are taken
                std::memset(acc, 0, sizeof(acc));
            }
            if (rel_n >= 0 && rel_n < 30 && standing) {
                for (int z = 0; z < kZones; ++z) {
                    const float d[3] = {bases[z][0] - cp[0], bases[z][1] - cp[1], bases[z][2] - cp[2]};
                    acc[z][0] += d[0] * right[0] + d[2] * right[2];
                    acc[z][1] += d[1];
                    acc[z][2] += d[0] * fwd[0] + d[2] * fwd[2];
                }
                if (++rel_n == 30) {
                    for (int z = 0; z < kZones; ++z)
                        for (int k = 0; k < 3; ++k) rel[z][k] = acc[z][k] / 30.0f;
                    rel_ok = true;
                    log::info("[holster] the headset anchor's places taken (the right hip %.2f m right, %.2f m below the head, %.2f m ahead)",
                              rel[0][0], -rel[0][1], rel[0][2]);
                }
            }
            {  // [Holsters] TurnWithHead: the frame turned by the headset's yaw since recentre, past the dead zone
                static float zyaw = 0.0f;
                static uint32_t yaw_gen = ~0u;
                if (gen != yaw_gen || was_anchor != 1) {  // a recentre, or the anchor taken again: facing the body
                    yaw_gen = gen;
                    zyaw = 0.0f;
                }
                float hy = 0.0f;
                if (!g_turn_head.load(std::memory_order_relaxed)) {
                    zyaw = 0.0f;
                } else if (camera_lever::head_yaw_deg(&hy)) {  // (no head pose this frame: kept where they point)
                    // [Holsters] TurnByHands: both hands well ahead of the neck: the target halfway to their direction
                    float hands_yaw = -999.0f;
                    float nk[3];
                    if (g_turn_hands.load(std::memory_order_relaxed) && bp.hand_ok[0] && bp.hand_ok[1] && camera_lever::neck_offset(nk)) {
                        const float nl[3] = {nk[0], nk[1] - 0.10f, nk[2] + 0.08f};  // the neck point (the head's model)
                        float nw[3];
                        for (int k = 0; k < 3; ++k) nw[k] = cp[k] + bp.cam[k] * nl[0] + bp.cam[4 + k] * nl[1] + bp.cam[8 + k] * nl[2];
                        // the aiming pose, wherever it points: both hands 0.30 m or more out from the neck, within 0.35 m of
                        // each other (a look aside while aiming is the case; not against the head's or the holsters' facing)
                        bool ahead = true;
                        float mx = 0, mz = 0;
                        for (int j = 0; j < 2; ++j) {
                            const float vx = bp.hand[j][0] - nw[0], vz = bp.hand[j][2] - nw[2];
                            ahead = ahead && std::sqrt(vx * vx + vz * vz) > 0.30f;
                            mx += 0.5f * vx, mz += 0.5f * vz;
                        }
                        const float sx = bp.hand[0][0] - bp.hand[1][0], sz = bp.hand[0][2] - bp.hand[1][2];
                        ahead = ahead && std::sqrt(sx * sx + sz * sz) < 0.35f;
                        if (ahead) {
                            hands_yaw = std::remainder(std::atan2(-mx, -mz) * 57.29578f - hd * 57.29578f, 360.0f);
                            hy += 0.5f * std::remainder(hands_yaw - hy, 360.0f);
                        }
                    }
                    g_hands_yaw_pub.store(hands_yaw, std::memory_order_relaxed);
                    float d = std::remainder(hy - zyaw, 360.0f);
                    const float dz = g_turn_dead.load(std::memory_order_relaxed);
                    if (d > dz) zyaw += d - dz;
                    else if (d < -dz) zyaw += d + dz;
                    zyaw = std::remainder(zyaw, 360.0f);
                }
                g_zone_yaw_pub.store(zyaw, std::memory_order_relaxed);
                const float a = hd + zyaw * 0.0174532925f;
                hf[0] = -std::sin(a), hf[2] = -std::cos(a);
                hr[0] = std::cos(a), hr[2] = -std::sin(a);
            }
            if (rel_ok) {
                for (int z = 0; z < kZones; ++z) {
                    for (int k = 0; k < 3; ++k)
                        zp[z][k] = cp[k] + hr[k] * (rel[z][0] + zones[z].off[0]) + up[k] * (rel[z][1] + zones[z].off[1]) +
                                   hf[k] * (rel[z][2] + zones[z].off[2]);
                    if (lefty) {  // mirrored across the camera's middle (the body's, as the body mode's)
                        const float d = (zp[z][0] - cp[0]) * hr[0] + (zp[z][2] - cp[2]) * hr[2];
                        for (int k = 0; k < 3; ++k) zp[z][k] -= 2.0f * d * hr[k];
                    }
                }
            }
            const bool follow = body::follows_head();
            float nn[3] = {no[0], no[1], no[2]};
            float lean = 0.0f;
            if (g_lean_steady.load(std::memory_order_relaxed)) {
                // [Holsters] LeanSteady: the torso pivoting at the hips: its drop gives the lean's most forward reach
                // (L sin a with L (1 - cos a) = the drop), the forward move up to that is the lean, the rest a step
                const float L = g_torso_len.load(std::memory_order_relaxed);
                const float drop = std::fmax(0.0f, -nn[1]), dh = std::sqrt(nn[0] * nn[0] + nn[2] * nn[2]);
                const float c = 1.0f - std::fmin(drop, L) / L;
                lean = std::fmin(dh, L * std::sqrt(std::fmax(0.0f, 1.0f - c * c)));
                if (dh > 1e-4f) {
                    nn[0] *= (dh - lean) / dh;
                    nn[2] *= (dh - lean) / dh;
                }
                const float sa = std::fmin(1.0f, lean / L);
                nn[1] += L * (1.0f - std::sqrt(1.0f - sa * sa));  // the drop the lean explains is not a crouch
            }
            g_lean_pub.store(lean, std::memory_order_relaxed);
            const float n[3] = {follow ? 0.0f : nn[0], nn[1], follow ? 0.0f : nn[2]};
            for (int k = 0; k < 3; ++k) sh[k] = bp.cam[k] * n[0] + bp.cam[4 + k] * n[1] + bp.cam[8 + k] * n[2];
            for (int z = 0; z < kZones; ++z)
                for (int k = 0; k < 3; ++k) zp[z][k] += sh[k];
        }
        was_anchor = use ? 1 : 0;
        g_anchor_used.store(use, std::memory_order_relaxed);
        std::lock_guard lock(g_zone_mutex);
        std::memcpy(g_anchor_shift, sh, sizeof(sh));
        if (bp.cam_ok) std::memcpy(g_anchor_cam, bp.cam + 12, sizeof(g_anchor_cam));
    }
    // [Horse] StickTurn (2026-10-10, the user: "When on a horse, looking around with the joystick. The holsters do not
    // turn with you"): riding, the right stick's turn of the view turns the whole layout with it, about the head's
    // vertical axis: the player's own hips turn with their view while John keeps facing the horse (run 9 had the holsters
    // follow the horse)
    const float ride_turn = (st.flags & RDRVR_ACTOR_MOUNTED) ? pose::ride_turn_deg() : 0.0f;
    if (ride_turn != 0.0f && bp.cam_ok) {
        const float* cp = bp.cam + 12;
        const float a = hd + ride_turn * 0.0174532925f;
        const float tf[3] = {-std::sin(a), 0.0f, -std::cos(a)}, tr[3] = {std::cos(a), 0.0f, -std::sin(a)};
        for (int z = 0; z < kZones; ++z) {  // each zone's place in the horse's frame, the same in the turned one
            const float d[3] = {zp[z][0] - cp[0], zp[z][1] - cp[1], zp[z][2] - cp[2]};
            const float x = d[0] * hr[0] + d[2] * hr[2], f = d[0] * hf[0] + d[2] * hf[2];
            for (int k = 0; k < 3; ++k) zp[z][k] = cp[k] + tr[k] * x + up[k] * d[1] + tf[k] * f;
        }
        std::memcpy(hr, tr, sizeof(hr));
        std::memcpy(hf, tf, sizeof(hf));
    }
    // the weapon manager (actor +0x70): +0x80 the item in hand, +0x448 the current slot, +0xa8 + s * 0x70 the slots
    uintptr_t wmgr = 0, in_hand = 0;
    int32_t cur = -1;
    rd(actor + 0x70, &wmgr);
    if (wmgr) {
        rd(wmgr + 0x80, &in_hand);
        rd(wmgr + 0x448, &cur);
    }
    auto slot_filled = [&](int s) {
        uintptr_t item = 0;
        return s >= 0 && s < 8 && wmgr && rd(wmgr + 0xa8 + static_cast<uintptr_t>(s) * 0x70, &item) && item;
    };
    auto slot_gun = [&](int s) -> int {  // the eWeapon of a slot's object (-1: none)
        uintptr_t item = 0, W = 0, info = 0;
        int16_t wt = -1;
        if (s >= 0 && s < 8 && wmgr && rd(wmgr + 0xa8 + static_cast<uintptr_t>(s) * 0x70, &item) && item && rd(item + 0xa0, &W) && W &&
            rd(W + 0x28, &info) && info)
            rd(info + 8, &wt);
        return wt;
    };
    auto slot_loaded = [&](int s) {  // the slot's gun model streamed in (W +0x2c0 = 2, as dual's resolve wants)
        uintptr_t item = 0, W = 0;
        int32_t prop = 0;
        return s >= 0 && s < 8 && wmgr && rd(wmgr + 0xa8 + static_cast<uintptr_t>(s) * 0x70, &item) && item && rd(item + 0xa0, &W) && W &&
               rd(W + 0x2c0, &prop) && prop == 2;
    };
    // a second gun waiting for its slot to hold the holster's gun (ACTOR_SET_NEXT_WEAPON queued), per controller
    struct SecWait {
        int slot = -1, weapon = -1, zone = -1;
        double since = 0.0;
    };
    static SecWait sec_wait[2];
    const bool choosing = g_weapon_choice.load(std::memory_order_relaxed) && st.owned_tick != 0;
    auto owns = [&](int w) { return choosing && w >= 0 && w < kWeapons && (st.owned >> w & 1); };
    // what each slot last held in hand (so a twin can draw another gun than its partner's)
    static int slot_weapon[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    static int zone_last[kZones] = {-1, -1, -1, -1, -1, -1, -1, -1};  // the gun each zone drew last (a twin keeps its own)
    static int drawn_from = -1;  // the zone of the last draw (round 13 item 10: the gun in hand's own holster)
    static int put_zone_of[kWeapons];  // run 7 item 1b: the zone each gun was last put away in (-1: not known)
    static const bool put_zone_init = [] {
        for (int& z : put_zone_of) z = -1;
        return true;
    }();
    (void)put_zone_init;
    controls::commit_draw_hand(st.weapon, in_hand != 0);  // a draw by the other hand: its hand once the gun is in it
    if (in_hand && cur >= 0 && cur < 8 && st.weapon >= 0 && st.weapon < kWeapons) slot_weapon[cur] = st.weapon;
    // a gun goes back to the holster that chose it; else to any holster of its slot
    auto exact = [&](int zi) { return in_hand && zok[zi] && owns(zones[zi].weapon) && st.weapon == zones[zi].weapon; };
    bool any_exact = false;
    for (int zi = 0; zi < kZones; ++zi) any_exact = any_exact || exact(zi);
    auto holds = [&](int zi) {
        if (!in_hand) return false;
        if (exact(zi)) return true;
        if (any_exact) return false;
        for (int s : zones[zi].slots)
            if (s >= 0 && s == cur) return true;
        return false;
    };
    // the weapon a zone draws: its choice; else for a twin, another owned weapon of its slots than the partner's (another
    // kind first: a rifle against a shotgun); else -1 (the slots in order, the game's)
    auto draw_weapon = [&](int zi) -> int {
        const Zone& z = zones[zi];
        if (owns(z.weapon)) return z.weapon;
        const int zl = zone_last[zi];
        if (choosing && zl >= 0 && zl < kWeapons && owns(zl)) {  // its own last gun, while owned and of its slots
            bool fits = false;
            for (int s : z.slots) fits = fits || (s >= 0 && st.equip_slot[zl] == s);
            const int other = z.partner >= 0 ? (owns(zones[z.partner].weapon) ? zones[z.partner].weapon : zone_last[z.partner]) : -1;
            if (fits && zl != other) return zl;
        }
        if (z.partner < 0 || !choosing) return -1;
        const Zone& p = zones[z.partner];
        int pw = owns(p.weapon) ? p.weapon : zone_last[z.partner] >= 0 && owns(zone_last[z.partner]) ? zone_last[z.partner] : -1;
        for (int s : p.slots)
            if (pw < 0 && s >= 0 && slot_weapon[s] >= 0) pw = slot_weapon[s];
        const int ps = pw >= 0 && pw < kWeapons ? st.equip_slot[pw] : -1;
        for (int pass = 0; pass < 2; ++pass)
            for (int s : z.slots) {
                if (s < 0 || (pass == 0) == (s == ps)) continue;
                for (int w = kWeapons - 1; w >= 0; --w)
                    if (owns(w) && st.equip_slot[w] == s && w != pw) return w;
            }
        return -1;
    };
    // the gun hand (a controller; John's right hand is always the gun's) and the front hand, the loading point (a
    // tenth of a metre out along the barrel from the gun hand's wrist) and the barrel's world direction
    const int gun_h = controls::gun_hand(), off_h = 1 - gun_h;
    // the barrel in the IK target's axes: the latest aim ray's, else its measured constant (rigid with the controller,
    // 10 degrees above the target's -z, every gun; the target's own -z is tipped by the hand calibration)
    float bt[3] = {0.0f, 0.174f, -0.985f}, u[3];
    aim::barrel_in_target(bt);
    for (int k = 0; k < 3; ++k)
        u[k] = bp.target_rot[bp.gun][k * 3] * bt[0] + bp.target_rot[bp.gun][k * 3 + 1] * bt[1] + bp.target_rot[bp.gun][k * 3 + 2] * bt[2];
    float load_pt[3];
    const int gj = bp.gun, fj = 1 - gj;  // John's gun hand and his front hand
    float gb[2][3];  // [Hands] InteractOffset: each hand's interaction spot (what grabs); the gun hand's wrist stays the gun's reference
    for (int j = 0; j < 2; ++j) grab_point(bp, j, gb[j]);
    float lo[3], lr = 0.12f;
    bool touch = false;
    {
        std::lock_guard lock(g_zone_mutex);
        touch = g_insert_touch;
    }
    g_gun_in_hand.store(st.weapon >= 0 && st.weapon < kAdjGuns ? st.weapon : -1, std::memory_order_relaxed);
    gun_adjust(st.weapon, kAdjLoadRing, lo, &lr);  // round 13 item 8: the gun's own loading ring, else every gun's
    {  // [Reload] LoadPointOffset in the gun's axes: right (the target's x), up (square to the barrel), forward (the barrel)
        const float* Rg = bp.target_rot[bp.gun];
        float rgt[3] = {Rg[0], Rg[3], Rg[6]}, upv[3] = {rgt[1] * u[2] - rgt[2] * u[1], rgt[2] * u[0] - rgt[0] * u[2], rgt[0] * u[1] - rgt[1] * u[0]};
        const float ul = std::sqrt(upv[0] * upv[0] + upv[1] * upv[1] + upv[2] * upv[2]);
        if (ul > 1e-4f)
            for (float& x : upv) x /= ul;
        else
            upv[0] = upv[2] = 0.0f, upv[1] = 1.0f;
        for (int k = 0; k < 3; ++k) load_pt[k] = bp.hand[gj][k] + u[k] * (0.10f + lo[2]) + rgt[k] * lo[0] + upv[k] * lo[1];
        std::lock_guard lock(g_zone_mutex);
        std::memcpy(g_load_diag.pt, load_pt, sizeof(g_load_diag.pt));
    }
    // [Reload] InsertOnTouch: the drawn round's distance from the point (the hand holding one), else the wrist's
    auto round_at = [&](int rj, float* d_round, float* d_wrist) {
        const float dx = gb[rj][0] - load_pt[0], dy = gb[rj][1] - load_pt[1], dz = gb[rj][2] - load_pt[2];
        *d_wrist = std::sqrt(dx * dx + dy * dy + dz * dz);
        *d_round = -1.0f;
        float pz[12];
        if (round_draw::pose(bp, rj, pz)) {
            const float ex = pz[9] - load_pt[0], ey = pz[10] - load_pt[1], ez = pz[11] - load_pt[2];
            *d_round = std::sqrt(ex * ex + ey * ey + ez * ez);
            std::lock_guard lock(g_zone_mutex);
            std::memcpy(g_load_diag.round, pz + 9, sizeof(g_load_diag.round));
            std::memcpy(g_load_diag.axis, pz + 3, sizeof(g_load_diag.axis));
            g_load_diag.round_ok = true;
        }
    };
    const bool gun = reload::is_gun(st.weapon) && in_hand, long_gun = gun && reload::is_long_gun(st.weapon);
    {  // the user, 2026-10-09: "a revolver facing backwards in my holster and then also coming out backwards when I grab it"
       // (the right hip, the right hand, the Cattleman and another revolver; not seen in the simulator): once a draw, 0.4 s
       // in, the drawn sidearm's barrel in the gun hand's frame (forward about (0 0.17 -0.98)), BACKWARDS when it points back
        static int diag_w = -1;
        static double diag_ms = 0.0;
        static bool diag_done = false;
        if (gun && st.weapon >= 0 && st.weapon <= 7) {
            if (st.weapon != diag_w) {
                diag_w = st.weapon;
                diag_ms = now_ms;
                diag_done = false;
            }
            float b[3];
            if (!diag_done && now_ms - diag_ms > 400.0 && aim::barrel_in_target(b)) {
                diag_done = true;
                const bool back = b[2] > 0.0f;
                log::write(back ? log::Level::Warn : log::Level::Info,
                           "[holster] the %s in hand (from the %s, the %s hand): its barrel in the hand's frame (%.2f %.2f %.2f)%s | the hips' choices %s, %s",
                           kWeaponLabel[st.weapon], drawn_from >= 0 ? zones[drawn_from].key : "?", gun_h ? "right" : "left", b[0], b[1], b[2],
                           back ? " BACKWARDS" : "", zones[0].weapon >= 0 ? kWeaponToken[zones[0].weapon] : "auto",
                           zones[5].weapon >= 0 ? kWeaponToken[zones[5].weapon] : "auto");
            }
        } else {
            diag_w = -1;
        }
    }
    // the empty gun clicks: the gun hand's trigger pulled with nothing loaded (with or without spare rounds), not while
    // the menu has the controllers. [Reload] EmptyClick: a click sound from the gun hand's side, the buzz, both or none
    {
        const bool t = hands::get(gun_h).trigger > 0.6f && !whistle::suppressed(gun_h);  // run 8: the whistle's trigger clicks nothing
        if (t && !trig_down && gun && st.clip < 0.5f && reload::hand_reload() && !menu::visible() &&
            !actions::fire_blocked()) {  // an open revolver or an unworked action: its own buzz, never the empty click (round 10)
            audio::empty_click(gun_h, st.weapon);  // the family's click (round 9: the user's sounds)
            g_clicks.fetch_add(1, std::memory_order_relaxed);
        }
        trig_down = t;
    }
    // the front hand at the foregrip: near the barrel line, ahead of the gun hand
    float fore_along = 0.0f, fore_perp = 1e9f;
    {
        float p[3], perp2 = 0.0f;
        for (int k = 0; k < 3; ++k) {
            p[k] = gb[fj][k] - bp.hand[gj][k];
            fore_along += p[k] * u[k];
        }
        for (int k = 0; k < 3; ++k) perp2 += (p[k] - fore_along * u[k]) * (p[k] - fore_along * u[k]);
        fore_perp = std::sqrt(perp2);
    }
    // the foregrip point (body.cpp: the game's grip on the drawn gun, moved by ForegripOffset) within its radius; until
    // the game's grip is known, the old test: near the barrel line, 0.12 - 0.85 m ahead of the gun hand
    float fore_r = 0.20f, fore_d = 1e9f, fore_zone[3] = {bp.fore[0], bp.fore[1], bp.fore[2]};
    {  // [Reload] SteadyRing: the grip point's mean in the drawn gun's frame 0.4-1.2 s after the draw, kept for that draw
        static int lock_w = -2;
        static double lock_t0 = 0.0;
        static float sum[3] = {}, locked[3] = {};
        static int n = 0;
        static bool have = false;
        if (st.weapon != lock_w || !long_gun) {
            lock_w = st.weapon;
            lock_t0 = now_ms;
            sum[0] = sum[1] = sum[2] = 0.0f;
            n = 0;
            have = false;
        }
        // (run 8: the lock caught the template before the gun's own grip was learned; the grip itself is steadied
        // now in body.cpp, so the ring is the grip point again: the lock stays off)
        if (false && long_gun && bp.fore_ok && bp.gun_frame_ok && g_steady_ring.load(std::memory_order_relaxed)) {
            const float* G = bp.gun_frame_R;  // columns: the gun's axes
            if (!have) {
                const double age = now_ms - lock_t0;
                if (age >= 400.0 && age <= 1200.0) {
                    const float d[3] = {bp.fore[0] - bp.gun_frame_o[0], bp.fore[1] - bp.gun_frame_o[1], bp.fore[2] - bp.gun_frame_o[2]};
                    for (int i = 0; i < 3; ++i) sum[i] += G[0 * 3 + i] * d[0] + G[1 * 3 + i] * d[1] + G[2 * 3 + i] * d[2];
                    ++n;
                } else if (age > 1200.0 && n > 0) {
                    for (int i = 0; i < 3; ++i) locked[i] = sum[i] / static_cast<float>(n);
                    have = true;
                    g_ring_locks.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (have)
                for (int k = 0; k < 3; ++k) fore_zone[k] = bp.gun_frame_o[k] + G[k * 3] * locked[0] + G[k * 3 + 1] * locked[1] + G[k * 3 + 2] * locked[2];
        }
    }
    {  // [Reload] ForegripZoneOffset / ForegripZoneRadius: the ring, apart from where John's hand sits (bp.fore)
        float zo[3];
        gun_adjust(st.weapon, kAdjForeRing, zo, &fore_r);  // round 13 item 8: the gun's own ring, else every gun's
        const float* Rg = bp.target_rot[bp.gun];
        float rgt[3] = {Rg[0], Rg[3], Rg[6]}, upv[3] = {rgt[1] * u[2] - rgt[2] * u[1], rgt[2] * u[0] - rgt[0] * u[2], rgt[0] * u[1] - rgt[1] * u[0]};
        const float ul = std::sqrt(upv[0] * upv[0] + upv[1] * upv[1] + upv[2] * upv[2]);
        if (ul > 1e-4f)
            for (float& x : upv) x /= ul;
        else
            upv[0] = upv[2] = 0.0f, upv[1] = 1.0f;
        if (bp.gun_frame_ok && g_ring_gun_axes.load(std::memory_order_relaxed)) {
            // run 8 item 2: the drawn gun's own axes (columns: x right, y up, z back), so the ring keeps its place on the gun
            // whatever the hand's turn against it (the Sawed-off's ring offset, 6 cm across, swung about the gun as the
            // controller's axes and the drawn gun's parted)
            const float* G = bp.gun_frame_R;
            for (int k = 0; k < 3; ++k) fore_zone[k] += G[k * 3] * zo[0] + G[k * 3 + 1] * zo[1] - G[k * 3 + 2] * zo[2];
        } else {
            for (int k = 0; k < 3; ++k) fore_zone[k] += rgt[k] * zo[0] + upv[k] * zo[1] + u[k] * zo[2];
        }
    }
    if (bp.fore_ok) {
        float dd = 0.0f;
        for (int k = 0; k < 3; ++k) dd += (gb[fj][k] - fore_zone[k]) * (gb[fj][k] - fore_zone[k]);
        fore_d = std::sqrt(dd);
    }
    if (bp.fore_ok && bp.gun_frame_ok && long_gun) fore_trace(st.weapon, bp.gun_frame_R, bp.gun_frame_o, fore_zone, now_ms);
    const dual::State ds = dual::state();  // [Hands] DualWield: the second gun, if one is out
    const bool fore_near = !ds.on && long_gun && reload::two_handed() && bp.hand_ok[0] && bp.hand_ok[1] &&
                           (bp.fore_ok ? fore_d < fore_r : fore_along > 0.12f && fore_along < 0.85f && fore_perp < 0.18f);
    // the Double-barrel open: the off hand is for its barrels (actions.cpp), no two-handed hold until it is shut
    const bool broken = actions::break_open();
    // a hand at an action's part (a bolt, the open barrels, a breech, the semi-auto's handle) grips that part, not the
    // foregrip (cycle M2: the Springfield's trapdoor lies 11 cm from the foregrip's point, inside its ring)
    const bool fore_ok = fore_near && !broken && !actions::grip_wanted(off_h);
    static bool was_down[2] = {false, false};
    int in_z[2] = {-1, -1};
    for (int h = 0; h < 2; ++h) {  // h: the controller; jh: John's hand on it (body.cpp's mapping)
        const int jh = bp.ctrl[0] == h ? 0 : 1;
        if (bp.hand_ok[jh]) {
            // [Holsters] SteadyZones: the zone the hand was in is kept until it is kZoneExit past its radius, and wins
            // over another by kZoneSticky (the left hip's holsters overlap)
            const bool steady = g_steady_zones.load(std::memory_order_relaxed);
            float best = 1e9f;
            for (int z = 0; z < kZones; ++z) {
                if (!zok[z]) continue;
                const float dx = gb[jh][0] - zp[z][0], dy = gb[jh][1] - zp[z][1], dz = gb[jh][2] - zp[z][2];
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                const bool was = steady && z == zone_in[h];
                const float score = was ? d - kZoneSticky : d;
                if (d < zones[z].radius + (was ? kZoneExit : 0.0f) && score < best) {
                    best = score;
                    in_z[h] = z;
                }
            }
        }
        if (in_z[h] != zone_in[h]) {
            if (in_z[h] >= 0) {
                controllers::pulse(h, 0.35f, 30);
                g_entries.fetch_add(1, std::memory_order_relaxed);
            }
            zone_in[h] = in_z[h];
        }
        if (sec_wait[h].slot >= 0) {  // the slot holds the holster's gun, its model in: the second gun now (3 s at most)
            SecWait& sw = sec_wait[h];
            const double waited = log::now_ms() - sw.since;
            const bool ready = slot_gun(sw.slot) == sw.weapon && slot_loaded(sw.slot);
            if (ready || waited > 3000.0 || ds.on || !gun) {
                if (ready && !ds.on && gun) {
                    dual::begin(h, jh, sw.slot, zones[sw.zone].key);
                    controllers::pulse(h, 0.5f, 30);
                    if (waited > 30.0) log::info("[wield] the %s ready in slot %d after %.0f ms", kWeaponLabel[sw.weapon], sw.slot, waited);
                } else if (!ready) {
                    controllers::pulse(h, 0.15f, 10);
                    log::info("[wield] the %s not ready in slot %d after %.0f ms (it holds the %s, model %s): no second gun", kWeaponLabel[sw.weapon],
                              sw.slot, waited, slot_gun(sw.slot) >= 0 && slot_gun(sw.slot) < kWeapons ? kWeaponLabel[slot_gun(sw.slot)] : "?",
                              slot_loaded(sw.slot) ? "in" : "not in");
                }
                sw.slot = -1;
            }
        }
        if (ds.on && h == ds.ctrl) {
            if (sec_zone[h] == -2) sec_zone[h] = in_z[h];
            if (in_z[h] < 0 || in_z[h] != sec_zone[h]) sec_armed[h] = true;
        } else {
            sec_zone[h] = -2;
            sec_armed[h] = false;
        }
        const bool down = grip_down[h], pressed = down && !was_down[h], released = !down && was_down[h];
        was_down[h] = down;
        {  // placing the holsters by hand: a grip in a ring takes that holster, the hand's move moves it, letting go keeps it
            static int place_z[2] = {-1, -1};
            if (!g_place.load(std::memory_order_relaxed)) {
                place_z[h] = -1;
            } else {
                if (pressed && in_z[h] >= 0) {
                    place_z[h] = in_z[h];
                    controllers::pulse(h, 0.5f, 30);
                    log::info("[holster] placing the %s by hand (the %s hand)", zones[in_z[h]].key, h ? "right" : "left");
                }
                const int pz = place_z[h];
                if (pz >= 0 && down && bp.hand_ok[jh]) {
                    // this frame's zone at zp with its offset; the hand at gb: the offset moved by the difference, in the
                    // holster's frame (right, up, forward; mirrored when left-handed)
                    const float d[3] = {gb[jh][0] - zp[pz][0], gb[jh][1] - zp[pz][1], gb[jh][2] - zp[pz][2]};
                    float loc[3] = {d[0] * hr[0] + d[2] * hr[2], d[1], d[0] * hf[0] + d[2] * hf[2]};
                    if (lefty) loc[0] = -loc[0];
                    std::lock_guard lock(g_zone_mutex);
                    for (int k = 0; k < 3; ++k) g_zones[pz].off[k] = std::fmin(0.8f, std::fmax(-0.8f, g_zones[pz].off[k] + loc[k]));
                }
                if (pz >= 0 && !down) {
                    save_zone(pz);
                    g_placed.fetch_add(1, std::memory_order_relaxed);
                    controllers::pulse(h, 0.3f, 20);
                    float o[3];
                    {
                        std::lock_guard lock(g_zone_mutex);
                        std::memcpy(o, g_zones[pz].off, sizeof(o));
                    }
                    log::info("[holster] the %s placed by hand: offset (%.3f %.3f %.3f)", zones[pz].key, o[0], o[1], o[2]);
                    place_z[h] = -1;
                }
                g_consumed[h] = place_z[h] >= 0;
                press_zone[h] = place_z[h];
                pending[h] = false;
                continue;  // no draws, put-aways or rounds while placing
            }
        }
        if (pressed && in_z[h] == kAmmoZone) {
            // a round from the chest for the gun in the other hand (the game's count: the gun not full, spare rounds)
            g_consumed[h] = true;
            press_zone[h] = in_z[h];
            pending[h] = false;
            const bool loadable = reload::hand_reload() && gun && st.clip + 0.5f <= st.clip_max && st.spare >= 1.0f && actions::can_load(st.weapon) &&
                                  !(st.weapon_flags & (RDRVR_WEAPON_DEADEYE | RDRVR_WEAPON_RELOADING));
            const int sq = reload::chest_squeeze();
            const bool sec_hand = ds.on && h == ds.ctrl;
            if (sec_hand && sq && ds.copy) {  // [Hands] DualWieldCopy: the copy pressed to the chest, a round of its own count
                round_held[h] = false;
                const bool in = reload::hand_reload() && !(st.weapon_flags & RDRVR_WEAPON_DEADEYE) && dual::load_copy_round(ds.weapon, st.spare, st.actor);
                if (in) g_squeezes.fetch_add(1, std::memory_order_relaxed);
                controllers::pulse(h, in ? 0.5f : 0.15f, in ? 25 : 10);
            } else if (sec_hand && sq) {  // the second gun pressed to the chest: one round from its own spare ammo (the game's trade)
                round_held[h] = false;
                if (ds.ready && reload::hand_reload() && !(st.weapon_flags & RDRVR_WEAPON_DEADEYE)) {
                    const uint64_t args[3] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), static_cast<uint64_t>(static_cast<uint32_t>(ds.weapon)),
                                              0x3F800000ull};  // 1.0f's bits (a float read with movss)
                    api::queue_native(0xCC69DCC1, args, 3, 0, nullptr);  // ACTOR_ADD_WEAPON_AMMO
                    g_squeezes.fetch_add(1, std::memory_order_relaxed);
                    controllers::pulse(h, 0.5f, 25);
                    log::info("[wield] the second gun squeezed at the chest: one round (weapon %d, clip %.0f)", ds.weapon, ds.clip);
                } else {
                    controllers::pulse(h, 0.15f, 10);
                }
            } else if (h == gun_h && sq) {  // the gun itself pressed to the chest: one round or a full clip
                round_held[h] = false;
                const bool full = sq == 2;
                const bool done = loadable && (full ? reload::fill_clip(st) : reload::insert_round(st));
                if (done) {
                    if (bp.hand_ok[gj]) actions::round_in(st.weapon, bp.hand[gj]);
                    float nr = full ? st.clip_max - st.clip : 1.0f;
                    if (nr > st.spare) nr = st.spare;
                    g_rounds_in.fetch_add(1, std::memory_order_relaxed);
                    g_squeezes.fetch_add(1, std::memory_order_relaxed);
                    controllers::pulse(h, full ? 0.8f : 0.5f, full ? 70 : 25);
                    log::info("[holster] the gun hand squeezed at the chest: %s (clip %.0f -> %.0f of %.0f, spare %.0f)", full ? "a full reload" : "one round",
                              st.clip, st.clip + nr, st.clip_max, st.spare);
                } else {
                    controllers::pulse(h, 0.15f, 10);  // full, no spare rounds, Dead Eye
                }
            }
            const bool can = loadable && h == off_h && !sec_hand;  // a hand holding the second gun takes no round
            const bool squeezed = (h == gun_h || sec_hand) && sq;
            if (!squeezed) round_held[h] = can;
            if (!squeezed) controllers::pulse(h, can ? 0.4f : 0.15f, can ? 25 : 10);
            if (can) {
                g_rounds_taken.fetch_add(1, std::memory_order_relaxed);
                log::info("[holster] %s hand took a round (clip %.0f/%.0f, spare %.0f)", h ? "right" : "left", st.clip, st.clip_max,
                          st.spare);
            }
        } else if (pressed && in_z[h] < 0 && h == off_h && fore_ok) {
            two_hold[h] = true;
            g_consumed[h] = true;
            g_two_grips.fetch_add(1, std::memory_order_relaxed);
            controllers::pulse(h, 0.25f, 20);
            if (bp.fore_ok)
                log::info("[holster] %s hand on the foregrip (%.2f m from its point, the game's grip): two-handed", h ? "right" : "left", fore_d);
            else
                log::info("[holster] %s hand on the foregrip (%.2f m ahead, %.2f off the barrel): two-handed", h ? "right" : "left", fore_along,
                          fore_perp);
        } else if (pressed && in_z[h] >= 0 && ds.on && h == ds.ctrl) {  // [Hands] DualWield: the second gun's hand at a holster
            const Zone& z = zones[in_z[h]];
            g_consumed[h] = true;
            press_zone[h] = in_z[h];
            pending[h] = false;
            bool its = false;
            for (int s : z.slots) its = its || (s >= 0 && (ds.copy ? s == 1 : s == ds.slot));  // the copy goes back at a hip
            sec_pending[h] = its && sec_armed[h];
            if (its && !sec_armed[h])
                log::info("[wield] %s hand at the %s with the second gun (slot %d): kept (it was just taken here; move away first)",
                          h ? "right" : "left", z.key, ds.slot);
            else if (its)
                log::info("[wield] %s hand at the %s with the second gun (slot %d): let go to put it back", h ? "right" : "left", z.key, ds.slot);
            else
                log::info("[wield] %s hand at the %s: nothing (it holds the second gun, of slot %d)", h ? "right" : "left", z.key, ds.slot);
        } else if (pressed && in_z[h] >= 0 && dual::copy_enabled() && !ds.on && gun && h == off_h && cur == 1 && st.weapon >= 0 &&
                   st.weapon <= 7 && [&] {
                       for (int s : zones[in_z[h]].slots)  // [Hands] DualWieldCopy: a hip (slot 1) with a sidearm in hand
                           if (s == 1) return true;
                       return false;
                   }()) {
            const Zone& z = zones[in_z[h]];
            g_consumed[h] = true;
            press_zone[h] = in_z[h];
            pending[h] = false;
            // round 13 item 10 ([Hands] DualWieldSameAtItsHolster): the gun in hand's own holster gives its own model
            const bool its_own = dual::same_at_its_holster() && drawn_from == in_z[h] && zone_last[in_z[h]] == st.weapon;
            if (its_own) {
                dual::set_copy_model(dual::copy_as_prop() ? st.weapon : -1);  // run 7 item 1: its own model as a prop
                log::info("[wield] %s hand at the %s, the %s's own holster: a second %s", h ? "right" : "left", z.key, kWeaponLabel[st.weapon],
                          kWeaponLabel[st.weapon]);
            } else {  // [Hands] DualWieldOwnModel: the copy's model, the hip's gun if another owned sidearm, else the first owned revolver
                // (2026-10-10: the hip's gun as it draws and shows it, automatic included; the hip's weapon alone gave an
                // automatic hip's copy the first owned revolver while the hip showed another, which stayed in the holster)
                auto own = [&](int w) { return w >= 0 && w < 8 && w != st.weapon && st.owned_tick != 0 && (st.owned >> w & 1); };
                const int hip_gun = draw_weapon(in_z[h]);
                int other = own(hip_gun) ? hip_gun : own(z.weapon) ? z.weapon : -1;
                static const int kPref[8] = {4, 5, 6, 7, 0, 1, 2, 3};
                for (int w : kPref)
                    if (other < 0 && dual::own_model() && own(w)) other = w;
                if (!dual::own_model()) other = -1;
                if (other < 0 && dual::copy_as_prop()) other = st.weapon;  // run 7 item 1: the same model, as a prop
                dual::set_copy_model(other);
            }
            dual::begin(h, jh, dual::kCopySlot, z.key);
            controllers::pulse(h, 0.5f, 30);
        } else if (pressed && in_z[h] >= 0 && dual::enabled() && !ds.on && gun && h == off_h && !holds(in_z[h]) && [&] {
                       for (int s : zones[in_z[h]].slots)  // a gun slot (1 the sidearms, 4-6 the long guns) other than the gun's
                           if ((s == 1 || (s >= 4 && s <= 6)) && s != cur && slot_filled(s)) return true;
                       return false;
                   }()) {
            const Zone& z = zones[in_z[h]];
            g_consumed[h] = true;
            press_zone[h] = in_z[h];
            pending[h] = false;
            // the gun this holster draws (its choice, its own last, its twin's other), in its slot; else the first filled slot
            const int dw = draw_weapon(in_z[h]);
            const int ws = dw >= 0 && dw < kWeapons ? st.equip_slot[dw] : -1;
            bool listed = false;
            for (int s : z.slots) listed = listed || (s >= 0 && s == ws);
            if (dw >= 0 && listed && ws == cur) {  // the game keeps one gun a slot: the gun in hand's
                g_sec_refused.fetch_add(1, std::memory_order_relaxed);
                controllers::pulse(h, 0.15f, 10);
                log::info("[wield] %s hand at the %s: the %s shares the slot of the %s in hand (%d): no second gun", h ? "right" : "left", z.key,
                          kWeaponLabel[dw], st.weapon >= 0 && st.weapon < kWeapons ? kWeaponLabel[st.weapon] : "?", cur);
            } else if (dw >= 0 && listed && slot_filled(ws) && !(slot_gun(ws) == dw && slot_loaded(ws))) {
                if (slot_gun(ws) != dw) {  // the game's last gun of that slot is another: switched to this holster's
                    const uint64_t args[2] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), static_cast<uint64_t>(dw)};
                    api::queue_native(kSetNextWeapon, args, 2, 0, nullptr);
                    g_sec_switches.fetch_add(1, std::memory_order_relaxed);
                    log::info("[wield] %s hand at the %s: slot %d switched to the %s (it held the %s)", h ? "right" : "left", z.key, ws,
                              kWeaponLabel[dw], slot_gun(ws) >= 0 && slot_gun(ws) < kWeapons ? kWeaponLabel[slot_gun(ws)] : "?");
                }
                sec_wait[h] = SecWait{ws, dw, in_z[h], log::now_ms()};
                zone_last[in_z[h]] = dw;
            } else if (dw >= 0 && listed && slot_filled(ws)) {  // the slot already holds it, loaded: at once
                zone_last[in_z[h]] = dw;
                dual::begin(h, jh, ws, z.key);
                controllers::pulse(h, 0.5f, 30);
            } else {
                int sslot = -1;
                for (int s : z.slots)
                    if (sslot < 0 && (s == 1 || (s >= 4 && s <= 6)) && s != cur && slot_filled(s)) sslot = s;
                dual::begin(h, jh, sslot, z.key);
                controllers::pulse(h, 0.5f, 30);
            }
        } else if (pressed && in_z[h] >= 0) {
            const Zone& z = zones[in_z[h]];
            g_consumed[h] = true;
            press_zone[h] = in_z[h];
            pending[h] = false;
            const int dw = draw_weapon(in_z[h]);
            const bool mine = h == gun_h || !controls::draw_any_active();  // the hand holding the gun
            if (holds(in_z[h]) && mine) {
                pending[h] = true;  // let go here: holstered
                log::info("[holster] %s hand at the %s with the %s in hand (slot %d): let go to put it away", h ? "right" : "left", z.key,
                          st.weapon >= 0 && st.weapon < kWeapons ? kWeaponLabel[st.weapon] : "?", cur);
            } else if (holds(in_z[h]) && dw < 0) {  // the other hand at the gun's own holster: the gun into that hand
                controls::set_draw_hand(h, -1, true);
                log::info("[holster] %s hand at the %s: the gun into this hand", h ? "right" : "left", z.key);
            } else if (dw >= 0) {  // the holster's chosen weapon, or a twin's other gun (any owned weapon draws)
                const bool instant = g_instant.load(std::memory_order_relaxed);
                const uint64_t args[3] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), static_cast<uint64_t>(dw), instant ? 0ull : 1ull};
                api::queue_native(kPutWeaponInHand, args, 3, 0, nullptr);
                zone_last[in_z[h]] = dw;
                drawn_from = in_z[h];
                controls::set_draw_hand(h, dw, !in_hand);  // the gun to the hand that grabbed it, once it is in hand
                g_draws.fetch_add(1, std::memory_order_relaxed);
                g_choice_draws.fetch_add(1, std::memory_order_relaxed);
                log::info("[holster] %s hand at the %s: draw the %s (weapon %d, %s%s)", h ? "right" : "left", z.key, kWeaponLabel[dw], dw,
                          owns(z.weapon) ? "the holster's choice" : z.partner < 0 ? "its own last" : "another than its twin's", instant ? "" : ", animated");
            } else {
                for (int s : z.slots) {
                    if (!slot_filled(s)) continue;
                    const uint64_t args[2] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), static_cast<uint64_t>(s)};
                    draw_req[h] = api::queue_native(kWeaponEquipped, args, 2, 0, nullptr);
                    draw_ms[h] = log::now_ms();
                    draw_slot[h] = s;
                    break;
                }
            }
        }
        if (draw_req[h]) {  // the slot's weapon: put it in the hand
            RdrvrNativeResult r{};
            if (api::wait_native(draw_req[h], &r, 0)) {
                draw_req[h] = 0;
                const int32_t w = static_cast<int32_t>(static_cast<uint32_t>(r.value));
                if (w >= 0) {
                    const bool instant = g_instant.load(std::memory_order_relaxed);
                    const uint64_t args[3] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), static_cast<uint64_t>(w),
                                              instant ? 0ull : 1ull};
                    api::queue_native(kPutWeaponInHand, args, 3, 0, nullptr);
                    if (press_zone[h] >= 0) zone_last[press_zone[h]] = w;
                    drawn_from = press_zone[h];
                    controls::set_draw_hand(h, w, !in_hand);
                    g_draws.fetch_add(1, std::memory_order_relaxed);
                    log::info("[holster] %s hand at the %s: draw slot %d (weapon %d%s)", h ? "right" : "left",
                              press_zone[h] >= 0 ? zones[press_zone[h]].key : "?", draw_slot[h], w, instant ? "" : ", animated");
                } else {
                    log::warn("[holster] slot %d holds no weapon (%d)", draw_slot[h], w);
                }
            } else if (log::now_ms() - draw_ms[h] > 1000.0) {
                draw_req[h] = 0;
                log::warn("[holster] no answer for the weapon in slot %d", draw_slot[h]);
            }
        }
        if (round_held[h] && down) {  // the round reaches the gun: in it goes
            const int rj = bp.ctrl[0] == h ? 0 : 1;  // John's hand holding the round
            float d_round = -1.0f, d_wrist = 1e9f;
            round_at(rj, &d_round, &d_wrist);
            {
                std::lock_guard lock(g_zone_mutex);
                g_load_diag.wrist_d = d_wrist;
                g_load_diag.round_d = d_round;
            }
            const bool by_touch = touch && d_round >= 0.0f;
            if (by_touch ? d_round < lr : d_wrist < lr) {
                round_held[h] = false;
                {
                    std::lock_guard lock(g_zone_mutex);
                    ++(by_touch ? g_load_diag.inserts_touch : g_load_diag.inserts_wrist);
                }
                if (by_touch) log::info("[holster] the round touched the loading point (the round %.3f m, the wrist %.3f m from it)", d_round, d_wrist);
                if (actions::can_load(st.weapon) && reload::insert_round(st)) {
                    g_rounds_in.fetch_add(1, std::memory_order_relaxed);
                    actions::round_in(st.weapon, load_pt);
                    controllers::pulse(h, 0.5f, 20);
                    controllers::pulse(gun_h, 0.5f, 20);
                    log::info("[holster] a round into the gun (clip %.0f -> %.0f of %.0f)", st.clip, st.clip + 1.0f, st.clip_max);
                } else {
                    log::info("[holster] the round did not go in (clip %.0f/%.0f, spare %.0f, flags %#x)", st.clip, st.clip_max, st.spare, st.weapon_flags);
                }
            }
        }
        if (two_hold[h] && !(down && long_gun)) two_hold[h] = false;
        if (broken && two_hold[h]) {
            two_hold[h] = false;
            two_paused[h] = true;
        }
        if (two_paused[h] && !(down && long_gun)) two_paused[h] = false;
        if (two_paused[h] && !broken) {  // shut with the hand still on the foregrip: two-handed again
            two_paused[h] = false;
            if (h == off_h && fore_near) {
                two_hold[h] = true;
                log::info("[holster] %s hand still on the foregrip as the gun shut: two-handed again", h ? "right" : "left");
            }
        }
        g_wanted[h] = in_z[h] >= 0 || (h == off_h && fore_ok);
        if (released && round_held[h]) {  // round 10: two rounds taken and let go before the gun, silently lost
            const int rj = bp.ctrl[0] == h ? 0 : 1;
            float d_round = -1.0f, d = 1e9f;
            round_at(rj, &d_round, &d);
            if (touch && d_round >= 0.0f) d = d_round;  // InsertOnTouch: the round's own distance
            if (d < lr + 0.04f && actions::can_load(st.weapon) && reload::insert_round(st)) {
                {
                    std::lock_guard lock(g_zone_mutex);
                    ++g_load_diag.inserts_let_go;
                }
                g_rounds_in.fetch_add(1, std::memory_order_relaxed);
                actions::round_in(st.weapon, load_pt);
                controllers::pulse(h, 0.5f, 20);
                controllers::pulse(gun_h, 0.5f, 20);
                log::info("[holster] a round into the gun, let go at it (%.2f m; clip %.0f -> %.0f of %.0f)", d, st.clip, st.clip + 1.0f, st.clip_max);
            } else {
                log::info("[holster] the round dropped: let go %.2f m from the gun's loading point (it goes in within %.2f m holding, %.2f m letting go)",
                          d, lr, lr + 0.04f);
            }
        }
        if (released && press_zone[h] >= 0 && press_zone[h] < kZones && in_z[h] != press_zone[h] && zok[press_zone[h]]) {
            const int pz = press_zone[h], jr = bp.ctrl[0] == h ? 0 : 1;
            const float dx = gb[jr][0] - zp[pz][0], dy = gb[jr][1] - zp[pz][1], dz = gb[jr][2] - zp[pz][2];
            const float rr = zones[pz].radius * g_release_margin.load(std::memory_order_relaxed);
            if (bp.hand_ok[jr] && dx * dx + dy * dy + dz * dz < rr * rr) in_z[h] = pz;  // let go at the edge: still there
        }
        if (released) {
            round_held[h] = false;
            two_hold[h] = false;
            if (sec_pending[h] && in_z[h] >= 0 && in_z[h] == press_zone[h]) {
                dual::end((std::string("put back at the ") + zones[in_z[h]].key).c_str());
                controllers::pulse(h, 0.4f, 25);
            }
            sec_pending[h] = false;
            if (pending[h] && !(g_consumed[h] && in_z[h] >= 0 && in_z[h] == press_zone[h] && in_hand))
                log::info("[holster] %s hand let go away from the %s (in zone %d, item %d): not put away", h ? "right" : "left",
                          press_zone[h] >= 0 ? zones[press_zone[h]].key : "?", in_z[h], in_hand ? 1 : 0);
            if (g_consumed[h] && pending[h] && in_z[h] >= 0 && in_z[h] == press_zone[h] && in_hand && ds.on && !ds.copy && ds.ready &&
                ds.weapon >= 0 && ds.weapon < kWeapons && ds.ctrl >= 0 && ds.ctrl != h) {
                // 2026-10-10 (the user's other PC: the gun in hand put away, the second gun stayed in the other hand with no
                // gun "in hand": every holster refused that hand): the second gun becomes the gun in hand, in its own hand
                if (st.weapon >= 0 && st.weapon < kWeapons) put_zone_of[st.weapon] = in_z[h];
                const int sec_ctrl = ds.ctrl, sec_w = ds.weapon;
                dual::end("the gun in hand put away: it becomes the gun in hand");
                const uint64_t args[3] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor)), static_cast<uint64_t>(sec_w), 0ull};
                api::queue_native(kPutWeaponInHand, args, 3, 0, nullptr);
                controls::set_draw_hand(sec_ctrl, sec_w, false);  // that hand, once the gun is in it
                g_puts.fetch_add(1, std::memory_order_relaxed);
                log::info("[holster] %s hand let go at the %s: put away; the second gun (the %s) is the gun in hand now, in the %s hand",
                          h ? "right" : "left", zones[in_z[h]].key, kWeaponLabel[sec_w], sec_ctrl ? "right" : "left");
            } else if (g_consumed[h] && pending[h] && in_z[h] >= 0 && in_z[h] == press_zone[h] && in_hand) {
                const uint64_t args[1] = {static_cast<uint64_t>(static_cast<uint32_t>(st.actor))};
                api::queue_native(kPutItemAway, args, 1, 0, nullptr);
                g_puts.fetch_add(1, std::memory_order_relaxed);
                if (st.weapon >= 0 && st.weapon < kWeapons) put_zone_of[st.weapon] = in_z[h];  // item 1b: where it was put away
                const bool fists = g_unarmed.load(std::memory_order_relaxed);
                if (fists) {
                    const uint64_t m[2] = {1, 1};
                    api::queue_native(kMeleeModeSelected, m, 2, 0, nullptr);
                    g_unarmed_sets.fetch_add(1, std::memory_order_relaxed);
                }
                log::info("[holster] %s hand let go at the %s: put away%s", h ? "right" : "left", zones[in_z[h]].key,
                          fists ? ", the fists selected" : "");
            }
            g_consumed[h] = false;
            pending[h] = false;
            press_zone[h] = -1;
        }
    }
    {  // [Hands] AimWhenRaised: the gun hand raised (the barrel near level, the hand ahead of the chest), debounced
        bool want = false;
        const bool was = g_raised.load(std::memory_order_relaxed);
        g_gun_at_zone.store(in_z[gun_h] >= 0, std::memory_order_relaxed);
        g_raise_dbg[3].store(static_cast<float>((gun ? 1 : 0) | (bp.hand_ok[gj] ? 2 : 0) | (bp.bone_ok[4] ? 4 : 0) | (in_z[gun_h] < 0 ? 8 : 0) |
                                                (!round_held[0] && !round_held[1] ? 16 : 0)),
                             std::memory_order_relaxed);
        if (gun && bp.hand_ok[gj] && bp.bone_ok[4] && in_z[gun_h] < 0 && !round_held[0] && !round_held[1]) {
            const float pitch = std::asin(u[1] < -1.0f ? -1.0f : u[1] > 1.0f ? 1.0f : u[1]) * 57.2957795f;
            const float dx = bp.hand[gj][0] - bp.bone[4][0], dz = bp.hand[gj][2] - bp.bone[4][2];
            const float reach = std::sqrt(dx * dx + dz * dz), ahead = dx * u[0] + dz * u[2];
            g_raise_dbg[0].store(pitch, std::memory_order_relaxed);
            g_raise_dbg[1].store(reach, std::memory_order_relaxed);
            g_raise_dbg[2].store(ahead, std::memory_order_relaxed);
            want = was ? pitch > g_raise_pmin - 10.0f && pitch < g_raise_pmax + 10.0f && reach > g_raise_reach - 0.07f
                       : pitch > g_raise_pmin && pitch < g_raise_pmax && reach > g_raise_reach && ahead > 0.0f;
        }
        if (want == was) {
            flip_since = 0;
        } else if (!gun || in_z[gun_h] >= 0) {
            g_raised.store(false, std::memory_order_relaxed);  // put away, or into a holster: at once
            flip_since = 0;
        } else {
            if (flip_since == 0) flip_since = now_ms;
            if (now_ms - flip_since >= (want ? 60.0 : 120.0)) {
                g_raised.store(want, std::memory_order_relaxed);
                if (want) g_raises.fetch_add(1, std::memory_order_relaxed);
                flip_since = 0;
            }
        }
    }
    {  // the two-handed weight toward its target, a tenth of a second either way
        const float target = two_hold[0] || two_hold[1] ? 1.0f : 0.0f;
        float b = g_two_blend.load(std::memory_order_relaxed);
        const float step = dt * 10.0f;
        b = b < target ? (b + step > target ? target : b + step) : (b - step < target ? target : b - step);
        g_two_blend.store(b, std::memory_order_relaxed);
    }
    {  // [Reload] RoundInHand: John's hand holding a round, for its draw (round_draw)
        int rh = -1;
        for (int h = 0; h < 2; ++h)
            if (round_held[h]) rh = bp.ctrl[0] == h ? 0 : 1;
        round_draw::set_held(rh, audio::click_family(st.weapon));
        // [Reload] ShowAmmo (run 6 item 6b): while a round could go in now (the gun open for it, or a gate gun with room),
        // a row of the gun's rounds at the chest: as many as could go in, up to six
        if (round_draw::show_ammo()) {
            const bool room = gun && reload::hand_reload() && st.clip + 0.5f <= st.clip_max && st.spare >= 1.0f && actions::can_load(st.weapon) &&
                              !(st.weapon_flags & (RDRVR_WEAPON_DEADEYE | RDRVR_WEAPON_RELOADING)) && zok[kAmmoZone];
            const float can = std::fmin(std::fmin(st.spare, st.clip_max - st.clip), 6.0f);
            round_draw::set_row(room ? audio::click_family(st.weapon) : 0, room ? static_cast<int>(can + 0.01f) : 0, zp[kAmmoZone], hr, up, hf);
        }
        // [Reload] RoundInHand = 2: the game's own shotgun shell in the hand (held_prop; its draw moved onto the pose)
        float pz[12];
        const bool shell = rh >= 0 && round_draw::mode() == 2 && audio::click_family(st.weapon) == 3 && round_draw::pose(bp, rh, pz);
        held_prop::want(0, shell ? "p_gen_shellshotgun01x" : nullptr, pz);
        // the copy's own model (body.cpp wants it at each placement while the copy is out with another model)
        if (!dual::copy_W() || !dual::sidearm_fragment(dual::copy_model())) held_prop::want(1, nullptr, nullptr);
        // [Holsters] ShowGuns: each gun zone's gun at it (the gun it draws; automatic: its slot's last gun in hand, else
        // the highest owned of its slot), none while that gun is in a hand (the gun in hand, the second gun)
        const bool show = g_show_guns.load(std::memory_order_relaxed);
        const bool show_back = g_show_back_guns.load(std::memory_order_relaxed);
        ShowDiag sd;
        {
            std::lock_guard lock(g_zone_mutex);
            sd = g_show_diag;
        }
        int shown_w[4] = {-1, -1, -1, -1};
        // the props' anchor: the pelvis as drawn (body.cpp publishes the same point to held_prop for the draws)
        const float* anc = bp.bone_ok[body::kPelvisPoint] ? bp.bone[body::kPelvisPoint] : bp.root;
        for (int i = 0; i < 4; ++i) {
            const int zi = kShowZones[i];
            int gw = -1;
            if (show && zok[zi] && (show_back || (i != 1 && i != 3))) {  // 1, 3: the back, the left shoulder
                gw = draw_weapon(zi);
                const int s0 = zones[zi].slots[0];
                if (gw < 0 && s0 >= 0 && s0 < 8) gw = slot_weapon[s0];
                if (gw < 0 && choosing)
                    for (int w = kWeapons - 1; w >= 0 && gw < 0; --w)
                        if (owns(w) && st.equip_slot[w] == s0) gw = w;
                if ((in_hand && gw == st.weapon) || (ds.on && gw == ds.weapon)) gw = -1;
                if (ds.on && ds.copy && gw == dual::copy_model()) gw = -1;  // the copy shows that model
            }
            shown_w[i] = gw;
        }
        // run 7 item 1b: twins never show the same gun (one sidearm owned: one hip shows it). Before, only a twin's
        // automatic gun was checked against its partner's, so a twin's own last gun (draw_weapon) could be the gun its
        // partner shows by its slot's fallback (one sidearm, last drawn from the left hip: both hips showed it). The
        // hip it was last put away in keeps it, else the one that has it by choice or by its own last draw, else the
        // first; the other shows nothing. (The table names the partner on the left twin only.)
        for (int i = 0; i < 4; ++i)
            for (int j = i + 1; j < 4; ++j) {
                const int zi = kShowZones[i], zj = kShowZones[j], gw = shown_w[i];
                if (gw < 0 || gw != shown_w[j] || (zones[zi].partner != zj && zones[zj].partner != zi)) continue;
                auto has = [&](int z) { return zones[z].weapon == gw || zone_last[z] == gw; };
                const int put = gw < kWeapons ? put_zone_of[gw] : -1;
                const bool keep_i = put == zi ? true : put == zj ? false : has(zi) || !has(zj);
                shown_w[keep_i ? j : i] = -1;
                g_twin_hidden.fetch_add(1, std::memory_order_relaxed);
            }
        for (int i = 0; i < 4; ++i) {
            const int zi = kShowZones[i];
            const int gw = shown_w[i];
            const char* frag = gw >= 0 ? gun_fragment(gw) : nullptr;
            float gp[12];
            if (frag) {
                show_pose(zi, zp[zi], hr, up, hf, bp.root, gp);
                float dp[3], rel[3];
                double dms = 0;
                uint64_t gated = 0;
                // the error of the frame drawn last, on the body: its drawn place less the anchor that draw used, against
                // this frame's wanted pose less its anchor (a frame's walk is not an error)
                const bool got = held_prop::last_drawn(held_prop::kHolsterSlot + i, dp, &dms, rel, &gated);
                sd.gated[i] = gated;
                if (sd.weapon[i] == gw && got && now_ms - dms < 100.0) {
                    const float ex = rel[0] - (gp[9] - anc[0]), ey = rel[1] - (gp[10] - anc[1]), ez = rel[2] - (gp[11] - anc[2]);
                    sd.err[i] = std::sqrt(ex * ex + ey * ey + ez * ez);
                    sd.worst[i] = std::fmax(sd.worst[i], sd.err[i]);
                    if (sd.err[i] > 0.02f) ++sd.over[i];
                } else if (sd.weapon[i] == gw && got) {
                    ++sd.stale[i];
                }
            }
            sd.weapon[i] = frag ? gw : -1;
            held_prop::want(held_prop::kHolsterSlot + i, frag, frag ? gp : nullptr, anc);
        }
        {  // [Holsters] ShowModels: the holster models at the hips (each enabled hip's, drawn or not)
            float mo[3], mr[3], raw[3];
            bool raw_on;
            {
                std::lock_guard lock(g_zone_mutex);
                std::memcpy(mo, g_model_off, sizeof(mo));
                std::memcpy(mr, g_model_rot, sizeof(mr));
                std::memcpy(raw, g_model_raw, sizeof(raw));
                raw_on = g_model_raw_on;
            }
            const bool models = g_show_models.load(std::memory_order_relaxed);
            for (int i = 0; i < 2; ++i) {
                const int zi = kModelZones[i];
                float mp[12];
                const bool want = models && zok[zi] && g_model_name[0];
                const float zero[3] = {0, 0, 0};
                if (want) model_pose(lefty ? 1 - i : i, zp[zi], hr, up, hf, mo, zero, mp);  // its place (the turn is the object's)
                held_prop::want(held_prop::kModelSlot + i, want ? g_model_name : nullptr, want ? mp : nullptr, anc);
                // its turn: SET_OBJECT_ORIENTATION's three angles (the model's tilt, its heading, its roll) from ModelTurn,
                // the heading plus the holsters' own (their frame's forward), the left hip's heading offset and roll reversed
                const float side = (lefty ? 1 - i : i) == 0 ? 1.0f : -1.0f;
                const float heading = std::atan2(-hf[0], -hf[2]) * 57.29578f;
                const float ang[3] = {mr[0], heading + side * mr[1], side * mr[2]};
                held_prop::want_angles(held_prop::kModelSlot + i, raw_on ? raw : ang);
            }
        }
        ++sd.frames;
        {
            std::lock_guard lock(g_zone_mutex);
            g_show_diag = sd;
        }
        held_prop::frame(static_cast<uint32_t>(st.actor));
    }
    std::lock_guard lock(g_hdiag_mutex);
    g_hdiag.valid = true;
    std::memcpy(g_hdiag.zone, zp, sizeof(zp));
    std::memcpy(g_hdiag.zone_ok, zok, sizeof(zok));
    for (int h = 0; h < 2; ++h) {
        const int jh = bp.ctrl[0] == h ? 0 : 1;
        g_hdiag.hand_ok[h] = bp.hand_ok[jh];
        std::memcpy(g_hdiag.hand[h], gb[jh], sizeof(g_hdiag.hand[h]));  // the interaction spot (the tests steer it)
        g_hdiag.in_zone[h] = in_z[h];
    }
    g_hdiag.cur_slot = cur;
    g_hdiag.in_hand = in_hand != 0;
    g_markers_valid = false;
    float hint_pos[3] = {};
    const int ahint = gun ? actions::hint(hint_pos) : 0;  // [Reload] ActionHints
    const bool placing = g_place.load(std::memory_order_relaxed);
    const bool zones_shown = g_show_zones.load(std::memory_order_relaxed) || placing;
    const bool dots_shown = g_show_dots.load(std::memory_order_relaxed), points_shown = g_show_points.load(std::memory_order_relaxed);
    if ((zones_shown || dots_shown || points_shown) && bp.cam_ok) {
        Markers& mk = g_markers;
        std::memcpy(mk.cam, bp.cam, sizeof(mk.cam));
        mk.n = 0;
        auto add = [&](const float* p, float r, MarkerKind k, MarkerState s, int id) {
            if (mk.n >= kMaxMarkers) return;
            Marker& m = mk.m[mk.n++];
            std::memcpy(m.pos, p, sizeof(m.pos));
            m.radius = r;
            m.kind = k;
            m.state = s;
            m.id = static_cast<int8_t>(id);
        };
        const bool near_only = g_zones_near.load(std::memory_order_relaxed) && !placing;
        const float near_d = g_near_dist.load(std::memory_order_relaxed);
        for (int z = 0; z < kZones && zones_shown; ++z) {  // the zones first (the dots are dropped first when layers run short)
            if (!zok[z]) continue;
            MarkerState ms = kIdle;
            for (int h = 0; h < 2; ++h) {
                if (g_consumed[h].load(std::memory_order_relaxed) && press_zone[h] == z)
                    ms = kHeld;
                else if (in_z[h] == z && ms == kIdle)
                    ms = kHandIn;
            }
            if (near_only && ms == kIdle) {  // [Holsters] ZonesNear: shown faint while a hand is near, else not at all
                bool close_by = false;
                for (int jh = 0; jh < 2; ++jh) {
                    if (!bp.hand_ok[jh]) continue;
                    const float dx = gb[jh][0] - zp[z][0], dy = gb[jh][1] - zp[z][1], dz = gb[jh][2] - zp[z][2];
                    close_by = close_by || std::sqrt(dx * dx + dy * dy + dz * dz) < zones[z].radius + near_d;
                }
                if (!close_by) continue;
                ms = kNear;
            }
            add(zp[z], zones[z].radius, kRing, ms, z);
        }
        if (points_shown && long_gun && reload::two_handed() && bp.hand_ok[0] && bp.hand_ok[1]) {
            const MarkerState fs = two_hold[off_h] ? kHeld : fore_ok ? kHandIn : kGunIdle;
            if (bp.fore_ok) {
                add(fore_zone, fore_r, kRing, fs, kZones);  // the foregrip ring: where the front hand engages, its radius
            } else {
                // before the game's grip is known: the barrel line's point nearest the front hand, within the span
                // that engages (0.12 - 0.85 m ahead of the gun hand's wrist), as wide as the distance that does
                const float al = fore_along < 0.12f ? 0.12f : fore_along > 0.85f ? 0.85f : fore_along;
                float c[3];
                for (int k = 0; k < 3; ++k) c[k] = bp.hand[gj][k] + u[k] * al;
                add(c, 0.18f, kRing, fs, kZones);
            }
        }
        if (points_shown && round_held[off_h]) add(load_pt, lr, kRing, kGunIdle, kZones + 1);  // where the round goes in
        // the action's want: the revolver open (where rounds go in), the bolt's zone, the lever or pump gun
        if (points_shown && ahint == 2) add(hint_pos, 0.05f, kRing, kHeld, kZones + 4);
        else if (points_shown && ahint) add(load_pt, ahint == 1 ? 0.06f : 0.04f, kRing, kHeld, kZones + 4);
        for (int h = 0; h < 2 && dots_shown; ++h) {
            const int jh = bp.ctrl[0] == h ? 0 : 1;
            if (bp.hand_ok[jh]) add(gb[jh], 0.025f, kDot, in_z[h] >= 0 || (h == off_h && fore_ok) ? kHandIn : kIdle, kZones + 2 + h);
        }
        g_markers_ms = now_ms;
        g_markers_valid = true;
    }
}

void load_zones() {
    std::lock_guard lock(g_zone_mutex);
    for (Zone& z : g_zones) {
        if (z.mirror && z.partner >= 0) {  // a twin: the partner's offset and size mirrored (the partner is loaded first)
            const Zone& p = g_zones[z.partner];
            z.off[0] = -p.off[0];
            z.off[1] = p.off[1];
            z.off[2] = p.off[2];
            z.radius = p.radius;
        }
        std::string o = config::get_string("Holsters", (std::string(z.key) + "Offset").c_str(), "");
        float v[3];
        if (!o.empty() && sscanf_s(o.c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3) std::memcpy(z.off, v, sizeof(v));
        z.radius = config::get_float("Holsters", (std::string(z.key) + "Radius").c_str(), z.radius);
        if (z.slots[0] >= 0) {
            // the lower back takes the old knife holster's choice unless it has its own
            const std::string def = !std::strcmp(z.key, "LowerBack") ? config::get_string("Holsters", "KnifeWeapon", "auto") : "auto";
            z.weapon = weapon_from_token(config::get_string("Holsters", (std::string(z.key) + "Weapon").c_str(), def.c_str()));
            z.enabled = config::get_bool("Holsters", (std::string(z.key) + "Enabled").c_str(), z.enabled);
        }
    }
}

void save_zone(int i) {
    Zone z;
    {
        std::lock_guard lock(g_zone_mutex);
        z = g_zones[i];
    }
    char b[64];
    std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", z.off[0], z.off[1], z.off[2]);
    config::set("Holsters", (std::string(z.key) + "Offset").c_str(), b);
    std::snprintf(b, sizeof(b), "%.3f", z.radius);
    config::set("Holsters", (std::string(z.key) + "Radius").c_str(), b);
}

// ---- trace ("holster trace"): the player's transitions and weapon ops, to the log
std::atomic<bool> g_trace{false};
std::atomic<int> g_trace_n{0};

uintptr_t rva_of(uintptr_t a) {
    const uintptr_t base = anchors::base();
    return a >= base && a < base + 0x4000000 ? a - base + 0x140000000 : a;
}

uintptr_t call_get(uintptr_t fn, uintptr_t obj) {
    __try {
        return reinterpret_cast<uintptr_t (*)(uintptr_t)>(fn)(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void log_trans(const char* what, uintptr_t op, uintptr_t node, float t, const char* note) {
    uintptr_t vt = 0, def = 0, tgtdef = 0, tgt = 0;
    float start = 0, timeout = 0;
    uint32_t prio = 0;
    uint8_t trig = 0;
    rd(op, &vt);
    rd(op + 0x30, &def);
    rd(op + 0x18, &tgt);
    rd(op + 0x24, &trig);
    if (def) {
        rd(def + 8, &start);
        rd(def + 0x18, &tgtdef);
        rd(def + 0x20, &timeout);
        rd(def + 0x28, &prio);
    }
    char conds[300] = {};
    int k = 0;
    uintptr_t dvt = 0, getter = 0;
    const uintptr_t base = anchors::base();
    if (def && rd(def, &dvt) && rd(dvt + 0x30, &getter) && getter >= base && getter < base + 0x4000000) {
        uintptr_t list = call_get(getter, def), arr = 0;
        uint16_t n = 0;
        if (list && rd(list, &arr) && rd(list + 8, &n)) {
            k += std::snprintf(conds + k, sizeof(conds) - k, "%u:", n);
            for (uint16_t i = 0; i < n && i < 6 && k < static_cast<int>(sizeof(conds)) - 32; ++i) {
                uintptr_t c = 0, cvt = 0;
                if (!rd(arr + i * 8u, &c) || !rd(c, &cvt)) break;
                k += std::snprintf(conds + k, sizeof(conds) - k, " %llx", static_cast<unsigned long long>(rva_of(cvt)));
            }
        }
    }
    log::info("[holster] %s node %llx t=%.2f%s def %llx start %.2f prio %u tgtdef %llx tgt %llx timeout %.2f trig %u "
              "aim-node %d | conds %s%s",
              what, static_cast<unsigned long long>(node), t, "", static_cast<unsigned long long>(def), start, prio & 0xffff,
              static_cast<unsigned long long>(tgtdef), static_cast<unsigned long long>(tgt), timeout, trig, has_aim_op(node) ? 1 : 0,
              conds, note);
    (void)vt;
}

// The transition op (vtables 0x141ccfa80, 0x141ccfda0), FUN_140599d50: each update it evaluates its definition's
// condition list (def +0x30, def vtable +0x30 the list {conditions*, u16 count}); once it passes it takes the target
// (op +0x18) and sets op +0x24, waits until op +0x20, and ends; its exit (FUN_140599e40) sets the node's +0xe1 and
// queues the target with def +0x28's priority, and the node ends (FUN_14058e3a0) whatever its other ops say.
// def +8 is the op's start time in the node (FUN_14058ddd0 starts each op when the node's time reaches it).
// (The holster transitions test ConditionNOT + ConditionWeaponDrawn: they follow the weapon controller's put-away,
// they do not decide it.) Traced only.
using TransOp_t = uint64_t (*)(uintptr_t op);
TransOp_t o_tupdate = nullptr, o_texit = nullptr;

uint64_t hk_tupdate(uintptr_t op) {
    if (!g_trace.load(std::memory_order_relaxed)) return o_tupdate(op);
    uint8_t trig0 = 0, trig1 = 0;
    rd(op + 0x24, &trig0);
    uint64_t r = o_tupdate(op);
    uintptr_t node = 0;
    float t = 0;
    if (trig0 || !rd(op + 0x24, &trig1) || !trig1 || !player_op(op, &node, &t)) return r;
    g_player_triggers.fetch_add(1, std::memory_order_relaxed);
    if (g_trace_n.fetch_add(1) < 200) log_trans("triggered", op, node, t, "");
    return r;
}

uint64_t hk_texit(uintptr_t op) {
    if (g_trace.load(std::memory_order_relaxed)) {
        uintptr_t node = 0;
        float t = 0;
        uint8_t trig = 0;
        if (rd(op + 0x24, &trig) && trig && player_op(op, &node, &t) && g_trace_n.fetch_add(1) < 200) log_trans("exit", op, node, t, "");
    }
    return o_texit(op);
}

// The weapon op (vtable 0x141d4be40, instant): its start, FUN_140d46e20, draws (def +0x2c == 0) or holsters
// (def +0x2c != 0) the gun. Traced only.
using WeaponOp_t = uint64_t (*)(uintptr_t op);
WeaponOp_t o_weapon_start = nullptr;

uint64_t hk_weapon_start(uintptr_t op) {
    if (g_trace.load(std::memory_order_relaxed)) {
        uintptr_t node = 0, def = 0;
        float t = 0, start = 0;
        uint8_t holster = 0;
        if (player_op(op, &node, &t) && rd(op + 0x20, &def) && rd(def + 8, &start) && rd(def + 0x2c, &holster) && g_trace_n.fetch_add(1) < 200)
            log::info("[holster] weapon op %s: node %llx t=%.2f def %llx start %.2f gun state %d", holster ? "holster" : "draw",
                      static_cast<unsigned long long>(node), t, static_cast<unsigned long long>(def), start,
                      gun_state(g_player_ped.load(std::memory_order_relaxed)));
    }
    return o_weapon_start(op);
}

// Research: the player's weapon and gun-animation state to a file, for diffing over time ("holster dump <file>").
std::string dump(const std::string& path) {
    uintptr_t actor = player_actor(), ped = 0, anim = 0, gun = 0, wmgr = 0;
    if (!actor) return "ERROR no player actor";
    rd(actor + 0x38, &ped);
    rd(actor + 0x70, &wmgr);
    if (ped) rd(ped + 0xaa8, &anim);
    if (anim) rd(anim + 8, &gun);
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") || !f) return "ERROR open " + path;
    struct Region {
        const char* name;
        uintptr_t a;
        uint32_t n;
    } regions[] = {{"actor", actor, 0x400}, {"ped", ped, 0x1000}, {"anim", anim, 0x100}, {"gun", gun, 0x800}, {"wmgr", wmgr, 0x500}};
    static uint8_t buf[0x1000];
    for (const Region& r : regions) {
        std::memset(buf, 0, sizeof(buf));
        for (uint32_t o = 0; r.a && o < r.n; o += 16) raw(r.a + o, buf + o, 16);
        char hdr[16] = {};
        strncpy_s(hdr, r.name, _TRUNCATE);
        std::fwrite(hdr, 1, 16, f);
        uint64_t a = r.a;
        std::fwrite(&a, 8, 1, f);
        std::fwrite(&r.n, 4, 1, f);
        std::fwrite(buf, 1, r.n, f);
    }
    std::fclose(f);
    char b[200];
    std::snprintf(b, sizeof(b), "dumped actor %llx ped %llx gun %llx wmgr %llx", static_cast<unsigned long long>(actor),
                  static_cast<unsigned long long>(ped), static_cast<unsigned long long>(gun), static_cast<unsigned long long>(wmgr));
    return b;
}

}  // namespace

void init() {
    g_keep = config::get_bool("Hands", "KeepGunDrawn", true);
    g_holsters = config::get_bool("Holsters", "Enabled", true);
    g_instant = config::get_bool("Holsters", "InstantDraw", true);
    g_grip_on = config::get_float("Controls", "GripThreshold", 0.6f);
    load_zones();
    log::info("[holster] keep the gun drawn %d, body holsters %d (instant draw %d, grip at %.2f)", g_keep.load() ? 1 : 0,
              g_holsters.load() ? 1 : 0, g_instant.load() ? 1 : 0, g_grip_on);
    // the player's ped for the transition hook (a frame-end listener: off the game's hot paths)
    g_unarmed = config::get_bool("Holsters", "UnarmedAfterHolster", true);
    g_raise_pmin = config::get_float("Hands", "RaisedPitchMin", -35.0f);
    g_raise_pmax = config::get_float("Hands", "RaisedPitchMax", 60.0f);
    g_raise_reach = config::get_float("Hands", "RaisedReach", 0.25f);
    g_show_zones = config::get_bool("Holsters", "ShowZones", false);
    g_zones_near = config::get_bool("Holsters", "ZonesNear", false);
    g_near_dist = std::fmin(1.0f, std::fmax(0.05f, config::get_float("Holsters", "ZonesNearDistance", 0.25f)));
    g_steady_zones = config::get_bool("Holsters", "SteadyZones", false);
    g_show_dots = config::get_bool("Holsters", "ShowHandDots", g_show_zones.load());
    g_ring_gun_axes = config::get_bool("Reload", "RingInGunAxes", true);
    g_steady_ring = config::get_bool("Reload", "SteadyRing", true);
    g_show_points = config::get_bool("Holsters", "ShowWeaponPoints", g_show_zones.load());
    g_release_margin = config::get_float("Holsters", "ReleaseMargin", 1.3f);
    g_weapon_choice = config::get_bool("Holsters", "WeaponChoice", true);
    g_any_weapon = config::get_bool("Holsters", "AnyWeapon", false);
    {
        char b[200];
        int o = std::snprintf(b, sizeof(b), "[holster] weapons: choice %d, any %d:", g_weapon_choice.load() ? 1 : 0, g_any_weapon.load() ? 1 : 0);
        for (int z = 0; z < kZones && o > 0 && o < static_cast<int>(sizeof(b)); ++z)
            if (g_zones[z].slots[0] >= 0)
                o += std::snprintf(b + o, sizeof(b) - o, " %s=%s", g_zones[z].key, g_zones[z].weapon >= 0 ? kWeaponToken[g_zones[z].weapon] : "auto");
        log::info("%s", b);
    }
    {
        float off[3] = {0, 0, 0};
        for (int w = 0; w < kAdjGuns; ++w) {  // round 13 item 8: each gun's own adjustments
            const std::string sec = std::string("Weapon.") + kWeaponToken[w];
            for (int what = 0; what < 3; ++what) {
                const std::string v = config::get_string(sec.c_str(), kAdjKey[what][0], "");
                float o[3];
                if (v.empty() || sscanf_s(v.c_str(), "%f %f %f", &o[0], &o[1], &o[2]) != 3) continue;
                const float r = kAdjKey[what][1] ? config::get_float(sec.c_str(), kAdjKey[what][1], what == kAdjForeRing ? 0.20f : 0.12f) : 0.0f;
                set_gun_adjust(w, what, o, r, false);
            }
        }
        float go[3] = {0, 0, 0};
        const std::string gs = config::get_string("Hands", "InteractOffset", "0 0 0");
        sscanf_s(gs.c_str(), "%f %f %f", &go[0], &go[1], &go[2]);
        set_interact_offset(go, false);
        const std::string fo = config::get_string("Reload", "ForegripOffset", "0 0 0");
        sscanf_s(fo.c_str(), "%f %f %f", &off[0], &off[1], &off[2]);
        set_foregrip(off, config::get_float("Reload", "ForegripRadius", 0.20f), false);
        float zo[3] = {0, 0, 0};
        const std::string fz = config::get_string("Reload", "ForegripZoneOffset", "0 0 0");
        sscanf_s(fz.c_str(), "%f %f %f", &zo[0], &zo[1], &zo[2]);
        set_foregrip_zone(zo, config::get_float("Reload", "ForegripZoneRadius", g_fore_radius), false);
        log::info("[holster] the foregrip ring: moved (%.3f %.3f %.3f) m from the grip, radius %.2f m", zo[0], zo[1], zo[2], g_fzone_radius);
        g_fore_snap = config::get_bool("Reload", "TwoHandedSnap", true);
        float lo[3] = {0, 0, 0};
        const std::string lp = config::get_string("Reload", "LoadPointOffset", "0 0 0");
        sscanf_s(lp.c_str(), "%f %f %f", &lo[0], &lo[1], &lo[2]);
        set_load_point(lo, config::get_float("Reload", "LoadPointRadius", 0.12f), config::get_bool("Reload", "InsertOnTouch", true), false);
        g_show_guns = config::get_bool("Holsters", "ShowGuns", false);
        g_show_back_guns = config::get_bool("Holsters", "ShowBackGuns", true);
        g_anchor = config::get_string("Holsters", "Anchor", "headset") == "body" ? 0 : 1;
        g_turn_head = config::get_bool("Holsters", "TurnWithHead", false);
        g_turn_hands = config::get_bool("Holsters", "TurnByHands", false);
        g_lean_steady = config::get_bool("Holsters", "LeanSteady", false);
        g_torso_len = std::fmin(0.9f, std::fmax(0.3f, config::get_float("Holsters", "TorsoLength", 0.55f)));
        g_show_models = config::get_bool("Holsters", "ShowModels", false);
        {
            const std::string mn = config::get_string("Holsters", "Model", "p_gen_gunbelt01x");
            std::snprintf(g_model_name, sizeof(g_model_name), "%s", mn.c_str());
            float o[3] = {0, 0, 0}, r[3] = {0, 0, 0};
            sscanf_s(config::get_string("Holsters", "ModelOffset", "0 0 0").c_str(), "%f %f %f", &o[0], &o[1], &o[2]);
            sscanf_s(config::get_string("Holsters", "ModelTurn", "90 90 0").c_str(), "%f %f %f", &r[0], &r[1], &r[2]);
            std::lock_guard lock(g_zone_mutex);
            std::memcpy(g_model_off, o, sizeof(o));
            std::memcpy(g_model_rot, r, sizeof(r));
        }
        g_turn_dead = std::fmin(90.0f, std::fmax(0.0f, config::get_float("Holsters", "TurnDeadZone", 0.0f)));
        log::info("[holster] the loading point moved (%.3f %.3f %.3f) m, radius %.2f m; a round goes in on touch %d", lo[0], lo[1], lo[2], g_load_radius,
                  g_insert_touch ? 1 : 0);
        log::info("[holster] foregrip: the game's grip moved (%.3f %.3f %.3f) m, radius %.2f m; the front hand snaps on %d", off[0], off[1],
                  off[2], g_fore_radius, g_fore_snap.load() ? 1 : 0);
    }
    log::info("[holster] a put-away selects the fists %d; shown: the holster rings %d, the hand dots %d, the weapon points %d", g_unarmed.load() ? 1 : 0,
              g_show_zones.load() ? 1 : 0, g_show_dots.load() ? 1 : 0, g_show_points.load() ? 1 : 0);
    d3d::add_frame_end_listener([](uint64_t) {
        uintptr_t actor = player_actor(), ped = 0;
        if (actor) rd(actor + 0x38, &ped);
        g_player_ped.store(ped, std::memory_order_relaxed);
        {  // the gun's aim pose (SEH reads, once a frame)
            uintptr_t anim = 0, gun = 0;
            uint8_t fl = 0;
            g_aim_pose.store(ped && rd(ped + 0xaa8, &anim) && anim && rd(anim + 8, &gun) && gun && rd(gun + 0x5d6, &fl) && (fl & 0x40),
                             std::memory_order_relaxed);
            // fire-ready (FUN_140d16400, the player's branch): the aim op up (+0x5d6 & 0xc0), not blocked, the state
            // ready (8/12 settled, 6 raised past 0.8, 11, 1/4 past 0.8, 5 out of cover) and the fire timer done
            bool ready = false;
            uint8_t b5d4 = 0, b5d5 = 0, b5d8 = 0;
            int32_t gs = -1;
            float f3c4 = 0.0f, f368 = 1.0f, f460 = 0.0f, f24 = 1.0f;
            if (gun && (fl & 0xc0) && rd(gun + 0x5d4, &b5d4) && rd(gun + 0x5d5, &b5d5) && rd(gun + 0x5d8, &b5d8) && rd(gun + 0x3bc, &gs) &&
                rd(gun + 0x3c4, &f3c4) && rd(gun + 0x368, &f368) && rd(gun + 0x460, &f460) && rd(gun + 0x24, &f24)) {
                bool cover = false;
                uintptr_t cv = 0, cb = 0;
                int32_t cst = 0;
                uint8_t cbc = 0, b5c = 0, b5e = 0;
                if (rd(anim + 0x18, &cv) && cv) {
                    rd(cv + 0x18, &cst);
                    rd(cv + 0xbc, &cbc);
                    cover = cst != 0 || (cbc & 4);
                }
                if (rd(anim + 0xb8, &cb) && cb) {
                    rd(cb + 0x5c, &b5c);
                    rd(cb + 0x5e, &b5e);
                    cover = cover || b5c || (b5e & 1);
                }
                ready = !(b5d4 & 0x40) && !(b5d5 & 2) &&
                        (((gs == 8 || gs == 12) && f368 <= 0.39f) || (gs == 6 && f3c4 >= 0.8f) || gs == 11 ||
                         ((gs == 1 || gs == 4) && f460 >= 0.8f) || (gs == 5 && !cover)) &&
                        (f24 < 0.0f || (b5d8 & 6));
            }
            g_fire_ready.store(ready, std::memory_order_relaxed);
            float ph = -1.0f;
            if (!(gun && rd(gun + 0x24, &ph)) || !(ph == ph)) ph = -1.0f;  // NaN: none
            g_fire_phase.store(ph, std::memory_order_relaxed);
        }
        holster_frame();
    });
}

bool install() {
    bool ok = hooks::install("RDR player weapon controller (holster)",
                             reinterpret_cast<void*>(anchors::addr(anchors::Id::PlayerWeaponUpdate)), hk_player_weapon, &o_player_weapon);
    // the action-tree trace (the automatic holster's research, ENGINE-NOTES item 0b): every actor's transition ops
    // run through the update each frame, so it is only hooked when asked for
    if (config::get_bool("Debug", "ActionTrace", false)) {
        ok &= hooks::install("RDR transition op update (holster trace)", reinterpret_cast<void*>(anchors::addr(anchors::Id::TransOpUpdate)),
                             hk_tupdate, &o_tupdate);
        ok &= hooks::install("RDR transition op exit (holster trace)", reinterpret_cast<void*>(anchors::addr(anchors::Id::TransOpExit)),
                             hk_texit, &o_texit);
        ok &= hooks::install("RDR weapon op start (holster trace)", reinterpret_cast<void*>(anchors::addr(anchors::Id::WeaponOpStart)),
                             hk_weapon_start, &o_weapon_start);
    }
    return ok;
}

bool keeps_drawn() { return g_keep.load(); }

bool holsters_enabled() { return g_holsters.load(); }
void set_holsters_enabled(bool on) {
    if (g_holsters.exchange(on) != on) log::info("[holster] body holsters: %s", on ? "on" : "off");
    config::set("Holsters", "Enabled", on ? "1" : "0");
}
bool grip_consumed(int h) { return h >= 0 && h < 2 && g_consumed[h].load(std::memory_order_relaxed); }
float two_hand_blend() { return g_two_blend.load(std::memory_order_relaxed); }
void foregrip(float off[3], float* radius) {
    std::lock_guard lock(g_zone_mutex);
    std::memcpy(off, g_fore_off, sizeof(g_fore_off));
    *radius = g_fore_radius;
}
void gun_adjust(int w, int what, float off[3], float* radius, bool* own) {
    std::lock_guard lock(g_zone_mutex);
    const bool mine = w >= 0 && w < kAdjGuns && what >= 0 && what < 3 && g_gun_adj[w].own[what];
    if (own) *own = mine;
    if (mine) {
        std::memcpy(off, g_gun_adj[w].off[what], sizeof(float) * 3);
        if (radius) *radius = what == kAdjForegrip ? g_fore_radius : g_gun_adj[w].radius[what];
        return;
    }
    const float* g = what == kAdjForegrip ? g_fore_off : what == kAdjForeRing ? g_fzone_off : g_load_off;
    std::memcpy(off, g, sizeof(float) * 3);
    if (radius) *radius = what == kAdjForegrip ? g_fore_radius : what == kAdjForeRing ? g_fzone_radius : g_load_radius;
}
void set_gun_adjust(int w, int what, const float off[3], float radius, bool save) {
    if (w < 0 || w >= kAdjGuns || what < 0 || what > 2) return;
    {
        std::lock_guard lock(g_zone_mutex);
        GunAdjSet& a = g_gun_adj[w];
        a.own[what] = true;
        for (int k = 0; k < 3; ++k) a.off[what][k] = off[k] < -0.4f ? -0.4f : off[k] > 0.4f ? 0.4f : off[k];
        a.radius[what] = radius < 0.03f ? 0.03f : radius > 0.5f ? 0.5f : radius;
    }
    if (save) {
        const std::string sec = std::string("Weapon.") + kWeaponToken[w];
        char b[64];
        std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", off[0], off[1], off[2]);
        config::set(sec.c_str(), kAdjKey[what][0], b);
        if (kAdjKey[what][1]) {
            std::snprintf(b, sizeof(b), "%.3f", radius);
            config::set(sec.c_str(), kAdjKey[what][1], b);
        }
    }
    log::info("[holster] %s: its own %s (%.3f %.3f %.3f) m%s", kWeaponLabel[w], what == kAdjForegrip ? "foregrip" : what == kAdjForeRing ? "foregrip ring" : "loading ring",
              off[0], off[1], off[2], save ? ", saved" : "");
}
void clear_gun_adjust(int w, int what) {
    if (w < 0 || w >= kAdjGuns || what < 0 || what > 2) return;
    {
        std::lock_guard lock(g_zone_mutex);
        g_gun_adj[w].own[what] = false;
    }
    const std::string sec = std::string("Weapon.") + kWeaponToken[w];
    config::set(sec.c_str(), kAdjKey[what][0], "");
    if (kAdjKey[what][1]) config::set(sec.c_str(), kAdjKey[what][1], "");
    log::info("[holster] %s: its %s back to every gun's", kWeaponLabel[w], what == kAdjForegrip ? "foregrip" : what == kAdjForeRing ? "foregrip ring" : "loading ring");
}
int gun_in_hand() { return g_gun_in_hand.load(std::memory_order_relaxed); }
void interact_offset(float off[3]) {
    for (int k = 0; k < 3; ++k) off[k] = g_grab_off[k].load(std::memory_order_relaxed);
}
void set_interact_offset(const float off[3], bool save) {
    for (int k = 0; k < 3; ++k) g_grab_off[k].store(off[k] < -0.25f ? -0.25f : off[k] > 0.25f ? 0.25f : off[k], std::memory_order_relaxed);
    if (save) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", g_grab_off[0].load(), g_grab_off[1].load(), g_grab_off[2].load());
        config::set("Hands", "InteractOffset", b);
    }
}
void grab_point(const body::BodyPoints& bp, int j, float out[3]) {
    // the target's axes (columns): x right (the left hand's mirrored), y up, z back (the fingers along -z)
    const float o[3] = {(j == 0 ? -1.0f : 1.0f) * g_grab_off[0].load(std::memory_order_relaxed), g_grab_off[1].load(std::memory_order_relaxed),
                        -g_grab_off[2].load(std::memory_order_relaxed)};
    const float* R = bp.target_rot[j];
    for (int k = 0; k < 3; ++k) out[k] = bp.hand[j][k] + R[k * 3] * o[0] + R[k * 3 + 1] * o[1] + R[k * 3 + 2] * o[2];
}
void set_foregrip(const float off[3], float radius, bool save) {
    {
        std::lock_guard lock(g_zone_mutex);
        for (int k = 0; k < 3; ++k) g_fore_off[k] = off[k] < -0.5f ? -0.5f : off[k] > 0.5f ? 0.5f : off[k];
        g_fore_radius = radius < 0.03f ? 0.03f : radius > 0.5f ? 0.5f : radius;
    }
    if (save) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", off[0], off[1], off[2]);
        config::set("Reload", "ForegripOffset", b);
        std::snprintf(b, sizeof(b), "%.3f", radius);
        config::set("Reload", "ForegripRadius", b);
    }
}
void load_point(float off[3], float* radius, bool* touch) {
    std::lock_guard lock(g_zone_mutex);
    std::memcpy(off, g_load_off, sizeof(g_load_off));
    *radius = g_load_radius;
    *touch = g_insert_touch;
}
void set_load_point(const float off[3], float radius, bool touch, bool save) {
    {
        std::lock_guard lock(g_zone_mutex);
        for (int k = 0; k < 3; ++k) g_load_off[k] = off[k] < -0.3f ? -0.3f : off[k] > 0.3f ? 0.3f : off[k];
        g_load_radius = radius < 0.03f ? 0.03f : radius > 0.4f ? 0.4f : radius;
        g_insert_touch = touch;
    }
    if (save) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", off[0], off[1], off[2]);
        config::set("Reload", "LoadPointOffset", b);
        std::snprintf(b, sizeof(b), "%.3f", radius);
        config::set("Reload", "LoadPointRadius", b);
        config::set("Reload", "InsertOnTouch", touch ? "1" : "0");
    }
}
void foregrip_zone(float off[3], float* radius) {
    std::lock_guard lock(g_zone_mutex);
    std::memcpy(off, g_fzone_off, sizeof(g_fzone_off));
    *radius = g_fzone_radius;
}
void set_foregrip_zone(const float off[3], float radius, bool save) {
    {
        std::lock_guard lock(g_zone_mutex);
        for (int k = 0; k < 3; ++k) g_fzone_off[k] = off[k] < -0.5f ? -0.5f : off[k] > 0.5f ? 0.5f : off[k];
        g_fzone_radius = radius < 0.03f ? 0.03f : radius > 0.5f ? 0.5f : radius;
    }
    if (save) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", off[0], off[1], off[2]);
        config::set("Reload", "ForegripZoneOffset", b);
        std::snprintf(b, sizeof(b), "%.3f", radius);
        config::set("Reload", "ForegripZoneRadius", b);
    }
}
bool foregrip_snap() { return g_fore_snap.load(); }
void set_foregrip_snap(bool on, bool save) {
    if (g_fore_snap.exchange(on) != on) log::info("[holster] the front hand snaps onto the gun %d", on ? 1 : 0);
    if (save) config::set("Reload", "TwoHandedSnap", on ? "1" : "0");
}
const char* weapon_label(int w) { return w >= 0 && w < kWeapons ? kWeaponLabel[w] : ""; }
const char* weapon_token(int w) { return w >= 0 && w < kWeapons ? kWeaponToken[w] : ""; }
bool weapon_choice() { return g_weapon_choice.load(); }
void set_weapon_choice(bool on) {
    g_weapon_choice = on;
    config::set("Holsters", "WeaponChoice", on ? "1" : "0");
}
bool any_weapon() { return g_any_weapon.load(); }
void set_any_weapon(bool on) {
    g_any_weapon = on;
    config::set("Holsters", "AnyWeapon", on ? "1" : "0");
}
int zone_weapon(int z) {
    if (z < 0 || z >= kZones) return -1;
    std::lock_guard lock(g_zone_mutex);
    return g_zones[z].weapon;
}
void set_zone_weapon(int z, int w, bool save) {
    if (z < 0 || z >= kZones || !zone_takes_weapons(z)) return;
    if (w < -1 || w >= kWeapons) w = -1;
    const char* key = nullptr;
    {
        std::lock_guard lock(g_zone_mutex);
        g_zones[z].weapon = w;
        key = g_zones[z].key;
    }
    log::info("[holster] the %s draws %s", key, w >= 0 ? kWeaponLabel[w] : "its slots (automatic)");
    if (save) config::set("Holsters", (std::string(key) + "Weapon").c_str(), w >= 0 ? kWeaponToken[w] : "auto");
}
bool zone_takes_weapons(int z) { return z >= 0 && z < kZones && g_zones[z].slots[0] >= 0; }
bool zone_enabled(int z) {
    std::lock_guard lock(g_zone_mutex);
    return z >= 0 && z < kZones && g_zones[z].enabled;
}
void set_zone_enabled(int z, bool on, bool save) {
    if (z < 0 || z >= kZones || g_zones[z].slots[0] < 0) return;  // the chest is always on
    {
        std::lock_guard lock(g_zone_mutex);
        g_zones[z].enabled = on;
    }
    log::info("[holster] the %s holster %s", g_zones[z].key, on ? "on" : "off");
    if (save) config::set("Holsters", (std::string(g_zones[z].key) + "Enabled").c_str(), on ? "1" : "0");
}
bool zone_fits(int z, int equip_slot) {
    if (z < 0 || z >= kZones) return false;
    for (int s : g_zones[z].slots)
        if (s >= 0 && s == equip_slot) return true;
    return false;
}
bool arsenal(Arsenal* out) {
    RdrvrActorState st{};
    if (!api::actor_state(&st) || !st.owned_tick) {
        out->valid = false;
        return false;
    }
    out->valid = true;
    out->owned = st.owned;
    std::memcpy(out->equip_slot, st.equip_slot, sizeof(out->equip_slot));
    return true;
}
bool placing() { return g_place.load(std::memory_order_relaxed); }
void set_placing(bool on) {
    if (g_place.exchange(on) != on) log::info("[holster] placing the holsters by hand: %s", on ? "on" : "off");
}
bool zones_near() { return g_zones_near.load(std::memory_order_relaxed); }
void set_zones_near(bool on, bool save) {
    if (g_zones_near.exchange(on) != on) log::info("[holster] the rings only near a hand: %s", on ? "on" : "off");
    if (save) config::set("Holsters", "ZonesNear", on ? "1" : "0");
}
bool steady_zones() { return g_steady_zones.load(std::memory_order_relaxed); }
void set_steady_zones(bool on, bool save) {
    if (g_steady_zones.exchange(on) != on) log::info("[holster] the holsters' edges steadied: %s", on ? "on" : "off");
    if (save) config::set("Holsters", "SteadyZones", on ? "1" : "0");
}
bool show_zones() { return g_show_zones.load(); }
void set_show_zones(bool on) {
    set_show_zones_session(on);
    config::set("Holsters", "ShowZones", on ? "1" : "0");
    // the other two written as they are, so they no longer follow ShowZones at the next start (the review's finding)
    config::set("Holsters", "ShowHandDots", g_show_dots.load() ? "1" : "0");
    config::set("Holsters", "ShowWeaponPoints", g_show_points.load() ? "1" : "0");
}
void set_show_zones_session(bool on) {
    if (g_show_zones.exchange(on) != on) log::info("[holster] rings %s", on ? "shown" : "hidden");
}
bool show_hand_dots() { return g_show_dots.load(); }
bool show_weapon_points() { return g_show_points.load(); }
void set_show_hand_dots(bool on, bool save) {
    if (g_show_dots.exchange(on) != on) log::info("[holster] the hand dots %s", on ? "shown" : "hidden");
    if (save) config::set("Holsters", "ShowHandDots", on ? "1" : "0");
}
void set_show_weapon_points(bool on, bool save) {
    if (g_show_points.exchange(on) != on) log::info("[holster] the weapon points %s", on ? "shown" : "hidden");
    if (save) config::set("Holsters", "ShowWeaponPoints", on ? "1" : "0");
}
bool markers(Markers* out) {
    if (!g_show_zones.load(std::memory_order_relaxed) && !g_show_dots.load(std::memory_order_relaxed) && !g_show_points.load(std::memory_order_relaxed))
        return false;
    std::lock_guard lock(g_hdiag_mutex);
    if (!g_markers_valid || log::now_ms() - g_markers_ms > 100.0) return false;
    *out = g_markers;
    return true;
}
bool aim_pose() { return g_aim_pose.load(std::memory_order_relaxed); }
bool gun_raised() { return g_raised.load(std::memory_order_relaxed); }
bool fire_ready() { return g_fire_ready.load(std::memory_order_relaxed); }
float fire_clip_phase() { return g_fire_phase.load(std::memory_order_relaxed); }
bool show_models() { return g_show_models.load(std::memory_order_relaxed); }
void set_show_models(bool on, bool save) {
    if (g_show_models.exchange(on) != on) log::info("[holster] the holster models (%s) at the hips: %s", g_model_name, on ? "on" : "off");
    if (save) config::set("Holsters", "ShowModels", on ? "1" : "0");
}
bool show_guns() { return g_show_guns.load(std::memory_order_relaxed); }
void set_show_guns(bool on, bool save) {
    if (g_show_guns.exchange(on) != on) log::info("[holster] the guns shown at the holsters: %s", on ? "on" : "off");
    if (save) config::set("Holsters", "ShowGuns", on ? "1" : "0");
}
int anchor() { return g_anchor.load(std::memory_order_relaxed); }
bool lean_steady() { return g_lean_steady.load(std::memory_order_relaxed); }
void set_lean_steady(bool on, bool save) {
    if (g_lean_steady.exchange(on) != on) log::info("[holster] the holsters steady when you lean: %s", on ? "on" : "off");
    if (save) config::set("Holsters", "LeanSteady", on ? "1" : "0");
}
bool turn_by_hands() { return g_turn_hands.load(std::memory_order_relaxed); }
void set_turn_by_hands(bool on, bool save) {
    if (g_turn_hands.exchange(on) != on) log::info("[holster] the holsters' turn by the head and the hands: %s", on ? "on" : "off");
    if (save) config::set("Holsters", "TurnByHands", on ? "1" : "0");
}
bool turn_with_head() { return g_turn_head.load(std::memory_order_relaxed); }
void set_turn_with_head(bool on, bool save) {
    if (g_turn_head.exchange(on) != on) log::info("[holster] the holsters turn with the headset: %s", on ? "on" : "off");
    if (save) config::set("Holsters", "TurnWithHead", on ? "1" : "0");
}
float turn_dead_zone() { return g_turn_dead.load(std::memory_order_relaxed); }
void set_turn_dead_zone(float deg, bool save) {
    deg = std::fmin(90.0f, std::fmax(0.0f, deg));
    g_turn_dead = deg;
    if (save) {
        char v[16];
        std::snprintf(v, sizeof(v), "%.0f", deg);
        config::set("Holsters", "TurnDeadZone", v);
    }
}
void set_anchor(int a, bool save) {
    a = a == 1 ? 1 : 0;
    if (g_anchor.exchange(a) != a) log::info("[holster] the holsters anchored to the %s", a ? "headset (the neck's offset)" : "body");
    if (save) config::set("Holsters", "Anchor", a ? "headset" : "body");
}
bool show_back_guns() { return g_show_back_guns.load(std::memory_order_relaxed); }
void set_show_back_guns(bool on, bool save) {
    if (g_show_back_guns.exchange(on) != on) log::info("[holster] the long guns shown on the back: %s", on ? "on" : "off");
    if (save) config::set("Holsters", "ShowBackGuns", on ? "1" : "0");
}
bool gun_at_zone() { return g_gun_at_zone.load(std::memory_order_relaxed); }
bool unarmed_after_holster() { return g_unarmed.load(); }
void set_unarmed_after_holster(bool on) {
    if (g_unarmed.exchange(on) != on) log::info("[holster] a put-away selects the fists %d", on ? 1 : 0);
    config::set("Holsters", "UnarmedAfterHolster", on ? "1" : "0");
}
bool grip_wanted(int h) { return h >= 0 && h < 2 && g_wanted[h].load(std::memory_order_relaxed); }
int zone_count() { return kZones; }
const char* zone_label(int i) { return i >= 0 && i < kZones ? g_zones[i].label : "?"; }
void zone(int i, float off[3], float* radius) {
    if (i < 0 || i >= kZones) return;
    std::lock_guard lock(g_zone_mutex);
    std::memcpy(off, g_zones[i].off, sizeof(g_zones[i].off));
    *radius = g_zones[i].radius;
}
void set_zone(int i, const float off[3], float radius, bool save) {
    if (i < 0 || i >= kZones) return;
    {
        std::lock_guard lock(g_zone_mutex);
        std::memcpy(g_zones[i].off, off, sizeof(g_zones[i].off));
        g_zones[i].radius = radius;
    }
    if (save) save_zone(i);
}

void set_keeps_drawn(bool on) {
    if (g_keep.exchange(on) != on) log::info("[holster] keep the gun drawn: %s", on ? "on" : "off");
}

std::string command(const std::string& line) {
    std::istringstream in(line);
    std::string c, v;
    in >> c >> v;
    if (v == "dump") {
        std::string path;
        in >> path;
        return dump(path);
    }
    if (v == "trace") {
        std::string x;
        in >> x;
        if (!o_tupdate) return "trace: not hooked ([Debug] ActionTrace=1 installs it at start)";
        g_trace_n = 0;
        g_trace = x != "off";
        return std::string("trace ") + (g_trace.load() ? "on" : "off") + "; player ped " + std::to_string(g_player_ped.load());
    }
    if (v == "weapon") {  // holster weapon <Key> <token|auto> (writes the user ini, as the menu does)
        std::string key, tok;
        in >> key >> tok;
        for (int z = 0; z < kZones; ++z)
            if (_stricmp(key.c_str(), g_zones[z].key) == 0) set_zone_weapon(z, weapon_from_token(tok), true);
        v = "weapons";
    }
    if (v == "slots") {  // holster slots: the weapon manager's slots, each one's gun (the game's object of that slot)
        const uintptr_t actor = player_actor();
        uintptr_t wmgr = 0, hand = 0;
        int32_t cur = -1;
        if (!actor || !rd(actor + 0x70, &wmgr) || !wmgr) return "slots: no weapon manager";
        rd(wmgr + 0x80, &hand);
        rd(wmgr + 0x448, &cur);
        std::string r = "slots: current " + std::to_string(cur) + ":";
        for (int s = 0; s < 8; ++s) {
            uintptr_t item = 0, W = 0, info = 0;
            int16_t wt = -1;
            if (rd(wmgr + 0xa8 + static_cast<uintptr_t>(s) * 0x70, &item) && item && rd(item + 0xa0, &W) && W && rd(W + 0x28, &info) && info)
                rd(info + 8, &wt);
            r += " " + std::to_string(s) + "=" + (wt >= 0 && wt < kWeapons ? kWeaponToken[wt] : item ? "?" : "-") + (item && item == hand ? "(in hand)" : "");
        }
        return r;
    }
    if (v == "weapons") {  // holster weapons: the owned weapons with their slots, each holster's choice
        Arsenal a{};
        const bool ok = arsenal(&a);
        std::string r = std::string("weapons: choice ") + (g_weapon_choice.load() ? "1" : "0") + ", owned" + (ok ? ":" : " (not read yet)");
        for (int w = 0; ok && w < kWeapons; ++w)
            if (a.owned >> w & 1) r += std::string(" ") + kWeaponToken[w] + "@" + std::to_string(a.equip_slot[w]);
        r += " | choices:";
        for (int z = 0; z < kZones; ++z)
            if (g_zones[z].slots[0] >= 0) {
                const int w = zone_weapon(z);
                r += std::string(" ") + g_zones[z].key + "=" + (w >= 0 ? kWeaponToken[w] : "auto");
                if (w >= 0 && ok && !(a.owned >> w & 1)) r += "(not owned)";
            }
        r += " | choice draws " + std::to_string(g_choice_draws.load()) + ", second-gun slot switches " + std::to_string(g_sec_switches.load()) +
             ", refused " + std::to_string(g_sec_refused.load());
        return r;
    }
    if (v == "load") {  // holster load [set <right> <up> <forward> [radius] | touch on|off] (the session only): the loading point
        std::string x;
        in >> x;
        float off[3], r = 0;
        bool touch = false;
        load_point(off, &r, &touch);
        if (x == "set") {
            in >> off[0] >> off[1] >> off[2];
            float r2 = r;
            if (in >> r2) r = r2;
            set_load_point(off, r, touch, false);
        } else if (x == "touch") {
            std::string y;
            in >> y;
            set_load_point(off, r, y == "on", false);
        }
        load_point(off, &r, &touch);
        LoadDiag d;
        {
            std::lock_guard lock(g_zone_mutex);
            d = g_load_diag;
        }
        char b[460];
        std::snprintf(b, sizeof(b), "load point (%.3f %.3f %.3f): offset (%.3f %.3f %.3f) m, radius %.2f m, on touch %d | the round held: the wrist %.3f m, the round %.3f m from it "
                      "(round at (%.3f %.3f %.3f) axis (%.3f %.3f %.3f)) | inserts: on touch %llu, by the wrist %llu, let go %llu",
                      d.pt[0], d.pt[1], d.pt[2], off[0], off[1], off[2], r, touch ? 1 : 0, d.wrist_d, d.round_d, d.round[0], d.round[1], d.round[2], d.axis[0], d.axis[1], d.axis[2],
                      static_cast<unsigned long long>(d.inserts_touch), static_cast<unsigned long long>(d.inserts_wrist),
                      static_cast<unsigned long long>(d.inserts_let_go));
        return b;
    }
    if (v == "anchor") {  // holster anchor [body|headset]: [Holsters] Anchor for the session; the last update's shift
        std::string x;
        in >> x;
        if (x == "body" || x == "headset") set_anchor(x == "headset" ? 1 : 0, false);
        if (x == "lean" || x == "hands") {  // holster anchor lean|hands on|off: LeanSteady, TurnByHands (the session only)
            std::string y;
            in >> y;
            if (x == "lean") set_lean_steady(y == "on", false);
            else set_turn_by_hands(y == "on", false);
        }
        if (x == "turn") {  // holster anchor turn on|off [dead zone]: the session only
            std::string y;
            float dz = -1.0f;
            in >> y >> dz;
            if (y == "on" || y == "off") set_turn_with_head(y == "on", false);
            if (dz >= 0.0f) set_turn_dead_zone(dz, false);
        }
        float sh[3], cp[3];
        {
            std::lock_guard lock(g_zone_mutex);
            std::memcpy(sh, g_anchor_shift, sizeof(sh));
            std::memcpy(cp, g_anchor_cam, sizeof(cp));
        }
        float no[3] = {0, 0, 0};
        const bool nk = camera_lever::neck_offset(no);
        float hy = 0.0f;
        const bool hk = camera_lever::head_yaw_deg(&hy);
        char b[420];
        std::snprintf(b, sizeof(b), "anchor %s, used %d, shift (%.3f %.3f %.3f) | neck since recentre%s (%.3f %.3f %.3f) | camera (%.3f %.3f %.3f) | "
                      "turn %d, dead zone %.0f, the holsters' yaw %.1f, the head's%s %.1f deg | lean steady %d (the lean %.3f m), by hands %d (%.1f)",
                      g_anchor.load() ? "headset" : "body", g_anchor_used.load() ? 1 : 0, sh[0], sh[1], sh[2], nk ? "" : " (none)", no[0],
                      no[1], no[2], cp[0], cp[1], cp[2], g_turn_head.load() ? 1 : 0, g_turn_dead.load(), g_zone_yaw_pub.load(), hk ? "" : " (none)", hy,
                      g_lean_steady.load() ? 1 : 0, g_lean_pub.load(), g_turn_hands.load() ? 1 : 0, g_hands_yaw_pub.load());
        return b;
    }
    if (v == "guns") {  // holster guns [on|off|reset|back on|off]: [Holsters] ShowGuns (ShowBackGuns) for the session; each shown gun, its drawn error
        std::string x;
        in >> x;
        if (x == "on" || x == "off") set_show_guns(x == "on", false);
        if (x == "back") {
            std::string y;
            in >> y;
            if (y == "on" || y == "off") set_show_back_guns(y == "on", false);
        }
        std::lock_guard lock(g_zone_mutex);
        if (x == "reset") {
            for (float& e : g_show_diag.worst) e = 0.0f;
            for (uint64_t& e : g_show_diag.stale) e = 0;
            for (uint64_t& e : g_show_diag.over) e = 0;
        }
        static const char* const kName[4] = {"right hip", "back", "left hip", "left shoulder"};
        std::string o = std::string("guns shown ") + (g_show_guns.load() ? "1" : "0") + " back " + (g_show_back_guns.load() ? "1" : "0");
        char b[200];
        for (int i = 0; i < 4; ++i) {
            std::snprintf(b, sizeof(b), " | %s %d err %.4f worst %.4f stale %llu gated %llu over %llu", kName[i], g_show_diag.weapon[i], g_show_diag.err[i],
                          g_show_diag.worst[i], static_cast<unsigned long long>(g_show_diag.stale[i]), static_cast<unsigned long long>(g_show_diag.gated[i]),
                          static_cast<unsigned long long>(g_show_diag.over[i]));
            o += b;
        }
        std::snprintf(b, sizeof(b), " | frames %llu | twins hidden %llu", static_cast<unsigned long long>(g_show_diag.frames),
                      static_cast<unsigned long long>(g_twin_hidden.load()));
        return o + b;
    }
    if (v == "gun") {  // holster gun: the gun in hand's adjustments in effect (round 13 item 8), its own or every gun's
        const int w = gun_in_hand();
        std::string o = std::string("gun ") + std::to_string(w);
        for (int what = 0; what < 3; ++what) {
            float off[3], r = 0;
            bool own = false;
            gun_adjust(w, what, off, &r, &own);
            char b[120];
            std::snprintf(b, sizeof(b), " | %s (%.3f %.3f %.3f) r %.3f %s", what == 0 ? "foregrip" : what == 1 ? "ring" : "load", off[0], off[1], off[2], r,
                          own ? "own" : "every");
            o += b;
        }
        return o;
    }
    if (v == "grab") {  // holster grab [<right> <up> <forward>]: [Hands] InteractOffset for the session; the spots
        float o[3];
        if (in >> o[0] >> o[1] >> o[2]) set_interact_offset(o, false);
        interact_offset(o);
        body::BodyPoints bp;
        char b[300];
        float g[2][3] = {};
        const bool ok = body::body_points(&bp);
        if (ok)
            for (int j = 0; j < 2; ++j) grab_point(bp, j, g[j]);
        std::snprintf(b, sizeof(b), "interaction spot (%.3f %.3f %.3f) | left spot (%.3f %.3f %.3f) wrist (%.3f %.3f %.3f) | right spot (%.3f %.3f %.3f) wrist (%.3f %.3f %.3f)",
                      o[0], o[1], o[2], g[0][0], g[0][1], g[0][2], bp.hand[0][0], bp.hand[0][1], bp.hand[0][2], g[1][0], g[1][1], g[1][2], bp.hand[1][0],
                      bp.hand[1][1], bp.hand[1][2]);
        return b;
    }
    if (v == "fore") {  // holster fore [set <right> <up> <forward> [radius] | snap on|off | trace [reset]] (the session only)
        std::string x;
        in >> x;
        if (x == "steady") {  // holster fore steady on|off: SteadyRing for the session (the learned grips, body.cpp)
            std::string y;
            in >> y;
            g_steady_ring = y != "off";
            return body::command(std::string("skel grips steadyring ") + (g_steady_ring.load() ? "on" : "off"));
        }
        if (x == "axes") {  // holster fore axes gun|controller: the ring offset's axes (RingInGunAxes, the session)
            std::string y;
            in >> y;
            g_ring_gun_axes = y != "controller";
            return std::string("the ring offset along the ") + (g_ring_gun_axes.load() ? "drawn gun's axes" : "controller's axes");
        }
        if (x == "trace") {  // the ring in the drawn gun's frame since the reset (run 8 item 2)
            std::string y;
            in >> y;
            std::lock_guard lock(g_fore_mutex);
            if (y == "reset") g_fore_trace = ForeTrace{};
            const ForeTrace& t = g_fore_trace;
            char b[400];
            std::snprintf(b, sizeof(b),
                          "fore trace: weapon %d, %llu frames, the ring in the gun's frame now (%.3f %.3f %.3f), range x %.1f y %.1f z %.1f mm, steps over "
                          "5 mm %llu (the largest %.1f mm) | the grip from %s",
                          t.weapon, static_cast<unsigned long long>(t.frames), t.last[0], t.last[1], t.last[2],
                          t.have ? (t.hi[0] - t.lo[0]) * 1000.0f : -1.0f, t.have ? (t.hi[1] - t.lo[1]) * 1000.0f : -1.0f,
                          t.have ? (t.hi[2] - t.lo[2]) * 1000.0f : -1.0f, static_cast<unsigned long long>(t.jumps), t.max_step * 1000.0f,
                          body::grip_source_name());
            return b;
        }
        float off[3], r = 0;
        foregrip(off, &r);
        if (x == "set") {
            in >> off[0] >> off[1] >> off[2];
            float r2 = r;
            if (in >> r2) r = r2;
            set_foregrip(off, r, false);
        } else if (x == "snap") {
            std::string y;
            in >> y;
            set_foregrip_snap(y == "on", false);
        } else if (x == "zone") {  // holster fore zone <right> <up> <forward> [radius]: the ring, apart from the grip
            float zo[3], zr = 0;
            foregrip_zone(zo, &zr);
            in >> zo[0] >> zo[1] >> zo[2];
            float r2 = zr;
            if (in >> r2) zr = r2;
            set_foregrip_zone(zo, zr, false);
        }
        body::BodyPoints bp;
        const bool have = body::body_points(&bp);
        float d = -1.0f;
        if (have && bp.fore_ok) {
            float dd = 0;
            for (int k = 0; k < 3; ++k) dd += (bp.hand[1 - bp.gun][k] - bp.fore[k]) * (bp.hand[1 - bp.gun][k] - bp.fore[k]);
            d = std::sqrt(dd);
        }
        float zo[3], zr = 0;
        foregrip_zone(zo, &zr);
        char b[400];
        std::snprintf(b, sizeof(b), "foregrip: offset (%.3f %.3f %.3f) radius %.2f snap %d | point %s (%.3f %.3f %.3f), the left hand %.3f m from it | "
                      "the ring: moved (%.3f %.3f %.3f) radius %.2f",
                      off[0], off[1], off[2], r, g_fore_snap.load() ? 1 : 0, have && bp.fore_ok ? "known" : "not known", have ? bp.fore[0] : 0.0f,
                      have ? bp.fore[1] : 0.0f, have ? bp.fore[2] : 0.0f, d, zo[0], zo[1], zo[2], zr);
        return b;
    }
    if (v == "rings") {  // holster rings [on|off|<name>]: the markers (the session only), each in the world and LOCAL; <name>: only those
        std::string x;
        in >> x;
        if (x == "on" || x == "off") set_show_zones_session(x == "on");
        if (x == "near" || x == "steady" || x == "place") {  // holster rings near|steady|place on|off (the session)
            std::string y;
            in >> y;
            if (x == "near") set_zones_near(y == "on", false);
            else if (x == "steady") set_steady_zones(y == "on", false);
            else set_placing(y == "on");
        }
        if (x == "dots" || x == "points") {  // holster rings dots|points on|off: the hand dots, the weapon points (the session)
            std::string y;
            in >> y;
            if (x == "dots") set_show_hand_dots(y == "on", false);
            else set_show_weapon_points(y == "on", false);
        }
        const std::string only =
            x == "on" || x == "off" || x == "dots" || x == "points" || x == "near" || x == "steady" || x == "place" ? std::string() : x;
        Markers mk;
        char sw[96];
        std::snprintf(sw, sizeof(sw), "(holsters %d, hand dots %d, weapon points %d, near %d, steady %d, placing %d, placed %llu) ",
                      g_show_zones.load() ? 1 : 0, g_show_dots.load() ? 1 : 0, g_show_points.load() ? 1 : 0, g_zones_near.load() ? 1 : 0,
                      g_steady_zones.load() ? 1 : 0, g_place.load() ? 1 : 0, static_cast<unsigned long long>(g_placed.load()));
        if (!markers(&mk)) return std::string("rings ") + sw + (g_show_zones.load() || g_show_dots.load() || g_show_points.load() ? "on, no markers yet" : "off");
        std::string r = std::string("rings ") + sw + "on, " + std::to_string(mk.n) + " markers:";
        static const char* const kState[5] = {"idle", "hand in", "held", "gun", "near"};
        for (int i = 0; i < mk.n; ++i) {
            const Marker& m = mk.m[i];
            float l[3] = {}, w2[3] = {};
            const bool lok = camera_lever::world_to_local(mk.cam, m.pos, l);
            float err = -1.0f;
            if (lok && camera_lever::local_to_world(mk.cam, l, nullptr, w2, nullptr))
                err = std::sqrt((w2[0] - m.pos[0]) * (w2[0] - m.pos[0]) + (w2[1] - m.pos[1]) * (w2[1] - m.pos[1]) + (w2[2] - m.pos[2]) * (w2[2] - m.pos[2]));
            const char* nm = m.id < kZones ? g_zones[m.id].key : m.id == kZones ? "Foregrip" : m.id == kZones + 1 ? "LoadPoint" : m.id == kZones + 2 ? "LeftHand"
                           : m.id == kZones + 3 ? "RightHand" : "ActionHint";
            if (!only.empty() && only != nm) continue;
            char b[200];
            std::snprintf(b, sizeof(b), " | %s %s r %.2f world (%.3f %.3f %.3f) local (%.3f %.3f %.3f) back %.5f", nm, kState[m.state < 5 ? m.state : 0], m.radius,
                          m.pos[0], m.pos[1], m.pos[2], l[0], l[1], l[2], err);
            r += b;
        }
        return r;
    }
    if (v == "zones") {
        HolsterDiag d;
        {
            std::lock_guard lock(g_hdiag_mutex);
            d = g_hdiag;
        }
        if (!d.valid) return "zones: not in first person (or no body drawn)";
        char b[700];
        int k = std::snprintf(b, sizeof(b), "slot %d in hand %d | draws %llu puts %llu entries %llu rounds %llu/%llu clicks %llu two %llu (%.2f) |",
                              d.cur_slot, d.in_hand ? 1 : 0, static_cast<unsigned long long>(g_draws.load()),
                              static_cast<unsigned long long>(g_puts.load()), static_cast<unsigned long long>(g_entries.load()),
                              static_cast<unsigned long long>(g_rounds_in.load()), static_cast<unsigned long long>(g_rounds_taken.load()),
                              static_cast<unsigned long long>(g_clicks.load()), static_cast<unsigned long long>(g_two_grips.load()),
                              g_two_blend.load());
        for (int z = 0; z < kZones && k < static_cast<int>(sizeof(b)) - 80; ++z)
            k += std::snprintf(b + k, sizeof(b) - k, " %s (%.3f %.3f %.3f)%s", g_zones[z].key, d.zone[z][0], d.zone[z][1], d.zone[z][2],
                               d.zone_ok[z] ? "" : "?");
        for (int h = 0; h < 2 && k < static_cast<int>(sizeof(b)) - 60; ++h)
            k += std::snprintf(b + k, sizeof(b) - k, " | %s hand (%.3f %.3f %.3f) in %d%s", h ? "right" : "left", d.hand[h][0], d.hand[h][1],
                               d.hand[h][2], d.in_zone[h], g_consumed[h].load() ? " grip held" : "");
        return b;
    }
    if (v == "model") {  // holster model [on|off] [offset x y z turn yaw pitch roll]: the session only; the models' state
        std::string x;
        in >> x;
        if (x == "on" || x == "off") set_show_models(x == "on", false);
        float o[3], r[3];
        if (x == "pose" && (in >> o[0] >> o[1] >> o[2] >> r[0] >> r[1] >> r[2])) {  // the offset and ModelTurn (the raw test off)
            std::lock_guard lock(g_zone_mutex);
            std::memcpy(g_model_off, o, sizeof(o));
            std::memcpy(g_model_rot, r, sizeof(r));
            g_model_raw_on = false;
        }
        if (x == "raw" && (in >> r[0] >> r[1] >> r[2])) {  // the native's three angles as given (the axes' test)
            std::lock_guard lock(g_zone_mutex);
            std::memcpy(g_model_raw, r, sizeof(r));
            g_model_raw_on = true;
        }
        float mo[3], mr[3];
        {
            std::lock_guard lock(g_zone_mutex);
            std::memcpy(mo, g_model_off, sizeof(mo));
            std::memcpy(mr, g_model_rot, sizeof(mr));
        }
        char b[240];
        std::snprintf(b, sizeof(b), "models %d (%s), offset (%.3f %.3f %.3f) turn (%.0f %.0f %.0f), shown right %d left %d", g_show_models.load() ? 1 : 0,
                      g_model_name, mo[0], mo[1], mo[2], mr[0], mr[1], mr[2], held_prop::shown(held_prop::kModelSlot) ? 1 : 0,
                      held_prop::shown(held_prop::kModelSlot + 1) ? 1 : 0);
        return b;
    }
    if (v == "instant") {  // the draw's mode for this session (not saved)
        std::string x;
        in >> x;
        g_instant = x != "off";
    }
    if (v == "on" || v == "off") set_keeps_drawn(v == "on");
    char b[400];
    std::snprintf(b, sizeof(b), "keep=%d hooked=%d gun_state=%d topped=%llu draw_time=%.2f player_triggers=%llu instant=%d | raised %d "
                  "(pitch %.1f reach %.2f ahead %.2f, conditions %d of 31, raises %llu), fire-ready %d",
                  g_keep.load() ? 1 : 0, o_player_weapon ? 1 : 0, gun_state(g_player_ped.load()),
                  static_cast<unsigned long long>(g_topped.load()), g_reset_seen.load(),
                  static_cast<unsigned long long>(g_player_triggers.load()), g_instant.load() ? 1 : 0, g_raised.load() ? 1 : 0,
                  g_raise_dbg[0].load(), g_raise_dbg[1].load(), g_raise_dbg[2].load(), static_cast<int>(g_raise_dbg[3].load()),
                  static_cast<unsigned long long>(g_raises.load()), g_fire_ready.load() ? 1 : 0);
    return b;
}

}  // namespace rdrvr::holster
