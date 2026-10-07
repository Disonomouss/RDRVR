// RDRVR.Gameplay.red: the RedHook plugin. Runs on RedHook's script fiber, the only place natives can be
// called. It reports each script tick to the core and executes the native calls the core queued.

#include <windows.h>
#include <psapi.h>

#include <corecrt_math.h>  // not <cmath>: the SDK include path has its own Math.h, which shadows <math.h>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <cstdio>
#include <cwchar>
#include <type_traits>

#include <RedHook.h>

#include "common/rdrvr_api.h"

namespace {

HMODULE g_module = nullptr;
const RdrvrApi* g_api = nullptr;

const RdrvrApi* find_core() {
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return nullptr;
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; ++i) {
        if (auto fn = reinterpret_cast<RdrvrGetApiFn>(GetProcAddress(mods[i], "RDRVR_GetApi"))) {
            const RdrvrApi* api = fn();
            if (api && api->version == RDRVR_API_VERSION) return api;
        }
    }
    return nullptr;
}

void hand_push(const RdrvrNativeRequest& req, RdrvrNativeResult* res);
void grab_op(const RdrvrNativeRequest& req, RdrvrNativeResult* res);

void run_request(const RdrvrNativeRequest& req, uint64_t tick) {
    RdrvrNativeResult res{};
    res.id = req.id;
    res.tick = tick;
    if (req.op == RDRVR_NATIVE_HAND_PUSH) {
        hand_push(req, &res);
        g_api->post_native_result(&res);
        return;
    }
    if (req.op == RDRVR_NATIVE_GRAB || req.op == RDRVR_NATIVE_GRAB_MOVE || req.op == RDRVR_NATIVE_GRAB_END) {
        grab_op(req, &res);
        g_api->post_native_result(&res);
        return;
    }
    if (req.op != RDRVR_NATIVE_RAW) {  // an op this plugin does not know: never run as a raw native
        g_api->post_native_result(&res);
        return;
    }
    for (int k = 0; k < 4; ++k) res.vec[k] = req.vec_in[k];  // an input Vector3 (zero unless the request set one)
    NativeInit(req.hash);
    for (uint32_t i = 0; i < req.argc; ++i) {
        uint64_t v = req.args[i];
        if (req.vec_out && i == req.vec_out - 1) v = reinterpret_cast<uint64_t>(&res.vec[0]);
        NativePush64(v);
    }
    uint64_t* r = NativeCall();
    res.value = r ? *r : 0;
    g_api->post_native_result(&res);
}

