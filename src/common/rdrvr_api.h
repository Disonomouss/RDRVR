#pragma once
// The C ABI between the core (dinput8.dll) and RDRVR.Gameplay.red.
//
// The core owns all state; the .red runs on RedHook's script fiber (the game's script tick) and is the only
// place natives can be called. Each tick it reports in, takes queued native requests from the core, runs
// them, and posts the results back. Find the core by enumerating loaded modules for the export
// "RDRVR_GetApi" (GetModuleHandle("dinput8.dll") can return the System32 copy the proxy forwards to).

#include <stdint.h>

#define RDRVR_API_VERSION 7u
#define RDRVR_WEAPONS 40  // eWeapon 0..37 (WeaponModel), padded

#ifdef __cplusplus
extern "C" {
#endif

typedef enum RdrvrNativeOp {
    RDRVR_NATIVE_RAW = 0,      // call `hash` with args[0..argc) as raw 64-bit pushes; result = 64 bits
    // v5, [Physics] HandCollision (research\run3\handphysics.md 6.3): one push in one tick. args: [0] the palm's x, y
    // (two floats), [1] its z, [2] the radius, [3] the gain, [4] the speed cap (floats), [5] the address of the game's
    // current script thread global (LOCATE reads its +0x38 unchecked: skipped while it is null), [6] the name filter
    // (RDRVR_PUSH_PART_*: 0 any p_* prop, then bottle, crate, chair, barrel, bucket, box); vec_in: the hand's
    // velocity (world, m/s). Result: value = the prop's handle | the outcome << 32 (RdrvrPush); vec = the velocity set
    // (RDRVR_PUSH_OK, _FAILED) or the prop's position (the others but NONE and GUARD).
    RDRVR_NATIVE_HAND_PUSH = 1,
    // v6, [Physics] Grab (research\run3\handphysics.md 6.6), each in one tick, every native generation-checked:
    // GRAB: args [0] the palm's x, y, [1] z, [2] the radius (as HAND_PUSH), [5] the script thread global, [6] the name
    // filter, [7] the player actor. The nearest loose prop is frozen and kept off the player (SET_PHYSINST_FROZEN 1,
    // SET_OBJECT_COLLIDE_WITH_OBJECT(h, the player's object, 0)). Result: value = handle | outcome << 32 (RDRVR_PUSH_OK:
    // grabbed; NONE, ACTOR, FIXED, GUARD as HAND_PUSH), vec = its position.
    RDRVR_NATIVE_GRAB = 2,
    // GRAB_MOVE: args [0] the handle, [1] x, y, [2] z: SET_OBJECT_POSITION if IS_PHYSINST_VALID. value = 1 moved, 0 gone.
    RDRVR_NATIVE_GRAB_MOVE = 3,
    // GRAB_END: args [0] the handle, [1] the player actor, [2] 0 let go (unfrozen, thrown with vec_in, m/s), 1 its
    // collisions with the player back on (half a second later). value = 1 done, 0 gone.
    RDRVR_NATIVE_GRAB_END = 4,
    // v7, [Gestures] GunMelee (research\round13\gun-melee.md 5.3): one scan in one tick. args: an RdrvrGunMeleeArgs
    // copied over args[0..11]. The object iterator on the ambient layout (type 15, a sphere of radius_cm about seg[0][1])
    // collects up to 10 actors (16 for a scan) and is destroyed before anything else runs; each live, human actor that
    // is not ragdolled, not the player and not held by a cutscene has its head, spine03, spine01 and pelvis tested as
    // spheres against the strike segments; the earliest entry moving into the bone at `speed` or more goes to the core's
    // gun_melee_hit (the game's hit is made there, in this tick). Result: value = the victim's handle (0 none) |
    // RdrvrGunMeleeOutcome << 32 | the core's RdrvrMeleeHitCode << 40 (0xff: not asked) | the actors seen << 48 (up to
    // 255) | the iterators made << 56 | destroyed << 60 (4 bits each); vec = the hit point (a scan: the nearest actor's
    // position), vec[3] its distance from seg[0][1] (m; -1 none). A scan (RDRVR_GUN_MELEE_SCAN) only logs each actor.
    RDRVR_NATIVE_GUN_MELEE = 5,
} RdrvrNativeOp;

// v7: RDRVR_NATIVE_GUN_MELEE's arguments, this struct copied over the request's args (96 bytes, the same size)
#define RDRVR_GUN_MELEE_POINT2 1u  // seg[1] and vel[1] are set (a second strike point)
#define RDRVR_GUN_MELEE_SCAN 2u    // list the actors in the sphere (the log), no hit
typedef struct RdrvrGunMeleeArgs {
    float seg[2][2][3];   // the strike points (0: a long gun's butt or a pistol's frame at the hand, 1: the barrel), each
                          // from where it was at the last scan to where it is now (world)
    float vel[2][3];      // each point's velocity (world, m/s): the hand's own motion, through the recentred local frame
    uint64_t guard;       // the script thread global's address (ScriptThreadCurrent; as HAND_PUSH's args[5])
    uint32_t actor;       // the player's actor handle (GET_PLAYER_ACTOR)
    int32_t weapon;       // the eWeapon in hand
    uint16_t flags;       // RDRVR_GUN_MELEE_*
    uint16_t radius_cm;   // the iterator's sphere about seg[0][1], centimetres (the actors' positions are their roots)
    float speed;          // the speed a hit needs (m/s; [Gestures] GunMeleeSpeed)
} RdrvrGunMeleeArgs;

typedef enum RdrvrGunMeleeOutcome {
    RDRVR_GUN_MELEE_NONE = 0,         // no bone sphere entered fast enough
    RDRVR_GUN_MELEE_HIT = 1,          // a bone entered: the core's answer (RdrvrMeleeHitCode) in bits 40-47
    RDRVR_GUN_MELEE_GUARD = 2,        // no current script thread: no native called
    RDRVR_GUN_MELEE_NO_ITERATOR = 3,  // CREATE_OBJECT_ITERATOR gave no handle
    RDRVR_GUN_MELEE_SCANNED = 4,      // a scan: the actors logged
    RDRVR_GUN_MELEE_NO_CORE = 5,      // the core has no gun_melee_hit
} RdrvrGunMeleeOutcome;

// v7: the hit the plugin found, for the core's gun_melee_hit
typedef struct RdrvrMeleeHit {
    uint32_t victim;      // the victim's actor handle (from the iterator, GET_ACTOR_FROM_OBJECT)
    uint32_t attacker;    // the player's actor handle
    float pos[3];         // the hit point (world): the strike point where it entered the bone's sphere
    float dir[3];         // the strike's unit direction (world)
    float speed;          // its speed (m/s, the hand's own motion)
    int32_t weapon;       // the eWeapon in hand
    char bone[16];        // the bone ("head", "spine03", "spine01", "pelvis"): the hit zone is the game's mapping of it
} RdrvrMeleeHit;

typedef enum RdrvrMeleeHitCode {
    RDRVR_MELEE_HIT_OK = 0,         // the game's hit made (FUN_140ae1760, once)
    RDRVR_MELEE_HIT_DRY = 1,        // built and logged, not made ([Gestures] GunMeleeDryRun)
    RDRVR_MELEE_HIT_NOT_NOW = 2,    // GunMelee off, the anchors stood down, or not inside the plugin's script tick
    RDRVR_MELEE_HIT_BAD_INPUT = 3,  // a point, direction or speed not finite, no bone name
    RDRVR_MELEE_HIT_HANDLE = 4,     // a handle did not resolve (ObjectsPool, ActorPool generations), not an actor, the
                                    // attacker not the local player, or the victim a player
    RDRVR_MELEE_HIT_VICTIM = 5,     // the victim's parts missing (health, AI, ped, physics, its controllers) or no health left
    RDRVR_MELEE_HIT_BLOCKED = 6,    // the victim blocks (its melee controller's state 2)
    RDRVR_MELEE_HIT_MOUNTED = 7,    // the victim rides
    RDRVR_MELEE_HIT_COOLDOWN = 8,   // the same victim was hit less than 500 ms ago
} RdrvrMeleeHitCode;

typedef enum RdrvrPush {
    RDRVR_PUSH_NONE = 0,     // no loose prop within the radius
    RDRVR_PUSH_OK = 1,       // SET_PROP_VELOCITY done
    RDRVR_PUSH_FAILED = 2,   // SET_PROP_VELOCITY refused (no free collider, or fixed by now)
    RDRVR_PUSH_ACTOR = 3,    // an actor: never pushed through the prop natives
    RDRVR_PUSH_FIXED = 4,    // IS_PROP_FIXED
    RDRVR_PUSH_BEHIND = 5,   // the hand is not moving toward it
    RDRVR_PUSH_ALREADY = 6,  // it already moves along the hand as fast as the push
    RDRVR_PUSH_GUARD = 7,    // no current script thread: LOCATE not called
    RDRVR_PUSH_FAR = 8,      // (GRAB) the located prop's own position is too far from the palm: not held
} RdrvrPush;

typedef struct RdrvrNativeRequest {
    uint64_t id;               // echoed in the result
    uint32_t op;               // RdrvrNativeOp
    uint32_t hash;             // native hash
    uint32_t argc;
    uint32_t vec_out;          // if nonzero: args[vec_out-1] is replaced by a pointer to result.vec (a Vector3)
    uint64_t args[12];
    float vec_in[4];           // result.vec starts as this (an input Vector3 such as TELEPORT_ACTOR's position)
} RdrvrNativeRequest;

typedef struct RdrvrNativeResult {
    uint64_t id;
    uint64_t value;            // first 64 bits of the native's return
    float vec[4];              // out Vector3 (x, y, z, pad), when the request asked for one
    uint64_t tick;             // script tick the call ran on
} RdrvrNativeResult;

// G-B camera anchor (v3): the plugin owns a scripted camera (FreeCameraRDR's sequence,
// research\camera-takeover-recipe.md) placed each tick at the player actor's position plus `height` along the up axis
// and `forward` along the heading, level, facing `heading_deg`.
typedef struct RdrvrCameraJob {
    uint32_t enabled;
    uint32_t heading_from_actor;  // 1: use the actor's own heading (the core adopts it from the posted state)
    uint32_t up_axis;             // 1: y is up (the game's world, cycle 36), 2: z is up
    float heading_deg;
    float height;                 // metres above the actor's root
    float forward;                // metres along the heading
    uint32_t orient_mode;         // SET_CAMERA_ORIENTATION's heading component: 0 z, 1 y, 2 x, 3 -y
    uint32_t no_idles;            // 1: SET_ACTOR_CAN_PLAY_BORED_IDLES(player, 0) while enabled (round 5c: idles moved the body)
    uint32_t hide_reticle;        // 1: SET_RETICLE_DRAW_DISABLED_BY_SCRIPT while enabled (shots leave the barrel)
    // riding (the core's [Horse]): with `saddle`, the camera sits above the mount's own root (the horse's position: no
    // gait bounce in it, unlike the rider's) by saddle_height, saddle_forward along the mount's heading, its height
    // low-passed over saddle_tau seconds; with heading_from_mount, it faces the mount's heading (stick steering).
    // saddle bit 1: the anchor; bit 2 (run 6, [Horse] SaddleClimb): the mount's own vertical speed fed into the height's
    // filter (which otherwise fell behind a climb by that speed times saddle_tau), and the camera never below the
    // rider's root + 0.5 m. A plugin that knows only bit 1 still places the anchor.
    uint32_t saddle;
    float saddle_height;
    float saddle_forward;
    float saddle_tau;
    uint32_t heading_from_mount;
    uint32_t weapons;             // v4: 1 to post the player's owned weapons (the holsters' weapon choice)
} RdrvrCameraJob;

typedef struct RdrvrActorState {
    uint64_t tick;
    float pos[3];                 // GET_POSITION of the player actor
    float heading_deg;            // GET_HEADING
    float camera_pos[3];          // where the scripted camera was put this tick
    int32_t camera;               // the scripted camera's handle, 0 if none
    uint32_t valid;
    float head_pos[3];            // GET_OBJECT_NAMED_BONE_POSITION(GET_OBJECT_FROM_ACTOR(actor), "head")
    uint32_t head_valid;
    int32_t object;               // GET_OBJECT_FROM_ACTOR(actor): the player's object handle (body.cpp)
    uint32_t flags;               // RDRVR_ACTOR_*: crouching, in cover, mounted, driving (body.cpp's automatic show)
    int32_t actor;                // GET_PLAYER_ACTOR(0): the player's actor handle (natives from the core)
    int32_t weapon;               // GET_WEAPON_IN_HAND: the eWeapon in hand, -1 none
    float clip;                   // ACTOR_GET_WEAPON_AMMO: its rounds loaded
    float clip_max;               // GET_WEAPON_MAX_AMMO: its clip's size
    float spare;                  // ACTOR_GET_INV_AMMO(actor, its ammo type, 0): the rounds left to load
    uint32_t weapon_flags;        // RDRVR_WEAPON_*
    int32_t mount;                // GET_MOUNT while riding and in the saddle, else 0
    float mount_pos[3];           // its GET_POSITION
    float mount_heading;          // its GET_HEADING
    // v4, with the job's `weapons`: bit e set when ACTOR_HAS_WEAPON(actor, e) (re-read when the inventory count changes
    // and once a second), and each owned weapon's GET_ITEM_EQUIPSLOT (-1 otherwise); owned_tick 0 before the first read
    uint64_t owned;
    uint64_t owned_tick;
    int8_t equip_slot[RDRVR_WEAPONS];
} RdrvrActorState;

#define RDRVR_WEAPON_DEADEYE 1u    // IS_PLAYER_DEADEYE
#define RDRVR_WEAPON_RELOADING 2u  // IS_ACTOR_RELOADING (the game's own reload)

#define RDRVR_ACTOR_CROUCHING 1u  // IS_ACTOR_CROUCHING
#define RDRVR_ACTOR_IN_COVER 2u   // IS_ACTOR_USING_COVER
#define RDRVR_ACTOR_MOUNTED 4u    // IS_ACTOR_RIDING_AND_IN_SADDLE (a horse; IS_ACTOR_MOUNTED stays 0 for the rider)
#define RDRVR_ACTOR_DRIVING 8u    // IS_ACTOR_DRIVING_VEHICLE (a wagon, a coach)

typedef struct RdrvrApi {
    uint32_t version;          // RDRVR_API_VERSION
    void (*log)(int level, const char* text);                    // 0 info, 1 warn, 2 error
    void (*on_script_tick)(uint64_t tick, double script_ms);     // once per script tick, first thing
    int (*pop_native_request)(RdrvrNativeRequest* out);          // 1 if a request was returned
    void (*post_native_result)(const RdrvrNativeResult* result);
    void (*get_camera_job)(RdrvrCameraJob* out);                 // v3: each tick, after on_script_tick
    void (*post_actor_state)(const RdrvrActorState* state);      // v3: after the camera was placed
    // v7, [Gestures] GunMelee: the game's hit on an actor, made by the core synchronously (RDRVR_NATIVE_GUN_MELEE's
    // tick); refused outside the plugin's tick (between on_script_tick and end_script_tick, on that thread).
    // Returns an RdrvrMeleeHitCode.
    int (*gun_melee_hit)(const RdrvrMeleeHit* hit);
    void (*end_script_tick)(uint64_t tick);                      // v7: after the tick's requests ran
} RdrvrApi;

typedef const RdrvrApi* (*RdrvrGetApiFn)(void);

#ifdef __cplusplus
}
#endif