// ---- G-B camera anchor (API v3; research\camera-takeover-recipe.md)
uint64_t invoke(uint32_t hash, std::initializer_list<uint64_t> args) {
    NativeInit(hash);
    for (uint64_t a : args) NativePush64(a);
    uint64_t* r = NativeCall();
    return r ? *r : 0;
}
uint64_t f32(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
float as_f32(uint64_t v) {
    uint32_t u = static_cast<uint32_t>(v);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
uint64_t vec2(float x, float y) { return f32(x) | (f32(y) << 32); }

const char kNoName[] = "";
int32_t g_cam = 0;

// ---- [Physics] HandCollision (API v5, research\run3\handphysics.md 6.3): the nearest loose prop within the radius of
// the palm, pushed along the hand. Every native here checks its handle's generation; all run in this one tick.
// the name filters (static strings: LOCATE keeps the pointer only for the call): prop fragments are p_* (never the
// guns: rifle_*, pistol_*), then a few kinds of loose object
const char* const kParts[] = {"p_", "bottle", "crate", "chair", "barrel", "bucket", "box"};
bool script_thread_ok(uint64_t at) {
    __try {
        const uintptr_t t = at ? *reinterpret_cast<const uintptr_t*>(at) : 0;
        return t && *reinterpret_cast<const uint64_t*>(t + 0x38);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void hand_push(const RdrvrNativeRequest& req, RdrvrNativeResult* res) {
    res->value = 0;
    if (!script_thread_ok(req.args[5])) {
        res->value = static_cast<uint64_t>(RDRVR_PUSH_GUARD) << 32;
        return;
    }
    const float px = as_f32(req.args[0]), py = as_f32(req.args[0] >> 32), pz = as_f32(req.args[1]);
    const float gain = as_f32(req.args[3]), vmax = as_f32(req.args[4]);
    const float* v = req.vec_in;
    // LOCATE_PHYSINST_OF_PARTIAL_TYPE(Vector3 centre as x,y then z, float radius, const char* part, BOOL fallback 0)
    const char* part = kParts[req.args[6] < sizeof(kParts) / sizeof(kParts[0]) ? req.args[6] : 0];
    const uint64_t h = static_cast<uint32_t>(invoke(0x4FF36FA7, {req.args[0], req.args[1], req.args[2], reinterpret_cast<uint64_t>(part), 0}));
    if (!h) return;
    auto done = [&](RdrvrPush code) { res->value = h | (static_cast<uint64_t>(code) << 32); };
    if (static_cast<int32_t>(invoke(0x261ECB20, {h})) == 15) return done(RDRVR_PUSH_ACTOR);  // GET_OBJECT_TYPE
    float p[4] = {}, v0[4] = {}, v1[4] = {};
    invoke(0x31201B4C, {h, reinterpret_cast<uint64_t>(p)});  // GET_OBJECT_POSITION
    for (int k = 0; k < 3; ++k) res->vec[k] = p[k];
    if (invoke(0xBD2FFD8C, {h}) & 0xff) return done(RDRVR_PUSH_FIXED);  // IS_PROP_FIXED
    {  // run 5: LOCATE can return a prop whose own origin is far from the point (as GRAB): never pushed
        const float dx = p[0] - px, dy = p[1] - py, dz = p[2] - pz, lim = as_f32(req.args[2]) + 0.25f;
        if (dx * dx + dy * dy + dz * dz > lim * lim) return done(RDRVR_PUSH_NONE);
    }
    const float vl = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (vl < 1e-3f || v[0] * (p[0] - px) + v[1] * (p[1] - py) + v[2] * (p[2] - pz) <= 0.0f) return done(RDRVR_PUSH_BEHIND);
    invoke(0x17B69196, {h, reinterpret_cast<uint64_t>(v0)});  // GET_PHYSINST_VELOCITY
    const float d[3] = {v[0] / vl, v[1] / vl, v[2] / vl};
    const float sp = gain * vl < vmax ? gain * vl : vmax, a = v0[0] * d[0] + v0[1] * d[1] + v0[2] * d[2];
    if (a >= sp) return done(RDRVR_PUSH_ALREADY);
    for (int k = 0; k < 3; ++k) v1[k] = v0[k] + (sp - a) * d[k];
    const float l1 = sqrtf(v1[0] * v1[0] + v1[1] * v1[1] + v1[2] * v1[2]);
    if (l1 > vmax)
        for (int k = 0; k < 3; ++k) v1[k] *= vmax / l1;
    if (v1[1] > 3.0f) v1[1] = 3.0f;  // y is up: props do not launch
    const bool ok = (invoke(0x28425D8C, {h, reinterpret_cast<uint64_t>(v1)}) & 0xff) != 0;  // SET_PROP_VELOCITY (wakes it)
    if (ok)
        for (int k = 0; k < 3; ++k) res->vec[k] = v1[k];
    done(ok ? RDRVR_PUSH_OK : RDRVR_PUSH_FAILED);
}

// ---- [Physics] Grab (API v6, research\run3\handphysics.md 6.6): a loose prop held in the hand. Every native here checks
// its handle's generation (the research's list); a grab and its first move run in separate ticks, each checked.
void grab_op(const RdrvrNativeRequest& req, RdrvrNativeResult* res) {
    res->value = 0;
    if (req.op == RDRVR_NATIVE_GRAB) {
        if (!script_thread_ok(req.args[5])) {
            res->value = static_cast<uint64_t>(RDRVR_PUSH_GUARD) << 32;
            return;
        }
        const char* part = kParts[req.args[6] < sizeof(kParts) / sizeof(kParts[0]) ? req.args[6] : 0];
        const uint64_t h = static_cast<uint32_t>(invoke(0x4FF36FA7, {req.args[0], req.args[1], req.args[2], reinterpret_cast<uint64_t>(part), 0}));
        if (!h) return;
        auto done = [&](RdrvrPush code) { res->value = h | (static_cast<uint64_t>(code) << 32); };
        if (static_cast<int32_t>(invoke(0x261ECB20, {h})) == 15) return done(RDRVR_PUSH_ACTOR);  // GET_OBJECT_TYPE
        float p[4] = {};
        invoke(0x31201B4C, {h, reinterpret_cast<uint64_t>(p)});  // GET_OBJECT_POSITION
        for (int k = 0; k < 3; ++k) res->vec[k] = p[k];
        if (invoke(0xBD2FFD8C, {h}) & 0xff) return done(RDRVR_PUSH_FIXED);  // IS_PROP_FIXED
        // LOCATE can return a prop whose own origin is far from the point (run 5: one 1.5 m away at the player's feet,
        // for a 0.15 m radius): only an object whose position is within the radius and a quarter metre is held
        const float dx = p[0] - as_f32(req.args[0]), dy = p[1] - as_f32(req.args[0] >> 32), dz = p[2] - as_f32(req.args[1]);
        const float lim = as_f32(req.args[2]) + 0.25f;
        if (dx * dx + dy * dy + dz * dz > lim * lim) return done(RDRVR_PUSH_FAR);
        invoke(0x2C0AF634, {h, 1});  // SET_PHYSINST_FROZEN
        const uint64_t po = static_cast<uint32_t>(invoke(0x4A2063EC, {static_cast<uint32_t>(req.args[7])}));  // GET_OBJECT_FROM_ACTOR
        if (po) invoke(0x9AC1CA75, {h, po, 0});  // SET_OBJECT_COLLIDE_WITH_OBJECT(h, the player, off)
        return done(RDRVR_PUSH_OK);
    }
    const uint64_t h = static_cast<uint32_t>(req.args[0]);
    if (!h || !(invoke(0x16C0A6CB, {h}) & 0xff)) return;  // IS_PHYSINST_VALID
    if (req.op == RDRVR_NATIVE_GRAB_MOVE) {
        invoke(0xC5D796F8, {h, req.args[1], req.args[2]});  // SET_OBJECT_POSITION(h, x y, z)
        res->value = 1;
        return;
    }
    const uint64_t po = static_cast<uint32_t>(invoke(0x4A2063EC, {static_cast<uint32_t>(req.args[1])}));  // GET_OBJECT_FROM_ACTOR
    if (req.args[2] == 1) {  // half a second after the release: its collisions with the player back
        if (po) invoke(0x9AC1CA75, {h, po, 1});
        res->value = 1;
        return;
    }
    invoke(0x2C0AF634, {h, 0});  // SET_PHYSINST_FROZEN off
    float v[4] = {req.vec_in[0], req.vec_in[1], req.vec_in[2], 0.0f};
    if (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] > 1e-4f) invoke(0x28425D8C, {h, reinterpret_cast<uint64_t>(v)});  // SET_PROP_VELOCITY
    for (int k = 0; k < 3; ++k) res->vec[k] = v[k];
    res->value = 1;
}

// The player's posture for the core's automatic body modes (RDRVR_ACTOR_*).
uint32_t actor_flags(int32_t actor) {
    const uint64_t a = static_cast<uint64_t>(static_cast<uint32_t>(actor));
    uint32_t f = 0;
    if (invoke(0xF6BF4242, {a}) & 0xff) f |= RDRVR_ACTOR_CROUCHING;  // IS_ACTOR_CROUCHING
    if (invoke(0xD39C4A9E, {a}) & 0xff) f |= RDRVR_ACTOR_IN_COVER;   // IS_ACTOR_USING_COVER
    if (invoke(0xF270EAC1, {a}) & 0xff) f |= RDRVR_ACTOR_MOUNTED;    // IS_ACTOR_RIDING_AND_IN_SADDLE (IS_ACTOR_MOUNTED is 0 for the rider)
    if (invoke(0xDC99C124, {a}) & 0xff) f |= RDRVR_ACTOR_DRIVING;    // IS_ACTOR_DRIVING_VEHICLE
    return f;
}

// The gun in the player's hand for the core's hand reload: the eWeapon, the rounds loaded, the clip's size, the spare
// rounds of its ammo type, Dead Eye and the game's own reload.
void weapon_state(int32_t actor, RdrvrActorState* st) {
    const uint64_t a = static_cast<uint64_t>(static_cast<uint32_t>(actor));
    st->weapon = static_cast<int32_t>(invoke(0xA4B2016D, {a}));  // GET_WEAPON_IN_HAND
    st->weapon_flags = 0;
    if (invoke(0x6148423A, {a}) & 0xff) st->weapon_flags |= RDRVR_WEAPON_DEADEYE;  // IS_PLAYER_DEADEYE
    if (st->weapon < 0) return;
    const uint64_t w = static_cast<uint64_t>(static_cast<uint32_t>(st->weapon));
    st->clip = as_f32(invoke(0x43DEDFAE, {a, w}));                       // ACTOR_GET_WEAPON_AMMO
    st->clip_max = as_f32(invoke(0xA677B204, {w}));                      // GET_WEAPON_MAX_AMMO
    const int32_t type = static_cast<int32_t>(invoke(0x17883570, {w}));  // GET_AMMOENUM_FOR_WEAPONENUM
    if (type >= 0) st->spare = as_f32(invoke(0xE224AC6F, {a, static_cast<uint64_t>(static_cast<uint32_t>(type)), 0}));  // ACTOR_GET_INV_AMMO
    if (invoke(0x39C518DB, {a}) & 0xff) st->weapon_flags |= RDRVR_WEAPON_RELOADING;  // IS_ACTOR_RELOADING
}

// The player's owned weapons (the holsters' weapon choice): ACTOR_HAS_WEAPON for each eWeapon when the inventory's
// count changes or once a second (60 ticks), and each owned one's equip slot (GET_ITEM_EQUIPSLOT, static per weapon;
// only for owned weapons: its handler does not check the weapon's item definition for null).
void weapons_state(int32_t actor, uint64_t tick, RdrvrActorState* st) {
    static int32_t last_actor = 0, last_count = -2;
    static uint64_t owned = 0, owned_tick = 0;
    static int8_t slot_of[RDRVR_WEAPONS];
    static bool slot_read[RDRVR_WEAPONS] = {};
    const uint64_t a = static_cast<uint64_t>(static_cast<uint32_t>(actor));
    if (actor != last_actor) {
        last_actor = actor;
        last_count = -2;
        owned = 0;
        owned_tick = 0;
    }
    const int32_t n = static_cast<int32_t>(invoke(0x118D085E, {a}));  // GET_NUM_WEAPONS_IN_INVENTORY
    if (n != last_count || tick - owned_tick >= 60) {
        uint64_t m = 0;
        for (int e = 0; e < 38; ++e)
            if (invoke(0x0D47CFBD, {a, static_cast<uint64_t>(e)}) & 0xff) m |= 1ull << e;  // ACTOR_HAS_WEAPON
        for (int e = 0; e < 38; ++e)
            if ((m >> e & 1) && !slot_read[e]) {
                slot_of[e] = static_cast<int8_t>(static_cast<int32_t>(invoke(0x0E0EFB13, {static_cast<uint64_t>(e)})));  // GET_ITEM_EQUIPSLOT
                slot_read[e] = true;
            }
        owned = m;
        last_count = n;
        owned_tick = tick ? tick : 1;
    }
    st->owned = owned;
    st->owned_tick = owned_tick;
    for (int e = 0; e < RDRVR_WEAPONS; ++e) st->equip_slot[e] = e < 38 && slot_read[e] && (owned >> e & 1) ? slot_of[e] : -1;
}

// The mount under the player (riding and in the saddle): its handle, position and heading.
void mount_state(int32_t actor, uint32_t flags, RdrvrActorState* st) {
    st->mount = 0;
    if (!(flags & RDRVR_ACTOR_MOUNTED)) return;
    const int32_t m = static_cast<int32_t>(invoke(0xDD31EC4E, {static_cast<uint64_t>(static_cast<uint32_t>(actor))}));  // GET_MOUNT
    if (!m) return;
    alignas(16) float mp[4] = {};
    invoke(0x99BD9D6F, {static_cast<uint64_t>(static_cast<uint32_t>(m)), reinterpret_cast<uint64_t>(mp)});  // GET_POSITION
    st->mount = m;
    std::memcpy(st->mount_pos, mp, sizeof(st->mount_pos));
    st->mount_heading = as_f32(invoke(0x42DE39F0, {static_cast<uint64_t>(static_cast<uint32_t>(m))}));  // GET_HEADING
}

void camera_tick(uint64_t tick) {
    if (g_api->version < 3 || !g_api->get_camera_job) return;
    RdrvrCameraJob job{};
    g_api->get_camera_job(&job);
    // the bored idles (looking around, stretching) off for the player while the anchor is on, back on after
    {
        static bool idles_off = false;
        static uint64_t last = 0;
        bool want_off = job.enabled && job.no_idles;
        if (want_off != idles_off || (want_off && tick - last > 300)) {
            int32_t a = static_cast<int32_t>(invoke(0xE8CFDD53, {0}));  // GET_PLAYER_ACTOR(0)
            if (a) {
                invoke(0x0B5E1904, {static_cast<uint64_t>(static_cast<uint32_t>(a)), want_off ? 0u : 1u});  // SET_ACTOR_CAN_PLAY_BORED_IDLES
                if (want_off != idles_off) g_api->log(0, want_off ? "bored idles off (first person)" : "bored idles back on");
                idles_off = want_off;
                last = tick;
            }
        }
    }
    // the reticle hidden in first person (the shots leave the barrel), shown again after
    {
        static bool hidden = false;
        static uint64_t last = 0;
        bool want = job.enabled && job.hide_reticle;
        if (want != hidden || (want && tick - last > 300)) {
            invoke(0xCE7CE46D, {0, want ? 1u : 0u});  // SET_RETICLE_DRAW_DISABLED_BY_SCRIPT
            if (want != hidden) g_api->log(0, want ? "reticle hidden (first person)" : "reticle shown");
            hidden = want;
            last = tick;
        }
    }
    if (!job.enabled) {
        if (g_cam) {
            invoke(0x423DB420, {static_cast<uint64_t>(g_cam), 0});  // REMOVE_CAMERA_FROM_CHANNEL
            invoke(0x767E08D0, {static_cast<uint64_t>(g_cam)});     // DESTROY_CAMERA
            char line[64];
            std::snprintf(line, sizeof(line), "camera anchor: camera %d removed", g_cam);
            g_api->log(0, line);
            g_cam = 0;
        }
        // the actor state without the camera (body.cpp needs the player's object for the head hide in any view)
        int32_t actor = static_cast<int32_t>(invoke(0xE8CFDD53, {0}));  // GET_PLAYER_ACTOR(0)
        if (actor) {
            RdrvrActorState st{};
            alignas(16) float pos[4] = {};
            st.tick = tick;
            invoke(0x99BD9D6F, {static_cast<uint64_t>(static_cast<uint32_t>(actor)), reinterpret_cast<uint64_t>(pos)});  // GET_POSITION
            std::memcpy(st.pos, pos, sizeof(st.pos));
            st.heading_deg = as_f32(invoke(0x42DE39F0, {static_cast<uint64_t>(static_cast<uint32_t>(actor))}));         // GET_HEADING
            st.object = static_cast<int32_t>(invoke(0x4A2063EC, {static_cast<uint64_t>(static_cast<uint32_t>(actor))}));  // GET_OBJECT_FROM_ACTOR
            st.flags = actor_flags(actor);
            st.actor = actor;
            weapon_state(actor, &st);
            if (job.weapons) weapons_state(actor, tick, &st);
            mount_state(actor, st.flags, &st);
            st.valid = 1;
            g_api->post_actor_state(&st);
        }
        return;
    }
    int32_t actor = static_cast<int32_t>(invoke(0xE8CFDD53, {0}));  // GET_PLAYER_ACTOR(0)
    if (!actor) return;
    alignas(16) float pos[4] = {};
    invoke(0x99BD9D6F, {static_cast<uint64_t>(static_cast<uint32_t>(actor)), reinterpret_cast<uint64_t>(pos)});  // GET_POSITION
    float actor_heading = as_f32(invoke(0x42DE39F0, {static_cast<uint64_t>(static_cast<uint32_t>(actor))}));    // GET_HEADING
    if (!g_cam) {
        uint64_t layout = invoke(0xB52A3D48, {});  // GET_AMBIENT_LAYOUT
        g_cam = static_cast<int32_t>(invoke(0x0B1569C5, {layout & 0xffffffffu, reinterpret_cast<uint64_t>(kNoName), 0}));
        if (!g_cam) return;
        invoke(0x2615309A, {static_cast<uint64_t>(g_cam)});                 // INIT_CAMERA_FROM_GAME_CAMERA
        invoke(0x3EA55678, {static_cast<uint64_t>(g_cam), 0, 0, 0, 0, 0, 0, 0, 0, 0});  // SET_CURRENT_CAMERA_ON_CHANNEL
        char line[96];
        std::snprintf(line, sizeof(line), "camera anchor: camera %d made in layout %llu", g_cam,
                      static_cast<unsigned long long>(layout & 0xffffffffu));
        g_api->log(0, line);
    }
    const uint32_t flags = actor_flags(actor);
    RdrvrActorState st{};
    mount_state(actor, flags, &st);
    float heading = job.heading_from_actor ? actor_heading : job.heading_deg;
    if (st.mount && job.heading_from_mount) heading = st.mount_heading;
    float h = heading * 0.0174532925f;
    float cam[3] = {pos[0], pos[1], pos[2]};
    // riding with the saddle anchor: above the mount's own root on its heading, the height low-passed. Otherwise
    // forward along the heading in the horizontal plane, then up by the height. y up (the game's world): heading h
    // faces (-sin h, -cos h) in x/z, positive turning left (cycle 36: heading 5.5 walked along (-0.14, -0.99)).
    static float saddle_y = 0.0f;
    static LARGE_INTEGER saddle_qpc{};
    if (st.mount && job.saddle && job.up_axis == 1) {
        const float mh = st.mount_heading * 0.0174532925f;
        cam[0] = st.mount_pos[0] - sinf(mh) * job.saddle_forward;
        cam[2] = st.mount_pos[2] - cosf(mh) * job.saddle_forward;
        const float want = st.mount_pos[1] + job.saddle_height;
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        const float dt = saddle_qpc.QuadPart ? static_cast<float>(now.QuadPart - saddle_qpc.QuadPart) / static_cast<float>(freq.QuadPart) : 1.0f;
        saddle_qpc = now;
        const float a = dt > 0.2f || job.saddle_tau <= 0.01f ? 1.0f : 1.0f - expf(-dt / job.saddle_tau);
        // [Horse] SaddleClimb (run 6 item 8; job.saddle bit 2): the height's low-pass fell behind any climb by the
        // horse's vertical speed times tau (a steep gallop: 0.6-0.9 m, the user's eyes at the saddle). The mount's own
        // vertical speed is fed forward and only the remainder filtered (on flat ground the same as before); and the
        // camera never below the rider's root + 0.5 m
        const bool climb = (job.saddle & 2u) != 0;
        if (climb && a < 1.0f) {
            alignas(16) float mv[4] = {};
            invoke(0xAD6AF65C, {static_cast<uint64_t>(static_cast<uint32_t>(st.mount)), reinterpret_cast<uint64_t>(mv)});  // GET_ACTOR_VELOCITY
            if (mv[1] == mv[1] && fabsf(mv[1]) < 30.0f) saddle_y += mv[1] * dt;
        }
        saddle_y = saddle_y + (want - saddle_y) * a;
        if (climb && saddle_y < pos[1] + 0.5f) saddle_y = pos[1] + 0.5f;
        cam[1] = saddle_y;
    } else if (job.up_axis == 1) {
        saddle_qpc.QuadPart = 0;
        cam[0] += -sinf(h) * job.forward;
        cam[2] += -cosf(h) * job.forward;
        cam[1] += job.height;
    } else {
        cam[0] += -sinf(h) * job.forward;
        cam[1] += cosf(h) * job.forward;
        cam[2] += job.height;
    }
    invoke(0x0B12CD8C, {static_cast<uint64_t>(g_cam), vec2(cam[0], cam[1]), f32(cam[2])});        // SET_CAMERA_POSITION
    // SET_CAMERA_ORIENTATION(cam, {a, b}, c, 0): which component turns the camera about the world's up axis is the
    // job's orient_mode (round 1: the heading on c rolled the view on its side).
    float ra = job.orient_mode == 2 ? heading : 0.0f, rb = job.orient_mode == 1 ? heading : job.orient_mode == 3 ? -heading : 0.0f,
          rc = job.orient_mode == 0 ? heading : 0.0f;
    invoke(0x486F4461, {static_cast<uint64_t>(g_cam), vec2(ra, rb), f32(rc), 0});                 // SET_CAMERA_ORIENTATION
    st.tick = tick;
    std::memcpy(st.pos, pos, sizeof(st.pos));
    st.heading_deg = actor_heading;
    std::memcpy(st.camera_pos, cam, sizeof(cam));
    st.camera = g_cam;
    st.valid = 1;
    int32_t object = static_cast<int32_t>(invoke(0x4A2063EC, {static_cast<uint64_t>(static_cast<uint32_t>(actor))}));  // GET_OBJECT_FROM_ACTOR
    st.object = object;
    st.flags = flags;
    st.actor = actor;
    weapon_state(actor, &st);
    if (job.weapons) weapons_state(actor, tick, &st);
    alignas(16) float head[4] = {};
    static const char kHead[] = "head";  // the camera skeleton mapping's head bone
    if (object && (invoke(0x30516389, {static_cast<uint64_t>(static_cast<uint32_t>(object)), reinterpret_cast<uint64_t>(kHead),
                                        reinterpret_cast<uint64_t>(head)}) & 0xff)) {  // GET_OBJECT_NAMED_BONE_POSITION
        std::memcpy(st.head_pos, head, sizeof(st.head_pos));
        st.head_valid = 1;
    }
    g_api->post_actor_state(&st);
}

void script_main() {
    g_api = find_core();
    if (!g_api) {
        Print(Log_Error, "[RDRVR] core (dinput8.dll proxy) not found; gameplay plugin idle");
        for (;;) ScriptWait(1000);
    }
    g_api->log(0, "gameplay plugin script started");
    uint64_t tick = 0;
    for (;;) {
        ++tick;
        g_api->on_script_tick(tick, 0.0);
        camera_tick(tick);
        RdrvrNativeRequest req;
        int budget = 64;  // bound per tick so a flood cannot stall the script VM
        while (budget-- > 0 && g_api->pop_native_request(&req)) run_request(req, tick);
        ScriptWait(0);
    }
}

}  // namespace

// RedHook also loads plugins into other executables in the game folder that import winmm (RDRMessage.exe, the crash
// reporter, on 2026-10-03: it then waited for a native invoker that never comes and did not exit). Register the
// script only inside RDR.exe.
bool host_is_game() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    return _wcsicmp(name ? name + 1 : path, L"RDR.exe") == 0;
}

bool g_registered = false;

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        if (host_is_game()) {
            ScriptRegister(module, script_main);
            g_registered = true;
        }
    }
    // No ScriptUnregister on DLL_PROCESS_DETACH, ever: RedHook FreeLibrary's its plugins while it shuts down, when its
    // script manager is already gone, and ScriptUnregister then faults in RedHook+0xebc6 (null+0x40). That crashed every
    // quit on 2026-10-03 (cycles 2 and 5; the game's Crashpad files each one). RedHook drops the script of a module it
    // unloads itself.
    (void)reserved;
    (void)g_registered;
    return TRUE;
}
